// GekkoWorld.h 30/09/2026. The shim world that Survive's lifted gekko code runs against, in the game and in the test harness.
#pragma once
#include <cstddef>
#include <cstdint>

namespace GekkoWorld
{
    struct Hooks
    {
        void (*trace)(const char* line) = nullptr;                             // one line per event, may be null
        void (*setMotion)(std::uint64_t ganiPathId, float blend) = nullptr;   // controller vtable +0x1f0
        void (*setSpeed)(float scale) = nullptr;                              // controller vtable +0x2b8
        // Survive's geo service (+0x18 and +0x20 of [[G+0x80]+8]) answers with TPP's CollisionLineCheck: (GeoCheckControl, flags, from, to, radius) -> hit count. Null headless: no hits.
        double (*runClock)() = nullptr;                                       // seconds of controller run time so far (the dt the controller stages were fed); null: unknown
        float (*clipSeconds)(std::uint64_t ganiPathId) = nullptr;             // length of a clip in seconds (mtar frames at 60 fps), 0 when unknown
        int (*lineCheck)(void* control, std::uint32_t flags, const void* from, const void* to, float radius) = nullptr;
    };

    // Builds the structures the action handlers read (layouts in docs/DOCUMENTATION.md) and registers the lifted module.
    void Init(const Hooks& hooks);
    bool Begin();   // the Observer setup, which creates the ActionController (needs the nav service final): call after StartNavController; idempotent, false when the controller was not created
    void* ActionControllerIface();   // [svc+0x80] after Begin, null before (test aid)
    bool ActionUpdate();   // the ActionController update (Survive 0x140e7a0f0): once per frame after the brain and before the locomotion tick; commits queued requests
    // Route the controller calls of the lifted code (set motion, speed, finished, play time) to the real TPP AnimationController. Call after Init.
    void SetController(void* ctrl);

    // ActionController mailbox as seen from outside (the lifted AI uses the same path): a request for `action` with a flag byte and target.
    void Request(int id, unsigned action, unsigned flag, const float* target);
    bool SendRequest(int id, std::uint8_t* req);   // a whole 0xc0-byte request (16-byte aligned) through Survive's mailbox 0x140e96b60, as the movement brain and the AI states send theirs
    void RequestUnitTarget(int id, unsigned action, unsigned flag, unsigned unitId, unsigned slot);   // Request() with a unit target descriptor (kind 2, selector 0): the resolver 0x140f7f240 asks the unit registry
    // Makes `action` the pending action of instance `id` (the instance initialiser 0x140fa1340 copies the first request the same way);
    // without a current action nothing polls the mailbox, so the first Dispatch(1) enters it.
    bool InitInstance(int id);
    void Prime(int id, unsigned action);
    void ResetUnit(int id);                      // puts the unit's lifted state back to the state after Begin (a start after a stop or a fault)
    bool AllocBodies(int id);                    // Survive's record object allocation 0x140f943b0 (the three target objects at record +0x180, +0x188, +0x190)
    std::uint8_t* RecordBytes(int id);           // the Realized record of a unit (0x2c0 bytes)
    // Realized-side event function 0x140f93d30: kind 1 polls requests and runs exit/enter/update, kind 3 handler event 5, kind 4 event 6.
    bool Dispatch(int kind, int id);
    // Animation event of the playing clip (StrCode32), through 0x140f94cb0, which sets the record's event flag bits.
    bool FireEvent(int id, std::uint32_t hash, unsigned param);
    // Runs one action handler directly with a handler event (1 enter, 2 exit, 3 update, 9 move step). False on a fault.
    bool Run(int action, int event);
    // Bring-up helper: calls any lifted Survive function with four register arguments and returns rax (the real host classes are built with it).
    bool CallAddr(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, std::uint64_t* rax);

