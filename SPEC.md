# SPEC: raidsim-player-guild-guard

## Bug

RaidSim can conscript random bots out of a guild that a **real player**
created/joined. Its guild-selection has no real-player-guild guard, unlike
`GuildLifecycle` which already gates on `IsBotManagedGuild` at
`PlayerbotGuildMgr.cpp:526`. Result: RaidSim teleports/loots bots belonging to
a human's guild, and its scheduler repeatedly targets such guilds (e.g.
"Lifestone"), emitting a recurring "no matching theme" WARN via
`ResolveGuildInstance`.

The per-bot gate `BotPassesBaseEligibility` (`!IsRandomBot(bot)`) only excludes
human-mastered alt companions; it does nothing for random bots a real player
invited into their own guild. That gate is correct and stays unchanged.

## Two entry points (both missing the guard)

1. Scheduler `RaidSimulationMgr::Update` — builds `candidateGuilds` from every
   online bot's `GetGuildId()` with no membership check
   (`RaidSimulationMgr.cpp:945-956`).
2. GM `RaidSimulationMgr::Start` — resolves the guild by name and proceeds with
   no membership check (`:760-767`).

## Fix

Authoritative test: `PlayerbotGuildMgr::IsBotManagedGuild(guildId)` — scans
every member, returns false if any is a real player or has an unknown/missing
account (already subsumes the leader-only `IsRealGuild` fast-path). RaidSim's
rule: run for a guild only if `IsBotManagedGuild(guildId)` is true. Evaluate
**once per guild** (it issues a `CharacterDatabase.Query` — never per-bot).

1. **Expose**: make `IsBotManagedGuild` public in `PlayerbotGuildMgr.h` (move
   the declaration + its two comment lines out of `private:`). No behavior
   change — single source of truth reused by both controllers.
2. **Scheduler `Update`**: after `candidateGuilds` is built (already deduped by
   guild id), drop any entry that fails `IsBotManagedGuild` — one DB test per
   distinct candidate guild per 60s pass. Excluded guilds never reach
   `ResolveGuildInstance`, so the Lifestone WARN stops.
3. **GM `Start`**: after `GetGuildByName` resolves (non-null), if the guild
   fails `IsBotManagedGuild`, `PSendSysMessage` a clear rejection and
   `return false` BEFORE the run-exists check.

Singleton accessor: `PlayerbotGuildMgr::instance()` (already used in
`RaidSimulationMgr.cpp:630`; header already included at `:24`).

## Out of scope

No core change, no SQL, no config key (guard is unconditional). Do not touch
`BotPassesBaseEligibility`, `IsBotManagedGuild`/`IsRealGuild` behavior,
GuildLifecycle/`ReconcileGuilds`, or `playerbots_guild_names`. No teardown of
in-flight runs (they finish naturally). No new source file.

## Acceptance criteria

- Scheduler stops targeting real-player guilds (e.g. Lifestone) and the
  recurring "no matching theme" WARN stops.
- GM `.playerbots raidsim start <realPlayerGuild>` is rejected with a clear
  message; no run starts.
- Fully bot-managed guilds still start runs (scheduler + GM) as before.
- In-flight runs are unaffected.
