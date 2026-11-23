#include "LoadBotClient.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bf2bot {

namespace {

static const uint64_t kConnectRetryUs   = 1000000ULL;  // 1.0 s
static const uint64_t kPingIntervalUs   = 2000000ULL;  // 2.0 s
static const uint64_t kReconnectDelayUs = 2000000ULL;  // 2.0 s
static const uint64_t kIdleTimeoutUs    = 20000000ULL; // 20.0 s

} // namespace

LoadBotClient::LoadBotClient(const LoadBotConfig& cfg,
                             const sockaddr_in& serverAddr,
                             const NavMesh* navMesh,
                             const RouteLoader* routeLoader,
                             Telemetry* telemetry)
    : m_config(cfg),
      m_serverAddr(serverAddr),
      m_sockFd(kInvalidBotSocket),
      m_rawSendHook(),
      m_playerName(),
      m_profileId(100000 + static_cast<int32_t>(cfg.botIndex + 1u)),
      m_state(BOT_DISCONNECTED),
      m_originUs(0),
      m_currentNowUs(0),
      m_nextConnectTryUs(cfg.startAtUs),
      m_nextPingUs(0),
      m_lastRxUs(0),
      m_lastDataPacketRxUs(0),
      m_nextDeliveryId(1u),
      m_packetGhostCount(0u),
      m_unackedDataPackets(0u),
      m_useContentChecking(false),
      m_sentInitialJoin(false),
      m_conn(),
      m_codec(),
      m_uplinkImpair(cfg.uplinkImpair),
      m_downlinkImpair(cfg.downlinkImpair),
      m_brain(cfg.botIndex, cfg.seed, navMesh, routeLoader),
      m_telemetry(telemetry),
      m_hasOwnPlayerId(false),
      m_ownPlayerId(0),
      m_ownTeam(m_brain.assignedTeam()),
      m_ownAlive(false),
      m_ownManDown(false),
      m_ownSoldierNetId(0),
      m_hasOwnPos(false),
      m_ownPos(),
      m_ownFacingDegrees(0.0f),
      m_ownPitchDegrees(0.0f),
      m_ownPose(0),
      m_players(),
      m_playerNetIdToPlayerId(),
      m_soldiers(),
      m_spawnGroups(),
      m_spawnGroupNetIdToId(),
      m_controlPoints(),
      m_playerToSoldierNetId(),
      m_soldierNetIdToPlayerId(),
      m_objectPositions() {
    char nameBuf[32];
    std::snprintf(nameBuf, sizeof(nameBuf), "LoadBot_%02u",
                  static_cast<unsigned>(cfg.botIndex + 1u));
    m_playerName = nameBuf;

    const int worldInt = static_cast<int>(cfg.worldSize > 64.0f ? cfg.worldSize : 1024.0f);
    m_codec.ghosts().setWorldSize(worldInt, worldInt);
}

LoadBotClient::~LoadBotClient() {
    closeSocket();
}

bool LoadBotClient::initSocket() {
    if (m_sockFd != kInvalidBotSocket) {
        return true;
    }
#ifdef _WIN32
    const BotSocket fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidBotSocket) {
        return false;
    }
    u_long nonBlocking = 1u;
    if (::ioctlsocket(fd, FIONBIO, &nonBlocking) != 0) {
        ::closesocket(fd);
        return false;
    }
    int rcvBuf = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf),
                 static_cast<int>(sizeof(rcvBuf)));
#else
    const BotSocket fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return false;
    }
    int rcvBuf = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
#endif

    sockaddr_in localAddr;
    std::memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&localAddr),
               static_cast<int>(sizeof(localAddr))) < 0) {
#ifdef _WIN32
        ::closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }

    m_sockFd = fd;
    return true;
}

void LoadBotClient::closeSocket() {
    if (m_sockFd != kInvalidBotSocket) {
#ifdef _WIN32
        ::closesocket(m_sockFd);
#else
        ::close(m_sockFd);
#endif
        m_sockFd = kInvalidBotSocket;
    }
}

uint32_t LoadBotClient::localTimeMs(uint64_t nowUs) const {
    if (m_originUs == 0 || nowUs <= m_originUs) {
        return 0u;
    }
    return static_cast<uint32_t>((nowUs - m_originUs) / 1000ULL);
}

uint32_t LoadBotClient::djb2LowerName(const std::string& s) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < s.size(); ++i) {
        int c = static_cast<unsigned char>(s[i]);
        if (std::isupper(c)) {
            c = std::tolower(c);
        }
        h = static_cast<uint32_t>(c) ^ (33u * h);
    }
    return h;
}

Vec3 LoadBotClient::dequantizeSpawnGroupPos(uint8_t quantX, uint8_t quantZ) const {
    const float w = (m_config.worldSize > 64.0f) ? m_config.worldSize : 1024.0f;
    const float x = (static_cast<float>(quantX) / 255.0f - 0.5f) * w;
    const float z = (static_cast<float>(quantZ) / 255.0f - 0.5f) * w;
    return Vec3(x, 0.0f, z);
}

