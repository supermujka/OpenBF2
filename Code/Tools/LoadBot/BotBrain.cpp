#include "BotBrain.h"

#include <algorithm>
#include <cmath>

namespace bf2bot {

namespace {

static const float kPi = 3.14159265358979323846f;
static const float kRadToDeg = 180.0f / kPi;
static const float kDegToRad = kPi / 180.0f;
static const float kLookFactor = 5.0f; // BF2 Soldier phy-soldier-look-factor-{x,y}
static const float kCombatRangeMeters = 50.0f;
static const float kMediumRangeMeters = 15.0f;
static const float kMaxHumanEngageRange = 95.0f;
static const float kBulletSpeedMps = 760.0f;
static const float kGravityMps2 = 9.81f;

// Infantry kits: SpecOps(0), Assault(2), Support(3), Medic(5), AntiTank(6)
static const int32_t kInfantryKits[5] = { 0, 2, 3, 5, 6 };

enum ObjectiveKind {
    OBJ_NEUTRAL = 0,
    OBJ_DEFEND  = 1,
    OBJ_ATTACK  = 2
};

struct TacticalObjective {
    uint32_t key;
    Vec3 pos;
    ObjectiveKind kind;
    bool isFriendly;
};

} // namespace

BotBrain::BotBrain(uint32_t botIndex, uint32_t seed,
                   const NavMesh* navMesh, const RouteLoader* routeLoader,
                   const AimConfig& aim, TeamTactics* teamTactics)
    : m_botIndex(botIndex),
      m_team((botIndex % 2u == 0u) ? 1 : 2),
      m_kit(kInfantryKits[botIndex % 5u]),
      m_role(TeamTactics::roleForBot(botIndex, kInfantryKits[botIndex % 5u])),
      m_rng(seed ^ (botIndex * 2654435761u + 0x9E3779B9u)),
      m_navMesh(navMesh),
      m_routeLoader(routeLoader),
      m_teamTactics(teamTactics),
      m_turnRateDegPerSec(240.0f),
      m_aimNoiseYawDeg(0.0f),
      m_aimNoisePitchDeg(0.0f),
      m_nextAimNoiseUs(0),
      m_targetNetId(0),
      m_targetAcquiredUs(0),
      m_reactionDelayUs(200000ULL),
      m_strafeRight((botIndex & 1u) != 0u),
      m_nextStrafeFlipUs(0),
      m_burstFiring(false),
      m_burstTicksLeft(0),
      m_fireTicksSinceReload(0),
      m_reloadThresholdTicks(30),
      m_totalReloads(0),
      m_reloadUntilUs(0),
      m_nextWeaponSwitchUs(0),
      m_switchBackToPrimaryUs(0),
      m_lastCombatUs(0),
      m_lastJumpUs(0),
      m_lastProneToggleUs(0),
      m_zoomToggleCooldownUs(0),
      m_aim(aim),
      m_aimInitYawDeg(0.0f),
      m_aimInitPitchDeg(0.0f),
      m_aimConvergeUs(400000ULL),
      m_jitterYawDeg(0.0f),
      m_jitterPitchDeg(0.0f),
      m_nextJitterUs(0),
      m_fireState(FIRE_WAIT),
      m_fireStateEndUs(0),
      m_burstShots(0),
      m_hasLastHp(false),
      m_lastOwnHp(255u),
      m_underFireUntilUs(0),
      m_lastDamageUs(0),
      m_hasLastKnownEnemy(false),
      m_lastKnownEnemyNetId(0),
      m_lastKnownEnemyPos(),
      m_lastKnownEnemyVel(),
      m_lastKnownEnemyUs(0),
      m_hasCoverStep(false),
      m_coverStepPos(),
      m_coverStepUntilUs(0),
      m_scanOffsetDeg(0.0f),
      m_nextScanFlipUs(0),
      m_sprintActive(true),
      m_utilAction(UTIL_NONE),
      m_utilPhase(UPHASE_SWITCH),
      m_utilPhaseStartUs(0),
      m_utilDeadlineUs(0),
      m_utilTargetPos(),
      m_utilTargetNetId(0),
      m_nextMedkitUs(0),
      m_nextAmmoBagUs(0),
      m_nextReviveUs(0),
      m_nextGrenadeUs(0),
      m_usedPistolThisTarget(false),
      m_nextSpawnRequestUs(0),
      m_spawnGroupRotateIdx(botIndex),
      m_hasActiveObjective(false),
      m_activeObjectiveKey(0u),
      m_activeObjectivePos(),
      m_activeObjectiveFriendly(false),
      m_objectiveHoldStartUs(0),
      m_hasDefendPost(false),
      m_defendPostPos(),
      m_defendHoldUntilUs(0),
      m_hasDefendWatchPoint(false),
      m_defendWatchPoint(),
      m_isStagingAssault(false),
      m_stagingCoverPos(),
      m_stagingHoldUntilUs(0),
      m_lastStagedObjKey(0u),
      m_hasCapCoverPos(false),
      m_capCoverPos(),
      m_capCoverRefreshUs(0),
      m_lastOwnPosForDeath(),
      m_hasLastOwnPosForDeath(false),
      m_waypoints(),
      m_waypointIdx(0),
      m_nextPathComputeUs(0),
      m_hasRouteTrack(false),
      m_routeTrackIdx(0),
      m_routeSampleIdx(0),
      m_lastRouteAdvanceUs(0),
      m_spawnAnchor(),
      m_hasSpawnAnchor(false),
      m_patrolGoal(),
      m_hasPatrolGoal(false),
      m_nextPatrolGoalUs(0),
      m_stuckAnchorPos(),
      m_stuckAnchorUs(0),
      m_hasStuckAnchor(false),
      m_stuckRecoverUntilUs(0),
      m_stuckStrafeDir(1.0f),
      m_stuckStage(0),
      m_blacklistedObjKey(0u),
      m_blacklistUntilUs(0) {
    m_turnRateDegPerSec = randFloat(210.0f, 300.0f);
    m_reloadThresholdTicks = rollReloadThreshold(m_kit);
}

int32_t BotBrain::rollReloadThreshold(int32_t kit) {
    if (kit == 3) {
        // Support LMG (M249/RPK-74/Type 95): large belts/drums.
        return randInt(80, 110);
    }
    return randInt(24, 32);
}

void BotBrain::reset() {
    resetFireState();
    m_aimNoiseYawDeg = 0.0f;
    m_aimNoisePitchDeg = 0.0f;
    m_nextAimNoiseUs = 0;

    m_targetNetId = 0;
    m_targetAcquiredUs = 0;
    m_reactionDelayUs = static_cast<uint64_t>(randInt(150, 250)) * 1000ULL;
    m_nextStrafeFlipUs = 0;
    m_burstFiring = false;
    m_burstTicksLeft = 0;
    m_fireTicksSinceReload = 0;
    m_reloadThresholdTicks = rollReloadThreshold(m_kit);
    m_totalReloads = 0;
    m_reloadUntilUs = 0;
    m_nextWeaponSwitchUs = 0;
    m_switchBackToPrimaryUs = 0;
    m_lastCombatUs = 0;
    m_lastJumpUs = 0;
    m_lastProneToggleUs = 0;
    m_zoomToggleCooldownUs = 0;

    m_hasLastHp = false;
    m_lastOwnHp = 255u;
    m_underFireUntilUs = 0;
    m_lastDamageUs = 0;
    m_hasLastKnownEnemy = false;
    m_lastKnownEnemyNetId = 0;
    m_lastKnownEnemyUs = 0;
    m_hasCoverStep = false;
    m_coverStepUntilUs = 0;
    m_scanOffsetDeg = 0.0f;
    m_nextScanFlipUs = 0;
    m_sprintActive = true;

    m_utilAction = UTIL_NONE;
    m_utilPhase = UPHASE_SWITCH;
    m_utilPhaseStartUs = 0;
    m_utilDeadlineUs = 0;
    m_utilTargetNetId = 0;
    m_nextMedkitUs = 0;
    m_nextAmmoBagUs = 0;
    m_nextReviveUs = 0;
    m_nextGrenadeUs = 0;
    m_usedPistolThisTarget = false;

    m_nextSpawnRequestUs = 0;
    m_hasActiveObjective = false;
    m_activeObjectiveKey = 0u;
    m_activeObjectiveFriendly = false;
    m_objectiveHoldStartUs = 0;
    m_hasDefendPost = false;
    m_defendHoldUntilUs = 0;
    m_hasDefendWatchPoint = false;
    m_isStagingAssault = false;
    m_stagingHoldUntilUs = 0;
    m_lastStagedObjKey = 0u;
    m_hasCapCoverPos = false;
    m_capCoverRefreshUs = 0;
    m_hasLastOwnPosForDeath = false;

    if (m_teamTactics) {
        m_teamTactics->clearClaim(m_team, m_botIndex);
    }

    m_waypoints.clear();
    m_waypointIdx = 0;
    m_nextPathComputeUs = 0;

    m_hasRouteTrack = false;
    m_routeTrackIdx = 0;
    m_routeSampleIdx = 0;
    m_lastRouteAdvanceUs = 0;

    m_hasSpawnAnchor = false;
    m_hasPatrolGoal = false;
    m_nextPatrolGoalUs = 0;

    m_hasStuckAnchor = false;
    m_stuckAnchorUs = 0;
    m_stuckRecoverUntilUs = 0;
    m_stuckStage = 0;
    m_blacklistedObjKey = 0u;
    m_blacklistUntilUs = 0;
}

void BotBrain::onSpawned(const Vec3& spawnPos, uint64_t nowUs) {
    resetFireState();
    m_spawnAnchor = spawnPos;
    m_hasSpawnAnchor = true;
    m_lastOwnPosForDeath = spawnPos;
    m_hasLastOwnPosForDeath = true;

    m_targetNetId = 0;
    m_burstFiring = false;
    m_burstTicksLeft = 0;
    m_fireTicksSinceReload = 0;
    m_reloadThresholdTicks = rollReloadThreshold(m_kit);
    m_totalReloads = 0;
    m_reloadUntilUs = 0;
    m_nextWeaponSwitchUs = nowUs + static_cast<uint64_t>(randInt(18000, 32000)) * 1000ULL;
    m_switchBackToPrimaryUs = 0;
    m_lastCombatUs = 0;
    m_lastJumpUs = 0;
    m_lastProneToggleUs = 0;
    m_zoomToggleCooldownUs = nowUs + 400000ULL;

    m_hasLastHp = true;
    m_lastOwnHp = 255u;
    m_underFireUntilUs = 0;
    m_lastDamageUs = 0;
    m_hasLastKnownEnemy = false;
    m_lastKnownEnemyNetId = 0;
    m_lastKnownEnemyUs = 0;
    m_hasCoverStep = false;
    m_coverStepUntilUs = 0;
    m_scanOffsetDeg = 0.0f;
    m_nextScanFlipUs = nowUs + static_cast<uint64_t>(randInt(1200, 2400)) * 1000ULL;
    m_sprintActive = true;

    m_utilAction = UTIL_NONE;
    m_utilPhase = UPHASE_SWITCH;
    m_utilPhaseStartUs = 0;
    m_utilDeadlineUs = 0;
    m_utilTargetNetId = 0;
    m_nextMedkitUs = nowUs + 2000000ULL;
    m_nextAmmoBagUs = nowUs + 4000000ULL;
    m_nextReviveUs = nowUs + 1500000ULL;
    m_nextGrenadeUs = nowUs + static_cast<uint64_t>(randInt(6000, 14000)) * 1000ULL;
    m_usedPistolThisTarget = false;

    m_hasActiveObjective = false;
    m_activeObjectiveKey = 0u;
    m_activeObjectiveFriendly = false;
    m_objectiveHoldStartUs = 0;
    m_hasDefendPost = false;
    m_defendHoldUntilUs = 0;
    m_hasDefendWatchPoint = false;
    m_isStagingAssault = false;
    m_stagingHoldUntilUs = 0;
    m_lastStagedObjKey = 0u;
    m_hasCapCoverPos = false;
    m_capCoverRefreshUs = 0;

    m_waypoints.clear();
    m_waypointIdx = 0;
    m_nextPathComputeUs = nowUs;

    m_hasRouteTrack = false;
    m_hasPatrolGoal = false;
    m_nextPatrolGoalUs = nowUs;

    m_stuckAnchorPos = spawnPos;
    m_stuckAnchorUs = nowUs;
    m_hasStuckAnchor = true;
    m_stuckRecoverUntilUs = 0;
    m_stuckStage = 0;
    m_blacklistedObjKey = 0u;
    m_blacklistUntilUs = 0;
}

void BotBrain::onDied(uint64_t nowUs) {
    if (m_teamTactics) {
        if (m_hasLastOwnPosForDeath) {
            m_teamTactics->reportFriendlyDeath(m_team, m_lastOwnPosForDeath, nowUs);
        }
        m_teamTactics->clearClaim(m_team, m_botIndex);
    }

    resetFireState();
    m_targetNetId = 0;
    m_burstFiring = false;
    m_burstTicksLeft = 0;
    m_utilAction = UTIL_NONE;
    m_hasLastKnownEnemy = false;
    m_hasCoverStep = false;
    m_hasActiveObjective = false;
    m_hasDefendPost = false;
    m_defendHoldUntilUs = 0;
    m_hasDefendWatchPoint = false;
    m_isStagingAssault = false;
    m_stagingHoldUntilUs = 0;
    m_lastStagedObjKey = 0u;
    m_hasCapCoverPos = false;
    m_capCoverRefreshUs = 0;
    m_underFireUntilUs = 0;
    m_waypoints.clear();
    m_waypointIdx = 0;
    m_hasRouteTrack = false;
    m_hasStuckAnchor = false;
    m_stuckRecoverUntilUs = 0;
    m_stuckStage = 0;
    m_nextSpawnRequestUs = nowUs + 500000ULL; // 0.5s after death before requesting respawn
}

float BotBrain::randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(m_rng);
}

int32_t BotBrain::randInt(int32_t lo, int32_t hi) {
    if (hi <= lo) {
        return lo;
    }
    std::uniform_int_distribution<int32_t> dist(lo, hi);
    return dist(m_rng);
}

