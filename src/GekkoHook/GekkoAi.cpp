// GekkoAi.cpp 01/10/2026. Per-frame glue between the game and Survive's own gekko brain (GekkoBrain: lifted AI, movement brain, locomotion, actions). The game gives the
// brain the gekko's pose and the place it is sent to; the brain's requests drive the clips, and the heading it asks for is handed to the CharaControl's own turn.
#include "GekkoAi.h"
#include "GekkoBrain.h"
#include "GekkoDriver.h"
#include "GekkoFull.h"
#include "GekkoHit.h"
#include "GekkoHitFx.h"
#include "GekkoLife.h"
#include "GekkoLog.h"
#include "GekkoNavStep.h"
#include "GekkoPush.h"
#include "GekkoRoute.h"
#include "GekkoTurnLog.h"
#include "GekkoType.h"
#include "GekkoWorld.h"

#include <cmath>

namespace
{
    float g_target[3] = {};
    bool  g_hasTarget = false;
    bool  g_interrupt = false;   // one frame: a target takes over from a route walk (GekkoWorld::SetTargetState)
    bool  g_deathHandled = false;

    // The Dead event reached the host: the brain stops, the push and hit targets stop answering, the last pose stays. Survive has no death action or clip in the gekko's action table
    // (docs/DOCUMENTATION.md 5.10), so none is played.
    void OnDeath()
    {
        g_deathHandled = true;
        GekkoLife::State l;
        GekkoLife::Read(l);
        Log("[life] DEAD: life %u of %u, %u hit(s) taken; brain, push targets and hit filter off, pose frozen\n", l.life, l.maxLife, l.hits);
        GekkoWorld::StopRouteLoop();
        GekkoPush::Stop();
        GekkoHit::SetEnabled(false);
        GekkoBrain::Stop();
        GekkoFull::Freeze(true);
    }

    // One game frame of the life side: TPP's hits since the last frame go through Survive's damage code, then Survive's life update runs. A hit on an object that is not one of the 21 hit spheres
    // (the push body) does not count.
    void LifeFrame(float dt)
    {
        GekkoHit::Record r[16];
        for (unsigned n; (n = GekkoHit::Drain(r, 16)) > 0;)
        {
            GekkoHitFx::Play(r, n);
            for (unsigned i = 0; i < n; ++i)
            {
                if (r[i].part < 0 || r[i].part >= GekkoLife::kParts || !r[i].damage[0]) continue;
                Log("[life] TPP hit on part %d, attack id %04x, ratio bytes %u %u %u flags %02x -> %u damage points\n", r[i].part, r[i].attackId, r[i].damage[0], r[i].damage[1], r[i].damage[2], r[i].flags, r[i].damage[0]);
                GekkoLife::Hit(r[i].part, r[i].damage[0]);
            }
        }
        GekkoLife::Update(dt);
        if (GekkoLife::Dead() && !g_deathHandled) OnDeath();
    }
}

namespace
{
    bool g_steerGate = true;
}

namespace GekkoAi
{
    void SetSteerGate(bool on) { g_steerGate = on; }
    void Start()
    {
        // A dead gekko stays dead for the session (no reset of the pools and break flags, reviving is open work): a start is refused, the death was logged once.
        if (GekkoLife::Dead()) { Log("[ai] start refused: the gekko is dead\n"); return; }
        g_deathHandled = false;
        GekkoHit::Clear();   // hits that arrived while no brain ran (a stopped brain drains nothing: 64 records were fed in one millisecond after the 20:16:32 fault) are stale
        GekkoHit::SetEnabled(true);
        GekkoFull::Freeze(false);
        GekkoPush::Stop();   // a start after a fault or a stop builds the push targets again
        GekkoBrain::Game g;
        g.controller = GekkoFull::Controller();
        g.trace = [](const char* line) { Log("%s\n", line); };
        const bool ok = GekkoBrain::Start(g, false);
        Log("[ai] Survive brain %s\n", ok ? "started" : "FAILED to start");
        if (ok)
        {
            GekkoPush::Hooks ph;
            ph.trace = [](const char* line) { Log("%s\n", line); };
            GekkoPush::Start(ph, GekkoWorld::RealTargetPool(), GekkoType::GameObjectId());
            GekkoRoute::Attach();
        }
    }

