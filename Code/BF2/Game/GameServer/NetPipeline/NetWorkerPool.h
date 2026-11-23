// [CHOSEN, not recovered] Persistent C++14 worker thread pool and Stage A/B/C multi-threaded
// network snapshot pipeline for 32v32 dedicated servers.
// Lazily spawns threads only when net-mt-enable != 0 (0 threads when disabled = 100% vanilla).
// Zero ABI, vtable, or class-layout changes on any existing engine class.

#ifndef GAME_GAMESERVER_NETPIPELINE_NETWORKERPOOL_H
#define GAME_GAMESERVER_NETPIPELINE_NETWORKERPOOL_H

#include "WorldNetSnapshot.h"
#include "ConstNetEncoders.h"
#include "../../Common/ClientConnection.h"
#include "../../../Math/Mat4.h"
#include "../../../Math/Vec3.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {

class ServerConnection;
class GhostManager;

namespace netpipeline {

// Verification record captured in Stage B when net-mt-verify != 0 and checked on main thread in Stage C.
struct VerifyDescriptorEntry
{
    const io::NetworkableDescriptor* descriptor;
    io::NetworkableStateMask         mask;
    bool                             hasMask;
    bool                             ghostTier;
    int                              precision;
    bool                             hasCompVec;
    Vec3                             compVec;
    bool                             isDelta;
    io::BaseLineData                 baselineBefore;
};

// Per-connection Stage A -> Stage B -> Stage C workspace.
// Reused across ticks (capacity preserved, zero steady-state heap allocations).
struct ConnectionStageTask
{
    // Stage A inputs (immutable during Stage B)
    ServerConnection*                conn;
    GhostManager*                    ghostManager;
    io::BitStream*                   bitStream;
    bool                             applyStateMasks;
    bool                             dueToSend;
    bool                             skipTransmitBackpressure;
    bool                             recording;
    int                              connectionState;
    bool                             bReset;
    bool                             shouldScope;
    bool                             hasCamera;
    Mat4                             viewTransform;
    float                            fovThreshold;
    float                            viewRange;
    float                            hysteresis;
    int                              playerId;
    int                              teamId;
    bool                             isCommander;
    bool                             hasSatScan;
    bool                             hasUav;
    Vec3                             uavCentre;
    float                            uavRadius;
    unsigned int                     tickCount;
    int                              effectiveMaxPacketBytes;
    int                              effectiveMaxGhosts;

    // Pre-captured control-object state (main-thread safe, prevents double getStateSync consumption)
    bool                             hasControlToWrite;
    bool                             stateSyncCaptured;
    unsigned short                   controlObjectNetId;
    unsigned short                   parachuteSoldierNetId;
    bool                             controlHasNetObj;
    bool                             controlIsSoldier;
    bool                             controlHasKit;
    unsigned short                   controlKitNetId;
    const io::NetworkableDescriptor* controlDescriptor;
    const io::NetworkableDescriptor* parachuteDescriptor;
    bool                             controlNeedsFreshBaseline;
    io::BaseLineData                 freshBaseLine;
    unsigned char                    stateSyncBuf[136];
    unsigned int                     stateSyncBits;

    // Pre-ghost stream state (PlayerActionManager + GameEventManager already packed in Stage A)
    PacketStatus                     status;
    unsigned int                     ghostBitLimit;
    int                              ghostStreamMgrId;
    bool                             preGhostOk;

    // Stage B outputs (written exclusively by the worker owning this connection)
    bool                             serialFallback;
    bool                             txOk;
    bool                             ghostTransmitDone;
    int                              scopeUs;
    int                              packUs;
    WorldNetSnapshot::TypeWriteBitStats writeBitStats;
    unsigned int                     didNotFitCount;
    unsigned int                     missingUpdatesCount;
    std::vector<VerifyDescriptorEntry> verifyEntries;

    ConnectionStageTask()
    {
        resetForTick();
    }

