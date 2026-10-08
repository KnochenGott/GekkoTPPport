// nav_proxy_test.cpp 02/10/2026. Headless test of src/GekkoHook/GekkoNavProxy.cpp: mock TPP S, B, P with the retail slot order, Survive-side calls through the proxies, the Path2Impl
// conversion checked by Survive's own lifted Path2Impl code (vtable slot 6 0x141bfb840, a length between two progress positions that sums the w lane of the points), and the lifted
// controller (creator 0x141408ca0, Initialize 0x141409320) running against the proxies. Run: world_test.exe nav
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "GekkoWorld.h"
#include "GekkoNavProxy.h"

namespace
{
    int g_fail = 0;
    void Check(bool ok, const char* what) { std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++g_fail; }

    // TPP layouts: Path2Impl count +0x20, points +8, segments +0x10 stride 0x40 (flag dword +0x30), flag bytes +0x18.
    struct TppPath { void* vtbl; void* pts; void* seg; void* flg; std::uint32_t count; std::uint32_t pad; };
    alignas(16) float g_pts[5][4] = { { 0, 0, 0, 1 }, { 1, 0, 0, 2 }, { 3, 0, 0, 3 }, { 6, 0, 0, 4 }, { 10, 0, 0, 0 } };   // w = length to the next point
    alignas(16) std::uint8_t g_seg[5][0x40]; std::uint8_t g_flg[5] = { 1, 1, 1, 1, 1 };
    TppPath g_tpath = { nullptr, g_pts, g_seg, g_flg, 5, 0 };

    std::uint64_t g_lastThis = 0; std::uint64_t g_reqId = 0; const void* g_reqFrom = nullptr; bool g_deleted = false;
    std::uint8_t g_table[8][0x10]; bool g_autoFill = false;
    // G_delay > 0 models the real pathfinder (TPP 0x141cc7500 / 0x141cc7410): entry +0 is a per-id holder that exists from creation (count 0), Request sets +0xa bits 0 and 1 and
    // leaves +9 alone (1 from creation), the result comes g_delay frames later (+9 = 3, holder filled, bit 0 cleared) and bit 1 stays set until Release.
    int g_delay = 0, g_wait[8]; TppPath g_holder[8];
    alignas(16) std::uint8_t g_mockB[0x40];
    void* g_bVt[6]; void* g_sVt[14]; void* g_qVt[24];
    alignas(16) std::uint8_t g_mockS[0x40], g_mockP[0x120], g_mockQ[0x20], g_mockX[0x20], g_mockT[0xb0];

