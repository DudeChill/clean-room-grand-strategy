// Mod loader internals: the resolved load order, cycle rejection, override and
// addition semantics on a hand-built data root, and the shipped example mod
// (data/mods/example_mod) end to end through Game::create, save/load and world_hash.
//
// The fixture pack under tests/mods/ and the CLI-facing contract live in
// tests/test_modpack.cpp; this file owns the pure ordering rules and the example.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "data/content.h"
#include "data/mod.h"
#include "game/game.h"
#include "save/save.h"
#include "test.h"

namespace {

using namespace hoi;

std::string repo_root() {
    std::string file(__FILE__);
    const std::string suffix = "/tests/test_mods.cpp";
    if (file.size() > suffix.size() &&
        file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return file.substr(0, file.size() - suffix.size());
    }
    return ".";
}

std::vector<std::string> order_names(const std::vector<const ModManifest*>& order) {
    std::vector<std::string> names;
    for (const ModManifest* m : order) names.push_back(m->name);
    return names;
}

ModManifest manifest(const std::string& name, const std::string& directory,
                     std::vector<std::string> dependencies = {},
                     std::vector<std::string> load_after = {}) {
    ModManifest m;
    m.name = name;
    m.version = "1.0.0";
    m.directory = directory;
    m.dependencies = std::move(dependencies);
    m.load_after = std::move(load_after);
    return m;
}

// A temporary tree of mods, each a directory holding mod.json and data files.
struct TempMods {
    std::filesystem::path dir;
    TempMods() {
        dir = std::filesystem::temp_directory_path() / "hoi4clone_test_mods";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~TempMods() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempMods(const TempMods&) = delete;
    TempMods& operator=(const TempMods&) = delete;

    void write(const std::string& relative, const std::string& text) const {
        const std::filesystem::path p = dir / relative;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        out << text;
    }
    [[nodiscard]] std::string root() const { return dir.string(); }
};

// Minimal but complete base data root: the six required files, one equipment model
// and one technology that unlocks it.
struct TempData {
    std::filesystem::path dir;
    TempData() {
        dir = std::filesystem::temp_directory_path() / "hoi4clone_test_mods_data";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "common", ec);

        write("constants.json", R"({"ic_per_military_factory": 9.0})");
        write("equipment.json", R"({
  "equipment": [
    {"key": "infantry_equipment", "name": "Infantry Equipment", "category": "infantry",
     "is_archetype": true, "organization": 60.0, "max_strength": 25.0},
    {"key": "infantry_equipment_1", "name": "Infantry Equipment I", "category": "infantry",
     "archetype": "infantry_equipment", "year": 1936, "soft_attack": 3.0, "defense": 4.0,
     "organization": 60.0, "max_strength": 25.0, "build_cost": 0.43, "manpower": 1000.0}
  ]
})");
        write("buildings.json", R"({
  "buildings": [
    {"key": "civilian_factory", "name": "Civilian Factory", "kind": "civilian_factory",
     "base_cost": 10800.0, "per_state": true, "max_level": 12}
  ]
})");
        write("technologies.json", R"({
  "technologies": [
    {"key": "infantry_weapons", "name": "Infantry Weapons", "category": "infantry",
     "year": 1936, "cost_days": 80.0, "unlock_equipment": ["infantry_equipment_1"]}
  ]
})");
        write("laws.json", R"({
  "laws": [
    {"key": "conscription_volunteer", "name": "Volunteer Only", "kind": 0, "level": 0}
  ]
})");
        write("templates.json", R"({
  "templates": [
    {"key": "infantry_division", "name": "Infantry Division", "train_days": 90.0,
     "battalions": [{"equipment": "infantry_equipment_1", "count": 3}]}
  ]
})");
    }
    ~TempData() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempData(const TempData&) = delete;
    TempData& operator=(const TempData&) = delete;

    void write(const std::string& name, const std::string& text) const {
        std::ofstream out(dir / "common" / name, std::ios::binary);
        out << text;
    }
    [[nodiscard]] std::string root() const { return dir.string(); }
};

// The shipped example mod, exercised through a real game.
const char* kExampleMod = "example_mod";
const char* kExampleModOverride = "infantry_equipment_1";
const char* kExampleModAddition = "assault_rifles_1";
const char* kExampleModTech = "assault_rifle_development";

}  // namespace

