# SPEC — raidsim-orphan-reaper

**Branch:** `feat/raidsim-orphan-reaper` (cut off master tip `8e81a702`)
**Target repo:** Saiska/mod-playerbots fork (in-tree `acore/modules/mod-playerbots`). Rebuild + deploy Y: (master-only).
**Scope of this lane:** mod-playerbots source + the in-repo conf template only. Edit-only. No core edits, no new source files, no CMake reconfigure, no build/deploy.

---

## 1. Problem (root cause — code-proven, live-sized)

RaidSim forms real, DB-persisted groups (`Group::Create` + `sGroupMgr->AddGroup` write `groups`/`group_member`). Teardown relies entirely on in-memory state (`_runs`, `_raiding`):

1. **Restart creates orphans.** On worldserver shutdown mid-run, `_runs`/`_raiding` are lost. The persisted groups reload on next boot with no active run backing them and no teardown ever scheduled.
2. **They never self-heal — a grouping-starvation deadlock.** The only orphan-disband is `RandomPlayerbotMgr::ProcessBot(Player*)` at `RandomPlayerbotMgr.cpp:1573-1578` ("remove from group since leader is random bot"). That overload is reached only via `RandomBotUpdateAction`, whose `isUseful()` arms the "random bot update" AI flag **only behind the group-guard at `RandomPlayerbotMgr.cpp:1477`** (`if (player->GetGroup() || player->HasUnitState(UNIT_STATE_IN_FLIGHT)) return false;`). A grouped bot is therefore excluded from the very update cycle that arms the flag that fires the action that would disband it. Closed loop: grouped → never scheduled → never disbanded → stays grouped, regardless of whether the leader is online.

**Live evidence (2026-06-21, `pbtest_characters`):** 141 persisted groups / 652 grouped bots; 129 groups (~592 bots) have all members on continent maps (0/1/530/571) = orphans, vs only 12 groups / 60 bots on instance maps (the ~11 legit in-flight runs). Size signature: 97×5-member, 36×4, 7×3, 1×2.

---

## 2. Goal

Give RaidSim a **leader-independent orphan reaper** that disbands any all-random-bot group not backed by a live `_run`, running **entirely outside** the starved per-bot update loop (sidesteps the deadlock). It clears the backlog and keeps it clear across restarts — with **zero measurable world-tick cost** (budgeted drip, never a storm). No change to legitimate in-flight runs, real-player groups, or any chat/gear behaviour.

---

## 3. Performance contract (FIRST-CLASS — "absolutely painless for world lag")

The reaper runs on the world thread (`RaidSimulationMgr::Update`). The hazard is the **boot pass**: disbanding ~129 groups at once is a guaranteed spike. Mandatory mitigations (mirror the shipped `gearfloor-tick-throttle` drain and PopDyn `DripDrain`):

- **Budgeted drip.** Disband at most `RaidSim.ReaperBatch` groups per reaper tick (default 3), draining over many ticks. A 129-orphan boot backlog must drain with **no correlated Perf.log world-tick spike**.
- **Bounded cadence.** Reaper work happens on a `RaidSim.ReaperInterval` timer (default 120 s), not every raw tick. The per-tick disband budget caps cost even during the drain.
- **Cheap predicate.** Per-group check uses only in-memory lookups (CharacterCache account id + random-account-set membership); **no per-member DB query**.
- **No lock held across `Group::Disband`.** The reaper takes `_mutex` only to advance its own timer and snapshot the active-run group-GUID set; it **releases the lock before** scanning groups and disbanding. `Group::Disband` does not re-enter RaidSim, so this is safe.

Acceptance is gated on a live Y: `Perf.log` proof that the boot drain produces no correlated tick spike (master task).

---

## 4. Confirmed core/fork APIs (verified against the real tip — NOT the charter's assumptions)

