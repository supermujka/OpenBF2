// [CHOSEN, not recovered] Server-side binary route and action recorder for 64-player loadbot replay.
// Disabled by default (sv-record-routes == 0); buffered binary I/O when active.

#include "RouteActionRecorder.h"

#include "../Utility/Vars.h"
#include "../Common/PlayerAction.h"
#include "../GameLogic/ActionBuffer.h"
#include "../Objects/IObject.h"
#include "../Objects/IObjectTemplate.h"
#include "../Objects/IPlayer.h"
#include "../Objects/Game/Soldier.h"
#include "../../Core/Ids.h"
#include "../../Core/SettingsRepostitory.h"
#include "../../IO/Input/PlayerInput.h"
#include "../../Scene/Object/PlayerManager.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

int* RouteActionRecorder::s_enabledCvar = 0;
int* RouteActionRecorder::s_hzCvar      = 0;

namespace {

const char* const kDefaultRouteFile = "openbf2_routes.bin";
enum { kFlushThreshold = 1024 };

bool                           s_cvarsInitialized = false;
std::FILE*                     s_file             = 0;
std::string                    s_openPath;
std::uint32_t                  s_activeHz         = 5u;
std::uint32_t                  s_tickAccumulator  = 0u;
std::vector<RouteActionSample> s_buffer;

std::uint32_t effectiveSampleHz(const int* hzCvar)
{
    int hz = (hzCvar != 0) ? *hzCvar : 5;
    if (hz < 1)
        hz = 1;
    else if (hz > 30)
        hz = 30;
    return static_cast<std::uint32_t>(hz);
}

std::string currentRoutePath()
{
    const char* q = Vars::queryString("sv-record-routes-file");
    if (q != 0 && q[0] != '\0')
        return std::string(q);
    return std::string(kDefaultRouteFile);
}

std::int16_t clampAxis16(short v)
{
    if (v < -32767)
        return static_cast<std::int16_t>(-32767);
    return static_cast<std::int16_t>(v);
}

std::uint32_t extractActionFlags(const PlayerAction& act,
                                 std::int16_t moveForward,
                                 std::int16_t moveStrafe)
{
    std::uint32_t flags = 0u;
    const unsigned int lo = act.m_buttonMaskLow;
    const unsigned int hi = act.m_buttonMaskHigh;

    if (lo & (1u << (io::PIFire - 8)))
        flags |= RouteActionRecorder::ACTION_FIRE;
    if (lo & (1u << (io::PIAltFire - 8)))
        flags |= RouteActionRecorder::ACTION_ALT_FIRE;
    if (lo & ((1u << (io::PISprint - 8)) | (1u << (io::PIAltSprint - 8))))
        flags |= RouteActionRecorder::ACTION_SPRINT;
    if (lo & (1u << (io::PICrouch - 8)))
        flags |= RouteActionRecorder::ACTION_CROUCH;
    if (lo & (1u << (io::PILie - 8)))
        flags |= RouteActionRecorder::ACTION_PRONE;
    if (lo & (1u << (io::PIAction - 8)))
        flags |= RouteActionRecorder::ACTION_JUMP;
    if (lo & (1u << (io::PIReload - 8)))
        flags |= RouteActionRecorder::ACTION_RELOAD;

    if (moveForward > 0)
        flags |= RouteActionRecorder::ACTION_MOVE_FWD;
    else if (moveForward < 0)
        flags |= RouteActionRecorder::ACTION_MOVE_BACK;

    if (moveStrafe < 0)
        flags |= RouteActionRecorder::ACTION_STRAFE_L;
    else if (moveStrafe > 0)
        flags |= RouteActionRecorder::ACTION_STRAFE_R;

    const unsigned int kWeaponSelectLowMask =
        ((1u << (io::PIWeaponSelect9 - 8 + 1)) - 1u) ^ ((1u << (io::PIWeaponSelect1 - 8)) - 1u);
    const unsigned int kWeaponSelectHighMask =
        (1u << (io::PISelectPrimWeapon - 40)) |
        (1u << (io::PISelectSecWeapon - 40)) |
        (1u << (io::PIToggleWeapon - 40));
    if ((lo & kWeaponSelectLowMask) || (hi & kWeaponSelectHighMask))
        flags |= RouteActionRecorder::ACTION_WEAPON_SW;

    return flags;
}

void flushBuffer()
{
    if (s_file != 0 && !s_buffer.empty())
    {
        std::fwrite(&s_buffer[0], sizeof(RouteActionSample), s_buffer.size(), s_file);
    }
    s_buffer.clear();
}

void closeFile()
{
    flushBuffer();
    if (s_file != 0)
    {
        std::fclose(s_file);
        s_file = 0;
    }
    s_openPath.clear();
    s_tickAccumulator = 0u;
}

bool openFile(const std::string& path, std::uint32_t sampleHz)
{
    closeFile();

    s_file = std::fopen(path.c_str(), "wb");
    if (s_file == 0)
        return false;

    RouteActionHeader hdr;
    std::memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0]   = 'O';
    hdr.magic[1]   = 'B';
    hdr.magic[2]   = 'R';
    hdr.magic[3]   = '1';
    hdr.version    = 1u;
    hdr.sampleHz   = sampleHz;
    hdr.recordSize = static_cast<std::uint32_t>(sizeof(RouteActionSample));