    template <int K> std::uint64_t SlotS(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); return 0x1000 + K; }
    template <int K> std::uint64_t SlotB(void* self, std::uint64_t, std::uint64_t, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); return 0x2000 + K; }
    void* MockCreate(void*, const void*) { return g_mockB; }
    void MockDelete(void*, void*) { g_deleted = true; }
    std::uint64_t MockRequest(void* self, std::uint64_t id, const void* from, const void*) { g_lastThis = reinterpret_cast<std::uint64_t>(self); g_reqId = id; g_reqFrom = from; return 0x2004; }
    std::uint64_t MockXSlot0(void*) { return 0x3000; }
    void* g_xVt[4] = { (void*)&MockXSlot0 };
    std::uint64_t MockType(void* self) { g_lastThis = reinterpret_cast<std::uint64_t>(self); return 1; }

    void FillS() { g_sVt[2] = (void*)&SlotS<2>; g_sVt[3] = (void*)&SlotS<3>; g_sVt[4] = (void*)&SlotS<4>; g_sVt[5] = (void*)&SlotS<5>; g_sVt[6] = (void*)&SlotS<6>; g_sVt[7] = (void*)&SlotS<7>;
        g_sVt[8] = (void*)&SlotS<8>; g_sVt[9] = (void*)&SlotS<9>; g_sVt[10] = (void*)&SlotS<10>; g_sVt[11] = (void*)&SlotS<11>; g_sVt[12] = (void*)&SlotS<12>; g_sVt[13] = (void*)&SlotS<13>;
        g_sVt[0] = (void*)&MockCreate; g_sVt[1] = (void*)&MockDelete; }
    // Slot 0 of B is the controller's Request(id, request struct): with g_autoFill the mock answers like TPP's pathfinder after its work is done (Request sets entry +0xa bits 0 and 1; NavMockStep then completes it like ReceiveResult and Update: +9 = 3 found with the path, bit 0 cleared, a step later bit 1).
    bool g_line = false;   // token navline: the mock path is the straight line from the request's start to its goal (the default path is fixed, and a unit that leaves it makes the controller cancel and request again every frame)
    std::uint64_t MockB0(void* self, std::uint64_t id, std::uint64_t req, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); if (g_line && req) { const float* r = reinterpret_cast<const float*>(req); float d = 0; for (int i = 0; i < 3; ++i) d += (r[4 + i] - r[i]) * (r[4 + i] - r[i]); std::memset(g_pts, 0, sizeof(g_pts)); for (int i = 0; i < 3; ++i) { g_pts[0][i] = r[i]; g_pts[1][i] = r[4 + i]; } g_pts[0][3] = std::sqrt(d); g_tpath.count = 2; } if (g_autoFill) { std::uint8_t* e = g_table[id & 7]; if (g_delay > 0) { g_holder[id & 7] = TppPath{ nullptr, g_pts, g_seg, g_flg, 0, 0 }; *reinterpret_cast<void**>(e) = &g_holder[id & 7]; e[0xa] |= 3; g_wait[id & 7] = g_delay; } else { *reinterpret_cast<void**>(e) = nullptr; e[9] = 0; e[0xa] = 3; } } return 0x2000; }
    // TPP Release (0x141cc6da0: clears +0xa bits 0 and 1) and Reset (0x141cc6e30: +9 = 1) instead of no-ops, so the controller's own releases show in the mock.
    std::uint64_t MockRelease(void* self, std::uint64_t id, std::uint64_t, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); g_table[id & 7][0xa] &= 0xfc; return 0; }
    std::uint64_t MockReset(void* self, std::uint64_t id, std::uint64_t, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); g_table[id & 7][9] = 1; return 0; }
    // ReceiveResult (TPP Pathfinder slot 3): state 3 (goal reached), bit 0 cleared, returns the result path (log 06/10 23:39:17: entry+8 ...010100 -> ...000300)
    std::uint64_t MockReceive(void* self, std::uint64_t id, std::uint64_t, std::uint64_t) { g_lastThis = reinterpret_cast<std::uint64_t>(self); std::uint8_t* e = g_table[id & 7]; e[9] = 3; e[0xa] &= static_cast<std::uint8_t>(~1); return reinterpret_cast<std::uint64_t>(&g_holder[id & 7]); }
    void FillB() { g_bVt[0] = (void*)&MockB0; g_bVt[1] = (void*)&MockRelease; g_bVt[2] = (void*)&MockReset; g_bVt[3] = (void*)&MockReceive; g_bVt[4] = (void*)&MockRequest; g_bVt[5] = (void*)&SlotB<5>; }
}

static void MockSetup()
{
    for (int i = 0; i < 5; ++i) std::memset(g_seg[i], 0, 0x40);
    *reinterpret_cast<std::uint32_t*>(g_seg[2] + 0x30) = 7;   // segment 2 carries the flag dword
    FillS(); FillB(); g_qVt[18] = (void*)&MockType;
    *reinterpret_cast<void**>(g_mockS) = g_sVt; *reinterpret_cast<void**>(g_mockB) = g_bVt; *reinterpret_cast<void**>(g_mockQ) = g_qVt; *reinterpret_cast<void**>(g_mockX) = g_xVt;
    *reinterpret_cast<void**>(g_mockB + 8) = g_table;
    for (int i = 0; i < 16; ++i) g_mockB[0x10 + i] = static_cast<std::uint8_t>(0xA0 + i);   // the embedded WorldHandle
    for (int i = 0; i < 4; ++i) { std::memset(g_table[i], 0, 0x10); g_table[i][9] = 1; }
    *reinterpret_cast<void**>(g_table[3]) = &g_tpath; g_table[3][0xa] = 3;
    *reinterpret_cast<void**>(g_mockP + 8) = g_mockX; *reinterpret_cast<void**>(g_mockP + 0x100) = g_mockQ;
    *reinterpret_cast<void**>(g_mockT + 0xa0) = g_mockS; *reinterpret_cast<void**>(g_mockT + 0x98) = g_mockP;

}

