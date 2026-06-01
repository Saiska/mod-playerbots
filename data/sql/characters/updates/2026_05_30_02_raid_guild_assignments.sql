-- Autonomous Instance Simulation — per-guild TIER offsets/ceilings.
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by the dbupdater at boot (updates/ stream). Ordered AFTER
-- 2026_05_30_00_raid_sim_schema.sql (which creates raid_offset/max_band) and
-- 2026_05_29_01_themed_guilds_data.sql (which seeds the guild rows), so the columns
-- and target rows both exist. Re-applicable (fixed-value UPDATEs). Edit per install.
-- (Moved here from data/sql/characters/custom/ on 2026-06-01: custom/ applies BEFORE
--  updates/, so this ran before its schema existed and aborted boot on a fresh install.)
--
-- Emergent model: EVERY guild participates based on how many of its level-80 bots are online
-- (headcount picks dungeon/10/25). raid_offset is a pure TIER modifier:
--   raid_offset >= 0  -> bands behind the progression frontier (0 = frontier).
--   raid_offset  < 0  -> unassigned; the server uses RaidSim.DefaultOffset (still participates).
-- max_band: hard ceiling (band index 0-7) that keeps a casual guild off high content.
--
-- Tailored to THIS install's themed-guild slugs ('wow_guild_*'). Guilds left untouched keep
-- raid_offset = -1, i.e. they run at RaidSim.DefaultOffset. Add a line below to pin a guild's tier.

-- Frontier raiding guild (reaches whatever the lead real player has unlocked):
UPDATE playerbots_guild_names SET raid_offset = 0, max_band = 7 WHERE theme_slug = 'wow_guild_stalwart_vigil';
-- A second progression guild kept one band behind the frontier:
UPDATE playerbots_guild_names SET raid_offset = 1, max_band = 7 WHERE theme_slug = 'wow_guild_stormwind_vanguard';
-- Casual guild: capped at heroic-dungeon / entry-raid bands, never walks into 25H:
UPDATE playerbots_guild_names SET raid_offset = 2, max_band = 1 WHERE theme_slug = 'wow_guild_argent_lance';
