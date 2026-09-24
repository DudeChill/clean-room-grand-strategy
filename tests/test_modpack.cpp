// Mod test pack (MOD-002 discovery, ordering and override rules; MOD-003 the pack).
//
// The fixtures under tests/mods/ are loaded through the same API a real run uses, so
// the assertions cover the frozen contract in docs/MODDING.md: the resolved load
// order, which definition survives an override, that additions append and colliding
// ones are errors, and that a broken mod is skipped as a whole with an actionable
// diagnostic naming the file or key at fault.
//
// The pack is grouped by mods root, one directory per case:
//   tests/mods/clean         a clean override of a shipped definition
//   tests/mods/chain         a dependency chain that must beat the name order
//   tests/mods/hint          a load_after hint that must beat the name order
//   tests/mods/append        the add_ addition form
//   tests/mods/broken_json   a malformed file
//   tests/mods/unknown_table a file naming a table the engine has no loader for
//   tests/mods/unknown_key   a known table with an unknown key inside it
//   tests/mods/conflict      an addition colliding with an existing key

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
    const std::string suffix = "/tests/test_modpack.cpp";
    if (file.size() > suffix.size() &&
        file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return file.substr(0, file.size() - suffix.size());
    }
    return ".";
}

std::string pack_root(const std::string& name) {
    return repo_root() + "/tests/mods/" + name;
}

// One load of the real content tree with one fixture root applied.
struct PackLoad {
    Content content;
    ModLoadReport report;
    std::string err;
    bool ok = false;

    explicit PackLoad(const std::string& mods_root) {
        const std::vector<std::string> roots{mods_root};
        ok = load_content(repo_root() + "/data", &content, &err, roots, &report);
    }

    [[nodiscard]] const LawDef* law(const std::string& key) const { return content.law(key); }

    [[nodiscard]] bool report_has(const std::string& needle) const {
        return report.text().find(needle) != std::string::npos;
    }

