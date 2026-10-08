// GekkoDebug.cpp 02/10/2026. Debug options and part-by-part calls into the lifted Survive code; each call drives the real lifted function and logs the result.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "GekkoDebug.h"
#include "GekkoBrain.h"
#include "GekkoDriver.h"
#include "GekkoHitFx.h"
#include "GekkoLife.h"
#include "GekkoLog.h"
#include "GekkoPush.h"
#include "GekkoWorld.h"

namespace
{
    bool g_navController = true;   // default on: without the controller the nav path is a straight line and nothing is avoided (the Lua option overrides it)

    void Pose(float* p) { Gekko::GetPose(p); }

    // Life, damage and death state of the lifted life class and LifeController: one [snap] block line plus the five pools.
    void LifeLines()
    {
        GekkoLife::State l;
        if (!GekkoLife::Read(l)) { Log("[snap] life: components not built\n"); return; }
        static const char* const kPool[5] = { "rightLeg", "leftLeg", "rightGun", "leftLauncher", "camera" };
        Log("[snap] life %u of %u, damage waiting %u, dead %d (Dead event %s), %u hit(s) taken\n", l.life, l.maxLife, l.pending, l.dead ? 1 : 0, GekkoLife::Dead() ? "sent" : "not sent", l.hits);
        for (int p = 0; p < 5; ++p) Log("[snap]   pool %-12s %u of %u%s\n", kPool[p], l.poolLeft[p], l.poolStart[p], l.broken[p] ? " BROKEN" : "");
    }
}

namespace GekkoDebug
{
    void SetNavController(bool on)
    {
        g_navController = on;
        Log("[debug] nav controller option %s (takes effect at the next brain start)\n", on ? "ON" : "off");
    }
    bool NavController() { return g_navController; }

    bool Action(unsigned id, unsigned flag, const float* t)
    {
        if (!GekkoBrain::Active()) { Log("[debug] action %u refused: brain not running\n", id); return false; }
        if (id > 31) { Log("[debug] action %u refused: range 0..31\n", id); return false; }
        const GekkoWorld::State b = GekkoWorld::Peek(0);
        GekkoBrain::Request(id, flag, t);
        const GekkoWorld::State a = GekkoWorld::Peek(0);
        Log("[debug] action request id %u flag %u target %.2f %.2f %.2f: action %u -> %u pending %u -> %u (the frame dispatch enters it)\n", id, flag, t[0], t[1], t[2], b.action, a.action, b.pending, a.pending);
        return true;
    }

    bool State(unsigned n)
    {
        if (!GekkoBrain::Active()) { Log("[debug] state %u refused: brain not running\n", n); return false; }
        if (n > 12) { Log("[debug] state %u refused: range 0..12\n", n); return false; }
        int before = -1;
        const bool ok = GekkoWorld::AiTransition(0, n, &before);
        Log("[debug] ai state %d -> %u: exit handler of %d, enter handler of %u %s (state now %d)%s%s\n", before, n, before, n, ok ? "ran" : "FAILED",
            GekkoBrain::AiState(), ok ? "" : ", fault: ", ok ? "" : GekkoWorld::LastFault());
        return ok;
    }

    int Choose()
    {
        if (!GekkoBrain::Active()) { Log("[debug] choose refused: brain not running\n"); return -1; }
        const int r = GekkoWorld::PickState(0);
        Log("[debug] state chooser 0x140f8d510 -> %d (fault: %s)\n", r, r < 0 ? GekkoWorld::LastFault() : "none");
        return r;
    }

    bool Event(std::uint32_t hash, unsigned param)
    {
        if (!GekkoBrain::Active()) { Log("[debug] event %08x refused: brain not running\n", hash); return false; }
        const bool ok = GekkoWorld::FireEvent(0, hash, param);
        Log("[debug] event %08x param %u -> %s, event flags now %x\n", hash, param, ok ? "ok" : GekkoWorld::LastFault(), GekkoWorld::Peek(0).eventFlags);
        return ok;
    }

    bool Dispatch()
    {
        if (!GekkoBrain::Active()) { Log("[debug] dispatch refused: brain not running\n"); return false; }
        const bool ok = GekkoWorld::Dispatch(1, 0);
        const GekkoWorld::State s = GekkoWorld::Peek(0);
        Log("[debug] dispatch kind 1 -> %s, action %u pending %u\n", ok ? "ok" : GekkoWorld::LastFault(), s.action, s.pending);
        return ok;
    }

