// GekkoLegIk.cpp 07/10/2026. Ground fit for the gekko's two ANIMAL_LEG feet, as an animation plugin on the gekko's AnimControl. Switch default OFF (Gekko.LegIk, Ivar gekkoLegIk).
// What Survive does is not known (docs/DOCUMENTATION.md 5.12), so this is the native step of the ladder: TPP's own plugin machinery with the walk the engine itself uses for pose edits.
//   - The plugin object is a real TPP PluginDogIk (retail Create 0x140b82cc0, ctor 0x140b81d70, base fox::animx::PluginIk 0x141aba3c0 = Survive's 0x1419da1d0, the base of Survive's gekko plugin too),
//     made with the gekko's Model (controller unit +0x68, Survive reads the same through its slot 0x440 = TPP 0x3f0) and RigDef (AnimControl +0x58), then registered with AnimControl::AddPlugins
//     (retail 0x141a6bca0 = Survive 0x14198d440). Only vtable slot 3 (Exec) is replaced; the dog's other slots, destructor and refcounting stay the engine's.
//   - Exec(AnimControl*): AnimControl +0x58 RigDef, +0x60 RigPose, +0x10 -> dt at +0x44; the plugin's +0x68 is the Model with its world matrix at +0x40 (retail 0x140b82d60 reads exactly these).
//   - RigDef: unit count at +0xc, unit def offset table at +0x20 (4 bytes each), def +0 is the unit type (10 = ANIMAL_LEG). RigPose: unit offset table at [+8], count word at +0xc; an ANIMAL_LEG pose
//     unit holds the foot target at +0x40 (and its copy at +0x30). fox::anim::RigAnimalLegDef::TranslatePose (dev 0x141902960, decomp AnimRigAnimalLeg.cpp) adds a vector to both, which is what the
//     engine does for root motion; the leg solver then reaches the translated target. This code does the same per foot with the vertical offset the ground under the foot asks for.
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "GekkoFull.h"
#include "GekkoGround.h"
#include "GekkoLegIk.h"
#include "GekkoLegIkMath.h"
#include "GekkoLegIkPose.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"

namespace
{
    constexpr std::uintptr_t kDogVtable = 0x1422fa388;   // retail PluginDogIk vtable (written by the ctor at 0x140b81db9)
    constexpr std::uintptr_t kDogExec = 0x140b82d60;     // its slot 3
    constexpr unsigned kSlots = 7, kExecSlot = 3;        // the primary vtable has 7 slots (0x1422fa388..0x1422fa3b8); Survive's gekko plugin vtable 0x1422598b8 has the same 7
    constexpr int kAnimalLeg = 10;                       // RigDef unit type (RigPose::TranslatePose switch case 10)
    constexpr unsigned kMaxLegs = 4;
    constexpr float kMaxDelta = 0.6f;                    // metres a foot may leave the animation's height
    constexpr float kRate = 14.0f;                       // smoothing, 1 / seconds (about 70 ms)

    using Create_t = void* (__fastcall*)(void** out, void* model, void* rigDef, unsigned flags);
    using Add_t = void (__fastcall*)(void* anim, void* plugin);
    using Exec_t = void (__fastcall*)(void* self, void* anim);

    volatile LONG g_want = 0;        // the switch
    bool g_installed = false, g_failed = false, g_dead = false;
    unsigned g_tries = 0;
    volatile LONG g_execs = 0;
    void* g_plugin = nullptr;
    void* g_sharedRef = nullptr;     // ponytail: the SharedPtr reference is kept for the process lifetime, never released
    void* g_vt[kSlots];
    unsigned g_legCount = 0;
    float g_smooth[kMaxLegs] = {};
    const std::uint8_t* g_units[kMaxLegs] = {};
    char g_status[160] = "off";
    LARGE_INTEGER g_lastQpc = {};

    // Wall-clock frame time. The value at AnimControl +0x10 -> +0x44 that the dog copies into its +0xf0 measured 4.995 and 3.7462 (ratio 0.75) in the 07/10/2026 log, so it is not seconds here.
    float FrameDt()
    {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        const float dt = g_lastQpc.QuadPart ? static_cast<float>(now.QuadPart - g_lastQpc.QuadPart) / static_cast<float>(f.QuadPart) : 0.0f;
        g_lastQpc = now;
        return dt;
    }

