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
#include <array>
#include <mutex>
#include <vector>

class Player;

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

    // --- pure math helpers (no DB, no locks); public for clarity/log-verification ---
    static uint32 BandOf(uint8 level);                     // level 1..79 -> band index 0..7 (80 = sink, handled separately)
    uint8  ComputeCap(uint8 frontier) const;               // C = min(80, max(MinCap, F+Headroom)) — UNCHANGED
    // Per-level targets target[1..80] (index 0 unused). Level in band b -> populationBracket[b];
    // level 80 -> remainder (MaxPopulation - sum of bands); any level > cap -> 0. Returns spawn target P.
    void   ComputeTargets(uint8 cap, Census const& census, std::array<uint32, 81>& targets, uint32& outP) const;
    uint8  CurrentCap() const { return _cap; }             // cached cap for DK-login gating (world-thread read)

private:
    PopulationDynamicsMgr() = default;

    // faction index: 0 = Alliance, 1 = Horde. Per-LEVEL census: count[f][level], level 1..80 (index 0 unused).
    struct Census { std::array<uint32, 81> count[2] = {}; uint32 total = 0; };
    bool   IsSafeBot(Player* bot) const;       // alive, in world, idle, not raid-sim, no real-player adjacency
    void   TakeCensus(Census& out) const;      // iterate live bots, bin by (faction, level)

    // Collect SAFE bots per LEVEL per faction (pointers); no sort (the conveyor picks randomly).
    void CollectSafeBots(std::array<std::vector<Player*>, 81> perLevel[2]) const;
    // The per-level deficit-fill conveyor (top-down L=79..2, random source from L-1, per-faction budget).
    uint32 DriftUp(std::array<uint32, 81> const& targets, Census const& census,
                   std::array<std::vector<Player*>, 81> perLevel[2]);    // returns promotions issued
    // The slow sink gate: only path into level 80 (random 79->80, SinkBatch/faction). Returns promotions issued.
    uint32 SinkGate(std::array<uint32, 81> const& targets, Census const& census,
                    std::array<std::vector<Player*>, 81> perLevel[2]);
    // Dormant insurance: sheds level-80 surplus if target[80] later drops below count[80] (per-faction budget).
    uint32 PruneTop(std::array<uint32, 81> const& targets, Census const& census,
                    std::array<std::vector<Player*>, 81> perLevel[2]);   // returns bots removed

    void PersistFrontier();                  // UPDATE playerbots_population_state ... WHERE id=1 (caller holds _mutex)

    std::mutex _mutex;
    uint8  _frontier = 0;                    // cached monotonic real-player max level (1..80)
    uint8  _cap = 0;                          // cached level cap = ComputeCap(_frontier); read by the DK-spawn gate
    uint32 _tickTimerMs = 0;                 // accumulator for Period (conveyor) cadence
    uint32 _sinkTimerMs = 0;                 // accumulator for SinkPeriod (level-80 sink-gate) cadence
};

#define sPopulationDynamicsMgr PopulationDynamicsMgr::instance()

#endif
