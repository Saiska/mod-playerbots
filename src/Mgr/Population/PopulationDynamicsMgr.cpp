/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "PopulationDynamicsMgr.h"
#include "PlayerbotAIConfig.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <algorithm>
#include "Group.h"
#include "ObjectAccessor.h"
#include "RaidSimulationMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

uint32 PopulationDynamicsMgr::BandOf(uint8 level)
{
    // Maps a LEVELING level (1..79) to its 10-level band 0..7. Level 80 is the sink, handled separately.
    if (level < 10) return 0;             // 1-9 -> band 0
    return uint32(level / 10);            // 10-19 -> 1, ..., 70-79 -> 7
}

uint8 PopulationDynamicsMgr::ComputeCap(uint8 frontier) const
{
    uint32 c = std::max<uint32>(sPlayerbotAIConfig.populationMinCap,
                                uint32(frontier) + sPlayerbotAIConfig.populationHeadroom);
    return uint8(std::min<uint32>(80, c));
}


void PopulationDynamicsMgr::ComputeTargets(uint8 cap, Census const& census,
                                           std::array<uint32, 81>& targets, uint32& outP) const
{
    targets = {};                                          // index 0 unused; all levels default 0
    outP = 0;

    // Leveling levels 1..79: each gets its band's bots-per-level knob, but only if authorized (<= cap).
    for (uint8 lvl = 1; lvl <= 79; ++lvl)
    {
        if (lvl > cap)                                     // level not yet authorized by the frontier
            continue;
        targets[lvl] = sPlayerbotAIConfig.populationBracket[BandOf(lvl)];
        outP += targets[lvl];                              // spawn target counts leveling targets only...
    }

    // Level 80 = the sink / remainder: MaxPopulation minus every band's full contribution.
    // span(band 0) = 9 levels (1..9); span(bands 1..7) = 10 levels each.
    uint32 bandsSum = sPlayerbotAIConfig.populationBracket[0] * 9u;
    for (uint32 b = 1; b < 8; ++b)
        bandsSum += sPlayerbotAIConfig.populationBracket[b] * 10u;

    uint32 maxPop = sPlayerbotAIConfig.populationMaxPopulation;
    if (bandsSum > maxPop)
    {
        LOG_WARN("playerbots", "PopDyn: band sum {} exceeds MaxPopulation {}; clamping target[80]=0.", bandsSum, maxPop);
        targets[80] = 0;
    }
    else
        targets[80] = maxPop - bandsSum;

    // ...plus the ACTUAL current level-80 population (count[80]), so P tracks the slowly-filling sink
    // (spec §3): the spawner injects fresh base bots only as the sink moves bots 79->80, never all at once.
    outP += census.count[0][80] + census.count[1][80];
}

void PopulationDynamicsMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (QueryResult r = CharacterDatabase.Query("SELECT max_player_level FROM playerbots_population_state WHERE id = 1"))
        _frontier = r->Fetch()[0].Get<uint8>();
    else
        LOG_INFO("playerbots", "PopDyn: playerbots_population_state row absent; frontier defaulting to 0.");

    LOG_INFO("playerbots", "PopDyn: loaded frontier={}.", uint32(_frontier));

    // Bound the native autologin to our target P from world-init, BEFORE the update-loop
    // RandomPlayerbotMgr autologin runs — otherwise it picks urand(min,max) and overshoots P
    // before the first controller tick can clamp it (there is no log-out-surplus path; the
    // population only grows, so it never needs one). Gated on enable so a disabled controller
    // never touches the native count.
    _cap = ComputeCap(_frontier);             // cache cap for the DK-login gate (set even when disabled is harmless)

    if (sPlayerbotAIConfig.populationDynamicsEnable)
    {
        Census census;
        TakeCensus(census);
        std::array<uint32, 81> targets{};
        uint32 P = 0;
        ComputeTargets(_cap, census, targets, P);
        sRandomPlayerbotMgr.SetPopulationTarget(P);
        LOG_INFO("playerbots", "PopDyn: initial population target P={} set at world-init (cap={}).", P, uint32(_cap));
    }
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

bool PopulationDynamicsMgr::IsSafeBot(Player* bot) const
{
    if (!bot || !bot->GetSession() || bot->GetSession()->isLogingOut() || bot->IsDuringRemoveFromWorld())
        return false;
    if (!bot->IsInWorld() || !bot->IsAlive())
        return false;
    if (bot->IsInCombat())
        return false;
    if (bot->InBattleground() || bot->InArena() || bot->inRandomLfgDungeon() || bot->InBattlegroundQueue())
        return false;
    if (bot->IsInFlight())
        return false;
    if (bot->GetMap() && bot->GetMap()->IsDungeon())     // in an instance map (raid-sim parks bots here)
        return false;
    if (sRaidSimulationMgr.IsRaiding(bot->GetGUID()))    // never touch a bot being geared by raid-sim
        return false;
    if (Group* group = bot->GetGroup())                  // skip bots grouped with a real player
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (m && m->IsInWorld() && !m->GetSession()->IsBot())
                return false;
        }
    }
    return true;
}

