#include "Bf2Protocol.h"

#include <cmath>
#include <cstring>

namespace bf2bot {

namespace {

int16_t quantizeAxis(float v) {
    float scaled = v * 100.0f;
    if (scaled > 32767.0f) {
        scaled = 32767.0f;
    } else if (scaled < -32767.0f) {
        scaled = -32767.0f;
    }
    return static_cast<int16_t>(static_cast<int32_t>(scaled));
}

float decodeAngle12(BitStream& bs, float range) {
    const uint32_t q = bs.readUInt(12);
    float centred = static_cast<float>(q) / 4095.0f;
    centred = centred + centred - 1.0f;
    if (std::fabs(centred) <= 0.00048840052f) {
        centred = 0.0f;
    }
    return centred * range;
}

float decodeCpUnit16(BitStream& bs) {
    const uint32_t q = bs.readUInt(16);
    const float t = static_cast<float>(static_cast<double>(q) / 65535.0);
    double f = static_cast<double>(t) + static_cast<double>(t) - 1.0;
    if (!(std::fabs(f) > 3.0518044e-05)) {
        f = 0.0;
    }
    return static_cast<float>(f * 1.25);
}

std::string trimFixedCString(const char* buf, size_t maxLen) {
    size_t len = 0;
    while (len < maxLen && buf[len] != '\0') {
        ++len;
    }
    return std::string(buf, len);
}

// Adapter sink that intercepts EnterVehicle/DestroyObject to keep GhostReceiver's
// soldier netId table synchronized while forwarding all callbacks to userSink.
class CodecEventSinkAdapter : public IGameEventSink {
public:
    CodecEventSinkAdapter(GhostReceiver& ghosts, IGameEventSink* userSink)
        : m_ghosts(ghosts), m_userSink(userSink) {}

