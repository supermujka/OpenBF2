#ifndef BF2BOT_TEAM_TACTICS_H
#define BF2BOT_TEAM_TACTICS_H

#include "WireBitStream.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace bf2bot {

enum BotTacticalRole {
    ROLE_POINTMAN = 0,
    ROLE_FLANKER  = 1,
    ROLE_SUPPORT  = 2,
    ROLE_MEDIC    = 3,
    ROLE_DEFENDER = 4
};

struct SpottedEnemy {
    uint16_t enemyNetId;
    Vec3 pos;
    Vec3 vel;
    uint64_t spottedUs;

    SpottedEnemy() : enemyNetId(0), pos(), vel(), spottedUs(0) {}
};

// Lightweight in-process team blackboard shared by bots on the same team.
// Models BF2 Coop SquadCoordinator + StrategicAreas.ai tactics:
//   - Q-spotted enemy callouts (4.5s TTL)
//   - Friendly death heat zones (18s TTL)
//   - Objective claim balancing across frontline flags
//   - 4-man squad cohesion: isSquadTakingFlag, pointman rally-wait, bounding overwatch
//   - DICE Strike at Karkand Coop StrategicAreas.ai multi-lane assault corridors
class TeamTactics {
public:
    static const uint64_t kSpotExpiryUs  = 4500000ULL;  // 4.5s spotted callout TTL
    static const uint64_t kDeathExpiryUs = 18000000ULL; // 18s danger-zone TTL
    static const uint64_t kClaimExpiryUs = 10000000ULL; // 10s objective claim TTL

    TeamTactics() {}

    void reset() {
        for (int t = 0; t < 2; ++t) {
            m_teams[t].spotted.clear();
            m_teams[t].deaths.clear();
            m_teams[t].claims.clear();
            m_teams[t].members.clear();
        }
    }

    static BotTacticalRole roleForBot(uint32_t botIndex, int32_t kit) {
        if (kit == 5) {
            return ROLE_MEDIC;
        }
        if (kit == 3) {
            return ROLE_SUPPORT;
        }
        if (kit == 0) {
            return ROLE_FLANKER;
        }
        const uint32_t slot = (botIndex / 2u) % 4u;
        if (slot == 1u) {
            return ROLE_FLANKER;
        }
        if (slot == 3u) {
            return ROLE_DEFENDER;
        }
        return ROLE_POINTMAN;
    }

    static uint32_t squadForBot(uint32_t botIndex) {
        return (botIndex / 2u) / 4u;
    }

    static uint32_t slotInSquadForBot(uint32_t botIndex) {
        return (botIndex / 2u) % 4u;
    }

    static float flankSideForBot(uint32_t botIndex) {
        return ((squadForBot(botIndex) & 1u) == 0u) ? 1.0f : -1.0f;
    }

    void reportSpottedEnemy(int32_t team, uint16_t enemyNetId,
                            const Vec3& pos, const Vec3& vel, uint64_t nowUs) {
        TeamBoard* b = boardForTeam(team);
        if (!b || enemyNetId == 0u) {
            return;
        }
        SpottedEnemy& s = b->spotted[enemyNetId];
        s.enemyNetId = enemyNetId;
        s.pos = pos;
        s.vel = vel;
        s.spottedUs = nowUs;
    }

