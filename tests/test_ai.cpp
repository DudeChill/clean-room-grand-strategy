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
#include "sim/navy.h"
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

// A third country that leads a faction and owns nothing: enough for another country
// with the same ideology to ask for membership.
CountryId add_faction_leader(World& w, const char* tag, Ideology ideology, uint32_t faction_id) {
    Country c;
    c.tag = tag;
    c.name = tag;
    c.ideology = ideology;
    c.manpower = 100000.0;
    c.faction = faction_id;
    const CountryId id = w.countries.create(std::move(c));
    w.country(id)->id = id;

    Faction faction;
    faction.id = faction_id;
    faction.name = "Northern League";
    faction.leader = id;
    faction.members.push_back(id);
    w.factions.push_back(faction);
    return id;
}

// A second, weaker neighbour of Auroria: two valid war targets exist, so only the
// declaration rate limit can keep the AI from attacking both at once.
CountryId add_weak_neighbour(Fixture& f, const char* tag, Ideology ideology) {
    World& w = f.g.world;
    Content& ct = f.g.content;
    Country c;
    c.tag = tag;
    c.name = tag;
    c.ideology = ideology;
    c.manpower = 50000.0;
    const CountryId id = w.countries.create(std::move(c));
    w.country(id)->id = id;

    const RegionId region = w.provinces[f.cap_a].region;
    const std::string state_name = std::string(tag) + " Land";
    const StateId state = add_state(w, state_name.c_str(), region, id, {}, 2, 1, 6);
    const ProvinceId prov = add_province(w, "Border Town", state, region, id, 3, true);
    w.state(state)->provinces = {prov};
    w.regions[region].provinces.push_back(prov);
    w.province(prov)->adj = {f.front_a};
    w.province(f.front_a)->adj.push_back(prov);
    w.country(id)->capital = state;

    const std::string tpl_name = std::string(tag) + "_infantry";
    const TemplateId tpl = add_template(ct, tpl_name.c_str(), id,
                                        ct.equipment_id("infantry_equipment_1"), 5, EquipmentId{});
    w.country(id)->templates.push_back(tpl);
    w.country(id)->equipment_stockpile.assign(ct.equipment.size(), 0.0);
    add_division(w, *w.country(id), tpl, prov, 1.0);
    return id;
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
    int fleets = 0;
    int task_forces = 0;
    int naval_missions = 0;
    int ship_assignments = 0;
    int invasions = 0;
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
            if (cmd.type == CommandType::CreateFleet) ++stats->fleets;
            if (cmd.type == CommandType::CreateTaskForce) ++stats->task_forces;
            if (cmd.type == CommandType::SetNavalMission) ++stats->naval_missions;
            if (cmd.type == CommandType::AssignShipToTaskForce) ++stats->ship_assignments;
            if (cmd.type == CommandType::LaunchNavalInvasion) ++stats->invasions;
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

HOI_TEST(ai_joins_a_friendly_faction_when_threatened) {
    Fixture f;
    build_world(f);
    const CountryId leader = add_faction_leader(f.g.world, "NOR", f.g.world.countries[f.a].ideology, 1);
    CHECK_EQ(f.g.world.countries[f.a].ideology, f.g.world.countries[leader].ideology);

    // The war turns against Auroria: Borealis fields three divisions to its one, so
    // the AI should look for a patron instead of standing alone.
    for (int i = 0; i < 2; ++i) {
        add_division(f.g.world, *f.g.world.country(f.b), f.tpl_b, f.cap_b, 1.0);
    }

    for (int tick = 0; tick < 30 * TICKS_PER_DAY; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);
        phase_commands(f.g);  // the real phase: every result lands in Game::log
        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }

    bool joined = false;
    for (const CommandRecord& rec : f.g.log.records) {
        CHECK(rec.command.country != f.b);  // the player's country never receives orders
        if (rec.command.type != CommandType::JoinFaction) continue;
        CHECK_EQ(rec.command.target_country, leader);
        CHECK_EQ(rec.result, CommandResult::Applied);
        joined = true;
    }
    CHECK(joined);
    CHECK_EQ(f.g.world.country(f.a)->faction, 1u);
}

