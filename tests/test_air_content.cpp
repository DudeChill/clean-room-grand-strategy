// Air content and map support: scenario air bases and starting wings.
//
// These cases are hermetic: a hand-built Content plus a tiny map/scenario in a
// temporary directory, so a failure points at the loader rule, not at the shipped
// data. The shipped scenario (data/scenarios/1936.json) is exercised by the CLI
// acceptance run. Simulation behaviour (phase_air) lives in the air tests proper.

#include <filesystem>
#include <fstream>
#include <string>

#include "data/content.h"
#include "sim/world.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;

// Temporary directory holding a map + scenario pair, removed on destruction.
struct TempScenario {
    std::filesystem::path dir;

    TempScenario() {
        dir = std::filesystem::temp_directory_path() / "hoi4clone_test_air_content";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~TempScenario() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempScenario(const TempScenario&) = delete;
    TempScenario& operator=(const TempScenario&) = delete;

    void write(const std::string& name, const std::string& text) const {
        std::ofstream out(dir / name, std::ios::binary);
        out << text;
    }
    std::string path(const std::string& name) const { return (dir / name).string(); }
};

// Content with one fighter and one CAS model on top of the shared test content.
Content make_air_content() {
    Content c = hoi_test::make_test_content();
    hoi_test::add_equipment(c, "fighter_1", EquipmentCategory::Aircraft, 0.0, 0.0, 10.0, 0.0,
                            20.0, 450.0);
    hoi_test::add_equipment(c, "cas_1", EquipmentCategory::Aircraft, 0.0, 0.0, 11.0, 0.0, 20.0,
                            320.0);
    return c;
}

// Key lookups (the loader's key index is local, so tests search by name).
CountryId country_tagged(const World& w, const std::string& tag) {
    CountryId found;
    w.countries.for_each([&](CountryId id, const Country& c) {
        if (c.tag == tag) found = id;
    });
    return found;
}

ProvinceId province_named(const World& w, const std::string& name) {
    ProvinceId found;
    w.provinces.for_each([&](ProvinceId id, const Province& p) {
        if (p.name == name) found = id;
    });
    return found;
}

RegionId region_named(const World& w, const std::string& name) {
    RegionId found;
    w.regions.for_each([&](RegionId id, const Region& r) {
        if (r.name == name) found = id;
    });
    return found;
}

// Four land provinces across two regions:
//   p1,p2 in region r1 (state s1 / s2), p3,p4 in region r2 (state s3).
// p1-p2-p3-p4 form a line. p2 declares air_base 4 in the map.
const char* kMap = R"({
  "regions": [
    {"key": "r1", "name": "Region One"},
    {"key": "r2", "name": "Region Two"}
  ],
  "states": [
    {"key": "s1", "name": "State One", "region": "r1", "building_slots": 10},
    {"key": "s2", "name": "State Two", "region": "r1", "building_slots": 10},
    {"key": "s3", "name": "State Three", "region": "r2", "building_slots": 10}
  ],
  "provinces": [
    {"key": "p1", "name": "P1", "state": "s1", "region": "r1", "terrain": "plains",
     "x": 0, "y": 0, "victory_points": 5, "population": 100000.0, "adj": ["p2"]},
    {"key": "p2", "name": "P2", "state": "s2", "region": "r1", "terrain": "plains",
     "x": 1, "y": 0, "victory_points": 1, "population": 50000.0, "air_base": 4,
     "adj": ["p1", "p3"]},
    {"key": "p3", "name": "P3", "state": "s3", "region": "r2", "terrain": "plains",
     "x": 2, "y": 0, "victory_points": 1, "population": 50000.0, "adj": ["p2", "p4"]},
    {"key": "p4", "name": "P4", "state": "s3", "region": "r2", "terrain": "plains",
     "x": 3, "y": 0, "victory_points": 3, "population": 80000.0, "adj": ["p3"]}
  ]
})";

// Alpha owns s1 and s2 (capital s1) and starts with a fighter and a CAS wing on its
// capital. Beta owns s3 (capital s3) and starts with none.
const char* kScenario = R"({
  "name": "air_test",
  "map": "map.json",
  "start_date": "1936-01-01",
  "seed": 7,
  "countries": [
    {"tag": "AAA", "name": "Alpha", "ideology": "democratic", "capital_state": "s1",
     "states": ["s1", "s2"], "civilian_factories": 2, "military_factories": 1,
     "dockyards": 0,
     "stockpile": {"fighter_1": 100, "cas_1": 50},
     "wings": [
       {"equipment": "fighter_1", "province": "p1", "planes": 100},
       {"equipment": "cas_1", "province": "p1", "planes": 50}
     ]},
    {"tag": "BBB", "name": "Beta", "ideology": "neutrality", "capital_state": "s3",
     "states": ["s3"], "civilian_factories": 1, "military_factories": 0,
     "dockyards": 0}
  ],
  "factions": [],
  "wars": []
})";

}  // namespace

