// GekkoFull.cpp 29/09/2026. The gekko on TPP's full AnimationController (tpp::gm::impl::AnimationControllerImpl), set up
// the way Survive sets up its gekko (Survive has no Basic controller caller at all). Sources:
//   Survive gekko setup 0x140c29ac0 -> 0x140c56a00 -> create 0x140bc1a20; per frame FUN_140c56850 (Ghidra, mgv.exe);
//   TPP create 0x140af0790 -> Init 0x140af1ae0 -> unit SetUp 0x140b6d980 -> SetupCharaControl 0x140b6f0b0 (Ghidra);
//   slot mapping, unit getters checked by their code (docs/DOCUMENTATION.md).
// The AnimationControllerDesc is TPP's layout; Survive's is the same with one extra qword before +0x38.
#include <Windows.h>
#include <cmath>
#include <vector>
#include <cstdint>
#include <cstring>

#include "GekkoFull.h"
#include "GekkoHit.h"
#include "GekkoLegIk.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"
#include "GekkoType.h"

namespace
{
    // TPP concrete vtable of AnimationControllerImpl (create 0x140af0790 writes it) and the slots used, with the
    // function each must hold (checked at spawn; a mismatch refuses to run).
    constexpr std::uintptr_t kCtrlVtable = 0x1422e6070;
    struct Slot { unsigned off; std::uintptr_t fn; const char* name; };
    // Per-frame stages in RealizedSahelan2Impl::Execute's order (TPP native): +0x00, +0x08, +0x38, +0x20. Slots 1, 7 and 4
    // are composites: 1 = slots 2+3 (act motion), 7 = 8 ActCharaControlStart + 9, 4 = 5 PutMotionStart +
    // 6 PutMotionEnd. A run executed only stages 0, 3, 2 and 8: no put-motion, so the model never got a pose (invisible).
    constexpr Slot kActMotionStart   { 0x000, 0x140af0340, "ActMotionStart" };
    constexpr Slot kActMotion        { 0x008, 0x140af02b0, "act motion (slots 2+3)" };
    constexpr Slot kCharaControl2    { 0x038, 0x140af01b0, "ActCharaControlStart + slot 9" };
    constexpr Slot kPutMotion        { 0x020, 0x140af21f0, "PutMotionStart + PutMotionEnd" };
    constexpr Slot kSkip             { 0x068, 0x140af3470, "unit +0xac |= 2 (skip)" };
    constexpr Slot kRun              { 0x070, 0x140af3850, "unit +0xac &= ~2 (run)" };
    constexpr Slot kSetDeltaTime     { 0x090, 0x140af2d50, "SetDeltaTime -> unit +0xa8" };
    constexpr Slot kSetMotion        { 0x1d0, 0x140af2f70, "set motion (Survive +0x1f0)" };
    constexpr Slot kAnimControl      { 0x400, 0x140af0a00, "unit +0x70 AnimControl" };
    constexpr Slot kCharaControl     { 0x478, 0x140af1110, "unit +0x60 CharaControl" };
    // Realize(index) -> 0x140b6c810(unit): if unit +0xac bit 0 is clear, 0x140b6fd00 calls every parts builder of the
    // unit's director (unit +0x58) through vfunc +0x28, then sets bit 0. A live unit dump showed that without
    // it +0xac stayed 4, the model never appeared and ActCharaControlStart (needs bit 0) never ticked the CharaControl.
    constexpr Slot kRealize          { 0x050, 0x140af0410, "Realize unit (parts builders, +0xac |= 1)" };
    // Hit ownership, found by scanning vtable 0x1422e6070 for the slots that reach PartsBuildDirector::SetOwnerId
    // 0x1404e0730 and CharaControl::SetGameObjectIdToTrapEvent 0x141ad4190 (TPP decomp: SetOwnerId(unit, u64),
    // SetGameObjectIdToCharaControl(unit, GameObjectId)). Natives pass their own game object id.
    constexpr Slot kSetOwnerId       { 0x088, 0x140af3430, "SetOwnerId -> parts targets" };
    constexpr Slot kSetCcObjectId    { 0x488, 0x140af2e40, "SetGameObjectIdToCharaControl" };
    constexpr Slot kAll[] = { kActMotionStart, kActMotion, kCharaControl2, kPutMotion, kRealize, kSkip, kRun,
                              kSetDeltaTime, kSetMotion, kAnimControl, kCharaControl, kSetOwnerId, kSetCcObjectId };

