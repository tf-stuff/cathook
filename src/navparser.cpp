/*
    This file is part of Cathook.

    Cathook is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Cathook is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Cathook. If not, see <https://www.gnu.org/licenses/>.
*/

// Codeowners: TotallyNotElite

#include "common.hpp"
#include "micropather.h"
#include "CNavFile.h"
#if ENABLE_VISUALS
#include "drawing.hpp"
#endif

#include <memory>
#include <boost/container_hash/hash.hpp>

namespace navparser
{

constexpr float PLAYER_WIDTH       = 49;
constexpr float HALF_PLAYER_WIDTH  = PLAYER_WIDTH / 2.0f;
constexpr float PLAYER_JUMP_HEIGHT = 41.5f;

static settings::Boolean enabled("nav.enabled", "false");
static settings::Boolean draw("nav.draw", "false");
static settings::Boolean draw_debug_areas("nav.draw.debug-areas", "false");
static settings::Boolean log_pathing{ "nav.log", "false" };
static settings::Int stuck_time{ "nav.stuck-time", "1000" };

// Cast a Ray and return if it hit
static bool CastRay(Vector origin, Vector endpos, unsigned mask, ITraceFilter *filter)
{
    trace_t trace;
    Ray_t ray;

    ray.Init(origin, endpos);

    // This was found to be So inefficient that it is literally unusable for our purposes. it is almost 1000x slower than the above.
    // ray.Init(origin, target, -right * HALF_PLAYER_WIDTH, right * HALF_PLAYER_WIDTH);

    PROF_SECTION(IEVV_TraceRay);
    g_ITrace->TraceRay(ray, mask, filter, &trace);

    return trace.DidHit();
}

// Vischeck that considers player width
static bool IsPlayerPassableNavigation(Vector origin, Vector target, unsigned int mask = MASK_PLAYERSOLID)
{
    Vector tr = target - origin;
    Vector angles;
    VectorAngles(tr, angles);

    Vector forward, right, up;
    AngleVectors3(VectorToQAngle(angles), &forward, &right, &up);
    right.z = 0;

    // We want to keep the same angle for these two bounding box traces
    Vector relative_endpos = forward * tr.Length();

    Vector left_ray_origin = origin - right * HALF_PLAYER_WIDTH;
    Vector left_ray_endpos = left_ray_origin + relative_endpos;

    // Left ray hit something
    if (CastRay(left_ray_origin, left_ray_endpos, mask, &trace::filter_navigation))
        return false;

    Vector right_ray_origin = origin + right * HALF_PLAYER_WIDTH;
    Vector right_ray_endpos = right_ray_origin + relative_endpos;

    // Return if the right ray hit something
    return !CastRay(right_ray_origin, right_ray_endpos, mask, &trace::filter_navigation);
}

enum class NavState
{
    Unavailable = 0,
    Active
};

struct CachedConnection
{
    int expire_tick;
    bool vischeck_state;
};

struct ConnectionInfo
{
    enum State
    {
        // Tried using this connection, failed for some reason
        STUCK,
    };
    int expire_tick;
    State state;
};

// Returns corrected "current_pos"
Vector handleDropdown(Vector current_pos, Vector next_pos)
{
    Vector to_target = (next_pos - current_pos);
    // Only do it if we'd fall quite a bit
    if (-to_target.z > PLAYER_JUMP_HEIGHT)
    {
        to_target.z = 0;
        to_target.NormalizeInPlace();
        Vector angles;
        VectorAngles(to_target, angles);
        current_pos = GetForwardVector(current_pos, angles, PLAYER_WIDTH);

        // Ensure current_pos does not get placed infront of next_pos
        Vector new_angles;

        Vector new_to_target = (next_pos - current_pos);
        new_to_target.z      = 0;
        new_to_target.NormalizeInPlace();

        VectorAngles(new_to_target, new_angles);
        // If the angles changed, we must have overshot
        if (angles != new_angles)
            current_pos = next_pos;
    }
    return current_pos;
}

class navPoints
{
public:
    Vector current;
    Vector center;
    Vector next;
    navPoints(Vector A, Vector B, Vector C) : current(A), center(B), next(C){};
};

// This function ensures that vischeck and pathing use the same logic.
navPoints determinePoints(CNavArea *current, CNavArea *next)
{
    auto area_center = current->m_center;
    auto next_center = next->m_center;
    // Gets a vector on the edge of the current area that is as close as possible to the center of the next area
    auto area_closest = current->getNearestPoint(next_center.AsVector2D());
    // Do the same for the other area
    auto next_closest = next->getNearestPoint(area_center.AsVector2D());

    // Use one of them as a center point, the one that is either x or y alligned with a center
    // Of the areas.
    // This will avoid walking into walls.
    auto center_point = area_closest;
    // Determine if alligned, if not, use the other one as the center point
    if (center_point.x != area_center.x && center_point.y != area_center.y && center_point.x != next_center.x && center_point.y != next_center.y)
        center_point = next_closest;

    return navPoints(area_center, center_point, next_center);
};

class Map : public micropather::Graph
{
public:
    CNavFile navfile;
    NavState state;
    micropather::MicroPather pather{ this, 3000, 6, true };
    std::string mapname;
    std::unordered_map<std::pair<CNavArea *, CNavArea *>, CachedConnection, boost::hash<std::pair<CNavArea *, CNavArea *>>> vischeck_cache;

