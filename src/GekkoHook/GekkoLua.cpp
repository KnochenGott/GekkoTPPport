// GekkoLua.cpp 30/09/2026. Lua library "Gekko", loaded from IH via package.loadlib(dll, "luaopen_Gekko").
// TPP links Lua 5.1 into the exe, so the DLL calls the game's own Lua C API. Addresses and how they were found: comment above the typedefs.
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GekkoAi.h"
#include "GekkoWorld.h"
#include "GekkoBrain.h"
#include "GekkoGround.h"
#include "GekkoDriver.h"
#include "GekkoDebug.h"
#include "GekkoBlocks.h"
#include "GekkoFiles.h"
#include "GekkoLegIk.h"
#include "GekkoLog.h"
#include "GekkoNavStep.h"
#include "GekkoSigCheck.h"
#include "GekkoTargetProbe.h"
#include "GekkoPushProbe.h"
#include "GekkoType.h"

struct lua_State;

namespace
{
    using lua_CFunction = int (*)(lua_State*);
    struct luaL_Reg { const char* name; lua_CFunction func; };

    // Retail 1.0.15.4 addresses. Taken from the working material_probe.dll (OctoCamo), which calls these at
    // exe base + RVA; names by matching against the steamless dev decomp (lapi.obj, retail = dev + 0x11911F0, minus
    // 0x10 from lua_toboolean on). Prologues are checked at load against GekkoSigs.h (tools/make_gekko_sigs.py).
    // Numbers are read with lua_tolstring + strtod because lua_tonumber is not among the functions probe_dll proved.
    using Register_t    = void (*)(lua_State*, const char*, const luaL_Reg*);
    using PushBool_t    = void (*)(lua_State*, int);
    using PushInteger_t = void (*)(lua_State*, std::ptrdiff_t);
    using PushString_t  = void (*)(lua_State*, const char*);
    using ToBoolean_t   = int (*)(lua_State*, int);
    using ToLString_t   = const char* (*)(lua_State*, int, std::size_t*);

    Register_t    luaL_register_;
    PushBool_t    lua_pushboolean_;
    PushInteger_t lua_pushinteger_;
    PushString_t  lua_pushstring_;
    ToBoolean_t   lua_toboolean_;
    ToLString_t   lua_tolstring_;

    bool CheckAndResolve(const char* name, void** out)
    {
        *out = GekkoSig::CheckByName(name, "lua");
        return *out != nullptr;
    }

    bool ResolveLuaApi()
    {
        static int state = 0;  // 0 untried, 1 ok, -1 failed
        if (state)
            return state > 0;
        bool ok = true;
        ok &= CheckAndResolve("Lua_Register", reinterpret_cast<void**>(&luaL_register_));
        ok &= CheckAndResolve("lua_pushboolean", reinterpret_cast<void**>(&lua_pushboolean_));
        ok &= CheckAndResolve("lua_pushinteger", reinterpret_cast<void**>(&lua_pushinteger_));
        ok &= CheckAndResolve("lua_pushstring", reinterpret_cast<void**>(&lua_pushstring_));
        ok &= CheckAndResolve("lua_toboolean", reinterpret_cast<void**>(&lua_toboolean_));
        ok &= CheckAndResolve("lua_tolstring", reinterpret_cast<void**>(&lua_tolstring_));
        state = ok ? 1 : -1;
        return ok;
    }

    float Num(lua_State* L, int i, float def)
    {
        const char* s = lua_tolstring_(L, i, nullptr);   // nil -> null; numbers convert in place (Lua 5.1)
        if (!s)
            return def;
        char* end = nullptr;
        const double v = std::strtod(s, &end);
        return end != s ? static_cast<float>(v) : def;
    }

    // A 32-bit id (route ids, StrCode32): Lua 5.1 prints numbers with %.14g, so the text holds every bit; Num's float would not.
    std::uint32_t Id(lua_State* L, int i, std::uint32_t def)
    {
        const char* s = lua_tolstring_(L, i, nullptr);
        return s ? static_cast<std::uint32_t>(std::strtod(s, nullptr)) : def;
    }

    int Ret(lua_State* L, bool ok)
    {
        lua_pushboolean_(L, ok ? 1 : 0);
        lua_pushstring_(L, Gekko::GetLastError());
        return 2;
    }

