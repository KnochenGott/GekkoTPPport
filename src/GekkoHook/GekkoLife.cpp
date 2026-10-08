// GekkoLife.cpp 06/10/2026. The gekko's life and damage components over the host NPC service (docs/DOCUMENTATION.md 4.6), and the creation table the host context also uses for the ActionController (07/10/2026).
#include "GekkoLife.h"
#include "GekkoParam.h"
#include "GekkoWorld.h"
#include "lift_rt.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // Creators (descriptor slot 1 of each class, read from mgv.exe): hash -> function that allocates and constructs the component. Setup is slot 1 of the new object's own vtable.
    struct Creator { std::uint64_t hash, fn; const char* name; };
    constexpr Creator kCreators[] = {
        { 0xd900643075cdull, 0x141040790ull, "life class" },
        { 0x2fbbd3df1420ull, 0x140f91970ull, "LifeController" },
        { 0x6a0681cf442full, 0x141087040ull, "generic DamageHandler" },
        { 0xef4194cb7e4cull, 0x140f905d0ull, "typed DamageHandler" },
        { 0x8747abb9b9d8ull, 0x140e786d0ull, "ActionController" },
        { 0xef6ac86358edull, 0x140f768c0ull, "gekko ActionController component" },
        { 0xa62b87fc4a6aull, 0x140f82ab0ull, "gekko action component 0xa62b87fc4a6a" },
    };
    constexpr std::uint64_t kCalculatorCreator = 0x140f51ef0ull;
    // TPP damage point -> Survive element amount of the TPP row (decided 06/10/2026: 1 point = 1 point).
    constexpr float kTppDamageScale = 1.0f;
    constexpr std::uint32_t kTppRowId = 0x7b0b0b01;   // not one of the row ids the gekko code or the calculator tests (0x68c4f7e9, 0x883c3b14, 0x18922635 ...)
    constexpr int kBulletElement = 3;                 // damage row element order: slash, strike, thrust, bullet, flame, ice, shock, blast
    constexpr std::uint32_t kRowIsLong = 0x4000;      // row flag bit 14 "ranged": the typed handler adds bullet_defense (0x140f8f4ee)
    constexpr std::uint16_t kTppImpact = 1200;        // D+0x2a: the most common impact among the 89 bullet rows of SsdDamageParameter_opt.json (31 rows; the rest 0, 2000, 2500, 5000, 10000); the calculator scales the hit by impact / defense (0x140f50e49)
    constexpr std::uint8_t kSpreadNeutral = 5;        // H+0x62: the calculator scales a hit without an attacker by (95 + this) percent (0x140f51224)

    template <class T> void put(std::uint8_t* base, int off, T v) { *reinterpret_cast<T*>(base + off) = v; }
    template <class T> T get(const std::uint8_t* base, int off) { return *reinterpret_cast<const T*>(base + off); }

    alignas(16) std::uint8_t g_param[0x1b0];          // SsdGeckoParameter image (GekkoParam::Fill)
    alignas(16) std::uint8_t g_holder[0x40];          // svc+0x120: [+0x20] = parameter struct + 8 (the Observer reads it at 0x140f740ff..0x140f74117)
    alignas(16) std::uint8_t g_hub[0x800];            // [ctx+0x98]: the hub the components read
    alignas(16) std::uint8_t g_dmgTable[0x80];        // [hub+0xa8]: damage parameter table (rows at +8, row count at +0x30)
    alignas(16) std::uint8_t g_dmgRows[2][0x48];      // row 0 is the empty default row, row 1 the TPP hit row
    alignas(16) std::uint8_t g_levelRate[0x40], g_durabRate[0x40];
    alignas(16) std::uint8_t g_hub0xb0[0x100], g_hub0xb0b[0x100];   // [hub+0xb0] -> {+0x58 -> {+0x8c u16}}: read by the damage handler slot +0x88 (0x141087570), zero
    alignas(16) std::uint8_t g_attackers[0x20];       // [hub+0x230]: attacker registry, slot +0x420 answers a pointer to the local player's attacker handle (0x141086fa0)
    void* g_attackersVtbl[256];
    std::uint32_t g_localAttacker = 0;                // the TPP player is the attacker of every TPP hit: handle 0, no attacker index in bits 16 to 22
    alignas(16) std::uint8_t g_msgHub[0x20];          // [ctx+0x60]: message hub, slot 8 = send (the Dead event)
    void* g_msgVtbl[64];
    void* g_zeroHandlePtr = nullptr;                  // svc slot +0x30 answers a pointer to a null handle
    std::uint8_t* g_svc = nullptr;
    std::uint8_t* g_lifeIface = nullptr;              // [svc+0x68] after the life class slot ran
    std::uint8_t* g_lcIface = nullptr;                // LifeController interface (obj+0x20)
    bool g_dead = false;
    unsigned g_deadEvents = 0;
    std::uint32_t g_hits = 0;

    struct Entry { void* pad[2]; void* obj; void* more[5]; };   // creation handle: +0x10 is the component the interface lookups ask
    void* g_lastHandle[16];

    void Log(const char* fmt, ...)
    {
        char b[256];
        va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
        GekkoWorld::Log(b);
    }

    // Message hub slot 8 (called from 0x141041552 with rdx = out, r8d = event hash, r9 = record).
    std::uint64_t MsgSend(void*, std::uint64_t, std::uint64_t hash, std::uint64_t rec)
    {
        const std::uint32_t h = static_cast<std::uint32_t>(hash);
        Log("[life] message %08x record %llx", h, rec ? static_cast<unsigned long long>(*reinterpret_cast<std::uint64_t*>(rec)) : 0ull);
        if (h == 0xcfc364d6u) { g_dead = true; ++g_deadEvents; }
        return 0;
    }

    // [ctx+8]: the frame scratch allocator. Slot +0x30 opens a scope in the caller's memory (its first qword is the scope's vtable), scope slot +0x10 (bytes, alignment) hands out zeroed memory,
    // scope slot +0x38 closes it (typed DamageHandler slot 0, 0x140f8edeb..0x140f8eee1). A ring of 64 KB is far more than a hit needs.
    alignas(16) std::uint8_t g_tempMgr[0x20], g_tempPool[1 << 16];
    void* g_tempMgrVtbl[64]; void* g_tempScopeVtbl[64];
    std::size_t g_tempOff = 0;
    std::uint64_t TempScopeOpen(void*, std::uint64_t scope, std::uint64_t, std::uint64_t) { *reinterpret_cast<void***>(scope) = g_tempScopeVtbl; return scope; }
    std::uint64_t TempAlloc(void*, std::uint64_t bytes, std::uint64_t align, std::uint64_t)
    {
        if (align < 8) align = 8;
        std::size_t o = (g_tempOff + align - 1) & ~(align - 1);
        if (o + bytes > sizeof(g_tempPool)) o = 0;
        g_tempOff = o + bytes;
        std::memset(g_tempPool + o, 0, bytes);
        return reinterpret_cast<std::uint64_t>(g_tempPool + o);
    }
    std::uint64_t TempScopeClose(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }

    std::uint64_t LocalAttacker(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(&g_localAttacker); }
    // svc slot +8: pointer to the unit's 64-bit identity (the life class copies it into the state-change record, 0x1410412d0..0x1410412da). The host unit has none: zero.
    std::uint64_t g_unitIdentity = 0;
    std::uint64_t SvcIdentitySlot(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(&g_unitIdentity); }
    // [svc+0x190] is the unit registry (Survive's [[hub+0x230]], the same object as g_attackers below). Slots +0x418 and +0x420 (out) answer a pointer to the unit's record; the life and damage code read
    // a word or dword from it (0x1410406b0, 0x140f8f18c, 0x141087e6a). The host unit has no attack record: a zeroed block.
    alignas(16) std::uint8_t g_unitRecord[0x40];
    alignas(16) std::uint8_t g_unitStore[0x20];
    void* g_unitStoreVtbl[256];
    std::uint64_t UnitRecord(void*, std::uint64_t out, std::uint64_t, std::uint64_t)
    {
        if (out) std::memset(reinterpret_cast<void*>(out), 0, 4);
        return reinterpret_cast<std::uint64_t>(g_unitRecord);
    }
    // Slots +0x2b8, +0x2c8, +0x2d8 (out, unit id) -> pointer to a 16-byte aligned position record (x, y, z, 0) of that unit, as the target resolver 0x140f7f240 reads them for a unit target (selector 0,
    // word +4 = 0, 1, 2) and 0x140f93982 for the sight line. Unit id 0 is the target the brain was given (the target provider answers id 0 while there is one): the TPP player or the Lua point
    // (GekkoWorld::SetTarget). What the three kinds differ by (feet, eye, centre) is not decoded: all three answer the target position.
    alignas(16) float g_unitPos[3][4];
    unsigned g_unitPosCalls[3] = {};
    template <int K> std::uint64_t UnitPosition(void*, std::uint64_t, std::uint64_t id, std::uint64_t)
    {
        const float* t = reinterpret_cast<const float*>(GekkoWorld::TargetObj() + 0x20);   // g_targetObj[8..10]
        ++g_unitPosCalls[K];
        g_unitPos[K][0] = t[0]; g_unitPos[K][1] = t[1]; g_unitPos[K][2] = t[2]; g_unitPos[K][3] = 0.0f;
        static bool said = false;
        if (id != 0 && !said) { said = true; Log("[life] unit position asked for unit %llu: answered with the target", static_cast<unsigned long long>(id)); }
        return reinterpret_cast<std::uint64_t>(g_unitPos[K]);
    }
    // Slot +0x318 (unit id, 0x89, 0x8b, 0) -> bool: 0x140f948f0 asks it before resolving a unit target; true skips the resolver and takes the default point. Answers false: the unit is not in the states
    // 0x89 to 0x8b (read as a range of unit states, not decoded).
    std::uint64_t UnitStateQuery(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return 0; }
    std::uint64_t SvcHandleSlot(void*, std::uint64_t, std::uint64_t, std::uint64_t) { return reinterpret_cast<std::uint64_t>(&g_zeroHandlePtr); }
    std::uint64_t SvcLifeSlot(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c)
    {
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(0x141073230ull, reinterpret_cast<std::uint64_t>(self), a, b, c, &rax)) Log("[life] svc life slot: %s", GekkoWorld::LastFault());
        g_lifeIface = get<std::uint8_t*>(static_cast<std::uint8_t*>(self), 0x68);
        return rax;
    }
    std::uint64_t SvcDamageSlot(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c)
    {
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddrX(0x141073010ull, reinterpret_cast<std::uint64_t>(self), a, b, c, 0.0f, 0, &rax)) Log("[life] svc damage slot: %s", GekkoWorld::LastFault());
        return rax;
    }

    bool CreateCalculator()
    {
        std::uint64_t obj = 0;
        if (!GekkoWorld::CallAddr(kCalculatorCreator, 0, 0, 0, 0, &obj) || !obj) { Log("[life] calculator creator: %s", GekkoWorld::LastFault()); return false; }
        Log("[life] damage calculator %llx, hub+0x30 = %llx", static_cast<unsigned long long>(obj), static_cast<unsigned long long>(get<std::uint64_t>(g_hub, 0x30)));
        return true;
    }
}

