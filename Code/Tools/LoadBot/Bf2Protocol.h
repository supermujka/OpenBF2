#ifndef BF2BOT_BF2_PROTOCOL_H
#define BF2BOT_BF2_PROTOCOL_H

#include "SWTransport.h"
#include "WireBitStream.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf2bot {

// ============================================================================
// Constants & Enums (BF2 v1.50)
// ============================================================================

static const uint32_t kGameEventTypeBits         = 7u;
static const int32_t  kGameIdBf2                 = 1059;
static const uint32_t kMaxUnconfirmedEventBlocks = 15u;
static const uint32_t kDataBlockMaxChunkSize     = 63u;

// PlayerAction buttonMaskLow bit flags (idx 8..39 -> bit = idx - 8)
static const uint32_t BTN_FIRE               = 0x00000001u; // b0  PIFire=8
static const uint32_t BTN_JUMP               = 0x00000002u; // b1  PIAction=9
static const uint32_t BTN_USE                = 0x00000004u; // b2  PIUse=10
static const uint32_t BTN_MOUSE_LOOK         = 0x00000008u; // b3  PIMouseLook=11
static const uint32_t BTN_ALT_SPRINT         = 0x00000010u; // b4  PIAltSprint=12
static const uint32_t BTN_SPRINT             = 0x00000020u; // b5  PISprint=13
static const uint32_t BTN_WEAPON_1           = 0x00000040u; // b6  PIWeaponSelect1=14
static const uint32_t BTN_WEAPON_2           = 0x00000080u; // b7  PIWeaponSelect2=15
static const uint32_t BTN_WEAPON_3           = 0x00000100u; // b8  PIWeaponSelect3=16
static const uint32_t BTN_WEAPON_4           = 0x00000200u; // b9  PIWeaponSelect4=17
static const uint32_t BTN_WEAPON_5           = 0x00000400u; // b10 PIWeaponSelect5=18
static const uint32_t BTN_ALT_FIRE           = 0x00800000u; // b23 PIAltFire=31
static const uint32_t BTN_RELOAD             = 0x01000000u; // b24 PIReload=32
static const uint32_t BTN_PRONE              = 0x20000000u; // b29 PILie=37
static const uint32_t BTN_CROUCH             = 0x40000000u; // b30 PICrouch=38

// PlayerAction buttonMaskHigh bit flags (9 bits: idx 40..48 -> bit = idx - 40)
static const uint32_t BTNH_PRIM_WEAPON       = 0x00000008u; // b3  PISelectPrimWeapon=43
static const uint32_t BTNH_SEC_WEAPON        = 0x00000010u; // b4  PISelectSecWeapon=44
static const uint32_t BTNH_TOGGLE_WEAPON     = 0x00000020u; // b5  PIToggleWeapon=45
static const uint32_t BTNH_TOGGLE_FIRE_RATE  = 0x00000040u; // b6  PIToggleFireRate=46
static const uint32_t BTNH_FLARE_FIRE        = 0x00000080u; // b7  PIFlareFire=47

