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

    PlayerNetState()
        : lossMask(0)
        , lossCount(0)
        , pingRingWrite(0)
        , pingRingCount(0)
        , lastRawPingMs(0)
        , lastQueueDepth(0)
        , lastReplayCount(0)
        , replayMask30(0)
    {
        std::memset(pingRing, 0, sizeof(pingRing));
    }
};

struct ShotRecord
{
    std::uint64_t shotId;
    int           fireTick;
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
    "q_depth,q_delay_ms,replay_cnt,replays_30t,v1_rewind_ms,van_rewind_ms,"
    "shot_dist_m,flight_ticks,coarse_dist_m,flyby,tgt_alive,tgt_mandown,"
    "tgt_live_pose,tgt_v1_pose,tgt_van_pose,tgt_pose_diff,tgt_speed,tgt_hspeed,tgt_vy,"
    "tgt_in_air,tgt_ring_valid,tgt_since_spawn_ms,"
    "v1_hit,v1_bone,v1_bone_name,v1_part,v1_first_bone,v1_miss_m,v1_near_bone,"
    "van_hit,van_bone,van_part,van_miss_m,van_ring_unfilled,"
    "pf00_hit,pf00_bone,pf00_miss_m,pf05_hit,pf05_bone,pf05_miss_m,"
    "pf075_hit,pf075_bone,pf075_miss_m,pf10_hit,pf10_bone,pf10_miss_m,"
    "raw05_hit,raw05_bone,raw05_miss_m,"
    "pf125_hit,pf125_bone,pf125_miss_m,uncl10_hit,uncl10_bone,uncl10_miss_m,"
    "active_v2,eff_q_delay_ms,tgt_ping_ms,ray_len_m,"
    "v2_bone_valid,v2_head_shift_m,v2_max_bone_shift_m,"
    "v2_hit,v2_bone,v2_part,v2_miss_m,"
    "v11_hit,v11_bone,v11_part,v11_miss_m,"
    "nodedup_hit,nodedup_bone,nodedup_miss_m,"
    "q_min_win,q_target,q_drain_drops,q_cap_drops,tick_us_p50,tick_us_p99,"
    "v2_vel_resid_m,v2_dt_err_ms,v11_dt_err_ms,"
    "v2a_hit,v2a_bone,v2a_miss_m,v2a_dt_err_ms,"
    "c17_hit,c17_bone,c17_miss_m,c17_dt_err_ms,"
    "c33_hit,c33_bone,c33_miss_m,c33_dt_err_ms,"
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

float clampRewindLatency(float sec)
{
    return clampLatencySec(sec, LatencyCompensation::getMaxLatency());
}

float computeVanillaLatencySec(IPlayer* owner)
{
    // Always model retail BF2 1.50 baseline (pingFactor=0.5, max=0.30s, unclamped interp,
    // no server queueDelay) regardless of V1.1 active tuning defaults.
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

float computeSweepLatencyFromInputs(IPlayer* owner, float pingFactorOverride,
                                       int pingMs, unsigned int rawInterpMs,
                                       float queueDelaySec, bool clampInterp,
                                       bool useQueueDelay)
{
    const float dbg = LatencyCompensation::getDebugLatency();
    if (dbg > 0.0f)
        return clampRewindLatency(dbg);
    if (!LatencyCompensation::m_isEnabled || !owner)
        return 0.0f;
    if (!owner->getIsRemote())
        return clampRewindLatency(LatencyCompensation::getLocalLatency(owner));

    float lat = static_cast<float>(pingMs) * pingFactorOverride * 0.001f;
    if (LatencyCompensation::isUseClientWindowEnabled())
    {
        const unsigned int interp = clampInterp
            ? LatencyCompensation::clampClientInterpolationTime(rawInterpMs)
            : rawInterpMs;
        lat += static_cast<float>(interp) * 0.001f;
    }
    else
    {
        lat += LatencyCompensation::getDefaultLatency();
    }
    if (useQueueDelay && LatencyCompensation::isQueueDelayEnabled())
        lat += queueDelaySec;

    return clampRewindLatency(lat);
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
        "boneHist=%d;qMinInt=%d;qDrain=%d;driftTick=%d",
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
        LatencyCompensation::isDriftFreeTickEnabled() ? 1 : 0);
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
        char line[768];
        std::snprintf(
            line, sizeof(line),
            "CONFIG,%d,%.3f"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,%s"
            ",,,,,,,,,,,,,,,,,\n",
            WorldPref::getTickCount(), WorldPref::getWorldTime(), sig);
        appendLine(line);
        flushBufferToDisk(true);
    }
}

