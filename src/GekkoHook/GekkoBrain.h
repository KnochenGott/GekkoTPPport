// GekkoBrain.h 30/09/2026. Survive's own gekko behaviour running in TPP: the lifted AI state machine requests actions, the lifted action
// handlers play the clips. This layer is the per-frame call order; clip state and events come from the real controller.
#pragma once
#include <cstdint>

namespace GekkoBrain
{
    struct Game
    {
        void (*play)(std::uint64_t ganiPathId, float blendFrames) = nullptr;    // headless tests only: start a clip (with a real controller the lifted code drives it)
        void (*trace)(const char* line) = nullptr;
        void* controller = nullptr;   // the real AnimationController; when set the lifted code drives it directly (set motion, finished, speed)
    };
    // Builds the world and enters the idle action. With spawnSequence the AI runs its own first states (fall, stunned on the ground, get up: about
    // 20 s), otherwise it starts at state 12, nothing left to do.
    bool Start(const Game& game, bool spawnSequence);
    void Stop();
    bool Active();
    // What the game tells the brain each frame: the gekko's own pose and the enemy it reacts to.
    struct Input { float pos[3]; float yaw; float target[3]; bool hasTarget; bool interrupt = false; };   // interrupt: one frame, the target takes over from a route walk
    // One game frame: AI state machine, movement brain (orders), locomotion states, action dispatch.
    void Frame(float dt, const Input& in);
    // The heading the locomotion asked for this frame (target yaw in radians, N the smoothing constant of the CharaControl turn); false when it asks for none.
    void ControllerRan();   // the controller's stages ran this frame: a motion requested before now has been applied (see GekkoWorld::NotifyControllerRan)
    bool TakeSteer(float* yawRad, float* smoothing);
    void Request(unsigned action, unsigned flag, const float* target);   // manual action request (test hook)
    bool Melee(const float* target);           // Survive's melee chooser requests an attack action (1, 2 or 3)
    bool Busy();                               // an action requested through the mailbox is still running
    int  AiState();                            // the AI record's state (12 = idle)
    unsigned Action();                         // the running action (0 none, 29 idle)
    const char* Status();                      // one line: AI state, action, clip and clip time
}