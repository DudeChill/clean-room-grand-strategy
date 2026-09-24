// Equipment designer tests (spec section 50).
//
// Every world is hand-built through the shared helpers in test_util.h plus the
// fixture below, so the expected numbers are arithmetic rather than fixtures from
// data files. The tests pin the rules the designer must not silently lose:
//
//  * every component field is observable on the produced EquipmentDef;
//  * build_cost is scaled by the product of the cost multipliers and resources add;
//  * reliability clamps to [0.1, 1.0], every other statistic to >= 0;
//  * a component locked by year, by trigger or by technology cannot be fitted;
//  * illegal, duplicate-slot and unavailable-archetype designs are rejected with no
//    state change, and duplicate names are rejected;
//  * keys are deterministic ("<tag>_<category>_<n>") and collision-free;
//  * a design's equipment can be built by a production line into the stockpile;
//  * the AI creates a design through the command queue within 30 simulated days;
//  * two identical runs hash identically.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/types.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/design.h"
#include "sim/industry.h"
#include "sim/research.h"
#include "sim/units.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

constexpr int STEEL = static_cast<int>(Resource::Steel);
constexpr int OIL = static_cast<int>(Resource::Oil);

// A small armor designer world: one armor archetype, one producible model, and six
// components that exercise every gate and every stat field.
struct DesignWorld {
    MiniWorld m;
    CountryId a;
    CountryId b;
    EquipmentId archetype;
    EquipmentId existing;
    uint32_t kit = 0;          // Armor, all fields set, year 1936
    uint32_t gun = 0;          // Weapon, cost multiplier + resource
    uint32_t plate = 0;        // Armor (duplicate-slot probe), year 1936
    uint32_t late_engine = 0;  // Engine, year 1940 (year gate)
    uint32_t flag_engine = 0;  // Engine, trigger gate
    uint32_t tech_armor = 0;   // Special, technology gate
    uint32_t junk = 0;         // negative stats for the clamp test
    uint32_t infantry_bit = 0;  // wrong category for the archetype
    TechId armor_tech;
};

uint32_t push_component(Content& c, const ComponentDef& in) {
    ComponentDef comp = in;
    comp.index = static_cast<uint32_t>(c.components.size());
    c.component_index[comp.key] = comp.index;
    c.components.push_back(comp);
    return comp.index;
}

