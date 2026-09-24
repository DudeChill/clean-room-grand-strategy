// Retrofit / equipment-variant tests (MIL-021).
//
// A battalion slot names one equipment family, not one exact model: a division
// short of an older model must be able to draw a newer member of the same family
// once that member is in the stockpile, without ever drawing a locked model or
// another country's design. Worlds are hand-built so every expected number is
// arithmetic, and each case is hermetic (no scenario files).

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "data/content.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/design.h"
#include "sim/industry.h"
#include "sim/research.h"
#include "sim/units.h"
#include "test.h"
#include "test_util.h"

namespace {

using namespace hoi;
using namespace hoi_test;

// A concrete model of the infantry family.
EquipmentId add_model(Content& c, const std::string& key, const std::string& archetype,
                      double soft_attack, double build_cost, double hp) {
    EquipmentDef e;
    e.key = key;
    e.name = key;
    e.category = EquipmentCategory::Infantry;
    e.archetype = archetype;
    e.year = 1936;
    e.soft_attack = soft_attack;
    e.defense = 20.0;
    e.breakthrough = 15.0;
    e.build_cost = build_cost;
    e.organization = 60.0;
    e.max_strength = hp;
    e.speed = 4.0;
    e.manpower = 100.0;
    e.supply_use = 0.5;
    const EquipmentId id(static_cast<uint32_t>(c.equipment.size()));
    e.id = id;
    c.equipment.push_back(e);
    c.equipment_by_key[key] = id;
    return id;
}

void set_stock(Game& g, CountryId id, EquipmentId eq, double amount) {
    Country* c = g.world.country(id);
    if (c->equipment_stockpile.size() <= eq.v) c->equipment_stockpile.resize(eq.v + 1, 0.0);
    c->equipment_stockpile[eq.v] = amount;
}

double stock(const Game& g, CountryId id, EquipmentId eq) {
    const Country* c = g.world.country(id);
    return c && eq.v < c->equipment_stockpile.size() ? c->equipment_stockpile[eq.v] : 0.0;
}

// A live production line, staffed from the state's existing military factories.
void add_line(Game& g, CountryId id, EquipmentId eq, int factories) {
    ProductionLine line;
    line.equipment = eq;
    line.factories = factories;
    line.efficiency = 1.0;
    line.efficiency_cap = 1.0;
    g.world.country(id)->lines.push_back(line);
}

// Two-country world whose infantry family has:
//   archetype  "infantry_equipment" (the abstract family)
//   old_model   infantry_equipment_1 (existing, lower soft attack)
//   new_model   infantry_equipment_2 (higher soft attack, unlocked)
//   locked      infantry_equipment_3 (gated behind a technology nobody holds)
//   other       B's own design (registered through design_create)
struct RetrofitWorld {
    MiniWorld m;
    CountryId a;
    CountryId b;
    TemplateId tmpl;
    EquipmentId archetype;
    EquipmentId old_model;
    EquipmentId new_model;
    EquipmentId locked;
    EquipmentId other_design;
    TechId lock_tech;
};

RetrofitWorld make_retrofit_world(bool with_new = true, bool with_locked = true,
                                  bool with_design = true) {
    RetrofitWorld w;
    w.m = make_mini_world();
    Content& c = w.m.game.content;
    Game& g = w.m.game;
    w.a = w.m.a;
    w.b = w.m.b;
    w.tmpl = w.m.tmpl;

    EquipmentDef arch;
    arch.key = "infantry_equipment";
    arch.name = "Infantry Equipment";
    arch.category = EquipmentCategory::Infantry;
    arch.is_archetype = true;
    arch.year = 1936;
    arch.soft_attack = 5.0;
    arch.defense = 18.0;
    arch.breakthrough = 13.5;
    arch.build_cost = 0.6;
    arch.max_strength = 25.0;
    arch.organization = 60.0;
    arch.manpower = 100.0;
    arch.speed = 4.0;
    arch.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    c.equipment.push_back(arch);
    c.equipment_by_key[arch.key] = arch.id;
    w.archetype = arch.id;

    w.old_model = c.equipment_by_key["infantry_equipment_1"];
    c.equipment[w.old_model.v].archetype = "infantry_equipment";
    w.new_model = w.old_model;

    if (with_new) {
        w.new_model = add_model(c, "infantry_equipment_2", "infantry_equipment", 12.0, 0.8, 25.0);
    }
    if (with_locked) {
        w.locked = add_model(c, "infantry_equipment_3", "infantry_equipment", 15.0, 0.9, 25.0);
        TechDef tech;
        tech.key = "infantry_equipment_3_tech";
        tech.name = tech.key;
        tech.category = "infantry";
        tech.year = 1936;
        tech.cost_days = 100.0;
        tech.unlock_equipment = {"infantry_equipment_3"};
        tech.id = TechId(static_cast<uint32_t>(c.techs.size()));
        c.techs.push_back(tech);
        c.tech_by_key[tech.key] = tech.id;
        w.lock_tech = tech.id;  // never completed: the model stays locked
    }
    if (with_design) {
        // One unlocked component of the archetype's category is all design_create
        // needs; B fits it into the infantry archetype and registers the result.
        ComponentDef comp;
        comp.index = static_cast<uint32_t>(c.components.size());
        comp.key = "infantry_barrel";
        comp.name = comp.key;
        comp.slot = ComponentSlot::Special;
        comp.category = EquipmentCategory::Infantry;
        comp.year = 1936;
        comp.soft_attack = 4.0;
        c.component_index[comp.key] = comp.index;
        c.components.push_back(comp);
        const uint32_t design_index =
            design_create(g, w.b, "B Advanced Rifle", w.archetype, {{ComponentSlot::Special, comp.index}});
        if (design_index != INVALID_ID) w.other_design = g.content.designs[design_index].produced;
    }

    // Start every family model from an empty stockpile: each test states the stock
    // it needs, so a leftover default cannot decide the result.
    set_stock(g, w.a, w.old_model, 0.0);
    set_stock(g, w.b, w.old_model, 0.0);
    if (with_new) {
        set_stock(g, w.a, w.new_model, 0.0);
        set_stock(g, w.b, w.new_model, 0.0);
    }
    if (with_locked) {
        set_stock(g, w.a, w.locked, 0.0);
        set_stock(g, w.b, w.locked, 0.0);
    }
    if (w.other_design.valid()) {
        set_stock(g, w.a, w.other_design, 0.0);
        set_stock(g, w.b, w.other_design, 0.0);
    }
    return w;
}

// A division on the first province of A, holding `held` of `held_model` only.
Division* division_of(Game& g, RetrofitWorld& w, EquipmentId held_model, double held,
                      double strength) {
    const DivisionId did = add_division(g, w.a, w.m.provinces[0], w.tmpl);
    Division* d = g.world.division(did);
    d->equipment.assign(g.content.equipment.size(), 0.0);
    if (held_model.valid()) d->equipment[held_model.v] = held;
    d->strength = strength;
    d->manpower = 1000.0 * strength;
    return d;
}

std::string temp_path(const std::string& name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path(ec);
    return (dir / ("hoi_retrofit_" + name)).string();
}

}  // namespace

