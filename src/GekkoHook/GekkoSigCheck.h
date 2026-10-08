// GekkoSigCheck.h 25/09/2026. One prologue check for every signature in GekkoSigs.h.
#pragma once
#include "GekkoSigs.h"

namespace GekkoSig
{
    // Compares the live bytes at sig.address with sig.bytes/mask. A prologue that starts with E9 (jmp rel32, a
    // detour another mod installed) is accepted when bytes 5.. still match; the jump target and the module that
    // owns it are logged. Logs every result with `tag`. Returns the live address, or nullptr on mismatch.
    void* Check(const GekkoSigs::FuncSig& sig, const char* tag);

    // Same, looked up by name. Returns nullptr if the name is not in GekkoSigs.h.
    void* CheckByName(const char* name, const char* tag);
}
