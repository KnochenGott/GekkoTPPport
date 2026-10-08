// GekkoLog.cpp 25/09/2026. Log file (MGS_TPP\Gekko.log, shared by every loaded copy) and game address rebasing.
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>

#include "GekkoLog.h"

namespace
{
    constexpr std::uintptr_t kExeImageBase = 0x140000000;  // preferred base of mgsvtpp.exe

    FILE* OpenLog()
    {
        static FILE* f = nullptr;
        static bool tried = false;
        if (tried)
            return f;
        tried = true;
        // One fixed file next to mgsvtpp.exe, appended and shared: IH's plugin loader and package.loadlib may each
        // load their own copy of the DLL (seen: MGS_TPP\plugins and mod\plugins), and both must log here.
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = std::strrchr(path, '\\');
        if (slash)
            slash[1] = 0;
        strcat_s(path, "Gekko.log");
        // Overwritten once per game start: the first copy of the DLL in the process truncates, the others append (a named mutex marks the process,).
        HANDLE once = CreateMutexA(nullptr, FALSE, "Local\\GekkoLogTruncated");
        const bool first = once && GetLastError() != ERROR_ALREADY_EXISTS;
        f = _fsopen(path, first ? "w" : "a", _SH_DENYNO);
        return f;
    }
}

void Log(const char* fmt, ...)
{
    FILE* f = OpenLog();
    if (!f)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fflush(f);
}

void* ResolveGameAddress(std::uintptr_t absAddr)
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    return base ? reinterpret_cast<void*>(base + (absAddr - kExeImageBase)) : nullptr;
}
