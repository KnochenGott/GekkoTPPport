// GekkoType.cpp 29/09/2026. Native TPP game object type "TppGekko": our factory and interface, registered with
// fox::gm::RegisterGameObjectFactory. Layouts and slot meanings: docs/DOCUMENTATION.md.
// The vtables are plain pointer arrays so the slot order is exactly the engine's, independent of MSVC class layout.
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "GekkoAi.h"
#include "GekkoBrain.h"
#include "GekkoDriver.h"
#include "GekkoFull.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"
#include "GekkoType.h"

namespace GekkoType
{
    namespace
    {
        // Free index, engine registers 0..91 (84 types), Lua declares 0..93, capacity 128,
        // 127 would give the NULL id 0xFFFF. GameObjectId = index << 9 | instance.
        constexpr std::uint16_t kTypeIndex = 100;
        // tools/pathcode.py string_id("TppGekko"); same hash as TppCommandPost2 0x8f0921fb6953 (in the map).
        constexpr std::uint64_t kTypeName = 0x8f62f51fb099ull;
        constexpr std::uint64_t kNoId = 0;   // slot 21 out value; the bear and CP2 write 0 (0x140926900)

        using RegisterFactory_t = void (*)(void* factory);
        using CreateGameObject_t = void** (*)(void** out, const void* desc);
        using DestroyGameObject_t = void (*)(void* gameObject);
        using ActivateGameObject_t = void (*)(void* gameObject);

        RegisterFactory_t registerFactory_;
        CreateGameObject_t createGameObject_;
        DestroyGameObject_t destroyGameObject_;
        ActivateGameObject_t activateGameObject_;

        // fox::gm::GameObjectDesc as SetUpGameObject 0x140bf8930 builds it: u16 type, then the data entity's
        // +0x84 qword (u32 totalCount, u32 realizedCount), then +0x90 (data entity pointer).
        #pragma pack(push, 4)
        struct Desc { std::uint16_t type; std::uint16_t pad; std::uint32_t total; std::uint32_t realized; std::uint32_t pad2; void* data; };
        #pragma pack(pop)
        static_assert(sizeof(Desc) == 0x18, "GameObjectDesc is 3 qwords (GameObjectImpl ctor copies 3)");

        // Interface object. Engine-written fields (GameObjectImpl ctor 0x140bf3af0, live on player/CP2/soldier):
        // +0x08 GameObjectImpl*, +0x1c u16 type index, +0x1e u16 flags (Allocate clears bit 1; AllocResources sets it
        // to be called again). Ours start at 0x40.
        struct Object
        {
            void* const* vtbl;
            void* gameObjectImpl;
            std::uint8_t engine[0x08];
            std::uint32_t pad;
            std::uint16_t typeIndex;
            std::uint16_t flags;
            std::uint8_t engine2[0x20];
            // ours
            std::uint32_t allocCalls, total, realized;
            void* data;
            std::uint32_t frames[3];     // ExecuteSerially, ExecuteInParallel, ExecuteLast
            bool active;
            std::uint8_t reserve[0x80];  // slack in case an engine path reads past what the ctor writes (not verified need)
        };
        static_assert(offsetof(Object, typeIndex) == 0x1c && offsetof(Object, flags) == 0x1e && offsetof(Object, allocCalls) == 0x40, "iface layout");

        bool registered_;
        void* handle_;        // what CreateGameObject returned: the engine GameObjectImpl (0x40)
        Object* last_;        // for Status only
        float pose_[4];       // x, y, z, yaw: a fox2 GameObject entity carries no position, Lua places it (Place)
        bool havePose_, allocated_;

        void SpawnAtPose()
        {
            // The driver's spawn: files, then the full AnimationController with Survive's gekko desc (GekkoFull).
            const bool ok = Gekko::Spawn(pose_[0], pose_[1], pose_[2], pose_[3]);
            Log("[type] Spawn at %.2f %.2f %.2f yaw %.3f id %u -> %s %s\n", pose_[0], pose_[1], pose_[2], pose_[3],
                GameObjectId(), ok ? "ok" : "FAILED", ok ? "" : Gekko::GetLastError());
        }

        Object* Self(void* p) { return static_cast<Object*>(p); }

