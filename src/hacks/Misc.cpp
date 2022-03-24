/*
 * Misc.cpp
 *
 *  Created on: Nov 5, 2016
 *      Author: nullifiedcat
 */

#include "common.hpp"
#include <unistd.h>
#include <regex>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <hacks/AntiAim.hpp>
#include <settings/Bool.hpp>

#include "core/sharedobj.hpp"
#include "filesystem.h"
#include "DetourHook.hpp"
#include "AntiCheatBypass.hpp"

#include "hack.hpp"
#include <thread>

namespace hacks::shared::misc
{
#if !ENFORCE_STREAM_SAFETY && ENABLE_VISUALS
static settings::Boolean render_zoomed{ "visual.render-local-zoomed", "true" };
#endif
static settings::Boolean anti_afk{ "misc.anti-afk", "false" };
static settings::Int auto_strafe{ "misc.autostrafe", "0" };
static settings::Boolean tauntslide{ "misc.tauntslide-tf2c", "false" };
static settings::Boolean tauntslide_tf2{ "misc.tauntslide", "false" };
static settings::Boolean flashlight_spam{ "misc.flashlight-spam", "false" };
static settings::Boolean auto_balance_spam{ "misc.auto-balance-spam", "false" };
static settings::Boolean nopush_enabled{ "misc.no-push", "false" };
static settings::Boolean dont_hide_stealth_kills{ "misc.dont-hide-stealth-kills", "true" };
static settings::Boolean unlimit_bumpercart_movement{ "misc.bumpercarthax.enable", "true" };
static settings::Boolean ping_reducer{ "misc.ping-reducer.enable", "false" };
static settings::Int force_ping{ "misc.ping-reducer.target", "0" };
static settings::Boolean force_wait{ "misc.force-enable-wait", "true" };
static settings::Boolean scc{ "misc.scoreboard.match-custom-team-colors", "false" };

#if ENABLE_VISUALS
static settings::Boolean debug_info{ "misc.debug-info", "false" };
static settings::Boolean show_spectators{ "misc.show-spectators", "false" };
static settings::Boolean misc_drawhitboxes{ "misc.draw-hitboxes", "false" };
// Useful for debugging with showlagcompensation
static settings::Boolean misc_drawhitboxes_dead{ "misc.draw-hitboxes.dead-players", "false" };
#endif

#if ENABLE_TEXTMODE
static settings::Boolean fix_cyoaanim{ "remove.contracker", "true" };
#else
static settings::Boolean fix_cyoaanim{ "remove.contracker", "false" };
#endif

#if !ENFORCE_STREAM_SAFETY && ENABLE_VISUALS
static void tryPatchLocalPlayerShouldDraw(bool after)
{
    static BytePatch patch_shoulddraw{ gSignatures.GetClientSignature, "80 BB ? ? ? ? ? 75 DE", 0xD, { 0xE0 } };
    static BytePatch patch_shoulddraw_wearable{ gSignatures.GetClientSignature("0F 85 ? ? ? ? E9 ? ? ? ? 85 D2") + 1, { 0x80 } };

    if (after)
    {
        patch_shoulddraw.Patch();
        patch_shoulddraw_wearable.Patch();
    }
    else
    {
        patch_shoulddraw.Shutdown();
        patch_shoulddraw_wearable.Shutdown();
    }
}
#endif

static Timer anti_afk_timer{};
static int last_buttons{ 0 };

static void updateAntiAfk()
{
    if (current_user_cmd->buttons != last_buttons || g_pLocalPlayer->life_state)
    {
        anti_afk_timer.update();
        last_buttons = current_user_cmd->buttons;
    }
    else
    {
        static auto afk_timer = g_ICvar->FindVar("mp_idlemaxtime");
        if (!afk_timer)
            afk_timer = g_ICvar->FindVar("mp_idlemaxtime");
        // Trigger 10 seconds before kick
        else if (afk_timer->GetInt() != 0 && anti_afk_timer.check(afk_timer->m_nValue * 60 * 1000 - 10000))
        {
            // Game also checks if you move if you are in spawn, so spam movement keys alternatingly
            bool flip = false;
            current_user_cmd->buttons |= flip ? IN_FORWARD : IN_BACK;
            // Flip flip
            flip = !flip;
            if (anti_afk_timer.check(afk_timer->GetInt() * 60 * 1000 + 1000))
            {
                anti_afk_timer.update();
            }
        }
        last_buttons = current_user_cmd->buttons;
    }
}

CatCommand fix_cursor("fix_cursor", "Fix the GUI cursor being visible",
                      []()
                      {
                          g_ISurface->LockCursor();
                          g_ISurface->SetCursorAlwaysVisible(false);
                      });

// Use to send a autobalance request to the server that doesnt prevent you from
// using it again, Allowing infinite use of it.
void SendAutoBalanceRequest()
{ // Credits to blackfire
    if (!g_IEngine->IsInGame())
        return;
    KeyValues *kv = new KeyValues("AutoBalanceVolunteerReply");
    kv->SetInt("response", 1);
    g_IEngine->ServerCmdKeyValues(kv);
}

// Catcommand for above
CatCommand SendAutoBlRqCatCom("request_balance", "Request Infinite Auto-Balance", [](const CCommand &args) { SendAutoBalanceRequest(); });

int last_number{ 0 };
static bool flash_light_spam_switch{ false };
static Timer auto_balance_timer{};

static ConVar *teammatesPushaway{ nullptr };

int getCarriedBuilding()
{
    if (CE_INT(LOCAL_E, netvar.m_bCarryingObject))
        return HandleToIDX(CE_INT(LOCAL_E, netvar.m_hCarriedObject));
    for (int i = 1; i < MAX_ENTITIES; i++)
    {
        auto ent = ENTITY(i);
        if (CE_BAD(ent) || ent->m_Type() != ENTITY_BUILDING)
            continue;
        if (HandleToIDX(CE_INT(ent, netvar.m_hBuilder)) != LOCAL_E->m_IDX)
            continue;
        if (!CE_INT(ent, netvar.m_bPlacing))
            continue;
        return i;
    }
    return -1;
}
#if ENABLE_VISUALS

struct wireframe_data
{
    Vector raw_min;
    Vector raw_max;
    Vector rotation;
    Vector origin;
};

std::vector<wireframe_data> wireframe_queue;
void QueueWireframeHitboxes(hitbox_cache::EntityHitboxCache &hb_cache)
{
    for (int i = 0; i < hb_cache.GetNumHitboxes(); i++)
    {
        auto hb        = hb_cache.GetHitbox(i);
        Vector raw_min = hb->bbox->bbmin;
        Vector raw_max = hb->bbox->bbmax;
        auto transform = hb_cache.GetBones()[hb->bbox->bone];
        Vector rotation;
        Vector origin;

        MatrixAngles(transform, *(QAngle *) &rotation, origin);
        wireframe_queue.push_back(wireframe_data{ raw_min, raw_max, rotation, origin });
    }
}
void DrawWireframeHitbox(wireframe_data data)
{
    g_IVDebugOverlay->AddBoxOverlay2(data.origin, data.raw_min, data.raw_max, VectorToQAngle(data.rotation), Color(0, 0, 0, 0), Color(255, 0, 0, 255), g_GlobalVars->interval_per_tick * 2);
}

#endif

static float normalizeRad(float a) noexcept
{
    return std::isfinite(a) ? std::remainder(a, PI * 2) : 0.0f;
}
static float angleDiffRad(float a1, float a2) noexcept
{
    float delta;

    delta = normalizeRad(a1 - a2);
    if (a1 > a2)
    {
        if (delta >= PI)
            delta -= PI * 2;
    }
    else
    {
        if (delta <= -PI)
            delta += PI * 2;
    }
    return delta;
}

void CreateMove()
{
#if ENABLE_VISUALS
    if (misc_drawhitboxes)
    {
        for (int i = 0; i <= g_IEngine->GetMaxClients(); i++)
        {
            auto ent = ENTITY(i);
            if (CE_INVALID(ent) || ent == LOCAL_E || (!misc_drawhitboxes_dead && !ent->m_bAlivePlayer()))
                continue;
            QueueWireframeHitboxes(ent->hitboxes);
        }
    }
#endif
    if (current_user_cmd->command_number)
        last_number = current_user_cmd->command_number;
    // AntiAfk That after a certian time without movement keys depressed, causes
    // random keys to be spammed for 1 second
    if (anti_afk)
        updateAntiAfk();

    // Automatically strafes in the air
    if (auto_strafe && CE_GOOD(LOCAL_E) && !g_pLocalPlayer->life_state)
    {
        auto flags    = CE_INT(LOCAL_E, netvar.iFlags);
        auto movetype = (unsigned) CE_VAR(LOCAL_E, 0x194, unsigned char);

        // Noclip
        if (movetype != 8)
        {
            switch (*auto_strafe)
            {
            case 0: // Off
                break;
            case 1: // Regular strafe
            {
                static bool was_jumping = false;
                bool is_jumping         = current_user_cmd->buttons & IN_JUMP;

                if (!(flags & FL_ONGROUND) && !(flags & FL_INWATER) && (!is_jumping || was_jumping))
                    if (current_user_cmd->mousedx)
                        current_user_cmd->sidemove = current_user_cmd->mousedx > 1 ? 450.f : -450.f;

                was_jumping = is_jumping;

                break;
            }
            case 2: // Multidirectional Airstrafe,
                    // Huge Credits to https://github.com/degeneratehyperbola/NEPS, as their airstrafe
                    // Apparently just works for tf2
                    // Also credits to "zelta" for porting it to tf2,
                    // And "Cyanide" for making it work with cathook.
            {
                const float speed = CE_VECTOR(LOCAL_E, netvar.vVelocity).Length2D();
                auto vel          = CE_VECTOR(LOCAL_E, netvar.vVelocity);

                if (flags & FL_ONGROUND || flags & FL_INWATER)
                    break;
                if (speed < 2.0f)
                    break;

                constexpr auto perfectDelta = [](float speed) noexcept
                {
                    static auto speedVar = CE_FLOAT(LOCAL_E, netvar.m_flMaxspeed);
                    static auto airVar   = g_ICvar->FindVar("sv_airaccelerate");
                    // This is hardcoded for tf2, unless you run sourcemod
                    static auto wishSpeed = 30.0f;

                    const auto term = wishSpeed / airVar->GetFloat() / speedVar * 100.0f / speed;

                    if (term < 1.0f && term > -1.0f)
                        return acosf(term);

                    return 0.0f;
                };

                const float pDelta = perfectDelta(speed);
                if (pDelta)
                {
                    const float yaw     = DEG2RAD(g_pLocalPlayer->v_OrigViewangles.y);
                    const float velDir  = atan2f(vel.y, vel.x) - yaw;
                    const float wishAng = atan2f(-current_user_cmd->sidemove, current_user_cmd->forwardmove);
                    const float delta   = angleDiffRad(velDir, wishAng); // Helpers::angleDiffRad(velDir, wishAng);

                    float moveDir = delta < 0.0f ? velDir + pDelta : velDir - pDelta;

                    current_user_cmd->forwardmove = cosf(moveDir) * 450.0f;
                    current_user_cmd->sidemove    = -sinf(moveDir) * 450.0f;
                }
                break;
            }
            default:
                break;
            }
        }
    }

    // TF2c Tauntslide
    IF_GAME(IsTF2C())
    {
        if (tauntslide)
            RemoveCondition<TFCond_Taunting>(LOCAL_E);
    }

    // HL2DM flashlight spam
    IF_GAME(IsHL2DM())
    {
        if (flashlight_spam)
        {
            if (flash_light_spam_switch && !current_user_cmd->impulse)
                current_user_cmd->impulse = 100;
            flash_light_spam_switch = !flash_light_spam_switch;
        }
    }

    IF_GAME(IsTF2())
    {
        // Tauntslide needs improvement for movement but it mostly works
        if (tauntslide_tf2)
        {
            // Check to prevent crashing
            if (CE_GOOD(LOCAL_E))
            {
                if (HasCondition<TFCond_Taunting>(LOCAL_E))
                {
                    // get directions
                    float forward = 0;
                    float side    = 0;
                    if (current_user_cmd->buttons & IN_FORWARD)
                        forward += 450;
                    if (current_user_cmd->buttons & IN_BACK)
                        forward -= 450;
                    if (current_user_cmd->buttons & IN_MOVELEFT)
                        side -= 450;
                    if (current_user_cmd->buttons & IN_MOVERIGHT)
                        side += 450;
                    current_user_cmd->forwardmove = forward;
                    current_user_cmd->sidemove    = side;

                    QAngle camera_angle;
                    g_IEngine->GetViewAngles(camera_angle);

                    // Doesnt work with anti-aim as well as I hoped... I guess
                    // this is as far as I can go with such a simple tauntslide
                    if (!hacks::shared::antiaim::isEnabled())
                        current_user_cmd->viewangles.y = camera_angle[1];
                    g_pLocalPlayer->v_OrigViewangles.y = camera_angle[1];

                    // Use silent since we dont want to prevent the player from
                    // looking around
                    g_pLocalPlayer->bUseSilentAngles = true;
                }
            }
        }

        // Spams infinite autobalance spam function
        if (auto_balance_spam && auto_balance_timer.test_and_set(150))
            SendAutoBalanceRequest();

        // Simple No-Push through cvars
        if (teammatesPushaway)
        {
            if (*nopush_enabled == teammatesPushaway->GetBool())
                teammatesPushaway->SetValue(!nopush_enabled);
        }
        else
            teammatesPushaway = g_ICvar->FindVar("tf_avoidteammates_pushaway");

        // Ping Reducer
        if (ping_reducer && !hacks::tf2::antianticheat::enabled)
        {
            static ConVar *cmdrate = g_ICvar->FindVar("cl_cmdrate");
            if (cmdrate == nullptr)
            {
                cmdrate = g_ICvar->FindVar("cl_cmdrate");
                return;
            }
            int ping = g_pPlayerResource->GetPing(g_IEngine->GetLocalPlayer());
            static Timer updateratetimer{};
            if (updateratetimer.test_and_set(500))
            {
                if (*force_ping <= ping)
                {
                    NET_SetConVar command("cl_cmdrate", "-1");
                    ((INetChannel *) g_IEngine->GetNetChannelInfo())->SendNetMsg(command);
                }
                else if (*force_ping > ping)
                {
                    NET_SetConVar command("cl_cmdrate", std::to_string(cmdrate->GetInt()).c_str());
                    ((INetChannel *) g_IEngine->GetNetChannelInfo())->SendNetMsg(command);
                }
            }
        }
    }
}

#if ENABLE_VISUALS
// Timer ussr{};
void Draw()
{
    if (misc_drawhitboxes)
    {
        for (auto &entry : wireframe_queue)
            DrawWireframeHitbox(entry);
        wireframe_queue.clear();
    }
    /*if (ussr.test_and_set(207000))
    {
        g_ISurface->PlaySound()
    }*/
    if (show_spectators)
    {
        for (int i = 0; i < PLAYER_ARRAY_SIZE; i++)
        {
            // Assign the for loops tick number to an ent
            CachedEntity *ent = ENTITY(i);
            player_info_s info;
            if (!CE_BAD(ent) && ent != LOCAL_E && ent->m_Type() == ENTITY_PLAYER && (CE_INT(ent, netvar.hObserverTarget) & 0xFFF) == LOCAL_E->m_IDX && CE_INT(ent, netvar.iObserverMode) >= 4 && GetPlayerInfo(i, &info))
            {
                auto observermode = "N/A";
                rgba_t color      = colors::green;

                switch (CE_INT(ent, netvar.iObserverMode))
                {
                case 4:
                    observermode = "1st Person";
                    color        = colors::red_s;
                    break;
                case 5:
                    observermode = "3rd Person";
                    break;
                case 7:
                    observermode = "Freecam";
                    break;
                }
                AddSideString(format(info.name, " - (", observermode, ")"), color);
            }
        }
    }
    if (!debug_info)
        return;
    auto local = LOCAL_W;
    if (CE_GOOD(local))
    {
        AddSideString(format("Slot: ", re::C_BaseCombatWeapon::GetSlot(RAW_ENT(local))));
        AddSideString(format("Taunt Concept: ", CE_INT(LOCAL_E, netvar.m_iTauntConcept)));
        AddSideString(format("Taunt Index: ", CE_INT(LOCAL_E, netvar.m_iTauntIndex)));
        AddSideString(format("Sequence: ", CE_INT(LOCAL_E, netvar.m_nSequence)));
        AddSideString(format("Velocity: ", LOCAL_E->m_vecVelocity.x, ' ', LOCAL_E->m_vecVelocity.y, ' ', LOCAL_E->m_vecVelocity.z));
        AddSideString(format("Velocity3: ", LOCAL_E->m_vecVelocity.Length()));
        AddSideString(format("Velocity2: ", LOCAL_E->m_vecVelocity.Length2D()));
        AddSideString("NetVar Velocity");
        Vector vel = CE_VECTOR(LOCAL_E, netvar.vVelocity);
        AddSideString(format("Velocity: ", vel.x, ' ', vel.y, ' ', vel.z));
        AddSideString(format("Velocity3: ", vel.Length()));
        AddSideString(format("Velocity2: ", vel.Length2D()));
        AddSideString(format("flSimTime: ", LOCAL_E->var<float>(netvar.m_flSimulationTime)));
        if (current_user_cmd)
            AddSideString(format("command_number: ", last_cmd_number));
        AddSideString(format("clip: ", CE_INT(g_pLocalPlayer->weapon(), netvar.m_iClip1)));
        if (local->m_iClassID() == CL_CLASS(CTFMinigun))
            AddSideString(format("Weapon state: ", CE_INT(local, netvar.iWeaponState)));
        AddSideString(format("ItemDefinitionIndex: ", CE_INT(local, netvar.iItemDefinitionIndex)));
        AddSideString(format("Maxspeed: ", CE_FLOAT(LOCAL_E, netvar.m_flMaxspeed)));
        /*AddSideString(colors::white, "Weapon: %s [%i]",
        RAW_ENT(g_pLocalPlayer->weapon())->GetClientClass()->GetName(),
        g_pLocalPlayer->weapon()->m_iClassID());
        //AddSideString(colors::white, "flNextPrimaryAttack: %f",
        CE_FLOAT(g_pLocalPlayer->weapon(), netvar.flNextPrimaryAttack));
        //AddSideString(colors::white, "nTickBase: %f",
        (float)(CE_INT(g_pLocalPlayer->entity, netvar.nTickBase)) *
        gvars->interval_per_tick); AddSideString(colors::white, "CanShoot: %i",
        CanShoot());
        //AddSideString(colors::white, "Damage: %f",
        CE_FLOAT(g_pLocalPlayer->weapon(), netvar.flChargedDamage)); if (TF2)
        AddSideString(colors::white, "DefIndex: %i",
        CE_INT(g_pLocalPlayer->weapon(), netvar.iItemDefinitionIndex));
        //AddSideString(colors::white, "GlobalVars: 0x%08x", gvars);
        //AddSideString(colors::white, "realtime: %f", gvars->realtime);
        //AddSideString(colors::white, "interval_per_tick: %f",
        gvars->interval_per_tick);
        //if (TF2) AddSideString(colors::white, "ambassador_can_headshot: %i",
        (gvars->curtime - CE_FLOAT(g_pLocalPlayer->weapon(),
        netvar.flLastFireTime)) > 0.95); AddSideString(colors::white,
        "WeaponMode: %i", GetWeaponMode(g_pLocalPlayer->entity));
        AddSideString(colors::white, "ToGround: %f",
        DistanceToGround(g_pLocalPlayer->v_Origin));
        AddSideString(colors::white, "ServerTime: %f",
        CE_FLOAT(g_pLocalPlayer->entity, netvar.nTickBase) *
        g_GlobalVars->interval_per_tick); AddSideString(colors::white, "CurTime:
        %f", g_GlobalVars->curtime); AddSideString(colors::white, "FrameCount:
        %i", g_GlobalVars->framecount); float speed, gravity;
        GetProjectileData(g_pLocalPlayer->weapon(), speed, gravity);
        AddSideString(colors::white, "ALT: %i",
        g_pLocalPlayer->bAttackLastTick); AddSideString(colors::white, "Speed:
        %f", speed); AddSideString(colors::white, "Gravity: %f", gravity);
        AddSideString(colors::white, "CIAC: %i", *(bool*)(RAW_ENT(LOCAL_W) +
        2380)); if (TF2) AddSideString(colors::white, "Melee: %i",
        vfunc<bool(*)(IClientEntity*)>(RAW_ENT(LOCAL_W), 1860 / 4,
        0)(RAW_ENT(LOCAL_W))); if (TF2) AddSideString(colors::white, "Bucket:
        %.2f", *(float*)((uintptr_t)RAW_ENT(LOCAL_W) + 2612u));
        //if (TF2C) AddSideString(colors::white, "Seed: %i",
        *(int*)(sharedobj::client->lmap->l_addr + 0x00D53F68ul));
        //AddSideString(colors::white, "IsZoomed: %i", g_pLocalPlayer->bZoomed);
        //AddSideString(colors::white, "CanHeadshot: %i", CanHeadshot());
        //AddSideString(colors::white, "IsThirdPerson: %i",
        iinput->CAM_IsThirdPerson());
        //if (TF2C) AddSideString(colors::white, "Crits: %i", s_bCrits);
        //if (TF2C) AddSideString(colors::white, "CritMult: %i",
        RemapValClampedNC( CE_INT(LOCAL_E, netvar.iCritMult), 0, 255, 1.0, 6 ));
        for (int i = 0; i <= HIGHEST_ENTITY; i++) {
            CachedEntity* e = ENTITY(i);
            if (CE_GOOD(e)) {
                if (e->m_Type() == EntityType::ENTITY_PROJECTILE) {
                    //logging::Info("Entity %i [%s]: V %.2f (X: %.2f, Y: %.2f,
        Z: %.2f) ACC %.2f (X: %.2f, Y: %.2f, Z: %.2f)", i,
        RAW_ENT(e)->GetClientClass()->GetName(), e->m_vecVelocity.Length(),
        e->m_vecVelocity.x, e->m_vecVelocity.y, e->m_vecVelocity.z,
        e->m_vecAcceleration.Length(), e->m_vecAcceleration.x,
        e->m_vecAcceleration.y, e->m_vecAcceleration.z);
                    AddSideString(colors::white, "Entity %i [%s]: V %.2f (X:
        %.2f, Y: %.2f, Z: %.2f) ACC %.2f (X: %.2f, Y: %.2f, Z: %.2f)", i,
        RAW_ENT(e)->GetClientClass()->GetName(), e->m_vecVelocity.Length(),
        e->m_vecVelocity.x, e->m_vecVelocity.y, e->m_vecVelocity.z,
        e->m_vecAcceleration.Length(), e->m_vecAcceleration.x,
        e->m_vecAcceleration.y, e->m_vecAcceleration.z);
                }
            }
        }//AddSideString(draw::white, draw::black, "???: %f",
        NET_FLOAT(g_pLocalPlayer->entity, netvar.test));
        //AddSideString(draw::white, draw::black, "VecPunchAngle: %f %f %f",
        pa.x, pa.y, pa.z);
        //draw::DrawString(10, y, draw::white, draw::black, false,
        "VecPunchAngleVel: %f %f %f", pav.x, pav.y, pav.z);
        //y += 14;
        //AddCenterString(fonts::font_handle,
        input->GetAnalogValue(AnalogCode_t::MOUSE_X),
        input->GetAnalogValue(AnalogCode_t::MOUSE_Y), draw::white,
        L"S\u0FD5");*/
    }
}

#endif

void generate_schema()
{
    std::ifstream in("tf/scripts/items/items_game.txt");
    std::string outS((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::ofstream out(paths::getDataPath("/items_game.txt"));
    std::regex a("\"equip_regions?\".*?\".*?\"");
    std::regex b("\"equip_regions?\"\\s*?\\n\\s*?\\{[\\s\\S\\n]*?\\}");
    outS = std::regex_replace(outS, a, "");
    out << std::regex_replace(outS, b, "");
    out.close();
    logging::Info("Generating complete!");
}
static CatCommand generateschema("schema_generate", "Generate custom schema", generate_schema);

bool InitSchema(const char *fileName, const char *pathID, CUtlVector<CUtlString> *pVecErrors /* = NULL */)
{
    static auto GetItemSchema = reinterpret_cast<void *(*) (void)>(gSignatures.GetClientSignature("55 89 E5 57 56 53 83 EC ? 8B 1D ? ? ? ? 85 DB 89 D8"));

    static auto BInitTextBuffer = reinterpret_cast<bool (*)(void *, CUtlBuffer &, CUtlVector<CUtlString> *)>(gSignatures.GetClientSignature("55 89 E5 57 56 53 8D 9D ? ? ? ? 81 EC ? ? ? ? 8B 7D ? 89 1C 24 "));
    void *schema                = (void *) ((uintptr_t) GetItemSchema() + 0x4);

    // Read the raw data
    CUtlBuffer bufRawData;
    bool bReadFileOK = g_IFileSystem->ReadFile(fileName, pathID, bufRawData);
    // Log something here if bReadFileOK is false

    if (!bReadFileOK)
    {
        logging::Info(("Failed reading item schema from " + std::string(fileName)).c_str());
        return false;
    }

    else
        logging::Info(("Read item schema from " + std::string(fileName)).c_str());

    // Wrap it with a text buffer reader
    CUtlBuffer bufText(bufRawData.Base(), bufRawData.TellPut(), CUtlBuffer::READ_ONLY | CUtlBuffer::TEXT_BUFFER);

    // Use the standard init path
    return BInitTextBuffer(schema, bufText, pVecErrors);
}

void Schema_Reload()
{
    logging::Info("Loading item schema...");
    bool ret = InitSchema(paths::getDataPath("/items_game.txt").c_str(), nullptr, nullptr);
    logging::Info("Loading %s", ret ? "Successful" : "Unsuccessful");
}

CatCommand schema("schema", "Load custom schema", Schema_Reload);

CatCommand update_gui_color("gui_color_update", "Update the GUI Color",
                            []()
                            {
                                hack::command_stack().push("cat set zk.style.tab-button.color.selected.background 446498ff;cat set zk.style.tab-button.color.separator 446498ff;cat set zk.style.tab-button.color.hover.underline 446498ff;cat set zk.style.tab-button.color.selected.underline 446498ff;cat set zk.style.tooltip.border 446498ff;cat set zk.style.box.color.border 446498ff;cat set zk.style.color-preview.color.border 446498ff;cat set zk.style.modal-container.color.border 446498ff;cat set zk.style.tab-selection.color.border 446498ff;cat set zk.style.table.color.border 446498ff;cat set zk.style.checkbox.color.border 446498ff;cat set zk.style.checkbox.color.checked 446498ff;cat set zk.style.checkbox.color.hover 00a098ff;cat set zk.style.input.color.border 446498ff;cat set zk.style.input.key.color.border 446498ff;cat set zk.style.input.select.border 446498ff;cat set zk.style.input.slider.color.handle_border 446498ff;cat set zk.style.input.slider.color.bar 446498ff;cat set zk.style.input.text.color.border.active 42BC99ff");
                                hack::command_stack().push("cat set zk.style.input.text.color.border.inactive 446498ff;cat set zk.style.tree-list-entry.color.lines 42BC99ff;cat set zk.style.task.color.background.hover 446498ff;cat set zk.style.task.color.border 446498ff;cat set zk.style.taskbar.color.border 446498ff;cat set zk.style.window.color.border 446498ff;cat set zk.style.window-close-button.color.border 446498ff;cat set zk.style.window-header.color.background.active 446498ff;cat set zk.style.window-header.color.border.inactive 446498ff;cat set zk.style.window-header.color.border.active 446498ff");
                            });

CatCommand name("name_set", "Immediate name change",
                [](const CCommand &args)
                {
                    if (args.ArgC() < 2)
                    {
                        logging::Info("Set a name, silly");
                        return;
                    }
                    if (g_Settings.bInvalid)
                    {
                        logging::Info("Only works ingame!");
                        return;
                    }
                    std::string new_name(args.ArgS());
                    ReplaceSpecials(new_name);
                    NET_SetConVar setname("name", new_name.c_str());
                    INetChannel *ch = (INetChannel *) g_IEngine->GetNetChannelInfo();
                    if (ch)
                    {
                        setname.SetNetChannel(ch);
                        setname.SetReliable(false);
                        ch->SendNetMsg(setname, false);
                    }
                });
CatCommand set_value("set", "Set value",
                     [](const CCommand &args)
                     {
                         if (args.ArgC() < 2)
                             return;
                         ConVar *var = g_ICvar->FindVar(args.Arg(1));
                         if (!var)
                             return;
                         std::string value(args.Arg(2));
                         ReplaceSpecials(value);
                         var->m_fMaxVal = 999999999.9f;
                         var->m_fMinVal = -999999999.9f;
                         var->SetValue(value.c_str());
                         logging::Info("Set '%s' to '%s'", args.Arg(1), value.c_str());
                     });
CatCommand get_value("get", "Set value",
                     [](const CCommand &args)
                     {
                         if (args.ArgC() < 2)
                             return;
                         ConVar *var = g_ICvar->FindVar(args.Arg(1));
                         if (!var)
                             return;
                         logging::Info("'%s': '%s'", args.Arg(1), var->GetString());
                     });
CatCommand say_lines("say_lines", "Say with newlines (\\n)",
                     [](const CCommand &args)
                     {
                         std::string message(args.ArgS());
                         ReplaceSpecials(message);
                         std::string cmd = format("say ", message);
                         g_IEngine->ServerCmd(cmd.c_str());
                     });
CatCommand disconnect("disconnect", "Disconnect with custom reason",
                      [](const CCommand &args)
                      {
                          INetChannel *ch = (INetChannel *) g_IEngine->GetNetChannelInfo();
                          if (!ch)
                              return;
                          std::string string = args.ArgS();
                          ReplaceSpecials(string);
                          ch->Shutdown(string.c_str());
                      });

CatCommand disconnect_vac("disconnect_vac", "Disconnect (fake VAC)",
                          []()
                          {
                              INetChannel *ch = (INetChannel *) g_IEngine->GetNetChannelInfo();
                              if (!ch)
                                  return;
                              ch->Shutdown("VAC banned from secure server\n");
                          });

// Netvars stuff
void DumpRecvTable(CachedEntity *ent, RecvTable *table, int depth, const char *ft, unsigned acc_offset)
{
    bool forcetable = ft && strlen(ft);
    if (!forcetable || !strcmp(ft, table->GetName()))
        logging::Info("==== TABLE: %s", table->GetName());
    for (int i = 0; i < table->GetNumProps(); i++)
    {
        RecvProp *prop = table->GetProp(i);
        if (!prop)
            continue;
        if (prop->GetDataTable())
        {
            DumpRecvTable(ent, prop->GetDataTable(), depth + 1, ft, acc_offset + prop->GetOffset());
        }
        if (forcetable && strcmp(ft, table->GetName()))
            continue;
        switch (prop->GetType())
        {
        case SendPropType::DPT_Float:
            logging::Info("TABLE %s IN DEPTH %d: %s [0x%04x] = %f", table ? table->GetName() : "none", depth, prop->GetName(), prop->GetOffset(), CE_FLOAT(ent, acc_offset + prop->GetOffset()));
            break;
        case SendPropType::DPT_Int:
            logging::Info("TABLE %s IN DEPTH %d: %s [0x%04x] = %i | %u | %hd | %hu", table ? table->GetName() : "none", depth, prop->GetName(), prop->GetOffset(), CE_INT(ent, acc_offset + prop->GetOffset()), CE_VAR(ent, acc_offset + prop->GetOffset(), unsigned int), CE_VAR(ent, acc_offset + prop->GetOffset(), short), CE_VAR(ent, acc_offset + prop->GetOffset(), unsigned short));
            break;
        case SendPropType::DPT_String:
            logging::Info("TABLE %s IN DEPTH %d: %s [0x%04x] = 'not yet supported'", table ? table->GetName() : "none", depth, prop->GetName(), prop->GetOffset());
            break;
        case SendPropType::DPT_Vector:
            logging::Info("TABLE %s IN DEPTH %d: %s [0x%04x] = (%f, %f, %f)", table ? table->GetName() : "none", depth, prop->GetName(), prop->GetOffset(), CE_FLOAT(ent, acc_offset + prop->GetOffset()), CE_FLOAT(ent, acc_offset + prop->GetOffset() + 4), CE_FLOAT(ent, acc_offset + prop->GetOffset() + 8));
            break;
        case SendPropType::DPT_VectorXY:
            logging::Info("TABLE %s IN DEPTH %d: %s [0x%04x] = (%f, %f)", table ? table->GetName() : "none", depth, prop->GetName(), prop->GetOffset(), CE_FLOAT(ent, acc_offset + prop->GetOffset()), CE_FLOAT(ent, acc_offset + prop->GetOffset() + 4));
            break;
            // There are some missing things here
        default:
            break;
        }
    }
    if (!ft || !strcmp(ft, table->GetName()))
        logging::Info("==== END OF TABLE: %s IN DEAPTH %d", table->GetName(), depth);
}

// CatCommand to dumb netvar info
static CatCommand dump_vars("debug_dump_netvars", "Dump netvars of entity",
                            [](const CCommand &args)
                            {
                                if (args.ArgC() < 2)
                                    return;
                                if (!atoi(args[1]))
                                    return;
                                int idx           = atoi(args[1]);
                                CachedEntity *ent = ENTITY(idx);
                                if (CE_BAD(ent))
                                    return;
                                ClientClass *clz = RAW_ENT(ent)->GetClientClass();
                                logging::Info("Entity %i: %s", ent->m_IDX, clz->GetName());
                                const char *ft = (args.ArgC() > 1 ? args[2] : 0);
                                DumpRecvTable(ent, clz->m_pRecvTable, 0, ft, 0);
                            });
static CatCommand dump_vars_by_name("debug_dump_netvars_name", "Dump netvars of entity with target name",
                                    [](const CCommand &args)
                                    {
                                        if (args.ArgC() < 2)
                                            return;
                                        std::string name(args.Arg(1));
                                        for (int i = 0; i <= HIGHEST_ENTITY; i++)
                                        {
                                            CachedEntity *ent = ENTITY(i);
                                            if (CE_BAD(ent))
                                                continue;
                                            ClientClass *clz = RAW_ENT(ent)->GetClientClass();
                                            if (!clz)
                                                continue;
                                            std::string clazz_name(clz->GetName());
                                            if (clazz_name.find(name) == clazz_name.npos)
                                                continue;
                                            logging::Info("Entity %i: %s", ent->m_IDX, clz->GetName());
                                            const char *ft = (args.ArgC() > 1 ? args[2] : 0);
                                            DumpRecvTable(ent, clz->m_pRecvTable, 0, ft, 0);
                                        }
                                    });

static CatCommand debug_print_weaponid("debug_weaponid", "Print the weapon IDs of all currently equiped weapons",
                                       [](const CCommand &)
                                       {
                                           // Invalid player
                                           if (CE_BAD(LOCAL_E))
                                               return;
                                           int *hWeapons = &CE_INT(LOCAL_E, netvar.hMyWeapons);
                                           // Go through the handle array and search for the item
                                           for (int i = 0; hWeapons[i]; i++)
                                           {
                                               if (IDX_BAD(HandleToIDX(hWeapons[i])))
                                                   continue;
                                               // Get the weapon
                                               CachedEntity *weapon = ENTITY(HandleToIDX(hWeapons[i]));
                                               // Print weaponid
                                               logging::Info("weapon %i: %i", i, re::C_TFWeaponBase::GetWeaponID(RAW_ENT(weapon)));
                                           }
                                       });

static CatCommand print_eye_diff("debug_print_eye_diff", "debug", []() { logging::Info("%f", g_pLocalPlayer->v_Eye.z - LOCAL_E->m_vecOrigin().z); });
void Shutdown()
{
}

static ProxyFnHook cyoa_anim_hook{};

void cyoaview_nethook(const CRecvProxyData *data, void *pPlayer, void *out)
{
    int value      = data->m_Value.m_Int;
    int *value_out = (int *) out;
    if (!fix_cyoaanim)
    {
        *value_out = value;
        return;
    }
    // Mark as not doing cyoa thing
    if (CE_BAD(LOCAL_E) || pPlayer != RAW_ENT(LOCAL_E))
        *value_out = false;
}

// This function does two things.
// 1. It patches the check that's supposed to update the "isWaitCommandEnabled" on the cvar buffer to always skip
// 2. It sets the wait command to enabled
// The Former is the main logic, the latter is so you don't accidentally perma disable it either
inline void force_wait_func(bool after)
{
    static auto enable_wait_signature = gSignatures.GetEngineSignature("74 ? A2 ? ? ? ? C7 44 24 ? 01 00 00 00");
    // Jump if not overflow, aka always jump in this case
    static BytePatch patch_wait(enable_wait_signature, { 0x71 });
    if (after)
    {
        // Enable the wait command
        int **enable_wait = (int **) (enable_wait_signature + 3);

        BytePatch::mprotectAddr((uintptr_t) enable_wait, 4, PROT_READ | PROT_WRITE | PROT_EXEC);
        BytePatch::mprotectAddr((uintptr_t) *enable_wait, 4, PROT_READ | PROT_WRITE | PROT_EXEC);

        **enable_wait = true;
        BytePatch::mprotectAddr((uintptr_t) enable_wait, 4, PROT_EXEC);

        patch_wait.Patch();
    }
    else
        patch_wait.Shutdown();
}

void callback_force_wait(settings::VariableBase<bool> &, bool after)
{
    force_wait_func(after);
}

static InitRoutine init(
    []()
    {
        HookNetvar({ "DT_TFPlayer", "m_bViewingCYOAPDA" }, cyoa_anim_hook, cyoaview_nethook);

        force_wait.installChangeCallback(callback_force_wait);
        force_wait_func(true);

        teammatesPushaway = g_ICvar->FindVar("tf_avoidteammates_pushaway");
        EC::Register(EC::Shutdown, Shutdown, "draw_local_player", EC::average);
        EC::Register(EC::CreateMove, CreateMove, "cm_misc_hacks", EC::average);
        EC::Register(EC::CreateMoveWarp, CreateMove, "cmw_misc_hacks", EC::average);
#if ENABLE_VISUALS
        EC::Register(EC::Draw, Draw, "draw_misc_hacks", EC::average);
#endif
    });
} // namespace hacks::shared::misc

/*void DumpRecvTable(CachedEntity* ent, RecvTable* table, int depth, const char*
ft, unsigned acc_offset) { bool forcetable = ft && strlen(ft); if (!forcetable
|| !strcmp(ft, table->GetName())) logging::Info("==== TABLE: %s",
table->GetName()); for (int i = 0; i < table->GetNumProps(); i++) { RecvProp*
prop = table->GetProp(i); if (!prop) continue; if (prop->GetDataTable()) {
            DumpRecvTable(ent, prop->GetDataTable(), depth + 1, ft, acc_offset +
prop->GetOffset());
        }
        if (forcetable && strcmp(ft, table->GetName())) continue;
        switch (prop->GetType()) {
        case SendPropType::DPT_Float:
            logging::Info("%s [0x%04x] = %f", prop->GetName(),
prop->GetOffset(), CE_FLOAT(ent, acc_offset + prop->GetOffset())); break; case
SendPropType::DPT_Int: logging::Info("%s [0x%04x] = %i | %u | %hd | %hu",
prop->GetName(), prop->GetOffset(), CE_INT(ent, acc_offset + prop->GetOffset()),
CE_VAR(ent, acc_offset +  prop->GetOffset(), unsigned int), CE_VAR(ent,
acc_offset + prop->GetOffset(), short), CE_VAR(ent, acc_offset +
prop->GetOffset(), unsigned short)); break; case SendPropType::DPT_String:
            logging::Info("%s [0x%04x] = %s", prop->GetName(),
prop->GetOffset(), CE_VAR(ent, prop->GetOffset(), char*)); break; case
SendPropType::DPT_Vector: logging::Info("%s [0x%04x] = (%f, %f, %f)",
prop->GetName(), prop->GetOffset(), CE_FLOAT(ent, acc_offset +
prop->GetOffset()), CE_FLOAT(ent, acc_offset + prop->GetOffset() + 4),
CE_FLOAT(ent, acc_offset + prop->GetOffset() + 8)); break; case
SendPropType::DPT_VectorXY: logging::Info("%s [0x%04x] = (%f, %f)",
prop->GetName(), prop->GetOffset(), CE_FLOAT(ent, acc_offset +
prop->GetOffset()), CE_FLOAT(ent,acc_offset +  prop->GetOffset() + 4)); break;
        }

    }
    if (!ft || !strcmp(ft, table->GetName()))
        logging::Info("==== END OF TABLE: %s", table->GetName());
}

void CC_DumpVars(const CCommand& args) {
    if (args.ArgC() < 1) return;
    if (!atoi(args[1])) return;
    int idx = atoi(args[1]);
    CachedEntity* ent = ENTITY(idx);
    if (CE_BAD(ent)) return;
    ClientClass* clz = RAW_ENT(ent)->GetClientClass();
    logging::Info("Entity %i: %s", ent->m_IDX, clz->GetName());
    const char* ft = (args.ArgC() > 1 ? args[2] : 0);
    DumpRecvTable(ent, clz->m_pRecvTable, 0, ft, 0);
}*/
