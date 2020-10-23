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
constexpr float PLAYER_JUMP_HEIGHT = 72.0f;

static settings::Boolean enabled("nav.enabled", "false");
static settings::Boolean draw("nav.draw", "false");
static settings::Boolean draw_debug_areas("nav.draw.debug-areas", "false");
static settings::Boolean log_pathing{ "nav.log", "false" };
static settings::Int stuck_time{ "nav.stuck-time", "1000" };
static settings::Boolean vischeck_runtime{ "nav.vischeck-runtime.enabled", "true" };
static settings::Int vischeck_time{ "nav.vischeck-runtime.delay", "2000" };
static settings::Int stuck_detect_time{ "nav.anti-stuck.detection-time", "5" };
// How long until accumulated "Stuck time" expires
static settings::Int stuck_expire_time{ "nav.anti-stuck.expire-time", "10" };
// How long we should blacklist the node after being stuck for too long?
static settings::Int stuck_blacklist_time{ "nav.anti-stuck.blacklist-time", "30" };

#define TICKCOUNT_TIMESTAMP(seconds) (g_GlobalVars->tickcount + int(seconds / g_GlobalVars->interval_per_tick))

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

struct CachedStucktime
{
    int expire_tick;
    int time_stuck;
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
        // We need to really make sure we fall, so we go two times as far out as we should have to
        current_pos = GetForwardVector(current_pos, angles, PLAYER_WIDTH * 2.0f);
    }
    return current_pos;
}

class navPoints
{
public:
    Vector current;
    Vector center;
    // The above but on the "next" vector, used for height checks.
    Vector center_next;
    Vector next;
    navPoints(Vector A, Vector B, Vector C, Vector D) : current(A), center(B), center_next(C), next(D){};
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
    {
        center_point = next_closest;
        // Use the point closest to next_closest on the "original" mesh for z
        center_point.z = current->getNearestPoint(next_closest.AsVector2D()).z;
    }

    // Nearest point to center on "next"m used for height checks
    auto center_next = next->getNearestPoint(center_point.AsVector2D());

    return navPoints(area_center, center_point, center_next, next_center);
};

class Map : public micropather::Graph
{
public:
    CNavFile navfile;
    NavState state;
    micropather::MicroPather pather{ this, 3000, 6, true };
    std::string mapname;
    std::unordered_map<std::pair<CNavArea *, CNavArea *>, CachedConnection, boost::hash<std::pair<CNavArea *, CNavArea *>>> vischeck_cache;
    std::unordered_map<std::pair<CNavArea *, CNavArea *>, CachedStucktime, boost::hash<std::pair<CNavArea *, CNavArea *>>> connection_stuck_time;

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
            // An area being entered twice means it is blacklisted from entry entirely
            auto connection_key    = std::pair<CNavArea *, CNavArea *>(connection.area, connection.area);
            auto cached_connection = vischeck_cache.find(connection_key);

            // Entered and marked bad?
            if (cached_connection != vischeck_cache.end())
                if (!cached_connection->second.vischeck_state)
                    continue;

            auto points = determinePoints(&area, connection.area);

            // Apply dropdown
            points.center = handleDropdown(points.center, points.next);

            float height_diff = points.center_next.z - points.center.z;

            // Too high for us to jump!
            if (height_diff > PLAYER_JUMP_HEIGHT)
                continue;

            points.current.z += PLAYER_JUMP_HEIGHT;
            points.center.z += PLAYER_JUMP_HEIGHT;
            points.next.z += PLAYER_JUMP_HEIGHT;

