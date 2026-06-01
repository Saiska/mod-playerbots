# Server Population Dynamics — As-Built Spec

**Branch:** `feat/server-population-dynamics`
**Module:** `mod-playerbots` (Saiska fork)
**Date:** 2026-06-01
**Design doc:** `asp-server-config/docs/superpowers/specs/2026-05-31-server-population-dynamics-design.md`
**Plan:** `asp-server-config/docs/superpowers/plans/2026-06-01-server-population-dynamics.md`
**Charter:** `asp-server-config/docs/charters/server-population-dynamics.md`

---

## Overview

The realm grows with its real players. A monotonic, persisted real-player max-level **frontier** drives two things: (1) the total bot **count** target fed to the native random-bot engine, and (2) an up-only level **conveyor** that cycles bots from the bottom of the level range up toward the frontier. New bots always spawn at the bottom decade; a drift pass promotes bots +1 level when higher brackets need population; a prune pass recycles surplus level-80 bots. No bot is ever down-levelled. The system subsumes and replaces `mod-player-bot-level-brackets` (removed by the master at integration).

---

## Schema

```sql
CREATE TABLE IF NOT EXISTS playerbots_population_state (
  id               TINYINT NOT NULL PRIMARY KEY DEFAULT 1,
  max_player_level TINYINT UNSIGNED NOT NULL DEFAULT 0
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

INSERT IGNORE INTO playerbots_population_state (id, max_player_level) VALUES (1, 0);
```

**File:** `data/sql/characters/updates/2026_06_01_00_population_state.sql`
Auto-applied by AC dbupdater at worldserver boot. The row stores the single monotonic frontier value. Decoupled from raid-sim's `playerbots_raid_server_state` by design (no cross-feature schema dependency).

---

## Frontier

`PopulationDynamicsMgr` (singleton, `sPopulationDynamicsMgr`) caches the frontier as `_frontier` (uint8).

**Load:** `LoadFromDB()` — called once at world init (inside `PlayerbotsWorldScript::OnWorldInit`). Reads `max_player_level` from the state row into `_frontier`. If the row is absent (first boot), logs a warning and defaults to 0.

**Feed:** `ConsiderPlayerLevel(Player*)` — caller is responsible for bot-filtering before calling. Wired at two call sites in `Playerbots.cpp`, both gated by `!player->GetSession()->IsBot()`:
- `OnPlayerLogin` — fires when a real player logs in.
- `OnPlayerLevelChanged` — fires when a real player levels up.

Bots are excluded using `GetSession()->IsBot()` (not `GET_PLAYERBOT_AI()`) because PlayerbotAI is not yet attached to autologin bots at `OnPlayerLogin`.

**Monotonic guarantee:** `ConsiderPlayerLevel` only updates `_frontier` when `lvl > _frontier`. The new value is immediately written to the DB via `PersistFrontier()` (`UPDATE playerbots_population_state SET max_player_level = {} WHERE id = 1`).

**Disabled:** when `PopulationDynamics.Enable = 0`, `ConsiderPlayerLevel` returns early (frontier never advances; no DB writes).

---

## Bracket Ladder

Nine fixed brackets, computed by static helpers:

| Bracket (b) | `BracketLower(b)` | `BracketUpper(b)` | Notes |
|---|---|---|---|
| 0 | 1 | 9 | Bottom inflow target for non-DKs |
| 1 | 10 | 19 | |
| 2 | 20 | 29 | |
| 3 | 30 | 39 | |
| 4 | 40 | 49 | |
| 5 | 50 | 59 | Lowest bracket DKs can enter (spawn at 55) |
| 6 | 60 | 69 | |
| 7 | 70 | 79 | |
| 8 | 80 | 80 | Top bracket; prune target |

`BracketOf(level)`: `>= 80` → 8; `< 10` → 0; otherwise `level / 10`.

DKs spawn at level 55 (bracket 5) and only occupy brackets 5–8.

---

## Math

### Cap

```
C = min(80, max(MinCap, frontier + Headroom))
```

### Openness

For bracket `b` with `[lo, hi]` and cap `C`:

```
openness = 1.0          if C >= hi
openness = 0.0          if C < lo
openness = (C - lo + 1) / (hi - lo + 1)   otherwise
```

