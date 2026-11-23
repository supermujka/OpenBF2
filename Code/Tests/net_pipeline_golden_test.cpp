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

        const unsigned char origAmmoStatus = s.rec.ammoStatus;

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
        cpSnap.rec.m_team                    = -1;
        cpSnap.rec.m_bool158                 = true;
        cpSnap.rec.m_takeOverCounter         = 0.75f;
        cpSnap.rec.m_takeOverChangePerSecond = -0.5f;
        cpSnap.rec.m_allowCaptureByTeam1     = true;
        cpSnap.rec.m_allowCaptureByTeam2     = false;
        cpSnap.rec.m_flagTeam                = 2;
        cpSnap.rec.m_capturingState          = -2;

        unsigned char bufA[128] = { 0 };
        unsigned char bufB[128] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));

        const unsigned int mask = 0x7Fu;
        const bool okA = ConstNetEncoders::encodeControlPoint(cpSnap, streamA, 0, mask, io::StatePrecisionFull);

        streamB.writeBits(&mask, 7);
        const unsigned int s1 = 1u, m1 = 1u;
        streamB.writeBits(&s1, 1); streamB.writeBits(&m1, 3);
        streamB.writeBits(&cpSnap.rec.m_bool158, 1);
        const unsigned int qCounter = static_cast<unsigned int>(((0.75 / 1.25 + 1.0) * 0.5) * 65535.0);
        const unsigned int qRate    = static_cast<unsigned int>(((-0.5 / 1.25 + 1.0) * 0.5) * 65535.0);
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
        soSnap.rec.byte9C           = 1;
        soSnap.rec.hitPoints        = 300;
        soSnap.rec.replicationState = 2;
        soSnap.rec.weaponIndex[0]   = 0;
        soSnap.rec.weaponIndex[1]   = 2;
        soSnap.rec.weaponIndex[2]   = -1;
        soSnap.rec.supplyB0         = 1;
        soSnap.rec.pcoB4            = 5;
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

            // Reference baseline delta encode
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
            streamB.writeBits(&soSnap.rec.byte9C, 1);
            streamB.writeBits(&soSnap.rec.supplyB0, 1);
            const unsigned int hpOff = 350u; // 300 + 50 in [0..500] -> 9 bits
            streamB.writeBits(&hpOff, 9);
            streamB.writeBits(&soSnap.rec.replicationState, 2);
            const unsigned int w0 = 1u, w1 = 3u, w2 = 0u, pcoB4 = 5u, duck = 1u;
            streamB.writeBits(&w0, 4);
            streamB.writeBits(&w1, 4);
            streamB.writeBits(&w2, 4);
            streamB.writeBits(&pcoB4, 4);
            streamB.writeBits(&duck, 1);

            checkCond(okA && baseA.m_cursor == baseB.m_cursor && baseA.m_cursor == 42,
                      "SimpleObjectNetworkable baseline cursor advances by +36");
            checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                      "SimpleObjectNetworkable baseline delta bit-exact match");
        }
    }

    // 7. Player
    {
        PlayerRecordSnap pSnap;
        std::memset(&pSnap, 0, sizeof(pSnap));
        pSnap.slot.m_spawned                  = true;
        pSnap.slot.m_down                     = false;
        pSnap.slot.m_spawnGroupId             = 4;
        pSnap.slot.m_team                     = 1;
        pSnap.slot.m_score                    = 42;
        pSnap.slot.m_skillScore               = 19;
        pSnap.slot.m_teamWorkScore            = 23;
        pSnap.slot.m_rank                     = 7;
        pSnap.slot.m_kills                    = 15;
        pSnap.slot.m_deaths                   = 6;
        pSnap.slot.m_ping                     = 38;
        pSnap.slot.m_vehicleValid             = true;
        pSnap.slot.m_vehicleNetId             = 1024;
        pSnap.slot.m_subPcoId                 = 0;
        pSnap.slot.m_cameraNetId              = 1025;
        pSnap.slot.m_defaultVehicleValid      = true;
        pSnap.slot.m_defaultVehicleNetId      = 2048;
        pSnap.slot.m_kitValid                 = true;
        pSnap.slot.m_kitTemplateId            = 333;
        pSnap.slot.m_isInsideCP               = true;
        pSnap.slot.m_cpTakeOverProgress       = 100.0f;
        pSnap.slot.m_cpTakeOverSpeed          = 0.5f;
        pSnap.slot.m_flagHolder               = false;
        pSnap.slot.m_squadId                  = 2;
        pSnap.slot.m_isSquadLeader            = true;
        pSnap.slot.m_Commander                = false;
        pSnap.slot.m_commanderApplicantsValid = true;
        pSnap.slot.m_ReplenishReq             = true;
        pSnap.slot.m_RepairPoint              = false;
        pSnap.slot.m_HealPoint                = true;
        pSnap.slot.m_ammo[0]                  = 29;
        pSnap.slot.m_nrOfMags[0]              = 5;
        pSnap.slot.m_heat[1]                  = 0.6f;
        pSnap.slot.m_ammo[2]                  = 1;
        pSnap.slot.m_nrOfMags[2]              = 3;
        pSnap.slot.m_fireRateMod[0]           = 1;
        pSnap.slot.m_fireRateMod[1]           = 0;
        pSnap.slot.m_fireRateMod[2]           = 2;
        pSnap.slot.m_LockonState              = 3;
        pSnap.slot.m_targetNetId              = 777;
        pSnap.slot.m_subControllerIndex       = 2;
        pSnap.slot.m_VoipOn                   = true;
        pSnap.slot.m_SquadLeaderVoipOn        = false;
        pSnap.isConnected                     = true;
        pSnap.weaponUsesHeat[0]               = false;
        pSnap.weaponUsesHeat[1]               = true;
        pSnap.weaponUsesHeat[2]               = false;

        unsigned char bufA[256] = { 0 };
        unsigned char bufB[256] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        io::BitStream streamB(bufB, sizeof(bufB));

        const unsigned int mask = 0x1FFFFFFFu;
        const bool okA = ConstNetEncoders::encodePlayer(pSnap, streamA, 0, mask, io::StatePrecisionFull);
        const bool okB = ConstNetEncoders::encodePlayer(pSnap, streamB, 0, mask, io::StatePrecisionGhost);
        checkCond(okA && okB && ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "Player full 28-bit stateMask encode succeeds deterministically");
        checkCond(streamA.getBitPosition() == 240u, "Player full 28-bit stateMask exact bit length (240 bits)");
    }

    // 8. ScoreManager (full 32-bit mask + capacity preflight check)
    {
        ScoreManagerRecordSnap smSnap;
        std::memset(&smSnap, 0, sizeof(smSnap));
        *reinterpret_cast<int*>(smSnap.record + 4)   = 1250; // team 1 score
        *reinterpret_cast<int*>(smSnap.record + 16)  = 185;  // team 1 tickets (15b mag)
        *reinterpret_cast<int*>(smSnap.record + 72)  = -12;  // team 1 ticketLossPerMin (7b mag)
        *reinterpret_cast<int*>(smSnap.record + 84)  = 980;  // team 2 score
        *reinterpret_cast<int*>(smSnap.record + 96)  = 140;  // team 2 tickets (15b mag)
        *reinterpret_cast<int*>(smSnap.record + 152) = 8;    // team 2 ticketLossPerMin (7b mag)
        *reinterpret_cast<int*>(smSnap.record + 160) = 1;    // winner (2b mag)
        *reinterpret_cast<int*>(smSnap.record + 156) = -3;   // victoryType (4b mag)

        unsigned char bufA[256] = { 0 };
        io::BitStream streamA(bufA, sizeof(bufA));
        const unsigned int mask = 0xC0042009u; // 7 bits set
        const bool okA = ConstNetEncoders::encodeScoreManager(smSnap, streamA, 0, mask, io::StatePrecisionFull);
        // 32 (mask) + (1+16) + (1+15) + (1+7) + (1+16) + (1+15) + (1+3) + (1+5) = 32 + 17 + 16 + 8 + 17 + 16 + 3 + 5 = 114 bits
        checkCond(okA && streamA.getBitPosition() == 114u,
                  "ScoreManager exact bit length (114 bits) for signed/variable-width fields");

        // Tight BitStream capacity preflight check: 7 set bits requires 32 + 16*7 = 144 bits remaining
        streamA.clear();
        streamA.m_bitLimit = 143;
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
        sgSnap.active         = 1;
        sgSnap.onlyForAI      = 0;
        sgSnap.hasSpawnPoints = 1;
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
        streamB.writeBits(&sgSnap.hasSpawnPoints, 1);
        const unsigned char teamByte = static_cast<unsigned char>(sgSnap.team);
        streamB.writeBits(&teamByte, 4);
        streamB.writeBits(&sgSnap.active, 1);
        streamB.writeBits(&sgSnap.onlyForAI, 1);
        streamB.writeBits(&sgSnap.posX, 8);
        streamB.writeBits(&sgSnap.posZ, 8);
        const unsigned int wireSquad = static_cast<unsigned int>(sgSnap.squad + 1);
        streamB.writeBits(&wireSquad, 8);

        checkCond(okA && streamA.getBitPosition() == 37u,
                  "SpawnGroup exact 37-bit length");
        checkCond(ConstNetEncoders::compareBitStreamsBitExact(streamA, streamB),
                  "SpawnGroup bit-exact match");

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
        cp.rec.m_team = 1; cp.rec.m_bool158 = true;
        cp.rec.m_takeOverCounter = 0.25f; cp.rec.m_takeOverChangePerSecond = 0.1f;
        cp.rec.m_allowCaptureByTeam1 = true; cp.rec.m_allowCaptureByTeam2 = true;
        cp.rec.m_flagTeam = 1; cp.rec.m_capturingState = 0;
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
        pr.slot.m_spawned = true; pr.slot.m_team = 2; pr.slot.m_score = 55;
        pr.slot.m_kills = 9; pr.slot.m_deaths = 4; pr.slot.m_ping = 27;
        pr.slot.m_ammo[0] = 30; pr.slot.m_nrOfMags[0] = 6;
        pr.isConnected = true;
        snap.m_playerRecords.push_back(pr);
        NetRecordSnap r;
        r.type = NetEncType::Player; r.networkableId = 107; r.typeRecordIndex = 0;
        snap.addRecordSnap(r);
    }
    {
        ScoreManagerRecordSnap sm;
        std::memset(&sm, 0, sizeof(sm));
        *reinterpret_cast<int*>(sm.record + 4)   = 400;
        *reinterpret_cast<int*>(sm.record + 16)  = 150;
        *reinterpret_cast<int*>(sm.record + 84)  = 350;
        *reinterpret_cast<int*>(sm.record + 96)  = 120;
        *reinterpret_cast<int*>(sm.record + 160) = 0;
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