void LoadBotClient::resetSession(uint64_t reconnectAtUs) {
    m_state = BOT_DISCONNECTED;
    m_nextConnectTryUs = reconnectAtUs;
    m_nextPingUs = 0;
    m_lastRxUs = 0;
    m_lastDataPacketRxUs = 0;
    m_nextDeliveryId = 1u;
    m_unackedDataPackets = 0u;
    m_useContentChecking = false;
    m_sentInitialJoin = false;

    m_conn.reset();
    m_codec.reset();
    const int worldInt = static_cast<int>(m_config.worldSize > 64.0f ? m_config.worldSize : 1024.0f);
    m_codec.ghosts().setWorldSize(worldInt, worldInt);

    m_uplinkImpair.clear();
    m_downlinkImpair.clear();
    m_brain.reset();

    m_hasOwnPlayerId = false;
    m_ownPlayerId = 0;
    m_ownTeam = m_brain.assignedTeam();
    m_ownAlive = false;
    m_ownManDown = false;
    m_ownSoldierNetId = 0;
    m_hasOwnPos = false;
    m_ownPos = Vec3();
    m_ownFacingDegrees = 0.0f;
    m_ownPitchDegrees = 0.0f;
    m_ownPose = 0;

    m_players.clear();
    m_playerNetIdToPlayerId.clear();
    m_soldiers.clear();
    m_spawnGroups.clear();
    m_spawnGroupNetIdToId.clear();
    m_controlPoints.clear();
    m_playerToSoldierNetId.clear();
    m_soldierNetIdToPlayerId.clear();
    m_objectPositions.clear();

    if (m_telemetry) {
        m_telemetry->clearEnemyTrackingForBot(static_cast<uint8_t>(m_config.botIndex));
    }
}

void LoadBotClient::sendDirectSocket(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return;
    }
    if (m_rawSendHook) {
        m_rawSendHook(data, len);
        return;
    }
    if (m_sockFd == kInvalidBotSocket) {
        return;
    }
#ifdef _WIN32
    ::sendto(m_sockFd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
             reinterpret_cast<const sockaddr*>(&m_serverAddr),
             static_cast<int>(sizeof(m_serverAddr)));
#else
    ::sendto(m_sockFd, data, len, 0,
             reinterpret_cast<const sockaddr*>(&m_serverAddr),
             static_cast<socklen_t>(sizeof(m_serverAddr)));
#endif
}

void LoadBotClient::injectRawDatagram(const uint8_t* data, size_t len, uint64_t nowUs) {
    if (!data || len == 0 || m_state == BOT_STOPPED) {
        return;
    }
    if (m_originUs == 0) {
        m_originUs = nowUs;
    }
    m_currentNowUs = nowUs;
    if (m_downlinkImpair.isActive()) {
        m_downlinkImpair.enqueue(nowUs, data, len);
    } else {
        processIncomingDatagram(data, len, nowUs);
    }
}

void LoadBotClient::sendRawDatagram(const uint8_t* data, size_t len, uint64_t nowUs) {
    if (!data || len == 0) {
        return;
    }
    if (m_telemetry) {
        m_telemetry->recordPacketTx(len);
    }
    if (m_uplinkImpair.isActive()) {
        m_uplinkImpair.enqueue(nowUs, data, len);
    } else {
        sendDirectSocket(data, len);
    }
}

void LoadBotClient::onSocketReadable(uint64_t nowUs) {
    if (m_sockFd == kInvalidBotSocket || m_state == BOT_STOPPED) {
        return;
    }
    if (m_originUs == 0) {
        m_originUs = nowUs;
    }
    m_currentNowUs = nowUs;

    uint8_t buf[2048];
    for (;;) {
        sockaddr_in fromAddr;
#ifdef _WIN32
        int fromLen = static_cast<int>(sizeof(fromAddr));
        const int n = ::recvfrom(m_sockFd, reinterpret_cast<char*>(buf),
                                 static_cast<int>(sizeof(buf)), 0,
                                 reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n < 0) {
            const int err = ::WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;
            }
            if (err == WSAECONNRESET || err == WSAEINTR) {
                continue;
            }
            break;
        }
#else
        socklen_t fromLen = static_cast<socklen_t>(sizeof(fromAddr));
        const ssize_t n = ::recvfrom(m_sockFd, buf, sizeof(buf), 0,
                                     reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
#endif
        if (n == 0) {
            continue;
        }

        const size_t pktLen = static_cast<size_t>(n);
        if (m_downlinkImpair.isActive()) {
            m_downlinkImpair.enqueue(nowUs, buf, pktLen);
        } else {
            processIncomingDatagram(buf, pktLen, nowUs);
        }
    }
}

