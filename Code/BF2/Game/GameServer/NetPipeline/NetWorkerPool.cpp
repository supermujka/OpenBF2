// [CHOSEN, not recovered] Persistent C++14 worker thread pool and Stage A/B/C multi-threaded
// network snapshot pipeline for 32v32 dedicated servers.

#include "NetWorkerPool.h"
#include "../../Utility/Vars.h"

#ifndef NET_WORKER_POOL_CORE_ONLY
#include "../ServerTickProfiler.h"
#include "../../Common/ServerConnection.h"
#include "../../Common/GhostManager.h"
#include "../../Common/GhostObject.h"
#include "../../Common/IStreamManager.h"
#include "../../Common/PlayerActionManager.h"
#include "../../Common/GameEventManager.h"
#include "../../Common/DataBlockManager.h"
#include "../../Common/StringManager.h"
#include "../../Objects/IObject.h"
#include "../../Objects/IObjectTemplate.h"
#include "../../Objects/IPlayer.h"
#include "../../Objects/ISoldier.h"
#include "../../Objects/ICameraObject.h"
#include "../../Objects/ObjectLayoutAccess.h"
#include "../../GameLogic/SquadManager/ISquadManager.h"
#include "../../IGameLogic.h"
#include "../../../Core/System.h"
#include "../../../Core/Ids.h"
#include "../../../Physics/LatencyCompensation.h"
#include "../../../Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dice {
namespace hfe {

#ifndef NET_WORKER_POOL_CORE_ONLY
extern System* g_system;
extern IBase*  squadManager;
#endif

namespace netpipeline {

// ---------------------------------------------------------------------------
// CVar accessors (lazy function-local statics for partial-link safety)
// ---------------------------------------------------------------------------

bool NetWorkerPool::isEnabled()
{
    static int* const s_mtEnable = Vars::getInt("net-mt-enable", 0);
    return s_mtEnable != 0 && (*s_mtEnable != 0);
}

int NetWorkerPool::getConfiguredThreadCount()
{
    static int* const s_mtThreads = Vars::getInt("net-mt-threads", 0);
    const int raw = s_mtThreads ? *s_mtThreads : 0;
    if (raw > 0)
        return raw > 16 ? 16 : raw;

    const unsigned int hw = std::thread::hardware_concurrency();
    int autoThreads = (hw > 0) ? static_cast<int>(hw / 2) : 2;
    if (autoThreads < 2)
        autoThreads = 2;
    if (autoThreads > 4)
        autoThreads = 4;
    return autoThreads;
}

bool NetWorkerPool::isVerifyEnabled()
{
    static int* const s_mtVerify = Vars::getInt("net-mt-verify", 0);
    return s_mtVerify != 0 && (*s_mtVerify != 0);
}

float NetWorkerPool::getScopeHysteresis()
{
    static float* const s_scopeDist  = Vars::getFloat("net-scopeDistance", 0.0f);
    if (!s_scopeDist || !(*s_scopeDist > 0.0f))
        return 0.0f;
    static float* const s_hysteresis = Vars::getFloat("net-scopeHysteresis", 0.0f);
    const float raw = s_hysteresis ? *s_hysteresis : 0.0f;
    if (raw <= 0.0f)
        return 0.0f;
    return raw > 200.0f ? 200.0f : raw;
}

// ---------------------------------------------------------------------------
// Thread pool core (generation barrier + atomic dynamic work stealing)
// ---------------------------------------------------------------------------

NetWorkerPool::NetWorkerPool()
    : m_stopping(false)
    , m_generation(0)
    , m_activeWorkers(0)
    , m_taskCount(0)
    , m_taskThunk(0)
    , m_taskCtx(0)
    , m_nextIndex(0)
{
}

NetWorkerPool::~NetWorkerPool()
{
    shutdown();
}

NetWorkerPool& NetWorkerPool::instance()
{
    static NetWorkerPool s_pool;
    return s_pool;
}

void NetWorkerPool::ensureStarted(int requestedThreads)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_workers.empty())
        return;

    int count = (requestedThreads > 0) ? requestedThreads : getConfiguredThreadCount();
    if (count < 1)
        count = 1;
    else if (count > 16)
        count = 16;

    // When count == 1, the calling thread executes work directly with zero extra threads.
    if (count == 1)
        return;

    m_stopping = false;
    // Spawn (count - 1) background workers; calling thread participates as worker #0.
    const std::size_t bgWorkers = static_cast<std::size_t>(count - 1);
    m_workers.reserve(bgWorkers);
    for (std::size_t i = 0; i < bgWorkers; ++i)
        m_workers.push_back(std::thread(&NetWorkerPool::workerLoop, this));
}

void NetWorkerPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_workers.empty())
            return;
        m_stopping = true;
    }
    m_workCv.notify_all();

    for (std::size_t i = 0; i < m_workers.size(); ++i)
    {
        if (m_workers[i].joinable())
            m_workers[i].join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers.clear();
        m_stopping      = false;
        m_generation    = 0;
        m_activeWorkers = 0;
        m_taskCount     = 0;
        m_taskThunk     = 0;
        m_taskCtx       = 0;
    }
}

std::size_t NetWorkerPool::workerCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_workers.size();
}

void NetWorkerPool::dispatchBatch(std::size_t count, TaskThunk thunk, void* ctx)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_taskCount     = count;
        m_taskThunk     = thunk;
        m_taskCtx       = ctx;
        m_nextIndex.store(0, std::memory_order_relaxed);
        m_activeWorkers = m_workers.size();
        ++m_generation;
    }
    m_workCv.notify_all();

    // Calling thread participates in dynamic work stealing.
    for (;;)
    {
        const std::size_t idx = m_nextIndex.fetch_add(1, std::memory_order_relaxed);
        if (idx >= count)
            break;
        thunk(ctx, idx);
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_doneCv.wait(lock, [this]() { return m_activeWorkers == 0; });
    m_taskThunk = 0;
    m_taskCtx   = 0;
    m_taskCount = 0;
}

void NetWorkerPool::workerLoop()
{
    unsigned long long seenGeneration = 0;
    for (;;)
    {
        TaskThunk   thunk = 0;
        void*       ctx   = 0;
        std::size_t count = 0;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workCv.wait(lock, [this, seenGeneration]() {
                return m_stopping || m_generation != seenGeneration;
            });
            if (m_stopping)
                return;

            seenGeneration = m_generation;
            thunk          = m_taskThunk;
            ctx            = m_taskCtx;
            count          = m_taskCount;
        }

        if (thunk)
        {
            for (;;)
            {
                const std::size_t idx = m_nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (idx >= count)
                    break;
                thunk(ctx, idx);
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_activeWorkers > 0)
            {
                --m_activeWorkers;
                if (m_activeWorkers == 0)
                    m_doneCv.notify_one();
            }
        }
    }
}

#ifndef NET_WORKER_POOL_CORE_ONLY