    // PartsBuildDirector::SetOwnerId takes the game object id tagged 0x88 in the top byte: natives pass
    // 0x8800000000000000 | id (TppVehicle2AttachmentBase 0x8800000000006400 for type 50; live target objects of natives
    // hold e.g. 0x8800000000005a07,). Ours held a bare 0xc800 and no hit resolved to the game object.
    constexpr std::uint64_t kOwnerTag = 0x8800000000000000ull;
    constexpr std::uint64_t kMotionLayer = 0xE6FBF8FAD071ull;   // TppMotionDesc +0, same id Survive's gekko passes
    constexpr std::uintptr_t kCcObjVtable = 0x1424c8ee0;         // CharaControl object vtable (GekkoSigs, measured)

    struct MotionDesc
    {
        std::uint64_t layerMask, ganiPathId;
        std::uint32_t pathArg;
        float blend, pathWeight;
        std::uint32_t field1c;
        std::uint16_t field20, layerCount;
        std::uint32_t pad;
    };
    static_assert(sizeof(MotionDesc) == 0x28, "TppMotionDesc layout");
    struct alignas(16) Vec4 { float x, y, z, w; };

    using Create_t   = void* (__fastcall*)(void* desc);
    using DescCtor_t = void* (__fastcall*)(void* desc);
    using LayerCtor_t = void* (__fastcall*)(void* entry);
    using AnimDescInit_t = void* (__fastcall*)(void* animDesc, void* layers, std::uint32_t count, std::uint32_t arg);
    using AddMtar_t  = bool (__fastcall*)(void* anim, void* mtar);
    using Stage_t    = void (__fastcall*)(void* ctrl);
    using Index_t    = void (__fastcall*)(void* ctrl, unsigned index);
    using Get_t      = void* (__fastcall*)(void* ctrl, unsigned index);
    using SetDt_t    = void (__fastcall*)(void* ctrl, unsigned index, float seconds);
    using SetMotion_t = void (__fastcall*)(void* ctrl, unsigned index, const MotionDesc* desc);
    using Warp_t     = void (__fastcall*)(void* self, const Vec4* pos, const Vec4* rot);
    using VelCreate_t = void** (__fastcall*)(void** out);
    using Regist_t   = void (__fastcall*)(void* action, void* charaControl, void* anim);

    alignas(16) std::uint8_t g_desc[0x100];
    alignas(16) std::uint8_t g_animDesc[0x60];
    alignas(16) std::uint8_t g_layer[0x20];
    void* g_partsFile = nullptr;   // desc +0x08 points here (File**, as Survive and Sahelan pass it)
    void* g_ctrl = nullptr;
    void* g_cc = nullptr;
    float g_yaw = 0.0f, g_wantYaw = 0.0f;
    bool g_steer = false;
    float g_move[3] = {};   // walk path correction, local frame, taken by the next run frame
    bool g_moveSet = false;
    // fox::cc::VelocityControlAction (TPP decomp CcVelocityControlAction.cpp Exec): each anim update it takes the clip's
    // root move and rotation, applies +0xc8 flags (8 = multiply the rotation by the quat at +0xa0), sends both to the
    // CharaControl and clears the flags. Survive's gekko gets one per unit (0x140c56ea0); steering goes in as that delta.
    void* g_vel = nullptr;   // ponytail: the SharedPtr reference is kept for the process lifetime, never released
    unsigned g_frames = 0;
    bool g_frozen = false;    // the gekko is dead: no clip, root motion or event runs any more
    double g_clock = 0.0;      // sum of every run frame's dt (RunClock)
    float g_runTime = 0.0f;   // seconds of run frames since the last ConsumeRunTime (the AI clock; 0 in pause)

    void* g_faultAt = nullptr;
    void* g_faultData = nullptr;
    int Fault(EXCEPTION_POINTERS* e)
    {
        g_faultAt = e->ExceptionRecord->ExceptionAddress;
        g_faultData = e->ExceptionRecord->NumberParameters >= 2 ? reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr;
        return 0;
    }

