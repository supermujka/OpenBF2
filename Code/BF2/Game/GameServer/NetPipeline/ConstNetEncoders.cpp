// [CHOSEN, not recovered] Pure-const, thread-safe bit-exact network encoders for Stage B
// worker threads. Every encoder matches its live *Networkable::getNetUpdate bit-for-bit.

#include "ConstNetEncoders.h"

#include <cstring>

namespace dice {
namespace hfe {
namespace netpipeline {

namespace {

inline void writeBit(io::BitStream& stream, bool value)
{
    const unsigned int bit = value ? 1u : 0u;
    stream.writeBits(&bit, 1);
}

inline void writeSignedMagnitude(io::BitStream& stream, int value, unsigned int magBits)
{
    const unsigned int sign = (value < 0) ? 1u : 0u;
    const unsigned int mag  = (value < 0)
        ? (0u - static_cast<unsigned int>(value))
        : static_cast<unsigned int>(value);
    stream.writeBits(&sign, 1);
    stream.writeBits(&mag, magBits);
}

inline void writeSigned17(io::BitStream& stream, int value)
{
    writeSignedMagnitude(stream, value, 17);
}

inline int scoreRecordInt(const unsigned char* record, int offset)
{
    int value = 0;
    std::memcpy(&value, record + offset, sizeof(value));
    return value;
}

inline int playerSlotDword(const NetUpdateSlot& slot, int offset)
{
    int value = 0;
    std::memcpy(&value, &slot.buff[offset], sizeof(value));
    return value;
}

inline unsigned short playerSlotWord(const NetUpdateSlot& slot, int offset)
{
    unsigned short value = 0;
    std::memcpy(&value, &slot.buff[offset], sizeof(value));
    return value;
}

inline float playerSlotFloat(const NetUpdateSlot& slot, int offset)
{
    float value = 0.0f;
    std::memcpy(&value, &slot.buff[offset], sizeof(value));
    return value;
}

inline void writePositiveFlagAsSigned3(io::BitStream& stream, bool flag)
{
    const unsigned int zero = 0;
    const unsigned int mag  = flag ? 1u : 0u;
    stream.writeBits(&zero, 1);
    stream.writeBits(&mag, 3);
}

inline void writeQuantizedTakeOver(io::BitStream& stream, float v)
{
    double t = (static_cast<double>(v) / 1.25 + 1.0) * 0.5;
    if (t < 0.0)
        t = 0.0;
    else if (t > 1.0)
        t = 1.0;
    const unsigned int q = static_cast<unsigned int>(static_cast<int>(t * 65535.0));
    stream.writeBits(&q, 16);
}

inline void writeRangedIntOffset(io::BitStream& stream, int value, int lower, int upper)
{
    if (value < lower || value > upper)
    {
        stream.setError(4);
        return;
    }

    const unsigned int span = static_cast<unsigned int>(upper - lower) + 1u;
    unsigned int bits = 0;
    if (span != 0)
    {
        do
        {
            ++bits;
        } while (span > (1u << bits) - 1u);
    }

    const unsigned int offset = static_cast<unsigned int>(value - lower);
    stream.writeBits(&offset, bits);
}

inline void writeRangedUIntZeroBased(io::BitStream& stream, int value, unsigned int maxVal, unsigned int bits)
{
    if (value < 0 || static_cast<unsigned int>(value) > maxVal)
    {
        stream.setError(4);
        return;
    }
    const unsigned int v = static_cast<unsigned int>(value);
    stream.writeBits(&v, bits);
}

} // anonymous namespace

bool ConstNetEncoders::encodeSoldier(
    const SoldierRecordSnap& snap,
    const RagDollParticleSnap* ragdollParticles,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int precision,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const SoldierNetworkable::UPDATE& rec = snap.rec;

    unsigned int mask;
    if (snap.isAwakeOccupiedRagdoll)
        mask = 0x8040u;
    else
        mask = stateMask & ~0x8000u;

    stream.writeBits(&mask, 21);

    if (snap.hasArmor)
    {
        if ((mask & 0x40u) != 0)
            stream.writeBits(&rec.hitPoints, 8);
        if ((mask & 0x40u) != 0)
            stream.writeBits(&rec.lastDamageType, 1);
    }

    if ((mask & 0x20u) != 0)
    {
        const unsigned int pose = static_cast<unsigned int>(rec.pose);
        if (pose > 4u)
            stream.setError(4);
        else
            stream.writeBits(&pose, 3);
    }

    if ((mask & 0x20u) != 0)
    {
        const unsigned int requestedPose = static_cast<unsigned int>(rec.requestedPose);
        if (requestedPose > 4u)
            stream.setError(4);
        else
            stream.writeBits(&requestedPose, 3);
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
        const unsigned int ragdollPayloadStart = stream.getBitPosition();

        const Vec3* reference = 0;
        const unsigned int count = snap.ragdollParticleCount;
        if (ragdollParticles != 0)
        {
            for (unsigned int i = 0; i < count; ++i)
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
        }

        payloadBits = stream.getBitPosition() - ragdollPayloadStart;
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
            {
                stream.writeCompressedVector(
                    rec.absoluteTrans, stream.getCompressionVector(), 0.001f);
            }
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
            if ((mask & 0x80u) != 0)
                stream.writeCompressedVector(rec.positionalSpeed, zeroVec3, 0.001f);
            if ((mask & 0x100u) != 0)
                stream.writeCompressedVector(rec.positionalAcceleration, zeroVec3, 0.000099999997f);
            if ((mask & 0x200u) != 0)
                stream.writeCompressedVector(rec.positionalFriction, zeroVec3, 0.000099999997f);
            if ((mask & 0x40000u) != 0)
                stream.writeCompressedVector(rec.localLinearSpeed, zeroVec3, 0.001f);
        }
        else if ((mask & 0x80u) != 0)
        {
            stream.writeCompressedVector(rec.positionalSpeed, zeroVec3, 0.010000001f);
        }

        if ((mask & 0x2u) != 0)
        {
            if (precision == io::StatePrecisionFull)
            {
                stream.writeBits(&rec.facingDegrees, 32);
            }
            else
            {
                float norm = (rec.facingDegrees / 360.0f + 1.0f) * 0.5f;
                if (norm > 1.0f) norm = 1.0f;
                else if (norm < 0.0f) norm = 0.0f;
                const unsigned int q = static_cast<unsigned int>(norm * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x4u) != 0)
        {
            if (precision == io::StatePrecisionFull)
            {
                stream.writeBits(&rec.lookLeftRightAngle, 32);
            }
            else
            {
                float norm = (rec.lookLeftRightAngle / 90.0f + 1.0f) * 0.5f;
                if (norm > 1.0f) norm = 1.0f;
                else if (norm < 0.0f) norm = 0.0f;
                const unsigned int q = static_cast<unsigned int>(norm * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x8u) != 0)
        {
            if (precision == io::StatePrecisionFull)
            {
                stream.writeBits(&rec.pendingTurn, 32);
            }
            else
            {
                float norm = (rec.pendingTurn / 180.0f + 1.0f) * 0.5f;
                if (norm > 1.0f) norm = 1.0f;
                else if (norm < 0.0f) norm = 0.0f;
                const unsigned int q = static_cast<unsigned int>(norm * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x10u) != 0)
        {
            if (precision == io::StatePrecisionFull)
            {
                stream.writeBits(&rec.lookUpDownAngle, 32);
            }
            else
            {
                float norm = (rec.lookUpDownAngle / 90.0f + 1.0f) * 0.5f;
                if (norm > 1.0f) norm = 1.0f;
                else if (norm < 0.0f) norm = 0.0f;
                const unsigned int q = static_cast<unsigned int>(norm * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        {
            const unsigned int weaponZoom = static_cast<unsigned int>(rec.weaponZoom);
            if (weaponZoom > 3u)
                stream.setError(4);
            else
                stream.writeBits(&weaponZoom, 3);
        }

        {
            const unsigned int weaponState = static_cast<unsigned int>(rec.weaponState);
            if (weaponState > 1u)
                stream.setError(4);
            else
                stream.writeBits(&weaponState, 2);
        }

        {
            const unsigned int skyDivingBit = rec.isSkyDiving ? 1u : 0u;
            stream.writeBits(&skyDivingBit, 1);
            const unsigned int swimmingBit = rec.isSwimming ? 1u : 0u;
            stream.writeBits(&swimmingBit, 1);
            const unsigned int stowedBit = rec.weaponStowed ? 1u : 0u;
            stream.writeBits(&stowedBit, 1);
        }

        if ((mask & 0x400u) != 0)
        {
            float norm = (rec.smoothedForward / 50.0f + 1.0f) * 0.5f;
            if (norm > 1.0f) norm = 1.0f;
            else if (norm < 0.0f) norm = 0.0f;

            if (precision == io::StatePrecisionFull)
            {
                const unsigned int q = static_cast<unsigned int>(norm * 65535.0f);
                stream.writeBits(&q, 16);
            }
            else
            {
                const unsigned int q = static_cast<unsigned int>(norm * 1023.0f);
                stream.writeBits(&q, 10);
            }
        }

        if ((mask & 0x800u) != 0)
        {
            float norm = (rec.smoothedStrafe / 50.0f + 1.0f) * 0.5f;
            if (norm > 1.0f) norm = 1.0f;
            else if (norm < 0.0f) norm = 0.0f;

            if (precision == io::StatePrecisionFull)
            {
                const unsigned int q = static_cast<unsigned int>(norm * 65535.0f);
                stream.writeBits(&q, 16);
            }
            else
            {
                const unsigned int q = static_cast<unsigned int>(norm * 1023.0f);
                stream.writeBits(&q, 10);
            }
        }

        {
            const unsigned int responseBit = rec.responseByteFA ? 1u : 0u;
            stream.writeBits(&responseBit, 1);
        }

        if ((mask & 0x4000u) != 0)
        {
            if (precision == io::StatePrecisionFull)
            {
                float clamped = rec.sprintStamina;
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < 0.0f) clamped = 0.0f;
                const unsigned int q = static_cast<unsigned int>(clamped * 127.0f);
                stream.writeBits(&q, 7);

                const unsigned int sprintingBit = rec.isSprinting ? 1u : 0u;
                stream.writeBits(&sprintingBit, 1);
            }
            else
            {
                float clamped = rec.sprintStamina;
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < 0.0f) clamped = 0.0f;
                const unsigned int q = static_cast<unsigned int>(clamped * 31.0f);
                stream.writeBits(&q, 5);
            }
        }

        if ((mask & 0x1000u) != 0)
        {
            const int invSize = snap.inventorySize;
            const unsigned int upper = static_cast<unsigned int>(invSize + 1);
            const unsigned int value = static_cast<unsigned int>(rec.itemIndex + 1);

            if (value > upper)
            {
                stream.setError(4);
            }
            else
            {
                unsigned int bits = 0;
                if (invSize != -2)
                {
                    const unsigned int range = static_cast<unsigned int>(invSize + 2);
                    do
                    {
                        ++bits;
                    } while (range > (1u << bits) - 1u);
                }
                stream.writeBits(&value, bits);
            }
        }

        if ((mask & 0x2000u) != 0)
        {
            stream.writeBits(&rec.poseInputLatch, 2);
        }

        if (precision == io::StatePrecisionFull && (mask & 0x20000u) != 0)
        {
            float norm = (rec.currentAbility + 1.0f) * 0.5f;
            if (norm > 1.0f) norm = 1.0f;
            else if (norm < 0.0f) norm = 0.0f;
            const unsigned int q = static_cast<unsigned int>(norm * 127.0f);
            stream.writeBits(&q, 7);
        }

        if ((mask & 0x10000u) != 0)
        {
            const unsigned int ladderId = rec.ladderId;
            if (ladderId > 0xFFu)
                stream.setError(4);
            else
                stream.writeBits(&ladderId, 9);

            if (rec.ladderId != 0)
            {
                float clamped = rec.ladderState;
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < 0.0f) clamped = 0.0f;
                const unsigned int q = static_cast<unsigned int>(clamped * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x100000u) != 0)
        {
            stream.writeBits(&rec.ziplineId, 16);
            if (rec.ziplineId != 0)
            {
                float clamped = rec.ziplineState;
                if (clamped > 1.0f) clamped = 1.0f;
                else if (clamped < 0.0f) clamped = 0.0f;
                const unsigned int q = static_cast<unsigned int>(clamped * 4095.0f);
                stream.writeBits(&q, 12);
            }
        }

        if ((mask & 0x80000u) != 0)
        {
            const unsigned int maskedAmmo = static_cast<unsigned int>(rec.ammoStatus & 0x0Fu);
            stream.writeBits(&maskedAmmo, 5);
        }
    }

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeItem(
    const ItemRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const ItemNetworkable::Record& rec = snap.rec;

    stream.writeBits(&stateMask, 5);

    if (stateMask & 0x01u)
    {
        if (stream.hasCompressionVector())
        {
            stream.writeCompressedVector(
                rec.position, stream.getCompressionVector(), 0.0005f);
        }
        else
        {
            const unsigned int zero = 0;
            stream.writeBits(&zero, 2);
            stream.writeBits(&rec.position.x, 32);
            stream.writeBits(&rec.position.y, 32);
            stream.writeBits(&rec.position.z, 32);
        }
    }

    if (stateMask & 0x02u)
        writeSignedMagnitude(stream, rec.ownerId, 8);

    if (stateMask & 0x08u)
        writeBit(stream, rec.enabled);

    if (stateMask & 0x10u)
        writeSignedMagnitude(stream, rec.team, 4);

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeKit(
    io::BitStream& /*stream*/,
    io::BaseLineData* /*baseline*/,
    unsigned int /*stateMask*/,
    int /*precision*/,
    int* outBitsWritten)
{
    if (outBitsWritten)
        *outBitsWritten = 0;
    return true;
}

bool ConstNetEncoders::encodeGenericProjectile(
    const GenericProjectileRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const GenericProjectileNetworkable::Record& rec = snap.rec;

    stream.writeBits(&stateMask, 5);

    if ((stateMask & 0x2u) != 0)
    {
        if (stream.hasCompressionVector())
        {
            stream.writeCompressedVector(
                rec.position, stream.getCompressionVector(), 0.0099999998f);
        }
        else
        {
            const unsigned int mode = 0;
            stream.writeBits(&mode, 2);
            stream.writeBits(&rec.position.x, 32);
            stream.writeBits(&rec.position.y, 32);
            stream.writeBits(&rec.position.z, 32);
        }
    }

    if ((stateMask & 0x1u) != 0)
    {
        stream.writeUnitQuaternion(rec.rotation, 12);
    }

    if ((stateMask & 0x4u) != 0)
    {
        const unsigned int detonateBit = (rec.detonate != 0) ? 1u : 0u;
        stream.writeBits(&detonateBit, 1);

        if (rec.detonate == 1)
        {
            writeSignedMagnitude(stream, rec.field1C4, 15);
            const Vec3 zeroVec = { 0.0f, 0.0f, 0.0f };
            stream.writeHighCompressedVector(rec.vector1CC, zeroVec, 0.0099999998f);

            const unsigned int hasTarget = rec.hasTargetNetworkable ? 1u : 0u;
            stream.writeBits(&hasTarget, 1);

            if (rec.hasTargetNetworkable)
            {
                stream.writeBits(&rec.targetNetworkableId, 16);
            }
        }
    }

    if (snap.hasArmor && (stateMask & 0x10u) != 0)
    {
        const int value = rec.hitPointsCeil + snap.armorMaxWreckHp;
        writeRangedIntOffset(stream, value, snap.armorLower, snap.armorUpper);
    }

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeControlPoint(
    const ControlPointRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const ControlPointNetworkable::BaseLine& rec = snap.rec;

    stream.writeBits(&stateMask, 7);

    if (stateMask & 0x01u)
        writeSignedMagnitude(stream, rec.team, 3);
    if (stateMask & 0x02u)
        writeBit(stream, rec.bool158);
    if (stateMask & 0x04u)
    {
        writeQuantizedTakeOver(stream, rec.takeOverCounter);
        writeQuantizedTakeOver(stream, rec.takeOverChangePerSecond);
    }
    if (stateMask & 0x08u)
        writePositiveFlagAsSigned3(stream, rec.allowCaptureByTeam1);
    if (stateMask & 0x10u)
        writePositiveFlagAsSigned3(stream, rec.allowCaptureByTeam2);
    if (stateMask & 0x20u)
        writeSignedMagnitude(stream, rec.flagTeam, 3);
    if (stateMask & 0x40u)
        writeSignedMagnitude(stream, rec.capturingState, 3);

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeSimpleObject(
    const SimpleObjectRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* baseline,
    unsigned int stateMask,
    int precision,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const SimpleObjectNetworkable::UPDATE& rec = snap.rec;
    const Vec3 zeroVec = { 0.0f, 0.0f, 0.0f };

    stream.writeBits(&stateMask, 19);

    if (baseline != 0)
    {
        const Vec3* ref =
            reinterpret_cast<const Vec3*>(baseline->m_data + baseline->m_cursor * 2);

        if ((stateMask & 0x2u) != 0)
            stream.writeCompressedVector(rec.position, ref[0], 0.00050000002f);

        if (snap.isMobilePhysics)
        {
            if ((stateMask & 0x4u) != 0)
                stream.writeCompressedVector(rec.vec38, ref[1], 0.001f);
            if ((stateMask & 0x8u) != 0)
                stream.writeCompressedVector(rec.vec5C, ref[2], 0.001f);
        }

        baseline->m_cursor += 36;
    }
    else
    {
        if ((stateMask & 0x2u) != 0)
        {
            if (stream.hasCompressionVector())
            {
                stream.writeCompressedVector(
                    rec.position, stream.getCompressionVector(), 0.00050000002f);
            }
            else
            {
                const unsigned int rawTag = 0;
                stream.writeBits(&rawTag, 2);
                stream.writeBits(&rec.position.x, 32);
                stream.writeBits(&rec.position.y, 32);
                stream.writeBits(&rec.position.z, 32);
            }
        }

        if (snap.isMobilePhysics)
        {
            if ((stateMask & 0x4u) != 0)
                stream.writeCompressedVector(rec.vec38, zeroVec, 0.001f);

            if ((stateMask & 0x8u) != 0)
            {
                if (precision != io::StatePrecisionFull)
                    stream.writeCompressedVector2(rec.vec5C, zeroVec, 0.0099999998f);
                else
                    stream.writeCompressedVector(rec.vec5C, zeroVec, 0.001f);
            }
        }
    }

    if ((stateMask & 0x1u) != 0)
        stream.writeUnitQuaternion(rec.rotation, 14);

    if (snap.isMobilePhysics)
    {
        if ((stateMask & 0x4000u) != 0)
            stream.writeCompressedVector(rec.vec80, zeroVec, 0.001f);
        if ((stateMask & 0x4000u) != 0)
            stream.writeCompressedVector(rec.vec8C, zeroVec, 0.001f);
        if ((stateMask & 0x20u) != 0)
            stream.writeCompressedVector(rec.vec44, zeroVec, 0.000099999997f);
        if ((stateMask & 0x40u) != 0)
            stream.writeCompressedVector(rec.vec68, zeroVec, 0.000099999997f);
        if ((stateMask & 0x100u) != 0)
            stream.writeCompressedVector(rec.vec50, zeroVec, 0.000099999997f);
        if ((stateMask & 0x200u) != 0)
            stream.writeCompressedVector(rec.vec74, zeroVec, 0.000099999997f);
        if ((stateMask & 0x80u) != 0)
            writeBit(stream, rec.byte9C != 0);
        if ((stateMask & 0x8000u) != 0)
            writeBit(stream, rec.supplyB0 != 0);
    }

    if (snap.hasArmor && (stateMask & 0x10u) != 0)
    {
        const int value = rec.hitPoints + snap.armorMaxWreckHp;
        writeRangedIntOffset(stream, value, snap.armorLower, snap.armorUpper);
    }

    if ((stateMask & 0x400u) != 0)
    {
        const unsigned int repState = rec.replicationState;
        stream.writeBits(&repState, 2);
    }

    if (snap.hasPco)
    {
        if (snap.isParachute)
        {
            if ((stateMask & 0x2000u) != 0)
            {
                const int value = rec.parachuteAC;
                if (value <= 7)
                {
                    const unsigned int out = static_cast<unsigned int>(value);
                    stream.writeBits(&out, 4);
                }
                else
                {
                    stream.setError(4);
                }
            }
        }
        else
        {
            static const unsigned int kWeaponSlotMasks[3] = { 0x800u, 0x1000u, 0x20000u };
            for (int slot = 0; slot < 3; ++slot)
            {
                if ((stateMask & kWeaponSlotMasks[slot]) != 0)
                {
                    const int value = rec.weaponIndex[slot] + 1;
                    if (value <= 8)
                    {
                        const unsigned int out = static_cast<unsigned int>(value);
                        stream.writeBits(&out, 4);
                    }
                    else
                    {
                        stream.setError(4);
                    }
                }
            }

            if ((stateMask & 0x10000u) != 0)
            {
                const int value = rec.pcoB4;
                if (value <= 15)
                {
                    const unsigned int out = static_cast<unsigned int>(value);
                    stream.writeBits(&out, 5);
                }
                else
                {
                    stream.setError(4);
                }
            }

            if ((stateMask & 0x40000u) != 0)
            {
                writeBit(stream, rec.isDucking);
            }
        }
    }

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodePlayer(
    const PlayerRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int precision,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const NetUpdateSlot& slot = snap.slot;

    unsigned int mask = stateMask;
    stream.writeBits(&mask, 28);

    if (stateMask & 0x00000001u)
        writeBit(stream, slot.isAlive != 0);

    if (stateMask & 0x00000002u)
        writeRangedIntOffset(stream, playerSlotDword(slot, 36), 0, 255);

    if (stateMask & 0x00000008u)
    {
        writeSignedMagnitude(stream, playerSlotDword(slot, 12), 11);
        writeRangedIntOffset(stream, slot.someScore2, 0, 2047);
        writeRangedIntOffset(stream, playerSlotDword(slot, 0), 0, 2047);
        writeSignedMagnitude(stream, playerSlotDword(slot, 4), 11);
        writeRangedIntOffset(stream, playerSlotDword(slot, 8), 0, 2047);
    }

    if (stateMask & 0x00200000u)
        writeBit(stream, slot.buff[68] != 0);
    if (stateMask & 0x00000010u)
        writeBit(stream, slot.buff[69] != 0);

    if (io::StatePrecisionGhost == precision)
    {
        static const unsigned int kFiringBits[3] = { 0x00000040u, 0x00000080u, 0x00020000u };

        if (stateMask & 0x02000000u)
        {
            const unsigned int presses = static_cast<unsigned int>(playerSlotDword(slot, 72)) & 3u;
            stream.writeBits(&presses, 2);
        }

        for (int w = 0; w <= 2; ++w)
        {
            if ((kFiringBits[w] & stateMask) == 0 && (stateMask & 0x00010000u) == 0)
                continue;

            const bool firing = (slot.buff[24 * w + 76] != 0);
            writeBit(stream, firing);

            if (!firing && (stateMask & 0x00010000u) == 0)
                continue;

            writeRangedIntOffset(stream, playerSlotDword(slot, 24 * w + 80) + 1, 0, 63);
            {
                const unsigned int scratch = static_cast<unsigned int>(playerSlotDword(slot, 24 * w + 84));
                stream.writeBits(&scratch, 2);
            }
            writeBit(stream, slot.buff[24 * w + 88] != 0);

            const float heatVal = playerSlotFloat(slot, 24 * w + 92);
            const bool hot = snap.weaponUsesHeat[w] && (heatVal >= 1.0f);
            writeBit(stream, hot);
            if (hot)
            {
                double scaled = static_cast<double>(heatVal) / 10.0;
                if (scaled < 0.0)
                    scaled = 0.0;
                else if (scaled > 1.0)
                    scaled = 1.0;
                const unsigned int quantised = static_cast<unsigned int>(scaled * 255.0);
                stream.writeBits(&quantised, 8);
            }
            else
            {
                double scaled = static_cast<double>(heatVal);
                if (scaled < 0.0)
                    scaled = 0.0;
                else if (scaled > 1.0)
                    scaled = 1.0;
                const unsigned int quantised = static_cast<unsigned int>(scaled * 63.0);
                stream.writeBits(&quantised, 6);
            }

            {
                const unsigned int scratch = static_cast<unsigned int>(playerSlotDword(slot, 24 * w + 96));
                stream.writeBits(&scratch, 4);
            }
        }
    }

    if (stateMask & 0x00040000u)
    {
        writeBit(stream, slot.buff[148] != 0);
        writeBit(stream, slot.buff[149] != 0);
    }

    if (stateMask & 0x00000100u)
        writeRangedIntOffset(stream, static_cast<int>(playerSlotWord(slot, 158)), 0, 1023);

    if (stateMask & 0x00000020u)
    {
        const unsigned int spawnTick = static_cast<unsigned int>(playerSlotDword(slot, 152));
        stream.writeBits(&spawnTick, 32);
        writeBit(stream, slot.buff[156] != 0);
    }

    if (stateMask & 0x00000200u)
        writeRangedIntOffset(stream, playerSlotDword(slot, 16) + 1, 0, 3);

    if (stateMask & 0x00000400u)
    {
        writeRangedIntOffset(stream, playerSlotDword(slot, 32), 0, 8);
        writeBit(stream, slot.buff[162] != 0);
    }

    if (stateMask & 0x00000800u)
    {
        const unsigned int timeOutside = slot.buff[160];
        stream.writeBits(&timeOutside, 9);
    }
    if (stateMask & 0x00001000u)
    {
        const unsigned int kitByte = slot.buff[161];
        stream.writeBits(&kitByte, 9);
    }

    if (stateMask & 0x00002000u)
        writeRangedIntOffset(stream, playerSlotDword(slot, 40), 0, 15);
    if (stateMask & 0x00004000u)
    {
        writeRangedIntOffset(stream, playerSlotDword(slot, 44) + 1, 0, 31);
        writeRangedIntOffset(stream, playerSlotDword(slot, 20), 0, 255);
        writeRangedIntOffset(stream, playerSlotDword(slot, 24), 0, 255);
        writeRangedIntOffset(stream, playerSlotDword(slot, 28), 0, 255);
    }

    if (stateMask & 0x00008000u)
        writeBit(stream, slot.buff[163] != 0);
    if (stateMask & 0x00400000u)
        writeBit(stream, slot.buff[164] != 0);
    if (stateMask & 0x00800000u)
    {
        const unsigned int inviteMask = slot.buff[165];
        stream.writeBits(&inviteMask, 8);
    }
    if (stateMask & 0x01000000u)
        writeRangedIntOffset(stream, playerSlotDword(slot, 168) + 1, 0, 65536);

    writeBit(stream, slot.buff[48] != 0);

    if (stateMask & 0x04000000u)
    {
        const bool commander = (slot.buff[49] != 0);
        writeBit(stream, commander);
        if (commander)
        {
            writeBit(stream, slot.buff[50] != 0);
            writeBit(stream, slot.buff[51] != 0);
            writeBit(stream, slot.buff[52] != 0);
            writeBit(stream, slot.buff[53] != 0);
            writeBit(stream, slot.buff[54] != 0);

            for (int f = 0; f < 3; ++f)
            {
                double scaled = static_cast<double>(playerSlotFloat(slot, 56 + 4 * f));
                if (scaled < 0.0)
                    scaled = 0.0;
                else if (scaled > 1.0)
                    scaled = 1.0;
                const unsigned int quantised = static_cast<unsigned int>(scaled * 255.0);
                stream.writeBits(&quantised, 8);
            }
        }
    }

    writeBit(stream, snap.isConnected);

    if (stateMask & 0x08000000u)
        writeRangedIntOffset(stream, playerSlotDword(slot, 172), 0, 3);

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeScoreManager(
    const ScoreManagerRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    if (stateMask != 0)
    {
        int requiredBits = 32;
        for (unsigned int bit = 0; bit < 32; ++bit)
        {
            if (stateMask & (1u << bit))
                requiredBits += 16;
        }
        if (requiredBits > static_cast<int>(stream.getRemainingBitCapacity()))
        {
            if (outBitsWritten)
                *outBitsWritten = 0;
            return false;
        }
    }

    const unsigned int startBitPos = stream.getBitPosition();
    const unsigned char* const record = snap.record;

    stream.writeBits(&stateMask, 32);

    // Team one (exact ScoreManager::getNetUpdate wire order & offsets).
    if (stateMask & 0x00000001u) writeSigned17(stream, scoreRecordInt(record, 0x04));
    if (stateMask & 0x00000002u) writeSigned17(stream, scoreRecordInt(record, 0x08));
    if (stateMask & 0x01000000u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x0C));
        writeSigned17(stream, scoreRecordInt(record, 0x40));
    }
    if (stateMask & 0x04000000u) writeSigned17(stream, scoreRecordInt(record, 0x10));
    if (stateMask & 0x10000000u) writeSigned17(stream, scoreRecordInt(record, 0x14));
    if (stateMask & 0x00000004u) writeSigned17(stream, scoreRecordInt(record, 0x18));
    if (stateMask & 0x00000008u) writeSigned17(stream, scoreRecordInt(record, 0x1C));
    if (stateMask & 0x00000010u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x20));
        writeSigned17(stream, scoreRecordInt(record, 0x3C));
    }
    if (stateMask & 0x00000020u) writeSigned17(stream, scoreRecordInt(record, 0x24));
    if (stateMask & 0x00000040u) writeSigned17(stream, scoreRecordInt(record, 0x28));
    if (stateMask & 0x00000080u) writeSigned17(stream, scoreRecordInt(record, 0x2C));
    if (stateMask & 0x00000100u) writeSigned17(stream, scoreRecordInt(record, 0x30));
    if (stateMask & 0x00000200u) writeSigned17(stream, scoreRecordInt(record, 0x34));
    if (stateMask & 0x00000400u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x44));
        writeBit(stream, record[0x50] != 0);
        writeSigned17(stream, scoreRecordInt(record, 0x4C));
    }
    if (stateMask & 0x00000800u) writeSigned17(stream, scoreRecordInt(record, 0x48));

    // Team two (exact ScoreManager::getNetUpdate wire order & offsets).
    if (stateMask & 0x00001000u) writeSigned17(stream, scoreRecordInt(record, 0x54));
    if (stateMask & 0x00002000u) writeSigned17(stream, scoreRecordInt(record, 0x58));
    if (stateMask & 0x02000000u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x5C));
        writeSigned17(stream, scoreRecordInt(record, 0x90));
    }
    if (stateMask & 0x08000000u) writeSigned17(stream, scoreRecordInt(record, 0x60));
    if (stateMask & 0x20000000u) writeSigned17(stream, scoreRecordInt(record, 0x64));
    if (stateMask & 0x00004000u) writeSigned17(stream, scoreRecordInt(record, 0x68));
    if (stateMask & 0x00008000u) writeSigned17(stream, scoreRecordInt(record, 0x6C));
    if (stateMask & 0x00010000u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x70));
        writeSigned17(stream, scoreRecordInt(record, 0x8C));
    }
    if (stateMask & 0x00020000u) writeSigned17(stream, scoreRecordInt(record, 0x74));
    if (stateMask & 0x00040000u) writeSigned17(stream, scoreRecordInt(record, 0x78));
    if (stateMask & 0x00080000u) writeSigned17(stream, scoreRecordInt(record, 0x7C));
    if (stateMask & 0x00100000u) writeSigned17(stream, scoreRecordInt(record, 0x80));
    if (stateMask & 0x00200000u) writeSigned17(stream, scoreRecordInt(record, 0x84));
    if (stateMask & 0x00400000u)
    {
        writeSigned17(stream, scoreRecordInt(record, 0x94));
        writeBit(stream, record[0xA0] != 0);
        writeSigned17(stream, scoreRecordInt(record, 0x9C));
    }
    if (stateMask & 0x00800000u) writeSigned17(stream, scoreRecordInt(record, 0x98));