    bool Melee(const float* t)
    {
        if (!GekkoBrain::Active()) { Log("[debug] melee refused: brain not running\n"); return false; }
        const bool ok = GekkoBrain::Melee(t);
        Log("[debug] melee chooser -> %s, now action %u pending %u\n", ok ? "ok" : "fault", GekkoWorld::Peek(0).action, GekkoWorld::Peek(0).pending);
        return ok;
    }

    void Snapshot(const char* label)
    {
        float p[4] = {};
        Pose(p);
        Log("[snap] ===== %s\n", label ? label : "");
        if (!GekkoBrain::Active())
        {
            Log("[snap] brain not running; pos %.2f %.2f %.2f yaw %.3f\n", p[0], p[1], p[2], p[3]);
            LifeLines();
            Log("[snap] =====\n");
            return;
        }
        const GekkoWorld::State s = GekkoWorld::Peek(0);
        const std::uint8_t* o = GekkoWorld::Order();
        std::uint32_t mode, route; std::uint16_t vert;
        std::memcpy(&mode, o + 0xb0, 4); route = mode; std::memcpy(&vert, o + 0xb4, 2);
        Log("[snap] action %u pending %u ai %u timer %.2f loco %u\n", s.action, s.pending, s.aiState, s.aiTimer, GekkoWorld::LocoStateNow(0));
        Log("[snap] event flags %08x controller flags %04x busy %d\n", s.eventFlags, s.controllerFlags, GekkoBrain::Busy() ? 1 : 0);
        Log("[snap] order +0xb0 %08x (mode hash or route id) +0xb4 %u +0xb8 mode byte %u\n", route, vert, o[0xb8]);
        Log("[snap] pos %.2f %.2f %.2f yaw %.3f\n", p[0], p[1], p[2], p[3]);
        Log("[snap] nav controller option %s, controller %s; push targets %s\n", g_navController ? "ON" : "off", GekkoWorld::NavControllerActive() ? "active" : "not started", GekkoPush::Active() ? "active" : "off");
        LifeLines();
        Log("[snap] =====\n");
    }

    bool Damage(int amount, int part)
    {
        if (part < 0 || part >= GekkoLife::kParts) { Log("[debug] damage refused: part %d out of range 0..%d\n", part, GekkoLife::kParts - 1); return false; }
        if (amount <= 0) { Log("[debug] damage refused: amount %d\n", amount); return false; }
        GekkoLife::State b, a;
        if (!GekkoLife::Read(b)) { Log("[debug] damage refused: life components not built (start the brain first)\n"); return false; }
        const bool ok = GekkoLife::Hit(part, amount) && GekkoLife::Update(1.0f / 60);
        GekkoLife::Read(a);
        unsigned bm = 0, am = 0;
        for (int p = 0; p < 5; ++p) { bm |= b.broken[p] ? 1u << p : 0u; am |= a.broken[p] ? 1u << p : 0u; }
        Log("[debug] damage %d on part %d (region %d): life %u -> %u, dead %d -> %d, broken pools mask %u -> %u%s\n", amount, part, GekkoLife::RegionOfPart(part), b.life, a.life, b.dead ? 1 : 0, a.dead ? 1 : 0, bm, am, ok ? "" : " (FAILED)");
        return ok;
    }
    bool Impact(int part, const float* player)
    {
        if (part < 0 || part >= GekkoLife::kParts) { Log("[debug] impact refused: part %d out of range 0..%d\n", part, GekkoLife::kParts - 1); return false; }
        float p[4];
        Pose(p);
        // The sphere centres are not known on the host side, so the point is 1.5 m above the feet and 1 m from the gekko's axis towards the player; the bullet flies from the player to that point.
        float d[3] = { p[0] - player[0], p[1] + 1.5f - player[1], p[2] - player[2] };
        const float hl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (hl < 0.01f || l < 0.5f) { Log("[debug] impact refused: player too close to the gekko pose\n"); return false; }
        const float hit[3] = { p[0] - d[0] / hl, p[1] + 1.5f, p[2] - d[2] / hl };
        for (float& v : d) v /= l;
        Log("[debug] impact on part %d at %.2f %.2f %.2f (gekko feet %.2f %.2f %.2f, player %.2f %.2f %.2f)\n", part, hit[0], hit[1], hit[2], p[0], p[1], p[2], player[0], player[1], player[2]);
        return GekkoHitFx::Debug(part, hit, d);
    }
}