    void Stop() { GekkoPush::Stop(); GekkoBrain::Stop(); Log("[ai] Survive brain stopped\n"); }

    void SetTarget(float x, float y, float z)
    {
        g_target[0] = x; g_target[1] = y; g_target[2] = z;
        if (!g_hasTarget) g_interrupt = true;   // from a route walk (Route) or from no target: Survive's own route cancel (disk 0x140fa47d5) ends the route; without a route it does nothing
        g_hasTarget = true;
        GekkoWorld::StopRouteLoop();
    }

    // A route walk has no target: the target provider reports none (id -1, strength 0), so the movement brain's re-plan on a moving target (0x140fa5f85..0x140fa5fb8, gated by
    // strength > 0 at 0x140fa477b) does not cut the walk to each vertex short (with strength 1 it skipped to the next vertex every 1.5 s).
    void Route(std::uint32_t routeId, unsigned vertex)
    {
        g_hasTarget = false;
        GekkoRoute::Walk(routeId, vertex);
    }

    int GetState() { return GekkoBrain::Active() ? 9 : -1; }

    void Update(float dt)
    {
        if (GekkoLife::Ready() && !g_deathHandled) LifeFrame(dt);
        else if (!GekkoLife::Ready())   // life components exist only once the brain started; the impact effect does not depend on them
        {
            GekkoHit::Record r[16];
            for (unsigned n; (n = GekkoHit::Drain(r, 16)) > 0;) GekkoHitFx::Play(r, n);
        }
        if (!GekkoBrain::Active()) return;
        float p[4];
        Gekko::GetPose(p);
        GekkoBrain::Input in = { { p[0], p[1], p[2] }, p[3], { g_target[0], g_target[1], g_target[2] }, g_hasTarget, g_interrupt };
        g_interrupt = false;
        GekkoWorld::NavSync();   // Survive-layout copies of TPP's paths for the lifted controller (nothing to do until a controller is created)
        if (GekkoWorld::NavControllerActive()) GekkoWorld::NavUpdate(0, p[0], p[1], p[2]);   // position feed + controller Update
        GekkoBrain::Frame(dt, in);
        if (!GekkoBrain::Active())   // the brain faulted in this frame: the push targets stop with it and the body's turn request is dropped, as a stop does
        {
            GekkoPush::Stop();
            GekkoFull::SetTurn(false, 0.0f, 0.0f);
            Log("[ai] brain stopped by a fault: push targets released, turn request cleared\n");
            return;
        }
        GekkoNavStep::Apply();   // the locomotion's path correction of this frame (Survive's move step), when its switch is on
        { static int nf = 0; GekkoWorld::NavDiag(0, ++nf); }
        GekkoWorld::RouteLoopStep();   // route end: the same route again from vertex 0 (docs/DOCUMENTATION.md)
        GekkoPush::RunKind(0);   // plugin frame, serial kind: decays the target-radius tables and clears the NPC event bits the brain just read (kinds 3 and 5 need the NPC service's component registry, PLAN 5.3)
        GekkoPush::Update(in.pos);
        // The locomotion's heading request goes to the CharaControl's own turn (GekkoFull::SetTurn): the engine rotates the body toward it after the clip's root rotation, and the request stays
        // until the locomotion clears it (turn clips and idle clear it, so a turn clip's rotation is not fought).
        // Measured (log of 08/10/2026 02:46 to 02:48): the request is written only by the walk state's steering tick, and nothing in the lifted code clears it when the walk ends, so the stale target
        // (the old path direction, frame 5144) kept turning the body through the turn clip and the idle that followed: about 90 degrees of slide after the clip. It is applied only while the walk
        // state is the current locomotion state.
        float yaw = 0, n = 0;
        if (GekkoBrain::TakeSteer(&yaw, &n) && (!g_steerGate || GekkoWorld::LocoStateNow(0) == 3)) GekkoFull::SetTurn(true, yaw, n);
        else GekkoFull::SetTurn(false, 0.0f, 0.0f);
        GekkoTurnLog::Tick(dt);
    }
}
