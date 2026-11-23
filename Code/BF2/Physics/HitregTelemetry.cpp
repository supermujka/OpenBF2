// [CHOSEN, not recovered] Buffered CSV telemetry for Shadow A/B and pingFactor calibration.

#include "HitregTelemetry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "IPhysicsNode.h"
#include "LatencyCompensation.h"
#include "SoldierPhysicsNode.h"
#include "../Animation/BoneAnimation/Skeleton.h"
#include "../Core/Ids.h"
#include "../Core/System.h"
#include "../Game/IGameLogic.h"
#include "../Game/Objects/IAnimatedBundle.h"
#include "../Game/Objects/ICameraObject.h"
#include "../Game/Objects/IObject.h"
#include "../Game/Objects/IObjectTemplate.h"
#include "../Game/Objects/IPlayer.h"
#include "../Game/Objects/ObjectLayoutAccess.h"
#include "../Game/Objects/Game/Soldier.h"
#include "../Game/Utility/Vars.h"
#include "../Math/Mat4.h"
#include "../Math/Vec3.h"
#include "../Scene/Object/ObjectUtil.h"

namespace dice {
namespace hfe {
namespace world {

namespace {

int*         const g_telemetryLevel  = Vars::getInt("phy-latencyComp-telemetry", 2);
const char*  const kDefaultTelemetryFile = "openbf2_hitreg.csv";
const bool         g_telemetryFileRegistered =
    (Vars::getString("phy-latencyComp-telemetryFile", kDefaultTelemetryFile) != 0);
int*         const g_telemetryFlush  = Vars::getInt("phy-latencyComp-telemetryFlushTicks", 30);
int*         const g_telemetrySplitMatches = Vars::getInt("phy-latencyComp-telemetrySplitMatches", 1);
int*         const g_netServerSendRate = Vars::getInt("net-serverSendRate", 30);

const float kServerTickSec = 0.033333335f;

struct PlayerNetState
{
    std::uint64_t lossMask;
    int           lossCount;
    int           pingRing[32];
    int           pingRingWrite;
    int           pingRingCount;
    int           lastRawPingMs;
    int           lastQueueDepth;
    int           lastReplayCount;
    std::uint32_t replayMask30;

    // [CHOSEN, not recovered] Hitreg V3 Phase 2 forensic counters & send-cadence tracker.
    std::uint64_t fireHeldCum;
    std::uint64_t shotsFiredCum;
    std::uint64_t fireDroppedCum;
    std::uint64_t earlyNoProbeCum;
    int           lastSnapTick;
    int           lastSnapGapTicks;
    int           lastDrainDropTick;

    PlayerNetState()
        : lossMask(0)
        , lossCount(0)
        , pingRingWrite(0)
        , pingRingCount(0)
        , lastRawPingMs(0)
        , lastQueueDepth(0)
        , lastReplayCount(0)
        , replayMask30(0)
        , fireHeldCum(0)
        , shotsFiredCum(0)
        , fireDroppedCum(0)
        , earlyNoProbeCum(0)
        , lastSnapTick(-1)
        , lastSnapGapTicks(-1)
        , lastDrainDropTick(-1)
    {
        std::memset(pingRing, 0, sizeof(pingRing));
    }
};

struct ShotRecord
{
    std::uint64_t shotId;
    int           fireTick;
    double        fireTimeSec;
    const IPlayer* ownerPtr;
    int           shooterId;
    std::string   shooterName;
    std::string   weaponName;
    Vec3          startPos;
    float         muzzleSpeed;
    float         v1RewindMs;
    float         vanFireRewindMs;
    unsigned int  pingMs;
    int           rawPingMs;
    float         rttJitterMs;
    float         pktLossDnPct;
    unsigned int  rawInterpMs;
    unsigned int  interpMs;
    int           queueDepth;
    float         queueDelayMs;
    int           replayCount;
    int           replays30t;

    // [CHOSEN, not recovered] Hitreg V3 Phase 2 forensic shot snapshot.
    Vec3          aimDir;
    Vec3          bulletDir;
    bool          hasAimDir;
    bool          hasBulletDir;
    bool          hasDeviation;
    float         devDeg;
    float         devConeDeg;
    bool          hasMuzzleOff;
    float         muzzleOffM;
    bool          fireEmitted;
    bool          hadSoldierProbe;
    std::uint64_t shHeldCum;
    std::uint64_t shShotsCum;
    std::uint64_t shFdropCum;
    std::uint64_t shEarlyNpCum;
    int           sndGapTicks;
    int           qDropAgeT;

    // Pending soldier contact arbitration on current tick.
    int           pendingTick;
    IObject*      pendingTarget;
    int           pendingTargetId;
    std::string   pendingTargetName;
    int           pendingBone;
    int           pendingPart;
    float         pendingDepthSq;
    Vec3          pendingRayStart;
    Vec3          pendingHitPos;
    bool          hasShooterEye;
    Vec3          shooterCenterPos;
    Vec3          shooterEyePos;

    // Damage trace for current collision.
    bool          hasDmgCalc;
    int           matA;
    int           matB;
    float         baseDmg;
    float         matMod;
    float         angleMod;
    bool          hasServerDmg;
    float         finalDmg;
    float         hpBefore;
    float         hpAfter;
    bool          killed;

    ShotRecord()
        : shotId(0)
        , fireTick(0)
        , fireTimeSec(0.0)
        , ownerPtr(0)
        , shooterId(-1)
        , muzzleSpeed(0.0f)
        , v1RewindMs(0.0f)
        , vanFireRewindMs(0.0f)
        , pingMs(0)
        , rawPingMs(0)
        , rttJitterMs(0.0f)
        , pktLossDnPct(0.0f)
        , rawInterpMs(0)
        , interpMs(0)
        , queueDepth(0)
        , queueDelayMs(0.0f)
        , replayCount(0)
        , replays30t(0)
        , hasAimDir(false)
        , hasBulletDir(false)
        , hasDeviation(false)
        , devDeg(0.0f)
        , devConeDeg(0.0f)
        , hasMuzzleOff(false)
        , muzzleOffM(0.0f)
        , fireEmitted(false)
        , hadSoldierProbe(false)
        , shHeldCum(0)
        , shShotsCum(0)
        , shFdropCum(0)
        , shEarlyNpCum(0)
        , sndGapTicks(-1)
        , qDropAgeT(-1)
        , pendingTick(-1)
        , pendingTarget(0)
        , pendingTargetId(-1)
        , pendingBone(-1)
        , pendingPart(0)
        , pendingDepthSq(-1.0f)
        , hasShooterEye(false)
        , hasDmgCalc(false)
        , matA(0)
        , matB(0)
        , baseDmg(0.0f)
        , matMod(0.0f)
        , angleMod(0.0f)
        , hasServerDmg(false)
        , finalDmg(0.0f)
        , hpBefore(0.0f)
        , hpAfter(0.0f)
        , killed(false)
    {
        startPos.x = startPos.y = startPos.z = 0.0f;
        aimDir.x = aimDir.y = aimDir.z = 0.0f;
        bulletDir.x = bulletDir.y = bulletDir.z = 0.0f;
        pendingRayStart.x = pendingRayStart.y = pendingRayStart.z = 0.0f;
        pendingHitPos.x = pendingHitPos.y = pendingHitPos.z = 0.0f;
        shooterCenterPos.x = shooterCenterPos.y = shooterCenterPos.z = 0.0f;
        shooterEyePos.x = shooterEyePos.y = shooterEyePos.z = 0.0f;
    }
};

struct TelemetryLoggerState
{
    FILE*       file;
    std::string activePath;
    std::string activeBasePath;
    bool        activeSplitMatches;
    std::string currentMapName;
    int         matchSeq;
    std::string buffer;
    std::string lastConfigSig;
    int         lastFlushTick;
    std::uint64_t nextShotId;
    bool        atexitRegistered;
    int         tickUsRing[60];
    int         tickUsWrite;
    int         tickUsCount;

