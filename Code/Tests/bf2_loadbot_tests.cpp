// Standalone C++14 unit test suite for OpenBF2 Phase 3 (64-player LoadBot & RouteActionRecorder).
//
// Standalone compile command:
//   g++ -std=c++14 -fno-strict-aliasing -pthread -I Code/BF2 -I Code/Tools/LoadBot \
//       Code/Tests/bf2_loadbot_tests.cpp \
//       Code/Tools/LoadBot/WireBitStream.cpp \
//       Code/Tools/LoadBot/SWTransport.cpp \
//       Code/Tools/LoadBot/Bf2Protocol.cpp \
//       Code/Tools/LoadBot/NavMesh.cpp \
//       Code/Tools/LoadBot/RouteLoader.cpp \
//       Code/Tools/LoadBot/NetImpairment.cpp \
//       Code/Tools/LoadBot/Telemetry.cpp \
//       Code/Tools/LoadBot/BotBrain.cpp \
//       Code/Tools/LoadBot/LoadBotClient.cpp \
//       -o /tmp/bf2_loadbot_tests && /tmp/bf2_loadbot_tests

#include "../Tools/LoadBot/LoadBotClient.h"

#include "../BF2/Core/Debug.h"
#include "../BF2/IO/BitStream.h"
#include "../BF2/Game/GameServer/RouteActionRecorder.h"

#include "../Tools/LoadBot/WireBitStream.h"
#include "../Tools/LoadBot/SWTransport.h"
#include "../Tools/LoadBot/Bf2Protocol.h"
#include "../Tools/LoadBot/RouteFormat.h"
#include "../Tools/LoadBot/NavMesh.h"
#include "../Tools/LoadBot/RouteLoader.h"
#include "../Tools/LoadBot/NetImpairment.h"
#include "../Tools/LoadBot/Telemetry.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Minimal dice::hfe::Debug stub required by ../BF2/IO/BitStream.cpp (matches net_pipeline_golden_test.cpp).
namespace dice {
namespace hfe {

Debug::Debug(DebugType type, const std::string& file, int line, const std::string& func)
    : m_type(type)
    , m_file(file)
    , m_func(func)
    , m_line(line)
{
}

Debug::~Debug()
{
}

} // namespace hfe
} // namespace dice

#include "../BF2/IO/BitStream.cpp"

