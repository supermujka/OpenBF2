// [CHOSEN, not recovered] Immutable per-tick world & networkable state snapshot for lock-free
// multi-threaded 32v32 ghost scoping and bit-exact packet encoding (Stage A -> Stage B -> Stage C).
// Built once per tick on the main thread; read-only across Stage B workers.

#ifndef GAME_GAMESERVER_NETPIPELINE_WORLDNETSNAPSHOT_H
#define GAME_GAMESERVER_NETPIPELINE_WORLDNETSNAPSHOT_H

#include "../../../Math/Vec3.h"
#include "../../../Math/Quat.h"
#include "../../../Math/Mat4.h"
#include "../../../IO/Network/NetworkManager.h"
#include "../../Objects/Networkables/Networkables.h"
#include "../../Objects/Networkables/ControlPointNetworkable.h"
#include "../../GameLogic/Player.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace dice {
namespace hfe {

namespace world { class ScoreManager; }
class SpawnGroup;

namespace netpipeline {

enum class NetEncType : unsigned char
{
    Unsupported       = 0,
    Soldier           = 1,
    Item              = 2,
    Kit               = 3,
    GenericProjectile = 4,
    ControlPoint      = 5,
    SimpleObject      = 6,
    Player            = 7,
    ScoreManager      = 8,
    SpawnGroup        = 9
};

struct RagDollParticleSnap
{
    bool netControlled;
    Vec3 position;
};

struct SoldierRecordSnap
{
    SoldierNetworkable::UPDATE rec;
    bool                       isAwakeOccupiedRagdoll;
    bool                       hasArmor;
    int                        inventorySize;
    unsigned int               ragdollParticleStart;
    unsigned int               ragdollParticleCount;
};

struct ItemRecordSnap
{
    ItemNetworkable::Record rec;
};

struct GenericProjectileRecordSnap
{
    GenericProjectileNetworkable::Record rec;
    bool                                 hasArmor;
    int                                  armorMaxWreckHp;
    int                                  armorLower;
    int                                  armorUpper;
};

struct ControlPointRecordSnap
{
    ControlPointNetworkable::BaseLine rec;
};

struct SimpleObjectRecordSnap
{
    SimpleObjectNetworkable::UPDATE rec;
    bool                            isMobilePhysics;
    bool                            hasArmor;
    int                             armorMaxWreckHp;
    int                             armorLower;
    int                             armorUpper;
    bool                            hasPco;
    bool                            isParachute;
};

struct PlayerRecordSnap
{
    NetUpdateSlot slot;
    bool          isConnected;
    bool          weaponUsesHeat[3];
};

struct ScoreManagerRecordSnap
{
    unsigned char record[164];
};

struct SpawnGroupRecordSnap
{
    int           team;
    unsigned char active;
    unsigned char onlyForAI;
    unsigned char hasSpawnPoints;
    unsigned char posX;
    unsigned char posZ;
    int           squad;
};

// Polymorphic tagged record snapshot stored in per-type dense vectors and indexed by NetRecordSnap.
struct NetRecordSnap
{
    NetEncType     type;
    unsigned short networkableId;
    unsigned int   ghostStateMask;
    unsigned int   minimumBitSize;
    unsigned int   typeRecordIndex; // index into WorldNetSnapshot's per-type vector
    bool           skipFlag20;
    bool           hasNetworkableObject;
    bool           forceNetworkableId;

    // Main-thread pointer used only in Stage A / Stage C commit or net-mt-verify.
    io::INetworkable* liveNetworkable;

    NetRecordSnap()
        : type(NetEncType::Unsupported)
        , networkableId(0)
        , ghostStateMask(0xFFFFFFFFu)
        , minimumBitSize(0)
        , typeRecordIndex(0)
        , skipFlag20(false)
        , hasNetworkableObject(false)
        , forceNetworkableId(false)
        , liveNetworkable(0)
    {
    }
};

struct ScopableEntrySnap
{
    unsigned short             networkableId;
    int                        ownerId;
    float                      basePriority;
    bool                       isProjectile;
    bool                       hasUnsupportedProperty;
    io::NetworkableDescriptor* descriptor;
};

struct ScopableRootSnap
{
    unsigned int rootObjectId;
    unsigned int entryStart;
    unsigned int entryCount;
    float        pos[3];
    float        velocity[3];
    bool         hasPhysicsNode;
    bool         hasUnsupportedEntry;
};

struct AlivePlayerVehicleSnap
{
    int                        playerId;
    int                        team;
    unsigned short             vehicleNetId;
    float                      pos[3];
    io::NetworkableDescriptor* descriptor;
};

class WorldNetSnapshot
{
public:
    enum
    {
        kGridDim          = 64,
        kGridCells        = kGridDim * kGridDim,
        kOverflowBucket   = kGridCells,
        kTotalGridBuckets = kGridCells + 1,
        kMaxNetId         = 65536
    };

