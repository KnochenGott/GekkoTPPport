// world_test.cpp 30/09/2026. Bring-up harness for lifted Survive gekko code on the shared shim world (src/GekkoHook/GekkoWorld.cpp).
// Usage: world_test.exe TOKEN...  Tokens run in order: req=A[,flag] (mailbox request), k1|k3|k4 (Realized dispatch kinds),
// h=A:E (action handler A with handler event E), ev=HEX (animation event on record 0), fin=0|1 (controller says motion finished),
// ai0 / ai (AI state 0 enter / update), tgt=x,y,z (target), pose=x,y,z,yaw (own pose), melee (chooser), x=N (repeat the previous token N more times). Every world call is printed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include "GekkoNodes.h"
#include "GekkoWorld.h"
#include "GekkoPush.h"
#include "GekkoLife.h"
#include "lift_rt.h"
#include "GekkoTable.h"

bool LifeTest();   // life_test.cpp: token lifetest
bool NavProxyTest();   // nav_proxy_test.cpp
void NavMockStep();
void NavMockDetour();   // nav_proxy_test.cpp: token navdetour
void NavMockForce(int v);    // nav_proxy_test.cpp: one frame of the mock pathfinder
void NavMockLine(bool on);   // nav_proxy_test.cpp: token navline=0|1, the mock path is the straight line from the request's start to its goal
void NavMockDelay(int frames);   // nav_proxy_test.cpp: token navdelay=N, the mock pathfinder answers N frames after the request with TPP's flag semantics
bool NavMockStart();   // nav_proxy_test.cpp: mock TPP nav world + proxies + the lifted controller as the nav service (tokens nav4, navup, walk=DX)
static float g_pos[4];
static void Trace(const char* line) { std::printf("  %s\n", line); if (std::strstr(line, "[request] id 0 action 31") || std::strstr(line, "[request] id 0 action 30")) { std::printf("  PCS at request:"); lift::pc_ring_dump(60); } }
static std::uint64_t g_clipId = 0; static float g_clipTime = 0, g_clipLen = 0; static bool g_clipWalk = false; static unsigned g_simFrame = 0;   // g_simFrame: frames run by sim=N so far, printed with every clip request
static bool g_clipLoops = false;   // idle and walk loop restart at their end: the game's finished answer is 1 for one frame per cycle (log 07/10 11:31, "finished -> 1" then "-> 0" 16 ms later); the other clips stay finished
static void SetMotion(std::uint64_t gani, float blend)
{
    std::printf("  -> game: play gani %llx blend %.2f (sim frame %u)\n", (unsigned long long)gani, blend, g_simFrame);
    g_clipId = gani; g_clipTime = 0;
    g_clipLen = 1.5f;   // clip lengths are the mtar frame counts at 60 fps (GekkoTable.h)
    for (int i = 0; i < Gekko::kGaniCount; ++i) if (Gekko::kGaniTable[i] == gani) g_clipLen = Gekko::kGaniFrames[i] / 60.0f;
    g_clipLoops = gani == 0xfc52ff90924aa9f7ull || gani == 0xfc50ac6a2c0a482bull;
    g_clipWalk = gani == 0xfc53795672c99917ull || gani == 0xfc50ac6a2c0a482bull;
}
static void SetSpeed(float s) { std::printf("  -> game: speed %.3f\n", s); }

static float g_tgt[3] = { 10, 0, 10 };
static std::uint32_t g_randRef = 0;

// Mock of TPP's target service, enough for GekkoPush: pool slots 1 (new object), 5 (release), 6 (new handle), 7 (release), 8 (update); object slots 5 (set up) and 11 (data); handle slot 0 (add).
struct MockObj { void** vtbl; std::uint8_t pad[0x38]; std::uint8_t body[0x80]; };   // object +0x28 user data, +0x44, +0x50 owner id
static void* g_mockObjVtbl[16]; static void* g_mockHandleVtbl[4]; static void* g_mockPoolVtbl[16]; static void* g_mockPool = g_mockPoolVtbl;
static std::uint64_t MockNew(void*, std::uint64_t, std::uint64_t, std::uint64_t) { MockObj* o = new MockObj{}; o->vtbl = g_mockObjVtbl; std::printf("  [mock] new object %p\n", (void*)o); return reinterpret_cast<std::uint64_t>(o); }
static std::uint64_t MockSetUp(void* self, std::uint64_t cat, std::uint64_t grp, std::uint64_t flags)
{
    std::printf("  [mock] set up %p: category %llx group %llx flags %llx\n", self, *(unsigned long long*)cat, *(unsigned long long*)grp, (unsigned long long)flags);
    return 0;
}
static std::uint64_t MockData(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(static_cast<MockObj*>(self)->body); }
static std::uint64_t MockHandle(void*, std::uint64_t, std::uint64_t, std::uint64_t) { void** h = new void*[8]{}; h[0] = g_mockHandleVtbl; return reinterpret_cast<std::uint64_t>(h); }
static std::uint64_t MockAdd(void* self, std::uint64_t obj, std::uint64_t, std::uint64_t) { std::printf("  [mock] handle %p adds object %p\n", self, (void*)obj); return 1; }
static std::uint64_t MockUpd(void*, std::uint64_t h, std::uint64_t, std::uint64_t) { std::printf("  [mock] update handle %p\n", (void*)h); return 0; }
static std::uint64_t MockFree(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }
// Mock of TPP's route system for the movement brain's route walk: GetRoute at slot 0 (out {+8 group, +0x10 index}), the route group list at +0x40 / +0x48 (node +8 next, +0x10 group) that
// GekkoWorld's generation walks, a group with vertex count (slot 1) and vertex position (slot 2). One route, id 0x1234, a zigzag of N vertices: 8 m forward and 3 m to the side per vertex.
static float g_rtVerts[32][4]; static unsigned g_rtCount = 0;
static void* g_rtGroupVtbl[8]; static void* g_rtGroup = g_rtGroupVtbl;
static void* g_rtSysVtbl[2]; static std::uint8_t g_rtSys[0x68]; static std::uint8_t g_rtNode[0x18];
static std::uint64_t MockGetRoute(void*, std::uint64_t out, std::uint64_t id, std::uint64_t)
{
    const bool hit = static_cast<std::uint32_t>(id) == 0x1234;
    *reinterpret_cast<void**>(out + 8) = hit ? &g_rtGroup : nullptr;
    *reinterpret_cast<std::uint16_t*>(out + 0x10) = hit ? 0 : 0xffff;
    std::printf("  [mock] GetRoute %08x -> %s\n", static_cast<unsigned>(id), hit ? "group" : "none");
    return out;
}
static std::uint64_t MockVertexCount(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return g_rtCount; }
static std::uint64_t MockVertexPos(void*, std::uint64_t out, std::uint64_t, std::uint64_t v)
{
    const unsigned i = static_cast<std::uint16_t>(v);
    std::memcpy(reinterpret_cast<void*>(out), g_rtVerts[i < g_rtCount ? i : 0], 16);
    std::printf("  [mock] vertex %u -> %.2f %.2f\n", i, g_rtVerts[i][0], g_rtVerts[i][2]);
    return out;
}
static void* MockRouteSystem(unsigned n)
{
    g_rtCount = n > 32 ? 32 : n;
    for (unsigned i = 0; i < g_rtCount; ++i) { g_rtVerts[i][0] = (i & 1) ? 3.0f : 0.0f; g_rtVerts[i][1] = 0; g_rtVerts[i][2] = 8.0f * (i + 1); g_rtVerts[i][3] = 1; }
    g_rtGroupVtbl[1] = (void*)&MockVertexCount; g_rtGroupVtbl[2] = (void*)&MockVertexPos;
    g_rtSysVtbl[0] = (void*)&MockGetRoute;
    *reinterpret_cast<void**>(g_rtSys) = g_rtSysVtbl;
    *reinterpret_cast<void**>(g_rtSys + 0x48) = g_rtNode;
    *reinterpret_cast<void**>(g_rtNode + 8) = g_rtSys + 0x40;
    *reinterpret_cast<void**>(g_rtNode + 0x10) = &g_rtGroup;
    return g_rtSys;
}