    void onChallenge(const ChallengeEventData& d) override {
        if (m_userSink) m_userSink->onChallenge(d);
    }
    void onServerInfo(const ServerInfoData& d) override {
        if (m_userSink) m_userSink->onServerInfo(d);
    }
    void onMapInfo(const MapInfoData& d) override {
        if (m_userSink) m_userSink->onMapInfo(d);
    }
    void onCreatePlayer(const CreatePlayerEventData& d) override {
        if (m_userSink) m_userSink->onCreatePlayer(d);
    }
    void onDestroyPlayer(uint8_t playerId) override {
        if (m_userSink) m_userSink->onDestroyPlayer(playerId);
    }
    void onCreateObject(const CreateObjectEventData& d) override {
        if (m_userSink) m_userSink->onCreateObject(d);
    }
    void onDestroyObject(uint16_t netId) override {
        m_ghosts.unregisterNetId(netId);
        if (m_userSink) m_userSink->onDestroyObject(netId);
    }
    void onEnterVehicle(const EnterVehicleEventData& d) override {
        if (d.setAsDefaultVehicle && d.netId != 0) {
            m_ghosts.registerSoldierNetId(d.netId);
        }
        if (m_userSink) m_userSink->onEnterVehicle(d);
    }
    void onExitVehicle(const ExitVehicleEventData& d) override {
        if (m_userSink) m_userSink->onExitVehicle(d);
    }
    void onCreateSpawnGroup(const CreateSpawnGroupEventData& d) override {
        if (m_userSink) m_userSink->onCreateSpawnGroup(d);
    }
    void onRemoveSpawnGroup(const RemoveSpawnGroupEventData& d) override {
        if (m_userSink) m_userSink->onRemoveSpawnGroup(d);
    }
    void onPostRemoteEvent(const PostRemoteEventData& d) override {
        if (m_userSink) m_userSink->onPostRemoteEvent(d);
    }
    void onKilledBy(const KilledByEventData& d) override {
        if (m_userSink) m_userSink->onKilledBy(d);
    }
    void onBeginRound(const BeginRoundEventData& d) override {
        if (m_userSink) m_userSink->onBeginRound(d);
    }
    void onEndOfRound(const EndOfRoundEventData& d) override {
        if (m_userSink) m_userSink->onEndOfRound(d);
    }

private:
    GhostReceiver& m_ghosts;
    IGameEventSink* m_userSink;
};

} // namespace

// ============================================================================
// 1. PlayerActionSender
// ============================================================================

PlayerActionSender::PlayerActionSender()
    : m_enabled(false),
      m_interpTimeMs(100u),
      m_nextInputIndex(1),
      m_window() {}

void PlayerActionSender::reset() {
    m_enabled = false;
    m_interpTimeMs = 100u;
    m_nextInputIndex = 1;
    m_window.clear();
}

int32_t PlayerActionSender::pushAction(const PlayerActionInput& action) {
    WireAction wa;
    for (int i = 0; i < 6; ++i) {
        wa.axes[i] = quantizeAxis(action.axes[i]);
    }
    wa.buttonMaskLow = action.buttonMaskLow;
    wa.buttonMaskHigh = action.buttonMaskHigh & 0x1FFu;
    wa.flag = action.flag;
    if (action.inputIndex > 0) {
        wa.inputIndex = action.inputIndex;
        if (action.inputIndex >= m_nextInputIndex) {
            m_nextInputIndex = action.inputIndex + 1;
        }
    } else {
        wa.inputIndex = m_nextInputIndex++;
    }

    if (m_window.size() >= 3u) {
        m_window.pop_front();
    }
    m_window.push_back(wa);
    m_enabled = true;
    return wa.inputIndex;
}

bool PlayerActionSender::transmit(BitStream& bs) const {
    if (!m_enabled || m_window.empty()) {
        bs.writeBool(false);
        return false;
    }

    const uint32_t count = static_cast<uint32_t>(m_window.size());
    bs.writeBool(true);
    bs.writeUInt(count, 4);
    bs.writeUInt(m_interpTimeMs & 0x1FFu, 9);
    bs.writeSignedMag(m_window.front().inputIndex, 32);

    for (size_t i = 0; i < m_window.size(); ++i) {
        const WireAction& wa = m_window[i];
        for (int a = 0; a < 6; ++a) {
            bs.writeSignedMag(static_cast<int32_t>(wa.axes[a]), 16);
        }
        bs.writeUInt(wa.buttonMaskLow, 32);
        bs.writeUInt(wa.buttonMaskHigh & 0x1FFu, 9);
        bs.writeBool(wa.flag);
    }
    return !bs.hasError();
}

bool PlayerActionSender::skipIncoming(BitStream& bs) {
    const bool present = bs.readBool();
    if (bs.hasError()) {
        return false;
    }
    if (!present) {
        return true;
    }
    const uint32_t count = bs.readUInt(4);
    bs.readUInt(9); // interpTimeMs
    if (bs.hasError()) {
        return false;
    }
    if (count > 0) {
        // Each PlayerAction on the BF2 v1.50 wire is 6*16 (axes) + 32 (maskLow) + 9 (maskHigh) + 1 (flag) = 138 bits.
        bs.skipBits(32u + count * 138u);
    }
    return !bs.hasError();
}

// ============================================================================
// 2. DataBlockReceiver
// ============================================================================

DataBlockReceiver::DataBlockReceiver()
    : m_active(false), m_blockEventId(0), m_expectedLength(0), m_buffer() {}

void DataBlockReceiver::reset() {
    m_active = false;
    m_blockEventId = 0;
    m_expectedLength = 0;
    m_buffer.clear();
}

void DataBlockReceiver::onNewBlock(uint32_t blockEventId, uint32_t blockLength) {
    m_active = true;
    m_blockEventId = blockEventId;
    m_expectedLength = blockLength;
    m_buffer.clear();
    if (blockLength <= 1024u * 1024u) {
        m_buffer.reserve(blockLength);
    }
}

void DataBlockReceiver::onChunk(const uint8_t* data, uint8_t size, IGameEventSink* sink) {
    if (!m_active) {
        return;
    }
    if (size > 0 && data) {
        m_buffer.insert(m_buffer.end(), data, data + size);
    }
    if (m_buffer.size() >= m_expectedLength) {
        const uint32_t blockId = m_blockEventId;
        const size_t len = m_expectedLength;
        const uint8_t* ptr = m_buffer.empty() ? nullptr : &m_buffer[0];
        if (sink && ptr && len > 0) {
            if (blockId == DB_SERVER_INFO) {
                ServerInfoData info;
                if (decodeServerInfo(ptr, len, info)) {
                    sink->onServerInfo(info);
                }
            } else if (blockId == DB_MAP_INFO) {
                MapInfoData mapInfo;
                if (decodeMapInfo(ptr, len, mapInfo)) {
                    sink->onMapInfo(mapInfo);
                }
            }
        }
        reset();
    }
}

bool DataBlockReceiver::decodeServerInfo(const uint8_t* buf, size_t len, ServerInfoData& out) {
    out = ServerInfoData();
    if (!buf || len == 0) {
        return false;
    }
    BitStream bs(buf, static_cast<uint32_t>(len));
    out.gameName = bs.readStringU16(1024u);
    const uint32_t welcomeCount = bs.readUInt(16);
    if (bs.hasError() || welcomeCount > 64u) {
        return false;
    }
    out.welcomeLines.reserve(welcomeCount);
    for (uint32_t i = 0; i < welcomeCount; ++i) {
        out.welcomeLines.push_back(bs.readStringU16(1024u));
        if (bs.hasError()) {
            return false;
        }
    }
    out.requestResponse = bs.readBool();
    // 6 x 1b flags: voipEnabled, allowNoseCam, hitIndicator, tkPunishDefault, allowFreeCam, allowExternalViews
    bs.skipBits(6u);
    // voipServerAddress(32), voipServerPort(16), voipBFClientPort(16), voipQuality(s32_sm), gameSpyPort(16)
    bs.skipBits(32u + 16u + 16u + 32u + 16u);
    // 3 x s32_sm: radioSpamInterval, radioMaxSpamFlagCount, radioBlockedDuration
    bs.skipBits(32u * 3u);
    out.useContentChecking = bs.readBool();
    return !bs.hasError();
}

bool DataBlockReceiver::decodeMapInfo(const uint8_t* buf, size_t len, MapInfoData& out) {
    out = MapInfoData();
    if (!buf || len == 0) {
        return false;
    }
    BitStream bs(buf, static_cast<uint32_t>(len));
    out.gameMode = bs.readStringU16(256u);
    out.levelPath = bs.readStringU16(512u);
    out.mapName = bs.readStringU16(256u);
    out.maxPlayers = bs.readSignedMag(32);
    out.commanderEnabled = bs.readBool();
    out.challengeOrdinal = bs.readSignedMag(32);
    return !bs.hasError();
}

// ============================================================================
// 3. GameEventStream
// ============================================================================

GameEventStream::GameEventStream()
    : m_sendQueue(),
      m_resendBlocks(),
      m_inFlightBlocks(),
      m_nextSendBlockIndex(0),
      m_confirmedTotal(0),
      m_nextRecvBlockIndex(0),
      m_pendingRecvBlocks(),
      m_dataBlocks() {
    std::memset(m_confirmedSlots, 0, sizeof(m_confirmedSlots));
}

void GameEventStream::reset() {
    m_sendQueue.clear();
    m_resendBlocks.clear();
    m_inFlightBlocks.clear();
    m_nextSendBlockIndex = 0;
    m_confirmedTotal = 0;
    std::memset(m_confirmedSlots, 0, sizeof(m_confirmedSlots));
    m_nextRecvBlockIndex = 0;
    m_pendingRecvBlocks.clear();
    m_dataBlocks.reset();
}

void GameEventStream::pushSerializedEvent(const BitStream& evStream) {
    if (evStream.hasError() || evStream.getBitSize() == 0) {
        return;
    }
    SerializedEvent ev;
    ev.bitLen = evStream.getBitSize();
    const uint32_t byteLen = evStream.getByteSize();
    ev.bytes.assign(evStream.data(), evStream.data() + byteLen);
    m_sendQueue.push_back(ev);
}

void GameEventStream::queueChallengeResponse(const uint8_t response73[73],
                                             uint32_t field58,
                                             uint32_t netVersion,
                                             int32_t gameId) {
    BitStream bs(128u);
    bs.writeUInt(GE_CHALLENGE_RESPONSE, kGameEventTypeBits);
    bs.writeFixedBytes(response73, 73u);
    bs.writeUInt(field58, 32);
    bs.writeUInt(netVersion, 32);
    bs.writeSignedMag(gameId, 32);
    pushSerializedEvent(bs);
}

void GameEventStream::queueDataBlock(uint32_t blockEventId, const uint8_t* payload,
                                     uint32_t payloadLen) {
    // Header event: isNewBlock = 1, blockEventId (32b), blockLength (32b)
    {
        BitStream bs(32u);
        bs.writeUInt(GE_DATA_BLOCK, kGameEventTypeBits);
        bs.writeBool(true);
        bs.writeUInt(blockEventId, 32);
        bs.writeUInt(payloadLen, 32);
        pushSerializedEvent(bs);
    }

    // Chunk events: <= 63 bytes each
    uint32_t offset = 0;
    while (offset < payloadLen) {
        uint32_t chunk = payloadLen - offset;
        if (chunk > kDataBlockMaxChunkSize) {
            chunk = kDataBlockMaxChunkSize;
        }
        BitStream bs(96u);
        bs.writeUInt(GE_DATA_BLOCK, kGameEventTypeBits);
        bs.writeBool(false);
        bs.writeUInt(chunk, 8);
        bs.writeFixedBytes(payload + offset, chunk);
        pushSerializedEvent(bs);
        offset += chunk;
    }
}

void GameEventStream::queueClientInfo(const std::string& playerName,
                                      uint32_t playerNameHash,
                                      int32_t profileId,
                                      const std::string& clanTag,
                                      const std::string& gameSpyAuth,
                                      bool usesVoip) {
    BitStream payload(512u);
    payload.writeStringU16(playerName);
    payload.writeUInt(playerNameHash, 32);
    payload.writeSignedMag(profileId, 32);
    payload.writeStringU16(clanTag);
    payload.writeStringU16(gameSpyAuth);
    payload.writeBool(usesVoip);
    if (!payload.hasError()) {
        queueDataBlock(DB_CLIENT_INFO, payload.data(), payload.getByteSize());
    }
}

void GameEventStream::queuePostRemoteEvent(uint8_t category, uint32_t eventId,
                                           const void* payload, uint8_t payloadLen,
                                           float delay) {
    BitStream bs(320u);
    bs.writeUInt(GE_POST_REMOTE, kGameEventTypeBits);
    bs.writeUInt(category & 0x0Fu, 4);
    bs.writeUInt(eventId, 32);
    bs.writeFloat(delay);
    bs.writeUInt(payloadLen, 8);
    if (payloadLen > 0 && payload) {
        bs.writeFixedBytes(payload, payloadLen);
    }
    pushSerializedEvent(bs);
}

void GameEventStream::queueMapLoaded() {
    queuePostRemoteEvent(EC_NETWORK, NRE_MAP_LOADED, nullptr, 0, 0.0f);
}

void GameEventStream::queueClientReady() {
    queuePostRemoteEvent(EC_NETWORK, NRE_CLIENT_READY, nullptr, 0, 0.0f);
}

void GameEventStream::queueConfirmReset() {
    queuePostRemoteEvent(EC_NETWORK, NRE_CONFIRM_RESET, nullptr, 0, 0.0f);
}

void GameEventStream::queueJoinTeam(int32_t team) {
    queuePostRemoteEvent(EC_NETWORK, NRE_JOIN_TEAM, &team, 4u, 0.0f);
}

void GameEventStream::queueSelectKit(int32_t kit, int32_t unlockLevel) {
    const int32_t packed = ((unlockLevel & 0x0F) << 4) | (kit & 0x0F);
    queuePostRemoteEvent(EC_NETWORK, NRE_SELECT_KIT, &packed, 4u, 0.0f);
}

void GameEventStream::queueSelectSpawnPoint(int32_t spawnGroupId) {
    queuePostRemoteEvent(EC_NETWORK, NRE_SELECT_SPAWN_POINT, &spawnGroupId, 4u, 0.0f);
}

void GameEventStream::queueSuicide() {
    queuePostRemoteEvent(EC_NETWORK, NRE_SUICIDE, nullptr, 0, 0.0f);
}

void GameEventStream::queueEndOfRoundAck() {
    queuePostRemoteEvent(EC_NETWORK, NRE_END_OF_ROUND_ACK, nullptr, 0, 0.0f);
}

void GameEventStream::queueContentCheck(const uint8_t miscHash[16],
                                        const uint8_t archiveHash[16],
                                        const uint8_t levelHash[16]) {
    BitStream bs(64u);
    bs.writeUInt(GE_CONTENT_CHECK, kGameEventTypeBits);
    bs.writeFixedBytes(miscHash, 16u);
    bs.writeFixedBytes(archiveHash, 16u);
    bs.writeFixedBytes(levelHash, 16u);
    pushSerializedEvent(bs);
}

void GameEventStream::queueConnectionType(uint32_t connectionType) {
    BitStream bs(16u);
    bs.writeUInt(GE_CONNECTION_TYPE, kGameEventTypeBits);
    bs.writeRanged(connectionType > 6u ? 5u : connectionType, 6u);
    pushSerializedEvent(bs);
}

bool GameEventStream::transmit(BitStream& bs, uint32_t deliveryId, uint32_t maxBitBudget) {
    const uint32_t startBits = bs.getBitOffset();
    const uint32_t capacityBits = bs.getCapacityBits();
    uint32_t limitBits = capacityBits;
    if (maxBitBudget > 0 && startBits + maxBitBudget < limitBits) {
        limitBits = startBits + maxBitBudget;
    }
    // Leave at least 1 bit for GhostManager's trailing present=0 bit.
    if (limitBits > 1u) {
        limitBits -= 1u;
    }

    if (startBits + 15u > limitBits) {
        bs.writeBool(false);
        return false;
    }

    bool isResend = false;
    uint32_t blockIndex = 0;
    std::vector<SerializedEvent> batch;

    if (!m_resendBlocks.empty()) {
        // Resend oldest lost block as a whole if it fits.
        const EventBlock& resend = m_resendBlocks.front();
        uint32_t totalBits = 15u;
        for (size_t i = 0; i < resend.events.size(); ++i) {
            totalBits += resend.events[i].bitLen;
        }
        if (startBits + totalBits <= limitBits) {
            isResend = true;
            blockIndex = resend.blockIndex;
            batch = resend.events;
            m_resendBlocks.pop_front();
        }
    } else if (!m_sendQueue.empty() && numUnconfirmedBlocks() < kMaxUnconfirmedEventBlocks) {
        uint32_t usedBits = 15u;
        while (!m_sendQueue.empty() && batch.size() < 255u) {
            const SerializedEvent& nextEv = m_sendQueue.front();
            if (startBits + usedBits + nextEv.bitLen > limitBits) {
                break;
            }
            usedBits += nextEv.bitLen;
            batch.push_back(nextEv);
            m_sendQueue.pop_front();
        }
        if (!batch.empty()) {
            blockIndex = m_nextSendBlockIndex & 31u;
            ++m_nextSendBlockIndex;
        }
    }

    if (batch.empty()) {
        bs.writeBool(false);
        return false;
    }

    bs.writeBool(true);
    bs.writeUInt(static_cast<uint32_t>(batch.size()), 8);
    bs.writeUInt(blockIndex & 31u, 5);
    bs.writeBool(isResend);
    for (size_t i = 0; i < batch.size(); ++i) {
        bs.writeBits(&batch[i].bytes[0], batch[i].bitLen);
    }

    if (bs.hasError()) {
        return false;
    }

    InFlightBlock inflight;
    inflight.deliveryId = deliveryId;
    inflight.blockIndex = blockIndex;
    inflight.events = batch;
    m_inFlightBlocks.push_back(inflight);
    return true;
}

void GameEventStream::onPacketStatus(uint32_t deliveryId, bool delivered) {
    for (std::deque<InFlightBlock>::iterator it = m_inFlightBlocks.begin();
         it != m_inFlightBlocks.end(); ++it) {
        if (it->deliveryId == deliveryId) {
            if (delivered) {
                m_confirmedSlots[it->blockIndex & 31u] = true;
                while (m_confirmedSlots[m_confirmedTotal & 31u]) {
                    m_confirmedSlots[m_confirmedTotal & 31u] = false;
                    ++m_confirmedTotal;
                }
            } else if (!it->events.empty()) {
                EventBlock resend;
                resend.blockIndex = it->blockIndex;
                resend.events = it->events;
                m_resendBlocks.push_back(resend);
            }
            m_inFlightBlocks.erase(it);
            return;
        }
    }
}

bool GameEventStream::parseSingleEvent(BitStream& bs, ParsedEvent& out) {
    out = ParsedEvent();
    const uint32_t typeId = bs.readUInt(kGameEventTypeBits);
    if (bs.hasError()) {
        return false;
    }
    out.typeId = typeId;

    switch (typeId) {
    case GE_CHALLENGE: {
        bs.readFixedBytes(out.challenge.challenge, 10u);
        const uint32_t len = bs.readUInt(8);
        if (bs.hasError()) return false;
        out.challenge.modName.resize(len);
        if (len > 0) {
            bs.readFixedBytes(&out.challenge.modName[0], len);
        }
        return !bs.hasError();
    }
    case GE_CHALLENGE_RESPONSE: {
        bs.skipBits(73u * 8u + 32u + 32u + 32u);
        return !bs.hasError();
    }
    case GE_CONNECTION_TYPE: {
        bs.skipBits(3u);
        return !bs.hasError();
    }
    case GE_DATA_BLOCK: {
        out.dbIsNewBlock = bs.readBool();
        if (out.dbIsNewBlock) {
            out.dbBlockId = bs.readUInt(32);
            out.dbBlockLen = bs.readUInt(32);
        } else {
            const uint32_t sz = bs.readUInt(8);
            if (bs.hasError()) return false;
            out.dbChunk.resize(sz);
            if (sz > 0) {
                bs.readFixedBytes(&out.dbChunk[0], sz);
            }
        }
        return !bs.hasError();
    }
    case GE_CREATE_PLAYER: {
        out.createPlayer.teamId = static_cast<uint8_t>(bs.readUInt(3));
        out.createPlayer.spawnGroupId = static_cast<uint8_t>(bs.readUInt(4));
        out.createPlayer.isRemote = bs.readBool();
        out.createPlayer.playerId = static_cast<uint8_t>(bs.readUInt(8));
        out.createPlayer.playerNetId = static_cast<uint16_t>(bs.readUInt(16));
        out.createPlayer.word34 = static_cast<uint16_t>(bs.readUInt(16));
        out.createPlayer.isAI = bs.readBool();
        char nameBuf[32];
        bs.readFixedBytes(nameBuf, 32u);
        if (!bs.hasError()) {
            out.createPlayer.name = trimFixedCString(nameBuf, 32u);
        }
        return !bs.hasError();
    }
    case GE_CREATE_OBJECT: {
        out.createObject.templateId = bs.readUInt(32);
        out.createObject.netId = static_cast<uint16_t>(bs.readUInt(16));
        out.createObject.defaultTeam = static_cast<uint8_t>(bs.readUInt(2));
        out.createObject.hasExplicitMode = bs.readBool();
        if (out.createObject.hasExplicitMode) {
            out.createObject.mode = static_cast<uint8_t>(bs.readUInt(8));
        } else {
            out.createObject.hasPos = bs.readBool();
            if (out.createObject.hasPos) {
                out.createObject.pos.x = bs.readFloat();
                out.createObject.pos.y = bs.readFloat();
                out.createObject.pos.z = bs.readFloat();
            }
            out.createObject.hasRot = bs.readBool();
            if (out.createObject.hasRot) {
                out.createObject.rot.x = bs.readFloat();
                out.createObject.rot.y = bs.readFloat();
                out.createObject.rot.z = bs.readFloat();
            }
        }
        return !bs.hasError();
    }
    case GE_DESTROY_OBJECT: {
        out.destroyObjectNetId = static_cast<uint16_t>(bs.readUInt(16));
        return !bs.hasError();
    }
    case GE_DESTROY_PLAYER: {
        out.destroyPlayerId = static_cast<uint8_t>(bs.readUInt(8));
        return !bs.hasError();
    }
    case GE_ENTER_VEHICLE: {
        out.enterVehicle.playerId = static_cast<uint8_t>(bs.readUInt(8));
        out.enterVehicle.netId = static_cast<uint16_t>(bs.readUInt(16));
        out.enterVehicle.setAsDefaultVehicle = bs.readBool();
        return !bs.hasError();
    }
    case GE_EXIT_VEHICLE: {
        out.exitVehicle.playerId = static_cast<uint8_t>(bs.readUInt(8));
        out.exitVehicle.field0D = bs.readBool();
        return !bs.hasError();
    }
    case GE_POST_REMOTE: {
        out.postRemote.category = static_cast<uint8_t>(bs.readUInt(4));
        out.postRemote.eventId = bs.readUInt(32);
        out.postRemote.delay = bs.readFloat();
        const uint32_t count = bs.readUInt(8);
        if (bs.hasError()) return false;
        if (count > 0) {
            out.postRemote.payload.resize(count);
            bs.readFixedBytes(&out.postRemote.payload[0], count);
        }
        return !bs.hasError();
    }
    case GE_CHANGE_PLAYER_NAME: {
        bs.skipBits(8u + 32u * 8u);
        return !bs.hasError();
    }
    case GE_HANDLE_DROP: {
        bs.skipBits(8u + 16u + 16u + 96u);
        return !bs.hasError();
    }
    case GE_HANDLE_PICKUP: {
        bs.skipBits(8u + 16u + 16u);
        return !bs.hasError();
    }
    case GE_STRING_BLOCK: {
        const bool newString = bs.readBool();
        if (newString) {
            bs.skipBits(8u);
        } else {
            const uint32_t sz = bs.readUInt(8);
            if (bs.hasError()) return false;
            bs.skipBits(sz * 8u);
        }
        return !bs.hasError();
    }
    case GE_JOIN_SQUAD:
    case GE_SET_SQUAD_LEADER: {
        bs.skipBits(24u); // u8 team, u8 playerId, u8 squadId
        return !bs.hasError();
    }
    case GE_LEAVE_SQUAD:
    case GE_INVITE: {
        bs.skipBits(25u); // u8, u8, u8, 1b
        return !bs.hasError();
    }
    case GE_COMMANDER: {
        bs.skipBits(4u + 8u + 16u); // u4, u8, 1b+15b
        return !bs.hasError();
    }
    case GE_RADIO_MESSAGE: {
        bs.skipBits(8u + 8u + 2u + 8u + 5u + 3u); // 34 bits
        return !bs.hasError();
    }
    case GE_KILLED_BY: {
        out.killedBy.killerId = static_cast<uint8_t>(bs.readUInt(8));
        out.killedBy.victimId = static_cast<uint8_t>(bs.readUInt(8));
        out.killedBy.killerAlive = bs.readBool();
        out.killedBy.field04 = static_cast<uint16_t>(bs.readUInt(16));
        out.killedBy.objTemplateId = bs.readUInt(32);
        out.killedBy.vehTemplateId = bs.readUInt(32);
        out.killedBy.lastHitBoneIdx = static_cast<uint8_t>(bs.readUInt(8));
        out.killedBy.lastHitDir = bs.readCompressedVector(bs.getCompressionVector(), 1000.0f);
        return !bs.hasError();
    }
    case GE_CHANGE_SQUAD_NAME: {
        bs.skipBits(24u); // u8 squadId, u8 team, u8 playerId
        const uint32_t len = bs.readUInt(8);
        if (bs.hasError()) return false;
        bs.skipBits(len * 8u);
        return !bs.hasError();
    }
    case GE_SET_PRIVATE_SQUAD: {
        bs.skipBits(17u); // u8, u8, 1b
        return !bs.hasError();
    }
    case GE_ISSUE_SQUAD_ORDER: {
        bs.skipBits(24u); // u8 squadId, u8 teamId, u8 orderId
        const bool hasTarget = bs.readBool();
        if (hasTarget) {
            bs.skipBits(16u);
        } else {
            bs.skipBits(9u * 32u);
        }
        bs.skipBits(9u); // u8 orderType, 1b flag37
        return !bs.hasError();
    }
    case GE_RANK: {
        bs.skipBits(2u + 6u + 32u + 8u); // 48 bits
        return !bs.hasError();
    }
    case GE_SET_ACCEPT_ORDER: {
        bs.skipBits(8u + 8u + 1u + 8u + 8u); // 33 bits
        return !bs.hasError();
    }
    case GE_SPOTTED: {
        bs.skipBits(16u + 16u + 64u + 8u + 32u + 32u);
        return !bs.hasError();
    }
    case GE_ARTILLERY: {
        const uint32_t artType = bs.readUInt(2);
        bs.skipBits(32u); // u32 team
        if (artType == 1u) {
            bs.skipBits(64u); // 2 x f32
        } else if (artType == 2u) {
            bs.skipBits(16u + 32u); // u16 weaponNetId, u32 fireTick
        }
        return !bs.hasError();
    }
    case GE_CREATE_KIT: {
        bs.skipBits(32u + 16u + 96u + 4u + 4u);
        return !bs.hasError();
    }
    case GE_STICKY_PROJECTILE: {
        bs.skipBits(16u); // projNetId
        const uint32_t targetNetId = bs.readUInt(16);
        if (targetNetId != 0) {
            bs.skipBits(16u); // u8 count, u8 childIndex
        }
        bs.skipBits(8u + 12u * 32u); // u8 playerId, 12 x f32
        return !bs.hasError();
    }
    case GE_AMBIENT_EFFECT_AREA: {
        bs.skipBits(16u + 8u + 16u + 96u);
        return !bs.hasError();
    }
    case GE_VOIP_ON_OFF: {
        bs.skipBits(9u);
        return !bs.hasError();
    }
    case GE_COMMANDER_CAM: {
        bs.skipBits(96u);
        return !bs.hasError();
    }
    case GE_SUPPLY_DROP: {
        bs.skipBits(32u + 64u);
        return !bs.hasError();
    }
    case GE_VOIP_PLAYER_MUTE: {
        bs.skipBits(17u);
        return !bs.hasError();
    }
    case GE_VOTE: {
        bs.skipBits(9u * 8u + 32u);
        return !bs.hasError();
    }
    case GE_TOGGLE_FREE_CAMERA: {
        return true; // 0 payload bits
    }
    case GE_MEDAL: {
        bs.skipBits(32u + 8u + 4u);
        return !bs.hasError();
    }
    case GE_UNLOCK: {
        bs.skipBits(2u + 8u + 14u * 4u);
        return !bs.hasError();
    }
    case GE_MISSILE_INIT: {
        bs.skipBits(16u + 8u + 4u + 1u);
        return !bs.hasError();
    }
    case GE_UAV: {
        bs.skipBits(64u + 8u + 1u);
        return !bs.hasError();
    }
    case GE_CONTENT_CHECK: {
        bs.skipBits(3u * 128u);
        return !bs.hasError();
    }
    case GE_TARGET_DIRECTION: {
        bs.skipBits(16u + 8u + 6u * 32u);
        return !bs.hasError();
    }
    case GE_END_OF_ROUND: {
        out.endOfRound.roundId = bs.readUInt(32);
        out.endOfRound.winnerTeamId = bs.readUInt(32);
        out.endOfRound.victoryType = bs.readUInt(32);
        return !bs.hasError();
    }
    case GE_PYTHON_COMMAND: {
        bs.skipBits(32u + 8u);
        const uint32_t argCount = bs.readUInt(4);
        if (bs.hasError()) return false;
        bs.skipBits(argCount * 32u);
        return !bs.hasError();
    }
    case GE_REQUEST: {
        bs.skipBits(8u + 8u + 3u);
        return !bs.hasError();
    }
    case GE_DROP_VEHICLE: {
        bs.skipBits(64u + 8u + 1u);
        return !bs.hasError();
    }
    case GE_VOIP_SESSION: {
        bs.skipBits(16u);
        return !bs.hasError();
    }
    case GE_KICK_BAN: {
        bs.skipBits(8u + 1u + 8u);
        return !bs.hasError();
    }
    case GE_BEGIN_ROUND: {
        out.beginRound.state = bs.readUInt(32);
        out.beginRound.nextStateChangeAtTick = bs.readUInt(32);
        return !bs.hasError();
    }
    case GE_CREATE_SPAWN_GROUP: {
        out.createSpawnGroup.spawnGroupId = static_cast<uint8_t>(bs.readUInt(8));
        out.createSpawnGroup.team = static_cast<uint8_t>(bs.readUInt(4));
        out.createSpawnGroup.active = bs.readBool();
        out.createSpawnGroup.aiOnly = bs.readBool();
        out.createSpawnGroup.hasSpawnPoints = bs.readBool();
        out.createSpawnGroup.quantX = static_cast<uint8_t>(bs.readUInt(8));
        out.createSpawnGroup.quantZ = static_cast<uint8_t>(bs.readUInt(8));
        out.createSpawnGroup.netId = static_cast<uint16_t>(bs.readUInt(16));
        return !bs.hasError();
    }
    case GE_REMOVE_SPAWN_GROUP: {
        out.removeSpawnGroup.spawnGroupId = static_cast<uint8_t>(bs.readUInt(8));
        out.removeSpawnGroup.netId = static_cast<uint16_t>(bs.readUInt(16));
        return !bs.hasError();
    }
    case GE_UPDATE_TRIGGER: {
        bs.skipBits(32u + 16u + 6u * 32u + 8u + 8u);
        return !bs.hasError();
    }
    case GE_GRAPPLE_CONTAINER_CREATE: {
        bs.skipBits(8u + 16u + 8u + 32u);
        return !bs.hasError();
    }
    case GE_GRAPPLE_CONTAINER_UPDATE: {
        bs.skipBits(8u + 8u + 32u + 32u);
        return !bs.hasError();
    }
    case GE_GRAPPLE_CONTAINER_DETACH: {
        bs.skipBits(8u + 96u);
        return !bs.hasError();
    }
    case GE_GRAPPLE_CREATE: {
        bs.skipBits(16u + 8u + 32u + 6u * 32u);
        return !bs.hasError();
    }
    case GE_GRAPPLE_UPDATE: {
        bs.skipBits(16u + 8u);
        bs.readCompressedVector(bs.getCompressionVector(), 1000.0f);
        bs.readCompressedVector(bs.getCompressionVector(), 1000.0f);
        bs.skipBits(1u);
        return !bs.hasError();
    }
    case GE_PLAYER_TEAR_GASSED: {
        bs.skipBits(8u + 96u);
        return !bs.hasError();
    }
    case GE_SET_NIGHT_VISION: {
        bs.skipBits(9u);
        return !bs.hasError();
    }
    case GE_VERIFY_PLAYER_TEAM:
    case GE_FIX_PLAYER_TEAM: {
        bs.skipBits(32u);
        return !bs.hasError();
    }
    default:
        // Unregistered event ID (0, 18, 33, 44, 52, 53, 67, >69)
        return false;
    }
}

void GameEventStream::dispatchEvent(const ParsedEvent& ev, IGameEventSink* sink) {
    if (ev.typeId == GE_DATA_BLOCK) {
        if (ev.dbIsNewBlock) {
            m_dataBlocks.onNewBlock(ev.dbBlockId, ev.dbBlockLen);
        } else {
            const uint8_t* ptr = ev.dbChunk.empty() ? nullptr : &ev.dbChunk[0];
            m_dataBlocks.onChunk(ptr, static_cast<uint8_t>(ev.dbChunk.size()), sink);
        }
        return;
    }

    if (!sink) {
        return;
    }

    switch (ev.typeId) {
    case GE_CHALLENGE:
        sink->onChallenge(ev.challenge);
        break;
    case GE_CREATE_PLAYER:
        sink->onCreatePlayer(ev.createPlayer);
        break;
    case GE_DESTROY_PLAYER:
        sink->onDestroyPlayer(ev.destroyPlayerId);
        break;
    case GE_CREATE_OBJECT:
        sink->onCreateObject(ev.createObject);
        break;
    case GE_DESTROY_OBJECT:
        sink->onDestroyObject(ev.destroyObjectNetId);
        break;
    case GE_ENTER_VEHICLE:
        sink->onEnterVehicle(ev.enterVehicle);
        break;
    case GE_EXIT_VEHICLE:
        sink->onExitVehicle(ev.exitVehicle);
        break;
    case GE_CREATE_SPAWN_GROUP:
        sink->onCreateSpawnGroup(ev.createSpawnGroup);
        break;
    case GE_REMOVE_SPAWN_GROUP:
        sink->onRemoveSpawnGroup(ev.removeSpawnGroup);
        break;
    case GE_POST_REMOTE:
        sink->onPostRemoteEvent(ev.postRemote);
        break;
    case GE_KILLED_BY:
        sink->onKilledBy(ev.killedBy);
        break;
    case GE_BEGIN_ROUND:
        sink->onBeginRound(ev.beginRound);
        break;
    case GE_END_OF_ROUND:
        sink->onEndOfRound(ev.endOfRound);
        break;
    default:
        break;
    }
}

bool GameEventStream::processReceivedPacket(BitStream& bs, IGameEventSink* sink) {
    const bool present = bs.readBool();
    if (bs.hasError()) {
        bs.clearError();
        return false;
    }
    if (!present) {
        return true;
    }

    const uint32_t count = bs.readUInt(8);
    const uint32_t blockIndex = bs.readUInt(5);
    bs.readBool(); // isResend (read & discarded, matching GameEventManager.cpp:513)
    if (bs.hasError()) {
        bs.clearError();
        return false;
    }

    if (count == 0) {
        return true;
    }

    std::vector<ParsedEvent> block;
    block.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ParsedEvent ev;
        if (!parseSingleEvent(bs, ev)) {
            bs.clearError();
            return false;
        }
        block.push_back(ev);
    }

