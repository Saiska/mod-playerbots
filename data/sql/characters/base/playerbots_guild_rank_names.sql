DROP TABLE IF EXISTS `playerbots_guild_rank_names`;
CREATE TABLE `playerbots_guild_rank_names` (
  `theme_slug` VARCHAR(64) NOT NULL,
  `rid`        TINYINT     NOT NULL,
  `rname`      VARCHAR(20) NOT NULL,
  PRIMARY KEY (`theme_slug`, `rid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Themed guild rank labels';

INSERT INTO `playerbots_guild_rank_names` (theme_slug, rid, rname) VALUES
  -- Acherus Vanguard
  ('wow_guild_acherus_vanguard', 0, 'Highlord'),
  ('wow_guild_acherus_vanguard', 1, 'Cavalier'),
  ('wow_guild_acherus_vanguard', 2, 'Rider'),
  ('wow_guild_acherus_vanguard', 3, 'Death Knight'),
  ('wow_guild_acherus_vanguard', 4, 'Initiate'),

  -- Sons of Gul'dan
  ('wow_guild_sons_of_guldan', 0, 'Shadow Lord'),
  ('wow_guild_sons_of_guldan', 1, 'Cabal Master'),
  ('wow_guild_sons_of_guldan', 2, 'Fel-bound'),
  ('wow_guild_sons_of_guldan', 3, 'Acolyte'),
  ('wow_guild_sons_of_guldan', 4, 'Initiate'),

  -- Bloodhoof Warband
  ('wow_guild_bloodhoof_warband', 0, 'Chieftain'),
  ('wow_guild_bloodhoof_warband', 1, 'Warband Captain'),
  ('wow_guild_bloodhoof_warband', 2, 'Hornblade'),
  ('wow_guild_bloodhoof_warband', 3, 'Warrior'),
  ('wow_guild_bloodhoof_warband', 4, 'Initiate'),

  -- Daughters of Cenarius
  ('wow_guild_daughters_cenarius', 0, 'Eldest'),
  ('wow_guild_daughters_cenarius', 1, 'Druidess'),
  ('wow_guild_daughters_cenarius', 2, 'Barkkeeper'),
  ('wow_guild_daughters_cenarius', 3, 'Daughter'),
  ('wow_guild_daughters_cenarius', 4, 'Acolyte'),

  -- Argent Vigil
  ('wow_guild_argent_vigil', 0, 'Highlord'),
  ('wow_guild_argent_vigil', 1, 'Crusader'),
  ('wow_guild_argent_vigil', 2, 'Templar'),
  ('wow_guild_argent_vigil', 3, 'Vigilant'),
  ('wow_guild_argent_vigil', 4, 'Pledged'),

  -- Stormwind Vanguard
  ('wow_guild_stormwind_vanguard', 0, 'Marshal'),
  ('wow_guild_stormwind_vanguard', 1, 'Captain'),
  ('wow_guild_stormwind_vanguard', 2, 'Sergeant'),
  ('wow_guild_stormwind_vanguard', 3, 'Soldier'),
  ('wow_guild_stormwind_vanguard', 4, 'Recruit');