    template <typename F>
    bool Guard(const char* what, F fn)
    {
        DWORD code = 0;
        __try { fn(); }
        __except (code = GetExceptionCode(), Fault(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
        {
            static const char* last = nullptr;   // one line per distinct failing call, not one per frame
            if (last != what)
                Log("[full] %s raised exception 0x%08lX at %p (data %p)\n", what, code, g_faultAt, g_faultData);
            last = what;
            return false;
        }
        return true;
    }

    void* Fn(const Slot& s) { return (*static_cast<void***>(g_ctrl))[s.off / 8]; }
    template <typename T> T* At(std::size_t off) { return reinterpret_cast<T*>(g_desc + off); }

    bool CheckVtable()
    {
        const auto vt = *static_cast<std::uintptr_t**>(g_ctrl);
        const auto want = reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kCtrlVtable));
        bool ok = reinterpret_cast<std::uintptr_t>(vt) == want;
        Log("[full] controller %p vtable %p %s\n", g_ctrl, vt, ok ? "ok" : "MISMATCH (want 0x1422e6070)");
        for (const auto& s : kAll)
        {
            const bool m = vt[s.off / 8] == reinterpret_cast<std::uintptr_t>(ResolveGameAddress(s.fn));
            Log("[full]   +0x%03x %-40s %s\n", s.off, s.name, m ? "ok" : "MISMATCH");
            ok &= m;
        }
        return ok;
    }

    // Hit zones. The parts director (unit +0x58) keeps a count byte (+0x62) and a start index (+0x73) into its builder array
    // (+0x48) for TargetBuilders (PartsBuildDirector::GetBuilderFromStringId<TargetBuilder>, decomp; live dump:
    // count 1, start 41, builder +0x10 target unit, +0x38 target service non-null). Gimmicks register each target builder
    // under two categories; the old Basic-controller build did too and the clean-up dropped it. Logs and registers both.
    void RegisterTargets()
    {
        const auto reg = reinterpret_cast<void (__fastcall*)(void*, std::uint64_t)>(GekkoSig::CheckByName("TargetBuilder_RegisterCategory", "target"));
        auto* unit = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_ctrl) + 0x10);
        auto* dir = unit ? *reinterpret_cast<std::uint8_t**>(unit + 0x58) : nullptr;
        if (!dir || !reg) { Log("[target] no director or signature\n"); return; }
        const unsigned count = dir[0x62], start = dir[0x73];
        auto** arr = *reinterpret_cast<std::uint8_t***>(dir + 0x48);
        Log("[target] director %p owner %u: %u target builder(s) from index %u\n", dir, *reinterpret_cast<unsigned*>(dir + 0x38), count, start);
        static const std::uint64_t kCategories[2] = { 0xc363a4045373ull, 0x2414f356b0acull };
        for (unsigned i = 0; i < count; ++i)
        {
            auto* b = arr[start + i];
            Log("[target] builder %p vtable %p unit %p service %p\n", b, *reinterpret_cast<void**>(b), *reinterpret_cast<void**>(b + 0x10), *reinterpret_cast<void**>(b + 0x38));
            for (std::uint64_t cat : kCategories)
                if (Guard("RegisterTargetSpecifiedCategory", [&] { reg(b, cat); })) Log("[target] registered category %llx\n", static_cast<unsigned long long>(cat));
        }
    }

    // Parts director builder tables (count byte dir[0x59+k], start byte dir[0x6a+k], live: kind 9 targets count 1 start 41, kind 11 (sim) count 1 start 42, kind 10 (physics) 0).
    // Logs every non-empty kind and the first builder of the sim kind (0x60 bytes) so a missing .sim shows in the next log.
    void LogBuilders()
    {
        auto* unit = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_ctrl) + 0x10);
        auto* dir = unit ? *reinterpret_cast<std::uint8_t**>(unit + 0x58) : nullptr;
        if (!dir) return;
        auto** arr = *reinterpret_cast<std::uint8_t***>(dir + 0x48);
        for (unsigned k = 0; k < 17; ++k)
            if (dir[0x59 + k]) Log("[builders] kind %u: count %u start %u\n", k, dir[0x59 + k], dir[0x6a + k]);
        if (dir[0x59 + 11] && arr)
        {
            const std::uint8_t* b = arr[dir[0x6a + 11]];
            Log("[builders] sim builder %p vtable %p:", b, *reinterpret_cast<void* const*>(b));
            for (int i = 8; i < 0x60; i += 8) Log(" %016llx", static_cast<unsigned long long>(*reinterpret_cast<const std::uint64_t*>(b + i)));
            Log("\n");
        }
        else Log("[builders] no sim builder\n");
    }
    Vec4 YawQuat(float yaw) { return { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) }; }
    float F(const void* p, std::size_t off) { return *reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(p) + off); }
}