    Map(const char *mapname) : navfile(mapname), mapname(mapname)
    {
        if (!navfile.m_isOK)
            state = NavState::Unavailable;
        else
            state = NavState::Active;
    }
    float LeastCostEstimate(void *start, void *end) override
    {
        return reinterpret_cast<CNavArea *>(start)->m_center.DistTo(reinterpret_cast<CNavArea *>(end)->m_center);
    }
    void AdjacentCost(void *main, std::vector<micropather::StateCost> *adjacent) override
    {
        CNavArea &area = *reinterpret_cast<CNavArea *>(main);
        for (NavConnect &connection : area.m_connections)
        {
            auto points = determinePoints(&area, connection.area);

            float height_diff = points.current.z - points.center.z;
            bool allowed      = height_diff <= -PLAYER_JUMP_HEIGHT;

            // Too high for us to jump!
            if (height_diff > PLAYER_JUMP_HEIGHT)
                continue;

            // Apply dropdown
            points.current = handleDropdown(points.current, points.center);
            points.center  = handleDropdown(points.center, points.next);

            points.current.z += PLAYER_JUMP_HEIGHT;
            points.center.z += PLAYER_JUMP_HEIGHT;
            points.next.z += PLAYER_JUMP_HEIGHT;

            auto key    = std::pair<CNavArea *, CNavArea *>(&area, connection.area);
            auto cached = vischeck_cache.find(key);
            if (cached != vischeck_cache.end())
            {
                if (cached->second.vischeck_state || allowed)
                {
                    float cost = connection.area->m_center.DistTo(area.m_center);
                    adjacent->push_back(micropather::StateCost{ reinterpret_cast<void *>(connection.area), cost });
                }
            }
            else
            {
                // 1. Check if this is a dropdown. It's in our best interest to trust the navmesh about dropdowns (bool allowed statement above)
                // 2. If not a dropdown, check if there is direct line of sight
                if (IsPlayerPassableNavigation(points.current, points.center) && IsPlayerPassableNavigation(points.center, points.next))
                {
                    vischeck_cache[key] = { g_GlobalVars->tickcount + int(10 / g_GlobalVars->interval_per_tick), true };
                    allowed             = true;
                }
                else
                {
                    vischeck_cache[key] = { g_GlobalVars->tickcount + int(10 / g_GlobalVars->interval_per_tick), false };
                }
                if (allowed)
                {
                    float cost = points.next.DistTo(points.current);
                    adjacent->push_back(micropather::StateCost{ reinterpret_cast<void *>(connection.area), cost });
                }
            }
        }
    }

    // Function for getting closest Area to player, aka "LocalNav"
    CNavArea *findClosestNavSquare(const Vector &vec)
    {
        auto vec_corrected = vec;
        vec_corrected.z += PLAYER_JUMP_HEIGHT;
        float ovBestDist = FLT_MAX, bestDist = FLT_MAX;
        // If multiple candidates for LocalNav have been found, pick the closest
        CNavArea *ovBestSquare = nullptr, *bestSquare = nullptr;
        for (auto &i : navfile.m_areas)
        {
            float dist = i.m_center.DistTo(vec);
            if (dist < bestDist)
            {
                bestDist   = dist;
                bestSquare = &i;
            }
            auto center_corrected = i.m_center;
            center_corrected.z += PLAYER_JUMP_HEIGHT;
            // Check if we are within x and y bounds of an area
            if (ovBestDist < dist || !i.IsOverlapping(vec) || !IsVectorVisibleNavigation(vec_corrected, center_corrected))
            {
                continue;
            }
            ovBestDist   = dist;
            ovBestSquare = &i;
        }
        if (!ovBestSquare)
            ovBestSquare = bestSquare;

        return ovBestSquare;
    }
    std::vector<void *> findPath(CNavArea *local, CNavArea *dest)
    {
        using namespace std::chrono;

        if (state != NavState::Active)
            return {};

        if (log_pathing)
        {
            logging::Info("Start: (%f,%f,%f)", local->m_center.x, local->m_center.y, local->m_center.z);
            logging::Info("End: (%f,%f,%f)", dest->m_center.x, dest->m_center.y, dest->m_center.z);
        }

        std::vector<void *> pathNodes;
        float cost;

        time_point begin_pathing = high_resolution_clock::now();
        int result               = pather.Solve(reinterpret_cast<void *>(local), reinterpret_cast<void *>(dest), &pathNodes, &cost);
        long long timetaken      = duration_cast<nanoseconds>(high_resolution_clock::now() - begin_pathing).count();
        if (log_pathing)
            logging::Info("Pathing: Pather result: %i. Time taken (NS): %lld", result, timetaken);
        // If no result found, return empty Vector
        if (0 == micropather::MicroPather::NO_SOLUTION)
            return {};

        return pathNodes;
    }

    void updateIgnores()
    {
        static Timer despam;
        if (!despam.test_and_set(1000))
            return;
        bool erased = false;
        // When we switch to c++20, we can use std::erase_if TODO: FIXME
        for (auto it = begin(vischeck_cache); it != end(vischeck_cache);)
        {
            if (it->second.expire_tick < g_GlobalVars->tickcount)
            {
                it     = vischeck_cache.erase(it); // previously this was something like m_map.erase(it++);
                erased = true;
            }
            else
                ++it;
        }
        if (erased)
            pather.Reset();
    }

    /*bool addIgnoreTime(CNavArea *begin, CNavArea *end, Timer &time)
    {
        if (!begin || !end)
        {
            return true;
        }
        using namespace std::chrono;
        // Check if connection is already known
        if (ignores.find({ begin, end }) == ignores.end())
        {
            ignores[{ begin, end }] = {};
        }
        ignoredata &connection = ignores[{ begin, end }];
        connection.stucktime += duration_cast<milliseconds>(system_clock::now() - time.last).count();
        if (connection.stucktime >= *stuck_time)
        {
            logging::Info("Ignored Connection %i-%i", begin->m_id, end->m_id);
            return addTime(connection, explicit_ignored);
        }
        return false;
    }*/

    void Reset()
    {
        vischeck_cache.clear();
        pather.Reset();
    }