    int l_Verify(lua_State* L) { const bool a = Gekko::Verify(); const bool c = GekkoGround::Verify(); return Ret(L, a && c); }
    int l_SetDebug(lua_State* L) { Gekko::SetDebugLevel(static_cast<int>(Num(L, 1, 1))); return 0; }
    int l_FindFiles(lua_State* L) { return Ret(L, Gekko::FindFiles()); }
    int l_Play(lua_State* L) { return Ret(L, Gekko::Play(static_cast<int>(Num(L, 1, 0)), Num(L, 2, 0.2f))); }
    int l_Status(lua_State* L)
    {
        Gekko::LogStatus();
        lua_pushinteger_(L, static_cast<int>(Gekko::GetStep()));
        lua_pushboolean_(L, Gekko::IsVerified() ? 1 : 0);
        lua_pushstring_(L, Gekko::GetLastError());
        return 3;
    }
    int l_AiStart(lua_State*) { GekkoAi::Start(); return 0; }
    int l_AiStop(lua_State*) { GekkoAi::Stop(); return 0; }
    int l_BrainStart(lua_State*) { GekkoAi::Start(); return 0; }
    int l_BrainStop(lua_State*) { GekkoAi::Stop(); return 0; }
    int l_BrainStatus(lua_State* L) { lua_pushstring_(L, GekkoBrain::Status()); return 1; }
    // Gekko.BrainRequest(action, flag, x, y, z): test hook, an action request as if the AI had sent it.
    int l_BrainRequest(lua_State* L)
    {
        const float t[3] = { Num(L, 3, 0), Num(L, 4, 0), Num(L, 5, 0) };
        Log("[lua] BrainRequest (menu / script, not the brain) action %u flag %u\n", static_cast<unsigned>(Num(L, 1, 0)), static_cast<unsigned>(Num(L, 2, 0)));   // told apart from the brain's own requests in the log
        GekkoBrain::Request(static_cast<unsigned>(Num(L, 1, 0)), static_cast<unsigned>(Num(L, 2, 0)), t);
        return 0;
    }
    int l_AiState(lua_State* L) { lua_pushinteger_(L, GekkoAi::GetState()); return 1; }
    // Returns "x y z yaw" as one string (lua_pushnumber is not among the verified Lua API functions).
    int l_GetPose(lua_State* L)
    {
        float p[4];
        Gekko::GetPose(p);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.4f %.4f %.4f %.5f", p[0], p[1], p[2], p[3]);
        lua_pushstring_(L, buf);
        return 1;
    }
    int l_AiTarget(lua_State* L) { GekkoAi::SetTarget(Num(L, 1, 0), Num(L, 2, 0), Num(L, 3, 0)); return 0; }
    // Gekko.AiRoute(routeId, vertex): walk a loaded TPP route (routeId = GsRoute.GetRouteId(name), or the id tools/frt_read.py prints); GsRoute.ROUTE_ID_EMPTY ends it.
    int l_AiRoute(lua_State* L) { GekkoAi::Route(Id(L, 1, 0xbf169f98u), static_cast<unsigned>(Num(L, 2, 0))); return 0; }
    int l_DumpFiles(lua_State* L)
    {
        lua_pushinteger_(L, GekkoFiles::Dump(static_cast<int>(Num(L, 1, 0))));
        return 1;
    }
    int l_DumpBlocks(lua_State* L)
    {
        lua_pushinteger_(L, GekkoBlocks::Dump());
        return 1;
    }
    int l_Log(lua_State* L)
    {
        const char* s = lua_tolstring_(L, 1, nullptr);
        Log("[lua] %s\n", s ? s : "");
        return 0;
    }

    int l_TypeRegister(lua_State* L) { lua_pushboolean_(L, GekkoType::Register() ? 1 : 0); return 1; }
    int l_TypeCreate(lua_State* L) { return Ret(L, GekkoType::Create(1, Num(L, 1, 0), Num(L, 2, 0), Num(L, 3, 0), Num(L, 4, 0))); }
    int l_TypePlace(lua_State* L) { return Ret(L, GekkoType::Place(Num(L, 1, 0), Num(L, 2, 0), Num(L, 3, 0), Num(L, 4, 0))); }
    int l_TypeDestroy(lua_State*) { GekkoType::Destroy(); return 0; }
    int l_DumpTargets(lua_State*) { GekkoTargetProbe::Run(); return 0; }
    int l_PushProbe(lua_State* L) { GekkoPushProbe::Run(static_cast<float>(Num(L, 1, 1.5)), static_cast<int>(Num(L, 2, 1))); return 0; }
    int l_TypeStatus(lua_State* L) { lua_pushstring_(L, GekkoType::Status()); return 1; }