### Targets

```
targets[b] = round(MaxPopulation * (BracketPct[b] / 100.0) * Openness(b, C))
P = sum(targets[0..8])
```

Brackets above the cap have `Openness = 0`, so their targets are 0. This bounds the conveyor at the frontier.

---

## Config Keys and Defaults

| Key | Default | Notes |
|---|---|---|
| `PopulationDynamics.Enable` | `1` (true) | Master on/off switch |
| `PopulationDynamics.MaxPopulation` | `2000` | Total bot count ceiling |
| `PopulationDynamics.Headroom` | `10` | Levels above frontier the cap extends |
| `PopulationDynamics.MinCap` | `20` | Floor for the cap regardless of frontier |
| `PopulationDynamics.Period` | `300` | Tick interval in seconds |
| `PopulationDynamics.DriftRate` | `0.02` | Fraction of per-faction deficit to promote each cycle |
| `PopulationDynamics.MaxPromotionsPerCycle` | `10` | Hard ceiling on drift promotions AND prune removals per cycle |
| `PopulationDynamics.Bracket1.Pct` | `6` | b0: levels 1–9 |
| `PopulationDynamics.Bracket2.Pct` | `6` | b1: 10–19 |
| `PopulationDynamics.Bracket3.Pct` | `6` | b2: 20–29 |
| `PopulationDynamics.Bracket4.Pct` | `6` | b3: 30–39 |
| `PopulationDynamics.Bracket5.Pct` | `6` | b4: 40–49 |
| `PopulationDynamics.Bracket6.Pct` | `6` | b5: 50–59 |
| `PopulationDynamics.Bracket7.Pct` | `6` | b6: 60–69 |
| `PopulationDynamics.Bracket8.Pct` | `8` | b7: 70–79 |
| `PopulationDynamics.Bracket9.Pct` | `50` | b8: level 80 |

Config keys are 1-indexed (`Bracket1.Pct` maps to `b=0`, …, `Bracket9.Pct` maps to `b=8`).

**Profile normalization:** at load, if the sum of the 9 `BracketPct` values does not equal 100, the code adds or subtracts `+1`/`-1` round-robin across non-zero brackets until the sum reaches 100. The normalized sum is logged at boot.

---

## Three Reconcile Flows (each `Period` seconds)

### 1. Bottom Inflow — `SetPopulationTarget(P)`

`P` (computed from `ComputeTargets`) is passed to `RandomPlayerbotMgr::SetPopulationTarget(P)`, which clamps `P` to `[MinRandomBots, MaxRandomBots]` and writes it as the `bot_count` event with a TTL of `Period + 60` seconds (outlives one controller cycle). The native random-bot engine respects this event to decide how many bots to maintain online.

New bots that spawn (via `RandomizeFirst`) enter at a random level in bracket 0 (levels 1–9). DKs enter at exactly level 55. This is an early-return path: when `populationDynamicsEnable` is true, `RandomizeFirst` skips the weighted level roll and the `disableRandomLevels` override and goes directly to `urand(lo, hi)`.

### 2. Up-Only Drift — `DriftUp`

**Goal:** move bots upward when higher brackets are under-populated.

**Algorithm:**
1. Compute per-faction deficit: for each `(faction f, bracket b)`, `deficit += max(0, targets[b]/2 - census.count[f][b])`.
2. `budget = clamp(round(DriftRate * deficit), 0, MaxPromotionsPerCycle)`.
3. Walk brackets low→high (`b = 0..7`), per faction. For each bracket `b`, check if any bracket `b+1..8` has `count[f][hb] < targets[hb]/2`. If so, promote the highest-level safe bots in bracket `b` by calling `IncreaseLevel(bot)` (+1 level, `PlayerbotFactory.Randomize(true)` re-gear) until `budget` is exhausted.

A bot's level is never lowered. `budget = 0` when the population is already well-distributed.

### 3. Top-Prune — `PruneTop`

**Goal:** recycle surplus level-80 bots when bracket 8 is over-target.

**Algorithm:** per faction, if `census.count[f][8] > targets[8]/2`, remove up to `MaxPromotionsPerCycle` safe level-80 bots via `sRandomPlayerbotMgr.Remove(bot)` (immediate delete + slot recycle). The same `MaxPromotionsPerCycle` constant is reused as the ceiling.

