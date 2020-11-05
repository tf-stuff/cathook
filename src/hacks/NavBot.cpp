#include "Settings.hpp"
#include "init.hpp"
#include "HookTools.hpp"
#include "interfaces.hpp"
#include "navparser.hpp"
#include "playerresource.h"
#include "localplayer.hpp"
#include "sdk.hpp"
#include "entitycache.hpp"
#include "CaptureLogic.hpp"
#include "PlayerTools.hpp"

namespace hacks::tf2::NavBot
{
static settings::Boolean enabled("navbot.enabled", "false");
static settings::Boolean search_health("navbot.search-health", "true");
static settings::Boolean search_ammo("navbot.search-ammo", "true");
static settings::Boolean stay_near("navbot.stay-near", "true");
static settings::Boolean capture_objectives("navbot.capture-objectives", "false");
static settings::Boolean snipe_sentries("navbot.snipe-sentries", "true");
static settings::Boolean snipe_sentries_shortrange("navbot.snipe-sentries.shortrange", "false");
static settings::Boolean escape_danger("navbot.escape-danger", "true");
static settings::Boolean escape_danger_ctf_cap("navbot.escape-danger.ctf-cap", "false");
static settings::Boolean autojump("navbot.autojump.enabled", "false");
static settings::Boolean primary_only("navbot.primary-only", "true");
static settings::Float jump_distance("navbot.autojump.trigger-distance", "300");
static settings::Int blacklist_delay("navbot.proximity-blacklist.delay", "500");
static settings::Int blacklist_delay_dormat("navbot.proximity-blacklist.delay-dormant", "1000");
static settings::Int blacklist_slightdanger_limit("navbot.proximity-blacklist.slight-danger.amount", "2");
#if ENABLE_VISUALS
static settings::Boolean draw_danger("navbot.draw-danger", "false");
#endif

enum Priority_list
{
    patrol = 5,
    lowprio_health,
    staynear,
    capture,
    snipe_sentry,
    ammo,
    health,
    danger
};

// Controls the bot parameters like distance from enemy
struct bot_class_config
{
    float min_full_danger;
    float min_slight_danger;
    float max;
    bool prefer_far;
};

// Sniper, Go close to enemy since we can oneshot him, but avoid if we have multiple
constexpr bot_class_config CONFIG_SNIPER{ 300.0f, 800.0f, FLT_MAX, true };
// A short range class like scout or heavy, run at the enemy
constexpr bot_class_config CONFIG_SHORT_RANGE{ 140.0f, 400.0f, 600.0f, false };
// A mid range class like the Soldier, don't get too close but also don't run too far away
constexpr bot_class_config CONFIG_MID_RANGE{ 200.0f, 400.0f, 4000.0f, true };

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

// Former is position, latter is until which tick it is ignored
std::vector<std::pair<Vector, int>> sniper_spots;

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
                sniper_spots.push_back(std::pair<Vector, int>(hiding_spot.m_pos, 0));
}