    // Uncesseray thing that is sadly necessary
    void PrintStateInfo(void *) override
    {
    }
};

struct Crumb
{
    CNavArea *navarea;
    Vector vec;
};

namespace NavEngine
{
std::unique_ptr<Map> map;
std::vector<Crumb> crumbs;

bool isReady()
{
    return enabled && map && map->state == NavState::Active;
}

static Timer inactivity{};

bool navTo(const Vector &destination, int priority, bool should_repath, bool nav_to_local, bool is_repath)
{
    if (!isReady())
        return false;
    crumbs.clear();

    CNavArea *start_area = map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
    CNavArea *dest_area  = map->findClosestNavSquare(destination);

    if (!start_area || !dest_area)
        return false;
    auto path = map->findPath(start_area, dest_area);
    if (path.empty())
        return false;

    if (!nav_to_local)
    {
        path.erase(path.begin());
        if (path.empty())
            return false;
    }
    for (size_t i = 0; i < path.size(); i++)
    {
        CNavArea *area = reinterpret_cast<CNavArea *>(path.at(i));

        // All entries besides the last need an extra crumb
        if (i != path.size() - 1)
        {
            CNavArea *next_area = (CNavArea *) path.at(i + 1);

            auto points = determinePoints(area, next_area);

            points.current = handleDropdown(points.current, points.center);
            points.center  = handleDropdown(points.center, points.next);

            crumbs.push_back({ area, std::move(points.current) });
            crumbs.push_back({ area, std::move(points.center) });
        }
        else
            crumbs.push_back({ area, area->m_center });
    }
    crumbs.push_back({ nullptr, destination });
    inactivity.update();

    return true;
}

// Code to detect "complicated" dropdowns
// We consider something a complicated dropdown, if the x/y position of crumbs[1] is "behind" crumbs[0]
/*bool isComplicatedDropdown()
{
    Vector to_target = (crumbs[1] - crumbs[0]);
    to_target.z      = 0;
    to_target.NormalizeInPlace();
    Vector angles;
    VectorAngles(to_target, angles);
    auto vecTargetForward = GetForwardVector(crumbs[0], angles, 1.0f);
    vecTargetForward.z    = 0;

    return DotProduct(to_target, vecTargetForward) <= 0.0f;
}*/

static Timer last_jump{};
// Used to determine if we want to jump or if we want to crouch
static bool crouch          = false;
static int ticks_since_jump = 0;
static float last_dist;

static void FollowCrumbs()
{
    size_t crumbs_amount = crumbs.size();

    if (!crumbs_amount)
        return;

    // We are close enough to the crumb to have reached it
    if (crumbs[0].vec.DistTo(g_pLocalPlayer->v_Origin) < 50)
    {
        crumbs.erase(crumbs.begin());
        if (!--crumbs_amount)
            return;
        inactivity.update();
    }
    // If we make any progress at all, reset this
    else
    {
        Vector vel;
        velocity::EstimateAbsVelocity(RAW_ENT(LOCAL_E), vel);
        if (!vel.IsZero(100.0f))
            inactivity.update();
    }

    // Detect when jumping is necessary.
    // 1. No jumping if zoomed (or revved)
    // 2. Jump if its necessary to do so based on z values
    // 3. Jump if stuck (not getting closer) for more than stuck_time/2 (500ms)
    if ((!(g_pLocalPlayer->holding_sniper_rifle && g_pLocalPlayer->bZoomed) && !(g_pLocalPlayer->bRevved || g_pLocalPlayer->bRevving) && (crouch || crumbs[0].vec.z - g_pLocalPlayer->v_Origin.z > 18) && last_jump.check(200)) || (last_jump.check(200) && inactivity.check(*stuck_time / 2)))
    {
        auto local = map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
        // Check if current area allows jumping
        if (!local || !(local->m_attributeFlags & (NAV_MESH_NO_JUMP | NAV_MESH_STAIRS)))
        {
            // Make it crouch until we land, but jump the first tick
            current_user_cmd->buttons |= crouch ? IN_DUCK : IN_JUMP;

            // Only flip to crouch state, not to jump state
            if (!crouch)
            {
                crouch           = true;
                ticks_since_jump = 0;
            }
            ticks_since_jump++;

            // Update jump timer now since we are back on ground
            if (crouch && CE_INT(LOCAL_E, netvar.iFlags) & FL_ONGROUND && ticks_since_jump > 3)
            {
                // Reset
                crouch = false;
                last_jump.update();
            }
        }
    }

    /*if (inactivity.check(*stuck_time) || (inactivity.check(*unreachable_time) && !IsVectorVisible(g_pLocalPlayer->v_Origin, *crumb_vec + Vector(.0f, .0f, 41.5f), false, LOCAL_E, MASK_PLAYERSOLID)))
    {
        if (crumbs[0].navarea)
            ignoremanager::addTime(last_area, *crumb, inactivity);

        repath();
        return;
    }*/

    WalkTo(crumbs[0].vec);
}

void CreateMove()
{
    if (!isReady())
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;

    FollowCrumbs();
}

void LevelInit()
{
    auto level_name = g_IEngine->GetLevelName();
    if (!map || map->mapname != level_name)
    {
        char *p, cwd[PATH_MAX + 1], nav_path[PATH_MAX + 1], lvl_name[256];

        std::strncpy(lvl_name, level_name, 255);
        lvl_name[255] = 0;
        p             = std::strrchr(lvl_name, '.');
        if (!p)
        {
            logging::Info("Failed to find dot in level name");
            return;
        }
        *p = 0;
        p  = getcwd(cwd, sizeof(cwd));
        if (!p)
        {
            logging::Info("Failed to get current working directory: %s", strerror(errno));
            return;
        }
        std::snprintf(nav_path, sizeof(nav_path), "%s/tf/%s.nav", cwd, lvl_name);
        logging::Info("Pathing: Nav File location: %s", nav_path);
        map = std::make_unique<Map>(nav_path);
    }
    else
    {
        map->Reset();
    }
}

#if ENABLE_VISUALS
void drawNavArea(CNavArea *area)
{
    Vector nw, ne, sw, se;
    bool nw_screen = draw::WorldToScreen(area->m_nwCorner, nw);
    bool ne_screen = draw::WorldToScreen(area->getNeCorner(), ne);
    bool sw_screen = draw::WorldToScreen(area->getSwCorner(), sw);
    bool se_screen = draw::WorldToScreen(area->m_seCorner, se);

    // Nw -> Ne
    if (nw_screen && ne_screen)
        draw::Line(nw.x, nw.y, ne.x - nw.x, ne.y - nw.y, colors::green, 1.0f);
    // Nw -> Sw
    if (nw_screen && sw_screen)
        draw::Line(nw.x, nw.y, sw.x - nw.x, sw.y - nw.y, colors::green, 1.0f);
    // Ne -> Se
    if (ne_screen && se_screen)
        draw::Line(ne.x, ne.y, se.x - ne.x, se.y - ne.y, colors::green, 1.0f);
    // Sw -> Se
    if (sw_screen && se_screen)
        draw::Line(sw.x, sw.y, se.x - sw.x, se.y - sw.y, colors::green, 1.0f);
}

void Draw()
{
    if (!isReady() || !draw)
        return;
    if (draw_debug_areas && CE_GOOD(LOCAL_E) && LOCAL_E->m_bAlivePlayer())
    {
        auto area = map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
        auto edge = area->getNearestPoint(g_pLocalPlayer->v_Origin.AsVector2D());
        Vector scrEdge;
        edge.z += PLAYER_JUMP_HEIGHT;
        if (draw::WorldToScreen(edge, scrEdge))
            draw::Rectangle(scrEdge.x - 2.0f, scrEdge.y - 2.0f, 4.0f, 4.0f, colors::red);
        drawNavArea(area);
    }

    if (crumbs.empty())
        return;

    for (size_t i = 0; i < crumbs.size(); i++)
    {
        Vector start_pos = crumbs[i].vec;

        Vector start_screen, end_screen;
        if (draw::WorldToScreen(start_pos, start_screen))
        {
            draw::Rectangle(start_screen.x - 5.0f, start_screen.y - 5.0f, 10.0f, 10.0f, colors::white);

            if (i < crumbs.size() - 1)
            {
                Vector end_pos = crumbs[i + 1].vec;
                if (draw::WorldToScreen(end_pos, end_screen))
                    draw::Line(start_screen.x, start_screen.y, end_screen.x - start_screen.x, end_screen.y - start_screen.y, colors::white, 2.0f);
            }
        }
    }
}
#endif
}; // namespace NavEngine

Vector loc;

static CatCommand nav_set("nav_set", "Debug nav find", []() { loc = g_pLocalPlayer->v_Origin; });

static CatCommand nav_path("nav_path", "Debug nav path", []() { NavEngine::navTo(loc, 5, false, true, false); });

static CatCommand nav_init("nav_init", "Reload nav mesh", []() {
    NavEngine::map.reset();
    NavEngine::LevelInit();
});

static CatCommand nav_debug_("nav_debug_check", "Perform nav checks between two areas. First area: cat_nav_set Second area: Your location while running this command.", []() {
    if (!NavEngine::isReady())
        return;
    auto current = NavEngine::map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
    auto next    = NavEngine::map->findClosestNavSquare(loc);

    auto points = determinePoints(current, next);

    points.current = handleDropdown(points.current, points.center);
    points.center  = handleDropdown(points.center, points.next);

    points.current.z += PLAYER_JUMP_HEIGHT;
    points.center += PLAYER_JUMP_HEIGHT;
    points.next.z += PLAYER_JUMP_HEIGHT;

    if (IsPlayerPassableNavigation(points.current, points.center) && IsPlayerPassableNavigation(points.center, points.next))
    {
        logging::Info("Nav: Area is player passable!");
    }
    else
    {
        logging::Info("Nav: Area is NOT player passable! %.2f,%.2f,%.2f %.2f,%.2f,%.2f", points.current.x, points.current.y, points.current.z, points.next.x, points.next.y, points.next.z);
    }
});

static InitRoutine init([]() {
    // this is a comment
    // so this doesn't get one linered
    EC::Register(EC::CreateMove, NavEngine::CreateMove, "navengine_cm");
    EC::Register(EC::LevelInit, NavEngine::LevelInit, "navengine_levelinit");
#if ENABLE_VISUALS
    EC::Register(EC::Draw, NavEngine::Draw, "navengine_draw");
#endif
    enabled.installChangeCallback([](settings::VariableBase<bool> &, bool after) {
        if (after && g_IEngine->IsInGame())
            NavEngine::LevelInit();
    });
});

} // namespace navparser

