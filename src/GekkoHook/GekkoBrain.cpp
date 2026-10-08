// GekkoBrain.cpp 01/10/2026. See GekkoBrain.h. Order per frame: AI state machine, movement brain, locomotion, then the Realized-side dispatch (poll requests, exit/enter/update of the action). The order
// follows the data flow (producers before consumers); the host's own tick function 0x141075xxx was read but its component order is not mapped to the lifted classes. Clip state comes from the real controller
// (GekkoWorld::SetController): finished, play time and the animation events are the engine's own.
#include "GekkoBrain.h"
#include "GekkoDebug.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "GekkoFull.h"
#include "GekkoSigCheck.h"
#include "GekkoTable.h"
#include "GekkoWorld.h"

namespace
{
    GekkoBrain::Game g_game;
    bool g_active = false;
    char g_status[160];

    void Trace(const char* fmt, ...)
    {
        if (!g_game.trace) return;
        char b[256];
        va_list a; va_start(a, fmt); std::vsnprintf(b, sizeof(b), fmt, a); va_end(a);
        g_game.trace(b);
    }

    // Only reached without a real controller (headless tests): the host plays the clip.
    void OnSetMotion(std::uint64_t gani, float blendFrames)
    {
        Trace("[brain] set motion gani %llx blend %.0f frames", static_cast<unsigned long long>(gani), blendFrames);
        if (g_game.play) g_game.play(gani, blendFrames);
    }

    // The world traces every record change; the game log only gets the events (record/request/ctlrec diffs are for the headless tests).
    void OnTrace(const char* line)
    {
        if (!std::strncmp(line, "[record]", 8) || !std::strncmp(line, "[ctlrec]", 8) || !std::strncmp(line, "[request] +", 11)) return;
        Trace("%s", line);
    }
    // Survive's geo service goes to TPP's Geo::CollisionLineCheck (signature verified at load by GekkoSig).
    int OnLineCheck(void* control, std::uint32_t flags, const void* from, const void* to, float radius)
    {
        using Line_t = int (__fastcall*)(void*, std::uint32_t, const void*, const void*, float);
        static const auto line = reinterpret_cast<Line_t>(GekkoSig::CheckByName("Geo_CollisionLineCheck", "brain"));
        if (!line) return 0;
        // Survive's flags are 0x4000500, TPP's own callers pass 0x4000700 / 0x700: the 0x200 bit is added (not verified meaning). The filter qword at +0 is Survive's (0x1000000000020 in the
        // probes); whether its groups mean the same in TPP is measured by the first results logged below.
        // The lifted code keeps its vectors and the control struct on the emulated stack, which is not 16-byte aligned in host memory (TPP's check uses movaps on them; 04:04:
        // fault at 0x141b9b19e). Aligned copies in, the control copied back out. Size = 0x70 header + (capacity + 1) results of 0x60, the capacity being the high word of +0x68.
        alignas(16) static std::uint8_t buf[0x70 + 128 * 0x60];
        alignas(16) static float a[4], b[4];
        const std::uint8_t* c0 = static_cast<const std::uint8_t*>(control);
        const std::size_t size = 0x70 + (static_cast<std::size_t>(*reinterpret_cast<const std::uint16_t*>(c0 + 0x6a)) + 1) * 0x60;
        if (size > sizeof(buf)) return 0;
        std::memcpy(buf, control, size); std::memcpy(a, from, 16); std::memcpy(b, to, 16);
        const int hits = line(buf, flags | 0x700u, a, b, radius);
        std::memcpy(control, buf, size);
        // the first 12 rays logged were all misses; the blocked bit (order record +0x18 bit 41) comes from a hit, so log the first hits too, with the ray ends.
        static int hitLines = 0;
        if (hits > 0 && hitLines++ < 16)
        {
            const float* f = static_cast<const float*>(from); const float* t = static_cast<const float*>(to);
            Trace("[geo] HIT %d flags %x from %.2f %.2f %.2f to %.2f %.2f %.2f radius %.2f", hits, flags, f[0], f[1], f[2], t[0], t[1], t[2], radius);
        }
        static int n = 0;
        if (n++ < 12)
        {
            const std::uint8_t* c = static_cast<const std::uint8_t*>(control);
            Trace("[geo] line check flags %x filter %llx +18 %x -> %d hits (count %u)", flags, *reinterpret_cast<const unsigned long long*>(c), *reinterpret_cast<const unsigned*>(c + 0x18), hits, *reinterpret_cast<const unsigned*>(c + 0x60));
        }
        return hits;
    }
}
namespace GekkoBrain
{
    bool Start(const Game& game, bool spawnSequence)
    {
        g_game = game;
        GekkoWorld::Hooks h;
        h.trace = OnTrace;
        h.setMotion = OnSetMotion;
        h.lineCheck = OnLineCheck;
        h.runClock = GekkoFull::RunClock;
        h.clipSeconds = [](std::uint64_t gani) -> float
        {
            for (int i = 0; i < Gekko::kGaniCount; ++i) if (Gekko::kGaniTable[i] == gani) return Gekko::kGaniFrames[i] / 60.0f;   // mtar frame count at 60 fps (docs 5.2)
            return 0.0f;
        };
        GekkoWorld::Init(h);
        if (game.controller) GekkoWorld::SetController(game.controller);
        if (game.controller && GekkoDebug::NavController() && GekkoWorld::StartNav()) GekkoWorld::StartNavController(8);   // TPP S/B behind Survive-order proxies, then Survive's own controller as the nav service
        GekkoWorld::SetDt(1.0f / 60);
        GekkoWorld::Begin();   // the Observer setup creates the ActionController, which caches the nav service chosen above
        GekkoWorld::ResetUnit(0);   // a second start, after a stop or a fault, begins from the state the first one found
        static const float origin[3] = {};
        GekkoWorld::Request(0, 29, 0, origin);   // the first request: the idle action, taken over by the instance initialiser
        bool ok = GekkoWorld::InitInstance(0) && GekkoWorld::Dispatch(1, 0);
        if (ok && spawnSequence) ok = GekkoWorld::AiStart(0);
        else if (ok) GekkoWorld::SetAiState(0, 12);
        if (!ok) Trace("[brain] start failed: %s", GekkoWorld::LastFault());
        g_active = ok;
        return ok;
    }