    void clearSpottedEnemy(int32_t team, uint16_t enemyNetId) {
        TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }
        b->spotted.erase(enemyNetId);
    }

    bool findNearestSpotted(int32_t team, const Vec3& fromPos, float maxRadius,
                            uint64_t nowUs, SpottedEnemy& outSpot) const {
        const TeamBoard* b = boardForTeam(team);
        if (!b || b->spotted.empty()) {
            return false;
        }
        const float maxSq = maxRadius * maxRadius;
        float bestSq = maxSq;
        bool found = false;
        for (std::map<uint16_t, SpottedEnemy>::const_iterator it = b->spotted.begin();
             it != b->spotted.end(); ++it) {
            const SpottedEnemy& s = it->second;
            if (nowUs > s.spottedUs && (nowUs - s.spottedUs) > kSpotExpiryUs) {
                continue;
            }
            const float dx = s.pos.x - fromPos.x;
            const float dz = s.pos.z - fromPos.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestSq) {
                bestSq = d2;
                outSpot = s;
                found = true;
            }
        }
        return found;
    }

    uint32_t countSpottedNear(int32_t team, const Vec3& center, float radius,
                              uint64_t nowUs) const {
        const TeamBoard* b = boardForTeam(team);
        if (!b || b->spotted.empty()) {
            return 0u;
        }
        const float rSq = radius * radius;
        uint32_t count = 0u;
        for (std::map<uint16_t, SpottedEnemy>::const_iterator it = b->spotted.begin();
             it != b->spotted.end(); ++it) {
            const SpottedEnemy& s = it->second;
            if (nowUs > s.spottedUs && (nowUs - s.spottedUs) > kSpotExpiryUs) {
                continue;
            }
            const float dx = s.pos.x - center.x;
            const float dz = s.pos.z - center.z;
            if (dx * dx + dz * dz <= rSq) {
                ++count;
            }
        }
        return count;
    }

    void reportFriendlyDeath(int32_t team, const Vec3& pos, uint64_t nowUs) {
        TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }
        DeathMark dm;
        dm.pos = pos;
        dm.deathUs = nowUs;
        if (b->deaths.size() >= 24u) {
            b->deaths.erase(b->deaths.begin());
        }
        b->deaths.push_back(dm);
    }

    uint32_t dangerHeatNear(int32_t team, const Vec3& pos, float radius,
                            uint64_t nowUs) const {
        const TeamBoard* b = boardForTeam(team);
        if (!b || b->deaths.empty()) {
            return 0u;
        }
        const float rSq = radius * radius;
        uint32_t count = 0u;
        for (size_t i = 0; i < b->deaths.size(); ++i) {
            const DeathMark& d = b->deaths[i];
            if (nowUs > d.deathUs && (nowUs - d.deathUs) > kDeathExpiryUs) {
                continue;
            }
            const float dx = d.pos.x - pos.x;
            const float dz = d.pos.z - pos.z;
            if (dx * dx + dz * dz <= rSq) {
                ++count;
            }
        }
        return count;
    }

    void claimObjective(int32_t team, uint32_t botIndex, uint32_t objectiveKey,
                        uint64_t nowUs) {
        TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }
        ObjectiveClaim& c = b->claims[botIndex];
        c.objectiveKey = objectiveKey;
        c.updatedUs = nowUs;
    }

    void clearClaim(int32_t team, uint32_t botIndex) {
        TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }
        b->claims.erase(botIndex);
        b->members.erase(botIndex);
    }

    uint32_t countClaimants(int32_t team, uint32_t objectiveKey,
                            uint32_t excludeBotIndex, uint64_t nowUs) const {
        const TeamBoard* b = boardForTeam(team);
        if (!b || b->claims.empty()) {
            return 0u;
        }
        uint32_t count = 0u;
        for (std::map<uint32_t, ObjectiveClaim>::const_iterator it = b->claims.begin();
             it != b->claims.end(); ++it) {
            if (it->first == excludeBotIndex) {
                continue;
            }
            if (nowUs > it->second.updatedUs && (nowUs - it->second.updatedUs) > kClaimExpiryUs) {
                continue;
            }
            if (it->second.objectiveKey == objectiveKey) {
                ++count;
            }
        }
        return count;
    }

    // ========================================================================
    // BF2 Coop SquadCoordinator & Karkand StrategicAreas.ai Assault Lanes
    // ========================================================================

    void updateSquadMember(int32_t team, uint32_t botIndex, const Vec3& pos,
                           bool alive, bool isFiring, uint32_t objectiveKey,
                           bool insideCapZone, uint64_t nowUs) {
        TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }
        SquadMemberState& m = b->members[botIndex];
        m.pos = pos;
        m.alive = alive;
        if (isFiring) {
            m.lastFireUs = nowUs;
        }
        m.objectiveKey = objectiveKey;
        m.insideCapZone = insideCapZone;
        m.updatedUs = nowUs;
    }

    // Mirrors BF2 Coop SquadCoordinator::isSquadTakingFlag, updateWaitForSquadMembers,
    // and squadManeuverAlternateFire (bounding overwatch).
    void querySquadCohesion(int32_t team, uint32_t botIndex, const Vec3& ownPos,
                            uint32_t objectiveKey, const Vec3& flagPos, uint64_t nowUs,
                            uint32_t& outSquadFiringCount,
                            bool& outSquadTakingFlag,
                            bool& outShouldRallyWait,
                            bool& outManeuverAdvance) const {
        outSquadFiringCount = 0u;
        outSquadTakingFlag = false;
        outShouldRallyWait = false;
        outManeuverAdvance = false;

        const TeamBoard* b = boardForTeam(team);
        if (!b) {
            return;
        }

        const uint32_t mySquad = squadForBot(botIndex);
        const float myDistToFlag = distXZLocal(ownPos, flagPos);
        bool anyCloserMember = false;
        uint32_t trailingCatchingUp = 0u;

        for (std::map<uint32_t, SquadMemberState>::const_iterator it = b->members.begin();
             it != b->members.end(); ++it) {
            if (squadForBot(it->first) != mySquad) {
                continue;
            }
            const SquadMemberState& m = it->second;
            if (!m.alive || (nowUs > m.updatedUs && (nowUs - m.updatedUs) > 3000000ULL)) {
                continue;
            }
            if (m.lastFireUs != 0 && nowUs >= m.lastFireUs && (nowUs - m.lastFireUs) < 1300000ULL &&
                distXZLocal(ownPos, m.pos) < 85.0f) {
                ++outSquadFiringCount;
            }
            if (objectiveKey != 0u && m.objectiveKey == objectiveKey) {
                if (m.insideCapZone) {
                    outSquadTakingFlag = true;
                }
                if (it->first != botIndex) {
                    const float mateDist = distXZLocal(m.pos, flagPos);
                    if (mateDist + 2.0f < myDistToFlag) {
                        anyCloserMember = true;
                    } else if (mateDist > myDistToFlag + 8.0f && mateDist < myDistToFlag + 46.0f) {
                        ++trailingCatchingUp;
                    }
                }
            }
        }

        // Pointman waits briefly at staging cover (15..34m out) so trailing squadmates rally for a wave breach!
        if (!outSquadTakingFlag && !anyCloserMember &&
            myDistToFlag >= 15.0f && myDistToFlag <= 34.0f && trailingCatchingUp > 0u) {
            outShouldRallyWait = true;
        }

        // Bounding overwatch (squadManeuverAlternateFire): when >=2 squadmates are firing,
        // alternate 3.2s advance windows between odd & even squad slots.
        if (outSquadFiringCount >= 2u) {
            const uint32_t phase = static_cast<uint32_t>((nowUs / 3200000ULL) & 1ULL);
            outManeuverAdvance = ((slotInSquadForBot(botIndex) & 1u) == phase);
        }
    }

    // Returns DICE Coop StrategicAreas.ai multi-lane intermediate assault waypoints on Strike at Karkand
    // (selecting low-death-heat flank corridors per squad and skipping waypoints already passed).
    bool selectKarkandCoopWaypoints(int32_t team, uint32_t squadId,
                                    const Vec3& fromPos, const Vec3& goalPos,
                                    uint64_t nowUs,
                                    std::vector<Vec3>& outWaypoints) const {
        outWaypoints.clear();
        const float totalDist = distXZLocal(fromPos, goalPos);
        if (totalDist < 34.0f) {
            return false;
        }

        const Vec3 hotelAnchor(-194.4f, 158.2f, 11.4f);
        const Vec3 squareAnchor(-202.4f, 155.7f, 97.7f);
        const Vec3 marketAnchor(-196.3f, 149.8f, 181.3f);

        int sector = -1;
        bool reverseDir = false;

        if (distXZLocal(goalPos, hotelAnchor) < 42.0f && fromPos.z < -28.0f) {
            sector = 0; reverseDir = false;
        } else if (distXZLocal(goalPos, squareAnchor) < 45.0f && fromPos.z < 55.0f && fromPos.z > -60.0f) {
            sector = 1; reverseDir = false;
        } else if (distXZLocal(goalPos, marketAnchor) < 45.0f && fromPos.z > 55.0f && fromPos.z < 145.0f) {
            sector = 2; reverseDir = false;
        } else if (distXZLocal(goalPos, hotelAnchor) < 42.0f && fromPos.z > 50.0f && fromPos.z < 140.0f) {
            sector = 1; reverseDir = true; // MEC counter-attacking Hotel from Square!
        }

        if (sector < 0) {
            return false;
        }

        // Exact DICE gpm_coop/16/ai/StrategicAreas.ai infantry waypoints & flank staging positions.
        struct LaneDef {
            int count;
            float pts[3][3];
        };

        static const LaneDef kGasToHotel[3] = {
            // Lane 0: USBASE_LEFTFLANK west ridge & rear-west Hotel alley
            { 3, { {-240.1f, 163.4f, -143.1f}, {-247.3f, 155.8f, -40.7f}, {-232.5f, 155.8f, 14.1f} } },
            // Lane 1: Center-west street cover breach
            { 3, { {-219.1f, 157.9f,  -72.7f}, {-215.7f, 155.8f, -52.4f}, {-181.0f, 155.8f, -16.3f} } },
            // Lane 2: USBASE_RIGHTFLANK east compound & staircase flank
            { 3, { {-162.7f, 162.5f, -127.9f}, {-164.9f, 155.8f, -32.2f}, {-167.6f, 155.8f,   0.0f} } }
        };

        static const LaneDef kHotelToSquare[4] = {
            // Lane 0: Far-west back alley into Square
            { 2, { {-238.8f, 155.8f, 56.8f}, {-213.7f, 155.8f, 75.0f}, {0.0f, 0.0f, 0.0f} } },
            // Lane 1: West-center arch exit
            { 1, { {-202.0f, 155.8f, 37.3f}, {0.0f, 0.0f, 0.0f},       {0.0f, 0.0f, 0.0f} } },
            // Lane 2: East-center main street cover
            { 2, { {-185.3f, 155.8f, 36.8f}, {-171.4f, 155.8f, 41.5f}, {0.0f, 0.0f, 0.0f} } },
            // Lane 3: Wide east boulevard flank
            { 2, { {-167.0f, 155.8f, 13.0f}, {-136.4f, 155.8f, 46.7f}, {0.0f, 0.0f, 0.0f} } }
        };

        static const LaneDef kSquareToMarket[3] = {
            { 2, { {-229.2f, 155.8f, 134.9f}, {-229.7f, 155.8f, 167.4f}, {0.0f, 0.0f, 0.0f} } },
            { 1, { {-193.8f, 150.7f, 126.1f}, {0.0f, 0.0f, 0.0f},        {0.0f, 0.0f, 0.0f} } },
            { 2, { {-154.5f, 155.8f, 116.4f}, {-155.1f, 149.8f, 168.1f}, {0.0f, 0.0f, 0.0f} } }
        };

        const LaneDef* lanes = nullptr;
        uint32_t numLanes = 0u;
        if (sector == 0)      { lanes = kGasToHotel;     numLanes = 3u; }
        else if (sector == 1) { lanes = kHotelToSquare;  numLanes = 4u; }
        else                  { lanes = kSquareToMarket; numLanes = 3u; }

        // Choose the squad's preferred lane, diverting away from lanes with heavy friendly death heat!
        uint32_t bestLane = squadId % numLanes;
        int32_t bestScore = -999999;
        for (uint32_t l = 0u; l < numLanes; ++l) {
            const uint32_t candLane = (squadId + l) % numLanes;
            uint32_t heat = 0u;
            for (int k = 0; k < lanes[candLane].count; ++k) {
                const Vec3 wp(lanes[candLane].pts[k][0], lanes[candLane].pts[k][1], lanes[candLane].pts[k][2]);
                heat += dangerHeatNear(team, wp, 24.0f, nowUs);
            }
            const int32_t prefBonus = (l == 0u) ? 3 : 0; // stick to squad lane unless >=2 deaths there
            const int32_t score = prefBonus - static_cast<int32_t>(heat) * 2;
            if (score > bestScore) {
                bestScore = score;
                bestLane = candLane;
            }
        }

        const LaneDef& chosen = lanes[bestLane];
        const float dirX = (goalPos.x - fromPos.x) / totalDist;
        const float dirZ = (goalPos.z - fromPos.z) / totalDist;

        for (int idx = 0; idx < chosen.count; ++idx) {
            const int k = reverseDir ? (chosen.count - 1 - idx) : idx;
            const Vec3 wp(chosen.pts[k][0], chosen.pts[k][1], chosen.pts[k][2]);
            const float proj = (wp.x - fromPos.x) * dirX + (wp.z - fromPos.z) * dirZ;
            const float distGoal = distXZLocal(wp, goalPos);
            if (proj > 10.0f && distGoal > 14.0f) {
                outWaypoints.push_back(wp);
            }
        }

        return !outWaypoints.empty();
    }