#if ENABLE_VISUALS
std::vector<Vector> slight_danger_drawlist_normal;
std::vector<Vector> slight_danger_drawlist_dormant;
#endif
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

    // Store the danger of the invidual nav areas
    std::unordered_map<CNavArea *, int> dormant_slight_danger;
    std::unordered_map<CNavArea *, int> normal_slight_danger;

    // This is used to cache Dangerous areas between ents
    std::unordered_map<CachedEntity *, std::vector<CNavArea *>> ent_marked_dormant_slight_danger;
    std::unordered_map<CachedEntity *, std::vector<CNavArea *>> ent_marked_normal_slight_danger;

    std::vector<std::pair<CachedEntity *, Vector>> checked_origins;
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

        // Find already dangerous marked areas by other entities
        auto to_loop = is_dormant ? &ent_marked_dormant_slight_danger : &ent_marked_normal_slight_danger;

        // Add new danger entries
        auto to_mark = is_dormant ? &dormant_slight_danger : &normal_slight_danger;

        for (auto &checked_origin : checked_origins)
        {
            // If this origin is closer than a quarter of the min HU (or less than 100 HU) to a cached one, don't go through all nav areas again
            // DistToSqr is much faster than DistTo which is why we use it here
            auto distance = selected_config.min_slight_danger;

            distance *= 0.25f;
            distance = std::max(100.0f, distance);

            // Square the distance
            distance *= distance;

            if ((*origin).DistToSqr(checked_origin.second) < distance)
            {
                should_check = false;

                bool is_absolute_danger = distance < selected_config.min_full_danger;
                if (!is_absolute_danger)
                    for (auto &area : (*to_loop)[checked_origin.first])
                    {
                        (*to_mark)[area]++;
                        if ((*to_mark)[area] >= *blacklist_slightdanger_limit)
                            (*navparser::NavEngine::getFreeBlacklist())[area] = is_dormant ? navparser::ENEMY_DORMANT : navparser::ENEMY_NORMAL;
                    }

                break;
            }
        }
        if (!should_check)
            continue;

        // Now check which areas they are close to
        for (CNavArea &nav_area : navparser::NavEngine::getNavFile()->m_areas)
        {
            float distance             = nav_area.m_center.DistTo(*origin);
            float slight_danger_dist   = selected_config.min_slight_danger;
            float absolute_danger_dist = selected_config.min_full_danger;

            // Not dangerous, Still don't bump
            if (!player_tools::shouldTarget(ent))
            {
                slight_danger_dist   = navparser::PLAYER_WIDTH * 2.0f;
                absolute_danger_dist = navparser::PLAYER_WIDTH * 2.0f;
            }

            // Too close to count as slight danger
            bool is_absolute_danger = distance < absolute_danger_dist;
            if (distance < slight_danger_dist)
            {
                // Add as marked area
                (*to_loop)[ent].push_back(&nav_area);

                // Just slightly dangerous, only mark as such if it's clear
                if (!is_absolute_danger)
                {
                    (*to_mark)[&nav_area]++;
                    if ((*to_mark)[&nav_area] < *blacklist_slightdanger_limit)
                        continue;
                }
                (*navparser::NavEngine::getFreeBlacklist())[&nav_area] = is_dormant ? navparser::ENEMY_DORMANT : navparser::ENEMY_NORMAL;
            }
        }
        checked_origins.push_back(std::pair<CachedEntity *, Vector>(ent, *origin));
    }
#if ENABLE_VISUALS
    if (should_run_dormant)
        slight_danger_drawlist_dormant.clear();
    if (should_run_normal)
        slight_danger_drawlist_normal.clear();

    // Store slight danger areas for drawing
    if (normal_slight_danger.size())
    {
        for (auto &area : normal_slight_danger)
            if (area.second < *blacklist_slightdanger_limit)
                slight_danger_drawlist_normal.push_back(area.first->m_center);
    }
    if (dormant_slight_danger.size())
    {
        for (auto &area : dormant_slight_danger)
            if (area.second < *blacklist_slightdanger_limit)
                slight_danger_drawlist_dormant.push_back(area.first->m_center);
    }
#endif
}

// Roam around map
bool doRoam()
{
    static Timer fail_timer;
    // No sniper spots :shrug:
    if (!sniper_spots.size())
        return false;
    // Failed recently, wait a while
    if (!fail_timer.check(1000))
        return false;
    // Don't overwrite current roam
    if (navparser::NavEngine::current_priority == patrol)
        return false;

    // Get closest sniper spots
    std::sort(sniper_spots.begin(), sniper_spots.end(), [](std::pair<Vector, int> a, std::pair<Vector, int> b) { return a.first.DistTo(g_pLocalPlayer->v_Origin) < b.first.DistTo(g_pLocalPlayer->v_Origin); });

    bool tried_pathing = false;
    for (int i = 0; i < sniper_spots.size(); i++)
    {
        // Timed out
        if (sniper_spots[i].second > g_GlobalVars->tickcount)
            continue;

        tried_pathing = true;

        // Ignore for spot for 30s
        sniper_spots[i].second = TICKCOUNT_TIMESTAMP(30);
        if (navparser::NavEngine::navTo(sniper_spots[i].first, patrol))
            return true;
    }

    // Every sniper spot is on cooldown, refresh cooldowns
    if (!tried_pathing)
        for (auto &spot : sniper_spots)
            spot.second = 0;
    // Failed, time out
    fail_timer.update();

    return false;
}

