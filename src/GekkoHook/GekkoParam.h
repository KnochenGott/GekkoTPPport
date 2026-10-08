// GekkoParam.h 03/10/2026. Native write of the gekko parameter block into an SsdGeckoParameter struct (the lifted loader 0x140b983f0 is the declined alternative).
#pragma once
#include <cstdint>
namespace GekkoParam {
constexpr unsigned kStructMin = 0x198;   // highest written byte is +0x197 (coop_exp dword at +0x194)
// Writes every entry of GekkoParamBlock::kTable at its loader offset; other bytes are left alone. structBase must have kStructMin bytes. Returns entries written.
int Fill(uint8_t* structBase);
}