DesignWorld make_design_world() {
    DesignWorld f;
    f.m = make_mini_world();
    f.a = f.m.a;
    f.b = f.m.b;
    Game& g = f.m.game;
    Content& c = g.content;
    g.world.date = GameDate{1936, 1, 1, 0};

    EquipmentDef arch;
    arch.key = "armor_archetype";
    arch.name = "Armor Archetype";
    arch.category = EquipmentCategory::Armor;
    arch.archetype = "armor_archetype";
    arch.is_archetype = true;
    arch.year = 1936;
    arch.soft_attack = 10.0;
    arch.hard_attack = 20.0;
    arch.armor = 30.0;
    arch.piercing = 15.0;
    arch.reliability = 0.8;
    arch.speed = 8.0;
    arch.max_strength = 30.0;
    arch.organization = 30.0;
    arch.defense = 12.0;
    arch.breakthrough = 14.0;
    arch.visibility = 1.0;
    arch.build_cost = 10.0;
    arch.resources[STEEL] = 1.0;
    arch.manpower = 500.0;
    arch.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    c.equipment.push_back(arch);
    c.equipment_by_key[arch.key] = arch.id;
    f.archetype = arch.id;

    EquipmentDef ex;
    ex.key = "armor_1";
    ex.name = "Armor I";
    ex.category = EquipmentCategory::Armor;
    ex.archetype = arch.key;
    ex.year = 1936;
    ex.soft_attack = 6.0;
    ex.hard_attack = 10.0;
    ex.armor = 15.0;
    ex.piercing = 8.0;
    ex.reliability = 0.8;
    ex.speed = 6.0;
    ex.max_strength = 25.0;
    ex.organization = 25.0;
    ex.defense = 8.0;
    ex.breakthrough = 8.0;
    ex.build_cost = 12.0;
    ex.resources[STEEL] = 1.0;
    ex.manpower = 500.0;
    ex.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    c.equipment.push_back(ex);
    c.equipment_by_key[ex.key] = ex.id;
    f.existing = ex.id;

    ComponentDef kit;
    kit.key = "kit";
    kit.slot = ComponentSlot::Armor;
    kit.category = EquipmentCategory::Armor;
    kit.year = 1936;
    kit.soft_attack = 1.0;
    kit.hard_attack = 2.0;
    kit.air_attack = 3.0;
    kit.air_defence = 4.0;
    kit.ground_attack = 5.0;
    kit.agility = 6.0;
    kit.armor = 7.0;
    kit.piercing = 8.0;
    kit.defense = 18.0;
    kit.breakthrough = 19.0;
    kit.hardness = 0.05;
    kit.max_strength = 9.0;
    kit.organization = 10.0;
    kit.speed = 11.0;
    kit.reliability = 0.1;
    kit.range = 12.0;
    kit.detection = 13.0;
    kit.sub_detection = 14.0;
    kit.naval_attack = 15.0;
    kit.torpedo_attack = 16.0;
    kit.visibility = 17.0;
    kit.build_cost_add = 1.0;
    kit.cost_multiplier = 1.5;
    kit.resources[STEEL] = 0.5;
    kit.resources[OIL] = 0.25;
    kit.fuel_use = 0.1;
    kit.supply_use = 0.2;
    kit.manpower = 100.0;
    f.kit = push_component(c, kit);

    ComponentDef gun;
    gun.key = "gun";
    gun.slot = ComponentSlot::Weapon;
    gun.category = EquipmentCategory::Armor;
    gun.year = 1936;
    gun.hard_attack = 15.0;
    gun.piercing = 20.0;
    gun.reliability = -0.15;
    gun.build_cost_add = 2.0;
    gun.cost_multiplier = 1.2;
    gun.resources[STEEL] = 1.0;
    f.gun = push_component(c, gun);

    ComponentDef plate;
    plate.key = "plate";
    plate.slot = ComponentSlot::Armor;
    plate.category = EquipmentCategory::Armor;
    plate.year = 1936;
    plate.armor = 5.0;
    f.plate = push_component(c, plate);

    ComponentDef late_engine;
    late_engine.key = "late_engine";
    late_engine.slot = ComponentSlot::Engine;
    late_engine.category = EquipmentCategory::Armor;
    late_engine.year = 1940;
    late_engine.speed = 4.0;
    f.late_engine = push_component(c, late_engine);

    ComponentDef flag_engine;
    flag_engine.key = "flag_engine";
    flag_engine.slot = ComponentSlot::Engine;
    flag_engine.category = EquipmentCategory::Armor;
    flag_engine.year = 1936;
    flag_engine.speed = 2.0;
    flag_engine.available = Json::object();
    flag_engine.available.set("has_country_flag", Json("engine_ok"));
    f.flag_engine = push_component(c, flag_engine);

    ComponentDef tech_armor;
    tech_armor.key = "tech_armor";
    tech_armor.slot = ComponentSlot::Special;
    tech_armor.category = EquipmentCategory::Armor;
    tech_armor.year = 1936;
    tech_armor.armor = 5.0;
    f.tech_armor = push_component(c, tech_armor);

    ComponentDef junk;
    junk.key = "junk";
    junk.slot = ComponentSlot::Special;
    junk.category = EquipmentCategory::Armor;
    junk.year = 1936;
    junk.soft_attack = -500.0;
    junk.resources[STEEL] = -999.0;
    junk.build_cost_add = -999.0;
    f.junk = push_component(c, junk);

    ComponentDef infantry_bit;
    infantry_bit.key = "infantry_bit";
    infantry_bit.slot = ComponentSlot::Armor;
    infantry_bit.category = EquipmentCategory::Infantry;
    infantry_bit.year = 1936;
    infantry_bit.armor = 1.0;
    f.infantry_bit = push_component(c, infantry_bit);

    TechDef tech;
    tech.key = "armor_tech";
    tech.name = "Armor Tech";
    tech.id = TechId(0);
    tech.unlock_equipment = {"tech_armor"};
    c.techs.push_back(tech);
    c.tech_by_key[tech.key] = tech.id;
    f.armor_tech = tech.id;

    // Keep every country's stockpile table the same width as Content::equipment now
    // that the fixture appended models.
    g.world.countries.for_each([&](CountryId, Country& country) {
        country.equipment_stockpile.assign(c.equipment.size(), 0.0);
    });
    return f;
}