    TelemetryLoggerState()
        : file(0)
        , activeSplitMatches(false)
        , matchSeq(0)
        , lastFlushTick(0)
        , nextShotId(1)
        , atexitRegistered(false)
        , tickUsWrite(0)
        , tickUsCount(0)
    {
        std::memset(tickUsRing, 0, sizeof(tickUsRing));
        buffer.reserve(65536);
    }
};

std::unordered_map<const IPlayer*, PlayerNetState>& playerNetStates()
{
    static std::unordered_map<const IPlayer*, PlayerNetState>* const map =
        new std::unordered_map<const IPlayer*, PlayerNetState>();
    return *map;
}

std::unordered_map<const IObject*, ShotRecord>& shotRecords()
{
    static std::unordered_map<const IObject*, ShotRecord>* const map =
        new std::unordered_map<const IObject*, ShotRecord>();
    return *map;
}

TelemetryLoggerState& loggerState()
{
    static TelemetryLoggerState* const state = new TelemetryLoggerState();
    return *state;
}

const char* kCsvHeader =
    "event,tick,time_s,shot_id,weapon,shooter_id,shooter,tgt_id,tgt,"
    "ping_ms,raw_ping_ms,rtt_jitter_ms,pkt_loss_dn_pct,raw_interp_ms,interp_ms,"
    "q_depth,q_delay_ms,replay_cnt,replays_30t,q_target,q_drain_drops,tick_us_p50,tick_us_p99,"
    "act_rewind_ms,van_rewind_ms,shot_dist_m,flight_ticks,coarse_dist_m,flyby,"
    "tgt_alive,tgt_mandown,tgt_live_pose,tgt_act_pose,tgt_speed,tgt_ticks_since_reseed,"
    "act_bone_hist,act_head_shift_m,act_hit,act_bone,act_bone_name,act_part,"
    "act_miss_m,act_near_bone,act_dt_err_ms,"
    "van_hit,van_bone,van_part,van_miss_m,van_ring_unfilled,"
    "dev_deg,dev_cone_deg,muzzle_off_m,sh_held_cum,sh_shots_cum,sh_fdrop_cum,sh_early_np_cum,"
    "snd_gap_ticks,q_drop_age_t,"
    "aim_hit,aim_miss_m,miss_dy_m,miss_lat_m,tgt_back_dot,ring_uf,"
    "outcome,blocker,blocker_mat,override_margin_m,t_wall_m,t_soldier_m,obj_class,eye_clear,"
    "mat_a,mat_b,base_dmg,mat_mod,angle_mod,final_dmg,hp_before,hp_after,killed,hitmarker\n";

void flushBufferToDisk(bool forceFlush)
{
    TelemetryLoggerState& st = loggerState();
    if (!st.file)
        return;

    if (!st.buffer.empty())
    {
        std::fwrite(st.buffer.data(), 1, st.buffer.size(), st.file);
        st.buffer.clear();
    }
    if (forceFlush)
        std::fflush(st.file);
}

void closeTelemetryFile()
{
    TelemetryLoggerState& st = loggerState();
    if (st.file)
    {
        flushBufferToDisk(true);
        std::fclose(st.file);
        st.file = 0;
        st.activePath.clear();
        st.activeBasePath.clear();
        st.activeSplitMatches = false;
        st.lastConfigSig.clear();
    }
    st.buffer.clear();
    shotRecords().clear();
}

void onProcessExit()
{
    closeTelemetryFile();
}

std::string sanitizeFilenameToken(const char* raw)
{
    if (!raw || !*raw)
        return std::string();
    std::string out;
    out.reserve(32);
    for (const char* p = raw; *p && out.size() < 32u; ++p)
    {
        const char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
            || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
        {
            out.push_back(ch);
        }
        else if (ch == ' ')
        {
            out.push_back('_');
        }
    }
    return out;
}

std::string buildMatchFilePath(const std::string& basePath, const std::string& mapToken, int matchSeq)
{
    const std::size_t slashPos = basePath.find_last_of("/\\");
    const std::size_t dotPos = basePath.find_last_of('.');
    const bool hasExt =
        (dotPos != std::string::npos) && (slashPos == std::string::npos || dotPos > slashPos);
    const std::string stem = hasExt ? basePath.substr(0, dotPos) : basePath;
    const std::string ext = hasExt ? basePath.substr(dotPos) : std::string(".csv");

    char tsBuf[32] = "00000000_000000";
    const std::time_t now = std::time(0);
    std::tm tmBuf;
    std::memset(&tmBuf, 0, sizeof(tmBuf));
    bool haveTm = false;
#if defined(_MSC_VER)
    haveTm = (localtime_s(&tmBuf, &now) == 0);
#else
    haveTm = (localtime_r(&now, &tmBuf) != 0);
#endif
    if (haveTm)
        std::strftime(tsBuf, sizeof(tsBuf), "%Y%m%d_%H%M%S", &tmBuf);

    char suffix[96];
    if (mapToken.empty())
        std::snprintf(suffix, sizeof(suffix), "_%s_m%d", tsBuf, matchSeq);
    else
        std::snprintf(suffix, sizeof(suffix), "_%s_%s_m%d", tsBuf, mapToken.c_str(), matchSeq);
    return stem + suffix + ext;
}

std::string sanitizeField(const char* raw)
{
    if (!raw || !*raw)
        return std::string();
    std::string out;
    out.reserve(32);
    for (const char* p = raw; *p && out.size() < 64u; ++p)
    {
        const char ch = *p;
        if (ch == ',' || ch == '"' || ch == '\r' || ch == '\n')
            out.push_back('_');
        else
            out.push_back(ch);
    }
    return out;
}

int countCommas(const char* s)
{
    int count = 0;
    if (!s)
        return 0;
    for (const char* p = s; *p; ++p)
    {
        if (*p == ',')
            ++count;
    }
    return count;
}

int expectedHeaderCommas()
{
    static const int kCommas = countCommas(kCsvHeader);
    return kCommas;
}

void appendLine(const char* line)
{
    if (!line || !*line)
        return;
    TelemetryLoggerState& st = loggerState();
    if (!st.file)
        return;
    st.buffer.append(line);
    if (st.buffer.size() >= 16384u)
        flushBufferToDisk(false);
}

void appendCheckedLine(const char* line, int written, std::size_t cap)
{
    if (!line || written <= 0 || static_cast<std::size_t>(written) >= cap)
    {
        static bool s_warnedTrunc = false;
        if (!s_warnedTrunc)
        {
            s_warnedTrunc = true;
            std::fprintf(stderr, "[HITREG] telemetry line truncated (written=%d cap=%u)\n",
                         written, static_cast<unsigned int>(cap));
            std::fflush(stderr);
        }
        return;
    }
    const int commas = countCommas(line);
    if (commas != expectedHeaderCommas())
    {
        static bool s_warnedCols = false;
        if (!s_warnedCols)
        {
            s_warnedCols = true;
            std::fprintf(stderr, "[HITREG] telemetry column mismatch (commas=%d expected=%d)\n",
                         commas, expectedHeaderCommas());
            std::fflush(stderr);
        }
    }
    appendLine(line);
}

bool normalizeVec3(const Vec3& v, Vec3& out)
{
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (lenSq <= 1e-8f)
    {
        out.x = out.y = out.z = 0.0f;
        return false;
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    out.x = v.x * invLen;
    out.y = v.y * invLen;
    out.z = v.z * invLen;
    return true;
}

int popCount32(std::uint32_t v)
{
    int count = 0;
    while (v)
    {
        v &= (v - 1u);
        ++count;
    }
    return count;
}

int popCount64(std::uint64_t v)
{
    int count = 0;
    while (v)
    {
        v &= (v - 1ull);
        ++count;
    }
    return count;
}

float computePktLossPct(const PlayerNetState& ns)
{
    if (ns.lossCount <= 0)
        return 0.0f;
    const int lost = popCount64(ns.lossMask);
    return (100.0f * static_cast<float>(lost)) / static_cast<float>(ns.lossCount);
}

float computeRttJitterMs(const PlayerNetState& ns)
{
    if (ns.pingRingCount < 2)
        return 0.0f;
    const int total = ns.pingRingCount;
    const int oldest = (ns.pingRingCount < 32) ? 0 : ns.pingRingWrite;
    int prev = ns.pingRing[oldest];
    long long sumDiff = 0;
    for (int i = 1; i < total; ++i)
    {
        const int idx = (oldest + i) & 31;
        const int cur = ns.pingRing[idx];
        const int diff = cur >= prev ? (cur - prev) : (prev - cur);
        sumDiff += diff;
        prev = cur;
    }
    return static_cast<float>(sumDiff) / static_cast<float>(total - 1);
}

float clampLatencySec(float sec, float maxLat)
{
    if (sec < 0.0f)
        return 0.0f;
    return sec > maxLat ? maxLat : sec;
}

float computeVanillaLatencySec(IPlayer* owner)
{
    // Always model retail BF2 1.50 baseline (pingFactor=0.5, max=0.30s, unclamped interp,
    // no server queueDelay) regardless of active tuning defaults.
    const float kRetailPingFactor = 0.5f;
    const float kRetailMaxLatency = 0.30f;

    const float dbg = LatencyCompensation::getDebugLatency();
    if (dbg > 0.0f)
        return clampLatencySec(dbg, kRetailMaxLatency);
    if (!LatencyCompensation::m_isEnabled || !owner)
        return 0.0f;

    float lat = 0.0f;
    if (owner->getIsRemote())
    {
        lat = static_cast<float>(owner->getPing()) * kRetailPingFactor * 0.001f;
        if (LatencyCompensation::isUseClientWindowEnabled())
            lat += static_cast<float>(owner->getInterpolationTime()) * 0.001f;
        else
            lat += LatencyCompensation::getDefaultLatency();
    }
    else
    {
        lat = LatencyCompensation::getLocalLatency(owner);
    }
    return clampLatencySec(lat, kRetailMaxLatency);
}

void computeTickUsPercentiles(int& outP50, int& outP99)
{
    outP50 = 0;
    outP99 = 0;
    const TelemetryLoggerState& st = loggerState();
    const int n = st.tickUsCount < 60 ? st.tickUsCount : 60;
    if (n <= 0)
        return;

    int sorted[60];
    for (int i = 0; i < n; ++i)
        sorted[i] = st.tickUsRing[i];
    std::sort(sorted, sorted + n);
    outP50 = sorted[n / 2];
    const int idx99 = (n * 99) / 100;
    outP99 = sorted[idx99 < n ? idx99 : (n - 1)];
}

void computeVelResidual(const anim::RaySkeletonProbe& p, const Vec3& vel,
                        float& outResidM, float& outDtErrMs)
{
    outResidM = 0.0f;
    outDtErrMs = 0.0f;
    const float speedSq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speedSq < 4.0f)
        return;
    const float speed = std::sqrt(speedSq);
    const float dx = p.nearestRayPt.x - p.nearestCapPt.x;
    const float dy = p.nearestRayPt.y - p.nearestCapPt.y;
    const float dz = p.nearestRayPt.z - p.nearestCapPt.z;
    const float resid = (dx * vel.x + dy * vel.y + dz * vel.z) / speed;
    float dtErrMs = -1000.0f * resid / speed;
    if (dtErrMs > 500.0f)
        dtErrMs = 500.0f;
    else if (dtErrMs < -500.0f)
        dtErrMs = -500.0f;
    outResidM = resid;
    outDtErrMs = dtErrMs;
}

void buildConfigSignature(char* buf, std::size_t bufSize)
{
    std::snprintf(
        buf, bufSize,
        "improved=%d;pf=%.2f;max=%.2f;maxInterp=%d;qDelay=%d;maxQ=%d;"
        "snap=%d;closest=%d;spawnFix=%d;bpFloor=%.1f;tel=%d;sendRate=%d;"
        "boneHist=%d;qMinInt=%d;qDrain=%d;driftTick=%d;qMaxT=%d;qDecay=%d;qBurst=%d",
        LatencyCompensation::isImprovedEnabled() ? 1 : 0,
        LatencyCompensation::getPingFactor(),
        LatencyCompensation::getMaxLatency(),
        LatencyCompensation::getMaxClientInterpMs(),
        LatencyCompensation::isQueueDelayEnabled() ? 1 : 0,
        LatencyCompensation::getMaxQueueTicks(),
        LatencyCompensation::isSnapshotProjectileEnabled() ? 1 : 0,
        LatencyCompensation::getClosestBoneMode(),
        LatencyCompensation::isSpawnRingFixEnabled() ? 1 : 0,
        LatencyCompensation::getBroadphaseSoldierSpeedFloor(),
        *g_telemetryLevel,
        *g_netServerSendRate,
        LatencyCompensation::isBoneHistoryEnabled() ? 1 : 0,
        LatencyCompensation::getQueueDelayMinInterpMs(),
        LatencyCompensation::isQueueDrainEnabled() ? 1 : 0,
        LatencyCompensation::isDriftFreeTickEnabled() ? 1 : 0,
        LatencyCompensation::getQueueDrainMaxTarget(),
        LatencyCompensation::getQueueDrainDecayTicks(),
        LatencyCompensation::getQueueDrainReplayBurst());
}

void ensureTelemetryOpenAndConfigLogged()
{
    if (*g_telemetryLevel <= 0)
        return;

    (void)g_telemetryFileRegistered;
    TelemetryLoggerState& st = loggerState();
    if (!st.atexitRegistered)
    {
        std::atexit(onProcessExit);
        st.atexitRegistered = true;
    }

    const char* qPath = Vars::queryString("phy-latencyComp-telemetryFile");
    if (!qPath || !*qPath)
        qPath = kDefaultTelemetryFile;
    const std::string basePath(qPath);
    const bool splitMatches = (*g_telemetrySplitMatches != 0);

    if (st.file && (st.activeBasePath != basePath || st.activeSplitMatches != splitMatches))
        closeTelemetryFile();

    if (!st.file)
    {
        const int nextSeq = st.matchSeq + 1;
        const std::string openPath =
            splitMatches ? buildMatchFilePath(basePath, st.currentMapName, nextSeq) : basePath;
        st.file = std::fopen(openPath.c_str(), "w");
        if (!st.file)
            return;
        if (splitMatches)
            st.matchSeq = nextSeq;
        st.activePath = openPath;
        st.activeBasePath = basePath;
        st.activeSplitMatches = splitMatches;
        st.nextShotId = 1;
        st.lastFlushTick = WorldPref::getTickCount();
        st.buffer.clear();
        st.buffer.append(kCsvHeader);
        std::fprintf(stderr, "[HITREG] telemetry file: %s\n", openPath.c_str());
        std::fflush(stderr);
    }

    char sig[384];
    buildConfigSignature(sig, sizeof(sig));
    if (st.lastConfigSig != sig)
    {
        st.lastConfigSig = sig;
        char line[1024];
        const int written = std::snprintf(
            line, sizeof(line),
            "CONFIG,%d,%.3f"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,%s"
            ",,,,,,,,,,,,,,,,,\n",
            WorldPref::getTickCount(), WorldPref::getWorldTime(), sig);
        appendCheckedLine(line, written, sizeof(line));
        flushBufferToDisk(true);
    }
}

void formatVanillaFields(const anim::RaySkeletonProbe& p, int ringUnfilled,
                         char* outBuf, std::size_t outSize)
{
    if (p.hitFirst)
    {
        std::snprintf(outBuf, outSize, "1,%d,%d,0.000,%d",
                      p.firstBone, p.firstPart, ringUnfilled);
    }
    else
    {
        const float d = p.minSurfaceDist < 999.0f ? p.minSurfaceDist : 999.0f;
        std::snprintf(outBuf, outSize, "0,%d,0,%.4f,%d",
                      p.nearestBone, d, ringUnfilled);
    }
}

void fillShotNetSnapshot(IPlayer* owner, ShotRecord& shot)
{
    shot.pingMs = owner ? owner->getPing() : 0u;
    shot.rawInterpMs = owner ? owner->getInterpolationTime() : 0u;
    shot.interpMs = LatencyCompensation::clampClientInterpolationTime(shot.rawInterpMs);
    shot.queueDelayMs = LatencyCompensation::getPlayerQueueDelay(owner) * 1000.0f;
    shot.v1RewindMs = LatencyCompensation::getProjectileLatency(0, owner) * 1000.0f;
    shot.vanFireRewindMs = computeVanillaLatencySec(owner) * 1000.0f;

    const std::unordered_map<const IPlayer*, PlayerNetState>& states = playerNetStates();
    std::unordered_map<const IPlayer*, PlayerNetState>::const_iterator it = states.find(owner);
    if (it != states.end())
    {
        shot.rawPingMs = it->second.lastRawPingMs;
        shot.rttJitterMs = computeRttJitterMs(it->second);
        shot.pktLossDnPct = computePktLossPct(it->second);
        shot.queueDepth = it->second.lastQueueDepth;
        shot.replayCount = it->second.lastReplayCount;
        shot.replays30t = popCount32(it->second.replayMask30);
        shot.shHeldCum = it->second.fireHeldCum;
        shot.shShotsCum = it->second.shotsFiredCum;
        shot.shFdropCum = it->second.fireDroppedCum;
        shot.shEarlyNpCum = it->second.earlyNoProbeCum;
        shot.sndGapTicks = it->second.lastSnapGapTicks;
        if (it->second.lastDrainDropTick >= 0)
        {
            const int age = shot.fireTick - it->second.lastDrainDropTick;
            shot.qDropAgeT = age >= 0 ? age : 0;
        }
    }
}

void emitFireRow(ShotRecord& shot)
{
    if (shot.fireEmitted)
        return;
    shot.fireEmitted = true;

    TelemetryLoggerState& st = loggerState();
    if (*g_telemetryLevel < 2 || !st.file)
        return;

    char devDegBuf[24] = "";
    char devConeBuf[24] = "";
    char muzzleOffBuf[24] = "";
    if (shot.hasDeviation)
    {
        std::snprintf(devDegBuf, sizeof(devDegBuf), "%.3f", shot.devDeg);
        std::snprintf(devConeBuf, sizeof(devConeBuf), "%.3f", shot.devConeDeg);
    }
    if (shot.hasMuzzleOff)
        std::snprintf(muzzleOffBuf, sizeof(muzzleOffBuf), "%.3f", shot.muzzleOffM);

    char line[1024];
    const int written = std::snprintf(
        line, sizeof(line),
        "FIRE,%d,%.3f,%llu,%s,%d,%s,,,"
        "%u,%d,%.2f,%.2f,%u,%u,"
        "%d,%.2f,%d,%d,,,,,"
        "%.2f,%.2f"
        ",,,,,,,,,,,,,,,,,,,,,,,,"
        ",%s,%s,%s,%llu,%llu,%llu,%llu,%d,%d"
        ",,,,,,,,,,,,,,,,,,,,,,,,\n",
        shot.fireTick, shot.fireTimeSec,
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        shot.pingMs, shot.rawPingMs, shot.rttJitterMs, shot.pktLossDnPct,
        shot.rawInterpMs, shot.interpMs,
        shot.queueDepth, shot.queueDelayMs, shot.replayCount, shot.replays30t,
        shot.v1RewindMs, shot.vanFireRewindMs,
        devDegBuf, devConeBuf, muzzleOffBuf,
        static_cast<unsigned long long>(shot.shHeldCum),
        static_cast<unsigned long long>(shot.shShotsCum),
        static_cast<unsigned long long>(shot.shFdropCum),
        static_cast<unsigned long long>(shot.shEarlyNpCum),
        shot.sndGapTicks, shot.qDropAgeT);
    appendCheckedLine(line, written, sizeof(line));
}

} // namespace

bool HitregTelemetry::isEnabled()
{
    return *g_telemetryLevel > 0;
}

int HitregTelemetry::getLevel()
{
    return *g_telemetryLevel;
}

void HitregTelemetry::notePacketDelivery(const IPlayer* player, bool delivered, int instantPingMs)
{
    if (!player || *g_telemetryLevel <= 0)
        return;

    PlayerNetState& ns = playerNetStates()[player];
    ns.lossMask = (ns.lossMask << 1) | (delivered ? 0ull : 1ull);
    if (ns.lossCount < 64)
        ++ns.lossCount;

    if (delivered && instantPingMs >= 0)
    {
        ns.lastRawPingMs = instantPingMs;
        ns.pingRing[ns.pingRingWrite] = instantPingMs;
        ns.pingRingWrite = (ns.pingRingWrite + 1) & 31;
        if (ns.pingRingCount < 32)
            ++ns.pingRingCount;
    }
}

void HitregTelemetry::notePlayerSimTick(const IPlayer* player, int queueDepth,
                                        int replayCount, bool replayed)
{
    if (!player || *g_telemetryLevel <= 0)
        return;

    PlayerNetState& ns = playerNetStates()[player];
    ns.lastQueueDepth = queueDepth;
    ns.lastReplayCount = replayCount;
    ns.replayMask30 = ((ns.replayMask30 << 1) | (replayed ? 1u : 0u)) & 0x3FFFFFFFu;
}

void HitregTelemetry::noteSnapshotSent(const IPlayer* player, int serverTick)
{
    if (!player || *g_telemetryLevel <= 0)
        return;

    PlayerNetState& ns = playerNetStates()[player];
    if (ns.lastSnapTick >= 0)
    {
        const int gap = serverTick - ns.lastSnapTick;
        ns.lastSnapGapTicks = gap >= 0 ? gap : 0;
    }
    ns.lastSnapTick = serverTick;
}

void HitregTelemetry::notePlayerInputConsumed(const IPlayer* player, bool fireHeld)
{
    if (!player || !fireHeld || *g_telemetryLevel <= 0)
        return;

    ++playerNetStates()[player].fireHeldCum;
}

void HitregTelemetry::notePlayerInputDropped(const IPlayer* player, bool fireHeld, bool isDrainDrop)
{
    if (!player || *g_telemetryLevel <= 0)
        return;

    PlayerNetState& ns = playerNetStates()[player];
    if (fireHeld)
        ++ns.fireDroppedCum;
    if (isDrainDrop)
        ns.lastDrainDropTick = WorldPref::getTickCount();
}

void HitregTelemetry::forgetPlayer(const IPlayer* player)
{
    if (player)
        playerNetStates().erase(player);
}

void HitregTelemetry::noteShotFired(const IObject* projectile, IPlayer* owner,
                                    const char* weaponName, const Vec3& startPos,
                                    const Vec3& launchVel)
{
    if (!projectile || !owner || *g_telemetryLevel <= 0)
        return;

    ensureTelemetryOpenAndConfigLogged();
    TelemetryLoggerState& st = loggerState();

    PlayerNetState& ownerNs = playerNetStates()[owner];
    ++ownerNs.shotsFiredCum;

    ShotRecord& shot = shotRecords()[projectile];
    shot = ShotRecord();
    shot.shotId = st.nextShotId++;
    shot.fireTick = WorldPref::getTickCount();
    shot.fireTimeSec = WorldPref::getWorldTime();
    shot.ownerPtr = owner;
    shot.shooterId = owner->getId();
    shot.shooterName = sanitizeField(owner->getName().c_str());
    shot.weaponName = sanitizeField(weaponName);
    shot.startPos = startPos;
    shot.muzzleSpeed = std::sqrt(launchVel.x * launchVel.x
                               + launchVel.y * launchVel.y
                               + launchVel.z * launchVel.z);
    if (normalizeVec3(launchVel, shot.aimDir))
    {
        shot.hasAimDir = true;
        shot.bulletDir = shot.aimDir;
        shot.hasBulletDir = true;
    }

    fillShotNetSnapshot(owner, shot);
    shot.v1RewindMs = LatencyCompensation::getProjectileLatency(projectile, owner) * 1000.0f;

    IObject* const shooterVeh = owner->getVehicle();
    if (shooterVeh && shooterVeh->getTemplate()
        && shooterVeh->getTemplate()->getClassID() == CID_SoldierTemplate)
    {
        Soldier* const shooterSoldier = static_cast<Soldier*>(shooterVeh);
        shot.shooterCenterPos = shooterSoldier->getAbsolutePosition();
        ICameraObject* const cam = shooterSoldier->getSoldierCamera();
        IObject* const camObj = cam ? static_cast<IObject*>(cam->queryInterface(IID_IObject)) : 0;
        shot.shooterEyePos = camObj
            ? camObj->getAbsolutePosition()
            : shooterSoldier->getSkeletonRayTestPoint(Soldier::SkeletonRayTestPointHead);
        shot.hasShooterEye = true;

        if (shot.hasAimDir)
        {
            const float ox = startPos.x - shot.shooterEyePos.x;
            const float oy = startPos.y - shot.shooterEyePos.y;
            const float oz = startPos.z - shot.shooterEyePos.z;
            shot.muzzleOffM = ox * shot.aimDir.x + oy * shot.aimDir.y + oz * shot.aimDir.z;
            shot.hasMuzzleOff = true;
        }
    }
}

void HitregTelemetry::noteShotDeviation(const IObject* projectile, const Vec3& deviatedVel,
                                        float deviationPct)
{
    if (!projectile || *g_telemetryLevel <= 0)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectile);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    Vec3 bDir;
    if (normalizeVec3(deviatedVel, bDir))
    {
        shot.bulletDir = bDir;
        shot.hasBulletDir = true;
        if (shot.hasAimDir)
        {
            float d = shot.aimDir.x * bDir.x + shot.aimDir.y * bDir.y + shot.aimDir.z * bDir.z;
            if (d > 1.0f)
                d = 1.0f;
            else if (d < -1.0f)
                d = -1.0f;
            shot.devDeg = std::acos(d) * 57.2957795f;
            shot.devConeDeg = std::atan(deviationPct * 0.01f) * 57.2957795f;
            shot.hasDeviation = true;
        }
    }
}

void HitregTelemetry::finalizeShotFired(const IObject* projectile)
{
    if (!projectile || *g_telemetryLevel <= 0)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectile);
    if (it != records.end())
        emitFireRow(it->second);
}

void HitregTelemetry::forgetProjectile(const IObject* projectile)
{
    if (!projectile)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectile);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    if (!shot.fireEmitted)
        emitFireRow(shot);

