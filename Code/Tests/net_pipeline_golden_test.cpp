// [CHOSEN, not recovered] Golden bit-exact encoder & 2D spatial grid equivalence test suite
// for OpenBF2 Phase 2A (WorldNetSnapshot + ConstNetEncoders + ServerTickProfiler).
//
// Standalone single-TU compile command on Linux:
//   g++ -std=c++14 -fno-strict-aliasing -pthread -I Code/BF2 \
//       Code/Tests/net_pipeline_golden_test.cpp -o net_pipeline_golden_test && ./net_pipeline_golden_test

#include "../BF2/Game/GameServer/NetPipeline/WorldNetSnapshot.h"
#include "../BF2/Game/GameServer/NetPipeline/ConstNetEncoders.h"
#include "../BF2/Game/GameServer/NetPipeline/NetWorkerPool.h"
#include "../BF2/Game/GameServer/ServerTickProfiler.h"
#include "../BF2/IO/BitStream.h"
#include "../BF2/IO/Network/INetworkable.h"
#include "../BF2/Game/Utility/Vars.h"
#include "../BF2/Core/Debug.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef NET_PIPELINE_GOLDEN_TEST_EXTERNAL_LINK

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

namespace {

std::map<std::string, int>& intCvarStore()
{
    static std::map<std::string, int> s_store;
    return s_store;
}

std::map<std::string, float>& floatCvarStore()
{
    static std::map<std::string, float> s_store;
    return s_store;
}

} // anonymous namespace

int* Vars::getInt(const char* name, int defaultValue)
{
    std::map<std::string, int>& store = intCvarStore();
    std::string key(name ? name : "");
    std::map<std::string, int>::iterator it = store.find(key);
    if (it == store.end())
        it = store.insert(std::make_pair(key, defaultValue)).first;
    return &it->second;
}

float* Vars::getFloat(const char* name, float defaultValue)
{
    std::map<std::string, float>& store = floatCvarStore();
    std::string key(name ? name : "");
    std::map<std::string, float>::iterator it = store.find(key);
    if (it == store.end())
        it = store.insert(std::make_pair(key, defaultValue)).first;
    return &it->second;
}

namespace io {
int StatePrecisionFull   = 3;
int StatePrecisionGhost  = 2;
int StatePrecisionLow    = 1;
int StatePrecisionLowest = 0;
} // namespace io

SimpleObjectNetworkable::UPDATE::UPDATE()
{
    std::memset(this, 0, sizeof(*this));
}

SoldierNetworkable::UPDATE::UPDATE()
{
    std::memset(this, 0, sizeof(*this));
}

} // namespace hfe
} // namespace dice

#include "../BF2/IO/BitStream.cpp"
#include "../BF2/IO/Network/INetworkable.cpp"
#include "../BF2/Game/GameServer/NetPipeline/ConstNetEncoders.cpp"
#define NET_WORKER_POOL_CORE_ONLY
#include "../BF2/Game/GameServer/NetPipeline/NetWorkerPool.cpp"
#undef NET_WORKER_POOL_CORE_ONLY
#include "../BF2/Game/GameServer/ServerTickProfiler.cpp"

#endif // NET_PIPELINE_GOLDEN_TEST_EXTERNAL_LINK

namespace dice {
namespace hfe {
namespace netpipeline {
namespace {

int g_failures = 0;
int g_passes   = 0;

inline Vec3 makeVec3(float x, float y, float z)
{
    Vec3 v = { x, y, z };
    return v;
}

inline Quat makeQuat(float x, float y, float z, float w)
{
    Quat q = { x, y, z, w };
    return q;
}

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

// Stub INetworkable for testing WorldNetSnapshot::findRecord and ConstNetEncoders::encodeDescriptor.
class DummyStubNetworkable : public io::INetworkable
{
public:
    explicit DummyStubNetworkable(unsigned short netId, unsigned int ghostMask = 0xFFFFFFFFu, unsigned int minBits = 32u)
        : m_ghostMask(ghostMask)
        , m_minBits(minBits)
    {
        m_networkableId = netId;
        m_basePriority  = 1.0f;
        m_ghostActive   = true;
    }

