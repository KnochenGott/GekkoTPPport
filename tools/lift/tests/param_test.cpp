// param_test.cpp 03/10/2026. Headless: GekkoParam::Fill into a struct image, read back at the offsets of loader 0x140b983f0 (token PARAM_BLOCK_OK).
#include "GekkoParam.h"
#include <cstdio>
#include <cstring>
static int bad = 0;
#define CHK(c) do { if (!(c)) { std::printf("FAIL %s line %d\n", #c, __LINE__); ++bad; } } while (0)
template <class T> static T rd(const uint8_t* b, unsigned o) { T v; std::memcpy(&v, b + o, sizeof v); return v; }
int main() {
    uint8_t s[0x200]; std::memset(s, 0xCD, sizeof s);
    int n = GekkoParam::Fill(s);
    CHK(n == 86);
    CHK(rd<uint32_t>(s, 0x120) == 18000);  CHK(rd<uint32_t>(s, 0x124) == 5000);  CHK(rd<uint32_t>(s, 0x14c) == 150);
    CHK(rd<uint16_t>(s, 0x128) == 3000);   CHK(rd<uint16_t>(s, 0x12a) == 3000);
    CHK(rd<uint16_t>(s, 0x12c) == 300);    CHK(rd<uint16_t>(s, 0x12e) == 300);   CHK(rd<uint16_t>(s, 0x130) == 70);
    CHK(rd<uint16_t>(s, 0x138) == 1000);   CHK(rd<uint16_t>(s, 0x13a) == 1000);  CHK(rd<uint16_t>(s, 0x13c) == 1000);
    CHK(rd<uint16_t>(s, 0x13e) == 1000);   CHK(rd<uint16_t>(s, 0x148) == 2000);
    CHK(s[0x150] == 60);  CHK(rd<uint32_t>(s, 0x154) == 4072259683u);  CHK(rd<uint32_t>(s, 0x194) == 500);
    CHK(s[0x151] == 0xCD);                       // untouched byte stays
    CHK(rd<float>(s, 0x8) > 0 || rd<float>(s, 0x8) == 0);  // contactSightForm_distance written (not 0xCDCDCDCD)
    CHK(rd<uint32_t>(s, 0x8) != 0xCDCDCDCDu);
    CHK(rd<uint32_t>(s, 0xf8) != 0xCDCDCDCDu && rd<uint32_t>(s, 0x11c) != 0xCDCDCDCDu); // hearing first/last
    CHK(rd<uint32_t>(s, 0xc8) != 0xCDCDCDCDu && rd<uint32_t>(s, 0xf4) != 0xCDCDCDCDu);  // camouflage first/last
    std::printf("%s\n", bad ? "PARAM_BLOCK_FAIL" : "PARAM_BLOCK_OK");
    return bad;
}
