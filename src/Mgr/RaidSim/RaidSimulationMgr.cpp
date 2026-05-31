/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 * RaidSimulationMgr — see RaidSimulationMgr.h and the design/plan docs.
 */

#include "RaidSimulationMgr.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"

#include <algorithm>
#include <memory>

namespace
{
    // Home = Dalaran (safe city both factions use at 80). Reused from the validated spike.
    constexpr uint32 HOME_MAP = 571;
    constexpr float HOME_X = 5804.15f, HOME_Y = 624.77f, HOME_Z = 647.76f, HOME_O = 1.64f;

    // Canonical map+difficulty -> equippable-item-entry query (v5 design). difficulty_entry_N is a
    // column name (cannot be a bind param) built from d; safe because d is 0..3 from our own table.
    std::string BuildPoolQuery(uint32 mapId, uint8 d, uint8 minQuality, uint16 ilvlCap)
    {
        std::string entryExpr = (d == 0)
            ? "b.entry"
            : "IF(b.difficulty_entry_" + std::to_string(d) + " = 0, b.entry, b.difficulty_entry_" +
                  std::to_string(d) + ")";

        return
            "SELECT DISTINCT it.entry FROM ("
            "  SELECT clt.Item AS item FROM creature c"
            "  JOIN creature_template b ON b.entry = c.id1"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  WHERE c.map = " + std::to_string(mapId) + " AND clt.Reference = 0 AND clt.Item <> 0"
            "  UNION"
            "  SELECT rlt.Item FROM creature c"
            "  JOIN creature_template b ON b.entry = c.id1"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  JOIN reference_loot_template rlt ON rlt.Entry = clt.Reference"
            "  WHERE c.map = " + std::to_string(mapId) + " AND clt.Reference <> 0 AND rlt.Item <> 0"
            ") pool"
            " JOIN item_template it ON it.entry = pool.item"
            " WHERE it.class IN (2, 4)"
            "   AND it.Quality >= " + std::to_string(uint32(minQuality)) +
            "   AND it.ItemLevel <= " + std::to_string(uint32(ilvlCap)) + ";";
    }

    void ParkBot(Player* p)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
        if (!ai)
            return;
        ai->ChangeStrategy("+passive", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("+passive", BOT_STATE_COMBAT);
        ai->ChangeStrategy("+stay", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("+stay", BOT_STATE_COMBAT);
    }

    void UnparkBot(Player* p)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
        if (!ai)
            return;
        ai->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("-stay", BOT_STATE_COMBAT);
        ai->ChangeStrategy("-passive", BOT_STATE_NON_COMBAT);
        ai->ChangeStrategy("-passive", BOT_STATE_COMBAT);
        ai->Reset();
    }

    class RaidSimFormationOperation : public PlayerbotOperation
    {
    public:
        RaidSimFormationOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids,
                                  uint32 mapId, uint8 difficulty, std::string label,
                                  float x, float y, float z, float o)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_mapId(mapId),
              m_difficulty(difficulty), m_label(std::move(label)), m_x(x), m_y(y), m_z(z), m_o(o)
        {
        }

        bool Execute() override
        {
            Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid);
            if (!leader)
            {
                LOG_ERROR("playerbots", "RaidSim: leader not found at formation");
                return false;
            }

            for (ObjectGuid const& guid : m_memberGuids)
            {
                if (guid == m_leaderGuid)
                    continue;
                if (Player* m = ObjectAccessor::FindPlayer(guid))
                    if (Group* g = m->GetGroup())
                        g->RemoveMember(guid);
            }
            if (Group* lg = leader->GetGroup())
                lg->Disband(true);

            Group* group = new Group();
            if (!group->Create(leader))
            {
                delete group;
                LOG_ERROR("playerbots", "RaidSim: failed to create group for leader {}", leader->GetName());
                return false;
            }
            sGroupMgr->AddGroup(group);
            MapEntry const* mapEntry = sMapStore.LookupEntry(m_mapId);
            if (mapEntry && mapEntry->IsRaid())
            {
                group->ConvertToRaid();
                group->SetRaidDifficulty(Difficulty(m_difficulty));
            }
            else
            {
                group->SetDungeonDifficulty(Difficulty(m_difficulty));
            }

            uint32 added = 0;
            for (ObjectGuid const& guid : m_memberGuids)
            {
                if (guid == m_leaderGuid)
                    continue;
                Player* m = ObjectAccessor::FindPlayer(guid);
                if (m && group->AddMember(m))
                    ++added;
            }

