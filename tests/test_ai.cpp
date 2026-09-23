// AI tests: the layered planners must be a first-class player.
//
// These tests build a tiny two-country world by hand (no data files, no Game::create)
// so a single layer can be exercised in isolation, then drive phase_ai through a
// full 30-day planning loop. What they pin down:
//
//  * the AI plans construction and production at all;
//  * every command it pushes validates and applies - it is a player, not a cheat;
//  * planning leaves world state untouched (the AI may only write its own AiState);
//  * a player-controlled country never receives AI orders.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/hash.h"
#include "core/types.h"
#include "data/content.h"
#include "game/game.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/world.h"
#include "test.h"

namespace {

using namespace hoi;

struct Fixture {
    Game g;
    CountryId a;  // AI-controlled in most tests
    CountryId b;  // player-controlled
    StateId state_a;
    StateId state_b;
    ProvinceId cap_a;
    ProvinceId front_a;
    ProvinceId cap_b;
    ProvinceId back_b;
    TemplateId tpl_a;
    TemplateId tpl_b;
    WarId war;
};

EquipmentId add_equipment(Content& ct, const char* key, const char* archetype,
                          EquipmentCategory cat, int year, double build_cost, double soft,
                          double def, bool is_archetype) {
    EquipmentDef d;
    d.id = EquipmentId(static_cast<uint32_t>(ct.equipment.size()));
    d.key = key;
    d.name = key;
    d.archetype = archetype;
    d.category = cat;
    d.year = year;
    d.build_cost = build_cost;
    d.soft_attack = soft;
    d.defense = def;
    d.max_strength = 1.0;
    d.reliability = 1.0;
    d.manpower = 10.0;
    d.is_archetype = is_archetype;
    ct.equipment_by_key[key] = d.id;
    ct.equipment.push_back(d);
    return d.id;
}

void add_building(Content& ct, BuildingKind kind, const char* key, double base_cost,
                  int max_level) {
    BuildingDef b;
    b.kind = kind;
    b.key = key;
    b.name = key;
    b.base_cost = base_cost;
    b.max_level = max_level;
    ct.buildings.push_back(b);
}

TechId add_tech(Content& ct, const char* key, const char* category, int year,
                const std::vector<std::string>& unlocks, ModifierKind mod, double mod_value) {
    TechDef t;
    t.id = TechId(static_cast<uint32_t>(ct.techs.size()));
    t.key = key;
    t.name = key;
    t.category = category;
    t.year = year;
    t.cost_days = 100.0;
    t.unlock_equipment = unlocks;
    if (mod != ModifierKind::Count) t.modifiers.add(mod, mod_value);
    ct.tech_by_key[key] = t.id;
    ct.techs.push_back(t);
    return t.id;
}

TemplateId add_template(Content& ct, const char* key, CountryId owner, EquipmentId line,
                        int line_count, EquipmentId support) {
    DivisionTemplate t;
    t.id = TemplateId(static_cast<uint32_t>(ct.templates.size()));
    t.key = key;
    t.name = key;
    t.country = owner;
    t.battalions.push_back(BattalionSlot{line, line_count, false});
    if (support.valid()) t.battalions.push_back(BattalionSlot{support, 1, true});
    t.combat_width = static_cast<double>(line_count) * 2.0;
    t.max_organization = 60.0;
    t.max_strength = 100.0;
    t.soft_attack = static_cast<double>(line_count) * 3.0;
    t.defense = static_cast<double>(line_count) * 4.0;
    t.speed = 4.0;
    t.supply_use = 1.0;
    t.manpower = 10000.0;
    t.build_cost = 1000.0;
    t.train_days = 90.0;
    ct.template_by_key[key] = t.id;
    ct.templates.push_back(t);
    return t.id;
}

ProvinceId add_province(World& w, const char* name, StateId state, RegionId region,
                        CountryId owner, int infrastructure, bool capital) {
    Province p;
    p.name = name;
    p.state = state;
    p.region = region;
    p.terrain = Terrain::Plains;
    p.owner = owner;
    p.controller = owner;
    p.infrastructure = infrastructure;
    p.population = 250000.0;
    p.is_capital = capital;
    const ProvinceId id = w.provinces.create(std::move(p));
    w.province(id)->id = id;
    return id;
}

StateId add_state(World& w, const char* name, RegionId region, CountryId owner,
                  std::vector<ProvinceId> provinces, int civ, int mil, int slots) {
    State s;
    s.name = name;
    s.owner = owner;
    s.controller = owner;
    s.region = region;
    s.provinces = std::move(provinces);
    s.civilian_factories = civ;
    s.military_factories = mil;
    s.building_slots = slots;
    s.manpower_pool = 500000.0;
    const StateId id = w.states.create(std::move(s));
    w.state(id)->id = id;
    return id;
}

void add_division(World& w, Country& c, TemplateId tpl, ProvinceId location, double strength) {
    Division d;
    d.country = c.id;
    d.template_id = tpl;
    d.name = "1. Division";
    d.location = location;
    d.organization = 60.0;
    d.max_organization = 60.0;
    d.strength = strength;
    d.supply = 1.0;
    d.manpower = 10000.0;
    const DivisionId id = w.divisions.create(std::move(d));
    w.division(id)->id = id;
    c.divisions.push_back(id);
}

// A minimal but complete world: two countries at war over a single land border, each
// with factories, one template, one division and the technologies that unlock their
// starting equipment and buildings.
void build_world(Fixture& f) {
    Game& g = f.g;
    g.seed = 42;
    g.rng.seed(42);
    g.start_date = GameDate{1936, 1, 1, 0};
    g.world.world_seed = 42;
    g.world.tick = 0;
    g.world.date = g.start_date;

    Content& ct = g.content;
    const EquipmentId eq_arch = add_equipment(ct, "infantry_equipment", "infantry_equipment",
                                              EquipmentCategory::Infantry, 1936, 0.0, 0.0, 0.0, true);
    const EquipmentId eq_1 = add_equipment(ct, "infantry_equipment_1", "infantry_equipment",
                                           EquipmentCategory::Infantry, 1936, 0.43, 3.0, 4.0, false);
    const EquipmentId eq_2 = add_equipment(ct, "infantry_equipment_2", "infantry_equipment",
                                           EquipmentCategory::Infantry, 1937, 0.50, 4.0, 5.0, false);
    (void)eq_arch;
    (void)eq_2;
    const EquipmentId art_1 = add_equipment(ct, "artillery_1", "artillery",
                                            EquipmentCategory::Artillery, 1936, 1.00, 6.0, 2.0, false);
    const EquipmentId sup_1 = add_equipment(ct, "support_equipment_1", "support_equipment",
                                            EquipmentCategory::Support, 1936, 0.50, 1.0, 2.0, false);

    add_building(ct, BuildingKind::CivilianFactory, "civilian_factory", 10800.0, 10);
    add_building(ct, BuildingKind::MilitaryFactory, "military_factory", 10800.0, 10);
    add_building(ct, BuildingKind::Infrastructure, "infrastructure", 3000.0, 10);
    add_building(ct, BuildingKind::SupplyHub, "supply_hub", 6000.0, 5);
    add_building(ct, BuildingKind::Fort, "fort", 2000.0, 5);

    std::vector<std::string> buildings = {"civilian_factory", "military_factory", "infrastructure",
                                          "supply_hub", "fort"};
    add_tech(ct, "basic_industry", "industry", 1936, buildings, ModifierKind::FactoryOutput, 0.05);
    add_tech(ct, "infantry_weapons", "infantry", 1936, {"infantry_equipment_1"},
             ModifierKind::Count, 0.0);
    add_tech(ct, "artillery_school", "artillery", 1936, {"artillery_1"}, ModifierKind::Count, 0.0);
    add_tech(ct, "support_companies", "support", 1936, {"support_equipment_1"},
             ModifierKind::Count, 0.0);
    add_tech(ct, "industry_1937", "industry", 1937, {}, ModifierKind::FactoryOutput, 0.10);
    add_tech(ct, "armor_1937", "armor", 1937, {}, ModifierKind::DivisionAttack, 0.05);
    add_tech(ct, "infantry_1938", "infantry", 1938, {"infantry_equipment_2"},
             ModifierKind::Count, 0.0);

    Region region_def;
    region_def.name = "North";
    const RegionId region = g.world.regions.create(std::move(region_def));

    Country ca;
    ca.tag = "AUR";
    ca.name = "Auroria";
    ca.ideology = Ideology::Democratic;
    ca.manpower = 200000.0;
    ca.political_power = 150.0;
    ca.at_war = true;
    ca.research.slots_unlocked = 3;
    ca.research.slots.assign(3, ResearchSlot{});
    f.a = g.world.countries.create(std::move(ca));
    g.world.country(f.a)->id = f.a;

    Country cb;
    cb.tag = "BOR";
    cb.name = "Borealis";
    cb.ideology = Ideology::Fascist;
    cb.manpower = 200000.0;
    cb.at_war = true;
    cb.research.slots_unlocked = 3;
    cb.research.slots.assign(3, ResearchSlot{});
    f.b = g.world.countries.create(std::move(cb));
    g.world.country(f.b)->id = f.b;

    // Provinces: pA_cap - pA_front - pB_cap - pB_back
    f.state_a = add_state(g.world, "Auroria Proper", region, f.a, {}, 5, 4, 12);
    f.state_b = add_state(g.world, "Borealis Proper", region, f.b, {}, 3, 2, 10);
    f.cap_a = add_province(g.world, "Aurelia", f.state_a, region, f.a, 5, true);
    f.front_a = add_province(g.world, "Auroria Frontier", f.state_a, region, f.a, 3, false);
    f.cap_b = add_province(g.world, "Borealis City", f.state_b, region, f.b, 4, true);
    f.back_b = add_province(g.world, "Borealis Interior", f.state_b, region, f.b, 4, false);
    g.world.province(f.cap_a)->adj = {f.front_a};
    g.world.province(f.front_a)->adj = {f.cap_a, f.cap_b};
    g.world.province(f.cap_b)->adj = {f.front_a, f.back_b};
    g.world.province(f.back_b)->adj = {f.cap_b};
    g.world.state(f.state_a)->provinces = {f.cap_a, f.front_a};
    g.world.state(f.state_b)->provinces = {f.cap_b, f.back_b};
    g.world.regions[region].provinces = {f.cap_a, f.front_a, f.cap_b, f.back_b};

    g.world.country(f.a)->capital = f.state_a;
    g.world.country(f.b)->capital = f.state_b;

    // War: Auroria attacks Borealis on tick 0.
    War war;
    war.aggressor = f.a;
    war.active = true;
    war.start_tick = 0;
    war.attackers.push_back(WarParticipant{f.a, 0.0, 0.0, 0.0});
    war.defenders.push_back(WarParticipant{f.b, 0.0, 0.0, 0.0});
    f.war = g.world.wars.create(std::move(war));
    g.world.war(f.war)->id = f.war;
    g.world.country(f.a)->wars.push_back(f.war);
    g.world.country(f.b)->wars.push_back(f.war);
    g.world.relation(f.a, f.b).at_war = true;
    g.world.relation(f.a, f.b).value = -50.0;

    f.tpl_a = add_template(ct, "auroria_infantry", f.a, eq_1, 9, sup_1);
    f.tpl_b = add_template(ct, "borealis_infantry", f.b, eq_1, 6, EquipmentId{});

    Country* a = g.world.country(f.a);
    a->templates.push_back(f.tpl_a);
    a->equipment_stockpile.assign(ct.equipment.size(), 0.0);
    a->equipment_stockpile[eq_1.v] = 4000.0;
    a->equipment_stockpile[art_1.v] = 400.0;
    a->equipment_stockpile[sup_1.v] = 400.0;
    a->research.completed = {ct.tech_id("basic_industry"), ct.tech_id("infantry_weapons"),
                             ct.tech_id("artillery_school"), ct.tech_id("support_companies")};

    Country* b = g.world.country(f.b);
    b->templates.push_back(f.tpl_b);
    b->equipment_stockpile.assign(ct.equipment.size(), 0.0);
    b->equipment_stockpile[eq_1.v] = 2000.0;
    b->research.completed = {ct.tech_id("basic_industry"), ct.tech_id("infantry_weapons")};

    add_division(g.world, *g.world.country(f.a), f.tpl_a, f.front_a, 1.0);
    add_division(g.world, *g.world.country(f.b), f.tpl_b, f.cap_b, 1.0);

    g.ai_controlled.assign(2, 0);
    g.set_ai(f.a, true);
    g.set_ai(f.b, false);
    g.player_country = f.b;
}

// Hash of everything the AI is not allowed to touch: the world and its economy.
// The Ai and Rng subsystems are deliberately excluded - the AI owns its own clock,
// reason log and counters, and never draws from the simulation RNG.
uint64_t world_state_hash(const Game& g) {
    const Subsystem parts[] = {Subsystem::Map,      Subsystem::Countries, Subsystem::Economy,
                               Subsystem::Military, Subsystem::Battles,   Subsystem::Diplomacy,
                               Subsystem::Politics};
    Hasher h;
    for (Subsystem s : parts) h.u64(subsystem_hash(g, s));
    return h.value();
}

int count_commands(const CommandQueue& q, CommandType type) {
    int n = 0;
    for (const Command& c : q.pending) {
        if (c.type == type) ++n;
    }
    return n;
}

uint64_t command_fingerprint(const Command& c) {
    Hasher h;
    h.u8(static_cast<uint8_t>(c.type));
    h.u32(c.country.v);
    h.u64(c.issued_tick);
    h.u32(c.province.v);
    h.u32(c.state.v);
    h.u32(c.division.v);
    h.u32(c.army.v);
    h.u32(c.character.v);
    h.u32(c.equipment.v);
    h.u32(c.template_id.v);
    h.u32(c.tech.v);
    h.u32(c.target_country.v);
    h.u32(c.war.v);
    h.i32(c.value);
    h.str(c.text);
    h.u32(static_cast<uint32_t>(c.divisions.size()));
    for (DivisionId d : c.divisions) h.u32(d.v);
    return h.value();
}

struct PlanStats {
    int construction = 0;
    int production = 0;
    int research = 0;
    int military = 0;
    int applied = 0;
    std::vector<uint64_t> signature;  // every queued command + the final world hash
};

// Drives `ticks` AI phases and applies the queue the way phase_commands does, on a
// fresh queue, insisting that every command the AI issued lands as Applied.
void run_ai_days(Fixture& f, int ticks, PlanStats* stats) {
    for (int tick = 0; tick < ticks; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);

        CommandQueue fresh;
        for (Command& c : f.g.queue.pending) fresh.push(std::move(c));
        f.g.queue.clear();
        for (const Command& cmd : fresh.pending) {
            CHECK(cmd.country == f.a);
            CHECK_EQ(validate_command(f.g, cmd), CommandResult::Applied);
            CHECK_EQ(apply_command(f.g, cmd), CommandResult::Applied);
            if (cmd.type == CommandType::StartConstruction) ++stats->construction;
            if (cmd.type == CommandType::SetProductionLine ||
                cmd.type == CommandType::RemoveProductionLine) {
                ++stats->production;
            }
            if (cmd.type == CommandType::StartResearch) ++stats->research;
            if (cmd.type == CommandType::RecruitDivision ||
                cmd.type == CommandType::SetDivisionOrder) {
                ++stats->military;
            }
            ++stats->applied;
            stats->signature.push_back(command_fingerprint(cmd));
        }

        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }
    stats->signature.push_back(world_state_hash(f.g));
}

}  // namespace