// ---------------------------------------------------------------- families ---

HOI_TEST(retrofit_slot_family_resolves_models_and_archetypes) {
    RetrofitWorld w = make_retrofit_world();
    const Content& c = w.m.game.content;

    // A concrete model belongs to its archetype family.
    CHECK_EQ(slot_family(c, w.old_model), std::string("infantry_equipment"));
    // A slot naming the archetype belongs to the archetype itself.
    CHECK_EQ(slot_family(c, w.archetype), std::string("infantry_equipment"));
    // An unrelated model and an invalid id have no family.
    const EquipmentId arty = c.equipment_by_key.at("artillery_1");
    CHECK(slot_family(c, arty).empty());  // artillery_1 has no archetype set
    CHECK_EQ(slot_family(c, EquipmentId{}), std::string());

    CHECK(equipment_fits_slot(w.m.game, w.a, w.old_model, w.new_model));
    CHECK(equipment_fits_slot(w.m.game, w.a, w.archetype, w.new_model));
    // The archetype itself is never a fieldable member.
    CHECK(!equipment_fits_slot(w.m.game, w.a, w.archetype, w.archetype));
    // A different model of another family does not fit.
    CHECK(!equipment_fits_slot(w.m.game, w.a, w.old_model, arty));
    // No live line yet, so the family has no production model.
    CHECK_EQ(family_production_model(w.m.game, w.a, w.old_model), EquipmentId{});

    // A live line makes the member it builds the family production model.
    add_line(w.m.game, w.a, w.new_model, 1);
    CHECK_EQ(family_production_model(w.m.game, w.a, w.old_model), w.new_model);
    CHECK_EQ(family_production_model(w.m.game, w.a, w.archetype), w.new_model);
    // A line for another family does not answer for this one.
    CHECK_EQ(family_production_model(w.m.game, w.a, arty), EquipmentId{});
}

