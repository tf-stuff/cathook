#include "Settings.hpp"
#include "init.hpp"
#include "HookTools.hpp"
#include "interfaces.hpp"
#include "navparser.hpp"
#include "playerresource.h"
#include "localplayer.hpp"
#include "sdk.hpp"
#include "entitycache.hpp"

namespace hacks::tf2::NavBot
{
static settings::Boolean enabled("navbot.enabled", "false");
static settings::Boolean search_health("navbot.search-health", "true");
static settings::Boolean search_ammo("navbot.search-ammo", "true");

enum Priority_list
{
    patrol = 5,
    staynear,
    ammo,
    health
};

// Should we search health at all?
bool shouldSearchHealth()
{
    // Priority too high
    if (navparser::NavEngine::current_priority > health)
        return false;
    float health_percent = LOCAL_E->m_iHealth() / g_pPlayerResource->GetMaxHealth(LOCAL_E);
    // Get health when below 65%, or below 90% and just patroling
    return health_percent < 0.64f || (navparser::NavEngine::current_priority <= 5 && health_percent <= 0.90f);
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
bool getHealth()
{
    if (shouldSearchHealth())
    {
        // Already pathing
        if (navparser::NavEngine::current_priority == health)
            return true;
        auto healthpacks = getEntities({ ITEM_HEALTH_SMALL, ITEM_HEALTH_MEDIUM, ITEM_HEALTH_LARGE });
        for (auto healthpack : healthpacks)
            if (navparser::NavEngine::navTo(healthpack->m_vecOrigin(), health))
                return true;
    }
    else if (navparser::NavEngine::current_priority == health)
        navparser::NavEngine::cancelPath();
    return false;
}

// Find ammo if needed
bool getAmmo()
{
    if (shouldSearchAmmo())
    {
        // Already pathing
        if (navparser::NavEngine::current_priority == ammo)
            return true;
        auto ammopacks = getEntities({ ITEM_AMMO_SMALL, ITEM_AMMO_MEDIUM, ITEM_AMMO_LARGE });
        for (auto ammopack : ammopacks)
            // If we succeeed,
            if (navparser::NavEngine::navTo(ammopack->m_vecOrigin(), ammo))
                return true;
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
    if (navparser::NavEngine::current_priority == 5)
        return false;

    // Randomly shuffle
    std::random_shuffle(sniper_spots.begin(), sniper_spots.end());

    if (navparser::NavEngine::navTo(sniper_spots[0]))
        return true;

    return false;
}

void CreateMove()
{
    if (!enabled || !navparser::NavEngine::isReady())
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;

    refreshSniperSpots();
    // TODO: staynear caching here

    // First priority should be getting health
    if (getHealth())
        return;
    // If we aren't getting health, get ammo
    else if (getAmmo())
        return;
    // TODO, staynear before the else if below
    // We have nothing else to do, roam
    else if (doRoam())
        return;
}

static InitRoutine init([]() { EC::Register(EC::CreateMove, CreateMove, "navbot_cm"); });

} // namespace hacks::tf2::NavBot
