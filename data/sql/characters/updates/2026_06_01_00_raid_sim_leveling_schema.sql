-- Raid-Sim Leveling Content — schema (sub-80 5-man dungeon ladder, level-gated).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by AzerothCore dbupdater at worldserver boot.

CREATE TABLE IF NOT EXISTS playerbots_leveling_instance (
  id          INT      NOT NULL AUTO_INCREMENT PRIMARY KEY,
  map_id      SMALLINT NOT NULL,
  difficulty  TINYINT  NOT NULL DEFAULT 0,      -- normal; leveling dungeons have no heroic split here
  level_lo    TINYINT  NOT NULL,                -- inclusive lower bound of the cohort window
  level_hi    TINYINT  NOT NULL,                -- inclusive upper bound
  ilvl_cap    SMALLINT NOT NULL,                -- loot pool ceiling (trims cross-content bleed)
  min_quality TINYINT  NOT NULL DEFAULT 2,      -- uncommon+ (rares scarce at low level)
  label       VARCHAR(48) NOT NULL DEFAULT '',
  entry_x FLOAT NOT NULL, entry_y FLOAT NOT NULL, entry_z FLOAT NOT NULL, entry_o FLOAT NOT NULL,
  park_x  FLOAT NOT NULL, park_y  FLOAT NOT NULL, park_z  FLOAT NOT NULL, park_o  FLOAT NOT NULL,
  INDEX idx_level_hi (level_hi)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Raid-sim leveling 5-man dungeon ladder (level-gated)';