// The same two-country world without the war: both sides at peace, so the only thing
// that can start hostilities is the AI's own declaration.
void build_peaceful_world(Fixture& f) {
    build_world(f);
    World& w = f.g.world;
    w.wars.clear();
    w.relations.clear();
    Country* a = w.country(f.a);
    Country* b = w.country(f.b);
    a->wars.clear();
    b->wars.clear();
    a->at_war = false;
    b->at_war = false;
    f.war = WarId{};
}

HOI_TEST(ai_declares_war_when_it_has_the_advantage) {
    Fixture f;
    build_peaceful_world(f);

    // Auroria fields four divisions to Borealis' one across a shared border: a real
    // advantage against the specific target, which is the only thing that may start
    // a war (the countries are neighbours and of different ideologies).
    Country* a = f.g.world.country(f.a);
    for (int i = 0; i < 3; ++i) add_division(f.g.world, *a, f.tpl_a, f.front_a, 1.0);

    bool declared = false;
    for (int tick = 0; tick < 60 * TICKS_PER_DAY && !declared; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);
        declared = count_commands(f.g.queue, CommandType::DeclareWar) > 0;
        phase_commands(f.g);
        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }

    CHECK(declared);
    CHECK_EQ(f.g.world.wars.size(), 1u);
    CHECK(f.g.world.at_war(f.a, f.b));
    CHECK_EQ(f.g.ai.posture[f.a.v], 2u);  // the posture that authorised the attack
}

HOI_TEST(ai_does_not_declare_war_when_outmatched) {
    Fixture f;
    build_peaceful_world(f);

    // The reverse: Borealis outnumbers Auroria six to one, so Auroria must not start
    // a war and must not even adopt an offensive posture.
    Country* b = f.g.world.country(f.b);
    for (int i = 0; i < 4; ++i) add_division(f.g.world, *b, f.tpl_b, f.cap_b, 1.0);

    for (int tick = 0; tick < 60 * TICKS_PER_DAY; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);
        CHECK_EQ(count_commands(f.g.queue, CommandType::DeclareWar), 0);
        phase_commands(f.g);
        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }

    CHECK_EQ(f.g.world.wars.size(), 0u);
    CHECK(!f.g.world.at_war(f.a, f.b));
    CHECK(f.g.ai.posture[f.a.v] != 2);  // no offensive posture against a superior enemy
}

HOI_TEST(ai_attacks_a_defended_border_and_starts_a_battle) {
    Fixture f;
    build_world(f);  // already at war: Auroria holds the frontier, Borealis the far side

    // Borealis leaves a garrison in the province across the border; Auroria masses
    // four divisions against it. Nothing moves unless the AI orders an attack, and a
    // defended province can only be taken through a battle.
    Country* a = f.g.world.country(f.a);
    Country* b = f.g.world.country(f.b);
    for (int i = 0; i < 3; ++i) add_division(f.g.world, *a, f.tpl_a, f.front_a, 1.0);
    for (int i = 0; i < 2; ++i) add_division(f.g.world, *b, f.tpl_b, f.cap_b, 1.0);

    bool attacked = false;
    bool battle = false;
    for (int tick = 0; tick < 90 * TICKS_PER_DAY && !battle; ++tick) {
        f.g.tick_once();
        if (!attacked) {
            for (const CommandRecord& rec : f.g.log.records) {
                if (rec.command.type == CommandType::MoveDivision) {
                    attacked = true;
                }
            }
        }
        battle = f.g.world.battles.size() > 0;
    }

    CHECK(attacked);
    CHECK(battle);
}

HOI_TEST(ai_limits_declarations_to_one_per_thirty_days) {
    Fixture f;
    build_peaceful_world(f);

    // Auroria is strong, and both Borealis and the new neighbour are weak, bordering
    // and of a different ideology: two legitimate targets, so only the rate limit can
    // stop the AI from declaring on both in the same week.
    Country* a = f.g.world.country(f.a);
    for (int i = 0; i < 3; ++i) add_division(f.g.world, *a, f.tpl_a, f.front_a, 1.0);
    add_weak_neighbour(f, "ZUR", Ideology::Neutrality);

    for (int tick = 0; tick < 60 * TICKS_PER_DAY; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);
        phase_commands(f.g);
        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }

    std::vector<Tick> declarations;
    for (const CommandRecord& rec : f.g.log.records) {
        if (rec.command.type == CommandType::DeclareWar && rec.command.country == f.a) {
            declarations.push_back(rec.tick);
        }
    }
    CHECK(declarations.size() >= 2);  // both targets are eventually attacked
    for (size_t i = 1; i < declarations.size(); ++i) {
        CHECK(declarations[i] - declarations[i - 1] >=
              30ull * static_cast<uint64_t>(TICKS_PER_DAY));
    }
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

