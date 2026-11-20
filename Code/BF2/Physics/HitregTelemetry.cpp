// [CHOSEN, not recovered] Buffered CSV telemetry for Shadow A/B and pingFactor calibration.

#include "HitregTelemetry.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "LatencyCompensation.h"
#include "SoldierPhysicsNode.h"
#include "../Animation/BoneAnimation/Skeleton.h"
#include "../Core/Ids.h"
#include "../Core/System.h"
#include "../Game/Objects/IObject.h"
#include "../Game/Objects/IObjectTemplate.h"
#include "../Game/Objects/IPlayer.h"
#include "../Game/Objects/Game/Soldier.h"
#include "../Game/Utility/Vars.h"
#include "../Math/Mat4.h"
#include "../Math/Vec3.h"

namespace dice {
namespace hfe {
namespace world {

namespace {

int*         const g_telemetryLevel  = Vars::getInt("phy-latencyComp-telemetry", 0);
const char*  const g_telemetryFile   = Vars::getString("phy-latencyComp-telemetryFile", "openbf2_hitreg.csv");
int*         const g_telemetryFlush  = Vars::getInt("phy-latencyComp-telemetryFlushTicks", 30);

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
    }
};

struct TelemetryLoggerState
{
    FILE*       file;
    std::string activePath;
    std::string buffer;
    std::string lastConfigSig;
    int         lastFlushTick;
    std::uint64_t nextShotId;
    bool        atexitRegistered;