    void SeedRandom(std::uint32_t state);   // the xorshift state word of the NPC service (svc+0x1c); seeded from the system tick count at Init
    void NameTrap(void* object, const char* name);   // names an object for the [trap] lines
    void SetDt(float seconds);               // time step the lifted timers read (0x140f9e8ff: NPC service +0x48 -> +0x38 table)
    // TPP applies a requested motion when the controller's stages run, Survive's controller has it applied when the call returns, so the finished query right after a motion request (the action's
    // enter and update run in one call) saw the previous clip: 06:00 (finished stayed 1 for 18 ms after the turn clip was set, the turn was cut off by the walk start).
    // The query answers 0 until the controller has run once after the request.
    void NotifyControllerRan();
    void SetMotionFinished(bool finished);   // what controller vtable +0x2f8 answers
    const char* LastFault();
    void Log(const char* line);   // one trace line through the host hook (no-op without one)

    // Services a port component hands to lifted code that lives outside the shim (GekkoPush):
    void AllowNative(std::uint64_t addr);   // a TPP function the native gate lets through: the caller has verified it (same slot and arguments as TPP's own callers)
    void* LiftedPool();                     // Survive's lifted shared-object pool ([[G+0x80]+0x10] in the lifted code's manager copy; headless tests use it as the target service)
    void* ControllerObject();               // an object whose vtable is the controller table the lifted code calls (TPP forwarders when a real controller is set)
    void* RealTargetPool();                 // TPP's target service pool (null headless)
    void* NpcService();                     // the shim's NPC service object (svc)
    void* NpcEntryTable();                  // NPC state entries, 0x130 bytes per unit
    void* TrapObject(const char* name, int depth);   // a logging trap object for service pointers not modelled yet
    void* SiblingComponent();               // the host block answering interface 0x2fbbd3df1420 (the plugin frame function reads its +0x10 u16 table)
    void* ObserverIface();                  // the gekko observer's interface (this+0x30) that the sibling components cache at +0x140

