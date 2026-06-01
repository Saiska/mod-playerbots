-- raid-sim: re-include the three chest-loot raids now that BuildPool mines gameobject_loot_template
-- (616 Eye of Eternity, 649 Trial of the Crusader, 650 Trial of the Champion). Module: mod-playerbots.
-- DB: acore_characters. DELETE+INSERT (scoped to these maps) = re-applicable. Coords from
-- areatrigger_teleport (target_map IN (616,649,650), verified pbtest_world 2026-06-01).
-- 650 ilvl_cap = 219 (its chest loot is ilvl 219; the band-0 nominal 200 would yield an empty pool).

DELETE FROM playerbots_raid_tier_instance WHERE map_id IN (616, 649, 650);
INSERT INTO playerbots_raid_tier_instance
  (band, map_id, difficulty, group_size, gate_ilvl, ilvl_cap, min_quality, label,
   entry_x, entry_y, entry_z, entry_o, park_x, park_y, park_z, park_o) VALUES
  -- Eye of Eternity (Tier 7): 10N band 1, 25N band 2
  (1, 616, 0, 10, 200, 213, 4, 'Eye of Eternity (10)',
     728.055, 1329.03, 275, 5.51524, 728.055, 1329.03, 275, 5.51524),
  (2, 616, 1, 25, 213, 226, 4, 'Eye of Eternity (25)',
     728.055, 1329.03, 275, 5.51524, 728.055, 1329.03, 275, 5.51524),
  -- Trial of the Crusader (Tier 9): normal band 4, heroic band 5
  (4, 649, 0, 10, 232, 245, 4, 'Trial of the Crusader (10)',
     563.61, 80.6815, 395.2, 1.59, 563.61, 80.6815, 395.2, 1.59),
  (4, 649, 1, 25, 232, 245, 4, 'Trial of the Crusader (25)',
     563.61, 80.6815, 395.2, 1.59, 563.61, 80.6815, 395.2, 1.59),
  (5, 649, 2, 10, 245, 264, 4, 'Trial of the Crusader (10H)',
     563.61, 80.6815, 395.2, 1.59, 563.61, 80.6815, 395.2, 1.59),
  (5, 649, 3, 25, 245, 264, 4, 'Trial of the Crusader (25H)',
     563.61, 80.6815, 395.2, 1.59, 563.61, 80.6815, 395.2, 1.59),
  -- Trial of the Champion (heroic 5-man): band 0, cap raised to 219 (actual chest-loot ilvl)
  (0, 650, 1, 5, 0, 219, 3, 'Trial of the Champion (H)',
     805.227, 618.038, 412.393, 3.1456, 805.227, 618.038, 412.393, 3.1456);