    virtual ~DummyStubNetworkable() {}
    virtual void addRef() {}
    virtual int getRef() { return 1; }
    virtual int release() { return 1; }
    virtual void* queryInterface(unsigned int) const { return 0; }
    virtual bool init(void*) { return true; }
    virtual unsigned int getGhostStateMask() const { return m_ghostMask; }
    virtual void updateStateMask(unsigned int*) {}
    virtual bool getNetUpdate(io::BitStream&, io::BaseLineData*, unsigned int, int) { return true; }
    virtual void setNetUpdate(io::BitStream&, float, bool, io::BaseLineData*, int) {}
    virtual bool getBaseLine(io::BaseLineData&) { return true; }
    virtual unsigned int getMinimumBitSize() const { return m_minBits; }

private:
    unsigned int m_ghostMask;
    unsigned int m_minBits;
};

// Reference vanilla encoders matching the exact recovered *Networkable::getNetUpdate implementations.
bool refEncodeSoldier(
    const SoldierRecordSnap& snap,
    const RagDollParticleSnap* ragdollParticles,
    io::BitStream& stream,
    unsigned int stateMask,
    int precision)
{
    const SoldierNetworkable::UPDATE& rec = snap.rec;
    unsigned int mask = snap.isAwakeOccupiedRagdoll ? 0x8040u : (stateMask & ~0x8000u);
    stream.writeBits(&mask, 21);

    if (snap.hasArmor)
    {
        if ((mask & 0x40u) != 0) stream.writeBits(&rec.hitPoints, 8);
        if ((mask & 0x40u) != 0) stream.writeBits(&rec.lastDamageType, 1);
    }
    if ((mask & 0x20u) != 0)
    {
        const unsigned int pose = static_cast<unsigned int>(rec.pose);
        if (pose > 4u) stream.setError(4);
        else stream.writeBits(&pose, 3);
    }
    if ((mask & 0x20u) != 0)
    {
        const unsigned int requestedPose = static_cast<unsigned int>(rec.requestedPose);
        if (requestedPose > 4u) stream.setError(4);
        else stream.writeBits(&requestedPose, 3);
    }

    if ((mask & 0x8000u) != 0)
    {
        io::BSPosition lengthFieldPos;
        lengthFieldPos.m_unknown00       = 0;
        lengthFieldPos.m_position        = stream.getBitPosition();
        lengthFieldPos.m_unknown08       = 0;
        lengthFieldPos.m_readWriteSwitch = stream.m_readWriteSwitch;

        unsigned int payloadBits = 0;
        stream.skipBits(9);

        const Vec3* reference = 0;
        for (unsigned int i = 0; i < snap.ragdollParticleCount; ++i)
        {
            const RagDollParticleSnap& p = ragdollParticles[snap.ragdollParticleStart + i];
            if (!p.netControlled)
                continue;
            if (i != 0 && reference != 0)
            {
                stream.writeCompressedVector2(p.position, *reference, 0.01f);
            }
            else
            {
                reference = &p.position;
                stream.writeBits(&p.position.x, 32);
                stream.writeBits(&p.position.y, 32);
                stream.writeBits(&p.position.z, 32);
            }
        }
        payloadBits = stream.getBitPosition() - lengthFieldPos.m_position - 9u;
        stream.setWritePosition(lengthFieldPos);
        stream.writeBits(&payloadBits, 9);
        stream.resetWritePosition();
    }
    else
    {
        const Vec3 zeroVec3 = { 0.0f, 0.0f, 0.0f };
        if ((mask & 0x1u) != 0)
        {
            if (stream.hasCompressionVector())
                stream.writeCompressedVector(rec.absoluteTrans, stream.getCompressionVector(), 0.001f);
            else
            {
                const unsigned int selector = 0;
                stream.writeBits(&selector, 2);
                stream.writeBits(&rec.absoluteTrans.x, 32);
                stream.writeBits(&rec.absoluteTrans.y, 32);
                stream.writeBits(&rec.absoluteTrans.z, 32);
            }
        }
        if (precision == io::StatePrecisionFull)
        {
            if ((mask & 0x80u) != 0) stream.writeCompressedVector(rec.positionalSpeed, zeroVec3, 0.001f);
            if ((mask & 0x100u) != 0) stream.writeCompressedVector(rec.positionalAcceleration, zeroVec3, 0.000099999997f);
            if ((mask & 0x200u) != 0) stream.writeCompressedVector(rec.positionalFriction, zeroVec3, 0.000099999997f);
            if ((mask & 0x40000u) != 0) stream.writeCompressedVector(rec.localLinearSpeed, zeroVec3, 0.001f);
        }
        else if ((mask & 0x80u) != 0)
        {
            stream.writeCompressedVector(rec.positionalSpeed, zeroVec3, 0.010000001f);
        }

        const unsigned int angleMasks[4] = { 0x2u, 0x4u, 0x8u, 0x10u };
        const float angleVals[4] = { rec.facingDegrees, rec.lookLeftRightAngle, rec.pendingTurn, rec.lookUpDownAngle };
        const float angleDivs[4] = { 360.0f, 90.0f, 180.0f, 90.0f };
        for (int a = 0; a < 4; ++a)
        {
            if ((mask & angleMasks[a]) != 0)
            {
                if (precision == io::StatePrecisionFull)
                {
                    stream.writeBits(&angleVals[a], 32);
                }
                else
                {
                    float norm = (angleVals[a] / angleDivs[a] + 1.0f) * 0.5f;
                    if (norm > 1.0f) norm = 1.0f;
                    else if (norm < 0.0f) norm = 0.0f;
                    unsigned int q = static_cast<unsigned int>(norm * 4095.0f);
                    stream.writeBits(&q, 12);
                }
            }
        }

        const unsigned int wz = static_cast<unsigned int>(rec.weaponZoom);
        if (wz > 3u) stream.setError(4); else stream.writeBits(&wz, 3);
        const unsigned int ws = static_cast<unsigned int>(rec.weaponState);
        if (ws > 1u) stream.setError(4); else stream.writeBits(&ws, 2);

        const unsigned int bSky = rec.isSkyDiving ? 1u : 0u;
        const unsigned int bSwm = rec.isSwimming ? 1u : 0u;
        const unsigned int bStw = rec.weaponStowed ? 1u : 0u;
        stream.writeBits(&bSky, 1);
        stream.writeBits(&bSwm, 1);
        stream.writeBits(&bStw, 1);

        const unsigned int moveMasks[2] = { 0x400u, 0x800u };
        const float moveVals[2] = { rec.smoothedForward, rec.smoothedStrafe };
        for (int m = 0; m < 2; ++m)
        {
            if ((mask & moveMasks[m]) != 0)
            {
                float norm = (moveVals[m] / 50.0f + 1.0f) * 0.5f;
                if (norm > 1.0f) norm = 1.0f;
                else if (norm < 0.0f) norm = 0.0f;
                if (precision == io::StatePrecisionFull)
                {
                    unsigned int q = static_cast<unsigned int>(norm * 65535.0f);
                    stream.writeBits(&q, 16);
                }
                else
                {
                    unsigned int q = static_cast<unsigned int>(norm * 1023.0f);
                    stream.writeBits(&q, 10);
                }
            }
        }

        const unsigned int bResp = rec.responseByteFA ? 1u : 0u;
        stream.writeBits(&bResp, 1);

        if ((mask & 0x4000u) != 0)
        {
            float clamped = rec.sprintStamina;
            if (clamped > 1.0f) clamped = 1.0f;
            else if (clamped < 0.0f) clamped = 0.0f;
            if (precision == io::StatePrecisionFull)
            {
                unsigned int q = static_cast<unsigned int>(clamped * 127.0f);
                stream.writeBits(&q, 7);
                const unsigned int bSpr = rec.isSprinting ? 1u : 0u;
                stream.writeBits(&bSpr, 1);
            }
            else
            {
                unsigned int q = static_cast<unsigned int>(clamped * 31.0f);
                stream.writeBits(&q, 5);
            }
        }

        if ((mask & 0x1000u) != 0)
        {
            const unsigned int upper = static_cast<unsigned int>(snap.inventorySize + 1);
            const unsigned int value = static_cast<unsigned int>(rec.itemIndex + 1);
            if (value > upper) stream.setError(4);
            else
            {
                unsigned int bits = 0;
                if (snap.inventorySize != -2)
                {
                    const unsigned int range = static_cast<unsigned int>(snap.inventorySize + 2);
                    do { ++bits; } while (range > (1u << bits) - 1u);
                }
                stream.writeBits(&value, bits);
            }
        }

        if ((mask & 0x2000u) != 0)
            stream.writeBits(&rec.poseInputLatch, 2);

        if (precision == io::StatePrecisionFull && (mask & 0x20000u) != 0)
        {
            float norm = (rec.currentAbility + 1.0f) * 0.5f;
            if (norm > 1.0f) norm = 1.0f;
            else if (norm < 0.0f) norm = 0.0f;
            unsigned int q = static_cast<unsigned int>(norm * 127.0f);
            stream.writeBits(&q, 7);
        }

        if ((mask & 0x10000u) != 0)
        {
            const unsigned int lid = rec.ladderId;
            if (lid > 0xFFu) stream.setError(4);
            else stream.writeBits(&lid, 9);
            if (rec.ladderId != 0)
            {
                float c = rec.ladderState;
                if (c > 1.0f) c = 1.0f; else if (c < 0.0f) c = 0.0f;
                unsigned int q = static_cast<unsigned int>(c * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x100000u) != 0)
        {
            stream.writeBits(&rec.ziplineId, 16);
            if (rec.ziplineId != 0)
            {
                float c = rec.ziplineState;
                if (c > 1.0f) c = 1.0f; else if (c < 0.0f) c = 0.0f;
                unsigned int q = static_cast<unsigned int>(c * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x80000u) != 0)
        {
            SoldierNetworkable::UPDATE mutableCopy = rec;
            mutableCopy.ammoStatus &= 0x0Fu;
            stream.writeBits(&mutableCopy.ammoStatus, 5);
        }
    }

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
    return true;
}

void testAllNineConstNetEncoders()
{
    WorldNetSnapshot worldSnap;
    const Vec3 compRef = { 120.5f, 45.0f, -310.25f };

    // 1. SoldierNetworkable (Ghost, Full, Ragdoll, Range Error)
    {
        SoldierRecordSnap s;
        std::memset(&s, 0, sizeof(s));
        s.rec.absoluteTrans          = makeVec3(123.45f, 45.12f, -308.0f);
        s.rec.positionalSpeed        = makeVec3(4.2f, -0.1f, 3.8f);
        s.rec.positionalAcceleration = makeVec3(0.05f, 0.0f, -0.02f);
        s.rec.positionalFriction     = makeVec3(-0.01f, 0.0f, 0.01f);
        s.rec.localLinearSpeed       = makeVec3(1.5f, 0.0f, 4.0f);
        s.rec.facingDegrees          = 135.0f;
        s.rec.lookLeftRightAngle     = -22.5f;
        s.rec.pendingTurn            = 15.0f;
        s.rec.lookUpDownAngle        = 12.0f;
        s.rec.smoothedForward        = 18.5f;
        s.rec.smoothedStrafe         = -7.25f;
        s.rec.hitPoints              = 87;
        s.rec.lastDamageType         = 1;
        s.rec.pose                   = 1;
        s.rec.requestedPose          = 2;
        s.rec.poseInputLatch         = 2;
        s.rec.itemIndex              = 3;
        s.rec.weaponZoom             = 1;
        s.rec.weaponState            = 1;
        s.rec.isSkyDiving            = false;
        s.rec.isSwimming             = true;
        s.rec.weaponStowed           = false;
        s.rec.responseByteFA         = 1;
        s.rec.isSprinting            = true;
        s.rec.sprintStamina          = 0.72f;
        s.rec.currentAbility         = 0.35f;
        s.rec.ladderId               = 14;
        s.rec.ladderState            = 0.6f;
        s.rec.ziplineId              = 512;
        s.rec.ziplineState           = 0.25f;
        s.rec.ammoStatus             = 0xF5; // high bits set to verify &= 0x0F behavior
        s.isAwakeOccupiedRagdoll     = false;
        s.hasArmor                   = true;
        s.inventorySize              = 9;
        s.ragdollParticleStart       = 0;
        s.ragdollParticleCount       = 0;

        const int origAmmoStatus = s.rec.ammoStatus;

        // Ghost precision with compression vector
        {
            unsigned char bufA[512] = { 0 };
            unsigned char bufB[512] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            io::BitStream streamB(bufB, sizeof(bufB));
            streamA.setCompressionVector(compRef);
            streamB.setCompressionVector(compRef);

            int bitsWritten = 0;
            const bool okA = ConstNetEncoders::encodeSoldier(
                s, 0, streamA, 0, 0x1FFFFFu, io::StatePrecisionGhost, &bitsWritten);
            const bool okB = refEncodeSoldier(
                s, 0, streamB, 0x1FFFFFu, io::StatePrecisionGhost);

            checkCond(okA && okB, "Soldier Ghost precision returns true");
            checkCond(s.rec.ammoStatus == origAmmoStatus, "Soldier ConstNetEncoders does not mutate ammoStatus");
            checkCond(bitsWritten == static_cast<int>(streamA.getBitPosition()), "Soldier bitsWritten matches bitPos");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                      "Soldier Ghost precision bit-exact match");
        }

        // Full precision without compression vector
        {
            unsigned char bufA[512] = { 0 };
            unsigned char bufB[512] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            io::BitStream streamB(bufB, sizeof(bufB));

            const bool okA = ConstNetEncoders::encodeSoldier(
                s, 0, streamA, 0, 0x1FFFFFu, io::StatePrecisionFull);
            const bool okB = refEncodeSoldier(
                s, 0, streamB, 0x1FFFFFu, io::StatePrecisionFull);

            checkCond(okA && okB, "Soldier Full precision returns true");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                      "Soldier Full precision bit-exact match");
        }

        // Awake occupied ragdoll with 4 particles
        {
            RagDollParticleSnap particles[4];
            particles[0].netControlled = true;
            particles[0].position      = makeVec3(100.0f, 20.0f, 300.0f);
            particles[1].netControlled = false;
            particles[1].position      = makeVec3(999.0f, 999.0f, 999.0f);
            particles[2].netControlled = true;
            particles[2].position      = makeVec3(100.25f, 20.5f, 299.8f);
            particles[3].netControlled = true;
            particles[3].position      = makeVec3(99.9f, 19.75f, 300.1f);

            SoldierRecordSnap ragSnap = s;
            ragSnap.isAwakeOccupiedRagdoll = true;
            ragSnap.ragdollParticleStart   = 0;
            ragSnap.ragdollParticleCount   = 4;

            unsigned char bufA[512] = { 0 };
            unsigned char bufB[512] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            io::BitStream streamB(bufB, sizeof(bufB));

            const bool okA = ConstNetEncoders::encodeSoldier(
                ragSnap, particles, streamA, 0, 0x1FFFFFu, io::StatePrecisionGhost);
            const bool okB = refEncodeSoldier(
                ragSnap, particles, streamB, 0x1FFFFFu, io::StatePrecisionGhost);

            checkCond(okA && okB, "Soldier Ragdoll returns true");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                      "Soldier Ragdoll bit-exact match (including 9-bit backfilled payloadBits)");
        }

        // Out-of-range pose rejection without Debug() abort
        {
            SoldierRecordSnap badSnap = s;
            badSnap.rec.pose = 7; // > 4
            unsigned char bufA[256] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            const bool okA = ConstNetEncoders::encodeSoldier(
                badSnap, 0, streamA, 0, 0x20u, io::StatePrecisionGhost);
            checkCond(!okA && !streamA.hasError(), "Soldier out-of-range pose returns false and clears error latch");
        }
    }

