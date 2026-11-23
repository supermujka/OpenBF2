// [CHOSEN, not recovered] Low-overhead per-phase server tick profiler for 32v32 performance analysis.
// Zero overhead when sv-perf-profile == 0; pre-allocated static buffers for zero heap allocation.

#ifndef GAME_GAMESERVER_SERVERTICKPROFILER_H
#define GAME_GAMESERVER_SERVERTICKPROFILER_H

#include <chrono>

namespace dice {
namespace hfe {

class ServerTickProfiler
{
public:
    enum Phase
    {
        PHASE_RECV             = 0,
        PHASE_SIM_PLAYERS      = 1,
        PHASE_SIM_OBJECTS      = 2,
        PHASE_SIM_PHYSICS_COLL = 3,
        PHASE_STATE_MASKS      = 4,
        PHASE_GHOST_SCOPE      = 5,
        PHASE_GHOST_PACK       = 6,
        PHASE_NET_SEND         = 7,
        PHASE_PYTHON           = 8,
        PHASE_TOTAL_US         = 9,
        PHASE_COUNT            = 10
    };

    typedef std::chrono::steady_clock::time_point TimePoint;

    static int* s_enabledCvar;
    static int* s_intervalCvar;
    static bool s_hasPendingSamples;

    static inline bool isEnabled()
    {
        return s_enabledCvar != 0 && (*s_enabledCvar != 0);
    }

    static inline bool hasPendingSamples()
    {
        return s_hasPendingSamples;
    }

    static inline TimePoint now()
    {
        return std::chrono::steady_clock::now();
    }

    static inline int elapsedUs(const TimePoint& start, const TimePoint& end)
    {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }

    // Lifecycle hooks in BF2Engine::mainLoop() and GameServer::update().
    static void beginTick();
    static void noteServerUpdate(int connectionCount);
    static void addPhaseUs(Phase phase, int microseconds);
    static void noteDroppedSteps(int count);
    static void notePacketSent(unsigned int bytes, int ghosts);
    static void endTick();
    static void flushAndReset();
};

} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_SERVERTICKPROFILER_H