    if (m_pendingRecvBlocks.find(blockIndex) != m_pendingRecvBlocks.end()) {
        return false; // Duplicate pending blockIndex
    }
    m_pendingRecvBlocks[blockIndex] = block;

    // Drain in exact 5-bit sequential order.
    for (;;) {
        const uint32_t expected = m_nextRecvBlockIndex & 31u;
        std::map<uint32_t, std::vector<ParsedEvent> >::iterator it =
            m_pendingRecvBlocks.find(expected);
        if (it == m_pendingRecvBlocks.end()) {
            break;
        }
        const std::vector<ParsedEvent>& ready = it->second;
        for (size_t i = 0; i < ready.size(); ++i) {
            dispatchEvent(ready[i], sink);
        }
        m_pendingRecvBlocks.erase(it);
        ++m_nextRecvBlockIndex;
    }

    return true;
}

// ============================================================================
// 4. GhostReceiver
// ============================================================================

GhostReceiver::GhostReceiver()
    : m_worldSizeX(1024),
      m_worldSizeZ(1024),
      m_lastTick(0),
      m_soldierNetIds(),
      m_controlPointNetIds() {}

void GhostReceiver::reset() {
    m_lastTick = 0;
    m_soldierNetIds.clear();
    m_controlPointNetIds.clear();
}

