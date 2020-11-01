#include "Settings.hpp"
#include "init.hpp"
#include "HookTools.hpp"
#include "interfaces.hpp"
#include "navparser.hpp"
#include "playerresource.h"
#include "localplayer.hpp"
#include "sdk.hpp"
#include "entitycache.hpp"
#include "PlayerTools.hpp"

namespace hacks::tf2::NavBot
{
static settings::Boolean enabled("navbot.enabled", "false");
static settings::Boolean search_health("navbot.search-health", "true");
static settings::Boolean search_ammo("navbot.search-ammo", "true");
static settings::Boolean stay_near("navbot.stay-near", "true");
static settings::Boolean snipe_sentries("navbot.snipe-sentries", "true");
static settings::Boolean snipe_sentries_shortrange("navbot.snipe-sentries.shortrange", "true");
static settings::Int blacklist_delay("navbot.proximity-blacklist.delay", "500");
static settings::Int blacklist_delay_dormat("navbot.proximity-blacklist.delay-dormant", "1000");

enum Priority_list
{
    patrol = 5,
    lowprio_health,
    staynear,
    snipe_sentry,
    ammo,
    health,
    danger
};

// For stay near, min and max distance we should be from the enemy.
struct bot_class_config
{
    float min;
    float max;
    bool prefer_far;
};

// Sniper, stay far away and snipe
constexpr bot_class_config CONFIG_SNIPER{ 700.0f, FLT_MAX, true };
// A short range class like scout or heavy, run at the enemy
constexpr bot_class_config CONFIG_SHORT_RANGE{ 0.0f, 400.0f, false };
// A mid range class like the Soldier, don't get too close but also don't run too far away
constexpr bot_class_config CONFIG_MID_RANGE{ 500.0f, 4000.0f, true };

bot_class_config selected_config = CONFIG_SNIPER;

static Timer health_cooldown{};
static Timer ammo_cooldown{};
// Should we search health at all?
bool shouldSearchHealth(bool low_priority = false)
{
    // Priority too high
    if (navparser::NavEngine::current_priority > health)
        return false;
    float health_percent = LOCAL_E->m_iHealth() / (float) g_pPlayerResource->GetMaxHealth(LOCAL_E);
    // Get health when below 65%, or below 90% and just patroling
    return health_percent < 0.64f || (low_priority && (navparser::NavEngine::current_priority <= patrol || navparser::NavEngine::current_priority == lowprio_health) && health_percent <= 0.90f);
}

// Should we search ammo at all?
bool shouldSearchAmmo()
{
    if (CE_BAD(LOCAL_W))
        return false;
    // Priority too high
    if (navparser::NavEngine::current_priority > ammo)
        return false;

    int *weapon_list = (int *) ((uint64_t)(RAW_ENT(LOCAL_E)) + netvar.hMyWeapons);
    if (!weapon_list)
        return false;
    if (g_pLocalPlayer->holding_sniper_rifle && CE_INT(LOCAL_E, netvar.m_iAmmo + 4) <= 5)
        return true;
    for (int i = 0; weapon_list[i]; i++)
    {
        int handle = weapon_list[i];
        int eid    = handle & 0xFFF;
        if (eid > MAX_PLAYERS && eid <= HIGHEST_ENTITY)
        {
            IClientEntity *weapon = g_IEntityList->GetClientEntity(eid);
            if (weapon and re::C_BaseCombatWeapon::IsBaseCombatWeapon(weapon) && re::C_TFWeaponBase::UsesPrimaryAmmo(weapon) && !re::C_TFWeaponBase::HasPrimaryAmmo(weapon))
                return true;
        }
    }
    return false;
}

// Get entities of given itemtypes (Used for health/ammo)
std::vector<CachedEntity *> getEntities(std::vector<k_EItemType> itemtypes)
{
    std::vector<CachedEntity *> entities;
    for (int i = g_IEngine->GetMaxClients() + 1; i < MAX_ENTITIES; i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_BAD(ent))
            continue;
        for (auto &itemtype : itemtypes)
        {
            if (ent->m_ItemType() == itemtype)
            {
                entities.push_back(ent);
                break;
            }
        }
    }
    // Sort by distance, closer is better
    std::sort(entities.begin(), entities.end(), [](CachedEntity *a, CachedEntity *b) { return a->m_flDistance() < b->m_flDistance(); });
    return entities;
}