// -------------------------------------------------------------- reinforcement -

HOI_TEST(retrofit_division_draws_newer_family_member_from_stock) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.7);
    set_stock(g, w.a, w.new_model, 100.0);

    // The slot wants six of the family; the division holds four of the old model,
    // so the newer member is the model the country would issue.
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.new_model);

    // Demand is expressed against the newer model, not the slot's exact model.
    std::vector<double> demand;
    compute_equipment_demand(g, w.a, &demand);
    CHECK_NEAR(demand[w.new_model.v], 2.0, 1e-12);
    CHECK_NEAR(demand[w.old_model.v], 0.0, 1e-12);

    // A call for the older model still lands on the newer one, because the older
    // one has no stock.
    const double moved = reinforce_division(g, *d, w.old_model, 100.0);
    CHECK_NEAR(moved, 2.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.new_model), 98.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.old_model), 0.0, 1e-12);
    // The newer index is consumed; the older gear the division already held stays.
    CHECK_NEAR(d->equipment[w.new_model.v], 2.0, 1e-12);
    CHECK_NEAR(d->equipment[w.old_model.v], 4.0, 1e-12);
    CHECK_GT(d->strength, 0.7);
    // 2 newer members * 25 hp on a 150 hp template, clamped at full strength.
    CHECK_NEAR(d->strength, 1.0, 1e-12);
}

HOI_TEST(retrofit_prefers_the_model_the_caller_asked_for_when_it_has_stock) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.old_model, 100.0);
    set_stock(g, w.a, w.new_model, 100.0);

    // Both members have stock; the better model is still what production should
    // issue, but a direct call for the older model draws the older stock first.
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.new_model);
    const double moved = reinforce_division(g, *d, w.old_model, 100.0);
    CHECK_NEAR(moved, 2.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.old_model), 98.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.new_model), 100.0, 1e-12);
    CHECK_NEAR(d->equipment[w.old_model.v], 6.0, 1e-12);
}

HOI_TEST(retrofit_mixed_stock_prefers_the_better_model) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    set_stock(g, w.a, w.old_model, 100.0);
    set_stock(g, w.a, w.new_model, 100.0);
    set_stock(g, w.a, w.locked, 100.0);  // even a locked model in the depot changes nothing

    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.new_model);
    CHECK_EQ(preferred_slot_model(g, w.a, w.archetype, w.new_model), w.new_model);
}

HOI_TEST(retrofit_locked_family_member_is_never_drawn) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.locked, 100.0);

    CHECK(!equipment_fits_slot(g, w.a, w.old_model, w.locked));
    // Rule 1 skips locked stock; rule 3 picks the best *fieldable* member instead.
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.new_model);

    // A call naming the locked model is refused outright.
    CHECK_NEAR(reinforce_division(g, *d, w.locked, 100.0), 0.0, 1e-12);
    // A call for the older model cannot reach the locked stock either.
    CHECK_NEAR(reinforce_division(g, *d, w.old_model, 100.0), 0.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.locked), 100.0, 1e-12);
    CHECK_NEAR(d->equipment[w.locked.v], 0.0, 1e-12);
    CHECK_NEAR(d->equipment[w.old_model.v], 4.0, 1e-12);
}

