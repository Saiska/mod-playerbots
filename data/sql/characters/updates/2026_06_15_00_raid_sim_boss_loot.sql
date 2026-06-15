-- raid-sim boss-loot mapping: instance (map,difficulty) -> boss creature entries whose loot
-- should be resolved DIRECTLY (not via the spawn join). This captures summoned/scripted bosses
-- (Ragnaros, Yogg-Saron, the entire Trial of the Crusader roster) that have 0 rows in the static
-- `creature` table, so RaidSim's map-keyed creature pool can never see them. Mirrors the sibling
-- table playerbots_raid_chest_loot. Module: mod-playerbots. DB: acore_characters.
-- DELETE+INSERT = re-applicable.

CREATE TABLE IF NOT EXISTS playerbots_raid_boss_loot (
  map_id         INT UNSIGNED     NOT NULL,
  difficulty     TINYINT UNSIGNED NOT NULL,
  creature_entry INT UNSIGNED     NOT NULL,
  PRIMARY KEY (map_id, difficulty, creature_entry)
);

DELETE FROM playerbots_raid_boss_loot WHERE map_id IN (409, 603, 649);
INSERT INTO playerbots_raid_boss_loot (map_id, difficulty, creature_entry) VALUES
  -- Molten Core (409): Ragnaros is summoned by Majordomo -> 0 static spawns.
  (409, 0, 11502),
  -- Ulduar (603) 10N / 25N: Yogg-Saron is summoned -> 0 static spawns (14-item loot table).
  (603, 0, 33288),
  (603, 1, 33288),
  -- Trial of the Crusader (649) 10N/25N/10H/25H: the entire roster is summoned.
  (649, 0, 34796), (649, 0, 35144), (649, 0, 34799), (649, 0, 34797),
  (649, 0, 34780), (649, 0, 34496), (649, 0, 34497), (649, 0, 34564),
  (649, 1, 34796), (649, 1, 35144), (649, 1, 34799), (649, 1, 34797),
  (649, 1, 34780), (649, 1, 34496), (649, 1, 34497), (649, 1, 34564),
  (649, 2, 34796), (649, 2, 35144), (649, 2, 34799), (649, 2, 34797),
  (649, 2, 34780), (649, 2, 34496), (649, 2, 34497), (649, 2, 34564),
  (649, 3, 34796), (649, 3, 35144), (649, 3, 34799), (649, 3, 34797),
  (649, 3, 34780), (649, 3, 34496), (649, 3, 34497), (649, 3, 34564);