namespace GekkoLife
{
    bool Owns(std::uint64_t hash)
    {
        for (const Creator& c : kCreators) if (c.hash == hash) return true;
        return false;
    }

    std::uint64_t Create(std::uint64_t outBuf, std::uint64_t hash, std::uint64_t desc)
    {
        const Creator* c = nullptr;
        for (const Creator& k : kCreators) if (k.hash == hash) c = &k;
        if (!c) return 0;
        std::uint64_t obj = 0;
        if (!GekkoWorld::CallAddr(c->fn, 0, 0, 0, 0, &obj) || !obj) { Log("[life] creator %s: %s", c->name, GekkoWorld::LastFault()); return 0; }
        const std::uint64_t vt = get<std::uint64_t>(reinterpret_cast<const std::uint8_t*>(obj), 0);
        const std::uint64_t setupTok = get<std::uint64_t>(reinterpret_cast<const std::uint8_t*>(vt), 8);
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(setupTok - lift::kTokenBase, obj, desc, 0, 0, &rax)) { Log("[life] Setup of %s: %s", c->name, GekkoWorld::LastFault()); return 0; }
        static int n = 0;
        Entry* e = new Entry{};
        e->obj = reinterpret_cast<void*>(obj);
        g_lastHandle[n & 15] = e;
        void** handle = &g_lastHandle[n & 15];
        ++n;
        if (outBuf) put<void*>(reinterpret_cast<std::uint8_t*>(outBuf), 0, e);   // one qword, as Survive's creation function writes it
        Log("[life] created %s at %llx", c->name, static_cast<unsigned long long>(obj));
        if (hash == 0x2fbbd3df1420ull) g_lcIface = reinterpret_cast<std::uint8_t*>(obj) + 0x20;
        return reinterpret_cast<std::uint64_t>(handle);
    }

    void InstallSvcSlots(void** vtbl)
    {
        vtbl[1] = reinterpret_cast<void*>(&SvcIdentitySlot);
        vtbl[6] = reinterpret_cast<void*>(&SvcHandleSlot);
        vtbl[129] = reinterpret_cast<void*>(&SvcLifeSlot);
        vtbl[130] = reinterpret_cast<void*>(&SvcDamageSlot);
    }

    std::uint8_t* ControllerIface() { return g_lcIface; }

    void* ParamHolder() { return g_holder; }
    void* UnitStore() { return g_unitStore; }
    void AdoptService(std::uint8_t* svc) { g_svc = svc; g_lifeIface = get<std::uint8_t*>(svc, 0x68); }

    void Prepare(std::uint8_t* ctxSlot, std::uint8_t* svc, void* ownerEntry)
    {
        g_svc = svc;
        GekkoParam::Fill(g_param);
        put<void*>(g_holder, 0x20, g_param + 8);
        put<void*>(svc, 0x120, g_holder);
        std::memcpy(g_unitStoreVtbl, *reinterpret_cast<void**>(GekkoWorld::TrapObject("unitstore", 0)), sizeof(g_unitStoreVtbl));
        g_unitStoreVtbl[131] = g_unitStoreVtbl[132] = reinterpret_cast<void*>(&UnitRecord);
        g_unitStoreVtbl[87] = reinterpret_cast<void*>(&UnitPosition<0>);
        g_unitStoreVtbl[89] = reinterpret_cast<void*>(&UnitPosition<1>);
        g_unitStoreVtbl[91] = reinterpret_cast<void*>(&UnitPosition<2>);
        g_unitStoreVtbl[99] = reinterpret_cast<void*>(&UnitStateQuery);
        GekkoWorld::NameTrap(g_unitStore, "unit registry");
        put<void*>(g_unitStore, 0, g_unitStoreVtbl);
        put<void*>(svc, 0x190, g_unitStore);
        put<void*>(svc, -0x10, ownerEntry);
        put<std::uint32_t>(svc, 0x1fc, 2);   // flag word: bit 1 set = the server side of the life update (damage is applied); bit 2 (replication) clear
        std::uint8_t* o48 = get<std::uint8_t*>(svc, 0x48);
        put<std::uint32_t>(o48, 0x40, 1);    // unit capacity read by the life class, damage handler and Observer setup
        put<std::uint32_t>(o48, 0x44, 1);    // hit-record slots
        static std::uint8_t aliveBits[8] = { 1 };
        put<void*>(o48, 0, aliveBits);       // the life update's unit bitmap

        // hub: every pointer slot is a logging trap object until a component asks for something specific.
        for (int off = 0x10; off < 0x600; off += 8)
            if (!get<void*>(g_hub, off)) put<void*>(g_hub, off, GekkoWorld::TrapObject("hub", 1));
        // hub vtable slot 2 (+0x10, id -> object lookup, used by the damage handler at 0x140eafd88 once the service says the damage is applied here): no actor object exists for the id, so null, which the caller skips.
        { static void* hubVtbl[256]; std::memcpy(hubVtbl, *reinterpret_cast<void**>(GekkoWorld::TrapObject("hubvt", 0)), sizeof(hubVtbl)); hubVtbl[2] = reinterpret_cast<void*>(+[](void*, std::uint64_t, std::uint64_t, std::uint64_t) -> std::uint64_t { return 0; }); put<void*>(g_hub, 0, hubVtbl); }
        put<void*>(g_dmgTable, 8, g_dmgRows);
        put<std::uint32_t>(g_dmgTable, 0x30, 2);
        put<void*>(g_dmgTable, 0x10, g_durabRate);
        put<void*>(g_dmgTable, 0x18, g_levelRate);
        put<void*>(g_hub, 0xa8, g_dmgTable);
        put<void*>(g_hub0xb0, 0x58, g_hub0xb0b);
        put<void*>(g_hub, 0xb0, g_hub0xb0);
        std::memcpy(g_attackersVtbl, *reinterpret_cast<void**>(GekkoWorld::TrapObject("attackers", 0)), sizeof(g_attackersVtbl));
        g_attackersVtbl[132] = reinterpret_cast<void*>(&LocalAttacker);
        put<void*>(g_attackers, 0, g_attackersVtbl);
        put<void*>(g_hub, 0x230, g_attackers);
        std::memcpy(g_tempMgrVtbl, *reinterpret_cast<void**>(GekkoWorld::TrapObject("tempmgr", 0)), sizeof(g_tempMgrVtbl));
        g_tempMgrVtbl[6] = reinterpret_cast<void*>(&TempScopeOpen);
        put<void*>(g_tempMgr, 0, g_tempMgrVtbl);
        std::memcpy(g_tempScopeVtbl, *reinterpret_cast<void**>(GekkoWorld::TrapObject("tempscope", 0)), sizeof(g_tempScopeVtbl));
        g_tempScopeVtbl[2] = reinterpret_cast<void*>(&TempAlloc);
        g_tempScopeVtbl[7] = reinterpret_cast<void*>(&TempScopeClose);
        put<void*>(ctxSlot, 8, g_tempMgr);
        std::uint8_t* old = get<std::uint8_t*>(ctxSlot, 0x98);
        if (old) { put<void*>(g_hub, 8, get<void*>(old, 8)); put<void*>(g_hub, 0x110, get<void*>(old, 0x110)); }
        put<void*>(ctxSlot, 0x98, g_hub);
        void* trapVt = *reinterpret_cast<void**>(GekkoWorld::TrapObject("msghub", 0));
        std::memcpy(g_msgVtbl, trapVt, sizeof(g_msgVtbl));
        g_msgVtbl[8] = reinterpret_cast<void*>(&MsgSend);
        put<void*>(g_msgHub, 0, g_msgVtbl);
        put<void*>(ctxSlot, 0x60, g_msgHub);
        for (int off : { 0x78, 0x88, 0x90 }) if (!get<void*>(ctxSlot, off)) put<void*>(ctxSlot, off, GekkoWorld::TrapObject("ctx", 0));
        CreateCalculator();
    }

    // The hit spheres of GeckoTargetDefense.tgt in the order of GekkoFull::TargetObjects: StrCode64 (low 48 bits) of each name, the key of Survive's region mapper 0x140f7f030.
    constexpr std::uint64_t kPartHash[kParts] = {
        0x3fc9b09a6411ull, 0x2e4794a10189ull, 0x3b2caf1fd140ull, 0x5a9fd5539acaull, 0x5cf3cc289c0cull, 0x2d82e3562e36ull, 0x81ae1cac5a5bull, 0x1ebf99f5f070ull,
        0x0244677828acull, 0xaaae433f6881ull, 0x31c73925e205ull, 0x9a3003c54d32ull, 0xfd6330009f85ull, 0xed1d43a5097cull, 0x7b28318d0176ull, 0xab0a142db251ull,
        0x48902daa4f8eull, 0xa2297bef239eull, 0x7991d9841fe1ull, 0xba8c5d6f3c70ull, 0xb62e6bca4403ull };   // Waist Head Head_Nose Waist_B Leg_L Leg_R Thigh_L Thigh_R Knee_L Knee_R Shin_L Shin_R Camera_Top Weak_Head Head_R Head_L Head_B GunR LocketL GasL GasR

    int RegionOfPart(int part)
    {
        if (part < 0 || part >= kParts) return 3;
        alignas(8) static std::uint32_t out[2];
        out[0] = 3; out[1] = 0;   // the receiver's default region for an unknown name (0x140f77880)
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(0x140f7f030ull, kPartHash[part], reinterpret_cast<std::uint64_t>(&out[0]), reinterpret_cast<std::uint64_t>(&out[1]), 0, &rax)) return 3;
        return static_cast<int>(out[0]);
    }

    // Region -> LifeController pool (0x140f8f230 jump on the region byte): 5 -> 0, 4 -> 1, 6 -> 2, 7 -> 3, 2 -> 4, others none.
    int PoolOfRegion(int region)
    {
        switch (region) { case 5: return 0; case 4: return 1; case 6: return 2; case 7: return 3; case 2: return 4; default: return -1; }
    }

    bool Ready() { return g_lifeIface != nullptr && g_lcIface != nullptr; }

    bool Read(State& s)
    {
        if (!Ready()) return false;
        const std::uint8_t* u = get<std::uint8_t*>(g_lifeIface, 8);   // unit records, 0x1c bytes
        s.life = get<std::uint32_t>(u, 0); s.maxLife = get<std::uint32_t>(u, 8); s.pending = get<std::uint32_t>(u, 0xc); s.dead = get<std::uint8_t>(u, 0x18) != 0;
        // LifeController pools of unit 0: the remaining life of each pool is [iface+0x20][pool], the damage waiting to be applied [iface+0x28][pool] (0x140f8f3ae..0x140f8f3bb: a pool
        // is broken when remaining <= waiting). The start values are the partsLife words of the parameter image.
        const std::uint16_t* left = get<std::uint16_t*>(g_lcIface, 0x20);
        const std::uint16_t* waiting = get<std::uint16_t*>(g_lcIface, 0x28);
        for (int p = 0; p < 5; ++p)
        {
            s.poolLeft[p] = left[p]; s.poolWaiting[p] = waiting[p];
            s.poolStart[p] = get<std::uint16_t>(g_param, 0x128 + 2 * p);
            std::uint64_t rax = 0;
            s.broken[p] = GekkoWorld::CallAddr(0x140f91a90ull, reinterpret_cast<std::uint64_t>(g_lcIface), 0, static_cast<std::uint64_t>(p), 0, &rax) && (rax & 0xff) != 0;
        }
        s.hits = g_hits; s.ready = true;
        return true;
    }

    bool Hit(int part, int damage, unsigned weaponClass)
    {
        if (!g_svc || !g_lifeIface) return false;
        std::uint8_t* x = get<std::uint8_t*>(g_svc, 0xa8);   // generic DamageHandler interface
        if (!x) return false;
        const int amount = static_cast<int>(damage * kTppDamageScale + 0.5f);
        std::uint8_t* row = g_dmgRows[1];
        std::memset(row, 0, 0x48);
        put<std::uint32_t>(row, 0, kTppRowId);
        put<std::uint16_t>(row, 0x18 + 2 * kBulletElement, static_cast<std::uint16_t>(amount > 0xffff ? 0xffff : amount < 0 ? 0 : amount));
        put<std::uint32_t>(row, 0x30, kRowIsLong);
        put<std::uint16_t>(row, 0x2a, kTppImpact);
        alignas(16) std::uint8_t h[0x70] = {};
        put<std::uint8_t>(h, 0x40, 8); put<std::uint8_t>(h, 0x41, 8);   // no element index: the typed handler adds no amount word (0x140f8f56e)
        put<std::uint32_t>(h, 0x4c, (weaponClass & 0x7f) << 16);
        put<std::uint16_t>(h, 0x52, 1);   // damage row 1: the TPP row above
        put<std::uint16_t>(h, 0x58, 100); put<std::uint16_t>(h, 0x5a, 100);
        put<std::uint8_t>(h, 0x5d, 100);
        put<std::uint8_t>(h, 0x60, static_cast<std::uint8_t>(RegionOfPart(part)));
        put<std::uint8_t>(h, 0x62, kSpreadNeutral);
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(0x1410878f0ull, reinterpret_cast<std::uint64_t>(x), 0, reinterpret_cast<std::uint64_t>(h), 1, &rax)) { Log("[life] hit: %s", GekkoWorld::LastFault()); return false; }
        ++g_hits;
        return true;
    }

    bool Update(float)
    {
        if (!g_lifeIface) return false;
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddrX(0x141041a60ull, reinterpret_cast<std::uint64_t>(g_lifeIface), 0, 0, 0, 0.0f, 0, &rax)) { Log("[life] update: %s", GekkoWorld::LastFault()); return false; }
        return true;
    }
    bool Dead() { return g_dead; }
    unsigned DeadEvents() { return g_deadEvents; }
    unsigned UnitPositionCalls(int kind) { return kind >= 0 && kind < 3 ? g_unitPosCalls[kind] : 0; }
}