HOI_TEST(retrofit_another_country_design_is_never_drawn) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    CHECK(w.other_design.valid());

    // The owner can field its own design; the other country cannot.
    CHECK(equipment_fits_slot(g, w.b, w.old_model, w.other_design));
    CHECK(!equipment_fits_slot(g, w.a, w.old_model, w.other_design));

    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.other_design, 100.0);
    CHECK(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}) != w.other_design);
    CHECK_NEAR(reinforce_division(g, *d, w.other_design, 100.0), 0.0, 1e-12);
    CHECK_NEAR(reinforce_division(g, *d, w.old_model, 100.0), 0.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.other_design), 100.0, 1e-12);
    CHECK_NEAR(d->equipment[w.other_design.v], 0.0, 1e-12);
}

HOI_TEST(retrofit_exact_model_path_still_works_with_a_single_member) {
    RetrofitWorld w = make_retrofit_world(/*with_new=*/false, /*with_locked=*/false,
                                          /*with_design=*/false);
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.old_model, 100.0);

    CHECK_EQ(slot_family(g.content, w.old_model), std::string("infantry_equipment"));
    CHECK(equipment_fits_slot(g, w.a, w.old_model, w.old_model));
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.old_model);

    const double moved = reinforce_division(g, *d, w.old_model, 100.0);
    CHECK_NEAR(moved, 2.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.old_model), 98.0, 1e-12);
    CHECK_NEAR(d->equipment[w.old_model.v], 6.0, 1e-12);
}

HOI_TEST(retrofit_slot_naming_an_archetype_still_works) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    // The template names the bare archetype instead of a concrete model.
    DivisionTemplate& t = g.content.templates[w.tmpl.v];
    t.battalions[0].equipment = w.archetype;
    recompute_template_stats(t, g.content.equipment);

    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.new_model, 100.0);

    CHECK(equipment_fits_slot(g, w.a, w.archetype, w.new_model));
    CHECK_EQ(preferred_slot_model(g, w.a, w.archetype, EquipmentId{}), w.new_model);
    const double moved = reinforce_division(g, *d, w.new_model, 100.0);
    CHECK_NEAR(moved, 2.0, 1e-12);
    CHECK_NEAR(d->equipment[w.new_model.v], 2.0, 1e-12);
    CHECK_NEAR(d->equipment[w.old_model.v], 4.0, 1e-12);
}

HOI_TEST(retrofit_call_for_unused_equipment_returns_zero) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    const EquipmentId arty = g.content.equipment_by_key.at("artillery_1");  // no template slot
    set_stock(g, w.a, w.new_model, 100.0);

    CHECK_NEAR(reinforce_division(g, *d, arty, 100.0), 0.0, 1e-12);
    CHECK_NEAR(d->equipment[arty.v], 0.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.new_model), 100.0, 1e-12);
}

HOI_TEST(retrofit_present_gear_counts_across_the_family) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    // The division already holds four old and two new: the family requirement of
    // six is satisfied, so a call for either model moves nothing.
    Division* d = division_of(g, w, w.old_model, 4.0, 1.0);
    d->equipment[w.new_model.v] = 2.0;
    set_stock(g, w.a, w.new_model, 100.0);

    CHECK_NEAR(reinforce_division(g, *d, w.new_model, 100.0), 0.0, 1e-12);
    std::vector<double> demand;
    compute_equipment_demand(g, w.a, &demand);
    CHECK_NEAR(demand[w.new_model.v], 0.0, 1e-12);
}

