// [CHOSEN, not recovered] Pure-const, thread-safe bit-exact network encoders for Stage B
// worker threads. Reads exclusively from WorldNetSnapshot and writes only to the caller's
// per-connection io::BitStream (and per-ghost io::BaseLineData cursor).
// Zero mutations to live INetworkable state, zero shared stats writes, zero Debug() calls.

#ifndef GAME_GAMESERVER_NETPIPELINE_CONSTNETENCODERS_H
#define GAME_GAMESERVER_NETPIPELINE_CONSTNETENCODERS_H

#include "WorldNetSnapshot.h"
#include "../../../IO/BitStream.h"
#include "../../../IO/Network/INetworkable.h"
#include "../../../IO/Network/NetworkManager.h"

namespace dice {
namespace hfe {
namespace netpipeline {

class ConstNetEncoders
{
public:
    // Per-type pure-const encoders. Each mirrors its live *Networkable::getNetUpdate bit-for-bit.
    static bool encodeSoldier(
        const SoldierRecordSnap& snap,
        const RagDollParticleSnap* ragdollParticles,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeItem(
        const ItemRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeKit(
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeGenericProjectile(
        const GenericProjectileRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeControlPoint(
        const ControlPointRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeSimpleObject(
        const SimpleObjectRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodePlayer(
        const PlayerRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeScoreManager(
        const ScoreManagerRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    static bool encodeSpawnGroup(
        const SpawnGroupRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    // Polymorphic dispatch by NetRecordSnap::type against WorldNetSnapshot tables.
    static bool encodeRecord(
        const WorldNetSnapshot& worldSnap,
        const NetRecordSnap& snap,
        io::BitStream& stream,
        io::BaseLineData* baseline,
        unsigned int stateMask,
        int precision,
        int* outBitsWritten = 0);

    // Thread-safe equivalent of io::NetworkManager::getNetUpdate().
    static bool encodeDescriptor(
        const WorldNetSnapshot& worldSnap,
        const io::NetworkableDescriptor* descriptor,
        io::BitStream& stream,
        const io::NetworkableStateMask* mask,
        int precision,
        bool ghostTier,
        WorldNetSnapshot::TypeWriteBitStats* outStats = 0,
        unsigned int* outDidNotFitCount = 0,
        unsigned int* outMissingUpdatesCount = 0);

    // Thread-safe equivalent of io::NetworkManager::getNetUpdateDelta().
    static bool encodeDescriptorDelta(
        const WorldNetSnapshot& worldSnap,
        const io::NetworkableDescriptor* descriptor,
        io::BitStream& stream,
        io::BaseLineData& baseLine,
        const io::NetworkableStateMask* mask,
        int precision,
        bool ghostTier,
        bool resetBaseLineCursor,
        WorldNetSnapshot::TypeWriteBitStats* outStats = 0,
        unsigned int* outMissingUpdatesCount = 0);

    // Compares two BitStreams for exact bit position, high-water mark, and bit-for-bit buffer payload.
    static bool compareBitStreamsBitExact(
        const io::BitStream& a,
        const io::BitStream& b);

    // Main-thread verification helper (for net-mt-verify and golden tests):
    // Encodes snap via ConstNetEncoders into stream A and liveNet->getNetUpdate() into stream B,
    // and returns true iff return status, cursor, high-water bits, baseline cursor, and all written bits match.
    static bool verifyBitExactAgainstLegacy(
        const WorldNetSnapshot& worldSnap,
        const NetRecordSnap& snap,
        io::INetworkable* liveNet,
        const Vec3* optionalCompressionVec,
        io::BaseLineData* optionalBaseline,
        unsigned int stateMask,
        int precision);
};

} // namespace netpipeline
} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_NETPIPELINE_CONSTNETENCODERS_H
