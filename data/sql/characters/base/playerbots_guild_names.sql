DROP TABLE IF EXISTS `playerbots_guild_names`;
CREATE TABLE `playerbots_guild_names` (
  `name_id`             INT(11)     NOT NULL AUTO_INCREMENT UNIQUE,
  `name`                VARCHAR(24) NOT NULL UNIQUE,
  `theme_slug`          VARCHAR(64) NOT NULL DEFAULT '',
  `faction`             TINYINT     NOT NULL DEFAULT 2,
  `class_mask`          INT         NOT NULL DEFAULT 0,
  `race_mask`           INT         NOT NULL DEFAULT 0,
  `affinity_class_mask` INT         NOT NULL DEFAULT 0,
  `affinity_race_mask`  INT         NOT NULL DEFAULT 0,
  `min_level`           TINYINT     NOT NULL DEFAULT 1,
  `target_size`         SMALLINT    NOT NULL DEFAULT 15,
  `tabard_emblem_style` SMALLINT    NULL,
  `tabard_emblem_color` SMALLINT    NULL,
  `tabard_border_style` SMALLINT    NULL,
  `tabard_border_color` SMALLINT    NULL,
  `tabard_bg_color`     SMALLINT    NULL,
  PRIMARY KEY (`name_id`),
  INDEX `idx_pgn_theme_slug` (`theme_slug`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Playerbot themed guild seeds';

-- 30 themed guilds derived from wow_guilds.json (20 from original RAG corpus + 10 added 2026-05-29).
-- Bitmask convention: see docs/superpowers/specs/2026-05-29-themed-bot-guilds-design.md §4.1.
-- target_size is the per-guild bot-fill cap; total ~1195 across 30 guilds targets
-- ~60% of a ~2000-character population guilded.
-- Tabard values are best-effort guesses based on lore; tune via in-game preview if needed.
INSERT INTO `playerbots_guild_names`
  (name_id, name, theme_slug, faction, class_mask, race_mask, affinity_class_mask, affinity_race_mask, min_level, target_size, tabard_emblem_style, tabard_emblem_color, tabard_border_style, tabard_border_color, tabard_bg_color)
VALUES
  -- Horde (original 9)
  (NULL, "Sons of Gul'dan",          'wow_guild_sons_of_guldan',           1, 0x100, 0x002, 0x020, 0x010 | 0x020, 30, 25, 53,  11, 4, 10, 21),
  (NULL, 'Acherus Vanguard',         'wow_guild_acherus_vanguard',         1, 0x020, 0x020, 0,     0x002 | 0x080 | 0x010, 55, 50, 100, 0, 5, 0,  21),
  (NULL, 'Darkspear Shadowstalkers', 'wow_guild_darkspear_shadowstalkers', 1, 0x008, 0x080, 0x100, 0,             25, 30, 12,  10, 1, 11, 7),
  (NULL, 'The Deathstalker Cabal',   'wow_guild_deathstalker_cabal',       1, 0x100, 0x010, 0x008, 0,             35, 30, 53,  0,  3, 0,  21),
  (NULL, 'Magisters of Silvermoon',  'wow_guild_magisters_silvermoon',     1, 0x080, 0x200, 0x002, 0,             40, 40, 47,  7,  4, 14, 14),
  (NULL, "Sin'dorei Blood Knights",  'wow_guild_sindorei_blood_knights',   1, 0x002, 0x200, 0x080, 0,             40, 45, 65,  11, 3, 14, 14),
  (NULL, 'Burning Blade Coven',      'wow_guild_burning_blade_coven',      1, 0x100, 0x002, 0,     0x020,         30, 25, 53,  10, 2, 21, 21),
  (NULL, 'Runetotem Earthwalkers',   'wow_guild_runetotem_earthwalkers',   1, 0x400, 0x020, 0,     0x200,         30, 35, 30,  12, 2, 9,  3),
  (NULL, 'Bloodhoof Warband',        'wow_guild_bloodhoof_warband',        1, 0x001, 0x020, 0x002, 0x002,         25, 55, 20,  10, 1, 11, 11),

  -- Horde (new 2)
  (NULL, 'Frostwolf Honor Guard',    'wow_guild_frostwolf_honor_guard',    1, 0x001, 0x002, 0x040, 0x020,         50, 55, 22,  0,  1, 0,  21),
  (NULL, "Spiritbearers of Sen'jin", 'wow_guild_spiritbearers_senjin',     1, 0x040 | 0x010, 0x080, 0, 0x002,     30, 30, 14,  0,  2, 3,  3),

  -- Alliance (original 8)
  (NULL, 'Stormwind Vanguard',       'wow_guild_stormwind_vanguard',       0, 0x001, 0x001, 0x002 | 0x010, 0x004 | 0x040, 30, 65, 65, 7, 3, 11, 14),
  (NULL, 'Ashenvale Sentinels',      'wow_guild_ashenvale_sentinels',      0, 0x001, 0x008, 0x004 | 0x008, 0,     20, 35, 30,  9,  2, 9,  4),
  (NULL, 'Knights of the Ebon Hold', 'wow_guild_knights_ebon_hold',        0, 0x020, 0x004, 0,     0x001 | 0x008 | 0x040, 55, 55, 100, 0, 5, 0, 21),
  (NULL, 'Stalkers of Teldrassil',   'wow_guild_stalkers_teldrassil',      0, 0x004, 0x008, 0x010, 0,             15, 30, 31,  7,  2, 9,  4),
  (NULL, 'Daughters of Cenarius',    'wow_guild_daughters_cenarius',       0, 0x400, 0x008, 0,     0,             30, 25, 30,  12, 2, 11, 4),
  (NULL, 'Aldor Lightbearers',       'wow_guild_aldor_lightbearers',       0, 0x010, 0x400, 0x002, 0,             40, 35, 27,  11, 3, 14, 14),
  (NULL, 'Acolytes of Acherus',      'wow_guild_acolytes_acherus',         0, 0x020, 0x004, 0,     0x001 | 0x008 | 0x040, 55, 35, 99, 0, 5, 0,  21),
  (NULL, 'The Emerald Wardens',      'wow_guild_emerald_wardens',          0, 0x400, 0x008, 0,     0,             50, 20, 32,  9,  2, 9,  4),

  -- Alliance (new 5)
  (NULL, 'Stormpike Wildcats',       'wow_guild_stormpike_wildcats',       0, 0x004, 0x004, 0x001, 0x001 | 0x040, 40, 40, 18, 7,  1, 11, 14),
  (NULL, 'SI:7 Black Daggers',       'wow_guild_si7_black_daggers',        0, 0x008, 0x001, 0,     0x008 | 0x040, 30, 35, 12, 0,  2, 0,  6),
  (NULL, 'Argent Lance',             'wow_guild_argent_lance',             0, 0x002, 0x001 | 0x004, 0x010, 0x400, 50, 55, 65, 11, 0, 11, 14),
  (NULL, 'Ironforge Mountainshields','wow_guild_ironforge_mountainshields',0, 0x001 | 0x002, 0x004, 0, 0x040,     30, 45, 20, 8,  1, 6,  6),
  (NULL, 'Reclaimers of Gnomeregan', 'wow_guild_reclaimers_gnomeregan',    0, 0,     0x040, 0,     0x004,         20, 35, 38, 8,  3, 6,  6),

  -- Neutral (original 3)
  (NULL, 'The Violet Eye',           'wow_guild_violet_eye',               2, 0x100, 0,     0x080, 0,             40, 20, 50,  6,  3, 6,  6),
  (NULL, 'Argent Vigil',             'wow_guild_argent_vigil',             2, 0x002, 0,     0x010, 0,             50, 65, 65,  11, 0, 11, 14),
  (NULL, 'Keepers of Moonglade',     'wow_guild_keepers_moonglade',        2, 0x400, 0x008, 0,     0x020,         35, 25, 30,  12, 2, 11, 4),

  -- Neutral (new 3)
  (NULL, 'Earthen Ring Conclave',    'wow_guild_earthen_ring_conclave',    2, 0x040, 0x020 | 0x400, 0, 0x002 | 0x080, 40, 35, 25, 12, 3, 8, 3),
  (NULL, 'Kirin Tor Magistrate',     'wow_guild_kirin_tor_magistrate',     2, 0x080, 0,     0x010, 0,             60, 40, 50,  6,  3, 0,  6),
  (NULL, 'The Stalwart Vigil',       'wow_guild_stalwart_vigil',           2, 0,     0,     0,     0,             80, 50, 89,  0,  0, 0,  21);
