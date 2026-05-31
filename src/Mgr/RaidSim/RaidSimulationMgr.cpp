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
#include "Playerbots.h"
#include "Random.h"
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

bool RaidSimulationMgr::ResolveGuildInstance(std::string const& guildName, RaidSimInstance& out) const
{
    GuildTheme const& theme = PlayerbotGuildMgr::instance().GetThemeByName(guildName);
    if (theme.raidOffset < 0)
        return false;  // not a sim guild

    uint8 baseBand = 0;
    if (!ResolveBaseBand(baseBand))
        return false;  // nothing unlocked yet (base_ilvl too low)

    int band = int(baseBand) - int(theme.raidOffset);
    if (band < 0)
        band = 0;
    if (band > int(theme.maxBand))
        band = int(theme.maxBand);

    auto it = _bands.find(uint8(band));
    if (it == _bands.end() || it->second.empty())
        return false;

    // Random instance within the band (variety).
    std::vector<RaidSimInstance> const& choices = it->second;
    out = choices[urand(0, choices.size() - 1)];
    return true;
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
    run.label = inst.label;
    _runs[guildId] = run;
    for (ObjectGuid const& g : members)
        _raiding.insert(g);

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimFormationOperation>(run.leader, members, inst.mapId, inst.difficulty,
                                                    inst.label, inst.entryX, inst.entryY, inst.entryZ, inst.entryO));

    LOG_INFO("playerbots", "RaidSim: LAUNCH guild='{}' band {} inst {} ({}) bots={} leader={}",
             guildName, uint32(inst.band), inst.id, inst.label, members.size(), run.leader.ToString());
}

void RaidSimulationMgr::EndRun(ActiveRun const& run)
{
    // Caller holds _mutex. _raiding stays set; the teardown op clears it once bots are home.
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RaidSimTeardownOperation>(run.leader, run.members, run.label)))
    {
        LOG_ERROR("playerbots", "RaidSim: teardown op dropped for guild '{}'; clearing raiding flags directly.",
                  run.guildName);
        for (ObjectGuid const& g : run.members)
            _raiding.erase(g);
    }
    _cooldownMs[run.guildId] = sPlayerbotAIConfig.raidSimGuildCooldown * 60u * 1000u;
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

    RaidSimInstance inst;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runs.count(guild->GetId()))
        {
            handler->PSendSysMessage("RaidSim: '{}' already has an active run. Stop it first.", guildName);
            return false;
        }
        if (!ResolveGuildInstance(guildName, inst))
        {
            handler->PSendSysMessage("RaidSim: '{}' has no eligible instance (raid_offset<0 or nothing unlocked).", guildName);
            return false;
        }
    }

    // CRITICAL: ObjectAccessor::GetPlayers() — the authoritative online-players map. NOT
    // sRandomPlayerbotMgr.GetPlayers() (only human-mastered alt bots; spike bug fixed in 12297d80).
    std::vector<ObjectGuid> eligible;
    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* bot = it.second;
        if (!bot || !bot->IsInWorld())
            continue;
        if (bot->GetGuildId() != guild->GetId())
            continue;
        if (!GET_PLAYERBOT_AI(bot))
            continue;
        if (bot->GetLevel() < 80)
            continue;
        if (bot->isDead() || bot->IsInCombat())
            continue;
        if (bot->InBattleground() || bot->InBattlegroundQueue())
            continue;
        if (bot->GetMap() && bot->GetMap()->IsDungeon())
            continue;  // already inside an instance; forming/disbanding would homebind it
        if (IsRaiding(bot->GetGUID()))
            continue;
        eligible.push_back(bot->GetGUID());
        if (eligible.size() >= inst.groupSize)
            break;
    }

    if (eligible.size() < sPlayerbotAIConfig.raidSimMinRaiders)
    {
        handler->PSendSysMessage("RaidSim: only {} eligible level-80 bots in '{}' (need {}).",
                                 uint32(eligible.size()), guildName, sPlayerbotAIConfig.raidSimMinRaiders);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runs.count(guild->GetId()))
        {
            handler->PSendSysMessage("RaidSim: '{}' was launched concurrently; skipping.", guildName);
            return false;
        }
        LaunchRun(guild->GetId(), guildName, inst, eligible);
    }

    handler->PSendSysMessage("RaidSim: launching '{}' -> {} (band {}) with {} bots.",
                             guildName, inst.label, uint32(inst.band), uint32(eligible.size()));
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

        RaidSimInstance inst;
        if (!ResolveGuildInstance(guildName, inst))
            continue;

        std::vector<ObjectGuid> eligible;
        for (auto const& it : ObjectAccessor::GetPlayers())
        {
            Player* bot = it.second;
            if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
                continue;
            if (bot->GetGuildId() != guildId)
                continue;
            if (bot->GetLevel() < 80 || bot->isDead() || bot->IsInCombat())
                continue;
            if (bot->InBattleground() || bot->InBattlegroundQueue())
                continue;
            if (bot->GetMap() && bot->GetMap()->IsDungeon())
                continue;  // already inside an instance
            if (_raiding.find(bot->GetGUID()) != _raiding.end())
                continue;
            eligible.push_back(bot->GetGUID());
            if (eligible.size() >= inst.groupSize)
                break;
        }
        if (eligible.size() >= sPlayerbotAIConfig.raidSimMinRaiders)
            LaunchRun(guildId, guildName, inst, eligible);
    }
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
                guild->BroadcastToGuild(bot->GetSession(), false, bot->GetName() + " receives " + itemName + ".",
                                        LANG_UNIVERSAL);
    }
}