    void Stop() { g_active = false; }
    bool Active() { return g_active; }

    void Request(unsigned action, unsigned flag, const float* target) { GekkoWorld::Request(0, action, flag, target); }

    bool Melee(const float* target)
    {
        GekkoWorld::SetTarget(target[0], target[1], target[2]);
        return GekkoWorld::MeleeChoose(0);
    }

    // The idle action (29) never counts as busy: it is what the gekko returns to between actions.
    bool Busy()
    {
        const GekkoWorld::State st = GekkoWorld::Peek(0);
        return st.action != 29 && !(st.controllerFlags & 0x20);
    }
    int AiState() { return GekkoWorld::Peek(0).aiState; }
    unsigned Action() { return GekkoWorld::Peek(0).action; }

    void Frame(float dt, const Input& in)
    {
        if (!g_active) return;
        GekkoWorld::SetDt(dt);
        GekkoWorld::SetPose(0, in.pos[0], in.pos[1], in.pos[2], in.yaw);
        GekkoWorld::SetTarget(in.target[0], in.target[1], in.target[2]);
        GekkoWorld::SetTargetState(in.hasTarget, in.hasTarget ? 1.0f : 0.0f, in.interrupt);   // strength > 0 means a live target to the movement brain (0x140fa476e..0x140fa477b)
        {
            // the row the movement brain reads (docs 5.13): one line whenever id, strength or the interrupt pulse changes
            static int lastId = -2, lastInt = -1; static float lastStr = -1.0f;
            const std::uint8_t* tg = GekkoWorld::TargetObj();
            const int id = *reinterpret_cast<const int*>(tg + 0x30); const float str = *reinterpret_cast<const float*>(tg + 0x34);
            if (id != lastId || str != lastStr || int(in.interrupt) != lastInt)
            {
                Trace("[target] row id %d strength %.2f interrupt %d route id %08x pos %.2f %.2f %.2f", id, str, int(in.interrupt), *reinterpret_cast<const unsigned*>(GekkoWorld::Order() + 0xb0), in.target[0], in.target[1], in.target[2]);
                lastId = id; lastStr = str; lastInt = int(in.interrupt);
            }
        }
        // Call order is the host's: AI states request actions, the movement brain turns the target into orders, the locomotion states and the action component consume them.
        GekkoWorld::SteerBegin();
        const bool ran = GekkoWorld::AiUpdate(0) && GekkoWorld::MoveUpdate(0) && GekkoWorld::ActionUpdate() && GekkoWorld::LocoTick(1, 0) && GekkoWorld::Dispatch(1, 0) && GekkoWorld::LocoTick(3, 0) && GekkoWorld::LocoTick(5, 0) && GekkoWorld::MoveStep(0);
        GekkoWorld::SteerEnd();
        if (!ran)
        {
            Trace("[brain] fault, stopping: %s", GekkoWorld::LastFault());
            g_active = false;
        }
        // every change of the movement brain's mode byte with the inputs the blocked / door branch reads. Capped.
        {
            static unsigned lastMode = ~0u; static int audit = 0;
            const std::uint8_t* o = GekkoWorld::Order();
            if (o[0xb8] != lastMode && audit < 200)
            {
                ++audit;
                const std::uint8_t* e = GekkoWorld::NpcEntry(0);
                const std::uint8_t* tg = GekkoWorld::TargetObj();
                const float* f = reinterpret_cast<const float*>(o);
                const unsigned long long e0 = *reinterpret_cast<const unsigned long long*>(e), e18 = *reinterpret_cast<const unsigned long long*>(e + 0x18);
                Trace("[audit] mode %u -> %u act %u bit53 %d blocked41 %d dy %.2f dist %.2f band %.2f rec40 %.2f %.2f %.2f rec50 %.2f %.2f %.2f target id %d strength %.2f flags68 %u flags6a %u pos %.2f %.2f %.2f",
                      lastMode, o[0xb8], GekkoWorld::Peek(0).action, int((e0 >> 53) & 1), int((e18 >> 41) & 1), f[0x84 / 4], f[0x90 / 4], f[0xa0 / 4], f[0x40 / 4], f[0x44 / 4], f[0x48 / 4], f[0x50 / 4], f[0x54 / 4], f[0x58 / 4],
                      *reinterpret_cast<const int*>(tg + 0x30), *reinterpret_cast<const float*>(tg + 0x34), unsigned(tg[0x68]), unsigned(tg[0x6a]), in.pos[0], in.pos[1], in.pos[2]);
                lastMode = o[0xb8];
            }
        }
        // Stall trace: while the movement brain sits in mode 3 and neither the body, the action, the controller flags nor the locomotion state changes, one line per second with everything the walk
        // waits on (docs 4.11). Diagnostics only: nothing here acts on the state.
        {
            static float acc = 0, px = 0, py = 0, pz = 0; static unsigned sig = ~0u;
            const GekkoWorld::State st = GekkoWorld::Peek(0);
            const unsigned now = st.action | (st.pending << 8) | (st.controllerFlags << 16) | (GekkoWorld::LocoStateNow(0) << 28);
            const float moved = std::fabs(in.pos[0] - px) + std::fabs(in.pos[1] - py) + std::fabs(in.pos[2] - pz);
            if (GekkoWorld::Order()[0xb8] != 3 || now != sig || moved > 0.05f) { acc = 0; sig = now; px = in.pos[0]; py = in.pos[1]; pz = in.pos[2]; }
            else if ((acc += dt) >= 1.0f)
            {
                acc = 0;
                char line[640];
                GekkoWorld::StallLine(0, line, sizeof(line));
                Trace("%s", line);
            }
        }
        // Dev trace for the turn / hiccup investigation: every change of the current action, the controller's finished bit or the AI state, with the body yaw. Capped.
        static unsigned lastAction = ~0u, lastCtl = ~0u, lastLoco = ~0u;
        static int lastAi = -99, lines = 0;
        static float t = 0;
        t += dt;
        const GekkoWorld::State s = GekkoWorld::Peek(0);
        const unsigned fin = s.controllerFlags & 0x20;
        const unsigned loco = GekkoWorld::LocoStateNow(0);
        if ((s.action != lastAction || fin != lastCtl || s.aiState != lastAi || loco != lastLoco) && lines < 400)
        {
            ++lines;
            Trace("[act] t %.2f action %u finished %u ai %d loco %u event flags %x yaw %.3f pos %.2f %.2f %.2f", t, s.action, fin ? 1u : 0u, s.aiState, loco, s.eventFlags, in.yaw, in.pos[0], in.pos[1], in.pos[2]);
            lastAction = s.action; lastCtl = fin; lastAi = s.aiState; lastLoco = loco;
        }
    }

    bool TakeSteer(float* yawRad, float* smoothing) { return GekkoWorld::TakeSteer(yawRad, smoothing); }
    void ControllerRan() { GekkoWorld::NotifyControllerRan(); }

    const char* Status()
    {
        const GekkoWorld::State s = GekkoWorld::Peek(0);
        std::snprintf(g_status, sizeof(g_status), "ai %u action %u flags %x ctl %x", s.aiState, s.action, s.eventFlags, s.controllerFlags);
        return g_status;
    }
}