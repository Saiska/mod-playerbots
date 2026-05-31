-- Autonomous Instance Simulation — per-guild offsets/ceilings (operator-tuned).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Re-applicable (fixed-value UPDATEs). Edit per install; matches themed_guilds_retrofit.sql.
-- raid_offset: bands behind the frontier (0 = frontier; <0 = NOT a sim guild). max_band: hard ceiling (band index 0-7).
--
-- Tailored to THIS install's themed-guild slugs (the seeded slugs are 'wow_guild_*', not the
-- generic plan placeholders). Any guild left untouched keeps raid_offset = -1 (not a sim guild).
-- To enrol another guild, add a line with its theme_slug (see playerbots_guild_names.theme_slug).

-- Frontier raiding guild (reaches whatever the lead real player has unlocked):
UPDATE playerbots_guild_names SET raid_offset = 0, max_band = 7 WHERE theme_slug = 'wow_guild_stalwart_vigil';
-- A second progression guild kept one band behind the frontier:
UPDATE playerbots_guild_names SET raid_offset = 1, max_band = 7 WHERE theme_slug = 'wow_guild_stormwind_vanguard';
-- Casual guild: capped at heroic-dungeon / entry-raid bands, never walks into 25H:
UPDATE playerbots_guild_names SET raid_offset = 2, max_band = 1 WHERE theme_slug = 'wow_guild_argent_lance';
