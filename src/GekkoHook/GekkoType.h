// GekkoType.h 29/09/2026. Native TPP game object type "TppGekko" (type index 100): factory + 24-slot interface.
#pragma once

namespace GekkoType
{
    bool Register();      // RegisterGameObjectFactory once; false if a signature check fails or the slot is taken
    // fox::gm::CreateGameObject {100, count, count, no data entity}, then Activate. AllocResources spawns the gekko
    // at the pose; ExecuteSerially ticks it (animation, probe, AI) every frame.
    bool Place(float x, float y, float z, float yaw);  // position for a fox2-created gekko; spawns it once realized
    bool Create(unsigned count, float x, float y, float z, float yaw);
    void Destroy();       // removes the object Create made; FreeResources despawns the gekko
    const char* Status(); // one line: registered, created, AllocResources desc, frames per Execute* slot
    unsigned GameObjectId();  // TppGekko instance 0: kTypeIndex << 9 (51200)
}
