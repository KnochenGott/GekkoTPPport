// GekkoHitFx.cpp 06/10/2026, rewritten 07/10/2026. TPP does not derive a character hit effect from the target: each game object's damage handler calls BulletSystem3::CreateRicochetEffect (service [[QST+0x98]+0x218], vtable slot 2,
// retail vtable 0x142337d18 slot 2 = 0x140d32d50; dev body 0x140c6c3c0) with a material code, a size and the attacker's id; the effect and the impact sound both come out of that call (sound flag = last argument).
// The body (retail disassembly, same offsets as dev): a material code with a zero high byte is looked up in the Lua-loaded recoil material table ([[svc+0xc0]+8], vtable slot 0, row: u32 parameter id, byte +4 ricochet
// effect index, +5 bullet mark index) and the call returns without effect AND without sound when the code is not in the table; the material code is the id MaterialDatabase::AddMaterial gave the name of the Lua entry, so
// it is not the entry's position. Codes with a high byte (0x1000000) skip the table. This file therefore picks its codes from the live table by what the row says (effect index 0 = iron sparks, 3 = plastic).
// TPP has no damage handler for the gekko game object type (Survive's own handler is lifted and runs in GekkoLife), so the call is made here. Size and special code follow the walker gear handler (CallDamageEffect, dev 0x141635f50):
// attack row flags (u16 at +0x14 of the row from [[QST+0x98]+0xa0] slot 0, bit 0 = bullet): bullet, not penetrating -> size 0; penetrating (0x200) -> special 0x1000000, size 2; other -> size 2.
#include <Windows.h>
#include <cstdint>
#include <cstdio>

#include "GekkoHitFx.h"
#include "GekkoHitMath.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"

namespace
{
    using GetQst_t = std::uint8_t* (__fastcall*)();
    using Lookup_t = std::uint8_t* (__fastcall*)(void*, std::uint32_t);
    using Ricochet_t = void (__fastcall*)(void*, const void*, std::uint32_t, std::int32_t, std::uint16_t, char);

    GetQst_t g_getQst = nullptr;
    bool g_tried = false;
    unsigned g_calls = 0;
    std::uint32_t g_iron = 0, g_plastic = 0;   // codes found in the live table (0 = not found yet)

    // Material role per sphere index (order of GeckoTargetDefense.tgt); Survive's tgt carries no material, the per-part identity is the region of mapper 0x140f7f030: head plates, waist, legs, gun and gas are
    // armour or metal (iron sparks), Weak_Head (index 13, the organic weak point) uses the plastic effect, the one non-metal impact of TPP's table that suits a hull.
    bool WeakPart(int part) { return part == 13; }

