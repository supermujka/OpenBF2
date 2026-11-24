#ifndef BF2BOT_BOT_BRAIN_H
#define BF2BOT_BOT_BRAIN_H

#include "Bf2Protocol.h"
#include "NavMesh.h"
#include "RouteLoader.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace bf2bot {

struct TrackedPlayer {
    uint8_t playerId;
    uint16_t playerNetId;
    int32_t team;
    bool isAlive;
    bool isManDown;
    uint8_t spawnGroupId;
    uint8_t kit;
    uint16_t soldierNetId;
    std::string name;

    TrackedPlayer()
        : playerId(0),
          playerNetId(0),
          team(0),
          isAlive(false),
          isManDown(false),
          spawnGroupId(0),
          kit(0),
          soldierNetId(0),
          name() {}
};

// Human-like aiming configuration (CLI: --aim-skill, --interp-ms, --aim-legacy).
struct AimConfig {
    bool legacy;       // true: old nearest-target / newest-position behaviour
    float skill;       // 0..1; scales reaction time, initial error and convergence time
    int32_t interpMs;  // client render delay used to pick the aimed-at position
    bool hasGoal;      // --goal: every bot moves on this world position instead of the flags
    float goalX, goalY, goalZ;
    bool keepRunning = false; // --keep-running: always move, alternating sprint and slow walk
    bool hasSpawnNear; // --spawn-near: spawn at the friendly spawn group closest to this point
    float spawnX, spawnY, spawnZ;

    AimConfig() : legacy(false), skill(0.7f), interpMs(100), hasGoal(false),
                  goalX(0.0f), goalY(0.0f), goalZ(0.0f), hasSpawnNear(false),
                  spawnX(0.0f), spawnY(0.0f), spawnZ(0.0f) {}
};

// One received snapshot position of a remote soldier (receive time in the bot clock).
struct PosSample {
    uint64_t us;
    Vec3 pos;
    PosSample() : us(0), pos() {}
};

struct TrackedSoldier {
    enum { kHistSize = 12 };
    uint16_t netId;
    uint8_t ownerPlayerId;
    int32_t team;
    Vec3 pos;
    Vec3 speed;
    float facingDegrees;
    float lookUpDownAngle;
    uint8_t pose;
    uint8_t hitPointsByte;
    bool isRagdoll;
    bool hasPos;
    uint64_t lastUpdateUs;
    PosSample hist[kHistSize]; // ring buffer of received positions
    uint8_t histCount;
    uint8_t histHead;          // next write slot

    void pushPos(uint64_t us, const Vec3& p) {
        if (histCount > 0u) {
            // Teleport (respawn): drop history so we never interpolate across it.
            const Vec3& l = hist[(histHead + kHistSize - 1u) % kHistSize].pos;
            const float ex = p.x - l.x, ey = p.y - l.y, ez = p.z - l.z;
            if (ex * ex + ey * ey + ez * ez > 25.0f * 25.0f) {
                histCount = 0;
            }
        }
        hist[histHead].us = us;
        hist[histHead].pos = p;
        histHead = static_cast<uint8_t>((histHead + 1u) % kHistSize);
        if (histCount < kHistSize) {
            ++histCount;
        }
    }
    // i = 0 is the oldest retained sample, histCount-1 the newest.
    const PosSample& histAt(size_t i) const {
        return hist[(histHead + kHistSize - histCount + i) % kHistSize];
    }

    TrackedSoldier()
        : netId(0),
          ownerPlayerId(0),
          team(0),
          pos(),
          speed(),
          facingDegrees(0.0f),
          lookUpDownAngle(0.0f),
          pose(0),
          hitPointsByte(255u),
          isRagdoll(false),
          hasPos(false),
          lastUpdateUs(0),
          hist(),
          histCount(0),
          histHead(0) {}
};

// Position a real client would render at renderUs: linear interpolation between the two
// received snapshots bracketing renderUs. Clamps to the oldest sample before the history
// and to the newest after it (no extrapolation). Returns false without any history.
bool interpolateSoldierPos(const TrackedSoldier& s, uint64_t renderUs, Vec3& out);