namespace {

// Resolves soldier object for control-object state sync (exact mirror of GhostManager.cpp getSoldier).
world::IObject* getSoldierForControlSnap(world::IObject* object)
{
    if (!object)
        return 0;

    if (world::object_detail::LayoutAccess::objectTemplate(object)->getClassID()
            == world::CID_SoldierTemplate)
        return object;

    world::IPlayerControlObject* pco = 0;
    for (world::IObject* o = object; o; o = o->getParent())
    {
        pco = static_cast<world::IPlayerControlObject*>(
                  o->queryInterface(world::IID_IPlayerControlObject));
        if (pco)
            break;
    }

    if (pco && pco->getOwnCameras()->empty() && pco->isChildSoldierActive())
        return pco->getOccupingObject();
    return 0;
}

// Writes raw pre-captured bit buffer (stateSyncBuf[0..bits-1]) into stream.
void writeRawBitBuffer(io::BitStream& stream, const unsigned char* buf, unsigned int totalBits)
{
    const unsigned int fullBytes = totalBits >> 3;
    const unsigned int remBits   = totalBits & 7u;
    for (unsigned int i = 0; i < fullBytes; ++i)
        stream.writeBits(&buf[i], 8);
    if (remBits > 0)
        stream.writeBits(&buf[fullBytes], remBits);
}

// Lock-free Stage B equivalent of GhostManager::ghostObject() (zero Debug(), zero live INetworkableInfo vcalls).
void stageBGhostObject(
    const WorldNetSnapshot& worldSnap,
    GhostManager* gm,
    io::NetworkableDescriptor* descriptor,
    unsigned short netId,
    float priority,
    bool keepHigher,
    bool& outSerialFallback)
{
    if (!descriptor || netId == 0)
        return;

    if (priority >= 10000.0f)
        priority = 10000.0f;

    GhostObject* const existing = gm->findGhostObjectInHash(netId);
    if (existing)
    {
        if (descriptor != existing->m_descriptor)
        {
            existing->m_descriptor = descriptor;
            for (int i = 0; i < 12; ++i)
                existing->m_stateMask[i] = -1;
        }

        existing->m_ghostableState = 1;

        if (priority < 0.0f)
        {
            for (int i = 0; i < 12; ++i)
                existing->m_stateMask[i] = -1;
        }

        if (keepHigher && existing->m_priority < 10000.0f)
        {
            if (existing->m_priority <= priority)
                existing->m_priority = priority;
        }
        else
        {
            existing->m_priority = priority;
        }
        return;
    }

    // Seed ghost ID pool if needed; if exhausted, fall back to main thread instead of Debug() on worker.
    if (!gm->m_ghostIdPoolSeeded)
    {
        gm->m_ghostIdPoolSeeded = true;
        for (unsigned short id = 0; id <= 0x3FE; ++id)
            gm->m_ghostIdPool.push_back(id);
    }
    if (gm->m_ghostIdPool.empty())
    {
        outSerialFallback = true;
        return;
    }

    const unsigned short ghostId = gm->m_ghostIdPool.front();
    gm->m_ghostIdPool.pop_front();
    if (gm->m_ghostObjects[ghostId])
    {
        gm->m_ghostIdPool.push_front(ghostId);
        outSerialFallback = true;
        return;
    }

    GhostObject* const ghost = new GhostObject;
    ghost->m_byte51              = false;
    ghost->m_ghostId             = ghostId;
    ghost->m_ghostableState      = 1;
    ghost->m_priority            = 10000.0f;
    ghost->m_networkableId       = netId;
    ghost->m_accumulatedPriority = 0.0f;
    for (int i = 0; i < 12; ++i)
        ghost->m_stateMask[i] = -1;
    ghost->m_descriptor       = descriptor;
    ghost->m_ghostablePending = true;
    ghost->m_isStaticGhost    = (priority < 0.0f);

    const NetRecordSnap* const recSnap = worldSnap.findRecordByNetId(netId);
    if (recSnap && recSnap->hasNetworkableObject)
    {
        if (!gm->m_ghostables.exists(netId))
        {
            if (!recSnap->forceNetworkableId)
                ghost->m_ghostablePending = false;
        }
    }

    gm->m_ghostObjects[ghostId] = ghost;
    gm->m_updateActiveGhostVec  = true;
    gm->addGhostObjectToHash(netId, ghost);
}

// Stage B lock-free spatial-grid scoping for one connection (mirrors ServerConnection::updateGhostManager).
void stageBScopeConnection(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task)
{
    GhostManager* const gm = task.ghostManager;
    gm->resetGhostStates(false);

    if (!task.hasCamera)
        return;

    task.conn->m_dword17C = 0;

    const float* const view    = task.viewTransform.m;
    const float camX           = view[12];
    const float camY           = view[13];
    const float camZ           = view[14];
    const float range          = task.viewRange;
    const float rangeSq        = range * range;
    const float hysteresis     = task.hysteresis;
    const float outerRange     = range + hysteresis;
    const float outerRangeSq   = outerRange * outerRange;

    // 1. Own vehicle's root group(s): always scoped at 9999.0f regardless of camera distance
    //    (handles commander overhead camera being far from commander's soldier).
    for (std::size_t i = 0; i < worldSnap.m_ownerRoots.size(); ++i)
    {
        if (worldSnap.m_ownerRoots[i].first != task.playerId)
            continue;
        const unsigned int rootIdx = worldSnap.m_ownerRoots[i].second;
        if (rootIdx >= worldSnap.m_scopableRoots.size())
            continue;
        const ScopableRootSnap& root = worldSnap.m_scopableRoots[rootIdx];
        for (unsigned int e = 0; e < root.entryCount; ++e)
        {
            const ScopableEntrySnap& entry = worldSnap.m_scopableEntries[root.entryStart + e];
            if (entry.ownerId == task.playerId)
                continue;
            const float bonus = entry.isProjectile ? 0.30000001f : 0.0f;
            stageBGhostObject(
                worldSnap, gm, entry.descriptor, entry.networkableId,
                (bonus + 9999.0f) * entry.basePriority, false, task.serialFallback);
        }
    }

    // 2. 2D uniform XZ spatial-grid query over all scopable roots within outerRange.
    worldSnap.queryRadius(camX, camZ, outerRange, [&](unsigned int rootIdx) {
        const ScopableRootSnap& root = worldSnap.m_scopableRoots[rootIdx];
        if (root.entryCount == 0)
            return;

        // Skip groups owned by this player (already ghosted at 9999.0f above).
        for (unsigned int e = 0; e < root.entryCount; ++e)
        {
            if (worldSnap.m_scopableEntries[root.entryStart + e].ownerId == task.playerId)
                return;
        }

        ++task.conn->m_dword17C;

        const float gx     = camX - root.pos[0];
        const float gy     = camY - root.pos[1];
        const float gz     = camZ - root.pos[2];
        const float distSq = gx * gx + gy * gy + gz * gz;

        if (!(outerRangeSq >= distSq))
            return;

        bool inHysteresisBand = false;
        if (!(rangeSq >= distSq))
        {
            if (hysteresis <= 0.0f)
                return;

            // Only keep ghosts that are ALREADY tracked by this connection inside the hysteresis band.
            bool alreadyTracked = false;
            for (unsigned int e = 0; e < root.entryCount; ++e)
            {
                const unsigned short netId = worldSnap.m_scopableEntries[root.entryStart + e].networkableId;
                if (gm->findGhostObjectInHash(netId) != 0)
                {
                    alreadyTracked = true;
                    break;
                }
            }
            if (!alreadyTracked)
                return;
            inHysteresisBand = true;
        }

        const float dx = root.pos[0] - camX;
        const float dy = root.pos[1] - camY;
        const float dz = root.pos[2] - camZ;
        float distance = std::sqrt(dy * dy + dx * dx + dz * dz);
        if (!(distance >= 1.1920929e-07f))
            distance = 0.001f;

        const float inv = 1.0f / distance;

        float cone = 0.0f;
        if (dy * inv * view[9] + dx * inv * view[8] + dz * inv * view[10] >= task.fovThreshold)
            cone = 1.0f;
        if (!(distance >= 10.0f))
            cone = 1.0f;

        float speed = 0.0f;
        if (root.hasPhysicsNode)
        {
            speed = (root.velocity[0] * root.velocity[0]
                   + root.velocity[1] * root.velocity[1]
                   + root.velocity[2] * root.velocity[2]) / 1600.0f;
            if (1.0f <= speed)
                speed = 1.0f;
        }

        float priority = 0.0f;
        if (!inHysteresisBand)
        {
            priority = cone + (1.0f - distance / range) + speed;
            if (priority <= 0.0f && hysteresis > 0.0f)
            {
                bool alreadyTracked = false;
                for (unsigned int e = 0; e < root.entryCount; ++e)
                {
                    const unsigned short netId = worldSnap.m_scopableEntries[root.entryStart + e].networkableId;
                    if (gm->findGhostObjectInHash(netId) != 0)
                    {
                        alreadyTracked = true;
                        break;
                    }
                }
                if (alreadyTracked)
                    priority = cone + (1.0f - distance / outerRange) + speed;
            }
        }
        else
        {
            priority = cone + (1.0f - distance / outerRange) + speed;
        }

        if (priority <= 0.0f)
            return;

        for (unsigned int e = 0; e < root.entryCount; ++e)
        {
            const ScopableEntrySnap& entry = worldSnap.m_scopableEntries[root.entryStart + e];
            if (entry.ownerId == task.playerId)
                continue;
            const float bonus = entry.isProjectile ? 0.30000001f : 0.0f;
            stageBGhostObject(
                worldSnap, gm, entry.descriptor, entry.networkableId,
                (bonus + priority) * entry.basePriority, false, task.serialFallback);
        }
    });

    // 3. Living teammates' vehicles at 0.03 floor with keepHigher.
    if (task.teamId == 1 || task.teamId == 2)
    {
        const std::vector<AlivePlayerVehicleSnap>& teammates =
            worldSnap.m_teamAliveVehicles[task.teamId];
        for (std::size_t i = 0; i < teammates.size(); ++i)
        {
            const AlivePlayerVehicleSnap& tm = teammates[i];
            if (tm.playerId == task.playerId)
                continue;
            stageBGhostObject(
                worldSnap, gm, tm.descriptor, tm.vehicleNetId, 0.03f, true, task.serialFallback);
        }
    }

    // 4. Commander tools (Satellite scan / UAV / Remote-controlled objects).
    if (worldSnap.m_hasCommanderTools && (task.teamId == 1 || task.teamId == 2))
    {
        const int enemyTeam = (task.teamId == 1) ? 2 : 1;
        if (task.hasSatScan)
        {
            const std::vector<AlivePlayerVehicleSnap>& enemies =
                worldSnap.m_teamAliveVehicles[enemyTeam];
            for (std::size_t i = 0; i < enemies.size(); ++i)
            {
                const AlivePlayerVehicleSnap& en = enemies[i];
                if (en.playerId == task.playerId)
                    continue;
                stageBGhostObject(
                    worldSnap, gm, en.descriptor, en.vehicleNetId, 0.5f, true, task.serialFallback);
            }
        }
        else if (task.hasUav)
        {
            const float radSq = (task.uavRadius > 0.0f) ? (task.uavRadius * task.uavRadius) : -1.0f;
            const std::vector<AlivePlayerVehicleSnap>& enemies =
                worldSnap.m_teamAliveVehicles[enemyTeam];
            for (std::size_t i = 0; i < enemies.size(); ++i)
            {
                const AlivePlayerVehicleSnap& en = enemies[i];
                if (en.playerId == task.playerId)
                    continue;
                bool inRange = (radSq <= 0.0f);
                if (!inRange)
                {
                    const float dx = en.pos[0] - task.uavCentre.x;
                    const float dy = en.pos[1] - task.uavCentre.y;
                    const float dz = en.pos[2] - task.uavCentre.z;
                    inRange = (dx * dx + dy * dy + dz * dz <= radSq);
                }
                if (inRange)
                {
                    stageBGhostObject(
                        worldSnap, gm, en.descriptor, en.vehicleNetId, 0.5f, true, task.serialFallback);
                }
            }
        }

        if (task.isCommander)
        {
            const std::vector<io::NetworkableDescriptor*>& specialDescs =
                worldSnap.m_teamSpecialCommanderDesc[task.teamId];
            for (std::size_t i = 0; i < specialDescs.size(); ++i)
            {
                io::NetworkableDescriptor* const desc = specialDescs[i];
                if (!desc || desc->properties.empty() || desc->properties[0].networkables.empty())
                    continue;
                const unsigned short netId = desc->properties[0].networkables[0]->m_networkableId;
                stageBGhostObject(worldSnap, gm, desc, netId, 0.03f, true, task.serialFallback);
            }
        }
    }
}

void recordVerifyEntry(
    ConnectionStageTask& task,
    const io::NetworkableDescriptor* desc,
    const io::NetworkableStateMask* mask,
    bool ghostTier,
    int precision,
    const io::BitStream& stream,
    bool isDelta,
    const io::BaseLineData* baselineBefore)
{
    if (task.verifyEntries.size() >= 64)
        return;

    VerifyDescriptorEntry ve;
    ve.descriptor = desc;
    if (mask)
    {
        ve.mask    = *mask;
        ve.hasMask = true;
    }
    else
    {
        std::memset(&ve.mask, 0, sizeof(ve.mask));
        ve.hasMask = false;
    }
    ve.ghostTier  = ghostTier;
    ve.precision  = precision;
    ve.hasCompVec = stream.hasCompressionVector();
    const Vec3 zeroVec = { 0.0f, 0.0f, 0.0f };
    ve.compVec    = ve.hasCompVec ? stream.getCompressionVector() : zeroVec;
    ve.isDelta    = isDelta;
    if (isDelta && baselineBefore)
        ve.baselineBefore = *baselineBefore;
    else
        std::memset(&ve.baselineBefore, 0, sizeof(ve.baselineBefore));

    task.verifyEntries.push_back(ve);
}

// Stage B pure-const control object state writer (bit-exact mirror of GhostManager::writeControlObjectState).
bool stageBWriteControlObjectState(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    bool verifyEnabled)
{
    GhostManager* const gm     = task.ghostManager;
    io::BitStream* const stream = task.bitStream;

    if (!task.hasControlToWrite || gm->m_actionBufferEntries == -1)
        return false;

    stream->m_byte38 = true;
    stream->m_positionCache = (stream->m_readWriteSwitch == 1) ? stream->m_readBitPos : stream->m_bitPos;
    stream->m_rwSwitchCache = stream->m_readWriteSwitch;
    stream->m_lengthCache   = stream->m_highWaterBits;

    io::BSPosition outerMark;
    outerMark.m_unknown00       = 0;
    outerMark.m_unknown08       = 0;
    outerMark.m_readWriteSwitch = stream->m_readWriteSwitch;
    outerMark.m_position        = stream->getBitPosition();

    stream->skipBits(12);
    const unsigned int payloadStart = stream->getBitPosition();

    if (gm->m_actionBufferEntries < 0)
    {
        const unsigned int one = 1;
        stream->writeBits(&one, 1);
        const unsigned int magnitude = static_cast<unsigned int>(-gm->m_actionBufferEntries);
        stream->writeBits(&magnitude, 31);
    }
    else
    {
        const unsigned int zero = 0;
        stream->writeBits(&zero, 1);
        const unsigned int magnitude = static_cast<unsigned int>(gm->m_actionBufferEntries);
        stream->writeBits(&magnitude, 31);
    }

    stream->resetCompressionVector();
    if (task.controlHasNetObj)
    {
        stream->writeBits(&gm->m_compressionVector.x, 32);
        stream->writeBits(&gm->m_compressionVector.y, 32);
        stream->writeBits(&gm->m_compressionVector.z, 32);
        stream->setCompressionVector(gm->m_compressionVector);
    }

    stream->writeBits(&task.controlObjectNetId, 16);

    const bool hasParachuteSoldier = (task.parachuteSoldierNetId != 0);
    {
        const unsigned int flag = hasParachuteSoldier ? 1u : 0u;
        stream->writeBits(&flag, 1);
    }
    if (hasParachuteSoldier)
        stream->writeBits(&task.parachuteSoldierNetId, 16);

    {
        const unsigned int flag = task.controlIsSoldier ? 1u : 0u;
        stream->writeBits(&flag, 1);
    }
    if (task.controlIsSoldier)
    {
        if (task.controlHasKit)
        {
            const unsigned int flag = 1;
            stream->writeBits(&flag, 1);
            stream->writeBits(&task.controlKitNetId, 16);
        }
        else
        {
            const unsigned int flag = 0;
            stream->writeBits(&flag, 1);
        }
    }

    auto rollback = [&]() -> bool {
        stream->setPosition(*reinterpret_cast<io::BSPosition*>(&stream->m_dword3C));
        stream->m_byte38        = false;
        stream->m_errorCode     = 0;
        stream->m_error         = false;
        stream->m_highWaterBits = stream->m_lengthCache;
        stream->resetCompressionVector();
        return false;
    };

    if (!task.controlNeedsFreshBaseline)
    {
        const unsigned int flag = 1;
        stream->writeBits(&flag, 1);
        const unsigned int ackedSlot = static_cast<unsigned int>(gm->m_baseLineIndex);
        stream->writeBits(&ackedSlot, 3);

        io::BaseLineData& baseLine = *reinterpret_cast<io::BaseLineData*>(
            gm->m_baseLineData + 1026 * gm->m_baseLineIndex);

        if (verifyEnabled)
        {
            baseLine.m_cursor = 0;
            recordVerifyEntry(task, task.controlDescriptor, 0, false, io::StatePrecisionFull, *stream, true, &baseLine);
        }

        bool failed = !ConstNetEncoders::encodeDescriptorDelta(
            worldSnap, task.controlDescriptor, *stream, baseLine, 0,
            io::StatePrecisionFull, false, true,
            &task.writeBitStats, &task.missingUpdatesCount, &task.encodedRecordIndices);
        if (!failed && hasParachuteSoldier)
        {
            if (verifyEnabled)
                recordVerifyEntry(task, task.parachuteDescriptor, 0, false, io::StatePrecisionFull, *stream, true, &baseLine);
            failed = !ConstNetEncoders::encodeDescriptorDelta(
                worldSnap, task.parachuteDescriptor, *stream, baseLine, 0,
                io::StatePrecisionFull, false, false,
                &task.writeBitStats, &task.missingUpdatesCount, &task.encodedRecordIndices);
        }
        if (failed)
            return rollback();

        --gm->m_deltasUntilBaseLine;
    }
    else
    {
        const unsigned int flag = 0;
        stream->writeBits(&flag, 1);

        const int slot = (gm->m_currentBaseLineSlot + 1) % 8;
        io::BaseLineData& baseLine = *reinterpret_cast<io::BaseLineData*>(
            gm->m_baseLineData + 1026 * slot);
        baseLine = task.freshBaseLine;

        const unsigned int newSlot = static_cast<unsigned int>(slot);
        stream->writeBits(&newSlot, 3);

        if (verifyEnabled)
            recordVerifyEntry(task, task.controlDescriptor, 0, false, io::StatePrecisionFull, *stream, false, 0);

        bool failed = !ConstNetEncoders::encodeDescriptor(
            worldSnap, task.controlDescriptor, *stream, 0,
            io::StatePrecisionFull, false,
            &task.writeBitStats, &task.didNotFitCount, &task.missingUpdatesCount, &task.encodedRecordIndices);
        if (!failed && hasParachuteSoldier)
        {
            if (verifyEnabled)
                recordVerifyEntry(task, task.parachuteDescriptor, 0, false, io::StatePrecisionFull, *stream, false, 0);
            failed = !ConstNetEncoders::encodeDescriptor(
                worldSnap, task.parachuteDescriptor, *stream, 0,
                io::StatePrecisionFull, false,
                &task.writeBitStats, &task.didNotFitCount, &task.missingUpdatesCount, &task.encodedRecordIndices);
        }
        if (failed)
            return rollback();

        gm->m_currentBaseLineSlot    = slot;
        gm->m_baseLineSlotThisPacket = slot;
        gm->m_deltasUntilBaseLine    = GhostManager::m_numDeltasPerBaseLine;
    }

    // 10-bit-wrapped Player::getStateSync block (pre-captured on main thread in Stage A).
    if (task.stateSyncError || task.stateSyncBits > 0x3FFu)
        return rollback();
    stream->writeBits(&task.stateSyncBits, 10);
    if (task.stateSyncBits > 0)
        writeRawBitBuffer(*stream, task.stateSyncBuf, task.stateSyncBits);

    // Outer 12-bit length backfill.
    const unsigned int payloadEnd = stream->getBitPosition();
    const unsigned int totalBits  = payloadEnd - payloadStart;
    if (totalBits > 0xFFFu)
        return rollback();

    stream->setWritePosition(outerMark);
    stream->writeBits(&totalBits, 12);
    if (stream->m_canResetWritePosition)
    {
        stream->m_canResetWritePosition = false;
        stream->setPosition(*reinterpret_cast<io::BSPosition*>(&stream->m_dword24));
    }
    else
    {
        return rollback();
    }

    if (stream->m_error)
        return rollback();

    stream->m_byte38 = false;
    return true;
}

// Stage B pure-const single ghost write (bit-exact mirror of GhostManager::writeData).
bool stageBWriteData(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    GhostAction action,
    GhostObject* ghost,
    bool flag,
    bool verifyEnabled)
{
    if (!ghost)
        return false;

    GhostManager* const gm     = task.ghostManager;
    io::BitStream& stream      = *task.bitStream;

    const unsigned int actionBits = static_cast<unsigned int>(action);
    stream.writeBits(&actionBits, 2);
    const unsigned short netId = ghost->m_networkableId;
    stream.writeBits(&netId, 16);

    if (action == 1)
    {
        const unsigned int flagBit = flag ? 1u : 0u;
        stream.writeBits(&flagBit, 1);

        io::BSPosition mark;
        mark.m_unknown00       = 0;
        mark.m_unknown08       = 0;
        mark.m_readWriteSwitch = stream.m_readWriteSwitch;
        mark.m_position        = stream.getBitPosition();
        stream.skipBits(static_cast<unsigned int>(gm->m_updateLengthBits));

        const unsigned int payloadStart = stream.getBitPosition();
        const io::NetworkableStateMask* const maskPtr =
            reinterpret_cast<const io::NetworkableStateMask*>(ghost->m_stateMask);

        if (verifyEnabled)
            recordVerifyEntry(task, ghost->m_descriptor, maskPtr, true, io::StatePrecisionGhost, stream, false, 0);

        if (!ConstNetEncoders::encodeDescriptor(
                worldSnap, ghost->m_descriptor, stream, maskPtr,
                io::StatePrecisionGhost, true,
                &task.writeBitStats, &task.didNotFitCount, &task.missingUpdatesCount,
                &task.encodedRecordIndices))
        {
            return false;
        }

        const unsigned int payloadEnd  = stream.getBitPosition();
        const unsigned int payloadBits = payloadEnd - payloadStart;

        stream.setWritePosition(mark);
        stream.writeBits(&payloadBits, static_cast<unsigned int>(gm->m_updateLengthBits));

        if (stream.m_canResetWritePosition)
        {
            stream.m_canResetWritePosition = false;
            stream.setPosition(*reinterpret_cast<io::BSPosition*>(&stream.m_dword24));
        }
        else
        {
            return false;
        }
    }

    if (stream.m_error)
    {
        stream.m_errorCode = 0;
        stream.m_error     = false;
        return false;
    }
    return true;
}

// Stage B pure-const ghost send + rollback + undo save record (bit-exact mirror of GhostManager::sendData).
bool stageBSendData(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    GhostAction action,
    GhostObject* ghost,
    bool flag,
    GhostManager::SaveListEntry& entry,
    bool verifyEnabled)
{
    GhostManager* const gm = task.ghostManager;
    io::BitStream& stream  = *task.bitStream;

    // Bit 5 skip gate snapshotted in WorldNetSnapshot (zero live IObject access).
    const NetRecordSnap* const recSnap = worldSnap.findRecordByNetId(ghost->m_networkableId);
    if (recSnap && recSnap->skipFlag20)
        return true;

    stream.m_byte38        = true;
    stream.m_positionCache = (stream.m_readWriteSwitch == 1) ? stream.m_readBitPos : stream.m_bitPos;
    stream.m_rwSwitchCache = stream.m_readWriteSwitch;
    stream.m_lengthCache   = stream.m_highWaterBits;

    if (!stageBWriteData(worldSnap, task, action, ghost, flag, verifyEnabled))
    {
        stream.setPosition(*reinterpret_cast<io::BSPosition*>(&stream.m_dword3C));
        stream.m_byte38        = false;
        stream.m_errorCode     = 0;
        stream.m_error         = false;
        stream.m_highWaterBits = stream.m_lengthCache;
        return false;
    }
    stream.m_byte38 = false;

    GhostManager::GhostSaveBufferEntry saved;
    saved.m_action        = static_cast<int>(action);
    saved.m_ghostId       = ghost->m_ghostId;
    saved.m_networkableId = ghost->m_networkableId;
    for (int i = 0; i < 12; ++i)
        saved.m_state[i] = ghost->m_stateMask[i];
    entry.m_buffer->push_back(saved);

    for (int i = 0; i < 12; ++i)
        ghost->m_stateMask[i] = 0;

    ++gm->m_ghostsThisPacket;
    return true;
}

// Stage B parallel ghost packing (bit-exact mirror of GhostManager::transmit).
void stageBGhostTransmit(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    bool useLegacyGhostsInStageC,
    bool verifyEnabled)
{
    GhostManager* const gm      = task.ghostManager;
    io::BitStream* const stream = task.bitStream;

    stream->m_bitLimit = task.ghostBitLimit;

    const unsigned int startLength = stream->m_highWaterBits;

    io::BSPosition mark;
    mark.m_unknown00       = 0;
    mark.m_unknown08       = 0;
    mark.m_readWriteSwitch = stream->m_readWriteSwitch;
    mark.m_position        = stream->getBitPosition();
    stream->m_positionCache = mark.m_position;
    stream->m_byte38        = true;
    stream->m_rwSwitchCache = stream->m_readWriteSwitch;
    stream->m_lengthCache   = stream->m_highWaterBits;

    gm->m_ghostsThisPacket = 0;

    if (!stream->skipBits(42))
    {
        stream->setPosition(*reinterpret_cast<io::BSPosition*>(&stream->m_dword3C));
        stream->m_byte38        = false;
        stream->m_errorCode     = 0;
        stream->m_highWaterBits = stream->m_lengthCache;
        stream->m_error         = false;
        const unsigned int zero = 0;
        stream->writeBits(&zero, 1);

        task.conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[0]  = 0;
        task.conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[1] += 0;
        task.txOk = (gm->getError() == 0);
        if (task.txOk && stream->hasError())
        {
            stream->clearError();
            task.txOk = false;
        }
        task.ghostTransmitDone = true;
        return;
    }
    stream->m_byte38 = false;

    if (gm->m_mode == 0)
        gm->m_capacity = (task.effectiveMaxGhosts > 0) ? task.effectiveMaxGhosts : 40;

    int remaining = gm->m_capacity;

    GhostManager::SaveListEntry entry;
    entry.m_buffer              = gm->allocGhostSaveBuffer(gm->m_capacity);
    entry.m_controlObjNetId     = 0xFFFF;
    entry.m_actionBufferEntries = 0;
    entry.m_baseLineIndex       = 0;

    bool hasControlState = false;
    gm->m_baseLineSlotThisPacket = -1;

    if (gm->m_updateControlObject && gm->m_actionBufferEntries != -1)
    {
        stream->resetCompressionVector();
        if (task.hasControlToWrite)
            hasControlState = stageBWriteControlObjectState(worldSnap, task, verifyEnabled);
        else if (useLegacyGhostsInStageC)
            hasControlState = gm->writeControlObjectState(stream);

        if (hasControlState)
        {
            entry.m_controlObjNetId     = gm->m_controlObjectNetworkableId;
            entry.m_actionBufferEntries = gm->m_actionBufferEntries;
            entry.m_baseLineIndex       = gm->m_baseLineSlotThisPacket;
        }
        gm->m_lastSentActionBufferEntries = gm->m_actionBufferEntries;
        gm->m_actionBufferEntries         = -1;
    }

    while (!gm->m_resendQueue.empty())
    {
        const GhostManager::ResendQueueEntry lost = gm->m_resendQueue.front();
        gm->m_resendQueue.pop_front();

        GhostObject replayed;
        replayed.m_ghostableState = lost.m_ghostableState;
        replayed.m_networkableId  = lost.m_networkableId;

        const bool ok = useLegacyGhostsInStageC
            ? gm->sendData(*stream, static_cast<GhostAction>(3), &replayed, false, entry)
            : stageBSendData(worldSnap, task, static_cast<GhostAction>(3), &replayed, false, entry, verifyEnabled);
        if (!ok)
            break;
    }

    while (!gm->m_ghostPrioHeap.empty() && remaining > 0)
    {
        GhostObject* const ghost = gm->m_ghostPrioHeap.front();
        std::pop_heap(gm->m_ghostPrioHeap.begin(), gm->m_ghostPrioHeap.end(), GhostLessComp());
        gm->m_ghostPrioHeap.pop_back();

        const bool ok = useLegacyGhostsInStageC
            ? gm->sendData(*stream, static_cast<GhostAction>(ghost->m_ghostableState), ghost, false, entry)
            : stageBSendData(worldSnap, task, static_cast<GhostAction>(ghost->m_ghostableState), ghost, false, entry, verifyEnabled);

        if (ok)
        {
            ++ghost->m_byte51;
            ghost->m_accumulatedPriority = 0.0f;
        }
        else if ((stream->m_bitLimit - stream->m_highWaterBits) <= 0x17u)
        {
            break;
        }
        --remaining;
    }

    if (hasControlState || gm->m_ghostsThisPacket)
    {
        reinterpret_cast<unsigned int*>(&task.status)[7] |= (1u << task.ghostStreamMgrId);
        gm->m_saveList.push_back(entry);

        stream->setWritePosition(mark);
        const unsigned int one = 1;
        stream->writeBits(&one, 1);
        stream->writeBits(&gm->m_tickCount, 32);
        const unsigned int ghosts = static_cast<unsigned int>(gm->m_ghostsThisPacket);
        stream->writeBits(&ghosts, 8);
        const unsigned int control = hasControlState ? 1u : 0u;
        stream->writeBits(&control, 1);

        if (stream->m_canResetWritePosition)
        {
            stream->m_canResetWritePosition = false;
            stream->setPosition(*reinterpret_cast<io::BSPosition*>(&stream->m_dword24));
        }
    }
    else
    {
        stream->setPosition(mark);
        stream->setBitSize(startLength);
        const unsigned int zero = 0;
        stream->writeBits(&zero, 1);
        gm->freeGhostSaveBuffer(entry.m_buffer);
    }

    reinterpret_cast<unsigned int*>(&task.status)[6] = static_cast<unsigned int>(gm->m_ghostsThisPacket);
    gm->removeOldGhosts(false);
    stream->resetCompressionVector();

    const unsigned int written = stream->m_highWaterBits - startLength;
    gm->m_writeBits = written;

    const unsigned int bytes = written >> 3;
    task.conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[0]  = bytes;
    task.conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[1] += bytes;

    if (gm->getError())
    {
        task.txOk = false;
    }
    else if (stream->hasError())
    {
        stream->clearError();
        task.txOk = false;
    }
    else
    {
        task.txOk = true;
    }
    task.ghostTransmitDone = true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Stage A: Main-thread per-connection snapshot & pre-ghost stream packing
// ---------------------------------------------------------------------------

void NetWorkerPool::prepareConnectionStageA(
    const WorldNetSnapshot& worldSnap,
    ServerConnection* conn,
    bool applyStateMasks,
    bool dueToSend,
    bool shouldScope,
    float viewRange,
    float hysteresis,
    unsigned int tickCount,
    int frameCount,
    int effectiveMaxPacketBytes,
    int effectiveMaxGhosts,
    ConnectionStageTask& outTask)
{
    outTask.resetForTick();
    outTask.conn                    = conn;
    outTask.ghostManager            = conn ? conn->m_ghostManager : 0;
    outTask.bitStream               = conn ? conn->m_bitStream : 0;
    outTask.applyStateMasks         = applyStateMasks;
    outTask.dueToSend               = dueToSend;
    outTask.shouldScope             = shouldScope;
    outTask.viewRange               = viewRange;
    outTask.hysteresis              = hysteresis;
    outTask.tickCount               = tickCount;
    outTask.effectiveMaxPacketBytes = effectiveMaxPacketBytes;
    outTask.effectiveMaxGhosts      = effectiveMaxGhosts;

    if (!conn || !dueToSend)
        return;

    outTask.recording       = conn->getIsRecordingConnection();
    outTask.connectionState = conn->m_connectionState;
    outTask.bReset          = conn->m_bReset;

    if (outTask.recording)
    {
        outTask.serialFallback = true;
        return;
    }

    if (conn->m_sentData)
    {
        outTask.skipTransmitBackpressure = true;
        outTask.txOk = true;
    }

    // 1. Update control object & capture camera/commander state on main thread.
    //    Runs even under backpressure (m_sentData == true), matching vanilla GameServer.
    if (conn->m_connectionState == 4 && conn->m_bReset)
    {
        conn->updatePlayerControlObject(frameCount);

        if (shouldScope)
        {
            world::IPlayer* const player = conn->getPlayer();
            if (player && player->getCamera())
            {
                outTask.hasCamera    = true;
                outTask.playerId     = player->getId();
                outTask.teamId       = player->getTeam();
                outTask.isCommander  = player->getIsCommander();
                outTask.fovThreshold = player->getFov() * 0.5f * 1.3333334f;
                outTask.viewTransform = player->getCamera()->getTransformation(0.0f);

                if (player->getOverheadCameraTopDownView() && squadManager)
                {
                    ISquadManager* const squads = static_cast<ISquadManager*>(squadManager);
                    world::IObject* const cam = static_cast<world::IObject*>(
                        squads->getCommanderCam(outTask.teamId));
                    if (cam)
                    {
                        world::ICameraObject* const camObj = static_cast<world::ICameraObject*>(
                            cam->queryInterface(world::IID_ICameraObject));
                        if (camObj)
                            outTask.viewTransform = camObj->getTransformation(0.0f);
                    }
                }

                if (worldSnap.m_hasCommanderTools && (outTask.teamId == 1 || outTask.teamId == 2))
                {
                    outTask.hasSatScan = outTask.isCommander && worldSnap.m_teamSatelliteScanActive[outTask.teamId];
                    outTask.hasUav     = !outTask.hasSatScan && worldSnap.m_teamUavActive[outTask.teamId];
                    outTask.uavCentre  = worldSnap.m_teamUavCentre[outTask.teamId];
                    outTask.uavRadius  = worldSnap.m_uavRadius;
                }
            }
        }
    }

    if (outTask.skipTransmitBackpressure)
        return;

    // 2. Pre-ghost portion of ClientConnection::transmitMsgs() (PlayerActionManager + GameEventManager).
    if (conn->m_player && !conn->m_player->getIsRemote() && io::networkManager)
    {
        const int interpTime = io::networkManager->getInterpolationTime();
        const float latency  = world::LatencyCompensation::getLocalLatency(conn->m_player);
        conn->m_playerActionManager->m_playerInterpolationTime =
            static_cast<int>(latency * 1000.0f) + interpTime;
    }

    unsigned int* const s = reinterpret_cast<unsigned int*>(&outTask.status);
    s[3] = 0;
    s[4] = 0;
    s[5] = 0;
    s[6] = 0;
    s[7] = 0;

    if (conn->m_demoMode != 1 && effectiveMaxPacketBytes > 0)
        conn->m_packetSizeOverride = effectiveMaxPacketBytes;

    conn->newPacket(conn->m_packetSizeOverride);
    outTask.bitStream = conn->m_bitStream;

    s[0]  = static_cast<unsigned int>(conn->m_dword78);
    s[10] = static_cast<unsigned int>(conn->m_dword78);

    conn->m_dataBlockManager->update();
    conn->m_stringManager->update();

    const unsigned int count    = static_cast<unsigned int>(conn->m_streamManagers.size());
    const unsigned int reserved = conn->m_bitStream->m_bitLimit - count;

    for (unsigned int i = 0; i < count; ++i)
    {
        IStreamManager* const mgr = conn->m_streamManagers[i];
        if (mgr == reinterpret_cast<IStreamManager*>(conn->m_ghostManager))
        {
            outTask.ghostBitLimit    = reserved + i;
            outTask.ghostStreamMgrId = mgr->getStreamManagerId();
            continue;
        }

        conn->m_bitStream->m_bitLimit = reserved + i;
        const unsigned int bits = mgr->transmit(conn->m_bitStream, outTask.status, 0);
        if (mgr->getError())
        {
            outTask.preGhostOk = false;
            outTask.txOk       = false;
            return;
        }
        const unsigned int bytes = bits >> 3;
        conn->m_streamManagerStats[mgr->getStreamManagerId()].m_opaque[0]  = bytes;
        conn->m_streamManagerStats[mgr->getStreamManagerId()].m_opaque[1] += bytes;
    }

    // 3. Control-object Stage A capture (if control object is supported by ConstNetEncoders).
    GhostManager* const gm = conn->m_ghostManager;
    if (gm && gm->m_updateControlObject && gm->m_actionBufferEntries != -1 && io::networkManager)
    {
        const unsigned short controlNetId = gm->m_controlObjectNetworkableId;
        const unsigned int bucket         = controlNetId % gm->m_ghostables.m_maxSize;
        if (gm->m_ghostables.exists(controlNetId, bucket))
        {
            io::NetworkableDescriptor* const controlDesc =
                io::networkManager->getDescriptor(controlNetId);
            const unsigned short parachuteNetId = gm->m_parachuteSoldierNetId;
            io::NetworkableDescriptor* const parachuteDesc =
                (parachuteNetId != 0) ? io::networkManager->getDescriptor(parachuteNetId) : 0;

            const bool supported =
                controlDesc != 0
                && worldSnap.isDescriptorFullySupported(controlDesc)
                && (parachuteNetId == 0
                    || (parachuteDesc != 0 && worldSnap.isDescriptorFullySupported(parachuteDesc)));

            if (!supported)
            {
                outTask.serialFallback = true;
                return;
            }

            outTask.controlObjectNetId    = controlNetId;
            outTask.parachuteSoldierNetId = parachuteNetId;
            outTask.controlDescriptor     = controlDesc;
            outTask.parachuteDescriptor   = parachuteDesc;
            outTask.controlHasNetObj      =
                (!controlDesc->properties.empty()
                 && !controlDesc->properties[0].networkables.empty()
                 && controlDesc->properties[0].networkables[0]->m_networkableObject != 0);

            world::IObject* targetObj = io::networkManager->getObject(controlNetId);
            world::IObject* const soldier = getSoldierForControlSnap(targetObj);
            outTask.controlIsSoldier = (soldier != 0);
            if (soldier)
            {
                targetObj = soldier;
                world::ISoldier* const soldierIf = static_cast<world::ISoldier*>(
                    soldier->queryInterface(world::IID_ISoldier));
                world::IObject* const kit =
                    soldierIf ? reinterpret_cast<world::IObject*>(soldierIf->getKit()) : 0;
                if (kit && kit->getNetworkable())
                {
                    outTask.controlHasKit   = true;
                    outTask.controlKitNetId = kit->getNetworkable()->m_networkableId;
                }
            }

            outTask.controlNeedsFreshBaseline = !(gm->m_baseLineIndex != -1 && gm->m_deltasUntilBaseLine > 0);
            if (outTask.controlNeedsFreshBaseline)
            {
                bool baseOk = io::networkManager->getBaseLine(outTask.freshBaseLine, controlDesc, true);
                if (baseOk && parachuteNetId != 0)
                    baseOk = io::networkManager->getBaseLine(outTask.freshBaseLine, parachuteDesc, false);
                if (!baseOk)
                {
                    outTask.serialFallback = true;
                    return;
                }
            }

            // Capture Player::getStateSync on the main thread (consumes hit indicator / shake counters once).
            io::BitStream syncStream(outTask.stateSyncBuf, sizeof(outTask.stateSyncBuf));
            if (gm->m_player)
                gm->m_player->getStateSync(&syncStream, targetObj);
            outTask.stateSyncBits     = syncStream.getBitPosition();
            outTask.stateSyncError    = syncStream.hasError() || (outTask.stateSyncBits > 0x3FFu);
            if (syncStream.hasError())
                syncStream.clearError();
            outTask.stateSyncCaptured = true;
            outTask.hasControlToWrite = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage B: Lock-free worker-thread execution (state masks, scope, heap, pack)
// ---------------------------------------------------------------------------

void NetWorkerPool::executeConnectionStageB(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    bool profEnabled,
    bool verifyEnabled)
{
    GhostManager* const gm = task.ghostManager;
    if (!gm)
        return;

    // 1. Apply per-tick state masks to every connection (lock-free, exclusive per-connection).
    if (task.applyStateMasks && !worldSnap.m_updatedStateMasks.empty())
        gm->updateStateMasks(worldSnap.m_updatedStateMasks);

    if (!task.dueToSend || task.recording)
        return;

    // 2. Scope & build priority heap (runs even under backpressure, matching vanilla GameServer).
    if (task.connectionState == 4 && task.bReset)
    {
        if (task.shouldScope)
        {
            const ServerTickProfiler::TimePoint tScope = profEnabled
                ? ServerTickProfiler::now()
                : ServerTickProfiler::TimePoint();
            stageBScopeConnection(worldSnap, task);
            if (profEnabled)
                task.scopeUs = ServerTickProfiler::elapsedUs(tScope, ServerTickProfiler::now());
        }

        const ServerTickProfiler::TimePoint tHeap = profEnabled
            ? ServerTickProfiler::now()
            : ServerTickProfiler::TimePoint();
        gm->update(task.tickCount);
        if (profEnabled)
            task.packUs += ServerTickProfiler::elapsedUs(tHeap, ServerTickProfiler::now());
    }

    if (task.skipTransmitBackpressure || !task.preGhostOk || task.serialFallback)
        return;

    // 3. Preflight hybrid-fallback check BEFORE touching BitStream or popping queues/heap.
    for (std::size_t i = 0; i < gm->m_ghostPrioHeap.size(); ++i)
    {
        const GhostObject* const ghost = gm->m_ghostPrioHeap[i];
        if (ghost && ghost->m_ghostableState == 1)
        {
            if (!worldSnap.isDescriptorFullySupported(ghost->m_descriptor))
            {
                task.serialFallback = true;
                return;
            }
        }
    }

    // 4. Parallel ghost packing into conn->m_bitStream via ConstNetEncoders.
    const ServerTickProfiler::TimePoint tPack = profEnabled
        ? ServerTickProfiler::now()
        : ServerTickProfiler::TimePoint();

    stageBGhostTransmit(worldSnap, task, false, verifyEnabled);

    if (profEnabled)
        task.packUs += ServerTickProfiler::elapsedUs(tPack, ServerTickProfiler::now());
}

void NetWorkerPool::runConnectionsStageB(
    const WorldNetSnapshot& worldSnap,
    std::vector<ConnectionStageTask>& tasks,
    bool profEnabled,
    bool verifyEnabled)
{
    runParallel(tasks.size(), [&](std::size_t idx) {
        executeConnectionStageB(worldSnap, tasks[idx], profEnabled, verifyEnabled);
    });
}

// ---------------------------------------------------------------------------
// Stage C: Main-thread fallback, PacketStatus finalization, and net-mt-verify
// ---------------------------------------------------------------------------

unsigned int NetWorkerPool::finishConnectionStageC(
    const WorldNetSnapshot& worldSnap,
    ConnectionStageTask& task,
    bool verifyEnabled)
{
    unsigned int verifyMismatches = 0;

    if (!task.conn || !task.dueToSend || task.skipTransmitBackpressure || task.recording)
        return 0;

    ServerConnection* const conn = task.conn;
    GhostManager* const gm       = task.ghostManager;

    // 1. Run serial fallback on main thread if Stage B deferred this connection.
    if (task.preGhostOk && !task.ghostTransmitDone && gm && conn->m_bitStream)
    {
        if (task.stateSyncCaptured)
        {
            // Control stateSync was already captured in Stage A: write it once and use legacy sendData for ghosts.
            stageBGhostTransmit(worldSnap, task, true, false);
        }
        else
        {
            conn->m_bitStream->m_bitLimit = task.ghostBitLimit;
            const unsigned int bits = gm->transmit(conn->m_bitStream, task.status, 0);
            if (gm->getError())
            {
                task.txOk = false;
            }
            else
            {
                const unsigned int bytes = bits >> 3;
                conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[0]  = bytes;
                conn->m_streamManagerStats[task.ghostStreamMgrId].m_opaque[1] += bytes;
                if (conn->m_bitStream->hasError())
                {
                    conn->m_bitStream->clearError();
                    task.txOk = false;
                }
                else
                {
                    task.txOk = true;
                }
            }
            task.ghostTransmitDone = true;
        }
    }

    // 2. Finalize ClientConnection::m_packetStatuses / m_dword78 on main thread ONLY.
    if (task.txOk)
    {
        if (task.status.m_streamManagerMask != 0)
        {
            *reinterpret_cast<double*>(&task.status.m_opaque00[4]) = g_system->getExactTime();
            conn->m_packetStatuses.push_back(task.status);
            ++conn->m_dword78;
        }
        else
        {
            conn->newPacket(conn->m_packetSizeOverride);
        }
    }

    // 3. Optional net-mt-verify bit-exact comparison against live INetworkable::getNetUpdate().
    if (verifyEnabled && !task.verifyEntries.empty())
    {
        for (std::size_t i = 0; i < task.verifyEntries.size(); ++i)
        {
            const VerifyDescriptorEntry& ve = task.verifyEntries[i];
            if (!ve.descriptor)
                continue;
            const int targetPrecision = ve.ghostTier ? ve.precision : static_cast<int>(io::StatePrecisionFull);
            io::BaseLineData runningBase = ve.baselineBefore;

            for (std::size_t p = 0; p < ve.descriptor->properties.size(); ++p)
            {
                if (ve.hasMask && !ve.isDelta && p > 11)
                    break;
                const io::UniqueNetProperty& prop = ve.descriptor->properties[p];
                if (prop.networkables.empty() || !prop.networkables[0])
                    continue;
                io::INetworkable* const liveNet = prop.networkables[0];
                const NetRecordSnap* const recSnap = worldSnap.findRecord(liveNet);
                if (!recSnap || recSnap->type == NetEncType::Unsupported)
                    continue;

                unsigned int propMask;
                if (ve.hasMask)
                    propMask = ve.ghostTier ? (ve.mask.m_opaque[p] & recSnap->ghostStateMask) : ve.mask.m_opaque[p];
                else
                    propMask = ve.ghostTier ? recSnap->ghostStateMask : 0xFFFFFFFFu;

                const Vec3* compPtr = ve.hasCompVec ? &ve.compVec : 0;
                io::BaseLineData* basePtr = ve.isDelta ? &runningBase : 0;

                if (!ConstNetEncoders::verifyBitExactAgainstLegacy(
                        worldSnap, *recSnap, liveNet, compPtr, basePtr, propMask, targetPrecision))
                {
                    ++verifyMismatches;
                    std::fprintf(stderr,
                                 "[NetMT][VERIFY MISMATCH] conn=%d netId=%u type=%u mask=0x%X prec=%d delta=%d\n",
                                 conn->m_connectionId,
                                 static_cast<unsigned int>(recSnap->networkableId),
                                 static_cast<unsigned int>(recSnap->type),
                                 propMask, targetPrecision, ve.isDelta ? 1 : 0);
                }
            }
        }
    }

    return verifyMismatches;
}

#endif // NET_WORKER_POOL_CORE_ONLY

} // namespace netpipeline
} // namespace hfe
} // namespace dice
