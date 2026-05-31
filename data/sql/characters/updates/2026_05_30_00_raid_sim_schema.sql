-- Autonomous Instance Simulation — schema (banded instance ladder, server state, guild columns).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by AzerothCore dbupdater at worldserver boot.

CREATE TABLE IF NOT EXISTS playerbots_raid_tier_instance (
  id          INT      NOT NULL AUTO_INCREMENT PRIMARY KEY,
  band        TINYINT  NOT NULL,                -- ilvl band (ladder position); many instances per band
  map_id      SMALLINT NOT NULL,
  difficulty  TINYINT  NOT NULL DEFAULT 0,      -- AC Difficulty: 5-man 0/1; raid 0=10N 1=25N 2=10H 3=25H
  group_size  TINYINT  NOT NULL DEFAULT 5,      -- 5 dungeon, 10/25 raid (explicit; diff index is ambiguous)
  gate_ilvl   SMALLINT NOT NULL,                -- base_ilvl at/above which this band unlocks (band-uniform)
  ilvl_cap    SMALLINT NOT NULL,                -- loot pool ceiling (trims shared-reference bleed)
  min_quality TINYINT  NOT NULL DEFAULT 4,      -- 3 dungeons, 4 raids
  label       VARCHAR(48) NOT NULL DEFAULT '',
  entry_x FLOAT NOT NULL, entry_y FLOAT NOT NULL, entry_z FLOAT NOT NULL, entry_o FLOAT NOT NULL,
  park_x  FLOAT NOT NULL, park_y  FLOAT NOT NULL, park_z  FLOAT NOT NULL, park_o  FLOAT NOT NULL,
  INDEX idx_band (band)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Raid-sim banded (instance,difficulty) ladder';

CREATE TABLE IF NOT EXISTS playerbots_raid_server_state (
  id        TINYINT  NOT NULL PRIMARY KEY DEFAULT 1,
  base_ilvl SMALLINT NOT NULL DEFAULT 0
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Raid-sim monotonic server base ilvl';
INSERT IGNORE INTO playerbots_raid_server_state (id, base_ilvl) VALUES (1, 0);

ALTER TABLE playerbots_guild_names
  ADD COLUMN raid_offset TINYINT NOT NULL DEFAULT -1,   -- <0 = not a sim guild; else bands behind frontier
  ADD COLUMN max_band    TINYINT NOT NULL DEFAULT 127;  -- ceiling: keep a casual guild on low bands