namespace GekkoFull
{
    bool IsLive() { return g_ctrl != nullptr; }
    // The gekko's hit-target objects over every target builder (director +0x48 array from start +0x73, count +0x62): builder +0x38 = TargetUnit,
    // unit +0x28 count, +0x30 object array . With the push-defense target the director has two builders ([builders] kind 9 count 2).
    bool TargetObjects(std::uint8_t*** objs, unsigned* count)
    {
        static std::vector<std::uint8_t*> all;
        all.clear();
        auto* unit = g_ctrl ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_ctrl) + 0x10) : nullptr;
        auto* dir = unit ? *reinterpret_cast<std::uint8_t**>(unit + 0x58) : nullptr;
        if (!dir || !dir[0x62]) return false;
        auto** arr = *reinterpret_cast<std::uint8_t***>(dir + 0x48);
        for (unsigned k = 0; k < dir[0x62]; ++k)
        {
            auto* b = arr[dir[0x73] + k];
            auto* tu = b ? *reinterpret_cast<std::uint8_t**>(b + 0x38) : nullptr;
            auto** o = tu ? *reinterpret_cast<std::uint8_t***>(tu + 0x30) : nullptr;
            if (!o) continue;
            for (unsigned i = 0, n = *reinterpret_cast<unsigned*>(tu + 0x28); i < n; ++i) all.push_back(o[i]);
        }
        *objs = all.data();
        *count = static_cast<unsigned>(all.size());
        return !all.empty();
    }
    void* Controller() { return g_ctrl; }
    void* CharaControl() { return g_cc; }
    bool Grounded() { return g_cc && (*reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(g_cc) + 0x1b0) & 0x20) != 0; }
    // Body heading from the CharaControl rotation quat at +0x70 ((0,-0.619,0,0.785) = -1.34 rad,
    // the heading the walk had set).
    float Yaw()
    {
        if (!g_cc) return g_yaw;
        return 2.0f * std::atan2(F(g_cc, 0x74), F(g_cc, 0x7c));
    }
    float ConsumeRunTime() { const float t = g_runTime; g_runTime = 0.0f; return t; }
    double RunClock() { return g_clock; }

    bool Spawn(void* partsFile, void* mtarFile, float x, float y, float z, float yaw)
    {
        if (g_ctrl) return true;
        const auto create = reinterpret_cast<Create_t>(GekkoSig::CheckByName("FullCtrl_Create", "full"));
        const auto descCtor = reinterpret_cast<DescCtor_t>(GekkoSig::CheckByName("AnimCtrlDesc_Ctor", "full"));
        const auto layerCtor = reinterpret_cast<LayerCtor_t>(GekkoSig::CheckByName("AnimLayerDesc_Ctor", "full"));
        const auto animInit = reinterpret_cast<AnimDescInit_t>(GekkoSig::CheckByName("AnimControlDesc_Init", "full"));
        const auto addMtar = reinterpret_cast<AddMtar_t>(GekkoSig::CheckByName("AnimControl_AddMtarFile", "full"));
        if (!create || !descCtor || !layerCtor || !animInit || !addMtar || !partsFile || !mtarFile)
        {
            Log("[full] Spawn refused: signature or file missing (parts %p mtar %p)\n", partsFile, mtarFile);
            return false;
        }
        g_partsFile = partsFile;
        if (!Guard("desc ctor", [&] { descCtor(g_desc); layerCtor(g_layer); animInit(g_animDesc, g_layer, 1, 0); })) return false;
        // AnimControl sub-desc as Survive's gekko fills it (0x140c29f58..): 1 layer, +0xc 1, +0x10 0xac, +0x1c 1 mtar
        // slot, +0x20 0xa. The layer entry keeps its ctor defaults (Survive's port fill 0x140bc2cd0 needs a data entity).
        *reinterpret_cast<std::uint32_t*>(g_animDesc + 0x0c) = 1;
        *reinterpret_cast<std::uint32_t*>(g_animDesc + 0x10) = 0xac;
        *reinterpret_cast<std::uint32_t*>(g_animDesc + 0x1c) = 1;
        *reinterpret_cast<std::uint32_t*>(g_animDesc + 0x20) = 0xa;
        // AnimationControllerDesc: Survive gekko values (0x140c29e63..0x140c29f84) moved to TPP offsets (-8 from +0x38).
        *At<std::uint64_t>(0x00) = 0x1000000000020ull;
        *At<void**>(0x08) = &g_partsFile;
        *At<void*>(0x20) = g_animDesc;
        // SetSize(+0x3c radius, +0x38 height). Survive writes height 0 (xorps at 0x140c29f02) and radius 0.4, but TPP's
        // CharaControl then keeps the feet 0.4 above the ground (+0x198), never grounds (state 0x11, fall speed +0x64
        // grew to -16839) and freezes. Every grounded native measured has height 0.8 / radius 0.4 (live).
        *At<float>(0x38) = 0.8f;
        *At<float>(0x3c) = 0.4f;
        *At<float>(0x40) = 0.1f;       // Survive +0x48 = 0x14200af58
        *At<float>(0x44) = 0.001f;     // Survive +0x4c = 0x14205b3e4
        *At<std::uint32_t>(0x48) = 1;  // units
        *At<std::uint32_t>(0x4c) = 0;  // start index
        *At<std::uint32_t>(0x50) |= 2; // use the flags at +0x6c
        *At<std::uint32_t>(0x54) = 1;  // Survive +0x5c when a parts file is given
        *At<std::uint32_t>(0x58) = 1;  // Survive +0x60: no data entity object
        *At<std::uint32_t>(0x5c) = 1;  // Survive +0x64
        // CharaControl flags (| 0x800 by SetupCharaControl). Survive's gekko uses 0x13 (no bit 2, the geo collision body);
        // every grounded native measured has bit 2 (0x81f/0x91e/0x91f). Bit 2 on for bullet and player collision; its
        // TagContainer is desc +0x70 from the default ctor (the zeroed one faulted at 0x14001D630, test 4).
        *At<std::uint32_t>(0x6c) = 0x17;
        Log("[full] create desc %p (parts %p, animDesc %p)\n", g_desc, partsFile, g_animDesc);
        if (!Guard("FullCtrl_Create", [&] { g_ctrl = create(g_desc); }) || !g_ctrl) { g_ctrl = nullptr; return false; }
        if (!CheckVtable()) { Log("[full] vtable check failed, controller left alone\n"); g_ctrl = nullptr; return false; }

        void* anim = nullptr;
        bool added = false;
        Guard("AnimControl", [&] { anim = reinterpret_cast<Get_t>(Fn(kAnimControl))(g_ctrl, 0); });
        if (anim) Guard("AddMtarFile", [&] { added = addMtar(anim, mtarFile); });
        Guard("CharaControl", [&] { g_cc = reinterpret_cast<Get_t>(Fn(kCharaControl))(g_ctrl, 0); });
        const auto ccVt = g_cc ? *static_cast<std::uintptr_t*>(g_cc) : 0;
        Log("[full] AnimControl %p AddMtarFile %d, CharaControl %p vtable %llx (%s)\n", anim, added, g_cc,
            static_cast<unsigned long long>(ccVt),
            ccVt == reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kCcObjVtable)) ? "is a CharaControl" : "NOT a CharaControl");
        if (!added) return false;
        if (ccVt != reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kCcObjVtable))) g_cc = nullptr;
        g_yaw = g_wantYaw = yaw;
        if (g_cc && anim)
        {
            const auto velCreate = reinterpret_cast<VelCreate_t>(GekkoSig::CheckByName("VelocityCtl_Create", "full"));
            const auto regist = reinterpret_cast<Regist_t>(GekkoSig::CheckByName("CcAction_RegistAction", "full"));
            if (velCreate && regist && !g_vel)
                Guard("VelocityControlAction", [&] { velCreate(&g_vel); if (g_vel) regist(g_vel, g_cc, anim); });
            Log("[full] VelocityControlAction %p registered on CharaControl %p / AnimControl %p\n", g_vel, g_cc, anim);
        }
        if (g_cc)
        {
            const float h = F(g_cc, 0x170);
            const Vec4 pos{ x, y + h, z, 1.0f }, rot = YawQuat(yaw);   // Warp takes the centre (feet + height)
            void* iface = static_cast<std::uint8_t*>(g_cc) + 0x10;
            Guard("Warp", [&] { reinterpret_cast<Warp_t>((*static_cast<void***>(iface))[13])(iface, &pos, &rot); });
            Log("[full] warp -> feet %.2f %.2f %.2f, height %.2f, cc flags %08x\n", F(g_cc, 0x20), F(g_cc, 0x24), F(g_cc, 0x28), h,
                *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(g_cc) + 0x188));
        }
        // Motion points: desc +0x10 (motion point data object) is null for the gekko, as in Survive, and unit SetUp leaves
        // unit +0xb0 = 0 and unit +0x98 = null. Survive's unit act-motion (0x140c37660) skips when +0xb0 == 0; TPP's
        // (0x140b6c410) does not and writes [+0x98]+6, +0, +4 (test 9 fault at 0x140b6c42e). With a zero count those
        // three writes are all it does, so a zeroed scratch state stands in for the missing buffer.
        auto* unit = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_ctrl) + 0x10);
        static std::uint8_t s_motionPointState[0x100];
        if (!*reinterpret_cast<void**>(unit + 0x98) && *reinterpret_cast<std::uint32_t*>(unit + 0xb0) == 0)
        {
            *reinterpret_cast<void**>(unit + 0x98) = s_motionPointState;
            Log("[full] no motion points (unit +0xb0 = 0): unit +0x98 -> scratch %p\n", s_motionPointState);
        }
        auto unitFlags = [] { return *reinterpret_cast<std::uint32_t*>(*reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(g_ctrl) + 0x10) + 0xac); };
        const std::uint32_t before = unitFlags();
        const bool realized = Guard("Realize", [&] { reinterpret_cast<Index_t>(Fn(kRealize))(g_ctrl, 0); });
        Log("[full] Realize -> %s, unit +0xac %08x -> %08x\n", realized ? "ok" : "FAULT", before, unitFlags());
        const unsigned id = GekkoType::GameObjectId();
        using SetOwner_t = void (__fastcall*)(void* ctrl, unsigned unit, std::uint64_t owner);
        using SetCcId_t = void (__fastcall*)(void* ctrl, unsigned unit, std::uint16_t id);
        const bool owned = realized && Guard("SetOwnerId", [&] { reinterpret_cast<SetOwner_t>(Fn(kSetOwnerId))(g_ctrl, 0, kOwnerTag | id); })
            && Guard("SetGameObjectIdToCharaControl", [&] { reinterpret_cast<SetCcId_t>(Fn(kSetCcObjectId))(g_ctrl, 0, static_cast<std::uint16_t>(id)); });
        Log("[full] owner id %u -> parts targets and CharaControl: %s\n", id, owned ? "ok" : "FAILED");
        RegisterTargets();
        GekkoHit::Install();
        LogBuilders();
        g_frames = 0;
        return realized;
    }

    bool Play(std::uint64_t ganiPathId, float blend)
    {
        if (!g_ctrl) return false;
        MotionDesc d{};
        d.layerMask = kMotionLayer;
        d.ganiPathId = ganiPathId;
        d.blend = blend;
        return Guard("SetMotion", [&] { reinterpret_cast<SetMotion_t>(Fn(kSetMotion))(g_ctrl, 0, &d); });
    }

    // fox::CoreFramework::GetGameTime (retail 0x1400641e0, signature checked): the GameTime object; dt at +8, rate * dt at +0xe0.
    void* g_gameTimeFn = nullptr;
    bool g_gameTimeTried = false;
    bool GameDt(float* raw, float* scaled)
    {
        if (!g_gameTimeTried) { g_gameTimeTried = true; g_gameTimeFn = GekkoSig::CheckByName("CoreFramework_GetGameTime", "full"); }
        void* fn = g_gameTimeFn;
        if (!fn) return false;
        __try
        {
            const auto* gt = static_cast<const std::uint8_t*>(reinterpret_cast<void* (__fastcall*)()>(fn)());
            if (!gt) return false;
            *raw = *reinterpret_cast<const float*>(gt + 8);
            *scaled = *reinterpret_cast<const float*>(gt + 0xe0);
            return *raw >= 0.0f && *scaled >= 0.0f && *scaled < 5.0f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Execute(const void* ctx)
    {
        if (!g_ctrl) return false;
        // Per instance run/skip, as RealizedSahelan2Impl::Execute 0x140b4db20 does: run when the instance's bit in the
        // engine's bitmap equals ctx +8, else skip (unit +0xac bit 1, which ActCharaControlStart honours). This is what
        // stops natives in pause. Instance 0 = bit 0 (our exec entry has one instance).
        const auto* c = static_cast<const std::uint8_t*>(ctx);
        const std::uint32_t flag = *reinterpret_cast<const std::uint32_t*>(c + 8);
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(c + 0xc);
        const std::uint8_t* bits = *reinterpret_cast<std::uint8_t* const*>(c + 0x10);
        const std::uint32_t bit = count > 0 && bits ? (bits[0] & 1) : 1;
        const int run = bit == flag ? 1 : 0;
        if (g_frames < 3 || g_frames % 600 == 0)   // the engine alternates run and skip every call (serial / parallel slot): a change test logged two lines per frame (32 MB log, hitches,)
            Log("[full] frame %u ctx flag %u count %u bit %u -> %s\n", g_frames, flag, count, bit, run ? "run" : "skip");
        if (g_frozen) { Guard("skip", [&] { reinterpret_cast<Index_t>(Fn(kSkip))(g_ctrl, 0); }); return false; }   // dead: the pause path, the last pose stays
        Guard(run ? "run" : "skip", [&] { reinterpret_cast<Index_t>(Fn(run ? kRun : kSkip))(g_ctrl, 0); });

        // dt: the engine's game time, as natives get it. GameTime::Update (retail 0x1400816a0) stores the frame dt at +8 and rate * dt at +0xe0 and forces dt to 0 while the pause system's mask is
        // set, so the pause menu stops the controller (clips, root motion, events) and the brain (ConsumeRunTime is 0). (FoxGameTime.cpp:188 and the retail body); which of +8 and
        // +0xe0 natives use is not read: +0xe0 (scaled) is fed, both are logged on change.
        float dt = 0.0f;
        if (run)
        {
            float raw = 0.0f, scaled = 0.0f;
            if (GameDt(&raw, &scaled)) dt = scaled;
            else { static bool said = false; if (!said) { said = true; Log("[full] game time unreadable: the gekko stands still (dt 0)\n"); } }
            static float lastScaled = -1.0f, lastRaw = -1.0f;
            static int dtLogs = 0;
            if ((raw != lastRaw || scaled != lastScaled) && (raw == 0.0f || lastRaw == 0.0f || dtLogs < 6) && dtLogs < 60) { ++dtLogs; Log("[full] game dt raw %.5f scaled %.5f (frame %u)\n", raw, scaled, g_frames); }
            lastRaw = raw; lastScaled = scaled;
            Guard("SetDeltaTime", [&] { reinterpret_cast<SetDt_t>(Fn(kSetDeltaTime))(g_ctrl, 0, dt); });
            g_runTime += dt;
            g_clock += dt;
        }

        // Manual heading (SetYaw): the delta since the last run frame as the action's delta rotation (flag 8), read and cleared by its Exec inside the act-motion stage below. One shot.
        if (run && g_vel && g_steer)
        {
            g_steer = false;
            float d = g_wantYaw - Yaw();
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            if (std::fabs(d) > 1e-4f)
            {
                auto* v = static_cast<std::uint8_t*>(g_vel);
                *reinterpret_cast<Vec4*>(v + 0xa0) = YawQuat(d);
                *reinterpret_cast<std::uint32_t*>(v + 0xc8) |= 8;
            }
        }

        // Walk path correction (SetMove): added to the root motion's translation by the action's flag 2 (VelocityControlAction::Exec 0x14193e2d0: translation += this+0x90), consumed there.
        if (run && g_vel && g_moveSet)
        {
            g_moveSet = false;
            auto* v = static_cast<std::uint8_t*>(g_vel);
            std::memcpy(v + 0x90, g_move, 3 * sizeof(float));
            *reinterpret_cast<float*>(v + 0x9c) = 0.0f;
            *reinterpret_cast<std::uint32_t*>(v + 0xc8) |= 2;
        }

        GekkoLegIk::Tick(g_ctrl);   // installs the leg fit plugin when its switch is on and the controller is ready

        // Stages in RealizedSahelan2Impl::Execute order (see kActMotionStart).
        static const Slot* const kStages[] = { &kActMotionStart, &kActMotion, &kCharaControl2, &kPutMotion };
        for (const Slot* s : kStages)
            if (!Guard(s->name, [&] { reinterpret_cast<Stage_t>(Fn(*s))(g_ctrl); })) break;
        if (g_cc && (g_frames < 3 || g_frames % 120 == 0))
            Log("[full] frame %u feet %.3f %.3f %.3f dt %.4f cc state %08x\n", g_frames, F(g_cc, 0x20), F(g_cc, 0x24), F(g_cc, 0x28),
                dt, *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(g_cc) + 0x1b0));
        ++g_frames;
        return run != 0;
    }

    bool Feet(float* xyz)
    {
        if (!g_cc) return false;
        xyz[0] = F(g_cc, 0x20); xyz[1] = F(g_cc, 0x24); xyz[2] = F(g_cc, 0x28);
        return true;
    }

    void SetYaw(float yaw) { g_wantYaw = yaw; g_steer = true; }
    void SetMove(const float* v) { std::memcpy(g_move, v, sizeof(g_move)); g_moveSet = true; }

    // The CharaControl's own turn, as the soldiers use it (Soldier2ActionUtil::SetTurnTarget 0x141605120 calls the interface's SetTurnTarget and SetRotInterp): flag 0x100 in the flag word cc + 0x188
    // makes ControlActImpl (0x141940f60) slerp the rotation, which the VelocityControlAction has just set from the clip's root rotation, toward the quaternion at cc + 0x90 by
    // 1 - pow(1 - 1/N, ticks * 0.2), N at cc + 0x1ac (N <= 1 means 8). Without the flag the engine copies the rotation into cc + 0x90 every frame. READ in CharaControl.cpp
    // (SetTurnTarget 0x141943ab0, UnsetTurn 0x141944640, SetRotInterp 0x1419439c0, ControlActImpl); the offsets are the primary object's, cc + 0x10 is the interface the decompile's `this` is.
    void SetTurn(bool on, float yaw, float n)
    {
        if (!g_cc) return;
        auto* cc = static_cast<std::uint8_t*>(g_cc);
        auto* flags = reinterpret_cast<std::uint32_t*>(cc + 0x188);
        if (on)
        {
            *reinterpret_cast<Vec4*>(cc + 0x90) = YawQuat(yaw);
            *reinterpret_cast<float*>(cc + 0x1ac) = n;
            *flags |= 0x100;
        }
        else
        {
            *flags &= ~0x100u;
            // Layout check, once: with the flag clear the engine copies the rotation (cc + 0x70) into the target (cc + 0x90) every frame, so after a few frames they are equal.
            static bool checked = false;
            if (!checked && g_frames >= 120)
            {
                checked = true;
                const bool same = F(g_cc, 0x90) == F(g_cc, 0x70) && F(g_cc, 0x94) == F(g_cc, 0x74) && F(g_cc, 0x98) == F(g_cc, 0x78) && F(g_cc, 0x9c) == F(g_cc, 0x7c);
                Log("[full] turn target cc+0x90 %s the rotation cc+0x70 (flags %08x, N %.1f)\n", same ? "follows" : "does NOT follow", *flags, F(g_cc, 0x1ac));
            }
        }
    }
    bool TurnState(std::uint32_t* flags, float* n, float* targetYaw)
    {
        if (!g_cc) return false;
        auto* cc = static_cast<std::uint8_t*>(g_cc);
        *flags = *reinterpret_cast<std::uint32_t*>(cc + 0x188);
        *n = F(g_cc, 0x1ac);
        *targetYaw = 2.0f * std::atan2(F(g_cc, 0x94), F(g_cc, 0x9c));
        return true;
    }
    void Freeze(bool on) { g_frozen = on; Log("[full] %s\n", on ? "frozen: the last pose stays" : "unfrozen"); }

    void Despawn()
    {
        // ponytail: the controller is not freed (its delete slot is not read yet); it stops being driven.
        Log("[full] despawn (controller %p left allocated)\n", g_ctrl);
        GekkoLegIk::OnDespawn();
        g_ctrl = nullptr;
        g_cc = nullptr;
        g_vel = nullptr;
    }
}
