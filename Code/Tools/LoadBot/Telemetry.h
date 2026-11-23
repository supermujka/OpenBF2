#ifndef BF2BOT_TELEMETRY_H
#define BF2BOT_TELEMETRY_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf2bot {

struct TelemetrySummary {
    uint32_t configured;
    uint32_t connected;
    uint32_t spawned;
    uint32_t alive;
    uint64_t disconnects;
    uint64_t protocolErrors;

    uint64_t packetsRx;
    uint64_t packetsTx;
    uint64_t bytesRx;
    uint64_t bytesTx;
    uint64_t ghostsTotal;

    uint64_t gapSampleCount;
    double snapHzP50;
    double snapHzP95;
    double snapHzP99;
    double gapMsP50;
    double gapMsP95;
    double gapMsP99;
    double maxGapMs;

    uint64_t enemyNearbyUpdates;
    uint64_t enemyGapOver100ms;
    uint64_t enemyGapOver200ms;
    double enemyGapOver100Pct;
    double enemyGapOver200Pct;
    double maxEnemyGapMs;
};

// Thread-safe aggregate telemetry collector across all bots (periodic window + session totals),
// including snapshot Hz percentiles and nearby-enemy sliding/extrapolation detection.
class Telemetry {
public:
    Telemetry();

    void setBotCounts(uint32_t configured, uint32_t connected,
                      uint32_t spawned, uint32_t alive);
    void recordDisconnect();
    void recordProtocolError();

    void recordPacketTx(size_t bytes);
    void recordPacketRx(size_t bytes, uint32_t ghostCount, double gapMs);

    // Tracks per-(botId, enemyNetId) update intervals for alive enemy soldiers within <50m
    // (60m hysteresis clear, >5000ms respawn/re-scope reset) to detect >100ms / >200ms gaps.
    void recordEnemySoldierUpdate(uint8_t botId, uint16_t enemyNetId,
                                  uint64_t nowUs, float distMeters, bool alive);

    void clearEnemyTrackingForBot(uint8_t botId);
    void clearAllEnemyTracking();

    // Resets periodic window counters while preserving cumulative session totals.
    void resetWindow();
    void resetAll();

    TelemetrySummary getWindowSummary() const;
    TelemetrySummary getTotalSummary() const;

    // Formats a readable console table covering periodic window and cumulative totals.
    std::string formatReport(const std::string& label, double elapsedSec) const;

private:
    struct Bucket {
        uint64_t disconnects;
        uint64_t protocolErrors;
        uint64_t packetsRx;
        uint64_t packetsTx;
        uint64_t bytesRx;
        uint64_t bytesTx;
        uint64_t ghostsTotal;

        uint64_t gapSampleCount;
        double maxGapMs;
        std::vector<double> gapSamplesMs;

        uint64_t enemyNearbyUpdates;
        uint64_t enemyGapOver100ms;
        uint64_t enemyGapOver200ms;
        double maxEnemyGapMs;

        Bucket();
        void reset();
    };

    void appendGapSample(Bucket& b, double gapMs);
    TelemetrySummary buildSummary(const Bucket& b) const;
    uint64_t nextRng();

    mutable std::mutex m_mutex;
    uint32_t m_configured;
    uint32_t m_connected;
    uint32_t m_spawned;
    uint32_t m_alive;

    Bucket m_window;
    Bucket m_total;

    std::unordered_map<uint32_t, uint64_t> m_enemyLastUpdateUs;
    uint64_t m_rngState;
};

} // namespace bf2bot

#endif // BF2BOT_TELEMETRY_H
