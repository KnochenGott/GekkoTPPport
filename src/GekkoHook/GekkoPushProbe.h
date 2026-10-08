// GekkoPushProbe.h 30/09/2026. Dev probe: does the player's CharaControl human collision query (CollisionHumanCheck) find the gekko's push target? See the .cpp.
#pragma once

namespace GekkoPushProbe
{
    void Run(float halfSpan, int withPushAttr);   // logs to Gekko.log with the [pushprobe] tag
}
