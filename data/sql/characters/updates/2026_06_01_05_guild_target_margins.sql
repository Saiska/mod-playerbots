-- GuildLifecycle — raid-guild target_size margins.
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by the dbupdater at boot (updates/ stream). Ordered AFTER
-- 2026_05_29_00_themed_guilds_schema.sql (adds the target_size column),
-- 2026_05_29_01_themed_guilds_data.sql (seeds the rows), and
-- 2026_05_30_02_raid_guild_assignments.sql (sets raid_offset/max_band). Re-applicable
-- (absolute fixed-value UPDATEs).
--
-- Raid guilds (raid_offset >= 0) need target_size = largest fielded content size + 5, so the
-- GuildLifecycle backfill keeps each raid guild above raid-sim's "field-what-you-can" headcount
-- threshold through pop-dynamics prune / offline churn. Keyed by theme_slug (matches the
-- raid_offset assignment convention; one theme can have many candidate name rows). Casual guilds
-- (raid_offset < 0) are unchanged. Margin convention: +5 over the largest content group_size
-- (25-man -> 30, 10-man -> 15).
--
-- Per-theme content sizes derived from the live ladder (playerbots_raid_tier_instance) bands
-- crossed with each guild's max_band:
--   wow_guild_stalwart_vigil     (offset 0, max_band 7) -> bands 0-7, max group_size 25 -> 30
--   wow_guild_stormwind_vanguard (offset 1, max_band 7) -> bands 0-7, max group_size 25 -> 30
--   wow_guild_argent_lance       (offset 2, max_band 1) -> bands 0-1, max group_size 10 -> 15

UPDATE playerbots_guild_names SET target_size = 30 WHERE theme_slug = 'wow_guild_stalwart_vigil';
UPDATE playerbots_guild_names SET target_size = 30 WHERE theme_slug = 'wow_guild_stormwind_vanguard';
UPDATE playerbots_guild_names SET target_size = 15 WHERE theme_slug = 'wow_guild_argent_lance';
