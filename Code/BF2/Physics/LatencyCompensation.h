// Shared latency-compensation controls and player-delay queries; header path [INFERRED].
// m_isEnabled defaults true and gates per-frame history publication.

#ifndef PHYSICS_LATENCYCOMPENSATION_H     // [INFERRED] guard style
#define PHYSICS_LATENCYCOMPENSATION_H

namespace dice {
namespace hfe {
namespace world {

class IObject;
class IPlayer;

class LatencyCompensation
{
public:
    static bool  m_isEnabled;                        // ds:0x898BA84 - static-init stores 1
    // Latency limits and local-player timing queries.
    static float getMaxLatency();
    static float getLocalLatency(IPlayer* player);
    static float getLatency(IPlayer* player);
    static float convertLatency(float latency);

    // [CHOSEN, not recovered] Version 1 hit-detection & lag-compensation controls and sidecars.
    static bool isImprovedEnabled();
    static bool isClosestBoneEnabled();
    static int getClosestBoneMode();
    static bool isSpawnRingFixEnabled();
    static bool isBoneHistoryEnabled();
    static bool isDebugLogEnabled();
    static bool isQueueDelayEnabled();
    static bool isSnapshotProjectileEnabled();
    static bool isUseClientWindowEnabled();
    static float getPingFactor();
    static float getDefaultLatency();
    static float getDebugLatency();
    static int getMaxClientInterpMs();
    static int getMaxQueueTicks();
    static int getQueueDelayMinInterpMs();
    static float computeEffectiveQueueDelay(unsigned int rawInterpMs, float rawQueueDelaySec);
    static unsigned int clampClientInterpolationTime(unsigned int ms);
    static float getBroadphaseSoldierSpeedFloor();
    static void notePlayerActionArrival(const IPlayer* player, int inputIndex, int serverTick);
    static void setPlayerActiveQueueDelay(const IPlayer* player, int inputIndex, int consumeTick);
    static void clearPlayerActiveQueueDelay(const IPlayer* player);
    static void forgetPlayer(const IPlayer* player);
    static float getPlayerQueueDelay(const IPlayer* player);
    static void snapshotProjectileLatency(const IObject* projectileObject, IPlayer* owner);
    static void clearProjectileLatency(const IObject* projectileObject);
    static float getProjectileLatency(const IObject* projectileObject, IPlayer* owner);

    // [CHOSEN, not recovered] Hitreg V3 Phase 1: ActionBuffer queue-drain & drift-free tick controls.
    static bool isQueueDrainEnabled();
    static bool isDriftFreeTickEnabled();
    static int  getQueueDrainMaxTarget();
    static int  getQueueDrainWindow();
    static int  getQueueDrainCooldown();
    static void noteQueueReplay(const IPlayer* player);
    static void noteQueueCapDrop(const IPlayer* player);
    static bool shouldDrainPlayerAction(const IPlayer* player, int prePopDepth, bool canDrop);
    static void getPlayerQueueStats(const IPlayer* player, int& outMinWin, int& outTarget,
                                    int& outDrainDrops, int& outCapDrops);
};

} // namespace world

// [INFERRED] Shared latency inputs written by renderer/network code. Renderer values
// are defined in the shared latency translation unit so dedicated builds also have storage.
extern float rendererInverseFrameTime;   // ds:0xA07E50 / ELF 0x8ABA444 - 1.0f / renderer +0xA0
extern int   rendererFrameCount;         // ds:0xA07E54 / ELF 0x8ABA448 - renderer +0xA8

namespace io {
// ds:0xA07E4C / ELF 0x8ABA440, published by io::getPlayerInput from the fire trigger's sample
// latency. Lives in IO/Input/InputManager.cpp, which both images build.
extern float inputSampleLatency;
} // namespace io

} // namespace hfe
} // namespace dice

#endif // PHYSICS_LATENCYCOMPENSATION_H