enum GameEventId {
    GE_CHALLENGE               = 1,
    GE_CHALLENGE_RESPONSE      = 2,
    GE_CONNECTION_TYPE         = 3,
    GE_DATA_BLOCK              = 4,
    GE_CREATE_PLAYER           = 5,
    GE_CREATE_OBJECT           = 6,
    GE_DESTROY_OBJECT          = 7,
    GE_DESTROY_PLAYER          = 8,
    GE_ENTER_VEHICLE           = 9,
    GE_EXIT_VEHICLE            = 10,
    GE_POST_REMOTE             = 11,
    GE_CHANGE_PLAYER_NAME      = 12,
    GE_HANDLE_DROP             = 13,
    GE_HANDLE_PICKUP           = 14,
    GE_STRING_BLOCK            = 15,
    GE_JOIN_SQUAD              = 16,
    GE_LEAVE_SQUAD             = 17,
    GE_COMMANDER               = 19,
    GE_RADIO_MESSAGE           = 20,
    GE_KILLED_BY               = 21,
    GE_CHANGE_SQUAD_NAME       = 22,
    GE_SET_PRIVATE_SQUAD       = 23,
    GE_ISSUE_SQUAD_ORDER       = 24,
    GE_INVITE                  = 25,
    GE_RANK                    = 26,
    GE_SET_ACCEPT_ORDER        = 27,
    GE_SET_SQUAD_LEADER        = 28,
    GE_SPOTTED                 = 29,
    GE_ARTILLERY               = 30,
    GE_CREATE_KIT              = 31,
    GE_STICKY_PROJECTILE       = 32,
    GE_AMBIENT_EFFECT_AREA     = 34,
    GE_VOIP_ON_OFF             = 35,
    GE_COMMANDER_CAM           = 36,
    GE_SUPPLY_DROP             = 37,
    GE_VOIP_PLAYER_MUTE        = 38,
    GE_VOTE                    = 39,
    GE_TOGGLE_FREE_CAMERA      = 40,
    GE_MEDAL                   = 41,
    GE_UNLOCK                  = 42,
    GE_MISSILE_INIT            = 43,
    GE_UAV                     = 45,
    GE_CONTENT_CHECK           = 46,
    GE_TARGET_DIRECTION        = 47,
    GE_END_OF_ROUND            = 48,
    GE_PYTHON_COMMAND          = 49,
    GE_REQUEST                 = 50,
    GE_DROP_VEHICLE            = 51,
    GE_VOIP_SESSION            = 54,
    GE_KICK_BAN                = 55,
    GE_BEGIN_ROUND             = 56,
    GE_CREATE_SPAWN_GROUP      = 57,
    GE_REMOVE_SPAWN_GROUP      = 58,
    GE_UPDATE_TRIGGER          = 59,
    GE_GRAPPLE_CONTAINER_CREATE = 60,
    GE_GRAPPLE_CONTAINER_UPDATE = 61,
    GE_GRAPPLE_CONTAINER_DETACH = 62,
    GE_GRAPPLE_CREATE          = 63,
    GE_GRAPPLE_UPDATE          = 64,
    GE_PLAYER_TEAR_GASSED      = 65,
    GE_SET_NIGHT_VISION        = 66,
    GE_VERIFY_PLAYER_TEAM      = 68,
    GE_FIX_PLAYER_TEAM         = 69
};

enum EventCategory {
    EC_GAME_LOGIC = 0,
    EC_NETWORK    = 6
};

enum NetworkRemoteEventId {
    NRE_MAP_LOADED         = 2,
    NRE_CLIENT_READY_ACK   = 3,
    NRE_CLIENT_READY       = 4,
    NRE_SELECT_SPAWN_POINT = 6,
    NRE_JOIN_TEAM          = 7,
    NRE_SELECT_KIT         = 8,
    NRE_SUICIDE            = 12
};

enum DataBlockId {
    DB_SERVER_INFO = 0,
    DB_CLIENT_INFO = 1,
    DB_MAP_INFO    = 2,
    DB_MAP_LIST    = 5,
    DB_PYTHON      = 6
};

// ============================================================================
// 1. PlayerActionInput & PlayerActionSender (Stream Manager ID 0)
// ============================================================================

struct PlayerActionInput {
    float axes[6];           // 0=strafe/yaw, 1=pitch, 2=roll, 3=forward, 4=mouseX, 5=mouseY
    uint32_t buttonMaskLow;  // 32 bits (inputs 8..39)
    uint32_t buttonMaskHigh; // 9 bits (inputs 40..48)
    bool flag;
    int32_t inputIndex;

    PlayerActionInput()
        : buttonMaskLow(0), buttonMaskHigh(0), flag(false), inputIndex(0) {
        for (int i = 0; i < 6; ++i) {
            axes[i] = 0.0f;
        }
    }
};

class PlayerActionSender {
public:
    PlayerActionSender();