void GhostReceiver::setWorldSize(int worldSizeX, int worldSizeZ) {
    if (worldSizeX > 0) m_worldSizeX = worldSizeX;
    if (worldSizeZ > 0) m_worldSizeZ = worldSizeZ;
}

void GhostReceiver::registerSoldierNetId(uint16_t netId) {
    if (netId != 0) {
        m_soldierNetIds.insert(netId);
    }
}

void GhostReceiver::registerControlPointNetId(uint16_t netId) {
    if (netId != 0) {
        m_controlPointNetIds.insert(netId);
    }
}

void GhostReceiver::unregisterNetId(uint16_t netId) {
    m_soldierNetIds.erase(netId);
    m_controlPointNetIds.erase(netId);
}

void GhostReceiver::decodeSoldierUpToSprint(BitStream& bs, uint16_t netId,
                                            bool isFullPrecision,
                                            SoldierGhostState& out) {
    out = SoldierGhostState();
    out.netId = netId;
    const uint32_t mask = bs.readUInt(21);
    out.mask = mask;
    if (bs.hasError()) {
        return;
    }

    if (mask & 0x40u) {
        out.hasHitPoints = true;
        out.hitPointsByte = static_cast<uint8_t>(bs.readUInt(8));
        out.lastDamageType = static_cast<uint8_t>(bs.readUInt(1));
    }

    if (mask & 0x20u) {
        out.hasPose = true;
        out.pose = static_cast<uint8_t>(bs.readUInt(3));
        out.requestedPose = static_cast<uint8_t>(bs.readUInt(3));
    }

    if (mask & 0x8000u) {
        out.isRagdoll = true;
        return; // Caller jumps to exact end-of-block / payloadStart + payloadBits
    }

    const Vec3 zeroRef;
    if (mask & 0x01u) {
        out.hasPos = true;
        out.pos = bs.readCompressedVector(bs.getCompressionVector(), 0.001f);
    }

    if (isFullPrecision) {
        if (mask & 0x80u) {
            out.hasSpeed = true;
            out.speed = bs.readCompressedVector(zeroRef, 0.001f);
        }
        if (mask & 0x100u) {
            bs.readCompressedVector(zeroRef, 0.000099999997f);
        }
        if (mask & 0x200u) {
            bs.readCompressedVector(zeroRef, 0.000099999997f);
        }
        if (mask & 0x40000u) {
            bs.readCompressedVector(zeroRef, 0.001f);
        }
    } else if (mask & 0x80u) {
        out.hasSpeed = true;
        out.speed = bs.readCompressedVector(zeroRef, 0.010000001f);
    }

    if (isFullPrecision) {
        if (mask & 0x02u) { out.hasFacing = true; out.facingDegrees = bs.readFloat(); }
        if (mask & 0x04u) { out.hasLookLR = true; out.lookLeftRightAngle = bs.readFloat(); }
        if (mask & 0x08u) { out.hasPendingTurn = true; out.pendingTurn = bs.readFloat(); }
        if (mask & 0x10u) { out.hasLookUD = true; out.lookUpDownAngle = bs.readFloat(); }
    } else {
        if (mask & 0x02u) { out.hasFacing = true; out.facingDegrees = decodeAngle12(bs, 360.0f); }
        if (mask & 0x04u) { out.hasLookLR = true; out.lookLeftRightAngle = decodeAngle12(bs, 90.0f); }
        if (mask & 0x08u) { out.hasPendingTurn = true; out.pendingTurn = decodeAngle12(bs, 180.0f); }
        if (mask & 0x10u) { out.hasLookUD = true; out.lookUpDownAngle = decodeAngle12(bs, 90.0f); }
    }

    // Unconditional weapon zoom/state & flags
    out.weaponZoom = static_cast<uint8_t>(bs.readUInt(3));
    out.weaponState = static_cast<uint8_t>(bs.readUInt(2));
    out.isSkyDiving = bs.readBool();
    out.isSwimming = bs.readBool();
    out.weaponStowed = bs.readBool();

    // Smoothed movement axes
    const uint32_t smoothBits = isFullPrecision ? 16u : 10u;
    if (mask & 0x400u) bs.skipBits(smoothBits);
    if (mask & 0x800u) bs.skipBits(smoothBits);

    // Unconditional grounded flag (responseByteFA)
    out.isGrounded = bs.readBool();

    // Sprint (0x4000) — last template-independent field before itemIndex (0x1000)
    if (mask & 0x4000u) {
        out.hasSprint = true;
        if (isFullPrecision) {
            out.sprintStamina = static_cast<float>(bs.readUInt(7)) / 127.0f;
            out.isSprinting = bs.readBool();
        } else {
            out.sprintStamina = static_cast<float>(bs.readUInt(5)) / 31.0f;
        }
    }
}