void formatProbeFields(const anim::RaySkeletonProbe& p, char* outBuf, std::size_t outSize)
{
    if (p.hitClosest)
    {
        std::snprintf(outBuf, outSize, "1,%d,0.000", p.closestBone);
    }
    else
    {
        const float d = p.minSurfaceDist < 999.0f ? p.minSurfaceDist : 999.0f;
        std::snprintf(outBuf, outSize, "0,%d,%.4f", p.nearestBone, d);
    }
}

void formatFullProbeFields(const anim::RaySkeletonProbe& p, char* outBuf, std::size_t outSize)
{
    if (p.hitClosest)
    {
        std::snprintf(outBuf, outSize, "1,%d,%d,0.0000", p.closestBone, p.closestPart);
    }
    else
    {
        const float d = p.minSurfaceDist < 999.0f ? p.minSurfaceDist : 999.0f;
        std::snprintf(outBuf, outSize, "0,%d,0,%.4f", p.nearestBone, d);
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
    }
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

void HitregTelemetry::forgetPlayer(const IPlayer* player)
{
    if (player)
        playerNetStates().erase(player);
}

void HitregTelemetry::noteShotFired(const IObject* projectile, IPlayer* owner,
                                    const char* weaponName, const Vec3& startPos,
                                    float muzzleSpeed)
{
    if (!projectile || !owner || *g_telemetryLevel <= 0)
        return;

    ensureTelemetryOpenAndConfigLogged();
    TelemetryLoggerState& st = loggerState();

    ShotRecord& shot = shotRecords()[projectile];
    shot = ShotRecord();
    shot.shotId = st.nextShotId++;
    shot.fireTick = WorldPref::getTickCount();
    shot.shooterId = owner->getId();
    shot.shooterName = sanitizeField(owner->getName().c_str());
    shot.weaponName = sanitizeField(weaponName);
    shot.startPos = startPos;
    shot.muzzleSpeed = muzzleSpeed;
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
    }

    if (*g_telemetryLevel >= 2 && st.file)
    {
        char line[768];
        std::snprintf(
            line, sizeof(line),
            "FIRE,%d,%.3f,%llu,%s,%d,%s,,,"
            "%u,%d,%.2f,%.2f,%u,%u,"
            "%d,%.2f,%d,%d,%.2f,%.2f"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,"
            ",,,,,,\n",
            shot.fireTick, WorldPref::getWorldTime(),
            static_cast<unsigned long long>(shot.shotId),
            shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
            shot.pingMs, shot.rawPingMs, shot.rttJitterMs, shot.pktLossDnPct,
            shot.rawInterpMs, shot.interpMs,
            shot.queueDepth, shot.queueDelayMs, shot.replayCount, shot.replays30t,
            shot.v1RewindMs, shot.vanFireRewindMs);
        appendLine(line);
    }
}

