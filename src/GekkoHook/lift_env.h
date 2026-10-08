// lift_env.h 29/09/2026. Environment functions the lifted Survive code calls (chosen with lift.py --env). Implemented in GekkoLift.cpp.
#pragma once
#include "lift_rt.h"

void env_alloc(lift::Cpu& c);      // Survive fox::BlockHeapAlloc 0x14006bd20: rcx size, rdx alignment, r8d flags; returns the block in rax
void env_free(lift::Cpu& c);       // Survive pool-block free 0x141ad6c10: rcx the block env_alloc returned
void env_memset(lift::Cpu& c);     // MSVCR110 memset thunk 0x141924790: rcx dst, edx value, r8 size; returns dst
void env_memcpy(lift::Cpu& c);     // memcpy thunk 0x141924360: rcx dst, rdx src, r8 size; returns dst
void env_memcmp(lift::Cpu& c);     // memcmp thunk 0x1419247fe: rcx a, rdx b, r8 size; returns int in eax
void env_alloc16(lift::Cpu& c);    // Survive allocator wrapper 0x140004a30: rcx size, alignment fixed 16
