/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * PopulationDynamicsMgr — server population dynamics controller. A monotonic, persisted, cached
 * real-player max-level frontier drives bot count + an up-only level conveyor. Mirrors
 * RaidSimulationMgr's frontier pattern. See
 * docs/superpowers/specs/2026-05-31-server-population-dynamics-design.md.
 */

#ifndef _PLAYERBOT_POPULATIONDYNAMICSMGR_H
#define _PLAYERBOT_POPULATIONDYNAMICSMGR_H

#include "Common.h"
#include "ObjectGuid.h"   // PendingPromotion stores a bot GUID resolved at drain time
#include <array>
#include <mutex>
#include <vector>

class Player;

// Class ids run 1..11 (10 is unused in WotLK); index arrays by class id directly. 9 census
// bands: 0..7 = the decades (BandOf), 8 = level 80 (the sink).
inline constexpr uint32 POPDYN_CLASS_SLOTS = 12;   // valid class ids < 12
inline constexpr uint32 POPDYN_BANDS       = 9;    // 0..7 decades + 8 = level 80

// Safe bots binned [faction][level][class] so the conveyor can draw a chosen class at L-1.
struct SafeBotPool {
    std::vector<Player*> bots[2][81][POPDYN_CLASS_SLOTS];
    void clear() {
        for (auto& f : bots) for (auto& l : f) for (auto& c : l) c.clear();
    }
};

// Server population dynamics controller. Mirrors RaidSimulationMgr's frontier pattern:
// a monotonic, persisted, cached real-player max-level value drives bot count + an up-only
// level conveyor. See docs/superpowers/specs/2026-05-31-server-population-dynamics-design.md.
class PopulationDynamicsMgr
{
public:
    static PopulationDynamicsMgr& instance()
    {
        static PopulationDynamicsMgr instance;
        return instance;
    }

    void LoadFromDB();                       // world startup: cache the frontier row
    void Update(uint32 diff);                // ticked from PlayerbotsWorldScript::OnUpdate
    void ConsiderPlayerLevel(Player* player);// monotonic frontier feed from a REAL player (caller bot-filters)

    // faction index: 0 = Alliance, 1 = Horde. Per-LEVEL census: count[f][level], level 1..80 (index 0 unused).
    // Declared here (before ComputeTargets) so the public signature below can name it.
    struct Census {
        std::array<uint32, 81> count[2] = {};                       // [faction][level] (unchanged)
        uint32 clsCount[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS] = {};   // [faction][band][class]
        uint32 total = 0;
    };

    // --- pure math helpers (no DB, no locks); public for clarity/log-verification ---
    static uint32 BandOf(uint8 level);                     // level 1..79 -> band index 0..7 (80 = sink, handled separately)
    static uint32 CensusBand(uint8 level);                 // 1..79 -> BandOf; 80 -> 8 (sink band)
    static uint32 EligibleClassCount(uint8 level);         // playable classes allowed at level (10, or 9 below 55)
    uint8  ComputeCap(uint8 frontier) const;               // C = min(80, max(MinCap, F+Headroom)) — UNCHANGED
    // Per-level targets target[1..80] (index 0 unused). Level in band b -> populationBracket[b];
    // level 80 -> remainder (MaxPopulation - sum of bands); any level > cap -> 0. Returns spawn target P.
    void   ComputeTargets(uint8 cap, Census const& census, std::array<uint32, 81>& targets, uint32& outP) const;
    uint8  CurrentCap() const { return _cap; }             // cached cap for DK-login gating (world-thread read)
    // Last per-level targets computed by Update()/LoadFromDB() (both-faction total per level; index 0
    // unused, target[80] = sink). Read by RandomPlayerbotMgr::AddRandomBots for the deterministic
    // per-(level, faction) login fill. Unlocked read mirrors CurrentCap()'s cached-value pattern.
    std::array<uint32, 81> const& GetTargets() const { return _lastTargets; }

private:
    PopulationDynamicsMgr() = default;