// One frame of the mock pathfinder: pending entries (+0xa bit 0) get their result (+9 = 3, path), then the bit 1 (busy) is cleared one step later.
int g_force = -1;   // token navforce=N: sets entry 0 flags byte (+0xa) to N once, standing in for whatever clears the real entry in the game
void NavMockForce(int v) { g_force = v; }
void NavMockStep()
{
    if (g_force >= 0) { g_table[0][0xa] = static_cast<std::uint8_t>(g_force); g_force = -1; }
    for (auto& e : g_table)
    {
        if (!g_autoFill) break;
        const int i = static_cast<int>(&e - &g_table[0]);
        if (g_delay > 0) { if ((e[0xa] & 2) && --g_wait[i] <= 0) { g_holder[i] = g_tpath; e[0xa] &= static_cast<std::uint8_t>(~2); } continue; }   // the search ends: bit 1 (busy) clears and bit 0 stays for the controller's receive (Gekko.log 06/10 23:39:17: entry+8 ...010100 before the receive)
        if (e[0xa] & 1) { *reinterpret_cast<void**>(e) = &g_tpath; e[9] = 3; e[0xa] &= static_cast<std::uint8_t>(~1); }
        else if (e[0xa] & 2) e[0xa] &= static_cast<std::uint8_t>(~2);
    }
}

// Token navdetour: the mock path goes around an obstacle on the way from (0,0) to (10,0): corners at (3,3) and (7,3) (x, z), w lane = length to the next point.
void NavMockDetour()
{
    const float p[4][4] = { { 0, 0, 0, 4.2426f }, { 3, 0, 3, 4.0f }, { 7, 0, 3, 4.2426f }, { 10, 0, 0, 0 } };
    std::memset(g_pts, 0, sizeof(g_pts));
    std::memcpy(g_pts, p, sizeof(p));
    g_tpath.count = 4;
}

// Mock TPP world + proxies + the lifted controller as [svc+0x60], for the brain tokens of world_test (nav4).
void NavMockDelay(int frames) { g_delay = frames; }
void NavMockLine(bool on) { g_line = on; }
bool NavMockStart()
{
    MockSetup();
    g_autoFill = true;
    if (g_delay == 0) g_delay = 1;   // the receive protocol of the real pathfinder (bit 1 clears when the search ends, the controller receives and clears bit 0)
    return GekkoWorld::StartNav(g_mockT, reinterpret_cast<std::uint64_t>(g_sVt), reinterpret_cast<std::uint64_t>(g_bVt)) && GekkoWorld::StartNavController(8);
}

