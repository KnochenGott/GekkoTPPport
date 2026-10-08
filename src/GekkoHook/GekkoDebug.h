// GekkoDebug.h 02/10/2026. Debug options and part-by-part calls into the lifted Survive code (Lua: Gekko.Debug*), every call logs what it did and the result.
#pragma once
#include <cstdint>

namespace GekkoDebug
{
    // Option: start Survive's lifted NavigationController2Impl as the nav service (GekkoWorld::StartNavController). Default on; read when the brain starts.
    void SetNavController(bool on);
    bool NavController();

    bool Action(unsigned id, unsigned flag, const float* target);   // mailbox request of Survive action id (0..31), as the AI would send it
    bool State(unsigned n);                                          // real transition: exit handler of the current AI state, enter handler of state n (0..12) (GekkoWorld::AiTransition)
    int  Choose();                                                   // runs Survive's AI state chooser 0x140f8d510 and returns the state it picked (-1 fault)
    bool Event(std::uint32_t hash, unsigned param);                  // animation event StrCode32 through the lifted event path
    bool Dispatch();                                                 // one lifted action dispatch (kind 1) outside the frame order
    bool Melee(const float* target);                                 // Survive's melee chooser requests an attack action
    bool Damage(int amount, int part);                               // TPP damage points on a hit sphere (0..20) through the same path as a bullet: Survive's damage code, then one life update
    bool Impact(int part, const float* player);                      // TPP impact effect (GekkoHitFx::Debug) on part 0..20 in front of the player position, no gun needed
    void Snapshot(const char* label);                                // one block of state lines into the log
}