    // Reads the recoil material table through its virtual lookup for every code 1..255 and remembers the first code whose row has effect index 0 and mark index 0 (iron) and the first with effect index 3 (plastic).
    // Logs the valid codes once. Returns false until the table holds an iron entry (Lua loads it when a mission starts).
    bool ScanTable(const std::uint8_t* svc)
    {
        if (g_iron) return true;
        char line[900];
        int len = 0;
        unsigned valid = 0;
        __try
        {
            const std::uint8_t* holder = *reinterpret_cast<const std::uint8_t* const*>(svc + 0xc0);
            void* tbl = holder ? *reinterpret_cast<void* const*>(holder + 8) : nullptr;
            if (!tbl) { Log("[hitfx] recoil material table object missing\n"); return false; }
            const auto lookup = reinterpret_cast<Lookup_t>((*reinterpret_cast<void***>(tbl))[0]);
            for (std::uint32_t c = 1; c < 256; ++c)
            {
                const std::uint8_t* row = lookup(tbl, c);
                if (!row) continue;
                ++valid;
                if (len < 840) len += std::snprintf(line + len, sizeof line - len, " %x:%u/%u", c, row[4], row[5]);
                if (!g_iron && row[4] == 0 && row[5] == 0) g_iron = c;
                if (!g_plastic && row[4] == 3) g_plastic = c;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[hitfx] recoil material table scan faulted\n"); return false; }
        line[len] = 0;
        Log("[hitfx] recoil material table: %u valid codes (code:effect/mark):%s\n", valid, valid ? line : " none");
        Log("[hitfx] iron code %x, plastic code %x\n", g_iron, g_plastic);
        return g_iron != 0;
    }

    // One call of the service; logs the first 16 calls and then every 50th: the arguments the service received.
    void Call(const std::uint8_t* svc, int part, std::uint32_t material, int size, const float* hit, const float* dir, std::uint16_t attacker, std::uint16_t attackId, unsigned flags)
    {
        alignas(16) float m[16];
        if (!GekkoHitMath::ImpactMatrix(dir, hit, m)) { Log("[hitfx] skipped: direction %.2f %.2f %.2f is not a unit vector\n", dir[0], dir[1], dir[2]); return; }
        const unsigned n = ++g_calls;
        if (n <= 16 || n % 50 == 0)
            Log("[hitfx] call #%u part %d attack %04x row flags %04x -> material %x size %d attacker %04x point %.2f %.2f %.2f normal %.2f %.2f %.2f\n", n, part, attackId, flags, material, size, attacker,
                hit[0], hit[1], hit[2], m[8], m[9], m[10]);
        __try
        {
            void* s = const_cast<std::uint8_t*>(svc);
            reinterpret_cast<Ricochet_t>((*reinterpret_cast<void***>(s))[2])(s, m, material, size, attacker, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[hitfx] impact call faulted\n"); }
    }

    const std::uint8_t* Service()
    {
        if (!g_tried) { g_tried = true; g_getQst = reinterpret_cast<GetQst_t>(GekkoSig::CheckByName("QuarkSystemTable_Get", "hitfx")); }
        if (!g_getQst) return nullptr;
        __try
        {
            const std::uint8_t* reg = *reinterpret_cast<std::uint8_t* const*>(g_getQst() + 0x98);
            return *reinterpret_cast<const std::uint8_t* const*>(reg + 0x218);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // Attack row flags of an attack id (u16 at row +0x14), or -1 when the row table or the row is missing.
    int RowFlags(std::uint16_t id)
    {
        __try
        {
            const std::uint8_t* reg = *reinterpret_cast<std::uint8_t* const*>(g_getQst() + 0x98);
            void* rows = *reinterpret_cast<void* const*>(reg + 0xa0);
            if (!rows) return -1;
            const std::uint8_t* row = reinterpret_cast<Lookup_t>((*reinterpret_cast<void***>(rows))[0])(rows, id);
            return row ? *reinterpret_cast<const std::uint16_t*>(row + 0x14) : -1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    void Emit(const GekkoHit::Record& r)
    {
        const std::uint8_t* svc = Service();
        if (!svc) return;
        const int flags = RowFlags(r.attackId);
        static unsigned skipped = 0;
        if (flags < 0 || !(flags & 1))
        {
            if (skipped++ < 6) Log("[hitfx] no impact for attack %04x: row flags %d (bullet bit not set, or row missing)\n", r.attackId, flags);
            return;
        }
        if (!ScanTable(svc)) return;
        std::uint32_t material = (WeakPart(r.part) && g_plastic) ? g_plastic : g_iron;
        int size = 0;
        if ((flags & 8) || (flags & 0x200)) { size = 2; if (!(flags & 8)) material = 0x1000000; }   // penetrating rows keep the special code
        Call(svc, r.part, material, size, r.hit, r.pos, r.attackerId, r.attackId, static_cast<unsigned>(flags));
    }
}

namespace GekkoHitFx
{
    void Play(const GekkoHit::Record* r, unsigned n)
    {
        for (unsigned i = 0; i < n; ++i)
        {
            if (r[i].part < 0 || !r[i].hasHit) continue;
            bool dup = false;
            for (unsigned j = 0; j < i && !dup; ++j)
            {
                const float dx = r[i].hit[0] - r[j].hit[0], dy = r[i].hit[1] - r[j].hit[1], dz = r[i].hit[2] - r[j].hit[2];
                dup = r[j].part >= 0 && r[j].hasHit && r[i].attackId == r[j].attackId && dx * dx + dy * dy + dz * dz < 0.25f;
            }
            if (!dup) Emit(r[i]);
        }
    }

    bool Debug(int part, const float* point, const float* bulletDir)
    {
        const std::uint8_t* svc = Service();
        if (!svc) { Log("[hitfx] debug impact: service not available\n"); return false; }
        if (!ScanTable(svc)) { Log("[hitfx] debug impact: no iron material in the recoil table (is a mission loaded?)\n"); return false; }
        const std::uint32_t material = (WeakPart(part) && g_plastic) ? g_plastic : g_iron;
        Call(svc, part, material, 0, point, bulletDir, 0, 0xffff, 0);
        return true;
    }
}
