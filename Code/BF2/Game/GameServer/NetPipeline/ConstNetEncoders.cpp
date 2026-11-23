// [CHOSEN, not recovered] Pure-const, thread-safe bit-exact network encoders for Stage B
// worker threads. Every encoder matches its live *Networkable::getNetUpdate bit-for-bit.

#include "ConstNetEncoders.h"

#include <cstring>

namespace dice {
namespace hfe {
namespace netpipeline {

namespace {

inline void writeSignedMagnitude(io::BitStream& stream, int value, unsigned int magBits)
{
    const unsigned int sign = (value < 0) ? 1u : 0u;
    const unsigned int mag  = (value < 0)
        ? static_cast<unsigned int>(-value)
        : static_cast<unsigned int>(value);
    stream.writeBits(&sign, 1);
    stream.writeBits(&mag, magBits);
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
    double norm = (static_cast<double>(v) / 1.25 + 1.0) * 0.5;
    if (norm > 1.0)
        norm = 1.0;
    else if (norm < 0.0)
        norm = 0.0;
    const unsigned int q = static_cast<unsigned int>(norm * 65535.0);
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
            const unsigned char maskedAmmo = static_cast<unsigned char>(rec.ammoStatus & 0x0Fu);
            stream.writeBits(&maskedAmmo, 5);
        }
    }

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
        stream.writeBits(&rec.enabled, 1);

    if (stateMask & 0x10u)
        writeSignedMagnitude(stream, rec.team, 4);

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
        writeSignedMagnitude(stream, rec.m_team, 3);
    if (stateMask & 0x02u)
        stream.writeBits(&rec.m_bool158, 1);
    if (stateMask & 0x04u)
    {
        writeQuantizedTakeOver(stream, rec.m_takeOverCounter);
        writeQuantizedTakeOver(stream, rec.m_takeOverChangePerSecond);
    }
    if (stateMask & 0x08u)
        writePositiveFlagAsSigned3(stream, rec.m_allowCaptureByTeam1);
    if (stateMask & 0x10u)
        writePositiveFlagAsSigned3(stream, rec.m_allowCaptureByTeam2);
    if (stateMask & 0x20u)
        writeSignedMagnitude(stream, rec.m_flagTeam, 3);
    if (stateMask & 0x40u)
        writeSignedMagnitude(stream, rec.m_capturingState, 3);

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
            stream.writeBits(&rec.byte9C, 1);
        if ((stateMask & 0x8000u) != 0)
            stream.writeBits(&rec.supplyB0, 1);
    }

    if (snap.hasArmor && (stateMask & 0x10u) != 0)
    {
        const int value = rec.hitPoints + snap.armorMaxWreckHp;
        writeRangedIntOffset(stream, value, snap.armorLower, snap.armorUpper);
    }

    if ((stateMask & 0x400u) != 0)
        stream.writeBits(&rec.replicationState, 2);