float BotBrain::wrapAngleDeg(float deg) {
    while (deg > 180.0f) {
        deg -= 360.0f;
    }
    while (deg < -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

float BotBrain::clampFloat(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float BotBrain::distXZ(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float BotBrain::dist3D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float BotBrain::currentAimYaw(const BotWorldView& world) const {
    if (world.hasPredictedAim) {
        return wrapAngleDeg(world.ownAimYawDegrees);
    }
    return wrapAngleDeg(world.ownFacingDegrees + world.ownLookLeftRightAngle);
}

float BotBrain::currentAimPitch(const BotWorldView& world) const {
    if (world.hasPredictedAim) {
        return clampFloat(world.ownAimPitchDegrees, -85.0f, 85.0f);
    }
    return clampFloat(world.ownPitchDegrees, -85.0f, 85.0f);
}

float BotBrain::effectiveMouseScale(const BotWorldView& world) const {
    float mod = world.ownMouseSpeedMod;
    if (mod < 0.15f || mod > 2.0f) {
        mod = (world.ownWeaponZoom > 0u) ? 0.55f : 1.0f;
    }
    return kLookFactor * mod;
}

int32_t BotBrain::activeKit(const BotWorldView& world) const {
    return (world.ownKit >= 0) ? world.ownKit : m_kit;
}

void BotBrain::steerAngles(float curYawDeg, float curPitchDeg,
                           float desiredYawDeg, float desiredPitchDeg,
                           float maxTurnDegPerSec, float mouseScale,
                           float dtSec, float& outMouseX, float& outMouseY) const {
    const float yawErr = wrapAngleDeg(desiredYawDeg - curYawDeg);
    const float pitchErr = clampFloat(desiredPitchDeg, -80.0f, 80.0f) - curPitchDeg;
    const float maxDelta = maxTurnDegPerSec * dtSec;
    const float appliedYaw = clampFloat(yawErr, -maxDelta, maxDelta);
    const float appliedPitch = clampFloat(pitchErr, -maxDelta * 0.8f, maxDelta * 0.8f);
    const float invScale = (mouseScale > 0.25f) ? (1.0f / mouseScale) : (1.0f / kLookFactor);
    outMouseX = appliedYaw * invScale;
    outMouseY = appliedPitch * invScale;
}

void BotBrain::steerToward(const Vec3& ownPos, float ownYawDeg, float ownPitchDeg,
                           const Vec3& targetPos, float mouseScale, float dtSec,
                           float& outMouseX, float& outMouseY) {
    const float dx = targetPos.x - ownPos.x;
    const float dy = targetPos.y - ownPos.y;
    const float dz = targetPos.z - ownPos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);

    // BF2 left-handed Y-up: +Z is yaw=0, +X is yaw=+90 deg.
    const float desiredYaw = std::atan2(dx, dz) * kRadToDeg;
    const float desiredPitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                          -75.0f, 75.0f);
    steerAngles(ownYawDeg, ownPitchDeg, desiredYaw, desiredPitch,
                m_turnRateDegPerSec, mouseScale, dtSec, outMouseX, outMouseY);
}

void BotBrain::computeRelativeMove(float aimYawDeg, float moveYawDeg, float speedScale,
                                   float& outStrafe, float& outForward) {
    const float relRad = wrapAngleDeg(moveYawDeg - aimYawDeg) * kDegToRad;
    outForward = std::cos(relRad) * speedScale;
    outStrafe = std::sin(relRad) * speedScale;
}

int32_t BotBrain::pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups) {
    return pickSpawnGroup(spawnGroups, nullptr, nullptr);
}

int32_t BotBrain::pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups,
                                 const std::map<uint16_t, TrackedControlPoint>* controlPoints,
                                 const std::map<uint16_t, TrackedSoldier>* soldiers) {
    if (spawnGroups.empty()) {
        return -1;
    }

    std::vector<uint8_t> tier1; // active, friendly, !aiOnly, hasSpawnPoints
    std::vector<uint8_t> tier2; // active, friendly, !aiOnly
    std::vector<uint8_t> tier3; // active, friendly
    std::vector<uint8_t> tier4; // friendly

    std::vector<Vec3> hostileObjectives;
    for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.begin();
         it != spawnGroups.end(); ++it) {
        const TrackedSpawnGroup& sg = it->second;
        if (static_cast<int32_t>(sg.team) != m_team) {
            if (sg.hasPos && !sg.aiOnly && (sg.team != 0u || sg.hasSpawnPoints)) {
                hostileObjectives.push_back(sg.pos);
            }
            continue;
        }
        tier4.push_back(sg.spawnGroupId);
        if (sg.active) {
            tier3.push_back(sg.spawnGroupId);
            if (!sg.aiOnly) {
                tier2.push_back(sg.spawnGroupId);
                if (sg.hasSpawnPoints) {
                    tier1.push_back(sg.spawnGroupId);
                }
            }
        }
    }

    if (controlPoints) {
        for (std::map<uint16_t, TrackedControlPoint>::const_iterator it = controlPoints->begin();
             it != controlPoints->end(); ++it) {
            const TrackedControlPoint& cp = it->second;
            if (cp.hasPos && cp.team != m_team) {
                hostileObjectives.push_back(cp.pos);
            }
        }
    }

    const std::vector<uint8_t>* chosen = nullptr;
    if (!tier1.empty()) {
        chosen = &tier1;
    } else if (!tier2.empty()) {
        chosen = &tier2;
    } else if (!tier3.empty()) {
        chosen = &tier3;
    } else if (!tier4.empty()) {
        chosen = &tier4;
    }

    if (!chosen || chosen->empty()) {
        return -1;
    }

    if (m_aim.hasSpawnNear) {
        // --spawn-near: always the friendly spawn group closest to the requested point.
        int32_t best = -1;
        float bestD = 1e30f;
        for (size_t i = 0; i < chosen->size(); ++i) {
            std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.find((*chosen)[i]);
            if (it == spawnGroups.end() || !it->second.hasPos) {
                continue;
            }
            const float dx = it->second.pos.x - m_aim.spawnX;
            const float dz = it->second.pos.z - m_aim.spawnZ;
            const float d = dx * dx + dz * dz;
            if (d < bestD) {
                bestD = d;
                best = static_cast<int32_t>((*chosen)[i]);
            }
        }
        if (best >= 0) {
            return best;
        }
    }

    if (m_aim.hasGoal) {
        // When --goal is set without --spawn-near, prefer the friendly spawn closest to --goal.
        int32_t best = -1;
        float bestD = 1e30f;
        for (size_t i = 0; i < chosen->size(); ++i) {
            std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.find((*chosen)[i]);
            if (it == spawnGroups.end() || !it->second.hasPos) {
                continue;
            }
            const float dx = it->second.pos.x - m_aim.goalX;
            const float dz = it->second.pos.z - m_aim.goalZ;
            const float d = dx * dx + dz * dz;
            if (d < bestD) {
                bestD = d;
                best = static_cast<int32_t>((*chosen)[i]);
            }
        }
        if (best >= 0) {
            return best;
        }
    }

    // Staged frontline plan (e.g. Karkand: Hotel -> Square): both attackers and defenders
    // spawn at their closest active spawn to the currently contested frontline flag.
    ActiveFrontlineStage flStage;
    if (resolveActiveFrontlineStage(controlPoints, &spawnGroups, flStage)) {
        int32_t best = -1;
        float bestScore = -1e30f;
        for (size_t i = 0; i < chosen->size(); ++i) {
            std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.find((*chosen)[i]);
            if (it == spawnGroups.end() || !it->second.hasPos) {
                continue;
            }
            const TrackedSpawnGroup& sg = it->second;
            const float d = distXZ(sg.pos, flStage.pos);
            int32_t nearbyEnemies = 0;
            if (soldiers) {
                for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = soldiers->begin();
                     sit != soldiers->end(); ++sit) {
                    const TrackedSoldier& s = sit->second;
                    if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                    if (s.team == 0 || s.team == m_team) continue;
                    if (distXZ(sg.pos, s.pos) < 16.0f) {
                        ++nearbyEnemies;
                    }
                }
            }
            // Prefer spawning directly at/nearest to the active frontline stage;
            // only divert to backup spawn if heavily overrun (>=4 enemies inside 16m).
            const float score = -d - (nearbyEnemies >= 4 ? 140.0f : 0.0f);
            if (score > bestScore) {
                bestScore = score;
                best = static_cast<int32_t>(sg.spawnGroupId);
            }
        }
        if (best >= 0) {
            return best;
        }
    }

    // Frontline spawn scoring: prefer friendly flags close to enemy/neutral objectives,
    // while avoiding heavily overrun spawns when another frontline spawn is available.
    if (chosen->size() > 1u && !hostileObjectives.empty()) {
        struct ScoredSpawn {
            uint8_t sgId;
            float score;
        };
        std::vector<ScoredSpawn> ranked;
        ranked.reserve(chosen->size());

        for (size_t i = 0; i < chosen->size(); ++i) {
            std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.find((*chosen)[i]);
            if (it == spawnGroups.end()) {
                continue;
            }
            const TrackedSpawnGroup& sg = it->second;
            float minHostileDist = 500.0f;
            if (sg.hasPos) {
                for (size_t h = 0; h < hostileObjectives.size(); ++h) {
                    const float d = distXZ(sg.pos, hostileObjectives[h]);
                    if (d < minHostileDist) {
                        minHostileDist = d;
                    }
                }
            }

            // Penalize if enemies are actively swarming right on top of this spawn.
            int32_t nearbyEnemies = 0;
            if (soldiers && sg.hasPos) {
                for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = soldiers->begin();
                     sit != soldiers->end(); ++sit) {
                    const TrackedSoldier& s = sit->second;
                    if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                    if (s.team == 0 || s.team == m_team) continue;
                    if (distXZ(sg.pos, s.pos) < 18.0f) {
                        ++nearbyEnemies;
                    }
                }
            }

            ScoredSpawn ss;
            ss.sgId = sg.spawnGroupId;
            // Lower distance to frontline = higher score; heavily camped spawn penalized.
            ss.score = -minHostileDist - (nearbyEnemies >= 3 ? 120.0f : static_cast<float>(nearbyEnemies) * 15.0f);
            ranked.push_back(ss);
        }

        if (!ranked.empty()) {
            std::sort(ranked.begin(), ranked.end(),
                      [](const ScoredSpawn& a, const ScoredSpawn& b) { return a.score > b.score; });
            // Spread spawns across the top 2-3 frontline flags so the team doesn't bottleneck.
            const size_t topK = (ranked.size() < 3u) ? ranked.size() : 3u;
            // Only include rank 1..topK-1 if within 130m of the closest frontline spawn.
            size_t usable = 1u;
            while (usable < topK && (ranked[0].score - ranked[usable].score) < 130.0f) {
                ++usable;
            }
            const size_t pick = (m_spawnGroupRotateIdx++) % usable;
            return static_cast<int32_t>(ranked[pick].sgId);
        }
    }

    const size_t idx = (m_spawnGroupRotateIdx++) % chosen->size();
    return static_cast<int32_t>((*chosen)[idx]);
}

bool BotBrain::hasClearLineOfSight(const BotWorldView& world, const Vec3& targetPos,
                                   uint8_t targetPose) const {
    float ownEyeH = 1.55f;
    if (world.ownPose == 1u) ownEyeH = 1.10f;
    else if (world.ownPose == 2u) ownEyeH = 0.42f;
    else if (world.ownPose == 3u) ownEyeH = 0.45f;

    float tgtH = 1.38f;
    if (targetPose == 1u) tgtH = 0.95f;
    else if (targetPose == 2u) tgtH = 0.34f;
    else if (targetPose == 3u) tgtH = 0.38f;

    const Vec3 ownEye(world.ownPos.x, world.ownPos.y + ownEyeH, world.ownPos.z);
    const Vec3 tgtPt(targetPos.x, targetPos.y + tgtH, targetPos.z);

    if (!terrainLineOfSight(m_navMesh, ownEye, tgtPt)) {
        return false;
    }

    if (m_navMesh && m_navMesh->isLoaded()) {
        if (!m_navMesh->raycastLineOfSight(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                           targetPos.x, targetPos.y, targetPos.z,
                                           ownEyeH, tgtH)) {
            return false;
        }
    }

    return true;
}

bool BotBrain::hasFriendInLineOfFire(const BotWorldView& world, const Vec3& targetPos) const {
    if (!world.soldiers || !world.hasOwnPos) {
        return false;
    }
    const float dx = targetPos.x - world.ownPos.x;
    const float dz = targetPos.z - world.ownPos.z;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist < 1.5f) {
        return false;
    }
    const float dirX = dx / dist;
    const float dirZ = dz / dist;
    const float ownEyeY = world.ownPos.y + (world.ownPose == 2u ? 0.42f : (world.ownPose == 1u ? 1.10f : 1.55f));
    const float tgtBodyY = targetPos.y + 1.25f;

    for (std::map<uint16_t, TrackedSoldier>::const_iterator it = world.soldiers->begin();
         it != world.soldiers->end(); ++it) {
        const TrackedSoldier& f = it->second;
        if (f.netId == world.ownSoldierNetId || f.team != world.ownTeam) {
            continue;
        }
        if (!f.hasPos || f.isRagdoll || f.hitPointsByte == 0u) {
            continue;
        }
        const float fx = f.pos.x - world.ownPos.x;
        const float fz = f.pos.z - world.ownPos.z;
        const float proj = fx * dirX + fz * dirZ;
        if (proj < 0.55f || proj > dist - 0.55f) {
            continue;
        }
        const float perp = std::fabs(fx * dirZ - fz * dirX);
        const float halfCorridor = 0.85f + proj * 0.035f;
        if (perp > halfCorridor) {
            continue;
        }
        const float rayY = ownEyeY + (tgtBodyY - ownEyeY) * (proj / dist);
        const float friendTopY = f.pos.y + (f.pose == 2u ? 0.65f : (f.pose == 1u ? 1.35f : 1.85f));
        if (rayY >= f.pos.y - 0.25f && rayY <= friendTopY) {
            return true;
        }
    }
    return false;
}

bool BotBrain::hasFriendNearBlast(const BotWorldView& world, const Vec3& blastPos, float radius) const {
    if (!world.soldiers) {
        return false;
    }
    const float rSq = radius * radius;
    for (std::map<uint16_t, TrackedSoldier>::const_iterator it = world.soldiers->begin();
         it != world.soldiers->end(); ++it) {
        const TrackedSoldier& f = it->second;
        if (f.team != world.ownTeam || !f.hasPos || f.isRagdoll || f.hitPointsByte == 0u) {
            continue;
        }
        const float dx = f.pos.x - blastPos.x;
        const float dz = f.pos.z - blastPos.z;
        if (dx * dx + dz * dz <= rSq) {
            return true;
        }
    }
    return false;
}

bool BotBrain::resolveActiveFrontlineStage(
    const std::map<uint16_t, TrackedControlPoint>* controlPoints,
    const std::map<uint8_t, TrackedSpawnGroup>* spawnGroups,
    ActiveFrontlineStage& outStage) const {
    if (m_aim.frontline.empty()) {
        return false;
    }

    const int32_t attackTeam = (m_aim.attackTeam == 1 || m_aim.attackTeam == 2)
        ? m_aim.attackTeam : 2;

    for (size_t s = 0; s < m_aim.frontline.size(); ++s) {
        const FrontlineStage& st = m_aim.frontline[s];
        const float maxR = (st.matchRadius > 5.0f) ? st.matchRadius : 45.0f;

        bool matched = false;
        ActiveFrontlineStage cand;
        cand.stageIdx = s;
        float bestDist = maxR;

        if (controlPoints) {
            for (std::map<uint16_t, TrackedControlPoint>::const_iterator it = controlPoints->begin();
                 it != controlPoints->end(); ++it) {
                const TrackedControlPoint& cp = it->second;
                if (!cp.hasPos) continue;
                const float d = distXZ(cp.pos, st.anchor);
                if (d <= bestDist) {
                    bestDist = d;
                    matched = true;
                    cand.objKey = 0x10000u | static_cast<uint32_t>(cp.netId);
                    cand.pos = cp.pos;
                    cand.ownerTeam = cp.team;
                    cand.isNeutral = cp.isNeutral || (cp.team == 0);
                    cand.underCapture = (cp.capturingState != 0 || cp.takeOverChangePerSecond < -0.01f);
                }
            }
        }

        if (!matched && spawnGroups) {
            for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups->begin();
                 it != spawnGroups->end(); ++it) {
                const TrackedSpawnGroup& sg = it->second;
                if (!sg.hasPos || sg.aiOnly) continue;
                const float d = distXZ(sg.pos, st.anchor);
                if (d <= bestDist) {
                    bestDist = d;
                    matched = true;
                    cand.objKey = 0x20000u | static_cast<uint32_t>(sg.spawnGroupId);
                    cand.pos = sg.pos;
                    if (std::fabs(cand.pos.y) < 0.01f && std::fabs(st.anchor.y) > 0.01f) {
                        cand.pos.y = st.anchor.y;
                    }
                    cand.ownerTeam = static_cast<int32_t>(sg.team);
                    cand.isNeutral = (sg.team == 0u);
                    cand.underCapture = false;
                }
            }
        }

        if (!matched) {
            continue;
        }

        // Advance to the next stage once attackers fully hold this flag.
        if (!cand.isNeutral && cand.ownerTeam == attackTeam) {
            continue;
        }

        // Compute expected attacker approach position for defender orientation.
        if (s > 0u) {
            cand.threatApproachPos = m_aim.frontline[s - 1u].anchor;
            cand.hasThreatApproachPos = true;
        } else {
            float bestAtkD = 1e30f;
            if (controlPoints) {
                for (std::map<uint16_t, TrackedControlPoint>::const_iterator it = controlPoints->begin();
                     it != controlPoints->end(); ++it) {
                    const TrackedControlPoint& cp = it->second;
                    if (!cp.hasPos || cp.team != attackTeam || cp.isNeutral) continue;
                    const float d = distXZ(cp.pos, cand.pos);
                    if (d > 5.0f && d < bestAtkD) {
                        bestAtkD = d;
                        cand.threatApproachPos = cp.pos;
                        cand.hasThreatApproachPos = true;
                    }
                }
            }
            if (!cand.hasThreatApproachPos && spawnGroups) {
                for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups->begin();
                     it != spawnGroups->end(); ++it) {
                    const TrackedSpawnGroup& sg = it->second;
                    if (!sg.hasPos || sg.aiOnly || static_cast<int32_t>(sg.team) != attackTeam) continue;
                    const float d = distXZ(sg.pos, cand.pos);
                    if (d > 5.0f && d < bestAtkD) {
                        bestAtkD = d;
                        cand.threatApproachPos = sg.pos;
                        cand.hasThreatApproachPos = true;
                    }
                }
            }
        }

        outStage = cand;
        return true;
    }

    return false;
}

