// GekkoHitFx.h 06/10/2026. Bullet impact effect and sound for hits on the gekko's target objects.
#pragma once
#include "GekkoHit.h"

namespace GekkoHitFx
{
    // Plays TPP's own impact (BulletSystem3 CreateRicochetEffect, the call every TPP damage handler makes) for each bullet record of one drained batch; records of one shot on neighbouring spheres give one effect.
    void Play(const GekkoHit::Record* r, unsigned n);

    // Debug: the same call for part (0..20) at a world point with the given bullet direction, as a bare bullet (size 0, no attack row, attacker 0). False when the service or the material table is missing.
    bool Debug(int part, const float* point, const float* bulletDir);
}