    // Debug API: every call logs what it did to Gekko.log.
    int l_DebugNavController(lua_State* L) { GekkoDebug::SetNavController(Num(L, 1, 0) != 0); return 0; }
    int l_DebugAction(lua_State* L)
    {
        const float t[3] = { Num(L, 3, 0), Num(L, 4, 0), Num(L, 5, 0) };
        lua_pushboolean_(L, GekkoDebug::Action(static_cast<unsigned>(Num(L, 1, 0)), static_cast<unsigned>(Num(L, 2, 0)), t) ? 1 : 0);
        return 1;
    }
    int l_DebugState(lua_State* L) { lua_pushboolean_(L, GekkoDebug::State(static_cast<unsigned>(Num(L, 1, 0))) ? 1 : 0); return 1; }
    int l_DebugChoose(lua_State* L) { lua_pushinteger_(L, GekkoDebug::Choose()); return 1; }
    int l_DebugEvent(lua_State* L) { lua_pushboolean_(L, GekkoDebug::Event(Id(L, 1, 0), static_cast<unsigned>(Num(L, 2, 0))) ? 1 : 0); return 1; }
    int l_DebugDispatch(lua_State* L) { lua_pushboolean_(L, GekkoDebug::Dispatch() ? 1 : 0); return 1; }
    int l_DebugMelee(lua_State* L)
    {
        const float t[3] = { Num(L, 1, 0), Num(L, 2, 0), Num(L, 3, 0) };
        lua_pushboolean_(L, GekkoDebug::Melee(t) ? 1 : 0);
        return 1;
    }
    int l_DebugDamage(lua_State* L) { lua_pushboolean_(L, GekkoDebug::Damage(static_cast<int>(Num(L, 1, 0)), static_cast<int>(Num(L, 2, 0))) ? 1 : 0); return 1; }   // DebugDamage(amount, part 0..20)
    int l_DebugImpact(lua_State* L)   // DebugImpact(part 0..20, playerX, playerY, playerZ)
    {
        const float pl[3] = { Num(L, 2, 0), Num(L, 3, 0), Num(L, 4, 0) };
        lua_pushboolean_(L, GekkoDebug::Impact(static_cast<int>(Num(L, 1, 0)), pl) ? 1 : 0);
        return 1;
    }
    // Gekko.LegIk(on): the leg fit switch (default off). Installs the plugin once the gekko's controller is ready; LegIkStatus returns one status line.
    int l_LegIk(lua_State* L) { GekkoLegIk::SetEnabled(Num(L, 1, 0) != 0); return 0; }
    int l_LegIkStatus(lua_State* L) { lua_pushstring_(L, GekkoLegIk::Status()); return 1; }
    // Gekko.NavStep(on): the walk state's path correction moves the body (default off, GekkoNavStep.cpp).
    int l_NavStep(lua_State* L) { GekkoNavStep::SetEnabled(Num(L, 1, 0) != 0); return 0; }
    // Gekko.SteerGate(on): the walk state's heading request is applied only while the walk state is current (default on).
    // Gekko.RealNpc(on): the next Begin builds Survive's real SsdNpc service instead of the host buffer (default off, docs 5.16a).
    int l_RealNpc(lua_State* L) { GekkoWorld::SetRealNpc(Num(L, 1, 0) != 0); return 0; }
    int l_SteerGate(lua_State* L) { GekkoAi::SetSteerGate(Num(L, 1, 1) != 0); return 0; }
    int l_DebugSnapshot(lua_State* L) { GekkoDebug::Snapshot(lua_tolstring_(L, 1, nullptr)); return 0; }

    const luaL_Reg kFuncs[] =
    {
        { "Verify", l_Verify }, { "SetDebug", l_SetDebug }, { "FindFiles", l_FindFiles }, { "Play", l_Play },
        { "Status", l_Status }, { "Log", l_Log }, { "DumpFiles", l_DumpFiles }, { "DumpBlocks", l_DumpBlocks },
        { "AiStart", l_AiStart }, { "AiStop", l_AiStop }, { "BrainStart", l_BrainStart }, { "BrainStop", l_BrainStop }, { "BrainStatus", l_BrainStatus }, { "BrainRequest", l_BrainRequest }, { "AiState", l_AiState }, { "GetPose", l_GetPose },
        { "AiTarget", l_AiTarget }, { "AiRoute", l_AiRoute }, { "TypeRegister", l_TypeRegister }, { "TypeCreate", l_TypeCreate }, { "TypePlace", l_TypePlace },
        { "TypeDestroy", l_TypeDestroy }, { "DumpTargets", l_DumpTargets }, { "TypeStatus", l_TypeStatus }, { "PushProbe", l_PushProbe },
        { "DebugNavController", l_DebugNavController }, { "DebugAction", l_DebugAction }, { "DebugState", l_DebugState }, { "DebugChoose", l_DebugChoose },
        { "RealNpc", l_RealNpc }, { "DebugEvent", l_DebugEvent }, { "DebugDispatch", l_DebugDispatch }, { "DebugMelee", l_DebugMelee }, { "DebugSnapshot", l_DebugSnapshot }, { "DebugDamage", l_DebugDamage }, { "DebugImpact", l_DebugImpact }, { "LegIk", l_LegIk }, { "LegIkStatus", l_LegIkStatus }, { "NavStep", l_NavStep }, { "SteerGate", l_SteerGate },
        { nullptr, nullptr },
    };
}

// Returns 1 with the Gekko table on the stack, or 0 (nothing registered) if the Lua API could not be verified.
extern "C" __declspec(dllexport) int luaopen_Gekko(lua_State* L)
{
    Log("[lua] luaopen_Gekko lua_State=%p\n", L);
    if (!L || !ResolveLuaApi())
    {
        Log("[lua] Lua C API not verified; Gekko table NOT registered\n");
        return 0;
    }
    luaL_register_(L, "Gekko", kFuncs);
    Log("[lua] Gekko registered\n");
    return 1;
}
