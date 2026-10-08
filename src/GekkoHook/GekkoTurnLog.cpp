// GekkoTurnLog.cpp 08/10/2026. One line per 0.25 s with the body yaw, its change, the clip, the engine's turn flags (cc+0x188 bit 0x100, N, target) and the lifted side (GekkoWorld::TurnLine).
#include "GekkoTurnLog.h"
#include "GekkoFull.h"
#include "GekkoLog.h"
#include "GekkoWorld.h"

#include <cmath>
#include <cstdint>

namespace GekkoTurnLog
{
    void Tick(float dt)
    {
        static float acc = 0, lastYaw = 0, lastPos[3] = {};
        static bool first = true;
        acc += dt;
        if (acc < 0.25f) return;
        acc = 0;
        float f[3];
        if (!GekkoFull::Feet(f)) return;
        const float yaw = GekkoFull::Yaw();
        float d = yaw - lastYaw;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        const float moved = std::sqrt((f[0] - lastPos[0]) * (f[0] - lastPos[0]) + (f[2] - lastPos[2]) * (f[2] - lastPos[2]));
        const bool quiet = !first && std::fabs(d) < 0.002f && moved < 0.01f;
        const float dyaw = d * 57.2957795f;
        lastYaw = yaw; lastPos[0] = f[0]; lastPos[1] = f[1]; lastPos[2] = f[2]; first = false;
        if (quiet) return;
        std::uint32_t flags = 0; float n = 0, tgt = 0;
        GekkoFull::TurnState(&flags, &n, &tgt);
        char w[320];
        GekkoWorld::TurnLine(w, sizeof(w));
        Log("[turnlog] yaw %.3f dyaw %.1f deg moved %.2f m | cc flag100 %d N %.0f target %.3f | %s\n", yaw, dyaw, moved, (flags & 0x100) ? 1 : 0, n, tgt, w);
    }
}