    if (!shot.hadSoldierProbe && shot.ownerPtr)
    {
        const int ageTicks = WorldPref::getTickCount() - shot.fireTick;
        if (ageTicks >= 0 && ageTicks <= 1)
        {
            std::unordered_map<const IPlayer*, PlayerNetState>& states = playerNetStates();
            std::unordered_map<const IPlayer*, PlayerNetState>::iterator pit =
                states.find(shot.ownerPtr);
            if (pit != states.end())
                ++pit->second.earlyNoProbeCum;
        }
    }

    records.erase(it);
}

void HitregTelemetry::recordSoldierProbe(IObject* projectileObject, IPlayer* owner,
                                         IObject* soldierObject,
                                         const SoldierPhysicsNode* soldierNode,
                                         const anim::Skeleton* skeleton,
                                         const Vec3& start, const Vec3& end,
                                         float activeLatency, int v1StateIndex,
                                         const Mat4& v1Transform, bool v1Hit,
                                         int v1HitBone, int v1HitPart, const Vec3& v1HitPos,
                                         bool flyBy, float coarseDist, bool targetInAir,
                                         bool usedBoneHistory,
                                         const anim::HistoricalBonePose* activeBonePose)
{
    if (*g_telemetryLevel <= 0 || !projectileObject || !owner || !soldierObject
        || !soldierNode || !skeleton)
        return;

    ensureTelemetryOpenAndConfigLogged();
    TelemetryLoggerState& st = loggerState();
    if (!st.file)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator shotIt = records.find(projectileObject);
    if (shotIt == records.end())
    {
        const IObjectTemplate* tmpl = projectileObject->getTemplate();
        noteShotFired(projectileObject, owner, tmpl ? tmpl->getName().c_str() : "",
                      start, Vec3::getZero());
        shotIt = records.find(projectileObject);
        if (shotIt == records.end())
            return;
    }
    ShotRecord& shot = shotIt->second;
    if (!shot.fireEmitted)
        emitFireRow(shot);
    shot.hadSoldierProbe = true;

    Soldier* const soldier = static_cast<Soldier*>(soldierObject);
    const int tgtLivePose = static_cast<int>(soldier->getPose());

    const int currentTick = WorldPref::getTickCount();
    const bool useFireSnapshot = LatencyCompensation::isSnapshotProjectileEnabled();
    const unsigned int probePingMs = useFireSnapshot ? shot.pingMs : owner->getPing();
    (void)targetInAir;
    const unsigned int probeRawInterpMs =
        useFireSnapshot ? shot.rawInterpMs : owner->getInterpolationTime();
    const unsigned int probeInterpMs =
        LatencyCompensation::clampClientInterpolationTime(probeRawInterpMs);
    const float probeQDelaySec =
        useFireSnapshot ? (shot.queueDelayMs * 0.001f)
                        : LatencyCompensation::getPlayerQueueDelay(owner);
    const float vanLat = computeVanillaLatencySec(owner);

    int ringValid = 0;
    int ticksSinceReseed = -1;
    int dummyPose = -1;
    Mat4 tmpMat = Mat4::getIdentity();
    Vec3 rewindVel;
    rewindVel.x = rewindVel.y = rewindVel.z = 0.0f;
    soldierNode->sampleLatencyTransform(activeLatency, false, tmpMat, &dummyPose,
                                        &ringValid, &ticksSinceReseed, 0, &rewindVel);
    (void)ringValid;

    Mat4 vanMat = Mat4::getIdentity();
    int vanPose = -1;
    bool vanUnfilledFlag = false;
    soldierNode->sampleLatencyTransform(vanLat, true, vanMat, &vanPose, 0, 0, &vanUnfilledFlag);
    const int vanRingUnfilled = vanUnfilledFlag ? 1 : 0;

    const int initialSkelPose =
        (v1StateIndex >= 0 && v1StateIndex < 4) ? v1StateIndex : tgtLivePose;

    // 1) Active probe (exact pose/bones used by live hitreg) + bone shift metric.
    anim::RaySkeletonProbe pActive;
    float actHeadShiftM = 0.0f;
    float actMaxBoneShiftM = 0.0f;
    if (usedBoneHistory && activeBonePose && activeBonePose->boneCount > 0)
    {
        skeleton->probeRayVsHistoricalPose(start, end, v1Transform, *activeBonePose, pActive);
        skeleton->computePoseBoneShifts(*activeBonePose, actHeadShiftM, actMaxBoneShiftM);
    }
    else
    {
        skeleton->probeRayVsSkeleton(start, end, v1Transform, pActive);
    }
    (void)actMaxBoneShiftM;

    // 2) Single Vanilla shadow probe (retail BF2 1.50 baseline: pf=0.5, max=0.30s, no bone history).
    anim::RaySkeletonProbe pVan;
    const int vanEffPose = (vanPose >= 0 && vanPose < 4) ? vanPose : tgtLivePose;
    const bool canSwitchPose =
        !soldier->m_ragDoll && soldier->m_collisionSkeleton && gameLogic && gameLogic->isServer();
    if (vanEffPose != initialSkelPose && canSwitchPose)
    {
        const int savedPose = soldier->m_pose;
        const int savedCharMode = soldier->m_characterMode;
        const int savedTick = soldier->m_dword26C;
        const int savedCachedPose = soldier->m_dword270;
        const Vec3 savedHeadPos = soldier->m_headPosition;
        const float savedFloat230 = soldier->m_float230;
        const float savedLookIdle = soldier->m_soldierLookIdleTime;
        const bool savedByte27F1 = soldier->m_byte27F[1];
        std::vector<Mat4> savedTransforms;
        if (soldier->m_collisionSkeleton->m_transforms
            && soldier->m_collisionSkeleton->m_boneCount > 0)
        {
            const int boneCount = soldier->m_collisionSkeleton->m_boneCount;
            savedTransforms.assign(
                soldier->m_collisionSkeleton->m_transforms,
                soldier->m_collisionSkeleton->m_transforms + boneCount);
        }

        soldier->m_byte27F[1] = true;
        soldier->m_characterMode = 1;
        soldier->m_pose = vanEffPose;
        soldier->m_headPosition = savedHeadPos;
        soldier->m_float230 = savedFloat230;
        soldier->m_soldierLookIdleTime = savedLookIdle;
        soldier->applyAnimationsOnSkeleton(0.0f, false);
        soldier->m_collisionSkeleton->probeRayVsSkeleton(start, end, vanMat, pVan);

        soldier->m_byte27F[1] = true;
        soldier->m_characterMode = 1;
        soldier->m_pose = initialSkelPose;
        soldier->m_headPosition = savedHeadPos;
        soldier->m_float230 = savedFloat230;
        soldier->m_soldierLookIdleTime = savedLookIdle;
        soldier->applyAnimationsOnSkeleton(0.0f, false);
        if (!savedTransforms.empty() && soldier->m_collisionSkeleton->m_transforms)
        {
            for (std::size_t b = 0; b < savedTransforms.size(); ++b)
                soldier->m_collisionSkeleton->m_transforms[b] = savedTransforms[b];
        }
        soldier->m_pose = savedPose;
        soldier->m_characterMode = savedCharMode;
        soldier->m_dword26C = savedTick;
        soldier->m_dword270 = savedCachedPose;
        soldier->m_headPosition = savedHeadPos;
        soldier->m_float230 = savedFloat230;
        soldier->m_soldierLookIdleTime = savedLookIdle;
        soldier->m_byte27F[1] = savedByte27F1;
        if (savedCharMode != 1)
        {
            IObject* const item = soldier->getItem(soldier->m_activeItemIndex);
            if (item && (item->getFlags() & 1u) == 0)
            {
                IAnimatedBundle* const animated = static_cast<IAnimatedBundle*>(
                    item->queryInterface(IID_IAnimatedBundle));
                if (animated)
                    animated->setActiveAnimationSystem(savedCharMode);
            }
        }
    }
    else
    {
        skeleton->probeRayVsSkeleton(start, end, vanMat, pVan);
    }

    IPlayer* const tgtPlayer = soldier->getOccupingPlayer();
    const int tgtId = tgtPlayer ? tgtPlayer->getId() : -1;
    const std::string tgtName = tgtPlayer ? sanitizeField(tgtPlayer->getName().c_str()) : std::string();
    const int tgtAlive = (tgtPlayer && tgtPlayer->getIsAlive()) ? 1 : 0;
    const int tgtManDown = (tgtPlayer && tgtPlayer->getIsManDown()) ? 1 : 0;

    const Vec3& vel = soldierNode->getPositionalSpeed();
    const float tgtSpeed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);

    const Vec3& soldierPos = soldierObject->getAbsolutePosition();
    const float dxShot = soldierPos.x - shot.startPos.x;
    const float dyShot = soldierPos.y - shot.startPos.y;
    const float dzShot = soldierPos.z - shot.startPos.z;
    const float shotDist = std::sqrt(dxShot * dxShot + dyShot * dyShot + dzShot * dzShot);
    const float dxRay = end.x - start.x;
    const float dyRay = end.y - start.y;
    const float dzRay = end.z - start.z;
    const int flightTicks = currentTick - shot.fireTick;

    const std::string actBoneName =
        v1Hit ? sanitizeField(skeleton->getBoneNameSafe(v1HitBone)) : std::string();
    const float actMissM = v1Hit ? 0.0f : (pActive.minSurfaceDist < 999.0f ? pActive.minSurfaceDist : 999.0f);

    char vanBuf[64];
    formatVanillaFields(pVan, vanRingUnfilled, vanBuf, sizeof(vanBuf));

    int qMinWin = 0, qTarget = 1, qDrainDrops = 0, qCapDrops = 0;
    LatencyCompensation::getPlayerQueueStats(owner, qMinWin, qTarget, qDrainDrops, qCapDrops);
    (void)qMinWin;
    (void)qCapDrops;
    int tickUsP50 = 0, tickUsP99 = 0;
    computeTickUsPercentiles(tickUsP50, tickUsP99);

    float actVelResidM = 0.0f, actDtErrMs = 0.0f;
    computeVelResidual(pActive, rewindVel, actVelResidM, actDtErrMs);
    (void)actVelResidM;

    // 3) Zero-deviation aim shadow probe + directional miss + back-shot dot + ring underflow estimate.
    char aimHitBuf[8] = "";
    char aimMissBuf[24] = "";
    if (shot.hasAimDir)
    {
        const float aimLen = shotDist + 2.5f;
        Vec3 aimEnd;
        aimEnd.x = shot.startPos.x + shot.aimDir.x * aimLen;
        aimEnd.y = shot.startPos.y + shot.aimDir.y * aimLen;
        aimEnd.z = shot.startPos.z + shot.aimDir.z * aimLen;

        anim::RaySkeletonProbe pAim;
        if (usedBoneHistory && activeBonePose && activeBonePose->boneCount > 0)
            skeleton->probeRayVsHistoricalPose(shot.startPos, aimEnd, v1Transform, *activeBonePose, pAim);
        else
            skeleton->probeRayVsSkeleton(shot.startPos, aimEnd, v1Transform, pAim);

        const bool aimHit = pAim.hitFirst || pAim.hitClosest;
        const float aimMissM = aimHit ? 0.0f : (pAim.minSurfaceDist < 999.0f ? pAim.minSurfaceDist : 999.0f);
        std::snprintf(aimHitBuf, sizeof(aimHitBuf), "%d", aimHit ? 1 : 0);
        std::snprintf(aimMissBuf, sizeof(aimMissBuf), "%.4f", aimMissM);
    }

    Vec3 rayDir;
    Vec3 rayVec;
    rayVec.x = dxRay;
    rayVec.y = dyRay;
    rayVec.z = dzRay;
    const bool hasRayDir = normalizeVec3(rayVec, rayDir);

    char missDyBuf[24] = "";
    char missLatBuf[24] = "";
    if (!v1Hit && pActive.nearestBone >= 0)
    {
        const float mdx = pActive.nearestRayPt.x - pActive.nearestCapPt.x;
        const float mdy = pActive.nearestRayPt.y - pActive.nearestCapPt.y;
        const float mdz = pActive.nearestRayPt.z - pActive.nearestCapPt.z;
        std::snprintf(missDyBuf, sizeof(missDyBuf), "%.4f", mdy);

        const Vec3& refDir = shot.hasAimDir ? shot.aimDir : rayDir;
        if (shot.hasAimDir || hasRayDir)
        {
            Vec3 rightRaw;
            rightRaw.x = refDir.z;
            rightRaw.y = 0.0f;
            rightRaw.z = -refDir.x;
            Vec3 rightDir;
            if (normalizeVec3(rightRaw, rightDir))
            {
                const float missLat = mdx * rightDir.x + mdz * rightDir.z;
                std::snprintf(missLatBuf, sizeof(missLatBuf), "%.4f", missLat);
            }
        }
    }

    char tgtBackDotBuf[24] = "";
    Vec3 tgtFwdRaw;
    tgtFwdRaw.x = v1Transform.m[8];
    tgtFwdRaw.y = v1Transform.m[9];
    tgtFwdRaw.z = v1Transform.m[10];
    Vec3 tgtFwd;
    const Vec3& bDir = shot.hasBulletDir ? shot.bulletDir : rayDir;
    if ((shot.hasBulletDir || hasRayDir) && normalizeVec3(tgtFwdRaw, tgtFwd))
    {
        const float backDot = bDir.x * tgtFwd.x + bDir.y * tgtFwd.y + bDir.z * tgtFwd.z;
        std::snprintf(tgtBackDotBuf, sizeof(tgtBackDotBuf), "%.3f", backDot);
    }

    const int kEst = static_cast<int>(static_cast<float>(probePingMs) / 33.333333f)
                   + (shot.queueDepth > 1 ? (shot.queueDepth - 1) : 0);
    const int ringUf =
        ((static_cast<float>(probeRawInterpMs) - 15.0f)
         > static_cast<float>(kEst + 4) * 33.333333f)
            ? 1 : 0;

    char line[2048];
    const int written = std::snprintf(
        line, sizeof(line),
        "PROBE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        "%u,%d,%.2f,%.2f,%u,%u,"
        "%d,%.2f,%d,%d,%d,%d,%d,%d,"
        "%.2f,%.2f,%.2f,%d,%.4f,%d,"
        "%d,%d,%d,%d,%.3f,%d,"
        "%d,%.4f,%d,%d,%s,%d,%.4f,%d,%.2f,"
        "%s"
        ",,,,,,,,,"
        ",%s,%s,%s,%s,%s,%d"
        ",,,,,,,,,,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        tgtId, tgtName.c_str(),
        probePingMs, shot.rawPingMs, shot.rttJitterMs, shot.pktLossDnPct,
        probeRawInterpMs, probeInterpMs,
        shot.queueDepth, probeQDelaySec * 1000.0f, shot.replayCount, shot.replays30t,
        qTarget, qDrainDrops, tickUsP50, tickUsP99,
        activeLatency * 1000.0f, vanLat * 1000.0f,
        shotDist, flightTicks, coarseDist, flyBy ? 1 : 0,
        tgtAlive, tgtManDown, tgtLivePose, v1StateIndex, tgtSpeed, ticksSinceReseed,
        usedBoneHistory ? 1 : 0, actHeadShiftM,
        v1Hit ? 1 : 0, v1Hit ? v1HitBone : -1, actBoneName.c_str(),
        v1Hit ? v1HitPart : 0, actMissM, pActive.nearestBone, actDtErrMs,
        vanBuf,
        aimHitBuf, aimMissBuf, missDyBuf, missLatBuf, tgtBackDotBuf, ringUf);
    appendCheckedLine(line, written, sizeof(line));

    if (v1Hit)
    {
        const Vec3& projEnd = projectileObject->getAbsolutePosition();
        const float dx = v1HitPos.x - projEnd.x;
        const float dy = v1HitPos.y - projEnd.y;
        const float dz = v1HitPos.z - projEnd.z;
        const float depthSq = dx * dx + dy * dy + dz * dz;
        if (shot.pendingTick != currentTick || !shot.pendingTarget || depthSq > shot.pendingDepthSq)
        {
            shot.pendingTick = currentTick;
            shot.pendingTarget = soldierObject;
            shot.pendingTargetId = tgtId;
            shot.pendingTargetName = tgtName;
            shot.pendingBone = v1HitBone;
            shot.pendingPart = v1HitPart;
            shot.pendingDepthSq = depthSq;
            shot.pendingRayStart = start;
            shot.pendingHitPos = v1HitPos;
        }
    }
}