// Check if an area is valid for stay near. the Third parameter is to save some performance.
bool isAreaValidForStayNear(Vector ent_origin, CNavArea *area, bool fix_local_z = true)
{
    if (fix_local_z)
        ent_origin.z += navparser::PLAYER_JUMP_HEIGHT;
    auto area_origin = area->m_center;
    area_origin.z += navparser::PLAYER_JUMP_HEIGHT;

    // Do all the distance checks
    float distance = ent_origin.DistToSqr(area_origin);

    // Too close
    if (distance < selected_config.min_full_danger * selected_config.min_full_danger)
        return false;
    // Blacklisted
    if (navparser::NavEngine::getFreeBlacklist()->find(area) != navparser::NavEngine::getFreeBlacklist()->end())
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
        if (!isAreaValidForStayNear(*ent_origin, &area, false))
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

// A bunch of basic checks to ensure we don't try to target an invalid entity
bool isStayNearTargetValid(CachedEntity *ent)
{
    return CE_VALID(ent) && g_pPlayerResource->isAlive(ent->m_IDX) && ent->m_IDX != g_pLocalPlayer->entity_idx && g_pLocalPlayer->team != ent->m_iTeam() && player_tools::shouldTarget(ent) && !IsPlayerInvisible(ent) && !IsPlayerInvulnerable(ent);
}

// Try to stay near enemies and stalk them (or in case of sniper, try to stay far from them
// and snipe them)
bool stayNear()
{
    PROF_SECTION(stayNear)
    static Timer staynear_cooldown{};
    static CachedEntity *previous_target = nullptr;

    // Stay near us expensive so we have to cache. We achieve this by only checking a pre-determined amount of players every CreateMove
    constexpr int MAX_STAYNEAR_CHECKS_RANGE = 3;
    constexpr int MAX_STAYNEAR_CHECKS_CLOSE = 2;
    static int lowest_check_index           = 0;

    // Stay near is off
    if (!stay_near)
        return false;
    // Don't constantly path, it's slow.
    // Far range classes do not need to repath nearly as often as close range ones.
    if (!staynear_cooldown.test_and_set(selected_config.prefer_far ? 2000 : 500))
        return navparser::NavEngine::current_priority == staynear;

    // Too high priority, so don't try
    if (navparser::NavEngine::current_priority > staynear)
        return false;

    // Check and use our previous target if available
    if (isStayNearTargetValid(previous_target))
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
                    if (isAreaValidForStayNear(*ent_origin, last_crumb.navarea))
                        return true;
                }
            }
            // Else Check our origin for validity (Only for ranged classes)
            else if (selected_config.prefer_far && isAreaValidForStayNear(*ent_origin, navparser::NavEngine::findClosestNavSquare(LOCAL_E->m_vecOrigin())))
                return true;
        }
        // Else we try to path again
        if (stayNearTarget(previous_target))
            return true;
        // Failed, invalidate previous target and try others
        previous_target = nullptr;
    }

    auto advance_count = selected_config.prefer_far ? MAX_STAYNEAR_CHECKS_RANGE : MAX_STAYNEAR_CHECKS_CLOSE;

    // Ensure it is in bounds and also wrap around
    if (lowest_check_index > g_IEngine->GetMaxClients())
        lowest_check_index = 0;

    int calls = 0;
    // Test all entities
    for (int i = lowest_check_index; i <= g_IEngine->GetMaxClients(); i++)
    {
        if (calls >= advance_count)
            break;
        calls++;
        lowest_check_index++;
        CachedEntity *ent = ENTITY(i);
        if (!isStayNearTargetValid(ent))
        {
            calls--;
            continue;
        }
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

enum capture_type
{
    no_capture,
    ctf,
    payload,
    controlpoints
};

static capture_type current_capturetype = no_capture;
// Overwrite to return true for payload carts as an example
static bool overwrite_capture = false;
// Doomsday is a ctf + payload map which breaks capturing...
static bool is_doomsday = false;

std::optional<Vector> getCtfGoal(int our_team, int enemy_team)
{
    // Get Flag related information
    auto status   = flagcontroller::getStatus(enemy_team);
    auto position = flagcontroller::getPosition(enemy_team);
    auto carrier  = flagcontroller::getCarrier(enemy_team);

    // No flag :(
    if (!position)
        return std::nullopt;

    current_capturetype = ctf;

    // Flag is taken by us
    if (status == TF_FLAGINFO_STOLEN)
    {
        // CTF is the current capture type.
        if (carrier == LOCAL_E)
        {
            // Return our capture point location
            auto team_flag = flagcontroller::getFlag(our_team);
            return team_flag.spawn_pos;
        }
    }
    // Get the flag if not taken by us already
    else
    {
        return position;
    }
    return std::nullopt;
}

std::optional<Vector> getPayloadGoal(int our_team)
{
    auto position = plcontroller::getClosestPayload(g_pLocalPlayer->v_Origin, our_team);
    // No payloads found :(
    if (!position)
        return std::nullopt;
    current_capturetype = payload;

    // Adjust position so it's not floating high up, provided the local player is close.
    if (LOCAL_E->m_vecOrigin().DistTo(*position) <= 150.0f)
        (*position).z = LOCAL_E->m_vecOrigin().z;
    // If close enough, don't move (mostly due to lifts)
    if ((*position).DistTo(LOCAL_E->m_vecOrigin()) <= 50.0f)
    {
        overwrite_capture = true;
        return std::nullopt;
    }
    else
        return position;
}

std::optional<Vector> getControlPointGoal(int our_team)
{
    static Vector previous_position(0.0f);
    static Vector randomized_position(0.0f);

    auto position = cpcontroller::getClosestControlPoint(g_pLocalPlayer->v_Origin, our_team);
    // No points found :(
    if (!position)
        return std::nullopt;

    // Randomize where on the point we walk a bit so bots don't just stand there
    if (previous_position != *position || !navparser::NavEngine::isPathing())
    {
        previous_position   = *position;
        randomized_position = *position;
        randomized_position.x += RandomFloat(0.0f, 100.0f);
        randomized_position.y += RandomFloat(0.0f, 100.0f);
    }

    current_capturetype = controlpoints;
    // Try to navigate
    return randomized_position;
}

// Try to capture objectives
bool captureObjectives()
{
    static Timer capture_timer;
    static Vector previous_target(0.0f);
    // Not active or on a doomsday map
    if (!capture_objectives || is_doomsday || !capture_timer.check(2000))
        return false;

    // Priority too high, don't try
    if (navparser::NavEngine::current_priority > capture)
        return false;

    // Where we want to go
    std::optional<Vector> target;

    int our_team   = g_pLocalPlayer->team;
    int enemy_team = our_team == TEAM_BLU ? TEAM_RED : TEAM_BLU;

    current_capturetype = no_capture;
    overwrite_capture   = false;

    // Run ctf logic
    target = getCtfGoal(our_team, enemy_team);
    // Not ctf, run payload
    if (current_capturetype == no_capture)
    {
        target = getPayloadGoal(our_team);
        // Not payload, run control points
        if (current_capturetype == no_capture)
        {
            target = getControlPointGoal(our_team);
        }
    }

    // Overwritten, for example because we are currently on the payload, cancel any sort of pathing and return true
    if (overwrite_capture)
    {
        navparser::NavEngine::cancelPath();
        return true;
    }
    // No target, bail and set on cooldown
    else if (!target)
    {
        capture_timer.update();
        return false;
    }
    // If priority is not capturing or we have a new target, try to path there
    else if (navparser::NavEngine::current_priority != capture || *target != previous_target)
    {
        if (navparser::NavEngine::navTo(*target, capture, true, !navparser::NavEngine::isPathing()))
        {
            previous_target = *target;
            return true;
        }
        else
            capture_timer.update();
    }
    return false;
}

// Run away from dangerous areas
bool escapeDanger()
{
    if (!escape_danger)
        return false;
    // Don't escape while we have the intel
    if (!escape_danger_ctf_cap)
    {
        auto flag_carrier = flagcontroller::getCarrier(g_pLocalPlayer->team);
        if (flag_carrier == LOCAL_E)
            return false;
    }
    auto *local_nav = navparser::NavEngine::findClosestNavSquare(g_pLocalPlayer->v_Origin);
    auto blacklist  = navparser::NavEngine::getFreeBlacklist();

    // In danger, try to run
    if (blacklist->find(local_nav) != blacklist->end())
    {
        static CNavArea *target_area = nullptr;
        // Already running and our target is still valid
        if (navparser::NavEngine::current_priority == danger && blacklist->find(target_area) == blacklist->end())
            return true;

        std::vector<CNavArea *> nav_areas_ptr;
        // Copy a ptr list (sadly cat_nav_init exists so this cannot be only done once)
        for (auto &nav_area : navparser::NavEngine::getNavFile()->m_areas)
            nav_areas_ptr.push_back(&nav_area);

        // Sort by distance
        std::sort(nav_areas_ptr.begin(), nav_areas_ptr.end(), [](CNavArea *a, CNavArea *b) { return a->m_center.DistToSqr(g_pLocalPlayer->v_Origin) < b->m_center.DistToSqr(g_pLocalPlayer->v_Origin); });

        int calls = 0;
        // Try to path away
        for (auto area : nav_areas_ptr)
        {
            if (blacklist->find(area) == blacklist->end())
            {
                // only try the 5 closest valid areas though, something is wrong if this fails
                calls++;
                if (calls > 5)
                    break;
                if (navparser::NavEngine::navTo(area->m_center, danger))
                {
                    target_area = area;
                    return true;
                }
            }
        }
    }
    // No longer in danger
    else if (navparser::NavEngine::current_priority == danger)
        navparser::NavEngine::cancelPath();
    return false;
}

static std::pair<CachedEntity *, float> getNearestPlayerDistance()
{
    float distance         = FLT_MAX;
    CachedEntity *best_ent = nullptr;
    for (int i = 1; i <= g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_VALID(ent) && ent->m_vecDormantOrigin() && ent->m_bAlivePlayer() && ent->m_bEnemy() && g_pLocalPlayer->v_Origin.DistTo(ent->m_vecOrigin()) < distance && player_tools::shouldTarget(ent) && !IsPlayerInvisible(ent))
        {
            distance = g_pLocalPlayer->v_Origin.DistTo(*ent->m_vecDormantOrigin());
            best_ent = ent;
        }
    }
    return { best_ent, distance };
}

