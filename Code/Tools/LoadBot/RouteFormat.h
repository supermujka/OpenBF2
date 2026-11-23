#ifndef BF2BOT_ROUTE_FORMAT_H
#define BF2BOT_ROUTE_FORMAT_H

#include <cstdint>

namespace bf2bot {

// Lightweight 3D vector to avoid header circular dependencies with WireBitStream.h.
struct NavVec3 {
    float x;
    float y;
    float z;
};

enum {
    ROUTE_VERSION = 1u
};

inline const char* routeMagicBytes() {
    return "OBR1";
}

// Bitmask constants for RouteActionSample::actionFlags (matches server RouteActionRecorder).
enum ActionFlag {
    AF_FIRE      = 0x0001u,
    AF_ALT_FIRE  = 0x0002u,
    AF_SPRINT    = 0x0004u,
    AF_CROUCH    = 0x0008u,
    AF_PRONE     = 0x0010u,
    AF_JUMP      = 0x0020u,
    AF_RELOAD    = 0x0040u,
    AF_MOVE_FWD  = 0x0080u,
    AF_MOVE_BACK = 0x0100u,
    AF_STRAFE_L  = 0x0200u,
    AF_STRAFE_R  = 0x0400u,
    AF_WEAPON_SW = 0x0800u
};

// Soldier stance values in RouteActionSample::stance.
enum RouteStance {
    STANCE_STAND  = 0,
    STANCE_CROUCH = 1,
    STANCE_PRONE  = 2,
    STANCE_SWIM   = 3
};

// 32-byte file header at offset 0 of openbf2_routes.bin (little-endian).
struct RouteActionHeader {
    char magic[4];
    uint32_t version;
    uint32_t sampleHz;
    uint32_t recordSize;
    char mapName[16];
};

// 36-byte sample record following RouteActionHeader (little-endian).
struct RouteActionSample {
    uint32_t tick;
    uint16_t playerId;
    uint8_t team;
    uint8_t stance;
    float posX;
    float posY;
    float posZ;
    float yawDeg;
    float pitchDeg;
    uint32_t actionFlags;
    int16_t moveForward;
    int16_t moveStrafe;
};

static_assert(sizeof(RouteActionHeader) == 32, "RouteActionHeader must be 32 bytes");
static_assert(sizeof(RouteActionSample) == 36, "RouteActionSample must be 36 bytes");

} // namespace bf2bot

#endif // BF2BOT_ROUTE_FORMAT_H
