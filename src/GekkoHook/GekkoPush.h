// GekkoPush.h 01/10/2026. The gekko's push-slide targets: Survive's own creation (0x140f80e30) and per-frame update (0x140f82280) run against TPP's real target service, so TPP's player slides away from the gekko.
#pragma once
#include <cstdint>

namespace GekkoPush
{
    struct Hooks { void (*trace)(const char* line) = nullptr; };

    // Builds the plugin-shaped block the two lifted functions read, creates the three PUSH_SLIDE offense targets of unit 0 (Survive's code; calls are forwarded to TPP's target service when
    // realPool is set, Survive's lifted pool is used in headless tests) and gives them TPP's user-data layout. gameObjectId is the gekko's game object id (the owner tag of its other targets).
    bool Start(const Hooks& hooks, void* realPool, unsigned gameObjectId);
    // Per frame: Survive's updater places the three spheres on the gekko's bones (controller slot 42), the adapter refreshes the TPP-side record fields.
    void Update(const float pos[3]);
    bool RunKind(int kind);   // the plugin's frame function for one execution kind (0 serial, 3, 5)
    void Stop();
    bool Active();
    void TestMode();   // headless test only: skip the retail address checks of the target service
}