void LoadBotClient::flushImpairmentQueues(uint64_t nowUs) {
    if (m_state == BOT_STOPPED) {
        return;
    }
    m_currentNowUs = nowUs;

    std::vector<uint8_t> pkt;
    while (m_uplinkImpair.popReady(nowUs, pkt)) {
        sendDirectSocket(pkt.data(), pkt.size());
    }
    while (m_downlinkImpair.popReady(nowUs, pkt)) {
        processIncomingDatagram(pkt.data(), pkt.size(), nowUs);
    }
}

uint64_t LoadBotClient::nextWakeUs() const {
    const uint64_t up = m_uplinkImpair.nextDeliverUs();
    const uint64_t dn = m_downlinkImpair.nextDeliverUs();
    return (up < dn) ? up : dn;
}

void LoadBotClient::processIncomingDatagram(const uint8_t* data, size_t len, uint64_t nowUs) {
    if (m_state == BOT_STOPPED) {
        return;
    }
    m_currentNowUs = nowUs;

    ParsedDatagram dgram;
    if (!parseIncomingDatagram(data, len, dgram)) {
        if (m_telemetry) {
            m_telemetry->recordProtocolError();
        }
        return;
    }

    if (dgram.isFilterPacket) {
        return; // Ignore PunkBuster pre-filter packets cleanly
    }

    m_lastRxUs = nowUs;

    switch (dgram.type) {
    case UDP_CONNECT_ACCEPT: {
        if (m_state == BOT_CONNECTING) {
            m_conn.setAssignedConnId(dgram.assignedConnId);
            uint8_t ackBuf[16];
            const size_t ackLen = buildConnectAcceptAck(ackBuf, sizeof(ackBuf), dgram.assignedConnId);
            if (ackLen > 0) {
                // Send 2x ConnectAcceptAck for reliability under simulated packet loss.
                sendRawDatagram(ackBuf, ackLen, nowUs);
                sendRawDatagram(ackBuf, ackLen, nowUs);
            }
            m_state = BOT_HANDSHAKING;
            m_nextPingUs = nowUs + kPingIntervalUs;
            if (m_config.verbose) {
                std::fprintf(stderr, "[%s] ConnectAccept connId=%u -> Handshaking\n",
                             m_playerName.c_str(), static_cast<unsigned>(dgram.assignedConnId));
            }
        }
        break;
    }

    case UDP_CONNECT_DENIED: {
        if (m_config.verbose) {
            std::fprintf(stderr, "[%s] ConnectDenied reason=%u text=\"%s\"\n",
                         m_playerName.c_str(), dgram.reason, dgram.text.c_str());
        }
        if (m_telemetry) {
            m_telemetry->recordDisconnect();
        }
        resetSession(nowUs + kReconnectDelayUs);
        break;
    }

    case UDP_DISCONNECT: {
        if (m_config.verbose) {
            std::fprintf(stderr, "[%s] Disconnect reason=%u text=\"%s\"\n",
                         m_playerName.c_str(), dgram.reason, dgram.text.c_str());
        }
        if (m_telemetry) {
            m_telemetry->recordDisconnect();
        }
        resetSession(nowUs + kReconnectDelayUs);
        break;
    }

    case UDP_PING_REQUEST: {
        if (!m_conn.hasAssignedConnId()) {
            break;
        }
        m_conn.handleRemoteAck(dgram.ack, dgram.ackMask,
            [this](uint32_t deliveryId, bool delivered) {
                m_codec.onPacketStatus(deliveryId, delivered);
            });
        uint8_t respBuf[64];
        const size_t respLen = buildPingResponse(respBuf, sizeof(respBuf), m_conn,
                                                 dgram.pingTimestampMs, localTimeMs(nowUs));
        if (respLen > 0) {
            m_unackedDataPackets = 0u;
            sendRawDatagram(respBuf, respLen, nowUs);
        }
        break;
    }

    case UDP_PING_RESPONSE: {
        if (!m_conn.hasAssignedConnId()) {
            break;
        }
        m_conn.handleRemoteAck(dgram.ack, dgram.ackMask,
            [this](uint32_t deliveryId, bool delivered) {
                m_codec.onPacketStatus(deliveryId, delivered);
            });
        m_conn.recordPingResponse(dgram.pingSenderTimestampMs, localTimeMs(nowUs));
        break;
    }

    case UDP_DATA_PACKET: {
        if (!m_conn.hasAssignedConnId()) {
            break;
        }
        if (!m_conn.acceptIncomingDataSeq(dgram.seq)) {
            break; // Duplicate or out-of-window sequence
        }
        ++m_unackedDataPackets;
        m_conn.handleRemoteAck(dgram.ack, dgram.ackMask,
            [this](uint32_t deliveryId, bool delivered) {
                m_codec.onPacketStatus(deliveryId, delivered);
            });

        m_packetGhostCount = 0u;
        const bool ok = m_codec.parseServerPacket(dgram.payload, dgram.payloadLen, this, this);
        if (!ok && m_telemetry) {
            m_telemetry->recordProtocolError();
        }

        double gapMs = 0.0;
        if (m_lastDataPacketRxUs != 0 && nowUs > m_lastDataPacketRxUs) {
            gapMs = static_cast<double>(nowUs - m_lastDataPacketRxUs) / 1000.0;
        }
        m_lastDataPacketRxUs = nowUs;

        if (m_telemetry) {
            m_telemetry->recordPacketRx(len, m_packetGhostCount, gapMs);
        }
        break;
    }

    default:
        break;
    }
}

