#ifndef BF2BOT_BOT_BRAIN_H
#define BF2BOT_BOT_BRAIN_H

#include "Bf2Protocol.h"
#include "NavMesh.h"
#include "RouteLoader.h"
#include "TeamTactics.h"

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
    bool isFiring;
    bool callForMedic;
    uint64_t lastFiredUs;
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
          isFiring(false),
          callForMedic(false),
          lastFiredUs(0),
          spawnGroupId(0),
          kit(0),
          soldierNetId(0),
          name() {}
};

// One ordered frontline flag stage (e.g., Karkand Hotel -> Square).
struct FrontlineStage {
    Vec3 anchor;       // Approximate world position (XZ used for matching live flags)
    float matchRadius; // Max XZ distance (m) to match a live ControlPoint / SpawnGroup

    FrontlineStage() : anchor(), matchRadius(45.0f) {}
    FrontlineStage(float x, float y, float z, float r = 45.0f)
        : anchor(x, y, z), matchRadius(r) {}
};

// Human-like aiming and tactical objective configuration.
struct AimConfig {
    bool legacy;       // true: old nearest-target / newest-position behaviour
    float skill;       // 0..1; scales reaction time, initial error, lead and convergence
    int32_t interpMs;  // client render delay used to pick the aimed-at position
    bool hasGoal;      // --goal: every bot moves on this world position instead of the flags
    float goalX, goalY, goalZ;
    bool keepRunning = false; // --keep-running: always move, alternating sprint and slow walk
    bool hasSpawnNear; // --spawn-near: spawn at the friendly spawn group closest to this point
    float spawnX, spawnY, spawnZ;
    bool autoMapFrontline; // auto-activate map frontline plan on MapInfo (e.g. Karkand Hotel -> Square)
    int32_t attackTeam;    // team attacking frontline stages (2 = USMC on Karkand); other team defends
    std::vector<FrontlineStage> frontline; // ordered stages; empty = dynamic Conquest

    AimConfig() : legacy(false), skill(0.7f), interpMs(100), hasGoal(false),
                  goalX(0.0f), goalY(0.0f), goalZ(0.0f), hasSpawnNear(false),
                  spawnX(0.0f), spawnY(0.0f), spawnZ(0.0f),
                  autoMapFrontline(true), attackTeam(2), frontline() {}
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
    bool isFiring;
    uint64_t lastFiredUs;
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
          isFiring(false),
          lastFiredUs(0),
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

// Estimates a soldier's smoothed world velocity (m/s) from recent snapshot history,
// falling back to s.speed when fewer than 2 samples are available.
bool estimateSoldierVelocity(const TrackedSoldier& s, Vec3& outVel);

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
    float takeOverCounter;
    float takeOverChangePerSecond;
    int32_t flagTeam;
    int32_t capturingState;
    Vec3 pos;
    bool hasPos;

    TrackedControlPoint()
        : netId(0),
          team(0),
          isNeutral(false),
          allowCaptureByTeam1(true),
          allowCaptureByTeam2(true),
          takeOverCounter(1.0f),
          takeOverChangePerSecond(0.0f),
          flagTeam(0),
          capturingState(0),
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
    float ownLookLeftRightAngle;
    float ownPendingTurn;
    float ownPitchDegrees;
    bool hasPredictedAim;
    float ownAimYawDegrees;
    float ownAimPitchDegrees;
    float ownMouseSpeedMod;
    uint8_t ownPose;
    uint8_t ownHitPointsByte;
    uint8_t ownWeaponZoom;
    uint8_t ownWeaponState;
    bool ownWeaponStowed;
    float ownSprintStamina;
    bool ownIsSprinting;
    bool ownIsGrounded;
    Vec3 ownSpeed;
    int32_t ownKit;
    float rttMs;
    float worldSize;