// ============================================================= air ============

namespace {

// Gives country A an air capability: a fighter model unlocked by a completed tech,
// an air base at the capital and on the frontier, and aircraft in the stockpile.
// Aircraft are added last, so every vector sized before this call is grown here.
EquipmentId add_air_capability(Fixture& f) {
    Content& ct = f.g.content;
    const EquipmentId fighter = add_equipment(ct, "fighter_1", "fighter",
                                              EquipmentCategory::Aircraft, 1936, 10.0, 0.0, 0.0,
                                              false);
    ct.equipment[fighter.v].air_attack = 12.0;
    ct.equipment[fighter.v].air_defence = 6.0;
    ct.equipment[fighter.v].agility = 20.0;
    ct.equipment[fighter.v].range = 5.0;  // strategic-region hops
    const TechId tech = add_tech(ct, "aircraft_design", "air", 1936, {"fighter_1"},
                                 ModifierKind::Count, 0.0);

    f.g.world.province(f.cap_a)->air_base = 2;
    f.g.world.province(f.front_a)->air_base = 2;

    Country* a = f.g.world.country(f.a);
    a->research.completed.push_back(tech);
    a->equipment_stockpile.resize(ct.equipment.size(), 0.0);
    a->equipment_stockpile[fighter.v] = 400.0;
    return fighter;
}

// Puts one under-strength wing of `model` on the map for country A at `base`.
AirWingId add_wing(Game& g, CountryId country, EquipmentId model, ProvinceId base, int planes,
                   int max_planes) {
    AirWing wing;
    wing.country = country;
    wing.equipment = model;
    wing.base = base;
    const Province* p = g.world.province(base);
    wing.region = p ? p->region : RegionId{};
    wing.planes = planes;
    wing.max_planes = max_planes;
    wing.mission = AirMission::AirSuperiority;
    const AirWingId id = g.world.air_wings.create(wing);
    g.world.air_wings[id].id = id;
    g.world.country(country)->wings.push_back(id);
    return id;
}

}  // namespace

// A country with aircraft in its stockpile and an air base forms a wing and gives it
// a valid mission, all through commands the command system accepts.
HOI_TEST(ai_forms_air_wings_and_assigns_missions) {
    Fixture f;
    build_world(f);
    add_air_capability(f);

    int creates = 0;
    int missions = 0;
    bool mission_region_valid = false;
    for (int tick = 0; tick < 30 * TICKS_PER_DAY; ++tick) {
        f.g.queue.clear();
        phase_ai(f.g);

        CommandQueue fresh;
        for (Command& c : f.g.queue.pending) fresh.push(std::move(c));
        f.g.queue.clear();
        for (const Command& cmd : fresh.pending) {
            CHECK(cmd.country == f.a);  // the player's country never receives orders
            CHECK_EQ(validate_command(f.g, cmd), CommandResult::Applied);
            CHECK_EQ(apply_command(f.g, cmd), CommandResult::Applied);
            if (cmd.type == CommandType::CreateAirWing) ++creates;
            if (cmd.type == CommandType::SetAirMission) {
                ++missions;
                if (cmd.region.valid()) mission_region_valid = true;
            }
        }
        f.g.world.tick += 1;
        f.g.ticks_run = f.g.world.tick;
    }

    CHECK_GT(creates, 0);
    CHECK_GT(missions, 0);
    CHECK(mission_region_valid);

    // The wing that was ordered into existence is real, manned from the stockpile.
    const Country* a = f.g.world.country(f.a);
    CHECK_GT(a->wings.size(), 0u);
    const AirWing* wing = f.g.world.wing(a->wings.front());
    CHECK(wing != nullptr);
    CHECK(wing->planes >= 50);
    CHECK(wing->mission != AirMission::None);
}

