// lift_rt.h 29/09/2026. Runtime for C++ emitted by tools/lift/lift.py: the emulated x64 register file, flag helpers, the
// native-call thunk and the token registry for lifted functions. Design and limits: docs/DOCUMENTATION.md.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <emmintrin.h>
#include <xmmintrin.h>
#include <type_traits>

namespace lift
{
    enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

    // Register file. Offsets are used by lift_native_call.asm: r at 0, x at 128.
    struct alignas(16) Cpu
    {
        std::uint64_t r[16];
        __m128 x[16];
    };

    struct Fl { bool ZF = false, SF = false, CF = false, OF = false, PF = false; };

    using LiftFn = void (*)(Cpu&);
    constexpr std::uint64_t kTokenBase = 0x00007A0000000000ull;   // Survive code addresses become tokens (TPP shares the VA range)

    void register_fn(std::uint64_t addr, LiftFn fn);
    LiftFn find_fn(std::uint64_t addr);
#ifdef LIFT_PC_TRACE   // headless tests: every pc write also lands in a ring (pc_ring_dump), to see which way a lifted function went
    struct PcVar
    {
        std::uint64_t v = 0;
        PcVar& operator=(std::uint64_t x);
        operator std::uint64_t() const { return v; }
    };
    extern PcVar g_pc;
    void pc_ring_dump(int count);
#else
    extern std::uint64_t g_pc;   // current Survive address (only written by code lifted with --pc)
#endif
    extern void (*g_fatal_log)(const char*, unsigned long long);   // optional: called before fatal raises
    [[noreturn]] void fatal(const char* what, std::uint64_t a);   // raises SEH 0xE1F70001 (catch with __try/__except)

    std::uint64_t import_addr(const char* dll, const char* fn);   // address of an imported function in this process (memset etc. built in)
    extern "C" void lift_native_call(Cpu* c, std::uint64_t target);   // lift_native_call.asm

    // Pushes the fake return address of an emulated call; the callee's ret pops it.
    inline void push_ret(Cpu& c) { c.r[RSP] -= 8; *reinterpret_cast<std::uint64_t*>(c.r[RSP]) = 0xDEADC0DEull; }

    // Optional gate on every call that leaves the lifted code for a real function: return true if the call was handled (or refused) here.
    // A port uses it to allow only twins it has verified: Survive's own function addresses are TPP addresses too, and a twin picked by
    // prologue similarity can be the wrong function.
    using NativeGate = bool (*)(Cpu&, std::uint64_t target);
    extern NativeGate g_native_gate;
    inline void call_native(Cpu& c, std::uint64_t target)
    {
        if (g_native_gate && g_native_gate(c, target)) return;
        lift_native_call(&c, target);
    }

    inline void call_ptr(Cpu& c, std::uint64_t target)
    {
#ifdef LIFT_TRACE
        std::fprintf(stderr, "call_ptr %llx (rcx %llx rdx %llx r8 %llx)\n", (unsigned long long)target, (unsigned long long)c.r[RCX], (unsigned long long)c.r[RDX], (unsigned long long)c.r[R8]);
#endif
        if (target >= kTokenBase && target < kTokenBase + 0x200000000ull)
        {
            LiftFn f = find_fn(target - kTokenBase);
            if (!f) fatal("call to a Survive function that was not lifted", target - kTokenBase);
            push_ret(c);
            f(c);
            return;
        }
        call_native(c, target);
    }

    // Runs a lifted function with a private stack: args already placed in c.r / c.x.
    inline void run(Cpu& c, LiftFn f)
    {
        alignas(64) static std::uint8_t stack[1 << 16];
        c.r[RSP] = (reinterpret_cast<std::uint64_t>(stack) + sizeof(stack) - 128) & ~15ull;
        push_ret(c);   // entry rsp = 8 mod 16, as after a real call
        f(c);
    }

