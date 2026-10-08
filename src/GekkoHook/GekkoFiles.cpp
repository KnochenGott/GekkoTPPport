// GekkoFiles.cpp 25/09/2026. Debug view of fox::File's global file table (g_files).
// from the steamless decomp (FoxCore_win64/FoxFile.cpp): File::Register inserts (u64 key = *(File+0x30),
// EntityHandle) into a std::multimap g_files; File::Find(PathId) locks a mutex and searches that map.
// MSVC tree layout as the decomp reads it: the global holds the head node pointer; head+0x08 = root;
// node +0x00 left, +0x08 parent, +0x10 right, +0x19 isnil, +0x20 key, +0x28 EntityHandle.
// The g_files address is not hardcoded: it is decoded at runtime from the rip-relative loads in the (signature
// checked) File::Find(PathId) body, and a candidate is accepted only if its head node has isnil == 1.
// The walk does not take the file mutex; it is read-only and every read is SEH-guarded.
#include <Windows.h>
#include <cstring>

#include "GekkoFiles.h"
#include "GekkoLog.h"
#include "GekkoSigs.h"

namespace GekkoFiles
{
    namespace
    {
        constexpr std::uint64_t kBaseMask = 0x3FFFFFFFFFFFFull;  // PATH_CODE_BASE_MASK (tools/pathcode.py)
        constexpr int kExtShift = 51;                             // PATH_CODE_EXTENSION_OFFSET
        constexpr int kMaxNodes = 200000;

        // tools/pathcode.py path_code() of each path. Control entries are vanilla TPP files, to show the key format.
        struct Known { std::uint64_t code; const char* path; };
        const Known kKnown[] =
        {
            { 0xE0AF934B21C9A656ull, "/Assets/ssd/parts/mecha/gec/gec0_main0_def_v00.parts" },
            { 0x67052989CE8858FFull, "/Assets/ssd/motion/mtar/zombie/Gecko_layers.mtar" },
            { 0x518867968EAF27ADull, "/Assets/tpp/level_asset/gekko/gekko_dll.fox2" },
            { 0x47242CAD048F25B3ull, "/Assets/ssd/rig/frig/ssd_gekkou.frig" },
            { 0x84A4C77AB12C64F7ull, "/Assets/ssd/mecha/gec/Scenes/gec0_main0_def.fmdl" },
            { 0xE494DB6789B5E840ull, "/Assets/ssd/parts/zombie/GeckoTargetDefense.tgt" },
            { 0x608FE67B11202C81ull, "/Assets/ssd/fova/mecha/gec/gec_def.fv2" },
            { 0x34953F6EA722EF24ull, "/Assets/ssd/mecha/gec/Fox_Files/gec0_main0_def.sim" },
            { 0x5229F88596B9BFEEull, "/Assets/tpp/pack/gekko/gekko_dll.fpk" },
            { 0xED51F88596B9BFEEull, "/Assets/tpp/pack/gekko/gekko_dll.fpkd" },
            { 0x51899C6BF892281Dull, "(control) /Assets/tpp/level_asset/chara/enemy/zombie_assets.fox2" },
            { 0x6703D0FD9C32A435ull, "(control) /Assets/tpp/motion/mtar/parasite/Parasite2_layers.mtar" },
            { 0xE0AB59EF80E66F59ull, "(control) /Assets/tpp/parts/chara/wmu/wmu3_main0_def_v00.parts" },
        };

        std::uintptr_t g_filesVar = 0;  // address of the global that holds the head node pointer