void PopulationDynamicsMgr::TakeCensus(Census& out) const
{
    out = Census{};
    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld() || !p->GetSession()->IsBot())
            continue;
        uint8 lvl = p->GetLevel();
        if (lvl < 1 || lvl > 80)                           // defensive: ignore out-of-range levels
            continue;
        uint32 faction = p->GetTeamId() == TEAM_ALLIANCE ? 0u : 1u;
        out.count[faction][lvl]++;
        out.total++;
    }
}

void PopulationDynamicsMgr::CollectSafeBots(std::array<std::vector<Player*>, 81> perLevel[2]) const
{
    for (uint32 f = 0; f < 2; ++f)
        for (uint32 lvl = 0; lvl < 81; ++lvl)
            perLevel[f][lvl].clear();

    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld() || !p->GetSession()->IsBot())
            continue;
        if (!IsSafeBot(p))
            continue;
        uint8 lvl = p->GetLevel();
        if (lvl < 1 || lvl > 80)
            continue;
        uint32 f = p->GetTeamId() == TEAM_ALLIANCE ? 0u : 1u;
        perLevel[f][lvl].push_back(p);
    }
    // No sort: the conveyor picks a RANDOM safe bot at each source level (fairer than highest-first,
    // lets the column equilibrate instead of escalating one cohort — spec §4).
}

uint32 PopulationDynamicsMgr::DriftUp(std::array<uint32, 81> const& targets, Census const& census,
                                      std::array<std::vector<Player*>, 81> perLevel[2])
{
    uint32 issuedPerFaction[2] = { 0, 0 };

    uint8 cap = CurrentCap();
    for (uint32 f = 0; f < 2; ++f)
    {
        // Per-faction budget: the MaxPromotionsPerCycle ceiling, consumed top-down so the highest
        // deficits fill first. No DriftRate (retired) — the cap alone bounds the per-cycle movement.
        uint32 budget = sPlayerbotAIConfig.populationMaxPromotionsPerCycle;
        if (budget == 0)
            continue;

        uint32 issued = 0;
        // Top-down conveyor: fill each authorized level L from a RANDOM safe bot at L-1.
        // NEVER touch L=80 (that is the sink gate, §5). L=1 has no L-1 (base is spawner-fed).
        for (int L = 79; L >= 2 && issued < budget; --L)
        {
            if (uint32(L) > cap)                           // level not authorized yet
                continue;

            uint32 want = targets[uint32(L)] / 2;          // per-faction target for level L
            if (census.count[f][uint32(L)] >= want)
                continue;                                  // level L already at/over target

            uint32 need = std::min(want - census.count[f][uint32(L)], budget - issued);
            std::vector<Player*>& src = perLevel[f][uint32(L) - 1];   // source: level L-1

            while (need > 0 && !src.empty())
            {
                // Random source pick (AzerothCore idiom): swap a random element to the back, pop it.
                uint32 idx = urand(0, uint32(src.size()) - 1);
                std::swap(src[idx], src.back());
                Player* bot = src.back();
                src.pop_back();

                sRandomPlayerbotMgr.IncreaseLevel(bot);    // +1 (L-1 -> L), re-gears
                ++issued;
                --need;
                if (issued >= budget)
                    break;
            }
        }
        issuedPerFaction[f] = issued;
    }

    uint32 total = issuedPerFaction[0] + issuedPerFaction[1];
    LOG_INFO("playerbots", "PopDyn conveyor: promoted={} (A={} H={}) (cycleCap={})",
             total, issuedPerFaction[0], issuedPerFaction[1],
             sPlayerbotAIConfig.populationMaxPromotionsPerCycle);
    return total;
}

uint32 PopulationDynamicsMgr::SinkGate(std::array<uint32, 81> const& targets, Census const& census,
                                       std::array<std::vector<Player*>, 81> perLevel[2])
{
    // The ONLY path into level 80, on its own slow timer. Decoupled from the conveyor so the
    // endgame fills gradually (spec §5). Returns early per faction when not applicable.
    uint32 issuedPerFaction[2] = { 0, 0 };

    if (CurrentCap() < 80)                                  // endgame not open (needs frontier >= 70)
    {
        // Still emit the line for greppability/symmetry once we are near the cap; cheap.
        LOG_INFO("playerbots", "PopDyn sink: promoted=0 (A=0 H=0) count80={} (closed: cap<80)",
                 census.count[0][80] + census.count[1][80]);
        return 0;
    }

    uint32 batch = sPlayerbotAIConfig.populationSinkBatch;
    for (uint32 f = 0; f < 2; ++f)
    {
        uint32 want = targets[80] / 2;                      // per-faction level-80 target
        if (census.count[f][80] >= want)
            continue;                                       // sink full for this faction

        uint32 room   = want - census.count[f][80];
        uint32 toMove = std::min(batch, room);
        std::vector<Player*>& src = perLevel[f][79];        // source: level 79

        while (toMove > 0 && !src.empty())
        {
            uint32 idx = urand(0, uint32(src.size()) - 1);  // random level-79 bot
            std::swap(src[idx], src.back());
            Player* bot = src.back();
            src.pop_back();

            sRandomPlayerbotMgr.IncreaseLevel(bot);         // +1 (79 -> 80), re-gears
            ++issuedPerFaction[f];
            --toMove;
        }
    }

    uint32 total = issuedPerFaction[0] + issuedPerFaction[1];
    LOG_INFO("playerbots", "PopDyn sink: promoted={} (A={} H={}) count80={}",
             total, issuedPerFaction[0], issuedPerFaction[1],
             census.count[0][80] + census.count[1][80]);
    return total;
}