    TelemetryLoggerState()
        : file(0)
        , lastFlushTick(0)
        , nextShotId(1)
        , atexitRegistered(false)
    {
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
    "outcome,blocker,blocker_mat,override_margin_m,"
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
        st.lastConfigSig.clear();
    }
}

void onProcessExit()
{
    closeTelemetryFile();
}

std::string sanitizeField(const char* raw)
{
    if (!raw || !*raw)
        return std::string();
    std::string out;
    out.reserve(32);
    for (const char* p = raw; *p; ++p)
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

float clampRewindLatency(float sec)
{
    if (sec < 0.0f)
        return 0.0f;
    const float maxLat = LatencyCompensation::getMaxLatency();
    return sec > maxLat ? maxLat : sec;
}

float computeVanillaLatencySec(IPlayer* owner)
{
    const float dbg = LatencyCompensation::getDebugLatency();
    if (dbg > 0.0f)
        return clampRewindLatency(dbg);
    if (!LatencyCompensation::m_isEnabled || !owner)
        return 0.0f;

    float lat = 0.0f;
    if (owner->getIsRemote())
    {
        lat = static_cast<float>(owner->getPing()) * LatencyCompensation::getPingFactor() * 0.001f;
        if (LatencyCompensation::isUseClientWindowEnabled())
            lat += static_cast<float>(owner->getInterpolationTime()) * 0.001f;
        else
            lat += LatencyCompensation::getDefaultLatency();
    }
    else
    {
        lat = LatencyCompensation::getLocalLatency(owner);
    }
    return clampRewindLatency(lat);
}

float computeSweepLatencySec(IPlayer* owner, float pingFactorOverride, int pingMsOverride,
                             float queueDelaySec)
{
    const float dbg = LatencyCompensation::getDebugLatency();
    if (dbg > 0.0f)
        return clampRewindLatency(dbg);
    if (!LatencyCompensation::m_isEnabled || !owner)
        return 0.0f;
    if (!owner->getIsRemote())
        return clampRewindLatency(LatencyCompensation::getLocalLatency(owner));

    const int pingVal = pingMsOverride >= 0 ? pingMsOverride : static_cast<int>(owner->getPing());
    float lat = static_cast<float>(pingVal) * pingFactorOverride * 0.001f;
    if (LatencyCompensation::isUseClientWindowEnabled())
    {
        const unsigned int interp =
            LatencyCompensation::clampClientInterpolationTime(owner->getInterpolationTime());
        lat += static_cast<float>(interp) * 0.001f;
    }
    else
    {
        lat += LatencyCompensation::getDefaultLatency();
    }
    if (LatencyCompensation::isQueueDelayEnabled())
        lat += queueDelaySec;

    return clampRewindLatency(lat);
}

void buildConfigSignature(char* buf, std::size_t bufSize)
{
    std::snprintf(
        buf, bufSize,
        "improved=%d;pf=%.2f;max=%.2f;maxInterp=%d;qDelay=%d;maxQ=%d;"
        "snap=%d;closest=%d;spawnFix=%d;bpFloor=%.1f;tel=%d",
        LatencyCompensation::isImprovedEnabled() ? 1 : 0,
        LatencyCompensation::getPingFactor(),
        LatencyCompensation::getMaxLatency(),
        LatencyCompensation::getMaxClientInterpMs(),
        LatencyCompensation::isQueueDelayEnabled() ? 1 : 0,
        LatencyCompensation::getMaxQueueTicks(),
        LatencyCompensation::isSnapshotProjectileEnabled() ? 1 : 0,
        LatencyCompensation::isClosestBoneEnabled() ? 1 : 0,
        LatencyCompensation::isSpawnRingFixEnabled() ? 1 : 0,
        LatencyCompensation::getBroadphaseSoldierSpeedFloor(),
        *g_telemetryLevel);
}

void ensureTelemetryOpenAndConfigLogged()
{
    if (*g_telemetryLevel <= 0)
        return;

    TelemetryLoggerState& st = loggerState();
    if (!st.atexitRegistered)
    {
        std::atexit(onProcessExit);
        st.atexitRegistered = true;
    }

    const char* qPath = Vars::queryString("phy-latencyComp-telemetryFile");
    if (!qPath || !*qPath)
        qPath = g_telemetryFile && *g_telemetryFile ? g_telemetryFile : "openbf2_hitreg.csv";
    const std::string targetPath(qPath);

    if (st.file && st.activePath != targetPath)
        closeTelemetryFile();

    if (!st.file)
    {
        st.file = std::fopen(targetPath.c_str(), "w");
        if (!st.file)
            return;
        st.activePath = targetPath;
        st.lastFlushTick = WorldPref::getTickCount();
        st.buffer.append(kCsvHeader);
    }

    char sig[256];
    buildConfigSignature(sig, sizeof(sig));
    if (st.lastConfigSig != sig)
    {
        st.lastConfigSig = sig;
        char line[512];
        std::snprintf(
            line, sizeof(line),
            "CONFIG,%d,%.3f,,,,,,,,,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,%s,,,,,,,,,,,,\n",
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

    if (*g_telemetryLevel >= 2 && st.file)
    {
        char line[768];
        std::snprintf(
            line, sizeof(line),
            "FIRE,%d,%.3f,%llu,%s,%d,%s,,,"
            "%u,%d,%.2f,%.2f,%u,%u,"
            "%d,%.2f,%d,%d,%.2f,%.2f"
            ",,,,,,,,,,,,,,,,,,,,,,,,,,"
            ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,\n",
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
                                         bool flyBy, float coarseDist, bool targetInAir)
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

    const int currentTick = WorldPref::getTickCount();
    const float qDelaySec = LatencyCompensation::getPlayerQueueDelay(owner);
    const float vanLat = computeVanillaLatencySec(owner);
    const float pf00Lat = 0.0f;
    const float pf05Lat = computeSweepLatencySec(owner, 0.50f, -1, qDelaySec);
    const float pf075Lat = computeSweepLatencySec(owner, 0.75f, -1, qDelaySec);
    const float pf10Lat = computeSweepLatencySec(owner, 1.00f, -1, qDelaySec);
    const float raw05Lat = computeSweepLatencySec(
        owner, 0.50f, shot.rawPingMs > 0 ? shot.rawPingMs : -1, qDelaySec);

    int ringValid = 0;
    int ticksSinceReseed = -1;
    int dummyPose = -1;
    Mat4 tmpMat = Mat4::getIdentity();
    soldierNode->sampleLatencyTransform(activeLatency, false, tmpMat, &dummyPose,
                                        &ringValid, &ticksSinceReseed);

    Mat4 vanMat = Mat4::getIdentity();
    int vanPose = -1;
    int vanValid = 0;
    int vanSince = -1;
    soldierNode->sampleLatencyTransform(vanLat, true, vanMat, &vanPose, &vanValid, &vanSince);
    int vanRingUnfilled = 0;
    if (vanLat > kServerTickSec)
    {
        const int needed = static_cast<int>(vanLat / kServerTickSec) + 1;
        if (vanValid < needed)
            vanRingUnfilled = 1;
    }

    Mat4 pf00Mat = Mat4::getIdentity();
    Mat4 pf05Mat = Mat4::getIdentity();
    Mat4 pf075Mat = Mat4::getIdentity();
    Mat4 pf10Mat = Mat4::getIdentity();
    Mat4 raw05Mat = Mat4::getIdentity();
    soldierNode->sampleLatencyTransform(pf00Lat, false, pf00Mat, 0, 0, 0);
    soldierNode->sampleLatencyTransform(pf05Lat, false, pf05Mat, 0, 0, 0);
    soldierNode->sampleLatencyTransform(pf075Lat, false, pf075Mat, 0, 0, 0);
    soldierNode->sampleLatencyTransform(pf10Lat, false, pf10Mat, 0, 0, 0);
    soldierNode->sampleLatencyTransform(raw05Lat, false, raw05Mat, 0, 0, 0);

    anim::RaySkeletonProbe pV1, pVan, p00, p05, p075, p10, pRaw05;
    skeleton->probeRayVsSkeleton(start, end, v1Transform, pV1);
    skeleton->probeRayVsSkeleton(start, end, vanMat, pVan);
    skeleton->probeRayVsSkeleton(start, end, pf00Mat, p00);
    skeleton->probeRayVsSkeleton(start, end, pf05Mat, p05);
    skeleton->probeRayVsSkeleton(start, end, pf075Mat, p075);
    skeleton->probeRayVsSkeleton(start, end, pf10Mat, p10);
    skeleton->probeRayVsSkeleton(start, end, raw05Mat, pRaw05);

    Soldier* const soldier = static_cast<Soldier*>(soldierObject);
    IPlayer* const tgtPlayer = soldier->getOccupingPlayer();
    const int tgtId = tgtPlayer ? tgtPlayer->getId() : -1;
    const std::string tgtName = tgtPlayer ? sanitizeField(tgtPlayer->getName().c_str()) : std::string();
    const int tgtAlive = (tgtPlayer && tgtPlayer->getIsAlive()) ? 1 : 0;
    const int tgtManDown = (tgtPlayer && tgtPlayer->getIsManDown()) ? 1 : 0;
    const int tgtLivePose = static_cast<int>(soldier->getPose());
    const int tgtPoseDiff = (v1StateIndex >= 0 && v1StateIndex != tgtLivePose) ? 1 : 0;

    const Vec3& vel = soldierNode->getPositionalSpeed();
    const float tgtSpeed = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    const float tgtHSpeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);

    const Vec3& soldierPos = soldierObject->getAbsolutePosition();
    const float dxShot = soldierPos.x - shot.startPos.x;
    const float dyShot = soldierPos.y - shot.startPos.y;
    const float dzShot = soldierPos.z - shot.startPos.z;
    const float shotDist = std::sqrt(dxShot * dxShot + dyShot * dyShot + dzShot * dzShot);
    const int flightTicks = currentTick - shot.fireTick;
    const float sinceSpawnMs =
        ticksSinceReseed >= 0 ? static_cast<float>(ticksSinceReseed) * kServerTickSec * 1000.0f : -1.0f;

    const std::string v1BoneName =
        v1Hit ? sanitizeField(skeleton->getBoneNameSafe(v1HitBone)) : std::string();
    const float v1MissM = v1Hit ? 0.0f : (pV1.minSurfaceDist < 999.0f ? pV1.minSurfaceDist : 999.0f);

    char vanBuf[64], pf00Buf[48], pf05Buf[48], pf075Buf[48], pf10Buf[48], raw05Buf[48];
    formatVanillaFields(pVan, vanRingUnfilled, vanBuf, sizeof(vanBuf));
    formatProbeFields(p00, pf00Buf, sizeof(pf00Buf));
    formatProbeFields(p05, pf05Buf, sizeof(pf05Buf));
    formatProbeFields(p075, pf075Buf, sizeof(pf075Buf));
    formatProbeFields(p10, pf10Buf, sizeof(pf10Buf));
    formatProbeFields(pRaw05, raw05Buf, sizeof(raw05Buf));

    char line[1280];
    std::snprintf(
        line, sizeof(line),
        "PROBE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        "%u,%d,%.2f,%.2f,%u,%u,"
        "%d,%.2f,%d,%d,%.2f,%.2f,"
        "%.2f,%d,%.4f,%d,%d,%d,"
        "%d,%d,%d,%d,%.3f,%.3f,%.3f,"
        "%d,%d,%.1f,"
        "%d,%d,%s,%d,%d,%.4f,%d,"
        "%s,%s,%s,%s,%s,%s,"
        ",,,,,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        tgtId, tgtName.c_str(),
        owner->getPing(), shot.rawPingMs, shot.rttJitterMs, shot.pktLossDnPct,
        owner->getInterpolationTime(),
        LatencyCompensation::clampClientInterpolationTime(owner->getInterpolationTime()),
        shot.queueDepth, qDelaySec * 1000.0f, shot.replayCount, shot.replays30t,
        activeLatency * 1000.0f, vanLat * 1000.0f,
        shotDist, flightTicks, coarseDist, flyBy ? 1 : 0, tgtAlive, tgtManDown,
        tgtLivePose, v1StateIndex, vanPose, tgtPoseDiff, tgtSpeed, tgtHSpeed, vel.y,
        targetInAir ? 1 : 0, ringValid, sinceSpawnMs,
        v1Hit ? 1 : 0, v1Hit ? v1HitBone : -1, v1BoneName.c_str(),
        v1Hit ? v1HitPart : 0, pV1.hitFirst ? pV1.firstBone : -1,
        v1MissM, pV1.nearestBone,
        vanBuf, pf00Buf, pf05Buf, pf075Buf, pf10Buf, raw05Buf);
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
        }
    }
}

void HitregTelemetry::noteProjectileContactDelivered(const IObject* projectileObject,
                                                     IObject* contactObject,
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

    if (contactObject != shot.pendingTarget)
    {
        const float winnerDist = contactDepthSq >= 0.0f ? std::sqrt(contactDepthSq) : 0.0f;
        const float soldierDist = shot.pendingDepthSq >= 0.0f ? std::sqrt(shot.pendingDepthSq) : 0.0f;
        marginM = winnerDist - soldierDist;

        if (!contactObject)
        {
            outcome = "OVERRIDE_TERRAIN";
            blocker = "TERRAIN";
        }
        else
        {
            const IObjectTemplate* tmpl = contactObject->getTemplate();
            if (tmpl && tmpl->getClassID() == CID_SoldierTemplate)
                outcome = "OVERRIDE_SOLDIER";
            else
                outcome = "OVERRIDE_WALL";
            blocker = tmpl ? sanitizeField(tmpl->getName().c_str()) : std::string("OBJECT");
        }
    }

    char line[640];
    std::snprintf(
        line, sizeof(line),
        "DELIVER,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"
        "%s,%s,%d,%.4f,,,,,,,,,,\n",
        currentTick, WorldPref::getWorldTime(),
        static_cast<unsigned long long>(shot.shotId),
        shot.weaponName.c_str(), shot.shooterId, shot.shooterName.c_str(),
        shot.pendingTargetId, shot.pendingTargetName.c_str(),
        outcome, blocker.c_str(), contactMaterial, marginM);
    appendLine(line);

    shot.pendingTarget = 0;
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

    char line[640];
    std::snprintf(
        line, sizeof(line),
        "DAMAGE,%d,%.3f,%llu,%s,%d,%s,%d,%s,"
        ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,"
        ",,,,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d\n",
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

    ensureTelemetryOpenAndConfigLogged();
    if (!st.file)
        return;

    const int currentTick = WorldPref::getTickCount();
    const int flushEvery = *g_telemetryFlush > 0 ? *g_telemetryFlush : 30;
    if (currentTick - st.lastFlushTick >= flushEvery || currentTick < st.lastFlushTick)
    {
        flushBufferToDisk(true);
        st.lastFlushTick = currentTick;
    }
}

} // namespace world
} // namespace hfe
} // namespace dice