    // ---- sub-register writes (reads are plain casts in the emitted code)
    inline void wr32(Cpu& c, int i, std::uint32_t v) { c.r[i] = v; }   // 32-bit writes zero-extend
    inline void wr16(Cpu& c, int i, std::uint16_t v) { c.r[i] = (c.r[i] & ~0xFFFFull) | v; }
    inline void wr8(Cpu& c, int i, std::uint8_t v) { c.r[i] = (c.r[i] & ~0xFFull) | v; }
    inline void wr8h(Cpu& c, int i, std::uint8_t v) { c.r[i] = (c.r[i] & ~0xFF00ull) | (std::uint64_t(v) << 8); }

    template <class T> inline T& mem(std::uint64_t a) { return *reinterpret_cast<T*>(a); }

    // ---- flags. T is the unsigned operand type.
    template <class T> inline constexpr int bits() { return int(sizeof(T) * 8); }
    template <class T> inline bool msb(T v) { return (v >> (bits<T>() - 1)) & 1; }
    template <class T> inline void szp(Fl& f, T r)
    {
        f.ZF = r == 0; f.SF = msb(r);
        f.PF = !(__popcnt(unsigned(r) & 0xFFu) & 1);
    }
    template <class T> inline T add(Fl& f, T a, T b) { T r = T(a + b); szp(f, r); f.CF = r < a; f.OF = msb(T((a ^ r) & (b ^ r))); return r; }
    template <class T> inline T adc(Fl& f, T a, T b) { bool c = f.CF; T r = T(a + b + c); szp(f, r); f.CF = c ? r <= a : r < a; f.OF = msb(T((a ^ r) & (b ^ r))); return r; }
    template <class T> inline T sub(Fl& f, T a, T b) { T r = T(a - b); szp(f, r); f.CF = a < b; f.OF = msb(T((a ^ b) & (a ^ r))); return r; }
    template <class T> inline T sbb(Fl& f, T a, T b) { bool c = f.CF; T r = T(a - b - c); szp(f, r); f.CF = c ? a <= b : a < b; f.OF = msb(T((a ^ b) & (a ^ r))); return r; }
    template <class T> inline T logic(Fl& f, T r) { szp(f, r); f.CF = false; f.OF = false; return r; }
    template <class T> inline T inc(Fl& f, T a) { T r = T(a + 1); szp(f, r); f.OF = r == T(T(1) << (bits<T>() - 1)); return r; }
    template <class T> inline T dec(Fl& f, T a) { T r = T(a - 1); szp(f, r); f.OF = a == T(T(1) << (bits<T>() - 1)); return r; }
    template <class T> inline T neg(Fl& f, T a) { T r = T(T(0) - a); szp(f, r); f.CF = a != 0; f.OF = a == T(T(1) << (bits<T>() - 1)); return r; }
    template <class T> inline T shl(Fl& f, T a, unsigned n)
    {
        n &= sizeof(T) == 8 ? 63u : 31u;
        if (!n) return a;
        T r = n >= unsigned(bits<T>()) ? T(0) : T(a << n);
        f.CF = n <= unsigned(bits<T>()) ? bool((a >> (bits<T>() - n)) & 1) : false; szp(f, r); f.OF = msb(r) != f.CF; return r;
    }
    template <class T> inline T shr(Fl& f, T a, unsigned n)
    {
        n &= sizeof(T) == 8 ? 63u : 31u;
        if (!n) return a;
        T r = n >= unsigned(bits<T>()) ? T(0) : T(a >> n);
        f.CF = (a >> (n - 1)) & 1; szp(f, r); f.OF = msb(a); return r;
    }
    template <class T> inline T sar(Fl& f, T a, unsigned n)
    {
        using S = std::make_signed_t<T>;
        n &= sizeof(T) == 8 ? 63u : 31u;
        if (!n) return a;
        T r = T(S(a) >> n);
        f.CF = (S(a) >> (n - 1)) & 1; szp(f, r); f.OF = false; return r;
    }
    template <class T> inline T rol(Fl& f, T a, unsigned n)
    {
        n &= sizeof(T) == 8 ? 63u : 31u; n %= unsigned(bits<T>());
        if (!n) return a;
        T r = T((a << n) | (a >> (bits<T>() - n)));
        f.CF = r & 1; f.OF = msb(r) != f.CF; return r;
    }
    template <class T> inline T ror(Fl& f, T a, unsigned n)
    {
        n &= sizeof(T) == 8 ? 63u : 31u; n %= unsigned(bits<T>());
        if (!n) return a;
        T r = T((a >> n) | (a << (bits<T>() - n)));
        f.CF = msb(r); f.OF = msb(r) != bool((r >> (bits<T>() - 2)) & 1); return r;
    }
    // signed multiply, low half; CF = OF = the full product does not fit
    inline std::uint64_t imul64(Fl& f, std::uint64_t a, std::uint64_t b)
    {
        std::int64_t hi; std::int64_t lo = _mul128(std::int64_t(a), std::int64_t(b), &hi);
        f.CF = f.OF = hi != (lo >> 63); return std::uint64_t(lo);
    }
    inline std::uint32_t imul32(Fl& f, std::uint32_t a, std::uint32_t b)
    {
        std::int64_t p = std::int64_t(std::int32_t(a)) * std::int32_t(b);
        f.CF = f.OF = p != std::int64_t(std::int32_t(p)); return std::uint32_t(p);
    }
    inline std::uint16_t imul16(Fl& f, std::uint16_t a, std::uint16_t b)
    {
        std::int32_t p = std::int32_t(std::int16_t(a)) * std::int16_t(b);
        f.CF = f.OF = p != std::int32_t(std::int16_t(p)); return std::uint16_t(p);
    }

