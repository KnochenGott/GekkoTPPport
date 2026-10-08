// test_main.cpp 29/09/2026. Differential test: each sample function runs natively (from samples.dll) and lifted (from the
// generated C++), on the same random input; results must match bit for bit.
#include "lift_rt.h"
#include <windows.h>
#include <random>
#include <string>
using namespace lift;

void lifted_register();
static std::mt19937_64 rng(20260929);
static HMODULE g_mod;
static int g_fail = 0, g_total = 0;

static void* sym(const char* n) { void* p = (void*)GetProcAddress(g_mod, n); if (!p) { std::printf("missing %s\n", n); std::exit(2); } return p; }
static LiftFn lf(const char* n) { LiftFn f = find_fn((std::uint64_t)sym(n)); if (!f) { std::printf("%s was not lifted\n", n); std::exit(2); } return f; }
static void report(const char* name, int bad, int trials)
{
    ++g_total; g_fail += bad != 0;
    std::printf("%-8s %s (%d/%d mismatches)\n", name, bad ? "FAIL" : "ok  ", bad, trials);
}
static std::uint64_t rnd() { return rng() >> (rng() % 60); }
static float rf() { return float(std::int64_t(rng() % 20001) - 10000) / 97.0f; }
static std::uint32_t bits(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; }

static void t_int(const char* name, int retbits, int trials = 3000)
{
    auto orig = reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t)>(sym(name));
    LiftFn f = lf(name);
    std::uint64_t mask = retbits == 64 ? ~0ull : (1ull << retbits) - 1;
    int bad = 0;
    for (int t = 0; t < trials; ++t)
    {
        std::uint64_t a = rnd(), b = rnd(), c = rnd(), d = rnd();
        std::uint64_t r0 = orig(a, b, c, d);
        Cpu cpu{}; cpu.r[RCX] = a; cpu.r[RDX] = b; cpu.r[R8] = c; cpu.r[R9] = d;
        run(cpu, f);
        if ((cpu.r[RAX] & mask) != (r0 & mask)) { if (bad++ < 3) std::printf("   %s(%llx,%llx,%llx,%llx): native %llx lifted %llx\n", name, a, b, c, d, r0 & mask, cpu.r[RAX] & mask); }
    }
    report(name, bad, trials);
}
static void t_float(const char* name, int nargs, int trials = 3000)
{
    auto orig = reinterpret_cast<float (*)(float, float, float)>(sym(name));
    LiftFn f = lf(name);
    int bad = 0;
    for (int t = 0; t < trials; ++t)
    {
        float a = rf(), b = rf(), c = rf();
        float r0 = orig(a, b, c);
        Cpu cpu{}; cpu.x[0] = _mm_set_ss(a); cpu.x[1] = _mm_set_ss(b); cpu.x[2] = _mm_set_ss(c);
        run(cpu, f);
        float r1 = _mm_cvtss_f32(cpu.x[0]);
        if (bits(r0) != bits(r1) && !(r0 != r0 && r1 != r1)) { if (bad++ < 3) std::printf("   %s(%g,%g,%g): native %g lifted %g\n", name, a, b, c, r0, r1); }
    }
    (void)nargs;
    report(name, bad, trials);
}