bool BotBrain::pickWalkablePerimeterPoint(const Vec3& center, float minRad, float maxRad,
                                          float biasDirX, float biasDirZ, float halfArcRad,
                                          Vec3& outPos) {
    const bool hasBias = (biasDirX * biasDirX + biasDirZ * biasDirZ) > 0.01f &&
                         halfArcRad < (kPi - 0.05f);
    const float baseAng = hasBias ? std::atan2(biasDirX, biasDirZ) : 0.0f;

    float refY = center.y;
    if (m_navMesh && m_navMesh->isLoaded()) {
        const uint32_t centerPoly = m_navMesh->findNearestPolygon(center.x, center.y, center.z, 35.0f);
        if (centerPoly != NavMesh::INVALID_POLYGON) {
            refY = m_navMesh->getPolygonCentroid(centerPoly).y;
        }
    }

    for (int attempt = 0; attempt < 18; ++attempt) {
        const float ang = hasBias
            ? (baseAng + randFloat(-halfArcRad, halfArcRad))
            : randFloat(0.0f, 2.0f * kPi);
        const float rad = randFloat(minRad, maxRad);
        const float px = center.x + std::sin(ang) * rad;
        const float pz = center.z + std::cos(ang) * rad;

        if (!m_navMesh || !m_navMesh->isLoaded()) {
            outPos = Vec3(px, refY, pz);
            return true;
        }
        const uint32_t poly = m_navMesh->findPolygonExact(px, refY, pz);
        if (poly != NavMesh::INVALID_POLYGON) {
            outPos = Vec3(px, m_navMesh->getPolygonHeight(poly, px, pz), pz);
            return true;
        }
    }

    if (m_navMesh && m_navMesh->isLoaded()) {
        const uint32_t nearPoly = m_navMesh->findNearestPolygon(center.x, refY, center.z, maxRad + 6.0f);
        if (nearPoly != NavMesh::INVALID_POLYGON) {
            const NavPoint c = m_navMesh->getPolygonCentroid(nearPoly);
            outPos = Vec3(c.x, c.y, c.z);
            return true;
        }
    }

    outPos = Vec3(center.x, refY, center.z);
    return true;
}

bool BotBrain::isObjectiveStillActive(const BotWorldView& world, uint32_t objKey,
                                      bool& outStillFriendly) const {
    outStillFriendly = false;
    if ((objKey & 0x30000u) == 0x10000u && world.controlPoints) {
        const uint16_t netId = static_cast<uint16_t>(objKey & 0xFFFFu);
        std::map<uint16_t, TrackedControlPoint>::const_iterator it = world.controlPoints->find(netId);
        if (it == world.controlPoints->end() || !it->second.hasPos) {
            return false;
        }
        outStillFriendly = (it->second.team == m_team && !it->second.isNeutral);
        return true;
    }
    if ((objKey & 0x30000u) == 0x20000u && world.spawnGroups) {
        const uint8_t sgId = static_cast<uint8_t>(objKey & 0xFFu);
        std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = world.spawnGroups->find(sgId);
        if (it == world.spawnGroups->end() || !it->second.hasPos) {
            return false;
        }
        outStillFriendly = (static_cast<int32_t>(it->second.team) == m_team);
        return true;
    }
    return false;
}

bool BotBrain::pickObjectiveGoal(uint64_t nowUs, const BotWorldView& world, Vec3& outGoal) {
    if (m_aim.hasGoal) {
        // Spread the bots over ~16 m around the commanded point so they do not stack.
        outGoal = Vec3(m_aim.goalX + randFloat(-8.0f, 8.0f), m_aim.goalY,
                       m_aim.goalZ + randFloat(-8.0f, 8.0f));
        m_hasActiveObjective = false;
        m_hasDefendPost = false;
        m_hasDefendWatchPoint = false;
        return true;
    }

    // Staged frontline plan (e.g. Karkand: USMC attacks Hotel -> Square, MEC defends Hotel -> Square).
    ActiveFrontlineStage flStage;
    if (resolveActiveFrontlineStage(world.controlPoints, world.spawnGroups, flStage)) {
        const int32_t attackTeam = (m_aim.attackTeam == 1 || m_aim.attackTeam == 2)
            ? m_aim.attackTeam : 2;
        const bool isAttacker = (m_team == attackTeam);

        m_hasActiveObjective = true;
        if (m_activeObjectiveKey != flStage.objKey) {
            m_objectiveHoldStartUs = 0;
            m_defendHoldUntilUs = 0;
        }
        m_activeObjectiveKey = flStage.objKey;
        m_activeObjectivePos = flStage.pos;

        if (m_teamTactics) {
            m_teamTactics->claimObjective(m_team, m_botIndex, flStage.objKey, nowUs);
        }

        if (isAttacker || flStage.isNeutral || flStage.ownerTeam != m_team) {
            // Attackers push into wall-hugging cover inside the capture zone; defenders rush back if neutralized.
            m_activeObjectiveFriendly = false;
            m_hasDefendPost = false;
            m_hasDefendWatchPoint = false;
            NavPoint wallCap;
            if (m_navMesh && m_navMesh->isLoaded() &&
                m_navMesh->findWallHuggingPoint(flStage.pos.x, flStage.pos.y, flStage.pos.z,
                                                2.2f, 8.0f, flStage.pos.x, flStage.pos.z + 40.0f,
                                                m_botIndex + 1u, wallCap)) {
                outGoal = Vec3(wallCap.x, wallCap.y, wallCap.z);
            } else {
                pickWalkablePerimeterPoint(flStage.pos, 2.0f, 7.5f, 0.0f, 0.0f, kPi, outGoal);
            }
            return true;
        }

        // Defender holding active frontline flag (e.g. MEC defending Hotel, then Square).
        m_activeObjectiveFriendly = true;

        float biasX = 0.0f;
        float biasZ = -1.0f;
        if (flStage.hasThreatApproachPos) {
            const float dx = flStage.threatApproachPos.x - flStage.pos.x;
            const float dz = flStage.threatApproachPos.z - flStage.pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len > 1.0f) {
                biasX = dx / len;
                biasZ = dz / len;
            }
            m_defendWatchPoint = flStage.threatApproachPos;
        } else {
            m_defendWatchPoint = Vec3(flStage.pos.x, flStage.pos.y, flStage.pos.z - 60.0f);
        }
        m_hasDefendWatchPoint = true;

        if (flStage.underCapture) {
            // Enemy is actively burning the flag -> collapse tight into the capture radius.
            m_hasDefendPost = false;
            m_defendHoldUntilUs = 0;
            pickWalkablePerimeterPoint(flStage.pos, 2.0f, 7.5f, 0.0f, 0.0f, kPi, outGoal);
        } else if ((m_botIndex % 3u) == 0u) {
            // Inner flag garrison (33% of defenders guard the flag courtyard itself).
            pickWalkablePerimeterPoint(flStage.pos, 4.0f, 11.0f, 0.0f, 0.0f, kPi, outGoal);
            m_defendPostPos = outGoal;
            m_hasDefendPost = true;
        } else {
            // Forward & flank perimeter defenders facing the attacker approach lanes.
            pickWalkablePerimeterPoint(flStage.pos, 10.0f, 23.0f, biasX, biasZ, 1.48f, outGoal);
            m_defendPostPos = outGoal;
            m_hasDefendPost = true;
        }
        return true;
    }

    m_hasDefendPost = false;
    m_hasDefendWatchPoint = false;

    std::vector<TacticalObjective> candidates;

    auto countEnemiesNear = [&](const Vec3& pos, float radius) -> int32_t {
        int32_t n = 0;
        if (world.soldiers) {
            for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->begin();
                 sit != world.soldiers->end(); ++sit) {
                const TrackedSoldier& s = sit->second;
                if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                if (s.team == 0 || s.team == m_team) continue;
                if (distXZ(pos, s.pos) <= radius) {
                    ++n;
                }
            }
        }
        if (n == 0 && m_teamTactics) {
            n = static_cast<int32_t>(m_teamTactics->countSpottedNear(m_team, pos, radius, nowUs));
        }
        return n;
    };

    if (world.controlPoints) {
        for (std::map<uint16_t, TrackedControlPoint>::const_iterator it = world.controlPoints->begin();
             it != world.controlPoints->end(); ++it) {
            const TrackedControlPoint& cp = it->second;
            if (!cp.hasPos) {
                continue;
            }
            const bool capturable = (m_team == 1) ? cp.allowCaptureByTeam1 : cp.allowCaptureByTeam2;
            if (cp.isNeutral && capturable) {
                TacticalObjective obj;
                obj.key = 0x10000u | static_cast<uint32_t>(cp.netId);
                obj.pos = cp.pos;
                obj.kind = OBJ_NEUTRAL;
                obj.isFriendly = false;
                candidates.push_back(obj);
            } else if (cp.team != m_team && capturable) {
                TacticalObjective obj;
                obj.key = 0x10000u | static_cast<uint32_t>(cp.netId);
                obj.pos = cp.pos;
                obj.kind = OBJ_ATTACK;
                obj.isFriendly = false;
                candidates.push_back(obj);
            } else if (cp.team == m_team) {
                const bool underCapture = (cp.capturingState != 0 || cp.takeOverChangePerSecond < -0.01f);
                if (underCapture || countEnemiesNear(cp.pos, 35.0f) > 0) {
                    TacticalObjective obj;
                    obj.key = 0x10000u | static_cast<uint32_t>(cp.netId);
                    obj.pos = cp.pos;
                    obj.kind = OBJ_DEFEND;
                    obj.isFriendly = true;
                    candidates.push_back(obj);
                }
            }
        }
    }

    if (candidates.empty() && world.spawnGroups) {
        for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = world.spawnGroups->begin();
             it != world.spawnGroups->end(); ++it) {
            const TrackedSpawnGroup& sg = it->second;
            if (!sg.hasPos || sg.aiOnly) {
                continue;
            }
            const int32_t sgTeam = static_cast<int32_t>(sg.team);
            if (sgTeam == 0 && (sg.hasSpawnPoints || sg.active)) {
                // Neutralized flag! High priority to finish capping.
                TacticalObjective obj;
                obj.key = 0x20000u | static_cast<uint32_t>(sg.spawnGroupId);
                obj.pos = sg.pos;
                obj.kind = OBJ_NEUTRAL;
                obj.isFriendly = false;
                candidates.push_back(obj);
            } else if (sgTeam != m_team && sgTeam != 0) {
                TacticalObjective obj;
                obj.key = 0x20000u | static_cast<uint32_t>(sg.spawnGroupId);
                obj.pos = sg.pos;
                obj.kind = OBJ_ATTACK;
                obj.isFriendly = false;
                candidates.push_back(obj);
            } else if (sgTeam == m_team && sg.active && countEnemiesNear(sg.pos, 35.0f) > 0) {
                TacticalObjective obj;
                obj.key = 0x20000u | static_cast<uint32_t>(sg.spawnGroupId);
                obj.pos = sg.pos;
                obj.kind = OBJ_DEFEND;
                obj.isFriendly = true;
                candidates.push_back(obj);
            }
        }
    }

    if (candidates.empty()) {
        m_hasActiveObjective = false;
        return false;
    }

    const uint32_t squadId = TeamTactics::squadForBot(m_botIndex);
    float bestScore = -1e9f;
    size_t bestIdx = 0u;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const TacticalObjective& obj = candidates[i];
        const float d = distXZ(world.ownPos, obj.pos);

        float score = 0.0f;
        if (obj.kind == OBJ_NEUTRAL) {
            score += 48.0f;
        } else if (obj.kind == OBJ_DEFEND) {
            score += (m_role == ROLE_DEFENDER) ? 46.0f : 30.0f;
            if (d < 65.0f) {
                score += 12.0f; // nearby responders rush to defend
            }
        } else {
            score += 32.0f;
        }

        // Strongly prefer frontline / nearby flags over crossing the entire map.
        score -= d * 0.14f;

        // Squad affinity: bots in the same squad prefer pushing together, while
        // different squads spread across distinct frontline flags.
        if (candidates.size() > 1u && ((i + squadId) % candidates.size()) == 0u) {
            score += 7.0f;
        }

        // Soft claim balancing via TeamTactics so 32 bots don't stack on a single flag.
        if (m_teamTactics) {
            const uint32_t claimants = m_teamTactics->countClaimants(m_team, obj.key, m_botIndex, nowUs);
            score -= static_cast<float>(claimants) * 5.0f;
        }

        // Keep slight stickiness to our current objective so bots commit to caps.
        if (m_hasActiveObjective && obj.key == m_activeObjectiveKey) {
            score += 9.0f;
        }

        // Avoid objectives we just got badly stuck trying to reach.
        if (m_blacklistedObjKey != 0u && obj.key == m_blacklistedObjKey && nowUs < m_blacklistUntilUs) {
            score -= 60.0f;
        }

        // Mild per-decision jitter for organic movement.
        score += randFloat(-3.0f, 3.0f);

        if (score > bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }

    const TacticalObjective& chosen = candidates[bestIdx];
    m_hasActiveObjective = true;
    if (m_activeObjectiveKey != chosen.key) {
        m_objectiveHoldStartUs = 0;
    }
    m_activeObjectiveKey = chosen.key;
    m_activeObjectivePos = chosen.pos;
    m_activeObjectiveFriendly = chosen.isFriendly;

    if (m_teamTactics) {
        m_teamTactics->claimObjective(m_team, m_botIndex, chosen.key, nowUs);
    }

    outGoal = chosen.pos;
    // Tactical perimeter spread around the flag so bots capture inside radius without stacking.
    const float ang = randFloat(0.0f, 2.0f * kPi);
    const float rad = randFloat(2.5f, 7.5f);
    outGoal.x += std::sin(ang) * rad;
    outGoal.z += std::cos(ang) * rad;
    return true;
}

bool BotBrain::pickRandomWalkableNavPoint(Vec3& outGoal) {
    if (!m_navMesh || !m_navMesh->isLoaded() || m_navMesh->polygonCount() == 0u) {
        return false;
    }
    const uint32_t count = m_navMesh->polygonCount();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const uint32_t idx = static_cast<uint32_t>(randInt(0, static_cast<int32_t>(count - 1u)));
        if (m_navMesh->isWalkablePolygon(idx)) {
            const NavPoint c = m_navMesh->getPolygonCentroid(idx);
            outGoal = Vec3(c.x, c.y, c.z);
            return true;
        }
    }
    return false;
}

void BotBrain::computeFriendlySeparation(const BotWorldView& world,
                                         float& outSepX, float& outSepZ) const {
    outSepX = 0.0f;
    outSepZ = 0.0f;
    if (!world.soldiers || !world.hasOwnPos) {
        return;
    }
    static const float kSepRadius = 2.4f;
    static const float kSepRadiusSq = kSepRadius * kSepRadius;

    for (std::map<uint16_t, TrackedSoldier>::const_iterator it = world.soldiers->begin();
         it != world.soldiers->end(); ++it) {
        const TrackedSoldier& s = it->second;
        if (s.netId == world.ownSoldierNetId || !s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) {
            continue;
        }
        if (s.team != world.ownTeam) {
            continue;
        }
        const float dx = world.ownPos.x - s.pos.x;
        const float dz = world.ownPos.z - s.pos.z;
        const float d2 = dx * dx + dz * dz;
        if (d2 > 0.001f && d2 < kSepRadiusSq) {
            const float d = std::sqrt(d2);
            const float weight = (kSepRadius - d) / kSepRadius;
            outSepX += (dx / d) * weight;
            outSepZ += (dz / d) * weight;
        }
    }
}

