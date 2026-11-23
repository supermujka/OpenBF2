#include "NetImpairment.h"

namespace bf2bot {

namespace {

ImpairmentConfig defaultConfig() {
    ImpairmentConfig cfg;
    cfg.oneWayDelayMs = 0.0;
    cfg.jitterMs = 0.0;
    cfg.lossPct = 0.0;
    cfg.seed = 1u;
    return cfg;
}

} // namespace

ImpairmentQueue::ImpairmentQueue()
    : m_config(defaultConfig())
    , m_rng(m_config.seed)
    , m_nextSeq(0u)
    , m_enqueuedCount(0u)
    , m_droppedCount(0u)
    , m_deliveredCount(0u) {
}

ImpairmentQueue::ImpairmentQueue(const ImpairmentConfig& cfg)
    : m_config(cfg)
    , m_rng(cfg.seed != 0u ? cfg.seed : 1u)
    , m_nextSeq(0u)
    , m_enqueuedCount(0u)
    , m_droppedCount(0u)
    , m_deliveredCount(0u) {
}

void ImpairmentQueue::reset(const ImpairmentConfig& cfg) {
    m_config = cfg;
    m_rng.seed(cfg.seed != 0u ? cfg.seed : 1u);
    clear();
}

void ImpairmentQueue::clear() {
    while (!m_heap.empty()) {
        m_heap.pop();
    }
    m_nextSeq = 0u;
    m_enqueuedCount = 0u;
    m_droppedCount = 0u;
    m_deliveredCount = 0u;
}

bool ImpairmentQueue::isActive() const {
    return m_config.oneWayDelayMs > 0.0 ||
           m_config.jitterMs > 0.0 ||
           m_config.lossPct > 0.0;
}

bool ImpairmentQueue::enqueue(uint64_t nowUs, const uint8_t* data, size_t len) {
    ++m_enqueuedCount;

    if (m_config.lossPct > 0.0) {
        if (m_config.lossPct >= 100.0) {
            ++m_droppedCount;
            return false;
        }
        std::uniform_real_distribution<double> lossDist(0.0, 100.0);
        if (lossDist(m_rng) < m_config.lossPct) {
            ++m_droppedCount;
            return false;
        }
    }

    double delayMs = m_config.oneWayDelayMs;
    if (m_config.jitterMs > 0.0) {
        std::uniform_real_distribution<double> jitDist(-m_config.jitterMs, m_config.jitterMs);
        delayMs += jitDist(m_rng);
    }
    if (delayMs < 0.0) {
        delayMs = 0.0;
    }

    const uint64_t delayUs = static_cast<uint64_t>(delayMs * 1000.0 + 0.5);
    QueuedPacket pkt;
    pkt.deliverAtUs = nowUs + delayUs;
    pkt.seq = m_nextSeq++;
    if (data && len > 0u) {
        pkt.data.assign(data, data + len);
    }
    m_heap.push(pkt);
    return true;
}

bool ImpairmentQueue::popReady(uint64_t nowUs, std::vector<uint8_t>& outPacket) {
    if (m_heap.empty() || m_heap.top().deliverAtUs > nowUs) {
        return false;
    }
    outPacket = m_heap.top().data;
    m_heap.pop();
    ++m_deliveredCount;
    return true;
}

uint64_t ImpairmentQueue::nextDeliverUs() const {
    if (m_heap.empty()) {
        return UINT64_MAX;
    }
    return m_heap.top().deliverAtUs;
}

} // namespace bf2bot