// Remaining aim offset (degrees) after elapsedUs of tracking: exponential decay that has
// dropped to ~5% of initialDeg at convergeUs.
float aimErrorDecay(float initialDeg, uint64_t elapsedUs, uint64_t convergeUs);

// Terrain line-of-sight approximation from the navmesh aerial height map: false when a
// height-map bump rises above the chord between both end heights. Fails open (true) when
// no height map is loaded.
bool terrainLineOfSight(const NavMesh* nav, const Vec3& a, const Vec3& b);

struct TrackedSpawnGroup {
    uint8_t spawnGroupId;
    uint16_t netId;
    uint8_t team;
    bool active;
    bool aiOnly;
    bool hasSpawnPoints;
    Vec3 pos;
    bool hasPos;

    TrackedSpawnGroup()
        : spawnGroupId(0),
          netId(0),
          team(0),
          active(false),
          aiOnly(false),
          hasSpawnPoints(false),
          pos(),
          hasPos(false) {}
};

struct TrackedControlPoint {
    uint16_t netId;
    int32_t team;
    bool isNeutral;
    bool allowCaptureByTeam1;
    bool allowCaptureByTeam2;
    Vec3 pos;
    bool hasPos;

    TrackedControlPoint()
        : netId(0),
          team(0),
          isNeutral(false),
          allowCaptureByTeam1(true),
          allowCaptureByTeam2(true),
          pos(),
          hasPos(false) {}
};

struct BotWorldView {
    uint8_t ownPlayerId;
    int32_t ownTeam;
    bool ownAlive;
    bool ownManDown;
    uint16_t ownSoldierNetId;
    bool hasOwnPos;
    Vec3 ownPos;
    float ownFacingDegrees;
    float ownPitchDegrees;
    uint8_t ownPose;
    float worldSize;

    const std::map<uint16_t, TrackedSoldier>* soldiers;
    const std::map<uint8_t, TrackedSpawnGroup>* spawnGroups;
    const std::map<uint16_t, TrackedControlPoint>* controlPoints;

    BotWorldView()
        : ownPlayerId(0),
          ownTeam(0),
          ownAlive(false),
          ownManDown(false),
          ownSoldierNetId(0),
          hasOwnPos(false),
          ownPos(),
          ownFacingDegrees(0.0f),
          ownPitchDegrees(0.0f),
          ownPose(0),
          worldSize(1024.0f),
          soldiers(nullptr),
          spawnGroups(nullptr),
          controlPoints(nullptr) {}
};

struct BotTickOutput {
    PlayerActionInput action;
    bool requestSpawn;
    int32_t desiredSpawnGroupId;
    int32_t desiredKit;

    BotTickOutput()
        : action(),
          requestSpawn(false),
          desiredSpawnGroupId(-1),
          desiredKit(0) {}
};

class BotBrain {
public:
    BotBrain(uint32_t botIndex, uint32_t seed,
             const NavMesh* navMesh, const RouteLoader* routeLoader,
             const AimConfig& aim = AimConfig());

    void reset();
    void onSpawned(const Vec3& spawnPos, uint64_t nowUs);
    void onDied(uint64_t nowUs);

    uint32_t botIndex() const { return m_botIndex; }
    int32_t assignedTeam() const { return m_team; }
    void setTeam(int32_t team) {
        if (team > 0 && team != m_team) {
            m_team = team;
            m_hasRouteTrack = false;
            m_hasPatrolGoal = false;
            m_waypoints.clear();
            m_waypointIdx = 0;
        }
    }
    int32_t assignedKit() const { return m_kit; }

    int32_t pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups);

    void tick(uint64_t nowUs, float dtSec, const BotWorldView& world, BotTickOutput& out);

