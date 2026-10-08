// GekkoLegIk.h 07/10/2026. Ground fit for the gekko's ANIMAL_LEG feet, an animation plugin on the gekko's AnimControl (switch, default OFF).
#pragma once

namespace GekkoLegIk
{
    void SetEnabled(bool on);       // the switch; installs the plugin once the controller is live (immediately, or at the next Tick)
    bool Enabled();
    void Tick(void* ctrl);          // GekkoFull::Execute, before the controller stages: installs the plugin when wanted and possible
    void OnDespawn();               // the controller is gone: forget the plugin
    const char* Status();           // one line for Gekko.LegIkStatus
}