        // ---- interface slots (numbers are the engine's slot index) ----
        void AllocResources(void* self, const Desc* d)   // 0; `d` is GameObjectImpl + 8, a copy of our Desc
        {
            Object* o = Self(self);
            ++o->allocCalls;
            o->total = d->total;
            o->realized = d->realized;
            o->data = d->data;
            Log("[type] AllocResources iface=%p impl=%p desc{type %u total %u realized %u data %p} flags=%04X thread %lu\n",
                self, o->gameObjectImpl, d->type, d->total, d->realized, d->data, o->flags, GetCurrentThreadId());
            // Synchronous: never sets iface flags bit 1, so the engine does not call this again.
            allocated_ = true;
            if (havePose_) SpawnAtPose();
            else Log("[type] allocated from fox2, waiting for Place\n");
        }
        void FreeResources(void* self)                                                                // 1
        {
            Log("[type] FreeResources %p\n", self);
            allocated_ = havePose_ = false;
            GekkoAi::Stop();
            Gekko::Despawn();
        }
        void Noop(void*) {}                                                                           // 2, 3, 12, 15, 22
        void Activate(void* self) { Self(self)->active = true; Log("[type] Activate %p\n", self); }    // 4
        void Deactivate(void* self) { Self(self)->active = false; Log("[type] Deactivate %p\n", self); } // 5
        void Locators(void*, void*) {}                                                                // 6, 7
        // 8: execution-entry info, read by the entry ctor 0x140bf4690 into entry+8 (from disassembly).
        // +0 u16, +4 u32 instance count, +8 u32, +0xc u32; when +4, +8 and +0xc are all non-zero the ctor allocates a
        // (count+7)/8 byte bitmap and sets count bits. Leaving the record untouched froze the game.
        // CP2 0x140a23a80 writes {1, [this+0x654], 0, 0}; bear 0x140a7b9f0 {1, count, instances, 3}.
        // Read: the parallel dispatcher 0x140bf4c50 calls slot 10 `last` times per frame (loop over entry
        // +0x14 = record +0xc), ctx {u32, u32 i, u32, u32 count, bitmap*}. With 0 there it never ran. One
        // parallel call per frame: {1, n, n, 1}; the non-zero +8 makes the ctor build the n-bit instance bitmap.
        struct InfoRecord { std::uint16_t one; std::uint16_t pad; std::uint32_t count, instances, parallelCalls; };
        InfoRecord* Info(void* self, InfoRecord* out)
        {
            const std::uint32_t n = Self(self)->total ? Self(self)->total : 1;
            *out = { 1, 0, n, n, 1 };
            return out;
        }
        template <int N> void Execute(void* self, void*)                                              // 9, 10, 11
        {
            if (Self(self)->frames[N]++ == 0)
                Log("[type] first Execute%s %p thread %lu\n", N == 0 ? "Serially" : N == 1 ? "InParallel" : "Last", self,
                    GetCurrentThreadId());
        }
        // 10: the full controller's frame, from the engine's GameObjectParallelExecutionContext (per-instance run/skip =
        // pause, then the stages in Sahelan's order). As RealizedSahelan2Impl::Execute and Survive FUN_140c56850 do.
        void ExecuteInParallel(void* self, void* ctx)
        {
            Execute<1>(self, ctx);
            if (GekkoFull::Execute(ctx)) GekkoBrain::ControllerRan();
        }
        // 9: the brain. The serial dispatcher 0x140bf5130 passes no context and is also called in pause (60/s
        // during pause), so the AI advances only by the frames the engine let the gekko run (GekkoFull).
        void ExecuteSerially(void* self, void* ctx)
        {
            Execute<0>(self, ctx);
            const float dt = GekkoFull::ConsumeRunTime();
            if (dt > 0.0f && Gekko::GetStep() == Gekko::Step::Placed && GekkoAi::GetState() >= 0)
                GekkoAi::Update(dt);
        }
        std::uint64_t ReturnZero(void*) { return 0; }                                                 // 13, 18
        // 14, 16, 17 are where a hit or a Lua SendCommand can arrive (not verified which). Each logs its arguments for the
        // first calls of every distinct id, so a live hit shows the path and the id; the return values stay neutral.
        void LogIncoming(const char* slot, std::uint64_t id, const void* data)
        {
            static std::uint64_t seen[32];
            static int n = 0;
            for (int i = 0; i < n; ++i)
                if (seen[i] == id) return;
            if (n < 32) seen[n++] = id;
            std::uint64_t head[4] = {};
            // data is not always a pointer (value 1 on the first signal): probe the pages instead of letting the read fault and be swallowed.
            MEMORY_BASIC_INFORMATION mbi{};
            if (data && VirtualQuery(data, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                static_cast<const std::uint8_t*>(data) + sizeof(head) <= static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize)
                std::memcpy(head, data, sizeof(head));
            Log("[type] %s id %llx data %p: %016llx %016llx %016llx %016llx\n", slot, static_cast<unsigned long long>(id),
                data, head[0], head[1], head[2], head[3]);
        }
        std::int32_t* ProcessSignal(void*, std::int32_t* out, std::uint32_t signal, void* data)                 // 14 (ErrorCode 0)
        {
            LogIncoming("ProcessSignal(14)", signal, data);
            *out = 0;
            return out;
        }
        // 16, 17: command forwarders with an out parameter (CP2 0x140a23b20, soldier 0x1409e91b0 return param 2).
        void* Command16(void*, void* out, std::uint64_t id, void* data)
        {
            LogIncoming("Command(16)", id, data);
            if (out) *static_cast<std::uint64_t*>(out) = 0;
            return out;
        }
        void* Command17(void*, void* out, std::uint64_t id, void* data)
        {
            LogIncoming("Command(17)", id | (1ull << 63), data);   // top bit keeps 17's ids apart from 16's
            if (out) *static_cast<std::uint64_t*>(out) = 0;
            return out;
        }
        std::uint64_t* Query18(void*, std::uint64_t* out, std::uint16_t) { *out = 0; return out; }     // 19 (= slot 18 result)
        bool CanBeRemoved(void*) { return true; }                                                    // 20 (player: ReturnOne)
        std::uint64_t* GetId(void*, std::uint64_t* out) { *out = kNoId; return out; }                 // 21
        void* DeletingDtor(void* self, unsigned flags)                                                // 23
        {
            if (flags & 1)
                delete Self(self);
            return self;
        }

        // Slots whose job is unknown (2, 3, 12, 13, 15, 18, 22 were Noop / ReturnZero): a bullet that reaches the gekko may arrive on any of them. Each logs its first calls and then every
        // 600th, with the raw argument registers, and returns 0 as before.
        template <int N> std::uint64_t Probe(void* self, std::uint64_t a, std::uint64_t b, std::uint64_t c)
        {
            static unsigned calls;
            if (++calls <= 12 || calls % 600 == 0)
                Log("[type] slot %d call #%u self %p args %llx %llx %llx thread %lu\n", N, calls, self, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                    static_cast<unsigned long long>(c), GetCurrentThreadId());
            return 0;
        }

        void* const kIfaceVtbl[24] =
        {
            (void*)&AllocResources, (void*)&FreeResources, (void*)&Probe<2>, (void*)&Probe<3>,
            (void*)&Activate, (void*)&Deactivate, (void*)&Locators, (void*)&Locators,
            (void*)&Info, (void*)&ExecuteSerially, (void*)&ExecuteInParallel, (void*)&Execute<2>,
            (void*)&Probe<12>, (void*)&Probe<13>, (void*)&ProcessSignal, (void*)&Probe<15>,
            (void*)&Command16, (void*)&Command17, (void*)&Probe<18>, (void*)&Query18,
            (void*)&CanBeRemoved, (void*)&GetId, (void*)&Probe<22>, (void*)&DeletingDtor,
        };

        // ---- factory (GameObjectFactoryInterface, slot order from CommandPost2FactoryImpl 0x1422ab7a0) ----
        void* FactoryDtor(void* self, unsigned) { return self; }                  // 0: static object, never freed
        std::uint64_t* GetTypeName(void*, std::uint64_t* out) { *out = kTypeName; return out; }  // 1
        std::uint32_t GetTypeIndex(void*) { return kTypeIndex; }                    // 2
        void* CreateObject(void*)                                                   // 3
        {
            Object* o = new Object{};
            o->vtbl = kIfaceVtbl;
            last_ = o;
            Log("[type] CreateGameObject -> iface %p\n", o);
            return o;   // the interface is the allocation start, as for Player2/Soldier2
        }
        void DeleteObject(void*, void* iface)                                       // 4
        {
            Log("[type] DeleteGameObject %p\n", iface);
            if (iface == last_)
                last_ = nullptr;
            delete Self(iface);
        }

        void* const kFactoryVtbl[5] = { (void*)&FactoryDtor, (void*)&GetTypeName, (void*)&GetTypeIndex, (void*)&CreateObject, (void*)&DeleteObject };
        struct Factory { void* const* vtbl; } factory_ = { kFactoryVtbl };

        // gamecore::GameObjectTypeManager::s_Instance, taken from RegisterGameObjectFactory's own
        // `mov rcx,[rip+disp32]` at +0x1b (bytes 48 8B 0D DE 39 04 02 -> 0x142c38020), so the data address
        // rides on the already-checked function instead of being hardcoded.
        void** TypeManagerSlot(std::uint16_t index)
        {
            const auto* code = reinterpret_cast<const std::uint8_t*>(registerFactory_) + 0x1b;
            if (code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x0D)
            {
                Log("[type] RegisterGameObjectFactory+0x1b is not mov rcx,[rip+x]: %02X %02X %02X\n", code[0], code[1], code[2]);
                return nullptr;
            }
            std::int32_t disp;
            std::memcpy(&disp, code + 3, 4);
            auto* manager = *reinterpret_cast<std::uint8_t* const*>(code + 7 + disp);
            return manager ? reinterpret_cast<void**>(manager + 8 + index * 8) : nullptr;   // FUN_140bf44c0
        }

        bool Resolve()
        {
            if (registerFactory_ && createGameObject_ && destroyGameObject_ && activateGameObject_)
                return true;
            registerFactory_ = reinterpret_cast<RegisterFactory_t>(GekkoSig::CheckByName("GameObject_RegisterFactory", "type"));
            createGameObject_ = reinterpret_cast<CreateGameObject_t>(GekkoSig::CheckByName("GameObject_Create", "type"));
            destroyGameObject_ = reinterpret_cast<DestroyGameObject_t>(GekkoSig::CheckByName("GameObject_Destroy", "type"));
            activateGameObject_ = reinterpret_cast<ActivateGameObject_t>(GekkoSig::CheckByName("GameObject_Activate", "type"));
            return registerFactory_ && createGameObject_ && destroyGameObject_ && activateGameObject_;
        }
    }

