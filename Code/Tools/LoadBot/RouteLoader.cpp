#include "RouteLoader.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <map>

namespace bf2bot {

namespace {

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

inline int16_t readI16LE(const uint8_t* p) {
    const uint16_t u = readU16LE(p);
    int16_t v = 0;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline float readF32LE(const uint8_t* p) {
    const uint32_t u = readU32LE(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

RouteActionSample parseSampleLE(const uint8_t* p) {
    RouteActionSample s;
    s.tick = readU32LE(p + 0u);
    s.playerId = readU16LE(p + 4u);
    s.team = p[6];
    s.stance = p[7];
    s.posX = readF32LE(p + 8u);
    s.posY = readF32LE(p + 12u);
    s.posZ = readF32LE(p + 16u);
    s.yawDeg = readF32LE(p + 20u);
    s.pitchDeg = readF32LE(p + 24u);
    s.actionFlags = readU32LE(p + 28u);
    s.moveForward = readI16LE(p + 32u);
    s.moveStrafe = readI16LE(p + 34u);
    return s;
}

struct TrackCandidate {
    size_t trackIdx;
    size_t sampleIdx;
    float distSq;
};

} // namespace

RouteLoader::RouteLoader()
    : m_loaded(false)
    , m_sampleHz(0u)
    , m_sampleCount(0u) {
}

void RouteLoader::clear() {
    m_loaded = false;
    m_sampleHz = 0u;
    m_sampleCount = 0u;
    m_mapName.clear();
    m_tracks.clear();
}

bool RouteLoader::load(const std::string& filePath) {
    clear();
    if (filePath.empty()) {
        return false;
    }

    std::ifstream ifs(filePath.c_str(), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return false;
    }

    const std::ifstream::pos_type endPos = ifs.tellg();
    if (endPos < static_cast<std::ifstream::pos_type>(sizeof(RouteActionHeader))) {
        return false;
    }
    const size_t fileSize = static_cast<size_t>(endPos);
    ifs.seekg(0, std::ios::beg);

    uint8_t hdrBytes[32];
    if (!ifs.read(reinterpret_cast<char*>(hdrBytes), 32)) {
        return false;
    }

    const char* magic = routeMagicBytes();
    if (hdrBytes[0] != static_cast<uint8_t>(magic[0]) ||
        hdrBytes[1] != static_cast<uint8_t>(magic[1]) ||
        hdrBytes[2] != static_cast<uint8_t>(magic[2]) ||
        hdrBytes[3] != static_cast<uint8_t>(magic[3])) {
        return false;
    }

    const uint32_t version = readU32LE(hdrBytes + 4u);
    const uint32_t sampleHz = readU32LE(hdrBytes + 8u);
    const uint32_t recordSize = readU32LE(hdrBytes + 12u);
    if (version != ROUTE_VERSION || sampleHz == 0u || sampleHz > 120u ||
        recordSize != sizeof(RouteActionSample)) {
        return false;
    }

    size_t mapLen = 0u;
    while (mapLen < 16u && hdrBytes[16u + mapLen] != 0u) {
        ++mapLen;
    }
    m_mapName.assign(reinterpret_cast<const char*>(hdrBytes + 16u), mapLen);
    m_sampleHz = sampleHz;

    const size_t payloadBytes = fileSize - 32u;
    const size_t numRecords = payloadBytes / sizeof(RouteActionSample);
    if (numRecords == 0u) {
        m_loaded = true;
        return true;
    }

    std::vector<uint8_t> rawRecords(numRecords * sizeof(RouteActionSample));
    if (!ifs.read(reinterpret_cast<char*>(&rawRecords[0]),
                  static_cast<std::streamsize>(rawRecords.size()))) {
        clear();
        return false;
    }

    // Group into continuous trajectories per (playerId, team).
    const uint32_t maxTickGap = sampleHz * 5u;
    const float maxTeleportSq = 15.0f * 15.0f;

    std::map<uint32_t, size_t> activeTrackByActor;

    for (size_t i = 0; i < numRecords; ++i) {
        const RouteActionSample s = parseSampleLE(&rawRecords[i * sizeof(RouteActionSample)]);
        const uint32_t actorKey = (static_cast<uint32_t>(s.playerId) << 16) |
                                  static_cast<uint32_t>(s.team);

        std::map<uint32_t, size_t>::iterator it = activeTrackByActor.find(actorKey);
        bool startNewTrack = (it == activeTrackByActor.end());

        if (!startNewTrack) {
            const RouteTrack& curTrack = m_tracks[it->second];
            const RouteActionSample& prev = curTrack.samples.back();
            if (s.tick < prev.tick || (s.tick - prev.tick) > maxTickGap) {
                startNewTrack = true;
            } else {
                const float dx = s.posX - prev.posX;
                const float dy = s.posY - prev.posY;
                const float dz = s.posZ - prev.posZ;
                if ((dx * dx + dy * dy + dz * dz) > maxTeleportSq) {
                    startNewTrack = true;
                }
            }
        }

        if (startNewTrack) {
            RouteTrack track;
            track.playerId = s.playerId;
            track.team = s.team;
            track.samples.push_back(s);
            activeTrackByActor[actorKey] = m_tracks.size();
            m_tracks.push_back(track);
        } else {
            m_tracks[it->second].samples.push_back(s);
        }
    }

    m_sampleCount = numRecords;
    m_loaded = true;
    return true;
}

bool RouteLoader::pickTrackForPosition(uint8_t team, float x, float z, uint32_t rngSeed,
                                       size_t& outTrackIdx, size_t& outStartIdx) const {
    if (!m_loaded || m_tracks.empty()) {
        return false;
    }

    const float nearRadiusSq = 40.0f * 40.0f;

    std::vector<TrackCandidate> teamNear;
    std::vector<TrackCandidate> teamAll;
    std::vector<TrackCandidate> anyNear;
    std::vector<TrackCandidate> anyAll;

    bool hasMultiSampleTeam = false;
    bool hasMultiSampleAny = false;
    for (size_t t = 0; t < m_tracks.size(); ++t) {
        if (m_tracks[t].samples.size() >= 2u) {
            hasMultiSampleAny = true;
            if (m_tracks[t].team == team) {
                hasMultiSampleTeam = true;
            }
        }
    }

    for (size_t t = 0; t < m_tracks.size(); ++t) {
        const RouteTrack& tr = m_tracks[t];
        if (tr.samples.empty()) {
            continue;
        }
        const bool isTeam = (tr.team == team);
        if (isTeam && hasMultiSampleTeam && tr.samples.size() < 2u) {
            continue;
        }
        if (!isTeam && hasMultiSampleAny && tr.samples.size() < 2u) {
            continue;
        }

        size_t bestIdx = 0u;
        float bestDistSq = std::numeric_limits<float>::max();
        for (size_t s = 0; s < tr.samples.size(); ++s) {
            const float dx = tr.samples[s].posX - x;
            const float dz = tr.samples[s].posZ - z;
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestDistSq) {
                bestDistSq = d2;
                bestIdx = s;
            }
        }

        TrackCandidate cand;
        cand.trackIdx = t;
        cand.sampleIdx = bestIdx;
        cand.distSq = bestDistSq;

        anyAll.push_back(cand);
        if (bestDistSq <= nearRadiusSq) {
            anyNear.push_back(cand);
        }
        if (isTeam) {
            teamAll.push_back(cand);
            if (bestDistSq <= nearRadiusSq) {
                teamNear.push_back(cand);
            }
        }
    }

    const std::vector<TrackCandidate>* pool = 0;
    if (!teamNear.empty()) {
        pool = &teamNear;
    } else if (!teamAll.empty()) {
        pool = &teamAll;
    } else if (!anyNear.empty()) {
        pool = &anyNear;
    } else if (!anyAll.empty()) {
        pool = &anyAll;
    } else {
        return false;
    }

    const TrackCandidate& chosen = (*pool)[static_cast<size_t>(rngSeed) % pool->size()];
    outTrackIdx = chosen.trackIdx;
    outStartIdx = chosen.sampleIdx;
    return true;
}

const RouteTrack& RouteLoader::getTrack(size_t idx) const {
    static const RouteTrack kEmptyTrack = {0u, 0u, std::vector<RouteActionSample>()};
    if (idx >= m_tracks.size()) {
        return kEmptyTrack;
    }
    return m_tracks[idx];
}

} // namespace bf2bot
