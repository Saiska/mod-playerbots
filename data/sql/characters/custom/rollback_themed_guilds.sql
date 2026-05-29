-- rollback_themed_guilds.sql
-- Drops the schema additions and restores the upstream 400-name pool.
--
-- IMPORTANT: this rolls back the DATABASE only. The mod-playerbots
-- worldserver binary still expects the new columns. Either:
--   (a) Replace Server\worldserver.exe with a pre-themed-guilds build, OR
--   (b) git revert the fork's themed-guilds commits, rebuild, redeploy,
--       and only then run this script.
-- Otherwise the next boot will throw query errors on LoadGuildNames.

USE acore_characters;

DROP TABLE IF EXISTS playerbots_guild_rank_names;

ALTER TABLE playerbots_guild_names
  DROP INDEX idx_pgn_theme_slug,
  DROP COLUMN theme_slug,
  DROP COLUMN faction,
  DROP COLUMN class_mask,
  DROP COLUMN race_mask,
  DROP COLUMN affinity_class_mask,
  DROP COLUMN affinity_race_mask,
  DROP COLUMN min_level,
  DROP COLUMN target_size,
  DROP COLUMN tabard_emblem_style,
  DROP COLUMN tabard_emblem_color,
  DROP COLUMN tabard_border_style,
  DROP COLUMN tabard_border_color,
  DROP COLUMN tabard_bg_color;

-- Restore the upstream 400-row pool from the canonical source.
-- Run AFTER `git revert` so this path serves the upstream file again.
SOURCE D:/wow/acore-pb-build/acore/modules/mod-playerbots/data/sql/characters/base/playerbots_guild_names.sql;
