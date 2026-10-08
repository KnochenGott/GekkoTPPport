// GekkoLegIkMath.h 07/10/2026. Pure math of the gekko foot-to-ground fit (no engine calls), shared by the DLL and the headless test.
#pragma once
#include <cmath>

namespace GekkoLegIkMath
{
    // Matrix layout of fox::gr::Model + 0x40 (Vectormath::Aos::Matrix4, read in TPP PluginDogIk::UpdateGroundInfo): four columns of four floats,
    // world = c0 * x + c1 * y + c2 * z + c3.
    inline void Transform(const float* m, const float p[3], float out[3])
    {
        for (int i = 0; i < 3; ++i) out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
    }

    inline float Clamp(float v, float lim) { return v > lim ? lim : (v < -lim ? -lim : v); }

    // How far a foot has to move along world up: the ground under it minus the ground the CharaControl stands on (the plane the animation was authored on).
    // No hit leaves the foot where the animation put it.
    inline float FootDelta(bool hit, float groundY, float feetY, float maxDelta) { return hit ? Clamp(groundY - feetY, maxDelta) : 0.0f; }

    // Exponential approach, frame rate independent.
    inline float Smooth(float cur, float target, float dt, float rate) { return cur + (target - cur) * (1.0f - std::exp(-rate * dt)); }

    // Object space vector that moves a point by dy along world up: R^T (0, dy, 0) divided by the squared uniform scale (column 0 length squared).
    inline void ObjectUp(const float* m, float dy, float out[3])
    {
        const float s2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        const float k = s2 > 1e-8f ? dy / s2 : 0.0f;
        out[0] = m[1] * k; out[1] = m[5] * k; out[2] = m[9] * k;
    }
}
