// GekkoBlocks.h 25/09/2026. Finds files inside loaded packs by walking fox's block groups.
#pragma once
#include <cstdint>

namespace GekkoBlocks
{
    // Logs every block group, loaded block and package (package PathId), marking the gekko pack. Returns the number
    // of loaded blocks, -1 on failure.
    int Dump();

    // Asks every loaded block for the file (engine fox::Block::GetFileForPathId). Returns the File* or nullptr.
    void* FindFile(std::uint64_t pathId);
}