void LoadBotClient::tick30Hz(uint64_t nowUs, float dtSec) {
    if (m_state == BOT_STOPPED) {
        return;
    }
    if (m_originUs == 0) {
        m_originUs = nowUs;
    }
    m_currentNowUs = nowUs;

    if (m_state == BOT_DISCONNECTED) {
        if (nowUs >= m_nextConnectTryUs) {
            m_state = BOT_CONNECTING;
            m_nextConnectTryUs = nowUs;
        } else {
            return;
        }
    }

    if (m_state == BOT_CONNECTING) {
        if (nowUs >= m_nextConnectTryUs) {
            uint8_t reqBuf[128];
            const size_t reqLen = buildConnectRequest(reqBuf, sizeof(reqBuf),
                                                      m_config.password, m_config.modName,
                                                      "", false);
            if (reqLen > 0) {
                sendRawDatagram(reqBuf, reqLen, nowUs);
            }
            m_nextConnectTryUs = nowUs + kConnectRetryUs;
        }
        return;
    }

    // Idle timeout check (server disappeared or dropped us silently)
    if (m_lastRxUs != 0 && nowUs > m_lastRxUs && (nowUs - m_lastRxUs) > kIdleTimeoutUs) {
        if (m_config.verbose) {
            std::fprintf(stderr, "[%s] Idle timeout (>20s), reconnecting\n", m_playerName.c_str());
        }
        if (m_telemetry) {
            m_telemetry->recordDisconnect();
        }
        resetSession(nowUs + kReconnectDelayUs);
        return;
    }

    // Periodic PingRequest keepalive every 2.0 s
    bool sentAckCarrier = false;
    if (nowUs >= m_nextPingUs) {
        uint8_t pingBuf[64];
        const size_t pingLen = buildPingRequest(pingBuf, sizeof(pingBuf), m_conn, localTimeMs(nowUs));
        if (pingLen > 0) {
            m_unackedDataPackets = 0u;
            sentAckCarrier = true;
            sendRawDatagram(pingBuf, pingLen, nowUs);
        }
        m_nextPingUs = nowUs + kPingIntervalUs;
    }

    // Run Combat & Navigation AI once InGame and initial JoinTeam/Kit/Spawn handshake is queued
    if (m_state == BOT_IN_GAME && m_sentInitialJoin) {
        BotWorldView view;
        view.ownPlayerId = m_ownPlayerId;
        view.ownTeam = m_ownTeam;
        view.ownAlive = m_ownAlive;
        view.ownManDown = m_ownManDown;
        view.ownSoldierNetId = m_ownSoldierNetId;
        view.hasOwnPos = m_hasOwnPos;
        view.ownPos = m_ownPos;
        view.ownFacingDegrees = m_ownFacingDegrees;
        view.ownPitchDegrees = m_ownPitchDegrees;
        view.ownPose = m_ownPose;
        view.worldSize = m_config.worldSize;
        view.soldiers = &m_soldiers;
        view.spawnGroups = &m_spawnGroups;
        view.controlPoints = &m_controlPoints;

        BotTickOutput out;
        m_brain.tick(nowUs, dtSec, view, out);

        if (out.requestSpawn && out.desiredSpawnGroupId >= 0) {
            m_codec.events().queueSelectSpawnPoint(out.desiredSpawnGroupId);
            m_codec.events().queueSelectKit(out.desiredKit, 0);
        }

        if (m_ownAlive || m_codec.actions().isEnabled()) {
            m_codec.actions().pushAction(out.action);
        }
    }

    // Transmit client->server DataPacket if we have pending events or enabled actions
    if (!m_conn.isSendWindowStalled()) {
        uint8_t payload[kMaxDataPayloadSize];
        size_t payloadLen = 0;
        const uint32_t deliveryId = m_nextDeliveryId;
        if (m_codec.buildClientPacket(deliveryId, payload, sizeof(payload), payloadLen)) {
            uint8_t wireBuf[kMaxDatagramSize];
            size_t wireLen = 0;
            if (m_conn.wrapDataPacket(payload, static_cast<uint16_t>(payloadLen),
                                      deliveryId, wireBuf, sizeof(wireBuf), wireLen)) {
                ++m_nextDeliveryId;
                m_unackedDataPackets = 0u;
                sentAckCarrier = true;
                sendRawDatagram(wireBuf, wireLen, nowUs);
            }
        }
    }

    // Keep the server's 31-packet SWConnection window flowing during handshaking bursts
    // (initial CreateObject/CreatePlayer database) when no C->S DataPacket is being sent.
    if (!sentAckCarrier && m_conn.hasAssignedConnId() &&
        ((m_state == BOT_HANDSHAKING && m_unackedDataPackets > 0u) || m_unackedDataPackets >= 8u)) {
        uint8_t pingBuf[64];
        const size_t pingLen = buildPingRequest(pingBuf, sizeof(pingBuf), m_conn, localTimeMs(nowUs));
        if (pingLen > 0) {
            m_unackedDataPackets = 0u;
            sendRawDatagram(pingBuf, pingLen, nowUs);
        }
    }
}