    if (snap.hasPco)
    {
        if (snap.isParachute)
        {
            if ((stateMask & 0x2000u) != 0)
            {
                const unsigned int value = static_cast<unsigned int>(rec.parachuteAC);
                if (value <= 7u)
                    stream.writeBits(&value, 4);
                else
                    stream.setError(4);
            }
        }
        else
        {
            static const unsigned int kWeaponSlotMasks[3] = { 0x800u, 0x1000u, 0x20000u };
            for (int slot = 0; slot < 3; ++slot)
            {
                if ((stateMask & kWeaponSlotMasks[slot]) != 0)
                {
                    const unsigned int value = static_cast<unsigned int>(rec.weaponIndex[slot] + 1);
                    if (value <= 8u)
                        stream.writeBits(&value, 4);
                    else
                        stream.setError(4);
                }
            }

            if ((stateMask & 0x10000u) != 0)
            {
                const unsigned int value = static_cast<unsigned int>(rec.pcoB4);
                if (value <= 15u)
                    stream.writeBits(&value, 4);
                else
                    stream.setError(4);
            }

            if ((stateMask & 0x40000u) != 0)
            {
                const unsigned int duckBit = rec.isDucking ? 1u : 0u;
                stream.writeBits(&duckBit, 1);
            }
        }
    }

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
    return true;
}

bool ConstNetEncoders::encodePlayer(
    const PlayerRecordSnap& snap,
    io::BitStream& stream,
    io::BaseLineData* /*baseline*/,
    unsigned int stateMask,
    int /*precision*/,
    int* outBitsWritten)
{
    const unsigned int startBitPos = stream.getBitPosition();
    const NetUpdateSlot& slot = snap.slot;

    stream.writeBits(&stateMask, 28);

    if (stateMask & 0x04u)
    {
        const unsigned int spawned = slot.m_spawned ? 1u : 0u;
        stream.writeBits(&spawned, 1);
        const unsigned int down = slot.m_down ? 1u : 0u;
        stream.writeBits(&down, 1);
    }

    if (stateMask & 0x08u)
        writeRangedUIntZeroBased(stream, slot.m_spawnGroupId + 1, 100u, 7);

    if (stateMask & 0x10u)
        writeRangedUIntZeroBased(stream, slot.m_team, 15u, 4);

    if (stateMask & 0x20u)
        stream.writeBits(&slot.m_score, 16);

    if (stateMask & 0x800u)
        stream.writeBits(&slot.m_skillScore, 16);

    if (stateMask & 0x40u)
        stream.writeBits(&slot.m_teamWorkScore, 16);

    if (stateMask & 0x100u)
        writeRangedUIntZeroBased(stream, slot.m_rank, 31u, 5);

    if (stateMask & 0x200u)
        stream.writeBits(&slot.m_kills, 11);

    if (stateMask & 0x400u)
        stream.writeBits(&slot.m_deaths, 11);

    if ((stateMask & 0x80u) && snap.isConnected)
        writeRangedUIntZeroBased(stream, slot.m_ping, 1023u, 10);

    if (stateMask & 0x01u)
    {
        const unsigned int valid = slot.m_vehicleValid ? 1u : 0u;
        stream.writeBits(&valid, 1);
        if (slot.m_vehicleValid)
        {
            stream.writeBits(&slot.m_vehicleNetId, 16);
            writeRangedUIntZeroBased(stream, slot.m_subPcoId + 1, 15u, 4);
            stream.writeBits(&slot.m_cameraNetId, 16);
        }
    }

    if (stateMask & 0x1000000u)
    {
        const unsigned int valid = slot.m_defaultVehicleValid ? 1u : 0u;
        stream.writeBits(&valid, 1);
        if (slot.m_defaultVehicleValid)
            stream.writeBits(&slot.m_defaultVehicleNetId, 16);
    }

    if (stateMask & 0x02u)
    {
        const unsigned int valid = slot.m_kitValid ? 1u : 0u;
        stream.writeBits(&valid, 1);
        if (slot.m_kitValid)
            stream.writeBits(&slot.m_kitTemplateId, 16);
    }

    if (stateMask & 0x1000u)
    {
        const unsigned int inside = slot.m_isInsideCP ? 1u : 0u;
        stream.writeBits(&inside, 1);
    }

    if (stateMask & 0x2000u)
    {
        double prog = static_cast<double>(slot.m_cpTakeOverProgress) * 0.005 * 255.0;
        if (prog > 255.0) prog = 255.0;
        else if (prog < 0.0) prog = 0.0;
        const unsigned int packedProg = static_cast<unsigned int>(prog);
        stream.writeBits(&packedProg, 8);

        double spd = (static_cast<double>(slot.m_cpTakeOverSpeed) / 1.25 + 1.0) * 0.5;
        if (spd > 1.0) spd = 1.0;
        else if (spd < 0.0) spd = 0.0;
        const unsigned int packedSpd = static_cast<unsigned int>(spd * 255.0);
        stream.writeBits(&packedSpd, 8);
    }

    if (stateMask & 0x10000000u)
    {
        const unsigned int fh = slot.m_flagHolder ? 1u : 0u;
        stream.writeBits(&fh, 1);
    }

    if (stateMask & 0x4000u)
        writeRangedUIntZeroBased(stream, slot.m_squadId, 9u, 4);

    if (stateMask & 0x8000u)
    {
        const unsigned int sl = slot.m_isSquadLeader ? 1u : 0u;
        stream.writeBits(&sl, 1);
    }

    if (stateMask & 0x4000000u)
    {
        const unsigned int cmd = slot.m_Commander ? 1u : 0u;
        stream.writeBits(&cmd, 1);
    }

    if (stateMask & 0x8000000u)
    {
        const unsigned int app = slot.m_commanderApplicantsValid ? 1u : 0u;
        stream.writeBits(&app, 1);
    }

    if (stateMask & 0x10000u)
    {
        const unsigned int rep = slot.m_ReplenishReq ? 1u : 0u;
        stream.writeBits(&rep, 1);
        const unsigned int rpr = slot.m_RepairPoint ? 1u : 0u;
        stream.writeBits(&rpr, 1);
        const unsigned int hl = slot.m_HealPoint ? 1u : 0u;
        stream.writeBits(&hl, 1);
    }

    static const unsigned int kWeaponMasks[3] = { 0x20000u, 0x40000u, 0x80000u };
    for (int w = 0; w < 3; ++w)
    {
        if (stateMask & kWeaponMasks[w])
        {
            if (snap.weaponUsesHeat[w])
            {
                double q = static_cast<double>(slot.m_heat[w]);
                if (q > 1.0) q = 1.0;
                else if (q < 0.0) q = 0.0;
                const unsigned int packed = static_cast<unsigned int>(q * 127.0);
                stream.writeBits(&packed, 7);
            }
            else
            {
                writeRangedUIntZeroBased(stream, slot.m_ammo[w] + 1, 1023u, 10);
                writeRangedUIntZeroBased(stream, slot.m_nrOfMags[w] + 1, 31u, 5);
            }
        }
    }

    if (stateMask & 0x100000u)
        writeRangedUIntZeroBased(stream, slot.m_fireRateMod[0], 15u, 4);

    if (stateMask & 0x200000u)
        writeRangedUIntZeroBased(stream, slot.m_fireRateMod[1], 15u, 4);

    if (stateMask & 0x400000u)
        writeRangedUIntZeroBased(stream, slot.m_fireRateMod[2], 15u, 4);

    if (stateMask & 0x800000u)
    {
        writeRangedUIntZeroBased(stream, slot.m_LockonState, 7u, 3);
        if (slot.m_LockonState > 1)
        {
            stream.writeBits(&slot.m_targetNetId, 16);
            stream.writeBits(&slot.m_subControllerIndex, 3);
        }
    }

    if (stateMask & 0x2000000u)
    {
        const unsigned int voip = slot.m_VoipOn ? 1u : 0u;
        stream.writeBits(&voip, 1);
        const unsigned int slVoip = slot.m_SquadLeaderVoipOn ? 1u : 0u;
        stream.writeBits(&slVoip, 1);
    }

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
    static const unsigned int kTeam1Masks[15] = {
        0x00000001u, 0x00000002u, 0x00000004u, 0x00000008u, 0x00000010u,
        0x00000020u, 0x00000040u, 0x00000080u, 0x00000100u, 0x00000200u,
        0x00000400u, 0x00000800u, 0x00001000u, 0x00002000u, 0x00004000u
    };
    static const unsigned int kTeam2Masks[15] = {
        0x00008000u, 0x00010000u, 0x00020000u, 0x00040000u, 0x00080000u,
        0x00100000u, 0x00200000u, 0x00400000u, 0x00800000u, 0x01000000u,
        0x02000000u, 0x04000000u, 0x08000000u, 0x10000000u, 0x20000000u
    };
    static const int kFieldByteOffsets[15] = {
        4, 8, 12, 16, 20, 24, 28, 32, 40, 44, 52, 56, 60, 72, 76
    };
    static const unsigned int kFieldMagBits[15] = {
        16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16, 16, 7, 16
    };

    if (stateMask != 0)
    {
        int setBits = 0;
        for (unsigned int m = stateMask; m != 0; m &= (m - 1))
            ++setBits;
        const int requiredBits = 32 + 16 * setBits;
        if (requiredBits > static_cast<int>(stream.getRemainingBitCapacity()))
        {
            if (outBitsWritten)
                *outBitsWritten = 0;
            return false;
        }
    }

    const unsigned int startBitPos = stream.getBitPosition();
    const unsigned char* rec = snap.record;

    stream.writeBits(&stateMask, 32);

    for (int i = 0; i < 15; ++i)
    {
        if (stateMask & kTeam1Masks[i])
        {
            const int value = *reinterpret_cast<const int*>(rec + kFieldByteOffsets[i]);
            writeSignedMagnitude(stream, value, kFieldMagBits[i]);
        }
    }

    for (int i = 0; i < 15; ++i)
    {
        if (stateMask & kTeam2Masks[i])
        {
            const int value = *reinterpret_cast<const int*>(rec + 80 + kFieldByteOffsets[i]);
            writeSignedMagnitude(stream, value, kFieldMagBits[i]);
        }
    }

    if (stateMask & 0x40000000u)
    {
        const int winner = *reinterpret_cast<const int*>(rec + 160);
        writeSignedMagnitude(stream, winner, 2);
    }

    if (stateMask & 0x80000000u)
    {
        const int victoryType = *reinterpret_cast<const int*>(rec + 156);
        writeSignedMagnitude(stream, victoryType, 4);
    }

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
        stream.writeBits(&snap.hasSpawnPoints, 1);

    if (stateMask & 0x02u)
    {
        const unsigned char teamByte = static_cast<unsigned char>(snap.team);
        stream.writeBits(&teamByte, 4);
    }

    if (stateMask & 0x04u)
        stream.writeBits(&snap.active, 1);

    if (stateMask & 0x08u)
        stream.writeBits(&snap.onlyForAI, 1);

    if (stateMask & 0x01u)
    {
        stream.writeBits(&snap.posX, 8);
        stream.writeBits(&snap.posZ, 8);
    }

    if (stateMask & 0x20u)
    {
        const unsigned int wireSquad = static_cast<unsigned int>(snap.squad + 1);
        stream.writeBits(&wireSquad, 8);
    }

    if (outBitsWritten)
        *outBitsWritten = static_cast<int>(stream.getBitPosition() - startBitPos);

    if (stream.hasError())
    {
        stream.clearError();
        return false;
    }
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
    unsigned int* outMissingUpdatesCount)
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
                    const unsigned int propMask = ghostTier
                        ? (mask->m_opaque[slot] & recSnap->ghostStateMask)
                        : mask->m_opaque[slot];
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, 0, propMask, targetPrecision, &bitsWritten);
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
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
                    const unsigned int propMask = ghostTier
                        ? recSnap->ghostStateMask
                        : 0xFFFFFFFFu;
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, 0, propMask, targetPrecision, &bitsWritten);
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
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
    unsigned int* outMissingUpdatesCount)
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
                    const unsigned int propMask = ghostTier
                        ? (mask->m_opaque[slot] & recSnap->ghostStateMask)
                        : mask->m_opaque[slot];
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, &baseLine, propMask, targetPrecision, &bitsWritten);
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
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
                    const unsigned int propMask = ghostTier
                        ? recSnap->ghostStateMask
                        : 0xFFFFFFFFu;
                    int bitsWritten = 0;
                    const bool walkOk = encodeRecord(
                        worldSnap, *recSnap, stream, &baseLine, propMask, targetPrecision, &bitsWritten);
                    if (outStats)
                        outStats->bitsByType[static_cast<unsigned int>(recSnap->type)] += bitsWritten;
                    if (!walkOk)
                    {
                        if (outMissingUpdatesCount)
                            ++(*outMissingUpdatesCount);
                        return false;
                    }
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

    if (optionalBaseline && baseA.m_cursor != baseB.m_cursor)
        return false;

    return compareBitStreamsBitExact(streamA, streamB);
}

} // namespace netpipeline
} // namespace hfe
} // namespace dice
