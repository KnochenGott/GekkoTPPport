// leg_ik_test.cpp 07/10/2026. Headless: the leg fit math (GekkoLegIkMath.h) and the RigDef / RigPose walk (GekkoLegIkPose.h) on a synthetic image built from the documented layouts (token LEG_IK_OK).
// It tests this code's own arithmetic and the table walk against the decomp's TranslatePose semantics; it says nothing about the live engine structures.
#include "GekkoLegIkMath.h"
#include "GekkoLegIkPose.h"
#include <cstdio>
#include <cstring>
#include <vector>
static int bad = 0;
#define CHK(c) do { if (!(c)) { std::printf("FAIL %s line %d\n", #c, __LINE__); ++bad; } } while (0)
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) < e; }

int main() {
    using namespace GekkoLegIkMath;
    // Transform: yaw 90 degrees about y (x' = z, z' = -x) with translation (10, 20, 30), columns c0..c3
    const float m[16] = { 0,0,-1,0,  0,1,0,0,  1,0,0,0,  10,20,30,1 };
    const float p[3] = { 1, 2, 3 };
    float w[3];
    Transform(m, p, w);
    CHK(near(w[0], 13) && near(w[1], 22) && near(w[2], 29));
    // FootDelta: no hit keeps the animation, a hit clamps
    CHK(FootDelta(false, 5.0f, 1.0f, 0.6f) == 0.0f);
    CHK(near(FootDelta(true, 1.3f, 1.0f, 0.6f), 0.3f));
    CHK(near(FootDelta(true, 9.0f, 1.0f, 0.6f), 0.6f));
    CHK(near(FootDelta(true, -9.0f, 1.0f, 0.6f), -0.6f));
    // Smooth: reaches the target, never overshoots, same result in one step as in two half steps
    float s = 0.0f;
    for (int i = 0; i < 600; ++i) { s = Smooth(s, 0.4f, 1.0f / 60.0f, 14.0f); CHK(s <= 0.4f + 1e-6f); }
    CHK(near(s, 0.4f, 1e-3f));
    CHK(near(Smooth(0.0f, 1.0f, 0.1f, 14.0f), Smooth(Smooth(0.0f, 1.0f, 0.05f, 14.0f), 1.0f, 0.05f, 14.0f), 1e-5f));
    // ObjectUp: the world image of the object vector is exactly (0, dy, 0), with rotation and uniform scale 2 (yaw 90 plus pitch 30 about the object x axis)
    const float c = std::cos(0.5236f), sn = std::sin(0.5236f);
    // R = Ry(90) * Rx(30), columns scaled by 2
    const float mm[16] = { 0,0,-2,0,   2*sn,2*c,0,0,   2*c,-2*sn,0,0,   5,6,7,1 };
    float d[3], back[3];
    ObjectUp(mm, 0.5f, d);
    for (int i = 0; i < 3; ++i) back[i] = mm[i] * d[0] + mm[4 + i] * d[1] + mm[8 + i] * d[2];   // linear part only
    CHK(near(back[0], 0.0f, 1e-3f) && near(back[1], 0.5f, 1e-3f) && near(back[2], 0.0f, 1e-3f));
    // degenerate scale gives a zero move, not NaN
    const float zero[16] = {};
    ObjectUp(zero, 1.0f, d);
    CHK(d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f);

    // Pose walk on a synthetic image. RigDef: 6 units, types 0 0 10 0 10 10 (unit 5 repeats the def of unit 4); RigPose: units 2 and 4 have a pose unit, unit 5 shares unit 4's pose unit, unit 3 is null
    std::vector<std::uint8_t> def(0x400, 0), pose(0x400, 0);
    auto* dp = def.data(); auto* pp = pose.data();
    *reinterpret_cast<std::uint32_t*>(dp + 0xc) = 6;
    const std::uint32_t defOff[6] = { 0x100, 0x110, 0x120, 0x130, 0x140, 0x140 };   // unit 4 and 5 share one def
    const std::int32_t types[6] = { 1, 3, 10, 7, 10, 10 };
    for (int i = 0; i < 6; ++i) {
        *reinterpret_cast<std::uint32_t*>(dp + 0x20 + 4 * i) = defOff[i];
        *reinterpret_cast<std::int32_t*>(dp + defOff[i]) = types[i];
    }
    *reinterpret_cast<std::int32_t*>(dp + 0x140) = 10;
    *reinterpret_cast<std::uint16_t*>(pp + 0xc) = 6;
    *reinterpret_cast<std::uint32_t*>(pp + 8) = 0x20;
    const std::uint32_t poseOff[6] = { 0x100, 0x140, 0x180, 0, 0x200, 0x200 };
    for (int i = 0; i < 6; ++i) *reinterpret_cast<std::uint32_t*>(pp + 0x20 + 4 * i) = poseOff[i];
    GekkoLegIkPose::Leg legs[4];
    const unsigned n = GekkoLegIkPose::FindLegs(dp, pp, legs, 4);
    CHK(n == 2);
    CHK(legs[0].index == 2 && legs[0].unit == pp + 0x180);
    CHK(legs[1].index == 4 && legs[1].unit == pp + 0x200);   // unit 5 is the same pose unit: reported once
    CHK(GekkoLegIkPose::FindLegs(dp, pp, legs, 1) == 1);     // the cap holds
    *reinterpret_cast<std::uint32_t*>(pp + 8) = 0;
    CHK(GekkoLegIkPose::FindLegs(dp, pp, legs, 4) == 0);     // no table, no legs
    // Translate: +0x40 gets the vector, +0x30 becomes the same sum (RigAnimalLegDef::TranslatePose)
    float* t = GekkoLegIkPose::FootTarget(pp + 0x180);
    t[0] = 1; t[1] = 2; t[2] = 3; t[3] = 1;
    const float mv[3] = { 0.5f, -1.0f, 2.0f };
    GekkoLegIkPose::Translate(pp + 0x180, mv);
    const float* old30 = reinterpret_cast<const float*>(pp + 0x180 + 0x30);
    CHK(near(t[0], 1.5f) && near(t[1], 1.0f) && near(t[2], 5.0f) && near(t[3], 1.0f));
    CHK(std::memcmp(old30, t, 16) == 0);

    if (bad) { std::printf("LEG_IK FAILED %d\n", bad); return 1; }
    std::printf("LEG_IK_OK\n");
    return 0;
}