| Need | Confirmed signature / fact | Source |
|---|---|---|
| Iterate all groups | **`GroupMgr::GroupStore` is `protected` and there is NO public `GetGroupStore()`** (charter assumption WRONG). `Group* GetGroupByGUID(ObjectGuid::LowType guid) const;` is public. | `acore/src/server/game/Groups/GroupMgr.h:32-48` |
| A bot's group | `Group* Player::GetGroup()` (existing, used everywhere) | core |
| Group GUID | `ObjectGuid Group::GetGUID() const;` | `Group.h:221` |
| Group kind predicates | `bool isLFGGroup(bool restricted=false)`, `bool isBGGroup()`, `bool isBFGroup()`, `bool isRaidGroup()` | `Group.h:213-216` |
| Group members (offline-safe) | `MemberSlotList const& GetMemberSlots() const` → `struct MemberSlot { ObjectGuid guid; std::string name; ... }`; `MemberSlotList = std::list<MemberSlot>` | `Group.h:171-179, 242` |
| Member count | `uint32 GetMembersCount() const` | `Group.h:245` |
| Account id for a guid (offline-safe) | `uint32 sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid guid) const;` | `acore/.../Cache/CharacterCache.h:75` |
| Random-bot account test | `bool PlayerbotAIConfig::IsInRandomAccountList(uint32 id);` (impl `PlayerbotAIConfig.cpp:1047`) | `PlayerbotAIConfig.h:212` |
| Disband a group | `void Group::Disband(bool hideDestroy=false)` (used today at formation `:277` and teardown `:368`) | `Group.h` / `RaidSimulationMgr.cpp` |
| Online-bot enumeration | `ObjectAccessor::GetPlayers()` (already used by `Update()` scheduler at `RaidSimulationMgr.cpp:899`) | core |

`#include "GroupMgr.h"` and `#include "Group.h"` are **already present** in `RaidSimulationMgr.cpp:11-12`. `CharacterCache` must be added (`#include "CharacterCache.h"`).

---

## 5. Design — how groups are enumerated (DEVIATION from charter, justified)

The charter assumed `GroupMgr::GetGroupStore()` exists for an all-groups sweep. **It does not** — `GroupStore` is `protected` with no public accessor, and this lane may not edit the core repo. The reaper therefore enumerates **candidate groups via online playerbots**, which is mod-playerbots-only and matches the perf contract:

1. Snapshot active-run group GUIDs under `_mutex`, then release.
2. Iterate `ObjectAccessor::GetPlayers()`; for each in-world bot with a `PlayerbotAI`, take `Player::GetGroup()`.
3. Dedupe candidate groups by `Group::GetGUID()` into a local set (so each group is examined once).
4. Apply the orphan predicate (§6) to each distinct candidate group; collect orphans.
5. Disband up to `ReaperBatch` of them this tick (drip; the rest next tick).

**Why this is sufficient on this realm:** bots autologin (`RandomBotAutologin`) and remain in-world parked/wandering — every orphan group's members are in-world (the live evidence shows 592 grouped bots present on continent maps). A group only causes the starvation harm when its members are *in-world* (the starved update cycle is for in-world bots), so reaping exactly the in-world-backed groups fixes exactly the harmful set. An all-offline orphan group (members logged out) causes no in-world starvation and will be reaped the moment any member logs back in. This is documented as the one accepted limitation vs. a full GroupStore sweep, and it requires no core edit.

> Implementation note for a future lane: if reaping fully-offline orphan groups is ever needed, the right move is a separate charter adding a public `GroupMgr::GetGroupStore()` core accessor — **out of scope here.**

---

## 6. Orphan predicate

Disband group `g` **iff ALL** hold:

