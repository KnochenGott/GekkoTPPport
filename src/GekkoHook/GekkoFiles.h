// GekkoFiles.h 25/09/2026. Debug view of fox::File's global file table (g_files), to see what File::Find can find.
#pragma once
#include <cstdint>

namespace GekkoFiles
{
    // Walks g_files and logs: entry count, count per extension code, every entry whose base code matches a known
    // gekko/control path, and the first `rawCount` raw keys. Returns the entry count, -1 on failure.
    int Dump(int rawCount);

    // Looks for a g_files key whose 50-bit base code equals that of `pathCode` (ignores user flag and extension
    // bits). Returns the exact key or 0.
}
