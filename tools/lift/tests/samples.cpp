// samples.cpp 29/09/2026. Functions the lifter is tested on: each is compiled by MSVC, lifted from the DLL's machine code and
// compared against the original on random input (tools/lift/tests/run_tests.py).
#include <cstdint>
#include <cmath>
#include <immintrin.h>
#define EXP extern "C" __declspec(dllexport) __declspec(noinline)

EXP int add3(int a, int b, int c) { return a + b + c; }
EXP uint64_t mix64(uint64_t a, uint64_t b) { a ^= b << 7; a += b >> 3; a *= 0x9E3779B97F4A7C15ull; return a ^ (a >> 29); }
EXP int popc(uint32_t x) { int n = 0; while (x) { x &= x - 1; ++n; } return n; }
EXP uint32_t gcd32(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }
EXP int sel(int a, int b, int c) { return a > b ? (a < c ? a : c) : b; }
EXP int sw(int x)
{
    switch (x)
    {
    case 0: return 11; case 1: return 22; case 2: return x * x + 5; case 3: return 100;
    case 4: return -7; case 5: return x << 3; case 6: return 9; case 7: return 1234; default: return x - 1;
    }
}
struct S { int a; int b; float f; };
EXP float sums(const S* p, int n) { float t = 0; for (int i = 0; i < n; i++) t += p[i].a * p[i].f + p[i].b; return t; }
EXP unsigned mix8(uint8_t a, uint8_t b) { uint8_t t = uint8_t(a * 3 + b); return (t >> 1) ^ unsigned(int8_t(b)); }
EXP int sx(int8_t a, int16_t b) { return a * b + int(int8_t(a ^ b)); }
EXP float fmix(float a, float b) { return a * b + 0.5f * (a - b) + sqrtf(fabsf(a * b)); }
EXP float fclamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
EXP float dot4(const float* a, const float* b)
{
    __m128 x = _mm_loadu_ps(a), y = _mm_loadu_ps(b);
    __m128 m = _mm_mul_ps(x, y);
    __m128 s = _mm_add_ps(m, _mm_shuffle_ps(m, m, 0xB1));
    s = _mm_add_ps(s, _mm_shuffle_ps(s, s, 0x4E));
    return _mm_cvtss_f32(s);
}
EXP float fconst(float x) { return x * 3.14159f + 2.5f; }
static const float kTab[8] = { 1.f, 2.5f, -3.f, 0.125f, 7.f, 9.5f, -0.25f, 100.f };
EXP float tab(int i) { return kTab[i & 7]; }
EXP int helper(int x) { return x * 7 + 3; }
EXP int callit(int x) { return helper(x) + helper(x + 1); }
EXP float nat_helper(int a, float b) { return float(a) * b + 1.0f; }
EXP float callnat(int x, float y) { return nat_helper(x, y) + nat_helper(x + 2, y * 0.5f); }
EXP void fill(int* d, int n, int v) { for (int i = 0; i < n; i++) d[i] = v + i; }
EXP int f2i(float x) { return int(x) + int(x * 2.5f); }
EXP float i2f(int a, int b) { return float(a) * 0.5f + float(b); }
EXP uint64_t u64ops(uint64_t a, uint64_t b) { return ((a >> 3) | (b << 5)) ^ (a & b); }
EXP int cmpops(int a, int b, unsigned ua, unsigned ub)
{
    int r = 0;
    if (a < b) r |= 1; if (a <= b) r |= 2; if (ua < ub) r |= 4; if (ua >= ub) r |= 8; if (a == b) r |= 16;
    return r;
}
EXP double dmix(double a, double b) { return a * b + a / (b == 0 ? 1.0 : b); }
EXP int64_t mulmix(int64_t a, int32_t b, int16_t c) { return a * b + int64_t(c) * 3 - (a >> 5); }
EXP void copy16(int* d, const int* s) { for (int i = 0; i < 16; i++) d[i] = s[i] ^ i; }EXP int64_t div100(int32_t a, int64_t b, int32_t c) { return int64_t(a / 100) + (b / 100) + (c / 7); }   // compilers turn the constant divisions into the one-operand imul (magic number multiply)
