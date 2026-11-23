#include "BotBrain.h"

#include <cmath>

namespace bf2bot {

namespace {

static const float kPi = 3.14159265358979323846f;
static const float kRadToDeg = 180.0f / kPi;
static const float kLookFactor = 5.0f; // BF2 Soldier phy-soldier-look-factor-{x,y}
static const float kCombatRangeMeters = 50.0f;
static const float kMediumRangeMeters = 15.0f;

// Infantry kits: SpecOps(0), Assault(2), Support(3), Medic(5), AntiTank(6)
static const int32_t kInfantryKits[5] = { 0, 2, 3, 5, 6 };

} // namespace

BotBrain::BotBrain(uint32_t botIndex, uint32_t seed,
                   const NavMesh* navMesh, const RouteLoader* routeLoader)
    : m_botIndex(botIndex),
      m_team((botIndex % 2u == 0u) ? 1 : 2),
      m_kit(kInfantryKits[botIndex % 5u]),
      m_rng(seed ^ (botIndex * 2654435761u + 0x9E3779B9u)),
      m_navMesh(navMesh),
      m_routeLoader(routeLoader),
      m_turnRateDegPerSec(220.0f),
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
      m_reloadUntilUs(0),
      m_nextWeaponSwitchUs(0),
      m_switchBackToPrimaryUs(0),
      m_nextSpawnRequestUs(0),
      m_spawnGroupRotateIdx(botIndex),
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
      m_stuckStrafeDir(1.0f) {
    m_turnRateDegPerSec = randFloat(180.0f, 260.0f);
    m_reloadThresholdTicks = randInt(25, 35);
}

void BotBrain::reset() {
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
    m_reloadThresholdTicks = randInt(25, 35);
    m_reloadUntilUs = 0;
    m_nextWeaponSwitchUs = 0;
    m_switchBackToPrimaryUs = 0;

    m_nextSpawnRequestUs = 0;

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
}

void BotBrain::onSpawned(const Vec3& spawnPos, uint64_t nowUs) {
    m_spawnAnchor = spawnPos;
    m_hasSpawnAnchor = true;

    m_targetNetId = 0;
    m_burstFiring = false;
    m_burstTicksLeft = 0;
    m_fireTicksSinceReload = 0;
    m_reloadThresholdTicks = randInt(25, 35);
    m_reloadUntilUs = 0;
    m_nextWeaponSwitchUs = nowUs + static_cast<uint64_t>(randInt(18000, 32000)) * 1000ULL;
    m_switchBackToPrimaryUs = 0;

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
}

void BotBrain::onDied(uint64_t nowUs) {
    m_targetNetId = 0;
    m_burstFiring = false;
    m_burstTicksLeft = 0;
    m_waypoints.clear();
    m_waypointIdx = 0;
    m_hasRouteTrack = false;
    m_hasStuckAnchor = false;
    m_stuckRecoverUntilUs = 0;
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

int32_t BotBrain::pickSpawnGroup(const std::map<uint8_t, TrackedSpawnGroup>& spawnGroups) {
    if (spawnGroups.empty()) {
        return -1;
    }

    std::vector<uint8_t> tier1; // active, friendly, !aiOnly, hasSpawnPoints
    std::vector<uint8_t> tier2; // active, friendly, !aiOnly
    std::vector<uint8_t> tier3; // active, friendly
    std::vector<uint8_t> tier4; // friendly

    for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = spawnGroups.begin();
         it != spawnGroups.end(); ++it) {
        const TrackedSpawnGroup& sg = it->second;
        if (static_cast<int32_t>(sg.team) != m_team) {
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

    const size_t idx = (m_spawnGroupRotateIdx++) % chosen->size();
    return static_cast<int32_t>((*chosen)[idx]);
}

bool BotBrain::pickObjectiveGoal(const BotWorldView& world, Vec3& outGoal) {
    std::vector<Vec3> candidates;

    if (world.controlPoints) {
        for (std::map<uint16_t, TrackedControlPoint>::const_iterator it = world.controlPoints->begin();
             it != world.controlPoints->end(); ++it) {
            const TrackedControlPoint& cp = it->second;
            if (!cp.hasPos) {
                continue;
            }
            const bool capturable = (m_team == 1) ? cp.allowCaptureByTeam1 : cp.allowCaptureByTeam2;
            if (cp.team != m_team && (cp.isNeutral || capturable || cp.team != 0)) {
                candidates.push_back(cp.pos);
            }
        }
    }

    if (candidates.empty() && world.spawnGroups) {
        for (std::map<uint8_t, TrackedSpawnGroup>::const_iterator it = world.spawnGroups->begin();
             it != world.spawnGroups->end(); ++it) {
            const TrackedSpawnGroup& sg = it->second;
            if (!sg.hasPos) {
                continue;
            }
            if (static_cast<int32_t>(sg.team) != m_team && sg.team != 0u) {
                candidates.push_back(sg.pos);
            }
        }
    }

    if (candidates.empty()) {
        return false;
    }

    const int32_t idx = randInt(0, static_cast<int32_t>(candidates.size()) - 1);
    outGoal = candidates[static_cast<size_t>(idx)];
    // Small random offset around the objective so 64 bots don't stack on a single point.
    outGoal.x += randFloat(-8.0f, 8.0f);
    outGoal.z += randFloat(-8.0f, 8.0f);
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

void BotBrain::steerToward(const Vec3& ownPos, float ownYawDeg, float ownPitchDeg,
                           const Vec3& targetPos, float dtSec,
                           float& outMouseX, float& outMouseY) {
    const float dx = targetPos.x - ownPos.x;
    const float dy = targetPos.y - ownPos.y;
    const float dz = targetPos.z - ownPos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);

    // BF2 left-handed Y-up: +Z is yaw=0, +X is yaw=+90 deg.
    const float desiredYaw = std::atan2(dx, dz) * kRadToDeg;
    const float desiredPitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                          -75.0f, 75.0f);

    const float yawErr = wrapAngleDeg(desiredYaw - ownYawDeg);
    const float pitchErr = desiredPitch - ownPitchDeg;

    const float maxDelta = m_turnRateDegPerSec * dtSec;
    const float appliedYaw = clampFloat(yawErr, -maxDelta, maxDelta);
    const float appliedPitch = clampFloat(pitchErr, -maxDelta * 0.75f, maxDelta * 0.75f);

    outMouseX = appliedYaw / kLookFactor;
    outMouseY = appliedPitch / kLookFactor;
}

void BotBrain::updateNavigation(uint64_t nowUs, const BotWorldView& world,
                                bool inCombat, Vec3& outMoveTarget, bool& outHasMoveTarget,
                                uint32_t& outNavButtons) {
    outHasMoveTarget = false;
    outNavButtons = 0u;

    if (!world.hasOwnPos) {
        return;
    }

    // Tier 1: NavMesh A* pathing
    if (m_navMesh && m_navMesh->isLoaded()) {
        bool needRecompute = m_waypoints.empty() ||
                             m_waypointIdx >= m_waypoints.size() ||
                             nowUs >= m_nextPathComputeUs;

        if (!needRecompute) {
            const NavPoint& wp = m_waypoints[m_waypointIdx];
            if (distXZ(world.ownPos, Vec3(wp.x, wp.y, wp.z)) < 2.5f) {
                ++m_waypointIdx;
                if (m_waypointIdx >= m_waypoints.size()) {
                    needRecompute = true;
                }
            }
        }

        if (needRecompute) {
            Vec3 goal;
            bool hasGoal = pickObjectiveGoal(world, goal);
            if (!hasGoal) {
                hasGoal = pickRandomWalkableNavPoint(goal);
            }
            if (hasGoal) {
                std::vector<NavPoint> newPath;
                if (m_navMesh->findPath(world.ownPos.x, world.ownPos.y, world.ownPos.z,
                                        goal.x, goal.y, goal.z, newPath) && !newPath.empty()) {
                    m_waypoints.swap(newPath);
                    m_waypointIdx = 0;
                    // Skip first waypoint if it is essentially our current location.
                    if (m_waypoints.size() > 1u &&
                        distXZ(world.ownPos, Vec3(m_waypoints[0].x, m_waypoints[0].y, m_waypoints[0].z)) < 2.0f) {
                        m_waypointIdx = 1;
                    }
                } else {
                    m_waypoints.clear();
                    m_waypointIdx = 0;
                    // Direct fallback toward goal if disconnected island.
                    outMoveTarget = goal;
                    outHasMoveTarget = true;
                }
            }
            m_nextPathComputeUs = nowUs + static_cast<uint64_t>(randInt(3500, 5000)) * 1000ULL;
        }

        if (m_waypointIdx < m_waypoints.size()) {
            const NavPoint& wp = m_waypoints[m_waypointIdx];
            outMoveTarget = Vec3(wp.x, wp.y, wp.z);
            outHasMoveTarget = true;
            if (!inCombat) {
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
        if (!pickObjectiveGoal(world, goal)) {
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

void BotBrain::tick(uint64_t nowUs, float dtSec, const BotWorldView& world, BotTickOutput& out) {
    out = BotTickOutput();
    if (dtSec <= 0.0f || dtSec > 0.25f) {
        dtSec = 1.0f / 30.0f;
    }

    // Dead or man-down: request spawn point + kit periodically so server auto-spawns us.
    if (!world.ownAlive || world.ownManDown) {
        if (nowUs >= m_nextSpawnRequestUs) {
            if (world.spawnGroups && !world.spawnGroups->empty()) {
                const int32_t sg = pickSpawnGroup(*world.spawnGroups);
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

    if (!m_hasSpawnAnchor) {
        m_spawnAnchor = world.ownPos;
        m_hasSpawnAnchor = true;
    }

    // 1. Scan for closest alive enemy soldier within combat range (< 50 m).
    const TrackedSoldier* bestTarget = nullptr;
    float bestDist = kCombatRangeMeters;
    if (world.soldiers) {
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
            if (d < bestDist) {
                bestDist = d;
                bestTarget = &s;
            }
        }
    }

    const bool inCombat = (bestTarget != nullptr);

    // 2. Stuck detection (only when not deliberately holding crouch in combat).
    if (!m_hasStuckAnchor) {
        m_stuckAnchorPos = world.ownPos;
        m_stuckAnchorUs = nowUs;
        m_hasStuckAnchor = true;
    } else if (nowUs >= m_stuckRecoverUntilUs && (nowUs - m_stuckAnchorUs) >= 1500000ULL) {
        const float moved = distXZ(world.ownPos, m_stuckAnchorPos);
        m_stuckAnchorPos = world.ownPos;
        m_stuckAnchorUs = nowUs;
        if (moved < 0.5f && !inCombat) {
            m_stuckRecoverUntilUs = nowUs + 1000000ULL; // 1.0s recovery maneuver
            m_stuckStrafeDir = (randInt(0, 1) == 0) ? -1.0f : 1.0f;
            m_nextPathComputeUs = nowUs; // force nav recompute after unsticking
            m_hasPatrolGoal = false;
        }
    }

    // 3. Navigation waypoint update.
    Vec3 moveTarget;
    bool hasMoveTarget = false;
    uint32_t navButtons = 0u;
    updateNavigation(nowUs, world, inCombat, moveTarget, hasMoveTarget, navButtons);

    PlayerActionInput act;

    // 4. Stuck recovery overrides movement cleanly.
    if (nowUs < m_stuckRecoverUntilUs) {
        act.axes[3] = -0.6f;                // back up slightly
        act.axes[0] = m_stuckStrafeDir;     // strafe perpendicular
        act.axes[4] = m_stuckStrafeDir * 1.2f; // turn away from obstacle
        act.buttonMaskLow |= BTN_JUMP;
        out.action = act;
        return;
    }

    // 5. Combat AI vs peaceful navigation.
    if (inCombat) {
        if (bestTarget->netId != m_targetNetId) {
            m_targetNetId = bestTarget->netId;
            m_targetAcquiredUs = nowUs;
            m_reactionDelayUs = static_cast<uint64_t>(randInt(150, 250)) * 1000ULL;
            m_burstFiring = false;
            m_burstTicksLeft = 0;
        }

        // Refresh slight human aim noise every ~250ms (±1.5..3.0 deg).
        if (nowUs >= m_nextAimNoiseUs) {
            m_aimNoiseYawDeg = randFloat(-2.5f, 2.5f);
            m_aimNoisePitchDeg = randFloat(-1.8f, 1.8f);
            m_nextAimNoiseUs = nowUs + static_cast<uint64_t>(randInt(200, 320)) * 1000ULL;
        }

        // Aim from own eye height (+1.55m) to enemy torso (+1.1m).
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

        const float yawErr = wrapAngleDeg(desiredYaw - world.ownFacingDegrees);
        const float pitchErr = desiredPitch - world.ownPitchDegrees;
        const float totalErrDeg = std::sqrt(yawErr * yawErr + pitchErr * pitchErr);

        const float maxDelta = m_turnRateDegPerSec * dtSec;
        const float appliedYaw = clampFloat(yawErr, -maxDelta, maxDelta);
        const float appliedPitch = clampFloat(pitchErr, -maxDelta * 0.8f, maxDelta * 0.8f);

        act.axes[4] = appliedYaw / kLookFactor;
        act.axes[5] = appliedPitch / kLookFactor;

        // Combat strafing (alternating every 0.6..1.4s).
        if (nowUs >= m_nextStrafeFlipUs) {
            m_strafeRight = !m_strafeRight;
            m_nextStrafeFlipUs = nowUs + static_cast<uint64_t>(randInt(600, 1400)) * 1000ULL;
        }
        act.axes[0] = m_strafeRight ? 1.0f : -1.0f;

        if (bestDist >= kMediumRangeMeters) {
            // Medium range (15..50m): crouch + ADS zoom, slow advance.
            act.axes[3] = 0.35f;
            act.buttonMaskLow |= BTN_CROUCH | BTN_ALT_FIRE;
        } else {
            // Close range (< 15m): strafe + maintain spacing.
            act.axes[3] = (bestDist < 5.0f) ? -0.5f : 0.4f;
        }

        // Occasional weapon switch / grenade toss cadence.
        if (m_switchBackToPrimaryUs != 0 && nowUs >= m_switchBackToPrimaryUs) {
            act.buttonMaskLow |= BTN_WEAPON_3;
            m_switchBackToPrimaryUs = 0;
        } else if (m_nextWeaponSwitchUs != 0 && nowUs >= m_nextWeaponSwitchUs && bestDist > 10.0f && bestDist < 30.0f) {
            act.buttonMaskLow |= BTN_WEAPON_4;
            m_switchBackToPrimaryUs = nowUs + 1200000ULL; // switch back after 1.2s
            m_nextWeaponSwitchUs = nowUs + static_cast<uint64_t>(randInt(20000, 35000)) * 1000ULL;
        }

        // Reload cadence after 25..35 fire ticks.
        if (m_fireTicksSinceReload >= m_reloadThresholdTicks && nowUs >= m_reloadUntilUs) {
            act.buttonMaskLow |= BTN_RELOAD;
            m_fireTicksSinceReload = 0;
            m_reloadThresholdTicks = randInt(25, 35);
            m_reloadUntilUs = nowUs + 1800000ULL; // ~1.8s reload window
            m_burstFiring = false;
            m_burstTicksLeft = 0;
        }

        // Burst fire when reaction time passed, not reloading, and aim error < 15 deg.
        const bool reacted = (nowUs >= m_targetAcquiredUs + m_reactionDelayUs);
        if (reacted && nowUs >= m_reloadUntilUs && totalErrDeg < 15.0f) {
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
    } else {
        // Out of combat: navigate smoothly along waypoints/route/patrol.
        m_targetNetId = 0;
        if (m_switchBackToPrimaryUs != 0 && nowUs >= m_switchBackToPrimaryUs) {
            act.buttonMaskLow |= BTN_WEAPON_3;
            m_switchBackToPrimaryUs = 0;
        }

        if (hasMoveTarget) {
            const Vec3 lookTarget(moveTarget.x, world.ownPos.y, moveTarget.z);
            float mouseX = 0.0f;
            float mouseY = 0.0f;
            steerToward(world.ownPos, world.ownFacingDegrees, world.ownPitchDegrees,
                        lookTarget, dtSec, mouseX, mouseY);
            act.axes[4] = mouseX;
            act.axes[5] = mouseY;
            act.axes[3] = 1.0f;
            act.buttonMaskLow |= navButtons;
        }
    }

    out.action = act;
}

} // namespace bf2bot
