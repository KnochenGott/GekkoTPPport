// life_test.cpp 06/10/2026. Headless: a sequence of synthetic TPP hits through GekkoLife (Survive's lifted damage and life code over the host service) against the values the damage rule gives.
// The rule is the one read from the code (docs/DOCUMENTATION.md 5.8), written out here independently of the lifted functions:
//  - region -> defense kind (typed DamageHandler 0x140f8f420, jump table 0x140f8f5e8): 1 weak, 2 and 7 locket, 4 and 5 leg, 6 gun use the kind's defense word, 0 and 3 the unit defense;
//    a bullet row (flag 0x4000, not 0x800) adds bullet_defense (0x140f8f4ee..0x140f8f507);
//  - calculator 0x140f509e0: with a weapon class the hit is scaled to min(100, max(1, impact * 100 / defense)) percent (0x140f50e49..0x140f50eb1), without one it is reduced by
//    clamp(defense / 10, 0, 100) percent (0x140f51073..0x140f510c0); one result sum is capped at 0x3fff;
//  - pools: the damage dealt is taken from the pool of the region (0x140f8f230: region 5 -> 0, 4 -> 1, 6 -> 2, 7 -> 3, 2 -> 4); a pool at 0 is broken;
//  - life: the update takes the waiting damage off the life, at 0 the unit is dead and the Dead event goes out once (0x1410420e0, 0x1410414a0).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "GekkoLife.h"
#include "GekkoParam.h"

namespace
{
    int g_fail = 0, g_checks = 0;

    // Only failures print; every step prints one summary line.
    void Check(bool ok, const char* what, long long got, long long want)
    {
        ++g_checks;
        if (!ok) { std::printf("  [FAIL] %s: %lld, expected %lld\n", what, got, want); ++g_fail; }
    }

    std::uint8_t g_param[0x1b0];
    unsigned W(int off) { std::uint16_t v; std::memcpy(&v, g_param + off, 2); return v; }
    unsigned D(int off) { std::uint32_t v; std::memcpy(&v, g_param + off, 4); return v; }

    constexpr int kImpact = 1200;                          // the impact GekkoLife gives a TPP row (GekkoLife.cpp kTppImpact)
    // region of each of the 21 hit spheres: research/gekko_part_regions.tsv of the hit-health round 7 (the lifted mapper 0x140f7f030 emulated on the sphere names)
    constexpr int kRegion[21] = { 3, 0, 0, 3, 3, 3, 4, 5, 4, 5, 4, 5, 2, 1, 0, 0, 0, 6, 7, 7, 6 };

    int PoolOf(int region) { switch (region) { case 5: return 0; case 4: return 1; case 6: return 2; case 7: return 3; case 2: return 4; default: return -1; } }
    unsigned DefenseOf(int region, bool bullet)
    {
        unsigned d = D(0x124);                              // unit defense
        switch (region) { case 1: d = W(0x138); break; case 4: case 5: d = W(0x13a); break; case 6: d = W(0x13c); break; case 2: case 7: d = W(0x13e); break; default: break; }
        return d + (bullet ? W(0x148) : 0);
    }
    int Damage(int amount, int region, unsigned weaponClass)
    {
        const int def = static_cast<int>(DefenseOf(region, true));
        int dmg;
        if (weaponClass) { const int pct = std::min(100, std::max(1, kImpact * 100 / def)); dmg = amount * pct / 100; }
        else { const int red = std::min(100, std::max(0, def / 10)); dmg = amount * (100 - red) / 100; }
        return std::min(dmg, 0x3fff);
    }

    struct Model
    {
        long long life = 0; int pool[5] = {}; bool dead = false;
        void Hit(int part, int amount, unsigned cls)
        {
            if (dead) return;   // a dead unit takes no hit (0x141086f17)
            const int region = kRegion[part], dmg = Damage(amount, region, cls), p = PoolOf(region);
            if (p >= 0 && dmg > 0) pool[p] = std::max(0, pool[p] - dmg);
            life = std::max(0LL, life - dmg);
            dead = life == 0;
        }
    };

