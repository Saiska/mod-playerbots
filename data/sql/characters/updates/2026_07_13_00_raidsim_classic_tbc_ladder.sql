-- RaidSim classic+TBC ladder (spec 2026-07-13-raidsim-classic-tbc-ladder-design.md).
-- Adds the 14 classic (Zul'Gurub/AQ20/MC/BWL/AQ40) and TBC (Kara/Gruul/Mag/SSC/TK/ZA/Hyjal/BT/SWP)
-- raid instances to the band-1 ladder (playerbots_raid_tier_instance was previously WotLK-only,
-- band 1 = Naxx/OS/VoA — see 2026_05_30_01_raid_sim_ladder_seed.sql). Verified live (192.168.1.41,
-- pbtest_characters, 2026-07-13): these 14 (map_id, difficulty=0) pairs had ZERO pre-existing rows.
-- All: band=1, difficulty=0, min_quality=4. gate_ilvl/group_size/ilvl_cap/coords are the MEASURED
-- values from docs/reports/raidsim_classic_tbc_ladder_curation_2026-07-13.md §1-2 (ilvl_cap =
-- MAX(static-spawn Q4 max, curated-boss Q4 max) per map -- not static-only; see manifest §2 for
-- why). Coords = areatrigger_teleport entrance foyer; park = entry (RaidSim parks at the entrance).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Idempotent: DELETE own rows (keyed on these 14 map_ids at difficulty=0) then INSERT.

DELETE FROM playerbots_raid_tier_instance WHERE map_id IN (309,409,469,509,531,532,534,544,548,550,564,565,568,580) AND difficulty = 0;
INSERT INTO playerbots_raid_tier_instance
  (band, map_id, difficulty, group_size, gate_ilvl, ilvl_cap, min_quality, label,
   entry_x, entry_y, entry_z, entry_o, park_x, park_y, park_z, park_o) VALUES
(1, 309, 0, 10, 200, 207, 4, 'Zul''Gurub (10)',             -11916.1,  -1230.53,   92.5334, 4.71867, -11916.1,  -1230.53,   92.5334, 4.71867),
(1, 509, 0, 10, 200, 207, 4, 'Ruins of Ahn''Qiraj (10)',     -8429.74,  1512.14,   31.9074, 2.58,     -8429.74,  1512.14,   31.9074, 2.58),
(1, 409, 0, 25, 200, 213, 4, 'Molten Core (25)',              1091.89,  -466.985, -105.084, 3.14159,   1091.89,  -466.985, -105.084, 3.14159),
(1, 469, 0, 25, 207, 226, 4, 'Blackwing Lair (25)',          -7673.03,  -1106.08,  396.651, 0.703353, -7673.03,  -1106.08,  396.651, 0.703353),
(1, 531, 0, 25, 207, 222, 4, 'Temple of Ahn''Qiraj (25)',    -8231.33,  2010.6,    129.331, 0.929912, -8231.33,  2010.6,    129.331, 0.929912),
(1, 532, 0, 10, 207, 225, 4, 'Karazhan (10)',                -11100,   -2003.98,   49.8927, 0.577268, -11100,   -2003.98,   49.8927, 0.577268),
(1, 565, 0, 25, 207, 229, 4, 'Gruul''s Lair (25)',               62.7842,   35.462,   -3.9835, 1.41844,    62.7842,   35.462,   -3.9835, 1.41844),
(1, 544, 0, 25, 207, 232, 4, 'Magtheridon''s Lair (25)',        187.843,   35.9232,   67.9252, 4.79879,   187.843,   35.9232,   67.9252, 4.79879),
(1, 548, 0, 25, 213, 236, 4, 'Serpentshrine Cavern (25)',         2.5343,  -0.022318, 821.727, 0.004512,    2.5343,  -0.022318, 821.727, 0.004512),
(1, 550, 0, 25, 213, 240, 4, 'Tempest Keep (25)',              -10.8021,  -1.15045,  -2.42833, 6.22821,   -10.8021,  -1.15045,  -2.42833, 6.22821),
(1, 568, 0, 10, 213, 243, 4, 'Zul''Aman (10)',                  120.7,  1776,        43.46,   4.7713,    120.7,  1776,        43.46,   4.7713),
(1, 534, 0, 25, 219, 232, 4, 'Battle of Mount Hyjal (25)',     4259.61,  -4233.77,  868.199, 2.53,      4259.61,  -4233.77,  868.199, 2.53),
(1, 564, 0, 25, 219, 251, 4, 'Black Temple (25)',                96.4462, 1002.35,  -86.9984, 6.15675,    96.4462, 1002.35,  -86.9984, 6.15675),
(1, 580, 0, 25, 226, 256, 4, 'Sunwell Plateau (25)',           1790.65,   925.67,    15.15,   3.1,      1790.65,   925.67,    15.15,   3.1);