    [[nodiscard]] bool error_has(const std::string& needle) const {
        for (const std::string& e : report.errors) {
            if (e.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

// The shipped value of the law the override and conflict fixtures target, so an
// assertion that a fixture did NOT apply can be stated against real data.
constexpr double kShippedConscriptionLimitedCost = 50.0;

std::vector<std::string> order_names(const std::vector<const ModManifest*>& order) {
    std::vector<std::string> names;
    for (const ModManifest* m : order) names.push_back(m->name);
    return names;
}

std::vector<std::string> resolve_names(const std::string& root) {
    std::vector<ModManifest> mods;
    std::string err;
    if (!discover_mods(root, &mods, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "discover_mods failed: " + err);
    }
    std::string order_err;
    std::vector<const ModManifest*> order = resolve_load_order(mods, &order_err);
    if (!order_err.empty()) {
        ::hoi_test::fail(__FILE__, __LINE__, "resolve_load_order failed: " + order_err);
    }
    CHECK_EQ(order.size(), mods.size());
    return order_names(order);
}

}  // namespace

HOI_TEST(MODPACK_001_clean_override_replaces_a_shipped_definition) {
    PackLoad load(pack_root("clean"));
    CHECK(load.ok);
    CHECK(load.report.errors.empty());

    const LawDef* replaced = load.law("conscription_limited");
    CHECK(replaced != nullptr);
    CHECK_NEAR(replaced->cost, 25.0, 1e-9);
    CHECK(replaced->name.find("rebalanced") != std::string::npos);
    // An override replaces in place: the rest of the table is untouched.
    const LawDef* untouched = load.law("conscription_volunteer");
    CHECK(untouched != nullptr);
    CHECK_NEAR(untouched->cost, 0.0, 1e-9);

    CHECK(load.report_has("mod clean_override: laws conscription_limited: replaced"));
    CHECK(load.report_has("mod clean_override"));
}

HOI_TEST(MODPACK_002_dependency_forces_the_base_mod_first) {
    // `alpha_top` sorts before `zeta_base` by both name and directory, so only the
    // declared dependency can put `zeta_base` first.
    const std::vector<std::string> expected{"zeta_base", "alpha_top"};
    CHECK(resolve_names(pack_root("chain")) == expected);

    PackLoad load(pack_root("chain"));
    CHECK(load.ok);
    CHECK(load.report.errors.empty());
    CHECK(load.report.load_order == expected);
    // The base adds the law, the dependent replaces it: one surviving definition.
    const LawDef* law = load.law("mod_chain_law");
    CHECK(law != nullptr);
    CHECK_NEAR(law->cost, 90.0, 1e-9);
    CHECK(load.report_has("mod zeta_base: laws mod_chain_law: added"));
    CHECK(load.report_has("mod alpha_top: laws mod_chain_law: replaced"));
}

HOI_TEST(MODPACK_003_load_after_hint_forces_the_hint_first) {
    // `gamma_late` sorts first by name; only the load_after hint can reorder it.
    const std::vector<std::string> expected{"hint_early", "gamma_late"};
    CHECK(resolve_names(pack_root("hint")) == expected);

    PackLoad load(pack_root("hint"));
    CHECK(load.ok);
    CHECK(load.report.errors.empty());
    CHECK(load.report.load_order == expected);
    const LawDef* law = load.law("mod_hint_law");
    CHECK(law != nullptr);
    CHECK_NEAR(law->cost, 75.0, 1e-9);
}

HOI_TEST(MODPACK_004_add_prefixed_key_appends_without_replacing) {
    PackLoad load(pack_root("append"));
    CHECK(load.ok);
    CHECK(load.report.errors.empty());

    // The prefix is stripped: the added key has no `add_` in the loaded content.
    CHECK(load.law("add_mod_appended_law") == nullptr);
    const LawDef* added = load.law("mod_appended_law");
    CHECK(added != nullptr);
    CHECK_NEAR(added->cost, 5.0, 1e-9);
    CHECK(load.report_has("mod append_mod: laws mod_appended_law: added"));
}

HOI_TEST(MODPACK_005_malformed_file_skips_the_whole_mod) {
    PackLoad load(pack_root("broken_json"));
    CHECK(load.error_has("mod broken_json: common/equipment.json: parse error"));
    // The rejected mod contributed nothing: the base content is intact.
    const LawDef* law = load.law("conscription_limited");
    CHECK(law != nullptr);
    CHECK_NEAR(law->cost, kShippedConscriptionLimitedCost, 1e-9);
}

HOI_TEST(MODPACK_006_unknown_table_is_reported_and_skipped) {
    PackLoad load(pack_root("unknown_table"));
    CHECK(load.error_has("common/hovertanks.json"));
    CHECK(load.error_has("mod unknown_table"));
    const LawDef* law = load.law("conscription_limited");
    CHECK(law != nullptr);
    CHECK_NEAR(law->cost, kShippedConscriptionLimitedCost, 1e-9);
}

HOI_TEST(MODPACK_007_unknown_key_is_reported_and_skipped) {
    PackLoad load(pack_root("unknown_key"));
    CHECK(load.error_has("unknown modifier 'NoSuchModifier'"));
    CHECK(load.law("mod_typo_law") == nullptr);
}

HOI_TEST(MODPACK_008_append_onto_an_existing_key_is_a_conflict) {
    PackLoad load(pack_root("conflict"));
    CHECK(load.error_has("conscription_limited"));
    CHECK(load.error_has("mod conflict_mod"));
    // The conflicting addition lost: the shipped definition is unchanged.
    const LawDef* law = load.law("conscription_limited");
    CHECK(law != nullptr);
    CHECK_NEAR(law->cost, kShippedConscriptionLimitedCost, 1e-9);
    CHECK(law->name.find("Should Have Used") == std::string::npos);
}

HOI_TEST(MODPACK_009_the_report_does_not_depend_on_the_path) {
    // The same mod installed at a different absolute path must produce the same
    // report text, so nothing about the file system leaks into the load.
    const std::string source = pack_root("chain");
    const std::filesystem::path copy =
        std::filesystem::temp_directory_path() / "hoi4clone_test_modpack_copy";
    std::error_code ec;
    std::filesystem::remove_all(copy, ec);
    std::filesystem::create_directories(copy, ec);
    std::filesystem::copy(source, copy / "chain",
                          std::filesystem::copy_options::recursive, ec);

    PackLoad in_place(source);
    PackLoad relocated((copy / "chain").string());
    CHECK(in_place.ok);
    CHECK(relocated.ok);
    CHECK_EQ(in_place.report.text(), relocated.report.text());
    CHECK_EQ(in_place.report.load_order, relocated.report.load_order);
    std::filesystem::remove_all(copy, ec);
}

HOI_TEST(MODPACK_010_no_mods_is_byte_identical_to_the_shipped_content) {
    // An empty mods root must behave exactly like no --mods at all, because the
    // release gate's determinism check compares hashes across both.
    const std::filesystem::path empty_root =
        std::filesystem::temp_directory_path() / "hoi4clone_test_modpack_empty";
    std::error_code ec;
    std::filesystem::remove_all(empty_root, ec);
    std::filesystem::create_directories(empty_root, ec);

    const std::string scenario = repo_root() + "/data/scenarios/1936.json";
    Game plain;
    Game empty;
    std::string err;
    if (!Game::create(repo_root() + "/data", scenario, 4242, &plain, &err)) {
        ::hoi_test::fail(__FILE__, __LINE__, "create without mods failed: " + err);
    }
    std::vector<std::string> roots{empty_root.string()};
    if (!Game::create(repo_root() + "/data", scenario, 4242, &empty, &err, roots)) {
        ::hoi_test::fail(__FILE__, __LINE__, "create with an empty mods root failed: " + err);
    }
    CHECK_EQ(world_hash(plain), world_hash(empty));
    std::filesystem::remove_all(empty_root, ec);
}