// A country with no aircraft must not conjure wings out of nothing - neither when it
// has an air base but no aircraft model to build, nor when aircraft exist in the
// world but the country controls no air base.
HOI_TEST(ai_issues_no_air_commands_without_aircraft) {
    {
        Fixture f;
        build_world(f);
        f.g.world.province(f.cap_a)->air_base = 2;  // a base with nothing to fly

        for (int tick = 0; tick < 30 * TICKS_PER_DAY; ++tick) {
            f.g.queue.clear();
            phase_ai(f.g);
            CHECK_EQ(count_commands(f.g.queue, CommandType::CreateAirWing), 0);
            CHECK_EQ(count_commands(f.g.queue, CommandType::SetAirMission), 0);
            phase_commands(f.g);
            f.g.world.tick += 1;
            f.g.ticks_run = f.g.world.tick;
        }
        CHECK_EQ(f.g.world.country(f.a)->wings.size(), 0u);
    }
    {
        Fixture f;
        build_world(f);
        const EquipmentId model = add_air_capability(f);
        (void)model;
        // No air base anywhere: there is nowhere to station a wing.
        f.g.world.province(f.cap_a)->air_base = 0;
        f.g.world.province(f.front_a)->air_base = 0;

        for (int tick = 0; tick < 30 * TICKS_PER_DAY; ++tick) {
            f.g.queue.clear();
            phase_ai(f.g);
            CHECK_EQ(count_commands(f.g.queue, CommandType::CreateAirWing), 0);
            phase_commands(f.g);
            f.g.world.tick += 1;
            f.g.ticks_run = f.g.world.tick;
        }
        CHECK_EQ(f.g.world.country(f.a)->wings.size(), 0u);
    }
}

// Aircraft are producible: with an air base the country opens a line for the model
// its wings would use, otherwise no aircraft model can ever reach the stockpile and
// the wings above could never be manned.
HOI_TEST(ai_opens_a_production_line_for_aircraft) {
    Fixture f;
    build_world(f);
    const EquipmentId model = add_air_capability(f);

    f.g.queue.clear();
    ai_production_layer(f.g, *f.g.world.country(f.a));

    bool opened = false;
    for (const Command& c : f.g.queue.pending) {
        if (c.type != CommandType::SetProductionLine) continue;
        if (c.equipment == model) opened = true;
        CHECK_EQ(validate_command(f.g, c), CommandResult::Applied);
    }
    CHECK(opened);
}

// A wing whose base is captured is rebased to a friendly air base, or disbanded when
// the country controls no air base at all.
HOI_TEST(ai_rebases_or_disbands_a_wing_whose_base_is_lost) {
    {
        Fixture f;
        build_world(f);
        const EquipmentId model = add_air_capability(f);
        add_wing(f.g, f.a, model, f.cap_a, 80, 100);
        // The capital falls; the frontier base remains.
        f.g.world.province(f.cap_a)->controller = f.b;

        ai_military_layer(f.g, *f.g.world.country(f.a));

        CHECK_EQ(count_commands(f.g.queue, CommandType::DeployAirWing), 1);
        CHECK_EQ(count_commands(f.g.queue, CommandType::DisbandAirWing), 0);
        const Command* rebase = nullptr;
        for (const Command& c : f.g.queue.pending) {
            if (c.type == CommandType::DeployAirWing) rebase = &c;
        }
        CHECK(rebase != nullptr);
        CHECK_EQ(validate_command(f.g, *rebase), CommandResult::Applied);
        CHECK_EQ(apply_command(f.g, *rebase), CommandResult::Applied);
        const AirWing* wing = f.g.world.wing(f.g.world.country(f.a)->wings.front());
        CHECK(wing != nullptr);
        CHECK_EQ(wing->base, f.front_a);
    }
    {
        Fixture f;
        build_world(f);
        const EquipmentId model = add_air_capability(f);
        add_wing(f.g, f.a, model, f.cap_a, 80, 100);
        // Every air base is lost: the wing has nowhere to go.
        f.g.world.province(f.cap_a)->controller = f.b;
        f.g.world.province(f.front_a)->controller = f.b;

        ai_military_layer(f.g, *f.g.world.country(f.a));

        CHECK_EQ(count_commands(f.g.queue, CommandType::DisbandAirWing), 1);
        CHECK_EQ(count_commands(f.g.queue, CommandType::DeployAirWing), 0);
    }
}