HOI_TEST(MODS_001_load_order_is_dependencies_then_hints_then_name) {
    // `gamma` needs `alpha`; `delta` wants to follow `gamma`; `alpha` and `beta` are
    // independent, so the name breaks the tie between them.
    std::vector<ModManifest> mods = {
        manifest("gamma", "/m/gamma", {"alpha"}),
        manifest("delta", "/m/delta", {}, {"gamma"}),
        manifest("beta", "/m/beta"),
        manifest("alpha", "/m/alpha"),
    };
    std::string err;
    std::vector<const ModManifest*> order = resolve_load_order(mods, &err);
    CHECK(err.empty());
    const std::vector<std::string> names = order_names(order);
    CHECK_EQ(names.size(), 4u);
    CHECK_EQ(names[0], std::string("alpha"));
    CHECK_EQ(names[1], std::string("beta"));
    CHECK_EQ(names[2], std::string("gamma"));
    CHECK_EQ(names[3], std::string("delta"));

    // The input order must not matter: the same set reversed resolves identically.
    std::vector<ModManifest> reversed(mods.rbegin(), mods.rend());
    std::string err2;
    const std::vector<std::string> names2 = order_names(resolve_load_order(reversed, &err2));
    CHECK(err2.empty());
    CHECK_EQ(names2, names);

    // A load_after hint beats the name order without a dependency.
    std::vector<ModManifest> hinted = {
        manifest("aaa", "/m/aaa", {}, {"zzz"}),
        manifest("zzz", "/m/zzz"),
    };
    std::string err3;
    const std::vector<std::string> hint_order =
        order_names(resolve_load_order(hinted, &err3));
    CHECK(err3.empty());
    CHECK_EQ(hint_order.size(), 2u);
    CHECK_EQ(hint_order[0], std::string("zzz"));
    CHECK_EQ(hint_order[1], std::string("aaa"));
}

HOI_TEST(MODS_002_cycles_and_dangling_references_are_errors) {
    std::vector<ModManifest> cycle = {
        manifest("a", "/m/a", {"c"}),
        manifest("b", "/m/b", {"a"}),
        manifest("c", "/m/c", {"b"}),
    };
    std::string err;
    const std::vector<const ModManifest*> order = resolve_load_order(cycle, &err);
    CHECK(order.empty());
    CHECK(err.find("cycle") != std::string::npos);
    CHECK(err.find("a") != std::string::npos);
    CHECK(err.find("b") != std::string::npos);
    CHECK(err.find("c") != std::string::npos);

    std::string missing_err;
    resolve_load_order({manifest("a", "/m/a", {"ghost"})}, &missing_err);
    CHECK(missing_err.find("ghost") != std::string::npos);

    std::string after_err;
    resolve_load_order({manifest("a", "/m/a", {}, {"ghost"})}, &after_err);
    CHECK(after_err.find("ghost") != std::string::npos);

    std::string self_err;
    resolve_load_order({manifest("a", "/m/a", {"a"})}, &self_err);
    CHECK(self_err.find("itself") != std::string::npos);

    std::string dup_err;
    resolve_load_order({manifest("a", "/m/one"), manifest("a", "/m/two")}, &dup_err);
    CHECK(dup_err.find("duplicate") != std::string::npos);
}

HOI_TEST(MODS_003_replacement_and_addition) {
    TempData data;
    TempMods mods;
    mods.write("rebalance/mod.json",
               R"({"name": "rebalance", "version": "2.0", "dependencies": [],
                   "load_after": [], "replace_paths": []})");
    mods.write("rebalance/common/equipment.json", R"JSON({
  "equipment": [
    {"key": "infantry_equipment_1", "name": "Infantry Equipment I (Reworked)",
     "category": "infantry", "archetype": "infantry_equipment", "soft_attack": 9.0,
     "defense": 4.0, "organization": 60.0, "max_strength": 25.0, "build_cost": 0.43,
     "manpower": 1000.0},
    {"key": "rifle_2", "name": "Rifle II", "category": "infantry",
     "archetype": "infantry_equipment", "soft_attack": 12.0, "build_cost": 0.6,
     "add": true}
  ]
})JSON");
    mods.write("rebalance/common/technologies.json", R"({
  "add_technologies": [
    {"key": "rifle_2_tech", "name": "Rifle II", "category": "infantry", "year": 1943,
     "cost_days": 100.0, "unlock_equipment": ["rifle_2"]}
  ]
})");

    Content content;
    ModLoadReport report;
    std::string err;
    const std::vector<std::string> roots{mods.root()};
    CHECK(load_content(data.root(), &content, &err, roots, &report));
    CHECK(report.load_order.size() == 1 && report.load_order[0] == "rebalance");
    CHECK(!report.has_errors());
    CHECK(content.load_errors.empty());

    // The override replaced the shipped entry in place: the new value is loaded and
    // the model keeps its id position among the archetypes.
    const EquipmentDef* replaced = content.equipment_def(content.equipment_id(kExampleModOverride));
    CHECK(replaced != nullptr);
    CHECK_NEAR(replaced->soft_attack, 9.0, 1e-9);
    CHECK(replaced->name == "Infantry Equipment I (Reworked)");
    CHECK_EQ(content.equipment_id(kExampleModOverride).v, 1u);

    // The addition appended, and the technology that gates it is registered.
    const EquipmentId added = content.equipment_id("rifle_2");
    CHECK(added.valid());
    const EquipmentDef* added_def = content.equipment_def(added);
    CHECK(added_def != nullptr && added_def->soft_attack == 12.0);
    CHECK(content.tech_id("rifle_2_tech").valid());

    CHECK_EQ(report.text(),
             std::string("mod rebalance: equipment infantry_equipment_1: replaced\n"
                         "mod rebalance: equipment rifle_2: added\n"
                         "mod rebalance: technologies rifle_2_tech: added\n"));
}

