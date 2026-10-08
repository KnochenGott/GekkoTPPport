// GekkoHit.cpp 30/09/2026. Live probe (Gekko.log [tprobe]): native hit targets of types 15, 45, 50 and 63 have a callback interface at +0x30 (callback +0x78 empty),
// types 76-91 have a callback at +0x78, the gekko (type 100) had neither.
// Bullet path, from the TPP decomp (GeoTargetCheck.cpp, TppBullet3.cpp): fox::geo::TargetSweepCheck::Hit calls, for both targets of a hit, the interface at object +0x30
// with vtable slot 1 (+8) as a filter (self, A+0x18, B+0x18, GeoResult*, user) and drops the hit when it returns 0; natives share `mov al,1; ret` (0x14084b020, bytes). Our
// interface had a void Noop there, so rax was garbage and every hit was filtered out. After the filter the sweep runs the victim's callback, slot 2 (+0x10), with the same arguments; natives'
// slot 2 reads the bullet's OffenseTargetData3 (interface +0x28 minus 0x20). Layout from TPP's own code (OffenseTargetData3::operator= 0x1406ba310, GameUtilityImpl::CalcDamage 0x1409e2a80,
// BlastSystem::Blast 0x14097ba90): +0x10 attacker position, +0x20 float distance, +0x24 and +0x26 words, +0x28 u16 AttackId (CalcDamage), +0x2a..+0x2d four bytes on a 0..200 scale where 200 is
// full strength (the blast code stores falloff * 200.0 into +0x2c; CalcDamage multiplies the life damage by byte(+0x2b) / 200), +0x2e flag bits. The damage amount itself is not in the record:
// TPP gets it from the attack id's damage row. The bytes are therefore ratios (200 = no decay), not points. The through value (object +0x5c) is
// not the cause: a native soldier's is 0 like the gekko's ([tprobe] type 45 dump), so it is left alone. This installs an interface that allows the hit, counts and logs it.
// Every hit that reaches one of the gekko's own target objects is also stored as a HitRecord (part index = position in the target object list, damage bytes, flags,
// attacker position) for the Survive side; the consumer is not wired yet (docs/DOCUMENTATION.md: Survive has no gekko life data and no gekko-specific damage handler class).
#include <Windows.h>
#include <cmath>
#include <cstdint>

#include "GekkoFull.h"
#include "GekkoHit.h"
#include "GekkoHitMath.h"
#include "GekkoLog.h"

namespace
{
    unsigned g_hits = 0;
    std::uint8_t** g_objs = nullptr;
    unsigned g_nobj = 0;
    constexpr unsigned kRing = 64;
    GekkoHit::Record g_ring[kRing];
    unsigned g_head = 0, g_tail = 0;   // monotonic; the ring keeps the newest kRing records
    void* g_vtbl[16];
    void* g_iface = g_vtbl;

    std::uint64_t OwnerOf(const void* sub)
    {
        std::uint64_t owner = 0;
        if (sub) __try { owner = *reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(sub) - 0x18 + 0x68); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return owner;
    }