void GhostReceiver::decodePlayerGhost(BitStream& bs, uint16_t netId, PlayerGhostState& out) {
    out = PlayerGhostState();
    out.netId = netId;
    out.playerId = static_cast<uint8_t>(netId - 1u);

    const uint32_t mask = bs.readUInt(28);
    out.mask = mask;
    if (bs.hasError()) {
        return;
    }

    if (mask & 0x1u) {
        out.hasIsAlive = true;
        out.isAlive = bs.readBool();
    }
    if (mask & 0x2u) {
        out.hasSpawnGroup = true;
        out.spawnGroup = bs.readRanged(255u); // 9 bits
    }
    if (mask & 0x8u) {
        bs.readSignedMag(12); // totalScore (1b + 11b)
        bs.readRanged(2047u); // someScore2 (12b)
        bs.readRanged(2047u); // score1 (12b)
        bs.readSignedMag(12); // score4 (1b + 11b)
        bs.readRanged(2047u); // score5 (12b)
    }
    if (mask & 0x200000u) bs.readBool(); // flagHolder
    if (mask & 0x10u) bs.readBool();     // callForMedic

    // Weapon section (StatePrecisionGhost)
    static const uint32_t kFiringBits[3] = { 0x40u, 0x80u, 0x20000u };
    if (mask & 0x2000000u) {
        bs.skipBits(2u);
    }
    for (int w = 0; w < 3; ++w) {
        if ((kFiringBits[w] & mask) == 0 && (mask & 0x10000u) == 0) {
            continue;
        }
        const bool firing = bs.readBool();
        if (!firing && (mask & 0x10000u) == 0) {
            continue;
        }
        bs.readRanged(63u); // deviation + 1 (7 bits)
        bs.skipBits(2u);
        bs.skipBits(1u);
        const bool hot = bs.readBool();
        bs.skipBits(hot ? 8u : 6u);
        bs.skipBits(4u);
    }

    if (mask & 0x40000u) {
        bs.skipBits(2u); // sprint, sprintLastTick
    }
    if (mask & 0x100u) {
        out.hasPing = true;
        out.ping = bs.readRanged(1023u); // 11 bits
    }
    if (mask & 0x20u) {
        out.hasSpawnAtTick = true;
        out.spawnAtTick = bs.readUInt(32);
        out.isManDown = bs.readBool();
    }
    if (mask & 0x200u) {
        out.hasTeam = true;
        out.team = static_cast<int32_t>(bs.readRanged(3u)) - 1; // 3 bits
    }
    if (mask & 0x400u) {
        bs.readRanged(8u); // 4 bits
        bs.readBool();
    }
    if (mask & 0x800u) {
        bs.skipBits(9u); // timeOutsideWorld
    }
    if (mask & 0x1000u) {
        out.hasKit = true;
        const uint32_t kitByte = bs.readUInt(9);
        out.kit = static_cast<uint8_t>(kitByte & 0x0Fu);
        out.unlockLevel = static_cast<uint8_t>((kitByte >> 4) & 0x0Fu);
    }
    if (mask & 0x2000u) {
        out.hasSquadId = true;
        out.squadId = static_cast<uint8_t>(bs.readRanged(15u)); // 5 bits
    }
    if (mask & 0x4000u) {
        bs.readRanged(31u);  // rank + 1 (6 bits)
        bs.readRanged(255u); // 9 bits
        bs.readRanged(255u); // 9 bits
        bs.readRanged(255u); // 9 bits
    }
    if (mask & 0x8000u) bs.readBool();
    if (mask & 0x400000u) bs.readBool();
    if (mask & 0x800000u) bs.skipBits(8u);
    if (mask & 0x1000000u) bs.readRanged(65536u); // 17 bits

    out.isSquadLeader = bs.readBool(); // Unconditional

    if (mask & 0x4000000u) {
        const bool isCommander = bs.readBool();
        if (isCommander) {
            bs.skipBits(5u + 24u);
        }
    }

    out.isConnected = bs.readBool(); // Unconditional

    if (mask & 0x8000000u) {
        bs.readRanged(3u); // 3 bits
    }
}

