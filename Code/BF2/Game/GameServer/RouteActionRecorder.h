// [CHOSEN, not recovered] Server-side binary route and action recorder for 64-player loadbot replay.
// Disabled by default (sv-record-routes == 0) with near-zero fast-path overhead.

#ifndef GAME_GAMESERVER_ROUTEACTIONRECORDER_H
#define GAME_GAMESERVER_ROUTEACTIONRECORDER_H

#include <cstdint>

namespace dice {
namespace hfe {

struct RouteActionHeader
{
    char          magic[4];    // {'O', 'B', 'R', '1'}
    std::uint32_t version;     // 1
    std::uint32_t sampleHz;    // clamped 1..30
    std::uint32_t recordSize;  // 36
    char          mapName[16]; // null-padded map name (zeros if unavailable)
};

struct RouteActionSample
{
    std::uint32_t tick;
    std::uint16_t playerId;
    std::uint8_t  team;
    std::uint8_t  stance;      // 0=Stand, 1=Crouch, 2=Prone, 3=Swim
    float         posX;
    float         posY;
    float         posZ;
    float         yawDeg;      // Soldier::m_facingDegrees
    float         pitchDeg;    // Soldier::m_lookUpDownAngle
    std::uint32_t actionFlags;
    std::int16_t  moveForward; // PlayerAction::m_axis[3], clamped [-32767, 32767]
    std::int16_t  moveStrafe;  // PlayerAction::m_axis[0], clamped [-32767, 32767]
};

static_assert(sizeof(RouteActionHeader) == 32, "RouteActionHeader must be 32 bytes");
static_assert(sizeof(RouteActionSample) == 36, "RouteActionSample must be 36 bytes");

class RouteActionRecorder
{
public:
    enum ActionFlag : std::uint32_t
    {
        ACTION_FIRE      = 0x0001u,
        ACTION_ALT_FIRE  = 0x0002u,
        ACTION_SPRINT    = 0x0004u,
        ACTION_CROUCH    = 0x0008u,
        ACTION_PRONE     = 0x0010u,
        ACTION_JUMP      = 0x0020u,
        ACTION_RELOAD    = 0x0040u,
        ACTION_MOVE_FWD  = 0x0080u,
        ACTION_MOVE_BACK = 0x0100u,
        ACTION_STRAFE_L  = 0x0200u,
        ACTION_STRAFE_R  = 0x0400u,
        ACTION_WEAPON_SW = 0x0800u
    };

    static void initCVars();
    static void recordStep(std::uint32_t simTick);
    static void shutdown();

    static inline bool isEnabled()
    {
        return s_enabledCvar != 0 && (*s_enabledCvar != 0);
    }

private:
    static int* s_enabledCvar;
    static int* s_hzCvar;
};

} // namespace hfe
} // namespace dice

#endif // GAME_GAMESERVER_ROUTEACTIONRECORDER_H