int main(int argc, char** argv)
{
    g_mod = LoadLibraryA(argv[1]);
    if (!g_mod) { std::printf("cannot load %s\n", argv[1]); return 2; }
    lifted_register();
    t_int("add3", 32); t_int("mix64", 64); t_int("popc", 32); t_int("gcd32", 32, 800); t_int("sel", 32); t_int("mix8", 32); t_int("sx", 32);
    t_int("cmpops", 32); t_int("u64ops", 64); t_int("mulmix", 64); t_int("div100", 64);
    for (int i = 0; i < 20; ++i)   // switch / jump table, including out-of-range indexes
    {
        auto orig = reinterpret_cast<int (*)(int)>(sym("sw")); Cpu cpu{}; cpu.r[RCX] = std::uint32_t(i - 3); run(cpu, lf("sw"));
        if (std::uint32_t(cpu.r[RAX]) != std::uint32_t(orig(i - 3))) { std::printf("   sw(%d) mismatch\n", i - 3); ++g_fail; }
    }
    ++g_total; std::printf("sw       done\n");
    t_int("callit", 32); t_int("helper", 32);
    t_float("fmix", 2); t_float("fclamp", 3); t_float("fconst", 1);
    { // table lookup through an embedded data window
        auto orig = reinterpret_cast<float (*)(int)>(sym("tab")); int bad = 0;
        for (int i = -4; i < 20; ++i) { Cpu cpu{}; cpu.r[RCX] = std::uint32_t(i); run(cpu, lf("tab")); if (bits(_mm_cvtss_f32(cpu.x[0])) != bits(orig(i))) ++bad; }
        report("tab", bad, 24);
    }
    { // int -> float and float -> int
        auto o1 = reinterpret_cast<int (*)(float)>(sym("f2i")); auto o2 = reinterpret_cast<float (*)(int, int)>(sym("i2f")); int bad = 0;
        for (int t = 0; t < 2000; ++t)
        {
            float x = rf(); Cpu c1{}; c1.x[0] = _mm_set_ss(x); run(c1, lf("f2i")); if (std::uint32_t(c1.r[RAX]) != std::uint32_t(o1(x))) ++bad;
            int a = int(rng()), b = int(rng() >> 40); Cpu c2{}; c2.r[RCX] = std::uint32_t(a); c2.r[RDX] = std::uint32_t(b); run(c2, lf("i2f"));
            if (bits(_mm_cvtss_f32(c2.x[0])) != bits(o2(a, b))) ++bad;
        }
        report("f2i/i2f", bad, 4000);
    }
    { // double precision
        auto orig = reinterpret_cast<double (*)(double, double)>(sym("dmix")); int bad = 0;
        for (int t = 0; t < 2000; ++t)
        {
            double a = double(rf()), b = t % 7 ? double(rf()) : 0.0; Cpu cpu{}; cpu.x[0] = _mm_castpd_ps(_mm_set_sd(a)); cpu.x[1] = _mm_castpd_ps(_mm_set_sd(b));
            run(cpu, lf("dmix")); double r = _mm_cvtsd_f64(_mm_castps_pd(cpu.x[0])); double e = orig(a, b);
            if (std::memcmp(&r, &e, 8) && !(r != r && e != e)) ++bad;
        }
        report("dmix", bad, 2000);
    }
    { // packed float dot product
        auto orig = reinterpret_cast<float (*)(const float*, const float*)>(sym("dot4")); int bad = 0;
        for (int t = 0; t < 2000; ++t)
        {
            alignas(16) float a[4], b[4]; for (int i = 0; i < 4; ++i) { a[i] = rf(); b[i] = rf(); }
            Cpu cpu{}; cpu.r[RCX] = (std::uint64_t)a; cpu.r[RDX] = (std::uint64_t)b; run(cpu, lf("dot4"));
            if (bits(_mm_cvtss_f32(cpu.x[0])) != bits(orig(a, b))) ++bad;
        }
        report("dot4", bad, 2000);
    }
    { // struct array walk with mixed int/float
        struct S { int a; int b; float f; };
        auto orig = reinterpret_cast<float (*)(const S*, int)>(sym("sums")); int bad = 0;
        for (int t = 0; t < 500; ++t)
        {
            S arr[24]; int n = int(rng() % 24); for (auto& s : arr) { s.a = int(rng() % 200) - 100; s.b = int(rng() % 200) - 100; s.f = rf(); }
            Cpu cpu{}; cpu.r[RCX] = (std::uint64_t)arr; cpu.r[RDX] = std::uint32_t(n); run(cpu, lf("sums"));
            if (bits(_mm_cvtss_f32(cpu.x[0])) != bits(orig(arr, n))) ++bad;
        }
        report("sums", bad, 500);
    }
    { // memory writers (vectorised loops, copies)
        auto o1 = reinterpret_cast<void (*)(int*, int, int)>(sym("fill")); auto o2 = reinterpret_cast<void (*)(int*, const int*)>(sym("copy16")); int bad = 0;
        for (int t = 0; t < 500; ++t)
        {
            int n = int(rng() % 60), v = int(rng()); int x[64] = {}, y[64] = {};
            o1(x, n, v); Cpu cpu{}; cpu.r[RCX] = (std::uint64_t)y; cpu.r[RDX] = std::uint32_t(n); cpu.r[R8] = std::uint32_t(v); run(cpu, lf("fill"));
            if (std::memcmp(x, y, sizeof x)) ++bad;
            int s[16], d1[16], d2[16]; for (int& e : s) e = int(rng()); o2(d1, s);
            Cpu c2{}; c2.r[RCX] = (std::uint64_t)d2; c2.r[RDX] = (std::uint64_t)s; run(c2, lf("copy16")); if (std::memcmp(d1, d2, sizeof d1)) ++bad;
        }
        report("fill/copy", bad, 1000);
    }
    { // native call with mixed int/float args and a float result (the thunk)
        auto orig = reinterpret_cast<float (*)(int, float)>(sym("callnat")); int bad = 0;
        for (int t = 0; t < 1000; ++t)
        {
            int x = int(rng() % 1000) - 500; float y = rf(); Cpu cpu{}; cpu.r[RCX] = std::uint32_t(x); cpu.x[1] = _mm_set_ss(y); run(cpu, lf("callnat"));
            if (bits(_mm_cvtss_f32(cpu.x[0])) != bits(orig(x, y))) ++bad;
        }
        report("callnat", bad, 1000);
    }
    std::printf("\n%d test groups, %d failed\n", g_total, g_fail);
    return g_fail != 0;
}