void LoadBotClient::disconnectGracefully() {
    if (m_state == BOT_STOPPED) {
        return;
    }
    if (m_conn.hasAssignedConnId() && (m_sockFd != kInvalidBotSocket || m_rawSendHook)) {
        uint8_t discBuf[32];
        const size_t discLen = buildDisconnect(discBuf, sizeof(discBuf),
                                               static_cast<uint8_t>(m_conn.assignedConnId()), 0u);
        if (discLen > 0) {
            // Send 3x directly (bypassing simulated delay) so server cleans up cleanly.
            for (int i = 0; i < 3; ++i) {
                sendDirectSocket(discBuf, discLen);
            }
        }
    }
    m_state = BOT_STOPPED;
}

// ============================================================================
// IGameEventSink implementation
// ============================================================================

void LoadBotClient::onChallenge(const ChallengeEventData& /*data*/) {
    uint8_t response73[73];
    std::memset(response73, 0, sizeof(response73));
    // Deterministic LAN response bytes (sv.internet 0 bypasses GameSpy CD-key validation)
    static const char kHex[] = "0123456789abcdef";
    uint32_t h = djb2LowerName(m_playerName) ^ 0x5A17C3E1u;
    for (int i = 0; i < 72; ++i) {
        h = h * 1664525u + 1013904223u;
        response73[i] = static_cast<uint8_t>(kHex[(h >> 24) & 0x0Fu]);
    }
    response73[72] = 0u;
    m_codec.events().queueChallengeResponse(response73, 0u, kBf2BuildVersion, kGameIdBf2);
    if (m_config.verbose) {
        std::fprintf(stderr, "[%s] Rx Challenge -> queued ChallengeResponse\n", m_playerName.c_str());
    }
}

void LoadBotClient::onServerInfo(const ServerInfoData& data) {
    m_useContentChecking = data.useContentChecking;
    // Only answer join ServerInfo (requestResponse == true); periodic updates have requestResponse == false
    // and answering them triggers a duplicate-player kick (reason 13) in GameServer::handleClientInfo.
    if (data.requestResponse) {
        m_codec.events().queueClientInfo(m_playerName, djb2LowerName(m_playerName),
                                         m_profileId, "", "", false);
    }
    if (m_config.verbose) {
        std::fprintf(stderr, "[%s] Rx ServerInfo (reqResp=%d contentCheck=%d)%s\n",
                     m_playerName.c_str(),
                     data.requestResponse ? 1 : 0,
                     m_useContentChecking ? 1 : 0,
                     data.requestResponse ? " -> queued ClientInfo" : "");
    }
}

void LoadBotClient::onMapInfo(const MapInfoData& data) {
    m_codec.events().queueMapLoaded();
    if (m_useContentChecking) {
        uint8_t zeroHash[16];
        std::memset(zeroHash, 0, sizeof(zeroHash));
        m_codec.events().queueContentCheck(zeroHash, zeroHash, zeroHash);
    }
    if (m_config.verbose) {
        std::fprintf(stderr, "[%s] Rx MapInfo (%s / %s) -> queued MapLoaded\n",
                     m_playerName.c_str(), data.mapName.c_str(), data.gameMode.c_str());
    }
}

void LoadBotClient::onCreatePlayer(const CreatePlayerEventData& data) {
    TrackedPlayer& p = m_players[data.playerId];
    p.playerId = data.playerId;
    p.playerNetId = data.playerNetId;
    p.team = static_cast<int32_t>(data.teamId);
    p.spawnGroupId = data.spawnGroupId;
    p.name = data.name;
    m_playerNetIdToPlayerId[data.playerNetId] = data.playerId;

    if (!data.isRemote || (!m_hasOwnPlayerId && data.name == m_playerName)) {
        m_hasOwnPlayerId = true;
        m_ownPlayerId = data.playerId;
        if (data.teamId != 0) {
            m_ownTeam = static_cast<int32_t>(data.teamId);
            m_brain.setTeam(m_ownTeam);
        }
    }
}

