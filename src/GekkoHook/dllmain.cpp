// dllmain.cpp 25/09/2026. Gekko.dll entry; all work starts from luaopen_Gekko (GekkoLua.cpp). Also installs a first-chance exception logger so a crash leaves its address in Gekko.log.
#include <Windows.h>

#include "GekkoLog.h"
#include "lift_rt.h"

namespace
{
    // Logs access violations, stack overflows, heap corruption and illegal instructions with the faulting module+offset, the last Survive pc the lifted code executed and the registers.
    // First-chance: faults the world shim catches on purpose (Guarded) are logged too, which is wanted. Capped so a loop cannot flood the log.
    LONG CALLBACK FaultLogger(EXCEPTION_POINTERS* e)
    {
        const DWORD code = e->ExceptionRecord->ExceptionCode;
        if (code != 0xC0000005 && code != 0xC00000FD && code != 0xC0000374 && code != 0xC0000409 && code != 0xC000001D && code != 0xC0000096) return EXCEPTION_CONTINUE_SEARCH;
        // The game and other plugins fault on purpose all the time (cui.dll, engine probes). Only faults with this DLL on the stack are ours: the lifted code or the shim
        // is running, directly or under a native call it made.
        void* frames[48] = {};
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&FaultLogger), &self);
        unsigned ours = 0;
        for (USHORT i = 1; i < n; ++i)
        {
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &m);
            if (m == self) ++ours;
        }
        if (!ours) return EXCEPTION_CONTINUE_SEARCH;
        static LONG count = 0;
        if (InterlockedIncrement(&count) > 40) return EXCEPTION_CONTINUE_SEARCH;
        const void* at = e->ExceptionRecord->ExceptionAddress;
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &mod)) GetModuleFileNameA(mod, name, MAX_PATH);
        const CONTEXT* c = e->ContextRecord;
        Log("[fault] code %08lx at %p (%s +0x%llx) access %llu addr %p, last survive pc 0x%llx\n", code, at, name, static_cast<unsigned long long>(reinterpret_cast<const char*>(at) - reinterpret_cast<const char*>(mod)),
            static_cast<unsigned long long>(e->ExceptionRecord->NumberParameters > 0 ? e->ExceptionRecord->ExceptionInformation[0] : 0), e->ExceptionRecord->NumberParameters > 1 ? reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1]) : nullptr,
            static_cast<unsigned long long>(lift::g_pc));
        Log("[fault] rax %llx rcx %llx rdx %llx rbx %llx rsp %llx rbp %llx rsi %llx rdi %llx r8 %llx r9 %llx\n", c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9);
        for (USHORT i = 1; i < n && i < 24; ++i)
        {
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &m);
            Log("[fault]   #%u %p %s+0x%llx\n", i, frames[i], m == self ? "Gekko.dll" : (m == GetModuleHandleA(nullptr) ? "mgsvtpp.exe" : "other"), static_cast<unsigned long long>(reinterpret_cast<const char*>(frames[i]) - reinterpret_cast<const char*>(m)));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        AddVectoredExceptionHandler(1, FaultLogger);
        Log("==== Gekko.dll loaded at %p from %s, exe at %p\n", module, path, GetModuleHandleA(nullptr));
    }
    return TRUE;
}
