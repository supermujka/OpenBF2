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
                   const NavMesh* navMesh, const RouteLoader* routeLoader,
                   const AimConfig& aim)
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
      m_aim(aim),
      m_aimInitYawDeg(0.0f),
      m_aimInitPitchDeg(0.0f),
      m_aimConvergeUs(450000ULL),
      m_jitterYawDeg(0.0f),
      m_jitterPitchDeg(0.0f),
      m_nextJitterUs(0),
      m_fireState(FIRE_WAIT),
      m_fireStateEndUs(0),
      m_burstShots(0),
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
    resetFireState();
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
    resetFireState();
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
                            const TrackedSoldier*& outTarget, Vec3& outRendered, float& outDist) {
    outTarget = nullptr;
    if (!world.soldiers) {
        return false;
    }
    const uint64_t interpUs = static_cast<uint64_t>(m_aim.interpMs > 0 ? m_aim.interpMs : 0) * 1000ULL;
    const uint64_t renderUs = nowUs > interpUs ? nowUs - interpUs : 0ULL;
    const Vec3 ownEye(world.ownPos.x, world.ownPos.y + 1.55f, world.ownPos.z);

    float bestDist = 1e9f;
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
        const bool current = (s.netId == m_targetNetId);
        const float d = dist3D(world.ownPos, r);
        if (d > (current ? 90.0f : 80.0f)) {
            continue;
        }
        const float bearing = std::atan2(r.x - world.ownPos.x, r.z - world.ownPos.z) * kRadToDeg;
        const float off = std::fabs(wrapAngleDeg(bearing - world.ownFacingDegrees));
        // Within 10 m a player hears and notices an enemy in any direction.
        if (d >= 10.0f && off > (current ? 70.0f : 45.0f)) {
            continue;
        }
        if (!terrainLineOfSight(m_navMesh, ownEye, Vec3(r.x, r.y + 1.4f, r.z))) {
            continue;
        }
        if (current) {
            // Stickiness: keep the current target as long as it stays valid.
            outTarget = &s;
            outRendered = r;
            outDist = d;
            return true;
        }
        if (d < bestDist) {
            bestDist = d;
            outTarget = &s;
            outRendered = r;
            outDist = d;
        }
    }
    return outTarget != nullptr;
}