    void reset();
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }
    void setInterpolationTimeMs(uint32_t ms) { m_interpTimeMs = ms & 0x1FFu; }
    uint32_t interpolationTimeMs() const { return m_interpTimeMs; }
    int32_t nextInputIndex() const { return m_nextInputIndex; }

    // Pushes a new input tick into the 3-slot sliding resend window.
    // If action.inputIndex <= 0, assigns m_nextInputIndex++.
    int32_t pushAction(const PlayerActionInput& action);

    // Serializes Stream Manager 0 (PlayerActionManager) into bs.
    // Returns true if actions were written (present == 1), false if empty (present == 0).
    bool transmit(BitStream& bs) const;

    // Reads/skips incoming Stream Manager 0 on S->C packets (normally 1-bit 0).
    static bool skipIncoming(BitStream& bs);

private:
    struct WireAction {
        int16_t axes[6];
        uint32_t buttonMaskLow;
        uint32_t buttonMaskHigh;
        bool flag;
        int32_t inputIndex;
    };

    bool m_enabled;
    uint32_t m_interpTimeMs;
    int32_t m_nextInputIndex;
    std::deque<WireAction> m_window; // Max 3 slots
};

// ============================================================================
// 2. GameEvent Structs, DataBlockReceiver, IGameEventSink, GameEventStream
// ============================================================================

struct ChallengeEventData {
    uint8_t challenge[10];
    std::string modName;
};

struct ServerInfoData {
    std::string gameName;
    std::vector<std::string> welcomeLines;
    bool requestResponse;
    bool useContentChecking;

    ServerInfoData() : requestResponse(false), useContentChecking(false) {}
};

struct MapInfoData {
    std::string gameMode;
    std::string levelPath;
    std::string mapName;
    int32_t maxPlayers;
    bool commanderEnabled;
    int32_t challengeOrdinal;

    MapInfoData() : maxPlayers(0), commanderEnabled(false), challengeOrdinal(0) {}
};

struct CreatePlayerEventData {
    uint8_t teamId;
    uint8_t spawnGroupId;
    bool isRemote;
    uint8_t playerId;
    uint16_t playerNetId;
    uint16_t word34;
    bool isAI;
    std::string name;

    CreatePlayerEventData()
        : teamId(0), spawnGroupId(0), isRemote(false), playerId(0),
          playerNetId(0), word34(0), isAI(false) {}
};

struct CreateObjectEventData {
    uint32_t templateId;
    uint16_t netId;
    uint8_t defaultTeam;
    bool hasExplicitMode;
    uint8_t mode;
    bool hasPos;
    Vec3 pos;
    bool hasRot;
    Vec3 rot;

    CreateObjectEventData()
        : templateId(0), netId(0), defaultTeam(0), hasExplicitMode(false),
          mode(0), hasPos(false), hasRot(false) {}
};

struct EnterVehicleEventData {
    uint8_t playerId;
    uint16_t netId;
    bool setAsDefaultVehicle;

    EnterVehicleEventData() : playerId(0), netId(0), setAsDefaultVehicle(false) {}
};

struct ExitVehicleEventData {
    uint8_t playerId;
    bool field0D;

    ExitVehicleEventData() : playerId(0), field0D(false) {}
};

struct CreateSpawnGroupEventData {
    uint8_t spawnGroupId;
    uint8_t team;
    bool active;
    bool aiOnly;
    bool hasSpawnPoints;
    uint8_t quantX;
    uint8_t quantZ;
    uint16_t netId;

    CreateSpawnGroupEventData()
        : spawnGroupId(0), team(0), active(false), aiOnly(false),
          hasSpawnPoints(false), quantX(0), quantZ(0), netId(0) {}
};

struct RemoveSpawnGroupEventData {
    uint8_t spawnGroupId;
    uint16_t netId;

    RemoveSpawnGroupEventData() : spawnGroupId(0), netId(0) {}
};

struct PostRemoteEventData {
    uint8_t category;
    uint32_t eventId;
    float delay;
    std::vector<uint8_t> payload;