bool has_design_event(const Game& g) {
    for (const SimEvent& e : g.events) {
        if (e.kind == "design") return true;
    }
    return false;
}

bool has_create_design_command(const Game& g, CountryId country) {
    for (const CommandRecord& rec : g.log.records) {
        if (rec.command.type == CommandType::CreateEquipmentDesign &&
            rec.command.country == country && rec.result == CommandResult::Applied) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ formula ---

HOI_TEST(design_compute_applies_every_component_field) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    const EquipmentDef* base = g.content.equipment_def(f.archetype);
    const ComponentDef* kit = g.content.component(f.kit);
    CHECK(base != nullptr && kit != nullptr);

    const EquipmentDef out =
        design_compute(g, f.archetype, {{ComponentSlot::Armor, f.kit}}, "k", "n");

    CHECK_NEAR(out.soft_attack, base->soft_attack + kit->soft_attack, 1e-9);
    CHECK_NEAR(out.hard_attack, base->hard_attack + kit->hard_attack, 1e-9);
    CHECK_NEAR(out.air_attack, base->air_attack + kit->air_attack, 1e-9);
    CHECK_NEAR(out.air_defence, base->air_defence + kit->air_defence, 1e-9);
    CHECK_NEAR(out.ground_attack, base->ground_attack + kit->ground_attack, 1e-9);
    CHECK_NEAR(out.agility, base->agility + kit->agility, 1e-9);
    CHECK_NEAR(out.armor, base->armor + kit->armor, 1e-9);
    CHECK_NEAR(out.piercing, base->piercing + kit->piercing, 1e-9);
    CHECK_NEAR(out.defense, base->defense + kit->defense, 1e-9);
    CHECK_NEAR(out.breakthrough, base->breakthrough + kit->breakthrough, 1e-9);
    CHECK_NEAR(out.hardness, base->hardness + kit->hardness, 1e-9);
    CHECK_NEAR(out.max_strength, base->max_strength + kit->max_strength, 1e-9);
    CHECK_NEAR(out.organization, base->organization + kit->organization, 1e-9);
    CHECK_NEAR(out.speed, base->speed + kit->speed, 1e-9);
    CHECK_NEAR(out.reliability, base->reliability + kit->reliability, 1e-9);
    CHECK_NEAR(out.range, base->range + kit->range, 1e-9);
    CHECK_NEAR(out.detection, base->detection + kit->detection, 1e-9);
    CHECK_NEAR(out.sub_detection, base->sub_detection + kit->sub_detection, 1e-9);
    CHECK_NEAR(out.naval_attack, base->naval_attack + kit->naval_attack, 1e-9);
    CHECK_NEAR(out.torpedo_attack, base->torpedo_attack + kit->torpedo_attack, 1e-9);
    CHECK_NEAR(out.visibility, base->visibility + kit->visibility, 1e-9);
    CHECK_NEAR(out.fuel_use, base->fuel_use + kit->fuel_use, 1e-9);
    CHECK_NEAR(out.supply_use, base->supply_use + kit->supply_use, 1e-9);
    CHECK_NEAR(out.manpower, base->manpower + kit->manpower, 1e-9);
    CHECK_NEAR(out.resources[STEEL], base->resources[STEEL] + kit->resources[STEEL], 1e-9);
    CHECK_NEAR(out.resources[OIL], base->resources[OIL] + kit->resources[OIL], 1e-9);
    // build_cost = (base + sum(add)) * product(multiplier).
    CHECK_NEAR(out.build_cost, (base->build_cost + kit->build_cost_add) * kit->cost_multiplier,
               1e-9);

    CHECK(!out.is_archetype);
    CHECK(out.category == base->category);
    CHECK(out.archetype == base->archetype);
    CHECK_EQ(out.year, base->year);
    CHECK_EQ(out.key, std::string("k"));
    CHECK_EQ(out.name, std::string("n"));
}

HOI_TEST(design_compute_scales_cost_and_adds_resources) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    const EquipmentDef* base = g.content.equipment_def(f.archetype);

    const EquipmentDef out = design_compute(
        g, f.archetype,
        {{ComponentSlot::Armor, f.kit}, {ComponentSlot::Weapon, f.gun}}, "k", "n");

    CHECK_NEAR(out.build_cost, (base->build_cost + 1.0 + 2.0) * (1.5 * 1.2), 1e-9);
    CHECK_NEAR(out.resources[STEEL], base->resources[STEEL] + 0.5 + 1.0, 1e-9);
    CHECK_NEAR(out.resources[OIL], base->resources[OIL] + 0.25, 1e-9);
    CHECK_NEAR(out.reliability, base->reliability + 0.1 - 0.15, 1e-9);
    CHECK_NEAR(out.hard_attack, base->hard_attack + 2.0 + 15.0, 1e-9);
}

