// GekkoNavProxy.h 02/10/2026. Survive-order proxies for TPP's NavigationSystem and Pathfinder plus the TPP -> Survive Path2Impl conversion (docs/DOCUMENTATION.md).
#pragma once
#include <cstdint>

namespace NavProxy
{
    struct Config
    {
        void* realScope = nullptr;               // TPP's scope table T (the result of the accessor 0x140bff050 in the game, a mock headless): +0xa0 = NavigationSystem S, +0x98 = object P
        std::uint64_t sVtbl = 0x142511820ull;    // expected [S]; a different vtable means another slot order, Init refuses
        std::uint64_t bVtbl = 0x142516090ull;    // expected [B] of every Pathfinder S creates
        void (*trace)(const char* line) = nullptr;
        bool (*callLifted)(std::uint64_t addr, std::uint64_t rcx, std::uint64_t rdx, std::uint64_t r8, std::uint64_t r9) = nullptr;   // runs a Survive function (the Path2Impl ctor 0x141bf1e80)
    };

    // Reads S and P from the real scope, checks [S], builds the proxy vtables. False (with a trace line) when anything does not match.
    bool Init(const Config& cfg);
    bool Ready();
    // Fills Survive's scope table T (the host stand-in): +0xa0 = proxy S, +0x98 = proxy P.
    void FillScope(std::uint8_t* surviveScope);
    // Rebuilds the Survive-layout path tables of every proxy Pathfinder from the real ones. Call once per frame before the controller Update.
    void Sync();
    void Diag(int id, int frame);   // Logs changes of the real entry state / flags / point count and of the converted path (capped)

    // Exposed for the test: converts one TPP Path2Impl into a Survive Path2Impl (already constructed). Returns false on a bad count.
    struct Path;
    Path* NewPath();
    bool ConvertPath(Path* dst, const std::uint8_t* tppPath);
    // TPP's accessor 0x140bff050 prologue check (13 bytes), false when the retail bytes differ.
    bool AccessorOk();
}