void HitregTelemetry::noteProjectileContactDelivered(const IObject* projectileObject,
                                                     IObject* contactObject,
                                                     const Vec3& contactPoint,
                                                     float contactDepthSq,
                                                     int contactMaterial)
{
    if (*g_telemetryLevel <= 0 || !projectileObject)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectileObject);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    const int currentTick = WorldPref::getTickCount();
    if (shot.pendingTick != currentTick || !shot.pendingTarget)
        return;

    const char* outcome = "DELIVERED";
    std::string blocker;
    float marginM = 0.0f;
    float tWallM = 0.0f;
    float tSoldierM = 0.0f;
    const char* objClass = "";
    int eyeClear = -1;

    if (contactObject != shot.pendingTarget)
    {
        const float winnerDist = contactDepthSq >= 0.0f ? std::sqrt(contactDepthSq) : 0.0f;
        const float soldierDist = shot.pendingDepthSq >= 0.0f ? std::sqrt(shot.pendingDepthSq) : 0.0f;
        marginM = winnerDist - soldierDist;

        const float wdx = contactPoint.x - shot.pendingRayStart.x;
        const float wdy = contactPoint.y - shot.pendingRayStart.y;
        const float wdz = contactPoint.z - shot.pendingRayStart.z;
        tWallM = std::sqrt(wdx * wdx + wdy * wdy + wdz * wdz);

        const float sdx = shot.pendingHitPos.x - shot.pendingRayStart.x;
        const float sdy = shot.pendingHitPos.y - shot.pendingRayStart.y;
        const float sdz = shot.pendingHitPos.z - shot.pendingRayStart.z;
        tSoldierM = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);

        if (!contactObject)
        {
            outcome = "OVERRIDE_TERRAIN";
            blocker = "TERRAIN";
            objClass = "terrain";
        }
        else
        {
            const IObjectTemplate* tmpl = contactObject->getTemplate();
            if (tmpl && tmpl->getClassID() == CID_SoldierTemplate)
            {
                outcome = "OVERRIDE_SOLDIER";
                objClass = "soldier";
            }
            else
            {
                outcome = "OVERRIDE_WALL";
                IPhysicsNode* const rootNode = static_cast<IPhysicsNode*>(
                    object_detail::LayoutAccess::physicsNode(getRootParent(contactObject)));
                objClass = (rootNode && rootNode->getIsMobile()) ? "dynamic" : "static";
            }
            blocker = tmpl ? sanitizeField(tmpl->getName().c_str()) : std::string("OBJECT");
        }

        if (shot.hasShooterEye)
        {
            IObject* dummyHit = 0;
            const bool guard1Clear = !checkIfRayHitAnyObjectOrGround(
                shot.shooterCenterPos, shot.shooterEyePos, 0, dummyHit, true, false, true, 0);
            const bool guard2Clear = guard1Clear && !checkIfRayHitAnyObjectOrGround(
                shot.shooterEyePos, shot.pendingHitPos, 0, dummyHit, true, false, true, 0);
            eyeClear = guard2Clear ? 1 : 0;
        }
    }

    char line[1024];
    const int written = std::snprintf(
        line, sizeof(line),
        "DELIVER,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,"
        "%s,%s,%d,%.4f,%.4f,%.4f,%s,%d"
        ",,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        shot.pendingTargetId, shot.pendingTargetName.c_str(),
        outcome, blocker.c_str(), contactMaterial, marginM,
        tWallM, tSoldierM, objClass, eyeClear);
    appendCheckedLine(line, written, sizeof(line));

    shot.pendingTarget = 0;
}