HOI_TEST(MODS_004_a_rejected_mod_changes_nothing) {
    TempData data;
    TempMods mods;
    // Two files: one that would override a shipped definition and one that names a
    // table the engine has no loader for. The whole mod must be rejected.
    mods.write("broken/mod.json", R"({"name": "broken", "version": "0.1"})");
    mods.write("broken/common/equipment.json", R"({
  "equipment": [
    {"key": "infantry_equipment_1", "name": "Should Not Load", "category": "infantry",
     "archetype": "infantry_equipment", "soft_attack": 99.0}
  ]
})");
    mods.write("broken/common/hovertanks.json", R"({"hovertanks": []})");

    Content base;
    Content with_bad;
    std::string err;
    CHECK(load_content(data.root(), &base, &err));

    ModLoadReport report;
    const std::vector<std::string> roots{mods.root()};
    CHECK(load_content(data.root(), &with_bad, &err, roots, &report));
    CHECK(report.has_errors());
    CHECK(report.text().find("mod broken: common/hovertanks.json: unknown content file") !=
          std::string::npos);
    // The rejected mod left no trace: the shipped value survived and the rest of the
    // table matches a base-only load exactly.
    CHECK_EQ(with_bad.equipment.size(), base.equipment.size());
    CHECK_EQ(with_bad.techs.size(), base.techs.size());
    const EquipmentDef* kept = with_bad.equipment_def(with_bad.equipment_id("infantry_equipment_1"));
    CHECK(kept != nullptr && kept->name == "Infantry Equipment I");
    CHECK_NEAR(kept->soft_attack, 3.0, 1e-9);

    // The same rule against the shipped content: a rejected mod must not move the
    // world hash, because a world built from it is the world built from base data.
    TempMods real_mods;
    real_mods.write("bad_real/mod.json", R"({"name": "bad_real", "version": "1.0"})");
    real_mods.write("bad_real/common/equipment.json", R"JSON({
  "equipment": [
    {"key": "infantry_equipment_1", "name": "Should Not Load", "category": "infantry",
     "archetype": "infantry_equipment", "soft_attack": 99.0, "add": true}
  ]
})JSON");
    real_mods.write("bad_real/common/unknown.json", R"({"unknown": []})");

    const std::string shipped = repo_root() + "/data";
    const std::string scenario = shipped + "/scenarios/1936.json";
    Game plain;
    Game with_bad_mod;
    std::string game_err;
    if (!Game::create(shipped, scenario, 11, &plain, &game_err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "base game failed to load: " + game_err);
    }
    const std::vector<std::string> bad_roots{real_mods.root()};
    if (!Game::create(shipped, scenario, 11, &with_bad_mod, &game_err, bad_roots)) {
        ::hoi_test::fail(__FILE__, __LINE__, "game with a rejected mod failed: " + game_err);
    }
    CHECK_EQ(world_hash(with_bad_mod), world_hash(plain));
    CHECK(!with_bad_mod.content.load_errors.empty());
}

