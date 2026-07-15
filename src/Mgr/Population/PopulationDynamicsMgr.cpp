/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "PopulationDynamicsMgr.h"
#include "PlayerbotAIConfig.h"
#include "Player.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <algorithm>
#include "Group.h"
#include "ObjectAccessor.h"
#include "RaidSimulationMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotGuildMgr.h"

uint32 PopulationDynamicsMgr::BandOf(uint8 level)
{
    // Maps a LEVELING level (1..79) to its 10-level band 0..7. Level 80 is the sink, handled separately.
    if (level < 10) return 0;             // 1-9 -> band 0
    return uint32(level / 10);            // 10-19 -> 1, ..., 70-79 -> 7
}

uint32 PopulationDynamicsMgr::CensusBand(uint8 level)
{
    return level >= 80 ? 8u : BandOf(level);          // 80 -> sink band 8
}

uint32 PopulationDynamicsMgr::EligibleClassCount(uint8 level)
{
    // WotLK: both factions can play all 10 classes; Death Knight only at level >= 55
    // (CONFIG_START_HEROIC_PLAYER_LEVEL). 9 below 55, 10 at/above.
    return uint32(level) >= sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL) ? 10u : 9u;
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
        _lastTargets = targets;                            // cache for RandomPlayerbotMgr::AddRandomBots (GetTargets)
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

bool PopulationDynamicsMgr::IsPromotable(Player* bot) const
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
    if (!sPlayerbotAIConfig.populationPromoteInInstances &&
        bot->GetMap() && bot->GetMap()->IsDungeon())
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

bool PopulationDynamicsMgr::IsSafeBot(Player* bot) const
{
    // Strict gatekeeper — always excludes dungeon maps regardless of PromoteInInstances.
    // Promotion sites use IsPromotable, which may relax the IsDungeon gate via config.
    return IsPromotable(bot) && !(bot->GetMap() && bot->GetMap()->IsDungeon());
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
        out.clsCount[faction][CensusBand(lvl)][p->getClass()]++;
        out.total++;
    }
}

void PopulationDynamicsMgr::ComputeClassDeficit(uint8 cap, Census const& census,
        std::array<uint32, 81> const& targets,
        float deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS]) const
{
    uint8 const dkFloor = uint8(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL)); // 55

    // 1) fair share per band per class = Sum over levels in band (<= cap) of
    //    (target[L]/2) / eligibleClasses(L), counting class c only where eligible.
    //    Faction-independent (per-faction want is target[L]/2 for both) -> compute once.
    float fair[POPDYN_BANDS][POPDYN_CLASS_SLOTS] = {};
    for (uint8 L = 1; L <= 80; ++L)
    {
        if (L > cap) continue;                          // unauthorized level contributes nothing
        uint32 const band = CensusBand(L);
        float const perClass = (float(targets[L]) / 2.0f) / float(EligibleClassCount(L));
        for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
        {
            if (c == 10u) continue;                       // class id 10 unused in WotLK
            if (c == static_cast<uint32>(CLASS_DEATH_KNIGHT) && L < dkFloor) continue;   // DK not eligible below 55
            fair[band][c] += perClass;
        }
    }

    // 2) deficit per faction = max(0, fair - census).
    for (uint32 f = 0; f < 2; ++f)
        for (uint32 b = 0; b < POPDYN_BANDS; ++b)
            for (uint32 c = 0; c < POPDYN_CLASS_SLOTS; ++c)
            {
                float d = fair[b][c] - float(census.clsCount[f][b][c]);
                deficit[f][b][c] = d > 0.0f ? d : 0.0f;
            }
}

void PopulationDynamicsMgr::CollectSafeBots(SafeBotPool& pool) const
{
    pool.clear();
    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* p = it.second;
        if (!p || !p->IsInWorld() || !p->GetSession()->IsBot())
            continue;
        if (!IsPromotable(p))
            continue;
        uint8 lvl = p->GetLevel();
        if (lvl < 1 || lvl > 80)
            continue;
        uint32 f = p->GetTeamId() == TEAM_ALLIANCE ? 0u : 1u;
        pool.bots[f][lvl][p->getClass()].push_back(p);
    }
    // No sort: the source pick is random within the chosen class (spec §3).
}

