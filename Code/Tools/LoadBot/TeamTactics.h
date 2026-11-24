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
// Models realistic BF2 team comms: short-lived Q-spotted enemy callouts (4.5s),
// recent friendly death heat zones (18s), and objective claim balancing so squads
// distribute across frontline flags and flank instead of clumping blindly.
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
        return (botIndex / 2u) / 5u;
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

private:
    struct DeathMark {
        Vec3 pos;
        uint64_t deathUs;
    };

    struct ObjectiveClaim {
        uint32_t objectiveKey;
        uint64_t updatedUs;
    };

    struct TeamBoard {
        std::map<uint16_t, SpottedEnemy> spotted;
        std::vector<DeathMark> deaths;
        std::map<uint32_t, ObjectiveClaim> claims;
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
