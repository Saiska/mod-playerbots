/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 * RaidSimulationMgr — see RaidSimulationMgr.h and the design/plan docs.
 */

#include "RaidSimulationMgr.h"

#include "Chat.h"
#include "ChatHelper.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "CharacterCache.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotMgr.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace
{
    // Home = Dalaran (safe city both factions use at 80). Reused from the validated spike.
    constexpr uint32 HOME_MAP = 571;
    constexpr float HOME_X = 5804.15f, HOME_Y = 624.77f, HOME_Z = 647.76f, HOME_O = 1.64f;
    // Minimum rest after a run, so a long Duration can never yield a zero/negative next cooldown.
    constexpr uint32 RAIDSIM_COOLDOWN_FLOOR_MS = 5u * 60u * 1000u;
    // Leveling-instance ids are stored in the shared _pools map offset by this base so they never
    // collide with endgame playerbots_raid_tier_instance ids (≈33 rows; 1,000,000 is safe headroom).
    constexpr uint32 LEVELING_ID_BASE = 1000000u;

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

    // All boss-drop item entries on a map (no class/quality/ilvl filter) — used to detect tokens
    // and emblems for currency expansion. Same boss UNION as BuildPoolQuery, minus the item join.
    std::string BuildRawLootQuery(uint32 mapId, uint8 d)
    {
        std::string entryExpr = (d == 0)
            ? "b.entry"
            : "IF(b.difficulty_entry_" + std::to_string(d) + " = 0, b.entry, b.difficulty_entry_" +
                  std::to_string(d) + ")";
        return
            "SELECT DISTINCT pool.item FROM ("
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
            ") pool;";
    }

    // All item entries inside a chest GameObject's loot (gameobject_template.Data1 = lootid for
    // CHEST-type GOs). No filter — caller gates + currency-expands.
    std::string BuildChestRawQuery(uint32 goEntry)
    {
        return
            "SELECT DISTINCT pool.item FROM ("
            "  SELECT glt.Item AS item FROM gameobject_template gt"
            "  JOIN gameobject_loot_template glt ON glt.Entry = gt.Data1"
            "  WHERE gt.entry = " + std::to_string(goEntry) + " AND glt.Reference = 0 AND glt.Item <> 0"
            "  UNION"
            "  SELECT rlt.Item FROM gameobject_template gt"
            "  JOIN gameobject_loot_template glt ON glt.Entry = gt.Data1"
            "  JOIN reference_loot_template rlt ON rlt.Entry = glt.Reference"
            "  WHERE gt.entry = " + std::to_string(goEntry) + " AND glt.Reference <> 0 AND rlt.Item <> 0"
            ") pool;";
    }

    // Comma-joined entry list for an IN(...) clause. Entries come from our own DB table (uint32),
    // never user input, so direct interpolation is safe. Empty -> "0" (matches nothing).
    std::string JoinEntries(std::vector<uint32> const& entries)
    {
        if (entries.empty())
            return "0";
        std::string s;
        for (uint32 e : entries)
        {
            if (!s.empty())
                s += ",";
            s += std::to_string(e);
        }
        return s;
    }

    // Curated-boss equivalent of BuildPoolQuery: resolve equippable items from explicit creature
    // entries via creature_template.lootid (+ reference walk), with the same difficulty_entry_N
    // indirection — but NO spawn join, so summoned bosses (0 static spawns) are reachable.
    // NOTE: stored entries MUST be BASE (normal-mode) creature_template.entry values, not
    // difficulty_entry_N variants — the IF() expression applies the difficulty redirect from the
    // base row, so a variant entry would double-redirect and resolve the wrong loot.
    std::string BuildBossPoolQuery(std::vector<uint32> const& entries, uint8 d, uint8 minQuality,
                                   uint16 ilvlCap)
    {
        std::string inList = JoinEntries(entries);
        std::string entryExpr = (d == 0)
            ? "b.entry"
            : "IF(b.difficulty_entry_" + std::to_string(d) + " = 0, b.entry, b.difficulty_entry_" +
                  std::to_string(d) + ")";
        return
            "SELECT DISTINCT it.entry FROM ("
            "  SELECT clt.Item AS item FROM creature_template b"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  WHERE b.entry IN (" + inList + ") AND clt.Reference = 0 AND clt.Item <> 0"
            "  UNION"
            "  SELECT rlt.Item FROM creature_template b"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  JOIN reference_loot_template rlt ON rlt.Entry = clt.Reference"
            "  WHERE b.entry IN (" + inList + ") AND clt.Reference <> 0 AND rlt.Item <> 0"
            ") pool"
            " JOIN item_template it ON it.entry = pool.item"
            " WHERE it.class IN (2, 4)"
            "   AND it.Quality >= " + std::to_string(uint32(minQuality)) +
            "   AND it.ItemLevel <= " + std::to_string(uint32(ilvlCap)) + ";";
    }

    // Curated-boss equivalent of BuildRawLootQuery: all raw drop entries (no filter) for token/
    // emblem currency expansion.
    std::string BuildBossRawQuery(std::vector<uint32> const& entries, uint8 d)
    {
        std::string inList = JoinEntries(entries);
        std::string entryExpr = (d == 0)
            ? "b.entry"
            : "IF(b.difficulty_entry_" + std::to_string(d) + " = 0, b.entry, b.difficulty_entry_" +
                  std::to_string(d) + ")";
        return
            "SELECT DISTINCT pool.item FROM ("
            "  SELECT clt.Item AS item FROM creature_template b"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  WHERE b.entry IN (" + inList + ") AND clt.Reference = 0 AND clt.Item <> 0"
            "  UNION"
            "  SELECT rlt.Item FROM creature_template b"
            "  JOIN creature_template e ON e.entry = " + entryExpr +
            "  JOIN creature_loot_template clt ON clt.Entry = e.lootid"
            "  JOIN reference_loot_template rlt ON rlt.Entry = clt.Reference"
            "  WHERE b.entry IN (" + inList + ") AND clt.Reference <> 0 AND rlt.Item <> 0"
            ") pool;";
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

    // Minimum online L80 bots needed to FIELD an instance of this group size (threshold sits below
    // the nominal size — runs are present-only, so an under-filled group is fine cosmetically).
    uint32 ThresholdFor(uint8 groupSize)
    {
        if (groupSize <= 5)
            return sPlayerbotAIConfig.raidSimMinDungeon;
        if (groupSize <= 10)
            return sPlayerbotAIConfig.raidSimMinRaid10;
        return sPlayerbotAIConfig.raidSimMinRaid25;
    }

    // Per-bot eligibility for a sim run, EXCLUDING the _raiding check (callers apply that with the
    // correct locking). Shared by the GM Start path and the scheduler tick.
    bool BotPassesBaseEligibility(Player* bot, uint32 guildId)
    {
        if (!bot || !bot->IsInWorld())
            return false;
        if (bot->GetGuildId() != guildId)
            return false;
        if (!GET_PLAYERBOT_AI(bot))
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            return false;  // autonomous random bots only — never conscript a real player's alt companion
        if (bot->GetGroup())
            return false;  // not in any group — the formation op disbands the bot's current group
        if (bot->isDead() || bot->IsInCombat())
            return false;
        if (bot->InBattleground() || bot->InBattlegroundQueue())
            return false;
        if (bot->GetMap() && bot->GetMap()->IsDungeon())
            return false;  // already inside an instance; forming/disbanding would homebind it
        return true;
    }

    class RaidSimFormationOperation : public PlayerbotOperation
    {
    public:
        RaidSimFormationOperation(uint32 guildId, ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids,
                                  uint32 mapId, uint8 difficulty, std::string label,
                                  float x, float y, float z, float o)
            : m_guildId(guildId), m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_mapId(mapId),
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
            sRaidSimulationMgr.SetRunGroupGuid(m_guildId, group->GetGUID());
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
                {
                    if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                    {
                        std::string msg = std::string(guild->GetName()) + " has set out for " + m_label + ".";
                        WorldPacket data;
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                        sWorldSessionMgr->SendGlobalMessage(&data);
                    }
                    else
                        guild->BroadcastToGuild(leader->GetSession(), false, "We set out for " + m_label + ".", LANG_UNIVERSAL);
                }
            return true;
        }

        ObjectGuid GetBotGuid() const override { return m_leaderGuid; }
        uint32 GetPriority() const override { return 60; }
        std::string GetName() const override { return "RaidSimFormation"; }
        bool IsValid() const override { return ObjectAccessor::FindPlayer(m_leaderGuid) != nullptr; }

    private:
        uint32 m_guildId;
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
        RaidSimTeardownOperation(ObjectGuid leaderGuid, ObjectGuid groupGuid,
                                 std::vector<ObjectGuid> memberGuids, std::string label)
            : m_leaderGuid(leaderGuid), m_groupGuid(groupGuid),
              m_memberGuids(std::move(memberGuids)), m_label(std::move(label))
        {
        }

        bool Execute() override
        {
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                    if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                    {
                        if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                        {
                            std::string msg = std::string(guild->GetName()) + " returns from " + m_label + ".";
                            WorldPacket data;
                            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                            sWorldSessionMgr->SendGlobalMessage(&data);
                        }
                        else
                            guild->BroadcastToGuild(leader->GetSession(), false, "We return from " + m_label + ".", LANG_UNIVERSAL);
                    }

            for (ObjectGuid const& guid : m_memberGuids)
            {
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p)
                    continue;
                UnparkBot(p);
                p->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                p->TeleportTo(HOME_MAP, HOME_X, HOME_Y, HOME_Z, HOME_O);
            }

            Group* g = nullptr;
            if (m_groupGuid)
                g = sGroupMgr->GetGroupByGUID(m_groupGuid.GetCounter());
            if (!g)
                if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                    g = leader->GetGroup();
            if (g)
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
        ObjectGuid m_groupGuid;
        std::vector<ObjectGuid> m_memberGuids;
        std::string m_label;
    };
}  // namespace

void RaidSimulationMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _bands.clear();
    _pools.clear();

    // --- Currency/token expansion graph: reqItem -> vendor items (built once, read-only after). ---
    _currencyExpansion.clear();
    if (QueryResult vr = WorldDatabase.Query(
            "SELECT item, ExtendedCost FROM npc_vendor WHERE ExtendedCost > 0"))
    {
        do
        {
            Field* vf = vr->Fetch();
            uint32 item = vf[0].Get<uint32>();
            uint32 ecId = vf[1].Get<uint32>();
            if (ItemExtendedCostEntry const* ec = sItemExtendedCostStore.LookupEntry(ecId))
                for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
                    if (ec->reqitem[i])
                        _currencyExpansion[ec->reqitem[i]].push_back(item);
        } while (vr->NextRow());
    }
    for (auto& kv : _currencyExpansion)  // dedup vendor lists per currency
    {
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
    }
    LOG_INFO("playerbots", "RaidSim: currency-expansion graph has {} token/currency entries.",
             uint32(_currencyExpansion.size()));

    // --- Chest-loot mapping (summoned caches; not static-joinable). ---
    _chestLoot.clear();
    if (QueryResult cr = CharacterDatabase.Query(
            "SELECT map_id, difficulty, gameobject_entry FROM playerbots_raid_chest_loot"))
    {
        do
        {
            Field* cf = cr->Fetch();
            _chestLoot[{cf[0].Get<uint32>(), cf[1].Get<uint8>()}].push_back(cf[2].Get<uint32>());
        } while (cr->NextRow());
    }
    LOG_INFO("playerbots", "RaidSim: chest-loot mapping has {} (map,difficulty) keys.",
             uint32(_chestLoot.size()));

    // --- Boss-loot mapping (summoned/scripted bosses; not static-joinable). ---
    _bossLoot.clear();
    if (QueryResult br = CharacterDatabase.Query(
            "SELECT map_id, difficulty, creature_entry FROM playerbots_raid_boss_loot"))
    {
        do
        {
            Field* bf = br->Fetch();
            _bossLoot[{bf[0].Get<uint32>(), bf[1].Get<uint8>()}].push_back(bf[2].Get<uint32>());
        } while (br->NextRow());
    }
    LOG_INFO("playerbots", "RaidSim: boss-loot mapping has {} (map,difficulty) keys.",
             uint32(_bossLoot.size()));

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

        _pools[inst.id] = BuildPool(inst);

        _bands[inst.band].push_back(inst);
        ++instanceCount;
    } while (result->NextRow());

    LOG_INFO("playerbots", "RaidSim: loaded {} bands / {} instances, base_ilvl={}.",
             uint32(_bands.size()), instanceCount, uint32(_baseIlvl));

    // --- Leveling dungeons (sub-80, level-gated). Same loot resolver, difficulty 0. ---
    _leveling.clear();
    if (QueryResult lr = CharacterDatabase.Query(
            "SELECT id, map_id, difficulty, level_lo, level_hi, ilvl_cap, min_quality, label, "
            "       entry_x, entry_y, entry_z, entry_o, park_x, park_y, park_z, park_o "
            "FROM playerbots_leveling_instance ORDER BY level_hi DESC, id ASC"))
    {
        uint32 levelingCount = 0;
        do
        {
            Field* f = lr->Fetch();
            RaidSimInstance inst;
            inst.id         = f[0].Get<uint32>() + LEVELING_ID_BASE;  // offset key for shared _pools
            inst.band       = 0;
            inst.mapId      = f[1].Get<uint32>();
            inst.difficulty = f[2].Get<uint8>();
            inst.groupSize  = 5;                                       // leveling is dungeons-only
            inst.levelLo    = f[3].Get<uint8>();
            inst.levelHi    = f[4].Get<uint8>();
            inst.ilvlCap    = f[5].Get<uint16>();
            inst.minQuality = f[6].Get<uint8>();
            inst.label      = f[7].Get<std::string>();
            inst.entryX = f[8].Get<float>();  inst.entryY = f[9].Get<float>();
            inst.entryZ = f[10].Get<float>(); inst.entryO = f[11].Get<float>();
            inst.parkX  = f[12].Get<float>(); inst.parkY  = f[13].Get<float>();
            inst.parkZ  = f[14].Get<float>(); inst.parkO  = f[15].Get<float>();

            _pools[inst.id] = BuildPool(inst);

            _leveling.push_back(inst);
            ++levelingCount;
        } while (lr->NextRow());

        LOG_INFO("playerbots", "RaidSim: loaded {} leveling dungeons.", levelingCount);
    }
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

