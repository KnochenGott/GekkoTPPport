// GekkoParam.cpp 03/10/2026. Writes the generated table (GekkoParamBlock.h, from tools/gen_gekko_params.py) into the SsdGeckoParameter struct.
#include "GekkoParam.h"
#include "GekkoParamBlock.h"
#include <cstring>
namespace GekkoParam {
int Fill(uint8_t* b) {
    for (int i = 0; i < GekkoParamBlock::kCount; ++i) {
        const GekkoParamBlock::Entry& e = GekkoParamBlock::kTable[i];
        uint32_t v = e.value;
        switch (e.kind) {
        case 0: case 1: std::memcpy(b + e.off, &v, 4); break;
        case 2: { uint16_t w = (uint16_t)v; std::memcpy(b + e.off, &w, 2); break; }
        default: b[e.off] = (uint8_t)v; break;
        }
    }
    return GekkoParamBlock::kCount;
}
}
