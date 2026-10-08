// GekkoLog.h 25/09/2026. Log file (MGS_TPP\Gekko.log, next to the exe) and game address rebasing.
#pragma once
#include <cstdint>

void  Log(const char* fmt, ...);          // appends and flushes every line, so a crash never loses the last one
void* ResolveGameAddress(std::uintptr_t absAddr);  // absolute address in a 0x140000000-based exe -> live address
