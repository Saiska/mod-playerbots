# SPEC — raidsim-broadcast-realmwide

Branch: `feat/raidsim-broadcast-realmwide` (off master tip `534fe392`)
Target: mod-playerbots fork (rebuild), deploy Y:. **mod-playerbots only, no core change, NO new files** (no CMake reconfigure).

## Problem

RaidSim's 3 broadcast messages (start / stop / loot) are sent via `Guild::BroadcastToGuild`,
i.e. into the **raiding bot guild's guild chat only**. RaidSim guilds are bot-only — no human is
ever a member — so nobody can read them (dead from a player POV). Config is already ON
(`RaidSim.Broadcast` / `BroadcastStartStop` / `BroadcastLoot` = 1). The wording is also
context-free (omits guild name; loot omits raid name; item is plain text, not a clickable link).

## Goal

Make start/stop/loot announcements **realm-wide** (yellow `[System]` line every online player sees)
and readable, carrying guild + raid name and a clickable, quality-colored item link, with a
**quality gate** on loot so a populated realm with many bot guilds isn't flooded. Preserve the
existing guild-chat path behind a toggle (zero behaviour change when the new mode is off).

## Design

### New config fields (2)

In `src/PlayerbotAIConfig.h`, in the contiguous `raidSim*` block (after `raidSimBroadcastLoot`,
near the orphan-reaper fields):

```cpp
bool   raidSimBroadcastRealmWide;      // new mode: realm-wide [System] lines vs legacy guild chat
uint32 raidSimBroadcastLootMinQuality; // loot quality gate (realm-wide path only); proto->Quality >= this
```

In `src/PlayerbotAIConfig.cpp`, immediately after the existing `raidSimBroadcastLoot` read
(currently line 456), matching the neighbours' `GetOption` style:

```cpp
raidSimBroadcastRealmWide      = sConfigMgr->GetOption<bool>("RaidSim.BroadcastRealmWide", true);
raidSimBroadcastLootMinQuality = sConfigMgr->GetOption<uint32>("RaidSim.BroadcastLootMinQuality", 3);
```

- `RaidSim.BroadcastRealmWide` default **true** (new behaviour on by default).
- `RaidSim.BroadcastLootMinQuality` default **3** (Rare/blue). 0 = announce everything.

Note: existing bool toggles in the RaidSim block use `GetOption<bool>`; ints use `GetOption<int32>`.
The min-quality field is a `uint32` quality value (never negative) → `GetOption<uint32>`, matching
the field type cleanly.

### Includes to add (in `RaidSimulationMgr.cpp`)

Already present: `Chat.h`, `Guild.h`, `GuildMgr.h`, `ItemTemplate.h`, `ObjectMgr.h`, `Player.h`,
`PlayerbotAIConfig.h`. **Add:**

```cpp
#include "ChatHelper.h"        // ChatHelper::FormatItem
#include "WorldSessionMgr.h"   // sWorldSessionMgr->SendGlobalMessage
#include "WorldPacket.h"       // local WorldPacket stack object
```

Confirmed: across the module, `ChatHelper.h` is included as `#include "ChatHelper.h"` (build adds
its dir to the include path; e.g. `src/Bot/PlayerbotAI.cpp`). `WorldSessionMgr.h` is included the
same way elsewhere (`src/Bot/PlayerbotMgr.cpp:37`). Files that build a stack `WorldPacket` include
`WorldPacket.h` explicitly (e.g. `AcceptInvitationAction.cpp`), so we add it even though Chat.h takes
`WorldPacket&` by reference.

### The realm-wide send (CORRECTED — the charter's call was wrong)

Verified `acore/src/server/game/Chat/Chat.h:51`:

```cpp
static void BuildChatPacket(
    WorldPacket& data, ChatMsg msgtype, std::string_view message, Language language = LANG_UNIVERSAL,
    PlayerChatTag chatTag = CHAT_TAG_NONE, ObjectGuid const& senderGuid = ObjectGuid(), ...);
```

→ **message is the 3rd arg, language the 4th.** The correct realm-wide send is:

```cpp
WorldPacket data;
ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, msg);   // language defaults LANG_UNIVERSAL
sWorldSessionMgr->SendGlobalMessage(&data);
```

`sWorldSessionMgr` is `#define sWorldSessionMgr WorldSessionMgr::Instance()` returning
`WorldSessionMgr*` (→ `->`). `SendGlobalMessage(WorldPacket const* packet, WorldSession* self =
nullptr, TeamId = TEAM_NEUTRAL)` at `WorldSessionMgr.h:82`. `msg` is a `std::string` (binds to
`std::string_view`).

### Per-site branch structure

Each of the 3 sites keeps its **existing outer guard** wrapping BOTH branches. The new code is a
branch on `raidSimBroadcastRealmWide`:

```
if (<existing outer guard: raidSimBroadcast && raidSimBroadcast{StartStop|Loot}>)
{
    if (sPlayerbotAIConfig.raidSimBroadcastRealmWide)
        <realm-wide path: build msg, BuildChatPacket, SendGlobalMessage>
    else
        <legacy BroadcastToGuild path — BYTE-IDENTICAL to today>
}
```