// Find health if needed
bool getHealth(bool low_priority = false)
{
    Priority_list priority = low_priority ? lowprio_health : health;
    if (!health_cooldown.check(1000))
        return navparser::NavEngine::current_priority == priority;
    if (shouldSearchHealth(low_priority))
    {
        // Already pathing
        if (navparser::NavEngine::current_priority == priority)
            return true;
        auto healthpacks = getEntities({ ITEM_HEALTH_SMALL, ITEM_HEALTH_MEDIUM, ITEM_HEALTH_LARGE });
        for (auto healthpack : healthpacks)
            // If we succeeed, don't try to path to other packs
            if (navparser::NavEngine::navTo(healthpack->m_vecOrigin(), priority))
                return true;
        health_cooldown.update();
    }
    else if (navparser::NavEngine::current_priority == priority)
        navparser::NavEngine::cancelPath();
    return false;
}

// Find ammo if needed
bool getAmmo()
{
    if (!ammo_cooldown.check(1000))
        return navparser::NavEngine::current_priority == ammo;
    if (shouldSearchAmmo())
    {
        // Already pathing
        if (navparser::NavEngine::current_priority == ammo)
            return true;
        auto ammopacks = getEntities({ ITEM_AMMO_SMALL, ITEM_AMMO_MEDIUM, ITEM_AMMO_LARGE });
        for (auto ammopack : ammopacks)
            // If we succeeed, don't try to path to other packs
            if (navparser::NavEngine::navTo(ammopack->m_vecOrigin(), ammo))
                return true;
        ammo_cooldown.update();
    }
    else if (navparser::NavEngine::current_priority == ammo)
        navparser::NavEngine::cancelPath();
    return false;
}

std::vector<Vector> sniper_spots;

// Used for time between refreshing sniperspots
static Timer refresh_sniperspots_timer{};

void refreshSniperSpots()
{
    if (!refresh_sniperspots_timer.test_and_set(60000))
        return;

    sniper_spots.clear();

    // Search all nav areas for valid sniper spots
    for (auto &area : navparser::NavEngine::getNavFile()->m_areas)
        for (auto &hiding_spot : area.m_hidingSpots)
            // Spots actually marked for sniping
            if (hiding_spot.IsExposed() || hiding_spot.IsGoodSniperSpot() || hiding_spot.IsIdealSniperSpot())
                sniper_spots.push_back(hiding_spot.m_pos);
}

