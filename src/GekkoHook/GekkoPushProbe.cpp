// GekkoPushProbe.cpp 30/09/2026. Dev probe for the character-to-gekko collision. READ (TPP decomp Cc_win64/CharaControl.cpp, CollisionHumanCheck 0x141940720): a CharaControl resolves contact
// with other characters through geo::CollisionLineCheck with a GeoCheckControl built from its own fields: +0x00 = cc+0x1c0, +0x08 = cc+0x1c8, +0x10 = cc+0x1d0, +0x18 = 0x80000004,
// +0x20 = cc+0x1f0, +0x68 = 0x200000 (the PUSH system attribute, the value our push-defense .tgt prims carry), flags cc+0x1f8 | 0x700, radius cc+0x178. This casts that query horizontally
// through the gekko and logs the results, to settle by measurement whether the push prims are found (then the fault is on the player's side) or not (then they are not in that world).
#include <Windows.h>
#include <cstdint>
#include <cstring>

#include "GekkoFull.h"
#include "GekkoLog.h"
#include "GekkoPushProbe.h"
#include "GekkoSigCheck.h"

namespace
{
    constexpr int kResults = 128;
    constexpr std::size_t kSize = 0x70 + kResults * 0x60;
    struct alignas(16) Vec4 { float x, y, z, w; };
    using Line_t = int (__fastcall*)(void* control, std::uint32_t flags, const Vec4* from, const Vec4* to, float radius);
    using Ctor_t = void* (__fastcall*)(void* self, std::uint32_t arg);
    alignas(16) std::uint8_t g_control[kSize];

    template <typename T> T Get(const void* p, std::size_t off) { return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(p) + off); }
}

namespace GekkoPushProbe
{
    void Run(float halfSpan, int withPushAttr)
    {
        void* cc = GekkoFull::CharaControl();
        if (!cc) { Log("[pushprobe] no gekko CharaControl\n"); return; }
        const auto line = reinterpret_cast<Line_t>(GekkoSig::CheckByName("Geo_CollisionLineCheck", "pushprobe"));
        const auto ctor = reinterpret_cast<Ctor_t>(GekkoSig::CheckByName("Geo_CheckControlN127", "pushprobe"));
        if (!line || !ctor) { Log("[pushprobe] signatures not verified\n"); return; }

        const float fx = Get<float>(cc, 0x20), fy = Get<float>(cc, 0x24), fz = Get<float>(cc, 0x28), radius = Get<float>(cc, 0x178);
        Log("[pushprobe] gekko feet %.2f %.2f %.2f radius %.2f; cc +1c0 %llx +1c8 %llx +1d0 %llx +1f0 %llx +1f8 %x +188 %x\n", fx, fy, fz, radius,
            Get<std::uint64_t>(cc, 0x1c0), Get<std::uint64_t>(cc, 0x1c8), Get<std::uint64_t>(cc, 0x1d0), Get<std::uint64_t>(cc, 0x1f0), Get<std::uint32_t>(cc, 0x1f8), Get<std::uint32_t>(cc, 0x188));
        std::uint8_t** objs = nullptr;
        unsigned nobj = 0;
        GekkoFull::TargetObjects(&objs, &nobj);

        // A gekko is about 1.6 m long and stands about 1.2 m tall, so one ray at half a CharaControl height misses it: rays at four heights and three lateral offsets, both axes.
        // Floor results (normal y > 0.8) are counted but not listed; every other result is logged with the gekko target object it matches, if any.
        static const float kHeights[] = { 0.3f, 0.6f, 0.9f, 1.2f }, kLateral[] = { -0.6f, 0.0f, 0.6f };
        for (const float h : kHeights)
            for (const float lat : kLateral)
                for (int axis = 0; axis < 2; ++axis)
                {
                    const Vec4 from = axis ? Vec4{ fx + lat, fy + h, fz - halfSpan, 1.0f } : Vec4{ fx - halfSpan, fy + h, fz + lat, 1.0f };
                    const Vec4 to = axis ? Vec4{ fx + lat, fy + h, fz + halfSpan, 1.0f } : Vec4{ fx + halfSpan, fy + h, fz + lat, 1.0f };
                    int hit = 0;
                    DWORD code = 0;
                    __try
                    {
                        ctor(g_control, 0);
                        *reinterpret_cast<std::uint64_t*>(g_control + 0x00) = Get<std::uint64_t>(cc, 0x1c0);
                        *reinterpret_cast<std::uint64_t*>(g_control + 0x08) = Get<std::uint64_t>(cc, 0x1c8);
                        *reinterpret_cast<std::uint64_t*>(g_control + 0x10) = Get<std::uint64_t>(cc, 0x1d0);
                        *reinterpret_cast<std::uint32_t*>(g_control + 0x18) = 0x80000004u;
                        *reinterpret_cast<std::uint64_t*>(g_control + 0x20) = Get<std::uint64_t>(cc, 0x1f0);
                        *reinterpret_cast<std::uint32_t*>(g_control + 0x64) = 0xffffffffu;
                        *reinterpret_cast<std::uint32_t*>(g_control + 0x68) = withPushAttr ? 0x200000u : 0u;
                        hit = line(g_control, Get<std::uint32_t>(cc, 0x1f8) | 0x700u, &from, &to, 0.25f);
                    }
                    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { Log("[pushprobe] exception %08lx\n", code); return; }
                    const unsigned count = Get<std::uint32_t>(g_control, 0x60);
                    int i = Get<std::int32_t>(g_control, 0x64);
                    unsigned floorHits = 0, other = 0;
                    for (int n = 0; hit && count && i >= 0 && i < kResults && n < 16; ++n)
                    {
                        const std::uint8_t* r = g_control + 0x70 + i * 0x60;
                        if (Get<float>(r, 0x24) > 0.8f) ++floorHits;
                        else
                        {
                            ++other;
                            Log("[pushprobe] h %.1f lat %.1f axis %d: #%d point %.2f %.2f %.2f normal %.2f %.2f %.2f d2 %.3f\n", h, lat, axis, i, Get<float>(r, 0), Get<float>(r, 4), Get<float>(r, 8),
                                Get<float>(r, 0x20), Get<float>(r, 0x24), Get<float>(r, 0x28), Get<float>(r, 0x2c));
                            for (std::size_t q = 0; q + 8 <= 0x60; q += 8)
                            {
                                const std::uint64_t v = Get<std::uint64_t>(r, q);
                                for (unsigned o = 0; o < nobj; ++o)
                                    if (v == reinterpret_cast<std::uint64_t>(objs[o]) || v == reinterpret_cast<std::uint64_t>(objs[o]) + 0x18)
                                        Log("[pushprobe]     result qword +0x%zx is gekko target object %u (%p)\n", q, o, objs[o]);
                            }
                        }
                        i = Get<std::int16_t>(r, 0x1c);
                    }
                    if (other || floorHits) Log("[pushprobe] h %.1f lat %.1f axis %d: ret %d, %u non-floor, %u floor\n", h, lat, axis, hit, other, floorHits);
                }
        Log("[pushprobe] done (push attr %d, %u gekko target objects)\n", withPushAttr, nobj);
    }
}