void HitregTelemetry::noteServerTickUs(int tickUs)
{
    if (*g_telemetryLevel <= 0 || tickUs <= 0)
        return;

    TelemetryLoggerState& st = loggerState();
    st.tickUsRing[st.tickUsWrite] = tickUs;
    st.tickUsWrite = (st.tickUsWrite + 1) % 60;
    if (st.tickUsCount < 60)
        ++st.tickUsCount;
}

void HitregTelemetry::noteProjectileDamageCalc(const IObject* projectileObject,
                                               int matA, int matB, float baseDamage,
                                               float matMod, float angleMod)
{
    if (*g_telemetryLevel <= 0 || !projectileObject)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectileObject);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    shot.hasDmgCalc = true;
    shot.matA = matA;
    shot.matB = matB;
    shot.baseDmg = baseDamage;
    shot.matMod = matMod;
    shot.angleMod = angleMod;
    shot.hasServerDmg = false;
    shot.finalDmg = 0.0f;
    shot.hpBefore = 0.0f;
    shot.hpAfter = 0.0f;
    shot.killed = false;
}

void HitregTelemetry::noteServerGiveDamage(const IObject* sourceObject,
                                           float finalDamage, float hpBefore,
                                           float hpAfter, bool killed)
{
    if (*g_telemetryLevel <= 0 || !sourceObject)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(sourceObject);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    shot.hasServerDmg = true;
    shot.finalDmg = finalDamage;
    shot.hpBefore = hpBefore;
    shot.hpAfter = hpAfter;
    shot.killed = killed;
}