Player* PopulationDynamicsMgr::PickAnySource(uint32 f, uint32 srcLevel, SafeBotPool& pool)
{
    // Uniform over ALL safe bots at srcLevel regardless of class (legacy distribution).
    uint32 total = 0;
    for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
        total += uint32(pool.bots[f][srcLevel][c].size());
    if (total == 0)
        return nullptr;
    uint32 r = urand(0, total - 1);
    for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
    {
        auto& v = pool.bots[f][srcLevel][c];
        if (r < uint32(v.size()))
        {
            uint32 idx = r;                          // already uniform within this class slice
            std::swap(v[idx], v.back());
            Player* bot = v.back();
            v.pop_back();
            return bot;
        }
        r -= uint32(v.size());
    }
    return nullptr;                                  // unreachable (total>0)
}

Player* PopulationDynamicsMgr::PickFavoredSource(uint32 f, uint32 srcLevel, uint32 band,
        float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool)
{
    float weight[POPDYN_CLASS_SLOTS] = {};
    float totalW = 0.0f;
    for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
    {
        if (pool.bots[f][srcLevel][c].empty())
            continue;
        weight[c] = deficit[f][band][c];             // 0 if class at/over fair share
        totalW += weight[c];
    }

    if (totalW <= 0.0f)                              // every available class already fair -> legacy uniform
        return PickAnySource(f, srcLevel, pool);

    // weighted-random by deficit
    float r = (float(urand(1, 10000)) / 10000.0f) * totalW;   // (0, totalW]
    uint32 chosen = 0;
    float acc = 0.0f;
    for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
    {
        if (weight[c] <= 0.0f)
            continue;
        acc += weight[c];
        if (r <= acc) { chosen = c; break; }
    }
    if (chosen == 0)                                 // fp rounding guard: take the last weighted class
    {
        // Safe reverse scan: find highest c in [1, POPDYN_CLASS_SLOTS) with weight[c]>0.
        // Using a signed loop to avoid uint32 underflow when decrementing past 0.
        for (int32 ci = int32(POPDYN_CLASS_SLOTS) - 1; ci >= 1; --ci)
            if (weight[uint32(ci)] > 0.0f) { chosen = uint32(ci); break; }
    }

    auto& v = pool.bots[f][srcLevel][chosen];
    uint32 idx = urand(0, uint32(v.size()) - 1);
    std::swap(v[idx], v.back());
    Player* bot = v.back();
    v.pop_back();
    return bot;
}

void PopulationDynamicsMgr::BuildPlan(std::array<uint32, 81> const& targets, Census const& census,
                                      float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool)
{
    _pending.clear();
    _planDrained = _planPromoted = _planSkipped = 0;

    uint32 queuedPerFaction[2] = { 0, 0 };
    uint8 cap = CurrentCap();
    for (uint32 f = 0; f < 2; ++f)
    {
        uint32 budget = sPlayerbotAIConfig.populationMaxPromotionsPerCycle;
        if (budget == 0)
            continue;

        uint32 issued = 0;
        for (int L = 79; L >= 2 && issued < budget; --L)
        {
            if (uint32(L) > cap)
                continue;

            // Split the both-faction target across factions WITHOUT dropping the odd unit — the
            // SAME rule the login fill (RandomPlayerbotMgr::AddRandomBots) uses, so the conveyor and
            // the fill agree on which faction gets the extra bot at each level. Plain target/2
            // truncates (bracket 3 -> 1/faction), which pinned the belt at 1/faction/level: census 1
            // >= want 1, no deficit, nothing queued, and the L1 reservoir never drawn up. Distribute
            // the remainder by level parity so each level reaches floor(3/2)+1 = 2 on one faction and
            // 1 on the other => 3/level.
            uint32 want = targets[uint32(L)] / 2;
            if ((targets[uint32(L)] & 1u) && f == uint32(L) % 2u)
                want += 1u;
            if (!want || census.count[f][uint32(L)] >= want)
                continue;

            uint32 need = std::min(want - census.count[f][uint32(L)], budget - issued);

            while (need > 0)
            {
                // SAME class-favored source pick as the original DriftUp (preserves the DK fix).
                // PickFavoredSource/PickAnySource REMOVE the bot from the pool; we queue its GUID
                // + source level instead of promoting it now.
                Player* bot = sPlayerbotAIConfig.populationClassFavor
                    ? PickFavoredSource(f, uint32(L) - 1, BandOf(uint8(L)), deficit, pool)
                    : PickAnySource(f, uint32(L) - 1, pool);
                if (!bot)
                    break;

                _pending.push_back(PendingPromotion{ bot->GetGUID(), uint8(L - 1) });
                ++issued;
                --need;
                if (issued >= budget)
                    break;
            }
        }
        queuedPerFaction[f] = issued;
    }

    _planTotal = uint32(_pending.size());
    LOG_INFO("playerbots", "PopDyn plan: queued={} (A={} H={}) (cycleCap={})",
             _planTotal, queuedPerFaction[0], queuedPerFaction[1],
             sPlayerbotAIConfig.populationMaxPromotionsPerCycle);
}