1. **Normal party/raid** — `!g->isLFGGroup() && !g->isBGGroup() && !g->isBFGroup()`. (LFG/BG/BF groups are dungeon-finder/battleground machinery, never RaidSim's.)
2. **All-random-bot** — for **every** `MemberSlot ms` in `g->GetMemberSlots()`: `sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(ms.guid))` is true. The first member whose account is NOT in the random list → the group is **never** touched. (Works for offline members too, unlike `IsRandomBot(LowType)` which needs `currentBots`.) An empty member list (degenerate) is treated as NOT all-bot → skip.
3. **Not a live run** — `g->GetGUID()` is NOT in the snapshotted active-run group-GUID set (§5 step 1).

All three are pure in-memory lookups; no DB query per member.

---

## 7. `ActiveRun.groupGuid` — record the live group

Add `ObjectGuid groupGuid;` to `struct ActiveRun` (`RaidSimulationMgr.h:69-82`).

- **Set it** in `RaidSimFormationOperation::Execute` (`RaidSimulationMgr.cpp:248`) **right after** the successful `group->Create(leader)` + `sGroupMgr->AddGroup(group)` (`:280-286`), by writing the new group's GUID back into the run. Because the formation op runs asynchronously on the world-thread processor (queued from `LaunchRun`), the op must reach the run via the manager: give `RaidSimulationMgr` a small setter `void SetRunGroupGuid(uint32 guildId, ObjectGuid groupGuid);` (locks `_mutex`, finds `_runs[guildId]` if present, sets `groupGuid`) and pass `guildId` into the formation op so it can call `sRaidSimulationMgr.SetRunGroupGuid(m_guildId, group->GetGUID())`. The op already has the leader; thread `guildId` through its ctor.
  - Predicate rule 3 (§6) reads `groupGuid` from each `_runs` entry to build the active-run GUID set.
- **Use it to harden teardown** (§8).

> Why the async setter is required: `groupGuid` cannot be set synchronously in `LaunchRun` because the `Group` does not exist until the formation op runs on the world-thread processor. Keep `guildId` (not a raw `ActiveRun*`) across the async boundary to avoid a dangling pointer.

The active-run GUID set for rule 3 = `{ run.groupGuid : run in _runs, run.groupGuid not empty }`. A run whose formation op has not yet executed has an empty `groupGuid` and thus no group to confuse the reaper with — excluding empty is correct.

---

## 8. Teardown hardening (disband-by-GUID)

In `RaidSimTeardownOperation::Execute` (`RaidSimulationMgr.cpp:349-373`), the current disband is:

```cpp
if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
    if (Group* g = leader->GetGroup())
        g->Disband(true);
```

This silently no-ops if the leader is momentarily unresolved (logged out / mid-teleport). Harden it to disband by recorded group GUID, falling back to the leader path:

```cpp
Group* g = nullptr;
if (m_groupGuid)
    g = sGroupMgr->GetGroupByGUID(m_groupGuid.GetCounter());
if (!g)
    if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
        g = leader->GetGroup();
if (g)
    g->Disband(true);
```

`GetGroupByGUID` takes `ObjectGuid::LowType` → pass `m_groupGuid.GetCounter()`. Thread the recorded `run.groupGuid` into `RaidSimTeardownOperation` (new ctor param `ObjectGuid groupGuid`); `EndRun` (`:684-688`) builds the op and already has the full `ActiveRun run`, so it passes `run.groupGuid`. Null-guard everything (`GetGroupByGUID` can return null; `FindPlayer` can return null).

---

## 9. Reaper wiring in `Update()`

`RaidSimulationMgr::Update(uint32 diff)` (`:856-894`) **holds `_mutex` across its whole body** (`std::lock_guard` at `:861`) and has early `return`s (`:893`). The reaper must NOT call `Group::Disband` under that lock, and must run regardless of the scheduler's early returns.

Chosen structure — **call `ReconcileOrphans(diff)` at the very top of `Update()`**, after the `raidSimEnable` check (`:858-859`) and before the existing `lock_guard` (`:861`). This keeps the reaper's timer and lock fully independent of the scheduler, leaves the existing scheduler body byte-unchanged, and is unaffected by the scheduler's early returns.

`ReconcileOrphans(uint32 diff)`:

1. If `!sPlayerbotAIConfig.raidSimOrphanReaper` return.
2. **Lock scope (short):** take `std::lock_guard<std::mutex> lock(_mutex);` in an inner block:
   - `_reaperTimerMs += diff;`
   - `if (_reaperTimerMs < sPlayerbotAIConfig.raidSimReaperInterval * 1000u) return;` (returns out of the function — the lock releases on the way out; fine).
   - `_reaperTimerMs = 0;`
   - Build `std::unordered_set<ObjectGuid> activeGroupGuids;` from `_runs`: insert each `run.groupGuid` that is not empty.
   - End the inner block → **lock released.**
3. **Lock-free scan:** build a candidate-group set from online bots — iterate `ObjectAccessor::GetPlayers()`; for each `Player* bot` that `IsInWorld()` and has `GET_PLAYERBOT_AI(bot)`, take `Group* grp = bot->GetGroup()`; if `grp` and its `GetGUID()` not already seen (dedupe via a local `std::unordered_set<ObjectGuid>`), evaluate the predicate (§6) with `activeGroupGuids` for rule 3. Collect orphan `Group*` into a vector; **stop collecting once it reaches `ReaperBatch`** (bounds the work).
4. **Disband (lock-free):** `uint32 disbanded = 0; for (Group* g : orphans) { g->Disband(true); ++disbanded; }` (`orphans.size() <= ReaperBatch`).
5. **Log** (§11) only when `disbanded > 0`.

Add a private field `uint32 _reaperTimerMs = 0;` next to `_schedTimerMs` (`RaidSimulationMgr.h:117`), and a private decl `void ReconcileOrphans(uint32 diff);` in the private-helpers area (near `:84-98`). The reaper's interval is fully independent of the 60 s scheduler.

> Lock-discipline note for the reviewer: the only lock the reaper holds is the inner scope in step 2, which contains zero `Group`/`Player` mutation. Steps 3-4 (scan + `Disband`) run with no lock held. `_runs` is read only inside the locked snapshot. This satisfies the charter's "no lock held across disband."

---

## 10. Config keys

Three new keys, read in `PlayerbotAIConfig::Initialize()` next to the existing `raidSim*` block (`PlayerbotAIConfig.cpp:442-457`), with members declared next to the `raidSim*` block in `PlayerbotAIConfig.h` (`:686-702`):

| Key | Type | Default | Member |
|---|---|---|---|
| `RaidSim.OrphanReaper` | bool | `true` | `raidSimOrphanReaper` |
| `RaidSim.ReaperInterval` | int32 (seconds) | `120` | `raidSimReaperInterval` |
| `RaidSim.ReaperBatch` | int32 (groups/tick) | `3` | `raidSimReaperBatch` |

Read with: `sConfigMgr->GetOption<bool>("RaidSim.OrphanReaper", true)`, `GetOption<int32>("RaidSim.ReaperInterval", 120)`, `GetOption<int32>("RaidSim.ReaperBatch", 3)` (match the existing `int32` style at `:443-452`). Member types: `bool raidSimOrphanReaper; uint32 raidSimReaperInterval; uint32 raidSimReaperBatch;`.

**Conf template** (`conf/playerbots.conf.dist`): the `.dist` template currently has **no RaidSim block at all** (the live `RaidSim.*` keys exist only in the deployed `run/configs/modules/playerbots.conf`). The lane adds a small, self-contained RaidSim-reaper block to the `.dist` so the keys are documented for fresh installs. **AC parser breaks on trailing `#` inline comments** → every comment goes on its own line. Block to add (at the end of the file, or adjacent to other RaidSim/Population sections):

```
###################################
#    RAIDSIM ORPHAN REAPER
###################################
#
#    RaidSim.OrphanReaper
#        Leader-independent reaper that disbands all-random-bot groups left
#        over from a restart (orphans not backed by a live run).
#        Default: 1 (enabled)

RaidSim.OrphanReaper = 1

#    RaidSim.ReaperInterval
#        Seconds between reaper passes.
#        Default: 120

RaidSim.ReaperInterval = 120

#    RaidSim.ReaperBatch
#        Max orphan groups disbanded per reaper pass (drip; keeps the boot
#        drain off the world-tick spike).
#        Default: 3

RaidSim.ReaperBatch = 3
```

> **Master-only deploy note (NOT this lane):** the live `run/configs/modules/playerbots.conf` (and the Y: copy `C:/server/w/configs/modules/playerbots.conf`) should get the same 3 keys appended to their RaidSim section. The coded defaults equal these values, so a missing key is safe (falls back to default + logs `Missing property`), but the keys should be present for tuning. Recorded as the final PLAN task.

---

## 11. Log line (boot-grep verify gate)

In `ReconcileOrphans`, after disbanding, emit (only when `disbanded > 0`):

```cpp
LOG_INFO("playerbots", "RaidSim: reaper disbanded {} orphan group(s) ({} remaining)",
         disbanded, remaining);
```

- Use fmt `{}` placeholders, NOT `%d`/`%u` (the maintenance-gear-floor lesson).
- `disbanded` = groups disbanded this pass; `remaining` = orphan candidates found this pass beyond the batch (i.e. backlog still to drain — computed as the count of orphans discovered minus `disbanded`; since scanning stops at `ReaperBatch`, track whether more were found by continuing a cheap count OR report `remaining` as best-effort). Simplest correct form: keep scanning to count all orphans found this pass (the predicate is cheap), but only disband the first `ReaperBatch`; then `remaining = totalOrphansFound - disbanded`. This makes the drip visible in `Playerbots.log` and is the perf-pacing proof for acceptance criterion 4.

> Implementer choice: counting ALL orphans (for an accurate `remaining`) vs. stopping at `ReaperBatch` (cheaper) is a minor trade. Because the predicate is pure in-memory and the candidate set is bounded by online-bot count, **count all orphans** so `remaining` is accurate; still disband only `ReaperBatch`. This keeps the log honest about drain progress at negligible cost.

---

## 12. Scope — OUT (do NOT touch)

- The `RandomPlayerbotMgr` per-bot update loop / the `:1477` guard / `ProcessBot` / `RandomBotUpdateAction`. Do NOT try to fix the starvation there. Leave the existing `:1573-1578` auto-disband as-is.
- Any change to run scheduling, formation, loot, or eligibility logic.
- Persisting `_runs`/`_raiding` across restarts.
- Off-thread disband / async DB (Player + Group mutation is world-thread-bound; the drip is the perf answer).
- Core repo edits (no `GetGroupStore()` accessor — §5).
- No new source files → no CMake reconfigure.

---

## 13. Acceptance criteria → how met

1. **Builds clean Release** (no new files / no reconfigure; no C2xxx/LNK; warnings OK, no /WX). All edits in existing `.cpp`/`.h`; new include `CharacterCache.h`. — Tasks 1-5; verified by master Task 6.
2. **Predicate correctness on review** — §6: a group is disbanded only when all 3 rules hold; any group with a non-random account is never touched; an active in-flight run (its `groupGuid` recorded, §7) is never disbanded.
3. **Functional (live Y:)** — after a restart taken with runs in-flight, the orphan backlog drains to ≈ live run count within a few reaper intervals (§9 drip); `.playerbots raidsim status runs=N` stays consistent (the reaper never touches `_runs`); in-flight runs survive (rule 3 + recorded `groupGuid`). — master Task 6.
4. **PERFORMANCE (live Y: Perf.log)** — during the boot drain of a large backlog, no correlated world-tick spike; drain visibly paced via the `disbanded {} ({} remaining)` log (§11) and the `ReaperBatch`/`ReaperInterval` budget (§3, §9). — master Task 6.
5. **Teardown disbands even when `leader->GetGroup()` is null** — §8 disband-by-GUID path with leader fallback.
