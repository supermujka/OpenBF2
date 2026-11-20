// [CHOSEN, not recovered] Non-intrusive hit-registration telemetry, Shadow A/B comparison,
// and pingFactor sweep logger for OpenBF2 Version 1 calibration. Zero ABI/layout impact.

#ifndef PHYSICS_HITREGTELEMETRY_H
#define PHYSICS_HITREGTELEMETRY_H

namespace dice {
namespace anim {
class Skeleton;
}
namespace hfe {

struct Mat4;
struct Vec3;

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
    static void forgetPlayer(const IPlayer* player);

    // Per-shot lifecycle.
    static void noteShotFired(const IObject* projectile, IPlayer* owner,
                              const char* weaponName, const Vec3& startPos, float muzzleSpeed);
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
                                   bool flyBy, float coarseDist, bool targetInAir);

    // Contact arbitration (delivered vs wall/terrain/other-soldier override).
    static void noteProjectileContactDelivered(const IObject* projectileObject,
                                               IObject* contactObject,
                                               float contactDepthSq,
                                               int contactMaterial);

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
};

} // namespace world
} // namespace hfe
} // namespace dice

#endif // PHYSICS_HITREGTELEMETRY_H