    WorldNetSnapshot()
        : m_stateMaskIndexByNetId(kMaxNetId, -1)
        , m_hasCommanderTools(false)
        , m_uavRadius(0.0f)
        , m_recordIndexByNetId(kMaxNetId, -1)
        , m_gridMinX(-2048.0f)
        , m_gridMinZ(-2048.0f)
        , m_gridCellSize(64.0f)
    {
        for (int t = 0; t < 3; ++t)
        {
            m_teamSatelliteScanActive[t] = false;
            m_teamUavActive[t]           = false;
            m_teamUavCentre[t].x         = 0.0f;
            m_teamUavCentre[t].y         = 0.0f;
            m_teamUavCentre[t].z         = 0.0f;
        }
        std::memset(m_gridCounts, 0, sizeof(m_gridCounts));
        std::memset(m_gridOffsets, 0, sizeof(m_gridOffsets));
    }

    ~WorldNetSnapshot()
    {
    }

    // Resets all vectors while preserving allocated capacity.
    void clear()
    {
        for (std::size_t i = 0; i < m_touchedMaskNetIds.size(); ++i)
            m_stateMaskIndexByNetId[m_touchedMaskNetIds[i]] = -1;
        m_touchedMaskNetIds.clear();
        m_updatedStateMasks.clear();

        m_scopableRoots.clear();
        m_scopableEntries.clear();
        m_ownerRoots.clear();

        for (int t = 0; t < 3; ++t)
        {
            m_teamAliveVehicles[t].clear();
            m_teamSpecialCommanderDesc[t].clear();
            m_teamSatelliteScanActive[t] = false;
            m_teamUavActive[t]           = false;
            m_teamUavCentre[t].x         = 0.0f;
            m_teamUavCentre[t].y         = 0.0f;
            m_teamUavCentre[t].z         = 0.0f;
        }
        m_hasCommanderTools = false;
        m_uavRadius         = 0.0f;

        for (std::size_t i = 0; i < m_touchedNetIds.size(); ++i)
            m_recordIndexByNetId[m_touchedNetIds[i]] = -1;
        m_touchedNetIds.clear();

        m_records.clear();
        m_soldierRecords.clear();
        m_ragdollParticles.clear();
        m_itemRecords.clear();
        m_projectileRecords.clear();
        m_controlPointRecords.clear();
        m_simpleObjectRecords.clear();
        m_playerRecords.clear();
        m_scoreManagerRecords.clear();
        m_spawnGroupRecords.clear();

        m_gridRootIndices.clear();
    }

    // Stage A: builds the complete immutable tick snapshot on the main thread after
    // NetworkManager::updateStateMasks() and GameServer::updateConnections().
    void buildFromLiveWorld(
        const std::vector<std::pair<const io::NetworkableDescriptor*, io::NetworkableStateMask> >* updatedMasks);

    // Captures a single INetworkable into the snapshot and returns its index in m_records.
    // Can be called standalone in golden unit tests without a live NetworkManager.
    unsigned int captureNetworkable(io::INetworkable* networkable);

    // Captures a pre-populated NetRecordSnap directly (for unit tests).
    unsigned int addRecordSnap(const NetRecordSnap& snap)
    {
        const unsigned int idx = static_cast<unsigned int>(m_records.size());
        m_records.push_back(snap);
        if (snap.networkableId != 0)
        {
            if (m_recordIndexByNetId[snap.networkableId] < 0)
                m_touchedNetIds.push_back(snap.networkableId);
            m_recordIndexByNetId[snap.networkableId] = static_cast<int>(idx);
        }
        return idx;
    }

    // Fast O(1) lookup by networkableId (returns nullptr if unmapped).
    const NetRecordSnap* findRecordByNetId(unsigned short netId) const
    {
        const int idx = m_recordIndexByNetId[netId];
        return (idx >= 0 && static_cast<std::size_t>(idx) < m_records.size())
            ? &m_records[static_cast<std::size_t>(idx)]
            : 0;
    }

    // Fast O(1) lookup by INetworkable* (falls back to m_networkableId then linear scan if needed).
    const NetRecordSnap* findRecord(const io::INetworkable* networkable) const
    {
        if (!networkable)
            return 0;
        if (networkable->m_networkableId != 0)
        {
            const NetRecordSnap* byId = findRecordByNetId(networkable->m_networkableId);
            if (byId && (byId->liveNetworkable == networkable || byId->liveNetworkable == 0))
                return byId;
        }
        for (std::size_t i = 0; i < m_records.size(); ++i)
        {
            if (m_records[i].liveNetworkable == networkable)
                return &m_records[i];
        }
        return 0;
    }