static void autoJump()
{
    if (!autojump)
        return;
    static Timer last_jump{};
    if (!last_jump.test_and_set(200))
        return;

    if (getNearestPlayerDistance().second <= *jump_distance)
        current_user_cmd->buttons |= IN_JUMP | IN_DUCK;
}

enum slots
{
    primary   = 1,
    secondary = 2,
    melee     = 3
};
static slots getBestSlot(slots active_slot)
{
    auto nearest = getNearestPlayerDistance();
    switch (g_pLocalPlayer->clazz)
    {
    case tf_scout:
        return primary;
    case tf_heavy:
        return primary;
    case tf_medic:
        return secondary;
    case tf_spy:
    {
        if (nearest.second > 200 && active_slot == primary)
            return active_slot;
        else if (nearest.second >= 250)
            return primary;
        else
            return melee;
    }
    case tf_sniper:
    {
        // Have a Huntsman, Always use primary
        if (HasWeapon(LOCAL_E, 56) || HasWeapon(LOCAL_E, 1005) || HasWeapon(LOCAL_E, 1092))
            return primary;

        if (nearest.second <= 300 && nearest.first->m_iHealth() < 75)
            return secondary;
        else if (nearest.second <= 400 && nearest.first->m_iHealth() < 75)
            return active_slot;
        else
            return primary;
    }
    case tf_pyro:
    {
        if (nearest.second > 450 && active_slot == secondary)
            return active_slot;
        else if (nearest.second <= 550)
            return primary;
        else
            return secondary;
    }
    case tf_soldier:
    {
        if (nearest.second <= 200)
            return secondary;
        else if (nearest.second <= 300)
            return active_slot;
        else
            return primary;
    }
    default:
    {
        if (nearest.second <= 400)
            return secondary;
        else if (nearest.second <= 500)
            return active_slot;
        else
            return primary;
    }
    }
}

