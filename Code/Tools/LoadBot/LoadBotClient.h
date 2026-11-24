#ifndef BF2BOT_LOAD_BOT_CLIENT_H
#define BF2BOT_LOAD_BOT_CLIENT_H

#include "Bf2Protocol.h"
#include "BotBrain.h"
#include "NavMesh.h"
#include "NetImpairment.h"
#include "RouteLoader.h"
#include "SWTransport.h"
#include "Telemetry.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace bf2bot {

#ifdef _WIN32
typedef SOCKET BotSocket;
static const BotSocket kInvalidBotSocket = INVALID_SOCKET;
#else
typedef int BotSocket;
static const BotSocket kInvalidBotSocket = -1;
#endif

struct LoadBotConfig {
    uint32_t botIndex;
    std::string modName;
    std::string password;
    float worldSize;
    uint32_t seed;
    uint64_t startAtUs;
    ImpairmentConfig uplinkImpair;
    ImpairmentConfig downlinkImpair;
    AimConfig aim;
    int32_t forceTeam;   // 0 = alternate by bot index, otherwise every bot requests this team
    bool verbose;

    LoadBotConfig()
        : botIndex(0),
          forceTeam(0),
          modName("mods/bf2"),
          password(),
          worldSize(1024.0f),
          seed(1337u),
          startAtUs(0),
          uplinkImpair(),
          downlinkImpair(),
          verbose(false) {
        uplinkImpair.oneWayDelayMs = 0.0;
        uplinkImpair.jitterMs = 0.0;
        uplinkImpair.lossPct = 0.0;
        uplinkImpair.seed = 1u;
        downlinkImpair.oneWayDelayMs = 0.0;
        downlinkImpair.jitterMs = 0.0;
        downlinkImpair.lossPct = 0.0;
        downlinkImpair.seed = 2u;
    }
};

enum BotConnState {
    BOT_DISCONNECTED = 0,
    BOT_CONNECTING   = 1,
    BOT_HANDSHAKING  = 2,
    BOT_IN_GAME      = 3,
    BOT_STOPPED      = 4
};

class LoadBotClient : public IGameEventSink, public IGhostSink {
public:
    LoadBotClient(const LoadBotConfig& cfg,
                  const sockaddr_in& serverAddr,
                  const NavMesh* navMesh,
                  const RouteLoader* routeLoader,
                  Telemetry* telemetry);
    ~LoadBotClient();

    bool initSocket();
    void closeSocket();
    BotSocket socketFd() const { return m_sockFd; }

    BotConnState state() const { return m_state; }
    bool isConnected() const { return m_state == BOT_HANDSHAKING || m_state == BOT_IN_GAME; }
    bool isInGame() const { return m_state == BOT_IN_GAME; }
    bool isSpawned() const { return m_state == BOT_IN_GAME && m_hasOwnPos; }
    bool isAlive() const { return m_state == BOT_IN_GAME && m_ownAlive && !m_ownManDown; }
    uint32_t botIndex() const { return m_config.botIndex; }
    const std::string& playerName() const { return m_playerName; }
    float smoothedRttMs() const { return m_conn.smoothedRttMs(); }

    // Drains all pending UDP datagrams from the non-blocking socket.
    void onSocketReadable(uint64_t nowUs);

    // Optional in-memory datagram transport hooks for deterministic unit/integration tests.
    typedef std::function<void(const uint8_t*, size_t)> RawSendCallback;
    void setRawSendHook(const RawSendCallback& cb) { m_rawSendHook = cb; }
    void injectRawDatagram(const uint8_t* data, size_t len, uint64_t nowUs);

    // Flushes ready uplink/downlink impairment queue packets.
    void flushImpairmentQueues(uint64_t nowUs);

    // Earliest timestamp when either impairment queue has a packet due (or UINT64_MAX).
    uint64_t nextWakeUs() const;

    // Called at 30 Hz from the runner event loop.
    void tick30Hz(uint64_t nowUs, float dtSec);

    // Sends 3x UDP_DISCONNECT directly and marks bot stopped.
    void disconnectGracefully();

