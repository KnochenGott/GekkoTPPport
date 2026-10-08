// GekkoWorld.cpp 30/09/2026. Shim world for Survive's lifted gekko code (whole module 0x140f60000-0x140fb0000 of the installed
// mgv.exe). Layouts from the handlers and confirmed by running all 31 enter events (docs/DOCUMENTATION.md). Pointer fields we have
// not identified point at trap objects: a virtual call on one is logged once per (object, slot) and returns 0.
#include "GekkoWorld.h"
#include "GekkoLife.h"

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "lift_rt.h"
#include "lift_env.h"
#include "GekkoNavProxy.h"
#include "GekkoNodes.h"

void gekko_module_register();

#include "GekkoCtrlSlots.h"
extern "C" void* g_fwdCtrl;

namespace
{
    using GekkoWorld::Hooks;

    // Action handlers, disk addresses (research/mgv/gekko_action_table_disk.txt).
    const std::uint64_t kHandlers[32] = {
        0x1402ba870, 0x140f9b570, 0x140f9ce80, 0x140f9bea0, 0x140f9bc00, 0x140f9ca80, 0x140f9c550, 0x140f96560, 0x140f9a1a0, 0x140f9eca0,
        0x140f99810, 0x140f99100, 0x140f9d540, 0x140f98220, 0x140f98710, 0x140f98540, 0x140f98940, 0x140f9e980, 0x140f9db90, 0x140f9df30,
        0x140f98dd0, 0x140f98940, 0x140fa01b0, 0x140f98b80, 0x140f9d790, 0x140f96980, 0x140f9e170, 0x140f9fe20, 0x140f962c0, 0x140f9e700,
        0x140f96d50, 0x140f97b80 };

    Hooks g_hooks;
    bool g_ready = false;
    std::uint64_t g_finished = 0;
    char g_fault[160] = "";
    std::uint8_t* g_scopeT = nullptr;   // the host stand-in for Survive's scope table T (BuildObserver); the nav proxies fill +0x98/+0xa0 (StartNav)

    void Trace(const char* fmt, ...)
    {
        if (!g_hooks.trace) return;
        char b[256];
        va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
        g_hooks.trace(b);
    }

    std::map<void*, std::string>& names() { static std::map<void*, std::string> m; return m; }
    std::map<std::pair<void*, int>, int>& seen() { static std::map<std::pair<void*, int>, int> m; return m; }