bool NavProxyTest()
{
    g_fail = 0;
    MockSetup();
    // wrong S vtable: Init must refuse
    Check(!GekkoWorld::StartNav(g_mockT, 0x1234, reinterpret_cast<std::uint64_t>(g_bVt)), "S vtable mismatch is refused");
    Check(GekkoWorld::StartNav(g_mockT, reinterpret_cast<std::uint64_t>(g_sVt), reinterpret_cast<std::uint64_t>(g_bVt)), "proxies built");

    alignas(16) std::uint8_t scope[0xb0] = {};
    NavProxy::FillScope(scope);
    auto** sp = *reinterpret_cast<void***>(scope + 0xa0);
    void** sv = *reinterpret_cast<void***>(sp);
    using F4 = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
    // Survive slot k -> TPP slot k (2..5), Survive k -> TPP k-1 (7..14)
    Check(reinterpret_cast<F4>(sv[3])(sp, 0, 0, 0) == 0x1003 && g_lastThis == reinterpret_cast<std::uint64_t>(g_mockS), "S slot 3 -> TPP slot 3 on the real S");
    Check(reinterpret_cast<F4>(sv[5])(sp, 0, 0, 0) == 0x1005, "S slot 5 -> TPP slot 5");
    Check(reinterpret_cast<F4>(sv[7])(sp, 0, 0, 0) == 0x1006, "S slot 7 -> TPP slot 6");
    Check(reinterpret_cast<F4>(sv[14])(sp, 0, 0, 0) == 0x100d, "S slot 14 -> TPP slot 13");
    // P
    auto** pp = *reinterpret_cast<void***>(scope + 0x98);
    Check(*reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(pp) + 8) == g_mockX, "P+8 is the real object");
    auto** qq = *reinterpret_cast<void***>(reinterpret_cast<std::uint8_t*>(pp) + 0x110);
    using F1 = std::uint64_t (*)(void*);
    Check(reinterpret_cast<F1>((*reinterpret_cast<void***>(qq))[29])(qq) == 1 && g_lastThis == reinterpret_cast<std::uint64_t>(g_mockQ), "Q slot 29 (+0xe8) -> real Q slot 18 (+0x90)");

    // CreatePathfinder through the proxy
    std::uint32_t desc[10] = { 4 };
    auto* b = reinterpret_cast<std::uint8_t* (*)(void*, const void*)>(sv[0])(sp, desc);
    Check(b && b != g_mockB, "CreatePathfinder returns a proxy, not the real object");
    void** bv = *reinterpret_cast<void***>(b);
    const float from[4] = { 1, 2, 3, 0 };
    Check(reinterpret_cast<std::uint64_t (*)(void*, std::uint64_t, const void*, const void*)>(bv[4])(b, 3, from, from) == 0x2004 && g_lastThis == reinterpret_cast<std::uint64_t>(g_mockB) && g_reqId == 3 && g_reqFrom == from, "B slot 4 request forwards (this swapped, args kept)");

    // Sync + Path2Impl conversion
    GekkoWorld::NavSync();
    const std::uint8_t* tbl = *reinterpret_cast<std::uint8_t**>(b + 8);
    std::uint8_t* path = *reinterpret_cast<std::uint8_t**>(const_cast<std::uint8_t*>(tbl) + 3 * 0x10);
    Check(path && tbl[3 * 0x10 + 0xa] == 3 && tbl[3 * 0x10 + 9] == 1, "table entry 3 copied with its flags");
    Check(!*reinterpret_cast<void**>(const_cast<std::uint8_t*>(tbl)), "empty entry 0 stays empty");
    Check(std::memcmp(b + 0x10, g_mockB + 0x10, 0x10) == 0, "WorldHandle copied to B'+0x10");
    if (path)
    {
        Check(*reinterpret_cast<void**>(path) != nullptr, "Survive constructor set the vtable");
        Check(*reinterpret_cast<std::uint32_t*>(path + 0x28) == 5, "count at +0x28");
        Check(std::memcmp(*reinterpret_cast<void**>(path + 8), g_pts, sizeof(g_pts)) == 0, "points at +8 equal");
        const std::uint8_t* sg = *reinterpret_cast<std::uint8_t**>(path + 0x10);
        Check(*reinterpret_cast<const std::uint32_t*>(sg + 2 * 0x30 + 0x28) == 7 && *reinterpret_cast<const std::uint32_t*>(sg + 1 * 0x30 + 0x28) == 0 && *reinterpret_cast<const std::uint32_t*>(sg + 3 * 0x30 + 0x28) == 0, "segment flag dword TPP +0x30 stride 0x40 -> Survive +0x28 stride 0x30");
        // Survive's own Path2Impl code on the converted object: vtable slot 6 (0x141bfb840): length between two {index, fraction} positions
        struct Pos { std::uint32_t idx; float frac; };
        alignas(16) Pos a = { 0, 0.0f }, c = { 4, 0.0f }, d = { 1, 0.5f };
        bool ran = GekkoWorld::CallAddr(0x141bfb840ull, reinterpret_cast<std::uint64_t>(path), reinterpret_cast<std::uint64_t>(&a), reinterpret_cast<std::uint64_t>(&c), 0, nullptr);
        Check(ran && GekkoWorld::LastFloat() == 10.0f, "lifted Path2Impl length(0:0 -> 4:0) = 10 on the converted path");
        ran = GekkoWorld::CallAddr(0x141bfb840ull, reinterpret_cast<std::uint64_t>(path), reinterpret_cast<std::uint64_t>(&d), reinterpret_cast<std::uint64_t>(&c), 0, nullptr);
        Check(ran && GekkoWorld::LastFloat() == 8.0f, "lifted Path2Impl length(1:0.5 -> 4:0) = 8");
        std::printf("  lifted length results: %.3f (fault: %s)\n", GekkoWorld::LastFloat(), GekkoWorld::LastFault());
    }
    // the lifted controller against the proxies, created the way the game path does it (GekkoWorld::StartNavController: creator 0x141408ca0, Initialize 0x141409320, [svc+0x60])
    bool ok = GekkoWorld::StartNavController(8);
    Check(ok, "StartNavController: creator + Initialize run against S'/P' and become [svc+0x60]");
    Check(GekkoWorld::NavControllerActive(), "controller active");
    if (ok)
    {
        // Update with no request: must run through the lifted per-unit loop without a fault
        Check(GekkoWorld::NavUpdate(0, 1.0f, 2.0f, 3.0f), "NavUpdate (no request) runs: Update 0x141409f10");
        std::printf("  NavUpdate: %s\n", GekkoWorld::LastFault());
    }
    std::printf("nav proxy test: %d failure(s)\n", g_fail);
    return g_fail == 0;
}