namespace {

int g_passes   = 0;
int g_failures = 0;

void checkCond(bool cond, const char* label)
{
    if (cond)
    {
        ++g_passes;
    }
    else
    {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n", label);
    }
}

bool nearFloat(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

bool nearDouble(double a, double b, double eps)
{
    return std::fabs(a - b) <= eps;
}

void writeU8LE(std::vector<uint8_t>& buf, uint8_t v)
{
    buf.push_back(v);
}

void writeU16LE(std::vector<uint8_t>& buf, uint16_t v)
{
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void writeU32LE(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void writeI32LE(std::vector<uint8_t>& buf, int32_t v)
{
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    writeU32LE(buf, u);
}

void writeF32LE(std::vector<uint8_t>& buf, float v)
{
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    writeU32LE(buf, u);
}

void writeAngle12(bf2bot::BitStream& bs, float angle, float range)
{
    float unit = (angle / range + 1.0f) * 0.5f;
    if (unit < 0.0f) unit = 0.0f;
    if (unit > 1.0f) unit = 1.0f;
    const uint32_t q = static_cast<uint32_t>(unit * 4095.0f + 0.5f);
    bs.writeUInt(q, 12);
}

void writeSoldierPayload(bf2bot::BitStream& bs,
                         bool isFullPrecision,
                         uint8_t hitPointsByte,
                         uint8_t pose,
                         const bf2bot::Vec3& pos,
                         const bf2bot::Vec3& compRef,
                         float facingDeg,
                         float lookUDDeg,
                         bool isGrounded)
{
    const uint32_t mask = 0x40u | 0x20u | 0x01u | 0x02u | 0x10u;
    bs.writeUInt(mask, 21);
    bs.writeUInt(hitPointsByte, 8);
    bs.writeUInt(0u, 1); // lastDamageType
    bs.writeUInt(pose, 3);
    bs.writeUInt(pose, 3);
    bs.writeCompressedVector(pos, compRef, 0.001f);
    if (isFullPrecision)
    {
        bs.writeFloat(facingDeg);
        bs.writeFloat(lookUDDeg);
    }
    else
    {
        writeAngle12(bs, facingDeg, 360.0f);
        writeAngle12(bs, lookUDDeg, 90.0f);
    }
    bs.writeUInt(1u, 3); // weaponZoom
    bs.writeUInt(2u, 2); // weaponState
    bs.writeBool(false); // isSkyDiving
    bs.writeBool(false); // isSwimming
    bs.writeBool(false); // weaponStowed
    bs.writeBool(isGrounded);
}

// ============================================================================
// Test 1: BitStream bit-exact cross-check against dice::hfe::io::BitStream
// ============================================================================
void testBitStreamCrossCheck()
{
    unsigned char diceBuf[1500];
    std::memset(diceBuf, 0, sizeof(diceBuf));
    dice::hfe::io::BitStream diceBs(diceBuf, static_cast<unsigned int>(sizeof(diceBuf)));
    bf2bot::BitStream botBs(1500u);

    const uint64_t v1  = 1u;
    const uint64_t v3  = 5u;
    const uint64_t v7  = 0x5Au;
    const uint64_t v11 = 0x5A3u;
    const uint64_t v16 = 0xBEEFu;
    const uint64_t v32 = 0xCAFEBABEu;
    const float    vf1 = -123.456f;
    const float    vf2 = 9876.5f;

    diceBs.writeBits(&v1, 1);
    botBs.writeUInt(static_cast<uint32_t>(v1), 1);

    diceBs.writeBits(&v3, 3);
    botBs.writeUInt(static_cast<uint32_t>(v3), 3);

    diceBs.writeBits(&v7, 7);
    botBs.writeUInt(static_cast<uint32_t>(v7), 7);

    diceBs.writeBits(&v11, 11);
    botBs.writeUInt(static_cast<uint32_t>(v11), 11);

    diceBs.writeBits(&v16, 16);
    botBs.writeUInt(static_cast<uint32_t>(v16), 16);

    diceBs.writeBits(&v32, 32);
    botBs.writeUInt(static_cast<uint32_t>(v32), 32);

    uint64_t vf1Bits = 0;
    uint64_t vf2Bits = 0;
    std::memcpy(&vf1Bits, &vf1, sizeof(vf1));
    std::memcpy(&vf2Bits, &vf2, sizeof(vf2));

    diceBs.writeBits(&vf1Bits, 32);
    botBs.writeFloat(vf1);

    diceBs.writeBits(&vf2Bits, 32);
    botBs.writeFloat(vf2);

    const dice::hfe::Vec3 diceRef = { 100.0f, 25.0f, -300.0f };
    const bf2bot::Vec3    botRef(100.0f, 25.0f, -300.0f);
    const float           scale = 0.001f;

    // Selector 3 (12-bit delta), Selector 2 (16-bit delta), Selector 1 (20-bit delta), Selector 0 (raw floats)
    struct VecSample {
        float x;
        float y;
        float z;
    };
    const VecSample samples[4] = {
        { 101.234f, 24.500f, -299.125f }, // |d| * 1000 < 2048 -> selector 3
        { 112.000f, 31.000f, -285.000f }, // 2048 <= |d| * 1000 < 32768 -> selector 2
        { 220.000f, 75.000f, -150.000f }, // 32768 <= |d| * 1000 < 524288 -> selector 1
        { 900.000f, 50.000f,  500.000f }  // |d| * 1000 >= 524288 -> selector 0
    };

    for (int i = 0; i < 4; ++i)
    {
        const dice::hfe::Vec3 dv = { samples[i].x, samples[i].y, samples[i].z };
        const bf2bot::Vec3    bv(samples[i].x, samples[i].y, samples[i].z);
        diceBs.writeCompressedVector(dv, diceRef, scale);
        botBs.writeCompressedVector(bv, botRef, scale);
    }

    const unsigned int diceBits = diceBs.getWriteBitPosition();
    const uint32_t     botBits  = botBs.getBitOffset();
    checkCond(diceBits == botBits && diceBits > 0,
              "BitStream cross-check: exact bit count match");

    const uint32_t byteLen = botBs.getByteSize();
    checkCond(byteLen == ((diceBits + 7u) >> 3),
              "BitStream cross-check: exact byte count match");
    checkCond(std::memcmp(diceBuf, botBs.data(), byteLen) == 0,
              "BitStream cross-check: memcmp == 0 byte-for-byte");

    // Round-trip read on both streams
    diceBs.setReadMode();
    bf2bot::BitStream botReader(botBs.data(), byteLen);
    botReader.setBitSize(botBits);

    uint64_t r1 = 0, r3 = 0, r7 = 0, r11 = 0, r16 = 0, r32 = 0;
    float rf1 = 0.0f, rf2 = 0.0f;

    checkCond(diceBs.readBits(&r1, 1) && botReader.readUInt(1) == r1 && r1 == v1,
              "BitStream round-trip 1b");
    checkCond(diceBs.readBits(&r3, 3) && botReader.readUInt(3) == r3 && r3 == v3,
              "BitStream round-trip 3b");
    checkCond(diceBs.readBits(&r7, 7) && botReader.readUInt(7) == r7 && r7 == v7,
              "BitStream round-trip 7b");
    checkCond(diceBs.readBits(&r11, 11) && botReader.readUInt(11) == r11 && r11 == v11,
              "BitStream round-trip 11b");
    checkCond(diceBs.readBits(&r16, 16) && botReader.readUInt(16) == r16 && r16 == v16,
              "BitStream round-trip 16b");
    checkCond(diceBs.readBits(&r32, 32) && botReader.readUInt(32) == r32 && r32 == v32,
              "BitStream round-trip 32b");
    checkCond(diceBs.readBits(&rf1, 32) && botReader.readFloat() == rf1 && rf1 == vf1,
              "BitStream round-trip float1");
    checkCond(diceBs.readBits(&rf2, 32) && botReader.readFloat() == rf2 && rf2 == vf2,
              "BitStream round-trip float2");

    for (int i = 0; i < 4; ++i)
    {
        const dice::hfe::Vec3 dv = diceBs.readCompressedVector(diceRef, scale);
        const bf2bot::Vec3    bv = botReader.readCompressedVector(botRef, scale);
        checkCond(dv.x == bv.x && dv.y == bv.y && dv.z == bv.z,
                  "BitStream round-trip compressed vector exact engine parity");
        checkCond(nearFloat(bv.x, samples[i].x, 0.01f) &&
                  nearFloat(bv.y, samples[i].y, 0.01f) &&
                  nearFloat(bv.z, samples[i].z, 0.01f),
                  "BitStream round-trip compressed vector accuracy");
    }

    checkCond(!diceBs.hasError() && !botReader.hasError() &&
              diceBs.getBitPosition() == diceBits &&
              botReader.getBitOffset() == botBits,
              "BitStream cross-check: final read cursor parity");

    // Additional scalar helpers: signedMag, ranged, stringU16
    bf2bot::BitStream aux(128u);
    aux.writeSignedMag(-777, 16);
    aux.writeSignedMag(12345, 16);
    aux.writeRanged(5u, 6u);
    aux.writeStringU16("OpenBF2");

    bf2bot::BitStream auxR(aux.data(), aux.getByteSize());
    auxR.setBitSize(aux.getBitSize());
    checkCond(auxR.readSignedMag(16) == -777 &&
              auxR.readSignedMag(16) == 12345 &&
              auxR.readRanged(6u) == 5u &&
              auxR.readStringU16() == "OpenBF2" &&
              !auxR.hasError(),
              "BitStream scalar helpers round-trip");
}

// ============================================================================
// Test 2: SWTransport round-trip & sliding window
// ============================================================================
void testSWTransport()
{
    uint8_t buf[256];
    const size_t reqLen = bf2bot::buildConnectRequest(buf, sizeof(buf), "secret", "mods/bf2", "", false);
    checkCond(reqLen == 78u, "SWTransport buildConnectRequest is 78 bytes");

    {
        const uint8_t* const readBuf = buf;
        bf2bot::BitStream bs(readBuf, static_cast<uint32_t>(reqLen));
        const uint32_t type = bs.readUInt(4);
        const uint32_t connId = bs.readUInt(8);
        const uint32_t protoVer = bs.readUInt(32);
        const uint32_t buildVer = bs.readUInt(32);
        const bool pb = bs.readBool();
        char reconnect[4], pass[32], mod[32];
        bs.readFixedBytes(reconnect, 4);
        bs.readFixedBytes(pass, 32);
        bs.readFixedBytes(mod, 32);
        checkCond(type == bf2bot::UDP_CONNECT_REQUEST &&
                  connId == 1u &&
                  protoVer == 4098u &&
                  buildVer == 0x150C5100u &&
                  !pb &&
                  std::strncmp(pass, "secret", 32) == 0 &&
                  std::strncmp(mod, "mods/bf2", 32) == 0 &&
                  !bs.hasError(),
                  "SWTransport ConnectRequest header & version fields");
    }

    const size_t ackLen = bf2bot::buildConnectAcceptAck(buf, sizeof(buf), 9u);
    checkCond(ackLen == 2u, "SWTransport buildConnectAcceptAck is 2 bytes");
    {
        bf2bot::ParsedDatagram dg;
        checkCond(bf2bot::parseIncomingDatagram(buf, ackLen, dg) &&
                  dg.type == bf2bot::UDP_CONNECT_ACCEPT_ACK &&
                  dg.headerConnId == 9u,
                  "SWTransport ConnectAcceptAck parse");
    }

    bf2bot::SWConnectionState conn;
    conn.setAssignedConnId(9u);

    const size_t pingReqLen = bf2bot::buildPingRequest(buf, sizeof(buf), conn, 12345u);
    checkCond(pingReqLen == 12u, "SWTransport buildPingRequest is 12 bytes");
    {
        bf2bot::ParsedDatagram dg;
        checkCond(bf2bot::parseIncomingDatagram(buf, pingReqLen, dg) &&
                  dg.type == bf2bot::UDP_PING_REQUEST &&
                  dg.headerConnId == 9u &&
                  !dg.haveSent &&
                  dg.pingTimestampMs == 12345u,
                  "SWTransport PingRequest parse");
    }

    const size_t pingRespLen = bf2bot::buildPingResponse(buf, sizeof(buf), conn, 12345u, 67890u);
    checkCond(pingRespLen == 16u, "SWTransport buildPingResponse is 16 bytes");
    {
        bf2bot::ParsedDatagram dg;
        checkCond(bf2bot::parseIncomingDatagram(buf, pingRespLen, dg) &&
                  dg.type == bf2bot::UDP_PING_RESPONSE &&
                  dg.headerConnId == 9u &&
                  dg.pingTimestampMs == 12345u &&
                  dg.pingSenderTimestampMs == 67890u,
                  "SWTransport PingResponse parse");
    }

    // PunkBuster pre-filter packet
    const uint8_t pbPacket[] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 'P', 'B', '_', 'X' };
    {
        bf2bot::ParsedDatagram dg;
        checkCond(bf2bot::parseIncomingDatagram(pbPacket, sizeof(pbPacket), dg) &&
                  dg.isFilterPacket,
                  "SWTransport PunkBuster FFFF PB_ filter ignored cleanly");
    }

    // 6-bit wraparound across 63 -> 0 -> 1 and handleRemoteAck callback verification
    // Advance localSeq from 1 up to 62 in batches of 15 with full acks.
    uint8_t pkt[256];
    const uint8_t samplePayload[5] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u };
    for (uint32_t id = 1; id <= 61; ++id)
    {
        size_t outLen = 0;
        const bool ok = conn.wrapDataPacket(samplePayload, sizeof(samplePayload), id, pkt, sizeof(pkt), outLen);
        if (!ok || outLen != bf2bot::kDataHeaderSize + sizeof(samplePayload))
        {
            checkCond(false, "SWTransport wrapDataPacket advance to seq 61");
            return;
        }
        if ((id % 15u) == 0u || id == 61u)
        {
            const uint32_t lastSeq = static_cast<uint32_t>(conn.lastSentSeq());
            conn.handleRemoteAck(lastSeq, 0xFFFFFFFFu, bf2bot::PacketStatusCallback());
        }
    }

    checkCond(conn.localSeq() == 62u && conn.lastAckedSeq() == 61u,
              "SWTransport ready at seq 62 before 63->0->1 wraparound");

    // Send seq 62 (id=100), 63 (id=101), 0 (id=102), 1 (id=103)
    uint32_t wrappedSeqs[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; ++i)
    {
        size_t outLen = 0;
        uint8_t pl[4] = { static_cast<uint8_t>(0xA0 + i), 0x5Au, 0x01u, 0x02u };
        checkCond(conn.wrapDataPacket(pl, sizeof(pl), static_cast<uint32_t>(100 + i), pkt, sizeof(pkt), outLen),
                  "SWTransport wrapDataPacket across 62..1");
        checkCond(outLen == 9u + sizeof(pl), "SWTransport 9-byte DataPacket header");

        bf2bot::ParsedDatagram dg;
        checkCond(bf2bot::parseIncomingDatagram(pkt, outLen, dg) &&
                  dg.type == bf2bot::UDP_DATA_PACKET &&
                  dg.payloadLen == sizeof(pl) &&
                  std::memcmp(dg.payload, pl, sizeof(pl)) == 0,
                  "SWTransport DataPacket parse round-trip");
        wrappedSeqs[i] = dg.seq;
    }

    checkCond(wrappedSeqs[0] == 62u && wrappedSeqs[1] == 63u &&
              wrappedSeqs[2] == 0u && wrappedSeqs[3] == 1u,
              "SWTransport 6-bit seq wraparound 62->63->0->1");

    // Ack seq 62 (bit 3) and seq 0 (bit 1); drop seq 63 (bit 2) and seq 1 (bit 0)
    struct AckResult {
        uint32_t id;
        bool delivered;
    };
    std::vector<AckResult> ackLog;
    const uint32_t ackMask = (1u << 3) | (1u << 1);
    const bool ackOk = conn.handleRemoteAck(1u, ackMask, [&](uint32_t deliveryId, bool delivered) {
        AckResult r;
        r.id = deliveryId;
        r.delivered = delivered;
        ackLog.push_back(r);
    });

    checkCond(ackOk && ackLog.size() == 4u &&
              ackLog[0].id == 100u && ackLog[0].delivered == true &&
              ackLog[1].id == 101u && ackLog[1].delivered == false &&
              ackLog[2].id == 102u && ackLog[2].delivered == true &&
              ackLog[3].id == 103u && ackLog[3].delivered == false,
              "SWTransport handleRemoteAck invokes onPacketStatus for acked & dropped across wraparound");
}

// ============================================================================
// Test 3: PlayerActionManager 144-bit & 3-slot window
// ============================================================================
void testPlayerActionManager()
{
    bf2bot::PlayerActionSender sender;
    sender.setInterpolationTimeMs(123u);

    for (int i = 1; i <= 4; ++i)
    {
        bf2bot::PlayerActionInput act;
        act.axes[0] = (i == 2) ? -0.75f : 0.25f * static_cast<float>(i);
        act.axes[1] = -0.50f * static_cast<float>(i);
        act.axes[2] = 0.0f;
        act.axes[3] = (i == 4) ? 1.0f : -1.0f;
        act.axes[4] = 12.34f;
        act.axes[5] = -56.78f;
        act.buttonMaskLow = bf2bot::BTN_FIRE | bf2bot::BTN_SPRINT | static_cast<uint32_t>(i);
        act.buttonMaskHigh = bf2bot::BTNH_PRIM_WEAPON | static_cast<uint32_t>(i & 0x07);
        act.flag = ((i & 1) == 0);
        const int32_t idx = sender.pushAction(act);
        checkCond(idx == i, "PlayerActionSender sequential inputIndex assignment");
    }

    bf2bot::BitStream bs(256u);
    checkCond(sender.transmit(bs), "PlayerActionSender::transmit wrote actions");
    // 1 (present) + 4 (count) + 9 (interp) + 32 (baseIndex) + 3 * 138 (6*16 + 32 + 9 + 1) = 460 bits
    checkCond(bs.getBitOffset() == 460u,
              "PlayerActionManager exact 3-slot 138-bit wire size (460 bits)");

    // Verify skipIncoming advances to the exact same bit offset as transmit
    {
        bf2bot::BitStream skipReader(bs.data(), bs.getByteSize());
        skipReader.setBitSize(bs.getBitSize());
        checkCond(bf2bot::PlayerActionSender::skipIncoming(skipReader) &&
                  skipReader.getBitOffset() == bs.getBitOffset() &&
                  !skipReader.hasError(),
                  "PlayerActionSender::skipIncoming exact 138-bit/slot parity");
    }

    bf2bot::BitStream reader(bs.data(), bs.getByteSize());
    reader.setBitSize(bs.getBitSize());

    const bool present = reader.readBool();
    const uint32_t count = reader.readUInt(4);
    const uint32_t interp = reader.readUInt(9);
    const int32_t baseIndex = reader.readSignedMag(32);

    checkCond(present && count == 3u && interp == 123u && baseIndex == 2,
              "PlayerActionManager 3-slot sliding window starts at index 2 with 9b interpTimeMs");

    // Verify all 3 slots (indices 2, 3, 4) decode identically
    for (int slot = 0; slot < 3; ++slot)
    {
        const int i = slot + 2;
        int32_t qAxes[6];
        for (int a = 0; a < 6; ++a)
        {
            qAxes[a] = reader.readSignedMag(16);
        }
        const uint32_t btnLo = reader.readUInt(32);
        const uint32_t btnHi = reader.readUInt(9);
        const bool flag = reader.readBool();

        const int32_t exp0 = (i == 2) ? -75 : static_cast<int32_t>(0.25f * static_cast<float>(i) * 100.0f);
        const int32_t exp1 = static_cast<int32_t>(-0.50f * static_cast<float>(i) * 100.0f);
        const int32_t exp3 = (i == 4) ? 100 : -100;
        const int32_t exp4 = static_cast<int32_t>(12.34f * 100.0f);
        const int32_t exp5 = static_cast<int32_t>(-56.78f * 100.0f);

        checkCond(qAxes[0] == exp0 && qAxes[1] == exp1 && qAxes[2] == 0 &&
                  qAxes[3] == exp3 && qAxes[4] == exp4 && qAxes[5] == exp5 &&
                  btnLo == (bf2bot::BTN_FIRE | bf2bot::BTN_SPRINT | static_cast<uint32_t>(i)) &&
                  btnHi == (bf2bot::BTNH_PRIM_WEAPON | static_cast<uint32_t>(i & 0x07)) &&
                  flag == ((i & 1) == 0),
                  "PlayerActionManager slot 144-bit round-trip");
    }

    // Negative baseIndex sign-magnitude check
    bf2bot::BitStream negBs(32u);
    negBs.writeSignedMag(-515, 32);
    bf2bot::BitStream negReader(negBs.data(), negBs.getByteSize());
    negReader.setBitSize(negBs.getBitSize());
    checkCond(negReader.readSignedMag(32) == -515,
              "PlayerActionManager negative baseIndex sign-magnitude round-trip");
}

// ============================================================================
// Test 4: GameEventManager reliable queue + S->C event parsing
// ============================================================================
class RecordingEventSink : public bf2bot::IGameEventSink {
public:
    std::vector<bf2bot::ChallengeEventData> challenges;
    std::vector<bf2bot::ServerInfoData> serverInfos;
    std::vector<bf2bot::MapInfoData> mapInfos;
    std::vector<bf2bot::CreatePlayerEventData> createPlayers;
    std::vector<bf2bot::CreateObjectEventData> createObjects;
    std::vector<bf2bot::EnterVehicleEventData> enterVehicles;
    std::vector<bf2bot::PostRemoteEventData> postRemotes;
    std::vector<bf2bot::KilledByEventData> killedBys;
    std::vector<bf2bot::BeginRoundEventData> beginRounds;
    std::vector<bf2bot::CreateSpawnGroupEventData> spawnGroups;

    void onChallenge(const bf2bot::ChallengeEventData& d) override { challenges.push_back(d); }
    void onServerInfo(const bf2bot::ServerInfoData& d) override { serverInfos.push_back(d); }
    void onMapInfo(const bf2bot::MapInfoData& d) override { mapInfos.push_back(d); }
    void onCreatePlayer(const bf2bot::CreatePlayerEventData& d) override { createPlayers.push_back(d); }
    void onCreateObject(const bf2bot::CreateObjectEventData& d) override { createObjects.push_back(d); }
    void onEnterVehicle(const bf2bot::EnterVehicleEventData& d) override { enterVehicles.push_back(d); }
    void onPostRemoteEvent(const bf2bot::PostRemoteEventData& d) override { postRemotes.push_back(d); }
    void onKilledBy(const bf2bot::KilledByEventData& d) override { killedBys.push_back(d); }
    void onBeginRound(const bf2bot::BeginRoundEventData& d) override { beginRounds.push_back(d); }
    void onCreateSpawnGroup(const bf2bot::CreateSpawnGroupEventData& d) override { spawnGroups.push_back(d); }
};

class RecordingGhostSink : public bf2bot::IGhostSink {
public:
    std::vector<bf2bot::ControlObjectStateData> controlStates;
    std::vector<bf2bot::PlayerGhostState> players;
    std::vector<bf2bot::SpawnGroupGhostState> spawnGroups;
    std::vector<bf2bot::SoldierGhostState> soldiers;
    std::vector<bf2bot::ControlPointGhostState> controlPoints;
    std::vector<uint16_t> removed;

    void onControlObjectState(const bf2bot::ControlObjectStateData& d) override { controlStates.push_back(d); }
    void onPlayerGhost(const bf2bot::PlayerGhostState& d) override { players.push_back(d); }
    void onSpawnGroupGhost(const bf2bot::SpawnGroupGhostState& d) override { spawnGroups.push_back(d); }
    void onSoldierGhost(const bf2bot::SoldierGhostState& d) override { soldiers.push_back(d); }
    void onControlPointGhost(const bf2bot::ControlPointGhostState& d) override { controlPoints.push_back(d); }
    void onGhostRemoved(uint16_t netId) override { removed.push_back(netId); }
};

void buildServerInfoBlock(std::vector<uint8_t>& outBytes)
{
    bf2bot::BitStream bs(512u);
    bs.writeStringU16("OpenBF2 64p Stress Server");
    bs.writeUInt(2u, 16);
    bs.writeStringU16("Welcome to the 64-player loadbot test server — line one exceeds 63 bytes!");
    bs.writeStringU16("Second welcome line for multi-chunk DataBlock reassembly verification.");
    bs.writeBool(true); // requestResponse
    bs.writeUInt(0u, 6); // 6 flags
    bs.writeUInt(0u, 32);
    bs.writeUInt(0u, 16);
    bs.writeUInt(0u, 16);
    bs.writeSignedMag(0, 32);
    bs.writeUInt(16567u, 16);
    bs.writeSignedMag(0, 32);
    bs.writeSignedMag(0, 32);
    bs.writeSignedMag(0, 32);
    bs.writeBool(false); // useContentChecking
    outBytes.assign(bs.data(), bs.data() + bs.getByteSize());
}

void buildMapInfoBlock(std::vector<uint8_t>& outBytes)
{
    bf2bot::BitStream bs(256u);
    bs.writeStringU16("gpm_cq");
    bs.writeStringU16("levels/strike_at_karkand");
    bs.writeStringU16("Strike at Karkand");
    bs.writeSignedMag(64, 32);
    bs.writeBool(true);
    bs.writeSignedMag(7, 32);
    outBytes.assign(bs.data(), bs.data() + bs.getByteSize());
}

void testGameEventManager()
{
    // 4a. Resend on onPacketStatus(id, false) + ClientInfo Block 1 chunking & reassembly
    {
        bf2bot::GameEventStream tx;
        tx.queueClientInfo("LoadBot_Alpha", 0xDEADBEEFu, 4242, "[OBF2]",
                           "long_gamespy_auth_token_payload_exceeding_63_bytes_to_trigger_multi_chunk_transfer",
                           true);

        bf2bot::BitStream pkt1(1458u);
        checkCond(tx.transmit(pkt1, 10u), "GameEventStream::transmit initial ClientInfo");

        tx.onPacketStatus(10u, false);
        checkCond(tx.hasPendingOutgoing(), "GameEventStream queues resend after onPacketStatus(false)");

        bf2bot::BitStream pkt2(1458u);
        checkCond(tx.transmit(pkt2, 11u), "GameEventStream::transmit resend ClientInfo");

        // Inspect resend header & reassemble GE_DATA_BLOCK chunks
        bf2bot::BitStream reader(pkt2.data(), pkt2.getByteSize());
        reader.setBitSize(pkt2.getBitSize());
        const bool present = reader.readBool();
        const uint32_t evCount = reader.readUInt(8);
        const uint32_t blkIdx = reader.readUInt(5);
        const bool isResend = reader.readBool();
        checkCond(present && evCount >= 3u && blkIdx == 0u && isResend,
                  "GameEventStream resend flag, blockIndex=0, and multi-chunk count");

        bool gotHeader = false;
        uint32_t dbId = 0, dbLen = 0;
        std::vector<uint8_t> reassembled;
        for (uint32_t i = 0; i < evCount; ++i)
        {
            const uint32_t typeId = reader.readUInt(bf2bot::kGameEventTypeBits);
            checkCond(typeId == bf2bot::GE_DATA_BLOCK, "ClientInfo emitted GE_DATA_BLOCK events");
            const bool isNew = reader.readBool();
            if (isNew)
            {
                gotHeader = true;
                dbId = reader.readUInt(32);
                dbLen = reader.readUInt(32);
            }
            else
            {
                const uint32_t sz = reader.readUInt(8);
                checkCond(sz <= bf2bot::kDataBlockMaxChunkSize, "DataBlock chunk <= 63 bytes");
                const size_t oldSz = reassembled.size();
                reassembled.resize(oldSz + sz);
                if (sz > 0) reader.readFixedBytes(&reassembled[oldSz], sz);
            }
        }

        checkCond(gotHeader && dbId == bf2bot::DB_CLIENT_INFO && reassembled.size() == dbLen,
                  "ClientInfo Block 1 chunking & exact byte reassembly");

        if (reassembled.size() == dbLen && dbLen > 0)
        {
            const uint8_t* const ciBuf = &reassembled[0];
            bf2bot::BitStream ci(ciBuf, static_cast<uint32_t>(reassembled.size()));
            checkCond(ci.readStringU16() == "LoadBot_Alpha" &&
                      ci.readUInt(32) == 0xDEADBEEFu &&
                      ci.readSignedMag(32) == 4242 &&
                      ci.readStringU16() == "[OBF2]" &&
                      ci.readStringU16() == "long_gamespy_auth_token_payload_exceeding_63_bytes_to_trigger_multi_chunk_transfer" &&
                      ci.readBool() == true &&
                      !ci.hasError(),
                      "ClientInfo Block 1 decoded fields match");
        }

        tx.onPacketStatus(11u, true);
        checkCond(!tx.hasPendingOutgoing() && tx.numUnconfirmedBlocks() == 0u,
                  "GameEventStream clears confirmed block after onPacketStatus(true)");
    }

    // 4b. Out-of-order 5-bit blockIndex reassembly + DataBlock ServerInfo(0) & MapInfo(2)
    {
        std::vector<uint8_t> srvBytes, mapBytes;
        buildServerInfoBlock(srvBytes);
        buildMapInfoBlock(mapBytes);

        bf2bot::GameEventStream sender;
        // Block 0: ServerInfo
        sender.queueDataBlock(bf2bot::DB_SERVER_INFO, &srvBytes[0], static_cast<uint32_t>(srvBytes.size()));
        bf2bot::BitStream pktBlock0(1458u);
        checkCond(sender.transmit(pktBlock0, 1u), "Transmit DataBlock ServerInfo (block 0)");

        // Block 1: MapInfo
        sender.queueDataBlock(bf2bot::DB_MAP_INFO, &mapBytes[0], static_cast<uint32_t>(mapBytes.size()));
        bf2bot::BitStream pktBlock1(1458u);
        checkCond(sender.transmit(pktBlock1, 2u), "Transmit DataBlock MapInfo (block 1)");

        RecordingEventSink sink;
        bf2bot::GameEventStream receiver;

        // Deliver block 1 BEFORE block 0
        bf2bot::BitStream r1(pktBlock1.data(), pktBlock1.getByteSize());
        r1.setBitSize(pktBlock1.getBitSize());
        checkCond(receiver.processReceivedPacket(r1, &sink), "Receive out-of-order block 1");
        checkCond(sink.serverInfos.empty() && sink.mapInfos.empty(),
                  "Out-of-order block 1 held until block 0 arrives");

        // Now deliver block 0 -> both block 0 and block 1 must drain in order
        bf2bot::BitStream r0(pktBlock0.data(), pktBlock0.getByteSize());
        r0.setBitSize(pktBlock0.getBitSize());
        checkCond(receiver.processReceivedPacket(r0, &sink), "Receive block 0");
        checkCond(sink.serverInfos.size() == 1u && sink.mapInfos.size() == 1u,
                  "Out-of-order blocks drained in order upon block 0 arrival");
        if (!sink.serverInfos.empty())
        {
            checkCond(sink.serverInfos[0].gameName == "OpenBF2 64p Stress Server" &&
                      sink.serverInfos[0].welcomeLines.size() == 2u &&
                      sink.serverInfos[0].requestResponse == true &&
                      sink.serverInfos[0].useContentChecking == false,
                      "ServerInfo Block 0 reassembly & decode");
        }
        if (!sink.mapInfos.empty())
        {
            checkCond(sink.mapInfos[0].gameMode == "gpm_cq" &&
                      sink.mapInfos[0].levelPath == "levels/strike_at_karkand" &&
                      sink.mapInfos[0].mapName == "Strike at Karkand" &&
                      sink.mapInfos[0].maxPlayers == 64 &&
                      sink.mapInfos[0].commanderEnabled == true &&
                      sink.mapInfos[0].challengeOrdinal == 7,
                      "MapInfo Block 2 reassembly & decode");
        }
    }

    // 4c. Multi-event S->C packet packing 12 distinct event IDs followed directly by GhostManager
    {
        bf2bot::BitStream pkt(1458u);
        // Stream Manager 0 (PlayerActionManager): present = 0
        pkt.writeBool(false);

        // Stream Manager 1 (GameEventManager): present=1, count=12, blockIndex=0, isResend=0
        pkt.writeBool(true);
        pkt.writeUInt(12u, 8);
        pkt.writeUInt(0u, 5);
        pkt.writeBool(false);

        // 1. ChallengeEvent(1)
        pkt.writeUInt(bf2bot::GE_CHALLENGE, bf2bot::kGameEventTypeBits);
        const uint8_t chal[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        pkt.writeFixedBytes(chal, 10u);
        pkt.writeUInt(8u, 8);
        pkt.writeFixedBytes("mods/bf2", 8u);

        // 2. CreatePlayerEvent(5)
        pkt.writeUInt(bf2bot::GE_CREATE_PLAYER, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(2u, 3);       // teamId
        pkt.writeUInt(3u, 4);       // spawnGroupId
        pkt.writeBool(false);       // isRemote
        pkt.writeUInt(7u, 8);       // playerId
        pkt.writeUInt(8u, 16);      // playerNetId
        pkt.writeUInt(0x1234u, 16); // word34
        pkt.writeBool(false);       // isAI
        char pname[32];
        std::memset(pname, 0, sizeof(pname));
        std::memcpy(pname, "Bot007", 6);
        pkt.writeFixedBytes(pname, 32u);

        // 3. CreateObjectEvent(6)
        pkt.writeUInt(bf2bot::GE_CREATE_OBJECT, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(0xABCD0001u, 32); // templateId
        pkt.writeUInt(650u, 16);        // netId
        pkt.writeUInt(1u, 2);           // defaultTeam
        pkt.writeBool(false);           // hasExplicitMode
        pkt.writeBool(true);            // hasPos
        pkt.writeFloat(100.0f);
        pkt.writeFloat(20.0f);
        pkt.writeFloat(300.0f);
        pkt.writeBool(false);           // hasRot

        // 4. EnterVehicleEvent(9)
        pkt.writeUInt(bf2bot::GE_ENTER_VEHICLE, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(7u, 8);     // playerId
        pkt.writeUInt(700u, 16);  // netId
        pkt.writeBool(true);      // setAsDefaultVehicle

        // 5. PostRemoteEvent(11)
        pkt.writeUInt(bf2bot::GE_POST_REMOTE, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(bf2bot::EC_NETWORK, 4);
        pkt.writeUInt(bf2bot::NRE_CLIENT_READY_ACK, 32);
        pkt.writeFloat(0.25f);
        const uint8_t nrePayload[4] = { 0x0Au, 0x0Bu, 0x0Cu, 0x0Du };
        pkt.writeUInt(4u, 8);
        pkt.writeFixedBytes(nrePayload, 4u);

        // 6. KilledByEvent(21)
        pkt.writeUInt(bf2bot::GE_KILLED_BY, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(9u, 8);
        pkt.writeUInt(7u, 8);
        pkt.writeBool(true);
        pkt.writeUInt(0x0042u, 16);
        pkt.writeUInt(0x11223344u, 32);
        pkt.writeUInt(0x55667788u, 32);
        pkt.writeUInt(3u, 8);
        pkt.writeCompressedVector(bf2bot::Vec3(0.5f, 0.25f, -0.75f), bf2bot::Vec3(), 1000.0f);

        // 7. CreateKitEvent(31) -> 152 payload bits
        pkt.writeUInt(bf2bot::GE_CREATE_KIT, bf2bot::kGameEventTypeBits);
        for (int b = 0; b < 19; ++b) pkt.writeUInt(0xAAu, 8);

        // 8. VoipOnOffEvent(35) -> 9 payload bits
        pkt.writeUInt(bf2bot::GE_VOIP_ON_OFF, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(0x155u, 9);

        // 9. UnlockEvent(42) -> 66 payload bits
        pkt.writeUInt(bf2bot::GE_UNLOCK, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(0u, 2);
        pkt.writeUInt(7u, 8);
        for (int u = 0; u < 14; ++u) pkt.writeUInt(static_cast<uint32_t>(u & 0xF), 4);

        // 10. VoipSessionEvent(54) -> 16 payload bits
        pkt.writeUInt(bf2bot::GE_VOIP_SESSION, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(0xBEEFu, 16);

        // 11. BeginRoundEvent(56)
        pkt.writeUInt(bf2bot::GE_BEGIN_ROUND, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(2u, 32);
        pkt.writeUInt(9000u, 32);

        // 12. CreateSpawnGroupEvent(57)
        pkt.writeUInt(bf2bot::GE_CREATE_SPAWN_GROUP, bf2bot::kGameEventTypeBits);
        pkt.writeUInt(5u, 8);   // spawnGroupId
        pkt.writeUInt(2u, 4);   // team
        pkt.writeBool(true);    // active
        pkt.writeBool(false);   // aiOnly
        pkt.writeBool(true);    // hasSpawnPoints
        pkt.writeUInt(64u, 8);  // quantX
        pkt.writeUInt(192u, 8); // quantZ
        pkt.writeUInt(520u, 16);// netId

        // Stream Manager 2 (GhostManager): starts right after event 12!
        const uint32_t expectedGhostStartBit = pkt.getBitOffset();
        pkt.writeBool(true);         // present
        pkt.writeUInt(7777u, 32);    // tick
        pkt.writeUInt(0u, 8);        // count = 0
        pkt.writeBool(false);        // hasControlState = false

        RecordingEventSink evSink;
        RecordingGhostSink ghSink;
        bf2bot::ClientPacketCodec codec;

        const bool parsed = codec.parseServerPacket(pkt.data(), pkt.getByteSize(), &evSink, &ghSink);
        checkCond(parsed && expectedGhostStartBit > 0,
                  "Multi-event S->C packet with 12 events + GhostManager parsed cleanly");
        checkCond(evSink.challenges.size() == 1u &&
                  evSink.challenges[0].modName == "mods/bf2" &&
                  std::memcmp(evSink.challenges[0].challenge, chal, 10) == 0,
                  "Multi-event decoded ChallengeEvent(1)");
        checkCond(evSink.createPlayers.size() == 1u &&
                  evSink.createPlayers[0].playerId == 7u &&
                  evSink.createPlayers[0].teamId == 2u &&
                  evSink.createPlayers[0].name == "Bot007",
                  "Multi-event decoded CreatePlayerEvent(5)");
        checkCond(evSink.createObjects.size() == 1u &&
                  evSink.createObjects[0].netId == 650u &&
                  evSink.createObjects[0].hasPos &&
                  evSink.createObjects[0].pos.x == 100.0f,
                  "Multi-event decoded CreateObjectEvent(6)");
        checkCond(evSink.enterVehicles.size() == 1u &&
                  evSink.enterVehicles[0].netId == 700u &&
                  evSink.enterVehicles[0].setAsDefaultVehicle,
                  "Multi-event decoded EnterVehicleEvent(9)");
        checkCond(evSink.postRemotes.size() == 1u &&
                  evSink.postRemotes[0].eventId == bf2bot::NRE_CLIENT_READY_ACK &&
                  evSink.postRemotes[0].payload.size() == 4u,
                  "Multi-event decoded PostRemoteEvent(11)");
        checkCond(evSink.killedBys.size() == 1u &&
                  evSink.killedBys[0].killerId == 9u &&
                  evSink.killedBys[0].victimId == 7u &&
                  evSink.killedBys[0].lastHitBoneIdx == 3u,
                  "Multi-event decoded KilledByEvent(21)");
        checkCond(evSink.beginRounds.size() == 1u &&
                  evSink.beginRounds[0].state == 2u &&
                  evSink.beginRounds[0].nextStateChangeAtTick == 9000u,
                  "Multi-event decoded BeginRoundEvent(56)");
        checkCond(evSink.spawnGroups.size() == 1u &&
                  evSink.spawnGroups[0].spawnGroupId == 5u &&
                  evSink.spawnGroups[0].netId == 520u &&
                  evSink.spawnGroups[0].active,
                  "Multi-event decoded CreateSpawnGroupEvent(57)");
        checkCond(codec.ghosts().lastTick() == 7777u,
                  "Multi-event bit cursor landed on exact start of GhostManager (tick=7777)");
    }
}

// ============================================================================
// Test 5: GhostManager ControlObjectState + known/unknown ghost skip
// ============================================================================
void writeGhostRecordHeader(bf2bot::BitStream& bs, uint16_t netId, uint32_t& outLenFieldBitOffset)
{
    bs.writeUInt(1u, 2); // action = 1 (update)
    bs.writeUInt(netId, 16);
    bs.writeBool(true);  // ghostActive
    outLenFieldBitOffset = bs.getBitOffset();
    bs.writeUInt(0u, 11); // placeholder for payloadBits
}

void patchBitField(bf2bot::BitStream& bs, uint32_t fieldBitOffset, uint32_t value, uint32_t widthBits)
{
    const uint32_t savedOffset = bs.getBitOffset();
    bs.setBitOffset(fieldBitOffset);
    bs.writeUInt(value, widthBits);
    bs.setBitOffset(savedOffset);
}

void testGhostManager()
{
    bf2bot::GhostReceiver ghosts;
    ghosts.setWorldSize(1024, 1024);
    ghosts.registerSoldierNetId(700u);
    ghosts.registerSoldierNetId(701u);
    ghosts.registerControlPointNetId(650u);

    bf2bot::BitStream bs(1458u);
    bs.writeBool(true);     // GhostManager present
    bs.writeUInt(4321u, 32);// tick
    bs.writeUInt(6u, 8);    // 6 ghost records
    bs.writeBool(true);     // hasControlState

    const bf2bot::Vec3 compVec(100.0f, 10.0f, 200.0f);

    // 1. ControlObjectState with extra tail bits (simulating weapon heat / kit inventory)
    {
        const uint32_t lenBitPos = bs.getBitOffset();
        bs.writeUInt(0u, 12); // placeholder totalBits
        const uint32_t payloadStart = bs.getBitOffset();

        bs.writeSignedMag(3, 32); // actionBufferEntries
        bs.writeFloat(compVec.x);
        bs.writeFloat(compVec.y);
        bs.writeFloat(compVec.z);
        bs.writeUInt(700u, 16); // controlNetId
        bs.writeBool(false);    // hasParachute
        bs.writeBool(true);     // isSoldier
        bs.writeBool(true);     // hasKit
        bs.writeUInt(800u, 16); // kitNetId
        bs.writeBool(false);    // isDelta
        bs.writeUInt(0u, 3);    // baseline slot

        writeSoldierPayload(bs, true, 240u, 1u,
                            bf2bot::Vec3(102.5f, 11.0f, 204.25f), compVec,
                            135.0f, -12.5f, true);

        // Extra tail bits simulating weapon heat / kit inventory
        bs.writeUInt(0x12345u, 23);
        const uint32_t totalBits = bs.getBitOffset() - payloadStart;
        patchBitField(bs, lenBitPos, totalBits, 12u);
    }

    // 2. Player ghost (netId=2)
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 2u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        const uint32_t mask = 0x1u | 0x200u | 0x1000u;
        bs.writeUInt(mask, 28);
        bs.writeBool(true);                  // isAlive
        bs.writeRanged(2u + 1u, 3u);         // team = 2
        bs.writeUInt((2u << 4) | 5u, 9u);    // kit=5, unlockLevel=2
        bs.writeBool(true);                  // isSquadLeader
        bs.writeBool(true);                  // isConnected
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    // 3. SpawnGroup ghost (netId=515)
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 515u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        const uint32_t mask = 0x02u | 0x04u | 0x01u;
        bs.writeUInt(mask, 6);
        bs.writeUInt(2u, 4);     // team
        bs.writeBool(true);      // active
        bs.writeUInt(128u, 8);   // quantX
        bs.writeUInt(192u, 8);   // quantZ
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    // 4. Soldier ghost (netId=700)
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 700u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        writeSoldierPayload(bs, false, 200u, 0u,
                            bf2bot::Vec3(105.0f, 10.5f, 205.5f), compVec,
                            90.0f, 15.0f, true);
        bs.writeUInt(0x1Fu, 5); // trailing template-specific bits
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    // 5. ControlPoint ghost (netId=650)
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 650u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        const uint32_t mask = 0x01u | 0x02u | 0x04u;
        bs.writeUInt(mask, 7);
        bs.writeSignedMag(1, 4);    // team = 1
        bs.writeBool(false);        // isNeutral
        bs.writeUInt(65535u, 16);   // takeOverCounter -> +1.25f
        bs.writeUInt(0u, 16);       // takeOverChangePerSecond -> -1.25f
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    // 6. Unknown vehicle ghost (netId=999, arbitrary 73-bit payload)
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 999u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        bs.writeUInt(0xDEADBEEFu, 32);
        bs.writeUInt(0xCAFEBABEu, 32);
        bs.writeUInt(0x155u, 9);
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    // 7. Second Soldier ghost (netId=701) AFTER the unknown vehicle ghost
    {
        uint32_t lenPos = 0;
        writeGhostRecordHeader(bs, 701u, lenPos);
        const uint32_t pStart = bs.getBitOffset();
        writeSoldierPayload(bs, false, 150u, 2u,
                            bf2bot::Vec3(110.0f, 10.0f, 210.0f), compVec,
                            -45.0f, 30.0f, false);
        patchBitField(bs, lenPos, bs.getBitOffset() - pStart, 11u);
    }

    bf2bot::BitStream reader(bs.data(), bs.getByteSize());
    reader.setBitSize(bs.getBitSize());

    RecordingGhostSink sink;
    checkCond(ghosts.processReceivedPacket(reader, &sink),
              "GhostReceiver parsed ControlObjectState + 6 ghosts cleanly");
    checkCond(ghosts.lastTick() == 4321u, "GhostReceiver tick == 4321");

    checkCond(sink.controlStates.size() == 1u &&
              sink.controlStates[0].controlNetId == 700u &&
              sink.controlStates[0].kitNetId == 800u &&
              sink.controlStates[0].hasSoldierState &&
              sink.controlStates[0].soldierState.hitPointsByte == 240u &&
              sink.controlStates[0].soldierState.pose == 1u &&
              nearFloat(sink.controlStates[0].soldierState.pos.x, 102.5f, 0.01f) &&
              nearFloat(sink.controlStates[0].soldierState.facingDegrees, 135.0f, 0.01f),
              "GhostManager ControlObjectState decoded & aligned past tail bits");

    checkCond(sink.players.size() == 1u &&
              sink.players[0].netId == 2u &&
              sink.players[0].playerId == 1u &&
              sink.players[0].isAlive &&
              sink.players[0].team == 2 &&
              sink.players[0].kit == 5u &&
              sink.players[0].unlockLevel == 2u &&
              sink.players[0].isSquadLeader &&
              sink.players[0].isConnected,
              "GhostManager Player ghost (netId=2) decoded");

    checkCond(sink.spawnGroups.size() == 1u &&
              sink.spawnGroups[0].netId == 515u &&
              sink.spawnGroups[0].team == 2u &&
              sink.spawnGroups[0].active &&
              sink.spawnGroups[0].quantX == 128u &&
              sink.spawnGroups[0].quantZ == 192u,
              "GhostManager SpawnGroup ghost (netId=515) decoded");

    checkCond(sink.controlPoints.size() == 1u &&
              sink.controlPoints[0].netId == 650u &&
              sink.controlPoints[0].team == 1 &&
              !sink.controlPoints[0].isNeutral &&
              nearFloat(sink.controlPoints[0].takeOverCounter, 1.25f, 0.01f) &&
              nearFloat(sink.controlPoints[0].takeOverChangePerSecond, -1.25f, 0.01f),
              "GhostManager ControlPoint ghost (netId=650) decoded");

    checkCond(sink.soldiers.size() == 2u &&
              sink.soldiers[0].netId == 700u &&
              sink.soldiers[0].hitPointsByte == 200u &&
              nearFloat(sink.soldiers[0].pos.x, 105.0f, 0.01f) &&
              sink.soldiers[1].netId == 701u &&
              sink.soldiers[1].hitPointsByte == 150u &&
              sink.soldiers[1].pose == 2u &&
              !sink.soldiers[1].isGrounded &&
              nearFloat(sink.soldiers[1].pos.x, 110.0f, 0.01f) &&
              nearFloat(sink.soldiers[1].pos.z, 210.0f, 0.01f),
              "GhostManager Soldier ghosts 700 & 701 decoded after skipping unknown ghost 999");
}

// ============================================================================
// Test 6: NavMesh synthetic v3 .qtr A* test
// ============================================================================
void testNavMesh()
{
    const char* const qtrPath = "bf2_loadbot_test_nav.qtr";
    std::remove(qtrPath);

    // Build a 3x3 grid of 10x10m quads in [0,30] x [0,30] at y=0.
    // Polygon index p = r * 3 + c. Center polygon p=4 has material=0 (unwalkable obstacle).
    std::vector<uint8_t> qtr;
    writeI32LE(qtr, 1);       // numberOfIslands
    writeI32LE(qtr, 3);       // version 3
    writeF32LE(qtr, 0.0f);    // minX
    writeF32LE(qtr, 0.0f);    // minZ
    writeF32LE(qtr, 30.0f);   // maxX
    writeF32LE(qtr, 30.0f);   // maxZ
    writeF32LE(qtr, 30.0f);   // size

    // 1 leaf QuadNode containing all 9 polygons
    writeU32LE(qtr, 1u);
    writeU32LE(qtr, 0u);
    writeU32LE(qtr, 0u);
    writeU32LE(qtr, 0u);
    writeU32LE(qtr, 0u);
    writeU8LE(qtr, 1u); // isLeaf
    writeU8LE(qtr, 9u); // polygonCount
    writeU8LE(qtr, 0u);
    writeU8LE(qtr, 0u);
    writeU32LE(qtr, 0u); // firstPolygon

    // 9 Polygons (20 bytes each)
    writeU32LE(qtr, 9u);
    for (uint32_t p = 0; p < 9u; ++p)
    {
        writeU8LE(qtr, 4u);                        // numVertices
        writeU8LE(qtr, (p == 4u) ? 0u : 1u);       // material (0 = unwalkable obstacle)
        writeU16LE(qtr, 0u);                       // cluster
        writeU32LE(qtr, p * 4u);                   // firstIndex
        writeF32LE(qtr, 0.0f);                     // normal.x
        writeF32LE(qtr, 1.0f);                     // normal.y
        writeF32LE(qtr, 0.0f);                     // normal.z
    }

    // NodePolygons: 0..8
    writeU32LE(qtr, 9u);
    for (uint32_t p = 0; p < 9u; ++p)
    {
        writeU32LE(qtr, p);
    }

    // 16 Vertices (4x4 grid, 10m spacing)
    writeU32LE(qtr, 16u);
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            writeF32LE(qtr, static_cast<float>(c * 10));
            writeF32LE(qtr, 0.0f);
            writeF32LE(qtr, static_cast<float>(r * 10));
        }
    }

    // 36 VertexIndices & 36 Neighbours
    std::vector<uint32_t> indices;
    std::vector<uint32_t> neighbours;
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            const uint32_t v00 = static_cast<uint32_t>(r * 4 + c);
            const uint32_t v01 = static_cast<uint32_t>((r + 1) * 4 + c);
            const uint32_t v11 = static_cast<uint32_t>((r + 1) * 4 + (c + 1));
            const uint32_t v10 = static_cast<uint32_t>(r * 4 + (c + 1));
            const uint32_t p = static_cast<uint32_t>(r * 3 + c);

            // Winding verified against NavMesh::pointWithinPolygonEdges
            indices.push_back(v00);
            indices.push_back(v01);
            indices.push_back(v11);
            indices.push_back(v10);

            neighbours.push_back(c > 0 ? (p - 1u) : bf2bot::NavMesh::INVALID_POLYGON); // edge v00->v01 (west)
            neighbours.push_back(r < 2 ? (p + 3u) : bf2bot::NavMesh::INVALID_POLYGON); // edge v01->v11 (north)
            neighbours.push_back(c < 2 ? (p + 1u) : bf2bot::NavMesh::INVALID_POLYGON); // edge v11->v10 (east)
            neighbours.push_back(r > 0 ? (p - 3u) : bf2bot::NavMesh::INVALID_POLYGON); // edge v10->v00 (south)
        }
    }

    writeU32LE(qtr, static_cast<uint32_t>(indices.size()));
    for (size_t i = 0; i < indices.size(); ++i) writeU32LE(qtr, indices[i]);

    writeU32LE(qtr, static_cast<uint32_t>(neighbours.size()));
    for (size_t i = 0; i < neighbours.size(); ++i) writeU32LE(qtr, neighbours[i]);

    {
        std::ofstream ofs(qtrPath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(&qtr[0]), static_cast<std::streamsize>(qtr.size()));
    }

    bf2bot::NavMesh nav;
    checkCond(nav.load(qtrPath) && nav.isLoaded() && nav.polygonCount() == 9u,
              "NavMesh::load synthetic v3 .qtr");
    checkCond(nav.findPolygonExact(15.0f, 0.0f, 15.0f) == bf2bot::NavMesh::INVALID_POLYGON,
              "NavMesh obstacle polygon (material=0) rejected by findPolygonExact");
    checkCond(nav.findPolygon(5.0f, 0.0f, 15.0f) == 3u &&
              nav.findPolygon(25.0f, 0.0f, 15.0f) == 5u,
              "NavMesh::findPolygon locates start (3) and goal (5)");

    std::vector<bf2bot::NavPoint> waypoints;
    const bool pathOk = nav.findPath(5.0f, 0.0f, 15.0f, 25.0f, 0.0f, 15.0f, waypoints);
    checkCond(pathOk && waypoints.size() >= 4u,
              "NavMesh::findPath detours around center obstacle");

    bool enteredObstacle = false;
    for (size_t i = 0; i < waypoints.size(); ++i)
    {
        const bf2bot::NavPoint& wp = waypoints[i];
        if (wp.x > 10.05f && wp.x < 19.95f && wp.z > 10.05f && wp.z < 19.95f)
        {
            enteredObstacle = true;
        }
    }
    checkCond(!enteredObstacle, "NavMesh A* path never steps inside obstacle polygon 4");

    std::remove(qtrPath);
}

// ============================================================================
// Test 7: RouteLoader "OBR1" v1 round-trip + ABI parity
// ============================================================================
bf2bot::RouteActionSample makeSample(uint32_t tick, uint16_t pid, uint8_t team,
                                     float x, float y, float z, uint32_t flags)
{
    bf2bot::RouteActionSample s;
    std::memset(&s, 0, sizeof(s));
    s.tick = tick;
    s.playerId = pid;
    s.team = team;
    s.stance = bf2bot::STANCE_STAND;
    s.posX = x;
    s.posY = y;
    s.posZ = z;
    s.yawDeg = 45.0f;
    s.pitchDeg = -2.0f;
    s.actionFlags = flags;
    s.moveForward = 16000;
    s.moveStrafe = -8000;
    return s;
}

void testRouteLoaderAndAbiParity()
{
    checkCond(sizeof(bf2bot::RouteActionHeader) == 32u &&
              sizeof(dice::hfe::RouteActionHeader) == 32u &&
              sizeof(bf2bot::RouteActionSample) == 36u &&
              sizeof(dice::hfe::RouteActionSample) == 36u,
              "RouteFormat vs RouteActionRecorder struct sizes == 32 and 36");

    checkCond(offsetof(bf2bot::RouteActionHeader, magic) == offsetof(dice::hfe::RouteActionHeader, magic) &&
              offsetof(bf2bot::RouteActionHeader, version) == offsetof(dice::hfe::RouteActionHeader, version) &&
              offsetof(bf2bot::RouteActionHeader, sampleHz) == offsetof(dice::hfe::RouteActionHeader, sampleHz) &&
              offsetof(bf2bot::RouteActionHeader, recordSize) == offsetof(dice::hfe::RouteActionHeader, recordSize) &&
              offsetof(bf2bot::RouteActionHeader, mapName) == offsetof(dice::hfe::RouteActionHeader, mapName),
              "RouteActionHeader field offset parity");

    checkCond(offsetof(bf2bot::RouteActionSample, tick) == offsetof(dice::hfe::RouteActionSample, tick) &&
              offsetof(bf2bot::RouteActionSample, playerId) == offsetof(dice::hfe::RouteActionSample, playerId) &&
              offsetof(bf2bot::RouteActionSample, team) == offsetof(dice::hfe::RouteActionSample, team) &&
              offsetof(bf2bot::RouteActionSample, stance) == offsetof(dice::hfe::RouteActionSample, stance) &&
              offsetof(bf2bot::RouteActionSample, posX) == offsetof(dice::hfe::RouteActionSample, posX) &&
              offsetof(bf2bot::RouteActionSample, posY) == offsetof(dice::hfe::RouteActionSample, posY) &&
              offsetof(bf2bot::RouteActionSample, posZ) == offsetof(dice::hfe::RouteActionSample, posZ) &&
              offsetof(bf2bot::RouteActionSample, yawDeg) == offsetof(dice::hfe::RouteActionSample, yawDeg) &&
              offsetof(bf2bot::RouteActionSample, pitchDeg) == offsetof(dice::hfe::RouteActionSample, pitchDeg) &&
              offsetof(bf2bot::RouteActionSample, actionFlags) == offsetof(dice::hfe::RouteActionSample, actionFlags) &&
              offsetof(bf2bot::RouteActionSample, moveForward) == offsetof(dice::hfe::RouteActionSample, moveForward) &&
              offsetof(bf2bot::RouteActionSample, moveStrafe) == offsetof(dice::hfe::RouteActionSample, moveStrafe),
              "RouteActionSample field offset parity");

    const char* const routePath = "bf2_loadbot_test_routes.bin";
    std::remove(routePath);

    bf2bot::RouteActionHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    std::memcpy(hdr.magic, bf2bot::routeMagicBytes(), 4u);
    hdr.version = bf2bot::ROUTE_VERSION;
    hdr.sampleHz = 10u;
    hdr.recordSize = sizeof(bf2bot::RouteActionSample);
    std::memcpy(hdr.mapName, "karkand", 7u);

    std::vector<bf2bot::RouteActionSample> samples;
    // Track 0: player 1, team 1, continuous
    samples.push_back(makeSample(0u,   1u, 1u, 10.0f, 0.0f, 10.0f, bf2bot::AF_MOVE_FWD));
    samples.push_back(makeSample(3u,   1u, 1u, 11.0f, 0.0f, 10.0f, bf2bot::AF_MOVE_FWD));
    samples.push_back(makeSample(6u,   1u, 1u, 12.0f, 0.0f, 10.0f, bf2bot::AF_FIRE));
    // Track 1: player 1, team 1, tick gap > sampleHz * 5 (50)
    samples.push_back(makeSample(100u, 1u, 1u, 13.0f, 0.0f, 10.0f, bf2bot::AF_SPRINT));
    samples.push_back(makeSample(103u, 1u, 1u, 14.0f, 0.0f, 10.0f, bf2bot::AF_SPRINT));
    // Track 2: player 1, team 1, teleport > 15m
    samples.push_back(makeSample(106u, 1u, 1u, 100.0f, 0.0f, 100.0f, bf2bot::AF_JUMP));
    samples.push_back(makeSample(109u, 1u, 1u, 101.0f, 0.0f, 100.0f, bf2bot::AF_CROUCH));
    // Track 3: player 2, team 2
    samples.push_back(makeSample(0u,   2u, 2u, 200.0f, 0.0f, 200.0f, bf2bot::AF_RELOAD));
    samples.push_back(makeSample(3u,   2u, 2u, 201.0f, 0.0f, 200.0f, bf2bot::AF_ALT_FIRE));

    {
        std::ofstream ofs(routePath, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        ofs.write(reinterpret_cast<const char*>(&samples[0]),
                  static_cast<std::streamsize>(samples.size() * sizeof(bf2bot::RouteActionSample)));
    }

    bf2bot::RouteLoader loader;
    checkCond(loader.load(routePath) &&
              loader.isLoaded() &&
              loader.sampleHz() == 10u &&
              loader.mapName() == "karkand" &&
              loader.sampleCount() == 9u &&
              loader.trackCount() == 4u,
              "RouteLoader OBR1 v1 segmentation across tick gap & teleport break");

    size_t tIdx = 999u, sIdx = 999u;
    checkCond(loader.pickTrackForPosition(1u, 100.5f, 100.0f, 7u, tIdx, sIdx) && tIdx == 2u,
              "RouteLoader::pickTrackForPosition selects nearby team-1 track 2");
    checkCond(loader.pickTrackForPosition(2u, 200.2f, 200.0f, 3u, tIdx, sIdx) && tIdx == 3u,
              "RouteLoader::pickTrackForPosition selects nearby team-2 track 3");

    std::remove(routePath);
}

// ============================================================================
// Test 8: NetImpairment queue
// ============================================================================
void testNetImpairment()
{
    // Loss rate check (20% over 1000 packets)
    {
        bf2bot::ImpairmentConfig cfg;
        cfg.oneWayDelayMs = 0.0;
        cfg.jitterMs = 0.0;
        cfg.lossPct = 20.0;
        cfg.seed = 12345u;
        bf2bot::ImpairmentQueue q(cfg);

        const uint8_t byte = 0x5Au;
        for (int i = 0; i < 1000; ++i)
        {
            q.enqueue(static_cast<uint64_t>(i) * 1000ULL, &byte, 1u);
        }
        checkCond(q.enqueuedCount() == 1000ULL &&
                  q.droppedCount() >= 140ULL &&
                  q.droppedCount() <= 260ULL &&
                  (q.droppedCount() + q.pendingCount() == 1000ULL),
                  "NetImpairment deterministic 20% loss within statistical bounds");
    }

    // FIFO ordering with jitter == 0
    {
        bf2bot::ImpairmentConfig cfg;
        cfg.oneWayDelayMs = 10.0;
        cfg.jitterMs = 0.0;
        cfg.lossPct = 0.0;
        cfg.seed = 77u;
        bf2bot::ImpairmentQueue q(cfg);

        for (uint8_t i = 0; i < 100u; ++i)
        {
            q.enqueue(static_cast<uint64_t>(i) * 100ULL, &i, 1u);
        }
        bool fifoOk = true;
        std::vector<uint8_t> pkt;
        for (uint8_t i = 0; i < 100u; ++i)
        {
            if (!q.popReady(1000000ULL, pkt) || pkt.size() != 1u || pkt[0] != i)
            {
                fifoOk = false;
                break;
            }
        }
        checkCond(fifoOk && !q.popReady(1000000ULL, pkt),
                  "NetImpairment strict FIFO ordering when jitter == 0");
    }

    // Bounded delivery timestamps with delay=20ms, jitter=5ms
    {
        bf2bot::ImpairmentConfig cfg;
        cfg.oneWayDelayMs = 20.0;
        cfg.jitterMs = 5.0;
        cfg.lossPct = 0.0;
        cfg.seed = 999u;
        bf2bot::ImpairmentQueue q(cfg);

        const uint64_t t0 = 1000000ULL;
        for (int i = 0; i < 200; ++i)
        {
            const uint8_t b = static_cast<uint8_t>(i & 0xFF);
            q.enqueue(t0, &b, 1u);
        }
        checkCond(q.nextDeliverUs() >= t0 + 15000ULL &&
                  q.nextDeliverUs() <= t0 + 25000ULL,
                  "NetImpairment earliest timestamp within [15ms, 25ms]");

        std::vector<uint8_t> pkt;
        checkCond(!q.popReady(t0 + 14999ULL, pkt),
                  "NetImpairment no packet delivered before delay - jitter (15ms)");

        size_t popped = 0u;
        while (q.popReady(t0 + 25000ULL, pkt))
        {
            ++popped;
        }
        checkCond(popped == 200u && q.pendingCount() == 0u,
                  "NetImpairment all packets delivered by delay + jitter (25ms)");
    }
}

// ============================================================================
// Test 9: Telemetry + Sliding detector
// ============================================================================
void testTelemetryAndSlidingDetector()
{
    bf2bot::Telemetry tel;
    tel.setBotCounts(64u, 64u, 64u, 60u);

    // 60 packets at 30 Hz (33.333ms) + 1 spike at 250ms
    const double dt30Hz = 1000.0 / 30.0;
    for (int i = 0; i < 60; ++i)
    {
        tel.recordPacketTx(64u);
        tel.recordPacketRx(256u, 8u, dt30Hz);
    }
    tel.recordPacketRx(256u, 8u, 250.0);

    // Nearby enemy (<50m) updates: t=0, +33ms, +183ms (gap=150ms >100ms), +433ms (gap=250ms >100ms & >200ms)
    const uint64_t tBase = 1000000ULL;
    tel.recordEnemySoldierUpdate(1u, 700u, tBase, 30.0f, true);
    tel.recordEnemySoldierUpdate(1u, 700u, tBase + 33000ULL, 30.0f, true);
    tel.recordEnemySoldierUpdate(1u, 700u, tBase + 183000ULL, 30.0f, true);
    tel.recordEnemySoldierUpdate(1u, 700u, tBase + 433000ULL, 30.0f, true);

    // Far enemy (80m > 50m): must be ignored by sliding detector
    tel.recordEnemySoldierUpdate(1u, 701u, tBase, 80.0f, true);
    tel.recordEnemySoldierUpdate(1u, 701u, tBase + 250000ULL, 80.0f, true);

    const bf2bot::TelemetrySummary sum = tel.getWindowSummary();
    checkCond(nearDouble(sum.snapHzP50, 30.0, 0.1) &&
              nearDouble(sum.maxGapMs, 250.0, 0.01),
              "Telemetry snapHzP50 ~ 30 Hz & maxGapMs == 250 ms");
    checkCond(sum.enemyNearbyUpdates == 3u &&
              sum.enemyGapOver100ms == 2u &&
              sum.enemyGapOver200ms == 1u &&
              nearDouble(sum.maxEnemyGapMs, 250.0, 0.01),
              "Telemetry nearby-enemy (<50m) sliding detector counts >100ms & >200ms while ignoring 80m");

    const std::string report = tel.formatReport("64p-unit", 100.0, 10.0);
    checkCond(!report.empty() &&
              report.find("LoadBot Telemetry [64p-unit]") != std::string::npos &&
              report.find("Slide") != std::string::npos,
              "Telemetry::formatReport produces formatted summary");
}

// ============================================================================
// Test 10: End-to-end handshake, gameplay, ragdoll, EOR & impairment harness
// ============================================================================
void testLoopbackHandshakeAndGameplay()
{
#ifdef _WIN32
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        checkCond(false, "Loopback test WSAStartup");
        return;
    }
#endif

    sockaddr_in srvAddr;
    std::memset(&srvAddr, 0, sizeof(srvAddr));
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srvAddr.sin_port = htons(19999u);

    bf2bot::Telemetry telemetry;
    bf2bot::LoadBotConfig cfg;
    cfg.botIndex = 0u;
    cfg.modName = "mods/bf2";
    cfg.worldSize = 1024.0f;
    cfg.seed = 42u;
    cfg.startAtUs = 1000000ULL;
    cfg.uplinkImpair.oneWayDelayMs = 15.0;
    cfg.uplinkImpair.jitterMs = 3.0;
    cfg.uplinkImpair.lossPct = 0.0;
    cfg.uplinkImpair.seed = 101u;
    cfg.downlinkImpair.oneWayDelayMs = 15.0;
    cfg.downlinkImpair.jitterMs = 3.0;
    cfg.downlinkImpair.lossPct = 0.0;
    cfg.downlinkImpair.seed = 202u;

    bf2bot::LoadBotClient bot(cfg, srvAddr, nullptr, nullptr, &telemetry);
    checkCond(bot.initSocket(), "LoadBotClient::initSocket succeeds");

    // In-memory bidirectional datagram queues (sandbox-safe & 100% deterministic)
    std::deque<std::vector<uint8_t> > c2sQueue;
    std::deque<std::vector<uint8_t> > s2cQueue;
    bot.setRawSendHook([&](const uint8_t* data, size_t len) {
        c2sQueue.push_back(std::vector<uint8_t>(data, data + len));
    });

    const uint8_t kConnId = 7u;
    bf2bot::SWConnectionState srvConn;
    srvConn.setAssignedConnId(kConnId);
    uint32_t srvNextBlockIdx = 0u;
    uint32_t srvDeliveryId = 1u;

    auto sendSrvRaw = [&](const uint8_t* data, size_t len) {
        if (!data || len == 0) return;
        s2cQueue.push_back(std::vector<uint8_t>(data, data + len));
    };

    auto sendSrvDataPayload = [&](const uint8_t* payload, size_t payloadLen) {
        uint8_t wire[bf2bot::kMaxDatagramSize];
        size_t wireLen = 0;
        if (srvConn.wrapDataPacket(payload, static_cast<uint16_t>(payloadLen),
                                   srvDeliveryId++, wire, sizeof(wire), wireLen))
        {
            sendSrvRaw(wire, wireLen);
        }
    };

    auto sendSrvEvents = [&](const std::vector<bf2bot::BitStream>& evs) {
        bf2bot::BitStream pkt(bf2bot::kMaxDataPayloadSize);
        pkt.writeBool(false); // PlayerActionManager present = 0
        pkt.writeBool(true);  // GameEventManager present = 1
        pkt.writeUInt(static_cast<uint32_t>(evs.size()), 8);
        pkt.writeUInt(srvNextBlockIdx & 31u, 5);
        ++srvNextBlockIdx;
        pkt.writeBool(false); // isResend
        for (size_t i = 0; i < evs.size(); ++i)
        {
            pkt.writeBits(evs[i].data(), evs[i].getBitSize());
        }
        pkt.writeBool(false); // GhostManager present = 0
        sendSrvDataPayload(pkt.data(), pkt.getByteSize());
    };

    auto appendDataBlockEvents = [&](std::vector<bf2bot::BitStream>& evs,
                                     uint32_t blockId, const uint8_t* bytes, uint32_t len) {
        bf2bot::BitStream hdr(32u);
        hdr.writeUInt(bf2bot::GE_DATA_BLOCK, bf2bot::kGameEventTypeBits);
        hdr.writeBool(true);
        hdr.writeUInt(blockId, 32);
        hdr.writeUInt(len, 32);
        evs.push_back(hdr);

        uint32_t off = 0u;
        while (off < len)
        {
            uint32_t chunk = len - off;
            if (chunk > bf2bot::kDataBlockMaxChunkSize) chunk = bf2bot::kDataBlockMaxChunkSize;
            bf2bot::BitStream ch(96u);
            ch.writeUInt(bf2bot::GE_DATA_BLOCK, bf2bot::kGameEventTypeBits);
            ch.writeBool(false);
            ch.writeUInt(chunk, 8);
            ch.writeFixedBytes(bytes + off, chunk);
            evs.push_back(ch);
            off += chunk;
        }
    };

    bool gotConnectReq = false;
    bool gotConnectAck = false;
    bool gotChallengeResp = false;
    int clientInfoCount = 0;
    uint32_t clientInfoHash = 0u;
    bool gotMapLoaded = false;
    bool gotClientReady = false;
    bool gotConnType = false;
    bool gotJoinTeam = false;
    bool gotSelectKit = false;
    bool gotSelectSpawn = false;
    bool gotPlayerAction = false;
    bool gotEndOfRoundAck = false;
    bool gotConfirmReset = false;
    bool gotDisconnect = false;

    bool sentChallenge = false;
    bool sentDbComplete = false;
    bool sentReadyAck = false;
    bool sentSpawnGhost = false;
    bool sentRagdollGhost = false;
    bool sentEorAndReset = false;

    // Client->Server DataBlock accumulator for verifying ClientInfo
    bool rxDbActive = false;
    uint32_t rxDbId = 0u;
    uint32_t rxDbLen = 0u;
    std::vector<uint8_t> rxDbBuf;

    auto drainServerSocket = [&]() {
        while (!c2sQueue.empty())
        {
            const std::vector<uint8_t> pkt = c2sQueue.front();
            c2sQueue.pop_front();
            const uint8_t* buf = pkt.empty() ? nullptr : &pkt[0];
            const size_t n = pkt.size();
            if (!buf || n == 0) continue;

            bf2bot::ParsedDatagram dg;
            if (!bf2bot::parseIncomingDatagram(buf, n, dg)) continue;

            if (dg.type == bf2bot::UDP_CONNECT_REQUEST)
            {
                gotConnectReq = true;
                uint8_t acc[16];
                std::memset(acc, 0, sizeof(acc));
                bf2bot::BitStream bs(acc, sizeof(acc));
                bs.writeUInt(bf2bot::UDP_CONNECT_ACCEPT, 4);
                bs.writeUInt(0u, 8);
                bs.writeUInt(kConnId, 8);
                bs.writeUInt(1000u, 32);
                bs.writeBool(false);
                sendSrvRaw(acc, bs.getByteSize());
            }
            else if (dg.type == bf2bot::UDP_CONNECT_ACCEPT_ACK)
            {
                gotConnectAck = true;
                if (!sentChallenge)
                {
                    sentChallenge = true;
                    std::vector<bf2bot::BitStream> evs;
                    bf2bot::BitStream ch(32u);
                    ch.writeUInt(bf2bot::GE_CHALLENGE, bf2bot::kGameEventTypeBits);
                    const uint8_t chal[10] = { 1,2,3,4,5,6,7,8,9,10 };
                    ch.writeFixedBytes(chal, 10u);
                    ch.writeUInt(8u, 8);
                    ch.writeFixedBytes("mods/bf2", 8u);
                    evs.push_back(ch);
                    sendSrvEvents(evs);
                }
            }
            else if (dg.type == bf2bot::UDP_DISCONNECT)
            {
                gotDisconnect = true;
            }
            else if (dg.type == bf2bot::UDP_PING_REQUEST)
            {
                srvConn.handleRemoteAck(dg.ack, dg.ackMask, bf2bot::PacketStatusCallback());
                uint8_t pr[64];
                const size_t prLen = bf2bot::buildPingResponse(pr, sizeof(pr), srvConn,
                                                               dg.pingTimestampMs, 2000u);
                sendSrvRaw(pr, prLen);
            }
            else if (dg.type == bf2bot::UDP_DATA_PACKET)
            {
                if (!srvConn.acceptIncomingDataSeq(dg.seq)) continue;
                srvConn.handleRemoteAck(dg.ack, dg.ackMask, bf2bot::PacketStatusCallback());

                bf2bot::BitStream bs(dg.payload, dg.payloadLen);
                // 0: PlayerActionManager
                const uint32_t posBeforeAct = bs.getBitOffset();
                if (!bf2bot::PlayerActionSender::skipIncoming(bs)) continue;
                if (bs.getBitOffset() > posBeforeAct + 1u)
                {
                    gotPlayerAction = true;
                }

                // 1: GameEventManager
                const bool evPresent = bs.readBool();
                if (evPresent && !bs.hasError())
                {
                    const uint32_t evCount = bs.readUInt(8);
                    bs.readUInt(5); // blockIndex
                    bs.readBool();  // isResend
                    for (uint32_t i = 0; i < evCount && !bs.hasError(); ++i)
                    {
                        const uint32_t tid = bs.readUInt(bf2bot::kGameEventTypeBits);
                        if (tid == bf2bot::GE_CHALLENGE_RESPONSE)
                        {
                            bs.skipBits(73u * 8u + 32u + 32u + 32u);
                            gotChallengeResp = true;
                            // Reply with ServerInfo (requestResponse=true)
                            std::vector<uint8_t> siBytes;
                            buildServerInfoBlock(siBytes);
                            std::vector<bf2bot::BitStream> evs;
                            appendDataBlockEvents(evs, bf2bot::DB_SERVER_INFO,
                                                  &siBytes[0], static_cast<uint32_t>(siBytes.size()));
                            sendSrvEvents(evs);
                        }
                        else if (tid == bf2bot::GE_CONNECTION_TYPE)
                        {
                            bs.skipBits(3u);
                            gotConnType = true;
                        }
                        else if (tid == bf2bot::GE_DATA_BLOCK)
                        {
                            const bool isNew = bs.readBool();
                            if (isNew)
                            {
                                rxDbActive = true;
                                rxDbId = bs.readUInt(32);
                                rxDbLen = bs.readUInt(32);
                                rxDbBuf.clear();
                            }
                            else
                            {
                                const uint32_t sz = bs.readUInt(8);
                                const size_t oldSz = rxDbBuf.size();
                                rxDbBuf.resize(oldSz + sz);
                                if (sz > 0) bs.readFixedBytes(&rxDbBuf[oldSz], sz);
                                if (rxDbActive && rxDbBuf.size() >= rxDbLen)
                                {
                                    if (rxDbId == bf2bot::DB_CLIENT_INFO && rxDbLen > 0)
                                    {
                                        ++clientInfoCount;
                                        const uint8_t* ptr = &rxDbBuf[0];
                                        bf2bot::BitStream ci(ptr, rxDbLen);
                                        ci.readStringU16();
                                        clientInfoHash = ci.readUInt(32);
                                        if (clientInfoCount == 1)
                                        {
                                            // Send a periodic ServerInfo with requestResponse=false (must NOT trigger another ClientInfo!)
                                            bf2bot::BitStream si2(256u);
                                            si2.writeStringU16("OpenBF2");
                                            si2.writeUInt(0u, 16);
                                            si2.writeBool(false); // requestResponse = false
                                            si2.writeUInt(0u, 6);
                                            si2.writeUInt(0u, 32);
                                            si2.writeUInt(0u, 16);
                                            si2.writeUInt(0u, 16);
                                            si2.writeSignedMag(0, 32);
                                            si2.writeUInt(16567u, 16);
                                            si2.writeSignedMag(0, 32);
                                            si2.writeSignedMag(0, 32);
                                            si2.writeSignedMag(0, 32);
                                            si2.writeBool(false);

                                            std::vector<uint8_t> mapBytes;
                                            buildMapInfoBlock(mapBytes);

                                            std::vector<bf2bot::BitStream> evs;
                                            appendDataBlockEvents(evs, bf2bot::DB_SERVER_INFO,
                                                                  si2.data(), si2.getByteSize());
                                            appendDataBlockEvents(evs, bf2bot::DB_MAP_INFO,
                                                                  &mapBytes[0], static_cast<uint32_t>(mapBytes.size()));
                                            sendSrvEvents(evs);
                                        }
                                    }
                                    rxDbActive = false;
                                }
                            }
                        }
                        else if (tid == bf2bot::GE_POST_REMOTE)
                        {
                            const uint32_t cat = bs.readUInt(4);
                            const uint32_t eid = bs.readUInt(32);
                            bs.readFloat();
                            const uint32_t plen = bs.readUInt(8);
                            if (plen > 0) bs.skipBits(plen * 8u);
                            if (cat == bf2bot::EC_NETWORK)
                            {
                                if (eid == bf2bot::NRE_MAP_LOADED)
                                {
                                    gotMapLoaded = true;
                                    if (!sentDbComplete)
                                    {
                                        sentDbComplete = true;
                                        std::vector<bf2bot::BitStream> evs;
                                        // Own player (id=5, team=1, isRemote=false)
                                        bf2bot::BitStream cp(64u);
                                        cp.writeUInt(bf2bot::GE_CREATE_PLAYER, bf2bot::kGameEventTypeBits);
                                        cp.writeUInt(1u, 3);
                                        cp.writeUInt(1u, 4);
                                        cp.writeBool(false);
                                        cp.writeUInt(5u, 8);
                                        cp.writeUInt(6u, 16);
                                        cp.writeUInt(0u, 16);
                                        cp.writeBool(false);
                                        char nm[32];
                                        std::memset(nm, 0, sizeof(nm));
                                        std::memcpy(nm, " LoadBot_01", 11);
                                        cp.writeFixedBytes(nm, 32u);
                                        evs.push_back(cp);

                                        // Enemy player (id=6, team=2, isRemote=true)
                                        bf2bot::BitStream ep(64u);
                                        ep.writeUInt(bf2bot::GE_CREATE_PLAYER, bf2bot::kGameEventTypeBits);
                                        ep.writeUInt(2u, 3);
                                        ep.writeUInt(2u, 4);
                                        ep.writeBool(true);
                                        ep.writeUInt(6u, 8);
                                        ep.writeUInt(7u, 16);
                                        ep.writeUInt(0u, 16);
                                        ep.writeBool(false);
                                        std::memset(nm, 0, sizeof(nm));
                                        std::memcpy(nm, " Enemy_01", 9);
                                        ep.writeFixedBytes(nm, 32u);
                                        evs.push_back(ep);

                                        // Spawn group for team 1
                                        bf2bot::BitStream sg(32u);
                                        sg.writeUInt(bf2bot::GE_CREATE_SPAWN_GROUP, bf2bot::kGameEventTypeBits);
                                        sg.writeUInt(1u, 8);
                                        sg.writeUInt(1u, 4);
                                        sg.writeBool(true);
                                        sg.writeBool(false);
                                        sg.writeBool(true);
                                        sg.writeUInt(128u, 8);
                                        sg.writeUInt(128u, 8);
                                        sg.writeUInt(515u, 16);
                                        evs.push_back(sg);

                                        // Enemy enters soldier netId=710
                                        bf2bot::BitStream ev(16u);
                                        ev.writeUInt(bf2bot::GE_ENTER_VEHICLE, bf2bot::kGameEventTypeBits);
                                        ev.writeUInt(6u, 8);
                                        ev.writeUInt(710u, 16);
                                        ev.writeBool(true);
                                        evs.push_back(ev);

                                        // BeginRound + DBComplete (ECNetwork, 4)
                                        bf2bot::BitStream br(16u);
                                        br.writeUInt(bf2bot::GE_BEGIN_ROUND, bf2bot::kGameEventTypeBits);
                                        br.writeUInt(2u, 32);
                                        br.writeUInt(0u, 32);
                                        evs.push_back(br);

                                        bf2bot::BitStream dbc(16u);
                                        dbc.writeUInt(bf2bot::GE_POST_REMOTE, bf2bot::kGameEventTypeBits);
                                        dbc.writeUInt(bf2bot::EC_NETWORK, 4);
                                        dbc.writeUInt(bf2bot::NRE_CLIENT_READY, 32);
                                        dbc.writeFloat(0.0f);
                                        dbc.writeUInt(0u, 8);
                                        evs.push_back(dbc);

                                        sendSrvEvents(evs);
                                    }
                                }
                                else if (eid == bf2bot::NRE_CLIENT_READY)
                                {
                                    gotClientReady = true;
                                    if (!sentReadyAck)
                                    {
                                        sentReadyAck = true;
                                        std::vector<bf2bot::BitStream> evs;
                                        bf2bot::BitStream ack(16u);
                                        ack.writeUInt(bf2bot::GE_POST_REMOTE, bf2bot::kGameEventTypeBits);
                                        ack.writeUInt(bf2bot::EC_NETWORK, 4);
                                        ack.writeUInt(bf2bot::NRE_CLIENT_READY_ACK, 32);
                                        ack.writeFloat(0.0f);
                                        ack.writeUInt(0u, 8);
                                        evs.push_back(ack);
                                        sendSrvEvents(evs);
                                    }
                                }
                                else if (eid == bf2bot::NRE_JOIN_TEAM) gotJoinTeam = true;
                                else if (eid == bf2bot::NRE_SELECT_KIT) gotSelectKit = true;
                                else if (eid == bf2bot::NRE_SELECT_SPAWN_POINT) gotSelectSpawn = true;
                                else if (eid == bf2bot::NRE_END_OF_ROUND_ACK) gotEndOfRoundAck = true;
                                else if (eid == bf2bot::NRE_CONFIRM_RESET) gotConfirmReset = true;
                            }
                        }
                    }
                }
            }
        }
    };

    // Drive 60 virtual 30 Hz ticks (2.0s) with simulated 30ms RTT +/- 6ms jitter
    uint64_t nowUs = 1000000ULL;
    for (int tick = 0; tick < 60; ++tick)
    {
        while (!s2cQueue.empty())
        {
            const std::vector<uint8_t>& p = s2cQueue.front();
            bot.injectRawDatagram(&p[0], p.size(), nowUs);
            s2cQueue.pop_front();
        }
        bot.flushImpairmentQueues(nowUs);
        bot.tick30Hz(nowUs, 1.0f / 30.0f);
        drainServerSocket();

        // Once bot joined team/kit/spawn, send a ghost snapshot spawning bot & showing a nearby enemy
        if (gotSelectSpawn && !sentSpawnGhost)
        {
            sentSpawnGhost = true;
            bf2bot::BitStream pkt(bf2bot::kMaxDataPayloadSize);
            pkt.writeBool(false); // PlayerActionManager
            pkt.writeBool(false); // GameEventManager
            pkt.writeBool(true);  // GhostManager
            pkt.writeUInt(100u, 32);
            pkt.writeUInt(1u, 8); // 1 ghost (enemy soldier 710)
            pkt.writeBool(true);  // hasControlState

            // ControlObjectState for own soldier (netId=700, pos=(10,2,10), hp=255)
            bf2bot::BitStream ctrl(256u);
            ctrl.writeSignedMag(0, 32);
            ctrl.writeFloat(10.0f);
            ctrl.writeFloat(2.0f);
            ctrl.writeFloat(10.0f);
            ctrl.setCompressionVector(bf2bot::Vec3(10.0f, 2.0f, 10.0f));
            ctrl.writeUInt(700u, 16);
            ctrl.writeBool(false); // hasParachute
            ctrl.writeBool(true);  // isSoldier
            ctrl.writeBool(false); // hasKit
            ctrl.writeBool(false); // isDelta
            ctrl.writeUInt(0u, 3); // baseline slot
            writeSoldierPayload(ctrl, true, 255u, 0u,
                                bf2bot::Vec3(10.0f, 2.0f, 10.0f),
                                bf2bot::Vec3(10.0f, 2.0f, 10.0f),
                                0.0f, 0.0f, true);

            pkt.writeUInt(ctrl.getBitSize(), 12);
            pkt.writeBits(ctrl.data(), ctrl.getBitSize());

            // Enemy soldier 710 at (10, 2, 25) -> 15m away
            bf2bot::BitStream enemy(256u);
            enemy.setCompressionVector(bf2bot::Vec3(10.0f, 2.0f, 10.0f));
            writeSoldierPayload(enemy, false, 255u, 0u,
                                bf2bot::Vec3(10.0f, 2.0f, 25.0f),
                                bf2bot::Vec3(10.0f, 2.0f, 10.0f),
                                180.0f, 0.0f, true);

            pkt.writeUInt(1u, 2);
            pkt.writeUInt(710u, 16);
            pkt.writeBool(true);
            pkt.writeUInt(enemy.getBitSize(), 11);
            pkt.writeBits(enemy.data(), enemy.getBitSize());

            sendSrvDataPayload(pkt.data(), pkt.getByteSize());
        }
        else if (sentSpawnGhost && gotPlayerAction && !sentRagdollGhost)
        {
            // Send ragdoll update for enemy 710 with ONLY mask 0x8000 (no pose bit 0x20!)
            sentRagdollGhost = true;
            bf2bot::BitStream pkt(bf2bot::kMaxDataPayloadSize);
            pkt.writeBool(false); // PlayerActionManager
            pkt.writeBool(false); // GameEventManager
            pkt.writeBool(true);  // GhostManager
            pkt.writeUInt(101u, 32);
            pkt.writeUInt(1u, 8);
            pkt.writeBool(false);

            bf2bot::BitStream rag(32u);
            rag.writeUInt(0x8000u, 21); // ragdoll only, no pose bit
            pkt.writeUInt(1u, 2);
            pkt.writeUInt(710u, 16);
            pkt.writeBool(true);
            pkt.writeUInt(rag.getBitSize(), 11);
            pkt.writeBits(rag.data(), rag.getBitSize());

            sendSrvDataPayload(pkt.data(), pkt.getByteSize());
        }
        else if (sentRagdollGhost && !sentEorAndReset)
        {
            sentEorAndReset = true;
            std::vector<bf2bot::BitStream> evs;
            bf2bot::BitStream eor(16u);
            eor.writeUInt(bf2bot::GE_END_OF_ROUND, bf2bot::kGameEventTypeBits);
            eor.writeUInt(1u, 32);
            eor.writeUInt(1u, 32);
            eor.writeUInt(0u, 32);
            evs.push_back(eor);

            bf2bot::BitStream rst(16u);
            rst.writeUInt(bf2bot::GE_POST_REMOTE, bf2bot::kGameEventTypeBits);
            rst.writeUInt(bf2bot::EC_NETWORK, 4);
            rst.writeUInt(bf2bot::NRE_CONFIRM_RESET, 32);
            rst.writeFloat(0.0f);
            rst.writeUInt(0u, 8);
            evs.push_back(rst);
            sendSrvEvents(evs);
        }

        nowUs += 33333ULL;
    }

    const bool wasInGame = bot.isInGame();
    const bool wasSpawned = bot.isSpawned();
    const bool wasAlive = bot.isAlive();

    bot.disconnectGracefully();
    drainServerSocket();

    // Expected djb2LowerName("LoadBot_01")
    uint32_t expectedDjb2 = 5381u;
    const char* lowerName = "loadbot_01";
    for (const char* p = lowerName; *p != '\0'; ++p)
    {
        expectedDjb2 = static_cast<uint32_t>(static_cast<unsigned char>(*p)) ^ (33u * expectedDjb2);
    }

    checkCond(gotConnectReq && gotConnectAck, "Loopback UDP ConnectRequest -> ConnectAccept -> Ack");
    checkCond(gotChallengeResp, "Loopback Challenge -> ChallengeResponse");
    checkCond(clientInfoCount == 1 && clientInfoHash == expectedDjb2,
              "Loopback ClientInfo sent once with djb2LowerName (ignores ServerInfo reqResp=false)");
    checkCond(gotMapLoaded && gotClientReady && wasInGame,
              "Loopback MapInfo -> MapLoaded -> DBComplete -> ClientReady -> InGame");
    checkCond(gotConnType && gotJoinTeam && gotSelectKit && gotSelectSpawn,
              "Loopback ClientReadyAck -> ConnectionType/JoinTeam/SelectKit/SelectSpawnPoint");
    checkCond(wasSpawned && wasAlive && gotPlayerAction,
              "Loopback ControlObjectState spawn -> 30Hz PlayerAction transmitted");
    checkCond(gotEndOfRoundAck && gotConfirmReset,
              "Loopback EndOfRoundAck(20) and ConfirmReset(5) acknowledged");
    checkCond(gotDisconnect, "Loopback graceful disconnect transmits UDP_DISCONNECT");
    checkCond(telemetry.getTotalSummary().protocolErrors == 0u,
              "Loopback session completed with zero protocolErrors");
    if (!(gotMapLoaded && gotClientReady && wasInGame && gotSelectSpawn && gotEndOfRoundAck))
    {
        std::fprintf(stderr, "  [DIAG] mapLoaded=%d clientReady=%d inGame=%d connType=%d joinTeam=%d kit=%d spawn=%d spawned=%d alive=%d act=%d eor=%d rst=%d\n",
                     gotMapLoaded ? 1 : 0, gotClientReady ? 1 : 0, wasInGame ? 1 : 0,
                     gotConnType ? 1 : 0, gotJoinTeam ? 1 : 0, gotSelectKit ? 1 : 0, gotSelectSpawn ? 1 : 0,
                     wasSpawned ? 1 : 0, wasAlive ? 1 : 0, gotPlayerAction ? 1 : 0,
                     gotEndOfRoundAck ? 1 : 0, gotConfirmReset ? 1 : 0);
    }

    bot.closeSocket();
#ifdef _WIN32
    ::WSACleanup();
#endif
}

void testHumanAim()
{
    using namespace bf2bot;

    // --- interpolation at (now - interp) ---
    TrackedSoldier s;
    Vec3 q;
    checkCond(!interpolateSoldierPos(s, 1000, q), "aim: no history -> false");
    for (int i = 0; i < 20; ++i)
    {
        // 20 samples, 50 ms apart, x moves 1 m per sample; ring holds only the last 12.
        s.pushPos(1000000ULL + static_cast<uint64_t>(i) * 50000ULL, Vec3(static_cast<float>(i), 0.0f, 0.0f));
    }
    checkCond(s.histCount == TrackedSoldier::kHistSize, "aim: ring buffer capped");
    Vec3 p;
    const uint64_t newest = 1000000ULL + 19ULL * 50000ULL;
    checkCond(interpolateSoldierPos(s, newest, p) && nearFloat(p.x, 19.0f, 1e-4f), "aim: newest at newest time");
    checkCond(interpolateSoldierPos(s, newest + 500000ULL, p) && nearFloat(p.x, 19.0f, 1e-4f),
              "aim: no extrapolation past newest");
    // now - 100ms = exactly two samples back
    checkCond(interpolateSoldierPos(s, newest - 100000ULL, p) && nearFloat(p.x, 17.0f, 1e-4f),
              "aim: now-100ms lands two samples back");
    // between samples: 25 ms after sample 16 -> 16.5
    checkCond(interpolateSoldierPos(s, 1000000ULL + 16ULL * 50000ULL + 25000ULL, p) && nearFloat(p.x, 16.5f, 1e-3f),
              "aim: linear interpolation between samples");
    checkCond(interpolateSoldierPos(s, 0, p) && nearFloat(p.x, 8.0f, 1e-4f), "aim: clamps to oldest retained");
    s.pushPos(newest + 50000ULL, Vec3(500.0f, 0.0f, 0.0f)); // teleport
    checkCond(s.histCount == 1u, "aim: teleport clears history");

    // --- aim error convergence ---
    const float e0 = 4.0f;
    const uint64_t conv = 450000ULL;
    float prev = e0;
    bool monotone = true;
    for (uint64_t t = 0; t <= 600000ULL; t += 30000ULL)
    {
        const float e = aimErrorDecay(e0, t, conv);
        if (e > prev + 1e-6f) monotone = false;
        prev = e;
    }
    checkCond(monotone, "aim: error decays monotonically");
    checkCond(nearFloat(aimErrorDecay(e0, 0, conv), e0, 1e-6f), "aim: error at t=0 is initial");
    checkCond(aimErrorDecay(e0, conv, conv) < e0 * 0.06f, "aim: error <6% at convergence time");

    // --- brain: reaction delay, fire discipline, convergence ---
    for (int legacy = 0; legacy < 2; ++legacy)
    {
        AimConfig cfg;
        cfg.legacy = (legacy != 0);
        BotBrain brain(0u, 42u, nullptr, nullptr, cfg);
        std::map<uint16_t, TrackedSoldier> soldiers;
        TrackedSoldier en;
        en.netId = 7;
        en.team = 2;
        en.hasPos = true;
        en.pos = Vec3(0.0f, 0.0f, 30.0f);
        soldiers[7] = en;

        BotWorldView w;
        w.ownTeam = brain.assignedTeam();
        w.ownAlive = true;
        w.hasOwnPos = true;
        w.ownSoldierNetId = 1;
        w.soldiers = &soldiers;

        const uint64_t t0 = 5000000ULL;
        brain.onSpawned(w.ownPos, t0);
        uint64_t firstFireUs = 0;
        bool movedWhileFiring = false;
        int burstLen = 0;
        int maxBurstTicks = 0;
        for (int tick = 0; tick < 150; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            soldiers[7].pushPos(now, soldiers[7].pos);
            BotTickOutput out;
            brain.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            w.ownPitchDegrees += out.action.axes[5] * 5.0f;
            const bool fire = (out.action.buttonMaskLow & BTN_FIRE) != 0u;
            if (fire)
            {
                if (firstFireUs == 0) firstFireUs = now - t0;
                if (!cfg.legacy &&
                    (out.action.axes[0] != 0.0f || out.action.axes[3] != 0.0f)) movedWhileFiring = true;
                ++burstLen;
                if (burstLen > maxBurstTicks) maxBurstTicks = burstLen;
            }
            else
            {
                burstLen = 0;
            }
        }
        if (!cfg.legacy)
        {
            checkCond(firstFireUs >= 100000ULL, "aim: reaction delay before first shot");
            checkCond(firstFireUs < 1500000ULL, "aim: eventually fires");
            checkCond(!movedWhileFiring, "aim: no forward/strafe input while firing");
            checkCond(maxBurstTicks <= 18, "aim: bursts are short (3-5 shots, ~100ms each)");
            checkCond(std::fabs(w.ownFacingDegrees) < 6.0f, "aim: yaw converged on target");
        }
        else
        {
            checkCond(firstFireUs != 0, "aim: legacy still fires");
        }
    }
}

void testSmartTacticalBots()
{
    using namespace bf2bot;

    // 1. Stationary lookLeftRightAngle + 90ms RTT + ControlObjectState baseline delta simulation!
    // Models real BF2 Soldier.cpp yaw pipeline: standing still modifies m_lookLeftRightAngle
    // inside +/-40 deg without moving m_facingDegrees, plus PIAltFire zoom toggle & mouseSpeedMod=0.5.
    {
        sockaddr_in srvAddr;
        std::memset(&srvAddr, 0, sizeof(srvAddr));
        srvAddr.sin_family = AF_INET;
        srvAddr.sin_port = htons(16567u);
        ::inet_pton(AF_INET, "127.0.0.1", &srvAddr.sin_addr);

        LoadBotConfig cfg;
        cfg.botIndex = 0u;
        cfg.seed = 777u;
        cfg.worldSize = 1024.0f;
        cfg.aim.skill = 0.9f;
        cfg.aim.interpMs = 100;
        cfg.uplinkImpair.oneWayDelayMs = 45.0;   // 90 ms RTT!
        cfg.downlinkImpair.oneWayDelayMs = 45.0;

        Telemetry tel;
        TeamTactics tactics;
        LoadBotClient bot(cfg, srvAddr, nullptr, nullptr, &tel, &tactics);

        // Simulated server soldier state matching Soldier.cpp:3820-3915
        float srvFacing = 0.0f;
        float srvLookLR = 0.0f;
        float srvPendingTurn = 0.0f;
        float srvPitch = 0.0f;
        uint8_t srvZoom = 0u;
        uint8_t srvPose = 0u;
        int32_t srvLastInputIdx = 0;
        int32_t srvBaseSlot = 0;
        float baseFacing = 0.0f;
        float baseLookLR = 0.0f;
        float basePitch = 0.0f;
        bool hasBaseline = false;

        // Enemy at 25m bearing +25 deg right (strictly inside +/-40 deg stationary lookLimit!)
        const float kRad = 3.14159265358979323846f / 180.0f;
        const Vec3 enemyPos(25.0f * std::sin(25.0f * kRad), 0.0f, 25.0f * std::cos(25.0f * kRad));

        // Complete minimal handshake directly via sink callbacks? Better: test full wire through
        // injectRawDatagram/rawSendHook or direct sink+tick! Let's drive real wire packets via hook!
        std::deque<std::vector<uint8_t> > c2sQ;
        std::deque<std::vector<uint8_t> > s2cQ;
        bot.setRawSendHook([&](const uint8_t* d, size_t l) {
            c2sQ.push_back(std::vector<uint8_t>(d, d + l));
        });

        SWConnectionState srvConn;
        srvConn.setAssignedConnId(9u);
        uint32_t srvEvBlock = 0u;
        uint32_t srvDeliv = 1u;

        auto sendSrvRaw = [&](const uint8_t* d, size_t l) {
            s2cQ.push_back(std::vector<uint8_t>(d, d + l));
        };
        auto sendSrvPayload = [&](const uint8_t* pl, size_t plLen) {
            uint8_t w[kMaxDatagramSize];
            size_t wl = 0;
            if (srvConn.wrapDataPacket(pl, static_cast<uint16_t>(plLen), srvDeliv++, w, sizeof(w), wl))
                sendSrvRaw(w, wl);
        };
        auto sendSrvEv = [&](const std::vector<BitStream>& evs) {
            BitStream pkt(kMaxDataPayloadSize);
            pkt.writeBool(false);
            pkt.writeBool(true);
            pkt.writeUInt(static_cast<uint32_t>(evs.size()), 8);
            pkt.writeUInt(srvEvBlock++ & 31u, 5);
            pkt.writeBool(false);
            for (size_t i = 0; i < evs.size(); ++i)
                pkt.writeBits(evs[i].data(), evs[i].getBitSize());
            pkt.writeBool(false);
            sendSrvPayload(pkt.data(), pkt.getByteSize());
        };

        auto writeFullCtrlSnapshot = [&](BitStream& pkt, uint32_t tickNum, bool sendDelta) {
            pkt.writeBool(false); // PlayerActionManager
            pkt.writeBool(false); // GameEventManager
            pkt.writeBool(true);  // GhostManager
            pkt.writeUInt(tickNum, 32);
            pkt.writeUInt(1u, 8); // 1 remote enemy ghost
            pkt.writeBool(true);  // hasControlState

            BitStream ctrl(256u);
            ctrl.writeSignedMag(srvLastInputIdx, 32);
            ctrl.writeFloat(0.0f); ctrl.writeFloat(0.0f); ctrl.writeFloat(0.0f);
            ctrl.setCompressionVector(Vec3(0.0f, 0.0f, 0.0f));
            ctrl.writeUInt(700u, 16);
            ctrl.writeBool(false); // hasParachute
            ctrl.writeBool(true);  // isSoldier
            ctrl.writeBool(false); // hasKit

            if (!sendDelta || !hasBaseline)
            {
                hasBaseline = true;
                baseFacing = srvFacing;
                baseLookLR = srvLookLR;
                basePitch = srvPitch;
                ctrl.writeBool(false);
                ctrl.writeUInt(static_cast<uint32_t>(srvBaseSlot & 7), 3);

                uint32_t mask = 0x40u | 0x20u | 0x01u | 0x02u | 0x04u | 0x08u | 0x10u;
                ctrl.writeUInt(mask, 21);
                ctrl.writeUInt(255u, 8); ctrl.writeUInt(0u, 1);
                ctrl.writeUInt(srvPose, 3); ctrl.writeUInt(srvPose, 3);
                ctrl.writeCompressedVector(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 0.0f), 0.001f);
                ctrl.writeFloat(srvFacing);
                ctrl.writeFloat(srvLookLR);
                ctrl.writeFloat(srvPendingTurn);
                ctrl.writeFloat(srvPitch);
                ctrl.writeUInt(srvZoom, 3);
                ctrl.writeUInt(0u, 2);
                ctrl.writeBool(false); ctrl.writeBool(false); ctrl.writeBool(false);
                ctrl.writeBool(true); // grounded
            }
            else
            {
                ctrl.writeBool(true);
                ctrl.writeUInt(static_cast<uint32_t>(srvBaseSlot & 7), 3);
                uint32_t mask = 0u;
                if (std::fabs(srvFacing - baseFacing) > 1e-4f) mask |= 0x02u;
                if (std::fabs(srvLookLR - baseLookLR) > 1e-4f) mask |= 0x04u;
                if (std::fabs(srvPitch - basePitch) > 1e-4f)   mask |= 0x10u;
                ctrl.writeUInt(mask, 21);
                if (mask & 0x02u) ctrl.writeFloat(srvFacing);
                if (mask & 0x04u) ctrl.writeFloat(srvLookLR);
                if (mask & 0x10u) ctrl.writeFloat(srvPitch);
                ctrl.writeUInt(srvZoom, 3);
                ctrl.writeUInt(0u, 2);
                ctrl.writeBool(false); ctrl.writeBool(false); ctrl.writeBool(false);
                ctrl.writeBool(true);
            }

            pkt.writeUInt(ctrl.getBitSize(), 12);
            pkt.writeBits(ctrl.data(), ctrl.getBitSize());

            // Enemy soldier 710 at enemyPos
            BitStream en(256u);
            en.setCompressionVector(Vec3(0.0f, 0.0f, 0.0f));
            writeSoldierPayload(en, false, 255u, 0u, enemyPos, Vec3(0.0f, 0.0f, 0.0f), 205.0f, 0.0f, true);
            pkt.writeUInt(1u, 2);
            pkt.writeUInt(710u, 16);
            pkt.writeBool(true);
            pkt.writeUInt(en.getBitSize(), 11);
            pkt.writeBits(en.data(), en.getBitSize());
        };

        bool inGameSent = false;
        int onTargetFireTicks = 0;
        int totalFireTicks = 0;
        bool sawZoomToggle = false;
        float maxOvershootAfterLock = 0.0f;
        bool lockedOnce = false;
        uint64_t nowUs = 1000000ULL;

        for (int tick = 0; tick < 180; ++tick)
        {
            while (!s2cQ.empty())
            {
                const std::vector<uint8_t>& p = s2cQ.front();
                bot.injectRawDatagram(&p[0], p.size(), nowUs);
                s2cQ.pop_front();
            }
            bot.flushImpairmentQueues(nowUs);
            bot.tick30Hz(nowUs, 1.0f / 30.0f);

            while (!c2sQ.empty())
            {
                const std::vector<uint8_t> pkt = c2sQ.front();
                c2sQ.pop_front();
                ParsedDatagram dg;
                if (!parseIncomingDatagram(&pkt[0], pkt.size(), dg)) continue;
                if (dg.type == UDP_CONNECT_REQUEST)
                {
                    uint8_t acc[16]; std::memset(acc, 0, sizeof(acc));
                    BitStream bs(acc, sizeof(acc));
                    bs.writeUInt(UDP_CONNECT_ACCEPT, 4);
                    bs.writeUInt(0u, 8); bs.writeUInt(9u, 8); bs.writeUInt(1000u, 32); bs.writeBool(false);
                    sendSrvRaw(acc, bs.getByteSize());
                }
                else if (dg.type == UDP_CONNECT_ACCEPT_ACK && !inGameSent)
                {
                    inGameSent = true;
                    std::vector<BitStream> evs;
                    // Create own player (id=1, team=1), enemy player (id=2, team=2), EnterVehicle, DBComplete, ReadyAck
                    BitStream cp1(64u);
                    cp1.writeUInt(GE_CREATE_PLAYER, kGameEventTypeBits);
                    cp1.writeUInt(1u, 3); cp1.writeUInt(0u, 4); cp1.writeBool(false);
                    cp1.writeUInt(1u, 8); cp1.writeUInt(2u, 16); cp1.writeUInt(0u, 16); cp1.writeBool(false);
                    char n1[32]; std::memset(n1, 0, 32); std::memcpy(n1, "LoadBot_01", 10);
                    cp1.writeFixedBytes(n1, 32u);
                    evs.push_back(cp1);

                    BitStream cp2(64u);
                    cp2.writeUInt(GE_CREATE_PLAYER, kGameEventTypeBits);
                    cp2.writeUInt(2u, 3); cp2.writeUInt(0u, 4); cp2.writeBool(true);
                    cp2.writeUInt(2u, 8); cp2.writeUInt(3u, 16); cp2.writeUInt(0u, 16); cp2.writeBool(false);
                    char n2[32]; std::memset(n2, 0, 32); std::memcpy(n2, "Enemy_01", 8);
                    cp2.writeFixedBytes(n2, 32u);
                    evs.push_back(cp2);

                    BitStream ev1(16u);
                    ev1.writeUInt(GE_ENTER_VEHICLE, kGameEventTypeBits);
                    ev1.writeUInt(1u, 8); ev1.writeUInt(700u, 16); ev1.writeBool(true);
                    evs.push_back(ev1);

                    BitStream ev2(16u);
                    ev2.writeUInt(GE_ENTER_VEHICLE, kGameEventTypeBits);
                    ev2.writeUInt(2u, 8); ev2.writeUInt(710u, 16); ev2.writeBool(true);
                    evs.push_back(ev2);

                    BitStream rdy(16u);
                    rdy.writeUInt(GE_POST_REMOTE, kGameEventTypeBits);
                    rdy.writeUInt(EC_NETWORK, 4); rdy.writeUInt(NRE_CLIENT_READY, 32);
                    rdy.writeFloat(0.0f); rdy.writeUInt(0u, 8);
                    evs.push_back(rdy);

                    BitStream ack(16u);
                    ack.writeUInt(GE_POST_REMOTE, kGameEventTypeBits);
                    ack.writeUInt(EC_NETWORK, 4); ack.writeUInt(NRE_CLIENT_READY_ACK, 32);
                    ack.writeFloat(0.0f); ack.writeUInt(0u, 8);
                    evs.push_back(ack);

                    sendSrvEv(evs);
                }
                else if (dg.type == UDP_DATA_PACKET)
                {
                    if (!srvConn.acceptIncomingDataSeq(dg.seq)) continue;
                    srvConn.handleRemoteAck(dg.ack, dg.ackMask, PacketStatusCallback());

                    BitStream bs(dg.payload, dg.payloadLen);
                    const bool hasAct = bs.readBool();
                    if (hasAct && !bs.hasError())
                    {
                        const uint32_t cnt = bs.readUInt(4);
                        bs.readUInt(9);
                        if (cnt > 0u)
                        {
                            const int32_t baseIdx = bs.readSignedMag(32);
                            for (uint32_t i = 0; i < cnt; ++i)
                            {
                                float ax[6];
                                for (int a = 0; a < 6; ++a)
                                    ax[a] = static_cast<float>(bs.readSignedMag(16)) / 100.0f;
                                const uint32_t btnLow = bs.readUInt(32);
                                bs.readUInt(9);
                                bs.readBool();

                                const int32_t idx = baseIdx + static_cast<int32_t>(i);
                                if (idx <= srvLastInputIdx) continue;
                                srvLastInputIdx = idx;

                                // Real BF2 Soldier.cpp yaw/pitch/zoom pipeline!
                                if ((btnLow & BTN_ALT_FIRE) != 0u)
                                {
                                    srvZoom = (srvZoom == 0u) ? 1u : 0u;
                                    sawZoomToggle = true;
                                }
                                srvPose = ((btnLow & BTN_CROUCH) != 0u) ? 1u : 0u;
                                const float mod = (srvZoom > 0u) ? 0.5f : 1.0f;
                                const float yawDelta = ax[4] * 5.0f * mod;
                                const float pitchDelta = ax[5] * 5.0f * mod;
                                srvPitch += pitchDelta;
                                if (srvPitch > 85.0f) srvPitch = 85.0f;
                                if (srvPitch < -85.0f) srvPitch = -85.0f;

                                float appliedYaw = yawDelta;
                                if (std::fabs(ax[3]) > 0.01f)
                                {
                                    if (srvLookLR != 0.0f) srvPendingTurn = srvLookLR;
                                }
                                else
                                {
                                    srvLookLR += yawDelta;
                                    if (srvLookLR >= 40.0f || srvLookLR <= -40.0f)
                                        srvPendingTurn = srvLookLR;
                                    else
                                        appliedYaw = 0.0f;
                                }
                                if (std::fabs(srvPendingTurn) > 0.0f)
                                {
                                    const float sgn = (srvPendingTurn > 0.0f) ? 1.0f : -1.0f;
                                    const float step = (sgn > 0.0f)
                                        ? (4.0f < srvPendingTurn ? 4.0f : srvPendingTurn)
                                        : (-4.0f > srvPendingTurn ? -4.0f : srvPendingTurn);
                                    srvPendingTurn -= step;
                                    srvLookLR -= step;
                                    appliedYaw += step;
                                }
                                srvFacing += appliedYaw;

                                const float totalYaw = srvFacing + srvLookLR;
                                const float yawErr = std::fabs(totalYaw - 25.0f);
                                if (yawErr < 3.5f) lockedOnce = true;
                                if (lockedOnce && yawErr > maxOvershootAfterLock)
                                    maxOvershootAfterLock = yawErr;
                                if ((btnLow & BTN_FIRE) != 0u)
                                {
                                    ++totalFireTicks;
                                    if (yawErr < 4.5f) ++onTargetFireTicks;
                                    // Simulate BF2 upward rifle recoil kick (-1.0 deg pitch per fire tick)
                                    srvPitch -= 1.0f;
                                    if (srvPitch < -85.0f) srvPitch = -85.0f;
                                }
                            }
                        }
                    }
                }
            }

            if (inGameSent && tick >= 12)
            {
                BitStream snap(kMaxDataPayloadSize);
                // Alternate baseline & delta packets (baseline every 12 ticks, deltas in between)
                const bool sendDelta = ((tick - 12) % 12 != 0);
                writeFullCtrlSnapshot(snap, static_cast<uint32_t>(100 + tick), sendDelta);
                sendSrvPayload(snap.data(), snap.getByteSize());
            }
            nowUs += 33333ULL;
        }

        const float finalAimYaw = srvFacing + srvLookLR;
        checkCond(std::fabs(finalAimYaw - 25.0f) < 4.0f,
                  "smart aim: stationary lookLeftRightAngle + 90ms RTT converges on +25 deg target");
        checkCond(lockedOnce && maxOvershootAfterLock < 6.0f,
                  "smart aim: zero ping-induced oscillation after locking on target");
        checkCond(totalFireTicks >= 15 && onTargetFireTicks >= 12,
                  "smart aim: fires accurately at 90ms RTT with delta baselines");
        checkCond(std::fabs(srvPitch) < 6.0f,
                  "smart aim: actively compensates vertical rifle recoil under 90ms RTT");
        checkCond(sawZoomToggle, "smart aim: engages ADS zoom toggle at 25m");
    }

    // 2. Target velocity leading test (enemy sprinting laterally at +6 m/s at 45m).
    {
        AimConfig cfg;
        cfg.skill = 1.0f;
        cfg.interpMs = 100;
        BotBrain brain(0u, 101u, nullptr, nullptr, cfg);

        std::map<uint16_t, TrackedSoldier> soldiers;
        TrackedSoldier& en = soldiers[9];
        en.netId = 9;
        en.team = 2;
        en.hasPos = true;

        BotWorldView w;
        w.ownTeam = 1;
        w.ownAlive = true;
        w.hasOwnPos = true;
        w.ownSoldierNetId = 1;
        w.soldiers = &soldiers;

        const uint64_t t0 = 5000000ULL;
        brain.onSpawned(w.ownPos, t0);
        for (int tick = 0; tick < 60; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            const float sec = static_cast<float>(tick) * (1.0f / 30.0f);
            en.pos = Vec3(-6.0f + 6.0f * sec, 0.0f, 45.0f);
            en.speed = Vec3(6.0f, 0.0f, 0.0f);
            en.pushPos(now, en.pos);

            BotTickOutput out;
            brain.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            w.ownPitchDegrees += out.action.axes[5] * 5.0f;
        }
        Vec3 rendered;
        const uint64_t lastNow = t0 + 59ULL * 33333ULL;
        interpolateSoldierPos(en, lastNow - 100000ULL, rendered);
        const float renderedBearing = std::atan2(rendered.x, rendered.z) * (180.0f / 3.14159265f);
        checkCond(w.ownFacingDegrees > renderedBearing + 0.10f,
                  "smart aim: leads laterally sprinting target ahead of rendered position");
    }

    // 3. 360-degree damage reaction & gunshot perception test.
    {
        AimConfig cfg;
        cfg.skill = 0.9f;
        BotBrain brain(0u, 202u, nullptr, nullptr, cfg);

        std::map<uint16_t, TrackedSoldier> soldiers;
        TrackedSoldier& behind = soldiers[12];
        behind.netId = 12;
        behind.team = 2;
        behind.hasPos = true;
        behind.pos = Vec3(0.0f, 0.0f, -25.0f); // directly behind (+180 deg)
        behind.facingDegrees = 0.0f;           // aiming right at our back

        BotWorldView w;
        w.ownTeam = 1;
        w.ownAlive = true;
        w.hasOwnPos = true;
        w.ownSoldierNetId = 1;
        w.ownFacingDegrees = 0.0f;
        w.ownHitPointsByte = 255u;
        w.soldiers = &soldiers;

        const uint64_t t0 = 5000000ULL;
        brain.onSpawned(w.ownPos, t0);
        behind.pushPos(t0, behind.pos);
        BotTickOutput out0;
        brain.tick(t0, 1.0f / 30.0f, w, out0);
        checkCond((out0.action.buttonMaskLow & BTN_FIRE) == 0u,
                  "awareness: silent enemy 25m directly behind is not seen initially");

        // Now bot takes bullet damage!
        w.ownHitPointsByte = 180u;
        bool turnedAndFired = false;
        for (int tick = 1; tick < 80; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            behind.pushPos(now, behind.pos);
            BotTickOutput out;
            brain.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            while (w.ownFacingDegrees > 180.0f) w.ownFacingDegrees -= 360.0f;
            while (w.ownFacingDegrees < -180.0f) w.ownFacingDegrees += 360.0f;
            if ((out.action.buttonMaskLow & BTN_FIRE) != 0u && std::fabs(w.ownFacingDegrees) > 165.0f)
            {
                turnedAndFired = true;
                break;
            }
        }
        checkCond(turnedAndFired, "awareness: taking damage triggers 360-deg spin and return fire");

        // Gunshot hearing test: separate bot hears actively firing enemy 30m behind!
        BotBrain brain2(2u, 203u, nullptr, nullptr, cfg);
        behind.pos = Vec3(0.0f, 0.0f, -30.0f);
        behind.isFiring = true;
        behind.lastFiredUs = t0;
        w.ownFacingDegrees = 0.0f;
        w.ownHitPointsByte = 255u;
        brain2.onSpawned(w.ownPos, t0);
        bool heardAndEngaged = false;
        for (int tick = 0; tick < 80; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            behind.lastFiredUs = now;
            behind.pushPos(now, behind.pos);
            BotTickOutput out;
            brain2.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            while (w.ownFacingDegrees > 180.0f) w.ownFacingDegrees -= 360.0f;
            while (w.ownFacingDegrees < -180.0f) w.ownFacingDegrees += 360.0f;
            if ((out.action.buttonMaskLow & BTN_FIRE) != 0u && std::fabs(w.ownFacingDegrees) > 165.0f)
            {
                heardAndEngaged = true;
                break;
            }
        }
        checkCond(heardAndEngaged, "awareness: actively firing enemy 30m behind is heard and engaged");
    }

    // 4. Threat prioritization: active shooter facing us beats closer unaware enemy.
    {
        AimConfig cfg;
        cfg.skill = 0.9f;
        BotBrain brain(0u, 303u, nullptr, nullptr, cfg);

        std::map<uint16_t, TrackedSoldier> soldiers;
        // Enemy A: 24m left-ish (-20 deg), facing AWAY, not firing, full HP
        TrackedSoldier& a = soldiers[20];
        a.netId = 20; a.team = 2; a.hasPos = true; a.hitPointsByte = 255u;
        a.pos = Vec3(-8.0f, 0.0f, 22.6f); a.facingDegrees = -20.0f;
        // Enemy B: 27m right-ish (+22 deg), facing RIGHT AT US, firing, low HP
        TrackedSoldier& b = soldiers[21];
        b.netId = 21; b.team = 2; b.hasPos = true; b.hitPointsByte = 90u;
        b.pos = Vec3(10.0f, 0.0f, 25.0f); b.facingDegrees = -158.0f;
        b.isFiring = true;

        BotWorldView w;
        w.ownTeam = 1; w.ownAlive = true; w.hasOwnPos = true; w.ownSoldierNetId = 1;
        w.soldiers = &soldiers;
        const uint64_t t0 = 5000000ULL;
        brain.onSpawned(w.ownPos, t0);
        for (int tick = 0; tick < 25; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            b.lastFiredUs = now;
            a.pushPos(now, a.pos);
            b.pushPos(now, b.pos);
            BotTickOutput out;
            brain.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
        }
        checkCond(w.ownFacingDegrees > 12.0f,
                  "tactics: prioritizes active firing threat facing us over slightly closer unaware enemy");
    }

    // 5. Medic heal & revive, Support ammo drop, and guaranteed return to Primary (slot 3).
    {
        // Medic bot (kit 5): botIndex=3 has kInfantryKits[3]=5
        BotBrain medic(3u, 404u, nullptr, nullptr);
        BotWorldView w;
        w.ownTeam = 2; w.ownAlive = true; w.hasOwnPos = true; w.ownSoldierNetId = 50;
        w.ownKit = 5;
        w.ownHitPointsByte = 110u; // wounded!

        const uint64_t t0 = 10000000ULL;
        medic.onSpawned(w.ownPos, t0);
        bool selectedMedkit = false;
        bool droppedMedkit = false;
        bool restoredPrimary = false;
        for (int tick = 0; tick < 160; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            BotTickOutput out;
            medic.tick(now, 1.0f / 30.0f, w, out);
            if ((out.action.buttonMaskLow & BTN_WEAPON_5) != 0u) selectedMedkit = true;
            if (selectedMedkit && (out.action.buttonMaskLow & BTN_FIRE) != 0u) droppedMedkit = true;
            if (droppedMedkit && (out.action.buttonMaskLow & BTN_WEAPON_3) != 0u) restoredPrimary = true;
        }
        checkCond(selectedMedkit && droppedMedkit && restoredPrimary,
                  "class AI: wounded Medic switches to medkit (slot 5), drops it, restores primary (slot 3)");

        // Medic revive test: downed teammate 6m ahead
        BotBrain medic2(3u, 405u, nullptr, nullptr);
        std::map<uint8_t, TrackedPlayer> players;
        std::map<uint16_t, TrackedSoldier> soldiers;
        TrackedPlayer& downed = players[7];
        downed.playerId = 7; downed.team = 2; downed.isManDown = true; downed.soldierNetId = 88;
        TrackedSoldier& body = soldiers[88];
        body.netId = 88; body.team = 2; body.hasPos = true; body.pos = Vec3(0.0f, 0.0f, 1.5f);

        w.ownHitPointsByte = 255u;
        w.players = &players;
        w.soldiers = &soldiers;
        medic2.onSpawned(w.ownPos, t0);
        bool selectedDefib = false;
        bool pulsedDefib = false;
        bool restoredAfterRevive = false;
        for (int tick = 0; tick < 180; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            BotTickOutput out;
            medic2.tick(now, 1.0f / 30.0f, w, out);
            if ((out.action.buttonMaskLow & BTN_WEAPON_6) != 0u) selectedDefib = true;
            if (selectedDefib && (out.action.buttonMaskLow & BTN_FIRE) != 0u)
            {
                pulsedDefib = true;
                downed.isManDown = false; // revived!
            }
            if (pulsedDefib && (out.action.buttonMaskLow & BTN_WEAPON_3) != 0u) restoredAfterRevive = true;
        }
        checkCond(selectedDefib && pulsedDefib && restoredAfterRevive,
                  "class AI: Medic switches to defib (slot 6), revives man-down ally, restores primary");

        // Support bot (kit 3): drops ammo bag near teammates
        BotBrain support(2u, 406u, nullptr, nullptr);
        std::map<uint16_t, TrackedSoldier> allies;
        allies[61].netId = 61; allies[61].team = 1; allies[61].hasPos = true; allies[61].pos = Vec3(2.0f, 0.0f, 2.0f);
        allies[62].netId = 62; allies[62].team = 1; allies[62].hasPos = true; allies[62].pos = Vec3(-2.0f, 0.0f, 2.0f);
        BotWorldView ws;
        ws.ownTeam = 1; ws.ownAlive = true; ws.hasOwnPos = true; ws.ownSoldierNetId = 60;
        ws.ownKit = 3; ws.soldiers = &allies;
        support.onSpawned(ws.ownPos, t0);
        bool droppedAmmo = false;
        bool restoredSupportPrimary = false;
        bool selAmmo = false;
        for (int tick = 0; tick < 240; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            BotTickOutput out;
            support.tick(now, 1.0f / 30.0f, ws, out);
            if ((out.action.buttonMaskLow & BTN_WEAPON_5) != 0u) selAmmo = true;
            if (selAmmo && (out.action.buttonMaskLow & BTN_FIRE) != 0u) droppedAmmo = true;
            if (droppedAmmo && (out.action.buttonMaskLow & BTN_WEAPON_3) != 0u) restoredSupportPrimary = true;
        }
        checkCond(selAmmo && droppedAmmo && restoredSupportPrimary,
                  "class AI: Support drops ammo bag near teammates and restores primary");
    }

    // 6. NavMesh funnel smoothing, walkability, building LOS, and cover/flank queries.
    {
        const char* const qtrPath = "bf2_loadbot_test_smart_nav.qtr";
        std::remove(qtrPath);

        // Build same 3x3 10x10m grid in [0,30]x[0,30] with center poly 4 unwalkable (building).
        std::vector<uint8_t> qtr;
        writeI32LE(qtr, 1); writeI32LE(qtr, 3);
        writeF32LE(qtr, 0.0f); writeF32LE(qtr, 0.0f);
        writeF32LE(qtr, 30.0f); writeF32LE(qtr, 30.0f); writeF32LE(qtr, 30.0f);
        writeU32LE(qtr, 1u);
        writeU32LE(qtr, 0u); writeU32LE(qtr, 0u); writeU32LE(qtr, 0u); writeU32LE(qtr, 0u);
        writeU8LE(qtr, 1u); writeU8LE(qtr, 9u); writeU8LE(qtr, 0u); writeU8LE(qtr, 0u);
        writeU32LE(qtr, 0u);
        writeU32LE(qtr, 9u);
        for (uint32_t p = 0; p < 9u; ++p)
        {
            writeU8LE(qtr, 4u);
            writeU8LE(qtr, (p == 4u) ? 0u : 1u);
            writeU16LE(qtr, 0u);
            writeU32LE(qtr, p * 4u);
            writeF32LE(qtr, 0.0f); writeF32LE(qtr, 1.0f); writeF32LE(qtr, 0.0f);
        }
        writeU32LE(qtr, 9u);
        for (uint32_t p = 0; p < 9u; ++p) writeU32LE(qtr, p);
        writeU32LE(qtr, 16u);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                writeF32LE(qtr, static_cast<float>(c * 10));
                writeF32LE(qtr, 0.0f);
                writeF32LE(qtr, static_cast<float>(r * 10));
            }
        writeU32LE(qtr, 36u);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                writeU32LE(qtr, static_cast<uint32_t>(r * 4 + c));
                writeU32LE(qtr, static_cast<uint32_t>((r + 1) * 4 + c));
                writeU32LE(qtr, static_cast<uint32_t>((r + 1) * 4 + (c + 1)));
                writeU32LE(qtr, static_cast<uint32_t>(r * 4 + (c + 1)));
            }
        writeU32LE(qtr, 36u);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                writeU32LE(qtr, (c > 0) ? static_cast<uint32_t>(r * 3 + (c - 1)) : NavMesh::INVALID_POLYGON);
                writeU32LE(qtr, (r < 2) ? static_cast<uint32_t>((r + 1) * 3 + c) : NavMesh::INVALID_POLYGON);
                writeU32LE(qtr, (c < 2) ? static_cast<uint32_t>(r * 3 + (c + 1)) : NavMesh::INVALID_POLYGON);
                writeU32LE(qtr, (r > 0) ? static_cast<uint32_t>((r - 1) * 3 + c) : NavMesh::INVALID_POLYGON);
            }
        {
            std::ofstream ofs(qtrPath, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(&qtr[0]), static_cast<std::streamsize>(qtr.size()));
        }

        NavMesh nav;
        checkCond(nav.load(qtrPath), "smart nav: loaded synthetic 3x3 obstacle mesh");
        std::vector<NavPoint> rawPath, smoothPath;
        const bool okRaw = nav.findPath(5.0f, 0.0f, 15.0f, 25.0f, 0.0f, 15.0f, rawPath);
        const bool okSmooth = nav.findSmoothedPath(5.0f, 0.0f, 15.0f, 25.0f, 0.0f, 15.0f, smoothPath);
        checkCond(okRaw && okSmooth && smoothPath.size() <= rawPath.size() && !smoothPath.empty(),
                  "smart nav: funnel findSmoothedPath simplifies corridor cleanly");
        bool smoothEnteredObs = false;
        for (size_t i = 0; i < smoothPath.size(); ++i)
            if (smoothPath[i].x > 10.05f && smoothPath[i].x < 19.95f &&
                smoothPath[i].z > 10.05f && smoothPath[i].z < 19.95f)
                smoothEnteredObs = true;
        checkCond(!smoothEnteredObs, "smart nav: smoothed path avoids center building obstacle");
        checkCond(nav.isPointWalkable(5.0f, 0.0f, 15.0f) && !nav.isPointWalkable(15.0f, 0.0f, 15.0f),
                  "smart nav: isPointWalkable distinguishes open ground vs building void");
        checkCond(!nav.hasNavLineOfSight(5.0f, 0.0f, 15.0f, 25.0f, 0.0f, 15.0f, 4.5f) &&
                  nav.hasNavLineOfSight(5.0f, 0.0f, 5.0f, 25.0f, 0.0f, 5.0f, 4.5f),
                  "smart nav: hasNavLineOfSight detects building occlusion vs clear street");
        NavPoint cov;
        const bool gotCov = nav.findCoverStep(7.0f, 0.0f, 15.0f, 25.0f, 15.0f, 5.0f, cov);
        checkCond(gotCov && nav.isPointWalkable(cov.x, cov.y, cov.z),
                  "smart nav: findCoverStep locates valid walkable cover offset");
        std::remove(qtrPath);
    }

    // 7. Frontline spawning, flag capture hold, and TeamTactics objective distribution + spotting.
    {
        std::map<uint8_t, TrackedSpawnGroup> sgs;
        // Rear friendly spawn at z=0, frontline friendly spawn at z=200, enemy flag at z=250.
        sgs[1].spawnGroupId = 1; sgs[1].team = 1; sgs[1].active = true; sgs[1].hasSpawnPoints = true;
        sgs[1].hasPos = true; sgs[1].pos = Vec3(0.0f, 0.0f, 0.0f);
        sgs[2].spawnGroupId = 2; sgs[2].team = 1; sgs[2].active = true; sgs[2].hasSpawnPoints = true;
        sgs[2].hasPos = true; sgs[2].pos = Vec3(0.0f, 0.0f, 200.0f);
        sgs[3].spawnGroupId = 3; sgs[3].team = 2; sgs[3].active = true; sgs[3].hasSpawnPoints = true;
        sgs[3].hasPos = true; sgs[3].pos = Vec3(0.0f, 0.0f, 250.0f);

        BotBrain frontBot(0u, 505u, nullptr, nullptr);
        const int32_t chosenSg = frontBot.pickSpawnGroup(sgs, nullptr, nullptr);
        checkCond(chosenSg == 2, "team tactics: pickSpawnGroup chooses frontline spawn over rear base");

        // Objective distribution: 10 Team-1 bots sharing TeamTactics split across 2 equidistant flags!
        TeamTactics shared;
        std::map<uint16_t, TrackedControlPoint> cps;
        cps[100].netId = 100; cps[100].team = 2; cps[100].hasPos = true; cps[100].pos = Vec3(-80.0f, 0.0f, 100.0f);
        cps[101].netId = 101; cps[101].team = 2; cps[101].hasPos = true; cps[101].pos = Vec3( 80.0f, 0.0f, 100.0f);

        BotWorldView tw;
        tw.ownTeam = 1; tw.ownAlive = true; tw.hasOwnPos = true; tw.ownPos = Vec3(0.0f, 0.0f, 0.0f);
        tw.controlPoints = &cps;
        const uint64_t tNow = 5000000ULL;
        for (uint32_t i = 0; i < 10u; ++i)
        {
            BotBrain b(i * 2u, 600u + i, nullptr, nullptr, AimConfig(), &shared);
            b.onSpawned(tw.ownPos, tNow);
            BotTickOutput out;
            b.tick(tNow, 1.0f / 30.0f, tw, out);
        }
        const uint32_t onFlagA = shared.countClaimants(1, 0x10000u | 100u, 999u, tNow);
        const uint32_t onFlagB = shared.countClaimants(1, 0x10000u | 101u, 999u, tNow);
        checkCond(onFlagA >= 3u && onFlagB >= 3u && (onFlagA + onFlagB) == 10u,
                  "team tactics: 10 bots split across two equidistant enemy flags via TeamTactics");

        // Flag capture hold: bot standing 5m from enemy flag stays crouched/holding inside capture zone!
        BotBrain capper(0u, 707u, nullptr, nullptr);
        std::map<uint16_t, TrackedControlPoint> oneFlag;
        oneFlag[200].netId = 200; oneFlag[200].team = 2; oneFlag[200].hasPos = true;
        oneFlag[200].pos = Vec3(0.0f, 0.0f, 5.0f);
        BotWorldView cw;
        cw.ownTeam = 1; cw.ownAlive = true; cw.hasOwnPos = true; cw.ownPos = Vec3(0.0f, 0.0f, 0.0f);
        cw.controlPoints = &oneFlag;
        capper.onSpawned(cw.ownPos, tNow);
        BotTickOutput capOut1, capOut2;
        capper.tick(tNow, 1.0f / 30.0f, cw, capOut1);
        capper.tick(tNow + 2000000ULL, 1.0f / 30.0f, cw, capOut2);
        checkCond((capOut2.action.buttonMaskLow & BTN_CROUCH) != 0u &&
                  std::fabs(capOut2.action.axes[3]) < 0.05f,
                  "objectives: bot holds inside enemy flag capture radius until captured");
    }

    // 8. Tactical out-of-combat reload & CQB empty-mag pistol fallback.
    {
        AimConfig cfg;
        cfg.skill = 0.9f;
        BotBrain brain(0u, 808u, nullptr, nullptr, cfg);

        std::map<uint16_t, TrackedSoldier> soldiers;
        TrackedSoldier& en = soldiers[33];
        en.netId = 33; en.team = 2; en.hasPos = true; en.pos = Vec3(0.0f, 0.0f, 8.0f); // CQB 8m

        BotWorldView w;
        w.ownTeam = 1; w.ownAlive = true; w.hasOwnPos = true; w.ownSoldierNetId = 1;
        w.ownKit = 0;
        w.soldiers = &soldiers;
        const uint64_t t0 = 5000000ULL;
        brain.onSpawned(w.ownPos, t0);

        bool swappedToPistol = false;
        bool firedPistol = false;
        bool restoredPrimaryAfterPistol = false;
        for (int tick = 0; tick < 360; ++tick)
        {
            const uint64_t now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            en.pushPos(now, en.pos);
            BotTickOutput out;
            brain.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            w.ownPitchDegrees += out.action.axes[5] * 5.0f;
            if ((out.action.buttonMaskLow & BTN_WEAPON_2) != 0u) swappedToPistol = true;
            if (swappedToPistol && (out.action.buttonMaskLow & BTN_FIRE) != 0u) firedPistol = true;
            if (firedPistol && (out.action.buttonMaskLow & BTN_WEAPON_3) != 0u)
            {
                restoredPrimaryAfterPistol = true;
                break;
            }
        }
        checkCond(swappedToPistol && firedPistol && restoredPrimaryAfterPistol,
                  "combat AI: empty mag in CQB swaps to pistol, fires, and restores primary");

        // Out-of-combat tactical reload after partial mag spent
        BotBrain brain2(0u, 809u, nullptr, nullptr, cfg);
        en.pos = Vec3(0.0f, 0.0f, 25.0f);
        w.ownFacingDegrees = 0.0f;
        w.ownPitchDegrees = 0.0f;
        brain2.onSpawned(w.ownPos, t0);
        // Engage long enough to spend ~15 rounds (half a mag), then enemy disappears.
        uint64_t now = t0;
        for (int tick = 0; tick < 90; ++tick)
        {
            now = t0 + static_cast<uint64_t>(tick) * 33333ULL;
            en.pushPos(now, en.pos);
            BotTickOutput out;
            brain2.tick(now, 1.0f / 30.0f, w, out);
            w.ownFacingDegrees += out.action.axes[4] * 5.0f;
            w.ownPitchDegrees += out.action.axes[5] * 5.0f;
        }
        en.hitPointsByte = 0u; // enemy killed!
        bool didTacticalReload = false;
        for (int tick = 0; tick < 90; ++tick)
        {
            now += 33333ULL;
            BotTickOutput out;
            brain2.tick(now, 1.0f / 30.0f, w, out);
            if ((out.action.buttonMaskLow & BTN_RELOAD) != 0u)
            {
                didTacticalReload = true;
                break;
            }
        }
        checkCond(didTacticalReload, "combat AI: performs proactive tactical reload out of combat");
    }
}

} // anonymous namespace

int main()
{
    testBitStreamCrossCheck();
    testSWTransport();
    testPlayerActionManager();
    testGameEventManager();
    testGhostManager();
    testNavMesh();
    testRouteLoaderAndAbiParity();
    testNetImpairment();
    testTelemetryAndSlidingDetector();
    testHumanAim();
    testSmartTacticalBots();
    testLoopbackHandshakeAndGameplay();

    if (g_failures == 0)
    {
        std::printf("[PASS] bf2_loadbot_tests: all %d checks passed.\n", g_passes);
        return 0;
    }

    std::fprintf(stderr, "[FAIL] bf2_loadbot_tests: %d failures (%d passed).\n",
                 g_failures, g_passes);
    return 1;
}
