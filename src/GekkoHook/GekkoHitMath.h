// GekkoHitMath.h 07/10/2026. Pure math of a hit: the world point of a GeoResult and the TPP impact matrix (header only, unit tested by tools/lift/tests/hit_math_test.cpp).
#pragma once
#include <Windows.h>
#include <cmath>
#include <cstdint>

namespace GekkoHitMath
{
    // GeoResult::GetPosition (dev 0x141a0ab30, read from the decomp): raw point at +0; unless flag 0x800 (word +0xc) is set or the matrix pointer (+0x58) is null it is carried through the matrix
    // (three normalised 4-float rows and a translation row) into world space.
    inline bool ResultPosition(const void* result, float* out)
    {
        if (!result) return false;
        __try
        {
            const std::uint8_t* g = static_cast<const std::uint8_t*>(result);
            const float* raw = reinterpret_cast<const float*>(g);
            const float* m = *reinterpret_cast<const float* const*>(g + 0x58);
            const bool world = (*reinterpret_cast<const std::uint16_t*>(g + 0xc) & 0x800) || !m;
            for (int k = 0; k < 3; ++k) out[k] = world ? raw[k] : m[12 + k];
            for (int row = 0; row < 3 && !world; ++row)
            {
                const float* r = m + 4 * row;
                const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
                if (len > 0.0f) for (int k = 0; k < 3; ++k) out[k] += raw[row] * r[k] / len;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Matrix4 of the TPP damage handlers (rows up x n, n x (up x n), n, point; n = -bullet direction, as CallDamageEffect dev 0x141635f50). False when the direction is not a unit-ish vector.
    inline bool ImpactMatrix(const float* bulletDir, const float* point, float* m)
    {
        float n[3] = { -bulletDir[0], -bulletDir[1], -bulletDir[2] };
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 0.5f) return false;
        for (int k = 0; k < 3; ++k) n[k] /= nl;
        float a[3] = { n[2], 0.0f, -n[0] };   // up x n
        float al = std::sqrt(a[0] * a[0] + a[2] * a[2]);
        if (al < 1e-4f) { a[0] = 1.0f; a[2] = 0.0f; al = 1.0f; }
        for (int k = 0; k < 3; ++k) a[k] /= al;
        const float b[3] = { n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0] };   // n x a
        const float r[16] = { a[0], a[1], a[2], 0, b[0], b[1], b[2], 0, n[0], n[1], n[2], 0, point[0], point[1], point[2], 1.0f };
        for (int k = 0; k < 16; ++k) m[k] = r[k];
        return true;
    }
}