void LoadBotClient::onDestroyPlayer(uint8_t playerId) {
    std::map<uint8_t, TrackedPlayer>::iterator pit = m_players.find(playerId);
    if (pit != m_players.end()) {
        m_playerNetIdToPlayerId.erase(pit->second.playerNetId);
        m_players.erase(pit);
    }
    std::map<uint8_t, uint16_t>::iterator sit = m_playerToSoldierNetId.find(playerId);
    if (sit != m_playerToSoldierNetId.end()) {
        m_soldierNetIdToPlayerId.erase(sit->second);
        m_playerToSoldierNetId.erase(sit);
    }
}

void LoadBotClient::onCreateObject(const CreateObjectEventData& data) {
    if (data.hasPos) {
        m_objectPositions[data.netId] = data.pos;
        std::map<uint16_t, TrackedControlPoint>::iterator cpit = m_controlPoints.find(data.netId);
        if (cpit != m_controlPoints.end()) {
            cpit->second.pos = data.pos;
            cpit->second.hasPos = true;
        }
    }
}

void LoadBotClient::onDestroyObject(uint16_t netId) {
    m_objectPositions.erase(netId);
    m_soldiers.erase(netId);
    m_controlPoints.erase(netId);

    std::map<uint16_t, uint8_t>::iterator s2p = m_soldierNetIdToPlayerId.find(netId);
    if (s2p != m_soldierNetIdToPlayerId.end()) {
        m_playerToSoldierNetId.erase(s2p->second);
        m_soldierNetIdToPlayerId.erase(s2p);
    }
}

void LoadBotClient::onEnterVehicle(const EnterVehicleEventData& data) {
    if (data.setAsDefaultVehicle && data.netId != 0u) {
        m_playerToSoldierNetId[data.playerId] = data.netId;
        m_soldierNetIdToPlayerId[data.netId] = data.playerId;

        TrackedSoldier& s = m_soldiers[data.netId];
        s.netId = data.netId;
        s.ownerPlayerId = data.playerId;

        std::map<uint8_t, TrackedPlayer>::iterator pit = m_players.find(data.playerId);
        if (pit != m_players.end()) {
            pit->second.soldierNetId = data.netId;
            if (pit->second.team != 0) {
                s.team = pit->second.team;
            }
        }

        if (m_hasOwnPlayerId && data.playerId == m_ownPlayerId) {
            m_ownSoldierNetId = data.netId;
        }
    }
}

void LoadBotClient::onExitVehicle(const ExitVehicleEventData& /*data*/) {
}

void LoadBotClient::onCreateSpawnGroup(const CreateSpawnGroupEventData& data) {
    TrackedSpawnGroup& sg = m_spawnGroups[data.spawnGroupId];
    sg.spawnGroupId = data.spawnGroupId;
    sg.netId = data.netId;
    sg.team = data.team;
    sg.active = data.active;
    sg.aiOnly = data.aiOnly;
    sg.hasSpawnPoints = data.hasSpawnPoints;
    sg.pos = dequantizeSpawnGroupPos(data.quantX, data.quantZ);
    sg.hasPos = true;
    m_spawnGroupNetIdToId[data.netId] = data.spawnGroupId;
}

void LoadBotClient::onRemoveSpawnGroup(const RemoveSpawnGroupEventData& data) {
    m_spawnGroups.erase(data.spawnGroupId);
    m_spawnGroupNetIdToId.erase(data.netId);
}

