-- themed_guilds_retrofit.sql
-- One-time (re-applicable): stamp themed tabards + rank labels onto live guilds
-- whose name matches a row in playerbots_guild_names.
-- Prereq: dbupdater has applied 2026_05_29_00_themed_guilds_schema.sql (theme
--         columns present) and the base seed for playerbots_guild_names is the
--         themed 20-row version.
-- Safe to re-run.

USE acore_characters;

UPDATE guild g
  JOIN playerbots_guild_names n ON g.name = n.name
   SET g.EmblemStyle     = COALESCE(n.tabard_emblem_style, g.EmblemStyle),
       g.EmblemColor     = COALESCE(n.tabard_emblem_color, g.EmblemColor),
       g.BorderStyle     = COALESCE(n.tabard_border_style, g.BorderStyle),
       g.BorderColor     = COALESCE(n.tabard_border_color, g.BorderColor),
       g.BackgroundColor = COALESCE(n.tabard_bg_color,     g.BackgroundColor)
 WHERE n.theme_slug <> '';

UPDATE guild_rank gr
  JOIN guild g ON g.guildid = gr.guildid
  JOIN playerbots_guild_names n ON n.name = g.name
  JOIN playerbots_guild_rank_names rn ON rn.theme_slug = n.theme_slug AND rn.rid = gr.rid
   SET gr.rname = rn.rname;

-- Report rows touched.
SELECT 'Tabard rows updated:' AS phase, ROW_COUNT() AS rows;