private:
    struct DeathMark {
        Vec3 pos;
        uint64_t deathUs;
    };

    struct ObjectiveClaim {
        uint32_t objectiveKey;
        uint64_t updatedUs;
    };

    struct SquadMemberState {
        Vec3 pos;
        bool alive;
        uint64_t lastFireUs;
        uint32_t objectiveKey;
        bool insideCapZone;
        uint64_t updatedUs;

        SquadMemberState()
            : pos(), alive(false), lastFireUs(0),
              objectiveKey(0u), insideCapZone(false), updatedUs(0) {}
    };

    static float distXZLocal(const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    struct TeamBoard {
        std::map<uint16_t, SpottedEnemy> spotted;
        std::vector<DeathMark> deaths;
        std::map<uint32_t, ObjectiveClaim> claims;
        std::map<uint32_t, SquadMemberState> members;
    };

    TeamBoard* boardForTeam(int32_t team) {
        if (team == 1) return &m_teams[0];
        if (team == 2) return &m_teams[1];
        return nullptr;
    }

    const TeamBoard* boardForTeam(int32_t team) const {
        if (team == 1) return &m_teams[0];
        if (team == 2) return &m_teams[1];
        return nullptr;
    }

    TeamBoard m_teams[2];
};

} // namespace bf2bot

#endif // BF2BOT_TEAM_TACTICS_H