uint32 PopulationDynamicsMgr::PruneTop(std::array<uint32, 81> const& targets, Census const& census,
                                       std::array<std::vector<Player*>, 81> perLevel[2])
{
    // Dormant insurance (spec §6): the conveyor never pushes past target and the sink stops at
    // target[80], so count[80] <= target[80] in normal operation. This fires only if target[80] later
    // DROPS below the current level-80 population (MaxPopulation lowered / band counts raised) — then it
    // sheds the excess endgame bots, per-faction budget, never below target. Level 80 is the ONLY level
    // that can exceed its target (all leveling movement is conveyor-driven + RandomBotFixedLevel).
    uint32 removed = 0;
    for (uint32 f = 0; f < 2; ++f)
    {
        uint32 budget = sPlayerbotAIConfig.populationMaxPromotionsPerCycle;  // PER-FACTION ceiling
        uint32 want   = targets[80] / 2;                 // per-faction target for level 80
        if (census.count[f][80] <= want)
            continue;
        uint32 surplus = census.count[f][80] - want;

        uint32 removedThisFaction = 0;
        for (Player* bot : perLevel[f][80])              // safe level-80 bots only
        {
            if (removedThisFaction >= budget || surplus == 0)
                break;
            sRandomPlayerbotMgr.Remove(bot);             // immediate delete + logout (recycles the slot)
            --surplus;
            ++removedThisFaction;
        }
        removed += removedThisFaction;
    }
    return removed;
}

void PopulationDynamicsMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.populationDynamicsEnable)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
    _tickTimerMs += diff;
    _sinkTimerMs += diff;

    bool conveyorTick = _tickTimerMs >= sPlayerbotAIConfig.populationPeriod * 1000u;
    bool sinkTick     = _sinkTimerMs >= sPlayerbotAIConfig.populationSinkPeriod * 1000u;
    if (!conveyorTick && !sinkTick)
        return;                                            // neither cadence elapsed — nothing to do

    uint8 cap = ComputeCap(_frontier);
    _cap = cap;                                            // keep the cached cap fresh for the DK-login gate

    Census census;
    TakeCensus(census);

    std::array<uint32, 81> targets{};
    uint32 P = 0;
    ComputeTargets(cap, census, targets, P);               // P uses census.count[80] (spec §3)

    // Band-summed census (can't log 81 per-level numbers). One sum per band 0..7 + count[80], per faction,
    // plus min/max occupied level. Greppable for live verification.
    uint32 bandA[8] = {}, bandH[8] = {};
    uint8  minLvl = 0, maxLvl = 0;
    for (uint8 lvl = 1; lvl <= 79; ++lvl)
    {
        uint32 a = census.count[0][lvl], h = census.count[1][lvl];
        bandA[BandOf(lvl)] += a;
        bandH[BandOf(lvl)] += h;
        if (a + h > 0) { if (minLvl == 0) minLvl = lvl; maxLvl = lvl; }
    }
    if (census.count[0][80] + census.count[1][80] > 0) { if (minLvl == 0) minLvl = 80; maxLvl = 80; }

    LOG_INFO("playerbots", "PopDyn tick: F={} C={} P={} count80={} occupied=[{}..{}]",
             uint32(_frontier), uint32(cap), P,
             census.count[0][80] + census.count[1][80], uint32(minLvl), uint32(maxLvl));
    LOG_INFO("playerbots", "PopDyn census: total={} bandA=[{},{},{},{},{},{},{},{}] bandH=[{},{},{},{},{},{},{},{}] c80=(A{} H{})",
             census.total,
             bandA[0],bandA[1],bandA[2],bandA[3],bandA[4],bandA[5],bandA[6],bandA[7],
             bandH[0],bandH[1],bandH[2],bandH[3],bandH[4],bandH[5],bandH[6],bandH[7],
             census.count[0][80], census.count[1][80]);

    sRandomPlayerbotMgr.SetPopulationTarget(P);

    std::array<std::vector<Player*>, 81> perLevel[2];
    CollectSafeBots(perLevel);

    if (conveyorTick)
    {
        _tickTimerMs = 0;
        DriftUp(targets, census, perLevel);                // logs its own "PopDyn conveyor" line
    }
    if (sinkTick)
    {
        _sinkTimerMs = 0;
        SinkGate(targets, census, perLevel);               // logs its own "PopDyn sink" line
    }

    uint32 pruned = PruneTop(targets, census, perLevel);   // dormant in normal operation (spec §6)
    LOG_INFO("playerbots", "PopDyn prune: removed={} (target80={})", pruned, targets[80]);
}
