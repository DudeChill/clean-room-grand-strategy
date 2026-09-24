// Content database tests: real files in a temporary data root, so the loader's
// file handling, key resolution and error reporting are exercised end to end.

#include <filesystem>
#include <fstream>
#include <string>

#include "core/json.h"
#include "data/content.h"
#include "test.h"

namespace {

using namespace hoi;

// Temporary data root with the six expected files; removed on destruction.
struct TempData {
    std::filesystem::path dir;
    std::filesystem::path common;
    std::string root;

    TempData() {
        dir = std::filesystem::temp_directory_path() / "hoi4clone_test_content";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        common = dir / "common";
        std::filesystem::create_directories(common);
        root = dir.string();
    }
    ~TempData() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempData(const TempData&) = delete;
    TempData& operator=(const TempData&) = delete;

    void write(const std::string& name, const std::string& text) const {
        std::ofstream out(common / name, std::ios::binary);
        out << text;
    }
    std::string path(const std::string& name) const { return (common / name).string(); }
};

const char* kConstants = R"({
  "ic_per_military_factory": 9.0,
  "research_base_days": 80.0,
  "air_base_capacity_per_level": 250.0,
  "air_cas_effect": 0.30
})";

const char* kEquipment = R"({
  "equipment": [
    {"key": "infantry_equipment", "name": "Infantry Equipment", "category": "infantry",
     "is_archetype": true, "organization": 60.0, "max_strength": 25.0,
     "speed": 4.0, "build_cost": 0.5},
    {"key": "infantry_equipment_1", "name": "Infantry Equipment I", "category": "infantry",
     "archetype": "infantry_equipment", "year": 1936, "soft_attack": 3.0,
     "defense": 4.0, "organization": 60.0, "max_strength": 25.0, "speed": 4.0,
     "build_cost": 0.43, "manpower": 1000.0, "resources": {"steel": 2}}
  ]
})";

const char* kBuildings = R"({
  "buildings": [
    {"key": "civilian_factory", "name": "Civilian Factory", "kind": "civilian_factory",
     "base_cost": 10800.0, "per_state": true, "max_level": 12}
  ]
})";

const char* kTechnologies = R"({
  "technologies": [
    {"key": "infantry_weapons", "name": "Infantry Weapons", "category": "infantry",
     "year": 1936, "cost_days": 80.0, "unlock_equipment": ["infantry_equipment_1"],
     "modifiers": {"DivisionAttack": 0.05}},
    {"key": "improved_infantry_weapons", "name": "Improved Infantry Weapons",
     "category": "infantry", "year": 1940, "cost_days": 40.0,
     "prerequisites": ["infantry_weapons"], "modifiers": {"division_attack": 0.10}}
  ]
})";

const char* kLaws = R"({
  "laws": [
    {"key": "conscription_volunteer", "name": "Volunteer Only", "kind": 0, "level": 0,
     "cost": 0.0, "modifiers": {"RecruitablePopulation": -0.01}},
    {"key": "conscription_limited", "name": "Limited Conscription", "kind": 0, "level": 1,
     "cost": 50.0, "requires_law": "conscription_volunteer", "requires_level": 0,
     "modifiers": {"RecruitablePopulation": 0.005}}
  ]
})";

const char* kTemplates = R"({
  "templates": [
    {"key": "infantry_division", "name": "Infantry Division", "train_days": 90.0,
     "battalions": [
       {"equipment": "infantry_equipment_1", "count": 3},
       {"equipment": "infantry_equipment_1", "count": 1, "support": true}
     ]}
  ]
})";

void write_standard(const TempData& tmp) {
    tmp.write("constants.json", kConstants);
    tmp.write("equipment.json", kEquipment);
    tmp.write("buildings.json", kBuildings);
    tmp.write("technologies.json", kTechnologies);
    tmp.write("laws.json", kLaws);
    tmp.write("templates.json", kTemplates);
}

}  // namespace