    void resetForTick()
    {
        conn                      = 0;
        ghostManager              = 0;
        bitStream                 = 0;
        applyStateMasks           = false;
        dueToSend                 = false;
        skipTransmitBackpressure  = false;
        recording                 = false;
        connectionState           = 0;
        bReset                    = false;
        shouldScope               = false;
        hasCamera                 = false;
        std::memset(&viewTransform, 0, sizeof(viewTransform));
        viewTransform.m[0]        = 1.0f;
        viewTransform.m[5]        = 1.0f;
        viewTransform.m[10]       = 1.0f;
        viewTransform.m[15]       = 1.0f;
        fovThreshold              = 0.0f;
        viewRange                 = 500.0f;
        hysteresis                = 0.0f;
        playerId                  = -1;
        teamId                    = 0;
        isCommander               = false;
        hasSatScan                = false;
        hasUav                    = false;
        uavCentre.x               = 0.0f;
        uavCentre.y               = 0.0f;
        uavCentre.z               = 0.0f;
        uavRadius                 = 0.0f;
        tickCount                 = 0;
        effectiveMaxPacketBytes   = 0;
        effectiveMaxGhosts        = 0;

        hasControlToWrite         = false;
        stateSyncCaptured         = false;
        controlObjectNetId        = 0;
        parachuteSoldierNetId     = 0;
        controlHasNetObj          = false;
        controlIsSoldier          = false;
        controlHasKit             = false;
        controlKitNetId           = 0;
        controlDescriptor         = 0;
        parachuteDescriptor       = 0;
        controlNeedsFreshBaseline = false;
        std::memset(&freshBaseLine, 0, sizeof(freshBaseLine));
        std::memset(stateSyncBuf, 0, sizeof(stateSyncBuf));
        stateSyncBits             = 0;

        std::memset(&status, 0, sizeof(status));
        ghostBitLimit             = 0;
        ghostStreamMgrId          = 2;
        preGhostOk                = true;

        serialFallback            = false;
        txOk                      = true;
        ghostTransmitDone         = false;
        scopeUs                   = 0;
        packUs                    = 0;
        writeBitStats             = WorldNetSnapshot::TypeWriteBitStats();
        didNotFitCount            = 0;
        missingUpdatesCount       = 0;
        verifyEntries.clear();
    }
};

class NetWorkerPool
{
public:
    NetWorkerPool();
    ~NetWorkerPool();

    static NetWorkerPool& instance();

    // CVar accessors (lazy function-local statics, all default 0 / 0.0f).
    static bool  isEnabled();
    static int   getConfiguredThreadCount();
    static bool  isVerifyEnabled();
    static float getScopeHysteresis();

    // Lazily starts worker threads (0 threads created if never called).
    void ensureStarted(int requestedThreads = 0);

    // Cleanly joins and destroys worker threads. Safe to call when not started or multiple times.
    void shutdown();

    std::size_t workerCount() const;

    // Zero-allocation dynamic work-stealing parallel dispatch across worker threads + calling thread.
    template <typename Fn>
    void runParallel(std::size_t count, Fn fn)
    {
        if (count == 0)
            return;
        if (count == 1)
        {
            fn(0);
            return;
        }
        ensureStarted();
        if (m_workers.empty())
        {
            for (std::size_t i = 0; i < count; ++i)
                fn(i);
            return;
        }
        struct Trampoline
        {
            Fn* f;
            static void invoke(void* ctx, std::size_t idx)
            {
                (*static_cast<Trampoline*>(ctx)->f)(idx);
            }
        };
        Trampoline tramp = { &fn };
        dispatchBatch(count, &Trampoline::invoke, &tramp);
    }

    // Stage A: main-thread per-connection snapshot + pre-ghost stream packing (Actions + Events).
    static void prepareConnectionStageA(
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
        ConnectionStageTask& outTask);

    // Stage B: lock-free worker-thread execution (state masks, spatial scoping, heap update, ghost packing).
    static void executeConnectionStageB(
        const WorldNetSnapshot& worldSnap,
        ConnectionStageTask& task,
        bool profEnabled,
        bool verifyEnabled);

    // Stage C: main-thread fallback execution (if serialFallback), PacketStatus finalization, and optional verify.
    static unsigned int finishConnectionStageC(
        const WorldNetSnapshot& worldSnap,
        ConnectionStageTask& task,
        bool verifyEnabled);

    // Convenience driver running executeConnectionStageB across all tasks in parallel.
    void runConnectionsStageB(
        const WorldNetSnapshot& worldSnap,
        std::vector<ConnectionStageTask>& tasks,
        bool profEnabled,
        bool verifyEnabled);

private:
    typedef void (*TaskThunk)(void*, std::size_t);

    void dispatchBatch(std::size_t count, TaskThunk thunk, void* ctx);
    void workerLoop();

    NetWorkerPool(const NetWorkerPool&);
    NetWorkerPool& operator=(const NetWorkerPool&);

    std::vector<std::thread> m_workers;
    mutable std::mutex       m_mutex;
    std::condition_variable  m_workCv;
    std::condition_variable  m_doneCv;
    bool                     m_stopping;
    unsigned long long       m_generation;
    std::size_t              m_activeWorkers;
    std::size_t              m_taskCount;
    TaskThunk                m_taskThunk;
    void*                    m_taskCtx;
    std::atomic<std::size_t> m_nextIndex;
};

} // namespace netpipeline
} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_NETPIPELINE_NETWORKERPOOL_H