        bool SafeRead(const void* p, void* out, size_t n)
        {
            __try
            {
                std::memcpy(out, p, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        template <typename T> bool Rd(std::uintptr_t a, T& v) { return SafeRead(reinterpret_cast<void*>(a), &v, sizeof(T)); }

        std::uintptr_t FindByIdAddress()
        {
            for (const auto& s : GekkoSigs::kFuncs)
                if (std::strcmp(s.name, "File_FindById") == 0)
                    return reinterpret_cast<std::uintptr_t>(ResolveGameAddress(s.address));
            return 0;
        }

        // Decode "REX.W 8B /r" with mod=00 rm=101 (mov r64,[rip+disp32]) in the first 0x100 bytes of Find(PathId).
        bool ResolveMap()
        {
            if (g_filesVar)
                return true;
            const std::uintptr_t fn = FindByIdAddress();
            std::uint8_t code[0x100];
            if (!fn || !SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)))
            {
                Log("[files] cannot read File::Find(PathId)\n");
                return false;
            }
            for (int i = 0; i + 7 <= static_cast<int>(sizeof(code)); ++i)
            {
                const bool rexw = (code[i] & 0xF8) == 0x48;
                if (!rexw || code[i + 1] != 0x8B || (code[i + 2] & 0xC7) != 0x05)
                    continue;
                std::int32_t disp;
                std::memcpy(&disp, code + i + 3, 4);
                const std::uintptr_t var = fn + i + 7 + disp;
                std::uintptr_t head = 0;
                std::uint8_t isnil = 0;
                const bool ok = Rd(var, head) && head && Rd(head + 0x19, isnil) && isnil == 1;
                Log("[files] candidate mov at +0x%X -> var %p head %p isnil %u %s\n", i, reinterpret_cast<void*>(var),
                    reinterpret_cast<void*>(head), isnil, ok ? "ACCEPTED" : "rejected");
                if (ok)
                {
                    g_filesVar = var;
                    return true;
                }
            }
            Log("[files] g_files not found in File::Find(PathId)\n");
            return false;
        }

        // In-order walk with an explicit stack; calls fn(key, handle) per node. Returns node count or -1.
        template <typename F>
        int Walk(F fn)
        {
            if (!ResolveMap())
                return -1;
            std::uintptr_t head = 0, root = 0;
            if (!Rd(g_filesVar, head) || !Rd(head + 0x08, root))
                return -1;
            static std::uintptr_t stack[256];
            int sp = 0, count = 0;
            std::uintptr_t cur = root;
            for (;;)
            {
                std::uint8_t nil = 1;
                while (cur && Rd(cur + 0x19, nil) && !nil && sp < 256)
                {
                    stack[sp++] = cur;
                    if (!Rd(cur + 0x00, cur))
                        return -1;
                }
                if (sp == 0)
                    break;
                cur = stack[--sp];
                std::uint64_t key = 0, handle = 0;
                if (!Rd(cur + 0x20, key) || !Rd(cur + 0x28, handle))
                    return -1;
                fn(key, handle);
                if (++count >= kMaxNodes || !Rd(cur + 0x10, cur))
                    break;
            }
            return count;
        }
    }

    int Dump(int rawCount)
    {
        Log("[files] ---- g_files dump ----\n");
        static int extCount[0x2000];
        std::memset(extCount, 0, sizeof(extCount));
        bool knownHit[sizeof(kKnown) / sizeof(kKnown[0])] = {};
        int raw = 0;
        const int n = Walk([&](std::uint64_t key, std::uint64_t handle)
        {
            ++extCount[(key >> kExtShift) & 0x1FFF];
            for (size_t k = 0; k < sizeof(kKnown) / sizeof(kKnown[0]); ++k)
            {
                if ((key & kBaseMask) == (kKnown[k].code & kBaseMask))
                {
                    knownHit[k] = true;
                    Log("[files] MATCH %s key %016llX (ours %016llX, %s) handle %016llX\n", kKnown[k].path, key,
                        kKnown[k].code, key == kKnown[k].code ? "exact" : "flag/ext bits differ", handle);
                }
            }
            if (raw < rawCount)
            {
                ++raw;
                Log("[files] key %016llX handle %016llX\n", key, handle);
            }
        });
        if (n < 0)
        {
            Log("[files] walk failed\n");
            return -1;
        }
        Log("[files] %d entries\n", n);
        for (int e = 0; e < 0x2000; ++e)
            if (extCount[e])
                Log("[files] ext code %04X: %d\n", e, extCount[e]);
        for (size_t k = 0; k < sizeof(kKnown) / sizeof(kKnown[0]); ++k)
            if (!knownHit[k])
                Log("[files] not present: %s\n", kKnown[k].path);
        Log("[files] ---- end ----\n");
        return n;
    }

}