bool RaidSimulationMgr::ResolveBaseBand(uint8& outBand) const
{
    // _bands is ordered ascending. Highest band whose gate_ilvl <= base_ilvl.
    bool any = false;
    for (auto const& kv : _bands)
    {
        if (kv.second.empty())
            continue;
        if (kv.second.front().gateIlvl <= _baseIlvl)  // band-uniform gate
        {
            outBand = kv.first;
            any = true;
        }
    }
    return any;
}

bool RaidSimulationMgr::ResolveGuildInstance(std::string const& guildName, uint32 avail, RaidSimInstance& out) const
{
    GuildTheme const& theme = PlayerbotGuildMgr::instance().GetThemeByName(guildName);

    // raid_offset is now a pure TIER modifier (not an on/off flag). Unassigned (<0) -> DefaultOffset,
    // so every guild participates; how high it reaches is offset/base_ilvl, capped by max_band.
    int offset = theme.raidOffset < 0 ? int(sPlayerbotAIConfig.raidSimDefaultOffset) : int(theme.raidOffset);

    uint8 baseBand = 0;
    if (!ResolveBaseBand(baseBand))
        return false;  // nothing unlocked yet (no band's gate_ilvl met — band 0 gates at 0, so rare)

    int top = int(baseBand) - offset;
    if (top < 0)
        top = 0;
    if (top > int(theme.maxBand))
        top = int(theme.maxBand);

    // "Field what you can": walk DOWN from the unlocked band; the first band holding an instance the
    // roster can field (avail >= threshold(group_size)) wins; random-pick among that band's fillable
    // instances for variety. Yields the highest-tier content this headcount supports.
    for (int b = top; b >= 0; --b)
    {
        auto it = _bands.find(uint8(b));
        if (it == _bands.end() || it->second.empty())
            continue;

        std::vector<RaidSimInstance> fillable;
        for (RaidSimInstance const& inst : it->second)
            if (avail >= ThresholdFor(inst.groupSize))
                fillable.push_back(inst);

        if (fillable.empty())
            continue;

        out = fillable[urand(0, fillable.size() - 1)];
        return true;
    }
    return false;
}

