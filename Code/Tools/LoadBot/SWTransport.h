#ifndef BF2BOT_SW_TRANSPORT_H
#define BF2BOT_SW_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace bf2bot {

// BF2 v1.50 UDP protocol constants (matching NetClient.cpp / NetServer.cpp / SWConnection.cpp).
static const uint32_t kBf2ProtocolVersion = 4098u;        // 0x1002
static const uint32_t kBf2BuildVersion    = 0x150C5100u;  // BuildNrUtil::getNetVersionNumber()
static const size_t   kMaxDatagramSize    = 1472u;
static const size_t   kMaxDataPayloadSize = 1458u;
static const size_t   kDataHeaderSize     = 9u;

enum UdpPacketType {
    UDP_CONNECT_REQUEST    = 1,
    UDP_CONNECT_ACCEPT     = 2,
    UDP_CONNECT_DENIED     = 3,
    UDP_CONNECT_ACCEPT_ACK = 4,
    UDP_DISCONNECT         = 5,
    UDP_PING_REQUEST       = 7,
    UDP_PING_RESPONSE      = 8,
    UDP_SERVER_INFO_REQ    = 9,
    UDP_DATA_PACKET        = 15
};

struct ParsedDatagram {
    bool isFilterPacket;
    uint32_t type;
    uint8_t headerConnId;

    // ConnectAccept (type 2)
    uint8_t assignedConnId;
    uint32_t serverTimeMs;
    bool pbEnabled;

    // ConnectDenied (type 3) / Disconnect (type 5)
    uint32_t reason;
    bool wrongMod;
    std::string text;

    // Extended header (types 7, 8, 15)
    uint32_t seq;
    uint32_t ack;
    uint32_t ackMask;

    // PingRequest (type 7) / PingResponse (type 8)
    bool haveSent;
    uint32_t pingTimestampMs;
    uint32_t pingSenderTimestampMs;

    // DataPacket (type 15)
    const uint8_t* payload;
    uint16_t payloadLen;

    ParsedDatagram()
        : isFilterPacket(false),
          type(0),
          headerConnId(0),
          assignedConnId(0),
          serverTimeMs(0),
          pbEnabled(false),
          reason(0),
          wrongMod(false),
          text(),
          seq(0),
          ack(0),
          ackMask(0),
          haveSent(false),
          pingTimestampMs(0),
          pingSenderTimestampMs(0),
          payload(nullptr),
          payloadLen(0) {}
};

typedef std::function<void(uint32_t deliveryId, bool delivered)> PacketStatusCallback;

class SWConnectionState {
public:
    SWConnectionState();

    void reset();
    void setAssignedConnId(uint8_t connId);
    int assignedConnId() const { return m_assignedConnId; }
    bool hasAssignedConnId() const { return m_assignedConnId >= 0; }

    uint32_t localSeq() const { return m_localSeq; }
    int32_t lastSentSeq() const { return m_lastSentSeq; }
    uint32_t lastAckedSeq() const { return m_lastAckedSeq; }
    uint32_t remoteSeq() const { return m_remoteSeq; }
    uint32_t recvAckMask() const { return m_recvAckMask; }

    bool isSendWindowStalled() const;
    size_t inFlightCount() const { return m_inFlight.size(); }

    // Processes piggybacked remote acks (from DataPacket, PingRequest, or PingResponse).
    // Invokes cb(deliveryId, delivered) in exact FIFO send order.
    bool handleRemoteAck(uint32_t remoteAck, uint32_t remoteAckMask, const PacketStatusCallback& cb);

    // Validates incoming DataPacket 6-bit sequence number (1 <= ((seq - remoteSeq) & 0x3F) <= 31)
    // and updates remoteSeq and recvAckMask.
    bool acceptIncomingDataSeq(uint32_t seq);

    // Wraps a client->server payload into a BF2 DataPacket (type 15, 9-byte header + payload).
    // Returns false if connId unset, payloadLen invalid, outCap too small, or send window stalled.
    bool wrapDataPacket(const uint8_t* payload, uint16_t payloadLen, uint32_t deliveryId,
                        uint8_t* outBuf, size_t outCap, size_t& outLen);

    // RTT measurement from PingResponse (type 8).
    void recordPingResponse(uint32_t echoTimeStampMs, uint32_t nowMs);
    uint32_t lastRttMs() const { return m_lastRttMs; }
    float smoothedRttMs() const { return m_smoothedRttMs; }
    uint32_t packetsSent() const { return m_packetsSent; }
    uint32_t packetsReceived() const { return m_packetsReceived; }
    uint32_t packetsAcked() const { return m_packetsAcked; }
    uint32_t packetsLost() const { return m_packetsLost; }

private:
    struct InFlightEntry {
        uint32_t seq;
        uint32_t deliveryId;
    };

    int m_assignedConnId;
    uint32_t m_localSeq;      // Next outbound 6-bit sequence number (starts at 1)
    int32_t m_lastSentSeq;    // Last actually sent data packet seq (-1 if none)
    uint32_t m_lastAckedSeq;  // Remote ack horizon (starts at 0)
    uint32_t m_remoteSeq;     // Highest accepted remote seq (starts at 0)
    uint32_t m_recvAckMask;   // Bit 0 = remoteSeq, bit k = (remoteSeq - k) & 0x3F
    std::deque<InFlightEntry> m_inFlight;

    uint32_t m_lastRttMs;
    float m_smoothedRttMs;
    bool m_hasRttSample;
    uint32_t m_packetsSent;
    uint32_t m_packetsReceived;
    uint32_t m_packetsAcked;
    uint32_t m_packetsLost;
};

// Standalone UDP packet builders (return byte count written, or 0 on error).
size_t buildConnectRequest(uint8_t* out, size_t cap,
                           const std::string& password,
                           const std::string& modName = "mods/bf2",
                           const std::string& reconnectPassword = "",
                           bool punkBuster = false);

size_t buildConnectAcceptAck(uint8_t* out, size_t cap, uint8_t connId);

size_t buildDisconnect(uint8_t* out, size_t cap, uint8_t connId, uint32_t reason);

size_t buildPingRequest(uint8_t* out, size_t cap,
                        const SWConnectionState& conn,
                        uint32_t timeStampMs);

size_t buildPingResponse(uint8_t* out, size_t cap,
                         const SWConnectionState& conn,
                         uint32_t echoTimeStampMs,
                         uint32_t localTimeStampMs);

// Parses an incoming UDP datagram. Returns false on malformed/truncated packets.
// PunkBuster filter packets ("FFFF PB_") return true with out.isFilterPacket == true.
bool parseIncomingDatagram(const uint8_t* buf, size_t len, ParsedDatagram& out);

} // namespace bf2bot

#endif // BF2BOT_SW_TRANSPORT_H
