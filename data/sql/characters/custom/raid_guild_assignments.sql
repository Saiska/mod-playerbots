-- Autonomous Instance Simulation — per-guild offsets/ceilings (operator-tuned).
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Re-applicable (fixed-value UPDATEs). Edit per install; matches themed_guilds_retrofit.sql.
-- raid_offset: bands behind the frontier (0 = frontier). max_band: hard ceiling (band index).

-- Frontier raiding guild (reaches whatever the lead real player has unlocked):
UPDATE playerbots_guild_names SET raid_offset = 0, max_band = 7 WHERE theme_slug = 'mythic-vanguard';
-- Casual guild: capped at heroic-dungeon / entry-raid bands, never walks into 25H:
UPDATE playerbots_guild_names SET raid_offset = 2, max_band = 1 WHERE theme_slug = 'weekend-warriors';