bool RaidSimulationMgr::ResolveLevelingInstance(std::vector<uint8> const& levels, RaidSimInstance& out) const
{
    // _leveling is pre-sorted by levelHi DESC (LoadFromDB). Highest dungeon whose in-range cohort
    // reaches MinDungeon wins. All leveling dungeons are group_size 5, so the threshold is MinDungeon.
    uint32 need = sPlayerbotAIConfig.raidSimMinDungeon;
    for (RaidSimInstance const& inst : _leveling)
    {
        uint32 inRange = 0;
        for (uint8 lv : levels)
            if (lv >= inst.levelLo && lv <= inst.levelHi)
                ++inRange;
        if (inRange >= need)
        {
            out = inst;
            return true;
        }
    }
    return false;
}

void RaidSimulationMgr::LaunchRun(uint32 guildId, std::string const& guildName, RaidSimInstance const& inst,
                                  std::vector<ObjectGuid> const& members)
{
    // Caller holds _mutex.
    if (members.empty())
    {
        LOG_ERROR("playerbots", "RaidSim: LaunchRun called with empty member list for guild '{}'", guildName);
        return;
    }

    ActiveRun run;
    run.guildId = guildId;
    run.guildName = guildName;
    run.leader = members.front();
    run.members = members;
    run.instanceId = inst.id;
    run.mapId = inst.mapId;
    run.difficulty = inst.difficulty;
    run.groupSize = inst.groupSize;
    run.label = inst.label;
    _runs[guildId] = run;
    for (ObjectGuid const& g : members)
        _raiding.insert(g);

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimFormationOperation>(guildId, run.leader, members, inst.mapId, inst.difficulty,
                                                    inst.label, inst.entryX, inst.entryY, inst.entryZ, inst.entryO));

    LOG_INFO("playerbots", "RaidSim: LAUNCH guild='{}' band {} inst {} ({}) bots={} leader={}",
             guildName, uint32(inst.band), inst.id, inst.label, members.size(), run.leader.ToString());
}

void RaidSimulationMgr::SetRunGroupGuid(uint32 guildId, ObjectGuid groupGuid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _runs.find(guildId);
    if (it != _runs.end())
        it->second.groupGuid = groupGuid;
}

void RaidSimulationMgr::EndRun(ActiveRun const& run)
{
    // Caller holds _mutex. _raiding stays set; the teardown op clears it once bots are home.
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RaidSimTeardownOperation>(run.leader, run.groupGuid, run.members, run.label)))
    {
        LOG_ERROR("playerbots", "RaidSim: teardown op dropped for guild '{}'; clearing raiding flags directly.",
                  run.guildName);
        for (ObjectGuid const& g : run.members)
            _raiding.erase(g);
    }
    // Per-content cadence: dungeons recur fast, raids slow. Jitter (redrawn each cycle) desyncs guilds.
    // Period is a START-TO-START target, so subtract the run's own Duration to get the rest interval.
    bool isDungeon = run.groupSize <= 5;
    uint32 basePeriodMin = isDungeon ? sPlayerbotAIConfig.raidSimDungeonPeriod
                                     : sPlayerbotAIConfig.raidSimRaidPeriod;
    uint32 jitterPct = sPlayerbotAIConfig.raidSimJitterPct;
    int32 signedDelta = 0;
    if (jitterPct > 0)
        signedDelta = int32(urand(0, 2u * jitterPct)) - int32(jitterPct);  // -jit .. +jit (percent)
    int64 periodMin = int64(basePeriodMin) + int64(basePeriodMin) * signedDelta / 100;
    if (periodMin < 1)
        periodMin = 1;
    uint32 periodMs = uint32(periodMin) * 60u * 1000u;
    uint32 durationMs = sPlayerbotAIConfig.raidSimDuration * 60u * 1000u;
    _cooldownMs[run.guildId] = (periodMs > durationMs + RAIDSIM_COOLDOWN_FLOOR_MS)
                                 ? (periodMs - durationMs)
                                 : RAIDSIM_COOLDOWN_FLOOR_MS;
    LOG_INFO("playerbots", "RaidSim: END guild='{}' inst {} ({})", run.guildName, run.instanceId, run.label);
}