// Walk loop clip events (slot 3, gani ac6a2c0a482b, 102 frames, db/gecko_actions.tsv) as its .enchnk carries them (dump, section c8f4fcb6): b9817d52 flag 0x81 = point event
// at 13101/65535; 3903ca38 flag 0x82 with 0x67640400 = range 0x0400..0x6764 (low / high word as start / end: not verified reading of the 0x82 layout). The game delivers a range
// event on every frame inside it (docs/DOCUMENTATION.md); a point event on the frame that crosses it.
static float g_loopTime = 0;
static void FireLoopEvents(float dt)
{
    const float len = 102.0f / 60.0f;
    const float a = std::fmod(g_loopTime, len) / len * 65535.0f;
    g_loopTime += dt;
    const float b = std::fmod(g_loopTime, len) / len * 65535.0f;
    if ((a < 13101.0f && b >= 13101.0f) || (b < a && (a < 13101.0f || b >= 13101.0f))) { std::printf("  [clip] event b9817d52\n"); GekkoWorld::FireEvent(0, 0xb9817d52u, 0); }
    if (b >= 1024.0f && b <= 26468.0f) GekkoWorld::FireEvent(0, 0x3903ca38u, 0);
}
static bool g_simHas = true;
static bool g_simInt = false;   // simint=1: the next sim=N raises the row's interrupt byte (+0x6a) in its first frame, as GekkoAi does when a target takes over from a route
static bool g_blk = false;
static float g_idleTurn = 0.0f;   // sim: total heading change (rad) in idle or stop frames
static unsigned g_steerIdle = 0, g_steerWalk = 0, g_steerOther = 0;   // sim: frames with the heading request set while the locomotion state is idle or stop (1, 2), walk (3), turn (4)
static unsigned g_pathFrames = 0, g_posFrames = 0, g_pathLast = 0, g_pathCount = 0;   // sim: frames in which the request entry was a path order (low bits 1) or a position order (2) of kind 1
static bool g_hop = false;
static bool g_hFirst = false;   // order=h: the Realized dispatch (kind 1) before the locomotion tick (kind 1) in sim=N
static float g_sx = 0, g_sy = 0, g_sz = 0, g_syaw = 0; static bool g_sSeeded = false;   // the sim body (file scope so walkturn can read it)
static bool g_navStep = false, g_detour = false;   // navstep=1: the sim body is moved by the walk state's path correction (GekkoWorld::TakeStep) as GekkoNavStep does in the game; g_detour: the mock path is the navdetour polyline
static double g_devMax = 0, g_devSum = 0, g_stepSum = 0; static unsigned g_devN = 0, g_stepN = 0;   // distance of the sim body from the detour polyline in walk frames; applied steps
static float PolylineDist(float px, float pz)   // distance from (px, pz) to the navdetour path (0,0) (3,3) (7,3) (10,0)
{
    static const float p[4][2] = { { 0, 0 }, { 3, 3 }, { 7, 3 }, { 10, 0 } };
    float best = 1e9f;
    for (int i = 0; i < 3; ++i)
    {
        const float ax = p[i][0], az = p[i][1], bx = p[i + 1][0] - ax, bz = p[i + 1][1] - az;
        float t = ((px - ax) * bx + (pz - az) * bz) / (bx * bx + bz * bz);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float dx = px - (ax + bx * t), dz = pz - (az + bz * t);
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < best) best = d;
    }
    return best;
}
static bool g_walkFix = false;   // walkturn=1: EXPERIMENT in the test only, not in src (docs 4.11): re-enter the walk state when the action that replaced the loop has finished

static void* MockPool()
{
    g_mockObjVtbl[5] = (void*)&MockSetUp; g_mockObjVtbl[11] = (void*)&MockData;
    g_mockHandleVtbl[0] = (void*)&MockAdd;
    g_mockPoolVtbl[1] = (void*)&MockNew; g_mockPoolVtbl[5] = (void*)&MockFree; g_mockPoolVtbl[6] = (void*)&MockHandle; g_mockPoolVtbl[7] = (void*)&MockFree; g_mockPoolVtbl[8] = (void*)&MockUpd;
    return &g_mockPool;
}

static bool g_started = false;
static void EnsureStart()   // the idle start the tokens after it assume (the same as the start token), once
{
    if (g_started) return;
    g_started = true;
    static const float origin[3] = {};
    GekkoWorld::Request(0, 29, 0, origin);
    GekkoWorld::ActionUpdate();
    GekkoWorld::Prime(0, 29);
    GekkoWorld::Dispatch(1, 0);
    GekkoWorld::SetAiState(0, 12);
}

