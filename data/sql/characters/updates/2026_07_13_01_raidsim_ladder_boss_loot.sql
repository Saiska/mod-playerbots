-- RaidSim classic+TBC ladder: summoned/scripted-boss loot curation (companion to
-- 2026_07_13_00_raidsim_classic_tbc_ladder.sql). These 15 bosses have ZERO static `creature`
-- spawns on their map (verified live 192.168.1.41, pbtest_world, 2026-07-13 -- see
-- docs/reports/raidsim_classic_tbc_ladder_curation_2026-07-13.md §4), so RaidSim's map-keyed
-- spawn-join pool can never see their loot without this direct (map_id,difficulty,creature_entry)
-- mapping. Mirrors 2026_06_15_00_raid_sim_boss_loot.sql's pattern; that seed already owns map 409's
-- Ragnaros row (409,0,11502) -- NOT touched here (this file's DELETE is scoped to only its own 15
-- new (map_id,difficulty,creature_entry) triplets, never a bare map_id, so the existing MC pool is
-- never at risk of regression).
--
-- EXCLUDED by design: Majordomo Executus (409, 0, 12018) -- vanilla-correct creature_template.lootid
-- = 0 (he summons Ragnaros and despawns, never drops loot himself). Curating him would be a pure
-- no-op (BuildBossPoolQuery resolves nothing off a lootid=0 entry) and dead weight in the table, so
-- per the curation manifest's recommendation he is intentionally left out.
--
-- Also excluded (already curated elsewhere -- see manifest §5, no action owed by this lane):
-- EoE/Malygos (616) -- lootid=0 on both variants by design, reward already resolves via the
-- existing playerbots_raid_chest_loot "Alexstrasza's Gift" chest rows; ToC (649) -- full 8-boss
-- roster already seeded 2026-06-15.
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Idempotent: DELETE own rows (exact triplets only) then INSERT.

DELETE FROM playerbots_raid_boss_loot WHERE (map_id, difficulty, creature_entry) IN (
  (309, 0, 15114), (309, 0, 14515),
  (469, 0, 11583),
  (531, 0, 15517),
  (532, 0, 16152),
  (534, 0, 17767), (534, 0, 17808), (534, 0, 17888), (534, 0, 17842),
  (548, 0, 21217),
  (564, 0, 23420),
  (580, 0, 24892), (580, 0, 25038), (580, 0, 25840), (580, 0, 25315)
);
INSERT INTO playerbots_raid_boss_loot (map_id, difficulty, creature_entry) VALUES
  -- Zul'Gurub (309): bonus/summoned bosses, no static spawns.
  (309, 0, 15114), -- Gahz'ranka (fish-summon trigger)
  (309, 0, 14515), -- High Priestess Arlokk (teleports/summoned at event trigger)
  -- Blackwing Lair (469): summoned after the Razorgore/Vael/Broodlord/Chromaggus chain.
  (469, 0, 11583), -- Nefarian
  -- Temple of Ahn'Qiraj (531): optional/bonus boss, burrows in.
  (531, 0, 15517), -- Ouro
  -- Karazhan (532): merges with "Midnight" mid-fight; the merged-form entry has 0 static spawns.
  (532, 0, 16152), -- Attumen the Huntsman
  -- Battle of Mount Hyjal (534): wave bosses 1-4 of 5 (wave 5, Archimonde, already has a static spawn).
  (534, 0, 17767), -- Rage Winterchill
  (534, 0, 17808), -- Anetheron
  (534, 0, 17888), -- Kaz'rogal
  (534, 0, 17842), -- Azgalor
  -- Serpentshrine Cavern (548): summoned from the lake, no ground spawn.
  (548, 0, 21217), -- The Lurker Below
  -- Black Temple (564): 3-phase encounter (Suffering->Desire->Anger); loot lives on the final-phase entry.
  (564, 0, 23420), -- Essence of Anger ("Reliquary of Souls" final phase)
  -- Sunwell Plateau (580): all four remaining bosses are summoned/scripted, no ground spawns.
  (580, 0, 24892), -- Sathrovarr the Corruptor (Kalecgos encounter, demon impostor credit-entry)
  (580, 0, 25038), -- Felmyst (flies in)
  (580, 0, 25840), -- Entropius (M'uru 2nd/hard-mode phase; drives the instance cap to 256 via the trinket reference group)
  (580, 0, 25315); -- Kil'jaeden
