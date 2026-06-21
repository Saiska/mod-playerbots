# PLAN — raidsim-broadcast-realmwide

Branch: `feat/raidsim-broadcast-realmwide` (off master tip `534fe392`). Edit-only lane (Tasks 1-4).
Task 5 is MASTER-ONLY (build/deploy/verify). See `SPEC.md` for the full design and confirmed APIs.

All paths are inside the worktree `D:/wow/acore-pb-build/mod-playerbots-wt3`.
mod-playerbots only, no core change, **no new files** (no CMake reconfigure).

Conventions confirmed from the real source:
- `ChatHandler::BuildChatPacket(WorldPacket& data, ChatMsg msgtype, std::string_view message, ...)` —
  message is the 3rd arg; language defaults to LANG_UNIVERSAL. (The charter's call was wrong.)
- `sWorldSessionMgr` = `WorldSessionMgr::Instance()` → `WorldSessionMgr*` → use `->`.
  `SendGlobalMessage(WorldPacket const* packet, ...)`.
- `ChatHelper::FormatItem(ItemTemplate const* proto)` → quality-colored `|Hitem:|h[Name]|h|r`.
- `ItemTemplate.Quality` is `uint32`.
- `.conf` parser breaks on trailing `#` inline comments → comments on own lines.
- Build has NO `/WX` → C4xxx warnings are non-fatal; only C2xxx/LNK break it.

---

## Task 1 — Config fields + reads (`PlayerbotAIConfig.h`, `PlayerbotAIConfig.cpp`)

**File A: `src/PlayerbotAIConfig.h`**

Find the contiguous `raidSim*` declaration block. The relevant lines are currently:

```cpp
    bool   raidSimBroadcast;           // master switch for ALL guild-chat broadcasts
    bool   raidSimBroadcastStartStop;  // "sets out for X" / "returns from X" lines
    bool   raidSimBroadcastLoot;       // "<bot> receives <item>" lines
    bool   raidSimAnnounce;            // server-wide SendWorldText announce (debug/flavor; orthogonal)
```

Insert the 2 new fields immediately AFTER the `raidSimBroadcastLoot` line (before `raidSimAnnounce`):

```cpp
    bool   raidSimBroadcast;           // master switch for ALL guild-chat broadcasts
    bool   raidSimBroadcastStartStop;  // "sets out for X" / "returns from X" lines
    bool   raidSimBroadcastLoot;       // "<bot> receives <item>" lines
    bool   raidSimBroadcastRealmWide;      // realm-wide [System] lines (new wording) vs legacy guild chat
    uint32 raidSimBroadcastLootMinQuality; // min item Quality for a realm-wide loot line (spam gate)
    bool   raidSimAnnounce;            // server-wide SendWorldText announce (debug/flavor; orthogonal)
```

**File B: `src/PlayerbotAIConfig.cpp`**

The reads are currently (lines ~454-456):

```cpp
    raidSimBroadcast          = sConfigMgr->GetOption<bool>("RaidSim.Broadcast", true);
    raidSimBroadcastStartStop = sConfigMgr->GetOption<bool>("RaidSim.BroadcastStartStop", true);
    raidSimBroadcastLoot      = sConfigMgr->GetOption<bool>("RaidSim.BroadcastLoot", true);
```

Insert the 2 new reads immediately AFTER the `raidSimBroadcastLoot` read:

```cpp
    raidSimBroadcast          = sConfigMgr->GetOption<bool>("RaidSim.Broadcast", true);
    raidSimBroadcastStartStop = sConfigMgr->GetOption<bool>("RaidSim.BroadcastStartStop", true);
    raidSimBroadcastLoot      = sConfigMgr->GetOption<bool>("RaidSim.BroadcastLoot", true);
    raidSimBroadcastRealmWide      = sConfigMgr->GetOption<bool>("RaidSim.BroadcastRealmWide", true);
    raidSimBroadcastLootMinQuality = sConfigMgr->GetOption<uint32>("RaidSim.BroadcastLootMinQuality", 3);
```

**Verify:**
```
grep -n "raidSimBroadcastRealmWide\|raidSimBroadcastLootMinQuality" src/PlayerbotAIConfig.h src/PlayerbotAIConfig.cpp
```
Expect 2 hits in each file (1 decl + 1 read per field). Defaults: `BroadcastRealmWide` true, `BroadcastLootMinQuality` 3.

---

## Task 2 — Includes + START + STOP sites (`src/Mgr/RaidSim/RaidSimulationMgr.cpp`)

**Step 2a — add includes.** The include block ends with (around line 30):

```cpp
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
```

Add the 3 new includes. Place them in the existing alphabetical-ish include list (e.g. after
`#include "Chat.h"` add `ChatHelper.h`; near the end add the two session/packet ones). A simple safe
edit: insert right after the existing `#include "Chat.h"` line:

```cpp
#include "Chat.h"
#include "ChatHelper.h"
```

and right after `#include "StatsWeightCalculator.h"`:

```cpp
#include "StatsWeightCalculator.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"
```

**Step 2b — START site.** Currently (lines ~323-325):

```cpp
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                    guild->BroadcastToGuild(leader->GetSession(), false, "We set out for " + m_label + ".", LANG_UNIVERSAL);
```

Replace with:

```cpp
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                {
                    if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                    {
                        std::string msg = std::string(guild->GetName()) + " has set out for " + m_label + ".";
                        WorldPacket data;
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                        sWorldSessionMgr->SendGlobalMessage(&data);
                    }
                    else
                        guild->BroadcastToGuild(leader->GetSession(), false, "We set out for " + m_label + ".", LANG_UNIVERSAL);
                }
```

**Step 2c — STOP site.** Currently (lines ~356-359):

```cpp
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                    if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                        guild->BroadcastToGuild(leader->GetSession(), false, "We return from " + m_label + ".", LANG_UNIVERSAL);
```

Replace with:

```cpp
            if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastStartStop)
                if (Player* leader = ObjectAccessor::FindPlayer(m_leaderGuid))
                    if (Guild* guild = sGuildMgr->GetGuildById(leader->GetGuildId()))
                    {
                        if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                        {
                            std::string msg = std::string(guild->GetName()) + " returns from " + m_label + ".";
                            WorldPacket data;
                            ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                            sWorldSessionMgr->SendGlobalMessage(&data);
                        }
                        else
                            guild->BroadcastToGuild(leader->GetSession(), false, "We return from " + m_label + ".", LANG_UNIVERSAL);
                    }
```

**Verify:**
```
grep -n "ChatHelper.h\|WorldPacket.h\|WorldSessionMgr.h" src/Mgr/RaidSim/RaidSimulationMgr.cpp
grep -n "has set out for\|returns from\|We set out for\|We return from" src/Mgr/RaidSim/RaidSimulationMgr.cpp
```
Expect: 3 include hits; the new "has set out for"/"returns from" strings AND the preserved legacy
"We set out for"/"We return from" strings both present (proves both branches exist).

---

## Task 3 — LOOT site (item link + quality gate) (`src/Mgr/RaidSim/RaidSimulationMgr.cpp`)

Currently (lines ~1214-1217), inside `RaidSimulationMgr::AwardLoot`:

```cpp
        if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastLoot)
            if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
                guild->BroadcastToGuild(bot->GetSession(), false, bot->GetName() + " receives " + itemName + ".",
                                        LANG_UNIVERSAL);
```

(The lines just above — `ItemTemplate const* proto = ...`, `std::string itemName = ...`, and the
`LOG_INFO(... "receives" ...)` — stay UNCHANGED.) Replace the block above with:

```cpp
        if (sPlayerbotAIConfig.raidSimBroadcast && sPlayerbotAIConfig.raidSimBroadcastLoot)
            if (Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId()))
            {
                if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
                {
                    if (proto && proto->Quality >= sPlayerbotAIConfig.raidSimBroadcastLootMinQuality)
                    {
                        std::string msg = bot->GetName() + " of " + run.guildName + " equips " +
                                          ChatHelper::FormatItem(proto) + " in " + run.label + ".";
                        WorldPacket data;
                        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);
                        sWorldSessionMgr->SendGlobalMessage(&data);
                    }
                }
                else
                    guild->BroadcastToGuild(bot->GetSession(), false, bot->GetName() + " receives " + itemName + ".",
                                            LANG_UNIVERSAL);
            }
```

**Verify:**
```
grep -n "equips\|ChatHelper::FormatItem\|raidSimBroadcastLootMinQuality\|receives " src/Mgr/RaidSim/RaidSimulationMgr.cpp
```
Expect: the new "equips" + `ChatHelper::FormatItem(proto)` + the `proto->Quality >= ...MinQuality`
gate, AND the preserved legacy "receives " string (both branches present). The `LOG_INFO` "receives"
line is also a "receives " hit — confirm by inspection the broadcast `else` branch retains
`bot->GetName() + " receives " + itemName + "."` verbatim.

---

## Task 4 — conf.dist keys (`conf/playerbots.conf.dist`)

The orphan-reaper block ends at line ~2859-2860 with:

```
RaidSim.ReaperBatch = 3
```

Append a new RAIDSIM BROADCAST block right after it (comments on their OWN lines):

```
###################################
#    RAIDSIM BROADCAST
###################################
#
#    RaidSim.BroadcastRealmWide
#        1 = start/stop/loot announcements are realm-wide [System] lines that every
#            online player sees, carrying guild + raid name and a clickable item link.
#        0 = legacy: messages go to the (bot-only) raiding guild's guild chat, old wording.
#        The master RaidSim.Broadcast and per-event BroadcastStartStop/BroadcastLoot
#        toggles still apply on top of this.
#        Default: 1

RaidSim.BroadcastRealmWide = 1

#    RaidSim.BroadcastLootMinQuality
#        Minimum item quality for a realm-wide loot announcement (spam control).
#        0=Poor 1=Common 2=Uncommon 3=Rare 4=Epic 5=Legendary.
#        Only loot at or above this quality announces realm-wide. 0 = announce all.
#        Has no effect on the legacy guild-chat path or when RaidSim.BroadcastLoot = 0.
#        Default: 3 (Rare/blue and above)

RaidSim.BroadcastLootMinQuality = 3
```

**Verify:**
```
grep -n "RaidSim.BroadcastRealmWide\|RaidSim.BroadcastLootMinQuality" conf/playerbots.conf.dist
```
Expect 2 hits (one assignment line each, no trailing inline `#` comments on the assignment lines).

---

## Task 5 — MASTER ONLY (NOT the lane): build + deploy + verify

> The feature lane STOPS after Task 4 (commit on `feat/raidsim-broadcast-realmwide`, say
> "feat/raidsim-broadcast-realmwide ready to integrate", stop). Task 5 is the master's integration
> step — do NOT do it as the lane.

1. Build Release, NO reconfigure (no new files):
   `cmake --build acore/build --config Release --target worldserver --parallel`
   Expect clean — no C2xxx / LNK. (C4xxx warnings are non-fatal; no `/WX`.)
2. Deploy: stop world, swap `acore/build/bin/Release/worldserver.exe` → `Y:\worldserver.exe`
   (world must be down or the file is locked). Copy changed `playerbots.conf` if the deploy syncs
   the .dist→.conf. Restart world.
3. Acceptance:
   - **AC1** build clean.
   - **AC2** set `RaidSim.BroadcastRealmWide = 0`, restart/reload, run a sim → the 3 messages are
     guild-chat only, byte-identical to today ("We set out for X.", "We return from X.",
     "<bot> receives <item>.").
   - **AC3/AC5** set `=1`, `.playerbots raidsim start <guild>` on a populated realm; from a character
     NOT in the bot guild, confirm yellow `[System]` lines: start "<Guild> has set out for <Raid>.",
     stop "<Guild> returns from <Raid>.", per equipped upgrade "<Bot> of <Guild> equips [clickable
     Item] in <Raid>." — guild + raid name correct, item link clickable + quality-colored.
   - **AC4** with `BroadcastLootMinQuality = 3` greens/whites don't announce; `= 0` everything does;
     `RaidSim.BroadcastLoot = 0` → no loot lines at all.