// Regression for the stock-transient oscillation: with a live line for the newer
// model, a division at 8/10 old gear, 50 old in the depot and 2 units/hour attrition,
// the preferred model must be constant whether it is sampled before or after
// phase_industry — and depot gear must still be consumed once no line builds the
// family.
HOI_TEST(retrofit_preference_is_stable_across_ticks_and_depot_is_not_stranded) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 8.0, 0.8);
    set_stock(g, w.a, w.old_model, 50.0);
    set_stock(g, w.a, w.new_model, 0.0);
    add_line(g, w.a, w.new_model, 1);  // 1 unit/hour of the newer model
    const DivisionId did = d->id;

    const int hours = 60 * TICKS_PER_DAY;
    for (int hour = 0; hour < hours; ++hour) {
        // Sampling BEFORE the industry phase and AFTER it must agree: the live
        // model, not whichever stock happens to be momentarily non-empty.
        const EquipmentId producing = family_production_model(g, w.a, w.old_model);
        CHECK_EQ(producing, w.new_model);
        const EquipmentId before = preferred_slot_model(g, w.a, w.old_model, producing);
        CHECK_EQ(before, w.new_model);

        g.world.tick += 1;
        g.world.date.hour = static_cast<uint8_t>(g.world.tick % 24);
        phase_industry(g);

        const EquipmentId after = preferred_slot_model(g, w.a, w.old_model, producing);
        CHECK_EQ(after, before);

        // 2 units/hour attrition, then top up with the model the country issues.
        Division* dd = g.world.division(did);
        CHECK(dd != nullptr);
        double loss = 2.0;
        for (size_t i = dd->equipment.size(); i-- > 0 && loss > 0.0;) {
            const double take = std::min(loss, dd->equipment[i]);
            dd->equipment[i] -= take;
            loss -= take;
        }
        reinforce_division(g, *dd, before, 2.0);
    }

    // The preference never flipped across the whole run.
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, family_production_model(g, w.a, w.old_model)),
             w.new_model);

    // With the line gone the family falls back to the stocked member, and a call for
    // it still draws the depot: gear is never stranded.
    g.world.country(w.a)->lines[0].factories = 0;
    CHECK_EQ(family_production_model(g, w.a, w.old_model), EquipmentId{});
    CHECK_EQ(preferred_slot_model(g, w.a, w.old_model, EquipmentId{}), w.old_model);
    Division* dd = g.world.division(did);
    const double before_stock = stock(g, w.a, w.old_model);
    CHECK_NEAR(before_stock, 50.0, 1e-12);
    CHECK_NEAR(reinforce_division(g, *dd, w.old_model, 2.0), 2.0, 1e-12);
    CHECK_NEAR(stock(g, w.a, w.old_model), before_stock - 2.0, 1e-12);
}

// ------------------------------------------------------------- determinism ---

HOI_TEST(retrofit_same_scenario_hashes_identically) {
    auto run = [] {
        RetrofitWorld w = make_retrofit_world();
        Game& g = w.m.game;
        Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
        set_stock(g, w.a, w.new_model, 100.0);
        set_stock(g, w.a, w.locked, 100.0);
        reinforce_division(g, *d, w.old_model, 100.0);
        std::vector<double> demand;
        compute_equipment_demand(g, w.a, &demand);
        return world_hash(g);
    };
    CHECK_GT(run(), 0u);
    CHECK_EQ(run(), run());
}

// ------------------------------------------------------------ save / load ---

HOI_TEST(retrofit_save_round_trip_keeps_divisions_and_equipment) {
    RetrofitWorld w = make_retrofit_world();
    Game& g = w.m.game;
    Division* d = division_of(g, w, w.old_model, 4.0, 0.6);
    set_stock(g, w.a, w.new_model, 100.0);
    reinforce_division(g, *d, w.old_model, 100.0);  // consumes 2 newer members
    const uint64_t hash = world_hash(g);

    const std::string path = temp_path("roundtrip.bin");
    std::string err;
    CHECK(save_game(g, path, &err));
    Game loaded;
    CHECK(load_game(loaded, path, &err));
    std::error_code ec;
    std::filesystem::remove(path, ec);

    CHECK_EQ(world_hash(loaded), hash);
    CHECK_EQ(loaded.world.divisions.size(), g.world.divisions.size());
    CHECK_EQ(loaded.content.equipment.size(), g.content.equipment.size());

    const Division* ld = loaded.world.divisions.try_get(DivisionId(0));
    CHECK(ld != nullptr);
    CHECK_EQ(ld->equipment.size(), d->equipment.size());
    CHECK_NEAR(ld->equipment[w.new_model.v], 2.0, 1e-12);
    CHECK_NEAR(ld->equipment[w.old_model.v], 4.0, 1e-12);
    CHECK_NEAR(loaded.world.country(w.a)->equipment_stockpile[w.new_model.v], 98.0, 1e-12);
}