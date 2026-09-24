# Modding

Every number and definition the simulation uses lives in JSON under `data/`: equipment,
technologies, buildings, laws, components, national focuses, events, decisions, spirits,
advisers, division templates and the tunable constants. A **mod** is a directory that carries
its own copy of that tree. The engine loads the base tree, then applies mods in a
deterministic order, so the same base data plus the same mods always produce the same world
(`world_hash` is unchanged across runs).

This document is the contract; the code that implements it is `src/data/mod.h`,
`src/data/mod.cpp` and the mod step of `src/data/content.cpp`.

## What a mod is

```
mods/my_mod/mod.json                 the manifest
mods/my_mod/common/equipment.json    content files mirror data/ exactly
mods/my_mod/common/focuses/italy.json
```

The prefix `mods/` is not special. The compiler of a loaded set is the **mods root**: a
directory whose immediate subdirectories each hold a `mod.json`, and the engine discovers
`<mods_root>/<mod>/mod.json`. `game --mods data/mods` therefore loads every mod installed
under `data/mods`, and `game --mods mods_a,mods_b` loads two roots at once.

Rules for the file tree of one mod:

* every file must map to a table the engine knows:

  | file | top-level member |
  |---|---|
  | `common/equipment.json` | `equipment` |
  | `common/buildings.json` | `buildings` |
  | `common/technologies.json` | `technologies` |
  | `common/laws.json` | `laws` |
  | `common/templates.json` | `templates` |
  | `common/events.json` | `events` |
  | `common/decisions.json` | `decisions` |
  | `common/spirits.json` | `spirits` |
  | `common/advisors.json` | `advisors` |
  | `common/components.json` | `components` |
  | `common/constants.json` | one object of tunable numbers |
  | `common/focuses/<anything>.json` | `focuses` |

* an unknown file, an unknown top-level member inside a known file, an unknown field on an
  entry, and an unknown nested modifier or resource name are all **errors**, not warnings.
  Unknown content is how a mod stops working after an engine update, so it is reported.
* a file with invalid JSON is an error naming the file and the parser message.
* a mod that produces any error is skipped **as a whole** and contributes nothing: a bad mod
  can never leave half-applied content behind.

## `mod.json`

```json
{
  "name": "example_mod",
  "version": "1.0.0",
  "dependencies": ["some_other_mod"],
  "load_after": ["another_mod"],
  "replace_paths": ["common/equipment.json"]
}
```

| field | required | meaning |
|---|---|---|
| `name` | yes | the mod's identity, non-empty and unique across the loaded roots. Used in every diagnostic and in the ordering tie-break. |
| `version` | no | informational string, printed nowhere yet; keep it truthful for packaging. |
| `dependencies` | no | mod **names** that must load before this one and whose content this one builds on. A dependency that is not installed is an error. |
| `load_after` | no | soft ordering hints: "this mod must load after those, but I do not require them". Naming a mod that is not installed is an error, so a typo cannot silently do nothing. |
| `replace_paths` | no | tables that come **only** from this mod. Every table is one document that mods merge into, so `replace_paths` drops everything already accumulated for those tables — base content and earlier mods — before this mod is applied. Use it for a total conversion of one table. Either spelling works: the logical name (`equipment`) or the data path (`common/equipment.json`, and `common/focuses/<tree>.json` for one focus tree). |

A missing or unreadable `mod.json` means the directory is not a mod and is ignored, but a
`mod.json` that parses and is wrong (missing `name`, a field that is not a string list, an
unknown key) is an error: the directory was clearly meant to be a mod.

Directory names are the secondary tie-break, never the identity: two mods may sit in
directories named anything as long as their `name` fields differ.

## Load order

The order is a total order, computed from the mods part of the data alone — the file system
never decides it:

1. **Dependencies first.** A dependency edge loads the dependency before its dependent.
2. **`load_after` hints next.** A hint orders its two ends; it does not require the other mod
   to exist, but naming one that is missing is still an error.
3. **Ties are broken by mod name, then by directory**, so independent mods always load in the
   same order on every machine.

Ordering problems are reported and reject the load rather than falling back to an arbitrary
order: a dependency on itself, a cycle (the message names every mod in the cycle), a
duplicate `name` across roots, a dependency that is not installed, or a `load_after` reference
that is not installed.

## Override rules

Applied in load order, definition by definition:

* **Same key replaces.** An entry whose key already exists (in the base content or in an
  earlier mod) replaces that definition. The replacement is reported:
  `mod <name>: <table> <key>: replaced`.
* **Additions append.** An entry is an addition when it says `"add": true`, when it is carried
  by an `add_<table>` member (for example `{"add_laws": [...]}` inside a mod's
  `common/laws.json`), or when its key starts with `add_` (the prefix is stripped, so
  `add_my_law` adds the key `my_law`). An addition always appends and is reported as
  `mod <name>: <table> <key>: added`.