HOI_TEST(design_compute_clamps_reliability_and_other_stats) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    Content& c = g.content;

    ComponentDef over;
    over.key = "over";
    over.slot = ComponentSlot::Armor;
    over.category = EquipmentCategory::Armor;
    over.year = 1936;
    over.reliability = 100.0;
    const uint32_t over_i = push_component(c, over);

    ComponentDef under;
    under.key = "under";
    under.slot = ComponentSlot::Weapon;
    under.category = EquipmentCategory::Armor;
    under.year = 1936;
    under.reliability = -100.0;
    const uint32_t under_i = push_component(c, under);

    const EquipmentDef high = design_compute(g, f.archetype, {{ComponentSlot::Armor, over_i}},
                                             "k", "n");
    CHECK_NEAR(high.reliability, 1.0, 1e-9);

    const EquipmentDef low = design_compute(g, f.archetype, {{ComponentSlot::Weapon, under_i}},
                                            "k", "n");
    CHECK_NEAR(low.reliability, 0.1, 1e-9);

    // Absurd negative component data cannot make a statistic negative, and an
    // infinite/NaN product cannot leak through.
    const EquipmentDef junk =
        design_compute(g, f.archetype, {{ComponentSlot::Special, f.junk}}, "k", "n");
    CHECK_NEAR(junk.soft_attack, 0.0, 1e-9);
    CHECK_NEAR(junk.resources[STEEL], 0.0, 1e-9);
    CHECK_NEAR(junk.build_cost, 0.0, 1e-9);
    CHECK(std::isfinite(junk.build_cost));
    CHECK(std::isfinite(junk.reliability));
}

// ------------------------------------------------------------------- gates ---

