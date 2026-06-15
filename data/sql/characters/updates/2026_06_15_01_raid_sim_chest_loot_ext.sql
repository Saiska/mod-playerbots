-- raid-sim chest-loot EXTENSION: summoned caches the original seed (616/649/650) didn't cover.
-- Ulduar keeper caches (Cache of Storms/Innovation, Freya's Gift, Rare Cache of Winter) are
-- SummonGameObject-spawned (not in static `gameobject`), so they need explicit (map,diff)->GO rows
-- exactly like the EoE/ToC caches. Cache of the Firelord is Majordomo's MC chest (which is why the
-- Majordomo creature itself has 0 loot). Module: mod-playerbots. DB: acore_characters.
-- DELETE scoped to THIS file's maps (409,603) only -- must not touch 616/649/650.

DELETE FROM playerbots_raid_chest_loot WHERE map_id IN (409, 603);
INSERT INTO playerbots_raid_chest_loot (map_id, difficulty, gameobject_entry) VALUES
  -- Molten Core (409): Cache of the Firelord (Majordomo Executus' chest).
  (409, 0, 179703),
  -- Ulduar (603) 10N: Thorim / Mimiron / Freya / Hodir keeper caches.
  (603, 0, 194313),
  (603, 0, 194957),
  (603, 0, 194324),
  (603, 0, 194200),
  -- Ulduar (603) 25N: same four caches, 25-man GO entries.
  (603, 1, 194315),
  (603, 1, 194958),
  (603, 1, 194325),
  (603, 1, 194201);