void GhostReceiver::decodeSpawnGroupGhost(BitStream& bs, uint16_t netId,
                                          SpawnGroupGhostState& out) const {
    out = SpawnGroupGhostState();
    out.netId = netId;
    const uint32_t mask = bs.readUInt(6);
    out.mask = mask;
    if (bs.hasError()) {
        return;
    }

    if (mask & 0x10u) {
        out.hasSpawnPointsField = true;
        out.hasSpawnPoints = bs.readBool();
    }
    if (mask & 0x02u) {
        out.hasTeam = true;
        out.team = static_cast<uint8_t>(bs.readUInt(4));
    }
    if (mask & 0x04u) {
        out.hasActive = true;
        out.active = bs.readBool();
    }
    if (mask & 0x08u) {
        out.hasOnlyForAI = true;
        out.onlyForAI = bs.readBool();
    }
    if (mask & 0x01u) {
        out.hasPos = true;
        out.quantX = static_cast<uint8_t>(bs.readUInt(8));
        out.quantZ = static_cast<uint8_t>(bs.readUInt(8));
        out.pos.x = static_cast<float>(m_worldSizeX) * (static_cast<float>(out.quantX) / 255.0f) -
                    static_cast<float>(m_worldSizeX / 2);
        out.pos.y = 0.0f;
        out.pos.z = static_cast<float>(m_worldSizeZ) * (static_cast<float>(out.quantZ) / 255.0f) -
                    static_cast<float>(m_worldSizeZ / 2);
    }
    if (mask & 0x20u) {
        out.hasSquad = true;
        out.squad = static_cast<int32_t>(bs.readUInt(8)) - 1;
    }
}

