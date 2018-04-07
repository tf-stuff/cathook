/*
 * hack.cpp
 *
 *  Created on: Oct 3, 2016
 *      Author: nullifiedcat
 */

#include "hack.hpp"
#include "common.hpp"

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)

#include "CDumper.hpp"

/*
 *  Credits to josh33901 aka F1ssi0N for butifel F1Public and Darkstorm 2015
 * Linux
 */

bool hack::shutdown    = false;
bool hack::initialized = false;

const std::string &hack::GetVersion()
{
    static std::string version("Unknown Version");
    static bool version_set = false;
    if (version_set)
        return version;
#if defined(GIT_COMMIT_HASH) && defined(GIT_COMMIT_DATE)
    version = "Version: #" GIT_COMMIT_HASH " " GIT_COMMIT_DATE;
#endif
    version_set = true;
    return version;
}

const std::string &hack::GetType()
{
    static std::string version("Unknown Type");
    static bool version_set = false;
    if (version_set)
        return version;
    version = "";
#if not defined(ENABLE_IPC)
    version += " NOIPC";
#endif
#if not ENABLE_GUI
    version += " NOGUI";
#else
    version += " IMGUI";
#endif

#ifndef DYNAMIC_CLASSES

#ifdef BUILD_GAME
    version += " GAME " TO_STRING(BUILD_GAME);
#else
    version += " UNIVERSAL";
#endif

#else
    version += " DYNAMIC";
#endif

#if not ENABLE_VISUALS
    version += " NOVISUALS";
#endif

    version     = version.substr(1);
    version_set = true;
    return version;
}

std::mutex hack::command_stack_mutex;
std::stack<std::string> &hack::command_stack()
{
    static std::stack<std::string> stack;
    return stack;
}

#if ENABLE_VISUALS == 1 /* Why would we need colored chat stuff in textmode?   \
                         */
#define red 184, 56, 59, 255
#define blu 88, 133, 162, 255
class AdvancedEventListener : public IGameEventListener
{
public:
    virtual void FireGameEvent(KeyValues *event)
    {
        if (!event_log)
            return;
        static ConVar *var = g_ICvar->FindVar("developer");
        static ConVar *filter = g_ICvar->FindVar("con_filter_text");
        static ConVar *enable = g_ICvar->FindVar("con_filter_enable");
        filter->SetValue("[CAT]");
        var->SetValue(1);
        enable->SetValue(1);
        const char *name = event->GetName();
        if (!strcmp(name, "player_connect_client"))
            PrintChat("\x07%06X%s\x01 \x07%06X%s\x01 joining", 0xa06ba0,event->GetString("name"), 0x914e65,event->GetString("networkid"));
        else if (!strcmp(name, "player_activate"))
        {
            int uid    = event->GetInt("userid");
            int entity = g_IEngine->GetPlayerForUserID(uid);
            player_info_s info;
            if (g_IEngine->GetPlayerInfo(entity, &info))
            	PrintChat("\x07%06X%s\x01 connected", 0xa06ba0, info.name);

        }
        else if (!strcmp(name, "player_disconnect"))
        {
            CachedEntity *player =
                ENTITY(g_IEngine->GetPlayerForUserID(event->GetInt("userid")));
            PrintChat("\x07%06X%s\x01 \x07%06X%s\x01 disconnected",colors::chat::team(player->m_iTeam),event->GetString("name"), 0x914e65,event->GetString("networkid"));
        }
        else if (!strcmp(name, "player_team"))
        {
            if (event->GetBool("disconnect") != 1)
            {
                int oteam           = event->GetInt("oldteam");
                int nteam           = event->GetInt("team");
                const char *oteam_s = teamname(oteam);
                const char *nteam_s = teamname(nteam);
                PrintChat("\x07%06X%s\x01 changed team (\x07%06X%s\x01 -> ""\x07%06X%s\x01)",0xa06ba0, event->GetString("name"),colors::chat::team(oteam), oteam_s,colors::chat::team(nteam), nteam_s);
            }
        }
        else if (!strcmp(name, "player_hurt")) {
        	int victim = event->GetInt("userid");
        	int attacker = event->GetInt("attacker");
        	int health = event->GetInt("health");
        	player_info_s kinfo;
        	player_info_s vinfo;
        	g_IEngine->GetPlayerInfo(g_IEngine->GetPlayerForUserID(victim), &vinfo);
        	g_IEngine->GetPlayerInfo(g_IEngine->GetPlayerForUserID(attacker), &kinfo);
        	PrintChat("\x07%06X%s\x01 hurt \x07%06X%s\x01 down to \x07%06X%d\x01hp", 0x4286f4, kinfo.name, 0xc11739, vinfo.name, 0x2aaf18, health);
        }
        else if (!strcmp(name, "player_death")) {
        	int victim = event->GetInt("userid");
        	int attacker = event->GetInt("attacker");
        	player_info_s kinfo;
        	player_info_s vinfo;
        	g_IEngine->GetPlayerInfo(g_IEngine->GetPlayerForUserID(victim), &vinfo);
        	g_IEngine->GetPlayerInfo(g_IEngine->GetPlayerForUserID(attacker), &kinfo);
        	PrintChat("\x07%06X%s\x01 killed \x07%06X%s\x01", 0x4286f4, kinfo.name, 0xc11739, vinfo.name);
        }
        else if (!strcmp(name, "player_spawn")) {
        	int id = event->GetInt("userid");
        	player_info_s info;
        	g_IEngine->GetPlayerInfo(g_IEngine->GetPlayerForUserID(id), &info);
        	PrintChat("\x07%06X%s\x01 (re)spawned", 0xa06ba0, info.name);
        }
        else if (!strcmp(name, "player_changeclass")) {
        	int id = event->GetInt("userid");
        	player_info_s info;
        	PrintChat("\x07%06X%s\x01 changed to \x07%06X%s\x01", 0xa06ba0, info.name, 0xa06ba0, classname(event->GetInt("class")));
        }
        else if (!strcmp(name, "player_builtobject")) {
        	int obj = event->GetInt("object");

        	logging::Info("%d, %d, %d", obj == ENTITY_BUILDING, obj == CL_CLASS(CObjectSentrygun), obj);
        }
    }
};

