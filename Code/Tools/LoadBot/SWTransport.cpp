#include "SWTransport.h"
#include "WireBitStream.h"

#include <algorithm>
#include <cstring>

namespace bf2bot {

namespace {

bool writeBasicHeader(BitStream& bs, uint8_t connId, uint32_t type) {
    return bs.writeUInt(type, 4) && bs.writeUInt(connId, 8);
}

bool readBasicHeader(BitStream& bs, uint8_t& connId, uint32_t& type) {
    type = bs.readUInt(4);
    connId = static_cast<uint8_t>(bs.readUInt(8));
    return !bs.hasError();
}

bool writeExtendedHeader(BitStream& bs, uint32_t localCounter,
                         uint32_t remoteCounter, uint32_t ackMask) {
    return bs.writeUInt(localCounter & 0x3Fu, 6) &&
           bs.writeUInt(remoteCounter & 0x3Fu, 6) &&
           bs.writeUInt(ackMask, 32);
}

bool readExtendedHeader(BitStream& bs, uint32_t& localCounter,
                        uint32_t& remoteCounter, uint32_t& ackMask) {
    localCounter = bs.readUInt(6);
    remoteCounter = bs.readUInt(6);
    ackMask = bs.readUInt(32);
    return !bs.hasError();
}

} // namespace

SWConnectionState::SWConnectionState()
    : m_assignedConnId(-1),
      m_localSeq(1),
      m_lastSentSeq(-1),
      m_lastAckedSeq(0),
      m_remoteSeq(0),
      m_recvAckMask(0),
      m_inFlight(),
      m_lastRttMs(0),
      m_smoothedRttMs(0.0f),
      m_hasRttSample(false),
      m_packetsSent(0),
      m_packetsReceived(0),
      m_packetsAcked(0),
      m_packetsLost(0) {}

void SWConnectionState::reset() {
    m_assignedConnId = -1;
    m_localSeq = 1;
    m_lastSentSeq = -1;
    m_lastAckedSeq = 0;
    m_remoteSeq = 0;
    m_recvAckMask = 0;
    m_inFlight.clear();
    m_lastRttMs = 0;
    m_smoothedRttMs = 0.0f;
    m_hasRttSample = false;
    m_packetsSent = 0;
    m_packetsReceived = 0;
    m_packetsAcked = 0;
    m_packetsLost = 0;
}

void SWConnectionState::setAssignedConnId(uint8_t connId) {
    m_assignedConnId = static_cast<int>(connId);
}

bool SWConnectionState::isSendWindowStalled() const {
    return ((m_localSeq - m_lastAckedSeq) & 0x3Fu) > 30u;
}

bool SWConnectionState::handleRemoteAck(uint32_t remoteAck, uint32_t remoteAckMask,
                                        const PacketStatusCallback& cb) {
    const uint32_t advance = (remoteAck - m_lastAckedSeq) & 0x3Fu;
    if (advance > 31u) {
        return true; // Stale / out-of-window ack: fail open like SWConnection::confirmPacket
    }

    m_lastAckedSeq = remoteAck & 0x3Fu;
    if (advance == 0) {
        return true;
    }

    uint32_t bit = advance;
    do {
        --bit;
        const bool delivered = (((1u << bit) & remoteAckMask) != 0u);
        if (delivered) {
            ++m_packetsAcked;
        } else {
            ++m_packetsLost;
        }
        if (!m_inFlight.empty()) {
            const uint32_t deliveryId = m_inFlight.front().deliveryId;
            m_inFlight.pop_front();
            if (cb) {
                cb(deliveryId, delivered);
            }
        }
    } while (bit != 0);

    return (m_packetsAcked + m_packetsLost) <= m_packetsSent;
}

bool SWConnectionState::acceptIncomingDataSeq(uint32_t seq) {
    const uint32_t maskedSeq = seq & 0x3Fu;
    const uint32_t advance = (maskedSeq - m_remoteSeq) & 0x3Fu;
    if (advance == 0 || advance > 31u) {
        return false;
    }

    if (advance > 1u) {
        m_recvAckMask <<= (advance - 1u);
    }
    m_recvAckMask = (m_recvAckMask << 1) | 1u;
    m_remoteSeq = maskedSeq;
    ++m_packetsReceived;
    return true;
}

bool SWConnectionState::wrapDataPacket(const uint8_t* payload, uint16_t payloadLen,
                                       uint32_t deliveryId, uint8_t* outBuf,
                                       size_t outCap, size_t& outLen) {
    outLen = 0;
    if (m_assignedConnId < 0 || !payload || payloadLen == 0 ||
        payloadLen > kMaxDataPayloadSize || !outBuf) {
        return false;
    }
    if (outCap < kDataHeaderSize + static_cast<size_t>(payloadLen)) {
        return false;
    }
    if (isSendWindowStalled()) {
        return false;
    }

    std::memset(outBuf, 0, kDataHeaderSize);
    BitStream bs(outBuf, static_cast<uint32_t>(kDataHeaderSize));
    const uint32_t seq = m_localSeq & 0x3Fu;
    if (!writeBasicHeader(bs, static_cast<uint8_t>(m_assignedConnId), UDP_DATA_PACKET) ||
        !writeExtendedHeader(bs, seq, m_remoteSeq, m_recvAckMask) ||
        !bs.writeUInt(payloadLen, 16) ||
        bs.hasError()) {
        return false;
    }

    std::memcpy(outBuf + kDataHeaderSize, payload, payloadLen);
    outLen = kDataHeaderSize + static_cast<size_t>(payloadLen);

    InFlightEntry entry;
    entry.seq = seq;
    entry.deliveryId = deliveryId;
    m_inFlight.push_back(entry);

    m_lastSentSeq = static_cast<int32_t>(seq);
    m_localSeq = (m_localSeq + 1u) & 0x3Fu;
    ++m_packetsSent;
    return true;
}

void SWConnectionState::recordPingResponse(uint32_t echoTimeStampMs, uint32_t nowMs) {
    const uint32_t rtt = nowMs - echoTimeStampMs;
    m_lastRttMs = rtt;
    if (!m_hasRttSample) {
        m_smoothedRttMs = static_cast<float>(rtt);
        m_hasRttSample = true;
    } else {
        m_smoothedRttMs = m_smoothedRttMs * 0.8f + static_cast<float>(rtt) * 0.2f;
    }
}

size_t buildConnectRequest(uint8_t* out, size_t cap,
                           const std::string& password,
                           const std::string& modName,
                           const std::string& reconnectPassword,
                           bool punkBuster) {
    if (!out || cap < 78u) {
        return 0;
    }
    std::memset(out, 0, 78u);

    char reconnectBuf[4];
    std::memset(reconnectBuf, 0, sizeof(reconnectBuf));
    if (!reconnectPassword.empty()) {
        std::memcpy(reconnectBuf, reconnectPassword.data(),
                    std::min(reconnectPassword.size(), sizeof(reconnectBuf)));
    }

    char passwordBuf[32];
    std::memset(passwordBuf, 0, sizeof(passwordBuf));
    if (!password.empty()) {
        std::memcpy(passwordBuf, password.data(),
                    std::min(password.size(), sizeof(passwordBuf)));
    }

    char modBuf[32];
    std::memset(modBuf, 0, sizeof(modBuf));
    if (!modName.empty()) {
        std::memcpy(modBuf, modName.data(),
                    std::min(modName.size(), sizeof(modBuf)));
    }

    BitStream bs(out, 78u);
    writeBasicHeader(bs, 1u, UDP_CONNECT_REQUEST);
    bs.writeUInt(kBf2ProtocolVersion, 32);
    bs.writeUInt(kBf2BuildVersion, 32);
    bs.writeBool(punkBuster);
    bs.writeFixedBytes(reconnectBuf, 4);
    bs.writeFixedBytes(passwordBuf, 32);
    bs.writeFixedBytes(modBuf, 32);
    if (bs.hasError()) {
        return 0;
    }
    return bs.getByteSize(); // Exact 78 bytes (621 bits)
}

size_t buildConnectAcceptAck(uint8_t* out, size_t cap, uint8_t connId) {
    if (!out || cap < 2u) {
        return 0;
    }
    std::memset(out, 0, 2u);
    BitStream bs(out, 2u);
    if (!writeBasicHeader(bs, connId, UDP_CONNECT_ACCEPT_ACK) || bs.hasError()) {
        return 0;
    }
    return bs.getByteSize(); // Exact 2 bytes (12 bits)
}

size_t buildDisconnect(uint8_t* out, size_t cap, uint8_t connId, uint32_t reason) {
    if (!out || cap < 6u) {
        return 0;
    }
    std::memset(out, 0, 6u);
    BitStream bs(out, 6u);
    if (!writeBasicHeader(bs, connId, UDP_DISCONNECT) ||
        !bs.writeUInt(reason, 32) ||
        bs.hasError()) {
        return 0;
    }
    return bs.getByteSize(); // Exact 6 bytes (44 bits)
}

size_t buildPingRequest(uint8_t* out, size_t cap,
                        const SWConnectionState& conn,
                        uint32_t timeStampMs) {
    if (!out || cap < 12u || !conn.hasAssignedConnId()) {
        return 0;
    }
    std::memset(out, 0, 12u);
    BitStream bs(out, 12u);
    const uint32_t localCounter = static_cast<uint32_t>(conn.lastSentSeq()) & 0x3Fu;
    const bool haveSent = (conn.lastSentSeq() != -1);
    if (!writeBasicHeader(bs, static_cast<uint8_t>(conn.assignedConnId()), UDP_PING_REQUEST) ||
        !writeExtendedHeader(bs, localCounter, conn.remoteSeq(), conn.recvAckMask()) ||
        !bs.writeBool(haveSent) ||
        !bs.writeUInt(timeStampMs, 32) ||
        bs.hasError()) {
        return 0;
    }
    return bs.getByteSize(); // Exact 12 bytes (89 bits)
}

size_t buildPingResponse(uint8_t* out, size_t cap,
                         const SWConnectionState& conn,
                         uint32_t echoTimeStampMs,
                         uint32_t localTimeStampMs) {
    if (!out || cap < 16u || !conn.hasAssignedConnId()) {
        return 0;
    }
    std::memset(out, 0, 16u);
    BitStream bs(out, 16u);
    const uint32_t localCounter = static_cast<uint32_t>(conn.lastSentSeq()) & 0x3Fu;
    const bool haveSent = (conn.lastSentSeq() != -1);
    if (!writeBasicHeader(bs, static_cast<uint8_t>(conn.assignedConnId()), UDP_PING_RESPONSE) ||
        !writeExtendedHeader(bs, localCounter, conn.remoteSeq(), conn.recvAckMask()) ||
        !bs.writeBool(haveSent) ||
        !bs.writeUInt(echoTimeStampMs, 32) ||
        !bs.writeUInt(localTimeStampMs, 32) ||
        bs.hasError()) {
        return 0;
    }
    return bs.getByteSize(); // Exact 16 bytes (121 bits)
}

bool parseIncomingDatagram(const uint8_t* buf, size_t len, ParsedDatagram& out) {
    out = ParsedDatagram();
    if (!buf || len < 2u) {
        return false;
    }

    // PunkBuster pre-filter: FF FF FF FF 'P' 'B' '_'
    if (len >= 7u &&
        buf[0] == 0xFFu && buf[1] == 0xFFu && buf[2] == 0xFFu && buf[3] == 0xFFu &&
        buf[4] == 'P' && buf[5] == 'B' && buf[6] == '_') {
        out.isFilterPacket = true;
        return true;
    }

    BitStream bs(buf, static_cast<uint32_t>(len));
    if (!readBasicHeader(bs, out.headerConnId, out.type)) {
        return false;
    }

    switch (out.type) {
    case UDP_CONNECT_ACCEPT: {
        out.assignedConnId = static_cast<uint8_t>(bs.readUInt(8));
        out.serverTimeMs = bs.readUInt(32);
        out.pbEnabled = bs.readBool();
        return !bs.hasError();
    }
    case UDP_CONNECT_DENIED: {
        out.reason = bs.readUInt(32);
        out.wrongMod = bs.readBool();
        if (out.wrongMod) {
            char modBuf[33];
            std::memset(modBuf, 0, sizeof(modBuf));
            bs.readFixedBytes(modBuf, 32);
            out.text = modBuf;
        }
        return !bs.hasError();
    }
    case UDP_DISCONNECT: {
        out.reason = bs.readUInt(32);
        if (bs.hasError()) {
            return false;
        }
        // S->C Disconnect appends 16-bit text length + text (max 200); C->S omits it.
        if (bs.getBitOffset() + 16u <= bs.getBitSize()) {
            out.text = bs.readStringU16(256u);
            if (bs.hasError()) {
                bs.clearError();
                out.text.clear();
            }
        }
        return true;
    }
    case UDP_PING_REQUEST: {
        if (!readExtendedHeader(bs, out.seq, out.ack, out.ackMask)) {
            return false;
        }
        out.haveSent = bs.readBool();
        out.pingTimestampMs = bs.readUInt(32);
        return !bs.hasError();
    }
    case UDP_PING_RESPONSE: {
        if (!readExtendedHeader(bs, out.seq, out.ack, out.ackMask)) {
            return false;
        }
        out.haveSent = bs.readBool();
        out.pingTimestampMs = bs.readUInt(32);
        out.pingSenderTimestampMs = bs.readUInt(32);
        return !bs.hasError();
    }
    case UDP_DATA_PACKET: {
        if (len < kDataHeaderSize) {
            return false;
        }
        if (!readExtendedHeader(bs, out.seq, out.ack, out.ackMask)) {
            return false;
        }
        out.payloadLen = static_cast<uint16_t>(bs.readUInt(16));
        if (bs.hasError()) {
            return false;
        }
        if (kDataHeaderSize + static_cast<size_t>(out.payloadLen) > len) {
            return false;
        }
        out.payload = buf + kDataHeaderSize;
        return true;
    }
    default:
        return true;
    }
}

} // namespace bf2bot
