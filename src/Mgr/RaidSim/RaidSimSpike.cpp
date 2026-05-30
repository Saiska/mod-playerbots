/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * RaidSimSpike — THROWAWAY de-risk spike. See RaidSimSpike.h and
 * docs/superpowers/specs/2026-05-29-raid-sim-spike-botraidbind.md.
 */

#include "RaidSimSpike.h"

#include "Chat.h"
#include "PlayerbotOperations.h"          // Group/Guild/ObjectAccessor/Player/PlayerbotAI/RandomPlayerbotMgr + base op
#include "PlayerbotWorldThreadProcessor.h"

#include <memory>

namespace
{
    // --- Hardcoded spike target: Naxxramas, 25-man normal, entrance foyer from areatrigger_teleport ---
    constexpr uint32 NAXX_MAP = 533;
    constexpr float ENTRY_X = 3005.68f, ENTRY_Y = -3447.77f, ENTRY_Z = 293.93f, ENTRY_O = 4.65f;
    constexpr uint8 SPIKE_DIFFICULTY = RAID_DIFFICULTY_25MAN_NORMAL;

    // Home = Dalaran (safe city both factions use at 80).
    constexpr uint32 HOME_MAP = 571;
    constexpr float HOME_X = 5804.15f, HOME_Y = 624.77f, HOME_Z = 647.76f, HOME_O = 1.64f;

    constexpr uint32 MIN_RAIDERS = 5;
    constexpr uint32 MAX_RAIDERS = 25;

    // Hold position + suppress combat in BOTH engine states. Direct ChangeStrategy does NOT
    // persist to the repo (PlayerbotAI.cpp:1576 -> Engine::ChangeStrategy), so a crash leaves no
    // frozen +stay/+passive residue. NOTE: a cross-map teleport may re-init the AI on arrival and
    // wipe these — whether that happens is exactly what spike criterion S7 observes.
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
        ai->Reset();  // rebuild default strategies for the bot's class/spec
    }

    // Forms the bot-only raid, sets difficulty, teleports all to the foyer, parks them.
    // Mirrors ArenaGroupFormationOperation (PlayerbotOperations.h) so it runs on the world thread.
    class RaidSimSpikeFormationOperation : public PlayerbotOperation
    {
    public:
        RaidSimSpikeFormationOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids))
        {
        }

        bool Execute() override
        {
            Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid);
            if (!leader)
            {
                LOG_ERROR("playerbots", "RaidSimSpike: leader not found at formation");
                return false;
            }

            // Tear down any pre-existing groups for everyone involved (leader handled separately).
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

            // Fresh raid group; difficulty set BEFORE entry so the InstanceMap is created at it.
            Group* group = new Group();
            if (!group->Create(leader))
            {
                delete group;
                LOG_ERROR("playerbots", "RaidSimSpike: failed to create group for leader {}", leader->GetName());
                return false;
            }
            sGroupMgr->AddGroup(group);
            group->ConvertToRaid();
            group->SetRaidDifficulty(Difficulty(SPIKE_DIFFICULTY));

            uint32 added = 0;
            for (ObjectGuid const& guid : m_memberGuids)
            {
                if (guid == m_leaderGuid)
                    continue;
                Player* m = ObjectAccessor::FindPlayer(guid);
                if (m && group->AddMember(m))
                    ++added;
            }

            // Teleport everyone (leader included) to the entrance foyer and park them.
            for (ObjectGuid const& guid : m_memberGuids)
            {
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p)
                    continue;
                p->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                p->TeleportTo(NAXX_MAP, ENTRY_X, ENTRY_Y, ENTRY_Z, ENTRY_O);
                ParkBot(p);
            }

            LOG_INFO("playerbots", "RaidSimSpike: formed raid leader={} members={} -> map {} diff {}",
                     leader->GetName(), added + 1, NAXX_MAP, uint32(SPIKE_DIFFICULTY));

            if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                guild->BroadcastToGuild(leader->GetSession(), false, "We set out for Naxxramas.", LANG_UNIVERSAL);
            return true;
        }

        ObjectGuid GetBotGuid() const override { return m_leaderGuid; }
        uint32 GetPriority() const override { return 60; }
        std::string GetName() const override { return "RaidSimSpikeFormation"; }
        bool IsValid() const override { return ObjectAccessor::FindPlayer(m_leaderGuid) != nullptr; }

    private:
        ObjectGuid m_leaderGuid;
        std::vector<ObjectGuid> m_memberGuids;
    };

    // Unparks, teleports home, disbands the group.
    class RaidSimSpikeTeardownOperation : public PlayerbotOperation
    {
    public:
        RaidSimSpikeTeardownOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids))
        {
        }

        bool Execute() override
        {
            if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                    guild->BroadcastToGuild(leader->GetSession(), false, "We return from Naxxramas.", LANG_UNIVERSAL);

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

            // Lift the guards only now that bots are home — until here RandomPlayerbotMgr stays off them.
            sRaidSimSpike.ClearRaidingFlags(m_memberGuids);

            LOG_INFO("playerbots", "RaidSimSpike: teardown complete leader={}", m_leaderGuid.ToString());
            return true;
        }

        ObjectGuid GetBotGuid() const override { return m_leaderGuid; }
        uint32 GetPriority() const override { return 60; }
        std::string GetName() const override { return "RaidSimSpikeTeardown"; }
        bool IsValid() const override { return true; }

    private:
        ObjectGuid m_leaderGuid;
        std::vector<ObjectGuid> m_memberGuids;
    };
}  // namespace

