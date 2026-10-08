// GekkoDriver.cpp 29/09/2026. Gekko spawn/animation driver on the full AnimationController (see GekkoDriver.h, GekkoFull).
// The Basic-controller path (CreateBasicAnimationController, SetUp, SetWorldMatrix per frame) was removed:
// Survive never uses a Basic controller for the gekko (docs/DOCUMENTATION.md), and the full path runs.
#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "GekkoDriver.h"
#include "GekkoBlocks.h"
#include "GekkoFull.h"
#include "GekkoSigCheck.h"
#include "GekkoTable.h"
#include "GekkoLog.h"

namespace Gekko
{
    namespace
    {
        // PathCode64 of our files (tools/pathcode.py, verified against modbldr 61,452/61,472). The engine keys its file
        // table by one of the two variants; which one is not verified, so FindFiles tries both and logs the hit.
        constexpr std::uint64_t kPartsPath[2] = { 0xE0AF934B21C9A656ull, 0xE0AB934B21C9A656ull }; // gec0_main0_def_v00.parts
        constexpr std::uint64_t kMtarPath[2]  = { 0x67052989CE8858FFull, 0x67012989CE8858FFull }; // Gecko_layers.mtar

        struct FilePtr { void* file; void* path; void* callback; std::uint64_t spare[5]; };  // EntityPtr + Path + cb
        using FindById_t = FilePtr* (__fastcall*)(FilePtr* out, std::uint64_t pathId);

        struct State
        {
            bool verified = false;
            int  debug = 1;
            Step step = Step::None;
            char lastError[256] = "";
            FindById_t findById = nullptr;
            FilePtr parts{}, mtar{};
            int lastSlot = -1;
        } g;

        void Fail(const char* fmt, ...)
        {
            va_list a;
            va_start(a, fmt);
            vsnprintf(g.lastError, sizeof(g.lastError), fmt, a);
            va_end(a);
            Log("[Gekko] ERROR %s\n", g.lastError);
        }

        void Dbg(int level, const char* fmt, ...)
        {
            if (g.debug < level)
                return;
            char buf[512];
            va_list a;
            va_start(a, fmt);
            vsnprintf(buf, sizeof(buf), fmt, a);
            va_end(a);
            Log("[Gekko] %s\n", buf);
        }

        template <typename F>
        bool Guard(const char* what, F fn)
        {
            DWORD code = 0;
            __try { fn(); }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                Fail("%s raised exception 0x%08lX", what, code);
                return false;
            }
            return true;
        }
    }

    bool Verify()
    {
        g.findById = reinterpret_cast<FindById_t>(GekkoSig::CheckByName("File_FindById", "Gekko"));
        g.verified = g.findById != nullptr;
        Log("[Gekko] Verify -> %s\n", g.verified ? "OK" : "FAILED (wrong exe build?)");
        return g.verified;
    }

    bool IsVerified() { return g.verified; }
    void SetDebugLevel(int level) { g.debug = level; Log("[Gekko] debug level %d\n", level); }
    Step GetStep() { return g.step; }
    const char* GetLastError() { return g.lastError; }

    bool FindFiles()
    {
        if (!g.verified && !Verify()) { Fail("FindFiles: driver not verified"); return false; }
        std::memset(&g.parts, 0, sizeof(g.parts));
        std::memset(&g.mtar, 0, sizeof(g.mtar));
        for (int v = 0; v < 2 && !g.parts.file; ++v)
            Guard("File::Find(parts)", [&] { g.findById(&g.parts, kPartsPath[v]); });
        for (int v = 0; v < 2 && !g.mtar.file; ++v)
            Guard("File::Find(mtar)", [&] { g.findById(&g.mtar, kMtarPath[v]); });
        // Measured: g_files holds only Lua scripts, so pack files are looked up in the loaded blocks.
        if (!g.parts.file) g.parts.file = GekkoBlocks::FindFile(kPartsPath[0]);
        if (!g.mtar.file) g.mtar.file = GekkoBlocks::FindFile(kMtarPath[0]);
        Dbg(1, "parts File* %p, mtar File* %p", g.parts.file, g.mtar.file);
        if (!g.parts.file || !g.mtar.file)
        {
            if (g.debug >= 2) GekkoBlocks::Dump();
            Fail("FindFiles: parts %p mtar %p (is the gekko pack loaded?)", g.parts.file, g.mtar.file);
            return false;
        }
        g.step = Step::FilesFound;
        return true;
    }

    void GetPose(float* xyzYaw)
    {
        if (!GekkoFull::Feet(xyzYaw)) { xyzYaw[0] = xyzYaw[1] = xyzYaw[2] = 0.0f; }
        xyzYaw[3] = GekkoFull::Yaw();
    }

    // The CharaControl owns the position (root motion); only the heading is ours.
    bool SetPose(float, float, float, float yaw)
    {
        if (!GekkoFull::IsLive()) return false;
        GekkoFull::SetYaw(yaw);
        return true;
    }


    bool Spawn(float x, float y, float z, float yaw)
    {
        Log("[Gekko] Spawn %.2f %.2f %.2f yaw %.2f (step now %d)\n", x, y, z, yaw, static_cast<int>(g.step));
        if (!g.verified && !Verify()) return false;
        if (g.step < Step::FilesFound && !FindFiles()) return false;
        if (!GekkoFull::Spawn(g.parts.file, g.mtar.file, x, y, z, yaw)) { Fail("full controller spawn failed"); return false; }
        g.step = Step::Placed;
        // Survive's gekko starts in its idle (slot 0, FUN_140f97310); an earlier run faulted in the stages every time
        // frame after Realize with no motion set.
        return Play(0, 0.0f);
    }

    bool PlayGani(std::uint64_t ganiPathId, float blend)
    {
        if (g.step < Step::Placed) { Fail("Play: gekko not spawned (step %d)", static_cast<int>(g.step)); return false; }
        return GekkoFull::Play(ganiPathId, blend);
    }

    bool Play(int slot, float blend)
    {
        if (slot < 0 || slot >= kGaniCount) { Fail("Play: slot %d out of range 0..%d", slot, kGaniCount - 1); return false; }
        Dbg(1, "Play slot %d gani %016llX", slot, kGaniTable[slot]);
        g.lastSlot = slot;
        return PlayGani(kGaniTable[slot], blend);
    }

    void Despawn()
    {
        Log("[Gekko] Despawn (step %d)\n", static_cast<int>(g.step));
        GekkoFull::Despawn();
        g.step = Step::None;
    }

    void LogStatus()
    {
        float p[4];
        GetPose(p);
        Log("[Gekko] status: verified %d debug %d step %d controller %p parts %p mtar %p feet %.2f %.2f %.2f yaw %.2f "
            "lastSlot %d lastError '%s'\n", g.verified ? 1 : 0, g.debug, static_cast<int>(g.step), GekkoFull::Controller(),
            g.parts.file, g.mtar.file, p[0], p[1], p[2], p[3], g.lastSlot, g.lastError);
    }
}
