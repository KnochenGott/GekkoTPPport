// brain_test.cpp 30/09/2026. Runs GekkoBrain (lifted AI + actions) for N frames without the game. Without a real controller this host plays the clips: it keeps the clip
// clock, answers "motion finished" and fires the .enchnk events (GekkoEvents.h, time = fraction of 65535, not verified unit) that the engine delivers in the game.
// Usage: brain_test.exe [FRAMES [TX TY TZ [HAS_TARGET [SPAWN_SEQUENCE]]]]. Prints every clip the gekko is told to play.
#include <cstdio>
#include <cstdlib>
#include "GekkoBrain.h"
#include "GekkoEvents.h"
#include "GekkoTable.h"
#include "GekkoWorld.h"

static int g_slot = -1;
static float g_t = 0, g_len = 0;

static void Play(std::uint64_t gani, float blend)
{
    g_slot = -1;
    for (int i = 0; i < Gekko::kGaniCount; ++i) if (Gekko::kGaniTable[i] == gani) g_slot = i;
    g_t = 0;
    g_len = g_slot >= 0 ? Gekko::kGaniFrames[g_slot] / 60.0f : 0;
    std::printf("    >> play slot %d (%.2f s) blend %.0f frames\n", g_slot, g_len, blend);
}

static void Clock(float dt)
{
    const float prev = g_t;
    g_t += dt;
    GekkoWorld::SetMotionFinished(g_len > 0 && g_t >= g_len);
    if (g_slot < 0 || g_len <= 0) return;
    const Gekko::EventList& l = Gekko::kEventTable[g_slot];
    const unsigned a = static_cast<unsigned>(prev / g_len * 65535.0f), b = static_cast<unsigned>((g_t > g_len ? g_len : g_t) / g_len * 65535.0f);
    for (int i = 0; i < l.n; ++i)
        if (l.e[i].t > a && l.e[i].t <= b) { std::printf("  [event] %08x at %.2f s\n", l.e[i].hash, g_t); GekkoWorld::FireEvent(0, l.e[i].hash, 0); }
}

static void Trace(const char* l) { std::printf("  %s\n", l); }

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 600;
    GekkoBrain::Input in = { { 0, 0, 0 }, 0.0f, { argc > 2 ? (float)std::atof(argv[2]) : 8.0f, 0.0f, argc > 4 ? (float)std::atof(argv[4]) : 8.0f }, argc > 5 ? std::atoi(argv[5]) != 0 : true };
    GekkoBrain::Game g;
    g.play = Play; g.trace = Trace;
    const bool seq = argc > 6 && std::atoi(argv[6]) != 0;
    if (!GekkoBrain::Start(g, seq)) { std::printf("start failed\n"); return 1; }
    for (int f = 0; f < frames && GekkoBrain::Active(); ++f)
    {
        Clock(1.0f / 60);
        GekkoBrain::Frame(1.0f / 60, in);
        if (f % 60 == 0) std::printf("frame %d: %s\n", f, GekkoBrain::Status());
    }
    return 0;
}
