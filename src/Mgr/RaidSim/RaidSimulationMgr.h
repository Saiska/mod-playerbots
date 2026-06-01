/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * RaidSimulationMgr — autonomous instance simulation as the sole post-creation gear source for
 * bot guilds. See docs/superpowers/specs/2026-05-29-guild-raid-tier-gearing-design.md (v5) and
 * docs/superpowers/plans/2026-05-30-raid-simulation-mgr.md. Generalizes the earlier validated spike.
 *
 * Instances are grouped into ilvl bands; a guild runs a RANDOM instance within its highest
 * unlocked band each run.
 */

#ifndef _RAIDSIM_MGR_H
#define _RAIDSIM_MGR_H

#include "ObjectGuid.h"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ChatHandler;
class Player;

// One ladder row = one (instance, difficulty) at a band.
struct RaidSimInstance
{
    uint32 id = 0;
    uint8  band = 0;
    uint32 mapId = 0;
    uint8  difficulty = 0;
    uint8  groupSize = 5;
    uint16 gateIlvl = 0;
    uint16 ilvlCap = 0;
    uint8  minQuality = 4;
    uint8  levelLo = 0;   // leveling rows only: cohort window lower bound (endgame rows leave 0)
    uint8  levelHi = 0;   // leveling rows only: cohort window upper bound
    std::string label;
    float entryX = 0, entryY = 0, entryZ = 0, entryO = 0;
    float parkX = 0, parkY = 0, parkZ = 0, parkO = 0;
};

class RaidSimulationMgr
{
public:
    static RaidSimulationMgr& instance()
    {
        static RaidSimulationMgr instance;
        return instance;
    }

    void LoadFromDB();                  // world startup: base_ilvl, banded ladder, per-instance pools
    void Update(uint32 diff);           // ticked from PlayerbotsWorldScript::OnUpdate (world thread)
    bool IsRaiding(ObjectGuid guid);    // RandomPlayerbotMgr guard set
    void ConsiderPlayerIlvl(Player* player);  // monotonic base_ilvl from real players

    bool Start(ChatHandler* handler, std::string const& guildName);  // GM cmd (world thread)
    bool Stop(ChatHandler* handler, std::string const& guildName);
    void Status(ChatHandler* handler);

    void ClearRaidingFlags(std::vector<ObjectGuid> const& members);  // called by teardown op

private:
    RaidSimulationMgr() = default;

    struct ActiveRun
    {
        uint32 guildId = 0;
        std::string guildName;
        ObjectGuid leader;
        std::vector<ObjectGuid> members;  // includes the leader
        uint32 instanceId = 0;            // chosen RaidSimInstance::id (keys the loot pool)
        uint32 mapId = 0;
        uint8  difficulty = 0;
        uint8  groupSize = 5;             // content size actually run (drives per-content cadence)
        std::string label;
        uint32 elapsedMs = 0;
        uint32 lootTimerMs = 0;
    };

    // helpers (defined in the .cpp)
    bool  ResolveBaseBand(uint8& outBand) const;
    bool  ResolveGuildInstance(std::string const& guildName, uint32 avail, RaidSimInstance& out) const;
    // Sub-80: highest seeded leveling dungeon whose in-range cohort (>= MinDungeon of the given bot
    // levels) can field it. `levels` = the guild's grabbable bots' levels. Returns the dungeon only;
    // the caller re-selects the in-range bots.
    bool  ResolveLevelingInstance(std::vector<uint8> const& levels, RaidSimInstance& out) const;
    void  LaunchRun(uint32 guildId, std::string const& guildName, RaidSimInstance const& inst,
                    std::vector<ObjectGuid> const& members);
    void  EndRun(ActiveRun const& run);
    void  AwardLoot(ActiveRun const& run);
    // Build instanceId's loot pool: creature base (class 2/4) + currency/token expansion
    // (all npc_vendor edges) + chest mining (chest instances only), deduped. Logs composition.
    std::vector<uint32> BuildPool(RaidSimInstance const& inst);
    void  PersistBaseIlvl();

    std::mutex _mutex;
    uint16 _baseIlvl = 0;
    std::map<uint8, std::vector<RaidSimInstance>> _bands;   // band -> instances (ordered by band)
    std::vector<RaidSimInstance> _leveling;                 // sub-80 dungeons, sorted by levelHi desc
    std::unordered_map<uint32, std::vector<uint32>> _pools; // instanceId -> equippable item entries
    // reqItem entry (tier token / emblem / badge) -> vendor item entries obtainable for it.
    // Built once at load from npc_vendor x ItemExtendedCost (DBC). Read-only after load.
    std::unordered_map<uint32, std::vector<uint32>> _currencyExpansion;
    // (mapId, difficulty) -> summoned cache GO entries for chest-loot instances (616/649/650).
    std::map<std::pair<uint32, uint8>, std::vector<uint32>> _chestLoot;
    std::unordered_map<uint32, ActiveRun> _runs;            // by guildId
    std::unordered_map<uint32, uint32> _cooldownMs;         // guildId -> remaining cooldown ms
    std::unordered_set<ObjectGuid> _raiding;
    uint32 _schedTimerMs = 0;
    std::unordered_set<uint32> _seenGuilds;  // guilds given a boot-spread initial cooldown already
};

#define sRaidSimulationMgr RaidSimulationMgr::instance()

#endif
