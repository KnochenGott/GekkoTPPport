// GekkoBlocks.cpp 25/09/2026. Finds files inside loaded packs by walking fox's block groups.
// Layout from the steamless decomp (FoxCore_win64/FoxBlock.cpp), block route:
//   g_blockGroups StringMap: u32 first index S; entries = *(S+0x18) (stride 0x18, next index at +0x0c, 0xFFFFFFFF
//   ends); values = *(S+0x20) (BlockGroup*). Offsets +0x18/+0x20 are the dev distances cb8 -> cd0 / cd8.
//   BlockGroup +0x1c block count, +0x48 Block array (stride 0x178). Block +0x80 state (2 or 3 = loaded),
//   +0x90 BlockPackageBase* array, +0x150 package count. BlockPackageBase +0x08 package PathId.
// S is decoded at runtime from the first 32-bit rip-relative load in the (signature checked) retail
// BlockGroup::SendActModeChangedToAllBlockGroups; the two 64-bit loads there are logged and must equal S+0x18 and
// S+0x20, otherwise the walk is refused. The block-groups lock is not taken; every read is SEH-guarded.
#include <Windows.h>
#include <cstring>

#include "GekkoBlocks.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"
#include "GekkoSigs.h"

namespace GekkoBlocks
{
    namespace
    {
        constexpr std::uint64_t kGekkoPack = 0x5229F88596B9BFEEull;  // path_code /Assets/tpp/pack/gekko/gekko_dll.fpk
        constexpr int kMaxGroups = 512;

        std::uintptr_t g_start = 0;  // address of the u32 first index S
        bool g_tried = false;

        using GetFile_t = void* (__fastcall*)(void* block, std::uint64_t pathId);

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

        void* CallGetFile(GetFile_t fn, void* block, std::uint64_t id, bool& faulted)
        {
            faulted = false;
            __try
            {
                return fn(block, id);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
                return nullptr;
            }
        }

        std::uintptr_t Addr(const char* name)
        {
            for (const auto& s : GekkoSigs::kFuncs)
                if (std::strcmp(s.name, name) == 0)
                    return reinterpret_cast<std::uintptr_t>(ResolveGameAddress(s.address));
            return 0;
        }

        bool CheckSig(const char* name) { return GekkoSig::CheckByName(name, "blocks") != nullptr; }

        bool Resolve()
        {
            if (g_start || g_tried)
                return g_start != 0;
            g_tried = true;
            if (!CheckSig("BlockGroup_SendActModeChangedToAll") || !CheckSig("Block_GetFileForPathId"))
                return false;
            const std::uintptr_t fn = Addr("BlockGroup_SendActModeChangedToAll");
            std::uint8_t code[0x140];  //: in the first 0x80 bytes only S and S+0x20 are loaded
            if (!SafeRead(reinterpret_cast<void*>(fn), code, sizeof(code)))
                return false;
            std::uintptr_t s32 = 0, q[8] = {};
            int nq = 0;
            for (int i = 0; i + 7 <= static_cast<int>(sizeof(code)); ++i)
            {
                int rex = 0;
                int j = i;
                if ((code[j] & 0xF0) == 0x40) { rex = code[j]; ++j; }
                if (j + 6 > static_cast<int>(sizeof(code)) || code[j] != 0x8B || (code[j + 1] & 0xC7) != 0x05)
                    continue;
                std::int32_t disp;
                std::memcpy(&disp, code + j + 2, 4);
                const std::uintptr_t target = fn + j + 6 + disp;
                const bool wide = (rex & 0x08) != 0;
                Log("[blocks] rip load at +0x%X %s -> %p\n", i, wide ? "64" : "32", reinterpret_cast<void*>(target));
                if (!wide && !s32)
                    s32 = target;
                else if (wide && nq < 8)
                    q[nq++] = target;
                i = j + 5;
            }
            bool entriesSeen = false, valuesSeen = false;
            for (int k = 0; k < nq; ++k)
            {
                entriesSeen |= q[k] == s32 + 0x18;
                valuesSeen |= q[k] == s32 + 0x20;
            }
            Log("[blocks] first index %p, entries %s, values %s\n", reinterpret_cast<void*>(s32),
                entriesSeen ? "at +0x18 ok" : "NOT at +0x18", valuesSeen ? "at +0x20 ok" : "NOT at +0x20");
            if (!s32 || !entriesSeen || !valuesSeen)
            {
                Log("[blocks] layout does not match the decomp; block walk disabled\n");
                return false;
            }
            g_start = s32;
            return true;
        }