void HitregTelemetry::forgetProjectile(const IObject* projectile)
{
    if (projectile)
        shotRecords().erase(projectile);
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
        noteShotFired(projectileObject, owner, tmpl ? tmpl->getName().c_str() : "", start, 0.0f);
        shotIt = records.find(projectileObject);
        if (shotIt == records.end())
            return;
    }
    ShotRecord& shot = shotIt->second;

    Soldier* const soldier = static_cast<Soldier*>(soldierObject);
    const int tgtLivePose = static_cast<int>(soldier->getPose());

    const int currentTick = WorldPref::getTickCount();
    const bool useFireSnapshot = LatencyCompensation::isSnapshotProjectileEnabled();
    const unsigned int probePingMs = useFireSnapshot ? shot.pingMs : owner->getPing();
    const unsigned int probeRawInterpMs =
        useFireSnapshot ? shot.rawInterpMs : owner->getInterpolationTime();
    const unsigned int probeInterpMs =
        LatencyCompensation::clampClientInterpolationTime(probeRawInterpMs);
    const float probeQDelaySec =
        useFireSnapshot ? (shot.queueDelayMs * 0.001f)
                        : LatencyCompensation::getPlayerQueueDelay(owner);
    const float probeEffQDelaySec =
        LatencyCompensation::computeEffectiveQueueDelay(probeRawInterpMs, probeQDelaySec);
    const int probeRawPingMs =
        shot.rawPingMs > 0 ? shot.rawPingMs : static_cast<int>(probePingMs);

    const float vanLat = computeVanillaLatencySec(owner);
    const float pf00Lat = 0.0f;
    const float pf05Lat = computeSweepLatencyFromInputs(
        owner, 0.50f, static_cast<int>(probePingMs), probeRawInterpMs, probeEffQDelaySec, true, true);
    const float pf075Lat = computeSweepLatencyFromInputs(
        owner, 0.75f, static_cast<int>(probePingMs), probeRawInterpMs, probeEffQDelaySec, true, true);
    const float pf10Lat = computeSweepLatencyFromInputs(
        owner, 1.00f, static_cast<int>(probePingMs), probeRawInterpMs, probeEffQDelaySec, true, true);
    const float raw05Lat = computeSweepLatencyFromInputs(
        owner, 0.50f, probeRawPingMs, probeRawInterpMs, probeEffQDelaySec, true, true);
    const float pf125Lat = computeSweepLatencyFromInputs(
        owner, 1.25f, static_cast<int>(probePingMs), probeRawInterpMs, probeEffQDelaySec, true, true);
    const float uncl10Lat = computeSweepLatencyFromInputs(
        owner, 1.00f, static_cast<int>(probePingMs), probeRawInterpMs, probeEffQDelaySec, false, false);
    const float nodedupLat = computeSweepLatencyFromInputs(
        owner, 1.00f, static_cast<int>(probePingMs), probeRawInterpMs, probeQDelaySec, true, true);
    const float c17Lat = clampRewindLatency(activeLatency + 0.017f);
    const float c33Lat = clampRewindLatency(activeLatency + kServerTickSec);

    int ringValid = 0;
    int ticksSinceReseed = -1;
    int dummyPose = -1;
    Mat4 tmpMat = Mat4::getIdentity();
    Vec3 rewindVel;
    rewindVel.x = rewindVel.y = rewindVel.z = 0.0f;
    soldierNode->sampleLatencyTransform(activeLatency, false, tmpMat, &dummyPose,
                                        &ringValid, &ticksSinceReseed, 0, &rewindVel);

    Mat4 vanMat = Mat4::getIdentity();
    int vanPose = -1;
    int vanValid = 0;
    int vanSince = -1;
    bool vanUnfilledFlag = false;
    soldierNode->sampleLatencyTransform(vanLat, true, vanMat, &vanPose, &vanValid, &vanSince,
                                        &vanUnfilledFlag);
    const int vanRingUnfilled = vanUnfilledFlag ? 1 : 0;

    Mat4 pf00Mat = Mat4::getIdentity();
    Mat4 pf05Mat = Mat4::getIdentity();
    Mat4 pf075Mat = Mat4::getIdentity();
    Mat4 pf10Mat = Mat4::getIdentity();
    Mat4 raw05Mat = Mat4::getIdentity();
    Mat4 pf125Mat = Mat4::getIdentity();
    Mat4 uncl10Mat = Mat4::getIdentity();
    Mat4 nodedupMat = Mat4::getIdentity();
    Mat4 c17Mat = Mat4::getIdentity();
    Mat4 c33Mat = Mat4::getIdentity();
    Vec3 c17Vel = rewindVel;
    Vec3 c33Vel = rewindVel;
    int pf00Pose = -1;
    int pf05Pose = -1;
    int pf075Pose = -1;
    int pf10Pose = -1;
    int raw05Pose = -1;
    int pf125Pose = -1;
    int uncl10Pose = -1;
    int nodedupPose = -1;
    int c17Pose = -1;
    int c33Pose = -1;
    soldierNode->sampleLatencyTransform(pf00Lat, false, pf00Mat, &pf00Pose, 0, 0);
    soldierNode->sampleLatencyTransform(pf05Lat, false, pf05Mat, &pf05Pose, 0, 0);
    soldierNode->sampleLatencyTransform(pf075Lat, false, pf075Mat, &pf075Pose, 0, 0);
    soldierNode->sampleLatencyTransform(pf10Lat, false, pf10Mat, &pf10Pose, 0, 0);
    soldierNode->sampleLatencyTransform(raw05Lat, false, raw05Mat, &raw05Pose, 0, 0);
    soldierNode->sampleLatencyTransform(pf125Lat, false, pf125Mat, &pf125Pose, 0, 0);
    soldierNode->sampleLatencyTransform(uncl10Lat, false, uncl10Mat, &uncl10Pose, 0, 0);
    soldierNode->sampleLatencyTransform(nodedupLat, false, nodedupMat, &nodedupPose, 0, 0);
    soldierNode->sampleLatencyTransform(c17Lat, false, c17Mat, &c17Pose, 0, 0, 0, &c17Vel);
    soldierNode->sampleLatencyTransform(c33Lat, false, c33Mat, &c33Pose, 0, 0, 0, &c33Vel);

    const int initialSkelPose =
        (v1StateIndex >= 0 && v1StateIndex < 4) ? v1StateIndex : tgtLivePose;
    int activeSkelPose = initialSkelPose;
    const anim::Skeleton* activeSkel = skeleton;

    // Evaluate V1.1 (live animation at v1StateIndex) and V2 / V2a (historical collision bones)
    // before any shadow pose switches touch soldier->m_collisionSkeleton.
    anim::RaySkeletonProbe pV11;
    activeSkel->probeRayVsSkeleton(start, end, v1Transform, pV11);

    anim::HistoricalBonePose sampledV2Pose;
    const anim::HistoricalBonePose* v2PosePtr = 0;
    if (activeBonePose && activeBonePose->boneCount > 0)
        v2PosePtr = activeBonePose;
    else if (soldierNode->sampleHistoricalBonePose(activeLatency, sampledV2Pose))
        v2PosePtr = &sampledV2Pose;

    const int v2BoneValid = (v2PosePtr && v2PosePtr->boneCount > 0) ? 1 : 0;
    float v2HeadShiftM = 0.0f;
    float v2MaxBoneShiftM = 0.0f;
    anim::RaySkeletonProbe pV2;
    if (v2BoneValid)
    {
        activeSkel->probeRayVsHistoricalPose(start, end, v1Transform, *v2PosePtr, pV2);
        activeSkel->computePoseBoneShifts(*v2PosePtr, v2HeadShiftM, v2MaxBoneShiftM);
    }
    else
    {
        pV2 = pV11;
    }

    const float v2aAnimLat =
        activeLatency > (static_cast<float>(probeInterpMs) * 0.001f)
            ? (activeLatency - static_cast<float>(probeInterpMs) * 0.001f)
            : 0.0f;
    anim::HistoricalBonePose sampledV2aPose;
    anim::RaySkeletonProbe pV2a = pV11;
    if (v2aAnimLat > kServerTickSec && soldierNode->sampleHistoricalBonePose(v2aAnimLat, sampledV2aPose))
        activeSkel->probeRayVsHistoricalPose(start, end, v1Transform, sampledV2aPose, pV2a);

    const anim::RaySkeletonProbe& pActive = (usedBoneHistory && v2BoneValid) ? pV2 : pV11;

    anim::RaySkeletonProbe pVan, p00, p05, p075, p10, pRaw05, p125, pUncl10, pNoDedup, pC17, pC33;
    struct ShadowProbeJob
    {
        const Mat4* mat;
        float lat;
        int pose;
        bool allowBones;
        anim::RaySkeletonProbe* out;
    };
    const ShadowProbeJob shadowJobs[11] = {
        { &vanMat,     vanLat,     vanPose,     false, &pVan     },
        { &pf00Mat,    pf00Lat,    pf00Pose,    false, &p00      },
        { &pf05Mat,    pf05Lat,    pf05Pose,    true,  &p05      },
        { &pf075Mat,   pf075Lat,   pf075Pose,   true,  &p075     },
        { &pf10Mat,    pf10Lat,    pf10Pose,    true,  &p10      },
        { &raw05Mat,   raw05Lat,   raw05Pose,   true,  &pRaw05   },
        { &pf125Mat,   pf125Lat,   pf125Pose,   true,  &p125     },
        { &uncl10Mat,  uncl10Lat,  uncl10Pose,  true,  &pUncl10  },
        { &nodedupMat, nodedupLat, nodedupPose, true,  &pNoDedup },
        { &c17Mat,     c17Lat,     c17Pose,     true,  &pC17     },
        { &c33Mat,     c33Lat,     c33Pose,     true,  &pC33     }
    };

    const bool canSwitchPose =
        !soldier->m_ragDoll && soldier->m_collisionSkeleton && gameLogic && gameLogic->isServer();
    bool switchedAnyPose = false;
    int savedPose = 0;
    int savedCharMode = 1;
    int savedTick = 0;
    int savedCachedPose = 0;
    Vec3 savedHeadPos = Vec3::getZero();
    float savedFloat230 = 0.0f;
    float savedLookIdle = 0.0f;
    bool savedByte27F1 = false;
    std::vector<Mat4> savedTransforms;
    anim::HistoricalBonePose swBones;

    for (int j = 0; j < 11; ++j)
    {
        if (shadowJobs[j].allowBones
            && shadowJobs[j].lat > kServerTickSec
            && soldierNode->sampleHistoricalBonePose(shadowJobs[j].lat, swBones))
        {
            skeleton->probeRayVsHistoricalPose(
                start, end, *shadowJobs[j].mat, swBones, *shadowJobs[j].out);
            continue;
        }

        const int effPose =
            (shadowJobs[j].pose >= 0 && shadowJobs[j].pose < 4) ? shadowJobs[j].pose : tgtLivePose;
        if (effPose != activeSkelPose && canSwitchPose)
        {
            if (!switchedAnyPose)
            {
                savedPose = soldier->m_pose;
                savedCharMode = soldier->m_characterMode;
                savedTick = soldier->m_dword26C;
                savedCachedPose = soldier->m_dword270;
                savedHeadPos = soldier->m_headPosition;
                savedFloat230 = soldier->m_float230;
                savedLookIdle = soldier->m_soldierLookIdleTime;
                savedByte27F1 = soldier->m_byte27F[1];
                if (soldier->m_collisionSkeleton->m_transforms
                    && soldier->m_collisionSkeleton->m_boneCount > 0)
                {
                    const int boneCount = soldier->m_collisionSkeleton->m_boneCount;
                    savedTransforms.assign(
                        soldier->m_collisionSkeleton->m_transforms,
                        soldier->m_collisionSkeleton->m_transforms + boneCount);
                }
                switchedAnyPose = true;
            }
            soldier->m_byte27F[1] = true;
            soldier->m_characterMode = 1;
            soldier->m_pose = effPose;
            soldier->m_headPosition = savedHeadPos;
            soldier->m_float230 = savedFloat230;
            soldier->m_soldierLookIdleTime = savedLookIdle;
            soldier->applyAnimationsOnSkeleton(0.0f, false);
            activeSkel = soldier->m_collisionSkeleton;
            activeSkelPose = effPose;
        }
        activeSkel->probeRayVsSkeleton(start, end, *shadowJobs[j].mat, *shadowJobs[j].out);
    }

    if (switchedAnyPose)
    {
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

    IPlayer* const tgtPlayer = soldier->getOccupingPlayer();
    const int tgtId = tgtPlayer ? tgtPlayer->getId() : -1;
    const std::string tgtName = tgtPlayer ? sanitizeField(tgtPlayer->getName().c_str()) : std::string();
    const int tgtAlive = (tgtPlayer && tgtPlayer->getIsAlive()) ? 1 : 0;
    const int tgtManDown = (tgtPlayer && tgtPlayer->getIsManDown()) ? 1 : 0;
    const unsigned int tgtPingMs = tgtPlayer ? tgtPlayer->getPing() : 0u;
    const int tgtPoseDiff = (v1StateIndex >= 0 && v1StateIndex != tgtLivePose) ? 1 : 0;

    const Vec3& vel = soldierNode->getPositionalSpeed();
    const float tgtSpeed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    const float tgtHSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);

    const Vec3& soldierPos = soldierObject->getAbsolutePosition();
    const float dxShot = soldierPos.x - shot.startPos.x;
    const float dyShot = soldierPos.y - shot.startPos.y;
    const float dzShot = soldierPos.z - shot.startPos.z;
    const float shotDist = std::sqrt(dxShot * dxShot + dyShot * dyShot + dzShot * dzShot);
    const float dxRay = end.x - start.x;
    const float dyRay = end.y - start.y;
    const float dzRay = end.z - start.z;
    const float rayLenM = std::sqrt(dxRay * dxRay + dyRay * dyRay + dzRay * dzRay);
    const int flightTicks = currentTick - shot.fireTick;
    const float sinceSpawnMs =
        ticksSinceReseed >= 0 ? static_cast<float>(ticksSinceReseed) * kServerTickSec * 1000.0f : -1.0f;

    const std::string v1BoneName =
        v1Hit ? sanitizeField(skeleton->getBoneNameSafe(v1HitBone)) : std::string();
    const float v1MissM = v1Hit ? 0.0f : (pActive.minSurfaceDist < 999.0f ? pActive.minSurfaceDist : 999.0f);

    char vanBuf[64], pf00Buf[48], pf05Buf[48], pf075Buf[48], pf10Buf[48];
    char raw05Buf[48], pf125Buf[48], uncl10Buf[48];
    char v2Buf[64], v11Buf[64], nodedupBuf[48], v2aBuf[48], c17Buf[48], c33Buf[48];
    formatVanillaFields(pVan, vanRingUnfilled, vanBuf, sizeof(vanBuf));
    formatProbeFields(p00, pf00Buf, sizeof(pf00Buf));
    formatProbeFields(p05, pf05Buf, sizeof(pf05Buf));
    formatProbeFields(p075, pf075Buf, sizeof(pf075Buf));
    formatProbeFields(p10, pf10Buf, sizeof(pf10Buf));
    formatProbeFields(pRaw05, raw05Buf, sizeof(raw05Buf));
    formatProbeFields(p125, pf125Buf, sizeof(pf125Buf));
    formatProbeFields(pUncl10, uncl10Buf, sizeof(uncl10Buf));
    formatFullProbeFields(pV2, v2Buf, sizeof(v2Buf));
    formatFullProbeFields(pV11, v11Buf, sizeof(v11Buf));
    formatProbeFields(pNoDedup, nodedupBuf, sizeof(nodedupBuf));
    formatProbeFields(pV2a, v2aBuf, sizeof(v2aBuf));
    formatProbeFields(pC17, c17Buf, sizeof(c17Buf));
    formatProbeFields(pC33, c33Buf, sizeof(c33Buf));

    int qMinWin = 0, qTarget = 1, qDrainDrops = 0, qCapDrops = 0;
    LatencyCompensation::getPlayerQueueStats(owner, qMinWin, qTarget, qDrainDrops, qCapDrops);
    int tickUsP50 = 0, tickUsP99 = 0;
    computeTickUsPercentiles(tickUsP50, tickUsP99);

    float v2VelResidM = 0.0f, v2DtErrMs = 0.0f;
    float dummyResid = 0.0f, v11DtErrMs = 0.0f, v2aDtErrMs = 0.0f, c17DtErrMs = 0.0f, c33DtErrMs = 0.0f;
    computeVelResidual(pV2, rewindVel, v2VelResidM, v2DtErrMs);
    computeVelResidual(pV11, rewindVel, dummyResid, v11DtErrMs);
    computeVelResidual(pV2a, rewindVel, dummyResid, v2aDtErrMs);
    computeVelResidual(pC17, c17Vel, dummyResid, c17DtErrMs);
    computeVelResidual(pC33, c33Vel, dummyResid, c33DtErrMs);

    char line[2560];
    std::snprintf(
        line, sizeof(line),
        "PROBE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        "%u,%d,%.2f,%.2f,%u,%u,"
        "%d,%.2f,%d,%d,%.2f,%.2f,"
        "%.2f,%d,%.4f,%d,%d,%d,"
        "%d,%d,%d,%d,%.3f,%.3f,%.3f,"
        "%d,%d,%.1f,"
        "%d,%d,%s,%d,%d,%.4f,%d,"
        "%s,%s,%s,%s,%s,%s,%s,%s,"
        "%d,%.2f,%u,%.3f,"
        "%d,%.4f,%.4f,"
        "%s,%s,%s,"
        "%d,%d,%d,%d,%d,%d,"
        "%.4f,%.2f,%.2f,"
        "%s,%.2f,"
        "%s,%.2f,"
        "%s,%.2f"
        ",,,,,,,,,,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        tgtId, tgtName.c_str(),
        probePingMs, shot.rawPingMs, shot.rttJitterMs, shot.pktLossDnPct,
        probeRawInterpMs, probeInterpMs,
        shot.queueDepth, probeQDelaySec * 1000.0f, shot.replayCount, shot.replays30t,
        activeLatency * 1000.0f, vanLat * 1000.0f,
        shotDist, flightTicks, coarseDist, flyBy ? 1 : 0, tgtAlive, tgtManDown,
        tgtLivePose, v1StateIndex, vanPose, tgtPoseDiff, tgtSpeed, tgtHSpeed, vel.y,
        targetInAir ? 1 : 0, ringValid, sinceSpawnMs,
        v1Hit ? 1 : 0, v1Hit ? v1HitBone : -1, v1BoneName.c_str(),
        v1Hit ? v1HitPart : 0, pActive.hitFirst ? pActive.firstBone : -1,
        v1MissM, pActive.nearestBone,
        vanBuf, pf00Buf, pf05Buf, pf075Buf, pf10Buf, raw05Buf, pf125Buf, uncl10Buf,
        usedBoneHistory ? 1 : 0, probeEffQDelaySec * 1000.0f, tgtPingMs, rayLenM,
        v2BoneValid, v2HeadShiftM, v2MaxBoneShiftM,
        v2Buf, v11Buf, nodedupBuf,
        qMinWin, qTarget, qDrainDrops, qCapDrops, tickUsP50, tickUsP99,
        v2VelResidM, v2DtErrMs, v11DtErrMs,
        v2aBuf, v2aDtErrMs,
        c17Buf, c17DtErrMs,
        c33Buf, c33DtErrMs);
    appendLine(line);

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
    std::snprintf(
        line, sizeof(line),
        "DELIVER,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        "%s,%s,%d,%.4f,%.4f,%.4f,%s,%d"
        ",,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        shot.pendingTargetId, shot.pendingTargetName.c_str(),
        outcome, blocker.c_str(), contactMaterial, marginM,
        tWallM, tSoldierM, objClass, eyeClear);
    appendLine(line);

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
    if (hitObject && hitObject->getTemplate()
        && hitObject->getTemplate()->getClassID() == CID_SoldierTemplate)
    {
        IPlayer* const tgtPlayer = static_cast<Soldier*>(hitObject)->getOccupingPlayer();
        if (tgtPlayer)
        {
            tgtId = tgtPlayer->getId();
            tgtName = sanitizeField(tgtPlayer->getName().c_str());
        }
    }

    char line[768];
    std::snprintf(
        line, sizeof(line),
        "DAMAGE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,,,,,,,,,,,,,"
        ",,,,,,,,"
        "%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
        WorldPref::getTickCount(), WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        tgtId, tgtName.c_str(),
        shot.matA, shot.matB, shot.baseDmg, shot.matMod, shot.angleMod,
        shot.hasServerDmg ? shot.finalDmg : 0.0f,
        shot.hasServerDmg ? shot.hpBefore : 0.0f,
        shot.hasServerDmg ? shot.hpAfter : 0.0f,
        (shot.hasServerDmg && shot.killed) ? 1 : 0,
        hitmarker ? 1 : 0);
    appendLine(line);

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
