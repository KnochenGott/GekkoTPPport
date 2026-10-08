// GekkoNodes.h 08/10/2026. Survive's behaviour container and its six child nodes: class registration and creation by key over the host NPC service (docs/DOCUMENTATION.md 5.15).
#pragma once
#include <cstdint>

namespace GekkoNodes
{
    // Runs the static initialisers of the container class and its six child classes (they append their descriptors to the scope singleton's class list).
    void Register(std::uint8_t* scope);
    // True when the class list holds a descriptor for the key.
    bool Owns(std::uint64_t key);
    // Slot 0 of the creation context: Survive's creator (descriptor slot 1), then Setup (slot 1 of the new object). Writes the handle to outBuf and returns a pointer to it.
    std::uint64_t Create(std::uint64_t outBuf, std::uint64_t key, std::uint64_t args);
    // Objects created so far with their key, for the log.
    unsigned Count();
    // Experiment: creates the real SsdNpc component (class key 0xb567a90fe2c5) and returns its instance, or null.
    std::uint8_t* CreateSsdNpc(std::uint8_t* ownerHandle, void* paramHolder = nullptr, const std::uint64_t* managers = nullptr);   // managers: descriptor +0x38, +0x40, +0x48 (the three accessor results)   // paramHolder: descriptor +0x30, stored by Setup at instance +0x140 (the Observer reads [[service+0x120]+0x20])
}