            auto key    = std::pair<CNavArea *, CNavArea *>(&area, connection.area);
            auto cached = vischeck_cache.find(key);
            if (cached != vischeck_cache.end())
            {
                if (cached->second.vischeck_state)
                {
                    float cost = connection.area->m_center.DistTo(area.m_center);
                    adjacent->push_back(micropather::StateCost{ reinterpret_cast<void *>(connection.area), cost });
                }
            }
            else
            {
                // Check if there is direct line of sight
                if (IsPlayerPassableNavigation(points.current, points.center) && IsPlayerPassableNavigation(points.center, points.next))
                {
                    vischeck_cache[key] = { TICKCOUNT_TIMESTAMP(10), true };

                    float cost = points.next.DistTo(points.current);
                    adjacent->push_back(micropather::StateCost{ reinterpret_cast<void *>(connection.area), cost });
                }
                else
                {
                    vischeck_cache[key] = { TICKCOUNT_TIMESTAMP(10), false };
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
        for (auto it = begin(connection_stuck_time); it != end(connection_stuck_time);)
        {
            if (it->second.expire_tick < g_GlobalVars->tickcount)
            {
                it     = connection_stuck_time.erase(it); // previously this was something like m_map.erase(it++);
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
        connection_stuck_time.clear();
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
Crumb last_crumb;
std::vector<Crumb> crumbs;

int current_priority    = 0;
bool current_navtolocal = false;
bool repath_on_fail     = false;
Vector last_destination;

bool isReady()
{
    return enabled && map && map->state == NavState::Active;
}

static Timer inactivity{};

bool navTo(const Vector &destination, int priority, bool should_repath, bool nav_to_local, bool is_repath)
{
    if (!isReady())
        return false;
    // Don't path, priority is too low
    if (priority < current_priority)
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

            points.center = handleDropdown(points.center, points.next);

            crumbs.push_back({ area, std::move(points.current) });
            crumbs.push_back({ area, std::move(points.center) });
        }
        else
            crumbs.push_back({ area, area->m_center });
    }
    crumbs.push_back({ nullptr, destination });
    inactivity.update();

    current_priority   = priority;
    current_navtolocal = nav_to_local;
    repath_on_fail     = should_repath;
    // Ensure we know where to go
    if (repath_on_fail)
        last_destination = destination;

    return true;
}

// Use when something unexpected happens, e.g. vischeck fails
void abandonPath()
{
    if (!map)
        return;
    map->pather.Reset();
    crumbs.clear();
    last_crumb.navarea = nullptr;
    // We want to repath on failure
    if (repath_on_fail)
        navTo(last_destination, current_priority, true, current_navtolocal, false);
}

static Timer last_jump{};
// Used to determine if we want to jump or if we want to crouch
static bool crouch          = false;
static int ticks_since_jump = 0;

static void followCrumbs()
{
    size_t crumbs_amount = crumbs.size();

    // No more crumbs, reset status
    if (!crumbs_amount)
    {
        // Invalidate last crumb
        last_crumb.navarea = nullptr;

        repath_on_fail   = false;
        current_priority = 0;
        return;
    }

    // We are close enough to the crumb to have reached it
    if (crumbs[0].vec.DistTo(g_pLocalPlayer->v_Origin) < 50)
    {
        last_crumb = crumbs[0];
        crumbs.erase(crumbs.begin());
        if (!--crumbs_amount)
            return;
        inactivity.update();
    }
    // We are close enough to the second crumb, Skip both (This is espcially helpful with drop downs)
    if (crumbs.size() > 1 && crumbs[1].vec.DistTo(g_pLocalPlayer->v_Origin) < 50)
    {
        last_crumb = crumbs[1];
        crumbs.erase(crumbs.begin(), std::next(crumbs.begin()));
        --crumbs_amount;
        if (!--crumbs_amount)
            return;
        inactivity.update();
    }

    // If we make any progress at all, reset this
    else
    {
        Vector vel;
        velocity::EstimateAbsVelocity(RAW_ENT(LOCAL_E), vel);
        // 44.0f -> Revved brass beast, do not use z axis as jumping counts towards that. Yes this will mean long falls will trigger it, but that is not really bad.
        if (!vel.AsVector2D().IsZero(40.0f))
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

static Timer vischeck_timer{};
void vischeckPath()
{
    // No crumbs to check, or vischeck timer should not run yet, bail.
    if (crumbs.size() < 2 || !vischeck_timer.test_and_set(*vischeck_time))
        return;

    // Iterate all the crumbs
    for (int i = 0; i < (int) crumbs.size() - 1; i++)
    {
        auto current_crumb  = crumbs[i];
        auto next_crumb     = crumbs[i + 1];
        auto current_center = current_crumb.vec;
        auto next_center    = next_crumb.vec;

        current_center.z += PLAYER_JUMP_HEIGHT;
        next_center.z += PLAYER_JUMP_HEIGHT;
        auto key = std::pair<CNavArea *, CNavArea *>(current_crumb.navarea, next_crumb.navarea);
        // Check if we can pass, if not, abort pathing and mark as bad
        if (!IsPlayerPassableNavigation(current_center, next_center))
        {
            // Mark as invalid for 10 seconds
            map->vischeck_cache[key] = { TICKCOUNT_TIMESTAMP(10), false };
            abandonPath();
        }
        // Else we can update the cache (if not marked bad before this)
        else if (map->vischeck_cache.find(key) == map->vischeck_cache.end() || map->vischeck_cache[key].vischeck_state)
        {
            map->vischeck_cache[key] = { TICKCOUNT_TIMESTAMP(10), true };
        }
    }
}

void updateStuckTime()
{
    // No crumbs
    if (!crumbs.size())
        return;
    // We're stuck, add time to connection
    if (inactivity.check(*stuck_time / 2))
    {
        std::pair<CNavArea *, CNavArea *> key;
        // last crumb is invalid
        if (!last_crumb.navarea)
            key = std::pair<CNavArea *, CNavArea *>(crumbs[0].navarea, crumbs[0].navarea);
        else
            key = std::pair<CNavArea *, CNavArea *>(last_crumb.navarea, crumbs[0].navarea);

        // Expires in 10 seconds
        map->connection_stuck_time[key].expire_tick = TICKCOUNT_TIMESTAMP(*stuck_expire_time);
        // Stuck for one tick
        map->connection_stuck_time[key].time_stuck += 1;

        // We are stuck for too long, blastlist node for a while and repath
        if (map->connection_stuck_time[key].time_stuck > TIME_TO_TICKS(*stuck_detect_time))
        {
            map->vischeck_cache[key].expire_tick    = TICKCOUNT_TIMESTAMP(*stuck_blacklist_time);
            map->vischeck_cache[key].vischeck_state = false;
            if (log_pathing)
                logging::Info("Blackisted connection %d->%d", key.first->m_id, key.second->m_id);
            abandonPath();
        }
    }
}

void CreateMove()
{
    if (!isReady())
        return;
    if (CE_BAD(LOCAL_E) || !LOCAL_E->m_bAlivePlayer())
        return;

    if (vischeck_runtime)
        vischeckPath();

    followCrumbs();
    updateStuckTime();
    map->updateIgnores();
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

static CatCommand nav_path("nav_path", "Debug nav path", []() { NavEngine::navTo(loc, 5, true, true, false); });

static CatCommand nav_path_noreapth("nav_path_norepath", "Debug nav path", []() { NavEngine::navTo(loc, 5, false, true, false); });

static CatCommand nav_init("nav_init", "Reload nav mesh", []() {
    NavEngine::map.reset();
    NavEngine::LevelInit();
});

static CatCommand nav_debug_check("nav_debug_check", "Perform nav checks between two areas. First area: cat_nav_set Second area: Your location while running this command.", []() {
    if (!NavEngine::isReady())
        return;
    auto next    = NavEngine::map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
    auto current = NavEngine::map->findClosestNavSquare(loc);

    auto points = determinePoints(current, next);

    points.center = handleDropdown(points.center, points.next);

    // Too high for us to jump!
    if (points.center_next.z - points.center.z > PLAYER_JUMP_HEIGHT)
    {
        return logging::Info("Nav: Area too high!");
    }

    points.current.z += PLAYER_JUMP_HEIGHT;
    points.center.z += PLAYER_JUMP_HEIGHT;
    points.next.z += PLAYER_JUMP_HEIGHT;

    if (IsPlayerPassableNavigation(points.current, points.center) && IsPlayerPassableNavigation(points.center, points.next))
    {
        logging::Info("Nav: Area is player passable!");
    }
    else
    {
        logging::Info("Nav: Area is NOT player passable! %.2f,%.2f,%.2f %.2f,%.2f,%.2f %.2f,%.2f,%.2f", points.current.x, points.current.y, points.current.z, points.center.x, points.center.y, points.center.z, points.next.x, points.next.y, points.next.z);
    }
});

static CatCommand nav_debug_blacklist("nav_debug_blacklist", "Blacklist connection between two areas for 30s. First area: cat_nav_set Second area: Your location while running this command.", []() {
    if (!NavEngine::isReady())
        return;
    auto next    = NavEngine::map->findClosestNavSquare(g_pLocalPlayer->v_Origin);
    auto current = NavEngine::map->findClosestNavSquare(loc);

    std::pair<CNavArea *, CNavArea *> key(current, next);
    NavEngine::map->vischeck_cache[key].expire_tick    = TICKCOUNT_TIMESTAMP(30);
    NavEngine::map->vischeck_cache[key].vischeck_state = false;
    NavEngine::map->pather.Reset();
    logging::Info("Nav: Connection %d->%d Blacklisted.", current->m_id, next->m_id);
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