HOI_TEST(ai_plans_construction_and_production_through_commands) {
    Fixture f;
    build_world(f);

    PlanStats stats;
    run_ai_days(f, 30 * TICKS_PER_DAY, &stats);

    CHECK_GT(stats.applied, 0);
    CHECK_GT(stats.construction, 0);
    CHECK_GT(stats.production, 0);
    CHECK_GT(stats.research, 0);
    CHECK_GT(stats.military, 0);
    CHECK_GT(f.g.ai.commands_issued, 0ull);
    CHECK_GT(f.g.ai.decisions_made, 0ull);
}

HOI_TEST(ai_plan_is_deterministic) {
    Fixture first;
    build_world(first);
    PlanStats a;
    run_ai_days(first, 30 * TICKS_PER_DAY, &a);
    CHECK_GT(a.signature.size(), 0u);

    Fixture second;
    build_world(second);
    PlanStats b;
    run_ai_days(second, 30 * TICKS_PER_DAY, &b);

    // Same scenario, same seed: the AI must issue the same commands in the same
    // order and leave the same world behind.
    CHECK_EQ(a.signature.size(), b.signature.size());
    CHECK(a.signature == b.signature);
}

HOI_TEST(ai_never_writes_world_state_outside_commands) {
    Fixture f;
    build_world(f);

    // Mid-run tick so the daily layer schedule is not the "never planned" case.
    f.g.world.tick = 100;
    const uint64_t before = world_state_hash(f.g);

    phase_ai(f.g);
    CHECK_GT(f.g.queue.pending.size(), 0u);
    CHECK_EQ(world_state_hash(f.g), before);  // planning alone changes nothing

    f.g.queue.clear();
    CHECK_EQ(world_state_hash(f.g), before);

    // ... and the run did happen: the AI wrote its own state, not the world's.
    CHECK_GT(f.g.ai.commands_issued, 0ull);
    CHECK_GT(f.g.ai.decisions_made, 0ull);
    CHECK(!f.g.ai.layer(AiLayer::Industry).last_reasons.empty());
}