    if (stateMask & 0x40000000u)
    {
        const unsigned int sign = 0;
        const unsigned int winner = record[0x00];
        stream.writeBits(&sign, 1);
        stream.writeBits(&winner, 2);
    }
    if (stateMask & 0x80000000u)
    {
        const unsigned int sign = 0;
        const unsigned int victoryType = record[0x01];
        stream.writeBits(&sign, 1);
        stream.writeBits(&victoryType, 2);
        writeSigned17(stream, scoreRecordInt(record, 0x38));
        writeSigned17(stream, scoreRecordInt(record, 0x88));
    }

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeSpawnGroup(
    const SpawnGroupRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();

    stream.writeBits(&stateMask, 6);

    if (stateMask & 0x10u)
        writeBit(stream, snap.hasSpawnPoints);

    if (stateMask & 0x02u)
    {
        const unsigned int teamVal = static_cast<unsigned int>(snap.team);
        stream.writeBits(&teamVal, 4);
    }

    if (stateMask & 0x04u)
        writeBit(stream, snap.active);

    if (stateMask & 0x08u)
        writeBit(stream, snap.onlyForAI);

    if (stateMask & 0x01u)
    {
        const unsigned int posX = static_cast<unsigned int>(snap.posX);
        const unsigned int posZ = static_cast<unsigned int>(snap.posZ);
        stream.writeBits(&posX, 8);
        stream.writeBits(&posZ, 8);
    }

    if (stateMask & 0x20u)
    {
        const unsigned int wireSquad = static_cast<unsigned int>(snap.squad + 1);
        stream.writeBits(&wireSquad, 8);
    }

    if (stream.hasError())
    {
        stream.clearError();
        if (outBitsWritten)
            *outBitsWritten = 0;
        return false;
    }
    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);
    return true;
}

bool ConstNetEncoders::encodeRecord(
    const WorldNetSnapshot& worldSnap,
    const NetRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* baseline,
    unsigned int stateMask,
    int precision,
    int* outBitsWritten)
{
    switch (snap.type)
    {
    case NetEncType::Soldier:
        if (snap.typeRecordIndex >= worldSnap.m_soldierRecords.size())
            return false;
        return encodeSoldier(
            worldSnap.m_soldierRecords[snap.typeRecordIndex],
            worldSnap.m_ragdollParticles.empty() ? 0 : &worldSnap.m_ragdollParticles[0],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::Item:
        if (snap.typeRecordIndex >= worldSnap.m_itemRecords.size())
            return false;
        return encodeItem(
            worldSnap.m_itemRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::Kit:
        return encodeKit(stream, baseline, stateMask, precision, outBitsWritten);

    case NetEncType::GenericProjectile:
        if (snap.typeRecordIndex >= worldSnap.m_projectileRecords.size())
            return false;
        return encodeGenericProjectile(
            worldSnap.m_projectileRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::ControlPoint:
        if (snap.typeRecordIndex >= worldSnap.m_controlPointRecords.size())
            return false;
        return encodeControlPoint(
            worldSnap.m_controlPointRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::SimpleObject:
        if (snap.typeRecordIndex >= worldSnap.m_simpleObjectRecords.size())
            return false;
        return encodeSimpleObject(
            worldSnap.m_simpleObjectRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::Player:
        if (snap.typeRecordIndex >= worldSnap.m_playerRecords.size())
            return false;
        return encodePlayer(
            worldSnap.m_playerRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::ScoreManager:
        if (snap.typeRecordIndex >= worldSnap.m_scoreManagerRecords.size())
            return false;
        return encodeScoreManager(
            worldSnap.m_scoreManagerRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::SpawnGroup:
        if (snap.typeRecordIndex >= worldSnap.m_spawnGroupRecords.size())
            return false;
        return encodeSpawnGroup(
            worldSnap.m_spawnGroupRecords[snap.typeRecordIndex],
            stream,
            baseline,
            stateMask,
            precision,
            outBitsWritten);

    case NetEncType::Unsupported:
    default:
        return false;
    }
}

bool ConstNetEncoders::encodeDescriptor(
    const WorldNetSnapshot& worldSnap,
    const io::NetworkableDescriptor* descriptor,
    io::BitStream& stream,
    const io::NetworkableStateMask* mask,
    int precision,
    bool ghostTier,
    WorldNetSnapshot::TypeWriteBitStats* outStats,
    unsigned int* outDidNotFitCount,
    unsigned int* outMissingUpdatesCount,
    std::vector<unsigned int>* outEncodedRecords)
{
    if (!descriptor)
        return false;

    unsigned int limit;
    unsigned int cursor;
    if (stream.m_readWriteSwitch == 1)
    {
        limit  = stream.m_highWaterBits;
        cursor = stream.m_readBitPos;
    }
    else
    {
        limit = stream.m_bitLimit;
        if (!limit)
            limit = 8u * stream.getSizeBytes();
        cursor = stream.m_bitPos;
    }
    if (static_cast<unsigned int>(descriptor->minimumBitSize) > limit - cursor)
    {
        if (outDidNotFitCount)
            ++(*outDidNotFitCount);
        return false;
    }

    typedef std::vector<io::UniqueNetProperty>::const_iterator PropIter;
    const PropIter end = descriptor->properties.end();

    if (mask)
    {
        const int targetPrecision = ghostTier ? precision : static_cast<int>(io::StatePrecisionFull);
        PropIter it = descriptor->properties.begin();
        int slot = 0;
        if (it != end)
        {
            for (;;)
            {
                const io::UniqueNetProperty& property = *it;
                if (property.networkables.empty())
                    return false;
                const io::INetworkable* const networkable = property.networkables[0];
                if (networkable)
                {
                    const NetRecordSnap* recSnap = worldSnap.findRecord(networkable);
                    if (!recSnap || recSnap->type == NetEncType::Unsupported)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outEncodedRecords && !worldSnap.m_records.empty())
                        outEncodedRecords->push_back(static_cast<unsigned int>(recSnap - &worldSnap.m_records[0]));
                    const unsigned int propMask = ghostTier
                        ? (mask->m_opaque[slot] & recSnap->ghostStateMask)
                        : mask->m_opaque[slot];
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, 0, propMask, targetPrecision, &bitsWritten);
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                }
                ++it;
                ++slot;
                if (it == end || slot > 11)
                    return true;
            }
        }
    }
    else
    {
        const int targetPrecision = ghostTier ? precision : static_cast<int>(io::StatePrecisionFull);
        PropIter it = descriptor->properties.begin();
        if (it != end)
        {
            for (;;)
            {
                const io::UniqueNetProperty& property = *it;
                if (property.networkables.empty())
                    return false;
                const io::INetworkable* const networkable = property.networkables[0];
                if (networkable)
                {
                    const NetRecordSnap* recSnap = worldSnap.findRecord(networkable);
                    if (!recSnap || recSnap->type == NetEncType::Unsupported)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outEncodedRecords && !worldSnap.m_records.empty())
                        outEncodedRecords->push_back(static_cast<unsigned int>(recSnap - &worldSnap.m_records[0]));
                    const unsigned int propMask = ghostTier
                        ? recSnap->ghostStateMask
                        : 0xFFFFFFFFu;
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, 0, propMask, targetPrecision, &bitsWritten);
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                }
                ++it;
                if (it == end)
                    return true;
            }
        }
    }

    return true;
}

bool ConstNetEncoders::encodeDescriptorDelta(
    const WorldNetSnapshot& worldSnap,
    const io::NetworkableDescriptor* descriptor,
    io::BitStream& stream,
    io::BaseLineData& baseLine,
    const io::NetworkableStateMask* mask,
    int precision,
    bool ghostTier,
    bool resetBaseLineCursor,
    WorldNetSnapshot::TypeWriteBitStats* outStats,
    unsigned int* outMissingUpdatesCount,
    std::vector<unsigned int>* outEncodedRecords)
{
    if (!descriptor)
        return false;

    unsigned int remaining;
    if (stream.m_readWriteSwitch == 1)
        remaining = stream.m_highWaterBits - stream.m_readBitPos;
    else if (stream.m_bitLimit)
        remaining = stream.m_bitLimit - stream.m_bitPos;
    else
        remaining = 8u * stream.getSizeBytes() - stream.m_bitPos;

    if (static_cast<unsigned int>(descriptor->minimumBitSize) > remaining)
        return false;

    if (resetBaseLineCursor)
        baseLine.m_cursor = 0;

    typedef std::vector<io::UniqueNetProperty>::const_iterator PropIter;
    const PropIter end = descriptor->properties.end();
    const int targetPrecision = ghostTier ? precision : static_cast<int>(io::StatePrecisionFull);

    if (mask)
    {
        PropIter it = descriptor->properties.begin();
        int slot = 0;
        if (it != end)
        {
            for (;;)
            {
                const io::UniqueNetProperty& property = *it;
                if (property.networkables.empty())
                    return false;
                const io::INetworkable* const networkable = property.networkables[0];
                if (networkable)
                {
                    const NetRecordSnap* recSnap = worldSnap.findRecord(networkable);
                    if (!recSnap || recSnap->type == NetEncType::Unsupported)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outEncodedRecords && !worldSnap.m_records.empty())
                        outEncodedRecords->push_back(static_cast<unsigned int>(recSnap - &worldSnap.m_records[0]));
                    const unsigned int propMask = ghostTier
                        ? (mask->m_opaque[slot] & recSnap->ghostStateMask)
                        : mask->m_opaque[slot];
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, &baseLine, propMask, targetPrecision, &bitsWritten);
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                }
                ++it;
                ++slot;
                if (it == end)
                    return true;
            }
        }
    }
    else
    {
        PropIter it = descriptor->properties.begin();
        if (it != end)
        {
            for (;;)
            {
                const io::UniqueNetProperty& property = *it;
                if (property.networkables.empty())
                    return false;
                const io::INetworkable* const networkable = property.networkables[0];
                if (networkable)
                {
                    const NetRecordSnap* recSnap = worldSnap.findRecord(networkable);
                    if (!recSnap || recSnap->type == NetEncType::Unsupported)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outEncodedRecords && !worldSnap.m_records.empty())
                        outEncodedRecords->push_back(static_cast<unsigned int>(recSnap - &worldSnap.m_records[0]));
                    const unsigned int propMask = ghostTier
                        ? recSnap->ghostStateMask
                        : 0xFFFFFFFFu;
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, &baseLine, propMask, targetPrecision, &bitsWritten);
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                }
                ++it;
                if (it == end)
                    return true;
            }
        }
    }

