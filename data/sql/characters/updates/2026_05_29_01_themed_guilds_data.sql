-- Loads the 30 themed guilds into playerbots_guild_names and the 30 rank rows
-- into playerbots_guild_rank_names, replacing whatever was there.
--
-- Required so installs upgrading from upstream (which had the 400-row generic
-- name pool) end up with the same data state as a fresh install of this fork.
--
-- Idempotent: TRUNCATE+INSERT is safe to re-run.

TRUNCATE TABLE playerbots_guild_names;

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

TRUNCATE TABLE playerbots_guild_rank_names;

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
