// melee_test.cpp 29/09/2026. Runs the lifted Survive melee chooser outside the game with the real host (GekkoLift.cpp) and stubs
// for the game-side calls. Expected from the disassembly (random & 3): 0 -> action 2, 1 -> action 3, 2 -> action 3 variant 1, 3 -> action 1.
#include <cstdarg>
#include <cstdio>
#include "GekkoLift.h"
#include "GekkoDriver.h"

void Log(const char* fmt, ...) { va_list a; va_start(a, fmt); std::vprintf(fmt, a); va_end(a); }
namespace Gekko { bool Play(int slot, float) { std::printf("[stub] Play slot %d\n", slot); return true; } }

int main()
{
    GekkoLift::SetTarget(10.5f, 20.25f, -30.75f);
    std::printf("one run: %s\n", GekkoLift::MeleeTest(1, true));
    std::printf("4000 runs: %s\n", GekkoLift::MeleeTest(4000, false));
    return 0;
}