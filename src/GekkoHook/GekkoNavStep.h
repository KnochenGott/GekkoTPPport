// GekkoNavStep.h 07/10/2026. Survive's walk path correction (the locomotion's move step) applied to the body through TPP's VelocityControlAction. Switch: Gekko.NavStep(on), default off.
#pragma once

namespace GekkoNavStep
{
    void SetEnabled(bool on);
    bool Enabled();
    void Apply();   // once per frame after the brain frame: takes the step the locomotion wrote and hands it to the next run frame of the body
}
