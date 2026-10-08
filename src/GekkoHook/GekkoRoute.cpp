// GekkoRoute.cpp 01/10/2026. TPP's route system for Survive's movement brain (docs/DOCUMENTATION.md): the lifted brain asks GetRoute and the route groups' vertex count / position directly.
// Retail addresses (mgsvtpp.exe 1.0.15.4): RouteSystemImpl::Init 0x140513930 stores the instance at 0x142baf2a8 (mov [rip+0x269b947], rax at 0x14051395a); instance vtable
// 0x1421872a0 (slot 0 GetRoute 0x140513860, slot 1 destructor); RouteGroupImpl vtable 0x142187500 (slot 1 GetVertexCount 0x140514cb0, slot 2 GetVertexPosition 0x140514d40).
// GetVertexCount was found by its exact bytes from the dev map symbol (tools/mgsv_addr.py), the others through the vtables; all bodies equal Survive's (tools/ctrl_slot_map.py norm).
#include "GekkoRoute.h"
#include "GekkoLog.h"
#include "GekkoSigCheck.h"
#include "GekkoWorld.h"

#include <windows.h>

namespace
{
    constexpr std::uintptr_t kInstance = 0x142baf2a8, kSysVtbl = 0x1421872a0, kGroupVtbl = 0x142187500;

    std::uintptr_t Slot(std::uintptr_t vtbl, int i) { return reinterpret_cast<const std::uintptr_t*>(vtbl)[i]; }

    void* ReadInstance()
    {
        __try { return *static_cast<void**>(ResolveGameAddress(kInstance)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
}

namespace GekkoRoute
{
    bool Attach()
    {
        void* get = GekkoSig::CheckByName("RouteSystem_GetRoute", "route");
        void* count = GekkoSig::CheckByName("RouteGroup_GetVertexCount", "route");
        void* pos = GekkoSig::CheckByName("RouteGroup_GetVertexPosition", "route");
        if (!get || !count || !pos) { Log("[route] signatures do not match: no route system\n"); return false; }
        const auto sysVt = reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kSysVtbl));
        const auto grpVt = reinterpret_cast<std::uintptr_t>(ResolveGameAddress(kGroupVtbl));
        if (Slot(sysVt, 0) != reinterpret_cast<std::uintptr_t>(get) || Slot(grpVt, 1) != reinterpret_cast<std::uintptr_t>(count) || Slot(grpVt, 2) != reinterpret_cast<std::uintptr_t>(pos))
        {
            Log("[route] vtables do not hold the checked functions: no route system\n");
            return false;
        }
        void* inst = ReadInstance();
        if (!inst || *static_cast<std::uintptr_t*>(inst) != sysVt)
        {
            Log("[route] instance %p is not a RouteSystemImpl (vtable %p): no route system\n", inst, inst ? *static_cast<void**>(inst) : nullptr);
            return false;
        }
        GekkoWorld::AllowNative(reinterpret_cast<std::uint64_t>(count));
        GekkoWorld::AllowNative(reinterpret_cast<std::uint64_t>(pos));
        GekkoWorld::SetRouteSystem(inst, reinterpret_cast<const void*>(grpVt));
        Log("[route] TPP route system %p attached\n", inst);
        return true;
    }

    void Walk(std::uint32_t routeId, unsigned vertex) { GekkoWorld::SetRoute(0, routeId, vertex); }
}