void PopulationDynamicsMgr::DripDrain()
{
    if (_pending.empty())
        return;

    uint32 periodMs = sPlayerbotAIConfig.populationPeriod * 1000u;
    if (periodMs == 0)
        periodMs = 1;
    uint32 elapsed = std::min(_tickTimerMs, periodMs);
    uint32 due = uint32(uint64(_planTotal) * elapsed / periodMs);
    uint32 toDrain = due > _planDrained ? due - _planDrained : 0;

    while (toDrain > 0 && !_pending.empty())
    {
        PendingPromotion pp = _pending.back();
        _pending.pop_back();
        ++_planDrained;
        --toDrain;

        Player* bot = ObjectAccessor::FindPlayer(pp.guid);
        if (!bot || !IsPromotable(bot) || bot->GetLevel() != pp.expectLevel)
        {
            ++_planSkipped;
            continue;
        }
        sRandomPlayerbotMgr.IncreaseLevel(bot);         // +1 (expectLevel -> expectLevel+1), slim LevelUp
        ++_planPromoted;
    }
}

Player* PopulationDynamicsMgr::PullHighestOfClass(uint32 f, uint32 cls, uint8 floor,
                                                  SafeBotPool& pool, uint8& outLevel, uint32& outClass)
{
    // Highest level first (79 down to floor) so a well-fed class is pulled from near the top and
    // the jump stays small; only a starved class forces a deep reach.
    for (int L = 79; L >= int(floor); --L)
    {
        if (cls != 0)
        {
            std::vector<Player*>& v = pool.bots[f][L][cls];
            if (v.empty())
                continue;
            uint32 idx = urand(0, uint32(v.size()) - 1);
            std::swap(v[idx], v.back());
            Player* bot = v.back();
            v.pop_back();
            outLevel = uint8(L);
            outClass = cls;
            return bot;
        }

        for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)      // cls == 0 -> any class at this level
        {
            if (c == 10u)
                continue;
            std::vector<Player*>& v = pool.bots[f][L][c];
            if (v.empty())
                continue;
            uint32 idx = urand(0, uint32(v.size()) - 1);
            std::swap(v[idx], v.back());
            Player* bot = v.back();
            v.pop_back();
            outLevel = uint8(L);
            outClass = c;
            return bot;
        }
    }
    return nullptr;
}