HOI_TEST(content_loads_all_files) {
    TempData tmp;
    write_standard(tmp);

    Content content;
    std::string err;
    CHECK(load_content(tmp.root, &content, &err));
    CHECK(content.load_errors.empty());
    CHECK(err.empty());

    // Constants are overridden by key, others keep their struct defaults.
    CHECK_NEAR(content.constants.ic_per_military_factory, 9.0, 1e-12);
    CHECK_NEAR(content.constants.research_base_days, 80.0, 1e-12);
    CHECK_NEAR(content.constants.ic_per_civilian_factory, 5.0, 1e-12);

    // Air constants load by key; keys absent from the file keep their default.
    CHECK_NEAR(content.constants.air_base_capacity_per_level, 250.0, 1e-12);
    CHECK_NEAR(content.constants.air_cas_effect, 0.30, 1e-12);
    CHECK_NEAR(content.constants.air_sortie_hours, 6.0, 1e-12);

    // Equipment by key, archetype validation, resource costs.
    const EquipmentId eq = content.equipment_id("infantry_equipment_1");
    CHECK(eq.valid());
    const EquipmentDef* def = content.equipment_def(eq);
    CHECK(def != nullptr);
    CHECK_EQ(def->archetype, std::string("infantry_equipment"));
    CHECK(!def->is_archetype);
    CHECK(content.equipment_id("infantry_equipment").valid());
    CHECK_NEAR(def->resources[static_cast<int>(Resource::Steel)], 2.0, 1e-12);
    CHECK_NEAR(def->resources[static_cast<int>(Resource::Rubber)], 0.0, 1e-12);
    CHECK(!content.equipment_id("does_not_exist").valid());

    // Buildings resolve through the engine's kind names.
    CHECK_EQ(content.buildings.size(), static_cast<size_t>(1));
    CHECK(content.buildings[0].kind == BuildingKind::CivilianFactory);
    CHECK_EQ(content.buildings[0].max_level, 12);

    // Technologies: parameters, prerequisites and unlocks.
    const TechId weapons = content.tech_id("infantry_weapons");
    const TechId improved = content.tech_id("improved_infantry_weapons");
    CHECK(weapons.valid() && improved.valid());
    const TechDef* improved_def = content.tech_def(improved);
    CHECK(improved_def != nullptr);
    CHECK_EQ(improved_def->prerequisites.size(), static_cast<size_t>(1));
    CHECK_EQ(improved_def->prerequisites[0], weapons);
    CHECK_EQ(content.tech_def(weapons)->unlock_equipment.size(), static_cast<size_t>(1));
    CHECK_EQ(content.tech_def(weapons)->unlock_equipment[0],
             std::string("infantry_equipment_1"));
    CHECK_NEAR(content.tech_def(weapons)
                   ->modifiers.get(ModifierKind::DivisionAttack),
               0.05, 1e-12);
    // Name matching ignores case and separators.
    CHECK_NEAR(improved_def->modifiers.get(ModifierKind::DivisionAttack), 0.10, 1e-12);

    // Laws are addressable by key, with kind/level/cost and prerequisites.
    const LawDef* law = content.law("conscription_limited");
    CHECK(law != nullptr);
    CHECK_EQ(law->kind, 0);
    CHECK_EQ(law->level, 1);
    CHECK_NEAR(law->cost, 50.0, 1e-12);
    CHECK_EQ(law->requires_law, std::string("conscription_volunteer"));
    CHECK_EQ(content.laws.size(), static_cast<size_t>(2));

    // Templates: battalion composition plus derived statistics.
    const TemplateId tid = content.template_id("infantry_division");
    CHECK(tid.valid());
    const DivisionTemplate* tmpl = content.template_def(tid);
    CHECK(tmpl != nullptr);
    CHECK_EQ(tmpl->battalions.size(), static_cast<size_t>(2));
    CHECK_EQ(tmpl->battalion_count(), 3);  // support companies are not line battalions
    CHECK_NEAR(tmpl->max_organization, 60.0, 1e-12);
    // Four battalions (three line, one support) at 1000 persons each.
    CHECK_NEAR(tmpl->manpower, 4000.0, 1e-12);
    CHECK_GT(tmpl->combat_width, 0.0);
}