    void Compare(const Model& m, const char* what)
    {
        GekkoLife::State s;
        GekkoLife::Read(s);
        char b[96];
        std::snprintf(b, sizeof b, "%s: life", what); Check(s.life == m.life, b, s.life, m.life);
        for (int p = 0; p < 5; ++p)
        {
            std::snprintf(b, sizeof b, "%s: pool %d left", what, p); Check(s.poolLeft[p] == m.pool[p], b, s.poolLeft[p], m.pool[p]);
            std::snprintf(b, sizeof b, "%s: pool %d broken", what, p); Check(s.broken[p] == (m.pool[p] == 0), b, s.broken[p], m.pool[p] == 0);
        }
    }

    bool Step(Model& m, int part, int amount, unsigned cls, const char* what)
    {
        const bool ok = GekkoLife::Hit(part, amount, cls) && GekkoLife::Update(1.0f / 60);
        m.Hit(part, amount, cls);
        Check(ok, what, ok, 1);
        GekkoLife::State s;
        GekkoLife::Read(s);
        std::printf("  step %-60s life %5u (model %5lld)  pools left %4u %4u %4u %4u %4u\n", what, s.life, m.life, s.poolLeft[0], s.poolLeft[1], s.poolLeft[2], s.poolLeft[3], s.poolLeft[4]);
        Compare(m, what);
        return ok;
    }
}

bool LifeTest()
{
    GekkoParam::Fill(g_param);
    GekkoLife::State s;
    if (!GekkoLife::Read(s)) { std::printf("  life test: the life components are not built\n"); return false; }
    Model m;
    m.life = D(0x120);
    for (int p = 0; p < 5; ++p) m.pool[p] = static_cast<int>(W(0x128 + 2 * p));
    Check(s.maxLife == D(0x120), "max life from the parameter block", s.maxLife, D(0x120));
    Compare(m, "start");
    for (int i = 0; i < 21; ++i)
    {
        char b[48]; std::snprintf(b, sizeof b, "region of sphere %d", i);
        Check(GekkoLife::RegionOfPart(i) == kRegion[i], b, GekkoLife::RegionOfPart(i), kRegion[i]);
    }
    Check(!GekkoLife::Dead(), "Dead event not sent yet", GekkoLife::Dead(), 0);

    const unsigned cls = GekkoLife::kTppWeaponClass;
    Step(m, 0, 100, cls, "waist, 100 points (unit defense + bullet defense)");
    Step(m, 7, 100, cls, "right thigh, 100 points (leg defense, right leg pool)");
    Step(m, 13, 100, cls, "weak point, 100 points (weak defense)");
    Step(m, 17, 1000, cls, "right gun, 1000 points (breaks the right arm pool)");
    Step(m, 12, 100, cls, "camera, 100 points");
    Step(m, 12, 100, cls, "camera again (breaks the camera pool)");
    Step(m, 18, 100, cls, "left launcher, 100 points");
    Step(m, 0, 100, 0, "waist without weapon class (defense / 10 percent off)");
    Step(m, 0, 65535, cls, "waist, 65535 points (one result sum is capped)");
    int guard = 0;
    while (!m.dead && guard++ < 20) Step(m, 13, 65535, cls, "weak point, 65535 points");
    GekkoLife::State e;
    GekkoLife::Read(e);
    Check(e.dead, "dead byte", e.dead, 1);
    Check(GekkoLife::Dead(), "Dead event sent", GekkoLife::Dead(), 1);
    Step(m, 0, 100, cls, "hit after death changes nothing");
    Check(GekkoLife::DeadEvents() == 1, "Dead events sent in all", GekkoLife::DeadEvents(), 1);
    std::printf("  life test: %s (%d of %d check(s) failed)\n", g_fail ? "FAILED" : "LIFE_TEST_OK", g_fail, g_checks);
    return g_fail == 0;
}