bool RaidSimulationMgr::Start(ChatHandler* handler, std::string const& guildName)
{
    Guild* guild = sGuildMgr->GetGuildByName(guildName);
    if (!guild)
    {
        handler->PSendSysMessage("RaidSim: guild '{}' not found.", guildName);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runs.count(guild->GetId()))
        {
            handler->PSendSysMessage("RaidSim: '{}' already has an active run. Stop it first.", guildName);
            return false;
        }
    }

    // CRITICAL: ObjectAccessor::GetPlayers() — the authoritative online-players map. NOT
    // sRandomPlayerbotMgr.GetPlayers() (only human-mastered alt bots; spike bug fixed in 12297d80).
    // Gather ALL grabbable bots (any level), keeping levels so we can bucket endgame vs leveling.
    std::vector<std::pair<ObjectGuid, uint8>> grabbable;
    std::vector<ObjectGuid> bots80;
    std::vector<uint8> levels;
    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* bot = it.second;
        if (!BotPassesBaseEligibility(bot, guild->GetId()))
            continue;
        if (IsRaiding(bot->GetGUID()))
            continue;
        uint8 lv = bot->GetLevel();
        grabbable.emplace_back(bot->GetGUID(), lv);
        levels.push_back(lv);
        if (lv >= 80)
            bots80.push_back(bot->GetGUID());
    }

    if (grabbable.size() < sPlayerbotAIConfig.raidSimMinDungeon)
    {
        handler->PSendSysMessage("RaidSim: only {} grabbable bots in '{}' (need {} to field a dungeon).",
                                 uint32(grabbable.size()), guildName, sPlayerbotAIConfig.raidSimMinDungeon);
        return false;
    }

    // Highest bracket first: endgame (level 80) when fillable, else the highest leveling dungeon.
    RaidSimInstance inst;
    std::vector<ObjectGuid> party;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (bots80.size() >= sPlayerbotAIConfig.raidSimMinDungeon &&
            ResolveGuildInstance(guildName, uint32(bots80.size()), inst))
        {
            party = bots80;
        }
        else if (ResolveLevelingInstance(levels, inst))
        {
            for (auto const& gl : grabbable)
                if (gl.second >= inst.levelLo && gl.second <= inst.levelHi)
                    party.push_back(gl.first);
        }
        else
        {
            handler->PSendSysMessage("RaidSim: '{}' has no fillable content (no level-80 cohort and no leveling dungeon its bots can field).", guildName);
            return false;
        }
    }

    if (party.size() > inst.groupSize)
        party.resize(inst.groupSize);  // bring min(cohort, group_size)

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runs.count(guild->GetId()))
        {
            handler->PSendSysMessage("RaidSim: '{}' was launched concurrently; skipping.", guildName);
            return false;
        }
        LaunchRun(guild->GetId(), guildName, inst, party);
    }

    handler->PSendSysMessage("RaidSim: launching '{}' -> {} (band {}) with {} bots.",
                             guildName, inst.label, uint32(inst.band), uint32(party.size()));
    return true;
}

bool RaidSimulationMgr::Stop(ChatHandler* handler, std::string const& guildName)
{
    Guild* guild = sGuildMgr->GetGuildByName(guildName);
    if (!guild)
    {
        handler->PSendSysMessage("RaidSim: guild '{}' not found.", guildName);
        return false;
    }

    ActiveRun run;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _runs.find(guild->GetId());
        if (it == _runs.end())
        {
            handler->PSendSysMessage("RaidSim: no active run for '{}'.", guildName);
            return false;
        }
        run = it->second;
        _runs.erase(it);
        EndRun(run);
    }

    handler->PSendSysMessage("RaidSim: tearing down '{}' ({} bots) -> home.", guildName, uint32(run.members.size()));
    return true;
}

