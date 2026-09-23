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
  "research_base_days": 80.0
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