* **An addition must not collide.** Appending a key that already exists is an error, never a
  silent win: if you meant to replace, drop the prefix or the flag. This keeps the two intents
  ("I am adding new content" and "I am deliberately changing yours") from being confused.
* **`replace_paths` resets a table.** For a table named in `replace_paths`, everything
  accumulated before that mod is dropped, so the mod ships the whole table.
* **Errors name the place.** A rejected entry is reported as
  `mod <name>: <table> <key>: <reason>`; a rejected file as
  `mod <name>: <path>: parse error: <detail>` or `mod <name>: <path>: unknown content file`;
  a bad manifest as `mod <name>: mod.json: <reason>`, or, when it cannot even be named,
  `<root>/<mod>/mod.json: parse error: <detail>`.

Every error also lands in `Content::load_errors`, so the engine has one diagnostic list
whether the content came from a mod or from the base tree.

## Running with mods

```sh
build/game --mods data/mods --scenario data/scenarios/1936.json --days 30
build/game --mods mods_big,mods_ui --serve          # the same set for the served client
```

The load report (every replacement, addition and error) is printed before the run. A run with
mod errors refuses to start: a mod that failed to apply must not be silently absent from the
game.

## Validating content

```sh
build/game --validate-content                          # shipped content, no mods
build/game --validate-content --mods data/mods         # shipped content plus the shipped mod
scripts/validate-content.sh                            # the gate: base tree, data/mods, tests/mods
```

`--validate-content` loads the content and the scenario through the same code path a real run
uses, prints the deterministic load report and every content diagnostic, then runs the world
auditor over a one-day run. It writes nothing: no save file, no state dump. Exit status is 0
with `content validation: OK`, 1 when any diagnostic or audit failure was reported, 2 for a
usage error.

`scripts/validate-content.sh` is the release-gate wrapper. It validates the shipped tree, then
every mod under `data/mods/` (each one alone, then all together), checks that two identical
runs with the shipped mods produce identical subsystem and world hashes, and finally runs the
mod test pack in `tests/mods/` — which contains deliberately broken mods, so each of its cases
asserts both a non-zero exit and the expected diagnostic text. Point it at another binary with
`GAME=... scripts/validate-content.sh`.

The mod test pack (`tests/mods/`) is also exercised by `hoi_tests` (`tests/test_modpack.cpp`),
which loads each fixture through the engine API and asserts the resulting definitions and
report, so an ordering or override regression fails the unit suite as well as the gate.

## The example mod

`data/mods/example_mod` is a working, minimal mod and the one the release gate validates:

* `common/equipment.json` **replaces** `infantry_equipment_1` (a deliberate rebalance: more
  soft attack and defence, slightly cheaper) and **adds** `assault_rifles_1` with `"add": true`,
  so the new model cannot silently overwrite an existing key.
* `common/technologies.json` **adds** `assault_rifle_development` (year 1943) with
  `"add": true`, unlocked by `improved_infantry_weapons` and unlocking `assault_rifles_1`.

The manifest declares no dependencies, no ordering hints and no `replace_paths`, so it proves
the simplest case: it loads, it is reported, and it validates against plain `data/` as well as
against `--mods data/mods`. Run it:

```sh
build/game --validate-content --mods data/mods      # report, then content validation: OK
build/game --mods data/mods --days 30 --summary     # play with it applied
```

It is small enough to copy as a starting point. To scaffold your own:

```sh
tools/make_mod.py my_mod --table common/laws.json --validate
```

`tools/make_mod.py` writes a manifest plus empty (valid) starter files for the tables you ask
for, then optionally runs the validator on the mods root so the scaffold is proven loadable
before you edit it.

## Packaging a mod for distribution

1. Keep the mod directory self-contained: `mod.json` plus the content tree, nothing else
   required at runtime. Add your own `README.md` describing what it changes.
2. Give it a real `version` and keep `name` stable across releases: `name` is the identity
   other mods depend on and the string every diagnostic uses.
3. Declare what you build on — `dependencies` for hard requirements, `load_after` for
   preferences — rather than relying on alphabetical luck.
4. Validate before shipping: `scripts/validate-content.sh` should pass with your mod as the
   only mod in a root, and with the whole set of mods it is meant to be played with.
5. Ship a zip whose top-level entry is the mod directory (`my_mod/mod.json`,
   `my_mod/common/...`), not a zip of the files at the top level and not a nested
   `my_mod/my_mod/` — users unzip it straight into their mods root.
6. Do not ship build output, saves, absolute paths or anything time-dependent. Mods are
   content: two installs of the same mod must produce the same world hash.
7. Say which base content you override. Prefer additive keys when a table already has what you
   need; when you deliberately replace a shipped definition, expect it in the load report, and
   expect another mod overriding the same key after you to win.

A save records the world, not the definitions: content is addressed by ids assigned during
load. Load a modded save with the same mods in the same load order, or the ids can refer to
different definitions.