    if (settings != 0)
    {
        const std::string map =
            settings->stringGet(std::string("GSLoadLevel"), std::string(""));
        std::size_t n = map.size();
        if (n > sizeof(hdr.mapName))
            n = sizeof(hdr.mapName);
        if (n > 0)
            std::memcpy(hdr.mapName, map.data(), n);
    }

    if (std::fwrite(&hdr, sizeof(hdr), 1u, s_file) != 1u)
    {
        std::fclose(s_file);
        s_file = 0;
        return false;
    }
    std::fflush(s_file);

    s_openPath        = path;
    s_activeHz        = sampleHz;
    s_tickAccumulator = (sampleHz < 30u) ? (30u - sampleHz) : 0u;
    if (s_buffer.capacity() < kFlushThreshold)
        s_buffer.reserve(kFlushThreshold);
    return true;
}

} // namespace

void RouteActionRecorder::initCVars()
{
    if (s_cvarsInitialized)
        return;
    s_cvarsInitialized = true;
    s_enabledCvar = Vars::getInt("sv-record-routes", 0);
    s_hzCvar      = Vars::getInt("sv-record-routes-hz", 5);
    Vars::getString("sv-record-routes-file", kDefaultRouteFile);
}

void RouteActionRecorder::recordStep(std::uint32_t simTick)
{
    if (!isEnabled())
    {
        if (s_file != 0)
            closeFile();
        return;
    }

    if (world::playerManager == 0)
        return;

    const std::uint32_t sampleHz = effectiveSampleHz(s_hzCvar);
    const std::string path = currentRoutePath();
    if (s_file == 0 || path != s_openPath)
    {
        if (!openFile(path, sampleHz))
            return;
    }
    else
    {
        s_activeHz = sampleHz;
    }

    s_tickAccumulator += s_activeHz;
    if (s_tickAccumulator < 30u)
        return;
    s_tickAccumulator -= 30u;

    const std::list<world::IPlayer*>& players = world::playerManager->getPlayers();
    for (std::list<world::IPlayer*>::const_iterator it = players.begin();
         it != players.end(); ++it)
    {
        world::IPlayer* const player = *it;
        if (player == 0 || !player->getIsAlive())
            continue;

        world::IObject* const veh = player->getVehicle();
        if (veh == 0)
            continue;

        const world::IObjectTemplate* const tpl = veh->getTemplate();
        if (tpl == 0 || tpl->getClassID() != world::CID_SoldierTemplate)
            continue;

        const world::Soldier* const soldier = static_cast<const world::Soldier*>(veh);
        const Vec3& pos = soldier->getAbsolutePosition();

        RouteActionSample sample;
        sample.tick     = simTick;
        const int pid   = player->getId();
        sample.playerId = static_cast<std::uint16_t>(pid >= 0 ? pid : 0);
        const int tm    = player->getTeam();
        sample.team     = static_cast<std::uint8_t>(tm >= 0 ? tm : 0);
        sample.stance   = static_cast<std::uint8_t>(soldier->getPose());
        sample.posX     = pos.x;
        sample.posY     = pos.y;
        sample.posZ     = pos.z;
        sample.yawDeg   = soldier->m_facingDegrees;
        sample.pitchDeg = soldier->m_lookUpDownAngle;

        PlayerAction action;
        bool haveAction = false;
        if (player->getIsAIPlayer())
        {
            const io::PlayerInput* const aiInput =
                static_cast<const io::PlayerInput*>(player->getAIPlayerInput());
            if (aiInput != 0)
            {
                action.set(*aiInput);
                haveAction = true;
            }
        }
        else
        {
            const ActionBuffer* const ab = player->getActionBuffer();
            if (ab != 0)
            {
                action = ab->m_current;
                haveAction = true;
            }
        }

        if (haveAction)
        {
            sample.moveForward = clampAxis16(action.m_axis[3]);
            sample.moveStrafe  = clampAxis16(action.m_axis[0]);
            sample.actionFlags = extractActionFlags(action, sample.moveForward, sample.moveStrafe);
        }
        else
        {
            sample.moveForward = 0;
            sample.moveStrafe  = 0;
            sample.actionFlags = 0u;
        }

        s_buffer.push_back(sample);
    }

    if (s_buffer.size() >= kFlushThreshold)
        flushBuffer();
}

void RouteActionRecorder::shutdown()
{
    closeFile();
}

} // namespace hfe
} // namespace dice