HOI_TEST(component_unlocked_honours_year_trigger_and_technology) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    Country& c = *g.world.country(f.a);

    CHECK(component_unlocked(g, f.a, f.kit));
    // The category match is the caller's job (unlocked_components filters on it);
    // component_unlocked itself only applies the year/trigger/technology gates.
    CHECK(component_unlocked(g, f.a, f.infantry_bit));

    // Year gate.
    CHECK(!component_unlocked(g, f.a, f.late_engine));
    g.world.date.year = 1940;
    CHECK(component_unlocked(g, f.a, f.late_engine));
    g.world.date.year = 1936;

    // Trigger gate.
    CHECK(!component_unlocked(g, f.a, f.flag_engine));
    c.country_flags.push_back("engine_ok");
    CHECK(component_unlocked(g, f.a, f.flag_engine));

    // Technology gate.
    CHECK(!component_unlocked(g, f.a, f.tech_armor));
    c.research.completed.push_back(f.armor_tech);
    CHECK(component_unlocked(g, f.a, f.tech_armor));
}

// --------------------------------------------------------------- validation --

HOI_TEST(design_create_rejects_illegal_components) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    const size_t equipment_before = g.content.equipment.size();
    const size_t designs_before = g.content.designs.size();

    // Wrong category for the archetype.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype, {{ComponentSlot::Armor, f.infantry_bit}}),
             INVALID_ID);
    // Unknown component index.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype, {{ComponentSlot::Armor, 9999u}}),
             INVALID_ID);
    // Invalid slot value.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype,
                           {{static_cast<ComponentSlot>(ComponentSlot::Count), f.kit}}),
             INVALID_ID);
    // Duplicate slot.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype,
                           {{ComponentSlot::Armor, f.kit}, {ComponentSlot::Armor, f.plate}}),
             INVALID_ID);
    // Year-locked component.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype,
                           {{ComponentSlot::Engine, f.late_engine}}),
             INVALID_ID);
    // Trigger-gated component without the flag.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype,
                           {{ComponentSlot::Engine, f.flag_engine}}),
             INVALID_ID);
    // Technology-gated component without the technology.
    CHECK_EQ(design_create(g, f.a, "Bad", f.archetype, {{ComponentSlot::Special, f.tech_armor}}),
             INVALID_ID);
    // Empty name.
    CHECK_EQ(design_create(g, f.a, "", f.archetype, {{ComponentSlot::Armor, f.kit}}), INVALID_ID);

    CHECK_EQ(g.content.equipment.size(), equipment_before);
    CHECK_EQ(g.content.designs.size(), designs_before);
    CHECK(g.world.country(f.a)->designs.empty());

    // After satisfying the gates the same fits are accepted.
    g.world.date.year = 1940;
    g.world.country(f.a)->country_flags.push_back("engine_ok");
    g.world.country(f.a)->research.completed.push_back(f.armor_tech);
    CHECK(design_create(g, f.a, "Ok", f.archetype,
                        {{ComponentSlot::Engine, f.late_engine},
                         {ComponentSlot::Special, f.tech_armor}}) != INVALID_ID);
}

HOI_TEST(design_create_rejects_unavailable_archetype) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    // A producible model is not an archetype: no designing from it.
    CHECK(!design_available(g, f.a, f.existing));
    CHECK_EQ(design_create(g, f.a, "No", f.existing, {{ComponentSlot::Armor, f.kit}}), INVALID_ID);
    // Unknown archetype id.
    CHECK_EQ(design_create(g, f.a, "No", EquipmentId(4242u), {}), INVALID_ID);
    CHECK_EQ(g.content.designs.size(), static_cast<size_t>(0));
}

HOI_TEST(design_create_rejects_duplicate_names_and_command_validation_agrees) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    const uint32_t first = design_create(g, f.a, "Alpha", f.archetype, {{ComponentSlot::Armor, f.kit}});
    CHECK(first != INVALID_ID);
    CHECK(design_exists(g, "Alpha"));
    // Same name, even a different country or fit, is rejected.
    CHECK_EQ(design_create(g, f.a, "Alpha", f.archetype, {{ComponentSlot::Weapon, f.gun}}),
             INVALID_ID);
    CHECK_EQ(design_create(g, f.b, "Alpha", f.archetype, {{ComponentSlot::Armor, f.kit}}),
             INVALID_ID);

    // The command layer's duplicate-name rejection reads the same predicate.
    Command cmd;
    cmd.type = CommandType::CreateEquipmentDesign;
    cmd.country = f.a;
    cmd.equipment = f.archetype;
    cmd.text = "Alpha";
    cmd.components = {{static_cast<uint8_t>(ComponentSlot::Armor), f.kit}};
    CHECK(validate_command(g, cmd) != CommandResult::Applied);
    cmd.text = "Beta";
    CHECK(validate_command(g, cmd) == CommandResult::Applied);
}