    bool   IsSafeBot(Player* bot) const;       // strict gatekeeper — always excludes dungeon maps; promotion sites use IsPromotable
    bool   IsPromotable(Player* bot) const;    // like IsSafeBot but IsDungeon gate is config-gated (PromoteInInstances)
    void   TakeCensus(Census& out) const;      // iterate live bots, bin by (faction, level)
    // fairShare - census, clamped >=0, per [faction][band][class]; pure (no DB/locks).
    void ComputeClassDeficit(uint8 cap, Census const& census,
                             std::array<uint32, 81> const& targets,
                             float deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS]) const;

    // Collect SAFE bots per LEVEL per CLASS per faction (pointers); no sort (the conveyor picks randomly).
    void CollectSafeBots(SafeBotPool& pool) const;
    // A queued promotion: a bot to level +1, resolved lazily at drain time. expectLevel is the
    // source level (L-1) at plan time — a staleness guard so a logged-out/already-moved bot is skipped.
    struct PendingPromotion { ObjectGuid guid; uint8 expectLevel; };

    // Build the conveyor plan once per Period: the same top-down, class-favored deficit fill the old
    // DriftUp did (PickFavoredSource/PickAnySource over the pool) — but push each picked bot onto
    // _pending instead of promoting it inline. Per-faction MaxPromotionsPerCycle budget. Logs "PopDyn plan".
    void BuildPlan(std::array<uint32, 81> const& targets, Census const& census,
                   float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool);
    // Drain queued promotions self-paced across world ticks (due = _planTotal * elapsed / Period).
    // Re-validates each bot (FindPlayer + IsPromotable + expectLevel) before IncreaseLevel.
    void DripDrain();
    // The slow sink gate: only path into level 80 (random 79->80, SinkBatch/faction). Returns promotions issued.
    uint32 SinkGate(std::array<uint32, 81> const& targets, Census const& census,
                    float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool);
    // Dormant insurance: sheds level-80 surplus if target[80] later drops below count[80] (per-faction budget).
    uint32 PruneTop(std::array<uint32, 81> const& targets, Census const& census, SafeBotPool& pool);  // returns bots removed
    // Draw a source bot from level `srcLevel`, faction `f`: weighted-random by class deficit in
    // `band` (favored), or uniform over all classes (legacy). Removes+returns it; nullptr if none.
    Player* PickFavoredSource(uint32 f, uint32 srcLevel, uint32 band,
                              float const deficit[2][POPDYN_BANDS][POPDYN_CLASS_SLOTS], SafeBotPool& pool);
    Player* PickAnySource(uint32 f, uint32 srcLevel, SafeBotPool& pool);

    void PersistFrontier();                  // UPDATE playerbots_population_state ... WHERE id=1 (caller holds _mutex)

    std::mutex _mutex;
    uint8  _frontier = 0;                    // cached monotonic real-player max level (1..80)
    uint8  _cap = 0;                          // cached level cap = ComputeCap(_frontier); read by the DK-spawn gate
    std::array<uint32, 81> _lastTargets{};    // cache of the last ComputeTargets() output (world-thread read via GetTargets)
    uint32 _tickTimerMs = 0;                 // accumulator for Period (conveyor) cadence
    uint32 _sinkTimerMs = 0;                 // accumulator for SinkPeriod (level-80 sink-gate) cadence
    // Conveyor drip state (BuildPlan fills once per Period; DripDrain consumes across ticks).
    // _tickTimerMs (above) doubles as the elapsed-in-period clock that paces the drain.
    std::vector<PendingPromotion> _pending;  // promotions queued by BuildPlan, drained over the period
    uint32 _planTotal    = 0;                // size of the current plan
    uint32 _planDrained  = 0;               // entries processed this period (promoted + skipped) — paces the drip
    uint32 _planPromoted = 0;               // promoted this period (summary line)
    uint32 _planSkipped  = 0;               // skipped (stale/unsafe) this period (summary line)
};

#define sPopulationDynamicsMgr PopulationDynamicsMgr::instance()

#endif
