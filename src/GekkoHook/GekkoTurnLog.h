// GekkoTurnLog.h 08/10/2026. Compact heading diagnostics: yaw, clip, heading request and its last writer every 0.25 s while the body moves or turns.
#pragma once

namespace GekkoTurnLog
{
    void Tick(float dt);   // once per AI update; prints a [turnlog] line every 0.25 s of game time when the yaw or the position changed
}
