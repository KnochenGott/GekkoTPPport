// lift_rt.cpp 29/09/2026. Registry and failure path of the lifter runtime (lift_rt.h).
#include "lift_rt.h"
#include <windows.h>
#include <unordered_map>
#include <string>

namespace lift
{
    static std::unordered_map<std::uint64_t, LiftFn>& reg()
    {
        static std::unordered_map<std::uint64_t, LiftFn> m;
        return m;
    }
    void register_fn(std::uint64_t addr, LiftFn fn) { reg()[addr] = fn; }
    LiftFn find_fn(std::uint64_t addr) { auto it = reg().find(addr); return it == reg().end() ? nullptr : it->second; }
    static void* b_memset(void* d, int v, std::size_t n) { return std::memset(d, v, n); }
    static void* b_memcpy(void* d, const void* s, std::size_t n) { return std::memcpy(d, s, n); }
    static void* b_memmove(void* d, const void* s, std::size_t n) { return std::memmove(d, s, n); }
    static int b_memcmp(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n); }
    static std::size_t b_strlen(const char* s) { return std::strlen(s); }

    std::uint64_t import_addr(const char* dll, const char* fn)
    {
        static std::unordered_map<std::string, std::uint64_t> cache;
        const std::string key = std::string(dll) + "!" + fn;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        std::uint64_t a = 0;
        const std::string f = fn;
        if (f == "memset") a = reinterpret_cast<std::uint64_t>(&b_memset);
        else if (f == "memcpy") a = reinterpret_cast<std::uint64_t>(&b_memcpy);
        else if (f == "memmove") a = reinterpret_cast<std::uint64_t>(&b_memmove);
        else if (f == "memcmp") a = reinterpret_cast<std::uint64_t>(&b_memcmp);
        else if (f == "strlen") a = reinterpret_cast<std::uint64_t>(&b_strlen);
        else
        {
            HMODULE m = GetModuleHandleA(dll);
            if (!m) m = LoadLibraryA(dll);
            FARPROC p = m ? GetProcAddress(m, fn[0] == '#' ? MAKEINTRESOURCEA(std::atoi(fn + 1)) : fn) : nullptr;
            a = reinterpret_cast<std::uint64_t>(p);
        }
        if (!a) fatal("unresolved import", 0);
        cache[key] = a;
        return a;
    }

#ifdef LIFT_PC_TRACE
    static std::uint64_t g_ring[4096]; static unsigned g_ringN = 0;
    PcVar& PcVar::operator=(std::uint64_t x) { v = x; g_ring[g_ringN++ & 4095] = x; return *this; }
    PcVar g_pc;
    void pc_ring_dump(int count)
    {
        for (int i = count; i > 0; --i) std::printf(" %llx", static_cast<unsigned long long>(g_ring[(g_ringN - i) & 4095]));
        std::printf("\n");
    }
#else
    std::uint64_t g_pc = 0;
#endif
    void (*g_fatal_log)(const char*, unsigned long long) = nullptr;
    NativeGate g_native_gate = nullptr;
    [[noreturn]] void fatal(const char* what, std::uint64_t a)
    {
        if (g_fatal_log) g_fatal_log(what, a);
        else { std::fprintf(stderr, "lift::fatal: %s (0x%llx)\n", what, static_cast<unsigned long long>(a)); std::fflush(stderr); }
        RaiseException(0xE1F70001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::abort();
    }
}