void HitregTelemetry::finishProjectileCollisionDamage(const IObject* projectileObject,
                                                      IObject* hitObject,
                                                      bool hitmarker)
{
    if (*g_telemetryLevel <= 0 || !projectileObject)
        return;

    std::unordered_map<const IObject*, ShotRecord>& records = shotRecords();
    std::unordered_map<const IObject*, ShotRecord>::iterator it = records.find(projectileObject);
    if (it == records.end())
        return;

    ShotRecord& shot = it->second;
    if (!shot.hasDmgCalc)
        return;

    if (!hitmarker && shot.baseDmg <= 0.0f && (!shot.hasServerDmg || shot.finalDmg <= 0.0f))
    {
        shot.hasDmgCalc = false;
        shot.hasServerDmg = false;
        return;
    }

    int tgtId = -1;
    std::string tgtName;
    const IPlayer* tgtPlayer = 0;
    if (hitObject && hitObject->getTemplate()
        && hitObject->getTemplate()->getClassID() == CID_SoldierTemplate)
    {
        tgtPlayer = static_cast<Soldier*>(hitObject)->getOccupingPlayer();
        if (tgtPlayer)
        {
            tgtId = tgtPlayer->getId();
            tgtName = sanitizeField(tgtPlayer->getName().c_str());
        }
    }

    char vHeldBuf[24] = "";
    char vShotsBuf[24] = "";
    char vFdropBuf[24] = "";
    char vEarlyNpBuf[24] = "";
    if (tgtPlayer)
    {
        const std::unordered_map<const IPlayer*, PlayerNetState>& states = playerNetStates();
        std::unordered_map<const IPlayer*, PlayerNetState>::const_iterator vit =
            states.find(tgtPlayer);
        if (vit != states.end())
        {
            std::snprintf(vHeldBuf, sizeof(vHeldBuf), "%llu",
                          static_cast<unsigned long long>(vit->second.fireHeldCum));
            std::snprintf(vShotsBuf, sizeof(vShotsBuf), "%llu",
                          static_cast<unsigned long long>(vit->second.shotsFiredCum));
            std::snprintf(vFdropBuf, sizeof(vFdropBuf), "%llu",
                          static_cast<unsigned long long>(vit->second.fireDroppedCum));
            std::snprintf(vEarlyNpBuf, sizeof(vEarlyNpBuf), "%llu",
                          static_cast<unsigned long long>(vit->second.earlyNoProbeCum));
        }
    }

    char line[1024];
    const int written = std::snprintf(
        line, sizeof(line),
        "DAMAGE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,%s,%s,%s,%s"
        ",,,,,,,,,,,,,,,,,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
        WorldPref::getTickCount(), WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        tgtId, tgtName.c_str(),
        vHeldBuf, vShotsBuf, vFdropBuf, vEarlyNpBuf,
        shot.matA, shot.matB, shot.baseDmg, shot.matMod, shot.angleMod,
        shot.hasServerDmg ? shot.finalDmg : 0.0f,
        shot.hasServerDmg ? shot.hpBefore : 0.0f,
        shot.hasServerDmg ? shot.hpAfter : 0.0f,
        (shot.hasServerDmg && shot.killed) ? 1 : 0,
        hitmarker ? 1 : 0);
    appendCheckedLine(line, written, sizeof(line));

    shot.hasDmgCalc = false;
    shot.hasServerDmg = false;
}

