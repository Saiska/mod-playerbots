-- Playerbot themed guild seeds — MINIMAL base skeleton (idempotent, non-destructive).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
--
-- IMPORTANT (lesson 2026-06-01): a base/ file must be a minimal, RE-APPLICABLE skeleton.
-- The theme columns, the 30-guild seed, and raid_offset/max_band all live in updates/
-- migrations (2026_05_29_00/01 themed-guilds, 2026_05_30_00 raid-sim). The previous
-- DROP TABLE + CREATE here duplicated that evolved schema, so on a FRESH install it
-- collided with the updates/ ALTERs ("Duplicate column 'theme_slug'") and, when this
-- base file's hash changed, its re-apply silently DROPPED the update-added raid_offset/
-- max_band columns ("Unknown column 'raid_offset'"). CREATE TABLE IF NOT EXISTS keeps
-- re-apply a no-op so it can never wipe update-owned columns.
--
-- Apply order (guaranteed by the dbupdater): base/ first (this skeleton), then updates/
-- in filename order add the theme columns + seed + raid-sim columns + per-guild offsets.

CREATE TABLE IF NOT EXISTS `playerbots_guild_names` (
  `name_id` INT(11)     NOT NULL AUTO_INCREMENT,
  `name`    VARCHAR(24) NOT NULL,
  PRIMARY KEY (`name_id`),
  UNIQUE KEY `uk_pgn_name` (`name`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Playerbot themed guild seeds (skeleton; evolved by updates/)';
