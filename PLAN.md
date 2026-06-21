# PLAN — raidsim-orphan-reaper

Branch: `feat/raidsim-orphan-reaper` (off master tip `8e81a702`). Edit-only lane (Tasks 1-5). Task 6 is master-only (build/deploy/perf-verify). See `SPEC.md` for the full design and confirmed APIs.

All paths are relative to the worktree `D:/wow/acore-pb-build/mod-playerbots-wt3/`. Line numbers are as-of the SPEC's anchoring; re-confirm by grepping the quoted surrounding code before editing (the file may shift by a few lines as earlier tasks land — prefer matching on the quoted code, not the number).

Conventions reused from the codebase:
- fmt `{}` placeholders in `LOG_*`, never `%d`/`%u`.
- AC `.conf` parser breaks on trailing `#` inline comments — comments on their own line.
- Build has no `/WX`; C4xxx warnings are non-fatal, only C2xxx/LNK break it.
- Null-guard every `FindPlayer`/`GetGroupByGUID` (can return null).

---

## Task 1 — Config keys (PlayerbotAIConfig.h + .cpp)

**Files:** `src/PlayerbotAIConfig.h`, `src/PlayerbotAIConfig.cpp`

### 1a. Declare members (`src/PlayerbotAIConfig.h`)

Find the end of the `raidSim*` member block (`:686-702`), which currently ends with:

```cpp
    bool   raidSimAnnounce;            // server-wide SendWorldText announce (debug/flavor; orthogonal)
```

Insert immediately after that line:

```cpp
    // Orphan reaper (raidsim-orphan-reaper): leader-independent teardown of all-random-bot groups
    // left over from a restart (orphans not backed by a live run). Budgeted drip; off the world tick.
    bool   raidSimOrphanReaper;    // master switch for the reaper
    uint32 raidSimReaperInterval;  // seconds between reaper passes
    uint32 raidSimReaperBatch;     // max orphan groups disbanded per pass (drip budget)
```

### 1b. Read keys (`src/PlayerbotAIConfig.cpp`)

Find the `raidSim*` read block (`:442-457`), which ends with:

```cpp
    raidSimAnnounce         = sConfigMgr->GetOption<bool>("RaidSim.Announce", false);
```

Insert immediately after that line:

```cpp
    raidSimOrphanReaper     = sConfigMgr->GetOption<bool>("RaidSim.OrphanReaper", true);
    raidSimReaperInterval   = sConfigMgr->GetOption<int32>("RaidSim.ReaperInterval", 120);
    raidSimReaperBatch      = sConfigMgr->GetOption<int32>("RaidSim.ReaperBatch", 3);
```

**Verify:** `grep -n "raidSimOrphanReaper\|raidSimReaperInterval\|raidSimReaperBatch" src/PlayerbotAIConfig.h src/PlayerbotAIConfig.cpp` shows 3 decls + 3 reads. Types line up (bool / uint32 / uint32 ← int32 option is fine, same as the other raidSim ints).

---

## Task 2 — `ActiveRun.groupGuid` field + setter (RaidSimulationMgr.h)

**File:** `src/Mgr/RaidSim/RaidSimulationMgr.h`

### 2a. Add the field to `struct ActiveRun` (`:69-82`)

The struct ends with:

```cpp
        uint32 elapsedMs = 0;
        uint32 lootTimerMs = 0;
    };
```

Add the field before the closing brace (after `lootTimerMs`):

```cpp
        uint32 elapsedMs = 0;
        uint32 lootTimerMs = 0;
        ObjectGuid groupGuid;             // the formed Group's GUID (set async by formation op)
    };
```

### 2b. Declare the setter + reaper helper + reaper timer field

In the private helpers area (the run of `bool/void` helper decls, `:84-98`), add after the existing helper decls (e.g. after `void  PersistBaseIlvl();`):