            for (ObjectGuid const& guid : m_memberGuids)
            {
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p)
                    continue;
                p->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                p->TeleportTo(m_mapId, m_x, m_y, m_z, m_o);
                ParkBot(p);
            }

            LOG_INFO("playerbots", "RaidSim: formed raid leader={} members={} -> map {} diff {} ({})",
                     leader->GetName(), added + 1, m_mapId, uint32(m_difficulty), m_label);

            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                    guild->BroadcastToGuild(leader->GetSession(), false, "We set out for " + m_label + ".", LANG_UNIVERSAL);
            return true;
        }

        ObjectGuid GetBotGuid() const override { return m_leaderGuid; }
        uint32 GetPriority() const override { return 60; }
        std::string GetName() const override { return "RaidSimFormation"; }
        bool IsValid() const override { return ObjectAccessor::FindPlayer(m_leaderGuid) != nullptr; }

    private:
        ObjectGuid m_leaderGuid;
        std::vector<ObjectGuid> m_memberGuids;
        uint32 m_mapId;
        uint8 m_difficulty;
        std::string m_label;
        float m_x, m_y, m_z, m_o;
    };

    class RaidSimTeardownOperation : public PlayerbotOperation
    {
    public:
        RaidSimTeardownOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids, std::string label)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_label(std::move(label))
        {
        }

        bool Execute() override
        {
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                    if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                        guild->BroadcastToGuild(leader->GetSession(), false, "We return from " + m_label + ".", LANG_UNIVERSAL);

            for (ObjectGuid const& guid : m_memberGuids)
            {
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p)
                    continue;
                UnparkBot(p);
                p->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                p->TeleportTo(HOME_MAP, HOME_X, HOME_Y, HOME_Z, HOME_O);
            }

            if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                if (Group* g = leader->GetGroup())
                    g->Disband(true);

            sRaidSimulationMgr.ClearRaidingFlags(m_memberGuids);
            LOG_INFO("playerbots", "RaidSim: teardown complete leader={}", m_leaderGuid.ToString());
            return true;
        }

        ObjectGuid GetBotGuid() const override { return m_leaderGuid; }
        uint32 GetPriority() const override { return 60; }
        std::string GetName() const override { return "RaidSimTeardown"; }
        bool IsValid() const override { return true; }

    private:
        ObjectGuid m_leaderGuid;
        std::vector<ObjectGuid> m_memberGuids;
        std::string m_label;
    };
}  // namespace

void RaidSimulationMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _bands.clear();
    _pools.clear();

    if (QueryResult s = CharacterDatabase.Query("SELECT base_ilvl FROM playerbots_raid_server_state WHERE id = 1"))
        _baseIlvl = s->Fetch()[0].Get<uint16>();
    else
        LOG_INFO("playerbots", "RaidSim: playerbots_raid_server_state row absent; base_ilvl defaulting to 0.");

    QueryResult result = CharacterDatabase.Query(
        "SELECT id, band, map_id, difficulty, group_size, gate_ilvl, ilvl_cap, min_quality, label, "
        "       entry_x, entry_y, entry_z, entry_o, park_x, park_y, park_z, park_o "
        "FROM playerbots_raid_tier_instance ORDER BY band ASC, id ASC");
    if (!result)
    {
        LOG_WARN("playerbots", "RaidSim: no rows in playerbots_raid_tier_instance; disabled.");
        return;
    }

    uint32 instanceCount = 0;
    do
    {
        Field* f = result->Fetch();
        RaidSimInstance inst;
        inst.id         = f[0].Get<uint32>();
        inst.band       = f[1].Get<uint8>();
        inst.mapId      = f[2].Get<uint32>();
        inst.difficulty = f[3].Get<uint8>();
        inst.groupSize  = f[4].Get<uint8>();
        inst.gateIlvl   = f[5].Get<uint16>();
        inst.ilvlCap    = f[6].Get<uint16>();
        inst.minQuality = f[7].Get<uint8>();
        inst.label      = f[8].Get<std::string>();
        inst.entryX = f[9].Get<float>();  inst.entryY = f[10].Get<float>();
        inst.entryZ = f[11].Get<float>(); inst.entryO = f[12].Get<float>();
        inst.parkX  = f[13].Get<float>(); inst.parkY  = f[14].Get<float>();
        inst.parkZ  = f[15].Get<float>(); inst.parkO  = f[16].Get<float>();

        std::vector<uint32> pool;
        if (QueryResult pr = WorldDatabase.Query(
                BuildPoolQuery(inst.mapId, inst.difficulty, inst.minQuality, inst.ilvlCap).c_str()))
        {
            do { pool.push_back(pr->Fetch()[0].Get<uint32>()); } while (pr->NextRow());
        }
        if (pool.empty())
            LOG_WARN("playerbots", "RaidSim: instance {} '{}' (map {} diff {}) has EMPTY loot pool.",
                     inst.id, inst.label, inst.mapId, uint32(inst.difficulty));
        _pools[inst.id] = std::move(pool);

        _bands[inst.band].push_back(inst);
        ++instanceCount;
    } while (result->NextRow());

    LOG_INFO("playerbots", "RaidSim: loaded {} bands / {} instances, base_ilvl={}.",
             uint32(_bands.size()), instanceCount, uint32(_baseIlvl));
}

void RaidSimulationMgr::ConsiderPlayerIlvl(Player* player)
{
    if (!player)
        return;

    uint32 sum = 0, count = 0;
    for (uint8 slot = EQUIPMENT_SLOT_HEAD; slot < EQUIPMENT_SLOT_TABARD; ++slot)
    {
        // Mirror Player::GetAverageItemLevel(): exclude shirt, off-hand (shield/frill), and
        // ranged/relic — so the server frontier tracks the standard avg-equipped-ilvl and is
        // not skewed (e.g. a hunter's high-ilvl ranged would otherwise inflate the monotonic gate).
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_OFFHAND || slot == EQUIPMENT_SLOT_RANGED)
            continue;
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            sum += item->GetTemplate()->ItemLevel;
            ++count;
        }
    }
    if (!count)
        return;

    uint16 avg = uint16(sum / count);
    std::lock_guard<std::mutex> lock(_mutex);
    if (avg > _baseIlvl)
    {
        _baseIlvl = avg;
        PersistBaseIlvl();
    }
}

void RaidSimulationMgr::PersistBaseIlvl()
{
    CharacterDatabase.Execute("UPDATE playerbots_raid_server_state SET base_ilvl = {} WHERE id = 1", _baseIlvl);
}

bool RaidSimulationMgr::IsRaiding(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _raiding.find(guid) != _raiding.end();
}

void RaidSimulationMgr::ClearRaidingFlags(std::vector<ObjectGuid> const& members)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (ObjectGuid const& g : members)
        _raiding.erase(g);
}