void BotBrain::updateNavigation(uint64_t nowUs, const BotWorldView& world,
                                bool inCombat, Vec3& outMoveTarget, bool& outHasMoveTarget,
                                uint32_t& outNavButtons) {
    outHasMoveTarget = false;
    outNavButtons = 0u;

    if (!world.hasOwnPos) {
        return;
    }

    // Refresh TeamTactics claim heartbeat if holding an active objective.
    if (m_teamTactics && m_hasActiveObjective) {
        m_teamTactics->claimObjective(m_team, m_botIndex, m_activeObjectiveKey, nowUs);
    }

    // Monitor staged frontline transitions (e.g. Hotel captured -> shift immediately to Square)
    // and let defenders hold their tactical perimeter angle around the active flag.
    if (!m_aim.hasGoal && !m_aim.keepRunning && !m_aim.frontline.empty()) {
        ActiveFrontlineStage flStage;
        if (resolveActiveFrontlineStage(world.controlPoints, world.spawnGroups, flStage)) {
            if (m_hasActiveObjective && m_activeObjectiveKey != flStage.objKey) {
                // Frontline shifted (e.g. Hotel fell -> fall back / push to Square immediately!).
                m_hasActiveObjective = false;
                m_hasDefendPost = false;
                m_defendHoldUntilUs = 0;
                m_objectiveHoldStartUs = 0;
                m_waypoints.clear();
                m_waypointIdx = 0;
                m_nextPathComputeUs = nowUs;
                m_hasPatrolGoal = false;
            } else if (m_hasActiveObjective && m_activeObjectiveFriendly) {
                if (flStage.isNeutral || flStage.ownerTeam != m_team || flStage.underCapture) {
                    // Defended flag is being neutralized/captured -> break hold & sprint to flagpole!
                    m_activeObjectiveFriendly = false;
                    m_hasDefendPost = false;
                    m_defendHoldUntilUs = 0;
                    m_waypoints.clear();
                    m_waypointIdx = 0;
                    m_nextPathComputeUs = nowUs;
                } else if (!inCombat && m_hasDefendPost) {
                    if (distXZ(world.ownPos, m_defendPostPos) < 3.5f) {
                        if (m_defendHoldUntilUs == 0) {
                            m_defendHoldUntilUs = nowUs + static_cast<uint64_t>(randInt(5000, 9000)) * 1000ULL;
                        }
                        if (nowUs < m_defendHoldUntilUs) {
                            // Hold defensive cover position & scan approach lanes (handled in tickOutOfCombat).
                            return;
                        }
                        // Shift to a fresh cover angle around the defended flag.
                        m_defendHoldUntilUs = 0;
                        m_hasDefendPost = false;
                        m_waypoints.clear();
                        m_waypointIdx = 0;
                        m_nextPathComputeUs = nowUs;
                    }
                }
            }
        }
    }

    // Check if we are actively assaulting or capturing an enemy/neutral flag (BF2 Coop two-stage
    // flag assault: AttackStagingCover + SquadRallyWait -> Breach -> Wall-Hugging Flag Cap!).
    if (!inCombat && m_hasActiveObjective && !m_activeObjectiveFriendly &&
        !m_aim.hasGoal && !m_aim.keepRunning) {
        bool nowFriendly = false;
        const bool stillExists = isObjectiveStillActive(world, m_activeObjectiveKey, nowFriendly);
        const float distToObj = distXZ(world.ownPos, m_activeObjectivePos);

        if (!stillExists || nowFriendly) {
            // Flag captured! Move on to the next frontline objective immediately.
            m_hasActiveObjective = false;
            m_objectiveHoldStartUs = 0;
            m_isStagingAssault = false;
            m_stagingHoldUntilUs = 0;
            m_hasCapCoverPos = false;
            m_nextPathComputeUs = nowUs;
            m_hasPatrolGoal = false;
        } else if (distToObj < 11.0f) {
            m_isStagingAssault = false;
            m_stagingHoldUntilUs = 0;
            if (m_objectiveHoldStartUs == 0) {
                m_objectiveHoldStartUs = nowUs;
            }
            // Hold the capture zone for up to 22s (timeout handles uncapturable enemy mains).
            if ((nowUs - m_objectiveHoldStartUs) < 22000000ULL) {
                if (m_navMesh && m_navMesh->isLoaded()) {
                    // BF2 Coop CoverPositionManager::RandomizeCoverPosAlongEdge:
                    // Hug a wall/pillar inside the capture radius instead of standing in the open!
                    if (!m_hasCapCoverPos || nowUs >= m_capCoverRefreshUs) {
                        NavPoint capPt;
                        const uint32_t seed = m_botIndex * 31u + static_cast<uint32_t>((nowUs / 7000000ULL) & 0xFFu);
                        if (m_navMesh->findWallHuggingPoint(
                                m_activeObjectivePos.x, m_activeObjectivePos.y, m_activeObjectivePos.z,
                                2.0f, 8.2f,
                                m_activeObjectivePos.x, m_activeObjectivePos.z + 45.0f,
                                seed, capPt)) {
                            m_capCoverPos = Vec3(capPt.x, capPt.y, capPt.z);
                            m_hasCapCoverPos = true;
                            m_capCoverRefreshUs = nowUs + static_cast<uint64_t>(randInt(6500, 9500)) * 1000ULL;
                        }
                    }
                    if (m_hasCapCoverPos && distXZ(world.ownPos, m_capCoverPos) > 1.8f) {
                        outMoveTarget = m_capCoverPos;
                        outHasMoveTarget = true;
                    }
                } else if (distToObj > 7.5f) {
                    outMoveTarget = m_activeObjectivePos;
                    outHasMoveTarget = true;
                }
                return;
            } else {
                // Likely an uncapturable base — temporarily blacklist and pick another flag.
                m_blacklistedObjKey = m_activeObjectiveKey;
                m_blacklistUntilUs = nowUs + 25000000ULL;
                m_hasActiveObjective = false;
                m_objectiveHoldStartUs = 0;
                m_hasCapCoverPos = false;
                m_nextPathComputeUs = nowUs;
            }
        } else if (m_navMesh && m_navMesh->isLoaded() && distToObj >= 14.0f && distToObj <= 34.0f &&
                   m_lastStagedObjKey != m_activeObjectiveKey) {
            // BF2 Coop pre-breach staging (getClosestAttackCoverPosition + updateWaitForSquadMembers):
            // Stage briefly behind a corner 15..26m out when defenders are present or squadmates are
            // rallying behind us, unless a squadmate is already inside the cap radius (isSquadTakingFlag)!
            uint32_t squadFiring = 0u;
            bool squadTakingFlag = false;
            bool shouldRallyWait = false;
            bool maneuverAdv = false;
            if (m_teamTactics) {
                m_teamTactics->querySquadCohesion(m_team, m_botIndex, world.ownPos,
                                                  m_activeObjectiveKey, m_activeObjectivePos, nowUs,
                                                  squadFiring, squadTakingFlag, shouldRallyWait, maneuverAdv);
            }

            int32_t defendersNear = 0;
            if (world.soldiers) {
                for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->begin();
                     sit != world.soldiers->end(); ++sit) {
                    const TrackedSoldier& s = sit->second;
                    if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                    if (s.team == 0 || s.team == m_team) continue;
                    if (distXZ(m_activeObjectivePos, s.pos) < 28.0f) {
                        ++defendersNear;
                    }
                }
            }
            if (defendersNear == 0 && m_teamTactics) {
                defendersNear = static_cast<int32_t>(
                    m_teamTactics->countSpottedNear(m_team, m_activeObjectivePos, 28.0f, nowUs) +
                    m_teamTactics->dangerHeatNear(m_team, m_activeObjectivePos, 24.0f, nowUs));
            }

            if (squadTakingFlag) {
                // Squadmate is already capping -> commit immediately to breach!
                m_isStagingAssault = false;
                m_stagingHoldUntilUs = 0;
                m_lastStagedObjKey = m_activeObjectiveKey;
            } else if (defendersNear > 0 || shouldRallyWait) {
                if (!m_isStagingAssault) {
                    NavPoint stageCov;
                    if (m_navMesh->findAttackStagingCover(
                            world.ownPos.x, world.ownPos.y, world.ownPos.z,
                            m_activeObjectivePos.x, m_activeObjectivePos.y, m_activeObjectivePos.z,
                            15.0f, 26.0f, m_botIndex + 1u, stageCov)) {
                        m_stagingCoverPos = Vec3(stageCov.x, stageCov.y, stageCov.z);
                        m_isStagingAssault = true;
                        m_stagingHoldUntilUs = 0;
                    } else {
                        m_lastStagedObjKey = m_activeObjectiveKey;
                    }
                }
                if (m_isStagingAssault) {
                    if (distXZ(world.ownPos, m_stagingCoverPos) > 2.6f) {
                        outMoveTarget = m_stagingCoverPos;
                        outHasMoveTarget = true;
                        return;
                    }
                    if (m_stagingHoldUntilUs == 0) {
                        m_stagingHoldUntilUs = nowUs + static_cast<uint64_t>(randInt(1800, 3100)) * 1000ULL;
                    }
                    if (nowUs < m_stagingHoldUntilUs) {
                        // Hold staging corner crouched, scanning the flag defenders & letting squad rally!
                        m_defendWatchPoint = m_activeObjectivePos;
                        m_hasDefendWatchPoint = true;
                        return;
                    }
                    // Staging complete -> breach the flag together!
                    m_isStagingAssault = false;
                    m_stagingHoldUntilUs = 0;
                    m_lastStagedObjKey = m_activeObjectiveKey;
                    m_waypoints.clear();
                    m_waypointIdx = 0;
                    m_nextPathComputeUs = nowUs;
                }
            }
        }
    }

    // Pursuit of a recently seen enemy who broke LOS: push toward their last-known flank!
    if (!inCombat && m_hasLastKnownEnemy && !m_aim.hasGoal &&
        (nowUs - m_lastKnownEnemyUs) < 4200000ULL) {
        const float dLast = distXZ(world.ownPos, m_lastKnownEnemyPos);
        if (dLast > 4.0f && dLast < 42.0f) {
            if (m_navMesh && m_navMesh->isLoaded()) {
                if (m_waypoints.empty() || m_waypointIdx >= m_waypoints.size() || nowUs >= m_nextPathComputeUs) {
                    std::vector<NavPoint> pursuitPath;
                    if (m_navMesh->findSmoothedPath(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                                    m_lastKnownEnemyPos.x, m_lastKnownEnemyPos.y, m_lastKnownEnemyPos.z,
                                                    pursuitPath, 0u) && !pursuitPath.empty()) {
                        m_waypoints.swap(pursuitPath);
                        m_waypointIdx = 0;
                        m_nextPathComputeUs = nowUs + 1200000ULL;
                    }
                }
                if (m_waypointIdx < m_waypoints.size()) {
                    const NavPoint& wp = m_waypoints[m_waypointIdx];
                    if (distXZ(world.ownPos, Vec3(wp.x, wp.y, wp.z)) < 2.2f && m_waypointIdx + 1u < m_waypoints.size()) {
                        ++m_waypointIdx;
                    }
                    const NavPoint& curWp = m_waypoints[m_waypointIdx];
                    outMoveTarget = Vec3(curWp.x, curWp.y, curWp.z);
                    outHasMoveTarget = true;
                    return; // Tactical advance (no sprint!) while hunting hidden enemy
                }
            } else {
                outMoveTarget = m_lastKnownEnemyPos;
                outHasMoveTarget = true;
                return;
            }
        }
    }

    // Tier 1: NavMesh funnel-smoothed A* pathing with route diversity & flanking
    if (m_navMesh && m_navMesh->isLoaded()) {
        bool needRecompute = m_waypoints.empty() ||
                             m_waypointIdx >= m_waypoints.size() ||
                             nowUs >= m_nextPathComputeUs;

        if (!needRecompute) {
            const NavPoint& wp = m_waypoints[m_waypointIdx];
            if (distXZ(world.ownPos, Vec3(wp.x, wp.y, wp.z)) < 2.4f) {
                ++m_waypointIdx;
                if (m_waypointIdx >= m_waypoints.size()) {
                    needRecompute = true;
                }
            }
        }

        if (needRecompute) {
            Vec3 goal;
            bool hasGoal = pickObjectiveGoal(nowUs, world, goal);
            if (!hasGoal) {
                hasGoal = pickRandomWalkableNavPoint(goal);
            }
            if (hasGoal) {
                // DICE Coop StrategicAreas.ai multi-lane squad corridors + flankers:
                const uint32_t divSeed = (m_botIndex + 1u) * 17u;
                const uint32_t squadId = TeamTactics::squadForBot(m_botIndex);
                std::vector<NavPoint> newPath;
                bool usedFlank = false;

                const float distGoal = distXZ(world.ownPos, goal);
                const bool hotZone = m_teamTactics &&
                    (m_teamTactics->dangerHeatNear(m_team,
                        Vec3(0.5f * (world.ownPos.x + goal.x), world.ownPos.y, 0.5f * (world.ownPos.z + goal.z)),
                        28.0f, nowUs) >= 2u);

                // 1) Try authentic DICE Coop StrategicAreas.ai multi-street waypoints (e.g. USBASE_LEFTFLANK /
                // USBASE_RIGHTFLANK -> Hotel, or 4-lane Hotel -> Square) with SquadFormation line spread!
                std::vector<Vec3> coopWps;
                if (!m_aim.hasGoal && m_teamTactics &&
                    m_teamTactics->selectKarkandCoopWaypoints(m_team, squadId, world.ownPos, goal, nowUs, coopWps)) {
                    static const float kSlotSpread[4] = { -3.2f, -1.1f, 1.1f, 3.2f };
                    const float latOff = kSlotSpread[TeamTactics::slotInSquadForBot(m_botIndex) & 3u];
                    const float dirLen = (distGoal > 1.0f) ? distGoal : 1.0f;
                    const float perpX = -(goal.z - world.ownPos.z) / dirLen;
                    const float perpZ =  (goal.x - world.ownPos.x) / dirLen;

                    Vec3 curStart = world.ownPos;
                    std::vector<NavPoint> chain;
                    bool chainOk = true;
                    for (size_t w = 0; w <= coopWps.size(); ++w) {
                        Vec3 targetPt = (w < coopWps.size()) ? coopWps[w] : goal;
                        if (w < coopWps.size()) {
                            const float sx = targetPt.x + perpX * latOff;
                            const float sz = targetPt.z + perpZ * latOff;
                            if (m_navMesh->isPointWalkable(sx, targetPt.y, sz)) {
                                targetPt.x = sx;
                                targetPt.z = sz;
                            }
                        }
                        std::vector<NavPoint> leg;
                        if (!m_navMesh->findSmoothedPath(curStart.x, curStart.y, curStart.z,
                                                         targetPt.x, targetPt.y, targetPt.z,
                                                         leg, divSeed) || leg.empty()) {
                            chainOk = false;
                            break;
                        }
                        chain.insert(chain.end(), leg.begin(), leg.end());
                        curStart = targetPt;
                    }
                    if (chainOk && !chain.empty()) {
                        newPath.swap(chain);
                        usedFlank = true;
                    }
                }

                // 2) Generic dynamic lateral flank when not on a named Coop corridor:
                if (!usedFlank && (m_role == ROLE_FLANKER || hotZone) && distGoal > 38.0f && !m_aim.hasGoal) {
                    NavPoint flankPt;
                    const float side = TeamTactics::flankSideForBot(m_botIndex);
                    const float off = randFloat(20.0f, 34.0f);
                    if (m_navMesh->findFlankWaypoint(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                                     goal.x, goal.y, goal.z, side, off, flankPt)) {
                        std::vector<NavPoint> leg1;
                        std::vector<NavPoint> leg2;
                        if (m_navMesh->findSmoothedPath(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                                        flankPt.x, flankPt.y, flankPt.z, leg1, divSeed) &&
                            !leg1.empty() &&
                            m_navMesh->findSmoothedPath(flankPt.x, flankPt.y, flankPt.z,
                                                        goal.x, goal.y, goal.z, leg2, divSeed) &&
                            !leg2.empty()) {
                            newPath = leg1;
                            newPath.insert(newPath.end(), leg2.begin(), leg2.end());
                            usedFlank = true;
                        }
                    }
                }

                if (!usedFlank) {
                    m_navMesh->findSmoothedPath(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                                goal.x, goal.y, goal.z, newPath, divSeed);
                }

                if (!newPath.empty()) {
                    m_waypoints.swap(newPath);
                    m_waypointIdx = 0;
                    if (m_waypoints.size() > 1u &&
                        distXZ(world.ownPos, Vec3(m_waypoints[0].x, m_waypoints[0].y, m_waypoints[0].z)) < 2.0f) {
                        m_waypointIdx = 1;
                    }
                } else {
                    m_waypoints.clear();
                    m_waypointIdx = 0;
                    outMoveTarget = goal;
                    outHasMoveTarget = true;
                }
            }
            m_nextPathComputeUs = nowUs + static_cast<uint64_t>(randInt(4000, 6500)) * 1000ULL;
        }

        if (m_waypointIdx < m_waypoints.size()) {
            const NavPoint& wp = m_waypoints[m_waypointIdx];
            outMoveTarget = Vec3(wp.x, wp.y, wp.z);
            outHasMoveTarget = true;
            // BF2 Coop m_slowDownToTarget: drop out of sprint within 24m of a hostile/contested
            // flag so the rifle is shouldered and ready to fire around the final corner!
            const bool approachingHostileFlag = m_hasActiveObjective && !m_activeObjectiveFriendly &&
                                                !m_aim.keepRunning &&
                                                distXZ(world.ownPos, m_activeObjectivePos) < 24.0f;
            if (!inCombat && !approachingHostileFlag) {
                outNavButtons |= BTN_SPRINT;
            }
            return;
        }
    }

    // Tier 2: Recorded RouteLoader playback
    if (m_routeLoader && m_routeLoader->isLoaded() && m_routeLoader->trackCount() > 0u) {
        if (!m_hasRouteTrack) {
            size_t tIdx = 0;
            size_t sIdx = 0;
            if (m_routeLoader->pickTrackForPosition(static_cast<uint8_t>(m_team),
                                                    world.ownPos.x, world.ownPos.z,
                                                    static_cast<uint32_t>(m_rng()),
                                                    tIdx, sIdx)) {
                m_hasRouteTrack = true;
                m_routeTrackIdx = tIdx;
                m_routeSampleIdx = sIdx;
                m_lastRouteAdvanceUs = nowUs;
            }
        }

        if (m_hasRouteTrack) {
            const RouteTrack& track = m_routeLoader->getTrack(m_routeTrackIdx);
            if (m_routeSampleIdx >= track.samples.size()) {
                m_hasRouteTrack = false;
            } else {
                const RouteActionSample& sample = track.samples[m_routeSampleIdx];
                Vec3 samplePos(sample.posX, sample.posY, sample.posZ);

                if (distXZ(world.ownPos, samplePos) < 2.5f ||
                    (nowUs > m_lastRouteAdvanceUs && (nowUs - m_lastRouteAdvanceUs) > 2500000ULL)) {
                    ++m_routeSampleIdx;
                    m_lastRouteAdvanceUs = nowUs;
                    if (m_routeSampleIdx >= track.samples.size()) {
                        m_hasRouteTrack = false;
                    }
                }

                if (m_hasRouteTrack) {
                    const RouteActionSample& cur = track.samples[m_routeSampleIdx];
                    outMoveTarget = Vec3(cur.posX, cur.posY, cur.posZ);
                    outHasMoveTarget = true;

                    if (!inCombat) {
                        if (cur.stance == STANCE_CROUCH || (cur.actionFlags & AF_CROUCH) != 0u) {
                            outNavButtons |= BTN_CROUCH;
                        } else if (cur.stance == STANCE_PRONE || (cur.actionFlags & AF_PRONE) != 0u) {
                            outNavButtons |= BTN_PRONE;
                        } else if ((cur.actionFlags & AF_SPRINT) != 0u) {
                            outNavButtons |= BTN_SPRINT;
                        }
                        if ((cur.actionFlags & AF_JUMP) != 0u) {
                            outNavButtons |= BTN_JUMP;
                        }
                    }
                    return;
                }
            }
        }
    }

    // Tier 3: Fallback patrol toward ControlPoints / enemy SpawnGroups / circling anchors
    if (!m_hasPatrolGoal || nowUs >= m_nextPatrolGoalUs ||
        distXZ(world.ownPos, m_patrolGoal) < 4.0f) {
        Vec3 goal;
        if (!pickObjectiveGoal(nowUs, world, goal)) {
            const Vec3 anchor = m_hasSpawnAnchor ? m_spawnAnchor : world.ownPos;
            const float angle = randFloat(0.0f, 2.0f * kPi);
            const float radius = randFloat(35.0f, 110.0f);
            const float halfWorld = (world.worldSize > 64.0f ? world.worldSize : 1024.0f) * 0.45f;
            goal.x = clampFloat(anchor.x + std::sin(angle) * radius, -halfWorld, halfWorld);
            goal.y = anchor.y;
            goal.z = clampFloat(anchor.z + std::cos(angle) * radius, -halfWorld, halfWorld);
        }
        m_patrolGoal = goal;
        m_hasPatrolGoal = true;
        m_nextPatrolGoalUs = nowUs + static_cast<uint64_t>(randInt(6000, 12000)) * 1000ULL;
    }

    outMoveTarget = m_patrolGoal;
    outHasMoveTarget = true;
    if (!inCombat) {
        outNavButtons |= BTN_SPRINT;
    }
}