// ============================================================= navy ==========

namespace {

struct NavalParts {
    EquipmentId destroyer;
    EquipmentId cruiser;
    EquipmentId convoy;
    RegionId sea_region;
    ProvinceId sea_zone;
    ProvinceId port_a;
    ProvinceId coast_b;
};

// Gives country A a navy: unlocked destroyer/cruiser/convoy models, hulls and
// convoys in the stockpile, dockyards to build more, and a sea zone joining an A
// port to a hostile B coast.
NavalParts add_naval_capability(Fixture& f) {
    Content& ct = f.g.content;
    World& w = f.g.world;
    NavalParts np;

    np.destroyer = add_equipment(ct, "destroyer_1", "destroyer", EquipmentCategory::Ship, 1936,
                                 4.0, 0.0, 0.0, false);
    ct.equipment[np.destroyer.v].naval_attack = 12.0;
    ct.equipment[np.destroyer.v].torpedo_attack = 6.0;
    ct.equipment[np.destroyer.v].sub_detection = 8.0;
    ct.equipment[np.destroyer.v].detection = 10.0;
    ct.equipment[np.destroyer.v].visibility = 0.6;
    ct.equipment[np.destroyer.v].max_strength = 1.0;

    np.cruiser = add_equipment(ct, "cruiser_1", "cruiser", EquipmentCategory::Ship, 1936, 9.0,
                               0.0, 0.0, false);
    ct.equipment[np.cruiser.v].naval_attack = 30.0;
    ct.equipment[np.cruiser.v].armor = 20.0;
    ct.equipment[np.cruiser.v].detection = 8.0;
    ct.equipment[np.cruiser.v].visibility = 0.7;
    ct.equipment[np.cruiser.v].max_strength = 1.0;

    np.convoy = add_equipment(ct, "convoy_1", "convoy", EquipmentCategory::Convoy, 1936, 1.0, 0.0,
                              0.0, false);
    ct.equipment[np.convoy.v].max_strength = 1.0;

    add_tech(ct, "basic_destroyer", "navy", 1936, {"destroyer_1"}, ModifierKind::Count, 0.0);
    add_tech(ct, "basic_cruiser", "navy", 1936, {"cruiser_1"}, ModifierKind::Count, 0.0);
    add_tech(ct, "basic_convoy", "navy", 1936, {"convoy_1"}, ModifierKind::Count, 0.0);
    add_building(ct, BuildingKind::Dockyard, "dockyard", 10800.0, 10);

    Region sreg;
    sreg.name = "Northern Sea";
    sreg.is_sea = true;
    np.sea_region = w.regions.create(std::move(sreg));
    w.regions[np.sea_region].id = np.sea_region;

    Province sz;
    sz.name = "North Sea Zone";
    sz.region = np.sea_region;
    sz.is_sea = true;
    np.sea_zone = w.provinces.create(std::move(sz));
    w.province(np.sea_zone)->id = np.sea_zone;

    const RegionId land_region = w.province(f.cap_a)->region;
    np.port_a = add_province(w, "Auroria Port", f.state_a, land_region, f.a, 5, false);
    w.province(np.port_a)->coastal = true;
    w.province(np.port_a)->naval_base = 3;
    w.province(np.port_a)->sea_adj.push_back(np.sea_zone);
    w.state(f.state_a)->provinces.push_back(np.port_a);

    np.coast_b = add_province(w, "Borealis Coast", f.state_b, land_region, f.b, 4, false);
    w.province(np.coast_b)->coastal = true;
    w.province(np.coast_b)->sea_adj.push_back(np.sea_zone);
    w.state(f.state_b)->provinces.push_back(np.coast_b);

    w.state(f.state_a)->dockyards = 4;
    Country* a = w.country(f.a);
    a->research.completed.push_back(ct.tech_id("basic_destroyer"));
    a->research.completed.push_back(ct.tech_id("basic_cruiser"));
    a->research.completed.push_back(ct.tech_id("basic_convoy"));
    a->equipment_stockpile.resize(ct.equipment.size(), 0.0);
    a->equipment_stockpile[np.destroyer.v] = 10.0;
    a->equipment_stockpile[np.convoy.v] = 50.0;
    return np;
}

// Puts a real task force of `count` ships on the map, so a test can hand the country
// naval control without relying on the naval phase to move it into the zone.
TaskForceId seed_task_force(Game& g, CountryId country, ProvinceId port, RegionId zone,
                            EquipmentId model, int count, bool register_fleet = true) {
    World& w = g.world;
    Country* c = w.country(country);
    Fleet fleet;
    fleet.country = country;
    fleet.name = "Seeded Fleet";
    const FleetId fid = w.fleets.create(std::move(fleet));
    w.fleet(fid)->id = fid;
    if (register_fleet) c->fleets.push_back(fid);

    TaskForce tf;
    tf.country = country;
    tf.fleet = fid;
    tf.name = "Seeded TF";
    tf.port = port;
    tf.sea_region = zone;
    tf.at_sea = true;
    tf.mission = NavalMission::Patrol;
    const TaskForceId tid = w.task_forces.create(std::move(tf));
    w.task_force(tid)->id = tid;
    w.fleet(fid)->task_forces.push_back(tid);

    for (int i = 0; i < count; ++i) {
        Ship s;
        s.country = country;
        s.equipment = model;
        s.fleet = fid;
        s.task_force = tid;
        s.port = port;
        s.sea_region = zone;
        s.at_sea = true;
        s.strength = 1.0;
        s.organisation = 1.0;
        const ShipId sid = w.ships.create(std::move(s));
        w.ship(sid)->id = sid;
        w.task_force(tid)->ships.push_back(sid);
    }
    return tid;
}

// Gives country A an army whose division stands in its port, ready to embark.
ArmyId add_landing_army(Fixture& f, ProvinceId port) {
    World& w = f.g.world;
    Country* a = w.country(f.a);
    add_division(w, *a, f.tpl_a, port, 1.0);
    const DivisionId did = a->divisions.back();
    Army army;
    army.country = f.a;
    army.name = "Landing Army";
    const ArmyId aid = w.armies.create(std::move(army));
    w.army(aid)->id = aid;
    w.army(aid)->divisions.push_back(did);
    w.division(did)->army = aid;
    a->armies.push_back(aid);
    return aid;
}

}  // namespace