HOI_TEST(design_keys_are_deterministic_and_collision_free) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    const uint32_t d1 = design_create(g, f.a, "One", f.archetype, {{ComponentSlot::Armor, f.kit}});
    const uint32_t d2 = design_create(g, f.a, "Two", f.archetype, {{ComponentSlot::Weapon, f.gun}});
    const uint32_t d3 = design_create(g, f.b, "Bee", f.archetype, {{ComponentSlot::Armor, f.kit}});
    CHECK(d1 != INVALID_ID && d2 != INVALID_ID && d3 != INVALID_ID);

    CHECK_EQ(g.content.designs[d1].key, std::string("VLA_armor_1"));
    CHECK_EQ(g.content.designs[d2].key, std::string("VLA_armor_2"));
    CHECK_EQ(g.content.designs[d3].key, std::string("NOR_armor_1"));
    CHECK_EQ(g.content.design(d1)->key, g.content.equipment[g.content.designs[d1].produced.v].key);
    CHECK_EQ(g.content.design_index[g.content.designs[d1].key], d1);
}

HOI_TEST(foreign_design_cannot_be_built_by_another_country) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    const uint32_t d = design_create(g, f.a, "National Model", f.archetype,
                                     {{ComponentSlot::Armor, f.kit}});
    CHECK(d != INVALID_ID);
    const EquipmentId produced = g.content.designs[d].produced;

    // The owner may build its design; the other country may not.
    CHECK(equipment_unlocked(g, f.a, produced));
    CHECK(!equipment_unlocked(g, f.b, produced));

    Command line;
    line.type = CommandType::SetProductionLine;
    line.equipment = produced;
    line.value = 5;
    line.country = f.b;
    CHECK(validate_command(g, line) != CommandResult::Applied);
    line.country = f.a;
    CHECK_EQ(validate_command(g, line), CommandResult::Applied);

    // Authored models with no owning design stay shared.
    CHECK(equipment_unlocked(g, f.b, f.existing));
}

// -------------------------------------------------------------- production ----

HOI_TEST(design_is_usable_by_a_production_line) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    const uint32_t d = design_create(g, f.a, "Line Model", f.archetype,
                                     {{ComponentSlot::Armor, f.kit}, {ComponentSlot::Weapon, f.gun}});
    CHECK(d != INVALID_ID);
    const EquipmentId produced = g.content.designs[d].produced;
    CHECK(produced.valid());
    CHECK(has_design_event(g));

    Command line;
    line.type = CommandType::SetProductionLine;
    line.country = f.a;
    line.equipment = produced;
    line.value = 5;
    CHECK_EQ(validate_command(g, line), CommandResult::Applied);
    g.queue.push(line);
    phase_commands(g);

    CHECK(g.world.country(f.a)->equipment_stockpile[produced.v] == 0.0);
    for (int i = 0; i < 2 * TICKS_PER_DAY; ++i) phase_industry(g);

    CHECK_GT(g.world.country(f.a)->equipment_stockpile[produced.v], 0.0);
}

// -------------------------------------------------------------------- AI ------

