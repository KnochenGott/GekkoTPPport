// GekkoAi.h 01/10/2026. Gekko brain in TPP: Survive's own lifted brain (GekkoBrain) driven once per game frame.
#pragma once
#include <cstdint>

namespace GekkoAi
{
    void Start();                                // builds the brain on the gekko's controller
    void Stop();
    void SetTarget(float x, float y, float z);   // where the brain sends the gekko (call when it changes; the brain keeps the last)
    void Route(std::uint32_t routeId, unsigned vertex);   // walk TPP route routeId (GsRoute.GetRouteId) from vertex with Survive's movement brain; drops the target
    void SetSteerGate(bool on);                  // heading request only while the walk state is current (default on, docs 4.13)
    void Update(float dt);                       // one game frame, after GekkoFull::Execute
    int  GetState();                             // -1 off, 9 brain running
}