    // 2. ItemNetworkable
    {
        ItemRecordSnap itemSnap;
        itemSnap.rec.position = makeVec3(121.0f, 45.25f, -310.0f);
        itemSnap.rec.ownerId  = -5;
        itemSnap.rec.enabled  = true;
        itemSnap.rec.team     = 2;

        unsigned char bufA[128] = { 0 };
        unsigned char bufB[128] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));
        streamA.setCompressionVector(compRef);
        streamB.setCompressionVector(compRef);

        const unsigned int mask = 0x1Fu;
        const bool okA = ConstNetEncoders::encodeItem(itemSnap, streamA, 0, mask, io::StatePrecisionGhost);

        // Reference write
        streamB.writeBits(&mask, 5);
        streamB.writeCompressedVector(itemSnap.rec.position, streamB.getCompressionVector(), 0.0005f);
        const unsigned int ownerSign = 1u;
        const unsigned int ownerMag  = 5u;
        streamB.writeBits(&ownerSign, 1);
        streamB.writeBits(&ownerMag, 8);
        streamB.writeBits(&itemSnap.rec.enabled, 1);
        const unsigned int teamSign = 0u;
        const unsigned int teamMag  = 2u;
        streamB.writeBits(&teamSign, 1);
        streamB.writeBits(&teamMag, 4);

        checkCond(okA, "ItemNetworkable encode returns true");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "ItemNetworkable bit-exact match");
    }

    // 3. KitNetworkable
    {
        unsigned char bufA[64] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        int bits = 99;
        const bool okA = ConstNetEncoders::encodeKit(streamA, 0, 0xFFFFFFFFu, io::StatePrecisionFull, &bits);
        checkCond(okA && bits == 0 && streamA.getBitPosition() == 0,
                  "KitNetworkable writes 0 bits and returns true");
    }

