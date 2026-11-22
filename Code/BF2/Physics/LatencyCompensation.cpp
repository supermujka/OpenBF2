// Latency configuration and query helpers. [INFERRED] Source path.

#include "LatencyCompensation.h"
#include "../Game/Objects/IPlayer.h"
#include "../Game/Utility/Vars.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace dice {
namespace hfe {

// The two renderer words - dice::hfe, the namespace BF2Render::render already writes them in. See
// the header for why they are defined here and not in BF2Render.cpp.
// A dedicated image never writes either one - it has no renderer - so both keep the zero below,
// which is what makes getLocalLatency's render-ahead term vanish there even when the cvar is on.
float rendererInverseFrameTime = 0.0f;   // ds:0xA07E50, BF2.exe 0x4534B7
int   rendererFrameCount = 0;            // ds:0xA07E54, BF2.exe 0x4534CA

namespace world {

// ds:0x898BA84 - the shipped static-init stores 1: lag compensation is on by default.
bool LatencyCompensation::m_isEnabled = true;

namespace {
// _Z41__static_initialization_and_destruction_0ii_378 @ 0x835C6B0 registers the
// complete latency-compensation family in this order.  Keeping the otherwise-unread handles here
// preserves the shipped console surface while getLatency uses the five marked below.
int*   const g_useClientWindow = Vars::getInt("phy-latencyComp-useClientWindow", 1); // +0x450, used
int*   const g_useInput = Vars::getInt("phy-latencyComp-useInput", 1);               // +0x454
int*   const g_useRenderAhead = Vars::getInt("phy-latencyComp-useRenderAhead", 0);   // +0x458
float* const g_maxRenderAheadTime =
    Vars::getFloat("phy-latencyComp-maxRenderAheadTime", 0.033333335f);               // +0x45C
float* const g_pingFactor = Vars::getFloat("phy-latencyComp-pingFactor", 1.0f);       // +0x460, used
float* const g_defaultLatency = Vars::getFloat("phy-latencyComp-default", 0.05f);    // +0x464, used
float* const g_maxLatency = Vars::getFloat("phy-latencyComp-max", 0.35f);            // +0x468, used
float* const g_debugLatency = Vars::getFloat("phy-latencyComp-debug", -1.0f);        // +0x46C, used

// [CHOSEN, not recovered] Version 1 hit-detection & lag-compensation cvars and sidecars.
int*   const g_improved           = Vars::getInt("phy-latencyComp-improved", 1);
int*   const g_maxClientInterpMs  = Vars::getInt("phy-latencyComp-maxClientInterpMs", 133);
int*   const g_useQueueDelay      = Vars::getInt("phy-latencyComp-queueDelay", 1);
int*   const g_maxQueueTicks      = Vars::getInt("phy-latencyComp-maxQueueTicks", 5);
int*   const g_snapshotProjectile = Vars::getInt("phy-latencyComp-snapshotProjectile", 1);
int*   const g_closestBone        = Vars::getInt("phy-latencyComp-closestBone", 0);
int*   const g_spawnRingFix       = Vars::getInt("phy-latencyComp-spawnRingFix", 1);
int*   const g_boneHistory        = Vars::getInt("phy-latencyComp-boneHistory", 1);
int*   const g_qDelayMinInterpMs  = Vars::getInt("phy-latencyComp-qDelayMinInterpMs", 100);
float* const g_broadphaseFloor    = Vars::getFloat("phy-latencyComp-broadphaseFloor", 8.0f);
int*   const g_debugLog           = Vars::getInt("phy-latencyComp-debugLog", 0);

struct ActionArrival
{
    int inputIndex;
    int arrivalTick;
};

struct PlayerQueueState
{
    std::vector<ActionArrival> arrivals;
    float activeDelaySec;
    PlayerQueueState() : activeDelaySec(0.0f) {}
};

std::unordered_map<const IPlayer*, PlayerQueueState>& playerQueues()
{
    static std::unordered_map<const IPlayer*, PlayerQueueState>* const map =
        new std::unordered_map<const IPlayer*, PlayerQueueState>();
    return *map;
}

std::unordered_map<const IObject*, float>& projectileLatencies()
{
    static std::unordered_map<const IObject*, float>* const map =
        new std::unordered_map<const IObject*, float>();
    return *map;
}

const float kServerTickPeriod = 0.033333335f;
const float kRetailPingFactor = 0.5f;
const float kRetailMaxLatency = 0.3f;
}

// one load of the cvar handle registered at static init, then its float.
float LatencyCompensation::getMaxLatency()
{
    return (*g_improved != 0) ? *g_maxLatency : kRetailMaxLatency;
}

// Returns local input delay plus optional render-ahead delay; null/remote players return zero.
// Only render-ahead is clamped here. [PLATFORM] Preserves ELF's addition to initial zero,
// which can differ from the PE direct load for negative zero.
float LatencyCompensation::getLocalLatency(IPlayer* player)
{
    if (!player || player->getIsRemote())               // ELF +0x5C / PE +0x58
        return 0.0f;

    float latency = 0.0f;
    if (*g_useInput)
        latency += io::inputSampleLatency;              // ds:0x8ABA440 / 0xA07E4C

    if (!*g_useRenderAhead)
        return latency;

    float renderAhead =
        static_cast<float>(rendererFrameCount) * rendererInverseFrameTime;
    if (renderAhead > *g_maxRenderAheadTime)
        renderAhead = *g_maxRenderAheadTime;
    return latency + renderAhead;
}

// Returns zero when disabled; otherwise adds default latency and clamps only the upper
// bound. Negative results remain negative. Unlike getLatency, debug latency cannot
// override the disabled state here.
float LatencyCompensation::convertLatency(float latency)
{
    if (!m_isEnabled)
        return 0.0f;

    latency += *g_defaultLatency;
    const float maxLat = getMaxLatency();
    if (latency > maxLat)
        return maxLat;
    return latency;
}

// Returns player compensation with an upper bound only; negative results remain possible.
// Positive debug latency overrides disabled state. Otherwise remote players use ping/interpolation
// and local players use getLocalLatency; disabled or null players return zero.
float LatencyCompensation::getLatency(IPlayer* player)
{
    float latency = *g_debugLatency;
    if (latency <= 0.0f)
    {
        latency = 0.0f;
        if (!m_isEnabled || !player)
            return latency;

        if (player->getIsRemote())
        {
            latency = static_cast<float>(player->getPing()) * getPingFactor() * 0.001f;
            if (*g_useClientWindow)
            {
                const unsigned int interpMs =
                    clampClientInterpolationTime(player->getInterpolationTime());
                latency += static_cast<float>(interpMs) * 0.001f;
            }
            else
                latency += *g_defaultLatency;
        }
        else
        {
            latency = getLocalLatency(player);                    // written above
        }
    }

    const float maxLat = getMaxLatency();
    if (latency > maxLat)
        return maxLat;
    return latency;
}

// [CHOSEN, not recovered] Feature-gate and sidecar helpers for Version 1 hitreg fixes.
bool LatencyCompensation::isImprovedEnabled()
{
    return *g_improved != 0;
}

int LatencyCompensation::getClosestBoneMode()
{
    return (*g_improved != 0) ? *g_closestBone : 0;
}

bool LatencyCompensation::isClosestBoneEnabled()
{
    return getClosestBoneMode() != 0;
}

bool LatencyCompensation::isSpawnRingFixEnabled()
{
    return *g_improved != 0 && *g_spawnRingFix != 0;
}

bool LatencyCompensation::isBoneHistoryEnabled()
{
    return *g_improved != 0 && *g_boneHistory != 0;
}

bool LatencyCompensation::isDebugLogEnabled()
{
    return *g_debugLog != 0;
}

bool LatencyCompensation::isQueueDelayEnabled()
{
    return *g_improved != 0 && *g_useQueueDelay != 0;
}

bool LatencyCompensation::isSnapshotProjectileEnabled()
{
    return *g_improved != 0 && *g_snapshotProjectile != 0;
}

bool LatencyCompensation::isUseClientWindowEnabled()
{
    return *g_useClientWindow != 0;
}

float LatencyCompensation::getPingFactor()
{
    return (*g_improved != 0) ? *g_pingFactor : kRetailPingFactor;
}

float LatencyCompensation::getDefaultLatency()
{
    return *g_defaultLatency;
}

float LatencyCompensation::getDebugLatency()
{
    return *g_debugLatency;
}

int LatencyCompensation::getMaxClientInterpMs()
{
    return *g_maxClientInterpMs;
}

int LatencyCompensation::getMaxQueueTicks()
{
    return *g_maxQueueTicks;
}

int LatencyCompensation::getQueueDelayMinInterpMs()
{
    return *g_qDelayMinInterpMs;
}

float LatencyCompensation::computeEffectiveQueueDelay(unsigned int rawInterpMs,
                                                      float rawQueueDelaySec)
{
    if (rawQueueDelaySec <= 0.0f)
        return 0.0f;
    const int minMs = *g_qDelayMinInterpMs;
    const int maxMs = *g_maxClientInterpMs;
    if (minMs <= 0 || maxMs <= minMs)
        return rawQueueDelaySec;
    if (static_cast<int>(rawInterpMs) <= minMs)
        return 0.0f;
    if (static_cast<int>(rawInterpMs) >= maxMs)
        return rawQueueDelaySec;
    const float scale = static_cast<float>(static_cast<int>(rawInterpMs) - minMs)
                      / static_cast<float>(maxMs - minMs);
    return rawQueueDelaySec * scale;
}

unsigned int LatencyCompensation::clampClientInterpolationTime(unsigned int ms)
{
    if (ms > 0x200u)
        ms = 0x200u;

    if (m_isEnabled && *g_improved != 0 && *g_maxClientInterpMs > 0)
    {
        const unsigned int cap = static_cast<unsigned int>(*g_maxClientInterpMs);
        if (ms > cap)
            ms = cap;
    }
    return ms;
}

float LatencyCompensation::getBroadphaseSoldierSpeedFloor()
{
    return (*g_improved != 0 && *g_broadphaseFloor > 0.0f) ? *g_broadphaseFloor : 0.0f;
}

void LatencyCompensation::notePlayerActionArrival(const IPlayer* player, int inputIndex,
                                                  int serverTick)
{
    if (!player || !m_isEnabled || *g_improved == 0 || *g_useQueueDelay == 0)
        return;

    std::vector<ActionArrival>& arrivals = playerQueues()[player].arrivals;
    ActionArrival entry;
    entry.inputIndex = inputIndex;
    entry.arrivalTick = serverTick;
    arrivals.push_back(entry);

    if (arrivals.size() > 16)
    {
        const std::ptrdiff_t drop = static_cast<std::ptrdiff_t>(arrivals.size() - 16);
        arrivals.erase(arrivals.begin(), arrivals.begin() + drop);
    }
}

void LatencyCompensation::setPlayerActiveQueueDelay(const IPlayer* player, int inputIndex,
                                                    int consumeTick)
{
    if (!player)
        return;

    std::unordered_map<const IPlayer*, PlayerQueueState>& queues = playerQueues();
    std::unordered_map<const IPlayer*, PlayerQueueState>::iterator it = queues.find(player);
    if (it == queues.end())
        return;

    std::vector<ActionArrival>& arrivals = it->second.arrivals;
    std::size_t writeIdx = 0;
    bool found = false;
    int arrivalTick = 0;

    for (std::size_t i = 0; i < arrivals.size(); ++i)
    {
        if (arrivals[i].inputIndex == inputIndex)
        {
            found = true;
            arrivalTick = arrivals[i].arrivalTick;
        }
        else if (arrivals[i].inputIndex > inputIndex)
        {
            arrivals[writeIdx++] = arrivals[i];
        }
    }
    arrivals.resize(writeIdx);

    if (found && m_isEnabled && *g_improved != 0 && *g_useQueueDelay != 0)
    {
        int waitedTicks = consumeTick - arrivalTick;
        if (waitedTicks < 0)
            waitedTicks = 0;
        const int maxTicks = *g_maxQueueTicks > 0 ? *g_maxQueueTicks : 0;
        if (waitedTicks > maxTicks)
            waitedTicks = maxTicks;
        it->second.activeDelaySec = static_cast<float>(waitedTicks) * kServerTickPeriod;
    }
    else
    {
        it->second.activeDelaySec = 0.0f;
    }
}

void LatencyCompensation::clearPlayerActiveQueueDelay(const IPlayer* player)
{
    if (!player)
        return;

    std::unordered_map<const IPlayer*, PlayerQueueState>& queues = playerQueues();
    std::unordered_map<const IPlayer*, PlayerQueueState>::iterator it = queues.find(player);
    if (it != queues.end())
        it->second.activeDelaySec = 0.0f;
}

void LatencyCompensation::forgetPlayer(const IPlayer* player)
{
    if (player)
        playerQueues().erase(player);
}

float LatencyCompensation::getPlayerQueueDelay(const IPlayer* player)
{
    if (!player || !m_isEnabled || *g_improved == 0 || *g_useQueueDelay == 0)
        return 0.0f;

    const std::unordered_map<const IPlayer*, PlayerQueueState>& queues = playerQueues();
    std::unordered_map<const IPlayer*, PlayerQueueState>::const_iterator it = queues.find(player);
    return it != queues.end() ? it->second.activeDelaySec : 0.0f;
}

void LatencyCompensation::snapshotProjectileLatency(const IObject* projectileObject,
                                                    IPlayer* owner)
{
    if (!projectileObject)
        return;

    if (!owner || !m_isEnabled || *g_improved == 0 || *g_snapshotProjectile == 0
        || *g_debugLatency > 0.0f)
    {
        clearProjectileLatency(projectileObject);
        return;
    }

    const float maxLat = getMaxLatency();
    float latency = getLatency(owner);
    if (owner->getIsRemote() && *g_useQueueDelay != 0)
    {
        latency += computeEffectiveQueueDelay(owner->getInterpolationTime(),
                                              getPlayerQueueDelay(owner));
        if (latency > maxLat)
            latency = maxLat;
    }
    projectileLatencies()[projectileObject] = latency;
}

void LatencyCompensation::clearProjectileLatency(const IObject* projectileObject)
{
    if (projectileObject)
        projectileLatencies().erase(projectileObject);
}

float LatencyCompensation::getProjectileLatency(const IObject* projectileObject, IPlayer* owner)
{
    if (*g_debugLatency > 0.0f)
        return getLatency(owner);

    if (!m_isEnabled || !owner)
        return 0.0f;

    if (*g_improved == 0)
        return getLatency(owner);

    const float maxLat = getMaxLatency();
    if (*g_snapshotProjectile != 0 && projectileObject)
    {
        const std::unordered_map<const IObject*, float>& map = projectileLatencies();
        std::unordered_map<const IObject*, float>::const_iterator it = map.find(projectileObject);
        if (it != map.end())
            return it->second > maxLat ? maxLat : it->second;
    }

    float latency = getLatency(owner);
    if (*g_useQueueDelay != 0 && owner->getIsRemote())
    {
        latency += computeEffectiveQueueDelay(owner->getInterpolationTime(),
                                              getPlayerQueueDelay(owner));
        if (latency > maxLat)
            latency = maxLat;
    }
    return latency;
}

} // namespace world
} // namespace hfe
} // namespace dice