    // ---- conditions
    inline bool cc_e(const Fl& f) { return f.ZF; }
    inline bool cc_ne(const Fl& f) { return !f.ZF; }
    inline bool cc_b(const Fl& f) { return f.CF; }
    inline bool cc_ae(const Fl& f) { return !f.CF; }
    inline bool cc_be(const Fl& f) { return f.CF || f.ZF; }
    inline bool cc_a(const Fl& f) { return !f.CF && !f.ZF; }
    inline bool cc_l(const Fl& f) { return f.SF != f.OF; }
    inline bool cc_ge(const Fl& f) { return f.SF == f.OF; }
    inline bool cc_le(const Fl& f) { return f.ZF || f.SF != f.OF; }
    inline bool cc_g(const Fl& f) { return !f.ZF && f.SF == f.OF; }
    inline bool cc_s(const Fl& f) { return f.SF; }
    inline bool cc_ns(const Fl& f) { return !f.SF; }
    inline bool cc_p(const Fl& f) { return f.PF; }
    inline bool cc_np(const Fl& f) { return !f.PF; }
    inline bool cc_o(const Fl& f) { return f.OF; }
    inline bool cc_no(const Fl& f) { return !f.OF; }

    // comiss / ucomiss / comisd: unordered 111, less CF, equal ZF
    inline void comi(Fl& f, double a, double b)
    {
        f.OF = f.SF = false;
        if (a != a || b != b) { f.ZF = f.PF = f.CF = true; return; }
        f.PF = false; f.ZF = a == b; f.CF = a < b;
    }
    inline __m128 loadss(std::uint64_t a) { return _mm_load_ss(reinterpret_cast<const float*>(a)); }
    inline __m128 loadps(std::uint64_t a) { return _mm_loadu_ps(reinterpret_cast<const float*>(a)); }
    inline void storeps(std::uint64_t a, __m128 v) { _mm_storeu_ps(reinterpret_cast<float*>(a), v); }
    inline void storess(std::uint64_t a, __m128 v) { _mm_store_ss(reinterpret_cast<float*>(a), v); }
    inline __m128i ci(__m128 v) { return _mm_castps_si128(v); }
    inline __m128 cf(__m128i v) { return _mm_castsi128_ps(v); }
}

#define LIFT_UNSUPPORTED(addr, text) ::lift::fatal("unsupported instruction " text, addr)