    // Checks whether every property of a descriptor is supported by ConstNetEncoders.
    bool isDescriptorFullySupported(const io::NetworkableDescriptor* descriptor) const
    {
        if (!descriptor)
            return false;
        for (std::size_t p = 0; p < descriptor->properties.size(); ++p)
        {
            const io::UniqueNetProperty& prop = descriptor->properties[p];
            if (prop.networkables.empty())
                return false;
            const io::INetworkable* const net = prop.networkables[0];
            if (!net)
                continue;
            const NetRecordSnap* const snap = findRecord(net);
            if (!snap || snap->type == NetEncType::Unsupported)
                return false;
        }
        return true;
    }

    // 2D uniform XZ spatial grid build and radius query.
    void configureSpatialGrid(float minX, float minZ, float cellSize)
    {
        m_gridMinX     = minX;
        m_gridMinZ     = minZ;
        m_gridCellSize = (cellSize > 1.0f) ? cellSize : 64.0f;
    }

    void addSpatialRootForTest(float x, float y, float z, unsigned int rootObjectId = 0)
    {
        ScopableRootSnap root;
        root.rootObjectId        = rootObjectId;
        root.entryStart          = 0;
        root.entryCount          = 0;
        root.pos[0]              = x;
        root.pos[1]              = y;
        root.pos[2]              = z;
        root.velocity[0]         = 0.0f;
        root.velocity[1]         = 0.0f;
        root.velocity[2]         = 0.0f;
        root.hasPhysicsNode      = false;
        root.hasUnsupportedEntry = false;
        m_scopableRoots.push_back(root);
    }

    void rebuildSpatialGrid()
    {
        std::memset(m_gridCounts, 0, sizeof(m_gridCounts));
        std::memset(m_gridOffsets, 0, sizeof(m_gridOffsets));

        const unsigned int count = static_cast<unsigned int>(m_scopableRoots.size());
        if (count == 0)
            return;

        const float invCell = (m_gridCellSize > 0.0f) ? (1.0f / m_gridCellSize) : (1.0f / 64.0f);

        for (unsigned int i = 0; i < count; ++i)
        {
            const ScopableRootSnap& root = m_scopableRoots[i];
            const int cx = static_cast<int>(std::floor((root.pos[0] - m_gridMinX) * invCell));
            const int cz = static_cast<int>(std::floor((root.pos[2] - m_gridMinZ) * invCell));
            int bucket = kOverflowBucket;
            if (cx >= 0 && cx < kGridDim && cz >= 0 && cz < kGridDim)
                bucket = cz * kGridDim + cx;
            ++m_gridCounts[bucket];
        }

        unsigned int running = 0;
        for (int b = 0; b < kTotalGridBuckets; ++b)
        {
            m_gridOffsets[b] = running;
            running += m_gridCounts[b];
            m_gridCounts[b] = m_gridOffsets[b];
        }
        m_gridOffsets[kTotalGridBuckets] = running;

        m_gridRootIndices.resize(count);

        for (unsigned int i = 0; i < count; ++i)
        {
            const ScopableRootSnap& root = m_scopableRoots[i];
            const int cx = static_cast<int>(std::floor((root.pos[0] - m_gridMinX) * invCell));
            const int cz = static_cast<int>(std::floor((root.pos[2] - m_gridMinZ) * invCell));
            int bucket = kOverflowBucket;
            if (cx >= 0 && cx < kGridDim && cz >= 0 && cz < kGridDim)
                bucket = cz * kGridDim + cx;
            m_gridRootIndices[m_gridCounts[bucket]++] = i;
        }
    }