Both start/stop legacy and realm-wide branches need the `Guild*` (legacy calls
`guild->BroadcastToGuild`; realm-wide needs `guild->GetName()`). So the `if (Guild* guild = ...)`
lookup stays and the `raidSimBroadcastRealmWide` branch sits **inside** it. If `guild` is null,
neither branch runs (the existing `if (Guild* guild = ...)` short-circuits) — graceful, no crash.

### Site 1 — START (`RaidSimFormationOperation::Execute`, currently :323-325)

Scope: `leader` (Player*), `m_label` (std::string), `guild` (Guild*).

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

Message: `"<Guild> has set out for <Raid>."`

### Site 2 — STOP (`RaidSimTeardownOperation::Execute`, currently :356-359)

Scope: `m_leaderGuid` → `leader` (Player*, via `ObjectAccessor::FindPlayer`), `m_label`, `guild`.

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

Message: `"<Guild> returns from <Raid>."`

### Site 3 — LOOT (`RaidSimulationMgr::AwardLoot`, currently :1214-1217)

This point is reached **only on a successful equip** (the `continue` filters above mean non-upgrades
never get here), so "equips" is definitively correct (decision 3). Scope: `run.guildName`,
`run.label`, `bot` (Player*), `proto` (ItemTemplate const*), `itemName`, `guild`.

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

Message: `"<Bot> of <Guild> equips <[ItemLink]> in <Raid>."`

Quality gate (realm-wide only): `proto && proto->Quality >= raidSimBroadcastLootMinQuality`.
Confirmed `ItemTemplate.Quality` is `uint32` (`acore/.../ItemTemplate.h:626`). `ChatHelper::FormatItem`
signature confirmed (`src/Bot/Cmd/ChatHelper.h:46`):
`static std::string const FormatItem(ItemTemplate const* proto, uint32 count = 0, uint32 total = 0);`
→ called `ChatHelper::FormatItem(proto)`, yields `|Hitem:...|h[Name]|h|r` (quality-colored).

The `LOG_INFO("playerbots", "RaidSim: '{}' {} receives {} ({})", ...)` line at :1211-1212 and
`itemName` are **left unchanged** (the legacy branch still uses `itemName`; the log is independent).

### conf.dist keys

In `conf/playerbots.conf.dist`, add a `RAIDSIM BROADCAST` block after the existing orphan-reaper
block (which ends at line 2860 with `RaidSim.ReaperBatch = 3`). Comments on **their own lines** (AC
parser breaks on trailing `#` inline comments):

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

## Acceptance criteria → how met

1. **Builds clean Release, no reconfigure, no C2xxx/LNK.** No new files; only existing files edited.
   Corrected `BuildChatPacket` signature compiles (message 3rd arg). Includes added for the 3 new
   symbols.
2. **`BroadcastRealmWide=0` → 3 messages byte-identical to today.** The `else` branches are the
   verbatim current `BroadcastToGuild` calls (same args, same wording "We set out for…", "We return
   from…", "<bot> receives <item>."). No quality gate on the legacy path.
3. **`=1` → human not in the bot guild sees yellow `[System]` lines** with correct guild+raid name and
   clickable quality-colored item link. `CHAT_MSG_SYSTEM` + `SendGlobalMessage` reaches all online
   sessions; `ChatHelper::FormatItem(proto)` gives the link.
4. **`BroadcastLootMinQuality` gates loot** (default 3 → greens/whites suppressed; 0 = all;
   `BroadcastLoot=0` = no loot lines, since the outer guard still requires `raidSimBroadcastLoot`).
5. **Live-verifiable** via `.playerbots raidsim start <guild>` on a populated realm; master confirms
   from a non-guild character (MASTER-ONLY step).

## Out of scope (do NOT touch)

- Any discard / "not taken" / "received but didn't equip" line (non-upgrades are a silent `continue`).
- RaidSim award/equip logic, loot composition, any non-broadcast behaviour.
- `ChatHelper::FormatItem`, `BroadcastToGuild`, `SendWorldText`, `BuildChatPacket`,
  `SendGlobalMessage` themselves (consumed, not changed).
- New files / timers / throttle (realm-wide chat is one cheap packet per event — start/stop are rare;
  loot already gated to upgrades and now quality).
- The `LOG_INFO` "RaidSim: '...' receives ... " server-log line (unchanged).

## Confirmed real facts (vs charter)

- `ChatHandler::BuildChatPacket` real signature has **message 3rd, language 4th** — the charter's
  `(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, msg)` was WRONG. Use
  `BuildChatPacket(data, CHAT_MSG_SYSTEM, msg)`.
- `ItemTemplate` quality field = `uint32 Quality;` (confirmed).
- `ChatHelper.h` is included as `#include "ChatHelper.h"` across the module.
- `WorldPacket.h` is added explicitly (module convention for stack `WorldPacket` objects).
