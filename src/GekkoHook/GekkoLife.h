// GekkoLife.h 06/10/2026. The gekko's life and damage components over the host NPC service: Survive's life class, LifeController, generic and typed DamageHandler and damage calculator are created by their lifted creators; TPP hits enter through the typed handler.
#pragma once
#include <cstdint>

namespace GekkoLife
{
    constexpr int kParts = 21;   // hit spheres of GeckoTargetDefense.tgt
    // Before the Observer is built: parameter image (GekkoParam::Fill), manager and hub objects the components read, svc fields (docs/DOCUMENTATION.md 4.6).
    void AdoptService(std::uint8_t* svc);   // real SsdNpc service: the life interface the real slot stored at svc+0x68 replaces the one the host slot hook captured
    void* UnitStore();     // the unit registry stand-in Prepare stores at svc+0x190 (position slots 87/89/91, record slots 131/132, state query 99)
    void* ParamHolder();   // the parameter holder Prepare stores at svc+0x120 (+0x20 = the gekko parameter struct + 8)
    void Prepare(std::uint8_t* ctxSlot, std::uint8_t* svc, void* ownerEntry);
    // The creation manager (slot 0 of the context object): creator, Setup, handle. Zero for a hash it does not own (the caller keeps its stand-in handle).
    bool Owns(std::uint64_t hash);
    std::uint64_t Create(std::uint64_t outBuf, std::uint64_t hash, std::uint64_t desc);
    // The svc slots that build the components: +0x30 (handle of a component the host does not create), +0x408 life class, +0x410 damage handler.
    void InstallSvcSlots(void** vtbl);
    // LifeController interface (obj+0x20) once the life class has made it, else null.
    std::uint8_t* ControllerIface();

    struct State
    {
        bool ready = false;
        std::uint32_t life = 0, maxLife = 0, pending = 0;
        bool dead = false;
        std::uint16_t poolLeft[5] = {}, poolWaiting[5] = {}, poolStart[5] = {};   // pools: right leg, left leg, right arm gun, left arm launcher, camera
        bool broken[5] = {};
        std::uint32_t hits = 0;
    };
    bool Ready();
    bool Read(State& s);

    // One TPP hit: partIndex = position in GekkoFull::TargetObjects (21 hit spheres), damage = TPP damage points, flags = TPP hit flags, headshot = TPP marked it a head hit.
    constexpr unsigned kTppWeaponClass = 0x63;   // H+0x4e bits 0 to 6: first of Survive's gun weapon classes (0x63 to 0x72); 0 would make the calculator reduce the hit by defense / 10 percent (0x140f51080)
    bool Hit(int partIndex, int damage, unsigned weaponClass = kTppWeaponClass);
    // One life update (Survive's per-frame function of the life class): applies pending damage, runs the death rule.
    bool Update(float dt);
    // True once Survive sent the Dead event (the life class through the message hub); stays set.
    bool Dead();
    unsigned UnitPositionCalls(int kind);   // how often the unit registry's position slot of that kind (0, 1, 2) was asked (test aid)
    unsigned DeadEvents();   // how often the event went out (test aid)
    int RegionOfPart(int partIndex);
    int PoolOfRegion(int region);
}
