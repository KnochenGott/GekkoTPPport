// GekkoNavStep.cpp 07/10/2026. Survive calls the locomotion's move step (slot 20, 0x141091f70) every frame with the clip's root translation and rotation and moves the unit by the result. In the
// walk state (event 7, 0x140f8a67b) the step adds a correction toward the path: when the unit is more than 0.05 off the current segment, a vector to the path point at the progress, at most
// 1 m/s (2 m/s when the locomotion record byte +0x46 is 1) times the frame time, in the unit's local frame (docs/DOCUMENTATION.md 5.13). The port computes that step with the lifted code
// (GekkoWorld::MoveStep) and TPP's VelocityControlAction, which is Survive's twin (CcVelocityControlAction.cpp Exec), adds it to the root motion (flag 2 of +0xc8, vector at +0x90).
#include "GekkoNavStep.h"

#include <cmath>

#include "GekkoFull.h"
#include "GekkoLog.h"
#include "GekkoWorld.h"

namespace
{
    bool g_on = false;
    unsigned g_frames = 0, g_moving = 0, g_lines = 0;
    float g_maxLen = 0.0f;
    double g_sum = 0.0;
}

namespace GekkoNavStep
{
    void SetEnabled(bool on)
    {
        if (on == g_on) return;
        g_on = on;
        Log("[navstep] path correction %s\n", on ? "ON: the walk state's step moves the body" : "OFF");
    }
    bool Enabled() { return g_on; }

    void Apply()
    {
        if (!g_on) return;
        float v[3];
        if (!GekkoWorld::TakeStep(v)) return;
        ++g_frames;
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!std::isfinite(len)) { Log("[navstep] step is not finite, ignored\n"); return; }
        if (len <= 0.0f) return;
        ++g_moving; g_sum += len;
        if (len > g_maxLen) g_maxLen = len;
        GekkoFull::SetMove(v);
        // the first 12 steps and then one line per 120 corrected frames: local step, length, running totals
        if (g_lines < 12 || g_moving % 120 == 0)
        {
            ++g_lines;
            Log("[navstep] local step %.4f %.4f %.4f len %.4f | corrected frames %u of %u, mean %.4f max %.4f\n", v[0], v[1], v[2], len, g_moving, g_frames, g_sum / g_moving, g_maxLen);
        }
    }
}