    PostRemoteEventData() : category(0), eventId(0), delay(0.0f) {}
};

struct KilledByEventData {
    uint8_t killerId;
    uint8_t victimId;
    bool killerAlive;
    uint16_t field04;
    uint32_t objTemplateId;
    uint32_t vehTemplateId;
    uint8_t lastHitBoneIdx;
    Vec3 lastHitDir;

    KilledByEventData()
        : killerId(0), victimId(0), killerAlive(false), field04(0),
          objTemplateId(0), vehTemplateId(0), lastHitBoneIdx(0) {}
};

struct BeginRoundEventData {
    uint32_t state;
    uint32_t nextStateChangeAtTick;

    BeginRoundEventData() : state(0), nextStateChangeAtTick(0) {}
};

struct EndOfRoundEventData {
    uint32_t roundId;
    uint32_t winnerTeamId;
    uint32_t victoryType;

    EndOfRoundEventData() : roundId(0), winnerTeamId(0), victoryType(0) {}
};

class IGameEventSink {
public:
    virtual ~IGameEventSink() {}
    virtual void onChallenge(const ChallengeEventData&) {}
    virtual void onServerInfo(const ServerInfoData&) {}
    virtual void onMapInfo(const MapInfoData&) {}
    virtual void onCreatePlayer(const CreatePlayerEventData&) {}
    virtual void onDestroyPlayer(uint8_t) {}
    virtual void onCreateObject(const CreateObjectEventData&) {}
    virtual void onDestroyObject(uint16_t) {}
    virtual void onEnterVehicle(const EnterVehicleEventData&) {}
    virtual void onExitVehicle(const ExitVehicleEventData&) {}
    virtual void onCreateSpawnGroup(const CreateSpawnGroupEventData&) {}
    virtual void onRemoveSpawnGroup(const RemoveSpawnGroupEventData&) {}
    virtual void onPostRemoteEvent(const PostRemoteEventData&) {}
    virtual void onKilledBy(const KilledByEventData&) {}
    virtual void onBeginRound(const BeginRoundEventData&) {}
    virtual void onEndOfRound(const EndOfRoundEventData&) {}
};

class DataBlockReceiver {
public:
    DataBlockReceiver();
    void reset();

    void onNewBlock(uint32_t blockEventId, uint32_t blockLength);
    void onChunk(const uint8_t* data, uint8_t size, IGameEventSink* sink);

    static bool decodeServerInfo(const uint8_t* buf, size_t len, ServerInfoData& out);
    static bool decodeMapInfo(const uint8_t* buf, size_t len, MapInfoData& out);

private:
    bool m_active;
    uint32_t m_blockEventId;
    uint32_t m_expectedLength;
    std::vector<uint8_t> m_buffer;
};

class GameEventStream {
public:
    GameEventStream();
    void reset();

    // Outgoing event builders (C->S).
    void queueChallengeResponse(const uint8_t response73[73],
                                uint32_t field58 = 0,
                                uint32_t netVersion = kBf2BuildVersion,
                                int32_t gameId = kGameIdBf2);

    void queueDataBlock(uint32_t blockEventId, const uint8_t* payload, uint32_t payloadLen);

    void queueClientInfo(const std::string& playerName,
                         uint32_t playerNameHash,
                         int32_t profileId,
                         const std::string& clanTag = "",
                         const std::string& gameSpyAuth = "",
                         bool usesVoip = false);

    void queuePostRemoteEvent(uint8_t category, uint32_t eventId,
                              const void* payload = nullptr,
                              uint8_t payloadLen = 0,
                              float delay = 0.0f);

    void queueMapLoaded();
    void queueClientReady();
    void queueJoinTeam(int32_t team);
    void queueSelectKit(int32_t kit, int32_t unlockLevel = 0);
    void queueSelectSpawnPoint(int32_t spawnGroupId);
    void queueSuicide();

    void queueContentCheck(const uint8_t miscHash[16],
                           const uint8_t archiveHash[16],
                           const uint8_t levelHash[16]);

