-- raid-sim chest-loot mapping: instance (map,difficulty) -> summoned cache GO entries.
-- These caches are SummonGameObject-spawned (not in the static `gameobject` table), so they
-- cannot be joined by map; the mapping is explicit. Module: mod-playerbots. DB: acore_characters.
-- DELETE+INSERT = re-applicable.

CREATE TABLE IF NOT EXISTS playerbots_raid_chest_loot (
  map_id           INT UNSIGNED     NOT NULL,
  difficulty       TINYINT UNSIGNED NOT NULL,
  gameobject_entry INT UNSIGNED     NOT NULL,
  PRIMARY KEY (map_id, difficulty, gameobject_entry)
);

DELETE FROM playerbots_raid_chest_loot WHERE map_id IN (616, 649, 650);
INSERT INTO playerbots_raid_chest_loot (map_id, difficulty, gameobject_entry) VALUES
  -- Eye of Eternity (616): Alexstrasza's Gift, 10N / 25N
  (616, 0, 193905),
  (616, 1, 193967),
  -- Trial of the Crusader (649): Crusaders' Cache, 10N / 25N / 10H / 25H
  (649, 0, 195631),
  (649, 1, 195632),
  (649, 2, 195633),
  (649, 3, 195635),
  -- Trial of the Champion (650, 5-man): heroic loot chests
  (650, 1, 195710),
  (650, 1, 195375),
  (650, 1, 195324);