    const std::map<uint16_t, TrackedSoldier>* soldiers;
    const std::map<uint8_t, TrackedPlayer>* players;
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
          ownLookLeftRightAngle(0.0f),
          ownPendingTurn(0.0f),
          ownPitchDegrees(0.0f),
          hasPredictedAim(false),
          ownAimYawDegrees(0.0f),
          ownAimPitchDegrees(0.0f),
          ownMouseSpeedMod(1.0f),
          ownPose(0),
          ownHitPointsByte(255u),
          ownWeaponZoom(0u),
          ownWeaponState(0u),
          ownWeaponStowed(false),
          ownSprintStamina(1.0f),
          ownIsSprinting(false),
          ownIsGrounded(true),
          ownSpeed(),
          ownKit(-1),
          rttMs(0.0f),
          worldSize(1024.0f),
          soldiers(nullptr),
          players(nullptr),
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
             const AimConfig& aim = AimConfig(),
             TeamTactics* teamTactics = nullptr);

    void reset();
    void onSpawned(const Vec3& spawnPos, uint64_t nowUs);
    void onDied(uint64_t nowUs);

    uint32_t botIndex() const { return m_botIndex; }
    int32_t assignedTeam() const { return m_team; }
    void setTeam(int32_t team) {
        if (team > 0 && team != m_team) {
            if (m_teamTactics) {
                m_teamTactics->clearClaim(m_team, m_botIndex);
            }
            m_team = team;
            m_hasRouteTrack = false;
            m_hasPatrolGoal = false;
            m_hasActiveObjective = false;
            m_waypoints.clear();
            m_waypointIdx = 0;
        }
    }
    int32_t assignedKit() const { return m_kit; }
    const AimConfig& aimConfig() const { return m_aim; }
    void setFrontlinePlan(int32_t attackTeam, const std::vector<FrontlineStage>& stages) {
        if (attackTeam == 1 || attackTeam == 2) {
            m_aim.attackTeam = attackTeam;
        }
        m_aim.frontline = stages;
        m_hasActiveObjective = false;
        m_hasDefendPost = false;
        m_defendHoldUntilUs = 0;
        m_waypoints.clear();
        m_waypointIdx = 0;
    }

    int32_t pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups);
    int32_t pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups,
                           const std::map<uint16_t, TrackedControlPoint>* controlPoints,
                           const std::map<uint16_t, TrackedSoldier>* soldiers);

    void tick(uint64_t nowUs, float dtSec, const BotWorldView& world, BotTickOutput& out);

