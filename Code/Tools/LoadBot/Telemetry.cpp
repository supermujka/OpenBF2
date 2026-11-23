#include "Telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace bf2bot {

namespace {

static const size_t MAX_STORED_GAP_SAMPLES = 262144u;
static const float ENEMY_NEAR_RADIUS_M = 50.0f;
static const float ENEMY_CLEAR_RADIUS_M = 60.0f;
static const double ENEMY_RESPAWN_GAP_MS = 5000.0;
static const double INTERP_THRESH_AUTO_MS = 100.0;
static const double INTERP_THRESH_VANILLA_MS = 200.0;

double computePercentile(const std::vector<double>& sortedValues, double quantile) {
    if (sortedValues.empty()) {
        return 0.0;
    }
    if (sortedValues.size() == 1u) {
        return sortedValues[0];
    }
    const double pos = quantile * static_cast<double>(sortedValues.size() - 1u);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    if (lo == hi || hi >= sortedValues.size()) {
        return sortedValues[lo];
    }
    const double frac = pos - static_cast<double>(lo);
    return sortedValues[lo] + (sortedValues[hi] - sortedValues[lo]) * frac;
}

std::string fmtDouble(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return std::string(buf);
}

} // namespace

Telemetry::Bucket::Bucket()
    : disconnects(0u)
    , protocolErrors(0u)
    , packetsRx(0u)
    , packetsTx(0u)
    , bytesRx(0u)
    , bytesTx(0u)
    , ghostsTotal(0u)
    , gapSampleCount(0u)
    , maxGapMs(0.0)
    , enemyNearbyUpdates(0u)
    , enemyGapOver100ms(0u)
    , enemyGapOver200ms(0u)
    , maxEnemyGapMs(0.0) {
}

void Telemetry::Bucket::reset() {
    disconnects = 0u;
    protocolErrors = 0u;
    packetsRx = 0u;
    packetsTx = 0u;
    bytesRx = 0u;
    bytesTx = 0u;
    ghostsTotal = 0u;
    gapSampleCount = 0u;
    maxGapMs = 0.0;
    gapSamplesMs.clear();
    enemyNearbyUpdates = 0u;
    enemyGapOver100ms = 0u;
    enemyGapOver200ms = 0u;
    maxEnemyGapMs = 0.0;
}

Telemetry::Telemetry()
    : m_configured(0u)
    , m_connected(0u)
    , m_spawned(0u)
    , m_alive(0u)
    , m_rngState(0x9E3779B97F4A7C15ULL) {
}

uint64_t Telemetry::nextRng() {
    uint64_t x = m_rngState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    m_rngState = x;
    return x * 0x2545F4914F6CDD1DULL;
}

void Telemetry::appendGapSample(Bucket& b, double gapMs) {
    ++b.gapSampleCount;
    if (gapMs > b.maxGapMs) {
        b.maxGapMs = gapMs;
    }
    if (b.gapSamplesMs.size() < MAX_STORED_GAP_SAMPLES) {
        b.gapSamplesMs.push_back(gapMs);
    } else {
        // Reservoir sampling keeps memory bounded on long soak runs while preserving exact max.
        const uint64_t slot = nextRng() % b.gapSampleCount;
        if (slot < MAX_STORED_GAP_SAMPLES) {
            b.gapSamplesMs[static_cast<size_t>(slot)] = gapMs;
        }
    }
}

void Telemetry::setBotCounts(uint32_t configured, uint32_t connected,
                             uint32_t spawned, uint32_t alive) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configured = configured;
    m_connected = connected;
    m_spawned = spawned;
    m_alive = alive;
}

void Telemetry::recordDisconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_window.disconnects;
    ++m_total.disconnects;
}

void Telemetry::recordProtocolError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_window.protocolErrors;
    ++m_total.protocolErrors;
}

void Telemetry::recordPacketTx(size_t bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_window.packetsTx;
    ++m_total.packetsTx;
    m_window.bytesTx += static_cast<uint64_t>(bytes);
    m_total.bytesTx += static_cast<uint64_t>(bytes);
}

void Telemetry::recordPacketRx(size_t bytes, uint32_t ghostCount, double gapMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_window.packetsRx;
    ++m_total.packetsRx;
    m_window.bytesRx += static_cast<uint64_t>(bytes);
    m_total.bytesRx += static_cast<uint64_t>(bytes);
    m_window.ghostsTotal += static_cast<uint64_t>(ghostCount);
    m_total.ghostsTotal += static_cast<uint64_t>(ghostCount);

    if (gapMs > 0.0) {
        appendGapSample(m_window, gapMs);
        appendGapSample(m_total, gapMs);
    }
}