    // IGameEventSink overrides
    void onChallenge(const ChallengeEventData& data) override;
    void onServerInfo(const ServerInfoData& data) override;
    void onMapInfo(const MapInfoData& data) override;
    void onCreatePlayer(const CreatePlayerEventData& data) override;
    void onDestroyPlayer(uint8_t playerId) override;
    void onCreateObject(const CreateObjectEventData& data) override;
    void onDestroyObject(uint16_t netId) override;
    void onEnterVehicle(const EnterVehicleEventData& data) override;
    void onExitVehicle(const ExitVehicleEventData& data) override;
    void onCreateSpawnGroup(const CreateSpawnGroupEventData& data) override;
    void onRemoveSpawnGroup(const RemoveSpawnGroupEventData& data) override;
    void onPostRemoteEvent(const PostRemoteEventData& data) override;
    void onKilledBy(const KilledByEventData& data) override;
    void onBeginRound(const BeginRoundEventData& data) override;
    void onEndOfRound(const EndOfRoundEventData& data) override;

    // IGhostSink overrides
    void onControlObjectState(const ControlObjectStateData& data) override;
    void onPlayerGhost(const PlayerGhostState& data) override;
    void onSoldierGhost(const SoldierGhostState& data) override;
    void onSpawnGroupGhost(const SpawnGroupGhostState& data) override;
    void onControlPointGhost(const ControlPointGhostState& data) override;
    void onGhostRemoved(uint16_t netId) override;

private:
    LoadBotClient(const LoadBotClient&);
    LoadBotClient& operator=(const LoadBotClient&);

    void resetSession(uint64_t reconnectAtUs);
    void sendRawDatagram(const uint8_t* data, size_t len, uint64_t nowUs);
    void sendDirectSocket(const uint8_t* data, size_t len);
    void processIncomingDatagram(const uint8_t* data, size_t len, uint64_t nowUs);
    uint32_t localTimeMs(uint64_t nowUs) const;
    static uint32_t djb2LowerName(const std::string& s);
    Vec3 dequantizeSpawnGroupPos(uint8_t quantX, uint8_t quantZ) const;

    LoadBotConfig m_config;
    sockaddr_in m_serverAddr;
    BotSocket m_sockFd;
    RawSendCallback m_rawSendHook;
    std::string m_playerName;
    int32_t m_profileId;

    BotConnState m_state;
    uint64_t m_originUs;
    uint64_t m_currentNowUs;
    uint64_t m_nextConnectTryUs;
    uint64_t m_nextPingUs;
    uint64_t m_lastRxUs;
    uint64_t m_lastDataPacketRxUs;
    uint32_t m_nextDeliveryId;
    uint32_t m_packetGhostCount;
    uint32_t m_unackedDataPackets;

    bool m_useContentChecking;
    bool m_sentInitialJoin;

    SWConnectionState m_conn;
    ClientPacketCodec m_codec;
    ImpairmentQueue m_uplinkImpair;
    ImpairmentQueue m_downlinkImpair;
    BotBrain m_brain;
    Telemetry* m_telemetry;

    // World state tables
    bool m_hasOwnPlayerId;
    uint8_t m_ownPlayerId;
    int32_t m_ownTeam;
    bool m_ownAlive;
    bool m_ownManDown;
    uint16_t m_ownSoldierNetId;
    bool m_hasOwnPos;
    Vec3 m_ownPos;
    float m_ownFacingDegrees;
    float m_ownPitchDegrees;
    uint8_t m_ownPose;

    std::map<uint8_t, TrackedPlayer> m_players;
    std::map<uint16_t, uint8_t> m_playerNetIdToPlayerId;
    std::map<uint16_t, TrackedSoldier> m_soldiers;
    std::map<uint8_t, TrackedSpawnGroup> m_spawnGroups;
    std::map<uint16_t, uint8_t> m_spawnGroupNetIdToId;
    std::map<uint16_t, TrackedControlPoint> m_controlPoints;
    std::map<uint8_t, uint16_t> m_playerToSoldierNetId;
    std::map<uint16_t, uint8_t> m_soldierNetIdToPlayerId;
    std::map<uint16_t, Vec3> m_objectPositions;
};

} // namespace bf2bot

#endif // BF2BOT_LOAD_BOT_CLIENT_H
