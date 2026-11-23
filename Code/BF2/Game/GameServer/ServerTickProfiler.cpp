// [CHOSEN, not recovered] Low-overhead per-phase server tick profiler for 32v32 performance analysis.
// Zero overhead when sv-perf-profile == 0; pre-allocated static buffers for zero heap allocation.

#include "ServerTickProfiler.h"
#include "../Utility/Vars.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dice {
namespace hfe {

int* ServerTickProfiler::s_enabledCvar       = Vars::getInt("sv-perf-profile", 0);
int* ServerTickProfiler::s_intervalCvar      = Vars::getInt("sv-perf-profile-interval", 300);
bool ServerTickProfiler::s_hasPendingSamples = false;

namespace {

enum { kMaxWindowSamples = 3600 };

struct PhasePercentiles
{
    int p50;
    int p95;
    int p99;
    int maxVal;
};

bool                         s_tickOpen = false;
bool                         s_hadServerUpdate = false;
ServerTickProfiler::TimePoint s_tickStart;
int                          s_curPhaseUs[ServerTickProfiler::PHASE_COUNT];
int                          s_curConns = 0;

int                          s_sampleCount = 0;
int                          s_samples[ServerTickProfiler::PHASE_COUNT][kMaxWindowSamples];
int                          s_scratch[kMaxWindowSamples];

long long                    s_windowConnsSum = 0;
int                          s_windowDroppedSteps = 0;
int                          s_windowOverruns33ms = 0;
long long                    s_windowPacketsSent = 0;
long long                    s_windowGhostsSent = 0;
long long                    s_windowBytesSent = 0;

int effectiveInterval()
{
    int interval = ServerTickProfiler::s_intervalCvar ? *ServerTickProfiler::s_intervalCvar : 300;
    if (interval < 1)
        interval = 1;
    else if (interval > kMaxWindowSamples)
        interval = kMaxWindowSamples;
    return interval;
}

PhasePercentiles computePercentiles(const int* data, int count)
{
    PhasePercentiles out = { 0, 0, 0, 0 };
    if (count <= 0)
        return out;

    std::memcpy(s_scratch, data, static_cast<std::size_t>(count) * sizeof(int));
    std::sort(s_scratch, s_scratch + count);

    out.p50    = s_scratch[(count - 1) * 50 / 100];
    out.p95    = s_scratch[(count - 1) * 95 / 100];
    out.p99    = s_scratch[(count - 1) * 99 / 100];
    out.maxVal = s_scratch[count - 1];
    return out;
}

void resetWindow()
{
    s_sampleCount        = 0;
    s_windowConnsSum     = 0;
    s_windowDroppedSteps = 0;
    s_windowOverruns33ms = 0;
    s_windowPacketsSent  = 0;
    s_windowGhostsSent   = 0;
    s_windowBytesSent    = 0;
    ServerTickProfiler::s_hasPendingSamples = false;
}

void emitWindowReport()
{
    if (s_sampleCount <= 0)
    {
        resetWindow();
        return;
    }

    PhasePercentiles p[ServerTickProfiler::PHASE_COUNT];
    for (int i = 0; i < ServerTickProfiler::PHASE_COUNT; ++i)
        p[i] = computePercentiles(s_samples[i], s_sampleCount);

    const double avgConns =
        static_cast<double>(s_windowConnsSum) / static_cast<double>(s_sampleCount);
    const double avgGhostsPerPkt = (s_windowPacketsSent > 0)
        ? (static_cast<double>(s_windowGhostsSent) / static_cast<double>(s_windowPacketsSent))
        : 0.0;
    const double avgBytesPerPkt = (s_windowPacketsSent > 0)
        ? (static_cast<double>(s_windowBytesSent) / static_cast<double>(s_windowPacketsSent))
        : 0.0;

    std::fprintf(
        stderr,
        "[SV-PERF] ticks=%d avg_conns=%.1f overruns_33ms=%d dropped_steps=%d "
        "pkts=%lld avg_ghosts_per_pkt=%.1f avg_bytes_per_pkt=%.1f\n"
        "[SV-PERF] us(p50/p95/p99/max) "
        "total_us=%d/%d/%d/%d "
        "recv=%d/%d/%d/%d "
        "sim_players=%d/%d/%d/%d "
        "sim_objects=%d/%d/%d/%d "
        "sim_physics_coll=%d/%d/%d/%d "
        "state_masks=%d/%d/%d/%d "
        "ghost_scope=%d/%d/%d/%d "
        "ghost_pack=%d/%d/%d/%d "
        "net_send=%d/%d/%d/%d "
        "python=%d/%d/%d/%d\n",
        s_sampleCount,
        avgConns,
        s_windowOverruns33ms,
        s_windowDroppedSteps,
        s_windowPacketsSent,
        avgGhostsPerPkt,
        avgBytesPerPkt,
        p[ServerTickProfiler::PHASE_TOTAL_US].p50,
        p[ServerTickProfiler::PHASE_TOTAL_US].p95,
        p[ServerTickProfiler::PHASE_TOTAL_US].p99,
        p[ServerTickProfiler::PHASE_TOTAL_US].maxVal,
        p[ServerTickProfiler::PHASE_RECV].p50,
        p[ServerTickProfiler::PHASE_RECV].p95,
        p[ServerTickProfiler::PHASE_RECV].p99,
        p[ServerTickProfiler::PHASE_RECV].maxVal,
        p[ServerTickProfiler::PHASE_SIM_PLAYERS].p50,
        p[ServerTickProfiler::PHASE_SIM_PLAYERS].p95,
        p[ServerTickProfiler::PHASE_SIM_PLAYERS].p99,
        p[ServerTickProfiler::PHASE_SIM_PLAYERS].maxVal,
        p[ServerTickProfiler::PHASE_SIM_OBJECTS].p50,
        p[ServerTickProfiler::PHASE_SIM_OBJECTS].p95,
        p[ServerTickProfiler::PHASE_SIM_OBJECTS].p99,
        p[ServerTickProfiler::PHASE_SIM_OBJECTS].maxVal,
        p[ServerTickProfiler::PHASE_SIM_PHYSICS_COLL].p50,
        p[ServerTickProfiler::PHASE_SIM_PHYSICS_COLL].p95,
        p[ServerTickProfiler::PHASE_SIM_PHYSICS_COLL].p99,
        p[ServerTickProfiler::PHASE_SIM_PHYSICS_COLL].maxVal,
        p[ServerTickProfiler::PHASE_STATE_MASKS].p50,
        p[ServerTickProfiler::PHASE_STATE_MASKS].p95,
        p[ServerTickProfiler::PHASE_STATE_MASKS].p99,
        p[ServerTickProfiler::PHASE_STATE_MASKS].maxVal,
        p[ServerTickProfiler::PHASE_GHOST_SCOPE].p50,
        p[ServerTickProfiler::PHASE_GHOST_SCOPE].p95,
        p[ServerTickProfiler::PHASE_GHOST_SCOPE].p99,
        p[ServerTickProfiler::PHASE_GHOST_SCOPE].maxVal,
        p[ServerTickProfiler::PHASE_GHOST_PACK].p50,
        p[ServerTickProfiler::PHASE_GHOST_PACK].p95,
        p[ServerTickProfiler::PHASE_GHOST_PACK].p99,
        p[ServerTickProfiler::PHASE_GHOST_PACK].maxVal,
        p[ServerTickProfiler::PHASE_NET_SEND].p50,
        p[ServerTickProfiler::PHASE_NET_SEND].p95,
        p[ServerTickProfiler::PHASE_NET_SEND].p99,
        p[ServerTickProfiler::PHASE_NET_SEND].maxVal,
        p[ServerTickProfiler::PHASE_PYTHON].p50,
        p[ServerTickProfiler::PHASE_PYTHON].p95,
        p[ServerTickProfiler::PHASE_PYTHON].p99,
        p[ServerTickProfiler::PHASE_PYTHON].maxVal);
    std::fflush(stderr);

    resetWindow();
}

} // namespace

void ServerTickProfiler::beginTick()
{
    if (!isEnabled())
    {
        if (s_sampleCount > 0)
            emitWindowReport();
        s_tickOpen = false;
        s_hadServerUpdate = false;
        return;
    }

    s_tickOpen = true;
    s_hadServerUpdate = false;
    s_curConns = 0;
    for (int i = 0; i < PHASE_COUNT; ++i)
        s_curPhaseUs[i] = 0;
    s_tickStart = now();
}

void ServerTickProfiler::noteServerUpdate(int connectionCount)
{
    if (!s_tickOpen)
        return;
    s_hadServerUpdate = true;
    s_curConns = connectionCount;
}

void ServerTickProfiler::addPhaseUs(Phase phase, int microseconds)
{
    if (!s_tickOpen)
        return;
    if (phase >= 0 && phase < PHASE_COUNT && microseconds > 0)
        s_curPhaseUs[phase] += microseconds;
}

void ServerTickProfiler::noteDroppedSteps(int count)
{
    if (!isEnabled() || count <= 0)
        return;
    s_windowDroppedSteps += count;
}

void ServerTickProfiler::notePacketSent(unsigned int bytes, int ghosts)
{
    if (!s_tickOpen)
        return;
    ++s_windowPacketsSent;
    s_windowBytesSent += static_cast<long long>(bytes);
    if (ghosts > 0)
        s_windowGhostsSent += static_cast<long long>(ghosts);
}

void ServerTickProfiler::endTick()
{
    if (!s_tickOpen)
        return;
    s_tickOpen = false;

    if (!s_hadServerUpdate || !isEnabled())
    {
        s_hadServerUpdate = false;
        return;
    }

    const int totalUs = elapsedUs(s_tickStart, now());
    s_curPhaseUs[PHASE_TOTAL_US] = totalUs > 0 ? totalUs : 0;

    if (s_sampleCount < kMaxWindowSamples)
    {
        for (int i = 0; i < PHASE_COUNT; ++i)
            s_samples[i][s_sampleCount] = s_curPhaseUs[i];
        ++s_sampleCount;
        s_hasPendingSamples = true;
    }

    s_windowConnsSum += s_curConns;
    if (totalUs > 33333)
        ++s_windowOverruns33ms;

    if (s_sampleCount >= effectiveInterval())
        emitWindowReport();
}

void ServerTickProfiler::flushAndReset()
{
    s_tickOpen = false;
    s_hadServerUpdate = false;
    if (s_sampleCount > 0)
        emitWindowReport();
    else
        resetWindow();
}

} // namespace hfe
} // namespace dice