    bool Register()
    {
        if (registered_)
            return true;
        if (!Resolve())
            return false;
        void** slot = TypeManagerSlot(kTypeIndex);
        if (!slot || *slot)
        {
            Log("[type] type index %u unusable: slot %p holds %p\n", kTypeIndex, slot, slot ? *slot : nullptr);
            return false;
        }
        registerFactory_(&factory_);
        registered_ = *slot == &factory_;
        Log("[type] RegisterGameObjectFactory TppGekko index %u -> %s\n", kTypeIndex, registered_ ? "ok" : "slot not set");
        return registered_;
    }

    bool Place(float x, float y, float z, float yaw)
    {
        pose_[0] = x; pose_[1] = y; pose_[2] = z; pose_[3] = yaw;
        const bool first = !havePose_;
        havePose_ = true;
        if (!allocated_) return false;                 // the fox2 entity is not realized yet: spawn on AllocResources
        if (first) SpawnAtPose();
        return true;
    }

    bool Create(unsigned count, float x, float y, float z, float yaw)
    {
        pose_[0] = x; pose_[1] = y; pose_[2] = z; pose_[3] = yaw;
        havePose_ = true;
        if (!Register() || handle_)
            return handle_ != nullptr;
        Desc d{ kTypeIndex, 0, count, count, 0, nullptr };
        createGameObject_(&handle_, &d);
        Log("[type] CreateGameObject(count %u) -> GameObjectImpl %p, id base %u\n", count, handle_, kTypeIndex << 9);
        // Test 2: Create alone leaves impl flags 0x2e and no Execute calls; natives follow Create
        // with Activate (0x140570170), which sets 0x10 (every live native shows 0x3e).
        if (handle_)
            activateGameObject_(handle_);
        return handle_ != nullptr;
    }

    void Destroy()
    {
        if (!handle_)
            return;
        Log("[type] destroy %p\n", handle_);
        destroyGameObject_(handle_);
        handle_ = nullptr;
    }

    unsigned GameObjectId() { return static_cast<unsigned>(kTypeIndex) << 9; }

    const char* Status()
    {
        static char buf[256];
        const Object* o = last_;
        std::snprintf(buf, sizeof(buf), "registered %d handle %p iface %p alloc %u total %u realized %u active %d frames %u/%u/%u",
                      registered_, handle_, o, o ? o->allocCalls : 0, o ? o->total : 0, o ? o->realized : 0, o ? o->active : 0,
                      o ? o->frames[0] : 0, o ? o->frames[1] : 0, o ? o->frames[2] : 0);
        return buf;
    }
}
