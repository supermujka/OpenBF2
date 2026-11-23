#ifndef BF2BOT_ROUTE_LOADER_H
#define BF2BOT_ROUTE_LOADER_H

#include "RouteFormat.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf2bot {

struct RouteTrack {
    uint16_t playerId;
    uint8_t team;
    std::vector<RouteActionSample> samples;
};

// Loads "OBR1" v1 binary recordings and splits them into continuous per-player
// trajectories. Read-only thread-safe after load().
class RouteLoader {
public:
    RouteLoader();

    bool load(const std::string& filePath);
    void clear();

    bool isLoaded() const { return m_loaded; }
    size_t sampleCount() const { return m_sampleCount; }
    size_t trackCount() const { return m_tracks.size(); }
    uint32_t sampleHz() const { return m_sampleHz; }
    const std::string& mapName() const { return m_mapName; }

    // Picks a track near (x, z) for the given team (falling back to any track for that
    // team, then any track). Uses rngSeed to diversify among matching tracks.
    bool pickTrackForPosition(uint8_t team, float x, float z, uint32_t rngSeed,
                              size_t& outTrackIdx, size_t& outStartIdx) const;

    const RouteTrack& getTrack(size_t idx) const;

private:
    bool m_loaded;
    uint32_t m_sampleHz;
    size_t m_sampleCount;
    std::string m_mapName;
    std::vector<RouteTrack> m_tracks;
};

} // namespace bf2bot

#endif // BF2BOT_ROUTE_LOADER_H
