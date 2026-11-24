// [CHOSEN, not recovered] Non-intrusive hit-registration telemetry, Shadow A/B comparison,
// and pingFactor sweep logger for OpenBF2 Version 1 calibration. Zero ABI/layout impact.

#ifndef PHYSICS_HITREGTELEMETRY_H
#define PHYSICS_HITREGTELEMETRY_H

#include "../Math/Vec3.h"  // Vec3 is a typedef (Math::Vec3), not a class

namespace dice {
namespace anim {
class Skeleton;
struct HistoricalBonePose;
}
namespace hfe {

struct Mat4;

namespace world {

class IObject;
class IPlayer;
class SoldierPhysicsNode;

class HitregTelemetry
{
public:
    static bool isEnabled();
    static int  getLevel();

    // Per-player network & simulation queue samplers.
    static void notePacketDelivery(const IPlayer* player, bool delivered, int instantPingMs);
    static void notePlayerSimTick(const IPlayer* player, int queueDepth,
                                  int replayCount, bool replayed);
    static void noteSnapshotSent(const IPlayer* player, int serverTick);
    static void notePlayerInputConsumed(const IPlayer* player, bool fireHeld);
    static void notePlayerInputDropped(const IPlayer* player, bool fireHeld, bool isDrainDrop);
    static void forgetPlayer(const IPlayer* player);

    // Per-shot lifecycle.
    static void noteShotFired(const IObject* projectile, IPlayer* owner,
                              const char* weaponName, const Vec3& startPos,
                              const Vec3& launchVel);
    // [CHOSEN, not recovered] Called by GameServer after the player's action is chosen.
    // replayed=true: no fresh client action (clientInputIdx is the last consumed one, unused).
    static void notePlayerActionExecuted(const IPlayer* player, int clientInputIdx, bool replayed);
    struct ShotDeviationInfo
    {
        int   fireInputIdx;     // game->getCurrentInputIndex() inside DeviationComp::postFire
        int   tableIdx;         // -1 when no table sample was drawn
        float sampleX;
        float sampleY;
        int   projectileIndex;
        bool  dummy;
        // Deviation terms at fire time (SoldierDeviationComp only; hasComponents=false otherwise).
        bool  hasComponents;
        float devMin;
        float devSpeed;
        float devTurn;
        float devMisc;
        float devFire;      // m_fireDeviation before this shot's fireDevAdd
        float devPoseMod;
        float devZoomMod;
        int   shPose;
        bool  shZoomed;
        float shSpeedMs;
        float shTurnRate;
    };
    static void noteShotDeviation(const IObject* projectile, const Vec3& deviatedVel,
                                  float deviationPct, const ShotDeviationInfo& info);
    static void finalizeShotFired(const IObject* projectile);
    static void forgetProjectile(const IObject* projectile);

    // Read-only Shadow A/B + pingFactor sweep probe (hits and near-misses <= 2.0 m).
    static void recordSoldierProbe(IObject* projectileObject, IPlayer* owner,
                                   IObject* soldierObject,
                                   const SoldierPhysicsNode* soldierNode,
                                   const anim::Skeleton* skeleton,
                                   const Vec3& start, const Vec3& end,
                                   float activeLatency, int v1StateIndex,
                                   const Mat4& v1Transform, bool v1Hit,
                                   int v1HitBone, int v1HitPart, const Vec3& v1HitPos,
                                   bool flyBy, float coarseDist, bool targetInAir,
                                   bool usedBoneHistory,
                                   const anim::HistoricalBonePose* activeBonePose);

    // Contact arbitration (delivered vs wall/terrain/other-soldier override).
    static void noteProjectileContactDelivered(const IObject* projectileObject,
                                               IObject* contactObject,
                                               const Vec3& contactPoint,
                                               float contactDepthSq,
                                               int contactMaterial);

    // Server main-loop tick duration sampler (microseconds).
    static void noteServerTickUs(int tickUs);

    // Confirmed damage & multipliers.
    static void noteProjectileDamageCalc(const IObject* projectileObject,
                                         int matA, int matB, float baseDamage,
                                         float matMod, float angleMod);
    static void noteServerGiveDamage(const IObject* sourceObject,
                                     float finalDamage, float hpBefore,
                                     float hpAfter, bool killed);
    static void finishProjectileCollisionDamage(const IObject* projectileObject,
                                                IObject* hitObject,
                                                bool hitmarker);

    // Periodic buffered flush & cvar config change tracker (called from GameServer::update).
    static void tickFlush();

    // Match lifecycle hooks for automatic per-match CSV file rotation.
    static void noteMatchStarted(const char* mapName);
    static void noteMatchEnded();
};

} // namespace world
} // namespace hfe
} // namespace dice

#endif // PHYSICS_HITREGTELEMETRY_H