uint32 PopulationDynamicsMgr::SinkGate(std::array<uint32, 81> const& targets, Census const& census,
                                       float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool)
{
    // The ONLY path into level 80. Greedy, deficit-targeted, widening: each promotion fills the single
    // most under-represented L80 class, pulling it from the highest available level in [floor..79] and
    // jumping it straight to 80. A live-mutable deficit copy makes a SinkBatch spread across classes.
    uint32 issuedPerFaction[2] = { 0, 0 };
    uint8  minSrc = 80;                                     // deepest widen this cycle (for the log)

    if (CurrentCap() < 80)                                  // endgame not open (needs frontier >= 70)
    {
        LOG_INFO("playerbots", "PopDyn sink: promoted=0 (A=0 H=0) count80={} (closed: cap<80)",
                 census.count[0][80] + census.count[1][80]);
        return 0;
    }

    uint32 batch = sPlayerbotAIConfig.populationSinkBatch;
    // ClassFavor off -> legacy: any-class, from level 79 only (no widening).
    uint8 floor = sPlayerbotAIConfig.populationClassFavor
        ? uint8(std::clamp<uint32>(sPlayerbotAIConfig.populationSinkReachFloor, 1u, 79u))
        : 79u;

    for (uint32 f = 0; f < 2; ++f)
    {
        uint32 want = targets[80] / 2;                      // per-faction level-80 target
        if (census.count[f][80] >= want)
            continue;
        uint32 toMove = std::min(batch, want - census.count[f][80]);

        // Live-mutable copy of the L80 per-class deficit (band 8) so this batch spreads across classes.
        float d[POPDYN_CLASS_SLOTS];
        for (uint32 c = 0; c < POPDYN_CLASS_SLOTS; ++c)
            d[c] = deficit[f][8][c];

        auto reachable = [&](uint32 c) -> bool
        {
            for (int L = 79; L >= int(floor); --L)
                if (!pool.bots[f][L][c].empty())
                    return true;
            return false;
        };

        while (toMove > 0)
        {
            uint8  srcLevel = 80;
            Player* bot = nullptr;

            if (sPlayerbotAIConfig.populationClassFavor)
            {
                uint32 chosen = 0;                          // 0 = fall back to any-class
                float bestD = 0.0f;                         // strictly-positive deficit only
                for (uint32 c = 1; c < POPDYN_CLASS_SLOTS; ++c)
                {
                    if (c == 10u)
                        continue;
                    if (d[c] <= bestD)
                        continue;
                    if (!reachable(c))
                        continue;
                    bestD = d[c];
                    chosen = c;
                }

                uint32 pulledClass = 0;
                bot = PullHighestOfClass(f, chosen, floor, pool, srcLevel, pulledClass);
                if (!bot)                                   // nothing reachable at all -> done this faction
                    break;

                if (pulledClass < POPDYN_CLASS_SLOTS)
                    d[pulledClass] -= 1.0f;                 // decrement so the batch spreads
            }
            else
            {
                bot = PickAnySource(f, 79u, pool);          // legacy: uniform any-class from level 79 only
                if (!bot)
                    break;
                srcLevel = 79;
            }

            sRandomPlayerbotMgr.IncreaseLevel(bot, 80);     // direct jump to 80, re-gears at 80
            if (srcLevel < minSrc)
                minSrc = srcLevel;
            ++issuedPerFaction[f];
            --toMove;
        }
    }

    uint32 total = issuedPerFaction[0] + issuedPerFaction[1];
    LOG_INFO("playerbots", "PopDyn sink: promoted={} (A={} H={}) count80={} minSrc={}",
             total, issuedPerFaction[0], issuedPerFaction[1],
             census.count[0][80] + census.count[1][80], uint32(minSrc));
    return total;
}