bool interpolateSoldierPos(const TrackedSoldier& s, uint64_t renderUs, Vec3& out) {
    if (s.histCount == 0u) {
        return false;
    }
    const PosSample& oldest = s.histAt(0);
    const PosSample& newest = s.histAt(static_cast<size_t>(s.histCount) - 1u);
    if (renderUs <= oldest.us) {
        out = oldest.pos;
        return true;
    }
    if (renderUs >= newest.us) {
        out = newest.pos;
        return true;
    }
    for (size_t i = 1; i < s.histCount; ++i) {
        const PosSample& b = s.histAt(i);
        if (renderUs <= b.us) {
            const PosSample& a = s.histAt(i - 1u);
            const uint64_t span = b.us - a.us;
            if (span == 0u) {
                out = b.pos;
                return true;
            }
            const float t = static_cast<float>(renderUs - a.us) / static_cast<float>(span);
            out.x = a.pos.x + (b.pos.x - a.pos.x) * t;
            out.y = a.pos.y + (b.pos.y - a.pos.y) * t;
            out.z = a.pos.z + (b.pos.z - a.pos.z) * t;
            return true;
        }
    }
    out = newest.pos;
    return true;
}

bool estimateSoldierVelocity(const TrackedSoldier& s, Vec3& outVel) {
    if (s.histCount >= 2u) {
        // Compute smoothed velocity over up to the last 4 samples (~100-150ms).
        const size_t newestIdx = static_cast<size_t>(s.histCount) - 1u;
        const size_t backSteps = (newestIdx >= 3u) ? 3u : newestIdx;
        const PosSample& a = s.histAt(newestIdx - backSteps);
        const PosSample& b = s.histAt(newestIdx);
        if (b.us > a.us) {
            const float dt = static_cast<float>(b.us - a.us) / 1000000.0f;
            if (dt > 0.01f && dt < 1.0f) {
                Vec3 v((b.pos.x - a.pos.x) / dt,
                       (b.pos.y - a.pos.y) / dt,
                       (b.pos.z - a.pos.z) / dt);
                const float spd = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                if (spd > 9.5f) {
                    const float sc = 9.5f / spd;
                    v.x *= sc;
                    v.y *= sc;
                    v.z *= sc;
                }
                outVel = v;
                return true;
            }
        }
    }
    const float spdSq = s.speed.x * s.speed.x + s.speed.y * s.speed.y + s.speed.z * s.speed.z;
    if (spdSq > 0.0001f) {
        outVel = s.speed;
        return true;
    }
    outVel = Vec3();
    return false;
}

float aimErrorDecay(float initialDeg, uint64_t elapsedUs, uint64_t convergeUs) {
    if (convergeUs == 0u) {
        return 0.0f;
    }
    const float t = static_cast<float>(elapsedUs) / static_cast<float>(convergeUs);
    return initialDeg * std::exp(-3.0f * t);
}

bool terrainLineOfSight(const NavMesh* nav, const Vec3& a, const Vec3& b) {
    if (!nav || !nav->hasAerialHeightMap()) {
        return true;
    }
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 8.0f) {
        return true;
    }
    // The aerial map's absolute offset to the ground is unknown, so only the bump above the
    // chord between both endpoints is considered (an eye is ~1.5 m above the ground).
    const float ha = nav->sampleAerialHeight(a.x, a.z, 0.0f);
    const float hb = nav->sampleAerialHeight(b.x, b.z, 0.0f);
    const int steps = static_cast<int>(len / 4.0f) + 1;
    for (int i = 1; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float chord = ha + (hb - ha) * t;
        const float h = nav->sampleAerialHeight(a.x + dx * t, a.z + dz * t, chord);
        if (h - chord > 2.0f) {
            return false;
        }
    }
    return true;
}

void BotBrain::resetFireState() {
    m_fireState = FIRE_WAIT;
    m_fireStateEndUs = 0;
    m_burstShots = 0;
}

bool BotBrain::selectTarget(uint64_t nowUs, const BotWorldView& world,
                            const TrackedSoldier*& outTarget, Vec3& outRendered,
                            float& outDist, int32_t& outVisibleEnemyCount) {
    outTarget = nullptr;
    outVisibleEnemyCount = 0;
    if (!world.soldiers) {
        return false;
    }

    const uint64_t interpUs = static_cast<uint64_t>(m_aim.interpMs > 0 ? m_aim.interpMs : 0) * 1000ULL;
    const uint64_t renderUs = nowUs > interpUs ? nowUs - interpUs : 0ULL;
    const float aimYaw = currentAimYaw(world);
    const bool underFire = (nowUs < m_underFireUntilUs);

    SpottedEnemy nearestSpot;
    const bool hasTeamSpot = m_teamTactics &&
        m_teamTactics->findNearestSpotted(world.ownTeam, world.ownPos, 65.0f, nowUs, nearestSpot);

    float bestScore = -1e9f;

    for (std::map<uint16_t, TrackedSoldier>::const_iterator it = world.soldiers->begin();
         it != world.soldiers->end(); ++it) {
        const TrackedSoldier& s = it->second;
        if (s.netId == world.ownSoldierNetId || !s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) {
            continue;
        }
        if (s.team == 0 || s.team == world.ownTeam) {
            continue;
        }

        Vec3 r = s.pos;
        if (!interpolateSoldierPos(s, renderUs, r)) {
            r = s.pos;
        }

        const bool isCurrent = (s.netId == m_targetNetId);
        const float d = dist3D(world.ownPos, r);
        const float maxRange = isCurrent ? (kMaxHumanEngageRange + 10.0f) : kMaxHumanEngageRange;
        if (d > maxRange) {
            continue;
        }

        const float bearing = std::atan2(r.x - world.ownPos.x, r.z - world.ownPos.z) * kRadToDeg;
        const float off = std::fabs(wrapAngleDeg(bearing - aimYaw));

        const bool recentlyFired = s.isFiring ||
            (s.lastFiredUs != 0 && nowUs >= s.lastFiredUs && (nowUs - s.lastFiredUs) < 1600000ULL);
        Vec3 sVel;
        estimateSoldierVelocity(s, sVel);
        const float speedXZ = std::sqrt(sVel.x * sVel.x + sVel.z * sVel.z);

        // Human-like multi-sensory perception cone:
        // - Footsteps/proximity (<12m, or <20m if running): 360 deg
        // - Under fire (just took damage): 360 deg
        // - Actively firing enemy: 360 deg within 45m, +/-135 deg within 85m
        // - Team-spotted enemy on minimap: +/-120 deg within 60m
        // - Normal visual cone: +/-55 deg (+/-85 deg sticky)
        bool perceived = false;
        if (d < 12.0f || (d < 20.0f && speedXZ > 2.8f) || underFire) {
            perceived = true;
        } else if (recentlyFired && (d < 45.0f || off <= 135.0f)) {
            perceived = true;
        } else if (hasTeamSpot && nearestSpot.enemyNetId == s.netId && d < 60.0f && off <= 120.0f) {
            perceived = true;
        } else if (off <= (isCurrent ? 85.0f : 55.0f)) {
            perceived = true;
        }

        if (!perceived) {
            continue;
        }

        // Hard wall & terrain line-of-sight gate: bots NEVER lock on or shoot through walls,
        // buildings, compound fences, or terrain hills. Also check the enemy's latest live
        // position so bots stop firing immediately when an enemy steps behind a wall corner.
        if (!hasClearLineOfSight(world, r, s.pose)) {
            continue;
        }
        if (dist3D(r, s.pos) > 0.30f && !hasClearLineOfSight(world, s.pos, s.pose)) {
            continue;
        }

        ++outVisibleEnemyCount;

        // Threat scoring: prioritize close enemies, enemies aiming/firing at us, and wounded kills.
        float score = 110.0f / (d + 5.0f);

        // Is this enemy facing toward us?
        const float enemyToUsBearing = std::atan2(world.ownPos.x - r.x, world.ownPos.z - r.z) * kRadToDeg;
        const float enemyLookOff = std::fabs(wrapAngleDeg(enemyToUsBearing - s.facingDegrees));
        if (enemyLookOff < 25.0f) {
            score += 8.5f;
        } else if (enemyLookOff < 55.0f) {
            score += 3.5f;
        }

        if (recentlyFired) {
            score += 5.5f;
        }
        if (s.hitPointsByte < 140u) {
            score += 3.5f; // finish wounded enemy
        }
        if (d < 12.0f) {
            score += 14.0f; // lethal close-quarters emergency priority
        }
        score += (1.0f - off / 180.0f) * 3.5f;

        if (isCurrent) {
            score += 5.5f; // hysteresis against target ping-pong
        }

        if (score > bestScore) {
            bestScore = score;
            outTarget = &s;
            outRendered = r;
            outDist = d;
        }
    }

    return outTarget != nullptr;
}

void BotBrain::updateDamageAndMemory(uint64_t nowUs, const BotWorldView& world,
                                     const TrackedSoldier* bestTarget, const Vec3& rendered) {
    // Detect incoming damage via HP byte drop.
    if (m_hasLastHp && world.ownHitPointsByte < m_lastOwnHp) {
        m_underFireUntilUs = nowUs + 5000000ULL;
        m_lastDamageUs = nowUs;
    }
    m_lastOwnHp = world.ownHitPointsByte;
    m_hasLastHp = true;

    if (bestTarget) {
        m_lastCombatUs = nowUs;
        m_hasLastKnownEnemy = true;
        m_lastKnownEnemyNetId = bestTarget->netId;
        m_lastKnownEnemyPos = rendered;
        estimateSoldierVelocity(*bestTarget, m_lastKnownEnemyVel);
        m_lastKnownEnemyUs = nowUs;

        if (m_teamTactics) {
            m_teamTactics->reportSpottedEnemy(world.ownTeam, bestTarget->netId,
                                              rendered, m_lastKnownEnemyVel, nowUs);
        }
    } else if (m_hasLastKnownEnemy) {
        // Expire memory if target died or memory is stale (> 5.0s).
        if ((nowUs - m_lastKnownEnemyUs) > 5000000ULL) {
            m_hasLastKnownEnemy = false;
        } else if (world.soldiers) {
            std::map<uint16_t, TrackedSoldier>::const_iterator it =
                world.soldiers->find(m_lastKnownEnemyNetId);
            if (it != world.soldiers->end() && (it->second.isRagdoll || it->second.hitPointsByte == 0u)) {
                if (m_teamTactics) {
                    m_teamTactics->clearSpottedEnemy(world.ownTeam, m_lastKnownEnemyNetId);
                }
                m_hasLastKnownEnemy = false;
            }
        }
    }
}

void BotBrain::startUtility(UtilityAction action, uint64_t nowUs,
                            const Vec3& aimPos, uint16_t targetNetId) {
    m_utilAction = action;
    m_utilPhase = UPHASE_SWITCH;
    m_utilPhaseStartUs = nowUs;
    m_utilTargetPos = aimPos;
    m_utilTargetNetId = targetNetId;
    m_utilDeadlineUs = nowUs + (action == UTIL_REVIVE ? 6000000ULL :
                                action == UTIL_PISTOL ? 3800000ULL : 2800000ULL);
    resetFireState();
}

void BotBrain::requestPrimaryRestore(uint64_t nowUs) {
    if (m_utilAction != UTIL_NONE && m_utilPhase != UPHASE_RESTORE) {
        m_utilPhase = UPHASE_RESTORE;
        m_utilPhaseStartUs = nowUs;
    }
}