void LoadBotClient::onPostRemoteEvent(const PostRemoteEventData& data) {
    if (data.category != EC_NETWORK) {
        return;
    }

    if (data.eventId == NRE_CLIENT_READY) {
        // Server finished sending initial DB -> respond ClientReady and enter InGame.
        m_codec.events().queueClientReady();
        m_state = BOT_IN_GAME;
        if (m_config.verbose) {
            std::fprintf(stderr, "[%s] Rx DBComplete(6,4) -> queued ClientReady -> InGame\n",
                         m_playerName.c_str());
        }
    } else if (data.eventId == NRE_CLIENT_READY_ACK) {
        if (!m_sentInitialJoin) {
            m_sentInitialJoin = true;
            m_codec.events().queueConnectionType(5u);
            m_codec.events().queueJoinTeam(m_brain.assignedTeam());
            m_codec.events().queueSelectKit(m_brain.assignedKit(), 0);
            const int32_t sg = m_brain.pickSpawnGroup(m_spawnGroups);
            if (sg >= 0) {
                m_codec.events().queueSelectSpawnPoint(sg);
            }
            if (m_config.verbose) {
                std::fprintf(stderr, "[%s] Rx ClientReadyAck(6,3) -> JoinTeam(%d) SelectKit(%d) SG(%d)\n",
                             m_playerName.c_str(), m_brain.assignedTeam(), m_brain.assignedKit(), sg);
            }
        }
    } else if (data.eventId == NRE_CONFIRM_RESET) {
        // Round / map reset synchronization (GameClient.cpp ECNetwork 5 -> GameServer::confirmReset).
        m_codec.events().queueConfirmReset();
    } else if (data.eventId == NRE_PLAYER_SPAWNED && data.payload.size() >= 4u) {
        int32_t pid = 0;
        std::memcpy(&pid, &data.payload[0], sizeof(pid));
        if (m_hasOwnPlayerId && pid == static_cast<int32_t>(m_ownPlayerId)) {
            const bool wasAlive = m_ownAlive && !m_ownManDown;
            m_ownAlive = true;
            m_ownManDown = false;
            if (!wasAlive && m_hasOwnPos) {
                m_brain.onSpawned(m_ownPos, m_currentNowUs);
            }
        }
    } else if (data.eventId == NRE_PLAYER_KILLED && data.payload.size() >= 4u) {
        int32_t pid = 0;
        std::memcpy(&pid, &data.payload[0], sizeof(pid));
        if (m_hasOwnPlayerId && pid == static_cast<int32_t>(m_ownPlayerId)) {
            if (m_ownAlive) {
                m_ownAlive = false;
                m_brain.onDied(m_currentNowUs);
            }
        }
    }
}

void LoadBotClient::onKilledBy(const KilledByEventData& data) {
    if (m_hasOwnPlayerId && data.victimId == m_ownPlayerId) {
        if (m_ownAlive) {
            m_ownAlive = false;
            m_brain.onDied(m_currentNowUs);
        }
    }
}

void LoadBotClient::onBeginRound(const BeginRoundEventData& /*data*/) {
}

void LoadBotClient::onEndOfRound(const EndOfRoundEventData& /*data*/) {
    // Acknowledge end-of-round guard so GameServer::waitForClients does not stall waiting on bots.
    m_codec.events().queueEndOfRoundAck();
}

// ============================================================================
// IGhostSink implementation
// ============================================================================

void LoadBotClient::onControlObjectState(const ControlObjectStateData& data) {
    ++m_packetGhostCount;

    const bool wasAlive = m_ownAlive;
    if (data.isSoldier) {
        m_ownSoldierNetId = data.controlNetId;
        if (data.hasSoldierState) {
            const SoldierGhostState& ss = data.soldierState;
            if (ss.hasPos) {
                m_ownPos = ss.pos;
                m_hasOwnPos = true;
            } else {
                m_ownPos = data.compressionVec;
                m_hasOwnPos = true;
            }
            if (ss.hasFacing) {
                m_ownFacingDegrees = ss.facingDegrees;
            }
            if (ss.hasLookUD) {
                m_ownPitchDegrees = ss.lookUpDownAngle;
            }
            if (ss.hasPose) {
                m_ownPose = ss.pose;
            }
            if (ss.hasHitPoints && ss.hitPointsByte > 0u && !ss.isRagdoll) {
                m_ownAlive = true;
                m_ownManDown = false;
            }
        } else {
            m_ownPos = data.compressionVec;
            m_hasOwnPos = true;
        }

        if (!wasAlive && m_ownAlive && m_hasOwnPos) {
            m_brain.onSpawned(m_ownPos, m_currentNowUs);
        }
    }
}

void LoadBotClient::onPlayerGhost(const PlayerGhostState& data) {
    ++m_packetGhostCount;

    uint8_t pid = data.playerId;
    if (pid == 0u) {
        std::map<uint16_t, uint8_t>::const_iterator it = m_playerNetIdToPlayerId.find(data.netId);
        if (it != m_playerNetIdToPlayerId.end()) {
            pid = it->second;
        }
    }

    TrackedPlayer& p = m_players[pid];
    p.playerId = pid;
    p.playerNetId = data.netId;
    m_playerNetIdToPlayerId[data.netId] = pid;

    if (data.hasTeam) {
        p.team = data.team;
        // Propagate updated team to this player's soldier if tracked.
        std::map<uint8_t, uint16_t>::const_iterator sit = m_playerToSoldierNetId.find(pid);
        if (sit != m_playerToSoldierNetId.end()) {
            std::map<uint16_t, TrackedSoldier>::iterator sIt = m_soldiers.find(sit->second);
            if (sIt != m_soldiers.end()) {
                sIt->second.team = data.team;
            }
        }
    }
    if (data.hasIsAlive) {
        p.isAlive = data.isAlive;
    }
    if (data.hasSpawnAtTick) {
        p.isManDown = data.isManDown;
    }
    if (data.hasSpawnGroup) {
        p.spawnGroupId = static_cast<uint8_t>(data.spawnGroup);
    }
    if (data.hasKit) {
        p.kit = data.kit;
    }

    if (m_hasOwnPlayerId && pid == m_ownPlayerId) {
        if (data.hasTeam && data.team != 0) {
            m_ownTeam = data.team;
            m_brain.setTeam(m_ownTeam);
        }
        const bool prevAlive = m_ownAlive && !m_ownManDown;
        if (data.hasIsAlive) {
            m_ownAlive = data.isAlive;
        }
        if (data.hasSpawnAtTick) {
            m_ownManDown = data.isManDown;
        }
        const bool nowAlive = m_ownAlive && !m_ownManDown;
        if (prevAlive && !nowAlive) {
            m_brain.onDied(m_currentNowUs);
        } else if (!prevAlive && nowAlive && m_hasOwnPos) {
            m_brain.onSpawned(m_ownPos, m_currentNowUs);
        }
    }
}