// A coastal country with hulls in the stockpile forms a fleet and a task force at
// its port within thirty days, and gives that force a valid mission - all through
// commands the command system accepts.
HOI_TEST(ai_forms_a_naval_task_force_and_assigns_missions) {
    Fixture f;
    build_world(f);
    add_naval_capability(f);

    PlanStats stats;
    run_ai_days(f, 30 * TICKS_PER_DAY, &stats);

    CHECK_GT(stats.fleets, 0);
    CHECK_GT(stats.task_forces, 0);
    CHECK_GT(stats.naval_missions, 0);

    const Country* a = f.g.world.country(f.a);
    CHECK_GT(a->fleets.size(), 0u);

    int ships = 0;
    bool mission_set = false;
    f.g.world.task_forces.for_each([&](TaskForceId, const TaskForce& tf) {
        if (tf.country != f.a) return;
        ships += static_cast<int>(tf.ships.size());
        if (tf.mission != NavalMission::None) mission_set = true;
    });
    CHECK_GT(ships, 0);
    CHECK(mission_set);
}

// Hulls and convoys are production planning: the production layer opens dockyard
// lines for the models the navy needs, inside the dockyard budget, and never spends
// military factories on them.
HOI_TEST(ai_opens_dockyard_lines_for_ships_and_convoys) {
    Fixture f;
    build_world(f);
    add_naval_capability(f);

    f.g.queue.clear();
    ai_production_layer(f.g, *f.g.world.country(f.a));

    int ship_factories = 0;
    int convoy_factories = 0;
    int military_factories = 0;
    bool ship_line = false;
    bool convoy_line = false;
    for (const Command& c : f.g.queue.pending) {
        if (c.type != CommandType::SetProductionLine) continue;
        CHECK_EQ(validate_command(f.g, c), CommandResult::Applied);
        const EquipmentDef* def = f.g.content.equipment_def(c.equipment);
        if (def && def->category == EquipmentCategory::Ship) {
            ship_factories += c.value;
            ship_line = true;
        } else if (def && def->category == EquipmentCategory::Convoy) {
            convoy_factories += c.value;
            convoy_line = true;
        } else {
            military_factories += c.value;
        }
    }

    CHECK(ship_line);
    CHECK(convoy_line);
    // The dockyard split never exceeds the controlled dockyards.
    const int dockyards = f.g.world.state(f.state_a)->dockyards;
    CHECK(ship_factories + convoy_factories <= dockyards);
    CHECK_GT(ship_factories + convoy_factories, 0);
    // Ships never eat the four military factories the fixture owns.
    CHECK(military_factories <= 4);
}