static Timer blacklist_update_timer{};
static Timer dormant_update_timer{};
void updateEnemyBlacklist()
{
    bool should_run_normal  = blacklist_update_timer.test_and_set(*blacklist_delay);
    bool should_run_dormant = dormant_update_timer.test_and_set(*blacklist_delay_dormat);
    // Don't run since we do not care here
    if (!should_run_dormant && !should_run_normal)
        return;

    // Clear blacklist for normal entities
    if (should_run_normal)
        navparser::NavEngine::clearFreeBlacklist(navparser::ENEMY_NORMAL);
    // Clear blacklist for dormant entities
    if (should_run_dormant)
        navparser::NavEngine::clearFreeBlacklist(navparser::ENEMY_DORMANT);

    std::vector<Vector> checked_origins;
    for (int i = 1; i <= g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        // Entity is generally invalid, ignore
        if (CE_INVALID(ent) || !g_pPlayerResource->isAlive(i))
            continue;
        // Not on our team, do not care
        if (g_pPlayerResource->GetTeam(i) == g_pLocalPlayer->team)
            continue;

        bool is_dormant = CE_BAD(ent);
        // Should not run on dormant and entity is dormant, ignore.
        if (!should_run_dormant && is_dormant)
            continue;
        // Should not run on normal entity and entity is not dormant, ignore
        else if (!should_run_normal && !is_dormant)
            continue;

        // Avoid excessive calls by ignoring new checks if people are too close to eachother
        auto origin = ent->m_vecDormantOrigin();
        if (!origin)
            continue;
        bool should_check = true;
        for (auto &checked_origin : checked_origins)
        {
            // If this origin is closer than a quarter of the min HU (or less than 100 HU) to a cached one, don't go through all nav areas again
            // DistToSqr is much faster than DistTo which is why we use it here
            auto distance = selected_config.min;

            distance *= 0.25f;
            distance = std::max(100.0f, distance);

            // Square the distance
            distance *= distance;

            if ((*origin).DistToSqr(checked_origin) < distance)
            {
                should_check = false;
                break;
            }
        }
        if (!should_check)
            continue;

        // Now check which areas they are close to
        for (CNavArea &nav_area : navparser::NavEngine::getNavFile()->m_areas)
            if (nav_area.m_center.DistTo(*origin) < selected_config.min)
                (*navparser::NavEngine::getFreeBlacklist())[&nav_area] = is_dormant ? navparser::ENEMY_DORMANT : navparser::ENEMY_NORMAL;

        checked_origins.push_back(*origin);
    }
}

void LevelInit()
{
    // Make it run asap
    refresh_sniperspots_timer.last -= std::chrono::seconds(60);
    sniper_spots.clear();
}

// Roam around map
bool doRoam()
{
    // No sniper spots :shrug:
    if (!sniper_spots.size())
        return false;
    // Don't overwrite current roam
    if (navparser::NavEngine::current_priority == patrol)
        return false;

    // Randomly shuffle
    std::random_shuffle(sniper_spots.begin(), sniper_spots.end());

    if (navparser::NavEngine::navTo(sniper_spots[0], patrol))
        return true;

    return false;
}

// Check if an area is valid for stay near. the Third parameter is to save some performance.
bool isAreaValidForStayNear(Vector ent_origin, Vector area_origin, bool fix_local_z = true)
{
    if (fix_local_z)
        ent_origin.z += navparser::PLAYER_JUMP_HEIGHT;
    area_origin.z += navparser::PLAYER_JUMP_HEIGHT;

    // Do all the distance checks
    float distance = ent_origin.DistToSqr(area_origin);

    // Too close
    if (distance < selected_config.min * selected_config.min)
        return false;
    // Too far away
    if (distance > selected_config.max * selected_config.max)
        return false;
    // Attempt to vischeck
    if (!IsVectorVisibleNavigation(ent_origin, area_origin))
        return false;
    return true;
}

// Actual logic, used to de-duplicate code
bool stayNearTarget(CachedEntity *ent)
{
    auto ent_origin = ent->m_vecDormantOrigin();
    // No origin recorded, don't bother
    if (!ent_origin)
        return false;

    // Add the vischeck height
    ent_origin->z += navparser::PLAYER_JUMP_HEIGHT;

    // Use std::pair to avoid using the distance functions more than once
    std::vector<std::pair<CNavArea *, float>> good_areas{};

    for (auto &area : navparser::NavEngine::getNavFile()->m_areas)
    {
        auto area_origin = area.m_center;

        // Is this area valid for stay near purposes?
        if (!isAreaValidForStayNear(*ent_origin, area_origin, false))
            continue;

        float distance = (*ent_origin).DistToSqr(area_origin);
        // Good area found
        good_areas.push_back(std::pair<CNavArea *, float>(&area, distance));
    }
    // Sort based on distance
    if (selected_config.prefer_far)
        std::sort(good_areas.begin(), good_areas.end(), [](std::pair<CNavArea *, float> a, std::pair<CNavArea *, float> b) { return a.second > b.second; });
    else
        std::sort(good_areas.begin(), good_areas.end(), [](std::pair<CNavArea *, float> a, std::pair<CNavArea *, float> b) { return a.second < b.second; });

    // If we're not already pathing we should reallign with the center of the area
    bool should_path_to_local = !navparser::NavEngine::isPathing();
    // Try to path to all the good areas, based on distance
    for (auto &area : good_areas)
        if (navparser::NavEngine::navTo(area.first->m_center, staynear, true, should_path_to_local))
            return true;

    return false;
}