bool BotBrain::tryStartUtilityAction(uint64_t nowUs, const BotWorldView& world,
                                     const TrackedSoldier* bestTarget,
                                     const Vec3& targetRendered, float targetDist) {
    if (m_utilAction != UTIL_NONE || m_aim.legacy || m_aim.keepRunning) {
        return false;
    }

    const int32_t kit = activeKit(world);
    const bool immediateCqb = (bestTarget != nullptr && targetDist < 12.0f);

    // 1. Medic (kit 5): Revive nearby man-down friendly when not in immediate CQB.
    if (kit == 5 && !immediateCqb && nowUs >= m_nextReviveUs && world.players && world.soldiers) {
        float bestD = 18.0f;
        Vec3 revivePos;
        uint16_t reviveNetId = 0;
        for (std::map<uint8_t, TrackedPlayer>::const_iterator pit = world.players->begin();
             pit != world.players->end(); ++pit) {
            const TrackedPlayer& p = pit->second;
            if (p.playerId == world.ownPlayerId || p.team != world.ownTeam || !p.isManDown) {
                continue;
            }
            if (p.soldierNetId == 0u) continue;
            std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->find(p.soldierNetId);
            if (sit == world.soldiers->end() || !sit->second.hasPos) continue;
            const float d = distXZ(world.ownPos, sit->second.pos);
            if (d < bestD) {
                bestD = d;
                revivePos = sit->second.pos;
                reviveNetId = p.soldierNetId;
            }
        }
        if (reviveNetId != 0u) {
            m_nextReviveUs = nowUs + 9000000ULL;
            startUtility(UTIL_REVIVE, nowUs, revivePos, reviveNetId);
            return true;
        }
    }

    // 2. Medic (kit 5): Drop Medkit when self or nearby friendly is wounded (< 65% HP).
    if (kit == 5 && !immediateCqb && nowUs >= m_nextMedkitUs) {
        bool needHeal = (world.ownHitPointsByte < 165u);
        Vec3 healPos = world.ownPos;
        if (!needHeal && world.soldiers) {
            for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->begin();
                 sit != world.soldiers->end(); ++sit) {
                const TrackedSoldier& s = sit->second;
                if (s.netId == world.ownSoldierNetId || s.team != world.ownTeam) continue;
                if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                if (s.hitPointsByte < 165u && distXZ(world.ownPos, s.pos) < 9.0f) {
                    needHeal = true;
                    healPos = s.pos;
                    break;
                }
            }
        }
        if (needHeal) {
            m_nextMedkitUs = nowUs + 14000000ULL;
            startUtility(UTIL_MEDKIT, nowUs, healPos, 0u);
            return true;
        }
    }

    // 3. Support (kit 3): Drop Ammo Bag near teammates or after sustained firing.
    if (kit == 3 && !bestTarget && nowUs >= m_nextAmmoBagUs) {
        int32_t nearbyAllies = 0;
        if (world.soldiers) {
            for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->begin();
                 sit != world.soldiers->end(); ++sit) {
                const TrackedSoldier& s = sit->second;
                if (s.netId == world.ownSoldierNetId || s.team != world.ownTeam) continue;
                if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                if (distXZ(world.ownPos, s.pos) < 10.0f) {
                    ++nearbyAllies;
                }
            }
        }
        if (nearbyAllies >= 2 || m_totalReloads >= 1) {
            m_nextAmmoBagUs = nowUs + 20000000ULL;
            startUtility(UTIL_AMMOBAG, nowUs, world.ownPos, 0u);
            return true;
        }
    }

    // 4. Smart Grenade / M203 toss at enemy cluster or recently hidden enemy behind cover (14..32m).
    if (!immediateCqb && nowUs >= m_nextGrenadeUs && nowUs >= m_reloadUntilUs) {
        Vec3 nadeTarget;
        bool shouldThrow = false;

        if (bestTarget && targetDist >= 15.0f && targetDist <= 32.0f && world.soldiers) {
            // Count enemy cluster near target.
            int32_t cluster = 0;
            for (std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->begin();
                 sit != world.soldiers->end(); ++sit) {
                const TrackedSoldier& s = sit->second;
                if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) continue;
                if (s.team == 0 || s.team == world.ownTeam) continue;
                if (distXZ(targetRendered, s.pos) < 7.0f) {
                    ++cluster;
                }
            }
            if (cluster >= 2) {
                nadeTarget = targetRendered;
                shouldThrow = true;
            }
        } else if (!bestTarget && m_hasLastKnownEnemy &&
                   (nowUs - m_lastKnownEnemyUs) > 400000ULL &&
                   (nowUs - m_lastKnownEnemyUs) < 2500000ULL) {
            const float dLast = distXZ(world.ownPos, m_lastKnownEnemyPos);
            if (dLast >= 14.0f && dLast <= 30.0f) {
                nadeTarget = m_lastKnownEnemyPos;
                shouldThrow = true;
            }
        }

        if (shouldThrow && hasClearLineOfSight(world, nadeTarget, 0u) &&
            !hasFriendNearBlast(world, nadeTarget, 6.5f) &&
            !hasFriendInLineOfFire(world, nadeTarget)) {
            m_nextGrenadeUs = nowUs + static_cast<uint64_t>(randInt(24000, 38000)) * 1000ULL;
            startUtility(UTIL_GRENADE, nowUs, nadeTarget, 0u);
            return true;
        }
    }

    return false;
}

bool BotBrain::tickUtilityAction(uint64_t nowUs, float dtSec, const BotWorldView& world,
                                 const TrackedSoldier* bestTarget, const Vec3& targetRendered,
                                 float targetDist, PlayerActionInput& act) {
    if (m_utilAction == UTIL_NONE) {
        return false;
    }

    const float aimYaw = currentAimYaw(world);
    const float aimPitch = currentAimPitch(world);
    const float mouseScale = effectiveMouseScale(world);

    // Emergency interrupt: if an enemy rushes into CQB while doing non-pistol utility, immediately draw primary!
    if (m_utilAction != UTIL_PISTOL && m_utilPhase != UPHASE_RESTORE &&
        bestTarget != nullptr && targetDist < 11.0f) {
        requestPrimaryRestore(nowUs);
    }

    // Hard deadline safety so no bot can ever get stuck holding a utility item.
    if (m_utilPhase != UPHASE_RESTORE && nowUs >= m_utilDeadlineUs) {
        requestPrimaryRestore(nowUs);
    }

    // Unzoom if somehow still zoomed.
    if (world.ownWeaponZoom > 0u && nowUs >= m_zoomToggleCooldownUs) {
        act.buttonMaskLow |= BTN_ALT_FIRE;
        m_zoomToggleCooldownUs = nowUs + 500000ULL;
    }

    const uint64_t phaseElapsed = nowUs - m_utilPhaseStartUs;

    if (m_utilPhase == UPHASE_SWITCH) {
        uint32_t slotBtn = BTN_WEAPON_3;
        if (m_utilAction == UTIL_MEDKIT || m_utilAction == UTIL_AMMOBAG) {
            slotBtn = BTN_WEAPON_5;
        } else if (m_utilAction == UTIL_REVIVE) {
            slotBtn = BTN_WEAPON_6;
        } else if (m_utilAction == UTIL_GRENADE) {
            slotBtn = BTN_WEAPON_4;
        } else if (m_utilAction == UTIL_PISTOL) {
            slotBtn = BTN_WEAPON_2;
        }

        if (phaseElapsed < 120000ULL) {
            act.buttonMaskLow |= slotBtn;
        }

        // For revive, start moving toward downed ally while switching to paddles.
        if (m_utilAction == UTIL_REVIVE) {
            const float d = distXZ(world.ownPos, m_utilTargetPos);
            if (d > 1.8f) {
                float mx = 0.0f, my = 0.0f;
                steerToward(world.ownPos, aimYaw, aimPitch,
                            Vec3(m_utilTargetPos.x, world.ownPos.y, m_utilTargetPos.z),
                            mouseScale, dtSec, mx, my);
                act.axes[4] = mx;
                act.axes[5] = my;
                act.axes[3] = 1.0f;
            }
        } else if (m_utilAction == UTIL_PISTOL && bestTarget) {
            // Track CQB target while drawing sidearm!
            const float dx = targetRendered.x - world.ownPos.x;
            const float dy = (targetRendered.y + 1.35f) - (world.ownPos.y + 1.55f);
            const float dz = targetRendered.z - world.ownPos.z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            const float dyaw = std::atan2(dx, dz) * kRadToDeg;
            const float dpitch = -std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg;
            steerAngles(aimYaw, aimPitch, dyaw, dpitch, 520.0f, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);
            act.axes[0] = m_strafeRight ? 0.85f : -0.85f;
        }

        const uint64_t switchWait = (m_utilAction == UTIL_PISTOL) ? 550000ULL : 800000ULL;
        if (phaseElapsed >= switchWait) {
            m_utilPhase = UPHASE_ACT;
            m_utilPhaseStartUs = nowUs;
        }
        return true;
    }

    if (m_utilPhase == UPHASE_ACT) {
        if (m_utilAction == UTIL_MEDKIT || m_utilAction == UTIL_AMMOBAG) {
            // Pitch down slightly and toss kit at feet/teammate.
            steerAngles(aimYaw, aimPitch, aimYaw, 28.0f, 320.0f, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);
            if (phaseElapsed < 200000ULL) {
                act.buttonMaskLow |= BTN_FIRE;
            } else {
                requestPrimaryRestore(nowUs);
            }
            return true;
        }

        if (m_utilAction == UTIL_REVIVE) {
            // Refresh downed ally position & check if still man-down.
            bool stillDown = false;
            if (world.players) {
                for (std::map<uint8_t, TrackedPlayer>::const_iterator pit = world.players->begin();
                     pit != world.players->end(); ++pit) {
                    if (pit->second.soldierNetId == m_utilTargetNetId && pit->second.isManDown) {
                        stillDown = true;
                        break;
                    }
                }
            }
            if (!stillDown) {
                requestPrimaryRestore(nowUs);
                return true;
            }
            if (world.soldiers) {
                std::map<uint16_t, TrackedSoldier>::const_iterator sit = world.soldiers->find(m_utilTargetNetId);
                if (sit != world.soldiers->end() && sit->second.hasPos) {
                    m_utilTargetPos = sit->second.pos;
                }
            }
            const float d = distXZ(world.ownPos, m_utilTargetPos);
            const float dx = m_utilTargetPos.x - world.ownPos.x;
            const float dy = (m_utilTargetPos.y + 0.35f) - (world.ownPos.y + 1.1f);
            const float dz = m_utilTargetPos.z - world.ownPos.z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            const float dyaw = std::atan2(dx, dz) * kRadToDeg;
            const float dpitch = -std::atan2(dy, (horiz > 0.1f ? horiz : 0.1f)) * kRadToDeg;
            steerAngles(aimYaw, aimPitch, dyaw, dpitch, 380.0f, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);

            if (d > 2.2f) {
                act.axes[3] = 1.0f;
            } else {
                act.buttonMaskLow |= BTN_CROUCH;
                // Pulse shock paddles.
                if ((phaseElapsed / 250000ULL) % 2ULL == 0ULL) {
                    act.buttonMaskLow |= BTN_FIRE;
                }
                if (phaseElapsed > 900000ULL) {
                    requestPrimaryRestore(nowUs);
                }
            }
            return true;
        }

        if (m_utilAction == UTIL_GRENADE) {
            const float dx = m_utilTargetPos.x - world.ownPos.x;
            const float dy = (m_utilTargetPos.y + 0.5f) - (world.ownPos.y + 1.55f);
            const float dz = m_utilTargetPos.z - world.ownPos.z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            const float dyaw = std::atan2(dx, dz) * kRadToDeg;
            // Ballistic upward lift proportional to throw distance.
            const float liftDeg = clampFloat(horiz * 0.55f, 7.0f, 22.0f);
            const float dpitch = clampFloat(-std::atan2(dy, (horiz > 0.2f ? horiz : 0.2f)) * kRadToDeg - liftDeg,
                                            -55.0f, 35.0f);
            steerAngles(aimYaw, aimPitch, dyaw, dpitch, 460.0f, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);

            const float yawErr = std::fabs(wrapAngleDeg(dyaw - aimYaw));
            if (yawErr < 10.0f && phaseElapsed >= 150000ULL) {
                act.buttonMaskLow |= BTN_FIRE;
                if (phaseElapsed >= 320000ULL) {
                    requestPrimaryRestore(nowUs);
                }
            } else if (phaseElapsed >= 700000ULL) {
                requestPrimaryRestore(nowUs);
            }
            return true;
        }

        if (m_utilAction == UTIL_PISTOL) {
            if (!bestTarget || targetDist > 18.0f) {
                requestPrimaryRestore(nowUs);
                return true;
            }
            const float dx = targetRendered.x - world.ownPos.x;
            const float dy = (targetRendered.y + 1.35f) - (world.ownPos.y + 1.55f);
            const float dz = targetRendered.z - world.ownPos.z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            const float dyaw = std::atan2(dx, dz) * kRadToDeg;
            const float dpitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                            -75.0f, 75.0f);
            steerAngles(aimYaw, aimPitch, dyaw, dpitch, 560.0f, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);

            const float off = std::fabs(wrapAngleDeg(dyaw - aimYaw));
            // Semi-auto rapid trigger pulses for BF2 pistols (holding fire if teammate crosses muzzle)!
            const bool firePulse = ((phaseElapsed / 110000ULL) % 2ULL == 0ULL);
            const bool friendBlocked = hasFriendInLineOfFire(world, targetRendered);
            if (off < 12.0f && firePulse && !friendBlocked) {
                act.buttonMaskLow |= BTN_FIRE;
            } else {
                act.axes[0] = m_strafeRight ? 0.85f : -0.85f;
            }
            if (phaseElapsed >= 2200000ULL) {
                requestPrimaryRestore(nowUs);
            }
            return true;
        }
    }

    // UPHASE_RESTORE: reliably return to Primary Rifle (slot 3).
    if (phaseElapsed < 140000ULL) {
        act.buttonMaskLow |= BTN_WEAPON_3;
    }
    if (bestTarget) {
        // Keep eyes locked on enemy while switching back to rifle!
        const float dx = targetRendered.x - world.ownPos.x;
        const float dy = (targetRendered.y + 1.35f) - (world.ownPos.y + 1.55f);
        const float dz = targetRendered.z - world.ownPos.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        const float dyaw = std::atan2(dx, dz) * kRadToDeg;
        const float dpitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                        -75.0f, 75.0f);
        steerAngles(aimYaw, aimPitch, dyaw, dpitch, 460.0f, mouseScale, dtSec,
                    act.axes[4], act.axes[5]);
        act.axes[0] = m_strafeRight ? 0.7f : -0.7f;
    }

    if (phaseElapsed >= 650000ULL) {
        const bool wasPistol = (m_utilAction == UTIL_PISTOL);
        m_utilAction = UTIL_NONE;
        if (wasPistol && m_fireTicksSinceReload >= m_reloadThresholdTicks) {
            act.buttonMaskLow |= BTN_RELOAD;
            m_fireTicksSinceReload = 0;
            m_reloadThresholdTicks = rollReloadThreshold(activeKit(world));
            ++m_totalReloads;
            m_reloadUntilUs = nowUs + 2200000ULL;
        }
    }
    return true;
}

