// [CHOSEN, not recovered] Immutable per-tick world & networkable state snapshot for lock-free
// multi-threaded 32v32 ghost scoping and bit-exact packet encoding (Stage A -> Stage B -> Stage C).

#include "WorldNetSnapshot.h"

#include "../../IGameLogic.h"
#include "../../Objects/IObject.h"
#include "../../Objects/IObjectTemplate.h"
#include "../../Objects/IPlayer.h"
#include "../../Objects/IWeaponObject.h"
#include "../../Objects/ObjectLayoutAccess.h"
#include "../../Objects/Game/Soldier.h"
#include "../../Objects/Game/SoldierTemplate.h"
#include "../../Objects/Game/CommanderToolsManager.h"
#include "../../Objects/GameLogicComp/ArmorGLComp.h"
#include "../../Objects/GameLogicComp/IArmorGLComp.h"
#include "../../GameLogic/ScoreSystem/ScoreManager.h"
#include "../../GameLogic/SpawnSystem/SpawnGroup.h"
#include "../../../Scene/Object/PlayerManager.h"
#include "../../../Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#include "../../../Physics/IPhysicsNode.h"
#include "../../../Animation/BoneAnimation/RagDoll.h"
#include "../../../IO/Network/NetworkableInfo.h"
#include "../../../Core/Ids.h"