    // 32-bit words of a structure, 8 per line, so the next run shows the real layout.
    void Dump(const char* what, const std::uint8_t* p, unsigned bytes)
    {
        for (unsigned o = 0; o < bytes; o += 32)
        {
            const std::uint32_t* w = reinterpret_cast<const std::uint32_t*>(p + o);
            const float* f = reinterpret_cast<const float*>(p + o);
            Log("[legik] dump %s +%02x: %08x %08x %08x %08x %08x %08x %08x %08x | %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", what, o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
    }

    struct CreateArgs { Create_t create; void* model; void* rigDef; void* out; };
    bool CreateGuarded(CreateArgs* a)
    {
        __try { a->create(&a->out, a->model, a->rigDef, 0); return a->out != nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[legik] Create raised exception 0x%08lX\n", GetExceptionCode()); return false; }
    }
    bool AddGuarded(Add_t add, void* anim, void* plugin)
    {
        __try { add(anim, plugin); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[legik] AddPlugins raised exception 0x%08lX\n", GetExceptionCode()); return false; }
    }

    // Per frame, inside the animation job: for each ANIMAL_LEG pose unit lift or lower the foot target to the ground under it.
    void ExecBody(void* self, std::uint8_t* ac)
    {
        if (!InterlockedCompareExchange(&g_want, 0, 0))
        {
            for (float& s : g_smooth) s = 0.0f;   // off: the pose is rebuilt from the animation every frame, nothing to undo
            return;
        }
        const std::uint8_t* model = *reinterpret_cast<std::uint8_t* const*>(static_cast<std::uint8_t*>(self) + 0x68);
        const std::uint8_t* rigDef = *reinterpret_cast<std::uint8_t* const*>(ac + 0x58);
        std::uint8_t* pose = *reinterpret_cast<std::uint8_t**>(ac + 0x60);
        if (!model || !rigDef || !pose) return;
        const float* m = reinterpret_cast<const float*>(model + 0x40);
        const std::uint8_t* state = *reinterpret_cast<std::uint8_t* const*>(ac + 0x10);
        const float engDt = state ? *reinterpret_cast<const float*>(state + 0x44) : 0.0f;
        const float dt = FrameDt();
        const unsigned defCount = *reinterpret_cast<const std::uint32_t*>(rigDef + 0xc);
        const unsigned poseCount = *reinterpret_cast<const std::uint16_t*>(pose + 0xc);
        const bool grounded = GekkoFull::Grounded();
        const LONG n = InterlockedIncrement(&g_execs);
        const bool say = n <= 6 || n % 240 == 0;
        const bool dump = n == 1 || n % 60 == 0;   // about once a second
        if (dump)
        {
            Dump("anim", ac + 0x50, 0x60);
            Dump("model40", model + 0x40, 0x40);
            Dump("poseHdr", pose, 0x40);
        }

        GekkoLegIkPose::Leg found[kMaxLegs];
        const unsigned legs = GekkoLegIkPose::FindLegs(rigDef, pose, found, kMaxLegs);
        for (unsigned l = 0; l < legs; ++l)
        {
            std::uint8_t* unit = found[l].unit;
            if (g_units[l] != unit) { g_units[l] = unit; g_smooth[l] = 0.0f; }
            const float* t = GekkoLegIkPose::FootTarget(unit);
            const float foot[3] = { t[0], t[1], t[2] };
            float world[3];
            GekkoLegIkMath::Transform(m, foot, world);
            const float feetY = m[13];
            float groundY = 0.0f;
            const bool hit = grounded && GekkoGround::Height(world[0], feetY, world[2], &groundY);
            // The pose unit target is not in model space: RigAnimalLegDef::UpdatePosePost (dev 0x141902a20, AnimRigAnimalLeg.cpp) subtracts the leg root joint translation from +0x40, so it is relative to that joint
            // (measured: foot obj y -1.96 with the model origin on the ground). Until the root joint position is read, a foot more than kMaxDelta from the model plane is not in the space we assume: leave it.
            const bool modelSpace = world[1] > feetY - kMaxDelta && world[1] < feetY + kMaxDelta;
            const float want = modelSpace ? GekkoLegIkMath::FootDelta(hit, groundY, feetY, kMaxDelta) : 0.0f;
            if (dt > 0.0f && dt < 0.25f) g_smooth[l] = GekkoLegIkMath::Smooth(g_smooth[l], want, dt, kRate);
            const float dy = g_smooth[l];
            if (dy > 1e-4f || dy < -1e-4f)
            {
                float d[3];
                GekkoLegIkMath::ObjectUp(m, dy, d);
                GekkoLegIkPose::Translate(unit, d);
            }
            if (dump)
            {
                Dump("unit", unit, 0x60);
                const std::uint32_t dOff = *reinterpret_cast<const std::uint32_t*>(rigDef + 0x20 + 4 * found[l].index);
                Dump("legdef", rigDef + dOff, 0x40);
            }
            if (say)
                Log("[legik] exec %ld leg %u unit %u modelSpace %d engDt %.4f: foot obj %.3f %.3f %.3f world %.3f %.3f %.3f feetY %.3f grounded %d ground %s %.3f want %.3f applied %.3f dt %.4f\n", n, l, found[l].index,
                    foot[0], foot[1], foot[2], world[0], world[1], world[2], feetY, grounded ? 1 : 0, hit ? "hit" : "none", groundY, want, dy, dt);
        }
        g_legCount = legs;
        if (say && (n <= 2))
            Log("[legik] layout: RigDef units %u, RigPose units %u, ANIMAL_LEG units used %u, model matrix col3 %.3f %.3f %.3f\n", defCount, poseCount, legs, m[12], m[13], m[14]);
    }

    void __fastcall ExecHook(void* self, void* ac)
    {
        if (g_dead) return;
        __try { ExecBody(self, static_cast<std::uint8_t*>(ac)); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_dead = true;   // one fault and the plugin goes quiet for good (the pose is the engine's again)
            Log("[legik] Exec raised exception 0x%08lX: leg fit disabled until the next spawn\n", GetExceptionCode());
        }
    }

    // Returns true when the plugin is installed or the attempt is final (g_failed set); false means "not ready yet, try again next frame".
    bool TryInstall(void* ctrl)
    {
        const auto create = reinterpret_cast<Create_t>(GekkoSig::CheckByName("Dog_CreatePlugin", "legik"));
        const auto add = reinterpret_cast<Add_t>(GekkoSig::CheckByName("AnimControl_AddPlugins", "legik"));
        auto* vt = static_cast<std::uintptr_t*>(ResolveGameAddress(kDogVtable));
        if (!create || !add || !vt || vt[kExecSlot] != reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kDogExec)))
        {
            Log("[legik] install refused: signature or dog vtable check failed (create %p add %p vtable %p)\n", reinterpret_cast<void*>(create), reinterpret_cast<void*>(add), static_cast<void*>(vt));
            g_failed = true;
            return true;
        }
        // Controller unit 0: Model = unit +0x68 (TPP slot 0x3f0, 0x140af1340), AnimControl = unit +0x70 (slot 0x400, 0x140af0a00); Survive's plugin builder 0x140c59220 reads the same two through its slots 0x440 / 0x450.
        auto* unit = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(ctrl) + 0x10);
        void* model = unit ? *reinterpret_cast<void**>(unit + 0x68) : nullptr;
        auto* anim = unit ? *reinterpret_cast<std::uint8_t**>(unit + 0x70) : nullptr;
        void* rigDef = anim ? *reinterpret_cast<void**>(anim + 0x58) : nullptr;
        // AddPlugins refuses a plugin whose flags have bit 4 (every PluginIk) while AnimControl +0xd0 is null.
        if (!model || !anim || !rigDef || !*reinterpret_cast<void**>(anim + 0xd0)) return false;
        const unsigned defCount = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(rigDef) + 0xc);
        if (defCount == 0 || defCount > 512)
        {
            Log("[legik] install refused: RigDef %p has %u units\n", rigDef, defCount);
            g_failed = true;
            return true;
        }
        CreateArgs a{ create, model, rigDef, nullptr };
        if (!CreateGuarded(&a)) { g_failed = true; return true; }
        auto* plugin = static_cast<void**>(a.out);
        if (reinterpret_cast<std::uintptr_t>(*plugin) != reinterpret_cast<std::uintptr_t>(vt))
        {
            Log("[legik] install refused: new plugin %p has vtable %p, want the dog's %p\n", static_cast<void*>(plugin), *plugin, static_cast<void*>(vt));
            g_failed = true;
            return true;
        }
        std::memcpy(g_vt, vt, sizeof g_vt);
        g_vt[kExecSlot] = reinterpret_cast<void*>(&ExecHook);
        *plugin = g_vt;
        const unsigned before = *reinterpret_cast<std::uint32_t*>(anim + 0x28);
        if (!AddGuarded(add, anim, plugin)) { g_failed = true; return true; }
        const unsigned after = *reinterpret_cast<std::uint32_t*>(anim + 0x28);
        Log("[legik] installed: plugin %p model %p rigDef %p (%u units) AnimControl %p, plugin count %u -> %u%s\n", static_cast<void*>(plugin), model, rigDef, defCount, static_cast<void*>(anim), before, after,
            after > before ? "" : " (NOT added: AddPlugins returned early)");
        g_sharedRef = a.out;
        g_plugin = plugin;
        g_installed = after > before;
        g_failed = !g_installed;
        return true;
    }
}