// Try to stay near enemies and stalk them (or in case of sniper, try to stay far from them
// and snipe them)
bool stayNear()
{
    static Timer staynear_cooldown{};
    static CachedEntity *previous_target = nullptr;

    // Stay near is off
    if (!stay_near)
        return false;
    // Don't constantly path, it's slow.
    // Far range classes do not need to repath nearly as often as close range ones.
    if (!staynear_cooldown.test_and_set(selected_config.prefer_far ? 2000 : 500))
        return navparser::NavEngine::current_priority == staynear;
    // Check and use our previous target if available
    if (CE_VALID(previous_target))
    {
        if (g_pPlayerResource->isAlive(previous_target->m_IDX) && previous_target->m_IDX != g_pLocalPlayer->entity_idx && g_pLocalPlayer->team != previous_target->m_iTeam() && player_tools::shouldTarget(previous_target))
        {
            auto ent_origin = previous_target->m_vecDormantOrigin();
            if (ent_origin)
            {
                // Check if current target area is valid
                if (navparser::NavEngine::isPathing())
                {
                    auto crumbs = navparser::NavEngine::getCrumbs();
                    // We cannot just use the last crumb, as it is always nullptr
                    if (crumbs->size() > 1)
                    {
                        auto last_crumb = (*crumbs)[crumbs->size() - 2];
                        // Area is still valid, stay on it
                        if (isAreaValidForStayNear(*ent_origin, last_crumb.navarea->m_center))
                            return true;
                    }
                }
                // Else Check our origin for validity (Only for ranged classes)
                else if (selected_config.prefer_far && isAreaValidForStayNear(*ent_origin, LOCAL_E->m_vecOrigin()))
                    return true;
            }
            // Else we try to path again
            if (stayNearTarget(previous_target))
                return true;
            // Failed, invalidate previous target and try others
            previous_target = nullptr;
        }
    }
    // Test all entities
    for (int i = 1; i <= g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_INVALID(ent) || i == g_pLocalPlayer->entity_idx || !g_pPlayerResource->isAlive(i) || g_pLocalPlayer->team == ent->m_iTeam() || !player_tools::shouldTarget(ent))
            continue;
        // Succeeded pathing
        if (stayNearTarget(ent))
        {
            previous_target = ent;
            return true;
        }
    }
    // Stay near failed to find any good targets, add extra delay
    staynear_cooldown.last += std::chrono::seconds(3);
    return false;
}

// Basically the same as isAreaValidForStayNear, but some restrictions lifted.
bool isAreaValidForSnipe(Vector ent_origin, Vector area_origin, bool fix_sentry_z = true)
{
    if (fix_sentry_z)
        ent_origin.z += 40.0f;
    area_origin.z += navparser::PLAYER_JUMP_HEIGHT;

    float distance = ent_origin.DistToSqr(area_origin);
    // Too close to be valid
    if (distance <= (1100.0f + navparser::HALF_PLAYER_WIDTH) * (1100.0f + navparser::HALF_PLAYER_WIDTH))
        return false;
    // Fails vischeck, bad
    if (!IsVectorVisibleNavigation(area_origin, ent_origin))
        return false;
    return true;
}