void RaidSimulationMgr::Status(ChatHandler* handler)
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint32 instCount = 0;
    for (auto const& kv : _bands)
        instCount += kv.second.size();
    handler->PSendSysMessage("RaidSim: base_ilvl={} | bands={} instances={} | runs={} | flagged={}",
                             uint32(_baseIlvl), uint32(_bands.size()), instCount, uint32(_runs.size()),
                             uint32(_raiding.size()));
    for (auto const& kv : _runs)
    {
        ActiveRun const& run = kv.second;
        uint32 onMap = 0, online = 0;
        for (ObjectGuid const& g : run.members)
        {
            Player* p = ObjectAccessor::FindPlayer(g);
            if (!p)
                continue;
            ++online;
            if (p->GetMapId() == run.mapId)
                ++onMap;
        }
        handler->PSendSysMessage("  '{}' {} | members {}, online {}, on-map {} | {}s elapsed",
                                 run.guildName, run.label, uint32(run.members.size()), online, onMap,
                                 run.elapsedMs / 1000u);
    }
}

void RaidSimulationMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidSimEnable)
        return;

    ReconcileOrphans(diff);

    std::lock_guard<std::mutex> lock(_mutex);

    for (auto it = _cooldownMs.begin(); it != _cooldownMs.end(); )
    {
        if (it->second <= diff) it = _cooldownMs.erase(it);
        else { it->second -= diff; ++it; }
    }

    uint32 lootIntervalMs = sPlayerbotAIConfig.raidSimLootInterval * 60u * 1000u;
    uint32 durationMs = sPlayerbotAIConfig.raidSimDuration * 60u * 1000u;
    std::vector<uint32> ended;
    for (auto& kv : _runs)
    {
        ActiveRun& run = kv.second;
        run.elapsedMs += diff;
        run.lootTimerMs += diff;
        if (run.lootTimerMs >= lootIntervalMs)
        {
            run.lootTimerMs = 0;
            AwardLoot(run);  // Task 8
        }
        if (run.elapsedMs >= durationMs)
            ended.push_back(kv.first);
    }
    for (uint32 guildId : ended)
    {
        EndRun(_runs[guildId]);
        _runs.erase(guildId);
    }

    _schedTimerMs += diff;
    if (_schedTimerMs < 60u * 1000u)
        return;
    _schedTimerMs = 0;

    // Candidate sim guilds: have an online bot, no active run, off cooldown.
    // ObjectAccessor::GetPlayers() (not sRandomPlayerbotMgr) — see the note in Start().
    std::unordered_map<uint32, std::string> candidateGuilds;
    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* bot = it.second;
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            continue;
        uint32 gid = bot->GetGuildId();
        if (!gid || _runs.count(gid) || _cooldownMs.count(gid))
            continue;
        if (Guild* g = sGuildMgr->GetGuildById(gid))
            candidateGuilds[gid] = g->GetName();
    }

    for (auto const& cg : candidateGuilds)
    {
        uint32 guildId = cg.first;
        std::string const& guildName = cg.second;

        // Boot-spread: the first time we ever consider a guild, give it a random initial cooldown in
        // [0, DungeonPeriod] instead of launching, so guilds don't all fire on the first pass after a
        // restart. (candidateGuilds already excludes guilds currently on cooldown or in a run.)
        if (!_seenGuilds.count(guildId))
        {
            _seenGuilds.insert(guildId);
            uint32 spreadMs = sPlayerbotAIConfig.raidSimDungeonPeriod * 60u * 1000u;
            if (spreadMs > 0)
            {
                _cooldownMs[guildId] = urand(0, spreadMs);
                continue;
            }
        }

        // Gather ALL grabbable bots (any level), keeping each bot's level so we can bucket: level-80
        // bots feed the endgame ilvl regime; the rest feed the level-gated leveling regime.
        std::vector<std::pair<ObjectGuid, uint8>> grabbable;
        std::vector<ObjectGuid> bots80;
        std::vector<uint8> levels;
        for (auto const& it : ObjectAccessor::GetPlayers())
        {
            Player* bot = it.second;
            if (!BotPassesBaseEligibility(bot, guildId))
                continue;
            if (_raiding.find(bot->GetGUID()) != _raiding.end())
                continue;
            uint8 lv = bot->GetLevel();
            grabbable.emplace_back(bot->GetGUID(), lv);
            levels.push_back(lv);
            if (lv >= 80)
                bots80.push_back(bot->GetGUID());
        }

        if (grabbable.size() < sPlayerbotAIConfig.raidSimMinDungeon)
            continue;  // not enough grabbable to field even a 5-man

        // Highest bracket first: endgame (level 80) wins when fillable.
        RaidSimInstance inst;
        if (bots80.size() >= sPlayerbotAIConfig.raidSimMinDungeon &&
            ResolveGuildInstance(guildName, uint32(bots80.size()), inst))
        {
            if (bots80.size() > inst.groupSize)
                bots80.resize(inst.groupSize);
            LaunchRun(guildId, guildName, inst, bots80);
            continue;
        }

        // Else the highest seeded leveling dungeon whose in-range cohort reaches MinDungeon.
        RaidSimInstance linst;
        if (ResolveLevelingInstance(levels, linst))
        {
            std::vector<ObjectGuid> cohort;
            for (auto const& gl : grabbable)
                if (gl.second >= linst.levelLo && gl.second <= linst.levelHi)
                    cohort.push_back(gl.first);
            if (cohort.size() > linst.groupSize)
                cohort.resize(linst.groupSize);
            LaunchRun(guildId, guildName, linst, cohort);
        }
    }
}

