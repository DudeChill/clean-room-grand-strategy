// Mod discovery, deterministic load ordering and the load report (spec section
// 168). See data/mod.h for the contract.

#include "data/mod.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <system_error>

#include "core/json.h"

namespace hoi {
namespace {

void note(std::string* errors, const std::string& line) {
    if (!errors->empty()) *errors += "\n";
    *errors += line;
}

}  // namespace

std::string mod_event_text(const ModLoadEvent& e) {
    if (e.action == "parse") {
        return "mod " + e.mod + ": " + e.table + ": parse error: " + e.detail;
    }
    if (e.action == "unknown_file") {
        return "mod " + e.mod + ": " + e.table + ": unknown content file";
    }
    if (e.action == "manifest") {
        return e.mod.empty() ? (e.table + ": " + e.detail)
                             : ("mod " + e.mod + ": mod.json: " + e.detail);
    }
    if (e.action == "error") {
        const std::string loc = e.key.empty() ? e.table : (e.table + " " + e.key);
        return "mod " + e.mod + ": " + loc + ": " + e.detail;
    }
    return "mod " + e.mod + ": " + e.table + " " + e.key + ": " + e.action;
}

namespace {

// `dependencies`, `load_after` and `replace_paths` are lists of names; everything
// else in a manifest is an error so a typo cannot silently disable an ordering hint.
bool parse_string_list(const Json& doc, const char* key, const std::string& where,
                       std::vector<std::string>* out, std::string* errors) {
    if (!doc.has(key)) return true;
    const Json& v = doc[key];
    if (!v.is_array()) {
        note(errors, where + ": " + key + " must be an array of strings");
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        if (!v[i].is_string() || v[i].as_string().empty()) {
            note(errors, where + ": " + key + "[" + std::to_string(i) +
                             "] must be a non-empty string");
            return false;
        }
        out->push_back(v[i].as_string());
    }
    return true;
}

bool parse_manifest(const Json& doc, const std::string& path, ModManifest* out,
                    std::string* errors) {
    if (!doc.is_object()) {
        note(errors, path + ": mod.json must be an object");
        return false;
    }
    ModManifest m;
    const std::string name = doc["name"].as_string();
    if (name.empty()) {
        note(errors, path + ": mod.json is missing a non-empty \"name\"");
        // The mod cannot even be named: report against the file, not a mod.
        return false;
    }
    m.name = name;
    const std::string where = "mod " + name;
    m.version = doc["version"].as_string();
    if (doc.has("version") && !doc["version"].is_string()) {
        note(errors, where + ": mod.json: version must be a string");
        return false;
    }
    bool ok = true;
    ok = parse_string_list(doc, "dependencies", where + ": mod.json", &m.dependencies, errors) && ok;
    ok = parse_string_list(doc, "load_after", where + ": mod.json", &m.load_after, errors) && ok;
    ok = parse_string_list(doc, "replace_paths", where + ": mod.json", &m.replace_paths, errors) && ok;
    for (const auto& item : doc.object_items()) {
        const std::string& k = item.first;
        if (k != "name" && k != "version" && k != "dependencies" && k != "load_after" &&
            k != "replace_paths") {
            note(errors, where + ": mod.json: unknown key '" + k + "'");
            ok = false;
        }
    }
    if (!ok) return false;
    *out = std::move(m);
    return true;
}

// Names every mod of one cycle, reached from `start` through the edges that were
// never satisfied. Deterministic: successors are visited in sorted index order.
std::vector<size_t> find_cycle(size_t start, const std::vector<std::set<size_t>>& succ,
                               const std::vector<bool>& emitted) {
    const size_t n = succ.size();
    std::vector<int> state(n, 0);  // 0 = unvisited, 1 = on the path, 2 = done
    std::vector<size_t> path;
    std::vector<size_t> cycle;
    std::function<bool(size_t)> dfs = [&](size_t u) -> bool {
        state[u] = 1;
        path.push_back(u);
        for (size_t v : succ[u]) {
            if (emitted[v]) continue;
            if (state[v] == 1) {
                auto it = std::find(path.begin(), path.end(), v);
                cycle.assign(it, path.end());
                cycle.push_back(v);
                return true;
            }
            if (state[v] == 0 && dfs(v)) return true;
        }
        path.pop_back();
        state[u] = 2;
        return false;
    };
    dfs(start);
    return cycle;
}

}  // namespace

std::string ModLoadReport::text() const {
    std::string out;
    for (const ModLoadEvent& e : events) out += mod_event_text(e) + "\n";
    for (const std::string& n : notes) out += n + "\n";
    return out;
}

bool discover_mods(const std::string& root, std::vector<ModManifest>* out, std::string* err) {
    namespace fs = std::filesystem;
    if (out == nullptr) {
        if (err) *err = "discover_mods: null output";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (err) *err = root + ": mods root is not a directory";
        return false;
    }
    std::vector<fs::path> dirs;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;  // files at the root are not mods
        dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());

