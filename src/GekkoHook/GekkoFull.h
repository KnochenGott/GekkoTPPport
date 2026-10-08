// GekkoFull.h 29/09/2026. The gekko on TPP's full AnimationController, set up with Survive's gekko values (no .mog).
#pragma once
#include <cstdint>

namespace GekkoFull
{
    // Creates the controller from the gekko's AnimationControllerDesc (parts + mtar File* already found), adds the mtar,
    // warps the controller's own CharaControl to x/y/z/yaw. Logs every object it gets by vtable.
    bool Spawn(void* partsFile, void* mtarFile, float x, float y, float z, float yaw);
    bool Play(std::uint64_t ganiPathId, float blend);  // TppMotionDesc to slot 58, as Survive's +0x1f0
    // One frame from ExecuteInParallel with the engine's GameObjectParallelExecutionContext: per-instance run/skip from
    // the bitmap, then the stages in Sahelan's order (act motion, CharaControl, put motion).
    bool Execute(const void* ctx);   // true when the stages ran (false: paused or no controller)
    bool Feet(float* xyz);                             // the CharaControl's feet (root) position
    void SetYaw(float yaw);                            // one-shot manual heading: delta rotation through the VelocityControlAction
    void SetMove(const float* v3);                     // a translation (the unit's local frame) added to the next run frame's root motion (VelocityControlAction flag 2): the walk path correction
    void SetTurn(bool on, float yaw, float n);         // the CharaControl's own turn target (flag 0x100): the heading request of the lifted locomotion
    bool TurnState(std::uint32_t* flags, float* n, float* targetYaw);   // the CharaControl's turn fields as the engine sees them (cc+0x188 flags, cc+0x1ac N, cc+0x90 target)
    void Freeze(bool on);                              // dead: the controller only gets the pause (skip) call, so the last pose stays
    float Yaw();                                       // body heading (CharaControl rotation)
    double RunClock();                                 // controller run time in seconds since load (the dt of every run frame, never reset)
    float ConsumeRunTime();                            // run-frame seconds since the last call (0 while paused)
    void* Controller();
    void* CharaControl();                              // the controller's CharaControl (dev probes)
    bool Grounded();                                   // CharaControl state bit 0x20 (grounded on every native standing, measured)
    bool TargetObjects(std::uint8_t*** objs, unsigned* count);   // the gekko's hit-target objects (dev probe)
    void Despawn();
    bool IsLive();
}