HOI_TEST(ai_creates_a_design_through_commands_within_30_days) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;
    g.set_ai(f.a, true);

    // The country already produces armor; the AI should replace that model.
    ProductionLine line;
    line.equipment = f.existing;
    line.factories = 5;
    line.started = 0;
    g.world.country(f.a)->lines.push_back(line);

    bool created = false;
    const EquipmentDef* base = g.content.equipment_def(f.archetype);
    for (int day = 0; day < 30 && !created; ++day) {
        g.world.tick = static_cast<Tick>(day) * TICKS_PER_DAY;
        ai_design_layer(g, *g.world.country(f.a));
        phase_commands(g);
        for (const EquipmentDesign& design : g.content.designs) {
            if (design.country != f.a) continue;
            const EquipmentDef& produced = g.content.equipment[design.produced.v];
            if (produced.category != EquipmentCategory::Armor) continue;
            created = true;
            // A design is a real, different model: at least one statistic moved.
            CHECK(produced.soft_attack != base->soft_attack ||
                  produced.hard_attack != base->hard_attack ||
                  produced.armor != base->armor ||
                  produced.build_cost != base->build_cost);
            CHECK(!produced.is_archetype);
        }
    }

    CHECK(created);
    CHECK(has_create_design_command(g, f.a));
    // A design was registered, and the AI explained itself with numeric factors.
    bool design_reason = false;
    for (const AiReason& reason : g.ai.layer(AiLayer::Industry).last_reasons) {
        if (reason.what.rfind("design:", 0) == 0) design_reason = true;
    }
    CHECK(design_reason);

    // The AI never edits state directly: the design arrived through the log.
    CHECK(!g.queue.pending.empty() || has_create_design_command(g, f.a));
}

HOI_TEST(ai_rejects_a_fit_below_the_gain_threshold_or_over_the_cost_margin) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    // A far stronger model the country already builds: no unlocked fit can clear
    // +10% capability over it, so the planner must stay quiet.
    EquipmentDef meta;
    meta.key = "armor_meta";
    meta.name = "Armor Meta";
    meta.category = EquipmentCategory::Armor;
    meta.year = 1936;
    meta.soft_attack = 200.0;
    meta.hard_attack = 200.0;
    meta.armor = 200.0;
    meta.piercing = 200.0;
    meta.reliability = 1.0;
    meta.speed = 8.0;
    meta.max_strength = 30.0;
    meta.organization = 50.0;
    meta.build_cost = 12.0;
    meta.id = EquipmentId(static_cast<uint32_t>(g.content.equipment.size()));
    g.content.equipment.push_back(meta);
    g.content.equipment_by_key[meta.key] = meta.id;
    g.world.country(f.a)->equipment_stockpile.push_back(0.0);

    // An unlocked component that would buy a lot of capability at an absurd price:
    // the cost margin must keep it out of any fit.
    ComponentDef gold;
    gold.key = "gold";
    gold.slot = ComponentSlot::Weapon;
    gold.category = EquipmentCategory::Armor;
    gold.year = 1936;
    gold.hard_attack = 500.0;
    gold.build_cost_add = 100.0;
    push_component(g.content, gold);

    ProductionLine line;
    line.equipment = meta.id;
    line.factories = 5;
    g.world.country(f.a)->lines.push_back(line);

    ai_design_layer(g, *g.world.country(f.a));
    phase_commands(g);
    CHECK(g.content.designs.empty());
    CHECK(!has_create_design_command(g, f.a));
    CHECK(g.ai.layer(AiLayer::Industry).last_reasons.empty());
}

HOI_TEST(ai_capability_reflects_defense_and_breakthrough) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    // A component worth its slot only through defense and breakthrough: the AI must
    // both score it (so it is fitted at all) and apply it to the produced model.
    ComponentDef bulwark;
    bulwark.key = "bulwark";
    bulwark.slot = ComponentSlot::Armor;
    bulwark.category = EquipmentCategory::Armor;
    bulwark.year = 1936;
    bulwark.defense = 200.0;
    bulwark.breakthrough = 200.0;
    push_component(g.content, bulwark);

    ProductionLine line;
    line.equipment = f.existing;
    line.factories = 5;
    g.world.country(f.a)->lines.push_back(line);

    ai_design_layer(g, *g.world.country(f.a));
    phase_commands(g);

    CHECK_EQ(g.content.designs.size(), static_cast<size_t>(1));
    const EquipmentDesign& design = g.content.designs[0];
    const EquipmentDef& produced = g.content.equipment[design.produced.v];
    CHECK_GT(produced.defense, 190.0);
    CHECK_GT(produced.breakthrough, 190.0);
}

