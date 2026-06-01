-- Server Population Dynamics — monotonic real-player level frontier state.
-- Module: mod-playerbots (Saiska fork). Database: acore_characters.
-- Auto-applied by AzerothCore dbupdater at worldserver boot. Re-applicable (IF NOT EXISTS + INSERT IGNORE).
-- Decoupled from raid-sim's playerbots_raid_server_state on purpose (no cross-feature schema dependency).

CREATE TABLE IF NOT EXISTS playerbots_population_state (
  id               TINYINT NOT NULL PRIMARY KEY DEFAULT 1,
  max_player_level TINYINT NOT NULL DEFAULT 0    -- monotonic: highest level ever reached by a real (non-bot) player
) ENGINE=MyISAM DEFAULT CHARSET=utf8 COMMENT='Population-dynamics monotonic real-player level frontier';

INSERT IGNORE INTO playerbots_population_state (id, max_player_level) VALUES (1, 0);