    template <int N> std::uint64_t TrapFn(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c)
    {
        if (seen()[{ self, N }]++ == 0)
            Trace("[trap] %s vslot %d (+0x%x) args %llx %llx %llx", names().count(self) ? names()[self].c_str() : "?", N, N * 8,
                  (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
        return 0;
    }
    void* g_trapVtbl[256];
    void* g_ownerEntry = nullptr;   // svc+0x68 lookup result, set by BuildObserver
    void* g_ctlVtbl[256];       // the animation controller: set motion, finished, motion output
    void* g_entVtbl[256];       // the per-unit animation controller: motion speed
    template <int... I> void fillTraps(std::integer_sequence<int, I...>)
    {
        ((g_trapVtbl[I] = reinterpret_cast<void*>(&TrapFn<I>)), ...);
    }

    // controller vtable +0x1f0 (slot 62): desc {+0 layer id, +8 gani path id, +0x14 blend}
    std::uint64_t CtlSetMotion(void*, std::uint64_t, std::uint64_t desc, std::uint64_t)
    {
        const std::uint64_t gani = *reinterpret_cast<std::uint64_t*>(desc + 8);
        const float blend = *reinterpret_cast<float*>(desc + 0x14);
        Trace("[motion] layer %llx gani %llx blend %.2f", *reinterpret_cast<unsigned long long*>(desc), (unsigned long long)gani, blend);
        if (g_hooks.setMotion) g_hooks.setMotion(gani, blend);
        g_finished = 0;   // headless: the new clip has not ended, as the game's FinishedLogged answers right after a request (the headless controller has no node of its own)
        return 0;
    }
    std::uint64_t CtlTrue(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 1; }
    // vtable +0x2f8 (slot 95): the current motion has finished
    std::uint64_t CtlFinished(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return g_finished; }
    // vtable +0x3d8 (slot 123): motion output struct; +0x2c holds a scale that must not be 0 (root motion is done by TPP's CharaControl)
    alignas(16) std::uint8_t g_motionOut[0x200];
    std::uint64_t CtlMotionOut(void*, std::uint64_t, std::uint64_t, std::uint64_t)
    {
        *reinterpret_cast<float*>(g_motionOut + 0x2c) = 1.0f;
        return reinterpret_cast<std::uint64_t>(g_motionOut);
    }
    // per-unit controller vtable +0x2b8 (slot 87): motion speed scale, float in xmm1
    void SpeedFn(void*, float s) { Trace("[speed] %.3f", s); if (g_hooks.setSpeed) g_hooks.setSpeed(s); }

    struct TrapObj { void** vtbl; std::uint8_t data[0x400]; };   // offsets in this code are object-relative: vtbl at +0, data from +8

    const int kPtrOffsets[] = { 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90,
                                0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xf8 };

    // Trap objects nest so chains like [[x+0x10]+0x40] survive; scalars stay 0.
    TrapObj* trap(const std::string& name, int depth = 2, void** vtbl = g_trapVtbl)
    {
        TrapObj* t = new TrapObj{}; t->vtbl = vtbl; names()[t] = name;
        if (depth > 0)
            for (int off : kPtrOffsets)
                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(t) + off) = trap(name + "->+0x" + std::to_string(off), depth - 1);
        return t;
    }
    // Fills every still-zero common pointer offset of an explicit structure with a nested trap object.
    void nest(std::uint8_t* base, const std::string& name, std::size_t size = 0x100)
    {
        for (int off : kPtrOffsets)
            if (off + 8 <= int(size) && !*reinterpret_cast<void**>(base + off))
                *reinterpret_cast<void**>(base + off) = trap(name + "->+0x" + std::to_string(off), 1);
    }
    template <class T> void put(std::uint8_t* base, int off, T v) { *reinterpret_cast<T*>(base + off) = v; }

    // The lifted ActionController (interface = [svc+0x80], created by the Observer setup through svc slot 126, see AcSlot): +8 the request array (0xc0 per unit), +0x10 the record array (0x1d0 per unit).
    std::uint8_t* g_ac = nullptr;
    std::uint8_t* AcRequest(int id) { return *reinterpret_cast<std::uint8_t**>(g_ac + 8) + static_cast<std::size_t>(id) * 0xc0; }
    std::uint8_t* AcRecord(int id) { return *reinterpret_cast<std::uint8_t**>(g_ac + 0x10) + static_cast<std::size_t>(id) * 0x1d0; }
    float g_dt[8];
    alignas(16) float g_targetObj[32];   // 0x80 bytes: the movement brain reads the row up to +0x6a (0x140fa4791), the old 0x40 byte array was read past its end for +0x68 and +0x6a
    void PosFn(void*, float* out, std::uint32_t) { out[0] = g_targetObj[8]; out[1] = g_targetObj[9]; out[2] = g_targetObj[10]; }
    // Melee chooser context (0x140f911f0, from its disassembly): ctx+0x50 position provider (vtable +0xe8 GetPos(out, id)), ctx+0x58 the NPC
    // service (vtable +0x70 random()). Its request goes to the mailbox: action 2, 3 (flag = variant) or 1 by random & 3.
    struct MeleeCtx { std::uint8_t pad[0x50]; void* pos; void* svc; };
    MeleeCtx g_meleeCtx;
    void* g_posVtbl[64];
    void* g_svcVtbl[256];
    // svc slots 12, 13 and 14 (+0x60, +0x68, +0x70): Survive's own xorshift32 (13, 7, 5) on the host svc, state word svc+0x1c: 0x141076380 float in [-1, 1), 0x141076330 float in [0, 1), 0x1410763e0 the raw word.
    // Defined after Guarded (they run lifted code).
    float RandSigned(void* self, std::uint64_t, std::uint64_t, std::uint64_t);
    float RandUnit(void* self, std::uint64_t, std::uint64_t, std::uint64_t);
    std::uint64_t RandRaw(void* self, std::uint64_t, std::uint64_t, std::uint64_t);
    std::uint32_t RandomSeedNow();
    // Navigation service [svc+0x60] (from the movement brain 0x140fa46d0): vtable +0x00 request path(id, req{+0 start, +0x10 goal}) -> handle,
    // +0x08 release(id, handle), +0x18 path record for id -> {A, .., id}, A vtable +0x98(id) -> path object {+8 vec4 waypoints, +0x28 count},
    // +0x40 snap point(out, in, ...) -> bool. The shim's path is a straight line, start then goal; TPP's own route/nav data would fill it later.
    // path object: +8 waypoints (16 bytes each), +0x10 segments (0x30 each, dword +0x28 = link type: 0 plain, others jump/climb, meaning not decoded), +0x28 count
    struct PathObj { std::uint8_t pad[8]; float (*pts)[4]; std::uint8_t (*seg)[0x30]; std::uint8_t pad2[0x10]; std::uint32_t count; };
    std::uint8_t g_seg[8][0x30];
    float g_wp[2][4];
    float (*g_pose)[4];                         // NPC service entries: x y z yaw
    PathObj g_path;
    struct PathRec { void* a; std::uint32_t pad, id; };
    PathRec g_pathRec;
    void* g_navVtbl[256];
    void* g_pathAVtbl[256];
    // The stand-in's path as a Survive Path2Impl (its own constructor 0x141bf1e80), because the path order branch of the locomotion measures the remaining length with the object's own methods
    // (0x140f888df, slot +0x30): points at +8 with the length to the next point in the w lane, segments at +0x10 (stride 0x30), flag bytes at +0x18, point count at +0x28.
    std::uint8_t* g_sPath = nullptr;
    bool CallLifted(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, const char*);
    void BuildStandInPath()
    {
        alignas(16) static float pts[2][4]; alignas(16) static std::uint8_t seg[2][0x30], flg[4]; static std::uint16_t u16[2];
        if (!g_sPath)
        {
            g_sPath = static_cast<std::uint8_t*>(_aligned_malloc(0x80, 16));
            std::memset(g_sPath, 0, 0x80);
            CallLifted(0x141bf1e80ull, reinterpret_cast<std::uint64_t>(g_sPath), 0, 0, 0, "stand-in path constructor");
        }
        float d = 0.0f;
        for (int i = 0; i < 3; ++i) d += (g_wp[1][i] - g_wp[0][i]) * (g_wp[1][i] - g_wp[0][i]);
        std::memcpy(pts[0], g_wp[0], 12); pts[0][3] = std::sqrt(d);
        std::memcpy(pts[1], g_wp[1], 12); pts[1][3] = 0.0f;
        std::memset(seg, 0, sizeof(seg)); flg[0] = flg[1] = 1; flg[2] = 0; u16[0] = u16[1] = 0;
        put<void*>(g_sPath, 8, pts); put<void*>(g_sPath, 0x10, seg); put<void*>(g_sPath, 0x18, flg); put<void*>(g_sPath, 0x20, u16); put<std::uint32_t>(g_sPath, 0x28, 2); put<void*>(g_sPath, 0x38, pts);
    }
    std::uint64_t NavRequest(void*, std::uint64_t, std::uint64_t req, std::uint64_t)
    {
        std::memcpy(g_wp[0], reinterpret_cast<void*>(req), 16);
        std::memcpy(g_wp[1], reinterpret_cast<void*>(req + 0x10), 16);
        g_path.pts = g_wp; g_path.seg = g_seg; g_path.count = 2;
        BuildStandInPath();
        return 1;
    }
    std::uint64_t NavRecord(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(&g_pathRec); }
    std::uint64_t NavSnap(void*, std::uint64_t out, std::uint64_t in, std::uint64_t) { std::memcpy(reinterpret_cast<void*>(out), reinterpret_cast<void*>(in), 16); return 1; }
    std::uint64_t PathA(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(g_sPath); }
    // Path record vtable +0x88 (handle): the path of the latest request has arrived (the lifted controller's 0x141409550: result counter equals request counter). The shim path is built inside the request.
    std::uint64_t PathCurrent(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return g_sPath ? 1 : 0; }
    // Path record vtable +0xa0 (id, out): where the NPC is along its path, {u32 segment index, float fraction into that segment} (0x140f88900 reads [out] and [out+4] as the lerp
    // factor; 0x140f8a6bd compares the index with the path count). Unwritten it left stack garbage in the locomotion's heading (the gekko walked away from its target in game).
    // The shim path is one segment, so: index 0, fraction = projection of the NPC's position on start -> goal, clamped.
    std::uint64_t PathProgress(void*, std::uint64_t out, std::uint64_t id, std::uint64_t)
    {
        float t = 0.0f;
        if (g_pose)
        {
            const float* p = g_pose[id & 7];
            float ab[3], ap[3], dd = 0.0f, dp = 0.0f;
            for (int i = 0; i < 3; ++i) { ab[i] = g_wp[1][i] - g_wp[0][i]; ap[i] = p[i] - g_wp[0][i]; dd += ab[i] * ab[i]; dp += ab[i] * ap[i]; }
            if (dd > 1e-6f) t = dp / dd < 0.0f ? 0.0f : dp / dd > 1.0f ? 1.0f : dp / dd;
        }
        *reinterpret_cast<std::uint32_t*>(out) = 0;
        *reinterpret_cast<float*>(out + 4) = t;
        return 0;
    }
    // Route system. The movement brain in mode 0 with a route id at order +0xb0 (StrCode32, 0xbf169f98 = "" = none) asks [[ctx+8]+0xa8] for the route and walks its vertices one by one
    // (0x140fa4c94..0x140fa4d71): slot 1 = generation (compared with order +0xac; a change re-fetches the handle into the per-unit cache [M+0x70], 0x18 bytes per unit), slot 0 =
    // GetRoute(out, routeId) -> out {+8 group, +0x10 route index word, 0xffff none}, then group slot 1 = vertex count(index) and slot 2 = vertex position(out, index, vertex); order +0xb4 = next vertex.
    // That object is Survive's fox::gs::impl::RouteSystemImpl (vtable 0x1420fe100): slot 0 GetRoute 0x140580430 has the same body as TPP's 0x140513860 (tools/ctrl_slot_map.py
    // norm, 49 instructions); slot 1 0x140580500 returns +0x68, a counter Survive's Add/RemoveRouteGroup increment (0x140580292, 0x1405806d3, 0x1405807b6, 0x140580906). TPP's class
    // (vtable 0x1421872a0: GetRoute, destructor) has no counter, so the generation is answered natively from TPP's own route group list (+0x40 sentinel, +0x48 first node, node +8 next,
    // +0x10 group; TPP GetRoute 0x140513894..0x1405138cd): it changes exactly when a route group is added or removed. The groups are TPP's RouteGroupImpl (vtable 0x142187500), all 14 slots
    // the same bodies as Survive's (vtable 0x1420fe360,), so the lifted code calls them directly.
    // Route end report: the movement brain calls [svc+0x78] vtable +0x60 (id, 0xbf169f98, 0xffff) when the route has no further vertex or no route is found (0x140fa4d73..0x140fa4d92) and then clears
    // order +0xb0 itself. In Survive the host picks the next route; here GekkoAi decides (it loops the route, docs/DOCUMENTATION.md), so the report is only recorded.
    bool g_routeEnded = false;
    std::uint32_t g_loopRoute = 0xbf169f98u;   // route being looped (0xbf169f98 = none)
    int g_sinceRestart = 99;
    std::uint64_t RouteEnd(void*, std::uint64_t id, std::uint64_t, std::uint64_t) { if (id == 0) g_routeEnded = true; return 0; }
    void* g_routeSys = nullptr;          // TPP's RouteSystemImpl instance (game), a mock (headless), or null: then no route is found
    const void* g_routeGroupVtbl = nullptr;   // the group vtable every fetched group must carry (null headless: not checked)
    struct RouteHandle { void* vtbl; void* group; std::uint16_t index; std::uint8_t pad[6]; };
    RouteHandle g_routeCache[8];
    void* g_routeVtbl[256];
    void* g_routeObj = g_routeVtbl;
    void* g_routePtr = &g_routeObj;
    std::uint64_t RouteGet(void*, std::uint64_t out, std::uint64_t routeId, std::uint64_t)
    {
        RouteHandle* h = reinterpret_cast<RouteHandle*>(out);
        h->group = nullptr; h->index = 0xffff;
        if (!g_routeSys) return out;
        RouteHandle r = {};
        reinterpret_cast<void* (__fastcall*)(void*, RouteHandle*, std::uint32_t)>((*reinterpret_cast<void***>(g_routeSys))[0])(g_routeSys, &r, static_cast<std::uint32_t>(routeId));
        if (r.group && g_routeGroupVtbl && *reinterpret_cast<void**>(r.group) != g_routeGroupVtbl)
        {
            Trace("[route] route %08x: group %p vtable %p is not TPP's RouteGroupImpl, refused", static_cast<unsigned>(routeId), r.group, *reinterpret_cast<void**>(r.group));
            return out;
        }
        h->group = r.group; h->index = r.group ? r.index : 0xffff;
        Trace("[route] route %08x -> group %p index %u", static_cast<unsigned>(routeId), h->group, h->index);
        return out;
    }
    std::uint64_t RouteGeneration(void*, std::uint64_t, std::uint64_t, std::uint64_t)
    {
        if (!g_routeSys) return 1;
        std::uint8_t* rs = static_cast<std::uint8_t*>(g_routeSys);
        std::uint32_t g = 2166136261u;
        for (std::uint8_t* n = *reinterpret_cast<std::uint8_t**>(rs + 0x48); n && n != rs + 0x40; n = *reinterpret_cast<std::uint8_t**>(n + 8))
            g = (g ^ static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(n + 0x10)) >> 4)) * 16777619u;
        return g == 0 || g == 0xffffffffu ? 1 : g;   // 0 is the zeroed order record, 0xffffffff the "unknown" the order initialiser 0x140fa6910 writes; the brain only tests for a change
    }
    // [H+0x1b8] (the object with the controller at +0x40): vtable +0xa0(id) returns a registry object G whose vtable slot 0 the initialiser calls (event hash 0xc034e674).
    void* g_xVtbl[256];
    std::uint64_t XSlot20(void*, std::uint64_t, std::uint64_t, std::uint64_t) { static void* g = trap("H+0x1b8 registry"); return reinterpret_cast<std::uint64_t>(g); }
    alignas(16) std::uint8_t g_ai[8][0x80];
    alignas(16) std::uint8_t S[0x400];
    // Movement brain component (0x140fa46d0): +0x58 -> NPC service, +0x68 receives the target object. Its order record (r8 -> pointer to it):
    // +0x20 target vec4, +0x70 own position, +0x80 offset, +0x90 distance, +0xb0 mode hash, +0xb8 mode byte (0..10), set from outside.
    alignas(16) std::uint8_t M[0x400];
    // Locomotion component (0x140f89c10, events 1 enter, 2 update, 5/6 late): this+0x78 NPC service, +0x90 -> 16-byte entries per id whose first qword
    // is the animation controller, +0x98 first id, +0xe0 -> records of 0x50 bytes per id (+0x10 vec4, +0x2c/+0x38 floats zeroed on enter, +0x44 word).
    alignas(16) std::uint8_t L[0x400];
    alignas(16) std::uint8_t g_locoRec[8][0x50];
    void* g_locoCtl[8][2];
    alignas(16) std::uint8_t g_order[0x200];
    std::uint8_t* g_orderPtr = g_order;
    const std::uint8_t g_zero[0xc0] = {};   // stands for the request entry before the ActionController exists (diagnostics only)
    alignas(16) std::uint8_t g_npcEntries[8][0x130];   // NPC state entries: dword +0x110 holds the message-driven state bits
    // The gekko plugin's own motion-event callback (0x140f811e0 -> mapper 0x140f81220) turns clip events into bits of dword +0x110 of the NPC state entry: this+0x6c first unit id, this+0x180 the entry table.
    alignas(16) std::uint8_t g_plugEv[0x200];
    std::uint8_t* g_svc = nullptr;   // the NPC service of the shim (svc), for components built outside the shim (GekkoPush)
    std::uint64_t ProvTarget(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(g_targetObj); }
    // The component registry at [svc+0x70] (the same object as the target provider): slot 0 stores the handle of the child created for an index (called by Survive's child creation 0x141072e00),
    // slot 1 closes the list, slot 8 returns a pointer to the cell holding the handle of an index.
    void* g_regCells[16];
    std::uint64_t RegAdd(void*, std::uint64_t idx, std::uint64_t cell, std::uint64_t) { if (idx < 16 && cell) g_regCells[idx] = *reinterpret_cast<void**>(cell); return 0; }
    std::uint64_t RegClose(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }
    std::uint64_t RegGet(void*, std::uint64_t, std::uint64_t idx, std::uint64_t) { return reinterpret_cast<std::uint64_t>(&g_regCells[idx < 16 ? idx : 0]); }
    alignas(16) std::uint8_t H[0x400];
    alignas(16) std::uint8_t rec[8][0x2c0];

    // Motion events from the engine. Survive registers 0x140f94c70 (a Survive-layout callback) for the event group of the simple-event track (desc {+0 fn, +8 user = H,
    // +0x10 u64 0x62bcc8f4fcb6 = the .enchnk section id, ...}, group name StringId in r8). TPP's callback info has another layout (TppMotionEventCallbackImpl::Callback,
    // TppBearBasicAction NormalMotionEventCallback): vtable +0x50 is the filter (0 = deliver), +0x18 the user pointer, +0x20 the event key. The trampoline below takes its place
    // and feeds the key to the lifted event handler 0x140f94cb0.
    // Edge log: the engine delivers an event on every run frame while its range is active (425b406c on every idle frame), so only the first frame of each run of frames is logged.
    void LogEventEdge(std::uint32_t key, bool deliver)
    {
        static std::map<std::uint32_t, std::uint64_t> lastSeen;
        static int logged = 0;
        const std::uint64_t now = GetTickCount64();   // ms; a gap of more than 60 ms (about 4 frames) starts a new run
        auto it = lastSeen.find(key);
        const bool begin = it == lastSeen.end() || now > it->second + 60;
        lastSeen[key] = now;
        if (begin && logged < 600) { ++logged; Trace("[event] begin key %08x %s", key, deliver ? "" : "(filtered)"); }
    }
    void __fastcall EvTramp(unsigned id, void* info)
    {
        std::uint32_t key = 0;
        bool deliver = false;
        __try
        {
            void** vt = *reinterpret_cast<void***>(info);
            deliver = reinterpret_cast<int (__fastcall*)(void*)>(vt[10])(info) == 0;
            key = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(info) + 0x20);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        LogEventEdge(key, deliver);
        if (deliver) GekkoWorld::FireEvent(0, key, 0);
        (void)id;
    }
    std::uint64_t RegisterEventShim(void*, std::uint64_t, std::uint64_t name, std::uint64_t desc)
    {
        static std::uint64_t copy[8];
        std::memcpy(copy, reinterpret_cast<void*>(desc), sizeof(copy));
        copy[0] = reinterpret_cast<std::uint64_t>(&EvTramp);
        void** vt = *reinterpret_cast<void***>(g_fwdCtrl);
        Trace("[events] register group %llx user %llx key %llx", static_cast<unsigned long long>(name), static_cast<unsigned long long>(copy[1]), static_cast<unsigned long long>(copy[2]));
        reinterpret_cast<void (__fastcall*)(void*, unsigned, std::uint64_t, void*)>(vt[73])(g_fwdCtrl, 0, name, copy);   // TPP RegisterMotionEventCallback (slot 73 = Survive 79)
        return 0;
    }

    // Survive controller vtable slot -> forwarder to the TPP slot with the same body (GekkoCtrlFwd.asm). Slot 123 (+0x3d8, returns [unit+0x60]+0x10) stays a shim
    // until the CharaControl layouts of the two builds are compared.
    // Logged wrappers over the forwarders: the clip the lifted code asks for and the finished answer the real controller gives, on change (the game log only shows what happens in the engine
    // through these two calls).
    using FwdFn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
    std::uint64_t g_ctlRuns = 0, g_motionAt = ~0ull;   // controller stage runs, and the run count when the last motion was requested
    int g_lastFinished = -1;   // the last finished answer the lifted code got (diagnostics)
    // TPP queues a requested motion and applies it in its stages, and reads the clip's end flag from a node record that still belongs to the previous clip until the new node has been written
    // (SetMotion 0x140b6e920 queues, the query 0x140af2110 reads the end bit of the current node buffer, 0x141a7e570). The previous clip has normally ended when the next one is requested, so a
    // query before the new node is in place answers 1. The answer is held at 0 until the real query has answered 0 once for the new clip ("applied"). A clip whose answer never leaves 1 is accepted
    // after its own length (mtar frames at 60 fps, Hooks::clipSeconds) of controller time, or after 2 stage runs when the length is unknown.
    bool g_applied = true;
    double g_setClock = 0; float g_setLength = 0; std::uint64_t g_setGani = 0, g_setRuns = 0;
    std::uint64_t SetMotionLogged(void* self, std::uint64_t a, std::uint64_t desc, std::uint64_t c)
    {
        g_setGani = *reinterpret_cast<unsigned long long*>(desc + 8);
        Trace("[ctl] set motion gani %llx layer %llx blend %.1f", (unsigned long long)g_setGani, *reinterpret_cast<unsigned long long*>(desc), *reinterpret_cast<float*>(desc + 0x14));
        g_motionAt = g_ctlRuns;
        g_setRuns = g_ctlRuns;
        g_applied = false;
        g_setClock = g_hooks.runClock ? g_hooks.runClock() : 0.0;
        g_setLength = g_hooks.clipSeconds ? g_hooks.clipSeconds(g_setGani) : 0.0f;
        return reinterpret_cast<FwdFn>(fwd_c62)(self, a, desc, c);
    }
    std::uint64_t FinishedLogged(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c)
    {
        std::uint64_t r = g_motionAt == g_ctlRuns ? 0 : reinterpret_cast<FwdFn>(fwd_c95)(self, a, b, c);   // 0 while the requested motion is not applied yet
        if (g_motionAt != g_ctlRuns && !g_applied)
        {
            const double played = g_hooks.runClock ? g_hooks.runClock() - g_setClock : 0.0;
            if (!(r & 0xff)) { g_applied = true; Trace("[ctl] clip %llx applied after %llu stage run(s)", (unsigned long long)g_setGani, (unsigned long long)(g_ctlRuns - g_setRuns)); }
            else if (g_setLength > 0 ? played >= g_setLength : g_ctlRuns - g_setRuns >= 2)
            {
                g_applied = true;
                Trace("[ctl] clip %llx: finished answered 1 from the start, accepted after %.2f s (clip %.2f s)", (unsigned long long)g_setGani, played, g_setLength);
            }
            else r = 0;
        }
        static int last = -1;
        if (static_cast<int>(r & 0xff) != last) { last = static_cast<int>(r & 0xff); Trace("[ctl] finished -> %d", last); }
        g_lastFinished = static_cast<int>(r & 0xff);
        return r;
    }

    void UseRealController()
    {
        for (const CtrlFwd& m : kCtrlFwd) g_ctlVtbl[m.slot] = reinterpret_cast<void*>(m.fn);
        g_ctlVtbl[62] = reinterpret_cast<void*>(&SetMotionLogged);   // logged wrappers over the forwarders for set motion and finished
        g_ctlVtbl[95] = reinterpret_cast<void*>(&FinishedLogged);
        g_ctlVtbl[79] = reinterpret_cast<void*>(&RegisterEventShim);
        g_entVtbl[87] = reinterpret_cast<void*>(&fwd_c87);
    }

    std::uint64_t AcSlot(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    std::uint64_t ChildrenSlot(void*, std::uint64_t, std::uint64_t, std::uint64_t);
    std::uint64_t ActionListSlot(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    void Build()
    {
        fillTraps(std::make_integer_sequence<int, 256>());
        std::memcpy(g_ctlVtbl, g_trapVtbl, sizeof(g_ctlVtbl));
        g_ctlVtbl[62] = reinterpret_cast<void*>(&CtlSetMotion);
        g_ctlVtbl[95] = reinterpret_cast<void*>(&CtlFinished);
        g_ctlVtbl[123] = reinterpret_cast<void*>(&CtlMotionOut);
        g_ctlVtbl[42] = reinterpret_cast<void*>(&CtlTrue);   // headless stand-in for TPP slot 38 (returns true on the real controller)
        std::memcpy(g_entVtbl, g_trapVtbl, sizeof(g_entVtbl));
        g_entVtbl[87] = reinterpret_cast<void*>(&SpeedFn);

        put<std::uint32_t>(H, 0x228, 0);                                        // first instance id
        put<void*>(H, 0x1d8, rec);
        static std::uint8_t perId[0x2000];
        put<void*>(H, 0x1f8, perId);   // per-id block array the record allocator points +0x1b0 into (0x140f943e6)
        for (int off = 0x40; off < 0x230; off += 8)
            if (off != 0x1d8 && off != 0x228 && off != 0x1f8) put<void*>(H, off, trap("H+0x" + std::to_string(off)));
        // H+0x1b8 -> obj, obj+8 -> table {+8 unit entries of 0x60, +0x18 float array, +0x20 stride, +0x24 first id}
        static std::uint8_t objA[0x100], tbl[0x100], entries[8][0x60]; static float arr[64];
        put<void*>(H, 0x1b8, objA);
        put<void*>(objA, 8, tbl);
        put<void*>(tbl, 0x18, arr);
        put<std::uint32_t>(tbl, 0x20, 1);
        put<std::uint32_t>(tbl, 0x24, 0);
        put<void*>(tbl, 8, entries);
        for (auto& e : entries)
        {
            put<void*>(e, 0x20, trap("entry.animController", 2, g_entVtbl));
            for (int o = 0x38; o < 0x50; o += 4) put<float>(e, o, 1.0f);
            put<std::uint32_t>(e, 0x4c, 0);
        }
        for (float& f : arr) f = 1.0f;
        // NPC service: H+0x1a0 -> svc; svc+0xf8 -> table {+8 entries of 0x28 bytes, +0x10 count}; svc+0x48 -> {+0x20 -> 16-byte entries, float at +0xc}
        static std::uint8_t svcBlk[0x230], tbl2[0x40], ents[8][0x28], o48[0x80]; static float sc[8][4];
        std::uint8_t* const svc = svcBlk + 0x20;   // [svc-0x10] is the owner entry the components read (GekkoLife::Prepare)
        g_svc = svc;
        for (auto& x : sc) x[3] = 1.0f;
        put<void*>(svc, 0, g_trapVtbl); names()[svc] = "npcService";
        put<void*>(svc, 0xf8, tbl2);
        put<void*>(tbl2, 0, g_trapVtbl); names()[tbl2] = "npcStateTable";   // vtable +0x10 is SetState(id, state, flag)
        put<void*>(tbl2, 8, ents);
        put<std::uint32_t>(tbl2, 0x10, 8);
        put<void*>(svc, 0x48, o48);
        put<void*>(o48, 0x20, sc);
        g_pose = sc;
        { static std::uint16_t unitIndex[8] = {}; put<void*>(o48, 0x28, unitIndex); }   // [[svc+0x48]+0x28]: word per unit id (0x140f82280, 0x140f78a90), 0 for unit 0
        put<void*>(o48, 0x38, g_dt);          // seconds of this frame per instance id; +0x48 is a scale (at 0x140f9e900)
        put<float>(o48, 0x48, 1.0f);
        put<void*>(H, 0x1a0, svc);
        // H+0x20 -> per-instance array of 16-byte entries, float at +0xc (a scale read by 0x140f949e0)
        static float e16[8][4]; for (auto& x : e16) x[3] = 1.0f; put<void*>(H, 0x20, e16);
        std::memcpy(g_xVtbl, g_trapVtbl, sizeof(g_xVtbl));
        g_xVtbl[20] = reinterpret_cast<void*>(&XSlot20);
        put<void*>(objA, 0, g_xVtbl); names()[objA] = "H+0x1b8 object";
        put<void*>(tbl, 0, g_trapVtbl); names()[tbl] = "H+0x1b8 table";
        nest(objA, "H+0x1b8");
        nest(tbl, "H+0x1b8->tbl");
        nest(svc, "npcService", 0x200);
        // Further service pointers the handlers call (+0xd0 slot 8 and +0x108 slot 17 with (id, hash, vec4): effect and sound triggers by name hash, at 0x140f94814..0x140f94824);
        // logged, not implemented. +0x1f8 is left alone: the dword at +0x1fc is a flag word.
        for (int off = 0x100; off < 0x1f8; off += 8)
            if (!*reinterpret_cast<void**>(svc + off) && off != 0x14 * 8)
                put<void*>(svc, off, trap("npcService->+0x" + std::to_string(off), 1));
        for (int off : { 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0 })
            if (!*reinterpret_cast<void**>(svc + off)) put<void*>(svc, off, trap("npcService->+0x" + std::to_string(off), 1));
        static void* endVtbl[256]; static std::uint8_t endObj[0x20];
        std::memcpy(endVtbl, g_trapVtbl, sizeof(endVtbl));
        endVtbl[12] = reinterpret_cast<void*>(&RouteEnd);
        put<void*>(endObj, 0, endVtbl); names()[endObj] = "npcService->+0x78";
        put<void*>(svc, 0x78, endObj);
        for (auto& r : rec)
        {
            put<void*>(r, 0x200, trap("record.controller", 2, g_ctlVtbl));
            // character bodies the actions write poses and surface ids into (+0x180, +0x188, +0x190; at 0x140f96040 calls vtable +0x40 on +0x180)
            for (int off : { 0x180, 0x188, 0x190 }) put<void*>(r, off, trap("record.body+0x" + std::to_string(off)));
        }
        put<void*>(objA, 0x40, *reinterpret_cast<void**>(rec[0] + 0x200));   // [[R+0x198]+0x40] is the controller the instance initialiser 0x140fa1340 stores in the record
        // [H+0x1c8] and [L+0x88] are the ActionController interface: set when the Observer setup has created it (AcSlot).
        // H+0x28 -> instance bitmap {+0x10 -> u32 words, +0x24 first id}, one bit per instance set when its action finished
        static std::uint8_t bitmapObj[0x40]; static std::uint32_t bits[8];
        put<void*>(bitmapObj, 0x10, bits);
        put<void*>(H, 0x28, bitmapObj);
        // AI object S (state machine 0x140f8d3f0 runs with rcx = S+0x20): S+0x1b0 -> the NPC service, S+0x1d8 -> 0x80-byte AI records
        // (+0x74 timer, +0x7c state, +0x7f flags). Service +0x80 -> the ActionController (its records: +0x1c4 bit 0x20 = action finished),
        // +0x70 -> target provider whose vtable +0x70 returns an object with the target vec4 at +0x20. from the state handlers.
        for (int off = 0x40; off < 0x230; off += 8)
            if (off != 0x1b0 && off != 0x1d8) put<void*>(S, off, trap("S+0x" + std::to_string(off)));
        put<void*>(S, 0x1b0, svc);
        put<void*>(S, 0x1d8, g_ai);
        put<std::uint32_t>(svc, 0x30, 0); put<void*>(svc, 0x38, nullptr);   // no list of units with pending work: the ActionController update (0x140e7a268..0x140e7a285) takes its all-units branch
        put<void*>(svc, 0x50, nullptr);   // the action list component: Survive's creation 0x141073e00 returns at once while the replication bit of [svc+0x1fc] is clear (it is), so the field stays zero
        static void* provVtbl[256];
        std::memcpy(provVtbl, g_trapVtbl, sizeof(provVtbl));
        provVtbl[0] = reinterpret_cast<void*>(&RegAdd);
        provVtbl[1] = reinterpret_cast<void*>(&RegClose);
        provVtbl[8] = reinterpret_cast<void*>(&RegGet);
        provVtbl[14] = reinterpret_cast<void*>(&ProvTarget);
        static std::uint8_t prov[0x40];
        put<void*>(prov, 0, provVtbl); names()[prov] = "targetProvider";
        put<void*>(svc, 0x70, prov);
        put<void*>(svc, 0x108, trap("npcService->+0x108 (event bus: vtable +0x88(id, hash, data))"));   // at 0x140f936ae..0x140f936b9
        // NPC count at svc+0x14 and the alive bitmap at [svc+0x48]+8 (peer loop in the movement brain, at 0x140fa52d8..0x140fa5323); one NPC
        put<void*>(svc, 0x10, nullptr);
        put<std::uint32_t>(svc, 0x14, 1);
        put<std::uint32_t>(svc, 0x18, 1);   // instance capacity read by the Observer setup (0x140f74146: sizes its per-instance arrays by [svc+0x18])
        static std::uint8_t alive[8] = { 1 };
        put<void*>(o48, 8, alive);
        for (int off = 0x40; off < 0x230; off += 8)
            if (off != 0x58) put<void*>(M, off, trap("M+0x" + std::to_string(off)));
        put<void*>(M, 0x58, svc);
        for (int off = 0x40; off < 0x230; off += 8)
            if (off != 0x78 && off != 0x80 && off != 0x88 && off != 0x90 && off != 0x98 && off != 0xd8 && off != 0xe0) put<void*>(L, off, trap("L+0x" + std::to_string(off)));
        // The locomotion component's real vtable (0x1422ae858): slot 6 is the state executor (table 0x143086190 of state handlers), 9 reset, 10 the tick the host calls, 17/20 base class.
        static void* locoVtbl[256];
        std::memcpy(locoVtbl, g_trapVtbl, sizeof(locoVtbl));
        for (auto sl : { std::pair<int, std::uint64_t>{ 6, 0x140f88720ull }, { 7, 0x140f89230ull }, { 9, 0x140f897d0ull }, { 10, 0x140f88c70ull }, { 17, 0x141091f00ull }, { 20, 0x141091f70ull } })
            locoVtbl[sl.first] = reinterpret_cast<void*>(lift::kTokenBase + sl.second);
        put<void*>(L, 0, locoVtbl);
        put<void*>(L, 0x78, svc);
        put<void*>(L, 0x80, objA);   // the per-unit animation table object (same as H+0x1b8): speed scales are written into its float array (0x140e7f210)
        // Path tracker the locomotion queries in its late event (0x140f8a629..0x140f8a676: [[L+0xd8]+0x10] vtable +0x68 (id, &a, &b); a sets bit 1 of the state flags, b bit 2). While bit 1 or 2
        // is set the walk state skips starting its loop clip (0x140f89fa4 tests the whole word) and runs the arrival test 0x140f88780 (3.0 m) that ends the walk with a stop clip.
        // The query itself is Survive's own 0x140f7f7f0 (lifted): it reads bit 9 (a) and bit 8 (b) of dword +0x110 of the NPC state entry (0x130 bytes per NPC at [this+0xa8], first id at
        // [this-0x6c]). The bits are set by the plugin's clip event mapper 0x140f81220 from the walk loop events (0xb9817d52 -> 0x200, 0x3903ca38 -> 0x100; slot 3 and run slot 7 only,
        // research/event_bits.tsv) and cleared by the plugin frame kind 0. Measured: with the bit set from the first frame the gekko never starts its loop clip.
        static void* trackVtbl[256]; static std::uint8_t trackBuf[0x400], trackIface[0x40];
        std::memcpy(trackVtbl, g_trapVtbl, sizeof(trackVtbl));
        trackVtbl[13] = reinterpret_cast<void*>(lift::kTokenBase + 0x140f7f7f0ull);
        std::uint8_t* trackObj = trackBuf + 0x100;
        put<void*>(trackObj, 0, trackVtbl);
        put<void*>(trackObj, 0xa8, g_npcEntries);
        put<std::uint32_t>(trackObj, -0x6c, 0);
        put<void*>(trackIface, 0x10, trackObj);
        put<void*>(L, 0xd8, trackIface);
        put<void*>(L, 0x90, g_locoCtl);
        put<std::uint64_t>(L, 0x98, 0);
        put<void*>(L, 0xe0, g_locoRec);
        for (int i = 0; i < 8; ++i) g_locoCtl[i][0] = *reinterpret_cast<void**>(rec[i] + 0x200);
        std::memcpy(g_navVtbl, g_trapVtbl, sizeof(g_navVtbl));
        g_navVtbl[0] = reinterpret_cast<void*>(&NavRequest);
        g_navVtbl[3] = reinterpret_cast<void*>(&NavRecord);
        g_navVtbl[8] = reinterpret_cast<void*>(&NavSnap);
        static void* nav = g_navVtbl;
        put<void*>(svc, 0x60, &nav);
        // svc+0x68: an owner lookup whose vtable +0x68 returns a pointer to the owner's entry; entry -> object whose +0x10 is the component container the Observer setup queries (0x140f744ab..c8; rdi is 0 there, so a null entry faults)
        static void* ownerVtbl[256]; static std::uint8_t ownerObj[0x20];
        std::memcpy(ownerVtbl, g_trapVtbl, sizeof(ownerVtbl));
        ownerVtbl[13] = reinterpret_cast<void*>(+[](void*, std::uint64_t, std::uint64_t, std::uint64_t) -> std::uint64_t { return reinterpret_cast<std::uint64_t>(&g_ownerEntry); });
        put<void*>(ownerObj, 0, ownerVtbl);
        static std::uint8_t ownerTbl[0x1c * 9];   // +8: per-instance array of 0x1c-byte entries, a word at +0x16 written by the Observer setup (0x140f74500)
        put<void*>(ownerObj, 8, ownerTbl);
        put<void*>(svc, 0x68, ownerObj);
        std::memcpy(g_pathAVtbl, g_trapVtbl, sizeof(g_pathAVtbl));
        g_pathAVtbl[17] = reinterpret_cast<void*>(&PathCurrent);
        g_pathAVtbl[19] = reinterpret_cast<void*>(&PathA);
        g_pathAVtbl[20] = reinterpret_cast<void*>(&PathProgress);
        static void* pathA = g_pathAVtbl;
        g_pathRec.a = &pathA;
        put<std::int32_t>(reinterpret_cast<std::uint8_t*>(g_targetObj), 0x30, -1);   // target object: +0x30 id or -1, +0x34 float > 0, +0x68 / +0x6a flag bytes
        std::memcpy(g_svcVtbl, g_trapVtbl, sizeof(g_svcVtbl));
        g_svcVtbl[12] = reinterpret_cast<void*>(&RandSigned);
        g_svcVtbl[13] = reinterpret_cast<void*>(&RandUnit);
        g_svcVtbl[14] = reinterpret_cast<void*>(&RandRaw);
        GekkoLife::InstallSvcSlots(g_svcVtbl);
        g_svcVtbl[123] = reinterpret_cast<void*>(&ChildrenSlot);     // +0x3d8: the container Setup creates its child nodes through Survive's own function 0x141072e00
        g_svcVtbl[126] = reinterpret_cast<void*>(&AcSlot);           // +0x3f0: the Observer setup creates the ActionController through Survive's own function 0x141072c50
        g_svcVtbl[139] = reinterpret_cast<void*>(&ActionListSlot);   // +0x458: the action list component, 0x141073e00
        put<void*>(svc, 0, g_svcVtbl);
        put<std::uint32_t>(svc, 0x1c, RandomSeedNow());   // the random state word, seeded once as Survive's init does (0x141076a25)
        std::memcpy(g_posVtbl, g_trapVtbl, sizeof(g_posVtbl));
        g_posVtbl[29] = reinterpret_cast<void*>(&PosFn);
        static void* posObj = g_posVtbl;
        g_meleeCtx.pos = &posObj;
        g_meleeCtx.svc = svc;
    }

    // Native-call policy: a call into the game exe's address range is a Survive->TPP twin picked by prologue similarity, so it is refused
    // and logged once unless verified (kAllowed). Calls into other modules (CRT, system) and into this DLL (the shim) pass.
    // Verified TPP twins: research/twin_verify.tsv (tools/verify_twins.py: same instruction stream, branch and call targets masked) plus the manager getter.
    const std::uint64_t kAllowed[] = { 0x140004350ull, 0x1400d93f0ull, 0x140a12220ull, 0x1407a9470ull, 0x14055ecd0ull, 0x141a05868ull, 0x141a058cc, 0x141a05fb0ull, 0x140bff120ull, 0x140bff050ull };
    // The pool classes' callees. Only real with the TPP exe present: headless they are refused.
    // All "same" in research/twin_verify.tsv (tools/verify_twins.py): Survive 0x140016a20 0x14001d9c0 0x14001dc20 0x14001dd60 0x14007ba30 0x14007ba90 0x14007bb70 0x141924274 0x141af1a80 0x141af72f0 0x141af7430.
    const std::uint64_t kGameOnly[] = { 0x140016090ull, 0x140023a90ull, 0x14001d010ull, 0x14001d270ull, 0x14001d3b0ull, 0x1400714b0ull, 0x140071510ull, 0x1400715f0ull, 0x141a05a7cull, 0x141bcb600ull, 0x141bd0f20ull, 0x141bd1060ull, 0x140023a90ull };

    // Survive's shared-object pool (vtable 0x1423d8cb0, 13 slots), lifted. The manager's pool [[G+0x80]+0x10] hands the record its pooled objects (0x140f943b0); TPP's own pool builds
    // other classes (slot 0x20 object with a null vtable, fault at 0x140f9460a). The object is one qword, the vtable of lifted slot functions (tokens).
    constexpr std::uint64_t kPoolSlots[13] = { 0x141b18fb0, 0x141b19060, 0x141b19090, 0x141b19000, 0x141b19030, 0x141b190f0, 0x141b190c0, 0x141b19130, 0x141b19310, 0x141b19320, 0x141b18ff0, 0x141b192d0, 0x141b19170 };
    void* g_poolVtbl[13];
    void* g_poolObj = g_poolVtbl;
    void* SurvivePool()
    {
        for (int i = 0; i < 13; ++i) g_poolVtbl[i] = reinterpret_cast<void*>(lift::kTokenBase + kPoolSlots[i]);
        return &g_poolObj;
    }

    // Headless tests have no TPP: the global manager getter 0x140bff050 (the real engine object in the game) answers with a stand-in of trap objects. Only used when
    // no real controller was set, i.e. never in the game.
    std::uint8_t* HeadlessManager()
    {
        static std::uint8_t obj[0x200], x[0x100], registry[0x40], poolMgr[0x40];
        static void* inner = trap("manager.inner");
        if (!*reinterpret_cast<void**>(obj + 0xa8)) *reinterpret_cast<void**>(obj + 0xa8) = &inner;
        *reinterpret_cast<void**>(obj + 0x98) = x;
        *reinterpret_cast<void**>(x + 0xa8) = registry;
        *reinterpret_cast<void**>(obj + 0x80) = poolMgr;
        *reinterpret_cast<void**>(poolMgr + 0x10) = SurvivePool();
        return obj;
    }

    // In the game the lifted code gets a copy of the engine's manager (every service pointer is the real one) whose pool slot [[G+0x80]+0x10] is Survive's lifted pool. The copy is
    // refreshed on every call so it never goes stale. 0x200 / 0x100 bytes cover the offsets the lifted code reads (+0x80, +0x98, +0xa8, +0xb0; block +0x8, +0x10).
    bool CopyBytes(void* dst, const void* src, std::size_t n)
    {
        __try { std::memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::uint8_t* ProxyManager()
    {
        static std::uint8_t mgr[0x200], blk[0x100];
        const auto real = reinterpret_cast<std::uint8_t* (*)()>(0x140bff050ull)();
        if (!real || !CopyBytes(mgr, real, sizeof(mgr))) return real;
        std::uint8_t* rb = *reinterpret_cast<std::uint8_t**>(mgr + 0x80);
        if (!rb || !CopyBytes(blk, rb, sizeof(blk))) return real;
        *reinterpret_cast<void**>(blk + 0x10) = SurvivePool();
        *reinterpret_cast<void**>(mgr + 0x80) = blk;
        // [[G+0x98]+0xa8] is Survive's hash registry (bucket count word +0x28, table +0x20, walked by 0x140f943b0 and the surface-state predicates); TPP's object at that place has another
        // layout, and walking it hung the game (FoxPhysics watchdog sample RIP in L_140f943b0 +0x7d1). An empty registry (count 0) answers "not found" for every hash.
        static std::uint8_t blk98[0x100], registry[0x80];
        std::uint8_t* r98 = *reinterpret_cast<std::uint8_t**>(mgr + 0x98);
        if (r98 && CopyBytes(blk98, r98, sizeof(blk98)))
        {
            *reinterpret_cast<void**>(blk98 + 0xa8) = registry;
            *reinterpret_cast<void**>(mgr + 0x98) = blk98;
        }
        return mgr;
    }

    std::set<std::uint64_t> g_dynAllowed;   // TPP addresses a port component registered at run time (GekkoWorld::AllowNative)

    bool NativeGate(lift::Cpu& c, std::uint64_t target)
    {
        if (target == 0x140bff050ull) { c.r[lift::RAX] = reinterpret_cast<std::uint64_t>(g_fwdCtrl ? ProxyManager() : HeadlessManager()); return true; }
        // TPP's stack probe (twin of Survive's __chkstk 0x1419247b0) walks down from the thread stack limit (gs:[0x10]) to rsp-size. The emulated stack is a static buffer outside the thread stack,
        // so the probe touched unmapped pages (stack overflow in the plugin frame function, in game). The buffer is committed already: nothing to probe.
        if (target == 0x141a05fb0ull) return true;
        // Headless only: the clock getter the registry lock reads (TPP 0x140023a90, twin of Survive 0x140016a80; in the game it is a checked twin and runs natively): milliseconds into *rcx, returns rcx.
        if (target == 0x140023a90ull && !g_fwdCtrl) { *reinterpret_cast<std::uint64_t*>(c.r[lift::RCX]) = GetTickCount(); c.r[lift::RAX] = c.r[lift::RCX]; return true; }
        if (target < 0x140000000ull || target >= 0x150000000ull) return false;
        for (std::uint64_t a : kAllowed) if (a == target) return false;
        if (g_dynAllowed.count(target)) return false;
        if (g_fwdCtrl) for (std::uint64_t a : kGameOnly) if (a == target) return false;
        static std::map<std::uint64_t, int> seenTargets;
        if (seenTargets[target]++ == 0) Trace("[native] refused TPP 0x%llx from survive pc 0x%llx", (unsigned long long)target, (unsigned long long)lift::g_pc);
        c.r[lift::RAX] = 0;
        c.x[0] = _mm_setzero_ps();
        return true;
    }

    int Filter(EXCEPTION_POINTERS* e, void** at, void** data)
    {
        *at = e->ExceptionRecord->ExceptionAddress;
        *data = e->ExceptionRecord->NumberParameters >= 2 ? reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    // No C++ objects here: __try cannot share a function with unwinding.
    std::uint64_t g_lastRax = 0;
    float g_lastFloat = 0;
    // A host function that lifted code calls (the component creation manager, the svc slot thunks) may call lifted code again. lift::run has one stack, so every nesting level gets its own.
    constexpr int kNest = 4;
    int g_nest = 0;
    alignas(64) std::uint8_t g_nestStack[kNest][1 << 16];
    bool Guarded(lift::LiftFn f, lift::Cpu& c, unsigned long& code, void*& at, void*& data)
    {
        if (g_nest >= kNest) { code = 0xE1F70002; return false; }
        std::uint8_t* st = g_nestStack[g_nest++];
        c.r[lift::RSP] = (reinterpret_cast<std::uint64_t>(st) + (1 << 16) - 128) & ~15ull;
        lift::push_ret(c);
        bool ok = true;
        __try { f(c); }
        __except (code = GetExceptionCode(), Filter(GetExceptionInformation(), &at, &data)) { ok = false; }
        --g_nest;
        return ok;
    }
    // Same as Guarded with the fifth argument on the stack (Win64: [entry rsp + 0x28]); lift::run owns its stack, so this has its own.
    // Stack arguments 5.. of the Windows x64 call (the callee's [rsp+0x28+8*i] at entry).
    bool GuardedS(lift::LiftFn f, lift::Cpu& c, const std::uint64_t* stack, int n, unsigned long& code)
    {
        if (g_nest >= kNest) { code = 0xE1F70002; return false; }
        std::uint8_t* st = g_nestStack[g_nest++];
        c.r[lift::RSP] = (reinterpret_cast<std::uint64_t>(st) + (1 << 16) - 128) & ~15ull;
        lift::push_ret(c);
        for (int i = 0; i < n; ++i) *reinterpret_cast<std::uint64_t*>(c.r[lift::RSP] + 0x28 + 8 * i) = stack[i];
        bool ok = true;
        __try { f(c); }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        --g_nest;
        return ok;
    }
    bool GuardedX(lift::LiftFn f, lift::Cpu& c, std::uint64_t arg5, unsigned long& code) { return GuardedS(f, c, &arg5, 1, code); }
    bool CallLiftedS(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, const std::uint64_t* stack, int n, const char* what)
    {
        const lift::LiftFn f = g_ready ? lift::find_fn(addr) : nullptr;
        if (!f) { std::snprintf(g_fault, sizeof(g_fault), "%s: 0x%llx not lifted", what, static_cast<unsigned long long>(addr)); return false; }
        lift::Cpu c{};
        c.r[lift::RCX] = rcx; c.r[lift::RDX] = rdx; c.r[lift::R8] = r8; c.r[lift::R9] = r9;
        unsigned long code = 0;
        if (!GuardedS(f, c, stack, n, code)) { std::snprintf(g_fault, sizeof(g_fault), "%s fault 0x%lx at survive pc 0x%llx", what, code, static_cast<unsigned long long>(lift::g_pc)); return false; }
        g_lastRax = c.r[lift::RAX];
        return true;
    }
    // svc slot 123 (+0x3d8), called by the container Setup (0x140f8232e) with the output array (container+0x30) and the child table (key, creation arguments per entry): Survive's function creates
    // each child through the creation context and registers its handle in the registry at [svc+0x70].
    std::uint64_t ChildrenSlot(void* self, std::uint64_t out, std::uint64_t table, std::uint64_t)
    {
        if (!CallLifted(0x141072e00ull, reinterpret_cast<std::uint64_t>(self), out, table, 0, "container children")) { Trace("[nodes] %s", g_fault); return 0; }
        return g_lastRax;
    }
    // svc slot 126 (+0x3f0), called by the Observer setup (0x140f743af) with the keys of the gekko's two ActionController components and five stack arguments: Survive's own function creates the
    // ActionController (creator 0x140e786d0 and Setup through the host's creation context) and stores its interface at [svc+0x80]. The consumers cache that pointer: [H+0x1c8], [L+0x88].
    std::uint64_t AcSlot(void* self, std::uint64_t owner, std::uint64_t key1, std::uint64_t key2, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8, std::uint64_t a9)
    {
        const std::uint64_t st[5] = { a5, a6, a7, a8, a9 };
        if (!CallLiftedS(0x141072c50ull, reinterpret_cast<std::uint64_t>(self), owner, key1, key2, st, 5, "action controller creation")) { Trace("[ac] %s", g_fault); return 0; }
        g_ac = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + 0x80);
        if (g_ac) { put<void*>(H, 0x1c8, g_ac); put<void*>(L, 0x88, g_ac); }
        Trace("[ac] ActionController %p, requests %p, records %p, svc+0x60 (nav) %p, flags %08x", g_ac, g_ac ? *reinterpret_cast<void**>(g_ac + 8) : nullptr, g_ac ? *reinterpret_cast<void**>(g_ac + 0x10) : nullptr,
              *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 0x60), *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(self) + 0x1fc));
        return 0;
    }
    // svc slot 139 (+0x458), also called by the Observer setup (0x140f74615): 0x141073e00 creates the action list component only with the replication bit of [svc+0x1fc] set; it is clear.
    std::uint64_t ActionListSlot(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t a5, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t)
    {
        const std::uint64_t st[1] = { a5 };
        if (!CallLiftedS(0x141073e00ull, reinterpret_cast<std::uint64_t>(self), a, b, c, st, 1, "action list creation")) { Trace("[ac] %s", g_fault); return 0; }
        return g_lastRax;
    }
    // Survive seeds the state at 0x141076a0c..0x141076a25 with (int)(clock service slot 2 + 331.0) (a double of seconds). TPP has no such clock object: seconds of the system tick count.
    std::uint32_t RandomSeedNow()
    {
        const std::uint32_t s = static_cast<std::uint32_t>(static_cast<std::int64_t>(GetTickCount64() / 1000.0 + 331.0));
        return s ? s : 0x12345678u;   // xorshift stays at 0 once there
    }
    bool RunRandom(std::uint64_t fn, void* self, std::uint64_t* rax, float* f)
    {
        const lift::LiftFn lf = lift::find_fn(fn);
        if (!lf) return false;
        lift::Cpu c{};
        c.r[lift::RCX] = reinterpret_cast<std::uint64_t>(self);
        unsigned long code = 0; void* at = nullptr; void* data = nullptr;
        if (!Guarded(lf, c, code, at, data)) return false;
        *rax = c.r[lift::RAX];
        std::memcpy(f, &c.x[0], sizeof(float));
        return true;
    }
    float RandSigned(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { std::uint64_t r; float f = 0; RunRandom(0x141076380ull, self, &r, &f); return f; }
    float RandUnit(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { std::uint64_t r; float f = 0; RunRandom(0x141076330ull, self, &r, &f); return f; }
    std::uint64_t RandRaw(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { std::uint64_t r = 0; float f; RunRandom(0x1410763e0ull, self, &r, &f); return r & 0xffffffffull; }
    void FatalLog(const char* what, unsigned long long a) { Trace("[FATAL] %s (0x%llx) at survive pc 0x%llx", what, a, (unsigned long long)lift::g_pc); }
}

namespace
{
    // Calls a lifted function; traces what it changed in record 0. No C++ objects live in the __try function.
    bool CallLifted(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, const char* what)
    {
        if (!g_ready) return false;
        const lift::LiftFn f = lift::find_fn(addr);
        if (!f) { std::snprintf(g_fault, sizeof(g_fault), "%s: 0x%llx not lifted", what, (unsigned long long)addr); return false; }
        lift::Cpu c{};
        c.r[lift::RCX] = rcx; c.r[lift::RDX] = rdx; c.r[lift::R8] = r8; c.r[lift::R9] = r9;
        unsigned long code = 0; void* at = nullptr; void* data = nullptr;
        std::uint8_t before[sizeof(rec[0])], beforeReq[0xc0] = {}, beforeCtl[0x1d0] = {};
        std::memcpy(before, rec[0], sizeof(before));
        const bool haveAc = g_ac && *reinterpret_cast<void**>(g_ac + 8) && *reinterpret_cast<void**>(g_ac + 0x10);
        if (haveAc) { std::memcpy(beforeReq, AcRequest(0), sizeof(beforeReq)); std::memcpy(beforeCtl, AcRecord(0), sizeof(beforeCtl)); }
        const bool ran = Guarded(f, c, code, at, data);
        g_lastRax = c.r[lift::RAX];
        std::memcpy(&g_lastFloat, &c.x[0], sizeof(float));   // xmm0 low lane: the float a lifted function returns
        if (!ran)
        {
            std::snprintf(g_fault, sizeof(g_fault), "%s fault 0x%lx at %p data %p, survive pc 0x%llx", what, code, at, data, (unsigned long long)lift::g_pc);
            return false;
        }
        if (g_hooks.trace)
        {
            const struct { const char* n; const std::uint8_t* a; const std::uint8_t* b; std::size_t size; } d[] = {
                { "record", before, rec[0], sizeof(before) }, { "request", beforeReq, haveAc ? AcRequest(0) : beforeReq, sizeof(beforeReq) }, { "ctlrec", beforeCtl, haveAc ? AcRecord(0) : beforeCtl, sizeof(beforeCtl) } };
            for (const auto& x : d)
                for (std::size_t i = 0; i + 4 <= x.size; i += 4)
                    if (std::memcmp(x.a + i, x.b + i, 4))
                    {
                        float fl; std::memcpy(&fl, x.b + i, 4);
                        Trace("[%s] +0x%zx: %08x -> %08x (float %.4f)", x.n, i, *reinterpret_cast<const std::uint32_t*>(x.a + i),
                              *reinterpret_cast<const std::uint32_t*>(x.b + i), fl);
                    }
        }
        return true;
    }

    std::uint64_t EvHash(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + 8); }
    void* g_evVtbl[16];
}

// Blocks env_alloc handed out. A free of anything else (memory a verified TPP twin allocated, e.g. string storage) is leaked on purpose: freeing it with the DLL heap corrupts the game's heap.
namespace
{
    std::set<void*>& OwnBlocks() { static std::set<void*> s; return s; }
}

void env_alloc(lift::Cpu& c)
{
    const std::uint64_t align = c.r[lift::RDX] < 16 ? 16 : c.r[lift::RDX];
    void* p = _aligned_malloc(static_cast<std::size_t>(c.r[lift::RCX]), static_cast<std::size_t>(align));
    if (p) { std::memset(p, 0, static_cast<std::size_t>(c.r[lift::RCX])); OwnBlocks().insert(p); }   // zeroed: Survive's constructors reset members through release-then-assign helpers
    c.r[lift::RAX] = reinterpret_cast<std::uint64_t>(p);
    static int n = 0;
    if (n++ < 12 && std::getenv("GEKKO_TRACE_ALLOC")) std::printf("[alloc] size %llx align %llx -> %p (pc 0x%llx)\n", (unsigned long long)c.r[lift::RCX], (unsigned long long)c.r[lift::RDX], p, (unsigned long long)lift::g_pc);
}

void env_alloc16(lift::Cpu& c)
{
    c.r[lift::RDX] = 16;
    env_alloc(c);
}

void env_memset(lift::Cpu& c)
{
    c.r[lift::RAX] = c.r[lift::RCX];
    std::memset(reinterpret_cast<void*>(c.r[lift::RCX]), static_cast<int>(c.r[lift::RDX] & 0xff), static_cast<std::size_t>(c.r[lift::R8]));
}

void env_memcpy(lift::Cpu& c)
{
    c.r[lift::RAX] = c.r[lift::RCX];
    std::memmove(reinterpret_cast<void*>(c.r[lift::RCX]), reinterpret_cast<const void*>(c.r[lift::RDX]), static_cast<std::size_t>(c.r[lift::R8]));
}

void env_memcmp(lift::Cpu& c)
{
    c.r[lift::RAX] = static_cast<std::uint32_t>(std::memcmp(reinterpret_cast<const void*>(c.r[lift::RCX]), reinterpret_cast<const void*>(c.r[lift::RDX]), static_cast<std::size_t>(c.r[lift::R8])));
}

void env_free(lift::Cpu& c)
{
    void* p = reinterpret_cast<void*>(c.r[lift::RCX]);
    if (OwnBlocks().erase(p)) _aligned_free(p);
}

namespace GekkoWorld
{
    unsigned PathCount(int id);   // points of the path the order holds (the lifted controller's table or the stand-in's), 0 when there is none
    // The component container the lifted components query for their siblings (Fox QueryInterface: vtable slot 3 = +0x18, id in rdx, result = the interface pointer, which is the
    // sub-object + 8; callers subtract 8 again). Survive's components cache what they ask for. Only what the lifted code has asked for so far is answered; others are logged.
    std::uint8_t* g_obs = nullptr;                     // the real SsdGeckoObserver (factory 0x140f72e40)
    void* g_qiVtbl[256];
    alignas(16) std::uint8_t g_qiObj[0x40], g_qiOwner[0x40];
    // The 0x2fbbd3df1420 block: +0x10 -> u16 per-unit table whose top three bits the plugin frame function reads (0x140f7dc08); zero until a producer is lifted.
    alignas(16) std::uint8_t g_sibBlk[0x200]; std::uint16_t g_sibTable[64];
    std::uint8_t* SiblingBlock() { put<void*>(g_sibBlk, 0x10, g_sibTable); return g_sibBlk; }
    std::uint64_t QiFn(void*, std::uint64_t id, std::uint64_t, std::uint64_t)
    {
        std::uint8_t* svc = *reinterpret_cast<std::uint8_t**>(H + 0x1a0);
        if (id == 0xb567a90fe2c5ull) return reinterpret_cast<std::uint64_t>(svc + 8);                          // SsdNpc service
        if (id == 0xdc11dba59fbbull && g_obs) return reinterpret_cast<std::uint64_t>(g_obs + 0x30 + 8);        // Observer (QI 0x140f738c0: this+0x30)
        if (id == 0x2c00097e7c98ull && g_obs) return reinterpret_cast<std::uint64_t>(g_obs + 0x20 + 8);        // Observer (this+0x20)
        if (id == 0xd2dac78d7ff2ull)   // per-instance state flags: Observer+0x88 (this+0x58 of the +0x30 interface); {+8 -> u64 entries (bit 1, 2, ... flags), +0x20 u32 count} (0x140f74a00)
        {
            alignas(16) static std::uint8_t blk[0x40]; static std::uint64_t entries[8];
            put<void*>(blk, 8, entries);
            put<std::uint32_t>(blk, 0x20, 8);
            return reinterpret_cast<std::uint64_t>(blk + 8);
        }
        if (id == 0x2fbbd3df1420ull)   // LifeController: the real one once the life class has made it (GekkoLife), the zeroed host block before
        {
            std::uint8_t* lc = GekkoLife::ControllerIface();
            return reinterpret_cast<std::uint64_t>((lc ? lc : SiblingBlock()) + 8);
        }
        static std::set<std::uint64_t> seen;
        if (seen.insert(id).second) Trace("[qi] unanswered interface id %llx from survive pc 0x%llx", static_cast<unsigned long long>(id), static_cast<unsigned long long>(lift::g_pc));
        return 0;
    }
    // Clock services the SsdNpc and its components read. Survive's own chain (time source object 0x14032d880, singleton proxy 0x142b18328) is engine infrastructure (locks, timers, per-thread
    // state) that was not lifted; TPP's tick count answers instead (stand-in for a service, no NPC decision depends on it):
    //  ctx+0x40: vtable slot 2 returns seconds as a double (SsdNpc Setup 0x141076a15, seeds the random state);
    //  ctx+0x78: proxy P; P slot 8 (+0x40) returns A, A slot 0 returns B, B slot 0 returns a byte (component 0x7cffa68e663e Setup 0x141320878..0x14132088c stores it at +0xa4); 0 = not paused.
    double ClockSeconds(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return static_cast<double>(GetTickCount64()) / 1000.0; }
    std::uint64_t ClockChainByte(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }
    void* g_clkB = nullptr; void* g_clkA = nullptr;
    std::uint64_t ClockChainB(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(g_clkB); }
    std::uint64_t ClockChainA(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(g_clkA); }
    // The creation context (scope singleton, scope table, the services the components resolve through it) and the TPP-independent half of the world; the nav service is started on top of it.
    bool BuildContext()
    {
        std::memcpy(g_qiVtbl, g_trapVtbl, sizeof(g_qiVtbl));
        g_qiVtbl[3] = reinterpret_cast<void*>(&QiFn);
        put<void*>(g_qiObj, 0, g_qiVtbl);
        put<void*>(g_qiOwner, 0x10, g_qiObj);
        g_ownerEntry = g_qiOwner;
        // Creation context: 0x141988fa0 is Survive's scope singleton, +8 a struct whose +0 holds the context object that constructors resolve shared handles through (vtable slot 0 with a name hash
        // in r8, at 0x140f746dc..0x140f746ed; the result is a pointer to the handle, stored at Observer+0x78). Host stand-in: every handle is a trap object.
        static void* ctxVtbl[256]; static std::uint8_t ctxObj[0x20]; static void* handle; std::uint8_t* ctxSlot = nullptr;
        std::memcpy(ctxVtbl, g_trapVtbl, sizeof(ctxVtbl));
        ctxVtbl[0] = reinterpret_cast<void*>(+[](void*, std::uint64_t out, std::uint64_t hash, std::uint64_t desc) -> std::uint64_t
        {
            if (GekkoLife::Owns(hash)) return GekkoLife::Create(out, hash, desc);   // life and damage components: Survive's creators and Setup functions
            if (GekkoNodes::Owns(hash)) { const std::uint64_t h = GekkoNodes::Create(out, hash, desc); if (h) return h; }   // the behaviour container and its child nodes: descriptors from Survive's own class list; a failed creation keeps the host handle below
            Trace("[ctx] handle for hash %llx", static_cast<unsigned long long>(hash));
#ifdef LIFT_PC_TRACE
            if (hash == 0x77e1d54ac2b1ull) lift::pc_ring_dump(30);
#endif
            handle = g_qiOwner;   // +0x10 -> the component container, answers the interface ids the setup asks for (0x140f74703)
            return reinterpret_cast<std::uint64_t>(&handle);
        });
        put<void*>(ctxObj, 0, ctxVtbl);
        // Survive's name registry, the global the target objects register in (Init 0x141abc990 -> 0x141af72f0): created by Survive's own creator, which nothing in the module calls.
        { static bool made = false; if (!made) { made = true; if (!CallLifted(0x141af7530ull, 0, 0, 0, 0, "name registry")) Trace("[init] name registry: %s", g_fault); } }
        const bool okCtx = CallLifted(0x141988fa0ull, 0, 0, 0, 0, "scope singleton");
        Trace("[ctx] scope singleton %d rax %llx %s", okCtx, static_cast<unsigned long long>(g_lastRax), okCtx ? "" : g_fault);
        const std::uint64_t scopeAddr = g_lastRax;
        // The context struct is Survive's own static (getter 0x141962ea0, the struct the scope singleton's +8 points at once the engine boot published it): its fields +0x18, +0x40, +0x48 and +0x78
        // are made by Survive's own static initialisers and setters below, not by the host.
        if (okCtx && scopeAddr && CallLifted(0x141962ea0ull, 0, 0, 0, 0, "context struct getter")) ctxSlot = reinterpret_cast<std::uint8_t*>(g_lastRax);
        g_lastRax = scopeAddr;
        if (okCtx && scopeAddr && ctxSlot) {
            // [ctx+8] is a struct: +0 the context object, +0xa8 -> pointer to the route system (RouteGet / RouteGeneration above).
            std::memcpy(g_routeVtbl, g_trapVtbl, sizeof(g_routeVtbl));
            g_routeVtbl[0] = reinterpret_cast<void*>(&RouteGet);
            g_routeVtbl[1] = reinterpret_cast<void*>(&RouteGeneration);
            for (auto& h : g_routeCache) { h.group = nullptr; h.index = 0xffff; }
            put<void*>(M, 0x70, g_routeCache);
            put<void*>(ctxSlot, 0, ctxObj);
            // ctx+0xa8 is read two ways: [[ctx+0xa8]] is the route system object (the movement brain), [[ctx+0xa8]+0x10] a registry whose vtable slot 1 makes a keyed handle (SsdNpc service slot 0x1410738e0,
            // stored at svc+0x118). The registry is a logging trap object, as svc+0x118 was in the host buffer.
            // The registries answer a keyed request with a per-unit array object ({+8 -> table of 8 bytes per unit}); the host makes it from zeroed memory.
            static void* arrVtbl[256]; static std::uint8_t arrReg[0x20];
            std::memcpy(arrVtbl, g_trapVtbl, sizeof(arrVtbl));
            arrVtbl[1] = arrVtbl[2] = reinterpret_cast<void*>(+[](void*, std::uint64_t, std::uint64_t, std::uint64_t) -> std::uint64_t
            { std::uint8_t* blk = new std::uint8_t[0x40](); *reinterpret_cast<void**>(blk + 8) = new std::uint8_t[0x400](); return reinterpret_cast<std::uint64_t>(blk); });
            put<void*>(arrReg, 0, arrVtbl);
            static void* routeBlk[8];
            routeBlk[0] = &g_routeObj; for (int i = 1; i < 8; ++i) routeBlk[i] = trap(("ctx+0xa8 +0x" + std::to_string(8 * i)).c_str(), 1);
            routeBlk[2] = routeBlk[3] = arrReg;   // +0x10 and +0x18 registries are read by the service slots 0x1410738e0 and 0x141073c80
            put<void*>(ctxSlot, 0xa8, routeBlk);
            // Fields the SsdNpc binds to (0x141077590): ctx +0x18, +0x40 (clock), +0x48, +0x60 (message hub, GekkoLife::Prepare), +0x78 (clock singleton proxy), +0x98 (hub), +0xa8. Survive's boot makes
            // +0x18, +0x40, +0x48 and +0x78 with the static initialisers and setters below (READ in the parked registry work, MERGE_NOTES round 7); +0x48 is the result of 0x1400fc230.
            for (std::uint64_t init : { 0x141fc1f20ull, 0x141f2fad0ull, 0x141f2faa0ull, 0x141f2e370ull, 0x141f40740ull, 0x141f40770ull, 0x141f407a0ull })
                if (!CallLifted(init, 0, 0, 0, 0, "ctx object static init")) Trace("[init] ctx object %llx: %s", static_cast<unsigned long long>(init), g_fault);
            for (std::uint64_t setter : { 0x14196e700ull, 0x1400fc5e0ull, 0x140339a90ull, 0x141bcd410ull })
                if (!CallLifted(setter, 0, 0, 0, 0, "ctx entry setter")) Trace("[init] ctx setter %llx: %s", static_cast<unsigned long long>(setter), g_fault);
            if (CallLifted(0x1400fc230ull, 0, 0, 0, 0, "ctx+0x48 object")) put<std::uint64_t>(ctxSlot, 0x48, g_lastRax);
            // Clock stand-ins (see ClockSeconds): ctx+0x40 and the ctx+0x78 chain.
            {
                static void* clockVtbl[256]; static std::uint8_t clockObj[0x20];
                std::memcpy(clockVtbl, g_trapVtbl, sizeof(clockVtbl));
                clockVtbl[2] = reinterpret_cast<void*>(&ClockSeconds);
                put<void*>(clockObj, 0, clockVtbl);
                put<void*>(ctxSlot, 0x40, clockObj);
                static void* pVtbl[256]; static void* aVtbl[256]; static void* bVtbl[256]; static std::uint8_t pObj[0x20], aObj[0x20], bObj[0x20];
                std::memcpy(pVtbl, g_trapVtbl, sizeof(pVtbl)); std::memcpy(aVtbl, g_trapVtbl, sizeof(aVtbl)); std::memcpy(bVtbl, g_trapVtbl, sizeof(bVtbl));
                pVtbl[8] = reinterpret_cast<void*>(&ClockChainA); aVtbl[0] = reinterpret_cast<void*>(&ClockChainB); bVtbl[0] = reinterpret_cast<void*>(&ClockChainByte);
                put<void*>(pObj, 0, pVtbl); put<void*>(aObj, 0, aVtbl); put<void*>(bObj, 0, bVtbl);
                g_clkA = aObj; g_clkB = bObj;
                put<void*>(ctxSlot, 0x78, pObj);
            }
            for (int o : { 0x18, 0x40, 0x48, 0x78 }) Trace("[ctx] +%02x = %llx", o, static_cast<unsigned long long>(*reinterpret_cast<std::uint64_t*>(ctxSlot + o)));
            // ctx+0x80 -> object whose +8 is Survive's geo query service (0x140f88900, 0x140fa1ea4): vtable +0x18 and +0x20 take (control, flags, from, to, radius) and return the hit count,
            // the same arguments as TPP's CollisionLineCheck (0x141b9b130, by GekkoPushProbe) and the same GeoCheckControl layout (Survive ctor 0x14062e480 = TPP 0x14045beb0, capacity
            // 1). Both slots go to TPP's check; +0x20 is the variant without result details in Survive, a superset here.
            static void* geoVtbl[256]; static std::uint8_t geoSvc[0x20], geoHolder[0x20];
            std::memcpy(geoVtbl, g_trapVtbl, sizeof(geoVtbl));
            geoVtbl[3] = geoVtbl[4] = reinterpret_cast<void*>(+[](void*, void* control, std::uint32_t flags, const void* from, const void* to, float radius) -> int
            {
                return g_hooks.lineCheck ? g_hooks.lineCheck(control, flags, from, to, radius) : 0;
            });
            put<void*>(geoSvc, 0, geoVtbl);
            put<void*>(geoHolder, 8, geoSvc);
            put<void*>(geoHolder, 0x10, SurvivePool());   // the same block holds the shared-object pool the record allocation uses ([[G+0x80]+0x10])
            put<void*>(ctxSlot, 0x80, geoHolder);
            g_scopeT = ctxSlot;
            NavProxy::FillScope(ctxSlot);
            put<void*>(reinterpret_cast<std::uint8_t*>(scopeAddr), 8, ctxSlot);
            GekkoNodes::Register(reinterpret_cast<std::uint8_t*>(scopeAddr));
            GekkoLife::Prepare(ctxSlot, g_svc, g_qiOwner);   // after the context getter answers: the calculator constructor calls it
        }   // 0x141989050 returns [ctx+8]; the caller loads the context object from it
        return true;
    }
    bool BuildObserver()
    {
        std::uint64_t obs = 0;
        if (!CallLifted(0x140f72e40ull, 0, 0, 0, 0, "observer factory") || !g_lastRax) { Trace("[init] observer factory: %s", g_fault); return false; }
        obs = g_lastRax;
        g_obs = reinterpret_cast<std::uint8_t*>(obs);
        put<void*>(g_obs, 0x10, g_qiOwner);   // the Observer's own shared handle: the container and its children query it for the Observer interface (id 0xdc11dba59fbb)
        static void* desc[4];
        desc[0] = g_qiOwner; desc[1] = g_qiOwner;
        if (!CallLifted(0x140f74080ull, obs, reinterpret_cast<std::uint64_t>(desc), 0, 0, "observer setup")) { Trace("[init] observer setup: %s", g_fault); return false; }
        put<void*>(M, 0x60, g_obs + 0x30);   // the movement brain's [param_1+0x60] (init stores QI(0xdc11dba59fbb) - 8 at P+0x80, update's frame is P+0x20)
        Trace("[init] observer %p set up", g_obs);
        return true;
    }

    void Init(const Hooks& hooks)
    {
        if (g_ready) return;
        g_hooks = hooks;
        Build();
        for (auto& v : g_evVtbl) v = reinterpret_cast<void*>(&EvHash);
        lift::g_fatal_log = &FatalLog;
        lift::g_native_gate = &NativeGate;
        gekko_module_register();
        g_ready = true;
        // Survive fills its handler tables at start-up; the embedded copies are zero until that initialiser runs (docs/DOCUMENTATION.md).
        for (std::uint64_t init : { 0x141f7e070ull, 0x141f7dbd0ull, 0x141f7de50ull, 0x141f7d660ull })   // action table, AI state table, second layer table, locomotion state table
            if (!CallLifted(init, 0, 0, 0, 0, "table initialiser")) Trace("[init] %s", g_fault);
        BuildContext();
    }
    // The unit's lifted state as the Observer setup and the locomotion init leave it: a start after a stop or a fault puts it back (ResetUnit), so every start begins like the first one.
    // The record's pooled objects (+0x180 to +0x1af) are kept: they are allocated once per process (InitInstance).
    struct UnitSnap { bool have; std::uint8_t rec[0x2c0], ai[0x80], ent[0x130], loco[0x50], ac[0x1d0], req[0xc0]; } g_snap[8];
    void TakeSnap(int id)
    {
        if (!g_ac) return;
        UnitSnap& u = g_snap[id];
        std::memcpy(u.rec, rec[id], sizeof(u.rec)); std::memcpy(u.ai, g_ai[id], sizeof(u.ai)); std::memcpy(u.ent, g_npcEntries[id], sizeof(u.ent)); std::memcpy(u.loco, g_locoRec[id], sizeof(u.loco));
        std::memcpy(u.ac, AcRecord(id), sizeof(u.ac)); std::memcpy(u.req, AcRequest(id), sizeof(u.req));
        u.have = true;
    }
    void ResetUnit(int id)
    {
        if (id < 0 || id > 7 || !g_snap[id].have || !g_ac) return;
        const UnitSnap& u = g_snap[id];
        std::memcpy(rec[id] + 0x1c0, u.rec + 0x1c0, sizeof(u.rec) - 0x1c0); std::memcpy(g_ai[id], u.ai, sizeof(u.ai)); std::memcpy(g_npcEntries[id], u.ent, sizeof(u.ent)); std::memcpy(g_locoRec[id], u.loco, sizeof(u.loco));
        std::memcpy(AcRecord(id), u.ac, sizeof(u.ac)); std::memcpy(AcRequest(id), u.req, sizeof(u.req));
        Trace("[init] unit %d state reset to the start state", id);
    }
    // The Observer setup and with it the ActionController creation. Separate from Init because the ActionController caches the nav service ([svc+0x60], Setup 0x140e794e0), so a nav controller that
    // replaces the straight-line stand-in (StartNavController) must be started first.
    // Switch to Survive's real SsdNpc object (08/10/2026): its service object (instance + 0x20) replaces the host service buffer for every holder of the pointer. Default off until the
    // headless suite passes with it; the fields only the host buffer filled (CompareSvc) are then created by the real object's own service slots or trap and are logged.
    bool g_realNpc = getenv("GEKKO_REALNPC") != nullptr;
    void SetRealNpc(bool on) { g_realNpc = on; }
    bool UseRealNpc()
    {
        // The descriptor qwords +0x38, +0x40, +0x48 of the NPC creator (0x140b6cd4f..0x140b6cd68) are the results of three accessors; Survive's own functions answer them.
        // +0x38 (accessor 0x140b6fbe0) is Survive's allocator object (the Observer calls its vtable slot 19 as alloc(kind 5, bytes) at 0x140f74757): the host answers with zeroed memory (service stand-in, docs 5.16).
        static void* allocVtbl[256]; static std::uint8_t allocObj[0x20];
        std::memcpy(allocVtbl, g_trapVtbl, sizeof(allocVtbl));
        allocVtbl[19] = reinterpret_cast<void*>(+[](void*, std::uint64_t, std::uint64_t bytes, std::uint64_t) -> std::uint64_t { return reinterpret_cast<std::uint64_t>(new std::uint8_t[bytes ? bytes : 1]()); });
        put<void*>(allocObj, 0, allocVtbl);
        std::uint64_t mgr[3] = { reinterpret_cast<std::uint64_t>(allocObj), 0, 0 };
        { int i = 1; for (std::uint64_t acc : { 0x140c0fea0ull, 0x140c10900ull }) { if (!CallLifted(acc, 0, 0, 0, 0, "npc manager accessor")) Trace("[realnpc] accessor %llx: %s", (unsigned long long)acc, g_fault); mgr[i++] = g_lastRax; } }
        std::uint8_t* inst = GekkoNodes::CreateSsdNpc(g_qiOwner, GekkoLife::ParamHolder(), mgr);
        if (!inst) { Trace("[realnpc] SsdNpc creation failed: %s", g_fault); return false; }
        std::uint8_t* rs = inst + 0x20;   // the service interface is the second base (vtable 0x40 after the object vtable, 149 slots); Setup works on the object pointer, so its inst+0x140 is service +0x120
        // The life class slot (0x141073230) hands [svc-0x10] on as the owner the Observer later queries for the LifeController (0x2fbbd3df1420); the SsdNpc answers only its own ids, so the host container handle (the one
        // every other component of the unit queries through) takes the place of the creation handle, as in the host buffer. Stand-in, listed in docs 5.16.
        put<void*>(rs, -0x10, g_qiOwner);
        // No list of units with pending work, so the ActionController update takes its all-units branch (0x140e7a268..0x140e7a285), as with the host buffer: the pending list is filled by Survive's per-frame unit
        // service, which does not run here yet. Stand-in, docs 5.16.
        put<std::uint32_t>(rs, 0x30, 0); put<void*>(rs, 0x38, nullptr);
        { std::uint8_t* bits = *reinterpret_cast<std::uint8_t**>(*reinterpret_cast<std::uint8_t**>(rs + 0x48)); if (bits) *bits |= 1; }   // unit 0 alive in the unit bitmap [[svc+0x48]]: the NPC manager's registration, not run here (stand-in, docs 5.16)
        { std::uint8_t* o = *reinterpret_cast<std::uint8_t**>(rs + 0x48); std::uint16_t* idx = o ? *reinterpret_cast<std::uint16_t**>(o + 0x28) : nullptr;
          Trace("[realnpc] unit index table %p, unit 0 entry %04x", static_cast<void*>(idx), idx ? idx[0] : 0u);
          if (idx) idx[0] = 0; }   // the NPC manager's registration gives unit 0 the first row of the per-unit tables (the push code indexes them with it, 0x140f822b1); stand-in, docs 5.16a
        put<void*>(rs, 0x190, GekkoLife::UnitStore());   // the unit registry the position and record queries go to (GekkoLife)
        g_svc = rs;
        put<void*>(S, 0x1b0, rs); put<void*>(L, 0x78, rs); put<void*>(M, 0x58, rs); put<void*>(H, 0x1a0, rs);
        g_meleeCtx.svc = rs;
        // The event bus: the NPC manager hands it to the service through vtable slot 63 (0x14107f6b0, stores [svc+0x108]); the observer sends its events through vtable +0x88(id, hash, data) (0x140f936a7..0x140f936b9).
        // TPP side: a logging trap object until the sends are routed to TPP's message system.
        if (!CallLifted(0x14107f6b0ull, reinterpret_cast<std::uint64_t>(rs), reinterpret_cast<std::uint64_t>(trap("npcService->+0x108 (event bus: vtable +0x88(id, hash, data))")), 0, 0, "set event bus")) Trace("[realnpc] set event bus: %s", g_fault);
        Trace("[realnpc] service switched to the SsdNpc instance %p", static_cast<void*>(inst));
        return true;
    }
    bool Begin()
    {
        static bool done = false;
        if (!done)
        {
            done = true;
            if (g_realNpc && !UseRealNpc()) return false;
            // The locomotion component's own per-unit init (slot 1 of its interface, 0x140f8ac40): takes the unit's navigator record from the nav service into the locomotion record (+0x20, what the path
            // order branch of the arrival test reads) and requests the idle state.
            const bool obsOk = BuildObserver();
            if (obsOk && g_realNpc)   // the real service slot 126 stored the ActionController at [svc+0x80]; the consumers cache it as AcSlot does for the host buffer
            {
                GekkoLife::AdoptService(g_svc);
                g_ac = *reinterpret_cast<std::uint8_t**>(g_svc + 0x80);
                if (g_ac) { put<void*>(H, 0x1c8, g_ac); put<void*>(L, 0x88, g_ac); }
                Trace("[life] real service: +0x68 %p +0xa8 %p", *reinterpret_cast<void**>(g_svc + 0x68), *reinterpret_cast<void**>(g_svc + 0xa8));
                Trace("[ac] ActionController %p (real service), svc+0x60 (nav) %p", static_cast<void*>(g_ac), *reinterpret_cast<void**>(g_svc + 0x60));
            }
            if (obsOk && !CallLifted(0x140f8ac40ull, reinterpret_cast<std::uint64_t>(L + 0x20), 0, 0, 0, "locomotion init")) Trace("[init] locomotion init: %s", g_fault);
            TakeSnap(0);
        }
        return g_ac != nullptr;
    }
    void AllowNative(std::uint64_t addr) { g_dynAllowed.insert(addr); }
    void* LiftedPool() { return SurvivePool(); }
    void* ControllerObject() { static void* obj[2]; obj[0] = g_ctlVtbl; return obj; }
    void* ObserverIface() { return g_obs ? g_obs + 0x30 : nullptr; }
    void* SiblingComponent() { std::uint8_t* lc = GekkoLife::ControllerIface(); return lc ? lc : SiblingBlock(); }
    void* NpcService() { return g_svc; }
    void* NpcEntryTable() { return g_npcEntries; }
    void* TrapObject(const char* name, int depth) { return trap(name, depth); }
    // TPP's target service pool, [[manager+0x80]+0x10]: the object TPP's own PushSlide code (TppSmartAnimalPushSlideTarget, Player2/Soldier2 action cores) creates targets with. Null when unreadable.
    void* RealTargetPool()
    {
        __try
        {
            std::uint8_t* mgr = reinterpret_cast<std::uint8_t* (*)()>(0x140bff050ull)();
            std::uint8_t* blk = mgr ? *reinterpret_cast<std::uint8_t**>(mgr + 0x80) : nullptr;
            return blk ? *reinterpret_cast<void**>(blk + 0x10) : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    void SetController(void* ctrl)
    {
        g_fwdCtrl = ctrl;
        if (ctrl) UseRealController();
    }
    void NotifyControllerRan() { ++g_ctlRuns; }
    void SetMotionFinished(bool f) { g_finished = f ? 1 : 0; }
    void SetDt(float seconds) { for (float& d : g_dt) d = seconds; }
    void SeedRandom(std::uint32_t state) { if (g_svc) *reinterpret_cast<std::uint32_t*>(g_svc + 0x1c) = state; }
    void NameTrap(void* object, const char* name) { names()[object] = name; }
    const char* LastFault() { return g_fault; }
    void Log(const char* line) { if (g_hooks.trace) g_hooks.trace(line); }

    // The host's own request: what an AI state builds (0x140f8d90c..0x140f8d97c: zeroed 0xc0 bytes, +0x10 target, +0x20 action, +0x24 flag byte, +0xb9 = 4 for an action) goes through Survive's
    // mailbox 0x140e96b60 into the ActionController; the next ActionController update commits it (ActionUpdate).
    bool SendRequest(int id, std::uint8_t* req)
    {
        if (!g_ac) { std::snprintf(g_fault, sizeof(g_fault), "the ActionController was not created (Begin)"); return false; }
        return CallLifted(0x140e96b60ull, reinterpret_cast<std::uint64_t>(g_ac), static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(req), 0, "mailbox");
    }
    void BuildRequest(std::uint8_t* r, unsigned action, unsigned flag, const float* target)
    {
        std::memset(r, 0, 0xc0);
        std::memcpy(r + 0x10, target, 12);
        *reinterpret_cast<std::uint32_t*>(r + 0x1c) = 1;   // target descriptor: low 3 bits of the 4th dword = type, 1 = plain position (0x140f7f240)
        *reinterpret_cast<std::uint32_t*>(r + 0x20) = action;
        r[0x24] = static_cast<std::uint8_t>(flag);
        r[0xb9] = 4;
    }
    void Request(int id, unsigned action, unsigned flag, const float* target)
    {
        if (id < 0 || id > 7) return;
        Trace("[request] id %d action %u flag %u target %.2f %.2f %.2f", id, action, flag, target[0], target[1], target[2]);
        alignas(16) std::uint8_t r[0xc0];
        BuildRequest(r, action, flag, target);
        SendRequest(id, r);
    }

    // A request whose target is a unit, as the brain's arrival request carries it (log 06/10/2026 20:16:32: kind 2, selector 0): descriptor dword +0 = unit id (low 16 bits) | selector << 16, word +4 = which
    // position slot of the unit registry answers (0, 1, 2), kind byte +0xc = 2 (0x140f7f240).
    void RequestUnitTarget(int id, unsigned action, unsigned flag, unsigned unitId, unsigned slot)
    {
        if (id < 0 || id > 7) return;
        static const float none[3] = {};
        alignas(16) std::uint8_t r[0xc0];
        BuildRequest(r, action, flag, none);
        std::memset(r + 0x10, 0, 0x10);
        *reinterpret_cast<std::uint32_t*>(r + 0x10) = unitId & 0xffff;
        *reinterpret_cast<std::uint16_t*>(r + 0x14) = static_cast<std::uint16_t>(slot);
        *reinterpret_cast<std::uint32_t*>(r + 0x1c) = 2;
        SendRequest(id, r);
    }

    // The ActionController update (interface slot 9, 0x140e7a0f0): commits the requests the mailbox queued (slot 15 builds the request array entry, a position order asks the nav service for a path
    // and the gekko component turns the path into a path order, 0x140f76a20), and marks finished actions in the record flags.
    void* ActionControllerIface() { return g_ac; }
    bool ActionUpdate()
    {
        if (!g_ac) return false;
        return CallLifted(0x140e7a0f0ull, reinterpret_cast<std::uint64_t>(g_ac), 0, 0, 0, "action controller update");
    }

    void SetTarget(float x, float y, float z)
    {
        g_targetObj[8] = x; g_targetObj[9] = y; g_targetObj[10] = z;
        *reinterpret_cast<std::uint32_t*>(&g_targetObj[11]) = 1;
    }

    void SetRouteSystem(void* routeSystem, const void* groupVtbl)
    {
        g_routeSys = routeSystem;
        g_routeGroupVtbl = groupVtbl;
        Trace("[route] route system %p (group vtable %p)", routeSystem, groupVtbl);
    }

    // What Survive's order initialiser 0x140fa6910 writes for the route (0x140fa6974..0x140fa69b7): route id at +0xb0, vertex word +0xb4 = 0, generation cache +0xac = -1, mode word +0xb8 = 0.
    // The path handle word +0xb6 is left alone, so the brain releases the running path itself (0x140fa4dc4). The initialiser takes the id from its host (slot 6 of [component-0x20]); here the caller gives it.
    void SetRoute(int id, std::uint32_t routeId, unsigned vertex)
    {
        if (id != 0) return;   // the order record is single-unit (g_order), as the rest of the shim
        *reinterpret_cast<std::uint32_t*>(g_order + 0xb0) = routeId;
        g_loopRoute = routeId; g_sinceRestart = 99;
        *reinterpret_cast<std::uint16_t*>(g_order + 0xb4) = static_cast<std::uint16_t>(vertex);
        *reinterpret_cast<std::uint32_t*>(g_order + 0xac) = 0xffffffffu;
        *reinterpret_cast<std::uint16_t*>(g_order + 0xb8) = 0;
        Trace("[route] unit %d route %08x from vertex %u", id, routeId, vertex);
    }

    // Route loop: TPP soldiers repeat their route, so when the brain reports the end the same route starts again from vertex 0. Infinite Heaven randomises which route a soldier
    // gets; which route the gekko walks stays with the Lua side (Gekko.AiRoute). A route that ends again within 2 frames of its restart has no vertices: the loop stops instead of spinning.
    void StopRouteLoop() { g_loopRoute = 0xbf169f98u; }
    void RouteLoopStep()
    {
        if (!g_routeEnded) { ++g_sinceRestart; return; }
        g_routeEnded = false;
        if (g_loopRoute == 0xbf169f98u) return;
        if (g_sinceRestart < 2) { Trace("[route] route %08x ends at once: loop stopped", g_loopRoute); g_loopRoute = 0xbf169f98u; return; }
        Trace("[route] route %08x ended: looping from vertex 0", g_loopRoute);
        const std::uint32_t r = g_loopRoute;
        SetRoute(0, r, 0);
        g_sinceRestart = 0;
    }

    void SetPose(int id, float x, float y, float z, float yaw)
    {
        if (id < 0 || id > 7 || !g_pose) return;
        g_pose[id][0] = x; g_pose[id][1] = y; g_pose[id][2] = z; g_pose[id][3] = yaw;
        // Headless: the shim motion output carries what the real one does, the unit position at +0x20 and its rotation at +0x60 (0x140f88d15: the heading to the target is computed from
        // the motion output's position; in the game: with zeros there the gekko steered toward the direction from the world origin).
        if (!g_fwdCtrl)
        {
            float* po = reinterpret_cast<float*>(g_motionOut + 0x20);
            po[0] = x; po[1] = y; po[2] = z; po[3] = 1.0f;
            float* q = reinterpret_cast<float*>(g_motionOut + 0x60);
            q[0] = 0.0f; q[1] = std::sin(yaw * 0.5f); q[2] = 0.0f; q[3] = std::cos(yaw * 0.5f);
        }
        // No state bits here: bits 8 / 9 of the NPC entry +0x110 (path tracker 0x140f7f7f0) come only from the walk loop's clip events 3903ca38 / b9817d52 through the plugin mapper
        // 0x140f81220 (FireEvent), and the plugin frame (kind 0, 0x140f7ee21..0x140f7eee5, GekkoPush::RunKind) clears the entry each frame. The 3.0 m stand-in that was here is removed.
    }

    bool MeleeChoose(int id)
    {
        alignas(16) std::uint8_t out[32] = {};
        return CallLifted(0x140f911f0ull, reinterpret_cast<std::uint64_t>(&g_meleeCtx), static_cast<std::uint64_t>(id), 0, reinterpret_cast<std::uint64_t>(out), "melee chooser");
    }

    bool AiStart(int id)
    {
        if (id < 0 || id > 7) return false;
        g_ai[id][0x7c] = 0;
        std::uint64_t evctx = 0;   // r8 points at a qword handed on to the state handlers as r9
        return CallLifted(0x140f8d8d0ull, reinterpret_cast<std::uint64_t>(S), static_cast<std::uint64_t>(id), 0, evctx, "ai state 0 enter");
    }

    // Debug transition (GekkoDebug::State): what the state machine 0x140f8d3f0 does when the state byte changes after an update: exit handler of the old state (mode 1),
    // timer (+0x74) = 0, enter handler of the new state (mode 0). Handlers are the 13 entries of the AI state table at 0x143086260, 16 bytes each {fn, this adjust}; the
    // adjust is 0 in all of them. initialiser 0x141f7dbd0 (leas decoded from the disassembly), machine calls at 0x140f8d476/0x140f8d4aa/0x140f8d4da with
    // rcx = S + adjust, edx = id, r8d = mode, r9 = the qword [r8] of the caller, fifth argument = 0 for exit/enter (the update passes a flag pointer).
    bool AiTransition(int id, unsigned n, int* before)
    {
        static const std::uint64_t kHandler[13] = { 0x140f8d8d0ull, 0x140f8da30ull, 0x140f8db50ull, 0x140f8dc90ull, 0x140f8e620ull, 0x140f8e410ull, 0x140f8e520ull,
                                                    0x140f8df50ull, 0x140f8e120ull, 0x140f8eb10ull, 0x140f8ddd0ull, 0x140f8e220ull, 0x1402ba870ull };
        if (id < 0 || id > 7 || n > 12) return false;
        const unsigned old = g_ai[id][0x7c];
        if (before) *before = static_cast<int>(old);
        if (old > 12) return false;
        const std::uint64_t s = reinterpret_cast<std::uint64_t>(S);
        if (!CallAddrX(kHandler[old], s, static_cast<std::uint64_t>(id), 1, 0, 0.0f, 0, nullptr)) return false;
        g_ai[id][0x7c] = static_cast<std::uint8_t>(n);
        *reinterpret_cast<std::uint32_t*>(g_ai[id] + 0x74) = 0;
        return CallAddrX(kHandler[n], s, static_cast<std::uint64_t>(id), 0, 0, 0.0f, 0, nullptr);
    }

    bool AiUpdate(int id)
    {
        static std::uint64_t evctx = 0;
        return CallLifted(0x140f8d3f0ull, reinterpret_cast<std::uint64_t>(S + 0x20), static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(&evctx), 0, "ai update");
    }

    // Row fields the movement brain reads at its entry (disk 0x140fa475f..0x140fa4798): +0x30 target id (-1 none), +0x34 strength (above 0 = live target), +0x68 byte, +0x6a byte. A non-zero +0x6a
    // with a route in the order record makes the brain end the route at once (0x140fa47d5..: report to [svc+0x78] vtable +0x60, route id back to none) and skip modes 1 to 3 for that frame:
    // the host raises it for one frame when a target takes over from a route walk (GekkoAi::SetTarget). +0x68 is never set (meaning not decoded).
    void SetTargetState(bool has, float strength, bool interrupt)
    {
        std::uint8_t* t = reinterpret_cast<std::uint8_t*>(g_targetObj);
        *reinterpret_cast<std::int32_t*>(t + 0x30) = has ? 0 : -1;
        *reinterpret_cast<float*>(t + 0x34) = strength;
        t[0x6a] = interrupt ? 1 : 0;
    }

    std::uint8_t* Order() { return g_order; }
    std::uint8_t* TargetObj() { return reinterpret_cast<std::uint8_t*>(g_targetObj); }

    bool MoveUpdate(int id)
    {
        return CallLifted(0x140fa46d0ull, reinterpret_cast<std::uint64_t>(M), static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(&g_orderPtr), 0, "movement brain");
    }

    void LocoTarget(int id, float x, float y, float z)
    {
        float* v = reinterpret_cast<float*>(g_locoRec[id] + 0x10);
        v[0] = x; v[1] = y; v[2] = z; v[3] = 0;
    }

    bool LocoRun(int event, int id)
    {
        alignas(16) std::uint8_t out[64] = {};
        char what[32];
        std::snprintf(what, sizeof(what), "locomotion event %d", event);
        return CallLifted(0x140f89c10ull, reinterpret_cast<std::uint64_t>(L), static_cast<std::uint64_t>(id), static_cast<std::uint64_t>(event), reinterpret_cast<std::uint64_t>(out), what);
    }

    // The component base class tick 0x141092020 on the locomotion component (sub-object L+0x20): kind 1 runs the pending state change (exit, enter) and the update of the
    // current state, kind 3 and 5 the late events. The state byte of an instance is in its controller record (+0xc current, +0xe requested, +0xf bit 0 force).
    bool LocoTick(int kind, int id)
    {
        return CallLifted(0x141092020ull, reinterpret_cast<std::uint64_t>(L + 0x20), static_cast<std::uint64_t>(kind), static_cast<std::uint64_t>(id), 0, "locomotion tick");
    }
    // Mode 7 of the current locomotion state through the wrapper in slot 20 of the locomotion component (0x141091f70, the same shape as the Realized hub's 0x140f93c80): the engine calls it every frame to
    // get the step the unit moves by. The turn state's mode 7 (0x140f89260) clears the heading request first, which is how Survive ends a heading request while a turn runs; the base ticks 1, 3 and 5
    // never reach mode 7. The inputs (two vectors, which Survive's caller is not identified for) are zero and the step in out+0x10 and out+0x20 is dropped: the body is moved by the clip's root motion.
    // The wrapper writes the translation step into out+0x10 and sets bit 0 of out+0x48 (0x140f93c80's shape, 0x141091f70). With the root motion input zero the walk state's step (event 7,
    // 0x140f8a67b..0x140f8ab93) is its path correction alone: the unit's offset from the path point at the progress, capped at 1 m/s (2 m/s when record byte +0x46 is 1) times the frame time,
    // when the sideways distance from the current segment exceeds 0.05 and the distance to the progress point 0.01, turned into the unit's local frame by the inverse of its rotation
    // (motion out +0x60). TakeStep hands it out; the body is moved by it only when GekkoAi applies it (GekkoNavStep).
    alignas(16) std::uint8_t g_stepOut[0x60];
    bool MoveStep(int id)
    {
        std::memset(g_stepOut, 0, sizeof(g_stepOut));
        alignas(16) float a[4] = {}, b[4] = {};
        const std::uint64_t st[1] = { static_cast<std::uint64_t>(id) };
        return CallLiftedS(0x141091f70ull, reinterpret_cast<std::uint64_t>(L + 0x40), reinterpret_cast<std::uint64_t>(g_stepOut), reinterpret_cast<std::uint64_t>(a), reinterpret_cast<std::uint64_t>(b), st, 1, "move step");
    }
    bool TakeStep(float* v)
    {
        if (!(*reinterpret_cast<const std::uint32_t*>(g_stepOut + 0x48) & 1)) return false;
        std::memcpy(v, g_stepOut + 0x10, 3 * sizeof(float));
        return true;
    }
    void LocoState(int id, unsigned state)
    {
        std::uint8_t* r = reinterpret_cast<std::uint8_t*>(g_locoCtl[id]);
        r[0xe] = static_cast<std::uint8_t>(state);
        r[0xf] |= 1;
    }
    // Test aid for the switch to the real SsdNpc: per qword offset of the service, the host buffer value against the real instance's value (marks what only the host fills).
    void CompareSvc(const std::uint8_t* realSvc)
    {
        if (!g_svc || !realSvc) return;
        for (int off = -0x10; off < 0x230; off += 8)
        {
            const std::uint64_t h = *reinterpret_cast<const std::uint64_t*>(g_svc + off), r = *reinterpret_cast<const std::uint64_t*>(realSvc + off);
            if (h || r) std::printf("  [svc%+#x] host %016llx real %016llx%s\n", off, (unsigned long long)h, (unsigned long long)r, (h && !r) ? "  HOST-ONLY" : (!h && r) ? "  real-only" : "");
        }
    }

    void DumpLoco(int id)
    {
        if (!g_ac) return;
        const std::uint8_t* q = AcRequest(id);
        std::printf("  [dump] request: b4 %02x%02x b6 %02x b8 %02x b9 %02x pos %.2f %.2f %.2f ae %04x\n", q[0xb5], q[0xb4], q[0xb6], q[0xb8], q[0xb9], *reinterpret_cast<const float*>(q + 0x90), *reinterpret_cast<const float*>(q + 0x94), *reinterpret_cast<const float*>(q + 0x98), *reinterpret_cast<const std::uint16_t*>(q + 0xae));
        const std::uint8_t* c = reinterpret_cast<const std::uint8_t*>(g_locoCtl[id]);
        std::printf("  [dump] loco ctl: idx %u cur %u next %u flag %02x; rec: +0x20 %p +0x28 %x +0x2c %.2f +0x34 %x +0x38 %x +0x46 %u\n", *reinterpret_cast<const std::uint32_t*>(c + 8), c[0xc], c[0xe], c[0xf],
                    *reinterpret_cast<void* const*>(g_locoRec[id] + 0x20), *reinterpret_cast<const std::uint32_t*>(g_locoRec[id] + 0x28), *reinterpret_cast<const float*>(g_locoRec[id] + 0x2c),
                    *reinterpret_cast<const std::uint16_t*>(g_locoRec[id] + 0x34), *reinterpret_cast<const std::uint32_t*>(g_locoRec[id] + 0x38), g_locoRec[id][0x46]);
    }
    unsigned LocoStateNow(int id) { return reinterpret_cast<std::uint8_t*>(g_locoCtl[id])[0xc]; }

    // One line of everything the walk waits on (brain mode and order, request entry, action record, locomotion record, event bits, heading request, last clip and finished answer).
    void StallLine(int id, char* out, std::size_t n)
    {
        const std::uint8_t* o = g_orderPtr;
        const std::uint8_t* q = g_ac ? AcRequest(id) : g_zero;
        const std::uint8_t* c = reinterpret_cast<const std::uint8_t*>(g_locoCtl[id]);
        const std::uint8_t* lr = g_locoRec[id];
        float yaw = 0, nn = 0;
        const bool steer = TakeSteer(&yaw, &nn);
        std::snprintf(out, n, "[stall] brain mode %u prev %u t94 %.2f t9c %.2f flags ba %u bb %u bc %u | entry b4 %04x b6 %02x ac %u ae %u | ctlrec %04x | action %u pending %u rec298 %u | loco cur %u next %u force %u t2c %.2f flags38 %x ser34 %u byte46 %u | npc110 %08x | steer %d yaw %.3f N %.0f | clip %llx finished %d",
                      o[0xb8], o[0xb9], *reinterpret_cast<const float*>(o + 0x94), *reinterpret_cast<const float*>(o + 0x9c), o[0xba], o[0xbb], o[0xbc],
                      *reinterpret_cast<const std::uint16_t*>(q + 0xb4), q[0xb6], *reinterpret_cast<const std::uint16_t*>(q + 0xac), *reinterpret_cast<const std::uint16_t*>(q + 0xae),
                      g_ac ? *reinterpret_cast<const std::uint16_t*>(AcRecord(id) + 0x1c4) : 0u, rec[id][0x29c], rec[id][0x29d], *reinterpret_cast<const std::uint16_t*>(rec[id] + 0x298),
                      c[0xc], c[0xe], c[0xf] & 1, *reinterpret_cast<const float*>(lr + 0x2c), *reinterpret_cast<const std::uint32_t*>(lr + 0x38), *reinterpret_cast<const std::uint16_t*>(lr + 0x34), lr[0x46],
                      *reinterpret_cast<const std::uint32_t*>(g_npcEntries[id] + 0x110), steer ? 1 : 0, yaw, nn, static_cast<unsigned long long>(g_setGani), g_lastFinished);
    }

    // The controller's motion output: in the game the real object ([unit+0x60]+0x10, Survive slot 123 = TPP slot 112; it holds the unit's position at +0x20 and rotation at +0x60, which the lifted
    // locomotion reads), headless the shim struct.
    std::uint8_t* MotionOut() { return g_fwdCtrl ? reinterpret_cast<std::uint8_t* (*)(void*, int)>(fwd_c123)(nullptr, 0) : g_motionOut; }
    const std::uint8_t* RequestBytes(int id) { return g_ac ? AcRequest(id & 7) : nullptr; }
    // The heading request the locomotion writes into the controller's motion output (0x140f8a36a..0x140f8a37d, 0x140f8a608..0x140f8a61b): target orientation quaternion at +0x80, flag 0x100 at +0x190
    // and N at +0x1a8 (30 and 90 seen). In the game the motion output is TPP's CharaControlInterface (cc + 0x10), whose layout differs from Survive's: +0x80 is TPP's turn target quaternion too, but
    // Survive's flag word +0x190 is cc + 0x1a0 and N +0x1a8 is cc + 0x1b8 (byte flags of the floor check, read by GetFloorNormal), neither of them a turn field. Survive's CharaControl
    // (ControlActImpl 0x1419f17d0: flag test at +0x1a0 bit 8, N at +0x1b8, ticks +0x1b0) and TPP's (0x141940f60: flag +0x188 bit 8, N +0x1ac) run the same turn: while the flag is set the rotation is
    // slerped toward the target by 1 - pow(1 - 1/N, ticks * 0.2) per frame (N <= 1 means 8), so N is a smoothing constant in frames (1/N of the remaining angle per frame at 60 fps), not a rate in degrees per second.
    // The lifted code expects the three fields to persist between frames, so they live in a shadow: SteerBegin puts the shadow into the motion output and keeps TPP's own bytes, SteerEnd takes the
    // lifted code's values back and restores TPP's. TakeSteer reads the shadow.
    struct SteerShadow { std::uint32_t flags = 0; std::uint32_t n = 0; std::uint8_t quat[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x3f }; };
    SteerShadow g_shadow, g_tppBytes;
    void SteerSwap(const SteerShadow& in, SteerShadow* out)
    {
        std::uint8_t* mo = MotionOut();
        if (out) { std::memcpy(&out->flags, mo + 0x190, 4); std::memcpy(&out->n, mo + 0x1a8, 4); std::memcpy(out->quat, mo + 0x80, 16); }
        std::memcpy(mo + 0x190, &in.flags, 4); std::memcpy(mo + 0x1a8, &in.n, 4); std::memcpy(mo + 0x80, in.quat, 16);
    }
    void SteerBegin() { SteerSwap(g_shadow, &g_tppBytes); }
    // Who wrote the heading request last: after the brain frame the shadow is compared with its value before; a change records the locomotion state (record +0xc) the frame ended in and the frame number.
    struct SteerWho { std::uint64_t frame = 0, changes = 0; unsigned state = 0, flagsBefore = 0, flagsAfter = 0; float nAfter = 0; } g_who;
    std::uint64_t g_steerFrames = 0;
    void SteerEnd()
    {
        const SteerShadow before = g_shadow;
        SteerSwap(g_tppBytes, &g_shadow);
        ++g_steerFrames;
        if (before.flags != g_shadow.flags || before.n != g_shadow.n || std::memcmp(before.quat, g_shadow.quat, 16) != 0)
        {
            float nn; std::memcpy(&nn, &g_shadow.n, 4);
            g_who.frame = g_steerFrames; ++g_who.changes; g_who.state = LocoStateNow(0); g_who.flagsBefore = before.flags; g_who.flagsAfter = g_shadow.flags; g_who.nAfter = nn;
        }
    }
    // Part of the [turnlog] line that belongs to the lifted side: current clip, locomotion state and mode, the request in the shadow and its last change.
    void TurnLine(char* out, std::size_t n)
    {
        float yaw = 0, nn = 0;
        const bool on = TakeSteer(&yaw, &nn);
        std::snprintf(out, n, "clip %llx loco %u | req %d target %.3f N %.0f | last change frame %llu (now %llu) in loco state %u flags %x -> %x N %.0f, %llu changes",
                      static_cast<unsigned long long>(g_setGani), LocoStateNow(0), on ? 1 : 0, yaw, nn, static_cast<unsigned long long>(g_who.frame), static_cast<unsigned long long>(g_steerFrames),
                      g_who.state, g_who.flagsBefore, g_who.flagsAfter, g_who.nAfter, static_cast<unsigned long long>(g_who.changes));
    }
    bool TakeSteer(float* yawRad, float* smoothing)
    {
        if (!(g_shadow.flags & 0x100)) return false;
        const float* q = reinterpret_cast<const float*>(g_shadow.quat);
        *yawRad = 2.0f * std::atan2(q[1], q[3]);
        std::memcpy(smoothing, &g_shadow.n, 4);
        return true;
    }
    bool LocoCall(std::uint64_t fn, int event, int id)
    {
        alignas(16) std::uint8_t out[64] = {};
        return CallLifted(fn, reinterpret_cast<std::uint64_t>(L), static_cast<std::uint64_t>(id), static_cast<std::uint64_t>(event), reinterpret_cast<std::uint64_t>(out), "locomotion handler");
    }

    std::uint8_t* NpcEntry(int id)
    {
        static std::uint8_t* base = *reinterpret_cast<std::uint8_t**>(*reinterpret_cast<std::uint8_t**>(*reinterpret_cast<std::uint8_t**>(S + 0x1b0) + 0xf8) + 8);
        return base + id * 0x28;
    }

    // The AI's state chooser 0x140f8d510(S, id): picks the state from the NPC state table entry's flag bits (and three helper predicates). Returns the chosen state.
    int PickState(int id)
    {
        if (!CallLifted(0x140f8d510ull, reinterpret_cast<std::uint64_t>(S), static_cast<std::uint64_t>(id), 0, 0, "ai state chooser")) return -1;
        return g_ai[id][0x7c];
    }

    void SetAiState(int id, unsigned state) { if (id >= 0 && id <= 7) g_ai[id][0x7c] = static_cast<std::uint8_t>(state); }

    // The instance initialiser 0x140fa1340(R, id): takes the controller, copies the first request into the record (+0x29d, the action to enter), sets the state flag
    // 0x1000, registers the motion event callback on the controller and sets the per-unit mode. The first request must have been made with Request().
    bool InitInstance(int id)
    {
        // The record's three target objects and three handles (+0x180 to +0x1a8) come from Survive's lifted pool through Survive's own allocation 0x140f943b0, once per process. Their Init registers
        // them in Survive's name registry, which Survive's own creator builds (BuildContext); the game's first run faulted on that registry being null (survive pc 0x14003aad8).
        if (!AllocBodies(id)) return false;
        if (!ActionUpdate()) return false;   // commits the request the instance initialiser copies
        return CallLifted(0x140fa1340ull, reinterpret_cast<std::uint64_t>(H + 0x20), static_cast<std::uint64_t>(id), 0, 0, "instance initialiser");
    }

    bool AllocBodies(int id)
    {
        static bool done[8];
        if (done[id & 7]) return true;
        done[id & 7] = CallLifted(0x140f943b0ull, reinterpret_cast<std::uint64_t>(H), static_cast<std::uint64_t>(id), 1, 0, "record object allocation");
        return done[id & 7];
    }
    std::uint8_t* RecordBytes(int id) { return rec[id]; }

    void Prime(int id, unsigned action)
    {
        if (id >= 0 && id <= 7) rec[id][0x29d] = static_cast<std::uint8_t>(action);   // what the instance initialiser 0x140fa1340 does with the first request
    }

    bool Dispatch(int kind, int id)
    {
        char what[32];
        std::snprintf(what, sizeof(what), "dispatch kind %d", kind);
        return CallLifted(0x140f93d30ull, reinterpret_cast<std::uint64_t>(H + 0x20), static_cast<std::uint64_t>(kind), static_cast<std::uint64_t>(id), 0, what);
    }

    bool FireEvent(int id, std::uint32_t hash, unsigned param)
    {
        alignas(16) std::uint8_t ev[32] = {};
        *reinterpret_cast<void**>(ev) = g_evVtbl;
        *reinterpret_cast<std::uint32_t*>(ev + 8) = hash;
        *reinterpret_cast<std::uint32_t*>(ev + 12) = param;
        const bool record = CallLifted(0x140f94cb0ull, reinterpret_cast<std::uint64_t>(H), static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(ev), 0, "event");
        // Survive registers two callbacks for the clip events, the action record's (0x140f94c70, registered by 0x140fa1340) and the plugin's (0x140f811e0, registered by 0x140f82a00); both see every event.
        std::memset(g_plugEv, 0, 8);
        *reinterpret_cast<void**>(g_plugEv + 0x180) = g_npcEntries;
        const bool plugin = CallLifted(0x140f81220ull, reinterpret_cast<std::uint64_t>(g_plugEv), static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(ev), 0, "plugin event");
        return record && plugin;
    }

    bool CallAddr(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, std::uint64_t* rax)
    {
        char what[32];
        std::snprintf(what, sizeof(what), "call 0x%llx", static_cast<unsigned long long>(addr));
        const bool ok = CallLifted(addr, rcx, rdx, r8, r9, what);
        if (rax) *rax = g_lastRax;
        return ok;
    }

    bool Run(int action, int event)
    {
        if (action < 0 || action > 31) return false;
        alignas(16) std::uint8_t out[64] = {};
        char what[32];
        std::snprintf(what, sizeof(what), "action %d event %d", action, event);
        return CallLifted(kHandlers[action], reinterpret_cast<std::uint64_t>(H), 0, static_cast<std::uint64_t>(event), reinterpret_cast<std::uint64_t>(out), what);
    }

    State Peek(int id)
    {
        State s{};
        if (id < 0 || id > 7) return s;
        s.action = rec[id][0x29c]; s.pending = rec[id][0x29d];
        s.eventFlags = *reinterpret_cast<std::uint32_t*>(rec[id] + 0x288);
        s.controllerFlags = g_ac ? *reinterpret_cast<std::uint16_t*>(AcRecord(id) + 0x1c4) : 0;
        s.aiState = g_ai[id][0x7c]; s.aiTimer = *reinterpret_cast<float*>(g_ai[id] + 0x74);
        return s;
    }
    void NavTrace(const char* line) { Trace("%s", line); }
    bool NavCallLifted(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9) { return CallAddr(addr, rcx, rdx, r8, r9, nullptr); }
    // Nav proxies (docs/DOCUMENTATION.md). realScope null = TPP's scope table from the accessor 0x140bff050, whose prologue is checked first; the vtable checks are NavProxy's.
    bool StartNav(void* realScope, std::uint64_t sVtbl, std::uint64_t bVtbl)
    {
        NavProxy::Config c;
        c.trace = &NavTrace; c.callLifted = &NavCallLifted;
        if (sVtbl) c.sVtbl = sVtbl;
        if (bVtbl) c.bVtbl = bVtbl;
        c.realScope = realScope;
        if (!c.realScope)
        {
            if (!NavProxy::AccessorOk()) { Trace("[navproxy] accessor 0x140bff050 prologue differs, not started"); return false; }
            c.realScope = reinterpret_cast<void* (*)()>(0x140bff050ull)();
        }
        if (!NavProxy::Init(c)) return false;
        if (g_scopeT) NavProxy::FillScope(g_scopeT);
        return true;
    }
    void NavSync() { NavProxy::Sync(); }

    // Survive's NavigationController2Impl replaces the straight-line stand-in as [svc+0x60].
    bool CallAddrX(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, float xmm3, std::uint64_t arg5, std::uint64_t* rax)
    {
        const lift::LiftFn f = g_ready ? lift::find_fn(addr) : nullptr;
        if (!f) { std::snprintf(g_fault, sizeof(g_fault), "call 0x%llx not lifted", static_cast<unsigned long long>(addr)); return false; }
        lift::Cpu c{};
        c.r[lift::RCX] = rcx; c.r[lift::RDX] = rdx; c.r[lift::R8] = r8; c.r[lift::R9] = r9;
        std::memcpy(&c.x[3], &xmm3, sizeof(float));
        unsigned long code = 0;
        if (!GuardedX(f, c, arg5, code)) { std::snprintf(g_fault, sizeof(g_fault), "call 0x%llx fault 0x%lx at survive pc 0x%llx", static_cast<unsigned long long>(addr), code, static_cast<unsigned long long>(lift::g_pc)); return false; }
        g_lastRax = c.r[lift::RAX];
        if (rax) *rax = g_lastRax;
        return true;
    }
    std::uint64_t g_navObj = 0;   // the lifted controller (primary object); the interface object [svc+0x60] is g_navObj + 0x20
    // Creator 0x141408ca0 and Initialize 0x141409320 (primary slot 1) with the descriptor Survive's own quark creators pass (4 creators 0x1412a52d9 0x1413547cb 0x14187fd47 0x1418a2066 and
    // the soldier-side 0x141416727: +8 key 0xb8a0bf169f98, +0x10 key 0x62bcc8f4fcb6 (the soldier-side one has 0x5b95c98dc68d), +0x18 u16, +0x1c float 9999.0). The gekko's own creator is not identified (not verified).
    bool StartNavController(std::uint32_t units)
    {
        if (g_navObj) return true;
        if (!g_svc || !NavProxy::Ready() || !g_scopeT) { Trace("[nav] controller needs the proxies (StartNav) and the NPC service"); return false; }
        std::uint64_t obj = 0;
        if (!CallAddr(0x141408ca0ull, 0, 0, 0, 0, &obj) || !obj) { Trace("[nav] controller creator failed: %s", g_fault); return false; }
        // Initialize builds the PathfinderDesc {count, 0xd8fd1368c292, desc+8, 0x62bcc8f4fcb6, desc+0x10}: desc+8 is the world name (0xb8a0bf169f98, the default world) and desc+0x10 is the key of the
        // path optimizer executor (TppNavigationController.cpp Initialize, NavPathfinderImpl.cpp PathfinderImpl::PathfinderImpl). 0x5b95c98dc68d is what TPP's Soldier2 controller passes.
        alignas(16) std::uint8_t d[0x20] = {};
        put<std::uint32_t>(d, 0, units); put<std::uint64_t>(d, 8, 0xb8a0bf169f98ull); put<std::uint64_t>(d, 0x10, 0x5b95c98dc68dull);
        put<std::uint16_t>(d, 0x18, 0xffff); put<float>(d, 0x1c, 9999.0f);
        if (!CallAddr(0x141409320ull, obj, reinterpret_cast<std::uint64_t>(d), 0, 0, nullptr)) { Trace("[nav] controller Initialize failed: %s", g_fault); return false; }
        g_navObj = obj;
        put<void*>(g_svc, 0x60, reinterpret_cast<void*>(obj + 0x20));
        if (g_ac) Trace("[nav] WARNING: the ActionController was created before this controller and caches the old [svc+0x60]; start the nav controller before Begin");
        Trace("[nav] lifted NavigationController2Impl is the nav service [svc+0x60] (units %u)", units);
        return true;
    }
    bool NavControllerActive() { return g_navObj != 0; }
    std::uint64_t NavControllerObject() { return g_navObj; }
    unsigned PathCount(int id)
    {
        if (id < 0 || id > 7) return 0;
        if (!g_navObj) return g_path.count;
        const std::uint8_t* tbl = *reinterpret_cast<std::uint8_t* const*>(*reinterpret_cast<std::uint8_t* const*>(g_navObj + 0x50) + 8);   // B proxy +8 = Survive-layout entry table
        const std::uint8_t* path = *reinterpret_cast<std::uint8_t* const*>(tbl + static_cast<std::size_t>(id) * 0x10);
        return path ? *reinterpret_cast<const std::uint32_t*>(path + 0x28) : 0;
    }
    // Once per frame after NavSync. The unit position goes in through interface slot 24 (0x14140a700: id, vec4 position, radius in xmm3, flag byte on the stack) while the brain's order holds a
    // path handle; the radius is the controller's own (navigator record +0xc, the value its Update passes: at 0x14140a19b..0x14140a1b0). Then Update (slot 15, 0x141409f10, this = object + 0x20).
    bool NavUpdate(int id, float x, float y, float z)
    {
        if (!g_navObj || id < 0 || id > 7) return false;
        const std::uint64_t iface = g_navObj + 0x20;
        const std::uint8_t* nrec = *reinterpret_cast<std::uint8_t* const*>(g_navObj + 0x70) + static_cast<std::size_t>(id) * 0x10;   // slot 3 body: [this+0x50] + id * 0x10
        if (*reinterpret_cast<std::uint16_t*>(g_order + 0xb6) != 0xffff)
        {
            alignas(16) float v[4] = { x, y, z, 0.0f };
            if (!CallAddrX(0x14140a700ull, iface, static_cast<std::uint64_t>(id), reinterpret_cast<std::uint64_t>(v), 0, *reinterpret_cast<const float*>(nrec + 0xc), 0, nullptr)) { Trace("[nav] controller position feed failed: %s", g_fault); return false; }
        }
        if (!CallAddr(0x141409f10ull, iface, 0, 0, 0, nullptr)) { Trace("[nav] controller Update failed: %s", g_fault); return false; }
        return true;
    }
    // Read-only log, one line per change of what the brain, the controller and the real pathfinder hold for unit id (capped 300). Order handle word +0xb6, mode +0xb8,
    // timer +0x94; controller: record flags +0xe0 (obj+0x60, stride 0xf0), token / handle / counter / current (obj+0x68, stride 0x10); the path object the brain reads (slot 19 body) and the
    // progress slot 20 gives (index, fraction); then NavProxy::Diag for the real entry.
    void NavDiag(int id, int frame)
    {
        if (!g_navObj || id < 0 || id > 7) return;
        static int lines = 0; static unsigned long long last[4];
        const std::uint8_t* rec = *reinterpret_cast<std::uint8_t* const*>(g_navObj + 0x60) + static_cast<std::size_t>(id) * 0xf0;
        const std::uint8_t* ctr = *reinterpret_cast<std::uint8_t* const*>(g_navObj + 0x68) + static_cast<std::size_t>(id) * 0x10;
        const std::uint16_t handle = *reinterpret_cast<std::uint16_t*>(g_order + 0xb6);
        const std::uint8_t mode = g_order[0xb8];
        const float timer = *reinterpret_cast<float*>(g_order + 0x94);
        const unsigned long long tok = *reinterpret_cast<const unsigned long long*>(ctr) & 0xffffffffffffull;
        const unsigned long long k[4] = { (static_cast<unsigned long long>(handle) << 8) | mode, rec[0xe0] | (static_cast<unsigned long long>(*reinterpret_cast<const std::uint16_t*>(ctr + 8)) << 8) | (static_cast<unsigned long long>(*reinterpret_cast<const std::uint16_t*>(ctr + 0xa)) << 24) | (static_cast<unsigned long long>(*reinterpret_cast<const std::uint16_t*>(ctr + 0xc)) << 40), tok, 0 };
        NavProxy::Diag(id, frame);
        if (k[0] == last[0] && k[1] == last[1] && k[2] == last[2]) return;
        last[0] = k[0]; last[1] = k[1]; last[2] = k[2];
        if (lines++ >= 300) return;
        const std::uint8_t* tbl = *reinterpret_cast<std::uint8_t* const*>(*reinterpret_cast<std::uint8_t* const*>(g_navObj + 0x50) + 8);   // B proxy +8 = Survive-layout entry table
        const std::uint8_t* path = *reinterpret_cast<std::uint8_t* const*>(tbl + static_cast<std::size_t>(id) * 0x10);
        std::uint32_t cnt = path ? *reinterpret_cast<const std::uint32_t*>(path + 0x28) : 0;
        std::uint64_t prog = 0;
        if (handle != 0xffff && cnt) CallAddr(0x141408cf0ull, g_navObj + 0x20, reinterpret_cast<std::uint64_t>(&prog), static_cast<std::uint64_t>(id), 0, nullptr);
        Trace("[navctl] frame %d unit %d order handle %04x mode %u timer %.2f | ctl rec flags %02x token %012llx handle %04x counter %04x current %04x | brain path obj %p count %u progress idx %u frac %.2f | start %.2f %.2f %.2f goal %.2f %.2f %.2f maxlen %.2f",
              frame, id, handle, mode, timer, rec[0xe0], tok, *reinterpret_cast<const std::uint16_t*>(ctr + 8), *reinterpret_cast<const std::uint16_t*>(ctr + 0xa), *reinterpret_cast<const std::uint16_t*>(ctr + 0xc), static_cast<const void*>(path), cnt,
              static_cast<unsigned>(prog & 0xffffffffu), *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(&prog) + 4), *reinterpret_cast<const float*>(rec + 0x90), *reinterpret_cast<const float*>(rec + 0x94), *reinterpret_cast<const float*>(rec + 0x98),
              *reinterpret_cast<const float*>(rec + 0xa0), *reinterpret_cast<const float*>(rec + 0xa4), *reinterpret_cast<const float*>(rec + 0xa8), *reinterpret_cast<const float*>(rec + 0xd0));
    }
    float LastFloat() { return g_lastFloat; }
}