static void updateSlot()
{
    static Timer slot_timer{};
    if (!slot_timer.test_and_set(300))
        return;
    if (CE_GOOD(LOCAL_E) && !HasCondition<TFCond_HalloweenGhostMode>(LOCAL_E) && CE_GOOD(LOCAL_W) && LOCAL_E->m_bAlivePlayer())
    {
        IClientEntity *weapon = RAW_ENT(LOCAL_W);
        // IsBaseCombatWeapon()
        if (re::C_BaseCombatWeapon::IsBaseCombatWeapon(weapon))
        {
            int slot    = re::C_BaseCombatWeapon::GetSlot(weapon) + 1;
            int newslot = getBestSlot(static_cast<slots>(slot));
            if (slot != newslot)
                g_IEngine->ClientCmd_Unrestricted(format("slot", newslot).c_str());
        }
    }
}

void CreateMove()
{
    if (!enabled || !navparser::NavEngine::isReady())
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer() || HasCondition<TFCond_HalloweenGhostMode>(LOCAL_E))
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
    {
        bool has_sniper = true;
        // Using a Huntsman, use close range config
        if (HasWeapon(LOCAL_E, 56) || HasWeapon(LOCAL_E, 1005) || HasWeapon(LOCAL_E, 1092))
            has_sniper = false;
        if (has_sniper)
            selected_config = CONFIG_SNIPER;
        else
            selected_config = CONFIG_SHORT_RANGE;
        break;
    }
    default:
        selected_config = CONFIG_MID_RANGE;
    }

    updateSlot();
    autoJump();
    updateEnemyBlacklist();

    // Try to escape danger first of all
    if (escapeDanger())
        return;
    // Second priority should be getting health
    else if (getHealth())
        return;
    // If we aren't getting health, get ammo
    else if (getAmmo())
        return;
    // Try to snipe sentries
    else if (snipeSentries())
        return;
    // Try to capture objectives
    else if (captureObjectives())
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