    void queueConnectionType(uint32_t connectionType = 5);

    bool hasPendingOutgoing() const { return !m_resendBlocks.empty() || !m_sendQueue.empty(); }
    uint32_t numUnconfirmedBlocks() const { return m_nextSendBlockIndex - m_confirmedTotal; }

    // Writes Stream Manager 1 (GameEventManager) into bs.
    // Returns true if an event block was written (present == 1).
    bool transmit(BitStream& bs, uint32_t deliveryId, uint32_t maxBitBudget = 0);

    // Driven by SWConnectionState::handleRemoteAck.
    void onPacketStatus(uint32_t deliveryId, bool delivered);

    // Parses incoming Stream Manager 1 (GameEventManager) and dispatches in-order blocks.
    // Returns false on malformed/unregistered event or duplicate pending blockIndex.
    bool processReceivedPacket(BitStream& bs, IGameEventSink* sink);

private:
    struct SerializedEvent {
        std::vector<uint8_t> bytes;
        uint32_t bitLen;
        SerializedEvent() : bitLen(0) {}
    };

    struct EventBlock {
        uint32_t blockIndex;
        std::vector<SerializedEvent> events;
        EventBlock() : blockIndex(0) {}
    };

    struct InFlightBlock {
        uint32_t deliveryId;
        uint32_t blockIndex;
        std::vector<SerializedEvent> events;
        InFlightBlock() : deliveryId(0), blockIndex(0) {}
    };

    struct ParsedEvent {
        uint32_t typeId;
        // DataBlock fields
        bool dbIsNewBlock;
        uint32_t dbBlockId;
        uint32_t dbBlockLen;
        std::vector<uint8_t> dbChunk;
        // Typed payloads for sink dispatch
        ChallengeEventData challenge;
        CreatePlayerEventData createPlayer;
        uint8_t destroyPlayerId;
        CreateObjectEventData createObject;
        uint16_t destroyObjectNetId;
        EnterVehicleEventData enterVehicle;
        ExitVehicleEventData exitVehicle;
        CreateSpawnGroupEventData createSpawnGroup;
        RemoveSpawnGroupEventData removeSpawnGroup;
        PostRemoteEventData postRemote;
        KilledByEventData killedBy;
        BeginRoundEventData beginRound;
        EndOfRoundEventData endOfRound;

        ParsedEvent()
            : typeId(0), dbIsNewBlock(false), dbBlockId(0), dbBlockLen(0),
              destroyPlayerId(0), destroyObjectNetId(0) {}
    };

    void pushSerializedEvent(const BitStream& evStream);
    static bool parseSingleEvent(BitStream& bs, ParsedEvent& out);
    void dispatchEvent(const ParsedEvent& ev, IGameEventSink* sink);

    std::deque<SerializedEvent> m_sendQueue;
    std::deque<EventBlock> m_resendBlocks;
    std::deque<InFlightBlock> m_inFlightBlocks;
    uint32_t m_nextSendBlockIndex;
    uint32_t m_confirmedTotal;
    bool m_confirmedSlots[32];

    uint32_t m_nextRecvBlockIndex;
    std::map<uint32_t, std::vector<ParsedEvent> > m_pendingRecvBlocks;
    DataBlockReceiver m_dataBlocks;
};

// ============================================================================
// 3. GhostReceiver (Stream Manager ID 2)
// ============================================================================

struct SoldierGhostState {
    uint16_t netId;
    uint32_t mask;
    bool hasHitPoints;
    uint8_t hitPointsByte; // 0..255 (ceil(hp)/maxHp * 255)
    uint8_t lastDamageType;
    bool hasPose;
    uint8_t pose;          // 0=Stand, 1=Crouch, 2=Prone, 3=Swim
    uint8_t requestedPose;
    bool isRagdoll;
    bool hasPos;
    Vec3 pos;
    bool hasSpeed;
    Vec3 speed;
    bool hasFacing;
    float facingDegrees;
    bool hasLookLR;
    float lookLeftRightAngle;
    bool hasPendingTurn;
    float pendingTurn;
    bool hasLookUD;
    float lookUpDownAngle;
    uint8_t weaponZoom;
    uint8_t weaponState;
    bool isSkyDiving;
    bool isSwimming;
    bool weaponStowed;
    bool isGrounded;
    bool hasSprint;
    float sprintStamina;
    bool isSprinting;

