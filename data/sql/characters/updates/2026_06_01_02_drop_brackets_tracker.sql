-- Drop the orphaned mod-player-bot-level-brackets tracker table.
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by the dbupdater at boot. Idempotent (DROP ... IF EXISTS).
--
-- Context: server-population-dynamics (PopulationDynamicsMgr) subsumes and replaces
-- mod-player-bot-level-brackets, which was removed from the build on 2026-06-01.
-- bot_level_brackets_guild_tracker was that module's per-guild redistribution tracker
-- and is now orphaned. (Lives here, in the module that replaced it, per the rule that
-- a fork must carry all the SQL it needs — no external custom_sql_script/ step.)

DROP TABLE IF EXISTS bot_level_brackets_guild_tracker;