HOI_TEST(content_reads_air_stats_and_rejects_negative) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("equipment.json", R"({
      "equipment": [
        {"key": "fighter_1", "name": "Fighter I", "category": "aircraft",
         "air_attack": 12.0, "air_defence": 8.0, "ground_attack": 24.0,
         "agility": 40.0, "range": 3.0},
        {"key": "bad_plane", "name": "Bad", "category": "aircraft",
         "air_defence": -1.0, "ground_attack": -2.0, "agility": -3.0, "range": -4.0}
      ]
    })");

    Content content;
    std::string err;
    CHECK(load_content(tmp.root, &content, &err));

    const EquipmentDef* ok = content.equipment_def(content.equipment_id("fighter_1"));
    CHECK(ok != nullptr);
    CHECK_NEAR(ok->air_attack, 12.0, 1e-12);
    CHECK_NEAR(ok->air_defence, 8.0, 1e-12);
    CHECK_NEAR(ok->ground_attack, 24.0, 1e-12);
    CHECK_NEAR(ok->agility, 40.0, 1e-12);
    CHECK_NEAR(ok->range, 3.0, 1e-12);

    // Negative air statistics are reported by name and clamped to zero so the
    // invalid value can never reach the simulation.
    const EquipmentDef* bad = content.equipment_def(content.equipment_id("bad_plane"));
    CHECK(bad != nullptr);
    CHECK_NEAR(bad->air_defence, 0.0, 1e-12);
    CHECK_NEAR(bad->ground_attack, 0.0, 1e-12);
    CHECK_NEAR(bad->agility, 0.0, 1e-12);
    CHECK_NEAR(bad->range, 0.0, 1e-12);
    for (const char* field : {"air_defence", "ground_attack", "agility", "range"}) {
        bool reported = false;
        const std::string needle = std::string("bad_plane: negative ") + field;
        for (const std::string& e : content.load_errors) {
            if (e.find(needle) != std::string::npos) reported = true;
        }
        CHECK(reported);
    }
}

HOI_TEST(content_rejects_duplicate_equipment_key) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("equipment.json", R"({
      "equipment": [
        {"key": "infantry_equipment_1", "category": "infantry", "is_archetype": true},
        {"key": "infantry_equipment_1", "category": "infantry"}
      ]
    })");

    Content content;
    std::string err;
    CHECK(!load_content(tmp.root, &content, &err));  // duplicate ids are fatal
    CHECK(err.find("duplicate") != std::string::npos);
    bool reported = false;
    for (const std::string& e : content.load_errors) {
        if (e.find("duplicate") != std::string::npos) reported = true;
    }
    CHECK(reported);
}

HOI_TEST(content_reports_unknown_modifier_and_archetype) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("technologies.json", R"({
      "technologies": [
        {"key": "bad_tech", "name": "Bad Tech", "category": "industry", "year": 1936,
         "cost_days": 10.0, "modifiers": {"NotAModifier": 0.5}}
      ]
    })");
    tmp.write("equipment.json", R"({
      "equipment": [
        {"key": "orphan_model", "category": "infantry", "archetype": "missing_archetype"}
      ]
    })");

    Content content;
    std::string err;
    // Unknown names are data smells, not fatal: the rest of the file still loads.
    CHECK(load_content(tmp.root, &content, &err));
    bool modifier_error = false;
    bool archetype_error = false;
    for (const std::string& e : content.load_errors) {
        if (e.find("unknown modifier NotAModifier") != std::string::npos &&
            e.find("bad_tech") != std::string::npos) {
            modifier_error = true;
        }
        if (e.find("unknown archetype missing_archetype") != std::string::npos &&
            e.find("orphan_model") != std::string::npos) {
            archetype_error = true;
        }
    }
    CHECK(modifier_error);
    CHECK(archetype_error);
}

HOI_TEST(content_missing_file_is_fatal) {
    TempData tmp;
    write_standard(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp.common / "laws.json", ec);

    Content content;
    std::string err;
    CHECK(!load_content(tmp.root, &content, &err));
    CHECK(err.find("laws.json") != std::string::npos);
}

HOI_TEST(content_json_parse_and_parse_file) {
    // Constants from a JSON string.
    std::string err;
    const Json j = Json::parse(R"({"ic_per_military_factory": 1.5, "nope": true})", &err);
    CHECK(err.empty());
    CHECK(j.is_object());
    const SimConstants constants = SimConstants::from_json(j);
    CHECK_NEAR(constants.ic_per_military_factory, 1.5, 1e-12);
    CHECK_NEAR(constants.ic_per_civilian_factory, 5.0, 1e-12);

    // The same document via a file path.
    TempData tmp;
    write_standard(tmp);
    Json from_file;
    CHECK(Json::parse_file(tmp.path("constants.json"), &from_file, &err));
    CHECK(from_file.has("ic_per_military_factory"));
    const SimConstants file_constants = SimConstants::from_json(from_file);
    CHECK_NEAR(file_constants.ic_per_military_factory, 9.0, 1e-12);

    Json equipment;
    CHECK(Json::parse_file(tmp.path("equipment.json"), &equipment, &err));
    CHECK_EQ(equipment["equipment"].size(), static_cast<size_t>(2));
    CHECK_EQ(equipment["equipment"][1]["key"].as_string(),
             std::string("infantry_equipment_1"));
    CHECK_EQ(equipment["missing"].as_string("fallback"), std::string("fallback"));
}

