/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * RaidSimSpike — THROWAWAY de-risk spike, not the production manager.
 * See docs/superpowers/specs/2026-05-29-raid-sim-spike-botraidbind.md.
 *
 * Purpose: prove the load-bearing assumption of the Autonomous Instance Simulation design
 * (2026-05-29-guild-raid-tier-gearing-design.md, v5): that a bot-only group can be formed,
 * set to raid difficulty, teleported into a raid instance, and HELD parked there without
 * RandomPlayerbotMgr disbanding the group or teleporting the bots out.
 *
 * Scope (deliberately minimal): one hardcoded instance (Naxxramas, map 533), manual GM
 * trigger, an in-memory RAIDING set queried by RandomPlayerbotMgr guards. No tier table, no
 * loot, no progression, no scheduler tick. Those belong to the real RaidSimulationMgr.
 */

#ifndef _RAIDSIM_SPIKE_H
#define _RAIDSIM_SPIKE_H

#include "ObjectGuid.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChatHandler;

class RaidSimSpike
{
public:
    static RaidSimSpike& instance()
    {
        static RaidSimSpike instance;
        return instance;
    }

    // Queried by RandomPlayerbotMgr's guard set (world thread). Cheap set lookup.
    bool IsRaiding(ObjectGuid guid);

    // GM-command entry points. Called on the world thread from PlayerbotCommandScript.
    bool Start(ChatHandler* handler, std::string const& guildName);
    bool Stop(ChatHandler* handler, std::string const& guildName);
    void Status(ChatHandler* handler);

    // Called by the teardown operation (world thread) once bots are escorted home, so the
    // RandomPlayerbotMgr guards keep protecting them until they actually leave the instance.
    void ClearRaidingFlags(std::vector<ObjectGuid> const& members);

private:
    RaidSimSpike() = default;

    struct Run
    {
        uint32 guildId = 0;
        std::string guildName;
        ObjectGuid leader;
        std::vector<ObjectGuid> members;  // includes the leader
        uint32 mapId = 0;
        uint8 difficulty = 0;
    };

    std::mutex _mutex;
    std::unordered_set<ObjectGuid> _raiding;  // fast guard lookup
    std::unordered_map<uint32, Run> _runs;    // keyed by guildId
};

#define sRaidSimSpike RaidSimSpike::instance()

#endif