    // 4. GenericProjectileNetworkable (in-flight + detonated + armor)
    {
        GenericProjectileRecordSnap projSnap;
        std::memset(&projSnap, 0, sizeof(projSnap));
        projSnap.rec.position             = makeVec3(125.0f, 48.0f, -300.0f);
        projSnap.rec.rotation             = makeQuat(0.0f, 0.70710678f, 0.0f, 0.70710678f);
        projSnap.rec.detonate             = 1;
        projSnap.rec.field1C4             = -1234;
        projSnap.rec.vector1CC            = makeVec3(0.25f, -0.5f, 1.25f);
        projSnap.rec.hasTargetNetworkable = true;
        projSnap.rec.targetNetworkableId  = 4321;
        projSnap.rec.hitPointsCeil        = 45;
        projSnap.hasArmor                 = true;
        projSnap.armorMaxWreckHp          = 20;
        projSnap.armorLower               = 0;
        projSnap.armorUpper               = 120;

        unsigned char bufA[256] = { 0 };
        unsigned char bufB[256] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));
        streamA.setCompressionVector(compRef);
        streamB.setCompressionVector(compRef);

        const unsigned int mask = 0x17u;
        const bool okA = ConstNetEncoders::encodeGenericProjectile(
            projSnap, streamA, 0, mask, io::StatePrecisionGhost);

        streamB.writeBits(&mask, 5);
        streamB.writeCompressedVector(projSnap.rec.position, streamB.getCompressionVector(), 0.0099999998f);
        streamB.writeUnitQuaternion(projSnap.rec.rotation, 12);
        const unsigned int detBit = 1u;
        streamB.writeBits(&detBit, 1);
        const unsigned int fSign = 1u;
        const unsigned int fMag  = 1234u;
        streamB.writeBits(&fSign, 1);
        streamB.writeBits(&fMag, 15);
        streamB.writeHighCompressedVector(projSnap.rec.vector1CC, makeVec3(0.0f, 0.0f, 0.0f), 0.0099999998f);
        const unsigned int hasTgt = 1u;
        streamB.writeBits(&hasTgt, 1);
        streamB.writeBits(&projSnap.rec.targetNetworkableId, 16);
        const unsigned int hpOffset = 65u; // 45 + 20 - 0 in [0..120] -> 7 bits
        streamB.writeBits(&hpOffset, 7);

        checkCond(okA, "GenericProjectileNetworkable encode returns true");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "GenericProjectileNetworkable bit-exact match");
    }

    // 5. ControlPointNetworkable
    {
        ControlPointRecordSnap cpSnap;
        cpSnap.rec.team                    = -1;
        cpSnap.rec.bool158                 = true;
        cpSnap.rec.takeOverCounter         = 0.75f;
        cpSnap.rec.takeOverChangePerSecond = -0.5f;
        cpSnap.rec.allowCaptureByTeam1     = true;
        cpSnap.rec.allowCaptureByTeam2     = false;
        cpSnap.rec.flagTeam                = 2;
        cpSnap.rec.capturingState          = -2;

        unsigned char bufA[128] = { 0 };
        unsigned char bufB[128] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));

        const unsigned int mask = 0x7Fu;
        const bool okA = ConstNetEncoders::encodeControlPoint(cpSnap, streamA, 0, mask, io::StatePrecisionFull);

        streamB.writeBits(&mask, 7);
        const unsigned int s1 = 1u, m1 = 1u;
        streamB.writeBits(&s1, 1); streamB.writeBits(&m1, 3);
        const unsigned int b158 = cpSnap.rec.bool158 ? 1u : 0u;
        streamB.writeBits(&b158, 1);
        const unsigned int qCounter = static_cast<unsigned int>(static_cast<int>(((0.75 / 1.25 + 1.0) * 0.5) * 65535.0));
        const unsigned int qRate    = static_cast<unsigned int>(static_cast<int>(((-0.5 / 1.25 + 1.0) * 0.5) * 65535.0));
        streamB.writeBits(&qCounter, 16);
        streamB.writeBits(&qRate, 16);
        const unsigned int z0 = 0u, flag1 = 1u, flag0 = 0u;
        streamB.writeBits(&z0, 1); streamB.writeBits(&flag1, 3);
        streamB.writeBits(&z0, 1); streamB.writeBits(&flag0, 3);
        const unsigned int s2 = 0u, m2 = 2u;
        streamB.writeBits(&s2, 1); streamB.writeBits(&m2, 3);
        const unsigned int s3 = 1u, m3 = 2u;
        streamB.writeBits(&s3, 1); streamB.writeBits(&m3, 3);

        checkCond(okA, "ControlPointNetworkable encode returns true");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "ControlPointNetworkable bit-exact match");
    }

    // 6. SimpleObjectNetworkable (Ghost, Full, Baseline Delta)
    {
        SimpleObjectRecordSnap soSnap;
        std::memset(&soSnap, 0, sizeof(soSnap));
        soSnap.rec.position         = makeVec3(122.0f, 46.0f, -309.0f);
        soSnap.rec.rotation         = makeQuat(0.0f, 0.0f, 0.0f, 1.0f);
        soSnap.rec.vec38            = makeVec3(2.0f, 0.0f, -1.0f);
        soSnap.rec.vec44            = makeVec3(0.01f, 0.0f, 0.01f);
        soSnap.rec.vec50            = makeVec3(-0.01f, 0.0f, 0.0f);
        soSnap.rec.vec5C            = makeVec3(0.5f, 0.1f, -0.2f);
        soSnap.rec.vec68            = makeVec3(0.0f, 0.02f, 0.0f);
        soSnap.rec.vec74            = makeVec3(0.0f, -0.01f, 0.0f);
        soSnap.rec.vec80            = makeVec3(1.0f, 0.0f, 0.0f);
        soSnap.rec.vec8C            = makeVec3(0.0f, 1.0f, 0.0f);
        soSnap.rec.byte9C           = true;
        soSnap.rec.hitPoints        = 300;
        soSnap.rec.replicationState = 2;
        soSnap.rec.weaponIndex[0]   = 0;
        soSnap.rec.weaponIndex[1]   = 2;
        soSnap.rec.weaponIndex[2]   = -1;
        soSnap.rec.supplyB0         = 4; // non-canonical even truthy int -> must normalize to 1
        soSnap.rec.pcoB4            = 15; // exercises 5-bit bitsToHold(16) width
        soSnap.rec.isDucking        = true;
        soSnap.isMobilePhysics      = true;
        soSnap.hasArmor             = true;
        soSnap.armorMaxWreckHp      = 50;
        soSnap.armorLower           = 0;
        soSnap.armorUpper           = 500;
        soSnap.hasPco               = true;
        soSnap.isParachute          = false;

        // Baseline delta path (verifies ref[0..2] at baseline->m_data + baseline->m_cursor * 2 and m_cursor += 36)
        {
            io::BaseLineData baseA;
            io::BaseLineData baseB;
            std::memset(&baseA, 0, sizeof(baseA));
            baseA.m_cursor = 6;
            Vec3* refs = reinterpret_cast<Vec3*>(baseA.m_data + baseA.m_cursor * 2);
            refs[0] = makeVec3(120.0f, 45.0f, -310.0f);
            refs[1] = makeVec3(1.5f, 0.0f, -0.5f);
            refs[2] = makeVec3(0.2f, 0.0f, -0.1f);
            baseB = baseA;

            unsigned char bufA[512] = { 0 };
            unsigned char bufB[512] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            io::BitStream streamB(bufB, sizeof(bufB));

            const unsigned int mask = 0x7DFFFu;
            const bool okA = ConstNetEncoders::encodeSimpleObject(
                soSnap, streamA, &baseA, mask, io::StatePrecisionGhost);

            // Reference baseline delta encode (matching SimpleObjectNetworkable::getNetUpdate)
            const Vec3 zeroVec = { 0.0f, 0.0f, 0.0f };
            streamB.writeBits(&mask, 19);
            const Vec3* refB = reinterpret_cast<const Vec3*>(baseB.m_data + baseB.m_cursor * 2);
            streamB.writeCompressedVector(soSnap.rec.position, refB[0], 0.00050000002f);
            streamB.writeCompressedVector(soSnap.rec.vec38, refB[1], 0.001f);
            streamB.writeCompressedVector(soSnap.rec.vec5C, refB[2], 0.001f);
            baseB.m_cursor += 36;
            streamB.writeUnitQuaternion(soSnap.rec.rotation, 14);
            streamB.writeCompressedVector(soSnap.rec.vec80, zeroVec, 0.001f);
            streamB.writeCompressedVector(soSnap.rec.vec8C, zeroVec, 0.001f);
            streamB.writeCompressedVector(soSnap.rec.vec44, zeroVec, 0.000099999997f);
            streamB.writeCompressedVector(soSnap.rec.vec68, zeroVec, 0.000099999997f);
            streamB.writeCompressedVector(soSnap.rec.vec50, zeroVec, 0.000099999997f);
            streamB.writeCompressedVector(soSnap.rec.vec74, zeroVec, 0.000099999997f);
            const unsigned int b9c = 1u, sb0 = 1u;
            streamB.writeBits(&b9c, 1);
            streamB.writeBits(&sb0, 1);
            const unsigned int hpOff = 350u; // 300 + 50 in [0..500] -> 9 bits
            streamB.writeBits(&hpOff, 9);
            const unsigned int repState = soSnap.rec.replicationState;
            streamB.writeBits(&repState, 2);
            const unsigned int w0 = 1u, w1 = 3u, w2 = 0u, pcoB4 = 15u, duck = 1u;
            streamB.writeBits(&w0, 4);
            streamB.writeBits(&w1, 4);
            streamB.writeBits(&w2, 4);
            streamB.writeBits(&pcoB4, 5); // bitsToHold(16) == 5 bits!
            streamB.writeBits(&duck, 1);

            checkCond(okA && baseA.m_cursor == baseB.m_cursor && baseA.m_cursor == 42,
                      "SimpleObjectNetworkable baseline cursor advances by +36");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                      "SimpleObjectNetworkable baseline delta bit-exact match (5-bit pcoB4 + normalized bools)");
        }
    }

    // 7. Player (bit-exact match against Player::getNetUpdate for Full and Ghost precision)
    {
        auto setSlotDword = [](NetUpdateSlot& s, int off, int v) {
            std::memcpy(&s.buff[off], &v, sizeof(v));
        };
        auto setSlotWord = [](NetUpdateSlot& s, int off, unsigned short v) {
            std::memcpy(&s.buff[off], &v, sizeof(v));
        };
        auto setSlotFloat = [](NetUpdateSlot& s, int off, float v) {
            std::memcpy(&s.buff[off], &v, sizeof(v));
        };
        auto writeBitRef = [](io::BitStream& bs, bool v) {
            const unsigned int b = v ? 1u : 0u;
            bs.writeBits(&b, 1);
        };
        auto writeRangedRef = [](io::BitStream& bs, int val, int lo, int hi) {
            const unsigned int span = static_cast<unsigned int>(hi - lo) + 1u;
            unsigned int bits = 0;
            do { ++bits; } while (span > (1u << bits) - 1u);
            const unsigned int off = static_cast<unsigned int>(val - lo);
            bs.writeBits(&off, bits);
        };

        PlayerRecordSnap pSnap;
        std::memset(&pSnap, 0, sizeof(pSnap));
        pSnap.slot.isAlive    = 1;
        pSnap.slot.someScore2 = 123;
        setSlotDword(pSnap.slot, 0, 45);      // m_field14
        setSlotDword(pSnap.slot, 4, -17);     // m_field04 (signed 1+11)
        setSlotDword(pSnap.slot, 8, 99);      // m_field18
        setSlotDword(pSnap.slot, 12, -42);    // totalScore (signed 1+11)
        setSlotDword(pSnap.slot, 16, 1);      // team (team+1 = 2 in [0..3])
        setSlotDword(pSnap.slot, 20, 11);     // m_field54
        setSlotDword(pSnap.slot, 24, 22);     // m_field58
        setSlotDword(pSnap.slot, 28, 33);     // m_field5C
        setSlotDword(pSnap.slot, 32, 3);      // cameraViewMode
        setSlotDword(pSnap.slot, 36, 4);      // spawnGroup
        setSlotDword(pSnap.slot, 40, 2);      // squadId
        setSlotDword(pSnap.slot, 44, 7);      // rank (rank+1 = 8 in [0..31])
        pSnap.slot.buff[48]   = 1;            // isSquadLeader (unmasked)
        pSnap.slot.buff[49]   = 1;            // commander
        pSnap.slot.buff[50]   = 1;            // uavReady
        pSnap.slot.buff[51]   = 0;            // satelliteReady
        pSnap.slot.buff[52]   = 1;
        pSnap.slot.buff[53]   = 0;
        pSnap.slot.buff[54]   = 1;
        setSlotFloat(pSnap.slot, 56, 0.25f);
        setSlotFloat(pSnap.slot, 60, 0.50f);
        setSlotFloat(pSnap.slot, 64, 0.75f);
        pSnap.slot.buff[68]   = 1;            // flagHolder
        pSnap.slot.buff[69]   = 1;            // callForMedic
        setSlotDword(pSnap.slot, 72, 2);      // firePressCount
        // Weapon 0: firing, non-heat
        pSnap.slot.buff[76]   = 1;
        setSlotDword(pSnap.slot, 80, 10);     // deviation (+1 = 11)
        setSlotDword(pSnap.slot, 84, 2);      // 2-bit
        pSnap.slot.buff[88]   = 1;
        setSlotFloat(pSnap.slot, 92, 0.6f);   // cold 6-bit
        setSlotDword(pSnap.slot, 96, 5);      // 4-bit
        // Weapon 1: firing, uses heat, hot (>= 1.0f -> 8-bit / 10.0)
        pSnap.slot.buff[100]  = 1;
        setSlotDword(pSnap.slot, 104, 20);
        setSlotDword(pSnap.slot, 108, 1);
        pSnap.slot.buff[112]  = 0;
        setSlotFloat(pSnap.slot, 116, 4.5f);
        setSlotDword(pSnap.slot, 120, 9);
        // Weapon 2: not firing, not forced
        pSnap.slot.buff[124]  = 0;
        pSnap.slot.buff[148]  = 1;            // sprint
        pSnap.slot.buff[149]  = 0;            // sprintLastTick
        setSlotDword(pSnap.slot, 152, 98765); // spawnAtTick
        pSnap.slot.buff[156]  = 1;            // isManDown
        setSlotWord(pSnap.slot, 158, 38);     // ping
        pSnap.slot.buff[160]  = 12;           // timeOutside (9-bit)
        pSnap.slot.buff[161]  = 3;            // kitByte (9-bit)
        pSnap.slot.buff[162]  = 1;
        pSnap.slot.buff[163]  = 1;            // freeCameraEnabled
        pSnap.slot.buff[164]  = 0;            // overheadCameraEnabled
        pSnap.slot.buff[165]  = 0x5Au;        // squadInviteMask (8-bit)
        setSlotDword(pSnap.slot, 168, 42);    // tracePlayerId (+1 = 43)
        setSlotDword(pSnap.slot, 172, 2);     // maskSetting
        pSnap.isConnected       = true;
        pSnap.weaponUsesHeat[0] = false;
        pSnap.weaponUsesHeat[1] = true;
        pSnap.weaponUsesHeat[2] = false;

        // Test Ghost precision (includes weapon section) and Full precision (omits weapon section)
        const unsigned int mask = 0x0FFFFFFFu & ~0x00010000u; // weapon 2 skips rest when not firing
        for (int precIdx = 0; precIdx < 2; ++precIdx)
        {
            const int precision = (precIdx == 0) ? io::StatePrecisionGhost : io::StatePrecisionFull;
            unsigned char bufA[256] = { 0 };
            unsigned char bufB[256] = { 0 };
            io::BitStream streamA(bufA, sizeof(bufA));
            io::BitStream streamB(bufB, sizeof(bufB));

            const bool okA = ConstNetEncoders::encodePlayer(pSnap, streamA, 0, mask, precision);

            // Reference Player::getNetUpdate bitstream
            streamB.writeBits(&mask, 28);
            writeBitRef(streamB, true);                         // 0x1 isAlive
            writeRangedRef(streamB, 4, 0, 255);                 // 0x2 spawnGroup
            {                                                   // 0x8 score block
                const unsigned int s1 = 1u, m1 = 42u;
                streamB.writeBits(&s1, 1); streamB.writeBits(&m1, 11);
                writeRangedRef(streamB, 123, 0, 2047);
                writeRangedRef(streamB, 45, 0, 2047);
                const unsigned int s2 = 1u, m2 = 17u;
                streamB.writeBits(&s2, 1); streamB.writeBits(&m2, 11);
                writeRangedRef(streamB, 99, 0, 2047);
            }
            writeBitRef(streamB, true);                         // 0x200000 flagHolder
            writeBitRef(streamB, true);                         // 0x10 callForMedic
            if (precision == io::StatePrecisionGhost)
            {
                const unsigned int presses = 2u;
                streamB.writeBits(&presses, 2);                 // 0x2000000
                // w = 0
                writeBitRef(streamB, true);
                writeRangedRef(streamB, 11, 0, 63);
                const unsigned int g0a = 2u, g0b = 5u;
                streamB.writeBits(&g0a, 2);
                writeBitRef(streamB, true);
                writeBitRef(streamB, false);
                const unsigned int q0 = static_cast<unsigned int>(static_cast<double>(0.6f) * 63.0);
                streamB.writeBits(&q0, 6);
                streamB.writeBits(&g0b, 4);
                // w = 1 (hot)
                writeBitRef(streamB, true);
                writeRangedRef(streamB, 21, 0, 63);
                const unsigned int g1a = 1u, g1b = 9u;
                streamB.writeBits(&g1a, 2);
                writeBitRef(streamB, false);
                writeBitRef(streamB, true);
                const unsigned int q1 = static_cast<unsigned int>((static_cast<double>(4.5f) / 10.0) * 255.0);
                streamB.writeBits(&q1, 8);
                streamB.writeBits(&g1b, 4);
                // w = 2 (not firing -> 1 bit only)
                writeBitRef(streamB, false);
            }
            writeBitRef(streamB, true);                         // 0x40000 sprint
            writeBitRef(streamB, false);
            writeRangedRef(streamB, 38, 0, 1023);               // 0x100 ping
            {
                const unsigned int st = 98765u;
                streamB.writeBits(&st, 32);                     // 0x20 spawnAtTick
                writeBitRef(streamB, true);                     // isManDown
            }
            writeRangedRef(streamB, 2, 0, 3);                   // 0x200 team+1
            writeRangedRef(streamB, 3, 0, 8);                   // 0x400 cameraViewMode
            writeBitRef(streamB, true);
            {
                const unsigned int tOut = 12u, kByte = 3u;
                streamB.writeBits(&tOut, 9);                    // 0x800
                streamB.writeBits(&kByte, 9);                   // 0x1000
            }
            writeRangedRef(streamB, 2, 0, 15);                  // 0x2000 squadId
            writeRangedRef(streamB, 8, 0, 31);                  // 0x4000 rank+1
            writeRangedRef(streamB, 11, 0, 255);
            writeRangedRef(streamB, 22, 0, 255);
            writeRangedRef(streamB, 33, 0, 255);
            writeBitRef(streamB, true);                         // 0x8000 freeCam
            writeBitRef(streamB, false);                        // 0x400000 overheadCam
            {
                const unsigned int inv = 0x5Au;
                streamB.writeBits(&inv, 8);                     // 0x800000
            }
            writeRangedRef(streamB, 43, 0, 65536);              // 0x1000000 tracePlayerId+1
            writeBitRef(streamB, true);                         // unmasked isSquadLeader
            {                                                   // 0x4000000 commander
                writeBitRef(streamB, true);
                writeBitRef(streamB, true);
                writeBitRef(streamB, false);
                writeBitRef(streamB, true);
                writeBitRef(streamB, false);
                writeBitRef(streamB, true);
                const unsigned int f0 = static_cast<unsigned int>(0.25 * 255.0);
                const unsigned int f1 = static_cast<unsigned int>(0.50 * 255.0);
                const unsigned int f2 = static_cast<unsigned int>(0.75 * 255.0);
                streamB.writeBits(&f0, 8);
                streamB.writeBits(&f1, 8);
                streamB.writeBits(&f2, 8);
            }
            writeBitRef(streamB, true);                         // unmasked m_isConnected
            writeRangedRef(streamB, 2, 0, 3);                   // 0x8000000 maskSetting

            checkCond(okA, precision == io::StatePrecisionGhost
                ? "Player Ghost precision encode returns true"
                : "Player Full precision encode returns true");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                precision == io::StatePrecisionGhost
                ? "Player Ghost precision bit-exact match vs Player::getNetUpdate"
                : "Player Full precision bit-exact match vs Player::getNetUpdate");
        }
    }

    // 8. ScoreManager (bit-exact match against ScoreManager::getNetUpdate + capacity preflight)
    {
        auto writeSigned17Ref = [](io::BitStream& bs, int v) {
            const unsigned int sign = (v < 0) ? 1u : 0u;
            const unsigned int mag  = (v < 0) ? (0u - static_cast<unsigned int>(v)) : static_cast<unsigned int>(v);
            bs.writeBits(&sign, 1);
            bs.writeBits(&mag, 17);
        };
        auto setRecInt = [](unsigned char* rec, int off, int v) {
            std::memcpy(rec + off, &v, sizeof(v));
        };

        ScoreManagerRecordSnap smSnap;
        std::memset(&smSnap, 0, sizeof(smSnap));
        smSnap.record[0x00] = 2;     // winner
        smSnap.record[0x01] = 1;     // victoryType
        setRecInt(smSnap.record, 0x04, 1250);  // T1 0x00000001
        setRecInt(smSnap.record, 0x0C, -50);   // T1 0x01000000 first
        setRecInt(smSnap.record, 0x40, 77);    // T1 0x01000000 second
        setRecInt(smSnap.record, 0x38, 13);    // T1 0x80000000 m_block[13]
        setRecInt(smSnap.record, 0x44, 185);   // T1 0x00000400 tickets
        smSnap.record[0x50] = 1;               // T1 0x00000400 bleed
        setRecInt(smSnap.record, 0x4C, -2);    // T1 0x00000400 ticketState
        setRecInt(smSnap.record, 0x88, -14);   // T2 0x80000000 m_block[13]
        setRecInt(smSnap.record, 0x94, 140);   // T2 0x00400000 tickets
        smSnap.record[0xA0] = 0;               // T2 0x00400000 bleed
        setRecInt(smSnap.record, 0x9C, 3);     // T2 0x00400000 ticketState

        unsigned char bufA[256] = { 0 };
        unsigned char bufB[256] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));

        // 6 set bits: 0x1, 0x01000000, 0x400, 0x00400000, 0x40000000, 0x80000000
        const unsigned int mask = 0xC1400401u;
        const bool okA = ConstNetEncoders::encodeScoreManager(smSnap, streamA, 0, mask, io::StatePrecisionFull);

        streamB.writeBits(&mask, 32);
        writeSigned17Ref(streamB, 1250);        // 0x00000001
        writeSigned17Ref(streamB, -50);         // 0x01000000 (+0x0C)
        writeSigned17Ref(streamB, 77);          // 0x01000000 (+0x40)
        writeSigned17Ref(streamB, 185);         // 0x00000400 (+0x44)
        { const unsigned int b = 1u; streamB.writeBits(&b, 1); } // +0x50 bleed
        writeSigned17Ref(streamB, -2);          // 0x00000400 (+0x4C)
        writeSigned17Ref(streamB, 140);         // 0x00400000 (+0x94)
        { const unsigned int b = 0u; streamB.writeBits(&b, 1); } // +0xA0 bleed
        writeSigned17Ref(streamB, 3);           // 0x00400000 (+0x9C)
        { const unsigned int s = 0u, w = 2u; streamB.writeBits(&s, 1); streamB.writeBits(&w, 2); } // 0x40000000
        { const unsigned int s = 0u, v = 1u; streamB.writeBits(&s, 1); streamB.writeBits(&v, 2); } // 0x80000000
        writeSigned17Ref(streamB, 13);          // +0x38
        writeSigned17Ref(streamB, -14);         // +0x88

        checkCond(okA, "ScoreManager encode returns true");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "ScoreManager bit-exact match vs ScoreManager::getNetUpdate");

        // Tight BitStream capacity preflight check: 6 set bits requires 32 + 16*6 = 128 bits remaining
        streamA.clear();
        streamA.m_bitLimit = 127;
        int preflightBits = 99;
        const bool okTight = ConstNetEncoders::encodeScoreManager(
            smSnap, streamA, 0, mask, io::StatePrecisionFull, &preflightBits);
        checkCond(!okTight && preflightBits == 0 && streamA.getBitPosition() == 0,
                  "ScoreManager preflight budget check rejects before writing 32-bit stateMask");
    }

    // 9. SpawnGroup
    {
        SpawnGroupRecordSnap sgSnap;
        sgSnap.team           = 2;
        sgSnap.active         = 2; // non-canonical even truthy byte -> must normalize to 1
        sgSnap.onlyForAI      = 0;
        sgSnap.hasSpawnPoints = 4; // non-canonical even truthy byte -> must normalize to 1
        sgSnap.posX           = 128;
        sgSnap.posZ           = 64;
        sgSnap.squad          = 3;

        unsigned char bufA[64] = { 0 };
        unsigned char bufB[64] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));

        const unsigned int mask = 0x3Fu;
        const bool okA = ConstNetEncoders::encodeSpawnGroup(sgSnap, streamA, 0, mask, io::StatePrecisionFull);

        streamB.writeBits(&mask, 6);
        const unsigned int b1 = 1u, b0 = 0u;
        streamB.writeBits(&b1, 1);
        const unsigned int teamVal = static_cast<unsigned int>(sgSnap.team);
        streamB.writeBits(&teamVal, 4);
        streamB.writeBits(&b1, 1);
        streamB.writeBits(&b0, 1);
        const unsigned int px = static_cast<unsigned int>(sgSnap.posX);
        const unsigned int pz = static_cast<unsigned int>(sgSnap.posZ);
        streamB.writeBits(&px, 8);
        streamB.writeBits(&pz, 8);
        const unsigned int wireSquad = static_cast<unsigned int>(sgSnap.squad + 1);
        streamB.writeBits(&wireSquad, 8);

        checkCond(okA && streamA.getBitPosition() == 37u,
                  "SpawnGroup exact 37-bit length");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "SpawnGroup bit-exact match (normalized bools + 4-byte widened fields)");

        // Error-path stats test: overflowing stream must return false and report 0 bitsWritten
        unsigned char tinyBuf[4] = { 0 };
        io::BitStream tinyStream(tinyBuf, sizeof(tinyBuf));
        tinyStream.m_bitLimit = 10;
        int errBits = 999;
        const bool okErr = ConstNetEncoders::encodeSpawnGroup(
            sgSnap, tinyStream, 0, mask, io::StatePrecisionFull, &errBits);
        checkCond(!okErr && errBits == 0,
                  "Encoder error path reports 0 bitsWritten (matching live stats behavior)");

        // Test encodeDescriptor via WorldNetSnapshot
        DummyStubNetworkable stubSg(77, 0x3Fu, 37);
        worldSnap.m_spawnGroupRecords.push_back(sgSnap);
        NetRecordSnap recSnap;
        recSnap.type            = NetEncType::SpawnGroup;
        recSnap.networkableId   = 77;
        recSnap.ghostStateMask  = 0x3Fu;
        recSnap.minimumBitSize  = 37;
        recSnap.typeRecordIndex = 0;
        recSnap.liveNetworkable = &stubSg;
        worldSnap.addRecordSnap(recSnap);

        io::NetworkableDescriptor desc;
        desc.controlObjId   = 1;
        desc.descId         = 0;
        desc.dword8         = 0;
        desc.addedToNetwork = true;
        desc.byte19         = false;
        desc.byte1A         = false;
        desc.byte1B         = false;
        desc.minimumBitSize = 37;
        desc.properties.resize(1);
        desc.properties[0].networkables.push_back(&stubSg);

        io::NetworkableStateMask descMask;
        std::memset(&descMask, 0, sizeof(descMask));
        descMask.m_opaque[0] = 0x3Fu;

        unsigned char bufDesc[64] = { 0 };
        io::BitStream streamDesc(bufDesc, sizeof(bufDesc));
        WorldNetSnapshot::TypeWriteBitStats bitStats;
        const bool okDesc = ConstNetEncoders::encodeDescriptor(
            worldSnap, &desc, streamDesc, &descMask, io::StatePrecisionGhost, true, &bitStats);

        checkCond(okDesc && ConstNetEncoders::compareBitStreamsBitExact(streamDesc, streamB),
                  "ConstNetEncoders::encodeDescriptor bit-exact match");
        checkCond(bitStats.bitsByType[static_cast<unsigned int>(NetEncType::SpawnGroup)] == 37,
                  "ConstNetEncoders::encodeDescriptor accumulates per-type writeBits");
    }
}

