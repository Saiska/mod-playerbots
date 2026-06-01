# sql_operator/ — manual operator scripts (NOT auto-applied)

SQL here is part of this fork (a person cloning the branch gets it), but it is **not**
under `data/sql/<db>/`, so the AzerothCore dbupdater does **not** run it at boot.

Use this folder for scripts that:
- act on **live runtime tables** that are empty at first boot (e.g. `guild`, `guild_rank`,
  `characters`) and therefore cannot be a boot-time migration, **or**
- are one-time **retrofits** onto a specific install's existing data.

Run them by hand against the CharacterDatabase, e.g.:

```
Server/mysql/bin/mysql.exe -uacore -pacore -h127.0.0.1 -P3306 <characters_db> < sql_operator/<script>.sql
```

## Scripts

- **`themed_guilds_retrofit.sql`** — stamp themed tabards + rank labels onto live guilds
  whose `name` matches a `playerbots_guild_names` seed row. Only needed to retrofit guilds
  that existed **before** the themed-guild code (which applies tabards on guild creation).
  Safe to re-run. Prereq: the themed-guild updates have applied (seed + rank names present)
  and the realm has live guilds to update.