    // Calls cb(unsigned int rootIndex) for every root whose 2D XZ distance to (cx, cz) is <= radius.
    template <typename Callback>
    void queryRadius(float cx, float cz, float radius, Callback cb) const
    {
        if (m_scopableRoots.empty() || radius < 0.0f)
            return;

        const float radiusSq = radius * radius;
        const float invCell  = (m_gridCellSize > 0.0f) ? (1.0f / m_gridCellSize) : (1.0f / 64.0f);

        const float minWorldX = cx - radius;
        const float maxWorldX = cx + radius;
        const float minWorldZ = cz - radius;
        const float maxWorldZ = cz + radius;

        int cellMinX = static_cast<int>(std::floor((minWorldX - m_gridMinX) * invCell));
        int cellMaxX = static_cast<int>(std::floor((maxWorldX - m_gridMinX) * invCell));
        int cellMinZ = static_cast<int>(std::floor((minWorldZ - m_gridMinZ) * invCell));
        int cellMaxZ = static_cast<int>(std::floor((maxWorldZ - m_gridMinZ) * invCell));

        const bool touchesOutOfBounds =
            (cellMinX < 0 || cellMaxX >= kGridDim || cellMinZ < 0 || cellMaxZ >= kGridDim);

        if (cellMinX < 0) cellMinX = 0;
        if (cellMaxX >= kGridDim) cellMaxX = kGridDim - 1;
        if (cellMinZ < 0) cellMinZ = 0;
        if (cellMaxZ >= kGridDim) cellMaxZ = kGridDim - 1;

        if (cellMinX <= cellMaxX && cellMinZ <= cellMaxZ)
        {
            for (int czIdx = cellMinZ; czIdx <= cellMaxZ; ++czIdx)
            {
                const int rowBase = czIdx * kGridDim;
                for (int cxIdx = cellMinX; cxIdx <= cellMaxX; ++cxIdx)
                {
                    const int bucket = rowBase + cxIdx;
                    const unsigned int begin = m_gridOffsets[bucket];
                    const unsigned int end   = m_gridOffsets[bucket + 1];
                    for (unsigned int i = begin; i < end; ++i)
                    {
                        const unsigned int rootIdx = m_gridRootIndices[i];
                        const ScopableRootSnap& root = m_scopableRoots[rootIdx];
                        const float dx = root.pos[0] - cx;
                        const float dz = root.pos[2] - cz;
                        if (dx * dx + dz * dz <= radiusSq)
                            cb(rootIdx);
                    }
                }
            }
        }

        if (touchesOutOfBounds)
        {
            const unsigned int begin = m_gridOffsets[kOverflowBucket];
            const unsigned int end   = m_gridOffsets[kOverflowBucket + 1];
            for (unsigned int i = begin; i < end; ++i)
            {
                const unsigned int rootIdx = m_gridRootIndices[i];
                const ScopableRootSnap& root = m_scopableRoots[rootIdx];
                const float dx = root.pos[0] - cx;
                const float dz = root.pos[2] - cz;
                if (dx * dx + dz * dz <= radiusSq)
                    cb(rootIdx);
            }
        }
    }

    // Stage C: commits main-thread side effects (m_next = m_head, soldier rec->ammoStatus &= 0xF)
    // once for all captured networkables, and adds accumulated per-type writeBits to stats.
    struct TypeWriteBitStats
    {
        int bitsByType[10];
        TypeWriteBitStats()
        {
            std::memset(bitsByType, 0, sizeof(bitsByType));
        }
    };
    void commitMainThreadSideEffects(const TypeWriteBitStats* optionalBitStats = 0) const;

public:
    // Dense state masks from NetworkManager::updateStateMasks().
    std::vector<std::pair<const io::NetworkableDescriptor*, io::NetworkableStateMask> > m_updatedStateMasks;
    std::vector<int> m_stateMaskIndexByNetId;

    // Scopable root groups and entries.
    std::vector<ScopableRootSnap>            m_scopableRoots;
    std::vector<ScopableEntrySnap>           m_scopableEntries;
    std::vector<std::pair<int, unsigned int> > m_ownerRoots; // (ownerId, rootIndex) for always-scoped own vehicle groups

    // Always-relevant team/commander snapshots (indexed by team 1 or 2).
    std::vector<AlivePlayerVehicleSnap>      m_teamAliveVehicles[3];
    std::vector<io::NetworkableDescriptor*>  m_teamSpecialCommanderDesc[3];
    bool                                     m_hasCommanderTools;
    bool                                     m_teamSatelliteScanActive[3];
    bool                                     m_teamUavActive[3];
    Vec3                                     m_teamUavCentre[3];
    float                                    m_uavRadius;

    // Per-type immutable record tables.
    std::vector<NetRecordSnap>               m_records;
    std::vector<int>                         m_recordIndexByNetId;
    std::vector<SoldierRecordSnap>           m_soldierRecords;
    std::vector<RagDollParticleSnap>         m_ragdollParticles;
    std::vector<ItemRecordSnap>              m_itemRecords;
    std::vector<GenericProjectileRecordSnap> m_projectileRecords;
    std::vector<ControlPointRecordSnap>      m_controlPointRecords;
    std::vector<SimpleObjectRecordSnap>      m_simpleObjectRecords;
    std::vector<PlayerRecordSnap>            m_playerRecords;
    std::vector<ScoreManagerRecordSnap>      m_scoreManagerRecords;
    std::vector<SpawnGroupRecordSnap>        m_spawnGroupRecords;

private:
    float                     m_gridMinX;
    float                     m_gridMinZ;
    float                     m_gridCellSize;
    unsigned int              m_gridCounts[kTotalGridBuckets];
    unsigned int              m_gridOffsets[kTotalGridBuckets + 1];
    std::vector<unsigned int> m_gridRootIndices;
    std::vector<unsigned short> m_touchedNetIds;
    std::vector<unsigned short> m_touchedMaskNetIds;
};

} // namespace netpipeline
} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_NETPIPELINE_WORLDNETSNAPSHOT_H
