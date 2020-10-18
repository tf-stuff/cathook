#include "MiscTemporary.hpp"
#include "Warp.hpp"

// Imported for various processes
void PrecalculateCanShoot();
extern settings::Int fullauto;
extern settings::Boolean roll_speedhack;
extern settings::Boolean forward_speedhack;

namespace hacks::tf2::warp
{
uintptr_t original_cm = 0;
typedef bool (*OriginalCM_t)(void *this_, float input_sample_time, CUserCmd *cmd);
#define CALL_CREATEMOVE(a, b, c) (((OriginalCM_t)(original_cm))(a, b, c))

// Remade createmove to reduce fps usage
bool WarpCreateMove(void *this_, float input_sample_time, CUserCmd *cmd)
{
    g_Settings.is_create_move = true;
    bool time_replaced, ret, speedapplied;
    float curtime_old, servertime, speed, yaw;
    Vector vsilent, ang;

    current_user_cmd = cmd;
    EC::run(EC::CreateMoveEarly);
    ret = CALL_CREATEMOVE(this_, input_sample_time, cmd);

    if (!cmd)
    {
        g_Settings.is_create_move = false;
        return ret;
    }

    // Disabled because this causes EXTREME aimbot inaccuracy
    // Actually dont disable it. It causes even more inaccuracy
    if (!cmd->command_number)
    {
        g_Settings.is_create_move = false;
        return ret;
    }

    tickcount++;

    if (!isHackActive())
    {
        g_Settings.is_create_move = false;
        return ret;
    }

    if (!g_IEngine->IsInGame())
    {
        g_Settings.bInvalid       = true;
        g_Settings.is_create_move = false;
        return true;
    }

    PROF_SECTION(WP_CreateMove);
    if (current_user_cmd && current_user_cmd->command_number)
        last_cmd_number = current_user_cmd->command_number;

    time_replaced = false;
    curtime_old   = g_GlobalVars->curtime;

    if (!g_Settings.bInvalid && CE_GOOD(g_pLocalPlayer->entity))
    {
        servertime            = (float) CE_INT(g_pLocalPlayer->entity, netvar.nTickBase) * g_GlobalVars->interval_per_tick;
        g_GlobalVars->curtime = servertime;
        time_replaced         = true;
    }
    {
        PROF_SECTION(WP_CM_LocalPlayer);
        g_pLocalPlayer->Update();
    }
    PrecalculateCanShoot();
    g_Settings.bInvalid = false;

    if (CE_GOOD(g_pLocalPlayer->entity))
    {
        if (!g_pLocalPlayer->life_state && CE_GOOD(g_pLocalPlayer->weapon()))
        {
            // Walkbot can leave game.
            if (!g_IEngine->IsInGame())
            {
                g_Settings.is_create_move = false;
                return ret;
            }
            static int attackticks = 0;
            if (current_user_cmd->buttons & IN_ATTACK)
                ++attackticks;
            else
                attackticks = 0;
            if (fullauto)
                if (current_user_cmd->buttons & IN_ATTACK)
                    if (attackticks % *fullauto + 1 < *fullauto)
                        current_user_cmd->buttons &= ~IN_ATTACK;
        }
    }
    {
        PROF_SECTION(CM_WRAPPER);
        EC::run(EC::CreateMove_NoEnginePred);
        EC::run(EC::CreateMove);
    }
    if (time_replaced)
        g_GlobalVars->curtime = curtime_old;
    g_Settings.bInvalid = false;
    if (CE_GOOD(g_pLocalPlayer->entity))
    {
        speedapplied = false;
        if (roll_speedhack && cmd->buttons & IN_DUCK && (CE_INT(g_pLocalPlayer->entity, netvar.iFlags) & FL_ONGROUND) && !(cmd->buttons & IN_ATTACK) && !HasCondition<TFCond_Charging>(LOCAL_E))
        {
            speed                     = Vector{ cmd->forwardmove, cmd->sidemove, 0.0f }.Length();
            static float prevspeedang = 0.0f;
            if (fabs(speed) > 0.0f)
            {

                if (forward_speedhack)
                {
                    cmd->forwardmove *= -1.0f;
                    cmd->sidemove *= -1.0f;
                    cmd->viewangles.x = 91;
                }
                Vector vecMove(cmd->forwardmove, cmd->sidemove, 0.0f);

                vecMove *= -1;
                float flLength = vecMove.Length();
                Vector angMoveReverse{};
                VectorAngles(vecMove, angMoveReverse);
                cmd->forwardmove = -flLength;
                cmd->sidemove    = 0.0f; // Move only backwards, no sidemove
                float res        = g_pLocalPlayer->v_OrigViewangles.y - angMoveReverse.y;
                while (res > 180)
                    res -= 360;
                while (res < -180)
                    res += 360;
                if (res - prevspeedang > 90.0f)
                    res = (res + prevspeedang) / 2;
                prevspeedang                     = res;
                cmd->viewangles.y                = res;
                cmd->viewangles.z                = 90.0f;
                g_pLocalPlayer->bUseSilentAngles = true;
                speedapplied                     = true;
            }
        }
        if (g_pLocalPlayer->bUseSilentAngles)
        {
            if (!speedapplied)
            {
                vsilent.x = cmd->forwardmove;
                vsilent.y = cmd->sidemove;
                vsilent.z = cmd->upmove;
                speed     = sqrt(vsilent.x * vsilent.x + vsilent.y * vsilent.y);
                VectorAngles(vsilent, ang);
                yaw                 = DEG2RAD(ang.y - g_pLocalPlayer->v_OrigViewangles.y + cmd->viewangles.y);
                cmd->forwardmove    = cos(yaw) * speed;
                cmd->sidemove       = sin(yaw) * speed;
                float clamped_pitch = fabsf(fmodf(cmd->viewangles.x, 360.0f));
                if (clamped_pitch >= 90 && clamped_pitch <= 270)
                    cmd->forwardmove = -cmd->forwardmove;
            }

            ret = false;
        }
        g_pLocalPlayer->UpdateEnd();
    }

    //	PROF_END("CreateMove");
    if (!(cmd->buttons & IN_ATTACK))
    {
        // LoadSavedState();
    }
    g_Settings.is_create_move = false;
    if (nolerp)
    {
        static const ConVar *pUpdateRate = g_pCVar->FindVar("cl_updaterate");
        if (!pUpdateRate)
            pUpdateRate = g_pCVar->FindVar("cl_updaterate");
        else
        {
            float interp = MAX(cl_interp->GetFloat(), cl_interp_ratio->GetFloat() / pUpdateRate->GetFloat());
            cmd->tick_count += TIME_TO_TICKS(interp);
        }
    }
    return ret;
}
} // namespace hacks::tf2::warp