void BotBrain::tickCombatHuman(uint64_t nowUs, float dtSec, const BotWorldView& world,
                               const TrackedSoldier& target, const Vec3& rendered, float dist,
                               int32_t visibleEnemies, PlayerActionInput& act) {
    const float skill = clampFloat(m_aim.skill, 0.0f, 1.0f);
    const float reactScale = 1.25f - 0.55f * skill;
    const float errScale = 1.85f - 1.45f * skill;
    const float convScale = 1.35f - 0.80f * skill;
    const int32_t kit = activeKit(world);

    if (target.netId != m_targetNetId) {
        m_targetNetId = target.netId;
        m_targetAcquiredUs = nowUs;
        m_usedPistolThisTarget = false;
        const float alertScale = (nowUs < m_underFireUntilUs) ? 0.72f : 1.0f;
        m_reactionDelayUs = static_cast<uint64_t>(
            randFloat(140.0f, 290.0f) * reactScale * alertScale * 1000.0f);
        if (m_reactionDelayUs < 110000ULL) {
            m_reactionDelayUs = 110000ULL;
        }
        // Initial error: a few degrees, growing mildly with distance, random direction.
        const float mag = randFloat(1.2f, 3.4f) * errScale * (0.55f + dist / 65.0f);
        m_aimInitYawDeg = randFloat(-1.0f, 1.0f) * mag;
        m_aimInitPitchDeg = randFloat(-1.0f, 1.0f) * mag * 0.55f;
        m_aimConvergeUs = static_cast<uint64_t>(randFloat(260.0f, 520.0f) * convScale * 1000.0f);
        m_nextJitterUs = 0;
        resetFireState();
    }

    // Small human micro-jitter, refreshed every ~55-95 ms.
    if (nowUs >= m_nextJitterUs) {
        const float j = 0.28f * errScale;
        m_jitterYawDeg = randFloat(-j, j);
        m_jitterPitchDeg = randFloat(-j, j) * 0.65f;
        m_nextJitterUs = nowUs + static_cast<uint64_t>(randInt(55, 95)) * 1000ULL;
    }

    // Target velocity leading & long-range ballistic drop compensation.
    Vec3 targetVel;
    estimateSoldierVelocity(target, targetVel);
    const float flightSec = dist / kBulletSpeedMps;
    const float leadScale = 0.35f + 0.65f * skill;
    float leadX = targetVel.x * flightSec * leadScale;
    float leadY = targetVel.y * flightSec * leadScale;
    float leadZ = targetVel.z * flightSec * leadScale;
    const float leadMag = std::sqrt(leadX * leadX + leadY * leadY + leadZ * leadZ);
    if (leadMag > 3.5f) {
        const float s = 3.5f / leadMag;
        leadX *= s;
        leadY *= s;
        leadZ *= s;
    }
    const float dropLift = (dist > 45.0f) ? (0.5f * kGravityMps2 * flightSec * flightSec * 0.45f) : 0.0f;

    // Aim point: upper chest / neck by pose (higher skill biases slightly toward head/upper chest).
    float tgtH = 1.38f + 0.12f * skill;
    if (target.pose == 1u) tgtH = 0.95f + 0.08f * skill;
    else if (target.pose == 2u) tgtH = 0.34f;
    else if (target.pose == 3u) tgtH = 0.38f;

    float eyeH = 1.55f;
    if (world.ownPose == 1u) eyeH = 1.10f;
    else if (world.ownPose == 2u) eyeH = 0.42f;
    else if (world.ownPose == 3u) eyeH = 0.45f;

    const float dx = (rendered.x + leadX) - world.ownPos.x;
    const float dy = (rendered.y + leadY + tgtH + dropLift) - (world.ownPos.y + eyeH);
    const float dz = (rendered.z + leadZ) - world.ownPos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);

    const uint64_t tracked = nowUs - m_targetAcquiredUs;
    const float errYaw = aimErrorDecay(m_aimInitYawDeg, tracked, m_aimConvergeUs) + m_jitterYawDeg;
    const float errPitch = aimErrorDecay(m_aimInitPitchDeg, tracked, m_aimConvergeUs) + m_jitterPitchDeg;

    const float trueYaw = std::atan2(dx, dz) * kRadToDeg;
    const float truePitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                       -75.0f, 75.0f);
    const float desiredYaw = trueYaw + errYaw;
    const float desiredPitch = clampFloat(truePitch + errPitch, -75.0f, 75.0f);

    const float aimYaw = currentAimYaw(world);
    const float aimPitch = currentAimPitch(world);
    const float mouseScale = effectiveMouseScale(world);
    const float maxTurnDegPerSec = 420.0f + 260.0f * skill;
    steerAngles(aimYaw, aimPitch, desiredYaw, desiredPitch,
                maxTurnDegPerSec, mouseScale, dtSec, act.axes[4], act.axes[5]);

    // Actual aim vs. the true (lead-compensated) aim point decides whether a shot would hit.
    const float tyaw = wrapAngleDeg(trueYaw - aimYaw);
    const float tpit = truePitch - aimPitch;
    const float offTarget = std::sqrt(tyaw * tyaw + tpit * tpit);
    const float fireTolerance = 2.6f + 42.0f / (dist > 4.0f ? dist : 4.0f);

    const bool closeRange = (dist < 12.0f);
    const bool longRange = (dist > 45.0f);
    const bool reacted = (nowUs >= m_targetAcquiredUs + m_reactionDelayUs);
    const bool reloading = (nowUs < m_reloadUntilUs);
    const bool jumpLocked = (m_lastJumpUs != 0 && (nowUs - m_lastJumpUs) < 1050000ULL);
    // BF2 Coop BotMain::m_friendsInLineOfFireTime: hold trigger if a teammate crosses our barrel!
    const bool friendInLoF = hasFriendInLineOfFire(world, rendered);

    // Smart ADS zoom management (BF2 PIAltFire is a toggle!).
    if (!m_aim.keepRunning && nowUs >= m_zoomToggleCooldownUs) {
        const bool wantZoom = (dist >= 18.0f && !reloading && !world.ownIsSprinting);
        const bool wantUnzoom = (dist < 11.0f || reloading);
        if (wantZoom && world.ownWeaponZoom == 0u) {
            act.buttonMaskLow |= BTN_ALT_FIRE;
            m_zoomToggleCooldownUs = nowUs + 550000ULL;
        } else if (wantUnzoom && world.ownWeaponZoom > 0u) {
            act.buttonMaskLow |= BTN_ALT_FIRE;
            m_zoomToggleCooldownUs = nowUs + 550000ULL;
        }
    }

    // Fire state machine: tight range- and weapon-adapted bursts with deviation-recovery pauses.
    if (m_fireState == FIRE_WAIT) {
        if (reacted && !reloading && !jumpLocked && !friendInLoF &&
            !world.ownWeaponStowed && offTarget < fireTolerance) {
            m_fireState = FIRE_BURST;
            if (longRange) {
                m_burstShots = (kit == 3) ? randInt(2, 3) : 1;
            } else if (closeRange) {
                m_burstShots = (kit == 3) ? randInt(5, 8) : randInt(4, 6);
            } else if (dist > 32.0f) {
                m_burstShots = (kit == 3) ? randInt(3, 5) : randInt(2, 3);
            } else {
                m_burstShots = (kit == 3) ? randInt(4, 6) : randInt(3, 5);
            }
            const uint64_t burstDurUs = (m_burstShots <= 1)
                ? static_cast<uint64_t>(dtSec * 1000000.0f)
                : static_cast<uint64_t>(m_burstShots) * 90000ULL;
            m_fireStateEndUs = nowUs + burstDurUs;
            m_fireTicksSinceReload += m_burstShots;
        }
    }

    if (m_fireState == FIRE_BURST) {
        if (friendInLoF) {
            // Immediately lift finger off trigger when a friendly crosses our muzzle!
            m_fireState = FIRE_PAUSE;
            m_fireStateEndUs = nowUs + 180000ULL;
        } else {
            act.buttonMaskLow |= BTN_FIRE;
            if (nowUs >= m_fireStateEndUs) {
                m_fireState = FIRE_PAUSE;
                const float pauseMs = closeRange
                    ? randFloat(130.0f, 220.0f)
                    : (longRange ? randFloat(240.0f, 380.0f) : randFloat(190.0f, 320.0f));
                m_fireStateEndUs = nowUs + static_cast<uint64_t>(pauseMs * 1000.0f);
                m_strafeRight = !m_strafeRight;
            }
        }
    } else if (m_fireState == FIRE_PAUSE) {
        if (nowUs >= m_fireStateEndUs) {
            m_fireState = FIRE_WAIT;
            if (m_fireTicksSinceReload >= m_reloadThresholdTicks && !reloading) {
                if (closeRange && !m_usedPistolThisTarget && !m_aim.keepRunning) {
                    // CQB empty mag -> draw sidearm instead of dying mid-reload!
                    m_usedPistolThisTarget = true;
                    startUtility(UTIL_PISTOL, nowUs, rendered, target.netId);
                    return;
                }
                act.buttonMaskLow |= BTN_RELOAD;
                m_fireTicksSinceReload = 0;
                m_reloadThresholdTicks = rollReloadThreshold(kit);
                ++m_totalReloads;
                m_reloadUntilUs = nowUs + 2200000ULL;
            }
        }
    }

    // Tactical combat movement & cover (counter-strafe, Coop squad bounding overwatch, and
    // lateral step-out when a teammate blocks our line of fire; always hold still while firing).
    const bool firingNow = ((act.buttonMaskLow & BTN_FIRE) != 0u);
    const bool lowHp = (world.ownHitPointsByte < 100u);
    const bool outnumbered = (visibleEnemies >= 2 && world.ownHitPointsByte < 165u);

    bool squadManeuverAdvance = false;
    if (m_teamTactics && !firingNow) {
        uint32_t sqFire = 0u;
        bool sqCap = false, sqWait = false;
        m_teamTactics->querySquadCohesion(m_team, m_botIndex, world.ownPos,
                                          m_hasActiveObjective ? m_activeObjectiveKey : 0u,
                                          m_activeObjectivePos, nowUs,
                                          sqFire, sqCap, sqWait, squadManeuverAdvance);
    }

    if (!firingNow && !m_aim.keepRunning) {
        // Compute a cover/evasion step when reloading, low HP, or outnumbered.
        const bool wantEvade = reloading || lowHp || outnumbered;
        if (wantEvade && (!m_hasCoverStep || nowUs >= m_coverStepUntilUs)) {
            NavPoint cov;
            if (m_navMesh && m_navMesh->isLoaded() &&
                m_navMesh->findCoverStep(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                         rendered.x, rendered.z, 6.5f, cov)) {
                m_coverStepPos = Vec3(cov.x, cov.y, cov.z);
                m_hasCoverStep = true;
                m_coverStepUntilUs = nowUs + 1600000ULL;
            }
        }

        if (m_hasCoverStep && nowUs < m_coverStepUntilUs && wantEvade) {
            if (distXZ(world.ownPos, m_coverStepPos) > 1.0f) {
                const float moveYaw = std::atan2(m_coverStepPos.x - world.ownPos.x,
                                                 m_coverStepPos.z - world.ownPos.z) * kRadToDeg;
                computeRelativeMove(aimYaw, moveYaw, 1.0f, act.axes[0], act.axes[3]);
            }
        } else if (friendInLoF) {
            // Sidestep clear of friendly soldier blocking our line of fire!
            act.axes[0] = m_strafeRight ? 0.90f : -0.90f;
        } else if (reloading || (m_fireState == FIRE_PAUSE && m_fireStateEndUs > nowUs + 95000ULL)) {
            // Counter-strafe during the early portion of burst pauses and during reloads;
            // stop ~95ms before next burst so speed deviation decays cleanly!
            const float rightYawRad = (aimYaw + (m_strafeRight ? 90.0f : -90.0f)) * kDegToRad;
            const float probeX = world.ownPos.x + std::sin(rightYawRad) * 2.0f;
            const float probeZ = world.ownPos.z + std::cos(rightYawRad) * 2.0f;
            if (m_navMesh && m_navMesh->isLoaded() &&
                !m_navMesh->isPointWalkable(probeX, world.ownPos.y, probeZ)) {
                m_strafeRight = !m_strafeRight;
            }
            act.axes[0] = m_strafeRight ? 0.85f : -0.85f;
            if (dist < 4.5f) {
                act.axes[3] = -0.65f; // avoid body-clipping at point-blank
            } else if ((squadManeuverAdvance && dist > 15.0f && !lowHp) ||
                       (kit == 6 && dist > 32.0f && !lowHp)) {
                // BF2 Coop squadManeuverAlternateFire: bound forward while squadmates suppress!
                act.axes[3] = 0.70f;
            }
        }
    }

    // Stance management: classic BF2 Coop prone drop under medium/long-range fire or when holding
    // a flag against >24m threats; crouch during bursts; stand up when CQB or reloading!
    const bool holdingFlagZone = m_hasActiveObjective &&
                                 distXZ(world.ownPos, m_activeObjectivePos) < 12.0f;
    const bool wantCoopProne = !reloading && !closeRange && !squadManeuverAdvance &&
                               dist >= 22.0f && world.ownPose != 2u &&
                               (nowUs - m_lastProneToggleUs) > 3800000ULL &&
                               ((nowUs < m_underFireUntilUs && world.ownHitPointsByte < 190u) ||
                                (holdingFlagZone && dist >= 24.0f));
    if (world.ownPose == 2u && (closeRange || reloading) &&
        (nowUs - m_lastProneToggleUs) > 1200000ULL) {
        act.buttonMaskLow |= BTN_PRONE; // stand up from prone
        m_lastProneToggleUs = nowUs;
    } else if (wantCoopProne) {
        act.buttonMaskLow |= BTN_PRONE; // drop prone for minimal hitbox & deviation!
        m_lastProneToggleUs = nowUs;
    } else if (world.ownPose != 2u && (firingNow || (!reloading && !closeRange && !lowHp))) {
        act.buttonMaskLow |= BTN_CROUCH;
    }
}

void BotBrain::tickOutOfCombat(uint64_t nowUs, float dtSec, const BotWorldView& world,
                               bool hasMoveTarget, const Vec3& moveTarget, uint32_t navButtons,
                               PlayerActionInput& act) {
    resetFireState();
    m_targetNetId = 0;

    if (m_switchBackToPrimaryUs != 0 && nowUs >= m_switchBackToPrimaryUs) {
        act.buttonMaskLow |= BTN_WEAPON_3;
        m_switchBackToPrimaryUs = 0;
    }

    // Unzoom when out of combat.
    if (world.ownWeaponZoom > 0u && nowUs >= m_zoomToggleCooldownUs) {
        act.buttonMaskLow |= BTN_ALT_FIRE;
        m_zoomToggleCooldownUs = nowUs + 500000ULL;
    }

    // Stand up from prone before moving.
    if (world.ownPose == 2u && (nowUs - m_lastProneToggleUs) > 1200000ULL) {
        act.buttonMaskLow |= BTN_PRONE;
        m_lastProneToggleUs = nowUs;
    }

    // Proactive tactical reload after combat ends if >= 40% of magazine was spent.
    if (nowUs >= m_reloadUntilUs &&
        m_fireTicksSinceReload >= (m_reloadThresholdTicks * 4) / 10 &&
        (m_lastCombatUs == 0 || (nowUs - m_lastCombatUs) > 1400000ULL)) {
        act.buttonMaskLow |= BTN_RELOAD;
        m_fireTicksSinceReload = 0;
        m_reloadThresholdTicks = rollReloadThreshold(activeKit(world));
        ++m_totalReloads;
        m_reloadUntilUs = nowUs + 2200000ULL;
    }

    const float aimYaw = currentAimYaw(world);
    const float aimPitch = currentAimPitch(world);
    const float mouseScale = effectiveMouseScale(world);

    // Check if there is an active threat angle to watch (under fire, recent hidden enemy, or team spot).
    bool hasWatchPoint = false;
    Vec3 watchPoint;
    bool highAlert = (nowUs < m_underFireUntilUs);

    if (m_hasLastKnownEnemy && (nowUs - m_lastKnownEnemyUs) < 4200000ULL) {
        hasWatchPoint = true;
        watchPoint = m_lastKnownEnemyPos;
        highAlert = true;
    } else if (m_teamTactics) {
        SpottedEnemy spot;
        if (m_teamTactics->findNearestSpotted(world.ownTeam, world.ownPos, 52.0f, nowUs, spot)) {
            hasWatchPoint = true;
            watchPoint = spot.pos;
            if (distXZ(world.ownPos, spot.pos) < 34.0f) {
                highAlert = true;
            }
        }
    }

    // If holding a flag capture point or defensive perimeter post, crouch and scan toward threats!
    if (!hasMoveTarget) {
        act.buttonMaskLow |= BTN_CROUCH;
        if (hasWatchPoint) {
            const Vec3 lookPt(watchPoint.x, world.ownPos.y + 1.3f, watchPoint.z);
            steerToward(world.ownPos, aimYaw, aimPitch, lookPt, mouseScale, dtSec,
                        act.axes[4], act.axes[5]);
        } else if (m_hasDefendWatchPoint && m_hasActiveObjective && m_activeObjectiveFriendly) {
            // Scan left-right across the expected attacker approach lane (e.g. Gas Station -> Hotel).
            if (nowUs >= m_nextScanFlipUs) {
                m_scanOffsetDeg = randFloat(-32.0f, 32.0f);
                m_nextScanFlipUs = nowUs + static_cast<uint64_t>(randInt(1400, 2600)) * 1000ULL;
            }
            const float approachYaw = std::atan2(m_defendWatchPoint.x - world.ownPos.x,
                                                 m_defendWatchPoint.z - world.ownPos.z) * kRadToDeg;
            steerAngles(aimYaw, aimPitch, wrapAngleDeg(approachYaw + m_scanOffsetDeg), 0.0f,
                        180.0f, mouseScale, dtSec, act.axes[4], act.axes[5]);
        } else {
            if (nowUs >= m_nextScanFlipUs) {
                m_scanOffsetDeg = randFloat(-75.0f, 75.0f);
                m_nextScanFlipUs = nowUs + static_cast<uint64_t>(randInt(1400, 2600)) * 1000ULL;
            }
            steerAngles(aimYaw, aimPitch, wrapAngleDeg(aimYaw + m_scanOffsetDeg * 0.35f), 0.0f,
                        160.0f, mouseScale, dtSec, act.axes[4], act.axes[5]);
        }
        return;
    }

    // Compute movement vector with friendly separation (anti-clumping in corridors/doorways).
    float moveDx = moveTarget.x - world.ownPos.x;
    float moveDz = moveTarget.z - world.ownPos.z;
    const float moveDist = std::sqrt(moveDx * moveDx + moveDz * moveDz);
    if (moveDist > 0.05f) {
        moveDx /= moveDist;
        moveDz /= moveDist;
    }

    float sepX = 0.0f, sepZ = 0.0f;
    computeFriendlySeparation(world, sepX, sepZ);
    moveDx += sepX * 0.65f;
    moveDz += sepZ * 0.65f;
    const float moveYaw = std::atan2(moveDx, moveDz) * kRadToDeg;

    // Stamina hysteresis + combat proximity gating for smart sprinting.
    if (world.ownSprintStamina < 0.08f) {
        m_sprintActive = false;
    } else if (world.ownSprintStamina > 0.28f) {
        m_sprintActive = true;
    }
    const bool hotArea = m_teamTactics &&
        (m_teamTactics->dangerHeatNear(world.ownTeam, world.ownPos, 24.0f, nowUs) >= 2u);
    const bool allowSprint = ((navButtons & BTN_SPRINT) != 0u) &&
                             m_sprintActive && !highAlert && !hotArea &&
                             (nowUs >= m_reloadUntilUs) && (moveDist > 9.0f);

    if (highAlert && hasWatchPoint) {
        // Move along path while keeping weapon aimed at the threat / last-known corner!
        const float dx = watchPoint.x - world.ownPos.x;
        const float dy = (watchPoint.y + 1.35f) - (world.ownPos.y + 1.55f);
        const float dz = watchPoint.z - world.ownPos.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);
        const float watchYaw = std::atan2(dx, dz) * kRadToDeg;
        const float watchPitch = clampFloat(-std::atan2(dy, (horiz > 0.1f ? horiz : 0.1f)) * kRadToDeg,
                                            -45.0f, 45.0f);
        steerAngles(aimYaw, aimPitch, watchYaw, watchPitch, 360.0f, mouseScale, dtSec,
                    act.axes[4], act.axes[5]);
        computeRelativeMove(aimYaw, moveYaw, 1.0f, act.axes[0], act.axes[3]);
    } else if (nowUs < m_underFireUntilUs && !hasWatchPoint) {
        // Took unseen sniper/flank damage -> evasive zig-zag + look around to spot shooter!
        if (nowUs >= m_nextScanFlipUs) {
            m_scanOffsetDeg = (m_scanOffsetDeg >= 0.0f) ? -135.0f : 135.0f;
            m_nextScanFlipUs = nowUs + 650000ULL;
            m_strafeRight = !m_strafeRight;
        }
        steerAngles(aimYaw, aimPitch, wrapAngleDeg(moveYaw + m_scanOffsetDeg), 0.0f,
                    420.0f, mouseScale, dtSec, act.axes[4], act.axes[5]);
        computeRelativeMove(aimYaw, moveYaw, 1.0f, act.axes[0], act.axes[3]);
    } else {
        // Smooth navigation steering with subtle organic horizon scanning.
        if (nowUs >= m_nextScanFlipUs) {
            m_scanOffsetDeg = hasWatchPoint ? 0.0f : randFloat(-14.0f, 14.0f);
            m_nextScanFlipUs = nowUs + static_cast<uint64_t>(randInt(1600, 3200)) * 1000ULL;
        }
        const float lookYaw = wrapAngleDeg(moveYaw + (allowSprint ? 0.0f : m_scanOffsetDeg));
        steerAngles(aimYaw, aimPitch, lookYaw, 0.0f, m_turnRateDegPerSec, mouseScale, dtSec,
                    act.axes[4], act.axes[5]);
        if (allowSprint) {
            act.axes[3] = 1.0f;
            act.buttonMaskLow |= BTN_SPRINT;
        } else {
            computeRelativeMove(aimYaw, moveYaw, 1.0f, act.axes[0], act.axes[3]);
            act.buttonMaskLow |= (navButtons & ~BTN_SPRINT);
        }
    }

    // Ledge/curb hop if waypoint rises significantly above our feet.
    if (world.ownIsGrounded && (moveTarget.y - world.ownPos.y) > 0.48f && moveDist < 4.5f &&
        (nowUs - m_lastJumpUs) > 1200000ULL) {
        act.buttonMaskLow |= BTN_JUMP;
        m_lastJumpUs = nowUs;
    }
}