namespace {

// The first diagnostic containing `needle`, or "" when none does.
std::string find_error(const Content& content, const std::string& needle) {
    for (const std::string& e : content.load_errors) {
        if (e.find(needle) != std::string::npos) return e;
    }
    return std::string();
}

}  // namespace

HOI_TEST(content_intelligence_is_optional) {
    TempData tmp;
    write_standard(tmp);  // no intelligence.json at all

    Content content;
    std::string err;
    CHECK(load_content(tmp.root, &content, &err));
    CHECK(content.load_errors.empty());
    CHECK(content.operations.empty());
    CHECK(content.agency_upgrades.empty());
}

HOI_TEST(content_loads_intelligence_tables) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("intelligence.json", R"({
      "operations": [
        {"key": "build_network", "name": "Build Network", "kind": "build_network",
         "days": 30, "pp_cost": 25, "risk": 0.05, "network_gain": 12},
        {"key": "steal_tech", "name": "Steal Tech", "kind": "Steal Tech",
         "days": 60, "pp_cost": 70, "network_required": 25, "risk": 0.15,
         "research_days": 120, "available": {"at_war": true}},
        {"key": "destabilise", "name": "Destabilise", "kind": "destabilise",
         "days": 70, "pp_cost": 65, "network_required": 30, "risk": 0.20,
         "stability_delta": -0.20, "ideology_shift": -0.10, "effect_days": 120}
      ],
      "agency_upgrades": [
        {"key": "academy", "name": "Academy", "year": 1936, "pp_cost": 120,
         "network_growth": 0.20},
        {"key": "residents", "name": "Residents", "year": 1941, "pp_cost": 220,
         "network_growth": 0.40, "requires_upgrades": ["academy"]}
      ]
    })");

    Content content;
    std::string err;
    CHECK(load_content(tmp.root, &content, &err));
    CHECK(content.load_errors.empty());

    CHECK_EQ(content.operations.size(), static_cast<size_t>(3));
    CHECK_EQ(content.agency_upgrades.size(), static_cast<size_t>(2));

    const uint32_t op = content.operation_id("build_network");
    CHECK(op != 0xFFFFFFFFu);
    const OperationDef* o = content.operation(op);
    CHECK(o != nullptr);
    CHECK_EQ(o->index, 0u);
    CHECK(o->kind == OperationKind::BuildNetwork);
    CHECK_EQ(o->days, 30);
    CHECK_NEAR(o->pp_cost, 25.0, 1e-12);
    CHECK_NEAR(o->risk, 0.05, 1e-12);
    CHECK_NEAR(o->network_gain, 12.0, 1e-12);

    // Kind matching ignores case and separators, like every other data name.
    const OperationDef* s = content.operation(content.operation_id("steal_tech"));
    CHECK(s != nullptr);
    CHECK(s->kind == OperationKind::StealTech);
    CHECK_NEAR(s->network_required, 25.0, 1e-12);
    CHECK_NEAR(s->research_days, 120.0, 1e-12);
    CHECK(s->available.is_object());

    // Signed deltas keep their sign: negative means the operation hurts the target.
    const OperationDef* d = content.operation(content.operation_id("destabilise"));
    CHECK(d != nullptr);
    CHECK(d->kind == OperationKind::Destabilise);
    CHECK_NEAR(d->stability_delta, -0.20, 1e-12);
    CHECK_NEAR(d->ideology_shift, -0.10, 1e-12);

    const uint32_t up = content.agency_upgrade_id("residents");
    CHECK(up != 0xFFFFFFFFu);
    const AgencyUpgradeDef* u = content.agency_upgrade(up);
    CHECK(u != nullptr);
    CHECK_EQ(u->index, 1u);
    CHECK_EQ(u->requires_upgrades.size(), static_cast<size_t>(1));
    CHECK_EQ(u->requires_upgrades[0], std::string("academy"));
    CHECK_NEAR(u->network_growth, 0.40, 1e-12);
    CHECK_EQ(content.agency_upgrade(content.agency_upgrade_id("academy"))->year, 1936);
}