void LoadBotClient::onSoldierGhost(const SoldierGhostState& data) {
    ++m_packetGhostCount;

    TrackedSoldier& s = m_soldiers[data.netId];
    s.netId = data.netId;
    s.lastUpdateUs = m_currentNowUs;

    if (data.hasPos) {
        s.pos = data.pos;
        s.hasPos = true;
    }
    if (data.hasSpeed) {
        s.speed = data.speed;
    }
    if (data.hasFacing) {
        s.facingDegrees = data.facingDegrees;
    }
    if (data.hasLookUD) {
        s.lookUpDownAngle = data.lookUpDownAngle;
    }
    if (data.hasPose) {
        s.pose = data.pose;
    }
    // Ragdoll flag (mask bit 0x8000) can be set independently of pose (0x20).
    s.isRagdoll = data.isRagdoll;
    if (data.hasHitPoints) {
        s.hitPointsByte = data.hitPointsByte;
    }

    if (s.team == 0) {
        std::map<uint16_t, uint8_t>::const_iterator pIt = m_soldierNetIdToPlayerId.find(data.netId);
        if (pIt != m_soldierNetIdToPlayerId.end()) {
            s.ownerPlayerId = pIt->second;
            std::map<uint8_t, TrackedPlayer>::const_iterator plIt = m_players.find(pIt->second);
            if (plIt != m_players.end()) {
                s.team = plIt->second.team;
            }
        }
    }

    // Feed enemy soldier update interval into Telemetry for sliding/extrapolation detection.
    if (m_telemetry && m_hasOwnPos && s.hasPos &&
        data.netId != m_ownSoldierNetId &&
        s.team != 0 && s.team != m_ownTeam) {
        const float dx = s.pos.x - m_ownPos.x;
        const float dy = s.pos.y - m_ownPos.y;
        const float dz = s.pos.z - m_ownPos.z;
        const float distMeters = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool alive = (!s.isRagdoll && s.hitPointsByte > 0u);
        m_telemetry->recordEnemySoldierUpdate(static_cast<uint8_t>(m_config.botIndex),
                                              data.netId, m_currentNowUs, distMeters, alive);
    }
}

void LoadBotClient::onSpawnGroupGhost(const SpawnGroupGhostState& data) {
    ++m_packetGhostCount;

    std::map<uint16_t, uint8_t>::const_iterator idIt = m_spawnGroupNetIdToId.find(data.netId);
    if (idIt == m_spawnGroupNetIdToId.end()) {
        return;
    }
    TrackedSpawnGroup& sg = m_spawnGroups[idIt->second];
    if (data.hasSpawnPointsField) {
        sg.hasSpawnPoints = data.hasSpawnPoints;
    }
    if (data.hasTeam) {
        sg.team = data.team;
    }
    if (data.hasActive) {
        sg.active = data.active;
    }
    if (data.hasOnlyForAI) {
        sg.aiOnly = data.onlyForAI;
    }
    if (data.hasPos) {
        sg.pos = data.pos;
        sg.hasPos = true;
    }
}

void LoadBotClient::onControlPointGhost(const ControlPointGhostState& data) {
    ++m_packetGhostCount;

    TrackedControlPoint& cp = m_controlPoints[data.netId];
    cp.netId = data.netId;
    if (data.hasTeam) {
        cp.team = data.team;
    }
    if (data.hasIsNeutral) {
        cp.isNeutral = data.isNeutral;
    }
    if (data.hasAllowTeam1) {
        cp.allowCaptureByTeam1 = data.allowCaptureByTeam1;
    }
    if (data.hasAllowTeam2) {
        cp.allowCaptureByTeam2 = data.allowCaptureByTeam2;
    }
    if (!cp.hasPos) {
        std::map<uint16_t, Vec3>::const_iterator posIt = m_objectPositions.find(data.netId);
        if (posIt != m_objectPositions.end()) {
            cp.pos = posIt->second;
            cp.hasPos = true;
        }
    }
}

void LoadBotClient::onGhostRemoved(uint16_t netId) {
    m_soldiers.erase(netId);
}

} // namespace bf2bot
