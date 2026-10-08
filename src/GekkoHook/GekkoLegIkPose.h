// GekkoLegIkPose.h 07/10/2026. Reads the engine's RigDef / RigPose tables for the ANIMAL_LEG units and moves a foot target (no engine calls, headless testable).
// Layouts from the decomp of fox::anim::RigPose::TranslatePose (AnimRigPose.cpp) and RigAnimalLegDef::TranslatePose (AnimRigAnimalLeg.cpp), retail Exec 0x140b82d60 for the pointers.
#pragma once
#include <cstdint>
#include <cstring>

namespace GekkoLegIkPose
{
    constexpr std::int32_t kAnimalLeg = 10;   // RigDef unit type (case 10 of the RigPose::TranslatePose switch)

    struct Leg { unsigned index; std::uint8_t* unit; };

    // RigDef: unit count dword at +0xc, unit def offsets (dword, 0 = none) from +0x20, def +0 = type. RigPose: offset of the unit offset table dword at +8, unit count word at +0xc, unit offsets are
    // relative to the pose. Returns the ANIMAL_LEG pose units (each pose unit once) in unit index order.
    inline unsigned FindLegs(const std::uint8_t* rigDef, std::uint8_t* pose, Leg* out, unsigned max)
    {
        const unsigned defCount = *reinterpret_cast<const std::uint32_t*>(rigDef + 0xc);
        const unsigned poseCount = *reinterpret_cast<const std::uint16_t*>(pose + 0xc);
        const std::uint32_t tabOff = *reinterpret_cast<const std::uint32_t*>(pose + 8);
        if (!tabOff || defCount > 512) return 0;
        const std::uint32_t* tab = reinterpret_cast<const std::uint32_t*>(pose + tabOff);
        unsigned n = 0;
        for (unsigned i = 0; i < defCount && i < poseCount && n < max; ++i)
        {
            const std::uint32_t dOff = *reinterpret_cast<const std::uint32_t*>(rigDef + 0x20 + 4 * i);
            if (!dOff || !tab[i] || *reinterpret_cast<const std::int32_t*>(rigDef + dOff) != kAnimalLeg) continue;
            std::uint8_t* unit = pose + tab[i];
            bool seen = false;
            for (unsigned k = 0; k < n; ++k) seen |= out[k].unit == unit;
            if (!seen) out[n++] = { i, unit };
        }
        return n;
    }

    // The pose unit's foot target is a vec4 at +0x40 with its copy at +0x30; TranslatePose adds the vector to the first and writes the sum to both.
    inline float* FootTarget(std::uint8_t* unit) { return reinterpret_cast<float*>(unit + 0x40); }
    inline void Translate(std::uint8_t* unit, const float d[3])
    {
        float* t = FootTarget(unit);
        for (int k = 0; k < 3; ++k) t[k] += d[k];
        std::memcpy(unit + 0x30, unit + 0x40, 16);
    }
}