HOI_TEST(ai_ignores_player_controlled_countries) {
    Fixture f;
    build_world(f);
    f.g.set_ai(f.a, false);  // both countries are played by a human

    for (int tick = 0; tick < 30 * TICKS_PER_DAY; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);
        CHECK(f.g.queue.empty());
        f.g.world.tick += 1;
    }
    CHECK_EQ(f.g.ai.commands_issued, 0ull);
    CHECK_EQ(f.g.ai.decisions_made, 0ull);
}

HOI_TEST(ai_records_scored_reasons_and_layer_schedule) {
    Fixture f;
    build_world(f);
    phase_ai(f.g);

    const AiLayerState& industry = f.g.ai.layer(AiLayer::Industry);
    CHECK(!industry.last_reasons.empty());
    CHECK(industry.last_reasons.size() <= 16u);
    for (const AiReason& r : industry.last_reasons) {
        CHECK(!r.what.empty());
        CHECK(!r.factors.empty());
        CHECK(std::isfinite(r.score));
        for (const auto& factor : r.factors) {
            CHECK(!factor.first.empty());
            CHECK(std::isfinite(factor.second));
        }
    }

    // Intervals are pinned on the first plan: daily, with the military layer twice
    // as fast.
    CHECK_EQ(f.g.ai.layer(AiLayer::Industry).interval_ticks, 24u);
    CHECK_EQ(f.g.ai.layer(AiLayer::Military).interval_ticks, 12u);

    phase_ai(f.g);  // same tick: nothing is due twice
    const size_t reasons_after_second_call = f.g.ai.layer(AiLayer::Industry).last_reasons.size();
    CHECK_EQ(reasons_after_second_call, industry.last_reasons.size());
}

