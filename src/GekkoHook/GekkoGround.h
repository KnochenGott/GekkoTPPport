// GekkoGround.h 29/09/2026. Ground height under a point, from the engine's own collision (fox::geo::CollisionLineCheck).
#pragma once

namespace GekkoGround
{
    bool Verify();
    // Casts a vertical ray from y + 2 m down to y - 8 m at x/z. Returns true and the hit height in *outY.
    bool Height(float x, float y, float z, float* outY);
}