/*#include "common.hpp"
#include "navparser.hpp"
#include <thread>
#include "micropather.h"
#include <pwd.h>
#include <boost/functional/hash.hpp>
#include <boost/container/flat_set.hpp>
#include <chrono>
#include <CNavFile.h>
#include "MiscAimbot.hpp"
#include "Aimbot.hpp"

namespace nav
{

static settings::Boolean enabled{ "misc.pathing", "true" };
// Whether or not to run vischecks at pathtime
static settings::Boolean vischecks{ "misc.pathing.pathtime-vischecks", "true" };
static settings::Boolean vischeckBlock{ "misc.pathing.pathtime-vischeck-block", "false" };
static settings::Boolean draw{ "misc.pathing.draw", "false" };
static settings::Boolean draw_priorities{ "misc.pathing.draw-priorities", "false" };
static settings::Boolean look{ "misc.pathing.look-at-path", "false" };
static settings::Int stuck_time{ "misc.pathing.stuck-time", "4000" };
static settings::Int unreachable_time{ "misc.pathing.unreachable-time", "1000" };
static settings::Boolean log_pathing{ "misc.pathing.log", "false" };

// Score based on how much the area was used by other players, in seconds
static std::unordered_map<int, float> area_score;
static std::vector<CNavArea *> crumbs;
static Vector startPoint, endPoint;

enum ignore_status : uint8_t
{
    // Status is unknown
    unknown = 0,
    // Something like Z check failed, these are unchanging
    const_ignored,
    // LOS between areas is given
    vischeck_success,
    // LOS if we ignore entities
    vischeck_blockedentity,
    // No LOS between areas
    vischeck_failed,
    // Failed to actually walk thru connection
    explicit_ignored,
    // Danger like sentry gun or sticky
    danger_found
};

void ResetPather();
void repath();

struct ignoredata
{
    ignore_status status{ unknown };
    float stucktime{ 0.0f };
    Timer ignoreTimeout{};
};

Vector GetClosestCornerToArea(CNavArea *CornerOf, const Vector &target)
{
    std::array<Vector, 4> corners{
        CornerOf->m_nwCorner,                                                       // NW
        CornerOf->m_seCorner,                                                       // SE
        { CornerOf->m_seCorner.x, CornerOf->m_nwCorner.y, CornerOf->m_nwCorner.z }, // NE
        { CornerOf->m_nwCorner.x, CornerOf->m_seCorner.y, CornerOf->m_seCorner.z }  // SW
    };

    Vector *bestVec = &corners[0], *bestVec2 = bestVec;
    float bestDist = corners[0].DistTo(target), bestDist2 = bestDist;

    for (size_t i = 1; i < corners.size(); i++)
    {
        float dist = corners[i].DistTo(target);
        if (dist < bestDist)
        {
            bestVec  = &corners[i];
            bestDist = dist;
        }
        if (corners[i] == *bestVec2)
            continue;

        if (dist < bestDist2)
        {
            bestVec2  = &corners[i];
            bestDist2 = dist;
        }
    }
    return (*bestVec + *bestVec2) / 2;
}

// Get the area score multiplier
float getAreaScoreMultiplier(float score)
{
    // Formula to calculate by how much % to reduce the distance by (https://xaktly.com/LogisticFunctions.html)
    return 2.0f * ((0.9f) / (1.0f + exp(-0.2f * score)) - 0.45f);
}

float getZBetweenAreas(CNavArea *start, CNavArea *end)
{
    float z1 = GetClosestCornerToArea(start, end->m_center).z;
    float z2 = GetClosestCornerToArea(end, start->m_center).z;

    return z2 - z1;
}

static std::unordered_map<std::pair<CNavArea *, CNavArea *>, ignoredata, boost::hash<std::pair<CNavArea *, CNavArea *>>> ignores;
namespace ignoremanager
{
static ignore_status vischeck(CNavArea *begin, CNavArea *end)
{
    Vector first  = begin->m_center;
    Vector second = end->m_center;
    first.z += 70;
    second.z += 70;
    // Is world blocking it?
    if (IsVectorVisibleNavigation(first, second, MASK_PLAYERSOLID))
    {
        // Is something else blocking it?
        if (!IsVectorVisible(first, second, true, LOCAL_E, MASK_PLAYERSOLID))
            return vischeck_blockedentity;
        else
            return vischeck_success;
    }
    return vischeck_failed;
}
static ignore_status runIgnoreChecks(CNavArea *begin, CNavArea *end)
{
    if (getZBetweenAreas(begin, end) > 70)
        return const_ignored;
    if (!vischecks)
        return vischeck_success;
    return vischeck(begin, end);
}
static void updateDanger()
{
    for (size_t i = 0; i <= HIGHEST_ENTITY; i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (CE_INVALID(ent))
            continue;
        if (ent->m_iClassID() == CL_CLASS(CObjectSentrygun))
        {
            if (!ent->m_bEnemy())
                continue;
            if (HasCondition<TFCond_Disguised>(LOCAL_E))
                continue;
            Vector loc = GetBuildingPosition(ent);
            if (RAW_ENT(ent)->IsDormant())
            {
                auto vec = ent->m_vecDormantOrigin();
                if (vec)
                {
                    loc -= RAW_ENT(ent)->GetCollideable()->GetCollisionOrigin();
                    loc += *vec;
                }
                else
                    continue;
            }
            // It's still building, ignore
            else if (CE_BYTE(ent, netvar.m_bBuilding) || CE_BYTE(ent, netvar.m_bPlacing))
                continue;

            // Keep track of good spots
            std::vector<CNavArea *> spot_list{};

            // Don't blacklist if local player is standing in it
            bool local_player_in_range = false;

            // Local player's nav area
            auto local_area = findClosestNavSquare(LOCAL_E->m_vecOrigin());

            // Actual building check
            for (auto &i : navfile->m_areas)
            {
                Vector area = i.m_center;
                area.z += 41.5f;
                if (loc.DistTo(area) > 1100)
                    continue;
                // Check if sentry can see us
                if (!IsVectorVisible(loc, area, true))
                    continue;
                // local player's nav area?
                if (local_area == &i)
                {
                    local_player_in_range = true;
                    break;
                }
                spot_list.push_back(&i);
            }

            // Local player is in the sentry range, let him nav
            if (local_player_in_range)
                continue;

            // Ignore these
            for (auto &i : spot_list)
            {
                ignoredata &data = ignores[{ i, nullptr }];
                data.status      = danger_found;
                data.ignoreTimeout.update();
                data.ignoreTimeout.last -= std::chrono::seconds(17);
            }
        }
        else if (ent->m_iClassID() == CL_CLASS(CTFGrenadePipebombProjectile))
        {
            if (!ent->m_bEnemy())
                continue;
            if (CE_INT(ent, netvar.iPipeType) == 1)
                continue;
            Vector loc = ent->m_vecOrigin();

            // Keep track of good spots
            std::vector<CNavArea *> spot_list{};

            // Don't blacklist if local player is standing in it
            bool local_player_in_range = false;

            // Local player's nav area
            auto local_area = findClosestNavSquare(LOCAL_E->m_vecOrigin());

            // Actual Sticky check
            for (auto &i : navfile->m_areas)
            {
                Vector area = i.m_center;
                area.z += 41.5f;
                if (loc.DistTo(area) > 130)
                    continue;
                // Check if in Sticky vis range
                if (!IsVectorVisible(loc, area, true))
                    continue;
                // local player's nav area?
                if (local_area == &i)
                {
                    local_player_in_range = true;
                    break;
                }
                spot_list.push_back(&i);
            }

            // Local player is in the sentry range, let him nav
            if (local_player_in_range)
                continue;

            // Ignore these
            for (auto &i : spot_list)
            {
                ignoredata &data = ignores[{ i, nullptr }];
                data.status      = danger_found;
                data.ignoreTimeout.update();
                data.ignoreTimeout.last -= std::chrono::seconds(17);
            }
        }
    }
}

static void checkPath()
{
    bool perform_repath = false;
    // Vischecks
    for (size_t i = 0; i < crumbs.size() - 1; i++)
    {
        CNavArea *begin = crumbs[i];
        CNavArea *end   = crumbs[i + 1];
        if (!begin || !end)
            continue;
        ignoredata &data = ignores[{ begin, end }];
        if (data.status == vischeck_failed)
            return;
        if (data.status == vischeck_blockedentity && vischeckBlock)
            return;
        auto vis_status = vischeck(begin, end);
        if (vis_status == vischeck_failed)
        {
            data.status = vischeck_failed;
            data.ignoreTimeout.update();
            perform_repath = true;
        }
        else if (vis_status == vischeck_blockedentity && vischeckBlock)
        {
            data.status = vischeck_blockedentity;
            data.ignoreTimeout.update();
            perform_repath = true;
        }
        else if (ignores[{ end, nullptr }].status == danger_found)
        {
            perform_repath = true;
        }
    }
    if (perform_repath)
        repath();
}
// 0 = Not ignored, 1 = low priority, 2 = ignored
static int isIgnored(CNavArea *begin, CNavArea *end)
{
    if (ignores[{ end, nullptr }].status == danger_found)
        return 2;
    ignore_status status = ignores[{ begin, end }].status;
    if (status == unknown)
        status = runIgnoreChecks(begin, end);
    if (status == vischeck_success)
        return 0;
    else if (status == vischeck_blockedentity && !vischeckBlock)
        return 1;
    else
        return 2;
}
static bool addTime(ignoredata &connection, ignore_status status)
{
    connection.status = status;
    connection.ignoreTimeout.update();

    return true;
}
static bool addTime(CNavArea *begin, CNavArea *end, ignore_status status)
{
    logging::Info("Ignored Connection %i-%i", begin->m_id, end->m_id);
    return addTime(ignores[{ begin, end }], status);
}
static bool addTime(CNavArea *begin, CNavArea *end, Timer &time)
{
    if (!begin || !end)
    {
        // We can't reach the destination vector. Destination vector might
        // be out of bounds/reach.
        clearInstructions();
        return true;
    }
    using namespace std::chrono;
    // Check if connection is already known
    if (ignores.find({ begin, end }) == ignores.end())
    {
        ignores[{ begin, end }] = {};
    }
    ignoredata &connection = ignores[{ begin, end }];
    connection.stucktime += duration_cast<milliseconds>(system_clock::now() - time.last).count();
    if (connection.stucktime >= *stuck_time)
    {
        logging::Info("Ignored Connection %i-%i", begin->m_id, end->m_id);
        return addTime(connection, explicit_ignored);
    }
    return false;
}
static void reset()
{
    ignores.clear();
    ResetPather();
}
static void updateIgnores()
{
    static Timer update{};
    static Timer last_pather_reset{};
    static bool reset_pather = false;
    if (!update.test_and_set(500))
        return;
    updateDanger();
    if (crumbs.empty())
    {
        for (auto &i : ignores)
        {
            switch (i.second.status)
            {
            case explicit_ignored:
                if (i.second.ignoreTimeout.check(60000))
                {
                    i.second.status    = unknown;
                    i.second.stucktime = 0;
                    reset_pather       = true;
                }
                break;
            case unknown:
                break;
            case danger_found:
                if (i.second.ignoreTimeout.check(20000))
                {
                    i.second.status = unknown;
                    reset_pather    = true;
                }
                break;
            case vischeck_failed:
            case vischeck_blockedentity:
            case vischeck_success:
            default:
                if (i.second.ignoreTimeout.check(30000))
                {
                    i.second.status    = unknown;
                    i.second.stucktime = 0;
                    reset_pather       = true;
                }
                break;
            }
        }
    }
    else
        checkPath();
    if (reset_pather && last_pather_reset.test_and_set(10000))
    {
        reset_pather = false;
        ResetPather();
    }
}
static bool isSafe(CNavArea *area)
{
    return !(ignores[{ area, nullptr }].status == danger_found);
}
}; // namespace ignoremanager

struct Graph : public micropather::Graph
{
    std::unique_ptr<micropather::MicroPather> pather;

    Graph()
    {
        pather = std::make_unique<micropather::MicroPather>(this, 3000, 6, true);
    }
    ~Graph() override
    {
    }
    void AdjacentCost(void *state, MP_VECTOR<micropather::StateCost> *adjacent) override
    {
        CNavArea *center = static_cast<CNavArea *>(state);
        for (auto &i : center->m_connections)
        {
            CNavArea *neighbour = i.area;
            int isIgnored       = ignoremanager::isIgnored(center, neighbour);
            if (isIgnored == 2)
                continue;
            float distance = center->m_center.DistTo(i.area->m_center);
            if (isIgnored == 1)
                distance += 2000;
            // Check priority based on usage
            else
            {
                float score = area_score[neighbour->m_id];
                // Formula to calculate by how much % to reduce the distance by (https://xaktly.com/LogisticFunctions.html)
                float multiplier = getAreaScoreMultiplier(score);
                distance *= 1.0f - multiplier;
            }

            adjacent->emplace_back(micropather::StateCost{ reinterpret_cast<void *>(neighbour), distance });
        }
    }
    float LeastCostEstimate(void *stateStart, void *stateEnd) override
    {
        CNavArea *start = reinterpret_cast<CNavArea *>(stateStart);
        CNavArea *end   = reinterpret_cast<CNavArea *>(stateEnd);
        return start->m_center.DistTo(end->m_center);
    }
    void PrintStateInfo(void *) override
    {
    }
};

// Navfile containing areas
std::unique_ptr<CNavFile> navfile;
// Status
std::atomic<init_status> status;

// See "Graph", does pathing and stuff I guess
static Graph Map;

void initThread()
{
    char *p, cwd[PATH_MAX + 1], nav_path[PATH_MAX + 1], lvl_name[256];

    std::strncpy(lvl_name, g_IEngine->GetLevelName(), 255);
    lvl_name[255] = 0;
    p             = std::strrchr(lvl_name, '.');
    if (!p)
    {
        logging::Info("Failed to find dot in level name");
        return;
    }
    *p = 0;
    p  = getcwd(cwd, sizeof(cwd));
    if (!p)
    {
        logging::Info("Failed to get current working directory: %s", strerror(errno));
        return;
    }
    std::snprintf(nav_path, sizeof(nav_path), "%s/tf/%s.nav", cwd, lvl_name);
    logging::Info("Pathing: Nav File location: %s", nav_path);
    navfile = std::make_unique<CNavFile>(nav_path);
    if (!navfile->m_isOK)
    {
        navfile.reset();
        status = unavailable;
        return;
    }
    logging::Info("Pather: Initing with %i Areas", navfile->m_areas.size());
    status = on;
}

void init()
{
    area_score.clear();
    endPoint.Invalidate();
    ignoremanager::reset();
    status = initing;
    std::thread thread;
    thread = std::thread(initThread);
    thread.detach();
}

bool prepare()
{
    if (!enabled)
        return false;
    init_status fast_status = status;
    if (fast_status == on)
        return true;
    if (fast_status == off)
    {
        init();
    }
    return false;
}

// This prevents the bot from gettings completely stuck in some cases
static std::vector<CNavArea *> findClosestNavSquare_localAreas(6);

// Function for getting closest Area to player, aka "LocalNav"
CNavArea *findClosestNavSquare(const Vector &vec)
{
    bool isLocal = vec == g_pLocalPlayer->v_Origin;
    if (isLocal && findClosestNavSquare_localAreas.size() > 5)
        findClosestNavSquare_localAreas.erase(findClosestNavSquare_localAreas.begin());

    float ovBestDist = FLT_MAX, bestDist = FLT_MAX;
    // If multiple candidates for LocalNav have been found, pick the closest
    CNavArea *ovBestSquare = nullptr, *bestSquare = nullptr;
    for (auto &i : navfile->m_areas)
    {
        // Make sure we're not stuck on the same area for too long
        if (isLocal && std::count(findClosestNavSquare_localAreas.begin(), findClosestNavSquare_localAreas.end(), &i) >= 3)
        {
            continue;
        }
        float dist = i.m_center.DistTo(vec);
        if (dist < bestDist)
        {
            bestDist   = dist;
            bestSquare = &i;
        }
        // Check if we are within x and y bounds of an area
        if (ovBestDist >= dist || !i.IsOverlapping(vec) || !IsVectorVisibleNavigation(vec, i.m_center, MASK_PLAYERSOLID))
        {
            continue;
        }
        ovBestDist   = dist;
        ovBestSquare = &i;
    }
    if (!ovBestSquare)
        ovBestSquare = bestSquare;

    if (isLocal)
        findClosestNavSquare_localAreas.push_back(ovBestSquare);

    return ovBestSquare;
}

std::vector<CNavArea *> findPath(const Vector &start, const Vector &end)
{
    using namespace std::chrono;

    if (status != on)
        return {};

    CNavArea *local, *dest;
    if (!(local = findClosestNavSquare(start)) || !(dest = findClosestNavSquare(end)))
        return {};

    if (log_pathing)
    {
        logging::Info("Start: (%f,%f,%f)", local->m_center.x, local->m_center.y, local->m_center.z);
        logging::Info("End: (%f,%f,%f)", dest->m_center.x, dest->m_center.y, dest->m_center.z);
    }
    float cost;
    std::vector<CNavArea *> pathNodes;

    time_point begin_pathing = high_resolution_clock::now();
    int result               = Map.pather->Solve(reinterpret_cast<void *>(local), reinterpret_cast<void *>(dest), reinterpret_cast<std::vector<void *> *>(&pathNodes), &cost);
    long long timetaken      = duration_cast<nanoseconds>(high_resolution_clock::now() - begin_pathing).count();
    if (log_pathing)
        logging::Info("Pathing: Pather result: %i. Time taken (NS): %lld", result, timetaken);
    // If no result found, return empty Vector
    if (result == micropather::MicroPather::NO_SOLUTION)
        return {};

    return pathNodes;
}

static Vector loc(0.0f, 0.0f, 0.0f);
static CNavArea *last_area = nullptr;
bool ReadyForCommands      = true;
static Timer inactivity{};
int curr_priority         = 0;
static bool ensureArrival = false;

bool navTo(const Vector &destination, int priority, bool should_repath, bool nav_to_local, bool is_repath)
{
    if (!prepare() || priority < curr_priority)
        return false;

    auto path = findPath(g_pLocalPlayer->v_Origin, destination);
    if (path.empty())
    {
        clearInstructions();
        return false;
    }
    auto crumb = crumbs.begin();
    if (crumb != crumbs.end() && ignoremanager::addTime(last_area, *crumb, inactivity))
        ResetPather();

    auto path_it = path.begin();
    last_area    = *path_it;
    if (!nav_to_local)
    {
        path.erase(path_it);
        if (path.empty())
            return false;
    }
    inactivity.update();
    if (!is_repath)
        findClosestNavSquare_localAreas.clear();

    ensureArrival    = should_repath;
    ReadyForCommands = false;
    curr_priority    = priority;
    crumbs           = std::move(path);
    endPoint         = destination;
    return true;
}

void repath()
{
    if (!ensureArrival)
        return;

    Vector last;
    if (!crumbs.empty())
        last = crumbs.back()->m_center;
    else if (endPoint.IsValid())
        last = endPoint;
    else
        return;

    clearInstructions();
    ResetPather();
    navTo(last, curr_priority, true, true, true);
}

// Track pather resets
static Timer reset_pather_timer{};
// Update area score to prefer paths used by actual players a bit more
void updateAreaScore()
{
    for (int i = 1; i <= g_IEngine->GetMaxClients(); i++)
    {
        CachedEntity *ent = ENTITY(i);
        if (i == g_pLocalPlayer->entity_idx || CE_INVALID(ent) || !g_pPlayerResource->isAlive(i))
            continue;

        // Get area
        CNavArea *closest_area = nullptr;
        if (ent->m_vecDormantOrigin())
            closest_area = findClosestNavSquare(*ent->m_vecDormantOrigin());

        // Add usage to area if valid
        if (closest_area)
            area_score[closest_area->m_id] += g_GlobalVars->interval_per_tick;
    }
    if (reset_pather_timer.test_and_set(10000))
        ResetPather();
}

static Timer last_jump{};
// Main movement function, gets path from NavTo
static void cm()
{
    if (!enabled || status != on)
        return;
    // Run the logic for Nav area score
    updateAreaScore();

    if (CE_BAD(LOCAL_E) || CE_BAD(LOCAL_W))
        return;
    if (!LOCAL_E->m_bAlivePlayer())
    {
        // Clear path if player dead
        clearInstructions();
        return;
    }
    ignoremanager::updateIgnores();

    auto crumb = crumbs.begin();
    const Vector *crumb_vec;
    // Crumbs empty, prepare for next instruction
    if (crumb == crumbs.end())
    {
        if (endPoint.IsValid())
            crumb_vec = &endPoint;
        else
        {
            curr_priority    = 0;
            ReadyForCommands = true;
            ensureArrival    = false;
            return;
        }
    }
    else
        crumb_vec = &(*crumb)->m_center;

    ReadyForCommands = false;
    // Remove old crumbs
    if (g_pLocalPlayer->v_Origin.DistTo(*crumb_vec) < 50.0f)
    {
        inactivity.update();
        if (crumb_vec == &endPoint)
        {
            endPoint.Invalidate();
            return;
        }
        last_area = *crumb;
        crumbs.erase(crumb);
        crumb = crumbs.begin();
        if (crumb == crumbs.end())
        {
            if (!endPoint.IsValid())
            {
                logging::Info("navparser.cpp cm -> endPoint.IsValid() == false");
                return;
            }
            crumb_vec = &endPoint;
        }
    }
    if (look && !hacks::shared::aimbot::CurrentTarget())
    {
        Vector next{ crumb_vec->x, crumb_vec->y, g_pLocalPlayer->v_Eye.z };
        next = GetAimAtAngles(g_pLocalPlayer->v_Eye, next);
        hacks::tf2::misc_aimbot::DoSlowAim(next);
        current_user_cmd->viewangles = next;
    }
    // Used to determine if we want to jump or if we want to crouch
    static bool crouch          = false;
    static int ticks_since_jump = 0;

    // Detect when jumping is necessary
    if ((!(g_pLocalPlayer->holding_sniper_rifle && g_pLocalPlayer->bZoomed) && (crouch || crumb_vec->z - g_pLocalPlayer->v_Origin.z > 18) && last_jump.check(200)) || (last_jump.check(200) && inactivity.check(*stuck_time / 2)))
    {
        auto local = findClosestNavSquare(g_pLocalPlayer->v_Origin);
        // Check if current area allows jumping
        if (!local || !(local->m_attributeFlags & (NAV_MESH_NO_JUMP | NAV_MESH_STAIRS)))
        {
            // Make it crouch until we land, but jump the first tick
            current_user_cmd->buttons |= crouch ? IN_DUCK : IN_JUMP;

            // Only flip to crouch state, not to jump state
            if (!crouch)
            {
                crouch           = true;
                ticks_since_jump = 0;
            }
            ticks_since_jump++;

            // Update jump timer now since we are back on ground
            if (crouch && CE_INT(LOCAL_E, netvar.iFlags) & FL_ONGROUND && ticks_since_jump > 3)
            {
                // Reset
                crouch = false;
                last_jump.update();
            }
        }
    }
    // Walk to next crumb
    WalkTo(*crumb_vec);
    /* If can't go through for some time (doors aren't instantly opening)
     * ignore that connection
     * Or if inactive for too long
     *
    if (inactivity.check(*stuck_time) || (inactivity.check(*unreachable_time) && !IsVectorVisible(g_pLocalPlayer->v_Origin, *crumb_vec + Vector(.0f, .0f, 41.5f), false, LOCAL_E, MASK_PLAYERSOLID)))
    {
        /* crumb is invalid if endPoint is used *
        if (crumb_vec != &endPoint)
            ignoremanager::addTime(last_area, *crumb, inactivity);

        repath();
        return;
    }
}

#if ENABLE_VISUALS
static void drawcrumbs()
{
    if (!enabled || !draw)
        return;
    if (CE_BAD(LOCAL_E) || CE_BAD(LOCAL_W))
        return;
    if (!LOCAL_E->m_bAlivePlayer())
        return;
    if (draw_priorities)
    {
        if (navfile && navfile->m_areas.size() && enabled && nav::status == nav::on)
            for (auto &area : navfile->m_areas)
            {
                Vector &pos = area.m_center;
                if (pos.DistTo(LOCAL_E->m_vecOrigin()) > 300.0f)
                    continue;
                Vector wts;
                if (draw::WorldToScreen(pos, wts))
                {
                    float score             = area_score[area.m_id];
                    float multiplier        = getAreaScoreMultiplier(score);
                    std::string draw_string = std::to_string(multiplier);
                    draw::String(wts.x, wts.y, colors::white, draw_string.c_str(), *fonts::esp);
                }
            }
    }
    if (crumbs.size() < 2)
        return;
    for (size_t i = 0; i < crumbs.size(); i++)
    {
        Vector wts1, wts2, *o1, *o2;
        rgba_t draw_color = colors::white;
        if (crumbs.size() - 1 == i)
        {
            if (!endPoint.IsValid())
                break;

            o2          = &endPoint;
            float score = area_score[crumbs.at(i)->m_id];
            // Formula to calculate by how much % to reduce the distance by (https://xaktly.com/LogisticFunctions.html)
            float multiplier = getAreaScoreMultiplier(score);
            // Calculate the color
            draw_color = colors::Fade(colors::white, colors::red, multiplier * (PI / 2.0));
        }
        else
        {
            o2          = &crumbs[i + 1]->m_center;
            float score = area_score[crumbs.at(i + 1)->m_id];
            // Formula to calculate by how much % to reduce the distance by (https://xaktly.com/LogisticFunctions.html)
            float multiplier = getAreaScoreMultiplier(score);
            // Calculate the color
            draw_color = colors::Fade(colors::white, colors::red, multiplier * (PI / 2.0));
        }

        o1 = &crumbs[i]->m_center;
        if (draw::WorldToScreen(*o1, wts1) && draw::WorldToScreen(*o2, wts2))
        {
            draw::Line(wts1.x, wts1.y, wts2.x - wts1.x, wts2.y - wts1.y, draw_color, 0.7f);
        }
    }
    Vector wts;
    if (!draw::WorldToScreen(crumbs[0]->m_center, wts))
        return;
    draw::Rectangle(wts.x - 4, wts.y - 4, 8, 8, colors::white);
    draw::RectangleOutlined(wts.x - 4, wts.y - 4, 7, 7, colors::white, 1.0f);
}
#endif

static InitRoutine runinit([]() {
    EC::Register(EC::CreateMove, cm, "cm_navparser", EC::average);
#if ENABLE_VISUALS
    EC::Register(EC::Draw, drawcrumbs, "draw_navparser", EC::average);
#endif
});

void ResetPather()
{
    Map.pather->Reset();
}

bool isSafe(CNavArea *area)
{
    return ignoremanager::isSafe(area);
}

static CatCommand nav_find("nav_find", "Debug nav find", []() {
    auto path = findPath(g_pLocalPlayer->v_Origin, loc);
    if (path.empty())
    {
        logging::Info("Pathing: No path found");
        return;
    }
    std::string output = "Pathing: Path found! Path: ";
    for (int i = 0; i < path.size(); i++)
    {
        output.append(format(path[i]->m_center.x, ",", format(path[i]->m_center.y), " "));
    }
    logging::Info(output.c_str());
});

static CatCommand nav_set("nav_set", "Debug nav find", []() { loc = g_pLocalPlayer->v_Origin; });

static CatCommand nav_init("nav_init", "Debug nav init", []() {
    status = off;
    prepare();
});

static CatCommand nav_path("nav_path", "Debug nav path", []() { navTo(loc); });

static CatCommand nav_path_no_local("nav_path_no_local", "Debug nav path", []() { navTo(loc, 5, false, false); });

static CatCommand nav_reset_ignores("nav_reset_ignores", "Reset all ignores.", []() { ignoremanager::reset(); });

void clearInstructions()
{
    crumbs.clear();
    endPoint.Invalidate();
    curr_priority = 0;
}
static CatCommand nav_stop("nav_cancel", "Cancel Navigation", []() { clearInstructions(); });
} // namespace nav
*/