void testSpatialGridEquivalence()
{
    WorldNetSnapshot snap;
    snap.configureSpatialGrid(-2048.0f, -2048.0f, 64.0f);

    // Deterministic LCG to place 256 entities (220 inside [-2048, +2048], 36 outside in overflow bucket)
    unsigned int seed = 0x13572468u;
    auto nextFloat = [&](float lo, float hi) -> float {
        seed = seed * 1664525u + 1013904223u;
        const float unit = static_cast<float>((seed >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
        return lo + unit * (hi - lo);
    };

    const unsigned int totalRoots = 256;
    for (unsigned int i = 0; i < totalRoots; ++i)
    {
        float x, z;
        if (i < 220)
        {
            x = nextFloat(-2040.0f, 2040.0f);
            z = nextFloat(-2040.0f, 2040.0f);
        }
        else
        {
            // Out-of-bounds entities to exercise kOverflowBucket
            x = (i & 1) ? nextFloat(-3500.0f, -2050.0f) : nextFloat(2050.0f, 3500.0f);
            z = (i & 2) ? nextFloat(-3500.0f, -2050.0f) : nextFloat(2050.0f, 3500.0f);
        }
        snap.addSpatialRootForTest(x, 50.0f, z, i + 1000u);
    }

    snap.rebuildSpatialGrid();

    bool allQueriesMatch = true;
    for (int q = 0; q < 64; ++q)
    {
        const float cx     = nextFloat(-2400.0f, 2400.0f);
        const float cz     = nextFloat(-2400.0f, 2400.0f);
        const float radius = nextFloat(50.0f, 1500.0f);
        const float radSq  = radius * radius;

        std::vector<unsigned int> gridHits;
        snap.queryRadius(cx, cz, radius, [&](unsigned int rootIdx) {
            gridHits.push_back(rootIdx);
        });

        std::vector<unsigned int> bruteHits;
        for (unsigned int i = 0; i < totalRoots; ++i)
        {
            const ScopableRootSnap& r = snap.m_scopableRoots[i];
            const float dx = r.pos[0] - cx;
            const float dz = r.pos[2] - cz;
            if (dx * dx + dz * dz <= radSq)
                bruteHits.push_back(i);
        }

        std::sort(gridHits.begin(), gridHits.end());
        std::sort(bruteHits.begin(), bruteHits.end());

        if (gridHits != bruteHits)
        {
            allQueriesMatch = false;
            std::fprintf(stderr, "[FAIL] SpatialGrid query %d mismatch (grid=%zu vs brute=%zu)\n",
                         q, gridHits.size(), bruteHits.size());
            break;
        }
    }

    checkCond(allQueriesMatch, "WorldNetSnapshot 64x64+overflow 2D spatial grid matches brute-force across 64 queries");
}

void testServerTickProfiler()
{
    checkCond(!ServerTickProfiler::isEnabled(), "ServerTickProfiler disabled by default (sv-perf-profile == 0)");

    int* enabledCvar  = Vars::getInt("sv-perf-profile", 0);
    int* intervalCvar = Vars::getInt("sv-perf-profile-interval", 300);
    *enabledCvar  = 1;
    *intervalCvar = 10;

    checkCond(ServerTickProfiler::isEnabled(), "ServerTickProfiler::isEnabled() reflects sv-perf-profile == 1");

    for (int t = 0; t < 5; ++t)
    {
        ServerTickProfiler::beginTick();
        ServerTickProfiler::noteServerUpdate(64);
        ServerTickProfiler::addPhaseUs(ServerTickProfiler::PHASE_RECV, 100 + t * 10);
        ServerTickProfiler::addPhaseUs(ServerTickProfiler::PHASE_GHOST_SCOPE, 800 + t * 50);
        ServerTickProfiler::addPhaseUs(ServerTickProfiler::PHASE_GHOST_PACK, 1200 + t * 50);
        ServerTickProfiler::notePacketSent(520, 32);
        ServerTickProfiler::noteDroppedSteps(1);
        ServerTickProfiler::endTick();
    }

    checkCond(ServerTickProfiler::hasPendingSamples(), "ServerTickProfiler records pending tick samples");
    ServerTickProfiler::flushAndReset();
    checkCond(!ServerTickProfiler::hasPendingSamples(), "ServerTickProfiler::flushAndReset() clears pending samples");

    *enabledCvar = 0;
    checkCond(!ServerTickProfiler::isEnabled(), "ServerTickProfiler returns to zero-overhead disabled state");
}

void testNetWorkerPoolStress()
{
    checkCond(!NetWorkerPool::isEnabled(), "NetWorkerPool disabled by default (net-mt-enable == 0)");
    checkCond(!NetWorkerPool::isVerifyEnabled(), "NetWorkerPool verify disabled by default (net-mt-verify == 0)");
    checkCond(NetWorkerPool::getScopeHysteresis() == 0.0f, "net-scopeHysteresis defaults to 0.0f");

    // Build shared immutable WorldNetSnapshot containing all 9 record types + 256 spatial roots.
    WorldNetSnapshot snap;
    snap.configureSpatialGrid(-2048.0f, -2048.0f, 64.0f);

    // Populate all 9 record types
    {
        SoldierRecordSnap s;
        std::memset(&s, 0, sizeof(s));
        s.rec.absoluteTrans      = makeVec3(105.5f, 24.0f, -212.25f);
        s.rec.positionalSpeed    = makeVec3(3.1f, 0.0f, -2.4f);
        s.rec.facingDegrees      = 45.0f;
        s.rec.lookLeftRightAngle = 10.0f;
        s.rec.pendingTurn        = -5.0f;
        s.rec.lookUpDownAngle    = 3.0f;
        s.rec.hitPoints          = 100;
        s.rec.pose               = 0;
        s.rec.requestedPose      = 1;
        s.rec.itemIndex          = 2;
        s.rec.weaponZoom         = 0;
        s.rec.weaponState        = 1;
        s.rec.sprintStamina      = 0.8f;
        s.rec.ammoStatus         = 0xF3;
        s.hasArmor               = true;
        s.inventorySize          = 9;
        snap.m_soldierRecords.push_back(s);
        NetRecordSnap r;
        r.type = NetEncType::Soldier; r.networkableId = 101; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        ItemRecordSnap it;
        it.rec.position = makeVec3(106.0f, 24.0f, -210.0f);
        it.rec.ownerId  = 12;
        it.rec.enabled  = true;
        it.rec.team     = 1;
        snap.m_itemRecords.push_back(it);
        NetRecordSnap r;
        r.type = NetEncType::Item; r.networkableId = 102; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        NetRecordSnap r;
        r.type = NetEncType::Kit; r.networkableId = 103; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        GenericProjectileRecordSnap gp;
        std::memset(&gp, 0, sizeof(gp));
        gp.rec.position = makeVec3(110.0f, 30.0f, -200.0f);
        gp.rec.rotation = makeQuat(0.0f, 0.0f, 0.0f, 1.0f);
        gp.rec.detonate = 0;
        gp.hasArmor     = true;
        gp.armorMaxWreckHp = 10; gp.armorLower = 0; gp.armorUpper = 100;
        gp.rec.hitPointsCeil = 50;
        snap.m_projectileRecords.push_back(gp);
        NetRecordSnap r;
        r.type = NetEncType::GenericProjectile; r.networkableId = 104; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        ControlPointRecordSnap cp;
        cp.rec.team = 1; cp.rec.bool158 = true;
        cp.rec.takeOverCounter = 0.25f; cp.rec.takeOverChangePerSecond = 0.1f;
        cp.rec.allowCaptureByTeam1 = true; cp.rec.allowCaptureByTeam2 = true;
        cp.rec.flagTeam = 1; cp.rec.capturingState = 0;
        snap.m_controlPointRecords.push_back(cp);
        NetRecordSnap r;
        r.type = NetEncType::ControlPoint; r.networkableId = 105; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        SimpleObjectRecordSnap so;
        std::memset(&so, 0, sizeof(so));
        so.rec.position = makeVec3(104.0f, 24.0f, -211.0f);
        so.rec.rotation = makeQuat(0.0f, 0.0f, 0.0f, 1.0f);
        so.rec.vec38    = makeVec3(1.0f, 0.0f, 0.5f);
        so.rec.vec5C    = makeVec3(0.0f, 0.1f, 0.0f);
        so.rec.hitPoints = 200;
        so.isMobilePhysics = true;
        so.hasArmor = true; so.armorMaxWreckHp = 20; so.armorLower = 0; so.armorUpper = 400;
        so.hasPco = true; so.isParachute = false;
        so.rec.weaponIndex[0] = 0; so.rec.weaponIndex[1] = -1; so.rec.weaponIndex[2] = -1;
        snap.m_simpleObjectRecords.push_back(so);
        NetRecordSnap r;
        r.type = NetEncType::SimpleObject; r.networkableId = 106; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        PlayerRecordSnap pr;
        std::memset(&pr, 0, sizeof(pr));
        pr.slot.isAlive = 1;
        pr.slot.someScore2 = 55;
        int teamVal = 2; std::memcpy(&pr.slot.buff[16], &teamVal, sizeof(teamVal));
        unsigned short pingVal = 27; std::memcpy(&pr.slot.buff[158], &pingVal, sizeof(pingVal));
        pr.slot.buff[76] = 1;
        pr.isConnected = true;
        snap.m_playerRecords.push_back(pr);
        NetRecordSnap r;
        r.type = NetEncType::Player; r.networkableId = 107; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        ScoreManagerRecordSnap sm;
        std::memset(&sm, 0, sizeof(sm));
        sm.record[0x00] = 1;
        sm.record[0x01] = 2;
        int t1s = 400, t1t = 150, t2s = 350, t2t = 120;
        std::memcpy(sm.record + 0x04, &t1s, sizeof(t1s));
        std::memcpy(sm.record + 0x44, &t1t, sizeof(t1t));
        std::memcpy(sm.record + 0x54, &t2s, sizeof(t2s));
        std::memcpy(sm.record + 0x94, &t2t, sizeof(t2t));
        sm.record[0x50] = 1;
        snap.m_scoreManagerRecords.push_back(sm);
        NetRecordSnap r;
        r.type = NetEncType::ScoreManager; r.networkableId = 108; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        SpawnGroupRecordSnap sg;
        sg.team = 1; sg.active = 1; sg.onlyForAI = 0; sg.hasSpawnPoints = 1;
        sg.posX = 90; sg.posZ = 140; sg.squad = 2;
        snap.m_spawnGroupRecords.push_back(sg);
        NetRecordSnap r;
        r.type = NetEncType::SpawnGroup; r.networkableId = 109; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }

    unsigned int seed = 0xCAFEBABEu;
    auto nextFloat = [&](float lo, float hi) -> float {
        seed = seed * 1664525u + 1013904223u;
        const float u = static_cast<float>((seed >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
        return lo + u * (hi - lo);
    };
    for (unsigned int i = 0; i < 256; ++i)
        snap.addSpatialRootForTest(nextFloat(-2400.0f, 2400.0f), 20.0f, nextFloat(-2400.0f, 2400.0f), i);
    snap.rebuildSpatialGrid();

    const std::size_t kConnections = 64;
    struct ConnJob
    {
        Vec3                     compVec;
        float                    queryX;
        float                    queryZ;
        float                    queryRadius;
        int                      precision;
        unsigned char            goldenBuf[1024];
        unsigned int             goldenBitPos;
        unsigned int             goldenHighWater;
        std::vector<unsigned int> goldenHits;
        unsigned char            mtBuf[1024];
        unsigned int             mtBitPos;
        unsigned int             mtHighWater;
        std::vector<unsigned int> mtHits;
        bool                     ok;
    };

    std::vector<ConnJob> jobs(kConnections);

    auto runOneConnection = [&](const ConnJob& job, unsigned char* outBuf,
                                unsigned int& outBitPos, unsigned int& outHighWater,
                                std::vector<unsigned int>& outHits) -> bool {
        std::memset(outBuf, 0, 1024);
        io::BitStream stream(outBuf, 1024);
        stream.setCompressionVector(job.compVec);

        io::BaseLineData base;
        std::memset(&base, 0, sizeof(base));
        Vec3* refs = reinterpret_cast<Vec3*>(base.m_data);
        refs[0] = job.compVec;
        refs[1] = makeVec3(0.5f, 0.0f, 0.25f);
        refs[2] = makeVec3(0.0f, 0.0f, 0.0f);

        bool allOk = true;
        for (unsigned short nid = 101; nid <= 109; ++nid)
        {
            const NetRecordSnap* rec = snap.findRecordByNetId(nid);
            io::BaseLineData* bp = (nid == 106) ? &base : 0;
            if (!rec || !ConstNetEncoders::encodeRecord(snap, *rec, stream, bp, 0xFFFFFFFFu, job.precision))
                allOk = false;
        }

        outBitPos    = stream.getBitPosition();
        outHighWater = stream.m_highWaterBits;

        outHits.clear();
        snap.queryRadius(job.queryX, job.queryZ, job.queryRadius, [&](unsigned int rootIdx) {
            outHits.push_back(rootIdx);
        });
        std::sort(outHits.begin(), outHits.end());
        return allOk;
    };

    // Single-threaded golden pass
    for (std::size_t c = 0; c < kConnections; ++c)
    {
        jobs[c].compVec     = makeVec3(100.0f + static_cast<float>(c), 24.0f, -210.0f - static_cast<float>(c));
        jobs[c].queryX      = nextFloat(-2200.0f, 2200.0f);
        jobs[c].queryZ      = nextFloat(-2200.0f, 2200.0f);
        jobs[c].queryRadius = nextFloat(100.0f, 800.0f);
        jobs[c].precision   = (c & 1) ? io::StatePrecisionFull : io::StatePrecisionGhost;
        jobs[c].ok          = runOneConnection(
            jobs[c], jobs[c].goldenBuf, jobs[c].goldenBitPos, jobs[c].goldenHighWater, jobs[c].goldenHits);
        checkCond(jobs[c].ok && jobs[c].goldenBitPos > 0, "MT stress golden reference encode succeeded");
    }

    // Multi-threaded stress pass: 4 threads x 64 connections x 8 ticks
    NetWorkerPool pool;
    pool.ensureStarted(4);
    checkCond(pool.workerCount() == 3, "NetWorkerPool started 3 background workers (+ main = 4 threads)");

    bool allTicksExact = true;
    for (int tick = 0; tick < 8; ++tick)
    {
        pool.runParallel(kConnections, [&](std::size_t idx) {
            jobs[idx].ok = runOneConnection(
                jobs[idx], jobs[idx].mtBuf, jobs[idx].mtBitPos, jobs[idx].mtHighWater, jobs[idx].mtHits);
        });

        for (std::size_t c = 0; c < kConnections; ++c)
        {
            if (!jobs[c].ok
                || jobs[c].mtBitPos != jobs[c].goldenBitPos
                || jobs[c].mtHighWater != jobs[c].goldenHighWater
                || jobs[c].mtHits != jobs[c].goldenHits)
            {
                allTicksExact = false;
                break;
            }
            const unsigned int fullBytes = jobs[c].goldenHighWater >> 3;
            const unsigned int remBits   = jobs[c].goldenHighWater & 7u;
            if (fullBytes > 0 && std::memcmp(jobs[c].mtBuf, jobs[c].goldenBuf, fullBytes) != 0)
            {
                allTicksExact = false;
                break;
            }
            if (remBits > 0)
            {
                const unsigned char mask = static_cast<unsigned char>((1u << remBits) - 1u);
                if ((jobs[c].mtBuf[fullBytes] & mask) != (jobs[c].goldenBuf[fullBytes] & mask))
                {
                    allTicksExact = false;
                    break;
                }
            }
        }
        if (!allTicksExact)
            break;
    }

    pool.shutdown();
    checkCond(pool.workerCount() == 0, "NetWorkerPool::shutdown() cleanly joined all worker threads");
    checkCond(allTicksExact, "NetWorkerPool 4-thread x 64-conn x 8-tick stress test: 0 bits diff & 0 spatial mismatches");

    // Verify shutdown() -> restart lifecycle (regression test for m_generation reset race).
    bool restartExact = true;
    for (int cycle = 0; cycle < 4; ++cycle)
    {
        pool.ensureStarted(4);
        pool.runParallel(kConnections, [&](std::size_t idx) {
            jobs[idx].ok = runOneConnection(
                jobs[idx], jobs[idx].mtBuf, jobs[idx].mtBitPos, jobs[idx].mtHighWater, jobs[idx].mtHits);
        });
        for (std::size_t c = 0; c < kConnections; ++c)
        {
            if (!jobs[c].ok
                || jobs[c].mtBitPos != jobs[c].goldenBitPos
                || jobs[c].mtHighWater != jobs[c].goldenHighWater
                || jobs[c].mtHits != jobs[c].goldenHits)
            {
                restartExact = false;
                break;
            }
        }
        pool.shutdown();
    }
    checkCond(restartExact && pool.workerCount() == 0,
              "NetWorkerPool shutdown/restart 4-cycle stress test: 0 races & bit-exact output");
}

} // anonymous namespace
} // namespace netpipeline
} // namespace hfe
} // namespace dice

int main()
{
    dice::hfe::netpipeline::testAllNineConstNetEncoders();
    dice::hfe::netpipeline::testSpatialGridEquivalence();
    dice::hfe::netpipeline::testServerTickProfiler();
    dice::hfe::netpipeline::testNetWorkerPoolStress();

    if (dice::hfe::netpipeline::g_failures == 0)
    {
        std::printf("[PASS] net_pipeline_golden_test: all %d checks passed.\n",
                    dice::hfe::netpipeline::g_passes);
        return 0;
    }

    std::fprintf(stderr, "[FAIL] net_pipeline_golden_test: %d failures (%d passed).\n",
                 dice::hfe::netpipeline::g_failures,
                 dice::hfe::netpipeline::g_passes);
    return 1;
}