**Thrash prevention:** drift and prune operate on the same census snapshot taken before either runs. Drift only promotes bots in brackets 0–7 when higher brackets are in deficit; prune only removes bots in bracket 8 when that bracket is in surplus. The conditions are mutually exclusive on any given census, so bracket 8 cannot be simultaneously promoted-into and pruned in one tick.

---

## `IsSafeBot` Predicate

A bot is eligible for drift promotion or top-prune only if all of the following hold:

- Session exists and is not logging out; not during `RemoveFromWorld`.
- `IsInWorld() && IsAlive()`.
- Not in combat.
- Not in a battleground, arena, random LFG dungeon, or battleground queue.
- Not in flight.
- Not on a dungeon/instance map (catches raid-sim parked bots).
- Not tracked as actively raiding by `sRaidSimulationMgr.IsRaiding(guid)`.
- Not in a group that contains at least one real (non-bot) online player.

**Deliberate simplification:** guild adjacency (whether a bot shares a guild with a real player) was part of the removed `mod-player-bot-level-brackets` module's original design. The implementation reduces this to GROUP adjacency only (the load-bearing real-player guard). A guild check can be added later if live testing shows real players guilded with bots experiencing unwanted re-levelling.

---

## Wiring in `Playerbots.cpp`

| Hook | What it does |
|---|---|
| `PlayerbotsWorldScript::OnWorldInit` | Calls `sPopulationDynamicsMgr.LoadFromDB()` at world startup (alongside `sRaidSimulationMgr.LoadFromDB()`). |
| `PlayerbotsWorldScript::OnUpdate(diff)` | Calls `sPopulationDynamicsMgr.Update(diff)` every world tick. |
| `PlayerbotsScript::OnPlayerLogin` | If `!IsBot()`, calls `sPopulationDynamicsMgr.ConsiderPlayerLevel(player)`. |
| `PlayerbotsScript::OnPlayerLevelChanged` | If `!IsBot()`, calls `sPopulationDynamicsMgr.ConsiderPlayerLevel(player)`. |

---

## Verification Log Lines

All lines are logged at `LOG_INFO("playerbots", ...)` → `Playerbots.log`.

| Event | Log line pattern |
|---|---|
| Config loaded | `PopDyn: config loaded — enable=.. Pmax=.. headroom=.. minCap=.. period=..s drift=.. maxPromo=.. profile=[..] sum=100` |
| Frontier loaded | `PopDyn: loaded frontier=N.` |
| Frontier row absent | `PopDyn: playerbots_population_state row absent; frontier defaulting to 0.` |
| Frontier advanced | `PopDyn: frontier advanced to N.` |
| Tick fired | `PopDyn tick: F=.. C=.. P=.. targets=[b0,b1,b2,b3,b4,b5,b6,b7,b8]` |
| Census taken | `PopDyn census: total=.. A=[b0..b8] H=[b0..b8]` |
| Drift result | `PopDyn drift: promoted=.. (driftRate=.. cycleCap=..)` |
| Prune result | `PopDyn prune: removed=.. (target80=..)` |

---

## Master Integration Actions (not this lane)

The following steps are performed by the master after merging this branch:

1. Add `PopulationDynamics.*` conf block to `asp-server-config` and apply to live `run/configs/modules/playerbots.conf`.
2. Set `AiPlayerbot.SyncLevelWithPlayers = 0` (conveyors conflicts with native level-sync).
3. Set wide `MinRandomBots` / `MaxRandomBots` envelope so `SetPopulationTarget` has room to operate.
4. Remove `mod-player-bot-level-brackets`: delete from clone-list, remove `acore/modules/mod-player-bot-level-brackets/`, reconfigure CMake (`cmake acore/build`), remove its conf file.
5. `DROP TABLE bot_level_brackets_guild_tracker` (if exists) in `pbtest_characters`.
6. Build: `cmake --build acore/build --config Release --target worldserver --parallel`.
7. Deploy: stop worldserver, copy binary to `run/`, restart.
8. Verify via log lines above + in-game census.
9. Merge branch, push, update integration log.