bool RaidSimSpike::IsRaiding(ObjectGuid guid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _raiding.find(guid) != _raiding.end();
}

void RaidSimSpike::ClearRaidingFlags(std::vector<ObjectGuid> const& members)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (ObjectGuid const& g : members)
        _raiding.erase(g);
}

bool RaidSimSpike::Start(ChatHandler* handler, std::string const& guildName)
{
    Guild* guild = sGuildMgr->GetGuildByName(guildName);
    if (!guild)
    {
        handler->PSendSysMessage("RaidSimSpike: guild '{}' not found.", guildName);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runs.count(guild->GetId()))
        {
            handler->PSendSysMessage("RaidSimSpike: guild '{}' already has an active run. Stop it first.",
                                     guildName);
            return false;
        }
    }

    // Gather online, level-80, non-busy bots of this guild.
    // NOTE: must iterate ObjectAccessor::GetPlayers() (all online players), NOT
    // RandomPlayerbotMgr::GetPlayers() — the latter only holds non-random "alt" bots with a
    // human master; autologin random bots take the IsRandomBot branch in OnPlayerLogin and are
    // never added to that vector, so it returns ~0 for the random-bot population.
    std::vector<ObjectGuid> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
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
        if (IsRaiding(bot->GetGUID()))
            continue;
        eligible.push_back(bot->GetGUID());
        if (eligible.size() >= MAX_RAIDERS)
            break;
    }

    if (eligible.size() < MIN_RAIDERS)
    {
        handler->PSendSysMessage("RaidSimSpike: only {} eligible level-80 bots in '{}' (need {}).",
                                 uint32(eligible.size()), guildName, MIN_RAIDERS);
        return false;
    }

    ObjectGuid leader = eligible.front();

    Run run;
    run.guildId = guild->GetId();
    run.guildName = guildName;
    run.leader = leader;
    run.members = eligible;
    run.mapId = NAXX_MAP;
    run.difficulty = SPIKE_DIFFICULTY;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (ObjectGuid const& g : eligible)
            _raiding.insert(g);
        _runs[guild->GetId()] = run;
    }

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimSpikeFormationOperation>(leader, eligible));

    handler->PSendSysMessage(
        "RaidSimSpike: launching '{}' -> Naxxramas (25) with {} bots (leader {}). Watch Playerbots.log + characters DB.",
        guildName, uint32(eligible.size()), leader.ToString());
    LOG_INFO("playerbots", "RaidSimSpike: START guild='{}' bots={} leader={}", guildName, eligible.size(),
             leader.ToString());
    return true;
}

bool RaidSimSpike::Stop(ChatHandler* handler, std::string const& guildName)
{
    Guild* guild = sGuildMgr->GetGuildByName(guildName);
    if (!guild)
    {
        handler->PSendSysMessage("RaidSimSpike: guild '{}' not found.", guildName);
        return false;
    }

    Run run;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _runs.find(guild->GetId());
        if (it == _runs.end())
        {
            handler->PSendSysMessage("RaidSimSpike: no active run for '{}'.", guildName);
            return false;
        }
        run = it->second;
        _runs.erase(it);
        // NB: _raiding stays set here — the teardown op clears it once the bots are home, so the
        // RandomPlayerbotMgr guards keep protecting them through the teleport-out window.
    }

    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimSpikeTeardownOperation>(run.leader, run.members));

    handler->PSendSysMessage("RaidSimSpike: tearing down '{}' ({} bots) -> home.", guildName,
                             uint32(run.members.size()));
    LOG_INFO("playerbots", "RaidSimSpike: STOP guild='{}' bots={}", guildName, run.members.size());
    return true;
}

void RaidSimSpike::Status(ChatHandler* handler)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_runs.empty())
    {
        handler->PSendSysMessage("RaidSimSpike: no active runs. ({} bots flagged raiding)", uint32(_raiding.size()));
        return;
    }
    for (auto const& kv : _runs)
    {
        Run const& run = kv.second;
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
        handler->PSendSysMessage("RaidSimSpike: '{}' map {} diff {} | members {}, online {}, on-map {}",
                                 run.guildName, run.mapId, uint32(run.difficulty),
                                 uint32(run.members.size()), online, onMap);
    }
}