void RaidSimulationMgr::ReconcileOrphans(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidSimOrphanReaper)
        return;

    // --- Short lock: advance our own timer and snapshot live-run group GUIDs. ---
    std::unordered_set<ObjectGuid> activeGroupGuids;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _reaperTimerMs += diff;
        if (_reaperTimerMs < sPlayerbotAIConfig.raidSimReaperInterval * 1000u)
            return;
        _reaperTimerMs = 0;
        for (auto const& kv : _runs)
            if (kv.second.groupGuid)
                activeGroupGuids.insert(kv.second.groupGuid);
    }
    // _mutex released — NO Group::Disband under the lock.

    uint32 const batch = sPlayerbotAIConfig.raidSimReaperBatch;

    // --- Lock-free scan: candidate groups via online bots, deduped by group GUID. ---
    std::unordered_set<ObjectGuid> seenGroups;
    std::vector<Group*> toDisband;   // capped at batch
    uint32 totalOrphans = 0;

    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* bot = it.second;
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            continue;

        Group* grp = bot->GetGroup();
        if (!grp)
            continue;

        ObjectGuid gguid = grp->GetGUID();
        if (!seenGroups.insert(gguid).second)
            continue;  // already evaluated this group

        // Rule 1: normal party/raid only.
        if (grp->isLFGGroup() || grp->isBGGroup() || grp->isBFGroup())
            continue;

        // Rule 3: skip live in-flight runs.
        if (activeGroupGuids.find(gguid) != activeGroupGuids.end())
            continue;

        // Rule 2: every member's account is a random-bot account.
        auto const& slots = grp->GetMemberSlots();
        if (slots.empty())
            continue;  // degenerate -> treat as NOT all-bot
        bool allBots = true;
        for (auto const& ms : slots)
        {
            uint32 acc = sCharacterCache->GetCharacterAccountIdByGuid(ms.guid);
            if (!sPlayerbotAIConfig.IsInRandomAccountList(acc))
            {
                allBots = false;
                break;
            }
        }
        if (!allBots)
            continue;

        // Orphan confirmed.
        ++totalOrphans;
        if (toDisband.size() < batch)
            toDisband.push_back(grp);
    }

    if (toDisband.empty())
        return;

    uint32 disbanded = 0;
    for (Group* g : toDisband)
    {
        g->Disband(true);
        ++disbanded;
    }

    uint32 remaining = (totalOrphans > disbanded) ? (totalOrphans - disbanded) : 0u;
    LOG_INFO("playerbots", "RaidSim: reaper disbanded {} orphan group(s) ({} remaining)",
             disbanded, remaining);
}

std::vector<uint32> RaidSimulationMgr::BuildPool(RaidSimInstance const& inst)
{
    std::vector<uint32> pool;
    uint32 baseCount = 0, bossCount = 0, currencyCount = 0, chestCount = 0;

    auto passesGate = [&](uint32 itemId) -> bool
    {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(itemId);
        return p && (p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR)
            && p->Quality >= inst.minQuality && p->ItemLevel <= inst.ilvlCap;
    };
    auto expandCurrency = [&](uint32 rawEntry)
    {
        auto it = _currencyExpansion.find(rawEntry);
        if (it == _currencyExpansion.end())
            return;
        for (uint32 v : it->second)
            if (passesGate(v)) { pool.push_back(v); ++currencyCount; }
    };

    // 1. Creature base — existing proven query (already class 2/4 + quality + ilvl filtered).
    if (QueryResult pr = WorldDatabase.Query(
            BuildPoolQuery(inst.mapId, inst.difficulty, inst.minQuality, inst.ilvlCap).c_str()))
        do { pool.push_back(pr->Fetch()[0].Get<uint32>()); ++baseCount; } while (pr->NextRow());

    // 1b. Curated boss entries — summoned/scripted bosses with no static spawn (resolved directly
    //     by creature_entry, not by map). Same item gate as the creature base; raw drops also feed
    //     currency expansion so summoned-boss tokens/emblems expand too. (A curated boss that also
    //     has static spawns would double-feed expandCurrency, but step-4 dedup makes that harmless.)
    auto bossIt = _bossLoot.find({inst.mapId, inst.difficulty});
    if (bossIt != _bossLoot.end())
    {
        if (QueryResult bpr = WorldDatabase.Query(
                BuildBossPoolQuery(bossIt->second, inst.difficulty, inst.minQuality,
                                   inst.ilvlCap).c_str()))
            do { pool.push_back(bpr->Fetch()[0].Get<uint32>()); ++bossCount; } while (bpr->NextRow());

        if (QueryResult brr = WorldDatabase.Query(
                BuildBossRawQuery(bossIt->second, inst.difficulty).c_str()))
            do { expandCurrency(brr->Fetch()[0].Get<uint32>()); } while (brr->NextRow());
    }

    // 2. Currency/token expansion over every boss-drop entry on the map.
    if (QueryResult rr = WorldDatabase.Query(BuildRawLootQuery(inst.mapId, inst.difficulty).c_str()))
        do { expandCurrency(rr->Fetch()[0].Get<uint32>()); } while (rr->NextRow());

    // 3. Chest mining (chest instances only) + currency expansion of chest contents.
    auto chestIt = _chestLoot.find({inst.mapId, inst.difficulty});
    if (chestIt != _chestLoot.end())
        for (uint32 go : chestIt->second)
            if (QueryResult cr = WorldDatabase.Query(BuildChestRawQuery(go).c_str()))
                do
                {
                    uint32 itemId = cr->Fetch()[0].Get<uint32>();
                    if (passesGate(itemId)) { pool.push_back(itemId); ++chestCount; }
                    expandCurrency(itemId);
                } while (cr->NextRow());

    // 4. Dedup.
    std::sort(pool.begin(), pool.end());
    pool.erase(std::unique(pool.begin(), pool.end()), pool.end());

    if (pool.empty())
        LOG_WARN("playerbots", "RaidSim: instance {} '{}' (map {} diff {}) has EMPTY loot pool.",
                 inst.id, inst.label, inst.mapId, uint32(inst.difficulty));
    // base/currency/chest are pre-dedup push counts; their sum may exceed pool.size() (step 4 dedup).
    LOG_INFO("playerbots", "RaidSim: instance {} '{}' pool={} (base {}, boss {}, currency {}, chest {}).",
             inst.id, inst.label, uint32(pool.size()), baseCount, bossCount, currencyCount, chestCount);
    return pool;
}

