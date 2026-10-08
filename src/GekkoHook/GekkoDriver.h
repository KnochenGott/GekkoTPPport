// GekkoDriver.h 29/09/2026. Spawns and animates the Metal Gear Survive gekko in TPP on the full AnimationController
// (GekkoFull), as Survive does. Finds the parts and mtar files, plays the Survive gekko clip table. Every engine call is
// logged and wrapped in SEH, so a fault is written to Gekko.log instead of taking the game down.
#pragma once

#include <cstdint>

namespace Gekko
{
    enum class Step : int
    {
        None = 0,
        FilesFound,        // parts and mtar File* resolved
        Placed,            // full controller created, realized and ticking
    };

    // Load-time check of the file lookup signature. False (and logged) on another exe build.
    bool Verify();
    bool IsVerified();

    // debug level: 0 = errors only, 1 = every step, 2 = every step + raw pointers
    void SetDebugLevel(int level);

    bool FindFiles();
    bool Spawn(float x, float y, float z, float yaw);    // yaw in radians; files, then GekkoFull::Spawn, then the idle

    bool Play(int slot, float blend);                    // slot = Survive gecko table index 0..78 (db/gecko_actions.tsv)
    bool PlayGani(std::uint64_t ganiPathId, float blend);
    void GetPose(float* xyzYaw);                         // the CharaControl's feet and the heading (radians)
    bool SetPose(float x, float y, float z, float yaw);  // heading only: the CharaControl owns the position
    void Despawn();

    Step        GetStep();
    const char* GetLastError();
    void        LogStatus();                             // writes the full state to the log
}
