// GekkoNodes.cpp 08/10/2026. Survive's behaviour container and its six child nodes: class registration and creation by key (docs/DOCUMENTATION.md 5.15).
#include "GekkoNodes.h"
#include "GekkoClassInits.h"
#include "GekkoWorld.h"
#include "lift_rt.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
    // Static initialisers of the classes in tools/lift/classes.txt (generated list): each stores {descriptor vtable, key, 0} in a global and appends it to the class list (register 0x141989090).

    std::uint8_t* g_scope = nullptr;   // scope singleton: +0x10 class count, +0x18 descriptor pointers (up to 0x800)
    struct Entry { void* pad[2]; void* obj; void* more[5]; };   // creation handle: +0x10 is the component the interface lookups ask
    void* g_handles[64];
    unsigned g_made = 0;

    void Log(const char* fmt, ...)
    {
        char b[256];
        va_list a; va_start(a, fmt); _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a); va_end(a);
        GekkoWorld::Log(b);
    }
    template <class T> T get(const void* base, std::size_t off) { return *reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(base) + off); }

    const std::uint8_t* Find(std::uint64_t key)
    {
        if (!g_scope) return nullptr;
        const std::uint32_t n = get<std::uint32_t>(g_scope, 0x10);
        for (std::uint32_t i = 0; i < n && i < 0x800; ++i)
        {
            const std::uint8_t* d = get<const std::uint8_t*>(g_scope, 0x18 + 8 * i);
            if (d && get<std::uint64_t>(d, 8) == key) return d;
        }
        return nullptr;
    }
}

namespace GekkoNodes
{
    void Register(std::uint8_t* scope)
    {
        if (g_scope) return;
        g_scope = scope;
        for (std::uint64_t init : kClassInits)
            if (!GekkoWorld::CallAddr(init, 0, 0, 0, 0, nullptr)) Log("[nodes] static init %llx: %s", static_cast<unsigned long long>(init), GekkoWorld::LastFault());
        Log("[nodes] class list holds %u descriptors", g_scope ? get<std::uint32_t>(g_scope, 0x10) : 0u);
    }

    bool Owns(std::uint64_t key) { return Find(key) != nullptr; }

    unsigned Count() { return g_made; }

    // Experiment (docs 5.16): the real SsdNpc component through the class list, with the creation descriptor the NPC creator 0x140b6c370 builds (READ at 0x140b6cd05..0x140b6cd7d): +8 first qword of
    // the type record, +0x18 type hash, +0x1c and +0x20 the NPC counts, +0x24 type id (the gekko is 13), +0x28 a type table dword. Values the creator derives from the NPC type database (hash
    // 0xbf304b9d for id 13, measured in the parked registry work) are set here.
    std::uint8_t* CreateSsdNpc(std::uint8_t* ownerHandle, void* paramHolder, const std::uint64_t* managers)
    {
        static alignas(16) std::uint8_t D[0x80];
        std::memset(D, 0, sizeof(D));
        *reinterpret_cast<void**>(D + 0x00) = ownerHandle;
        *reinterpret_cast<std::uint64_t*>(D + 0x08) = 0xb8a0bf169f98ull;
        *reinterpret_cast<std::uint32_t*>(D + 0x18) = 0xbf304b9du;
        *reinterpret_cast<std::uint32_t*>(D + 0x1c) = 1;
        *reinterpret_cast<std::uint32_t*>(D + 0x20) = 1;
        *reinterpret_cast<std::uint16_t*>(D + 0x24) = 13;
        *reinterpret_cast<void**>(D + 0x30) = paramHolder;   // Setup 0x141076a28..0x141076a2c stores D+0x30 at instance +0x140
        if (managers) for (int i = 0; i < 3; ++i) *reinterpret_cast<std::uint64_t*>(D + 0x38 + 8 * i) = managers[i];
        static void* out[2] = {};
        const std::uint64_t h = Create(reinterpret_cast<std::uint64_t>(out), 0xb567a90fe2c5ull, reinterpret_cast<std::uint64_t>(D));
        if (!h) return nullptr;
        const std::uint8_t* handle = static_cast<const std::uint8_t*>(out[0]);
        return handle ? *reinterpret_cast<std::uint8_t* const*>(handle + 0x10) : nullptr;
    }

    std::uint64_t Create(std::uint64_t outBuf, std::uint64_t key, std::uint64_t args)
    {
        const std::uint8_t* d = Find(key);
        if (!d) return 0;
        const std::uint64_t creator = get<std::uint64_t>(reinterpret_cast<const void*>(get<std::uint64_t>(d, 0)), 8);
        std::uint64_t obj = 0;
        if (!GekkoWorld::CallAddr(creator - lift::kTokenBase, 0, 0, 0, 0, &obj) || !obj) { Log("[nodes] creator of %llx: %s", static_cast<unsigned long long>(key), GekkoWorld::LastFault()); return 0; }
        Entry* e = new Entry{};
        e->obj = reinterpret_cast<void*>(obj);
        *reinterpret_cast<std::uint64_t*>(obj + 8) = key;
        *reinterpret_cast<void**>(obj + 0x10) = e;
        // Survive's creation function (0x140ca7720, READ): after the creator, object+8 = key and object+0x10 = the creation handle, both before Setup; the components read it as their owner (SsdNpc +0x2b8).
        const std::uint64_t setup = get<std::uint64_t>(reinterpret_cast<const void*>(get<std::uint64_t>(reinterpret_cast<const void*>(obj), 0)), 8);
        std::uint64_t rax = 0;
        if (!GekkoWorld::CallAddr(setup - lift::kTokenBase, obj, args, 0, 0, &rax)) { Log("[nodes] Setup of %llx: %s", static_cast<unsigned long long>(key), GekkoWorld::LastFault()); return 0; }
        void** handle = &g_handles[g_made & 63];
        *handle = e;
        ++g_made;
        if (outBuf) *reinterpret_cast<void**>(outBuf) = e;   // one qword: Survive's creation function (0x140ca7720) writes only [out]; callers pass an out slot that overlaps their own saved registers
        Log("[nodes] created %llx at %llx", static_cast<unsigned long long>(key), static_cast<unsigned long long>(obj));
        return reinterpret_cast<std::uint64_t>(handle);
    }
}