static bool RunToken(const std::string& t)
{
    bool ok = true;
    if (t.rfind("nav", 0) != 0) GekkoWorld::Begin();   // the Observer setup and the lifted ActionController (the nav tokens start the nav service first)
    if (t == "nav") return NavProxyTest();
    if (t.rfind("navline=", 0) == 0) { NavMockLine(t[8] == '1'); return true; }
    if (t.rfind("navdelay=", 0) == 0) { NavMockDelay(std::atoi(t.c_str() + 9)); return true; }
    if (t.rfind("navforce=", 0) == 0) { NavMockForce(std::atoi(t.c_str() + 9)); return true; }
    if (t == "nav4") return NavMockStart();
    if (t == "navdetour") { NavMockDetour(); g_detour = true; return true; }
    if (t.rfind("navstep=", 0) == 0) { g_navStep = t[8] == '1'; return true; }
    if (t == "navprog")   // the controller's own path progress for unit 0 (path object A slot 20, as the brain asks): {segment index, fraction}
    {
        const std::uint64_t o = GekkoWorld::NavControllerObject();
        if (!o) return false;
        const std::uint64_t R = *reinterpret_cast<const std::uint64_t*>(o + 0x70);
        const std::uint64_t A = *reinterpret_cast<const std::uint64_t*>(R);
        const std::uint32_t h = *reinterpret_cast<const std::uint32_t*>(R + 8);
        if (!A) { std::printf("  navprog: no path object yet (handle %u)\n", h); return true; }
        const std::uint64_t fn = (*reinterpret_cast<const std::uint64_t* const*>(A))[20] - lift::kTokenBase;
        alignas(16) std::uint8_t out[16] = {};
        const bool ok2 = GekkoWorld::CallAddr(fn, A, reinterpret_cast<std::uint64_t>(out), h, 0, nullptr);
        std::printf("  navprog: handle %u -> index %u fraction %.3f (%s)\n", h, *reinterpret_cast<std::uint32_t*>(out), *reinterpret_cast<float*>(out + 4), ok2 ? "ok" : GekkoWorld::LastFault());
        return ok2;
    }
    if (t == "navup") { static int nf = 0; NavMockStep(); GekkoWorld::NavSync(); const bool r = GekkoWorld::NavUpdate(0, g_pos[0], g_pos[1], g_pos[2]); GekkoWorld::NavDiag(0, ++nf); return r; }
    if (t.rfind("walk=", 0) == 0) { g_pos[0] += static_cast<float>(std::atof(t.c_str() + 5)); GekkoWorld::SetPose(0, g_pos[0], g_pos[1], g_pos[2], g_pos[3]); std::printf("  pose x %.2f\n", g_pos[0]); return true; }
    if (t.rfind("req=", 0) == 0)
    {
        unsigned a = 0, fl = 0;
        std::sscanf(t.c_str() + 4, "%u,%u", &a, &fl);
        GekkoWorld::Request(0, a, fl, g_tgt);
        ok = GekkoWorld::ActionUpdate();
    }
    else if (t == "start")   // what GekkoBrain::Start does after Init: the idle request, the instance initialiser, the first dispatch, locomotion idle
    {
        g_started = true;
        static const float origin[3] = {};
        GekkoWorld::Request(0, 29, 0, origin);
        ok = GekkoWorld::ActionUpdate();
        GekkoWorld::Prime(0, 29);   // the instance initialiser needs the record pool objects (game: real controller, headless: not built since the manager became lifted); Prime does its copy
        ok = GekkoWorld::Dispatch(1, 0);
        GekkoWorld::SetAiState(0, 12);
    }
    else if (t == "bodies")   // the record's three target objects from Survive's own allocation 0x140f943b0
    {
        ok = GekkoWorld::AllocBodies(0);
        const std::uint8_t* r = GekkoWorld::RecordBytes(0);
        for (int off : { 0x180, 0x188, 0x190, 0x198, 0x1a0, 0x1a8 }) std::printf("  record +0x%x = %p\n", off, *reinterpret_cast<void* const*>(r + off));
    }
    else if (t.rfind("prime=", 0) == 0) GekkoWorld::Prime(0, static_cast<unsigned>(std::atoi(t.c_str() + 6)));
    else if (t[0] == 'k') ok = GekkoWorld::Dispatch(std::atoi(t.c_str() + 1), 0);
    else if (t.rfind("h=", 0) == 0) { int a = 0, e = 0; std::sscanf(t.c_str() + 2, "%d:%d", &a, &e); ok = GekkoWorld::Run(a, e); }
    else if (t.rfind("ev=", 0) == 0) ok = GekkoWorld::FireEvent(0, static_cast<std::uint32_t>(std::strtoul(t.c_str() + 3, nullptr, 16)), 0);
    else if (t.rfind("call=", 0) == 0)   // call=ADDR[,rcx,rdx,r8,r9] (hex): any lifted function, prints rax; "$" prefix reuses the previous rax
    {
        static std::uint64_t last = 0;
        unsigned long long a[5] = {};
        std::string s = t.substr(5);
        size_t pos = 0; int n = 0;
        while (n < 5 && pos <= s.size())
        {
            size_t e = s.find(',', pos); if (e == std::string::npos) e = s.size();
            std::string tok = s.substr(pos, e - pos);
            a[n++] = tok == "$" ? last : std::strtoull(tok.c_str(), nullptr, 16);
            pos = e + 1;
        }
        std::uint64_t rax = 0;
        ok = GekkoWorld::CallAddr(a[0], a[1], a[2], a[3], a[4], &rax);
        last = rax;
        std::printf("  rax = 0x%llx\n", static_cast<unsigned long long>(rax));
    }
    else if (t == "melee") ok = GekkoWorld::MeleeChoose(0);
    else if (t == "move") ok = GekkoWorld::MoveUpdate(0);
    else if (t.rfind("mode=", 0) == 0) GekkoWorld::Order()[0xb8] = static_cast<std::uint8_t>(std::atoi(t.c_str() + 5));
    else if (t.rfind("has=", 0) == 0) GekkoWorld::SetTargetState(t[4] == '1', 1.0f);
    else if (t.rfind("tobj=", 0) == 0) { unsigned o = 0, v = 0; std::sscanf(t.c_str() + 5, "%x,%x", &o, &v); GekkoWorld::TargetObj()[o] = static_cast<std::uint8_t>(v); }
    else if (t.rfind("order=", 0) == 0) { unsigned o = 0, v = 0; std::sscanf(t.c_str() + 6, "%x,%x", &o, &v); GekkoWorld::Order()[o] = static_cast<std::uint8_t>(v); }
    else if (t.rfind("loco=", 0) == 0) ok = GekkoWorld::LocoRun(std::atoi(t.c_str() + 5), 0);
    else if (t.rfind("lh=", 0) == 0) { unsigned long long f = 0; int e = 0; std::sscanf(t.c_str() + 3, "%llx:%d", &f, &e); ok = GekkoWorld::LocoCall(f, e, 0); }   // lh=ADDR:EVENT
    else if (t.rfind("lt=", 0) == 0) { ok = GekkoWorld::LocoTick(std::atoi(t.c_str() + 3), 0); std::printf("  loco state %u\n", GekkoWorld::LocoStateNow(0)); }
    else if (t.rfind("pcs=", 0) == 0) { std::printf("  pcs:"); lift::pc_ring_dump(std::atoi(t.c_str() + 4)); }
    else if (t == "mo") { const std::uint32_t* w = reinterpret_cast<const std::uint32_t*>(GekkoWorld::MotionOut()); std::printf("  motion out:"); for (int i = 0; i < 128; ++i) if (w[i]) std::printf(" +%x=%08x(%g)", i * 4, w[i], *reinterpret_cast<const float*>(w + i)); std::printf("\n"); }
    else if (t == "actest")   // the lifted ActionController with synthetic requests: an action, a position order that is answered with a path order, and a direct position order
    {
        auto send = [&](std::uint8_t b8, std::uint8_t kind, float x, float z, std::uint32_t action)
        {
            alignas(16) std::uint8_t r[0xc0] = {};
            r[0xb8] = b8; r[0xb9] = kind;
            *reinterpret_cast<std::uint32_t*>(r + 0x20) = action;
            *reinterpret_cast<float*>(r + 0x90) = x; *reinterpret_cast<float*>(r + 0x98) = z;
            return GekkoWorld::SendRequest(0, r);
        };
        auto elem = [&]() { return GekkoWorld::RequestBytes(0); };
        int bad = 0;
        auto check = [&](bool c, const char* what) { std::printf("  [%s] %s\n", c ? "ok" : "FAIL", what); if (!c) ++bad; };
        // 1. an action request (kind 4, category 0): committed into the request array with its serial, record flags 0x10 (issued) and 0x08 (accepted)
        const std::uint16_t serial0 = *reinterpret_cast<const std::uint16_t*>(elem() + 0xae);
        ok = send(0, 4, 0, 0, 13) && GekkoWorld::ActionUpdate();
        check(ok && elem()[0xb6] == 4 && *reinterpret_cast<const std::uint32_t*>(elem() + 0x20) == 13, "action request committed: kind 4, action 13");
        check(*reinterpret_cast<const std::uint16_t*>(elem() + 0xae) != serial0, "the serial of the request changed");
        check((GekkoWorld::Peek(0).controllerFlags & 0x18) == 0x18, "record flags: issued 0x10 and accepted 0x08");
        // 2. a direct position order (category 1, byte +0xb8 bit 3): the entry becomes a position order (low bits 2) with the goal at +0x90
        ok = ok && send(0x08, 1, 5.0f, 7.0f, 0) && GekkoWorld::ActionUpdate();
        check((*reinterpret_cast<const std::uint16_t*>(elem() + 0xb4) & 3) == 2 && elem()[0xb6] == 1 && *reinterpret_cast<const float*>(elem() + 0x90) == 5.0f && *reinterpret_cast<const float*>(elem() + 0x98) == 7.0f,
              "category 1: position order (type 2) at 5, 7");
        std::printf("  actest %s\n", bad ? "FAILED" : "OK");
        ok = ok && bad == 0;
    }
    else if (t == "acdbg")   // the unit's navigator record and what the ActionController asks of it
    {
        std::uint8_t* svc = static_cast<std::uint8_t*>(GekkoWorld::NpcService());
        std::uint8_t* nav = *reinterpret_cast<std::uint8_t**>(svc + 0x60);
        using Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
        const std::uint64_t* vt = *reinterpret_cast<const std::uint64_t* const*>(nav);
        std::uint64_t rec = 0;
        ok = GekkoWorld::CallAddr(vt[3] - lift::kTokenBase, reinterpret_cast<std::uint64_t>(nav), 0, 0, 0, &rec);
        if (ok && rec)
        {
            const std::uint64_t A = *reinterpret_cast<const std::uint64_t*>(rec); const std::uint32_t h = *reinterpret_cast<const std::uint32_t*>(rec + 8);
            std::printf("  nav record: object %llx handle %u\n", (unsigned long long)A, h);
            if (A)
            {
                const std::uint64_t* avt = *reinterpret_cast<const std::uint64_t* const*>(A);
                std::uint64_t r17 = 0, r19 = 0;
                GekkoWorld::CallAddr(avt[17] - lift::kTokenBase, A, h, 0, 0, &r17);
                GekkoWorld::CallAddr(avt[19] - lift::kTokenBase, A, h, 0, 0, &r19);
                { const std::uint8_t* tb = *reinterpret_cast<const std::uint8_t* const*>(A + 0x48); for (unsigned e = 0; e < 3; ++e) { std::printf("  A+0x48 entry[%u]:", e); for (int b = 0; b < 16; ++b) std::printf(" %02x", tb[e * 16 + b]); std::printf("  "); } std::printf("\n"); }
                std::printf("  A slot 17 (%llx): %llx, slot 19: path %llx count %u\n", (unsigned long long)avt[17], (unsigned long long)(r17 & 0xff), (unsigned long long)r19, r19 ? *reinterpret_cast<const std::uint32_t*>(r19 + 0x28) : 0);
            }
        }
        const std::uint8_t* ac = static_cast<const std::uint8_t*>(GekkoWorld::ActionControllerIface());
        std::printf("  ActionController %p: +0x28 (local-authority byte) %u %u, +0x30 %p\n", ac, ac[0x28], ac[0x29], *reinterpret_cast<void* const*>(ac + 0x30));
    }
    else if (t == "pathcheck")   // after sim=N with a nav controller: the lifted ActionController answered the brain's position orders with path orders that end at the last point of the nav path
    {
        ok = g_pathFrames > 0 && g_pathCount > 1 && g_pathLast + 1 == g_pathCount && g_posFrames == 0;
        std::printf("  pathcheck %s\n", ok ? "OK" : "FAILED");
    }
    else if (t == "steer") { float y = 0, r = 0; const bool on = GekkoWorld::TakeSteer(&y, &r); std::printf("  steer %d yaw %.3f N %.1f\n", on, y, r); }
    else if (t == "steerrule")   // the heading request shadow: persists between frames, is cleared only by the lifted code, and leaves TPP's own bytes in the motion output untouched
    {
        std::uint8_t* mo = GekkoWorld::MotionOut();
        const std::uint32_t tppFlags = 0x12345678, tppN = 0x9abcdef0;
        std::memcpy(mo + 0x190, &tppFlags, 4); std::memcpy(mo + 0x1a8, &tppN, 4);
        float y = 0, n = 0;
        auto run = [&](void (*body)(std::uint8_t*)) { GekkoWorld::SteerBegin(); body(mo); GekkoWorld::SteerEnd(); };
        run([](std::uint8_t* m) { const float q[4] = { 0, std::sin(0.5f), 0, std::cos(0.5f) }, nn = 90.0f; std::memcpy(m + 0x80, q, 16); std::memcpy(m + 0x1a8, &nn, 4); *reinterpret_cast<std::uint32_t*>(m + 0x190) |= 0x100; });
        bool good = GekkoWorld::TakeSteer(&y, &n) && std::fabs(y - 1.0f) < 1e-5f && n == 90.0f;
        std::uint32_t f2 = 0, n2 = 0; std::memcpy(&f2, mo + 0x190, 4); std::memcpy(&n2, mo + 0x1a8, 4);
        good = good && f2 == tppFlags && n2 == tppN;                                  // TPP's bytes are back after the frame
        run([](std::uint8_t*) {});                                                    // a frame in which the locomotion writes nothing: the request stays
        good = good && GekkoWorld::TakeSteer(&y, &n);
        run([](std::uint8_t* m) { *reinterpret_cast<std::uint32_t*>(m + 0x190) &= ~0x100u; });   // turn state or idle clears it
        good = good && !GekkoWorld::TakeSteer(&y, &n);
        // The real locomotion states over the shadow (4.8): the heading request is Survive's persistent CharaControl state, so idle (1) and stop (2) leave it as it is (their late events only OR bits 0 and 1
        // into +0x190, into the shadow, never into TPP's bytes), the walk state clears it on enter (0x140f8a157) and the turn state (4) on every update (0x140f89260).
        {
            auto request = []() { std::uint8_t* m = GekkoWorld::MotionOut(); GekkoWorld::SteerBegin(); *reinterpret_cast<std::uint32_t*>(m + 0x190) |= 0x100; GekkoWorld::SteerEnd(); };
            auto state = [&](unsigned st, int kind) { GekkoWorld::LocoState(0, st); GekkoWorld::SteerBegin(); const bool r = GekkoWorld::LocoTick(kind, 0); GekkoWorld::SteerEnd(); return r; };
            bool states = true;
            for (unsigned st : { 1u, 2u })
            {
                request();
                const bool r1 = state(st, 1), r3 = state(st, 3);
                const bool kept = GekkoWorld::TakeSteer(&y, &n);
                std::uint32_t f3 = 0; std::memcpy(&f3, mo + 0x190, 4);
                std::printf("  steerrule state %u: ticks %d %d, request kept %d, TPP flags untouched %d\n", st, r1, r3, kept, f3 == tppFlags);
                states = states && r1 && r3 && kept && f3 == tppFlags;
            }
            request();
            GekkoWorld::SetMotionFinished(false);   // the turn state returns to idle once its clip is finished
            GekkoWorld::LocoState(0, 4);
            bool r4 = true;
            for (int kind : { 1, 3, 5 }) { GekkoWorld::SteerBegin(); r4 = GekkoWorld::LocoTick(kind, 0) && r4; GekkoWorld::SteerEnd(); }
            const bool keptByTicks = GekkoWorld::TakeSteer(&y, &n);   // the base ticks never run mode 7 (the move step, interface slot 20), so they do not clear it
            GekkoWorld::SteerBegin(); r4 = GekkoWorld::MoveStep(0) && r4; GekkoWorld::SteerEnd();   // mode 7 of the turn state calls 0x140f89260, which clears the flag
            const bool cleared = !GekkoWorld::TakeSteer(&y, &n);
            std::printf("  steerrule state 4 (turn): state now %u, base ticks left the request %d, mode 7 ran %d and cleared it %d\n", GekkoWorld::LocoStateNow(0), keptByTicks, r4, cleared);
            states = states && r4 && cleared && keptByTicks;
            good = good && states;
        }
        std::printf("  steerrule %s\n", good ? "OK" : "FAILED");
        ok = good;
    }
    else if (t.rfind("sim=", 0) == 0)   // sim=N: N frames of the in-game call order with a crude body (walks along its heading while the locomotion is in its walk state)
    {
        const int n = std::atoi(t.c_str() + 4);
        const float dt = 1.0f / 60;
        float& x = g_sx; float& y = g_sy; float& z = g_sz; float& yaw = g_syaw; bool& seeded = g_sSeeded;
        if (!seeded) { seeded = true; x = g_pos[0]; y = g_pos[1]; z = g_pos[2]; yaw = g_pos[3]; }   // pose= seeds the sim body
        GekkoWorld::SetDt(dt);
        float maxz = 0.0f;
        const bool pulse = g_simInt; g_simInt = false;
        for (int f = 0; f < n && ok; ++f)
        {
            ++g_simFrame;
            GekkoWorld::SetPose(0, x, y, z, yaw);
            if (g_simHas) GekkoWorld::SetTarget(g_tgt[0], g_tgt[1], g_tgt[2]);
            GekkoWorld::SetTargetState(g_simHas, g_simHas ? 1.0f : 0.0f, pulse && f == 0);   // as GekkoBrain::Frame
            g_clipTime += dt;
            if (g_clipId == 0xfc50ac6a2c0a482bull) FireLoopEvents(dt); else g_loopTime = 0;   // the controller delivers the clip's events before the brain runs (GekkoFull::Execute, then ExecuteSerially)
            GekkoWorld::SetMotionFinished(g_clipLoops ? std::fmod(g_clipTime - dt, g_clipLen) > std::fmod(g_clipTime, g_clipLen) : g_clipTime >= g_clipLen);
            if (GekkoWorld::NavControllerActive()) { NavMockStep(); GekkoWorld::NavSync(); GekkoWorld::NavUpdate(0, x, y, z); }   // as GekkoAi::Update
            GekkoWorld::SteerBegin();
            ok = GekkoWorld::AiUpdate(0) && GekkoWorld::MoveUpdate(0) && GekkoWorld::ActionUpdate() && (g_hFirst ? GekkoWorld::Dispatch(1, 0) && GekkoWorld::LocoTick(1, 0) : GekkoWorld::LocoTick(1, 0) && GekkoWorld::Dispatch(1, 0)) && GekkoWorld::LocoTick(3, 0) && GekkoWorld::LocoTick(5, 0) && GekkoWorld::MoveStep(0);
            GekkoWorld::SteerEnd();
            if (g_navStep)   // GekkoNavStep::Apply and the VelocityControlAction: the local step is rotated into the world by the body yaw (x right, z forward) and added to the position
            {
                float st[3];
                if (GekkoWorld::TakeStep(st) && (st[0] != 0 || st[1] != 0 || st[2] != 0))
                {
                    x += st[0] * std::cos(yaw) + st[2] * std::sin(yaw); z += -st[0] * std::sin(yaw) + st[2] * std::cos(yaw); y += st[1];
                    g_stepSum += std::sqrt(st[0] * st[0] + st[1] * st[1] + st[2] * st[2]); ++g_stepN;
                }
            }
            if (g_walkFix && (GekkoWorld::Peek(0).controllerFlags & 0x20) && GekkoWorld::LocoStateNow(0) == 3 && !g_clipWalk) GekkoWorld::LocoState(0, 3);
            { const std::uint8_t* q = GekkoWorld::RequestBytes(0); if (q && q[0xb6] == 1) { const unsigned ty = *reinterpret_cast<const std::uint16_t*>(q + 0xb4) & 3; if (ty == 1) { ++g_pathFrames; g_pathLast = *reinterpret_cast<const std::uint16_t*>(q + 0xac); g_pathCount = GekkoWorld::PathCount(0); } else if (ty == 2) ++g_posFrames; } }
            if (g_hop) *reinterpret_cast<std::uint64_t*>(GekkoWorld::NpcEntry(0)) |= 1ull << 53;   // hop=1: what the plugin frame (0x140f78a90) writes when its headroom sweep is free
            if (g_blk) *reinterpret_cast<std::uint64_t*>(GekkoWorld::NpcEntry(0) + 0x18) |= 1ull << 41;   // blk=1: the locomotion ray reports a hit every frame
            if (ok && !GekkoPush::RunKind(0) && f == 0) std::printf("  WARNING: plugin frame not running (no 'push' token): the NPC event bits are never cleared\n");   // GekkoAi::Update order
            GekkoWorld::RouteLoopStep();   // as GekkoAi::Update, right after the brain
            float wy = 0, n = 0;
            if (GekkoWorld::TakeSteer(&wy, &n))   // the CharaControl's turn: the remaining angle shrinks by pow(1 - 1/N, 59.94 * dt) per frame (N <= 1 means 8)
            {
                { const unsigned ls = GekkoWorld::LocoStateNow(0); if (ls == 3) ++g_steerWalk; else if (ls == 1 || ls == 2) ++g_steerIdle; else ++g_steerOther; }
                float d = wy - yaw;
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                const float step = d * (1.0f - std::pow(1.0f - 1.0f / (n > 1.0f ? n : 8.0f), 59.94f * dt));
                { const unsigned ls = GekkoWorld::LocoStateNow(0); if (ls == 1 || ls == 2) g_idleTurn += std::fabs(step); }
                yaw += step;
            }
            if (GekkoWorld::LocoStateNow(0) == 3 && g_clipWalk) { x += std::sin(yaw) * 1.4f * dt; z += std::cos(yaw) * 1.4f * dt; }
            if (std::fabs(z) > maxz) maxz = std::fabs(z);
            if (g_detour && GekkoWorld::LocoStateNow(0) == 3) { const double d = PolylineDist(x, z); g_devSum += d; ++g_devN; if (d > g_devMax) g_devMax = d; }
            const std::uint8_t* od = GekkoWorld::Order();
            if (f % 15 == 0 || !ok) std::printf("  sim f%d pos %.2f %.2f yaw %.2f loco %u action %u clip %llx mode %u dist %.2f goal %.2f %.2f vertex %u\n", f, x, z, yaw, GekkoWorld::LocoStateNow(0), GekkoWorld::Peek(0).action,
                (unsigned long long)g_clipId, od[0xb8], *reinterpret_cast<const float*>(od + 0x90), *reinterpret_cast<const float*>(od), *reinterpret_cast<const float*>(od + 8), *reinterpret_cast<const std::uint16_t*>(od + 0xb4));
        }
        std::printf("  sim summary: max |z| %.2f, end pos %.2f %.2f\n", maxz, x, z);
        if (g_detour) std::printf("  sim path deviation (navdetour polyline, walk frames %u): max %.3f mean %.3f; path correction %s: %u step(s), total %.3f m\n", g_devN, g_devMax, g_devN ? g_devSum / g_devN : 0.0, g_navStep ? "ON" : "off", g_stepN, g_stepSum);
        std::printf("  sim heading request: set in %u walk frame(s), %u idle or stop frame(s), %u other; heading change in the idle or stop frames %.3f rad\n", g_steerWalk, g_steerIdle, g_steerOther, g_idleTurn);
        std::printf("  sim orders: path order in %u frame(s) (last point %u of %u), position order in %u frame(s)\n", g_pathFrames, g_pathLast, g_pathCount, g_posFrames);
    }
    else if (t == "push")   // Survive's push-slide creation and update against a mock of TPP's target service (objects with the slots the code calls)
    {
        GekkoPush::TestMode();
        GekkoPush::Hooks ph; ph.trace = Trace;
        ok = GekkoPush::Start(ph, MockPool(), 51200);
    }
    else if (t.rfind("route=", 0) == 0) { GekkoWorld::SetRouteSystem(MockRouteSystem(static_cast<unsigned>(std::atoi(t.c_str() + 6))), nullptr); GekkoWorld::SetRoute(0, 0x1234, 0); }
    else if (t == "entry") { const std::uint64_t* e = reinterpret_cast<const std::uint64_t*>(GekkoWorld::NpcEntry(0)); std::printf("  npc entry: +0 %016llx (bit53 %d) +8 %016llx +10 %016llx +18 %016llx (bit41 %d)\n", (unsigned long long)e[0], int((e[0] >> 53) & 1), (unsigned long long)e[1], (unsigned long long)e[2], (unsigned long long)e[3], int((e[3] >> 41) & 1)); }
    else if (t.rfind("hop=", 0) == 0) g_hop = t[4] == '1';
    else if (t == "order=h") g_hFirst = true;
    else if (t.rfind("blk=", 0) == 0) g_blk = t[4] == '1';
    else if (t.rfind("simhas=", 0) == 0) g_simHas = t[7] == '1';
    else if (t.rfind("simint=", 0) == 0) g_simInt = t[7] == '1';
    else if (t.rfind("routecut=", 0) == 0)   // a route walk, then a target far to the side (docs 5.13): with the interrupt byte Survive's route cancel (0x140fa47d5) must end the route and the body must turn to the target
    {
        const bool withPulse = t[9] == '1';
        ok = RunToken("sim=300");
        const std::uint32_t routeBefore = *reinterpret_cast<const std::uint32_t*>(GekkoWorld::Order() + 0xb0);
        const float x0 = g_sx, z0 = g_sz;
        GekkoWorld::StopRouteLoop();   // GekkoAi::SetTarget does this too
        ok = ok && RunToken("tgt=-25,0,10") && RunToken("simhas=1");
        g_simInt = withPulse;
        ok = ok && RunToken("sim=60");   // the pulse lasts one frame; the cancel is done in it
        const std::uint32_t routeAfter = *reinterpret_cast<const std::uint32_t*>(GekkoWorld::Order() + 0xb0);
        ok = ok && RunToken("sim=840");
        const float d0 = std::hypot(-25.0f - x0, 10.0f - z0), d1 = std::hypot(-25.0f - g_sx, 10.0f - g_sz);
        const bool cut = routeAfter == 0xbf169f98u, closer = d1 < d0 - 5.0f;
        std::printf("  routecut=%d: route id %08x -> %08x (%s), distance to the target %.2f -> %.2f (%s)\n", withPulse ? 1 : 0, routeBefore, routeAfter, cut ? "cancelled" : "still set", d0, d1, closer ? "approached" : "not approached");
        if (withPulse) { std::printf("  routecut %s\n", (cut && closer) ? "OK" : "FAILED"); ok = ok && cut && closer; }
    }
    else if (t == "svccmp") { std::uint8_t* inst = GekkoNodes::CreateSsdNpc(nullptr); ok = inst != nullptr; if (ok) GekkoWorld::CompareSvc(inst + 0x20); }
    else if (t == "realnpc") { std::uint8_t* inst = GekkoNodes::CreateSsdNpc(nullptr); std::printf("  realnpc: SsdNpc instance %p%s\n", static_cast<void*>(inst), inst ? "" : " (not created)"); ok = inst != nullptr; }
    else if (t.rfind("plug=", 0) == 0) { ok = GekkoPush::RunKind(std::atoi(t.c_str() + 5)); }
    else if (t.rfind("pushup", 0) == 0) { const float p[3] = { 1, 2, 3 }; GekkoPush::Update(p); ok = GekkoPush::Active(); }
    else if (t == "dump") GekkoWorld::DumpLoco(0);
    else if (t == "walkturn" || t == "walkturn=1")   // the stall of 07/10/2026 11:31:32: the brain asks for a turn (action 27) while the walk loop is running; after the clip the gekko must walk on
    {
        g_walkFix = t.size() > 8;
        const unsigned loco0 = GekkoWorld::LocoStateNow(0);
        const bool looping = g_clipId == 0xfc50ac6a2c0a482bull;
        ok = RunToken("req=27,0") && RunToken("sim=200");
        const float zMid = g_sz;
        ok = ok && RunToken("sim=300");
        const float moved = g_sz - zMid;
        char line[320];
        GekkoWorld::StallLine(0, line, sizeof(line));
        std::printf("  walkturn%s: loco %u and walk loop %d before the turn; in the 300 frames after the turn clip the body moved %.2f m, loco now %u, clip %llx -> %s\n", g_walkFix ? "=1 (test-only re-enter)" : "", loco0, looping ? 1 : 0, moved, GekkoWorld::LocoStateNow(0), (unsigned long long)g_clipId,
                    moved > 1.0f ? "WALKING" : "STALLED");
        std::puts(line);
        g_walkFix = false;
    }
    else if (t == "turnline") { char l[400]; GekkoWorld::TurnLine(l, sizeof(l)); std::printf("[turnlog] %s\n", l); }
    else if (t == "stallline") { char l[640]; GekkoWorld::StallLine(0, l, sizeof(l)); std::puts(l); }
    else if (t.rfind("ls=", 0) == 0) GekkoWorld::LocoState(0, static_cast<unsigned>(std::atoi(t.c_str() + 3)));
    else if (t.rfind("locotgt=", 0) == 0) { float x = 0, y = 0, z = 0; std::sscanf(t.c_str() + 8, "%f,%f,%f", &x, &y, &z); GekkoWorld::LocoTarget(0, x, y, z); }
    else if (t == "bittable")   // every flag bit of the NPC state entry, one at a time: which state does the chooser pick?
    {
        for (unsigned off : { 0x00u, 0x08u, 0x10u, 0x18u, 0x20u })
            for (unsigned bit = 0; bit < (off >= 0x20 ? 32u : 64u); ++bit)
            {
                std::uint8_t* e = GekkoWorld::NpcEntry(0);
                std::memset(e, 0, 0x28);
                if (off >= 0x20) *reinterpret_cast<std::uint32_t*>(e + off) = 1u << bit; else *reinterpret_cast<std::uint64_t*>(e + off) = 1ull << bit;
                GekkoWorld::SetAiState(0, 12);
                const int st = GekkoWorld::PickState(0);
                if (st != 12 && st != 0) std::printf("BIT entry+0x%02x bit %2u -> state %d\n", off, bit, st);
            }
        std::memset(GekkoWorld::NpcEntry(0), 0, 0x28);
    }
    else if (t.rfind("seed=", 0) == 0) { g_randRef = static_cast<std::uint32_t>(std::strtoul(t.c_str() + 5, nullptr, 16)); GekkoWorld::SeedRandom(g_randRef); }
    else if (t.rfind("rand=", 0) == 0)   // N draws of each NPC service random slot against a reference xorshift32 (13, 7, 5) from the seed: slot 13 float [0,1), slot 12 float [-1,1), slot 14 the raw word
    {
        const int n = std::atoi(t.c_str() + 5);
        void** vt = *static_cast<void***>(GekkoWorld::NpcService());
        using FloatSlot = float (*)(void*); using RawSlot = std::uint64_t (*)(void*);
        void* svc = GekkoWorld::NpcService();
        int bad = 0; float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < n * 3; ++i)
        {
            const int which = i % 3;   // 13, 12, 14 in turn, one state step each
            std::uint32_t x = g_randRef; x ^= x << 13; x ^= x >> 7; x ^= x << 5; g_randRef = x;
            const float unit = static_cast<float>(static_cast<std::int64_t>(x)) * 2.3283064365386963e-10f;
            if (which == 0) { const float v = reinterpret_cast<FloatSlot>(vt[13])(svc); if (v != unit || v < 0.0f || v >= 1.0f) ++bad; if (v < lo) lo = v; if (v > hi) hi = v; }
            else if (which == 1) { const float v = reinterpret_cast<FloatSlot>(vt[12])(svc); if (v != unit * 2.0f - 1.0f || v < -1.0f || v >= 1.0f) ++bad; }
            else { const std::uint64_t v = reinterpret_cast<RawSlot>(vt[14])(svc); if (static_cast<std::uint32_t>(v) != x) ++bad; }
        }
        std::printf("  random slots: %d draws of 12, 13, 14 against xorshift32 13/7/5, %d mismatch(es), slot 13 range %.6f .. %.6f\n", n * 3, bad, lo, hi);
        ok = bad == 0;
    }
    else if (t.rfind("arrive=", 0) == 0)   // the arrival request (action 6) with a unit target, position slot 0, 1 or 2 of the unit registry: enter and updates must run without a fault and ask the registry
    {
        EnsureStart();
        const unsigned slot = static_cast<unsigned>(std::atoi(t.c_str() + 7));
        const unsigned before = GekkoLife::UnitPositionCalls(static_cast<int>(slot));
        GekkoWorld::RequestUnitTarget(0, 6, 0, 0, slot);
        ok = GekkoWorld::ActionUpdate();
        for (int i = 0; i < 4 && ok; ++i) ok = GekkoWorld::Dispatch(1, 0);
        const unsigned calls = GekkoLife::UnitPositionCalls(static_cast<int>(slot)) - before;
        std::printf("  arrival request (unit target, slot %u): action now %u, unit registry position slot %u asked %u time(s), target %.2f %.2f %.2f\n", slot, GekkoWorld::Peek(0).action, slot, calls, g_tgt[0], g_tgt[1], g_tgt[2]);
        if (ok && calls == 0) { std::printf("  FAIL: the resolver never asked the unit registry\n"); ok = false; }
    }
    else if (t.rfind("arrivefull=", 0) == 0 || t.rfind("restart=", 0) == 0)
    {
        // arrivefull=SLOT: the arrival request (action 6) run to its end with the record's target objects allocated: frames of the in-game order (no AI, no movement brain) until the clip has ended
        // and the ActionController reports the action finished (controller flag 0x20). restart=SLOT: the same request stopped half way, then the unit state reset and a fresh idle start.
        EnsureStart();
        const bool restart = t[0] == 'r';
        const unsigned slot = static_cast<unsigned>(std::atoi(t.c_str() + (restart ? 8 : 11)));
        const float dt = 1.0f / 60;
        ok = GekkoWorld::AllocBodies(0);
        if (!ok) { std::printf("  FAIL: record objects: %s\n", GekkoWorld::LastFault()); return false; }
        GekkoWorld::SetDt(dt);
        GekkoWorld::SetPose(0, 0, 0, 0, 0);
        static const float tgt[3] = { 10, 0, 10 };
        GekkoWorld::SetTarget(tgt[0], tgt[1], tgt[2]);
        GekkoWorld::RequestUnitTarget(0, 6, 0, 0, slot);
        int doneAt = -1, clipEnd = -1; const int limit = restart ? 40 : 900;
        for (int f = 0; f < limit && ok; ++f)
        {
            g_clipTime += dt;
            GekkoWorld::SetMotionFinished(g_clipLoops ? std::fmod(g_clipTime - dt, g_clipLen) > std::fmod(g_clipTime, g_clipLen) : g_clipTime >= g_clipLen);
            if (clipEnd < 0 && g_clipLen > 0 && g_clipTime >= g_clipLen) clipEnd = f;
            ok = GekkoWorld::ActionUpdate() && GekkoWorld::LocoTick(1, 0) && GekkoWorld::Dispatch(1, 0) && GekkoWorld::LocoTick(3, 0) && GekkoWorld::LocoTick(5, 0);
            if (ok && doneAt < 0 && clipEnd >= 0 && (GekkoWorld::Peek(0).controllerFlags & 0x20)) { doneAt = f; break; }
        }
        std::printf("  arrival run: clip %llx length %.2f s, clip end at frame %d, action finished at frame %d, action now %u, fault: %s\n", (unsigned long long)g_clipId, g_clipLen, clipEnd, doneAt, GekkoWorld::Peek(0).action, ok ? "none" : GekkoWorld::LastFault());
        if (!restart) { if (ok && doneAt < 0) { std::printf("  FAIL: the arrival action never finished\n"); ok = false; } }
        else if (ok)
        {
            GekkoWorld::ResetUnit(0);
            static const float origin[3] = {};
            GekkoWorld::Request(0, 29, 0, origin);
            ok = GekkoWorld::InitInstance(0) && GekkoWorld::Dispatch(1, 0);   // what GekkoBrain::Start does
            for (int f = 0; f < 120 && ok; ++f) ok = GekkoWorld::ActionUpdate() && GekkoWorld::LocoTick(1, 0) && GekkoWorld::Dispatch(1, 0) && GekkoWorld::LocoTick(3, 0) && GekkoWorld::LocoTick(5, 0);
            std::printf("  after reset: action %u pending %u, fault: %s\n", GekkoWorld::Peek(0).action, GekkoWorld::Peek(0).pending, ok ? "none" : GekkoWorld::LastFault());
            if (ok && GekkoWorld::Peek(0).action != 29) { std::printf("  FAIL: the idle request was not entered after the reset\n"); ok = false; }
        }
    }
    else if (t == "ai0") ok = GekkoWorld::AiStart(0);
    else if (t == "ai") ok = GekkoWorld::AiUpdate(0);
    else if (t == "life")
    {
        GekkoLife::State l;
        if (!GekkoLife::Read(l)) { std::printf("  life: not ready\n"); ok = false; }
        else
        {
            std::printf("  life %u / %u pending %u dead %d hits %u pools", l.life, l.maxLife, l.pending, l.dead, l.hits);
            for (int p = 0; p < 5; ++p) std::printf(" [%u/%u w%u%s]", l.poolLeft[p], l.poolStart[p], l.poolWaiting[p], l.broken[p] ? " broken" : "");
            std::printf("\n");
        }
    }
    else if (t.rfind("hit=", 0) == 0) { int part = 0, dmg = 0, cls = -1; std::sscanf(t.c_str() + 4, "%d,%d,%d", &part, &dmg, &cls); ok = cls < 0 ? GekkoLife::Hit(part, dmg) : GekkoLife::Hit(part, dmg, static_cast<unsigned>(cls)); }   // hit=PART,DAMAGE[,WEAPONCLASS]
    else if (t == "lifeup") ok = GekkoLife::Update(0.016f);
    else if (t == "lifetest") ok = LifeTest();
    else if (t.rfind("region=", 0) == 0) { int part = std::atoi(t.c_str() + 7); std::printf("  region of part %d: %d (pool %d)\n", part, GekkoLife::RegionOfPart(part), GekkoLife::PoolOfRegion(GekkoLife::RegionOfPart(part))); }
    else if (t.rfind("st=", 0) == 0) { int before = -1; ok = GekkoWorld::AiTransition(0, static_cast<unsigned>(std::atoi(t.c_str() + 3)), &before); std::printf("  transition from %d\n", before); }
    else if (t.rfind("tgt=", 0) == 0) { float x = 0, y = 0, z = 0; std::sscanf(t.c_str() + 4, "%f,%f,%f", &x, &y, &z); GekkoWorld::SetTarget(x, y, z); g_tgt[0] = x; g_tgt[1] = y; g_tgt[2] = z; }
    else if (t.rfind("pose=", 0) == 0) { float x = 0, y = 0, z = 0, w = 0; std::sscanf(t.c_str() + 5, "%f,%f,%f,%f", &x, &y, &z, &w); g_pos[0] = x; g_pos[1] = y; g_pos[2] = z; g_pos[3] = w; GekkoWorld::SetPose(0, x, y, z, w); }
    else if (t.rfind("dt=", 0) == 0) GekkoWorld::SetDt(static_cast<float>(std::atof(t.c_str() + 3)));
    else if (t.rfind("fin=", 0) == 0) GekkoWorld::SetMotionFinished(t[4] == '1');
    else { std::printf("unknown token %s\n", t.c_str()); return false; }
    if (!ok) { std::printf("  FAULT %s\n  pcs:", GekkoWorld::LastFault()); lift::pc_ring_dump(16); }
    const GekkoWorld::State s = GekkoWorld::Peek(0);
    std::printf("  state: action %u pending %u eventFlags %x controllerFlags %x ai %u timer %.3f\n", s.action, s.pending, s.eventFlags, s.controllerFlags, s.aiState, s.aiTimer);
    { const std::uint8_t* o = GekkoWorld::Order(); std::printf("  order: mode %u prev %u flag %u sub %u dist %.2f target %.2f %.2f %.2f handle %04x\n", o[0xb8], o[0xb9], o[0xbb], o[0xba], *reinterpret_cast<const float*>(o + 0x90), *reinterpret_cast<const float*>(o + 0x20), *reinterpret_cast<const float*>(o + 0x24), *reinterpret_cast<const float*>(o + 0x28), *reinterpret_cast<const std::uint16_t*>(o + 0xb6)); }
    return ok;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash in lifted code must not lose the log
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not lose the output
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(240)); std::printf("  HANG: no token finished in 240 s, survive pc 0x%llx\n", (unsigned long long)lift::g_pc); std::fflush(stdout); std::_Exit(3); }).detach();   // a lifted loop that never ends must name its address
    GekkoWorld::Hooks h;
    h.trace = Trace; h.setMotion = SetMotion; h.setSpeed = SetSpeed;
    GekkoWorld::Init(h);   // the Observer setup (Begin) follows with the first token that is not a nav token
    GekkoWorld::SetDt(1.0f / 60);
    std::string prev;
    for (int i = 1; i < argc; ++i)
    {
        std::string t = argv[i];
        if (t.rfind("x=", 0) == 0) { for (int n = std::atoi(t.c_str() + 2); n > 0; --n) { std::printf("-- %s (repeat)\n", prev.c_str()); RunToken(prev); } continue; }
        std::printf("-- %s\n", t.c_str());
        prev = t;
        if (!RunToken(t) && t[0] != 'k') break;
    }
    return 0;
}