void Telemetry::recordEnemySoldierUpdate(uint8_t botId, uint16_t enemyNetId,
                                         uint64_t nowUs, float distMeters, bool alive) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint32_t key = (static_cast<uint32_t>(botId) << 16) |
                         static_cast<uint32_t>(enemyNetId);

    if (!alive || distMeters > ENEMY_CLEAR_RADIUS_M) {
        m_enemyLastUpdateUs.erase(key);
        return;
    }

    std::unordered_map<uint32_t, uint64_t>::iterator it = m_enemyLastUpdateUs.find(key);
    if (it == m_enemyLastUpdateUs.end()) {
        if (distMeters < ENEMY_NEAR_RADIUS_M) {
            m_enemyLastUpdateUs[key] = nowUs;
        }
        return;
    }

    const uint64_t prevUs = it->second;
    const double gapMs = (nowUs >= prevUs) ? static_cast<double>(nowUs - prevUs) / 1000.0 : 0.0;
    if (gapMs > ENEMY_RESPAWN_GAP_MS) {
        if (distMeters < ENEMY_NEAR_RADIUS_M) {
            it->second = nowUs;
        } else {
            m_enemyLastUpdateUs.erase(it);
        }
        return;
    }

    it->second = nowUs;
    if (distMeters < ENEMY_NEAR_RADIUS_M) {
        ++m_window.enemyNearbyUpdates;
        ++m_total.enemyNearbyUpdates;
        if (gapMs > INTERP_THRESH_AUTO_MS) {
            ++m_window.enemyGapOver100ms;
            ++m_total.enemyGapOver100ms;
        }
        if (gapMs > INTERP_THRESH_VANILLA_MS) {
            ++m_window.enemyGapOver200ms;
            ++m_total.enemyGapOver200ms;
        }
        if (gapMs > m_window.maxEnemyGapMs) {
            m_window.maxEnemyGapMs = gapMs;
        }
        if (gapMs > m_total.maxEnemyGapMs) {
            m_total.maxEnemyGapMs = gapMs;
        }
    }
}

void Telemetry::clearEnemyTrackingForBot(uint8_t botId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint32_t prefix = static_cast<uint32_t>(botId) << 16;
    std::unordered_map<uint32_t, uint64_t>::iterator it = m_enemyLastUpdateUs.begin();
    while (it != m_enemyLastUpdateUs.end()) {
        if ((it->first & 0xFFFF0000u) == prefix) {
            it = m_enemyLastUpdateUs.erase(it);
        } else {
            ++it;
        }
    }
}

void Telemetry::clearAllEnemyTracking() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enemyLastUpdateUs.clear();
}

void Telemetry::resetWindow() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_window.reset();
}

void Telemetry::resetAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_configured = 0u;
    m_connected = 0u;
    m_spawned = 0u;
    m_alive = 0u;
    m_window.reset();
    m_total.reset();
    m_enemyLastUpdateUs.clear();
}

TelemetrySummary Telemetry::buildSummary(const Bucket& b) const {
    TelemetrySummary s;
    s.configured = m_configured;
    s.connected = m_connected;
    s.spawned = m_spawned;
    s.alive = m_alive;
    s.disconnects = b.disconnects;
    s.protocolErrors = b.protocolErrors;

    s.packetsRx = b.packetsRx;
    s.packetsTx = b.packetsTx;
    s.bytesRx = b.bytesRx;
    s.bytesTx = b.bytesTx;
    s.ghostsTotal = b.ghostsTotal;

    s.gapSampleCount = b.gapSampleCount;
    s.maxGapMs = b.maxGapMs;

    if (!b.gapSamplesMs.empty()) {
        std::vector<double> sorted = b.gapSamplesMs;
        std::sort(sorted.begin(), sorted.end());
        s.gapMsP50 = computePercentile(sorted, 0.50);
        s.gapMsP95 = computePercentile(sorted, 0.95);
        s.gapMsP99 = computePercentile(sorted, 0.99);
        s.snapHzP50 = (s.gapMsP50 > 0.001) ? (1000.0 / s.gapMsP50) : 0.0;
        s.snapHzP95 = (s.gapMsP95 > 0.001) ? (1000.0 / s.gapMsP95) : 0.0;
        s.snapHzP99 = (s.gapMsP99 > 0.001) ? (1000.0 / s.gapMsP99) : 0.0;
    } else {
        s.gapMsP50 = 0.0;
        s.gapMsP95 = 0.0;
        s.gapMsP99 = 0.0;
        s.snapHzP50 = 0.0;
        s.snapHzP95 = 0.0;
        s.snapHzP99 = 0.0;
    }

    s.enemyNearbyUpdates = b.enemyNearbyUpdates;
    s.enemyGapOver100ms = b.enemyGapOver100ms;
    s.enemyGapOver200ms = b.enemyGapOver200ms;
    s.maxEnemyGapMs = b.maxEnemyGapMs;
    if (b.enemyNearbyUpdates > 0u) {
        const double denom = static_cast<double>(b.enemyNearbyUpdates);
        s.enemyGapOver100Pct = 100.0 * static_cast<double>(b.enemyGapOver100ms) / denom;
        s.enemyGapOver200Pct = 100.0 * static_cast<double>(b.enemyGapOver200ms) / denom;
    } else {
        s.enemyGapOver100Pct = 0.0;
        s.enemyGapOver200Pct = 0.0;
    }

    return s;
}

