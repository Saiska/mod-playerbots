#!/usr/bin/env python3
"""
Import missing travel-node graph data from cmangos-playerbots into AC mod-playerbots.

Strategy:
  1. Match cmangos nodes to ours by (name, mapId) — closest by position wins
     when there are duplicates.
  2. Identify missing nodes (cmangos has, we don't).
  3. Assign new IDs starting at max(existing) + 1.
  4. Build cmangosId -> ourId remap covering matched + new.
  5. For links: insert only those involving at least one NEW node — preserves
     all our existing existing-to-existing relationships.
  6. For paths: same rule.

Output: SQL update files in data/sql/playerbots/updates/.
"""

import os
import re
import sys
from pathlib import Path

# Inputs
CMANGOS_SQL = Path(r"C:/Users/Admin/git/scratch/cmangos-playerbots/sql/world/wotlk/ai_playerbot_travel_nodes.sql")
MOD_DIR = Path(r"C:/Users/Admin/git/main/azerothcore-wotlk/modules/mod-playerbots")
OUR_NODE_SQL = MOD_DIR / "data/sql/playerbots/base/playerbots_travelnode.sql"
OUR_LINK_SQL = MOD_DIR / "data/sql/playerbots/base/playerbots_travelnode_link.sql"

# Output
UPDATES_DIR = MOD_DIR / "data/sql/playerbots/updates"
NODE_OUT = UPDATES_DIR / "2026_05_09_01_travelnode_cmangos_import.sql"
LINK_OUT = UPDATES_DIR / "2026_05_09_02_travelnode_link_cmangos_import.sql"
PATH_OUT = UPDATES_DIR / "2026_05_09_03_travelnode_path_cmangos_import.sql"

NODE_RE = re.compile(
    r"\((\d+),\s*'([^']*)',\s*(\d+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),\s*(\d+)\)"
)
LINK_RE = re.compile(
    r"\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\)"
)
PATH_RE = re.compile(
    r"\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+),\s*(-?[\d.eE+-]+)\)"
)


def parse_nodes(path: Path):
    """Parse all rows from a `*_travelnode` table-style SQL file. Returns dict id -> (name, mapId, x, y, z, linked)."""
    out = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    # Skip CREATE TABLE blocks: only count rows after the INSERT line in the
    # node table. The link/path table rows have a different shape so they
    # won't match NODE_RE anyway, but constraining via `INSERT INTO ... travelnode`
    # boundary makes intent explicit.
    in_node_block = False
    for line in text.splitlines():
        if "INSERT INTO" in line and "travelnode" in line.lower() and "_link" not in line.lower() and "_path" not in line.lower():
            in_node_block = True
            continue
        if "INSERT INTO" in line and ("_link" in line.lower() or "_path" in line.lower()):
            in_node_block = False
            continue
        if not in_node_block:
            continue
        for m in NODE_RE.finditer(line):
            nid, name, mapId, x, y, z, linked = m.groups()
            out[int(nid)] = (name, int(mapId), float(x), float(y), float(z), int(linked))
    return out


def parse_links(path: Path):
    """Parse links into list[(node_id, to_node_id, type, object, distance, swim, extra, calc, mc0, mc1, mc2)]."""
    out = []
    text = path.read_text(encoding="utf-8", errors="replace")
    in_link_block = False
    for line in text.splitlines():
        if "INSERT INTO" in line and "travelnode_link" in line.lower():
            in_link_block = True
            continue
        if "INSERT INTO" in line and "travelnode_link" not in line.lower():
            in_link_block = False
            continue
        if "CREATE TABLE" in line:
            in_link_block = False
            continue
        if not in_link_block:
            continue
        for m in LINK_RE.finditer(line):
            g = m.groups()
            out.append((int(g[0]), int(g[1]), int(g[2]), int(g[3]),
                        float(g[4]), float(g[5]), float(g[6]),
                        int(g[7]), int(g[8]), int(g[9]), int(g[10])))
    return out


def parse_paths(path: Path):
    """Parse paths into list[(node_id, to_node_id, nr, map_id, x, y, z)]."""
    out = []
    text = path.read_text(encoding="utf-8", errors="replace")
    in_path_block = False
    for line in text.splitlines():
        if "INSERT INTO" in line and "travelnode_path" in line.lower():
            in_path_block = True
            continue
        if "INSERT INTO" in line and "travelnode_path" not in line.lower():
            in_path_block = False
            continue
        if "CREATE TABLE" in line:
            in_path_block = False
            continue
        if not in_path_block:
            continue
        for m in PATH_RE.finditer(line):
            g = m.groups()
            out.append((int(g[0]), int(g[1]), int(g[2]), int(g[3]),
                        float(g[4]), float(g[5]), float(g[6])))
    return out