// Try to snipe the sentry
bool tryToSnipe(CachedEntity *ent)
{
    auto ent_origin = GetBuildingPosition(ent);
    // Add some z to dormant sentries as it only returns origin
    if (CE_BAD(ent))
        ent_origin.z += 40.0f;

    std::vector<std::pair<CNavArea *, float>> good_areas;
    for (auto &area : navparser::NavEngine::getNavFile()->m_areas)
    {
        // Not usable
        if (!isAreaValidForSnipe(ent_origin, area.m_center, false))
            continue;
        good_areas.push_back(std::pair<CNavArea *, float>(&area, area.m_center.DistToSqr(ent_origin)));
    }

    // Sort based on distance
    if (selected_config.prefer_far)
        std::sort(good_areas.begin(), good_areas.end(), [](std::pair<CNavArea *, float> a, std::pair<CNavArea *, float> b) { return a.second > b.second; });
    else
        std::sort(good_areas.begin(), good_areas.end(), [](std::pair<CNavArea *, float> a, std::pair<CNavArea *, float> b) { return a.second < b.second; });

    for (auto &area : good_areas)
        if (navparser::NavEngine::navTo(area.first->m_center, snipe_sentry))
            return true;
    return false;
}

// Is our target valid?
bool isSnipeTargetValid(CachedEntity *ent)
{
    return CE_VALID(ent) && ent->m_bAlivePlayer() && ent->m_iTeam() != g_pLocalPlayer->team && ent->m_iClassID() == CL_CLASS(CObjectSentrygun);
}

// Try to Snipe sentries
bool snipeSentries()
{
    static Timer sentry_snipe_cooldown;
    static CachedEntity *previous_target = nullptr;

    if (!snipe_sentries)
        return false;

    // Sentries don't move often, so we can use a slightly longer timer
    if (!sentry_snipe_cooldown.test_and_set(2000))
        return navparser::NavEngine::current_priority == snipe_sentry || isSnipeTargetValid(previous_target);

    if (isSnipeTargetValid(previous_target))
    {
        auto crumbs = navparser::NavEngine::getCrumbs();
        // We cannot just use the last crumb, as it is always nullptr
        if (crumbs->size() > 1)
        {
            auto last_crumb = (*crumbs)[crumbs->size() - 2];
            // Area is still valid, stay on it
            if (isAreaValidForSnipe(GetBuildingPosition(previous_target), last_crumb.navarea->m_center))
                return true;
        }
        if (tryToSnipe(previous_target))
            return true;
    }

    // Make sure we don't try to do it on shortrange classes unless specified
    if (!snipe_sentries_shortrange && (g_pLocalPlayer->clazz == tf_scout || g_pLocalPlayer->clazz == tf_pyro))
        return false;

    for (int i = g_IEngine->GetMaxClients() + 1; i < MAX_ENTITIES; i++)
    {
        CachedEntity *ent = ENTITY(i);
        // Invalid sentry
        if (!isSnipeTargetValid(ent))
            continue;
        // Succeeded in trying to snipe it
        if (tryToSnipe(ent))
        {
            previous_target = ent;
            return true;
        }
    }
    return false;
}

void CreateMove()
{
    if (!enabled || !navparser::NavEngine::isReady())
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;

    refreshSniperSpots();

    // Update the distance config
    switch (g_pLocalPlayer->clazz)
    {
    case tf_scout:
    case tf_heavy:
        selected_config = CONFIG_SHORT_RANGE;
        break;
    case tf_sniper:
        selected_config = CONFIG_SNIPER;
        break;
    default:
        selected_config = CONFIG_MID_RANGE;
    }

    updateEnemyBlacklist();

    // First priority should be getting health
    if (getHealth())
        return;
    // If we aren't getting health, get ammo
    else if (getAmmo())
        return;
    // Try to snipe sentries
    else if (snipeSentries())
        return;
    // Try to stalk enemies
    else if (stayNear())
        return;
    // Try to get health with a lower prioritiy
    else if (getHealth(true))
        return;
    // We have nothing else to do, roam
    else if (doRoam())
        return;
}

static InitRoutine init([]() { EC::Register(EC::CreateMove, CreateMove, "navbot_cm"); });

} // namespace hacks::tf2::NavBot
