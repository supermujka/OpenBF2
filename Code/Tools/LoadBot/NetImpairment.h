#ifndef BF2BOT_NET_IMPAIRMENT_H
#define BF2BOT_NET_IMPAIRMENT_H

#include <cstddef>
#include <cstdint>
#include <queue>
#include <random>
#include <vector>

namespace bf2bot {

struct ImpairmentConfig {
    double oneWayDelayMs;
    double jitterMs;
    double lossPct;
    uint32_t seed;
};

// Deterministic per-direction UDP impairment queue (delay, uniform jitter, Bernoulli loss).
// Preserves FIFO packet ordering when jitterMs == 0 or timestamps tie.
class ImpairmentQueue {
public:
    ImpairmentQueue();
    explicit ImpairmentQueue(const ImpairmentConfig& cfg);

    void reset(const ImpairmentConfig& cfg);
    void clear();

    const ImpairmentConfig& config() const { return m_config; }
    bool isActive() const;

    // Enqueues packet with simulated loss/delay/jitter; returns false if dropped by loss.
    bool enqueue(uint64_t nowUs, const uint8_t* data, size_t len);

    // Pops earliest packet with deliverAtUs <= nowUs into outPacket.
    bool popReady(uint64_t nowUs, std::vector<uint8_t>& outPacket);

    // Returns earliest scheduled delivery timestamp in microseconds, or UINT64_MAX if empty.
    uint64_t nextDeliverUs() const;

    size_t pendingCount() const { return m_heap.size(); }
    uint64_t enqueuedCount() const { return m_enqueuedCount; }
    uint64_t droppedCount() const { return m_droppedCount; }
    uint64_t deliveredCount() const { return m_deliveredCount; }

private:
    struct QueuedPacket {
        uint64_t deliverAtUs;
        uint64_t seq;
        std::vector<uint8_t> data;
    };

    struct PacketLater {
        bool operator()(const QueuedPacket& a, const QueuedPacket& b) const {
            if (a.deliverAtUs != b.deliverAtUs) {
                return a.deliverAtUs > b.deliverAtUs;
            }
            return a.seq > b.seq;
        }
    };

    ImpairmentConfig m_config;
    std::mt19937 m_rng;
    std::priority_queue<QueuedPacket, std::vector<QueuedPacket>, PacketLater> m_heap;
    uint64_t m_nextSeq;
    uint64_t m_enqueuedCount;
    uint64_t m_droppedCount;
    uint64_t m_deliveredCount;
};

} // namespace bf2bot

#endif // BF2BOT_NET_IMPAIRMENT_H