    // The AI state machine 0x140f8d3f0: enter state 0, then update once per frame (it requests actions through the mailbox).
    // The melee chooser 0x140f911f0 (event 0): picks action 2, 3 (flag = variant) or 1 at random and requests it through the mailbox.
    bool MeleeChoose(int id);
    bool AiStart(int id);
    bool AiTransition(int id, unsigned n, int* before);   // exit handler of the current AI state, enter handler of state n (0..12), as the state machine does on a change
    bool AiUpdate(int id);
    void SetTarget(float x, float y, float z);   // what the AI sees as the enemy position (target provider)
    void SetTargetState(bool has, float strength, bool interrupt = false);   // target provider row: +0x30 id or -1, +0x34 strength (above 0 = live), +0x6a one-frame route interrupt
    std::uint8_t* Order();                           // the order record the movement brain reads (layout in GekkoWorld.cpp)
    std::uint8_t* TargetObj();                       // the target provider object bytes (test access)
    bool MoveUpdate(int id);
    void LocoTarget(int id, float x, float y, float z);   // record +0x10: where the locomotion component walks to
    bool LocoTick(int kind, int id);                  // component base class tick on the locomotion component (1 main, 3 and 5 late)
    bool MoveStep(int id);                            // the locomotion's move step (mode 7 of its state, interface slot 20): the turn state clears the heading request here; the step it computes is kept for TakeStep
    bool TakeStep(float* v);                          // the translation of the last MoveStep (3 floats, the unit's local frame): the walk state's correction toward the path; false when the step was not written
    void LocoState(int id, unsigned state);           // request a locomotion state (force)
    unsigned LocoStateNow(int id);
    void SetRealNpc(bool on);                         // use Survive's real SsdNpc service instead of the host buffer (before Begin); default off, env GEKKO_REALNPC turns it on
    void CompareSvc(const std::uint8_t* realSvc);     // test aid: host service buffer against a real SsdNpc service, per qword
    void DumpLoco(int id);   // test aid: prints the request and the locomotion records
    void StallLine(int id, char* out, std::size_t n);   // one line with everything the walk waits on (brain, order entry, action, locomotion, events, heading request, clip); used by the once-per-second stall trace
    const std::uint8_t* RequestBytes(int id);   // the last whole request of an instance (diagnostics)
    std::uint8_t* MotionOut();   // the controller's motion output struct the lifted code writes (headless: shim; in the game the real controller's)
    void SteerBegin();   // around the lifted brain frame: the locomotion's heading request (shadow of motion output +0x80, +0x190, +0x1a8) goes in, TPP's own bytes there are kept
    void SteerEnd();     // the shadow takes the brain's values back and TPP's bytes are restored
    void TurnLine(char* out, std::size_t n);   // clip, locomotion state, heading request and who changed it last (for the [turnlog] line)
    bool TakeSteer(float* yawRad, float* smoothing);   // the heading the locomotion asks for: target yaw and N (frames, see GekkoWorld.cpp); the request stays until the locomotion clears it
    bool LocoCall(std::uint64_t fn, int event, int id);   // one of the locomotion handlers (0x140f89af0, 0x140f89c10, 0x140f89ec0) with an event
    bool LocoRun(int event, int id);                 // the locomotion component 0x140f89c10 (events 1, 2, 5, 6)                         // the movement brain 0x140fa46d0: mode + target -> movement action requests
    unsigned PathCount(int id);    // points of the path the order holds, 0 when none
    void SetPose(int id, float x, float y, float z, float yaw);   // the NPC's own position and heading, as the lifted code reads them
    // The route system the movement brain walks routes from (TPP's fox::gs::impl::RouteSystemImpl instance in the game, verified by the caller; groups must carry groupVtbl unless null).
    void SetRouteSystem(void* routeSystem, const void* groupVtbl);
    // Slot-order proxies for TPP's NavigationSystem and Pathfinder behind Survive's scope table (GekkoNavProxy.cpp). realScope null: the real scope table of the game. sVtbl/bVtbl 0: retail values.
    bool StartNav(void* realScope = nullptr, std::uint64_t sVtbl = 0, std::uint64_t bVtbl = 0);
    float LastFloat();   // xmm0 of the last lifted call (test aid)
    void NavSync();   // converts the real pathfinders' paths to Survive layout; once per frame before the controller update
    // The lifted NavigationController2Impl as [svc+0x60] (after StartNav). units = capacity of its per-unit tables. NavUpdate: position feed (slot 24) and Update (slot 15), once per frame after NavSync.
    bool StartNavController(std::uint32_t units);
    bool NavControllerActive();
    std::uint64_t NavControllerObject();   // the primary object (interface = +0x20), test aid
    bool NavUpdate(int id, float x, float y, float z);
    void NavDiag(int id, int frame);   // Read-only log of order handle / mode / timer, controller record, real pathfinder entry (changes only, capped)
    bool CallAddrX(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9, float xmm3, std::uint64_t arg5, std::uint64_t* rax);   // CallAddr with xmm3 and the fifth (stack) argument
    void RouteLoopStep();   // call once per frame after the brain: restarts the route from vertex 0 when the brain reported its end
    void StopRouteLoop();
    void SetRoute(int id, std::uint32_t routeId, unsigned vertex);   // order record: walk route routeId (StrCode32 of the route name) from vertex; 0xbf169f98 = none

    std::uint8_t* NpcEntry(int id);              // the NPC state table entry (0x28 bytes: flag qwords at +0x00, +0x10, +0x18, dwords at +0x20, +0x24)
    int PickState(int id);                        // the AI's state chooser 0x140f8d510: flag bits in, state out (-1 on a fault)
    void SetAiState(int id, unsigned state);   // writes the AI record state byte (12 = nothing left to do), without enter/exit events
    struct State { std::uint8_t action, pending; std::uint32_t eventFlags; std::uint16_t controllerFlags; std::uint8_t aiState; float aiTimer; };
    State Peek(int id);
}