AdvancedEventListener adv_event_listener{};

#endif /* TEXTMODE */

void hack::ExecuteCommand(const std::string command)
{
    std::lock_guard<std::mutex> guard(hack::command_stack_mutex);
    hack::command_stack().push(command);
}

ConCommand *hack::c_Cat = 0;

void hack::CC_Cat(const CCommand &args)
{
    g_ICvar->ConsoleColorPrintf(Color(255, 255, 255, 255), "cathook");
    g_ICvar->ConsoleColorPrintf(Color(0, 0, 255, 255), " by ");
    g_ICvar->ConsoleColorPrintf(Color(255, 0, 0, 255), "nullifiedcat\n");
}

void hack::Initialize()
{
    signal(SIGPIPE, SIG_IGN);
    time_injected = time(nullptr);
/*passwd *pwd   = getpwuid(getuid());
char *logname = strfmt("/tmp/cathook-game-stdout-%s-%u.log", pwd->pw_name,
time_injected);
freopen(logname, "w", stdout);
free(logname);
logname = strfmt("/tmp/cathook-game-stderr-%s-%u.log", pwd->pw_name,
time_injected);
freopen(logname, "w", stderr);
free(logname);*/
// Essential files must always exist, except when the game is running in text
// mode.
#if ENABLE_VISUALS == 1

    {
        std::vector<std::string> essential = { "shaders/v2f-c4f.frag",
                                               "shaders/v2f-c4f.vert",
                                               "shaders/v2f-t2f-c4f.frag",
                                               "shaders/v2f-t2f-c4f.vert",
                                               "shaders/v3f-t2f-c4f.frag",
                                               "shaders/v3f-t2f-c4f.vert",
                                               "menu.json",
                                               "fonts/tf2build.ttf" };
        for (const auto &s : essential)
        {
            std::ifstream exists(DATA_PATH "/" + s, std::ios::in);
            if (not exists)
            {
                Error("Missing essential file: " DATA_PATH
                      "/%s\nYou MUST run check-data script to finish "
                      "installation",
                      s.c_str());
            }
        }
    }

#endif /* TEXTMODE */

    logging::Info("Initializing...");
    srand(time(0));
    sharedobj::LoadAllSharedObjects();
    CreateInterfaces();
    CDumper dumper;
    dumper.SaveDump();
    logging::Info("Is TF2? %d", IsTF2());
    logging::Info("Is TF2C? %d", IsTF2C());
    logging::Info("Is HL2DM? %d", IsHL2DM());
    logging::Info("Is CSS? %d", IsCSS());
    logging::Info("Is TF? %d", IsTF());
    InitClassTable();

    BeginConVars();
    hack::c_Cat = CreateConCommand(CON_NAME, &hack::CC_Cat, "Info");
    g_Settings.Init();
    EndConVars();

#if ENABLE_VISUALS == 1
    draw::Initialize();
#if ENABLE_GUI

    g_pGUI = new CatGUI();
    g_pGUI->Setup();

#endif

#endif /* TEXTMODE */

    gNetvars.init();
    InitNetVars();
    g_pLocalPlayer    = new LocalPlayer();
    g_pPlayerResource = new TFPlayerResource();
#if ENABLE_VISUALS == 1
    hooks::panel.Set(g_IPanel);
    hooks::panel.HookMethod((void *) PaintTraverse_hook,
                            offsets::PaintTraverse());
    hooks::panel.Apply();
#endif
    uintptr_t *clientMode = 0;
    // Bad way to get clientmode.
    // FIXME [MP]?
    while (!(
        clientMode = **(
            uintptr_t ***) ((uintptr_t)((*(void ***) g_IBaseClient)[10]) + 1)))
    {
        sleep(1);
    }
    hooks::clientmode.Set((void *) clientMode);
    hooks::clientmode.HookMethod((void *) CreateMove_hook,
                                 offsets::CreateMove());
#if ENABLE_VISUALS == 1
    hooks::clientmode.HookMethod((void *) OverrideView_hook,
                                 offsets::OverrideView());
#endif
    hooks::clientmode.HookMethod((void *) LevelInit_hook, offsets::LevelInit());
    hooks::clientmode.HookMethod((void *) LevelShutdown_hook,
                                 offsets::LevelShutdown());
    hooks::clientmode.Apply();
    hooks::clientmode4.Set((void *) (clientMode), 4);
    hooks::clientmode4.HookMethod((void *) FireGameEvent_hook,
                                  offsets::FireGameEvent());
    hooks::clientmode4.Apply();
    hooks::client.Set(g_IBaseClient);

#if ENABLE_VISUALS == 1
    hooks::client.HookMethod((void *) FrameStageNotify_hook,
                             offsets::FrameStageNotify());
#endif
    hooks::client.HookMethod((void *) DispatchUserMessage_hook,
                             offsets::DispatchUserMessage());

#if ENABLE_VISUALS == 1
    hooks::vstd.Set((void *) g_pUniformStream);
    hooks::vstd.HookMethod((void *) RandomInt_hook, offsets::RandomInt());
    hooks::vstd.Apply();
#endif

#if ENABLE_NULL_GRAPHICS == 1
    g_IMaterialSystem->SetInStubMode(true);
    IF_GAME(IsTF2())
    {
        logging::Info("Graphics Nullified");
        logging::Info("The game will crash");
        // TODO offsets::()?
        hooks::materialsystem.Set((void *) g_IMaterialSystem);
        uintptr_t base = *(uintptr_t *) (g_IMaterialSystem);
        hooks::materialsystem.HookMethod((void *) ReloadTextures_null_hook, 70);
        hooks::materialsystem.HookMethod((void *) ReloadMaterials_null_hook,
                                         71);
        hooks::materialsystem.HookMethod((void *) FindMaterial_null_hook, 73);
        hooks::materialsystem.HookMethod((void *) FindTexture_null_hook, 81);
        hooks::materialsystem.HookMethod((void *) ReloadFilesInList_null_hook,
                                         121);
        hooks::materialsystem.HookMethod((void *) FindMaterialEx_null_hook,
                                         123);
        hooks::materialsystem.Apply();
        // hooks::materialsystem.HookMethod();
    }
#endif
#if ENABLE_VISUALS == 1
    hooks::client.HookMethod((void *) IN_KeyEvent_hook, offsets::IN_KeyEvent());
#endif
    hooks::client.Apply();
    hooks::input.Set(g_IInput);
    hooks::input.HookMethod((void *) GetUserCmd_hook, offsets::GetUserCmd());
    hooks::input.Apply();
#ifndef HOOK_DME_DISABLED
#if ENABLE_VISUALS == 1
    hooks::modelrender.Set(g_IVModelRender);
    hooks::modelrender.HookMethod((void *) DrawModelExecute_hook,
                                  offsets::DrawModelExecute());
    hooks::modelrender.Apply();
#endif
#endif
    hooks::enginevgui.Set(g_IEngineVGui);
    hooks::enginevgui.HookMethod(
        (void *) Paint_hook,
        offsets::PlatformOffset(14, offsets::undefined, offsets::undefined));
    hooks::enginevgui.Apply();
    hooks::steamfriends.Set(g_ISteamFriends);
    hooks::steamfriends.HookMethod((void *) GetFriendPersonaName_hook,
                                   offsets::GetFriendPersonaName());
    hooks::steamfriends.Apply();
    // logging::Info("After hacking: %s", g_ISteamFriends->GetPersonaName());
    // Sadly, it doesn't work as expected :(
    /*hooks::hkBaseClientState = new hooks::VMTHook();
    hooks::hkBaseClientState->Init((void*)g_IBaseClientState, 0);
    hooks::hkBaseClientState->HookMethod((void*)GetClientName_hook,
    hooks::offGetClientName); hooks::hkBaseClientState->Apply();*/
    // hooks::hkBaseClientState8 = new hooks::VMTHook();
    // hooks::hkBaseClientState8->Init((void*)g_IBaseClientState, 8);
    // hooks::hkBaseClientState8->HookMethod((void*)ProcessSetConVar_hook,
    // hooks::offProcessSetConVar);
    // hooks::hkBaseClientState8->HookMethod((void*)ProcessGetCvarValue_hook,
    // hooks::offProcessGetCvarValue);  hooks::hkBaseClientState8->Apply();

    // FIXME [MP]
    hacks::shared::killsay::Init();
    hacks::shared::announcer::init();
    hacks::tf2::killstreak::init();
    hacks::shared::catbot::init();
    logging::Info("Hooked!");
    velocity::Init();
    playerlist::Load();

#if ENABLE_VISUALS == 1

    InitStrings();
#if ENABLE_GUI
    // cat_reloadscheme to load imgui
    hack::command_stack().push("cat_reloadscheme");
#endif
#ifndef FEATURE_EFFECTS_DISABLED
    if (g_ppScreenSpaceRegistrationHead && g_pScreenSpaceEffects)
    {
        effect_chams::g_pEffectChams = new CScreenSpaceEffectRegistration(
            "_cathook_chams", &effect_chams::g_EffectChams);
        g_pScreenSpaceEffects->EnableScreenSpaceEffect("_cathook_chams");
        effect_chams::g_EffectChams.Init();
        effect_glow::g_pEffectGlow = new CScreenSpaceEffectRegistration(
            "_cathook_glow", &effect_glow::g_EffectGlow);
        g_pScreenSpaceEffects->EnableScreenSpaceEffect("_cathook_glow");
    }
    logging::Info("SSE enabled..");
#endif
    DoSDLHooking();
    logging::Info("SDL hooking done");
    g_IGameEventManager->AddListener(&adv_event_listener, false);

#endif /* TEXTMODE */

    hacks::shared::anticheat::Init();
    hacks::tf2::healarrow::Init();

#if ENABLE_VISUALS == 1
#ifndef FEATURE_FIDGET_SPINNER_ENABLED
    InitSpinner();
    logging::Info("Initialized Fidget Spinner");
#endif
    hacks::shared::spam::Init();
    backpacktf::init();
    logging::Info("Initialized Backpack.TF integration");
#endif

    hacks::shared::walkbot::Initialize();

    logging::Info("Clearing initializer stack");
    while (!init_stack().empty())
    {
        init_stack().top()();
        init_stack().pop();
    }
    logging::Info("Initializer stack done");

#if not ENABLE_VISUALS
    hack::command_stack().push("exec cat_autoexec_textmode");
#endif
    hack::command_stack().push("exec cat_autoexec");
    hack::command_stack().push("cat_killsay_reload");
    hack::command_stack().push("cat_spam_reload");
    hack::initialized = true;
}

void hack::Think()
{
    usleep(250000);
}

void hack::Shutdown()
{
    if (hack::shutdown)
        return;
    hack::shutdown = true;
    playerlist::Save();
    DoSDLUnhooking();
    logging::Info("Unregistering convars..");
    ConVar_Unregister();
    logging::Info("Shutting down killsay...");
    hacks::shared::killsay::Shutdown();
    hacks::shared::announcer::shutdown();
    logging::Info("Success..");
}