def sq2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def build_remap(cmangos_nodes, our_nodes):
    """Build cmangosId -> ourId map.

    Returns (remap, missing_cmangos_ids, new_id_assignments).
    new_id_assignments is dict cmangosId -> assigned ourId for the missing ones.
    """
    # Bucket our nodes by (name, mapId) for fast lookup; multiple ours may
    # share the same name (different positions), so we keep a list.
    our_by_key = {}
    for oid, (name, mid, x, y, z, lk) in our_nodes.items():
        our_by_key.setdefault((name, mid), []).append((oid, x, y, z))

    remap = {}
    missing = []
    for cid, (name, mid, x, y, z, lk) in cmangos_nodes.items():
        candidates = our_by_key.get((name, mid))
        if candidates:
            # Closest by position
            best = min(candidates, key=lambda c: sq2((c[1], c[2], c[3]), (x, y, z)))
            remap[cid] = best[0]
        else:
            missing.append(cid)

    # Assign new IDs to missing
    max_our = max(our_nodes.keys()) if our_nodes else 0
    new_assign = {}
    next_id = max_our + 1
    for cid in missing:
        new_assign[cid] = next_id
        remap[cid] = next_id
        next_id += 1

    return remap, missing, new_assign


def write_node_sql(out_path, cmangos_nodes, missing_ids, new_assign):
    rows = []
    for cid in missing_ids:
        ourId = new_assign[cid]
        name, mid, x, y, z, lk = cmangos_nodes[cid]
        # Escape single quotes in name
        ename = name.replace("'", "''")
        rows.append(f"({ourId}, '{ename}', {mid}, {x:.4f}, {y:.4f}, {z:.4f}, {lk})")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("-- Imported from cmangos-playerbots ai_playerbot_travelnode (wotlk)\n")
        f.write(f"-- {len(rows)} new nodes (cmangos has them, we didn't)\n")
        f.write("-- Matched on (name, mapId) + closest position; remaining are unique to cmangos.\n\n")
        if not rows:
            f.write("-- (no new nodes to insert)\n")
            return
        f.write("INSERT INTO `playerbots_travelnode` (`id`, `name`, `map_id`, `x`, `y`, `z`, `linked`) VALUES\n")
        # Batch in chunks of 500 rows per INSERT for readability
        BATCH = 500
        i = 0
        while i < len(rows):
            chunk = rows[i:i + BATCH]
            f.write(",\n".join(chunk))
            if i + BATCH < len(rows):
                f.write(";\nINSERT INTO `playerbots_travelnode` (`id`, `name`, `map_id`, `x`, `y`, `z`, `linked`) VALUES\n")
            else:
                f.write(";\n")
            i += BATCH


def write_link_sql(out_path, cmangos_links, remap, new_ids_set, our_link_pairs):
    """Insert cmangos links involving at least one NEW node, mapped to our IDs.

    Skip links where both endpoints are existing-to-existing (we keep our own).
    """
    rows = []
    skipped_existing = 0
    skipped_unmapped = 0
    skipped_dup_with_ours = 0

    for c_from, c_to, t, obj, dist, swim, extra, calc, mc0, mc1, mc2 in cmangos_links:
        if c_from not in remap or c_to not in remap:
            skipped_unmapped += 1
            continue
        o_from = remap[c_from]
        o_to = remap[c_to]
        # Skip if both endpoints are existing — preserve our own link data.
        if o_from not in new_ids_set and o_to not in new_ids_set:
            skipped_existing += 1
            continue
        # If we already have this exact link (shouldn't happen since both new
        # IDs are fresh, but defensive), skip.
        if (o_from, o_to) in our_link_pairs:
            skipped_dup_with_ours += 1
            continue
        rows.append(
            f"({o_from}, {o_to}, {t}, {obj}, {dist:.4f}, {swim:.4f}, {extra:.4f}, {calc}, {mc0}, {mc1}, {mc2})"
        )

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("-- Imported from cmangos-playerbots ai_playerbot_travelnode_link (wotlk)\n")
        f.write(f"-- {len(rows)} new links involving new nodes (mapped cmangos IDs to our IDs)\n")
        f.write(f"-- skipped: {skipped_existing} existing-to-existing, "
                f"{skipped_unmapped} unmapped, {skipped_dup_with_ours} dup-with-ours\n\n")
        if not rows:
            f.write("-- (no new links to insert)\n")
            return
        f.write(
            "INSERT INTO `playerbots_travelnode_link` "
            "(`node_id`, `to_node_id`, `type`, `object`, `distance`, `swim_distance`, "
            "`extra_cost`, `calculated`, `max_creature_0`, `max_creature_1`, `max_creature_2`) VALUES\n"
        )
        BATCH = 500
        i = 0
        while i < len(rows):
            chunk = rows[i:i + BATCH]
            f.write(",\n".join(chunk))
            if i + BATCH < len(rows):
                f.write(
                    ";\nINSERT INTO `playerbots_travelnode_link` "
                    "(`node_id`, `to_node_id`, `type`, `object`, `distance`, `swim_distance`, "
                    "`extra_cost`, `calculated`, `max_creature_0`, `max_creature_1`, `max_creature_2`) VALUES\n"
                )
            else:
                f.write(";\n")
            i += BATCH


