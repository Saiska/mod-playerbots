This folder should contain only **re-applicable AND schema-independent** SQL.

⚠️ **Ordering trap (learned 2026-06-01):** the dbupdater applies `custom/` **before**
`updates/`. So `custom/` SQL must NOT depend on any column/table that an `updates/`
migration creates — on a fresh install it will run first and abort the boot
(e.g. `raid_guild_assignments.sql` set `raid_offset` before `updates/` added the column →
`Unknown column 'raid_offset'`). Anything that depends on `updates/` schema belongs in
`updates/` (with a filename that sorts after its schema), not here. Anything that touches
live runtime tables (guild, characters, …) belongs in `../../../sql_operator/` (manual).

Re-applicable forms only:

- CREATE TABLE IF NOT EXISTS
- REPLACE INTO
- DELETE + INSERT
- INSERT IGNORE / INSERT ... ON DUPLICATE KEY UPDATE
- UPDATEs with fixed values

etc.