namespace GekkoLegIk
{
    void SetEnabled(bool on)
    {
        InterlockedExchange(&g_want, on ? 1 : 0);
        Log("[legik] switch %s (installed %d)\n", on ? "ON" : "OFF", g_installed ? 1 : 0);
    }

    bool Enabled() { return InterlockedCompareExchange(&g_want, 0, 0) != 0; }

    void Tick(void* ctrl)
    {
        if (!Enabled() || g_installed || g_failed || !ctrl) return;
        if (++g_tries > 600) { g_failed = true; Log("[legik] install gave up: the controller never became ready (Model, AnimControl +0x58 / +0xd0)\n"); return; }
        TryInstall(ctrl);
    }

    void OnDespawn()
    {
        g_installed = g_failed = g_dead = false;
        g_tries = 0;
        g_plugin = nullptr;
        g_legCount = 0;
        for (auto& u : g_units) u = nullptr;
        for (float& s : g_smooth) s = 0.0f;
    }

    const char* Status()
    {
        std::snprintf(g_status, sizeof g_status, "switch %d installed %d failed %d dead %d execs %ld legs %u tries %u", Enabled() ? 1 : 0, g_installed ? 1 : 0, g_failed ? 1 : 0, g_dead ? 1 : 0,
                      static_cast<long>(g_execs), g_legCount, g_tries);
        return g_status;
    }
}