void GhostReceiver::decodeControlPointGhost(BitStream& bs, uint16_t netId,
                                            ControlPointGhostState& out) {
    out = ControlPointGhostState();
    out.netId = netId;
    const uint32_t mask = bs.readUInt(7);
    out.mask = mask;
    if (bs.hasError()) {
        return;
    }

    if (mask & 0x01u) { out.hasTeam = true; out.team = bs.readSignedMag(4); }
    if (mask & 0x02u) { out.hasIsNeutral = true; out.isNeutral = bs.readBool(); }
    if (mask & 0x04u) {
        out.hasTakeOver = true;
        out.takeOverCounter = decodeCpUnit16(bs);
        out.takeOverChangePerSecond = decodeCpUnit16(bs);
    }
    if (mask & 0x08u) { out.hasAllowTeam1 = true; out.allowCaptureByTeam1 = (bs.readSignedMag(4) != 0); }
    if (mask & 0x10u) { out.hasAllowTeam2 = true; out.allowCaptureByTeam2 = (bs.readSignedMag(4) != 0); }
    if (mask & 0x20u) { out.hasFlagTeam = true; out.flagTeam = bs.readSignedMag(4); }
    if (mask & 0x40u) { out.hasCapturingState = true; out.capturingState = bs.readSignedMag(4); }
}

