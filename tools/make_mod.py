#!/usr/bin/env python3
"""Scaffolds a content mod for this engine (MOD-002 helper).

    tools/make_mod.py my_mod --table common/laws.json
    tools/make_mod.py my_mod --depends base_mod --load-after other_mod --validate

Creates <root>/<name>/mod.json plus a starter file for every requested content table,
all of which already satisfy the loader (an empty array is valid content). --validate
runs the built game's `--validate-content --mods` on the mods root afterwards, so a
scaffold is proven loadable before it is edited.

The mod.json written here is the manifest documented in docs/MODDING.md and read by
src/data/mod.h: name, version, dependencies, load_after, replace_paths.
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# table path inside a mod -> the top-level JSON key it must hold. Mirrors the loader's
# known table list in src/data/mod.cpp; an unknown path is rejected by the loader.
TABLES = {
    "common/equipment.json": "equipment",
    "common/buildings.json": "buildings",
    "common/technologies.json": "technologies",
    "common/laws.json": "laws",
    "common/templates.json": "templates",
    "common/events.json": "events",
    "common/decisions.json": "decisions",
    "common/spirits.json": "spirits",
    "common/advisors.json": "advisors",
    "common/components.json": "components",
}
CONSTANTS = "common/constants.json"


def display(path):
    """Relative to the repo when that stays inside it, absolute otherwise."""
    rel = os.path.relpath(path, ROOT)
    return path if rel.startswith("..") else rel


def parse_list(value):
    if value is None:
        return []
    return [item.strip() for item in value.split(",") if item.strip()]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("name", help="mod name and directory name (letters, digits, _ and -)")
    parser.add_argument("--root", default=os.path.join(ROOT, "data", "mods"),
                        help="mods root; the mod is created at <root>/<name> (default data/mods)")
    parser.add_argument("--version", default="1.0.0", help="mod version string (default 1.0.0)")
    parser.add_argument("--depends", default=None,
                        help="comma-separated mod names that must load before this one")
    parser.add_argument("--load-after", default=None,
                        help="comma-separated mod names this one prefers to load after")
    parser.add_argument("--replace", default=None,
                        help="comma-separated file paths that replace rather than merge, "
                             "relative to the mod (e.g. common/equipment.json)")
    parser.add_argument("--table", action="append", default=[], metavar="PATH",
                        help="create a starter file for this table (repeatable); "
                             "one of: " + ", ".join(sorted(TABLES)))
    parser.add_argument("--force", action="store_true", help="overwrite an existing mod.json")
    parser.add_argument("--game", default=os.path.join(ROOT, "build", "game"),
                        help="game binary used by --validate (default build/game)")
    parser.add_argument("--validate", action="store_true",
                        help="run the game's --validate-content on the mods root afterwards")
    args = parser.parse_args(argv)

    name = args.name
    if not name or any(c in name for c in "/\\, \t"):
        parser.error("mod name must not contain path separators, commas or spaces")
    if name.startswith("add_"):
        parser.error("a mod name starting with add_ collides with the append-key prefix")

    tables = list(dict.fromkeys(args.table))
    for path in tables:
        if path == CONSTANTS:
            parser.error("--table does not accept constants.json: it holds one object, not a table")
        if path not in TABLES:
            parser.error("unknown table %r; known tables: %s"
                         % (path, ", ".join(sorted(TABLES))))

    mod_dir = os.path.join(args.root, name)
    manifest_path = os.path.join(mod_dir, "mod.json")
    if os.path.exists(manifest_path) and not args.force:
        print("refusing to overwrite %s (pass --force)" % manifest_path, file=sys.stderr)
        return 2

    replaces = parse_list(args.replace)
    for path in replaces:
        if path == CONSTANTS or path.startswith("common/focuses/"):
            continue
        if path not in TABLES:
            parser.error("--replace path %r is not a known table path" % path)

    manifest = {
        "name": name,
        "version": args.version,
        "dependencies": parse_list(args.depends),
        "load_after": parse_list(args.load_after),
        "replace_paths": replaces,
    }

    os.makedirs(os.path.join(mod_dir, "common"), exist_ok=True)
    with open(manifest_path, "w") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")
    written = [display(manifest_path)]

    for path in tables:
        target = os.path.join(mod_dir, path)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if os.path.exists(target) and not args.force:
            print("keeping existing %s" % display(target))
            continue
        with open(target, "w") as handle:
            json.dump({TABLES[path]: []}, handle, indent=2)
            handle.write("\n")
        written.append(display(target))

    print("created mod %s in %s" % (name, display(mod_dir)))
    for path in written:
        print("  %s" % path)

    if args.validate:
        if not os.access(args.game, os.X_OK):
            print("cannot validate: %s is not executable" % args.game, file=sys.stderr)
            return 2
        command = [args.game, "--validate-content", "--mods", args.root]
        print("$ " + " ".join(command))
        return subprocess.call(command)

    print("validate with: %s --validate-content --mods %s"
          % (display(args.game), display(args.root)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))