    void DumpOffense(const void* iface)
    {
        __try
        {
            const std::uint8_t* od = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(iface) + 0x28);
            if (!od) { Log("[hit]   no offense data\n"); return; }
            od -= 0x20;
            const float* p = reinterpret_cast<const float*>(od + 0x10);
            Log("[hit]   offense data %p: pos %.2f %.2f %.2f, dist %.2f, words %04x %04x attack id %04x, ratio bytes %u %u %u %u, flags %02x\n", od, p[0], p[1], p[2], *reinterpret_cast<const float*>(od + 0x20),
                *reinterpret_cast<const std::uint16_t*>(od + 0x24), *reinterpret_cast<const std::uint16_t*>(od + 0x26), *reinterpret_cast<const std::uint16_t*>(od + 0x28), od[0x2a], od[0x2b], od[0x2c], od[0x2d], od[0x2e]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[hit]   offense data unreadable\n"); }
    }

    // Part index of a callback argument (object base + 0x18), or -1 when it is not one of the gekko's target objects.
    int PartOf(const void* sub)
    {
        const std::uint8_t* base = static_cast<const std::uint8_t*>(sub) - 0x18;
        for (unsigned i = 0; i < g_nobj; ++i) if (g_objs[i] == base) return static_cast<int>(i);
        return -1;
    }

    void Store(int part, const void* attacker, const void* result)
    {
        GekkoHit::Record r = {};
        r.part = part;
        r.hasHit = GekkoHitMath::ResultPosition(result, r.hit);
        r.owner = OwnerOf(attacker);
        __try
        {
            const std::uint8_t* od = *reinterpret_cast<const std::uint8_t* const*>(static_cast<const std::uint8_t*>(attacker) + 0x28);
            if (od)
            {
                od -= 0x20;
                const float* p = reinterpret_cast<const float*>(od + 0x10);
                r.pos[0] = p[0]; r.pos[1] = p[1]; r.pos[2] = p[2];
                r.dist = *reinterpret_cast<const float*>(od + 0x20);
                r.damage[0] = od[0x2a]; r.damage[1] = od[0x2b]; r.damage[2] = od[0x2c]; r.flags = od[0x2e];
                r.attackId = *reinterpret_cast<const std::uint16_t*>(od + 0x28);
                r.attackerId = *reinterpret_cast<const std::uint16_t*>(od + 0x24);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        // one line per distinct source kind (attack id, flag bits, owner): which attacks reach the gekko, so a contact or push object is told from a bullet in the log
        static std::uint64_t kinds[32]; static unsigned nk = 0;
        const std::uint64_t key = (static_cast<std::uint64_t>(r.attackId) << 48) ^ (static_cast<std::uint64_t>(r.flags) << 40) ^ r.owner ^ (r.dist == 0.0f ? 1ull << 33 : 0);
        unsigned k = 0;
        while (k < nk && kinds[k] != key) ++k;
        if (k == nk && nk < 32)
        {
            kinds[nk++] = key;
            Log("[hit] new source: attack id %04x flags %02x owner %llx distance %.2f ratio bytes %u %u %u on part %d\n", r.attackId, r.flags, static_cast<unsigned long long>(r.owner), r.dist,
                r.damage[0], r.damage[1], r.damage[2], part);
        }
        g_ring[g_head++ % kRing] = r;
        if (g_head - g_tail > kRing) g_tail = g_head - kRing;
    }

    std::uint32_t OnHit(void*, void* a, void* b, void* result, void* user)
    {
        ++g_hits;
        // Which of the two is the gekko's part: the attacker (bullet) is the other one and carries the OffenseTargetData3 at +0x28.
        const int pb = PartOf(b), pa = PartOf(a);
        if (pb >= 0) Store(pb, a, result); else if (pa >= 0) Store(pa, b, result);
        // First 6 hits per victim object, so the push body (hit every frame while touching) does not hide bullets on the defense objects.
        static void* seen[64];
        static unsigned cnt[64];
        static unsigned used = 0;
        unsigned k = 0;
        while (k < used && seen[k] != b) ++k;
        if (k == used && used < 64) { seen[used] = b; cnt[used++] = 0; }
        if (k < 64 && cnt[k]++ < 6)
        {
            Log("[hit] #%u part A %d B %d, A %p owner %llx, B %p owner %llx, result %p user %p\n", g_hits, pa, pb, a, static_cast<unsigned long long>(OwnerOf(a)), b,
                static_cast<unsigned long long>(OwnerOf(b)), result, user);
            DumpOffense(a);
        }
        return 1;
    }
    void Noop(void*) {}
    bool g_enabled = true;
    std::uint8_t Allow(void*, void*, void*, void*, void*) { return g_enabled ? 1 : 0; }   // slot 1: the hit filter (0 = the sweep drops the hit: a dead gekko is shot through)
}

namespace GekkoHit
{
    void Install()
    {
        std::uint8_t** objs = nullptr;
        unsigned n = 0;
        if (!GekkoFull::TargetObjects(&objs, &n)) { Log("[hit] no target objects\n"); return; }
        for (auto& v : g_vtbl) v = reinterpret_cast<void*>(&Noop);
        g_vtbl[1] = reinterpret_cast<void*>(&Allow);
        g_vtbl[2] = reinterpret_cast<void*>(&OnHit);
        g_hits = 0;
        g_objs = objs;
        g_nobj = n;
        g_head = g_tail = 0;
        for (unsigned i = 0; i < n; ++i) *reinterpret_cast<void**>(objs[i] + 0x30) = &g_iface;
        Log("[hit] callback interface installed on %u target objects\n", n);
    }

    unsigned Count() { return g_hits; }
    void Clear() { g_tail = g_head; }
    void SetEnabled(bool on) { g_enabled = on; }

    unsigned Drain(Record* out, unsigned max)
    {
        unsigned n = 0;
        while (g_tail != g_head && n < max) out[n++] = g_ring[g_tail++ % kRing];
        return n;
    }
}