```cpp
    void  SetRunGroupGuid(uint32 guildId, ObjectGuid groupGuid);  // formation op -> record live group
    void  ReconcileOrphans(uint32 diff);  // leader-independent orphan-group reaper (drip)
```

In the private data-member area, find `uint32 _schedTimerMs = 0;` (`:117`) and add right after it:

```cpp
    uint32 _schedTimerMs = 0;
    uint32 _reaperTimerMs = 0;  // raidsim-orphan-reaper cadence
```

`ObjectGuid` is already available (`#include "ObjectGuid.h"` at `:15`).

**Verify:** `grep -n "groupGuid\|SetRunGroupGuid\|ReconcileOrphans\|_reaperTimerMs" src/Mgr/RaidSim/RaidSimulationMgr.h` shows the field, two decls, and the timer.

---

## Task 3 — `SetRunGroupGuid` impl + formation records groupGuid (RaidSimulationMgr.cpp)

**File:** `src/Mgr/RaidSim/RaidSimulationMgr.cpp`

### 3a. Add `CharacterCache.h` include

Find the include block (`:6-29`). After `#include "ObjectAccessor.h"` (`:17`), add:

```cpp
#include "CharacterCache.h"
```

(Alphabetical-ish; exact position not critical. This is needed by Task 5's predicate but add it now so the file compiles after each task. Safe to add here.)

### 3b. Thread `guildId` into `RaidSimFormationOperation`

The op ctor (`:251-257`) is:

```cpp
        RaidSimFormationOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids,
                                  uint32 mapId, uint8 difficulty, std::string label,
                                  float x, float y, float z, float o)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_mapId(mapId),
              m_difficulty(difficulty), m_label(std::move(label)), m_x(x), m_y(y), m_z(z), m_o(o)
        {
        }
```

Add a leading `uint32 guildId` param and init `m_guildId`:

```cpp
        RaidSimFormationOperation(uint32 guildId, ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids,
                                  uint32 mapId, uint8 difficulty, std::string label,
                                  float x, float y, float z, float o)
            : m_guildId(guildId), m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_mapId(mapId),
              m_difficulty(difficulty), m_label(std::move(label)), m_x(x), m_y(y), m_z(z), m_o(o)
        {
        }
```

Add the member to the op's private data (`:332-338`), after `ObjectGuid m_leaderGuid;`:

```cpp
    private:
        uint32 m_guildId;
        ObjectGuid m_leaderGuid;
```

### 3c. Record the GUID right after group creation

In `RaidSimFormationOperation::Execute`, find (`:286`):

```cpp
            sGroupMgr->AddGroup(group);
```

Add immediately after it:

```cpp
            sRaidSimulationMgr.SetRunGroupGuid(m_guildId, group->GetGUID());
```

### 3d. Update the construction site in `LaunchRun`

Find (`:676-678`):

```cpp
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimFormationOperation>(run.leader, members, inst.mapId, inst.difficulty,
                                                    inst.label, inst.entryX, inst.entryY, inst.entryZ, inst.entryO));
```

Add `guildId` as the first arg:

```cpp
    PlayerbotWorldThreadProcessor::instance().QueueOperation(
        std::make_unique<RaidSimFormationOperation>(guildId, run.leader, members, inst.mapId, inst.difficulty,
                                                    inst.label, inst.entryX, inst.entryY, inst.entryZ, inst.entryO));
```

(`guildId` is the function param of `LaunchRun(uint32 guildId, ...)` — in scope.)

### 3e. Implement `SetRunGroupGuid`

Add a definition (place it near `EndRun`/`LaunchRun`, e.g. right after `LaunchRun`'s closing brace at `:682`):

```cpp
void RaidSimulationMgr::SetRunGroupGuid(uint32 guildId, ObjectGuid groupGuid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _runs.find(guildId);
    if (it != _runs.end())
        it->second.groupGuid = groupGuid;
}
```

**Verify:** `grep -n "SetRunGroupGuid\|m_guildId\|<RaidSimFormationOperation>(guildId" src/Mgr/RaidSim/RaidSimulationMgr.cpp` — ctor takes guildId, member set, Execute calls the setter, LaunchRun passes guildId, impl present. Re-read Execute to confirm `SetRunGroupGuid` is after a successful `Create` (the `if (!group->Create(...)) { delete group; return false; }` block already returns early on failure, so reaching `AddGroup` means success).

---

## Task 4 — Teardown disband-by-GUID (RaidSimulationMgr.cpp)

**File:** `src/Mgr/RaidSim/RaidSimulationMgr.cpp`

### 4a. Thread `groupGuid` into `RaidSimTeardownOperation`

The ctor (`:344-347`):

```cpp
        RaidSimTeardownOperation(ObjectGuid leaderGuid, std::vector<ObjectGuid> memberGuids, std::string label)
            : m_leaderGuid(leaderGuid), m_memberGuids(std::move(memberGuids)), m_label(std::move(label))
        {
        }
```

becomes:

```cpp
        RaidSimTeardownOperation(ObjectGuid leaderGuid, ObjectGuid groupGuid,
                                 std::vector<ObjectGuid> memberGuids, std::string label)
            : m_leaderGuid(leaderGuid), m_groupGuid(groupGuid),
              m_memberGuids(std::move(memberGuids)), m_label(std::move(label))
        {
        }
```

Add the member to the op's private data (`:380-383`), after `ObjectGuid m_leaderGuid;`:

```cpp
    private:
        ObjectGuid m_leaderGuid;
        ObjectGuid m_groupGuid;
```

### 4b. Harden the disband in `Execute`

Find (`:366-368`):

```cpp
            if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                if (Group* g = leader->GetGroup())
                    g->Disband(true);
```

Replace with:

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

### 4c. Pass `run.groupGuid` at the construction site in `EndRun`

Find (`:687-688`):

```cpp
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RaidSimTeardownOperation>(run.leader, run.members, run.label)))
```

becomes:

```cpp
    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RaidSimTeardownOperation>(run.leader, run.groupGuid, run.members, run.label)))
```

**Verify:** `grep -n "m_groupGuid\|RaidSimTeardownOperation>(run.leader" src/Mgr/RaidSim/RaidSimulationMgr.cpp` — ctor + member + GUID-disband + construction site all updated. `Group.h`/`GroupMgr.h` already included.

---

## Task 5 — The reaper: `ReconcileOrphans` + Update() wiring (RaidSimulationMgr.cpp)

**File:** `src/Mgr/RaidSim/RaidSimulationMgr.cpp`

### 5a. Wire the call into `Update()`

Find the top of `Update` (`:856-861`):

```cpp
void RaidSimulationMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidSimEnable)
        return;

    std::lock_guard<std::mutex> lock(_mutex);
```

Insert the reaper call between the `raidSimEnable` early-return and the `lock_guard` (so the reaper runs lock-independently and is unaffected by the scheduler's later early returns):

```cpp
void RaidSimulationMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidSimEnable)
        return;

    ReconcileOrphans(diff);

    std::lock_guard<std::mutex> lock(_mutex);
```

### 5b. Implement `ReconcileOrphans`

Add the definition (place it right after the `Update` function's closing brace, end of file region after `:894`-ish — or anywhere at file scope in the .cpp). Full body:

```cpp
void RaidSimulationMgr::ReconcileOrphans(uint32 diff)
{
    if (!sPlayerbotAIConfig.raidSimOrphanReaper)
        return;

    // --- Short lock: advance our own timer and snapshot live-run group GUIDs. ---
    std::unordered_set<ObjectGuid> activeGroupGuids;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _reaperTimerMs += diff;
        if (_reaperTimerMs < sPlayerbotAIConfig.raidSimReaperInterval * 1000u)
            return;
        _reaperTimerMs = 0;
        for (auto const& kv : _runs)
            if (kv.second.groupGuid)
                activeGroupGuids.insert(kv.second.groupGuid);
    }
    // _mutex released — NO Group::Disband under the lock.

    uint32 const batch = sPlayerbotAIConfig.raidSimReaperBatch;

    // --- Lock-free scan: candidate groups via online bots, deduped by group GUID. ---
    std::unordered_set<ObjectGuid> seenGroups;
    std::vector<Group*> toDisband;   // capped at batch
    uint32 totalOrphans = 0;

    for (auto const& it : ObjectAccessor::GetPlayers())
    {
        Player* bot = it.second;
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
            continue;

        Group* grp = bot->GetGroup();
        if (!grp)
            continue;

        ObjectGuid gguid = grp->GetGUID();
        if (!seenGroups.insert(gguid).second)
            continue;  // already evaluated this group

        // Rule 1: normal party/raid only.
        if (grp->isLFGGroup() || grp->isBGGroup() || grp->isBFGroup())
            continue;

        // Rule 3: skip live in-flight runs.
        if (activeGroupGuids.find(gguid) != activeGroupGuids.end())
            continue;

        // Rule 2: every member's account is a random-bot account.
        auto const& slots = grp->GetMemberSlots();
        if (slots.empty())
            continue;  // degenerate -> treat as NOT all-bot
        bool allBots = true;
        for (auto const& ms : slots)
        {
            uint32 acc = sCharacterCache->GetCharacterAccountIdByGuid(ms.guid);
            if (!sPlayerbotAIConfig.IsInRandomAccountList(acc))
            {
                allBots = false;
                break;
            }
        }
        if (!allBots)
            continue;

        // Orphan confirmed.
        ++totalOrphans;
        if (toDisband.size() < batch)
            toDisband.push_back(grp);
    }

    if (toDisband.empty())
        return;

    uint32 disbanded = 0;
    for (Group* g : toDisband)
    {
        g->Disband(true);
        ++disbanded;
    }

    uint32 remaining = (totalOrphans > disbanded) ? (totalOrphans - disbanded) : 0u;
    LOG_INFO("playerbots", "RaidSim: reaper disbanded {} orphan group(s) ({} remaining)",
             disbanded, remaining);
}
```

Notes for the implementer:
- `GET_PLAYERBOT_AI(bot)` is the macro the existing scheduler uses for the same online-bot filter (`:902`). Reuse it verbatim.
- `ObjectAccessor::GetPlayers()` is already used at `:899` — same iteration shape (`for (auto const& it : ...) it.second`).
- `Group`, `GroupMgr`, `ObjectAccessor`, `Player`, `PlayerbotAIConfig` headers are all already included; `CharacterCache.h` was added in Task 3a.
- `<unordered_set>` / `<vector>` are already included via the .h (`RaidSimulationMgr.h:21,23`) and transitively; if the compiler complains, add `#include <unordered_set>` / `#include <vector>` to the .cpp (defensive — should not be needed).
- We count ALL orphans (`totalOrphans`) for an accurate `remaining`, but only disband the first `batch`. The predicate is pure in-memory so the full count is cheap.

**Verify (lane, no build):**
- `grep -n "ReconcileOrphans\|reaper disbanded" src/Mgr/RaidSim/RaidSimulationMgr.cpp` — call in Update + impl + log line present.
- Re-read the impl and confirm: no `Group::Disband` inside the `{ std::lock_guard ... }` block; the lock scope contains only timer + snapshot; predicate uses all 3 rules; `toDisband` capped at `batch`; log uses `{}` not `%`.
- Confirm `Update`'s existing body below the inserted call is byte-unchanged (the `std::lock_guard lock(_mutex)` line and everything after are untouched).

---

## Task 5b — Conf template block (conf/playerbots.conf.dist)

**File:** `conf/playerbots.conf.dist`

The `.dist` has no RaidSim section. Append the reaper block at the end of the file (or adjacent to the Population Dynamics block if one exists). Comments on their OWN lines (AC parser breaks on trailing `#`):

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

**Verify:** `grep -n "RaidSim.OrphanReaper\|RaidSim.ReaperInterval\|RaidSim.ReaperBatch" conf/playerbots.conf.dist` — 3 keys present, no trailing `#` on any key line.

---

## Lane wrap-up (after Tasks 1-5b)

- `git -C D:/wow/acore-pb-build/mod-playerbots-wt3 add -A && git commit -m "feat(raidsim): leader-independent orphan-group reaper"` (Conventional Commits; scope `raidsim`).
- Say **"feat/raidsim-orphan-reaper ready to integrate"** and STOP. Do NOT build/merge/deploy.

---

## Task 6 — MASTER-ONLY (NOT the lane): build, deploy, live perf-verify

> This task is executed by the MASTER console only. The lane does NOT do any of this.

1. **Build** (no new files, no reconfigure needed):
   `cmake --build acore/build --config Release --target worldserver --parallel`. Expect clean Release (warnings OK, no /WX). Watch for C2xxx/LNK.
2. **Deploy live conf keys.** Append the 3 reaper keys (`RaidSim.OrphanReaper=1`, `RaidSim.ReaperInterval=120`, `RaidSim.ReaperBatch=3`) to the RaidSim section of both `run/configs/modules/playerbots.conf` and the Y: copy `C:/server/w/configs/modules/playerbots.conf` (comments on own lines). Defaults equal these so a miss is safe, but present-for-tuning.
3. **Deploy binary:** stop world (`POST /stop/world`), back up + swap `acore/build/bin/Release/worldserver.exe` → `Y:/worldserver.exe`, `POST /start/world`.
4. **Functional verify (live Y:):**
   - Take a restart while runs are in-flight (or against the current backlog: 141 groups / ~129 orphans).
   - Watch `Y:/Playerbots.log` for `RaidSim: reaper disbanded {} orphan group(s) ({} remaining)` — confirm the backlog drains by `ReaperBatch` per `ReaperInterval`, down to ≈ live run count.
   - `.playerbots raidsim status` — `runs=N` stays consistent; in-flight runs survive (their `groupGuid`-recorded groups are NOT disbanded).
   - DB cross-check (`pbtest_characters`): persisted `groups` count drops to ≈ in-flight-run count; instance-map groups untouched.
5. **PERFORMANCE verify (acceptance gate):** during the boot drain, inspect `Y:/Perf.log` (or the maps/tick instrumentation) — confirm **no correlated world-tick spike** aligned with the reaper passes. The `disbanded {} ({} remaining)` cadence in `Playerbots.log` should pace the drain across many intervals, not a single storm.
6. **Teardown-by-GUID verify:** confirm a normal run teardown still disbands (and, if observable, that a teardown with a momentarily-unresolved leader still disbands via the GUID path).
7. Record done work in `asp-server-config/docs/integration-log.md` (commit + date); delete `feat/raidsim-orphan-reaper` at merge.

---

## Open questions / risks (surfaced to master at hand-off)

1. **GroupMgr has NO public group-store iterator** (charter assumed `GetGroupStore()`). `GroupStore` is `protected`. The reaper therefore enumerates candidate groups via **online bots** (`ObjectAccessor::GetPlayers()` → `GetGroup()`), not a full group sweep. This is mod-playerbots-only (no core edit) and covers exactly the harmful set on an autologin realm (all orphan members are in-world). **Accepted limitation:** a fully-offline orphan group is reaped only when a member logs in. If full-sweep reaping is ever required, that needs a separate charter adding a public core accessor. — Confirm the master accepts this design choice (it satisfies the live-evidence case: 592 grouped bots are all in-world).
2. **`.dist` template had no RaidSim block at all** — the live `RaidSim.*` keys exist only in the deployed conf. The lane adds the reaper block to `.dist`; the master must add the 3 keys to the live/Y: conf (Task 6 step 2). Coded defaults match, so safe either way.
