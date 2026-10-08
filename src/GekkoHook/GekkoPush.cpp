// GekkoPush.cpp 01/10/2026. The gekko's push-slide targets. Survive's gekko plugin (class vtable 0x1422ada20) creates three PUSH_SLIDE offense targets per unit (0x140f80e30, group FRIEND, flags 0x100000)
// and places them on bones 5, 0x23 and 0x10 every frame (0x140f82280). TPP's player slides away from any such target (Player2 DefensePushSlideCallback -> AddPushSlide). Both functions run lifted here;
// their calls into the target service go to TPP's real service through the proxy below, whose functions were read in the retail exe (mgsvtpp.exe 1.0.15.4) and are verified when first met.
#include "GekkoPush.h"
#include "GekkoWorld.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr std::uint64_t kCreate = 0x140f80e30ull;       // Survive: three PUSH_SLIDE offense targets per unit (the plugin's setup 0x140f7fbe0 calls it)
    constexpr std::uint64_t kUpdate = 0x140f82280ull;       // Survive: per unit, bones 5 / 0x23 / 0x10 -> target position and radius (radius 0.75 / 0.65 / 0.65), controller slot 42
    constexpr std::uint64_t kNewRecords = 0x140c10a60ull;   // Survive NewObjectArray of the 0x70-byte target record (allocator, count, alignment); its constructor sets the weight word +0x5e to 100
    // TPP retail target service (mgsvtpp.exe 1.0.15.4): pool vtable, slot 1 = new target object (ctor 0x141b97470 stores vtable 0x1424e5190 at alloc+0x18, the returned pointer),
    // slot 6 = new registration handle (ctor 0x141bb19a0, vtable 0x1424ead30 at alloc+0x10), slot 8 = re-register after a change, slots 5 / 7 = release object / handle.
    constexpr std::uint64_t kPoolVtbl = 0x1424f39f0ull, kPoolNew = 0x141bf21c0ull, kPoolHandle = 0x141bf2220ull, kPoolUpdate = 0x141bf2470ull, kPoolFreeObj = 0x141bf2250ull, kPoolFreeHandle = 0x141bf2290ull;
    constexpr std::uint64_t kObjVtbl = 0x1424e5190ull, kObjSetUp = 0x141b98910ull, kObjData = 0x141b97fb0ull;   // slot 5 (+0x28, category / group / flags), slot 11 (+0x58, geometry data: +0x10 position, +0x20 radius)
    constexpr std::uint64_t kHandleVtbl = 0x1424ead30ull, kHandleAdd = 0x141bb1d10ull;                          // slot 0: add the object to the target world
    constexpr int kSets = 3;

    GekkoPush::Hooks g_hooks;
    bool g_active = false;
    bool g_planFailed = false;   // the plugin frame function faulted once: it is not called again
    bool g_verify = true;   // off only in the headless test, whose mock pool does not carry the retail addresses
    void* g_realPool = nullptr;
    unsigned g_id = 0;

    alignas(16) std::uint8_t P[0x260], D[0x100], unitData[0x60 * 2], a8Blk[0x80], bBlk[0x80], eBlk[0x80], eArr[0x1c * 8], chunk[0x800], alloc[0x40], tppRec[kSets][0x30];
    alignas(16) std::uint16_t idxArr[8];
    alignas(16) float defaults[16];
    void* g_objs[kSets][2];
    void* g_handles[kSets][2];
    std::uint8_t* g_recs[kSets];
    void* g_proxyVtbl[16];
    void* g_proxyObj = g_proxyVtbl;   // an object is a pointer to its vtable

    void Trace(const char* fmt, ...)
    {
        if (!g_hooks.trace) return;
        char buf[320];
        va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        g_hooks.trace(buf);
    }
    template <class T> void put(std::uint8_t* base, int off, T v) { *reinterpret_cast<T*>(base + off) = v; }
    void** Vtbl(void* obj) { return *reinterpret_cast<void***>(obj); }
    std::uint64_t Slot(void* obj, int i) { return reinterpret_cast<std::uint64_t>(Vtbl(obj)[i]); }

    // Real calls of the lifted code: every function it reaches on a real object is checked against the retail addresses above before it is allowed through the native gate.
    std::uint64_t Px_New(void*, std::uint64_t, std::uint64_t, std::uint64_t)
    {
        void* obj = reinterpret_cast<void* (__fastcall*)(void*)>(Slot(g_realPool, 1))(g_realPool);
        if (!obj) return 0;
        if (g_verify && (Vtbl(obj) != reinterpret_cast<void**>(kObjVtbl) || Slot(obj, 5) != kObjSetUp || Slot(obj, 11) != kObjData))
        {
            Trace("[push] target object vtable %p slot5 %llx slot11 %llx: not the retail TargetObject, refused", static_cast<void*>(Vtbl(obj)), static_cast<unsigned long long>(Slot(obj, 5)), static_cast<unsigned long long>(Slot(obj, 11)));
            reinterpret_cast<void (__fastcall*)(void*, void*)>(Slot(g_realPool, 5))(g_realPool, obj);
            return 0;
        }
        GekkoWorld::AllowNative(kObjSetUp);
        GekkoWorld::AllowNative(kObjData);
        return reinterpret_cast<std::uint64_t>(obj);
    }
    std::uint64_t Px_Handle(void*, std::uint64_t, std::uint64_t, std::uint64_t)
    {
        void* h = reinterpret_cast<void* (__fastcall*)(void*)>(Slot(g_realPool, 6))(g_realPool);
        if (!h) return 0;
        if (g_verify && (Vtbl(h) != reinterpret_cast<void**>(kHandleVtbl) || Slot(h, 0) != kHandleAdd))
        {
            Trace("[push] registration handle vtable %p slot0 %llx: not the retail handle, refused", static_cast<void*>(Vtbl(h)), static_cast<unsigned long long>(Slot(h, 0)));
            reinterpret_cast<void (__fastcall*)(void*, void*)>(Slot(g_realPool, 7))(g_realPool, h);
            return 0;
        }
        GekkoWorld::AllowNative(kHandleAdd);
        return reinterpret_cast<std::uint64_t>(h);
    }
    std::uint64_t Px_Update(void*, std::uint64_t handle, std::uint64_t, std::uint64_t)
    {
        reinterpret_cast<void (__fastcall*)(void*, void*)>(Slot(g_realPool, 8))(g_realPool, reinterpret_cast<void*>(handle));
        return 0;
    }
    bool PoolOk(void* pool)
    {
        __try
        {
            return Vtbl(pool) == reinterpret_cast<void**>(kPoolVtbl) && Slot(pool, 1) == kPoolNew && Slot(pool, 6) == kPoolHandle && Slot(pool, 8) == kPoolUpdate
                && Slot(pool, 5) == kPoolFreeObj && Slot(pool, 7) == kPoolFreeHandle;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // TPP's player callback reads the pusher's record at (user data - 0x20): +0x26 the pusher's id word, +0x2c a weight byte (x 0.005). Survive's callback reads its own 0x70-byte record instead: +0x54 id
    // dword and +0x5e weight word (x 0.01; default 100 from the record constructor), so the TPP-layout record is filled from Survive's: weight byte = word * 2 (1.0 -> 200). Survive's own record stays untouched.
    void Adapt()
    {
        const std::uint16_t id = static_cast<std::uint16_t>(g_id);
        for (int k = 0; k < kSets; ++k)
        {
            void* obj = g_objs[k][0];
            if (!obj) continue;
            const unsigned w = *reinterpret_cast<std::uint16_t*>(g_recs[k] + 0x5e) * 2u;
            put<std::uint16_t>(tppRec[k], 0x26, id);
            put<std::uint16_t>(tppRec[k], 0x28, 0x34);
            put<std::uint8_t>(tppRec[k], 0x2c, static_cast<std::uint8_t>(w > 255u ? 255u : w));
            put<void*>(reinterpret_cast<std::uint8_t*>(obj), 0x28, tppRec[k] + 0x20);
            put<std::uint64_t>(reinterpret_cast<std::uint8_t*>(obj), 0x50, 0x8800000000000000ull | id);
        }
    }
}

namespace GekkoPush
{
    bool Active() { return g_active; }
    void TestMode() { g_verify = false; }

    bool Start(const Hooks& hooks, void* realPool, unsigned gameObjectId)
    {
        if (g_active) return true;
        g_hooks = hooks;
        g_id = gameObjectId;
        g_realPool = realPool;
        std::memset(P, 0, sizeof(P)); std::memset(D, 0, sizeof(D)); std::memset(unitData, 0, sizeof(unitData)); std::memset(a8Blk, 0, sizeof(a8Blk));
        std::memset(bBlk, 0, sizeof(bBlk)); std::memset(eBlk, 0, sizeof(eBlk)); std::memset(eArr, 0, sizeof(eArr)); std::memset(chunk, 0, sizeof(chunk));
        std::memset(alloc, 0, sizeof(alloc)); std::memset(tppRec, 0, sizeof(tppRec)); std::memset(idxArr, 0, sizeof(idxArr)); std::memset(g_objs, 0, sizeof(g_objs)); std::memset(g_handles, 0, sizeof(g_handles));
        void* observer = GekkoWorld::ObserverIface();
        if (!observer) { Trace("[push] no observer interface"); return false; }
        if (realPool && g_verify && !PoolOk(realPool)) { Trace("[push] pool %p is not the retail target service pool (vtable %p), push targets not created", realPool, static_cast<void*>(*reinterpret_cast<void***>(realPool))); return false; }

        // The fields the two functions read (0x140f80e30, 0x140f82280): +0x58 the unit group descriptor (+0x40 controller, +0x64 unit count), +0x6c first unit id, +0x50 per-unit data (0x60 bytes), +0xa8 -> +0x48 -> +0x28 a
        // word table unit -> index, +0x218 -> +8 a table of 0x1c-byte entries whose byte +0x18 = 1 disables the unit, +0x140 the observer, +0x1f0 the target service pool.
        put<void*>(D, 0x40, GekkoWorld::ControllerObject());
        put<std::uint32_t>(D, 0x64, 1);
        put<void*>(P, 0x58, D);
        put<std::uint32_t>(P, 0x6c, 0);
        put<void*>(P, 0x50, unitData);
        put<void*>(unitData, 0x20, GekkoWorld::ControllerObject());   // the unit's NPC object: the base class per-unit code calls its slot 0x3d8 (the controller's motion output, at 0x140e7e4ff)
        put<void*>(P, 0xa8, GekkoWorld::NpcService());   // [[svc+0x48]+0x28] is the unit index word table, [svc+0xf8] the type table, [svc+0x1fc] the flag word
        put<void*>(P, 0x180, GekkoWorld::NpcEntryTable());
        put<void*>(eBlk, 8, eArr);
        put<void*>(P, 0x218, eBlk);
        put<void*>(P, 0x140, observer);
        put<void*>(P, 0x220, GekkoWorld::SiblingComponent());
        if (realPool)
        {
            g_proxyVtbl[1] = reinterpret_cast<void*>(&Px_New);
            g_proxyVtbl[6] = reinterpret_cast<void*>(&Px_Handle);
            g_proxyVtbl[8] = reinterpret_cast<void*>(&Px_Update);
            put<void*>(P, 0x1f0, &g_proxyObj);
        }
        else put<void*>(P, 0x1f0, GekkoWorld::LiftedPool());

        // The three record arrays come from Survive's own allocation function, so the record defaults (id -1, weight 100) are Survive's. Allocator: +0 buffer, +0x18 cursor, +0x22 bit 0 = second pass.
        put<void*>(alloc, 0, chunk);
        put<std::uint16_t>(alloc, 0x22, 1);
        const int recOff[kSets] = { 0x1a0, 0x1b8, 0x1d0 }, objOff[kSets] = { 0x1a8, 0x1c0, 0x1d8 }, hOff[kSets] = { 0x1b0, 0x1c8, 0x1e0 };
        for (int k = 0; k < kSets; ++k)
        {
            std::uint64_t rax = 0;
            if (!GekkoWorld::CallAddr(kNewRecords, reinterpret_cast<std::uint64_t>(alloc), 1, 0x10, 0, &rax) || !rax) { Trace("[push] record array %d: %s", k, GekkoWorld::LastFault()); return false; }
            g_recs[k] = reinterpret_cast<std::uint8_t*>(rax);
            put<void*>(P, recOff[k], g_recs[k]);
            put<void*>(P, objOff[k], g_objs[k]);
            put<void*>(P, hOff[k], g_handles[k]);
        }
        // The four target-radius countdown tables the frame function decays (P+0x148..0x160): the generic component 0x25c4f9793649, built the way Setup 0x140f7fbe0 gets them from the factory:
        // creator 0x1410700d0 (object, interface vtable 0x1422d1530 at +0x20), then its Setup slot 1 (0x141070200) with {unit count, first unit} sizes the arrays. at 0x1410700d0, 0x141070200, 0x1410704b0.
        for (int k = 0; k < 4; ++k)
        {
            std::uint64_t obj = 0, r = 0;
            std::uint32_t args[2] = { 1, 0 };   // [P+0x70] count, [P+0x6c] first unit
            if (!GekkoWorld::CallAddr( 0x1410700d0ull, 0, 0, 0, 0, &obj) || !obj) { Trace("[push] quark %d create: %s", k, GekkoWorld::LastFault()); return false; }
            if (!GekkoWorld::CallAddr(0x141070200ull, obj, reinterpret_cast<std::uint64_t>(args), 0, 0, &r)) { Trace("[push] quark %d setup: %s", k, GekkoWorld::LastFault()); return false; }
            put<std::uint64_t>(P, 0x148 + 8 * k, obj + 0x20);
        }
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(kCreate, reinterpret_cast<std::uint64_t>(P), 0, 0, 0, &rax)) { Trace("[push] Survive creation faulted: %s", GekkoWorld::LastFault()); return false; }
        for (int k = 0; k < kSets; ++k)
            if (!g_objs[k][0] || !g_handles[k][0]) { Trace("[push] set %d: object %p handle %p, creation incomplete", k, g_objs[k][0], g_handles[k][0]); return false; }
        if (realPool) Adapt();
        g_active = true;
        Trace("[push] %d PUSH_SLIDE targets created (%s), objects %p %p %p, record weight words %u %u %u, owner id %u", kSets, realPool ? "TPP target service" : "lifted pool", g_objs[0][0], g_objs[1][0], g_objs[2][0],
            *reinterpret_cast<std::uint16_t*>(g_recs[0] + 0x5e), *reinterpret_cast<std::uint16_t*>(g_recs[1] + 0x5e), *reinterpret_cast<std::uint16_t*>(g_recs[2] + 0x5e), g_id);
        return true;
    }

    void Update(const float pos[3])
    {
        if (!g_active) return;
        // [r8+0x10] and [r8+0x20]: the positions the updater keeps when the bone query fails
        for (int off = 4; off <= 8; off += 4) { defaults[off] = pos[0]; defaults[off + 1] = pos[1]; defaults[off + 2] = pos[2]; defaults[off + 3] = 1.0f; }
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(kUpdate, reinterpret_cast<std::uint64_t>(P), 0, reinterpret_cast<std::uint64_t>(defaults), 0, &rax))
        {
            Trace("[push] Survive update faulted, push targets stopped: %s", GekkoWorld::LastFault());
            g_active = false;
            return;
        }
        if (g_realPool) Adapt();
        static int logged = 0;
        if (g_realPool && logged < 3 && ++logged)
        {
            for (int k = 0; k < kSets; ++k)
            {
                std::uint8_t* data = reinterpret_cast<std::uint8_t*>(reinterpret_cast<void* (__fastcall*)(void*)>(Slot(g_objs[k][0], 11))(g_objs[k][0]));
                Trace("[push] set %d data pos %.2f %.2f %.2f radius %.2f (gekko %.2f %.2f %.2f)", k, *reinterpret_cast<float*>(data + 0x10), *reinterpret_cast<float*>(data + 0x14), *reinterpret_cast<float*>(data + 0x18),
                    *reinterpret_cast<float*>(data + 0x20), pos[0], pos[1], pos[2]);
            }
        }
    }

    // The plugin's own per-frame function 0x140f78a90 (slot 13): this = P + 0x20, edx = execution kind (0 serial, 3, 5 late), r8d = unit. Kind 0 clears the NPC state bits and sets the target radii
    // (the early-out block 0x140f7ee21), kind 5 is the main update (events -> footstep effects, bone transforms, push spheres).
    bool RunKind(int kind)
    {
        if (!g_active || g_planFailed) return false;
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(0x140f78a90ull, reinterpret_cast<std::uint64_t>(P + 0x20), static_cast<std::uint64_t>(kind), 0, 0, &rax))
        {
            Trace("[push] plugin frame kind %d faulted, plugin frame stopped: %s", kind, GekkoWorld::LastFault());
            g_planFailed = true;
            return false;
        }
        return true;
    }

    void Stop()
    {
        if (!g_active && !g_objs[0][0]) return;
        g_active = false;
        if (g_realPool)
        {
            __try
            {
                for (int k = 0; k < kSets; ++k)
                {
                    if (g_objs[k][0]) reinterpret_cast<void (__fastcall*)(void*, void*)>(Slot(g_realPool, 5))(g_realPool, g_objs[k][0]);
                    if (g_handles[k][0]) reinterpret_cast<void (__fastcall*)(void*, void*)>(Slot(g_realPool, 7))(g_realPool, g_handles[k][0]);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[push] release faulted"); }
        }
        std::memset(g_objs, 0, sizeof(g_objs)); std::memset(g_handles, 0, sizeof(g_handles));
        Trace("[push] stopped");
    }
}
