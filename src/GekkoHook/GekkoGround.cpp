// GekkoGround.cpp 29/09/2026. Floor ray for the walk. The CharaControl's own check (0x141ad10c0) only sweeps the old ->
// new position, which for a walk is horizontal, so it never finds the floor (measured: 3 runs, no correction). This
// casts a vertical ray with the same engine call it uses. Layout from research/tpp/ground_hit_rig_dump.txt and
// research/tpp/cc_tick_dump.txt (0x141ad10c0, which builds the control on its stack):
//   GeoCheckControlN<N> (ctor 0x14171b450): 0x70 header + (N + 1) results of 0x60 bytes. Header +0x00 = 0x10 and
//   +0x18 = 0x80000004 as the CharaControl sets them, +0x60 non-zero when there are results, +0x64 index of the first.
//   Result (live buffer, tools/scratch/geo_results.py): +0x00 hit point (terrain), +0x10 ray
//   vector to - from, +0x1c short next index (-1 = end), +0x20 unit normal, +0x2c squared distance from the ray start,
//   +0x30 triangle vertex heights. Reading the normal at +0x10 took the ray y (-10) and rejected every floor.
//   CollisionLineCheck(control, flags, from, to, radius) returns non-zero on a hit; flags 0x700 as the CharaControl.
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "GekkoGround.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"

namespace
{
    constexpr int kResults = 128;                                  // N = 127
    constexpr std::size_t kSize = 0x70 + kResults * 0x60;
    constexpr float kUp = 2.0f, kDown = 8.0f;                      // ray span around the current height (not verified)
    constexpr float kMinNormalY = 0.5f;                            // ignore walls and ceilings

    struct alignas(16) Vec4 { float x, y, z, w; };
    using Line_t = int (__fastcall*)(void* control, std::uint32_t flags, const Vec4* from, const Vec4* to, float radius);
    using Ctor_t = void* (__fastcall*)(void* self, std::uint32_t arg);

    Line_t g_line = nullptr;
    Ctor_t g_ctor = nullptr;
    bool g_ok = false, g_dead = false;
    int g_calls = 0;

    template <typename F>
    bool Guard(const char* what, F fn)
    {
        DWORD code = 0;
        __try { fn(); }
        __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[ground] %s raised exception 0x%08lX, ground ray disabled\n", what, code);
            return false;
        }
        return true;
    }
}

namespace GekkoGround
{
    bool Verify()
    {
        g_line = reinterpret_cast<Line_t>(GekkoSig::CheckByName("Geo_CollisionLineCheck", "ground"));
        g_ctor = reinterpret_cast<Ctor_t>(GekkoSig::CheckByName("Geo_CheckControlN127", "ground"));
        g_ok = g_line && g_ctor;
        Log("[ground] Verify -> %s\n", g_ok ? "OK" : "FAILED");
        return g_ok;
    }

    bool Height(float x, float y, float z, float* outY)
    {
        if (g_dead || (!g_ok && !Verify())) return false;
        // The control block lives on the caller's stack: the leg fit (GekkoLegIk) asks from the animation job thread while the game thread may ask too.
        alignas(16) std::uint8_t control[kSize];
        const Vec4 from{ x, y + kUp, z, 1.0f }, to{ x, y - kDown, z, 1.0f };
        int hit = 0;
        const bool ok = Guard("CollisionLineCheck", [&] {
            g_ctor(control, 0);
            *reinterpret_cast<std::uint64_t*>(control + 0x00) = 0x10;
            *reinterpret_cast<std::uint32_t*>(control + 0x18) = 0x80000004u;
            hit = g_line(control, 0x700u, &from, &to, 0.0f);
        });
        if (!ok) { g_dead = true; return false; }

        const std::uint64_t any = *reinterpret_cast<std::uint64_t*>(control + 0x60);
        int i = *reinterpret_cast<std::int32_t*>(control + 0x64);
        float best = -1.0f, bestNy = 0.0f;
        int n = 0;
        for (; hit && any && i >= 0 && i < kResults && n < kResults; ++n)
        {
            const std::uint8_t* r = control + 0x70 + i * 0x60;
            const float d2 = *reinterpret_cast<const float*>(r + 0x2c);
            const float ny = *reinterpret_cast<const float*>(r + 0x24);
            if (d2 >= 0.0f && ny >= kMinNormalY && (best < 0.0f || d2 < best)) { best = d2; bestNy = ny; }
            i = *reinterpret_cast<const std::int16_t*>(r + 0x1c);
        }
        const bool found = best >= 0.0f;
        if (found) *outY = from.y - std::sqrt(best);   // vertical ray: distance along it is the drop
        if (++g_calls <= 5 || g_calls % 120 == 0)
            Log("[ground] #%d at %.2f %.2f %.2f: ret %d, %d results, %s y %.3f (normal y %.2f)\n", g_calls, x, y, z,
                hit, n, found ? "floor" : "no floor", found ? *outY : 0.0f, bestNy);
        return found;
    }
}
