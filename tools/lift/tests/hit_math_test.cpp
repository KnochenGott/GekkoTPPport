// hit_math_test.cpp 07/10/2026. Headless: GeoResult decode (GekkoHitMath::ResultPosition) against a hand-built result, and the impact matrix (token HIT_MATH_OK).
#include "GekkoHitMath.h"
#include <cstdio>
#include <cstring>
static int bad = 0;
#define CHK(c) do { if (!(c)) { std::printf("FAIL %s line %d\n", #c, __LINE__); ++bad; } } while (0)
static bool near3(const float* a, float x, float y, float z) { return std::fabs(a[0] - x) < 1e-4f && std::fabs(a[1] - y) < 1e-4f && std::fabs(a[2] - z) < 1e-4f; }
int main() {
    alignas(16) std::uint8_t g[0x80] = {};
    float* raw = reinterpret_cast<float*>(g);
    raw[0] = 1; raw[1] = 2; raw[2] = 3;
    float out[3];
    // world flag 0x800: the raw point is the world point
    *reinterpret_cast<std::uint16_t*>(g + 0xc) = 0x800;
    CHK(GekkoHitMath::ResultPosition(g, out) && near3(out, 1, 2, 3));
    // no matrix pointer: also world
    *reinterpret_cast<std::uint16_t*>(g + 0xc) = 0;
    CHK(GekkoHitMath::ResultPosition(g, out) && near3(out, 1, 2, 3));
    // identity rotation with translation (10, 20, 30)
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 };
    const float* mp = m;
    std::memcpy(g + 0x58, &mp, sizeof mp);
    CHK(GekkoHitMath::ResultPosition(g, out) && near3(out, 11, 22, 33));
    // 90 degrees about y (rows scaled 2x, normalised by the decode): local (1,2,3) -> (3,2,-1) + translation
    float r[16] = { 0,0,-2,0, 0,2,0,0, 2,0,0,0, 10,20,30,1 };
    mp = r; std::memcpy(g + 0x58, &mp, sizeof mp);
    CHK(GekkoHitMath::ResultPosition(g, out) && near3(out, 10 + 3, 20 + 2, 30 - 1));
    CHK(!GekkoHitMath::ResultPosition(nullptr, out));
    // impact matrix: bullet flies along +z, the surface normal faces the shooter (-z)... n = -dir
    const float dir[3] = { 0, 0, 1 }, pt[3] = { 5, 6, 7 };
    float im[16];
    CHK(GekkoHitMath::ImpactMatrix(dir, pt, im));
    CHK(near3(im + 8, 0, 0, -1));            // row 2 = n
    CHK(near3(im + 12, 5, 6, 7) && im[15] == 1.0f);   // translation row = hit point
    for (int a = 0; a < 3; ++a) for (int b = a; b < 3; ++b) {   // orthonormal rows
        const float* x = im + 4 * a; const float* y = im + 4 * b;
        const float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
        CHK(std::fabs(d - (a == b ? 1.0f : 0.0f)) < 1e-4f);
    }
    // straight up (degenerate up x n) still gives a valid frame
    const float up[3] = { 0, -1, 0 };
    CHK(GekkoHitMath::ImpactMatrix(up, pt, im) && near3(im + 8, 0, 1, 0));
    // a zero direction is refused
    const float zero[3] = { 0, 0, 0 };
    CHK(!GekkoHitMath::ImpactMatrix(zero, pt, im));
    std::printf("%s\n", bad ? "HIT_MATH_FAIL" : "HIT_MATH_OK");
    return bad;
}