    SoldierGhostState()
        : netId(0), mask(0), hasHitPoints(false), hitPointsByte(0), lastDamageType(0),
          hasPose(false), pose(0), requestedPose(0), isRagdoll(false),
          hasPos(false), hasSpeed(false), hasFacing(false), facingDegrees(0.0f),
          hasLookLR(false), lookLeftRightAngle(0.0f), hasPendingTurn(false), pendingTurn(0.0f),
          hasLookUD(false), lookUpDownAngle(0.0f), weaponZoom(0), weaponState(0),
          isSkyDiving(false), isSwimming(false), weaponStowed(false), isGrounded(false),
          hasSprint(false), sprintStamina(0.0f), isSprinting(false) {}
};

struct ControlObjectStateData {
    uint32_t tick;
    int32_t actionBufferEntries;
    Vec3 compressionVec;
    uint16_t controlNetId;
    bool hasParachute;
    uint16_t parachuteNetId;
    bool isSoldier;
    bool hasKit;
    uint16_t kitNetId;
    bool hasSoldierState;
    SoldierGhostState soldierState;

    ControlObjectStateData()
        : tick(0), actionBufferEntries(0), controlNetId(0), hasParachute(false),
          parachuteNetId(0), isSoldier(false), hasKit(false), kitNetId(0),
          hasSoldierState(false) {}
};

struct PlayerGhostState {
    uint16_t netId;
    uint8_t playerId;
    uint32_t mask;
    bool hasIsAlive;
    bool isAlive;
    bool hasSpawnGroup;
    uint32_t spawnGroup;
    bool hasPing;
    uint32_t ping;
    bool hasSpawnAtTick;
    uint32_t spawnAtTick;
    bool isManDown;
    bool hasTeam;
    int32_t team;
    bool hasKit;
    uint8_t kit;
    uint8_t unlockLevel;
    bool hasSquadId;
    uint8_t squadId;
    bool isSquadLeader;
    bool isConnected;

    PlayerGhostState()
        : netId(0), playerId(0), mask(0), hasIsAlive(false), isAlive(false),
          hasSpawnGroup(false), spawnGroup(0), hasPing(false), ping(0),
          hasSpawnAtTick(false), spawnAtTick(0), isManDown(false),
          hasTeam(false), team(0), hasKit(false), kit(0), unlockLevel(0),
          hasSquadId(false), squadId(0), isSquadLeader(false), isConnected(false) {}
};

struct SpawnGroupGhostState {
    uint16_t netId;
    uint32_t mask;
    bool hasSpawnPointsField;
    bool hasSpawnPoints;
    bool hasTeam;
    uint8_t team;
    bool hasActive;
    bool active;
    bool hasOnlyForAI;
    bool onlyForAI;
    bool hasPos;
    uint8_t quantX;
    uint8_t quantZ;
    Vec3 pos;
    bool hasSquad;
    int32_t squad;

    SpawnGroupGhostState()
        : netId(0), mask(0), hasSpawnPointsField(false), hasSpawnPoints(false),
          hasTeam(false), team(0), hasActive(false), active(false),
          hasOnlyForAI(false), onlyForAI(false), hasPos(false),
          quantX(0), quantZ(0), hasSquad(false), squad(-1) {}
};

struct ControlPointGhostState {
    uint16_t netId;
    uint32_t mask;
    bool hasTeam;
    int32_t team;
    bool hasIsNeutral;
    bool isNeutral;
    bool hasTakeOver;
    float takeOverCounter;
    float takeOverChangePerSecond;
    bool hasAllowTeam1;
    bool allowCaptureByTeam1;
    bool hasAllowTeam2;
    bool allowCaptureByTeam2;
    bool hasFlagTeam;
    int32_t flagTeam;
    bool hasCapturingState;
    int32_t capturingState;

