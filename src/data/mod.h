#pragma once
// Mod layer: manifests, deterministic load order and the load report that records
// every override, addition and conflict (spec section 168).
//
// A mod is a directory holding `mod.json` and a data tree mirroring `data/`:
//
//   mods/my_mod/mod.json
//   mods/my_mod/common/equipment.json
//   mods/my_mod/common/focuses/my_tree.json
//
// Discovery is `<mods_root>/<mod>/mod.json`, so the CLI's `--mods dir[,dir...]`
// takes mods roots, not individual mods.
//
// Override rules, applied in resolved load order:
//   * an entry whose key already exists replaces the earlier definition in place,
//     and the replacement is reported;
//   * an entry marked `"add": true`, carried by an `"add_<table>"` member, or whose
//     key starts with `add_` is an addition: it appends, and colliding with an
//     existing key is an error, never a silent win;
//   * `replace_paths` names tables that come only from that mod: everything
//     accumulated for those tables (base content and earlier mods) is dropped first.
//
// A mod that fails validation is skipped as a whole, with one diagnostic per
// problem: a rejected mod contributes nothing, so a bad mod can never leave
// half-applied content behind. Unknown tables, unknown fields and unknown nested
// modifier/resource names are errors, not warnings.

#include <string>
#include <vector>

namespace hoi {

// One `<mods_root>/<mod>/mod.json`.
struct ModManifest {
    std::string name;
    std::string version;
    std::string directory;                   // path of the mod directory itself
    std::vector<std::string> dependencies;   // mod names that must load first
    std::vector<std::string> load_after;     // soft ordering hints
    std::vector<std::string> replace_paths;  // tables taken only from this mod
};

// What happened to one definition while the mods were applied. `action` is
// "replaced" or "added" for a definition, and "error" with `detail` holding the
// reason when the entry (and its whole mod) was rejected.
struct ModLoadEvent {
    std::string mod;
    std::string table;
    std::string key;
    std::string action;
    std::string detail;
};

// Deterministic record of a mod load: `events` in load-order then table then key
// order, `notes` for diagnostics that belong to no single definition (a malformed
// manifest, an unresolvable load order), `errors` one rendered line per problem and
// `load_order` the resolved mod names. `text()` renders `events` then `notes`, one
// line each.
struct ModLoadReport {
    std::vector<ModLoadEvent> events;
    std::vector<std::string> notes;
    std::vector<std::string> errors;
    std::vector<std::string> load_order;

    [[nodiscard]] bool has_errors() const { return !errors.empty(); }
    [[nodiscard]] std::string text() const;
};

// Renders one load event as its report line:
//   mod <name>: <table> <key>: replaced|added|<reason>
//   mod <name>: <relative path>: parse error: <reason>
//   mod <name>: <relative path>: unknown content file
std::string mod_event_text(const ModLoadEvent& event);

// Scans every immediate subdirectory of `root` for `mod.json`. Directories without
// a manifest and non-directory entries are ignored. A manifest that cannot be read
// or is malformed is skipped with one diagnostic line appended to `*err` (lines are
// newline separated); every manifest that parsed is still returned, and the return
// value reports whether all of them were accepted.
bool discover_mods(const std::string& root, std::vector<ModManifest>* out, std::string* err);

// Total order over `mods`: dependencies first, then `load_after` hints; independent
// mods are ordered by mod name and then by directory, so the order never depends on
// the file system. Unknown dependencies, unknown `load_after` targets, duplicate
// names and cycles are errors (newline separated in `*err`); on a cycle the message
// names every mod in the cycle. Returns an empty vector on any error.
std::vector<const ModManifest*> resolve_load_order(const std::vector<ModManifest>& mods,
                                                   std::string* err);

}  // namespace hoi