private:
    enum NavTier {
        NAV_NAVMESH = 0,
        NAV_ROUTES  = 1,
        NAV_FALLBACK = 2
    };

    float randFloat(float lo, float hi);
    int32_t randInt(int32_t lo, int32_t hi);

    static float wrapAngleDeg(float deg);
    static float clampFloat(float v, float lo, float hi);
    static float distXZ(const Vec3& a, const Vec3& b);
    static float dist3D(const Vec3& a, const Vec3& b);

    bool pickObjectiveGoal(const BotWorldView& world, Vec3& outGoal);
    bool pickRandomWalkableNavPoint(Vec3& outGoal);
    void updateNavigation(uint64_t nowUs, const BotWorldView& world,
                          bool inCombat, Vec3& outMoveTarget, bool& outHasMoveTarget,
                          uint32_t& outNavButtons);
    void steerToward(const Vec3& ownPos, float ownYawDeg, float ownPitchDeg,
                     const Vec3& targetPos, float dtSec, float& outMouseX, float& outMouseY);

    // Human-like combat (non-legacy).
    bool selectTarget(uint64_t nowUs, const BotWorldView& world, const TrackedSoldier*& outTarget,
                      Vec3& outRendered, float& outDist);
    void tickCombatHuman(uint64_t nowUs, float dtSec, const BotWorldView& world,
                         const TrackedSoldier& target, const Vec3& rendered, float dist,
                         PlayerActionInput& act);
    void resetFireState();

    uint32_t m_botIndex;
    int32_t m_team;
    int32_t m_kit;
    std::mt19937 m_rng;

    const NavMesh* m_navMesh;
    const RouteLoader* m_routeLoader;

    // Humanized aiming parameters
    float m_turnRateDegPerSec;
    float m_aimNoiseYawDeg;
    float m_aimNoisePitchDeg;
    uint64_t m_nextAimNoiseUs;

    // Combat state
    uint16_t m_targetNetId;
    uint64_t m_targetAcquiredUs;
    uint64_t m_reactionDelayUs;
    bool m_strafeRight;
    uint64_t m_nextStrafeFlipUs;
    bool m_runSprint = false;        // --keep-running phase: sprint or slow walk
    uint64_t m_nextRunFlipUs = 0;
    bool m_burstFiring;
    int32_t m_burstTicksLeft;
    int32_t m_fireTicksSinceReload;
    int32_t m_reloadThresholdTicks;
    uint64_t m_reloadUntilUs;
    uint64_t m_nextWeaponSwitchUs;
    uint64_t m_switchBackToPrimaryUs;

    // Human aim model state
    AimConfig m_aim;
    float m_aimInitYawDeg;
    float m_aimInitPitchDeg;
    uint64_t m_aimConvergeUs;
    float m_jitterYawDeg;
    float m_jitterPitchDeg;
    uint64_t m_nextJitterUs;
    enum FireState { FIRE_WAIT = 0, FIRE_BURST = 1, FIRE_PAUSE = 2 };
    FireState m_fireState;
    uint64_t m_fireStateEndUs;
    int32_t m_burstShots;

    // Respawn cadence
    uint64_t m_nextSpawnRequestUs;
    uint32_t m_spawnGroupRotateIdx;

    // Navigation state (Tier 1: NavMesh)
    std::vector<NavPoint> m_waypoints;
    size_t m_waypointIdx;
    uint64_t m_nextPathComputeUs;

    // Navigation state (Tier 2: RouteLoader)
    bool m_hasRouteTrack;
    size_t m_routeTrackIdx;
    size_t m_routeSampleIdx;
    uint64_t m_lastRouteAdvanceUs;

    // Navigation state (Tier 3: Fallback patrol)
    Vec3 m_spawnAnchor;
    bool m_hasSpawnAnchor;
    Vec3 m_patrolGoal;
    bool m_hasPatrolGoal;
    uint64_t m_nextPatrolGoalUs;

    // Stuck detection
    Vec3 m_stuckAnchorPos;
    uint64_t m_stuckAnchorUs;
    bool m_hasStuckAnchor;
    uint64_t m_stuckRecoverUntilUs;
    float m_stuckStrafeDir;
};

} // namespace bf2bot

#endif // BF2BOT_BOT_BRAIN_H