HOI_TEST(ai_production_lines_fit_the_factory_budget) {
    Fixture f;
    build_world(f);
    Country* a = f.g.world.country(f.a);

    ai_production_layer(f.g, *a);

    int lines = 0;
    int factories = 0;
    for (const Command& c : f.g.queue.pending) {
        if (c.type != CommandType::SetProductionLine) continue;
        ++lines;
        factories += c.value;
    }
    CHECK_GT(lines, 0);
    CHECK(lines <= 3);
    // Auroria owns four military factories; the plan must fit inside them.
    CHECK(factories <= 4);

    CommandQueue fresh;
    for (Command& c : f.g.queue.pending) fresh.push(std::move(c));
    f.g.queue.clear();
    for (const Command& cmd : fresh.pending) {
        CHECK_EQ(static_cast<int>(validate_command(f.g, cmd)),
                 static_cast<int>(CommandResult::Applied));
        CHECK_EQ(static_cast<int>(apply_command(f.g, cmd)),
                 static_cast<int>(CommandResult::Applied));
    }
    CHECK(!f.g.world.country(f.a)->lines.empty());
}

HOI_TEST(ai_military_layer_forms_armies_for_idle_divisions) {
    Fixture f;
    build_world(f);
    Country* a = f.g.world.country(f.a);
    for (int i = 0; i < 4; ++i) add_division(f.g.world, *a, f.tpl_a, f.front_a, 1.0);

    f.g.queue.clear();
    ai_military_layer(f.g, *a);
    CHECK_EQ(count_commands(f.g.queue, CommandType::CreateArmy), 1);

    CommandQueue fresh;
    for (Command& c : f.g.queue.pending) fresh.push(std::move(c));
    f.g.queue.clear();
    for (const Command& cmd : fresh.pending) {
        CHECK_EQ(static_cast<int>(apply_command(f.g, cmd)),
                 static_cast<int>(CommandResult::Applied));
    }
    CHECK_EQ(f.g.world.country(f.a)->armies.size(), 1u);

    // Second run: the new army exists, so the idle divisions join it.
    ai_military_layer(f.g, *a);
    CHECK_GT(count_commands(f.g.queue, CommandType::AssignDivisionToArmy), 0);
    CHECK_GT(count_commands(f.g.queue, CommandType::RecruitDivision), 0);
    for (const Command& cmd : f.g.queue.pending) {
        CHECK_EQ(static_cast<int>(validate_command(f.g, cmd)),
                 static_cast<int>(CommandResult::Applied));
    }
}
