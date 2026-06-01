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
    static uint8  BracketLower(uint32 b) { return uint8(b == 0 ? 1 : b * 10); }
    static uint8  BracketUpper(uint32 b) { return uint8(b < 8 ? b * 10 + 9 : 80); }
    static uint32 BracketOf(uint8 level);                  // level -> bracket index 0..8
    uint8  ComputeCap(uint8 frontier) const;               // C = min(80, max(MinCap, F+Headroom))
    float  Openness(uint32 b, uint8 cap) const;            // fraction of bracket b's span <= cap
    void   ComputeTargets(uint8 cap, std::array<uint32, 9>& targets, uint32& outP) const;
    uint8  CurrentCap() const { return _cap; }             // cached cap for DK-login gating (world-thread read)

private:
    PopulationDynamicsMgr() = default;

    // faction index: 0 = Alliance, 1 = Horde
    struct Census { std::array<uint32, 9> count[2] = {}; uint32 total = 0; };
    bool   IsSafeBot(Player* bot) const;       // alive, in world, idle, not raid-sim, no real-player adjacency
    void   TakeCensus(Census& out) const;      // iterate live bots, bin by (faction, bracket)

    // Collect SAFE bots per bracket per faction (pointers), highest-level first within each bracket.
    void CollectSafeBots(std::array<std::vector<Player*>, 9> perBracket[2]) const;
    uint32 DriftUp(std::array<uint32, 9> const& targets, Census const& census,
                   std::array<std::vector<Player*>, 9> perBracket[2]);   // returns promotions issued
    uint32 PruneTop(std::array<uint32, 9> const& targets, Census const& census,
                    std::array<std::vector<Player*>, 9> perBracket[2]);   // returns bots removed

    void PersistFrontier();                  // UPDATE playerbots_population_state ... WHERE id=1 (caller holds _mutex)

    std::mutex _mutex;
    uint8  _frontier = 0;                    // cached monotonic real-player max level (1..80)
    uint8  _cap = 0;                          // cached level cap = ComputeCap(_frontier); read by the DK-spawn gate
    uint32 _tickTimerMs = 0;                 // accumulator for Period cadence
};

#define sPopulationDynamicsMgr PopulationDynamicsMgr::instance()

#endif