    return true;
}

bool ConstNetEncoders::compareBitStreamsBitExact(
    const io::BitStream& a,
    const io::BitStream& b)
{
    if (a.getBitPosition() != b.getBitPosition())
        return false;
    if (a.m_highWaterBits != b.m_highWaterBits)
        return false;

    const unsigned int totalBits = a.m_highWaterBits;
    const unsigned int fullBytes = totalBits >> 3;
    const unsigned int remBits   = totalBits & 7u;

    const unsigned char* bufA = a.getBuffer();
    const unsigned char* bufB = b.getBuffer();
    if (!bufA || !bufB)
        return (totalBits == 0);

    if (fullBytes > 0 && std::memcmp(bufA, bufB, fullBytes) != 0)
        return false;

    if (remBits > 0)
    {
        const unsigned char mask = static_cast<unsigned char>((1u << remBits) - 1u);
        if ((bufA[fullBytes] & mask) != (bufB[fullBytes] & mask))
            return false;
    }

    return true;
}

bool ConstNetEncoders::verifyBitExactAgainstLegacy(
    const WorldNetSnapshot& worldSnap,
    const NetRecordSnap& snap,
    io::INetworkable* liveNet,
    const Vec3* optionalCompressionVec,
    io::BaseLineData* optionalBaseline,
    unsigned int stateMask,
    int precision)
{
    if (!liveNet)
        return false;

    unsigned char bufA[2048];
    unsigned char bufB[2048];
    std::memset(bufA, 0, sizeof(bufA));
    std::memset(bufB, 0, sizeof(bufB));

    io::BitStream streamA(bufA, sizeof(bufA));
    io::BitStream streamB(bufB, sizeof(bufB));

    if (optionalCompressionVec)
    {
        streamA.setCompressionVector(*optionalCompressionVec);
        streamB.setCompressionVector(*optionalCompressionVec);
    }

    io::BaseLineData baseA;
    io::BaseLineData baseB;
    io::BaseLineData* ptrBaseA = 0;
    io::BaseLineData* ptrBaseB = 0;
    if (optionalBaseline)
    {
        baseA = *optionalBaseline;
        baseB = *optionalBaseline;
        ptrBaseA = &baseA;
        ptrBaseB = &baseB;
    }

    const bool okA = encodeRecord(worldSnap, snap, streamA, ptrBaseA, stateMask, precision);
    const bool okB = liveNet->getNetUpdate(streamB, ptrBaseB, stateMask, precision);

    if (okA != okB)
        return false;

    if (optionalBaseline)
    {
        if (baseA.m_cursor != baseB.m_cursor)
            return false;
        optionalBaseline->m_cursor = baseA.m_cursor;
    }

    return compareBitStreamsBitExact(streamA, streamB);
}

} // namespace netpipeline
} // namespace hfe
} // namespace dice