TelemetrySummary Telemetry::getWindowSummary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return buildSummary(m_window);
}

TelemetrySummary Telemetry::getTotalSummary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return buildSummary(m_total);
}

std::string Telemetry::formatReport(const std::string& label, double elapsedSec) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const TelemetrySummary win = buildSummary(m_window);
    const TelemetrySummary tot = buildSummary(m_total);

    const double safeSec = (elapsedSec > 0.001) ? elapsedSec : 1.0;
    const double winRxMbps = (static_cast<double>(win.bytesRx) * 8.0) / (safeSec * 1000000.0);
    const double winTxMbps = (static_cast<double>(win.bytesTx) * 8.0) / (safeSec * 1000000.0);

    std::ostringstream oss;
    oss << "=== LoadBot Telemetry [" << label << "] (" << fmtDouble(elapsedSec, 1) << "s) ===\n";
    oss << "  Bots   : cfg=" << win.configured
        << " conn=" << win.connected
        << " spawn=" << win.spawned
        << " alive=" << win.alive
        << " | disc=" << win.disconnects << " (tot " << tot.disconnects << ")"
        << " protoErr=" << win.protocolErrors << " (tot " << tot.protocolErrors << ")\n";

    oss << "  Window : rx=" << win.packetsRx << " pkts (" << win.bytesRx << " B, "
        << fmtDouble(winRxMbps, 2) << " Mbps, ghosts=" << win.ghostsTotal << ")"
        << " | tx=" << win.packetsTx << " pkts (" << win.bytesTx << " B, "
        << fmtDouble(winTxMbps, 2) << " Mbps)\n";

    oss << "  SnapHz : win p50/p95/p99="
        << fmtDouble(win.snapHzP50, 1) << "/"
        << fmtDouble(win.snapHzP95, 1) << "/"
        << fmtDouble(win.snapHzP99, 1) << " Hz"
        << " (gap p50/p95/p99/max="
        << fmtDouble(win.gapMsP50, 1) << "/"
        << fmtDouble(win.gapMsP95, 1) << "/"
        << fmtDouble(win.gapMsP99, 1) << "/"
        << fmtDouble(win.maxGapMs, 1) << " ms)\n";

    oss << "  Total  : rx=" << tot.packetsRx << " pkts (" << tot.bytesRx << " B, ghosts="
        << tot.ghostsTotal << ") | tx=" << tot.packetsTx << " pkts (" << tot.bytesTx << " B)\n";

    oss << "  TotHz  : tot p50/p95/p99="
        << fmtDouble(tot.snapHzP50, 1) << "/"
        << fmtDouble(tot.snapHzP95, 1) << "/"
        << fmtDouble(tot.snapHzP99, 1) << " Hz"
        << " (gap p50/p95/p99/max="
        << fmtDouble(tot.gapMsP50, 1) << "/"
        << fmtDouble(tot.gapMsP95, 1) << "/"
        << fmtDouble(tot.gapMsP99, 1) << "/"
        << fmtDouble(tot.maxGapMs, 1) << " ms)\n";

    oss << "  Slide  : win (<50m)=" << win.enemyNearbyUpdates
        << " >100ms=" << win.enemyGapOver100ms << " (" << fmtDouble(win.enemyGapOver100Pct, 2) << "%)"
        << " >200ms=" << win.enemyGapOver200ms << " (" << fmtDouble(win.enemyGapOver200Pct, 2) << "%)"
        << " max=" << fmtDouble(win.maxEnemyGapMs, 1) << " ms"
        << " | tot=" << tot.enemyNearbyUpdates
        << " >100ms=" << tot.enemyGapOver100ms << " (" << fmtDouble(tot.enemyGapOver100Pct, 2) << "%)"
        << " >200ms=" << tot.enemyGapOver200ms << " (" << fmtDouble(tot.enemyGapOver200Pct, 2) << "%)"
        << " max=" << fmtDouble(tot.maxEnemyGapMs, 1) << " ms\n";

    return oss.str();
}

} // namespace bf2bot