void LevelInit()
{
    // Make it run asap
    refresh_sniperspots_timer.last -= std::chrono::seconds(60);
    sniper_spots.clear();
    is_doomsday = false;
    // Doomsday sucks
    // TODO: add proper doomsday implementation
    auto map_name = std::string(g_IEngine->GetLevelName());
    if (g_IEngine->GetLevelName() && map_name.find("sd_doomsday") != map_name.npos)
        is_doomsday = true;
}
#if ENABLE_VISUALS
void Draw()
{
    if (!draw_danger || !navparser::NavEngine::isReady())
        return;
    for (auto &area : slight_danger_drawlist_normal)
    {
        Vector out;

        if (draw::WorldToScreen(area, out))
            draw::Rectangle(out.x - 2.0f, out.y - 2.0f, 4.0f, 4.0f, colors::orange);
    }
    for (auto &area : slight_danger_drawlist_dormant)
    {
        Vector out;

        if (draw::WorldToScreen(area, out))
            draw::Rectangle(out.x - 2.0f, out.y - 2.0f, 4.0f, 4.0f, colors::orange);
    }
    for (auto &area : *navparser::NavEngine::getFreeBlacklist())
    {
        Vector out;

        if (draw::WorldToScreen(area.first->m_center, out))
            draw::Rectangle(out.x - 2.0f, out.y - 2.0f, 4.0f, 4.0f, colors::red);
    }
}
#endif

static InitRoutine init([]() {
    EC::Register(EC::CreateMove, CreateMove, "navbot_cm");
    EC::Register(EC::LevelInit, LevelInit, "navbot_levelinit");
#if ENABLE_VISUALS
    EC::Register(EC::Draw, Draw, "navbot_draw");
#endif
    LevelInit();
});

} // namespace hacks::tf2::NavBot