uint32 PopulationDynamicsMgr::PruneTop(std::array<uint32, 81> const& targets, Census const& census,
                                       SafeBotPool& pool)
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
        for (uint32 c = 1; c < POPDYN_CLASS_SLOTS && surplus > 0 && removedThisFaction < budget; ++c)
            for (Player* bot : pool.bots[f][80][c])
            {
                if (removedThisFaction >= budget || surplus == 0)
                    break;

                // [GuildAscensor] real-guild bots are pool-sticky: never bench/recycle the user's guildmates.
                if (bot->GetGuildId() && PlayerbotGuildMgr::instance().IsRealGuild(bot->GetGuildId()))
                    continue;

                sRandomPlayerbotMgr.Remove(bot);         // immediate delete + logout (recycles the slot)
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

    if (conveyorTick || sinkTick)
    {
        uint8 cap = ComputeCap(_frontier);
        _cap = cap;                                        // keep the cached cap fresh for the DK-login gate

        Census census;
        TakeCensus(census);

        std::array<uint32, 81> targets{};
        uint32 P = 0;
        ComputeTargets(cap, census, targets, P);           // P uses census.count[80] (spec §3)
        _lastTargets = targets;                             // cache for RandomPlayerbotMgr::AddRandomBots (GetTargets)

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

        float deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS] = {};
        if (sPlayerbotAIConfig.populationClassFavor)
            ComputeClassDeficit(cap, census, targets, deficit);

        SafeBotPool pool;
        CollectSafeBots(pool);

        // DK is the canary: log its per-band census each tick (greppable: "PopDyn classfavor").
        LOG_INFO("playerbots", "PopDyn classfavor: favor={} dkBand=[b5(A{} H{}) b6(A{} H{}) b7(A{} H{}) b8(A{} H{})]",
                 sPlayerbotAIConfig.populationClassFavor ? 1 : 0,
                 census.clsCount[0][5][CLASS_DEATH_KNIGHT], census.clsCount[1][5][CLASS_DEATH_KNIGHT],
                 census.clsCount[0][6][CLASS_DEATH_KNIGHT], census.clsCount[1][6][CLASS_DEATH_KNIGHT],
                 census.clsCount[0][7][CLASS_DEATH_KNIGHT], census.clsCount[1][7][CLASS_DEATH_KNIGHT],
                 census.clsCount[0][8][CLASS_DEATH_KNIGHT], census.clsCount[1][8][CLASS_DEATH_KNIGHT]);

        // Level-80 per-class census per faction — the direct readout of endgame class repartition.
        // Order: warrior, paladin, hunter, rogue, priest, deathknight, shaman, mage, warlock, druid.
        LOG_INFO("playerbots",
                 "PopDyn L80dist: A=[{},{},{},{},{},{},{},{},{},{}] H=[{},{},{},{},{},{},{},{},{},{}]",
                 census.clsCount[0][8][1], census.clsCount[0][8][2], census.clsCount[0][8][3],
                 census.clsCount[0][8][4], census.clsCount[0][8][5], census.clsCount[0][8][6],
                 census.clsCount[0][8][7], census.clsCount[0][8][8], census.clsCount[0][8][9],
                 census.clsCount[0][8][11],
                 census.clsCount[1][8][1], census.clsCount[1][8][2], census.clsCount[1][8][3],
                 census.clsCount[1][8][4], census.clsCount[1][8][5], census.clsCount[1][8][6],
                 census.clsCount[1][8][7], census.clsCount[1][8][8], census.clsCount[1][8][9],
                 census.clsCount[1][8][11]);

        if (conveyorTick)
        {
            // Flush the PREVIOUS plan's remainder at full period before rebuilding. The self-paced
            // ramp's `elapsed` maxes out at periodMs-diff before this reset, so `due` never reaches
            // planTotal — without this flush the last few promotions of every plan are dropped on
            // clear(), and a tiny plan (due rounds to 0) would starve permanently. Forcing
            // elapsed=periodMs drains the tail; then we log the plan's completion summary.
            if (_planTotal > 0)
            {
                _tickTimerMs = std::max<uint32>(sPlayerbotAIConfig.populationPeriod * 1000u, 1u);  // elapsed=periodMs -> due=_planTotal -> drain all remaining
                DripDrain();
                LOG_INFO("playerbots", "PopDyn conveyor: promoted={} skipped={} of {}",
                         _planPromoted, _planSkipped, _planTotal);
            }
            _tickTimerMs = 0;                              // new period's drip clock starts at 0 (no inline burst)
            BuildPlan(targets, census, deficit, pool);     // queues the class-favored picks; logs "PopDyn plan"
        }
        if (sinkTick)
        {
            _sinkTimerMs = 0;
            SinkGate(targets, census, deficit, pool);      // logs its own "PopDyn sink" line (tiny, kept inline)
        }

        uint32 pruned = PruneTop(targets, census, pool);
        LOG_INFO("playerbots", "PopDyn prune: removed={} (target80={})", pruned, targets[80]);
    }

    DripDrain();                                            // every tick: self-paced conveyor promotions
}