void RaidSimulationMgr::AwardLoot(RaidSimulationMgr::ActiveRun const& run)
{
    // Caller (Update) holds _mutex and runs on the world thread. Must NOT re-lock _mutex.
    auto poolIt = _pools.find(run.instanceId);
    if (poolIt == _pools.end() || poolIt->second.empty())
        return;
    std::vector<uint32> const& pool = poolIt->second;

    // Members online AND on the instance map (parked).
    std::vector<Player*> present;
    for (ObjectGuid const& g : run.members)
    {
        Player* p = ObjectAccessor::FindPlayer(g);
        if (p && p->IsInWorld() && p->GetMapId() == run.mapId)
            present.push_back(p);
    }
    if (present.empty())
        return;

    uint32 rolls = std::min<uint32>(sPlayerbotAIConfig.raidSimRollsPerInterval, present.size());
    for (uint32 r = 0; r < rolls; ++r)
    {
        Player* bot = present[(r + urand(0, present.size() - 1)) % present.size()];
        uint32 itemId = pool[urand(0, pool.size() - 1)];

        // swap=true so an OCCUPIED slot still resolves a valid dest (a geared L80 bot has every
        // slot full; swap=false would reject every upgrade — the steady-state case we exist for).
        uint16 dest = 0;
        InventoryResult can = bot->CanEquipNewItem(NULL_SLOT, dest, itemId, true);
        if (can != EQUIP_ERR_OK)
            continue;  // bot's class/spec/proficiency can't use this item at all

        uint8 slot = uint8(dest & 255);
        StatsWeightCalculator calc(bot);
        float newScore = calc.CalculateItem(itemId, 0, int32(slot));
        float curScore = 0.0f;
        if (Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            curScore = calc.CalculateItem(current->GetTemplate()->ItemId, current->GetItemRandomPropertyId(),
                                          int32(slot));

        if (sPlayerbotAIConfig.raidSimOnlyUpgrades && newScore <= curScore)
            continue;  // not an upgrade for this slot; skip (present-only sim, never downgrades)

        // Replace whatever is in the slot. Destroy the displaced item (no bag hoarding) BEFORE equipping
        // into the now-free slot — CanEquipNewItem(swap=true) already validated usability.
        if (Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

        Item* equipped = bot->EquipNewItem(dest, itemId, true);
        if (!equipped)
            continue;
        bot->AutoUnequipOffhandIfNeed();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        std::string itemName = proto ? proto->Name1 : std::to_string(itemId);
        LOG_INFO("playerbots", "RaidSim: '{}' {} receives {} ({})",
                 run.guildName, bot->GetName(), itemName, run.label);

        if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastLoot)
            if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
            {
                if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                {
                    if (proto && proto->Quality >= sPlayerbotAIConfig.raidSimBroadcastLootMinQuality)
                    {
                        std::string msg = bot->GetName() + " of " + run.guildName + " equips " +
                                          ChatHelper::FormatItem(proto) + " in " + run.label + ".";
                        WorldPacket data;
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                        sWorldSessionMgr->SendGlobalMessage(&data);
                    }
                }
                else
                    guild->BroadcastToGuild(bot->GetSession(), false, bot->GetName() + " receives " + itemName + ".",
                                            LANG_UNIVERSAL);
            }
    }
}
