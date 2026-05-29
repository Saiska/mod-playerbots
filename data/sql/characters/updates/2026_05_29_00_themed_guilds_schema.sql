-- Adds theme columns to playerbots_guild_names and creates the rank-names table.
-- Module: mod-playerbots (Saiska fork — themed guilds)
-- Database: acore_characters
-- Auto-applied by AzerothCore dbupdater at worldserver boot.

ALTER TABLE playerbots_guild_names
  ADD COLUMN theme_slug          VARCHAR(64) NOT NULL DEFAULT '' AFTER name,
  ADD COLUMN faction             TINYINT     NOT NULL DEFAULT 2,
  ADD COLUMN class_mask          INT         NOT NULL DEFAULT 0,
  ADD COLUMN race_mask           INT         NOT NULL DEFAULT 0,
  ADD COLUMN affinity_class_mask INT         NOT NULL DEFAULT 0,
  ADD COLUMN affinity_race_mask  INT         NOT NULL DEFAULT 0,
  ADD COLUMN min_level           TINYINT     NOT NULL DEFAULT 1,
  ADD COLUMN target_size         SMALLINT    NOT NULL DEFAULT 15,
  ADD COLUMN tabard_emblem_style SMALLINT    NULL,
  ADD COLUMN tabard_emblem_color SMALLINT    NULL,
  ADD COLUMN tabard_border_style SMALLINT    NULL,
  ADD COLUMN tabard_border_color SMALLINT    NULL,
  ADD COLUMN tabard_bg_color     SMALLINT    NULL,
  ADD INDEX idx_pgn_theme_slug (theme_slug);

CREATE TABLE playerbots_guild_rank_names (
  theme_slug VARCHAR(64) NOT NULL,
  rid        TINYINT     NOT NULL,
  rname      VARCHAR(20) NOT NULL,
  PRIMARY KEY (theme_slug, rid)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Themed guild rank labels';