HOI_TEST(content_intelligence_reports_and_skips_bad_entries) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("intelligence.json", R"({
      "operations": [
        {"key": "dup", "kind": "build_network", "days": 30},
        {"key": "dup", "kind": "build_network", "days": 30},
        {"key": "bad_kind", "kind": "mind_control", "days": 30},
        {"key": "no_kind", "days": 30},
        {"key": "bad_days", "kind": "steal_tech", "days": 0},
        {"key": "bad_network", "kind": "steal_tech", "days": 30, "network_required": 150},
        {"key": "bad_risk", "kind": "destabilise", "days": 30, "risk": 1.5},
        {"key": "bad_cost", "kind": "sabotage_industry", "days": 30, "pp_cost": -5},
        {"key": "bad_effect", "kind": "steal_tech", "days": 30, "research_days": -1},
        {"key": "bad_trigger", "kind": "build_network", "days": 30,
         "available": {"not_a_trigger": true}}
      ],
      "agency_upgrades": [
        {"key": "orphan", "year": 1936, "pp_cost": 10, "requires_upgrades": ["missing"]},
        {"key": "cycle_b", "year": 1936, "pp_cost": 10, "requires_upgrades": ["cycle_c"]},
        {"key": "cycle_c", "year": 1936, "pp_cost": 10, "requires_upgrades": ["cycle_b"]},
        {"key": "dup_up", "year": 1936, "pp_cost": 10},
        {"key": "dup_up", "year": 1936, "pp_cost": 10},
        {"key": "bad_year", "year": 1900, "pp_cost": 10},
        {"key": "bad_counter", "year": 1936, "pp_cost": 10, "counter_intel": 1.5}
      ]
    })");

    Content content;
    std::string err;
    CHECK(load_content(tmp.root, &content, &err));  // bad entries are warnings, not fatal

    // Every bad operation is skipped; only the first "dup" survives.
    CHECK_EQ(content.operations.size(), static_cast<size_t>(1));
    CHECK_EQ(content.operations[0].key, std::string("dup"));
    CHECK(content.operation_id("bad_kind") == 0xFFFFFFFFu);
    CHECK(content.operation_id("bad_trigger") == 0xFFFFFFFFu);

    // The unknown-reference upgrade and both cycle members are dropped; the first
    // duplicate survives and the malformed entries never enter the table.
    CHECK_EQ(content.agency_upgrades.size(), static_cast<size_t>(1));
    CHECK_EQ(content.agency_upgrades[0].key, std::string("dup_up"));
    CHECK(content.agency_upgrade_id("orphan") == 0xFFFFFFFFu);
    CHECK(content.agency_upgrade_id("cycle_b") == 0xFFFFFFFFu);
    CHECK(content.agency_upgrade_id("bad_year") == 0xFFFFFFFFu);

    // One diagnostic per error class, addressed file:key: reason.
    CHECK(!find_error(content, "duplicate operation key").empty());
    CHECK(!find_error(content, "bad_kind: unknown operation kind mind_control").empty());
    CHECK(!find_error(content, "no_kind: missing kind").empty());
    CHECK(!find_error(content, "bad_days: days must be a positive number").empty());
    CHECK(!find_error(content, "bad_network: network_required must be between 0 and 100")
               .empty());
    CHECK(!find_error(content, "bad_risk: risk must be between 0 and 1").empty());
    CHECK(!find_error(content, "bad_cost: negative pp_cost").empty());
    CHECK(!find_error(content, "bad_effect: negative research_days").empty());
    CHECK(!find_error(content, "bad_trigger: available.not_a_trigger: unknown trigger key")
               .empty());
    CHECK(!find_error(content, "orphan: unknown requires_upgrades missing").empty());
    CHECK(!find_error(content, "requires_upgrades cycle among cycle_b, cycle_c").empty());
    CHECK(!find_error(content, "duplicate agency upgrade key").empty());
    CHECK(!find_error(content, "bad_year: year must be 1936 or later").empty());
    CHECK(!find_error(content, "bad_counter: counter_intel must be between 0 and 1").empty());
}

HOI_TEST(content_intelligence_malformed_file_is_fatal) {
    TempData tmp;
    write_standard(tmp);
    tmp.write("intelligence.json", R"({"operations": {"key": "not_an_array"}})");

    Content content;
    std::string err;
    CHECK(!load_content(tmp.root, &content, &err));
    CHECK(err.find("operations: expected an array") != std::string::npos);
}