void BotBrain::tickCombatHuman(uint64_t nowUs, float dtSec, const BotWorldView& world,
                               const TrackedSoldier& target, const Vec3& rendered, float dist,
                               PlayerActionInput& act) {
    const float skill = clampFloat(m_aim.skill, 0.0f, 1.0f);
    const float reactScale = 1.3f - 0.6f * skill;
    const float errScale = 2.0f - 1.5f * skill;
    const float convScale = 1.4f - 0.8f * skill;

    if (target.netId != m_targetNetId) {
        m_targetNetId = target.netId;
        m_targetAcquiredUs = nowUs;
        m_reactionDelayUs = static_cast<uint64_t>(randFloat(150.0f, 350.0f) * reactScale * 1000.0f);
        // Initial error: a few degrees, growing mildly with distance, random direction.
        const float mag = randFloat(1.5f, 4.0f) * errScale * (0.6f + dist / 60.0f);
        m_aimInitYawDeg = randFloat(-1.0f, 1.0f) * mag;
        m_aimInitPitchDeg = randFloat(-1.0f, 1.0f) * mag * 0.6f;
        m_aimConvergeUs = static_cast<uint64_t>(randFloat(300.0f, 600.0f) * convScale * 1000.0f);
        m_nextJitterUs = 0;
        resetFireState();
    }

    // Small jitter, refreshed every ~50-90 ms.
    if (nowUs >= m_nextJitterUs) {
        const float j = 0.35f * errScale;
        m_jitterYawDeg = randFloat(-j, j);
        m_jitterPitchDeg = randFloat(-j, j) * 0.7f;
        m_nextJitterUs = nowUs + static_cast<uint64_t>(randInt(50, 90)) * 1000ULL;
    }

    // Aim point: upper chest by pose (feet-relative).
    float tgtH = 1.4f;
    if (target.pose == 1u) tgtH = 1.0f;
    else if (target.pose == 2u) tgtH = 0.35f;
    else if (target.pose == 3u) tgtH = 0.4f;
    float eyeH = 1.55f;
    if (world.ownPose == 1u) eyeH = 1.1f;
    else if (world.ownPose == 2u) eyeH = 0.45f;

    const float dx = rendered.x - world.ownPos.x;
    const float dy = (rendered.y + tgtH) - (world.ownPos.y + eyeH);
    const float dz = rendered.z - world.ownPos.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);

    const uint64_t tracked = nowUs - m_targetAcquiredUs;
    const float errYaw = aimErrorDecay(m_aimInitYawDeg, tracked, m_aimConvergeUs) + m_jitterYawDeg;
    const float errPitch = aimErrorDecay(m_aimInitPitchDeg, tracked, m_aimConvergeUs) + m_jitterPitchDeg;

    const float trueYaw = std::atan2(dx, dz) * kRadToDeg;
    const float truePitch = clampFloat(-std::atan2(dy, (horiz > 0.05f ? horiz : 0.05f)) * kRadToDeg,
                                       -75.0f, 75.0f);
    const float desiredYaw = trueYaw + errYaw;
    const float desiredPitch = clampFloat(truePitch + errPitch, -75.0f, 75.0f);

    const float yawErr = wrapAngleDeg(desiredYaw - world.ownFacingDegrees);
    const float pitchErr = desiredPitch - world.ownPitchDegrees;
    // Humans flick onto a target far faster than the patrol turn rate.
    const float maxDelta = (400.0f + 200.0f * skill) * dtSec;
    act.axes[4] = clampFloat(yawErr, -maxDelta, maxDelta) / kLookFactor;
    act.axes[5] = clampFloat(pitchErr, -maxDelta * 0.8f, maxDelta * 0.8f) / kLookFactor;

    // Actual aim vs. the true (rendered) point decides whether a shot would plausibly hit.
    const float tyaw = wrapAngleDeg(trueYaw - world.ownFacingDegrees);
    const float tpit = truePitch - world.ownPitchDegrees;
    const float offTarget = std::sqrt(tyaw * tyaw + tpit * tpit);
    const float fireTolerance = 3.0f + 40.0f / (dist > 5.0f ? dist : 5.0f);

    const bool closeRange = dist < 15.0f;
    const bool reacted = nowUs >= m_targetAcquiredUs + m_reactionDelayUs;
    const bool reloading = nowUs < m_reloadUntilUs;
    const bool tapOnly = dist > 40.0f;

    if (m_fireState == FIRE_WAIT) {
        if (reacted && !reloading && offTarget < fireTolerance) {
            m_fireState = FIRE_BURST;
            m_burstShots = tapOnly ? 1 : randInt(3, 5);
            // ~100 ms per round; a tap lasts a single tick so the trigger is released.
            m_fireStateEndUs = nowUs + (tapOnly ? static_cast<uint64_t>(dtSec * 1000000.0f)
                                                : static_cast<uint64_t>(m_burstShots) * 100000ULL);
            m_fireTicksSinceReload += m_burstShots;
        }
    }
    if (m_fireState == FIRE_BURST) {
        act.buttonMaskLow |= BTN_FIRE;
        if (nowUs >= m_fireStateEndUs) {
            m_fireState = FIRE_PAUSE;
            const float pauseMs = randFloat(200.0f, 400.0f) * (tapOnly ? 1.2f : 1.0f);
            m_fireStateEndUs = nowUs + static_cast<uint64_t>(pauseMs * 1000.0f);
            if (closeRange) {
                m_strafeRight = !m_strafeRight;
            }
        }
    } else if (m_fireState == FIRE_PAUSE) {
        // Let fire deviation recover. No strafing: bots mostly meet at close range, and
        // flipping strafe every pause made them jitter and lose their own aim.
        if (nowUs >= m_fireStateEndUs) {
            m_fireState = FIRE_WAIT;
            if (m_fireTicksSinceReload >= m_reloadThresholdTicks && !reloading) {
                act.buttonMaskLow |= BTN_RELOAD;
                m_fireTicksSinceReload = 0;
                m_reloadThresholdTicks = randInt(25, 35);
                m_reloadUntilUs = nowUs + 1800000ULL;
            }
        }
    }

    // Stand still and crouch while engaging so speed deviation decays and crouch spread applies.
    (void)closeRange;
    act.buttonMaskLow |= BTN_CROUCH;
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
    Vec3 targetRendered;
    float targetDist = 0.0f;
    float bestDist = kCombatRangeMeters;
    if (!m_aim.legacy) {
        selectTarget(nowUs, world, bestTarget, targetRendered, targetDist);
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
    if (inCombat && m_aim.legacy) {
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
    } else if (inCombat) {
        tickCombatHuman(nowUs, dtSec, world, *bestTarget, targetRendered, targetDist, act);
    } else {
        // Out of combat: navigate smoothly along waypoints/route/patrol.
        resetFireState();
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