void BotBrain::tick(uint64_t nowUs, float dtSec, const BotWorldView& world, BotTickOutput& out) {
    out = BotTickOutput();
    if (dtSec <= 0.0f || dtSec > 0.25f) {
        dtSec = 1.0f / 30.0f;
    }

    // Dead or man-down: request spawn point + kit periodically so server auto-spawns us.
    if (!world.ownAlive || world.ownManDown) {
        if (nowUs >= m_nextSpawnRequestUs) {
            if (world.spawnGroups && !world.spawnGroups->empty()) {
                const int32_t sg = pickSpawnGroup(*world.spawnGroups,
                                                  world.controlPoints,
                                                  world.soldiers);
                if (sg >= 0) {
                    out.requestSpawn = true;
                    out.desiredSpawnGroupId = sg;
                    out.desiredKit = m_kit;
                }
            }
            m_nextSpawnRequestUs = nowUs + 1500000ULL; // retry every 1.5s until alive
        }
        return;
    }

    if (!world.hasOwnPos) {
        return;
    }

    m_lastOwnPosForDeath = world.ownPos;
    m_hasLastOwnPosForDeath = true;
    m_role = TeamTactics::roleForBot(m_botIndex, activeKit(world));

    if (!m_hasSpawnAnchor) {
        m_spawnAnchor = world.ownPos;
        m_hasSpawnAnchor = true;
    }

    // 1. Scan for enemy soldiers.
    const TrackedSoldier* bestTarget = nullptr;
    Vec3 targetRendered;
    float targetDist = 0.0f;
    int32_t visibleEnemies = 0;
    float bestDist = kCombatRangeMeters;

    if (!m_aim.legacy) {
        selectTarget(nowUs, world, bestTarget, targetRendered, targetDist, visibleEnemies);
    } else if (world.soldiers) {
        for (std::map<uint16_t, TrackedSoldier>::const_iterator it = world.soldiers->begin();
             it != world.soldiers->end(); ++it) {
            const TrackedSoldier& s = it->second;
            if (s.netId == world.ownSoldierNetId) {
                continue;
            }
            if (!s.hasPos || s.isRagdoll || s.hitPointsByte == 0u) {
                continue;
            }
            if (s.team == 0 || s.team == world.ownTeam) {
                continue;
            }
            const float d = dist3D(world.ownPos, s.pos);
            if (d >= kCombatRangeMeters) {
                continue;
            }
            if (!hasClearLineOfSight(world, s.pos, s.pose)) {
                continue;
            }
            ++visibleEnemies;
            if (d < bestDist) {
                bestDist = d;
                bestTarget = &s;
                targetRendered = s.pos;
                targetDist = d;
            }
        }
    }

    const bool inCombat = (bestTarget != nullptr);
    updateDamageAndMemory(nowUs, world, bestTarget, targetRendered);

    // 2. Navigation waypoint update.
    Vec3 moveTarget;
    bool hasMoveTarget = false;
    uint32_t navButtons = 0u;
    updateNavigation(nowUs, world, inCombat, moveTarget, hasMoveTarget, navButtons);

    // 3. Multi-stage stuck detection (only when intending to move and not in combat/utility).
    const bool intendingToMove = hasMoveTarget && !inCombat && (m_utilAction == UTIL_NONE);
    if (!m_hasStuckAnchor) {
        m_stuckAnchorPos = world.ownPos;
        m_stuckAnchorUs = nowUs;
        m_hasStuckAnchor = true;
    } else if (!intendingToMove) {
        m_stuckAnchorPos = world.ownPos;
        m_stuckAnchorUs = nowUs;
    } else if (nowUs >= m_stuckRecoverUntilUs && (nowUs - m_stuckAnchorUs) >= 1300000ULL) {
        const float moved = distXZ(world.ownPos, m_stuckAnchorPos);
        m_stuckAnchorPos = world.ownPos;
        m_stuckAnchorUs = nowUs;
        if (moved < 0.55f) {
            ++m_stuckStage;
            const float aimYaw = currentAimYaw(world);
            // Prefer stepping toward the walkable side on NavMesh.
            float preferDir = (randInt(0, 1) == 0) ? -1.0f : 1.0f;
            if (m_navMesh && m_navMesh->isLoaded()) {
                const float rRad = (aimYaw + 90.0f) * kDegToRad;
                const float lRad = (aimYaw - 90.0f) * kDegToRad;
                const bool rightOk = m_navMesh->isPointWalkable(
                    world.ownPos.x + std::sin(rRad) * 2.5f, world.ownPos.y,
                    world.ownPos.z + std::cos(rRad) * 2.5f);
                const bool leftOk = m_navMesh->isPointWalkable(
                    world.ownPos.x + std::sin(lRad) * 2.5f, world.ownPos.y,
                    world.ownPos.z + std::cos(lRad) * 2.5f);
                if (rightOk && !leftOk) preferDir = 1.0f;
                else if (leftOk && !rightOk) preferDir = -1.0f;
            }
            m_stuckStrafeDir = preferDir;
            m_stuckRecoverUntilUs = nowUs + (m_stuckStage == 1 ? 850000ULL : 1200000ULL);
            m_nextPathComputeUs = nowUs;
            m_hasPatrolGoal = false;
            m_waypoints.clear();
            m_waypointIdx = 0;
            if (m_stuckStage >= 3 && m_hasActiveObjective) {
                m_blacklistedObjKey = m_activeObjectiveKey;
                m_blacklistUntilUs = nowUs + 16000000ULL;
                m_hasActiveObjective = false;
                m_hasRouteTrack = false;
                m_stuckStage = 0;
            }
        } else if (moved > 2.0f) {
            m_stuckStage = 0;
        }
    }

    PlayerActionInput act;

    // 4. Multi-stage stuck recovery maneuver.
    if (nowUs < m_stuckRecoverUntilUs && !inCombat) {
        act.axes[3] = (m_stuckStage >= 2) ? -0.85f : -0.4f;
        act.axes[0] = m_stuckStrafeDir;
        act.axes[4] = m_stuckStrafeDir * (m_stuckStage >= 2 ? 2.2f : 1.2f);
        if (world.ownIsGrounded && (nowUs - m_lastJumpUs) > 700000ULL) {
            act.buttonMaskLow |= BTN_JUMP;
            m_lastJumpUs = nowUs;
        }
        out.action = act;
        return;
    }

    // 5. Tactical class utility actions (Medic heal/revive, Support ammo, smart grenade, CQB pistol).
    if (!m_aim.legacy) {
        tryStartUtilityAction(nowUs, world, bestTarget, targetRendered, targetDist);
        if (tickUtilityAction(nowUs, dtSec, world, bestTarget, targetRendered, targetDist, act)) {
            out.action = act;
            return;
        }
    }

    // 6. Combat AI vs out-of-combat navigation.
    if (inCombat && m_aim.legacy) {
        if (bestTarget->netId != m_targetNetId) {
            m_targetNetId = bestTarget->netId;
            m_targetAcquiredUs = nowUs;
            m_reactionDelayUs = static_cast<uint64_t>(randInt(150, 250)) * 1000ULL;
            m_burstFiring = false;
            m_burstTicksLeft = 0;
        }

        if (nowUs >= m_nextAimNoiseUs) {
            m_aimNoiseYawDeg = randFloat(-2.5f, 2.5f);
            m_aimNoisePitchDeg = randFloat(-1.8f, 1.8f);
            m_nextAimNoiseUs = nowUs + static_cast<uint64_t>(randInt(200, 320)) * 1000ULL;
        }

        const Vec3 ownEye(world.ownPos.x, world.ownPos.y + 1.55f, world.ownPos.z);
        const Vec3 enemyTorso(bestTarget->pos.x, bestTarget->pos.y + 1.1f, bestTarget->pos.z);

        const float dx = enemyTorso.x - ownEye.x;
        const float dy = enemyTorso.y - ownEye.y;
        const float dz = enemyTorso.z - ownEye.z;
        const float horiz = std::sqrt(dx * dx + dz * dz);

        const float desiredYaw = std::atan2(dx, dz) * kRadToDeg + m_aimNoiseYawDeg;
        const float desiredPitch = clampFloat(
            -std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg + m_aimNoisePitchDeg,
            -75.0f, 75.0f);

        const float aimYaw = currentAimYaw(world);
        const float aimPitch = currentAimPitch(world);
        const float yawErr = wrapAngleDeg(desiredYaw - aimYaw);
        const float pitchErr = desiredPitch - aimPitch;
        const float totalErrDeg = std::sqrt(yawErr * yawErr + pitchErr * pitchErr);

        const float maxDelta = m_turnRateDegPerSec * dtSec;
        const float appliedYaw = clampFloat(yawErr, -maxDelta, maxDelta);
        const float appliedPitch = clampFloat(pitchErr, -maxDelta * 0.8f, maxDelta * 0.8f);

        const float mouseScale = effectiveMouseScale(world);
        act.axes[4] = appliedYaw / mouseScale;
        act.axes[5] = appliedPitch / mouseScale;

        if (nowUs >= m_nextStrafeFlipUs) {
            m_strafeRight = !m_strafeRight;
            m_nextStrafeFlipUs = nowUs + static_cast<uint64_t>(randInt(600, 1400)) * 1000ULL;
        }
        act.axes[0] = m_strafeRight ? 1.0f : -1.0f;

        if (bestDist >= kMediumRangeMeters) {
            act.axes[3] = 0.35f;
            act.buttonMaskLow |= BTN_CROUCH;
            if (world.ownWeaponZoom == 0u && nowUs >= m_zoomToggleCooldownUs) {
                act.buttonMaskLow |= BTN_ALT_FIRE;
                m_zoomToggleCooldownUs = nowUs + 600000ULL;
            }
        } else {
            act.axes[3] = (bestDist < 5.0f) ? -0.5f : 0.4f;
        }

        if (m_switchBackToPrimaryUs != 0 && nowUs >= m_switchBackToPrimaryUs) {
            act.buttonMaskLow |= BTN_WEAPON_3;
            m_switchBackToPrimaryUs = 0;
        } else if (m_nextWeaponSwitchUs != 0 && nowUs >= m_nextWeaponSwitchUs &&
                   bestDist > 10.0f && bestDist < 30.0f) {
            act.buttonMaskLow |= BTN_WEAPON_4;
            m_switchBackToPrimaryUs = nowUs + 1200000ULL;
            m_nextWeaponSwitchUs = nowUs + static_cast<uint64_t>(randInt(20000, 35000)) * 1000ULL;
        }

        if (m_fireTicksSinceReload >= m_reloadThresholdTicks && nowUs >= m_reloadUntilUs) {
            act.buttonMaskLow |= BTN_RELOAD;
            m_fireTicksSinceReload = 0;
            m_reloadThresholdTicks = randInt(25, 35);
            m_reloadUntilUs = nowUs + 1800000ULL;
            m_burstFiring = false;
            m_burstTicksLeft = 0;
        }

        const bool reacted = (nowUs >= m_targetAcquiredUs + m_reactionDelayUs);
        const bool friendBlocked = hasFriendInLineOfFire(world, bestTarget->pos);
        if (reacted && nowUs >= m_reloadUntilUs && totalErrDeg < 15.0f && !friendBlocked) {
            if (m_burstTicksLeft <= 0) {
                m_burstFiring = !m_burstFiring;
                m_burstTicksLeft = m_burstFiring ? randInt(4, 10) : randInt(3, 6);
            }
            if (m_burstFiring) {
                act.buttonMaskLow |= BTN_FIRE;
                ++m_fireTicksSinceReload;
            }
            --m_burstTicksLeft;
        }
    } else if (inCombat) {
        tickCombatHuman(nowUs, dtSec, world, *bestTarget, targetRendered, targetDist,
                        visibleEnemies, act);
    } else {
        tickOutOfCombat(nowUs, dtSec, world, hasMoveTarget, moveTarget, navButtons, act);
    }

    if (m_teamTactics) {
        const bool insideCap = m_hasActiveObjective && !m_activeObjectiveFriendly &&
                               distXZ(world.ownPos, m_activeObjectivePos) < 10.5f;
        m_teamTactics->updateSquadMember(m_team, m_botIndex, world.ownPos,
                                         world.ownAlive && !world.ownManDown,
                                         ((act.buttonMaskLow & BTN_FIRE) != 0u),
                                         m_hasActiveObjective ? m_activeObjectiveKey : 0u,
                                         insideCap, nowUs);
    }

    if (m_aim.keepRunning && world.ownAlive && !world.ownManDown) {
        // --keep-running: never stand still; alternate sprint and a slow walk every 2-4 s.
        if (nowUs >= m_nextRunFlipUs) {
            m_runSprint = !m_runSprint;
            m_nextRunFlipUs = nowUs + static_cast<uint64_t>(randInt(2000, 4000)) * 1000ULL;
        }
        act.buttonMaskLow &= ~(BTN_CROUCH | BTN_PRONE);
        if (m_runSprint) {
            act.axes[3] = 1.0f;
            act.buttonMaskLow |= BTN_SPRINT;
        } else {
            act.axes[3] = 0.4f;
            act.buttonMaskLow &= ~BTN_SPRINT;
        }
    }

    out.action = act;
}

} // namespace bf2bot
