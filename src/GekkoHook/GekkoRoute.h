// GekkoRoute.h 01/10/2026. TPP's route system (fox::gs::impl::RouteSystemImpl) as the route service of Survive's movement brain.
#pragma once
#include <cstdint>

namespace GekkoRoute
{
    // Finds TPP's route system instance, checks the signatures and vtables, and hands it to GekkoWorld (call after GekkoWorld::Init). False when anything does not match: no route is found then.
    bool Attach();
    // Unit 0 walks route routeId (the number GsRoute.GetRouteId(name) returns, StrCode32 of the name) from vertex. 0xbf169f98 (GsRoute.ROUTE_ID_EMPTY) clears it.
    void Walk(std::uint32_t routeId, unsigned vertex);
}
