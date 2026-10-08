// GekkoTargetProbe.cpp 30/09/2026. Dev probe: compares the gekko's hit-target objects with the ones live natives own. Finds every object
// with the gekko target object's vtable in committed memory and logs owner (+0x68), category (+0x60), callback (+0x78) and callback
// interface (+0x30/+0x38). Offsets from TargetSweepCheck::ExecCallback and geo::TargetObject::SetCallback (tpp decomp).
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <map>

#include "GekkoFull.h"
#include "GekkoLog.h"
#include "GekkoTargetProbe.h"

namespace
{
    std::uint64_t Q(const std::uint8_t* p, std::size_t off) { return *reinterpret_cast<const std::uint64_t*>(p + off); }

    void Dump(const char* tag, const std::uint8_t* o)
    {
        Log("[tprobe] %s object %p vtable %llx\n", tag, o, static_cast<unsigned long long>(Q(o, 0)));
        for (int i = 0; i < 0x100; i += 0x20)
            Log("[tprobe]   +%02x %016llx %016llx %016llx %016llx\n", i, static_cast<unsigned long long>(Q(o, i)), static_cast<unsigned long long>(Q(o, i + 8)),
                static_cast<unsigned long long>(Q(o, i + 16)), static_cast<unsigned long long>(Q(o, i + 24)));
    }

    bool SeenType(std::uint64_t t) { static std::map<std::uint64_t, int> seen; return seen[t]++ != 0; }

    // First object of each owner type: its callback interface object and vtable. No unwinding objects here: __try.
    void DumpInterface(const std::uint8_t* o, std::uint64_t owner)
    {
        if (SeenType(owner >> 9 & 0x7f)) return;
        Dump("tag88 object", o);   // the whole target object of the first native of each type (soldier 45 etc.) next to the gekko's, to compare +0x50/+0x58 flags
        const std::uint8_t* itf = reinterpret_cast<const std::uint8_t*>(Q(o, 0x30));
        __try
        {
            Log("[tprobe] type %llu interface %p user(+0x38) %llx\n", static_cast<unsigned long long>(owner >> 9 & 0x7f), itf, static_cast<unsigned long long>(Q(o, 0x38)));
            Dump("interface", itf);
            const std::uint8_t* vt = reinterpret_cast<const std::uint8_t*>(Q(itf, 0));
            Log("[tprobe]   vtable %p: %llx %llx %llx %llx %llx\n", vt, static_cast<unsigned long long>(Q(vt, 0)), static_cast<unsigned long long>(Q(vt, 8)),
                static_cast<unsigned long long>(Q(vt, 16)), static_cast<unsigned long long>(Q(vt, 24)), static_cast<unsigned long long>(Q(vt, 32)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // No C++ objects with destructors in the __try function.
    unsigned ScanRegion(const std::uint8_t* base, std::size_t size, std::uint64_t vtbl, const std::uint8_t** out, unsigned max)
    {
        unsigned n = 0;
        __try
        {
            for (std::size_t i = 0; i + 0x100 <= size && n < max; i += 8)
                if (Q(base, i) == vtbl) out[n++] = base + i;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }
}

namespace GekkoTargetProbe
{
    void Run()
    {
        std::uint8_t** objs = nullptr;
        unsigned count = 0;
        if (!GekkoFull::TargetObjects(&objs, &count) || !count) { Log("[tprobe] the gekko has no target objects\n"); return; }
        const std::uint8_t* mine = objs[0];
        const std::uint64_t vtbl = Q(mine, 0);
        Log("[tprobe] gekko: %u target objects, first %p\n", count, mine);
        Dump("gekko[0]", mine);
        const std::uint64_t myOwner = Q(mine, 0x68);

        static const std::uint8_t* found[4096];
        unsigned total = 0;
        MEMORY_BASIC_INFORMATION mbi;
        for (const std::uint8_t* a = nullptr; VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi); a = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize)
        {
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
            if (readable && total < 4096) total += ScanRegion(static_cast<const std::uint8_t*>(mbi.BaseAddress), mbi.RegionSize, vtbl, found + total, 4096 - total);
        }
        Log("[tprobe] %u objects with that vtable in memory\n", total);
        // group by (has callback, has interface, category) and by owner type (owner >> 9 & 0x7f)
        std::map<std::uint64_t, unsigned> byShape;
        unsigned shown = 0;
        for (unsigned i = 0; i < total; ++i)
        {
            const std::uint8_t* o = found[i];
            const std::uint64_t owner = Q(o, 0x68);
            const std::uint64_t shape = (Q(o, 0x78) ? 1 : 0) | (Q(o, 0x30) ? 2 : 0) | ((owner >> 56) == 0x88 ? 4 : 0) | ((owner & 0xffffffffffffull) >> 9) << 8;
            if ((owner >> 56) == 0x88 && Q(o, 0x30)) DumpInterface(o, owner);
            if (byShape[shape]++ == 0 && owner != myOwner && shown < 10)
            {
                ++shown;
                Dump("native", o);
            }
        }
        for (const auto& kv : byShape)
            Log("[tprobe] shape callback %d interface %d tag88 %d ownerType %llu: %u objects\n", int(kv.first & 1), int((kv.first >> 1) & 1), int((kv.first >> 2) & 1),
                static_cast<unsigned long long>(kv.first >> 8), kv.second);
    }
}