HOI_TEST(MODS_006_replace_paths_take_a_table_from_that_mod_alone) {
    TempData data;
    TempMods mods;
    mods.write("rewrite/mod.json",
               R"({"name": "rewrite", "version": "1.0", "replace_paths": ["laws"]})");
    mods.write("rewrite/common/laws.json", R"({
  "laws": [
    {"key": "mod_law", "name": "Mod Law", "kind": 0, "level": 0, "cost": 7.0}
  ]
})");

    Content content;
    ModLoadReport report;
    std::string err;
    const std::vector<std::string> roots{mods.root()};
    CHECK(load_content(data.root(), &content, &err, roots, &report));
    CHECK(!report.has_errors());
    // The shipped law is gone: the table comes only from the mod.
    CHECK(content.law("conscription_volunteer") == nullptr);
    CHECK_EQ(content.laws.size(), 1u);
    const LawDef* law = content.law("mod_law");
    CHECK(law != nullptr && law->cost == 7.0);
    CHECK_EQ(report.text(), std::string("mod rewrite: laws mod_law: added\n"));

    // A table name the engine has no loader for is an error naming the mod.
    TempMods bad;
    bad.write("typo/mod.json",
              R"({"name": "typo", "version": "1.0", "replace_paths": ["ghosts"]})");
    Content ignored;
    ModLoadReport bad_report;
    const std::vector<std::string> bad_roots{bad.root()};
    CHECK(load_content(data.root(), &ignored, &err, bad_roots, &bad_report));
    CHECK(bad_report.text().find("mod typo: ghosts: replace_paths names unknown table") !=
          std::string::npos);
    CHECK_EQ(ignored.laws.size(), 1u);  // nothing was cleared
}

HOI_TEST(MODS_005_the_example_mod_loads_replaces_adds_and_unlocks) {
    const std::string data = repo_root() + "/data";
    const std::string scenario = data + "/scenarios/1936.json";

    Game plain;
    std::string err;
    if (!Game::create(data, scenario, 1936, &plain, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "base game failed to load: " + err);
    }

    const std::vector<std::string> roots{data + "/mods"};
    Game modded;
    ModLoadReport report;
    if (!Game::create(data, scenario, 1936, &modded, &err, roots, &report)) {
        ::hoi_test::fail(__FILE__, __LINE__, "example mod failed to load: " + err);
    }
    CHECK(!report.has_errors());
    CHECK(report.load_order.size() == 1 && report.load_order[0] == kExampleMod);
    // Exact report: one replacement and two additions, in load-order/table/key order.
    CHECK_EQ(report.text(),
             std::string("mod example_mod: equipment assault_rifles_1: added\n"
                         "mod example_mod: equipment infantry_equipment_1: replaced\n"
                         "mod example_mod: technologies assault_rifle_development: added\n"));

    // (a) the override, (b) the new model, (c) the technology that unlocks it.
    const EquipmentDef* replaced =
        modded.content.equipment_def(modded.content.equipment_id(kExampleModOverride));
    CHECK(replaced != nullptr && replaced->soft_attack > 3.0);
    CHECK(modded.content.equipment_id(kExampleModAddition).valid());
    const TechDef* tech = modded.content.tech_def(modded.content.tech_id(kExampleModTech));
    CHECK(tech != nullptr);
    bool unlocks = false;
    for (const std::string& ek : tech->unlock_equipment) {
        if (ek == kExampleModAddition) unlocks = true;
    }
    CHECK(unlocks);

    // The new equipment model is world state (countries stockpile per equipment id),
    // so the example mod must move the world hash.
    CHECK(world_hash(plain) != world_hash(modded));

    // The same inputs produce the same world hash twice.
    Game again;
    ModLoadReport again_report;
    if (!Game::create(data, scenario, 1936, &again, &err, roots, &again_report)) {
        ::hoi_test::fail(__FILE__, __LINE__, "second example mod load failed: " + err);
    }
    CHECK_EQ(world_hash(again), world_hash(modded));
    CHECK_EQ(again_report.text(), report.text());

    // A modded save round-trips through a game created with the same mods.
    const std::string save_path =
        (std::filesystem::temp_directory_path() / "hoi4clone_test_mods.save").string();
    if (!save_game(modded, save_path, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "save failed: " + err);
    }
    Game reloaded;
    if (!Game::create(data, scenario, 1936, &reloaded, &err, roots)) {
        ::hoi_test::fail(__FILE__, __LINE__, "reload create failed: " + err);
    }
    if (!load_game(reloaded, save_path, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "load failed: " + err);
    }
    CHECK_EQ(world_hash(reloaded), world_hash(modded));
    std::error_code ec;
    std::filesystem::remove(save_path, ec);
}