    std::string errors;
    for (const fs::path& dir : dirs) {
        const fs::path manifest = dir / "mod.json";
        if (!fs::is_regular_file(manifest, ec)) continue;  // a directory without a manifest
        Json doc;
        std::string parse_err;
        if (!Json::parse_file(manifest.string(), &doc, &parse_err)) {
            note(&errors, manifest.string() + ": parse error: " +
                              (parse_err.empty() ? std::string("cannot read file") : parse_err));
            continue;
        }
        ModManifest m;
        if (!parse_manifest(doc, manifest.string(), &m, &errors)) continue;
        m.directory = dir.string();
        out->push_back(std::move(m));
    }
    if (err) *err = errors;
    return errors.empty();
}

std::vector<const ModManifest*> resolve_load_order(const std::vector<ModManifest>& mods,
                                                   std::string* err) {
    const size_t n = mods.size();
    std::string errors;
    std::map<std::string, size_t> by_name;
    for (size_t i = 0; i < n; ++i) {
        auto it = by_name.find(mods[i].name);
        if (it != by_name.end()) {
            note(&errors, "duplicate mod name '" + mods[i].name + "' in " +
                              mods[it->second].directory + " and " + mods[i].directory);
            continue;
        }
        by_name[mods[i].name] = i;
    }

    // Edge u -> v means "u loads before v".
    std::vector<std::set<size_t>> succ(n);
    std::vector<int> indeg(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const ModManifest& m = mods[i];
        for (const std::string& dep : m.dependencies) {
            auto it = by_name.find(dep);
            if (it == by_name.end()) {
                note(&errors, "mod " + m.name + ": dependency '" + dep + "' is not installed");
                continue;
            }
            if (it->second == i) {
                note(&errors, "mod " + m.name + ": depends on itself");
                continue;
            }
            if (succ[it->second].insert(i).second) ++indeg[i];
        }
        for (const std::string& after : m.load_after) {
            auto it = by_name.find(after);
            if (it == by_name.end()) {
                note(&errors,
                     "mod " + m.name + ": load_after references unknown mod '" + after + "'");
                continue;
            }
            if (it->second == i) continue;  // loading after itself is a no-op
            if (succ[it->second].insert(i).second) ++indeg[i];
        }
    }
    if (!errors.empty()) {
        if (err) *err = errors;
        return {};
    }

    // Kahn's algorithm with a deterministic ready set: independent mods are emitted
    // in (name, directory) order, so the file system never decides the load order.
    const auto cmp = [&mods](size_t a, size_t b) {
        if (mods[a].name != mods[b].name) return mods[a].name < mods[b].name;
        if (mods[a].directory != mods[b].directory) return mods[a].directory < mods[b].directory;
        return a < b;
    };
    std::set<size_t, decltype(cmp)> ready(cmp);
    for (size_t i = 0; i < n; ++i) {
        if (indeg[i] == 0) ready.insert(i);
    }
    std::vector<bool> emitted(n, false);
    std::vector<const ModManifest*> order;
    order.reserve(n);
    while (!ready.empty()) {
        const size_t i = *ready.begin();
        ready.erase(ready.begin());
        emitted[i] = true;
        order.push_back(&mods[i]);
        for (size_t s : succ[i]) {
            if (--indeg[s] == 0) ready.insert(s);
        }
    }
    if (order.size() != n) {
        std::vector<size_t> cycle;
        for (size_t i = 0; i < n && cycle.empty(); ++i) {
            if (!emitted[i]) cycle = find_cycle(i, succ, emitted);
        }
        std::string line = "mod dependency cycle:";
        for (size_t c : cycle) line += " " + mods[c].name + " ->";
        if (!cycle.empty()) line.erase(line.size() - 3);  // drop the trailing " ->"
        note(&errors, line);
        if (err) *err = errors;
        return {};
    }
    return order;
}

}  // namespace hoi