// GekkoSigCheck.cpp 25/09/2026. One prologue check for every signature in GekkoSigs.h.
// Measured: at runtime Block::GetFileForPathId (retail 0x14006D9A0) starts with E9 while the exe on disk
// has 48: another loaded mod detours it. Calling a detoured function still reaches the original through the hook.
#include <Windows.h>
#include <cstring>

#include "GekkoLog.h"
#include "GekkoSigCheck.h"

namespace GekkoSig
{
    namespace
    {
        bool SafeRead(const void* p, void* out, size_t n)
        {
            __try
            {
                std::memcpy(out, p, n);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        int FirstMismatch(const GekkoSigs::FuncSig& sig, const std::uint8_t* live, int from)
        {
            for (int i = from; i < static_cast<int>(sizeof(sig.bytes)); ++i)
                if (sig.mask[i] && live[i] != sig.bytes[i])
                    return i;
            return -1;
        }
    }

    void* Check(const GekkoSigs::FuncSig& sig, const char* tag)
    {
        auto* p = static_cast<std::uint8_t*>(ResolveGameAddress(sig.address));
        std::uint8_t live[sizeof(sig.bytes)];
        if (!p || !SafeRead(p, live, sizeof(live)))
        {
            Log("[%s] signature UNREADABLE %s @ %p\n", tag, sig.name, p);
            return nullptr;
        }
        const int bad = FirstMismatch(sig, live, 0);
        if (bad < 0)
        {
            Log("[%s] signature ok %s @ %p\n", tag, sig.name, p);
            return p;
        }
        if (bad == 0 && live[0] == 0xE9 && FirstMismatch(sig, live, 5) < 0)
        {
            std::int32_t rel;
            std::memcpy(&rel, live + 1, 4);
            const std::uintptr_t target = reinterpret_cast<std::uintptr_t>(p) + 5 + rel;
            HMODULE owner = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(target), &owner) && owner)
                GetModuleFileNameA(owner, name, MAX_PATH);
            Log("[%s] signature ok (hooked) %s @ %p: jmp -> %p in %s, bytes 5..%zu match\n", tag, sig.name, p,
                reinterpret_cast<void*>(target), owner ? name : "(no module: trampoline memory)", sizeof(live) - 1);
            return p;
        }
        Log("[%s] signature MISMATCH %s @ %p byte %d: have %02X want %02X\n", tag, sig.name, p, bad, live[bad], sig.bytes[bad]);
        return nullptr;
    }

    void* CheckByName(const char* name, const char* tag)
    {
        for (const auto& s : GekkoSigs::kFuncs)
            if (std::strcmp(s.name, name) == 0)
                return Check(s, tag);
        Log("[%s] %s missing from GekkoSigs.h (re-run make_gekko_sigs.py)\n", tag, name);
        return nullptr;
    }
}