HOI_TEST(air_content_scenario_forms_wings_and_draws_stockpile) {
    TempScenario tmp;
    tmp.write("map.json", kMap);
    tmp.write("scenario.json", kScenario);
    Content content = make_air_content();

    World world;
    std::string warnings;
    CHECK(load_scenario(tmp.path("scenario.json"), content, &world, &warnings));
    CHECK(warnings.empty());

    const CountryId aaa = country_tagged(world, "AAA");
    const CountryId bbb = country_tagged(world, "BBB");
    CHECK(aaa.valid());
    CHECK(bbb.valid());
    const Country* a = world.country(aaa);
    const Country* b = world.country(bbb);
    CHECK(a != nullptr && b != nullptr);

    // Alpha formed both wings, based on its capital, drawing the aircraft from the
    // stockpile rather than conjuring them.
    CHECK_EQ(a->wings.size(), static_cast<size_t>(2));
    CHECK_EQ(b->wings.size(), static_cast<size_t>(0));
    const EquipmentId fighter = content.equipment_id("fighter_1");
    const EquipmentId cas = content.equipment_id("cas_1");
    const AirWing* fighter_wing = world.wing(a->wings[0]);
    const AirWing* cas_wing = world.wing(a->wings[1]);
    CHECK(fighter_wing != nullptr && cas_wing != nullptr);
    CHECK(fighter_wing->equipment == fighter);
    CHECK(cas_wing->equipment == cas);
    CHECK_EQ(fighter_wing->planes, 100);
    CHECK_EQ(fighter_wing->max_planes, 100);
    CHECK_EQ(cas_wing->planes, 50);
    CHECK_EQ(fighter_wing->country, aaa);
    const ProvinceId capital = province_named(world, "P1");
    CHECK(fighter_wing->base == capital);
    CHECK(cas_wing->base == capital);
    const RegionId r1 = region_named(world, "Region One");
    CHECK(fighter_wing->region == r1);
    // The stockpile paid for the planes.
    CHECK_NEAR(a->equipment_stockpile[fighter.v], 0.0, 1e-12);
    CHECK_NEAR(a->equipment_stockpile[cas.v], 0.0, 1e-12);

    // Air bases: the declared map level survives; every capital gets at least 2 and
    // the largest industrial state at least 1.
    CHECK_EQ(world.province(province_named(world, "P2"))->air_base, 4);
    CHECK(world.province(province_named(world, "P1"))->air_base >= 2);
    CHECK(world.province(province_named(world, "P4"))->air_base >= 2);
}

HOI_TEST(air_content_scenario_wings_warn_and_clamp_to_stockpile) {
    TempScenario tmp;
    tmp.write("map.json", kMap);
    tmp.write("scenario.json", R"({
      "name": "air_test",
      "map": "map.json",
      "start_date": "1936-01-01",
      "seed": 7,
      "countries": [
        {"tag": "AAA", "name": "Alpha", "ideology": "democratic", "capital_state": "s1",
         "states": ["s1"], "civilian_factories": 1, "military_factories": 0,
         "dockyards": 0,
         "stockpile": {"fighter_1": 30},
         "wings": [
           {"equipment": "fighter_1", "province": "p1", "planes": 100},
           {"equipment": "fighter_1", "province": "missing", "planes": 10},
           {"equipment": "no_such_model", "province": "p1", "planes": 10},
           {"equipment": "cas_1", "province": "p1", "planes": 0}
         ]}
      ],
      "factions": [],
      "wars": []
    })");
    Content content = make_air_content();

    World world;
    std::string warnings;
    CHECK(load_scenario(tmp.path("scenario.json"), content, &world, &warnings));
    // Exactly one wing is usable; the stockpile clamps the request and the other
    // three entries are reported, not silently dropped.
    const CountryId aaa = country_tagged(world, "AAA");
    const Country* a = world.country(aaa);
    CHECK(a != nullptr);
    CHECK_EQ(a->wings.size(), static_cast<size_t>(1));
    const AirWing* wing = world.wing(a->wings[0]);
    CHECK(wing != nullptr);
    CHECK_EQ(wing->planes, 30);
    CHECK_EQ(wing->max_planes, 100);
    CHECK_NEAR(a->equipment_stockpile[content.equipment_id("fighter_1").v], 0.0, 1e-12);
    CHECK(warnings.find("missing") != std::string::npos);
    CHECK(warnings.find("no_such_model") != std::string::npos);
    CHECK(warnings.find("non-positive") != std::string::npos);
}

HOI_TEST(air_content_scenario_load_is_deterministic) {
    TempScenario tmp;
    tmp.write("map.json", kMap);
    tmp.write("scenario.json", kScenario);
    Content content = make_air_content();

    auto load = [&]() {
        World world;
        std::string warnings;
        CHECK(load_scenario(tmp.path("scenario.json"), content, &world, &warnings));
        return world;
    };
    World first = load();
    World second = load();

    CHECK_EQ(first.air_wings.size(), second.air_wings.size());
    CHECK_EQ(first.provinces.size(), second.provinces.size());
    first.air_wings.for_each([&](AirWingId id, const AirWing& w) {
        const AirWing* other = second.wing(id);
        CHECK(other != nullptr);
        CHECK_EQ(w.country, other->country);
        CHECK_EQ(w.equipment, other->equipment);
        CHECK_EQ(w.planes, other->planes);
        CHECK_EQ(w.base, other->base);
        CHECK_EQ(w.region, other->region);
        CHECK_EQ(w.mission, other->mission);
    });
    first.provinces.for_each([&](ProvinceId id, const Province& p) {
        const Province* other = second.province(id);
        CHECK(other != nullptr);
        CHECK_EQ(p.air_base, other->air_base);
    });
}