private:
    enum NavTier {
        NAV_NAVMESH = 0,
        NAV_ROUTES  = 1,
        NAV_FALLBACK = 2
    };

    enum FireState {
        FIRE_WAIT  = 0,
        FIRE_BURST = 1,
        FIRE_PAUSE = 2
    };

    enum UtilityAction {
        UTIL_NONE    = 0,
        UTIL_MEDKIT  = 1,
        UTIL_AMMOBAG = 2,
        UTIL_REVIVE  = 3,
        UTIL_GRENADE = 4,
        UTIL_PISTOL  = 5
    };

    enum UtilityPhase {
        UPHASE_SWITCH  = 0,
        UPHASE_ACT     = 1,
        UPHASE_RESTORE = 2
    };

    float randFloat(float lo, float hi);
    int32_t randInt(int32_t lo, int32_t hi);

    static float wrapAngleDeg(float deg);
    static float clampFloat(float v, float lo, float hi);
    static float distXZ(const Vec3& a, const Vec3& b);
    static float dist3D(const Vec3& a, const Vec3& b);

    float currentAimYaw(const BotWorldView& world) const;
    float currentAimPitch(const BotWorldView& world) const;
    float effectiveMouseScale(const BotWorldView& world) const;
    int32_t activeKit(const BotWorldView& world) const;
    int32_t rollReloadThreshold(int32_t kit);

    void steerAngles(float curYawDeg, float curPitchDeg,
                     float desiredYawDeg, float desiredPitchDeg,
                     float maxTurnDegPerSec, float mouseScale,
                     float dtSec, float& outMouseX, float& outMouseY) const;
    void steerToward(const Vec3& ownPos, float ownYawDeg, float ownPitchDeg,
                     const Vec3& targetPos, float mouseScale, float dtSec,
                     float& outMouseX, float& outMouseY);
    static void computeRelativeMove(float aimYawDeg, float moveYawDeg, float speedScale,
                                    float& outStrafe, float& outForward);

    struct ActiveFrontlineStage {
        size_t stageIdx;
        uint32_t objKey;
        Vec3 pos;
        int32_t ownerTeam;
        bool isNeutral;
        bool underCapture;
        Vec3 threatApproachPos;
        bool hasThreatApproachPos;

        ActiveFrontlineStage()
            : stageIdx(0u), objKey(0u), pos(), ownerTeam(0),
              isNeutral(false), underCapture(false),
              threatApproachPos(), hasThreatApproachPos(false) {}
    };

    bool resolveActiveFrontlineStage(const std::map<uint16_t, TrackedControlPoint>* controlPoints,
                                     const std::map<uint8_t, TrackedSpawnGroup>* spawnGroups,
                                     ActiveFrontlineStage& outStage) const;
    bool hasClearLineOfSight(const BotWorldView& world, const Vec3& targetPos,
                             uint8_t targetPose) const;
    bool pickWalkablePerimeterPoint(const Vec3& center, float minRad, float maxRad,
                                    float biasDirX, float biasDirZ, float halfArcRad,
                                    Vec3& outPos);

    bool isObjectiveStillActive(const BotWorldView& world, uint32_t objKey,
                                bool& outStillFriendly) const;
    bool pickObjectiveGoal(uint64_t nowUs, const BotWorldView& world, Vec3& outGoal);
    bool pickRandomWalkableNavPoint(Vec3& outGoal);
    void computeFriendlySeparation(const BotWorldView& world, float& outSepX, float& outSepZ) const;
    void updateNavigation(uint64_t nowUs, const BotWorldView& world,
                          bool inCombat, Vec3& outMoveTarget, bool& outHasMoveTarget,
                          uint32_t& outNavButtons);

    // Human-like combat (non-legacy).
    bool selectTarget(uint64_t nowUs, const BotWorldView& world, const TrackedSoldier*& outTarget,
                      Vec3& outRendered, float& outDist, int32_t& outVisibleEnemyCount);
    void updateDamageAndMemory(uint64_t nowUs, const BotWorldView& world,
                               const TrackedSoldier* bestTarget, const Vec3& rendered);
    bool tryStartUtilityAction(uint64_t nowUs, const BotWorldView& world,
                               const TrackedSoldier* bestTarget, const Vec3& targetRendered,
                               float targetDist);
    bool tickUtilityAction(uint64_t nowUs, float dtSec, const BotWorldView& world,
                           const TrackedSoldier* bestTarget, const Vec3& targetRendered,
                           float targetDist, PlayerActionInput& act);
    void tickCombatHuman(uint64_t nowUs, float dtSec, const BotWorldView& world,
                         const TrackedSoldier& target, const Vec3& rendered, float dist,
                         int32_t visibleEnemies, PlayerActionInput& act);
    void tickOutOfCombat(uint64_t nowUs, float dtSec, const BotWorldView& world,
                         bool hasMoveTarget, const Vec3& moveTarget, uint32_t navButtons,
                         PlayerActionInput& act);
    void resetFireState();
    void startUtility(UtilityAction action, uint64_t nowUs, const Vec3& aimPos, uint16_t targetNetId);
    void requestPrimaryRestore(uint64_t nowUs);

    uint32_t m_botIndex;
    int32_t m_team;
    int32_t m_kit;
    BotTacticalRole m_role;
    std::mt19937 m_rng;

    const NavMesh* m_navMesh;
    const RouteLoader* m_routeLoader;
    TeamTactics* m_teamTactics;

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
    int32_t m_totalReloads;
    uint64_t m_reloadUntilUs;
    uint64_t m_nextWeaponSwitchUs;
    uint64_t m_switchBackToPrimaryUs;
    uint64_t m_lastCombatUs;
    uint64_t m_lastJumpUs;
    uint64_t m_lastProneToggleUs;
    uint64_t m_zoomToggleCooldownUs;

    // Human aim model state
    AimConfig m_aim;
    float m_aimInitYawDeg;
    float m_aimInitPitchDeg;
    uint64_t m_aimConvergeUs;
    float m_jitterYawDeg;
    float m_jitterPitchDeg;
    uint64_t m_nextJitterUs;
    FireState m_fireState;
    uint64_t m_fireStateEndUs;
    int32_t m_burstShots;

    // Damage, threat memory, cover & scanning state
    bool m_hasLastHp;
    uint8_t m_lastOwnHp;
    uint64_t m_underFireUntilUs;
    uint64_t m_lastDamageUs;
    bool m_hasLastKnownEnemy;
    uint16_t m_lastKnownEnemyNetId;
    Vec3 m_lastKnownEnemyPos;
    Vec3 m_lastKnownEnemyVel;
    uint64_t m_lastKnownEnemyUs;
    bool m_hasCoverStep;
    Vec3 m_coverStepPos;
    uint64_t m_coverStepUntilUs;
    float m_scanOffsetDeg;
    uint64_t m_nextScanFlipUs;
    bool m_sprintActive;

    // Tactical utility / class action state machine
    UtilityAction m_utilAction;
    UtilityPhase m_utilPhase;
    uint64_t m_utilPhaseStartUs;
    uint64_t m_utilDeadlineUs;
    Vec3 m_utilTargetPos;
    uint16_t m_utilTargetNetId;
    uint64_t m_nextMedkitUs;
    uint64_t m_nextAmmoBagUs;
    uint64_t m_nextReviveUs;
    uint64_t m_nextGrenadeUs;
    bool m_usedPistolThisTarget;

    // Respawn cadence
    uint64_t m_nextSpawnRequestUs;
    uint32_t m_spawnGroupRotateIdx;

    // Active objective, flag capture hold & defensive perimeter watch
    bool m_hasActiveObjective;
    uint32_t m_activeObjectiveKey;
    Vec3 m_activeObjectivePos;
    bool m_activeObjectiveFriendly;
    uint64_t m_objectiveHoldStartUs;
    bool m_hasDefendPost;
    Vec3 m_defendPostPos;
    uint64_t m_defendHoldUntilUs;
    bool m_hasDefendWatchPoint;
    Vec3 m_defendWatchPoint;
    Vec3 m_lastOwnPosForDeath;
    bool m_hasLastOwnPosForDeath;

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

    // Multi-stage stuck detection
    Vec3 m_stuckAnchorPos;
    uint64_t m_stuckAnchorUs;
    bool m_hasStuckAnchor;
    uint64_t m_stuckRecoverUntilUs;
    float m_stuckStrafeDir;
    int32_t m_stuckStage;
    uint32_t m_blacklistedObjKey;
    uint64_t m_blacklistUntilUs;
};

} // namespace bf2bot

#endif // BF2BOT_BOT_BRAIN_H