bool GhostReceiver::readControlObjectState(BitStream& bs, uint32_t tick, IGhostSink* sink) {
    const uint32_t totalBits = bs.readUInt(12);
    if (bs.hasError()) {
        return false;
    }
    const uint32_t startPos = bs.getBitOffset();
    const uint32_t endPos = startPos + totalBits;
    if (endPos > bs.getBitSize()) {
        return false;
    }

    ControlObjectStateData data;
    data.tick = tick;
    data.actionBufferEntries = bs.readSignedMag(32);

    bs.clearCompressionVector();
    data.compressionVec.x = bs.readFloat();
    data.compressionVec.y = bs.readFloat();
    data.compressionVec.z = bs.readFloat();
    bs.setCompressionVector(data.compressionVec);

    data.controlNetId = static_cast<uint16_t>(bs.readUInt(16));
    data.hasParachute = bs.readBool();
    if (data.hasParachute) {
        data.parachuteNetId = static_cast<uint16_t>(bs.readUInt(16));
    }
    data.isSoldier = bs.readBool();
    if (data.isSoldier) {
        registerSoldierNetId(data.controlNetId);
        data.hasKit = bs.readBool();
        if (data.hasKit) {
            data.kitNetId = static_cast<uint16_t>(bs.readUInt(16));
        }
    }

    if (!bs.hasError() && data.isSoldier && bs.getBitOffset() + 4u <= endPos) {
        bs.readBool();   // isDelta
        bs.readUInt(3);  // baseline slot 0..7
        if (!bs.hasError() && bs.getBitOffset() < endPos) {
            decodeSoldierUpToSprint(bs, data.controlNetId, true, data.soldierState);
            if (!bs.hasError() && bs.getBitOffset() <= endPos) {
                data.hasSoldierState = true;
            }
        }
    }

    // Unconditional exact end-of-block alignment jump (matching GhostManager.cpp:1111-1114).
    bs.clearError();
    if (!bs.setBitOffset(endPos)) {
        return false;
    }

    if (sink) {
        sink->onControlObjectState(data);
    }
    return true;
}

bool GhostReceiver::processReceivedPacket(BitStream& bs, IGhostSink* sink) {
    const bool present = bs.readBool();
    if (bs.hasError()) {
        bs.clearError();
        return false;
    }
    if (!present) {
        return true;
    }

    const uint32_t tick = bs.readUInt(32);
    const uint32_t count = bs.readUInt(8);
    const bool hasControlState = bs.readBool();
    if (bs.hasError()) {
        bs.clearError();
        return false;
    }

    m_lastTick = tick;
    bs.clearCompressionVector();

    if (hasControlState) {
        if (!readControlObjectState(bs, tick, sink)) {
            bs.clearError();
            return false;
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t action = bs.readUInt(2);
        const uint16_t netId = static_cast<uint16_t>(bs.readUInt(16));
        if (bs.hasError()) {
            bs.clearError();
            return false;
        }

        if (action == 0u) {
            continue; // No-op
        }
        if (action == 2u) {
            return false; // Protocol error
        }
        if (action == 3u) {
            if (sink) {
                sink->onGhostRemoved(netId);
            }
            continue;
        }

        // action == 1: update
        bs.readBool(); // ghostActive
        const uint32_t payloadBits = bs.readUInt(11);
        if (bs.hasError()) {
            bs.clearError();
            return false;
        }

        const uint32_t payloadStart = bs.getBitOffset();
        const uint32_t payloadEnd = payloadStart + payloadBits;
        if (payloadEnd > bs.getBitSize()) {
            return false;
        }

        if (netId >= 1u && netId <= 256u) {
            PlayerGhostState player;
            decodePlayerGhost(bs, netId, player);
            if (!bs.hasError() && bs.getBitOffset() <= payloadEnd && sink) {
                sink->onPlayerGhost(player);
            }
        } else if (netId >= 514u && netId <= 641u) {
            SpawnGroupGhostState sg;
            decodeSpawnGroupGhost(bs, netId, sg);
            if (!bs.hasError() && bs.getBitOffset() <= payloadEnd && sink) {
                sink->onSpawnGroupGhost(sg);
            }
        } else if (m_soldierNetIds.find(netId) != m_soldierNetIds.end()) {
            SoldierGhostState soldier;
            decodeSoldierUpToSprint(bs, netId, false, soldier);
            if (!bs.hasError() && bs.getBitOffset() <= payloadEnd && sink) {
                sink->onSoldierGhost(soldier);
            }
        } else if (m_controlPointNetIds.find(netId) != m_controlPointNetIds.end()) {
            ControlPointGhostState cp;
            decodeControlPointGhost(bs, netId, cp);
            if (!bs.hasError() && bs.getBitOffset() <= payloadEnd && sink) {
                sink->onControlPointGhost(cp);
            }
        }

        // Guaranteed exact bit alignment after every ghost record!
        bs.clearError();
        if (!bs.setBitOffset(payloadEnd)) {
            return false;
        }
    }

    bs.clearCompressionVector();
    return true;
}

// ============================================================================
// 5. ClientPacketCodec
// ============================================================================

ClientPacketCodec::ClientPacketCodec()
    : m_actions(), m_events(), m_ghosts() {}

void ClientPacketCodec::reset() {
    m_actions.reset();
    m_events.reset();
    m_ghosts.reset();
}

bool ClientPacketCodec::buildClientPacket(uint32_t deliveryId, uint8_t* outPayload,
                                          size_t outCap, size_t& outLen) {
    outLen = 0;
    if (!outPayload || outCap == 0) {
        return false;
    }

    const size_t usableCap = (outCap > kMaxDataPayloadSize) ? kMaxDataPayloadSize : outCap;
    std::memset(outPayload, 0, usableCap);
    BitStream bs(outPayload, static_cast<uint32_t>(usableCap));

    // Exact BF2 stream manager order: 0=PlayerActionManager, 1=GameEventManager, 2=GhostManager
    const bool wroteActions = m_actions.transmit(bs);
    const bool wroteEvents = m_events.transmit(bs, deliveryId, 0);
    bs.writeBool(false); // GhostManager C->S is always present=0

    if (bs.hasError()) {
        return false;
    }
    if (!wroteActions && !wroteEvents) {
        return false; // Nothing to transmit (matches ClientConnection::transmitMsgs)
    }

    outLen = bs.getByteSize();
    return outLen > 0;
}

bool ClientPacketCodec::parseServerPacket(const uint8_t* payload, size_t len,
                                          IGameEventSink* eventSink, IGhostSink* ghostSink) {
    if (!payload || len == 0) {
        return false;
    }
    BitStream bs(payload, static_cast<uint32_t>(len));

    // 0: PlayerActionManager
    if (!PlayerActionSender::skipIncoming(bs)) {
        return false;
    }

    // 1: GameEventManager (executed before GhostManager, matching ClientConnection.cpp:475-488)
    CodecEventSinkAdapter adapter(m_ghosts, eventSink);
    if (!m_events.processReceivedPacket(bs, &adapter)) {
        return false;
    }

    // 2: GhostManager
    if (!m_ghosts.processReceivedPacket(bs, ghostSink)) {
        return false;
    }

    return true;
}

void ClientPacketCodec::onPacketStatus(uint32_t deliveryId, bool delivered) {
    m_events.onPacketStatus(deliveryId, delivered);
}

} // namespace bf2bot