        // Calls fn(block, state, packageArray, packageCount, groupIndex, blockIndex) for every block.
        template <typename F>
        int Walk(F fn)
        {
            if (!Resolve())
                return -1;
            std::uint32_t idx = 0;
            std::uintptr_t entries = 0, values = 0;
            if (!Rd(g_start, idx) || !Rd(g_start + 0x18, entries) || !Rd(g_start + 0x20, values))
                return -1;
            int groups = 0, loaded = 0;
            while (idx != 0xFFFFFFFFu && groups < kMaxGroups)
            {
                std::uintptr_t group = 0;
                std::uint32_t count = 0;
                std::uintptr_t blocks = 0;
                if (!Rd(values + idx * 8ull, group) || !group || !Rd(group + 0x1c, count) || !Rd(group + 0x48, blocks))
                    return -1;
                for (std::uint32_t b = 0; b < count && b < 256; ++b)
                {
                    const std::uintptr_t block = blocks + b * 0x178ull;
                    std::int32_t state = 0;
                    std::uintptr_t pkgs = 0;
                    std::uint32_t npkg = 0;
                    if (!Rd(block + 0x80, state) || !Rd(block + 0x90, pkgs) || !Rd(block + 0x150, npkg))
                        return -1;
                    if (state == 2 || state == 3)
                        ++loaded;
                    fn(block, state, pkgs, npkg, groups, static_cast<int>(b));
                }
                ++groups;
                if (!Rd(entries + idx * 0x18ull + 0x0c, idx))
                    return -1;
            }
            return loaded;
        }
    }

    int Dump()
    {
        Log("[blocks] ---- block dump ----\n");
        bool gekkoSeen = false;
        const int n = Walk([&](std::uintptr_t block, std::int32_t state, std::uintptr_t pkgs, std::uint32_t npkg, int g, int b)
        {
            Log("[blocks] group %d block %d @ %p state %d packages %u\n", g, b, reinterpret_cast<void*>(block), state, npkg);
            for (std::uint32_t k = 0; k < npkg && k < 64; ++k)
            {
                std::uintptr_t pkg = 0;
                std::uint64_t id = 0;
                if (!Rd(pkgs + k * 8ull, pkg) || !pkg || !Rd(pkg + 0x08, id))
                    continue;
                const bool ours = id == kGekkoPack;
                gekkoSeen |= ours;
                Log("[blocks]    package %p id %016llX%s\n", reinterpret_cast<void*>(pkg), id, ours ? "  <-- gekko_dll.fpk" : "");
            }
        });
        Log("[blocks] %d loaded blocks, gekko_dll.fpk %s\n", n, gekkoSeen ? "FOUND" : "not found");
        Log("[blocks] ---- end ----\n");
        return n;
    }

    void* FindFile(std::uint64_t pathId)
    {
        const auto getFile = reinterpret_cast<GetFile_t>(Addr("Block_GetFileForPathId"));
        void* found = nullptr;
        Walk([&](std::uintptr_t block, std::int32_t state, std::uintptr_t, std::uint32_t, int g, int b)
        {
            if (found || (state != 2 && state != 3))
                return;
            bool faulted = false;
            void* f = CallGetFile(getFile, reinterpret_cast<void*>(block), pathId, faulted);
            if (faulted)
                Log("[blocks] GetFileForPathId faulted on group %d block %d\n", g, b);
            if (f)
            {
                Log("[blocks] %016llX found in group %d block %d -> File* %p\n", pathId, g, b, f);
                found = f;
            }
        });
        return found;
    }
}