namespace dice {
namespace hfe {

extern IGameLogic* gameLogic;
namespace world { extern PlayerManager* playerManager; }

namespace netpipeline {

namespace {

int remoteControlledObjectTeamSnap(world::IObject* object)
{
    world::IObject* const root = world::getRootParent(object);
    if (!root)
        return 0;
    world::IPlayerControlObject* const pco = static_cast<world::IPlayerControlObject*>(
        root->queryInterface(world::IID_IPlayerControlObject));
    return pco ? pco->getTeam() : 0;
}

int remoteControlledObjectStateSnap(world::IObject* object)
{
    const char* const templ = reinterpret_cast<const char*>(
        world::object_detail::LayoutAccess::objectTemplate(object));
    return templ ? *reinterpret_cast<const int*>(templ + 0x180) : 0;
}

} // namespace

unsigned int WorldNetSnapshot::captureNetworkable(io::INetworkable* networkable)
{
    typedef world::object_detail::LayoutAccess L;

    NetRecordSnap meta;
    meta.liveNetworkable = networkable;
    if (!networkable)
        return addRecordSnap(meta);

    if (const NetRecordSnap* existing = findRecord(networkable))
        return static_cast<unsigned int>(existing - &m_records[0]);

    meta.networkableId        = networkable->m_networkableId;
    meta.ghostStateMask       = networkable->getGhostStateMask();
    meta.minimumBitSize       = networkable->getMinimumBitSize();
    meta.hasNetworkableObject = (networkable->m_networkableObject != 0);
    meta.skipFlag20           = (networkable->m_networkableObject != 0
        && networkable->m_networkableObject->m_object != 0
        && (networkable->m_networkableObject->m_object->getFlags() & 0x20) != 0);
    meta.forceNetworkableId   = (networkable->m_networkableInfo != 0
        && networkable->m_networkableInfo->getForceNetworkableId());

    // 1. SoldierNetworkable
    if (SoldierNetworkable* const soldierNet = dynamic_cast<SoldierNetworkable*>(networkable))
    {
        const int head = soldierNet->getHeadSlot();
        const SoldierNetworkable::UPDATE* const rec =
            soldierNet->getUpdate(static_cast<unsigned int>(head));
        world::Soldier* const soldier = static_cast<world::Soldier*>(soldierNet->m_object);
        if (rec && soldier)
        {
            SoldierRecordSnap s;
            s.rec = *rec;
            anim::RagDoll* const ragdoll = soldier->m_ragDoll;
            s.isAwakeOccupiedRagdoll =
                (ragdoll != 0 && ragdoll->m_awake && soldier->isOccupied());
            if (!s.isAwakeOccupiedRagdoll)
                s.rec.ammoStatus &= 0xF;
            s.hasArmor = (L::armorComponent(soldier) != 0);

            const world::IObjectTemplate* const templ = soldier->getTemplate();
            s.inventorySize = templ
                ? static_cast<const world::SoldierTemplate*>(templ)->getInventorySize()
                : 0;

            s.ragdollParticleStart = static_cast<unsigned int>(m_ragdollParticles.size());
            s.ragdollParticleCount = 0;
            if (s.isAwakeOccupiedRagdoll && ragdoll)
            {
                for (std::size_t i = 0; i < ragdoll->m_particles.size(); ++i)
                {
                    RagDollParticleSnap pSnap;
                    pSnap.netControlled = ragdoll->m_particles[i].m_netControlled;
                    pSnap.position      = ragdoll->m_particles[i].m_position;
                    m_ragdollParticles.push_back(pSnap);
                    ++s.ragdollParticleCount;
                }
            }

            meta.type            = NetEncType::Soldier;
            meta.typeRecordIndex = static_cast<unsigned int>(m_soldierRecords.size());
            m_soldierRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 2. ItemNetworkable
    if (ItemNetworkable* const itemNet = dynamic_cast<ItemNetworkable*>(networkable))
    {
        const int head = itemNet->getHeadSlot();
        if (itemNet->m_records && head >= 0 && head < 4)
        {
            ItemRecordSnap s;
            s.rec                = itemNet->m_records[head];
            meta.type            = NetEncType::Item;
            meta.typeRecordIndex = static_cast<unsigned int>(m_itemRecords.size());
            m_itemRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 3. KitNetworkable
    if (dynamic_cast<KitNetworkable*>(networkable) != 0)
    {
        meta.type            = NetEncType::Kit;
        meta.typeRecordIndex = 0;
        return addRecordSnap(meta);
    }

    // 4. GenericProjectileNetworkable
    if (GenericProjectileNetworkable* const projNet =
            dynamic_cast<GenericProjectileNetworkable*>(networkable))
    {
        const int head = projNet->getHeadSlot();
        if (projNet->m_records && head >= 0 && head < 4)
        {
            GenericProjectileRecordSnap s;
            s.rec = projNet->m_records[head];
            world::ArmorGLComp* const armor = projNet->m_object
                ? static_cast<world::ArmorGLComp*>(L::armorComponent(projNet->m_object))
                : 0;
            s.hasArmor = (armor != 0);
            if (armor)
            {
                s.armorMaxWreckHp = static_cast<int>(armor->getMaxWreckHitPoints());
                s.armorLower      = static_cast<int>(-armor->getMaxWreckHitPoints()) + s.armorMaxWreckHp;
                s.armorUpper      = static_cast<int>(armor->getMaxHitPoints()) + s.armorMaxWreckHp;
            }
            else
            {
                s.armorMaxWreckHp = 0;
                s.armorLower      = 0;
                s.armorUpper      = 0;
            }
            meta.type            = NetEncType::GenericProjectile;
            meta.typeRecordIndex = static_cast<unsigned int>(m_projectileRecords.size());
            m_projectileRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 5. ControlPointNetworkable (reads m_baselines[m_next] without setting m_next = m_head!)
    if (ControlPointNetworkable* const cpNet = dynamic_cast<ControlPointNetworkable*>(networkable))
    {
        const int nextSlot = cpNet->getNextSlot();
        const ControlPointNetworkable::BaseLine* const base = cpNet->getBaselineAt(nextSlot);
        if (base)
        {
            ControlPointRecordSnap s;
            s.rec                = *base;
            meta.type            = NetEncType::ControlPoint;
            meta.typeRecordIndex = static_cast<unsigned int>(m_controlPointRecords.size());
            m_controlPointRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 6. SimpleObjectNetworkable (reads m_records[m_next] without setting m_next = m_head!)
    if (SimpleObjectNetworkable* const simpleNet =
            dynamic_cast<SimpleObjectNetworkable*>(networkable))
    {
        const int nextSlot = simpleNet->getNextSlot();
        if (simpleNet->m_records && nextSlot >= 0 && nextSlot < 4)
        {
            SimpleObjectRecordSnap s;
            s.rec = simpleNet->m_records[nextSlot];

            world::IObject* const obj = simpleNet->m_object;
            world::IPhysicsNode* const phys = obj
                ? static_cast<world::IPhysicsNode*>(L::physicsNode(obj))
                : 0;
            s.isMobilePhysics = (phys != 0 && phys->getIsMobile());

            world::IArmorGLComp* const armor = obj
                ? static_cast<world::IArmorGLComp*>(L::armorComponent(obj))
                : 0;
            s.hasArmor = (armor != 0);
            if (armor)
            {
                s.armorMaxWreckHp = static_cast<int>(armor->getMaxWreckHitPoints());
                s.armorLower      = static_cast<int>(-armor->getMaxWreckHitPoints()) + s.armorMaxWreckHp;
                s.armorUpper      = static_cast<int>(armor->getMaxHitPoints()) + s.armorMaxWreckHp;
            }
            else
            {
                s.armorMaxWreckHp = 0;
                s.armorLower      = 0;
                s.armorUpper      = 0;
            }

            world::IPlayerControlObject* pco = 0;
            for (world::IObject* o = obj; o; o = o->getParent())
            {
                pco = static_cast<world::IPlayerControlObject*>(
                    o->queryInterface(world::IID_IPlayerControlObject));
                if (pco)
                    break;
            }
            s.hasPco = (pco != 0);
            s.isParachute = s.hasPco && obj && L::objectTemplate(obj)
                && (L::objectTemplate(obj)->getClassID() == world::CID_ParachuteTemplate);

            meta.type            = NetEncType::SimpleObject;
            meta.typeRecordIndex = static_cast<unsigned int>(m_simpleObjectRecords.size());
            m_simpleObjectRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 7. Player (reads m_netUpdateBuffer[m_head] directly)
    if (Player* const player = dynamic_cast<Player*>(networkable))
    {
        const int head = player->getHeadSlot();
        const NetUpdateSlot* const slot = player->getNetUpdateSlotAt(head);
        if (slot)
        {
            PlayerRecordSnap s;
            s.slot        = *slot;
            s.isConnected = player->getIsConnected();
            s.weaponUsesHeat[0] = false;
            s.weaponUsesHeat[1] = false;
            s.weaponUsesHeat[2] = false;

            world::IPlayerControlObject* pco = 0;
            if (player->getVehicle())
                pco = player->getWeaponVehicle();
            if (pco)
            {
                for (int w = 0; w <= 2; ++w)
                {
                    world::IObject* const weapon = pco->getActiveWeapon(w);
                    if (weapon)
                    {
                        world::IWeaponObject* const weaponInterface =
                            static_cast<world::IWeaponObject*>(
                                weapon->queryInterface(world::IID_IWeaponObject));
                        if (weaponInterface && weaponInterface->getUsesHeat())
                            s.weaponUsesHeat[w] = true;
                    }
                }
            }

            meta.type            = NetEncType::Player;
            meta.typeRecordIndex = static_cast<unsigned int>(m_playerRecords.size());
            m_playerRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 8. ScoreManager
    if (world::ScoreManager* const scoreMgr = dynamic_cast<world::ScoreManager*>(networkable))
    {
        const int head = scoreMgr->getHeadSlot();
        const unsigned char* const rec = scoreMgr->getScoreRecordAt(head);
        if (rec)
        {
            ScoreManagerRecordSnap s;
            std::memcpy(s.record, rec, 164);
            meta.type            = NetEncType::ScoreManager;
            meta.typeRecordIndex = static_cast<unsigned int>(m_scoreManagerRecords.size());
            m_scoreManagerRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    // 9. SpawnGroup
    if (SpawnGroup* const spawnGrp = dynamic_cast<SpawnGroup*>(networkable))
    {
        const int head = spawnGrp->getHeadSlot();
        SpawnGroupRecordSnap s;
        if (spawnGrp->getBaselineFieldsAt(head, s.team, s.active, s.onlyForAI,
                                          s.hasSpawnPoints, s.posX, s.posZ, s.squad))
        {
            meta.type            = NetEncType::SpawnGroup;
            meta.typeRecordIndex = static_cast<unsigned int>(m_spawnGroupRecords.size());
            m_spawnGroupRecords.push_back(s);
            return addRecordSnap(meta);
        }
    }

    meta.type = NetEncType::Unsupported;
    return addRecordSnap(meta);
}

void WorldNetSnapshot::buildFromLiveWorld(
    const std::vector<std::pair<const io::NetworkableDescriptor*, io::NetworkableStateMask> >* updatedMasks)
{
    typedef world::object_detail::LayoutAccess L;

    clear();

    // 1. Copy updated state masks and index by first property's networkableId.
    if (updatedMasks)
    {
        m_updatedStateMasks = *updatedMasks;
        for (std::size_t i = 0; i < m_updatedStateMasks.size(); ++i)
        {
            const io::NetworkableDescriptor* const desc = m_updatedStateMasks[i].first;
            if (!desc || desc->properties.empty() || desc->properties[0].networkables.empty())
                continue;
            const io::INetworkable* const rep = desc->properties[0].networkables[0];
            if (!rep || rep->m_networkableId == 0)
                continue;
            const unsigned short netId = rep->m_networkableId;
            if (m_stateMaskIndexByNetId[netId] < 0)
                m_touchedMaskNetIds.push_back(netId);
            m_stateMaskIndexByNetId[netId] = static_cast<int>(i);
        }
    }

    if (!io::networkManager)
        return;

    // 2. Capture all registered descriptors' representative networkables.
    std::map<unsigned short, io::NetworkableDescriptor*>& descMap =
        io::networkManager->getDescriptorMap();
    if (m_records.capacity() < descMap.size() + 64)
        m_records.reserve(descMap.size() + 64);

    for (std::map<unsigned short, io::NetworkableDescriptor*>::iterator it = descMap.begin();
         it != descMap.end(); ++it)
    {
        io::NetworkableDescriptor* const desc = it->second;
        if (!desc)
            continue;
        for (std::size_t p = 0; p < desc->properties.size(); ++p)
        {
            io::UniqueNetProperty& prop = desc->properties[p];
            if (!prop.networkables.empty() && prop.networkables[0])
                captureNetworkable(prop.networkables[0]);
        }
    }

    // 3. Snapshot scopable root groups in exact traversal order.
    auto appendRootGroupEntry = [&](io::INetworkableObject* objEntry,
                                    io::NetworkableDescriptor* desc,
                                    ScopableRootSnap& rootSnap,
                                    bool& rootInitialized)
    {
        if (!objEntry || !desc || desc->properties.empty() || desc->properties[0].networkables.empty())
            return;
        io::INetworkable* const rep = desc->properties[0].networkables[0];
        if (!rep)
            return;

        if (!rootInitialized && objEntry->m_object)
        {
            world::IObject* const rootObj = world::getRootParent(objEntry->m_object);
            if (rootObj)
            {
                rootSnap.rootObjectId = rootObj->getObjectId();
                const Vec3& pos = rootObj->getAbsolutePosition();
                rootSnap.pos[0] = pos.x;
                rootSnap.pos[1] = pos.y;
                rootSnap.pos[2] = pos.z;
                world::IPhysicsNode* const node =
                    static_cast<world::IPhysicsNode*>(L::physicsNode(rootObj));
                if (node)
                {
                    const Vec3& vel = node->getPositionalSpeed();
                    rootSnap.velocity[0]    = vel.x;
                    rootSnap.velocity[1]    = vel.y;
                    rootSnap.velocity[2]    = vel.z;
                    rootSnap.hasPhysicsNode = true;
                }
                rootInitialized = true;
            }
        }

        ScopableEntrySnap entry;
        entry.networkableId          = rep->m_networkableId;
        entry.ownerId                = objEntry->m_ownerId;
        entry.basePriority           = rep->m_basePriority;
        entry.isProjectile           = (objEntry->m_object != 0
            && objEntry->m_object->queryInterface(world::IID_IProjectileObject) != 0);
        entry.hasUnsupportedProperty = !isDescriptorFullySupported(desc);
        entry.descriptor             = desc;

        if (entry.hasUnsupportedProperty)
            rootSnap.hasUnsupportedEntry = true;

        m_scopableEntries.push_back(entry);
        ++rootSnap.entryCount;
    };

    if (io::isFastStateMasksEnabled())
    {
        const io::FastScopableRootBuffer& fastRoots = io::getFastScopableRootBuffer();
        m_scopableRoots.reserve(fastRoots.groups.size());
        m_scopableEntries.reserve(fastRoots.entries.size());

        for (int g = static_cast<int>(fastRoots.groups.size()) - 1; g >= 0; --g)
        {
            ScopableRootSnap rootSnap;
            rootSnap.rootObjectId        = fastRoots.groups[g].rootId;
            rootSnap.entryStart          = static_cast<unsigned int>(m_scopableEntries.size());
            rootSnap.entryCount          = 0;
            rootSnap.pos[0]              = 0.0f;
            rootSnap.pos[1]              = 0.0f;
            rootSnap.pos[2]              = 0.0f;
            rootSnap.velocity[0]         = 0.0f;
            rootSnap.velocity[1]         = 0.0f;
            rootSnap.velocity[2]         = 0.0f;
            rootSnap.hasPhysicsNode      = false;
            rootSnap.hasUnsupportedEntry = false;

            bool rootInitialized = false;
            for (unsigned int idx = fastRoots.groups[g].firstEntry;
                 idx != 0xFFFFFFFFu;
                 idx = fastRoots.entries[idx].nextInGroup)
            {
                appendRootGroupEntry(fastRoots.entries[idx].netObject,
                                     fastRoots.entries[idx].descriptor,
                                     rootSnap, rootInitialized);
            }
            if (rootSnap.entryCount > 0)
                m_scopableRoots.push_back(rootSnap);
        }
    }
    else
    {
        typedef std::list<std::pair<io::INetworkableObject*, io::NetworkableDescriptor*> > Group;
        std::list<Group>& roots = io::networkManager->getScopableRootDescriptors();
        for (std::list<Group>::iterator group = roots.begin(); group != roots.end(); ++group)
        {
            ScopableRootSnap rootSnap;
            rootSnap.rootObjectId        = 0;
            rootSnap.entryStart          = static_cast<unsigned int>(m_scopableEntries.size());
            rootSnap.entryCount          = 0;
            rootSnap.pos[0]              = 0.0f;
            rootSnap.pos[1]              = 0.0f;
            rootSnap.pos[2]              = 0.0f;
            rootSnap.velocity[0]         = 0.0f;
            rootSnap.velocity[1]         = 0.0f;
            rootSnap.velocity[2]         = 0.0f;
            rootSnap.hasPhysicsNode      = false;
            rootSnap.hasUnsupportedEntry = false;

            bool rootInitialized = false;
            for (Group::iterator it = group->begin(); it != group->end(); ++it)
            {
                appendRootGroupEntry(it->first, it->second, rootSnap, rootInitialized);
            }
            if (rootSnap.entryCount > 0)
                m_scopableRoots.push_back(rootSnap);
        }
    }

    // 4. Record owner->rootIndex pairs and build 2D uniform XZ spatial grid over scopable roots.
    for (unsigned int r = 0; r < static_cast<unsigned int>(m_scopableRoots.size()); ++r)
    {
        const ScopableRootSnap& root = m_scopableRoots[r];
        for (unsigned int e = 0; e < root.entryCount; ++e)
        {
            const int ownerId = m_scopableEntries[root.entryStart + e].ownerId;
            if (ownerId >= 0)
            {
                bool dup = false;
                for (std::size_t k = m_ownerRoots.size(); k > 0; --k)
                {
                    if (m_ownerRoots[k - 1].second != r)
                        break;
                    if (m_ownerRoots[k - 1].first == ownerId)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                    m_ownerRoots.push_back(std::make_pair(ownerId, r));
            }
        }
    }
    rebuildSpatialGrid();

    // 5. Snapshot living teammates/enemies per team (1 and 2).
    if (world::playerManager)
    {
        for (int team = 1; team <= 2; ++team)
        {
            const std::list<world::IPlayer*>& players = world::playerManager->getPlayers(team);
            for (std::list<world::IPlayer*>::const_iterator it = players.begin();
                 it != players.end(); ++it)
            {
                world::IPlayer* const p = *it;
                if (!p || !p->getIsAlive())
                    continue;
                world::IObject* const veh = p->getVehicle();
                if (!veh)
                    continue;
                io::INetworkable* const net = veh->getNetworkable();
                if (!net || !net->m_ghostActive || !(net->m_basePriority > 0.0f))
                    continue;
                io::NetworkableDescriptor* const desc =
                    io::networkManager->getDescriptor(net->m_networkableId);
                if (!desc)
                    continue;

                AlivePlayerVehicleSnap snap;
                snap.playerId     = p->getId();
                snap.team         = team;
                snap.vehicleNetId = net->m_networkableId;
                const Vec3& pos   = veh->getAbsolutePosition();
                snap.pos[0]       = pos.x;
                snap.pos[1]       = pos.y;
                snap.pos[2]       = pos.z;
                snap.descriptor   = desc;
                m_teamAliveVehicles[team].push_back(snap);
            }
        }
    }

    // 6. Snapshot commander UAV/satellite scan & RC objects per team (1 and 2).
    m_hasCommanderTools = (world::commanderToolsManager != 0);
    if (m_hasCommanderTools)
    {
        m_uavRadius = gameLogic ? gameLogic->getUAVRadius() : 0.0f;
        for (int team = 1; team <= 2; ++team)
        {
            m_teamSatelliteScanActive[team] =
                world::commanderToolsManager->getSatelliteScanActive(team);
            Vec3 centre;
            centre.x = centre.y = centre.z = 0.0f;
            m_teamUavActive[team] =
                world::commanderToolsManager->getUAVActive(team, &centre);
            m_teamUavCentre[team] = centre;
        }

        if (gameLogic && gameLogic->getRemoteControlledObjectMap())
        {
            std::map<unsigned int, world::IObject*>& rcObjects =
                *static_cast<std::map<unsigned int, world::IObject*>*>(
                    gameLogic->getRemoteControlledObjectMap());
            for (std::map<unsigned int, world::IObject*>::iterator it = rcObjects.begin();
                 it != rcObjects.end(); ++it)
            {
                world::IObject* const object = it->second;
                if (!object)
                    continue;
                const int team = remoteControlledObjectTeamSnap(object);
                if (team != 1 && team != 2)
                    continue;
                const int state = remoteControlledObjectStateSnap(object);
                if (state != 1 && state != 2 && state != 3)
                    continue;
                io::INetworkable* const net = object->getNetworkable();
                if (!net)
                    continue;
                io::NetworkableDescriptor* const desc =
                    io::networkManager->getDescriptor(net->m_networkableId);
                if (desc)
                    m_teamSpecialCommanderDesc[team].push_back(desc);
            }
        }
    }
}

void WorldNetSnapshot::commitMainThreadSideEffects(const TypeWriteBitStats* optionalBitStats) const
{
    for (std::size_t i = 0; i < m_records.size(); ++i)
    {
        const NetRecordSnap& meta = m_records[i];
        io::INetworkable* const live = meta.liveNetworkable;
        if (!live)
            continue;

        switch (meta.type)
        {
        case NetEncType::Soldier:
        {
            SoldierNetworkable* const soldierNet = static_cast<SoldierNetworkable*>(live);
            soldierNet->pinNextToHead();
            const SoldierRecordSnap& s = m_soldierRecords[meta.typeRecordIndex];
            if (!s.isAwakeOccupiedRagdoll)
            {
                SoldierNetworkable::UPDATE* const rec =
                    soldierNet->getUpdate(static_cast<unsigned int>(soldierNet->getHeadSlot()));
                if (rec)
                    rec->ammoStatus &= 0xF;
            }
            break;
        }
        case NetEncType::Item:
        case NetEncType::GenericProjectile:
        case NetEncType::ScoreManager:
        case NetEncType::SpawnGroup:
        {
            io::NetworkableBase* const base = static_cast<io::NetworkableBase*>(live);
            base->pinNextToHead();
            break;
        }
        case NetEncType::Kit:
        case NetEncType::ControlPoint:
        case NetEncType::SimpleObject:
        case NetEncType::Player:
        case NetEncType::Unsupported:
        default:
            break;
        }
    }

    if (optionalBitStats)
    {
        SoldierNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::Soldier)];
        ItemNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::Item)];
        KitNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::Kit)];
        GenericProjectileNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::GenericProjectile)];
        ControlPointNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::ControlPoint)];
        SimpleObjectNetworkable::m_stats.m_writeBits +=
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::SimpleObject)];
        Player::addNetWriteBits(
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::Player)]);
        world::ScoreManager::addNetWriteBits(
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::ScoreManager)]);
        SpawnGroup::addNetWriteBits(
            optionalBitStats->bitsByType[static_cast<int>(NetEncType::SpawnGroup)]);
    }
}

} // namespace netpipeline
} // namespace hfe
} // namespace dice