def write_path_sql(out_path, cmangos_paths, remap, new_ids_set, our_link_pairs):
    """Insert cmangos paths whose link involves at least one NEW node."""
    rows = []
    skipped_existing = 0
    skipped_unmapped = 0
    skipped_dup_with_ours = 0

    # Stream rows; the path list is millions of entries
    for c_from, c_to, nr, mid, x, y, z in cmangos_paths:
        if c_from not in remap or c_to not in remap:
            skipped_unmapped += 1
            continue
        o_from = remap[c_from]
        o_to = remap[c_to]
        if o_from not in new_ids_set and o_to not in new_ids_set:
            skipped_existing += 1
            continue
        if (o_from, o_to) in our_link_pairs:
            skipped_dup_with_ours += 1
            continue
        rows.append((o_from, o_to, nr, mid, x, y, z))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("-- Imported from cmangos-playerbots ai_playerbot_travelnode_path (wotlk)\n")
        f.write(f"-- {len(rows)} new path waypoints belonging to links involving new nodes\n")
        f.write(f"-- skipped: {skipped_existing} existing-to-existing, "
                f"{skipped_unmapped} unmapped, {skipped_dup_with_ours} dup-with-ours\n\n")
        if not rows:
            f.write("-- (no new path rows to insert)\n")
            return
        f.write(
            "INSERT INTO `playerbots_travelnode_path` "
            "(`node_id`, `to_node_id`, `nr`, `map_id`, `x`, `y`, `z`) VALUES\n"
        )
        BATCH = 1000
        i = 0
        while i < len(rows):
            chunk_strs = []
            for o_from, o_to, nr, mid, x, y, z in rows[i:i + BATCH]:
                chunk_strs.append(f"({o_from}, {o_to}, {nr}, {mid}, {x:.4f}, {y:.4f}, {z:.4f})")
            f.write(",\n".join(chunk_strs))
            if i + BATCH < len(rows):
                f.write(
                    ";\nINSERT INTO `playerbots_travelnode_path` "
                    "(`node_id`, `to_node_id`, `nr`, `map_id`, `x`, `y`, `z`) VALUES\n"
                )
            else:
                f.write(";\n")
            i += BATCH


def parse_our_link_pairs(path: Path):
    """Return set of (node_id, to_node_id) pairs we already have."""
    pairs = set()
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in LINK_RE.finditer(text):
        pairs.add((int(m.group(1)), int(m.group(2))))
    return pairs


def main():
    print("Parsing cmangos nodes...", flush=True)
    cmangos_nodes = parse_nodes(CMANGOS_SQL)
    print(f"  {len(cmangos_nodes)} nodes")

    print("Parsing our nodes...", flush=True)
    our_nodes = parse_nodes(OUR_NODE_SQL)
    print(f"  {len(our_nodes)} nodes")

    print("Building remap...", flush=True)
    remap, missing, new_assign = build_remap(cmangos_nodes, our_nodes)
    print(f"  matched: {len(remap) - len(missing)}, missing (new): {len(missing)}")

    new_ids_set = set(new_assign.values())

    print("Parsing cmangos links...", flush=True)
    cm_links = parse_links(CMANGOS_SQL)
    print(f"  {len(cm_links)} links")

    print("Parsing our existing link pairs...", flush=True)
    our_link_pairs = parse_our_link_pairs(OUR_LINK_SQL)
    print(f"  {len(our_link_pairs)} existing pairs")

    print("Parsing cmangos paths...", flush=True)
    cm_paths = parse_paths(CMANGOS_SQL)
    print(f"  {len(cm_paths)} path rows")

    UPDATES_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Writing {NODE_OUT.name}...", flush=True)
    write_node_sql(NODE_OUT, cmangos_nodes, missing, new_assign)

    print(f"Writing {LINK_OUT.name}...", flush=True)
    write_link_sql(LINK_OUT, cm_links, remap, new_ids_set, our_link_pairs)

    print(f"Writing {PATH_OUT.name}...", flush=True)
    write_path_sql(PATH_OUT, cm_paths, remap, new_ids_set, our_link_pairs)

    print("Done.")


if __name__ == "__main__":
    main()