void HitregTelemetry::tickFlush()
{
    TelemetryLoggerState& st = loggerState();
    if (*g_telemetryLevel <= 0)
    {
        closeTelemetryFile();
        return;
    }

    if (!st.file)
    {
        // With split-matches enabled, defer file creation until the first shot/probe
        // of the match so empty rounds never leave 0-shot CSV files behind.
        if (*g_telemetrySplitMatches != 0)
            return;
        ensureTelemetryOpenAndConfigLogged();
        if (!st.file)
            return;
    }
    else
    {
        ensureTelemetryOpenAndConfigLogged();
        if (!st.file)
            return;
    }

    const int currentTick = WorldPref::getTickCount();
    const int flushEvery = *g_telemetryFlush > 0 ? *g_telemetryFlush : 30;
    if (currentTick - st.lastFlushTick >= flushEvery || currentTick < st.lastFlushTick)
    {
        flushBufferToDisk(true);
        st.lastFlushTick = currentTick;
    }
}

void HitregTelemetry::noteMatchStarted(const char* mapName)
{
    TelemetryLoggerState& st = loggerState();
    st.currentMapName = sanitizeFilenameToken(mapName);
    if (*g_telemetrySplitMatches != 0)
        closeTelemetryFile();
}

void HitregTelemetry::noteMatchEnded()
{
    if (*g_telemetrySplitMatches != 0)
        closeTelemetryFile();
}

} // namespace world
} // namespace hfe
} // namespace dice