// At war, with transports, a landing army at a usable port and naval superiority in
// the crossing zone, the AI eventually launches an invasion through the command
// system.
HOI_TEST(ai_launches_an_invasion_with_naval_superiority) {
    Fixture f;
    build_world(f);
    const NavalParts np = add_naval_capability(f);

    add_landing_army(f, np.port_a);
    // Eight destroyers holding the crossing zone: naval superiority over B's coast.
    seed_task_force(f.g, f.a, np.port_a, np.sea_region, np.destroyer, 8);

    PlanStats stats;
    run_ai_days(f, 30 * TICKS_PER_DAY, &stats);

    CHECK_GT(stats.invasions, 0);
    CHECK_GT(f.g.world.invasions.size(), 0u);
}

// A country without naval superiority must not gamble on a landing: it holds its
// ships at home and issues no invasion.
HOI_TEST(ai_does_not_invade_without_superiority) {
    Fixture f;
    build_world(f);
    const NavalParts np = add_naval_capability(f);

    add_landing_army(f, np.port_a);
    // No hulls in the crossing zone: no naval control, so no invasion may start.
    f.g.world.country(f.a)->equipment_stockpile[np.destroyer.v] = 0.0;

    PlanStats stats;
    run_ai_days(f, 30 * TICKS_PER_DAY, &stats);

    CHECK_EQ(stats.invasions, 0);
    CHECK_EQ(f.g.world.invasions.size(), 0u);
}

// Same scenario, same seed: the naval plan and the world it leaves behind are
// identical on two runs.
HOI_TEST(ai_naval_plan_is_deterministic) {
    Fixture first;
    build_world(first);
    const NavalParts np1 = add_naval_capability(first);
    add_landing_army(first, np1.port_a);
    PlanStats a;
    run_ai_days(first, 30 * TICKS_PER_DAY, &a);

    Fixture second;
    build_world(second);
    const NavalParts np2 = add_naval_capability(second);
    add_landing_army(second, np2.port_a);
    PlanStats b;
    run_ai_days(second, 30 * TICKS_PER_DAY, &b);

    CHECK_EQ(a.signature.size(), b.signature.size());
    CHECK(a.signature == b.signature);
    CHECK_EQ(world_hash(first.g), world_hash(second.g));
}

// Ninety simulated days of the naval layer: the fleet, task force and mission chain
// runs, and the whole mix of naval commands is issued through the command system.
HOI_TEST(ai_naval_commands_run_for_ninety_days) {
    Fixture f;
    build_world(f);
    const NavalParts np = add_naval_capability(f);
    add_landing_army(f, np.port_a);
    // The force is seeded without registering a fleet, so the AI must create one.
    seed_task_force(f.g, f.a, np.port_a, np.sea_region, np.destroyer, 8, false);

    PlanStats stats;
    run_ai_days(f, 90 * TICKS_PER_DAY, &stats);

    CHECK_GT(stats.fleets, 0);
    CHECK_GT(stats.task_forces, 0);
    CHECK_GT(stats.naval_missions, 0);
    CHECK_GT(stats.invasions, 0);
    CHECK_GT(stats.applied, 0);
}