// The design the AI creates for a category it cannot otherwise build must be picked
// up by the production layer as a line target. This is the wiring the design system
// needs to be more than a paper exercise; it fails until Main teaches
// ai_production_layer to treat designs as candidates for a category that has demand.
HOI_TEST(ai_design_enters_production_within_365_days) {
    DesignWorld f = make_design_world();
    Game& g = f.m.game;

    // The country's real armor model is behind an unresearched technology, so the
    // design is its only buildable armor model; the army nevertheless wants armor.
    TechDef gate;
    gate.key = "armor_gate";
    gate.name = "Armor Gate";
    gate.id = TechId(1);
    gate.unlock_equipment = {"armor_1"};
    g.content.techs.push_back(gate);
    g.content.tech_by_key[gate.key] = gate.id;
    CHECK(!equipment_unlocked(g, f.a, f.existing));

    DivisionTemplate armored;
    armored.key = "armored_template";
    armored.name = "Armored";
    armored.country = f.a;
    BattalionSlot battalion;
    battalion.equipment = f.archetype;  // the generic family model; production redirects it
    battalion.count = 3;
    armored.battalions.push_back(battalion);
    recompute_template_stats(armored, g.content.equipment);
    const TemplateId armored_id(static_cast<uint32_t>(g.content.templates.size()));
    g.content.templates.push_back(armored);
    g.content.template_by_key[armored.key] = armored_id;
    g.world.country(f.a)->templates.push_back(armored_id);

    // The country keeps an armor line (the archetype is producible), which is what
    // lets the design layer see the category at all. Enough factories exist that the
    // production layer can add a line for the design without retiring this one.
    g.world.state(f.m.state_a)->military_factories = 12;
    ProductionLine line;
    line.equipment = f.archetype;
    line.factories = 2;
    line.started = 0;
    g.world.country(f.a)->lines.push_back(line);

    EquipmentId design_equipment;
    bool on_a_line = false;
    for (int day = 0; day < 365 && !on_a_line; ++day) {
        g.world.tick = static_cast<Tick>(day) * TICKS_PER_DAY;
        Country& c = *g.world.country(f.a);
        ai_design_layer(g, c);
        ai_production_layer(g, c);
        phase_commands(g);
        for (uint32_t index : c.designs) {
            const EquipmentDesign* d = g.content.design(index);
            if (d != nullptr && d->country == f.a) design_equipment = d->produced;
        }
        if (!design_equipment.valid()) continue;
        for (const ProductionLine& l : c.lines) {
            if (l.equipment == design_equipment && l.factories > 0) on_a_line = true;
        }
    }

    CHECK(design_equipment.valid());
    CHECK(on_a_line);
}

// ------------------------------------------------------------- determinism ----

HOI_TEST(design_runs_are_deterministic) {
    const auto run = []() {
        DesignWorld f = make_design_world();
        Game& g = f.m.game;
        g.set_ai(f.a, true);
        ProductionLine line;
        line.equipment = f.existing;
        line.factories = 5;
        g.world.country(f.a)->lines.push_back(line);
        for (int day = 0; day < 30; ++day) {
            g.world.tick = static_cast<Tick>(day) * TICKS_PER_DAY;
            ai_design_layer(g, *g.world.country(f.a));
            phase_commands(g);
            for (int i = 0; i < TICKS_PER_DAY; ++i) phase_industry(g);
        }
        return world_hash(g);
    };
    CHECK_EQ(run(), run());
}

}  // namespace