    ControlPointGhostState()
        : netId(0), mask(0), hasTeam(false), team(0), hasIsNeutral(false), isNeutral(false),
          hasTakeOver(false), takeOverCounter(0.0f), takeOverChangePerSecond(0.0f),
          hasAllowTeam1(false), allowCaptureByTeam1(false),
          hasAllowTeam2(false), allowCaptureByTeam2(false),
          hasFlagTeam(false), flagTeam(0), hasCapturingState(false), capturingState(0) {}
};

class IGhostSink {
public:
    virtual ~IGhostSink() {}
    virtual void onControlObjectState(const ControlObjectStateData&) {}
    virtual void onPlayerGhost(const PlayerGhostState&) {}
    virtual void onSoldierGhost(const SoldierGhostState&) {}
    virtual void onSpawnGroupGhost(const SpawnGroupGhostState&) {}
    virtual void onControlPointGhost(const ControlPointGhostState&) {}
    virtual void onGhostRemoved(uint16_t) {}
};

class GhostReceiver {
public:
    GhostReceiver();
    void reset();

    void setWorldSize(int worldSizeX, int worldSizeZ);
    void registerSoldierNetId(uint16_t netId);
    void registerControlPointNetId(uint16_t netId);
    void unregisterNetId(uint16_t netId);

    uint32_t lastTick() const { return m_lastTick; }

    // Parses incoming Stream Manager 2 (GhostManager) from S->C packet.
    bool processReceivedPacket(BitStream& bs, IGhostSink* sink);

private:
    bool readControlObjectState(BitStream& bs, uint32_t tick, IGhostSink* sink);
    static void decodeSoldierUpToSprint(BitStream& bs, uint16_t netId, bool isFullPrecision,
                                        SoldierGhostState& out);
    static void decodePlayerGhost(BitStream& bs, uint16_t netId, PlayerGhostState& out);
    void decodeSpawnGroupGhost(BitStream& bs, uint16_t netId, SpawnGroupGhostState& out) const;
    static void decodeControlPointGhost(BitStream& bs, uint16_t netId, ControlPointGhostState& out);

    int m_worldSizeX;
    int m_worldSizeZ;
    uint32_t m_lastTick;
    std::set<uint16_t> m_soldierNetIds;
    std::set<uint16_t> m_controlPointNetIds;
};

// ============================================================================
// 4. ClientPacketCodec (Combines Stream Managers 0, 1, 2 in Exact BF2 Order)
// ============================================================================

class ClientPacketCodec {
public:
    ClientPacketCodec();
    void reset();

    PlayerActionSender& actions() { return m_actions; }
    const PlayerActionSender& actions() const { return m_actions; }

    GameEventStream& events() { return m_events; }
    const GameEventStream& events() const { return m_events; }

    GhostReceiver& ghosts() { return m_ghosts; }
    const GhostReceiver& ghosts() const { return m_ghosts; }

    // Builds a client->server DataPacket payload (managers 0, 1, 2).
    // Returns false if nothing to send (empty packet suppressed like ClientConnection::transmitMsgs).
    bool buildClientPacket(uint32_t deliveryId, uint8_t* outPayload, size_t outCap, size_t& outLen);

    // Parses a server->client DataPacket payload (managers 0, 1, 2).
    // Automatically registers soldier netIds from EnterVehicleEvent / ControlObjectState.
    bool parseServerPacket(const uint8_t* payload, size_t len,
                           IGameEventSink* eventSink, IGhostSink* ghostSink);

    // Forwards transport ack/loss notifications to GameEventStream.
    void onPacketStatus(uint32_t deliveryId, bool delivered);

private:
    PlayerActionSender m_actions;
    GameEventStream m_events;
    GhostReceiver m_ghosts;
};

} // namespace bf2bot

#endif // BF2BOT_BF2_PROTOCOL_H
