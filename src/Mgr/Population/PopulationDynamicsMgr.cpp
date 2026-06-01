/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "PopulationDynamicsMgr.h"
#include "PlayerbotAIConfig.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <algorithm>

uint32 PopulationDynamicsMgr::BracketOf(uint8 level)
{
    if (level >= 80) return 8;
    if (level < 10)  return 0;
    return uint32(level / 10);            // 10-19 -> 1, ..., 70-79 -> 7
}

uint8 PopulationDynamicsMgr::ComputeCap(uint8 frontier) const
{
    uint32 c = std::max<uint32>(sPlayerbotAIConfig.populationMinCap,
                                uint32(frontier) + sPlayerbotAIConfig.populationHeadroom);
    return uint8(std::min<uint32>(80, c));
}

float PopulationDynamicsMgr::Openness(uint32 b, uint8 cap) const
{
    uint8 lo = BracketLower(b), hi = BracketUpper(b);
    if (cap >= hi) return 1.0f;
    if (cap <  lo) return 0.0f;
    return float(int(cap) - int(lo) + 1) / float(int(hi) - int(lo) + 1);
}

void PopulationDynamicsMgr::ComputeTargets(uint8 cap, std::array<uint32, 9>& targets, uint32& outP) const
{
    outP = 0;
    for (uint32 b = 0; b < 9; ++b)
    {
        float share = float(sPlayerbotAIConfig.populationBracketPct[b]) / 100.0f;
        uint32 t = uint32(std::lround(float(sPlayerbotAIConfig.populationMaxPopulation) * share * Openness(b, cap)));
        targets[b] = t;
        outP += t;
    }
}

void PopulationDynamicsMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (QueryResult r = CharacterDatabase.Query("SELECT max_player_level FROM playerbots_population_state WHERE id = 1"))
        _frontier = r->Fetch()[0].Get<uint8>();
    else
        LOG_INFO("playerbots", "PopDyn: playerbots_population_state row absent; frontier defaulting to 0.");

    LOG_INFO("playerbots", "PopDyn: loaded frontier={}.", uint32(_frontier));
}

void PopulationDynamicsMgr::PersistFrontier()
{
    CharacterDatabase.Execute("UPDATE playerbots_population_state SET max_player_level = {} WHERE id = 1", uint32(_frontier));
}

void PopulationDynamicsMgr::ConsiderPlayerLevel(Player* player)
{
    if (!player)
        return;
    if (!sPlayerbotAIConfig.populationDynamicsEnable)
        return;
    uint8 lvl = player->GetLevel();
    std::lock_guard<std::mutex> lock(_mutex);
    if (lvl > _frontier)                      // monotonic — only ever rises
    {
        _frontier = lvl;
        PersistFrontier();
        LOG_INFO("playerbots", "PopDyn: frontier advanced to {}.", uint32(_frontier));
    }
}

void PopulationDynamicsMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.populationDynamicsEnable)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    _tickTimerMs += diff;
    if (_tickTimerMs < sPlayerbotAIConfig.populationPeriod * 1000u)
        return;
    _tickTimerMs = 0;

    uint8 cap = ComputeCap(_frontier);
    std::array<uint32, 9> targets{};
    uint32 P = 0;
    ComputeTargets(cap, targets, P);

    LOG_INFO("playerbots", "PopDyn tick: F={} C={} P={} targets=[{},{},{},{},{},{},{},{},{}]",
             uint32(_frontier), uint32(cap), P,
             targets[0], targets[1], targets[2], targets[3], targets[4],
             targets[5], targets[6], targets[7], targets[8]);

    // Reconcile flows (census, bottom inflow, drift, top-prune) land in Tasks 5-8.
}
