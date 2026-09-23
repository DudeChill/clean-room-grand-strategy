#pragma once
// Authoritative world state.
//
// Everything the simulation needs lives here exactly once. UI, AI, save files and
// network replication all read from these structures; nothing keeps a private
// mirror of gameplay state (see docs/STATE_OWNERSHIP.md).

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/store.h"
#include "core/types.h"
#include "sim/units.h"

namespace hoi {

// ---------------------------------------------------------------- map model --

struct Province {
    ProvinceId id;
    std::string name;
    StateId state;
    RegionId region;
    Terrain terrain = Terrain::Plains;
    CountryId owner;       // political owner
    CountryId controller;  // military controller (may differ from owner)
    std::vector<ProvinceId> adj;      // land neighbours, sorted by id
    std::vector<ProvinceId> sea_adj;  // adjacent sea zones (empty if landlocked)
    bool coastal = false;
    bool is_sea = false;
    int x = 0;  // grid coordinates from the map data, used for rendering only
    int y = 0;
    int victory_points = 0;
    int infrastructure = 3;  // 0..10
    int fort_level = 0;
    int air_base = 0;
    int naval_base = 0;
    int radar = 0;
    bool supply_hub = false;
    int railway_level = 0;  // 0..5
    double population = 0.0;
    double resource_yield[RESOURCE_COUNT] = {};
    bool is_capital = false;

    // Last logistics result, kept for UI/debug ("why is this division unsupplied").
    double supply_level = 1.0;
    ProvinceId supply_source;
    ProvinceId supply_bottleneck;
};

struct State {
    StateId id;
    std::string name;
    CountryId owner;
    CountryId controller;
    RegionId region;
    std::vector<ProvinceId> provinces;
    std::vector<CountryId> core_owners;
    int civilian_factories = 0;
    int military_factories = 0;
    int dockyards = 0;
    int building_slots = 0;
    double manpower_pool = 0.0;  // recruitable manpower residing in this state
    bool impassable = false;

    // Occupation model.
    double resistance = 0.0;
    double compliance = 0.0;
    double garrison_required = 0.0;

    [[nodiscard]] int total_factories() const {
        return civilian_factories + military_factories + dockyards;
    }
};

struct Region {  // strategic region: weather + future air/naval grouping
    RegionId id;
    std::string name;
    bool is_sea = false;
    std::vector<ProvinceId> provinces;
    // Weather (recomputed daily).
    double temperature = 15.0;
    bool rain = false;
    bool snow = false;
    bool mud = false;
    bool sandstorm = false;
};

// ------------------------------------------------------------ economy state --

struct ProductionLine {
    EquipmentId equipment;   // what the line builds today
    int factories = 0;       // assigned military factories
    double efficiency = 0.10;      // 0.10 .. 1.0
    double efficiency_cap = 0.50;  // ceiling efficiency grows while producing
    double output_today = 0.0;
    double output_total = 0.0;
    double resource_shortage = 0.0;  // 0 = fully supplied, 1 = no resources
    Tick started = 0;
    std::vector<EquipmentId> previous;  // switched-from models (efficiency retention)
};

enum class BuildingKind : uint8_t {
    CivilianFactory = 0,
    MilitaryFactory,
    Dockyard,
    Infrastructure,
    Railway,
    SupplyHub,
    AirBase,
    NavalBase,
    Radar,
    Fort,
    AntiAir,
    SyntheticRefinery,
    Count
};

const char* building_kind_name(BuildingKind k);

struct ConstructionProject {
    BuildingKind kind = BuildingKind::CivilianFactory;
    ProvinceId province;
    StateId state;
    int target_level = 1;
    double progress = 0.0;  // accumulated industrial capacity-days
    double cost = 0.0;      // total capacity-days required
    bool repair = false;
};

struct ConstructionState {
    std::vector<ConstructionProject> queue;
    int max_queue_size = 15;
};

// ------------------------------------------------------------- research ------

struct ResearchSlot {
    TechId tech;
    double progress = 0.0;  // research-days accumulated
    bool active = false;
};

struct ResearchState {
    std::vector<TechId> completed;
    std::vector<ResearchSlot> slots;
    int slots_unlocked = 3;

    [[nodiscard]] bool has_tech(TechId t) const {
        for (TechId c : completed)
            if (c == t) return true;
        return false;
    }
};

// --------------------------------------------------------------- military ----

enum class OrderKind : uint8_t {
    None = 0,
    FrontLine,
    Offensive,
    Fallback,
    Garrison,
    NavalInvasion,
    Paradrop,
    Count
};

const char* order_kind_name(OrderKind k);

struct Division {
    DivisionId id;
    CountryId country;
    TemplateId template_id;
    std::string name;
    ProvinceId location;
    ProvinceId previous_location;  // last province before current move (retreat origin)

    double organization = 0.0;
    double max_organization = 0.0;
    double strength = 1.0;  // 0..1 share of template manpower/HP present
    double experience = 0.0;
    double entrenchment = 0.0;
    double planning = 0.0;
    double supply = 1.0;  // 0..1 from logistics
    double fuel = 1.0;    // 0..1 fuel satisfaction
    double manpower = 0.0;
    std::vector<double> equipment;  // current count per EquipmentId index

    ArmyId army;
    OrderKind order = OrderKind::None;
    ProvinceId order_target;

    // Movement.
    bool moving = false;
    ProvinceId move_from;
    ProvinceId move_to;
    double move_progress = 0.0;  // 0..1 across the current edge
    std::vector<ProvinceId> path;  // remaining path including move_to as path[0]
    bool retreating = false;

    BattleId battle;  // active battle, if any
    double combat_attack_modifier = 0.0;  // scratch for debugger

    // Training: a division with an invalid location is off-map. It consumes
    // equipment and manpower until training_days_left reaches 0; movement, combat
    // and supply phases must skip it.
    double training_days_left = 0.0;

    double losses_manpower = 0.0;
    double losses_equipment = 0.0;
    Tick created_tick = 0;

    [[nodiscard]] bool in_training() const { return !location.valid(); }
    [[nodiscard]] bool in_combat() const { return battle.valid(); }
};

struct ArmyOrder {
    OrderKind kind = OrderKind::None;
    std::vector<ProvinceId> line;         // front / fallback provinces (ordered)
    std::vector<ProvinceId> target_line;  // offensive target provinces
    double progress = 0.0;                // plan preparation 0..1
    Tick started = 0;
};

struct Army {
    ArmyId id;
    CountryId country;
    std::string name;
    CharacterId general;
    std::vector<DivisionId> divisions;
    ArmyOrder order;
    uint8_t stance = 1;    // 0 = aggressive, 1 = defensive, 2 = garrison
    int motorization = 0;  // 0..5 supply motorisation level
};

struct BattleSideState {
    std::vector<DivisionId> divisions;
    std::vector<double> width_used;  // parallel to divisions
    double total_soft_attack = 0.0;
    double total_hard_attack = 0.0;
    double total_defense = 0.0;
    double total_breakthrough = 0.0;
    double total_armor = 0.0;
    double total_piercing = 0.0;
};

struct BattleDebugLine {
    DivisionId division;
    double base_attack = 0.0;
    double planning_mod = 0.0;
    double terrain_mod = 0.0;
    double supply_mod = 0.0;
    double commander_mod = 0.0;
    double experience_mod = 0.0;
    double final_attack = 0.0;
    double enemy_defense = 0.0;
    double damage = 0.0;
    double org_damage = 0.0;
    double strength_damage = 0.0;
};

struct Battle {
    BattleId id;
    ProvinceId province;
    Tick start_tick = 0;
    BattleSideState attacker;
    BattleSideState defender;
    double progress = 0.0;  // 0..1 attacker progress
    Terrain terrain = Terrain::Plains;
    bool river_crossing = false;
    bool encirclement = false;  // defender is cut off from supply
    CountryId attacker_lead;
    CountryId defender_lead;
    std::vector<BattleDebugLine> debug;  // last tick breakdown, capped
    Tick last_tick = 0;
};

// ------------------------------------------------------------- diplomacy -----

struct WarGoal {
    CountryId claimant;
    CountryId target;
    StateId state;      // INVALID for annex/puppet wars
    bool annex_country = false;
    bool puppet = false;
};

struct WarParticipant {
    CountryId country;
    double casualties_manpower = 0.0;
    double casualties_equipment = 0.0;
    double occupation_share = 0.0;
};

struct War {
    WarId id;
    std::vector<WarParticipant> attackers;
    std::vector<WarParticipant> defenders;
    std::vector<WarGoal> goals;
    Tick start_tick = 0;
    bool active = true;
    CountryId aggressor;  // who declared
};

struct Relation {
    double value = 0.0;  // -100..100
    bool non_aggression = false;
    bool military_access = false;
    bool guarantee = false;
    bool at_war = false;
};

struct Faction {
    uint32_t id = 0;
    std::string name;
    CountryId leader;
    std::vector<CountryId> members;
};

// -------------------------------------------------------------- country ------

struct TrainingDivision {
    DivisionId division;  // created immediately, location INVALID until deployed
    TemplateId template_id;
    double days_left = 0.0;
};

struct Country {
    CountryId id;
    std::string tag;
    std::string name;
    bool alive = true;
    Ideology ideology = Ideology::Neutrality;
    CountryId overlord;  // INVALID when independent
    std::vector<CountryId> puppets;
    StateId capital;

    double political_power = 0.0;
    double stability = 0.5;
    double war_support = 0.5;
    double manpower = 0.0;  // recruitable manpower pool (persons)
    double fuel = 0.0;
    double fuel_capacity = 0.0;

    std::vector<double> equipment_stockpile;  // per EquipmentId
    std::vector<ProductionLine> lines;
    ConstructionState construction;
    ResearchState research;
    std::vector<uint8_t> law_levels;
    Modifiers base_modifiers;
    Modifiers tech_modifiers;
    Modifiers law_modifiers;
    Modifiers national_modifiers;

    std::vector<DivisionId> divisions;
    std::vector<ArmyId> armies;
    std::vector<TemplateId> templates;
    std::vector<CharacterId> generals;
    std::vector<TrainingDivision> training;  // divisions being trained (off-map)
    std::vector<WarId> wars;
    uint32_t faction = 0;  // 0 = none

    double consumer_goods_ratio = 0.35;
    double army_experience = 0.0;
    bool at_war = false;
    bool fuel_priority = false;  // armoured/air formations draw fuel first
    int starting_factories = 0;  // baseline for capitulation thresholds
    Tick last_capitulation_check = 0;

    // Aggregated each tick for UI/AI: per-resource produced/consumed/imported.
    double resources_produced[RESOURCE_COUNT] = {};
    double resources_consumed[RESOURCE_COUNT] = {};
    double resources_imported[RESOURCE_COUNT] = {};
    double resources_exported[RESOURCE_COUNT] = {};

    [[nodiscard]] Modifiers total_modifiers() const {
        Modifiers m = base_modifiers;
        m.add(tech_modifiers);
        m.add(law_modifiers);
        m.add(national_modifiers);
        return m;
    }
};

struct Character {
    CharacterId id;
    CountryId country;
    std::string name;
    bool is_general = true;
    int skill = 1;
    int attack = 1;
    int defense = 1;
    int planning = 1;
    int logistics = 1;
    ArmyId army;
};

// ---------------------------------------------------------------- world ------

struct World {
    Store<Province> provinces;
    Store<State> states;
    Store<Region> regions;
    Store<Country> countries;
    Store<Division> divisions;
    Store<Army> armies;
    Store<Battle> battles;
    Store<War> wars;
    Store<Character> characters;

    std::vector<Faction> factions;
    // Relations keyed by ordered pair (low id first) for deterministic iteration.
    std::map<std::pair<uint32_t, uint32_t>, Relation> relations;

    Tick tick = 0;
    GameDate date;
    uint64_t world_seed = 0;

    // Accessors ------------------------------------------------------------
    [[nodiscard]] Province* province(ProvinceId id) { return provinces.try_get(id); }
    [[nodiscard]] const Province* province(ProvinceId id) const { return provinces.try_get(id); }
    [[nodiscard]] State* state(StateId id) { return states.try_get(id); }
    [[nodiscard]] const State* state(StateId id) const { return states.try_get(id); }
    [[nodiscard]] Country* country(CountryId id) { return countries.try_get(id); }
    [[nodiscard]] const Country* country(CountryId id) const { return countries.try_get(id); }
    [[nodiscard]] Division* division(DivisionId id) { return divisions.try_get(id); }
    [[nodiscard]] const Division* division(DivisionId id) const { return divisions.try_get(id); }
    [[nodiscard]] Battle* battle(BattleId id) { return battles.try_get(id); }
    [[nodiscard]] const Battle* battle(BattleId id) const { return battles.try_get(id); }
    [[nodiscard]] Army* army(ArmyId id) { return armies.try_get(id); }
    [[nodiscard]] const Army* army(ArmyId id) const { return armies.try_get(id); }
    [[nodiscard]] War* war(WarId id) { return wars.try_get(id); }
    [[nodiscard]] const War* war(WarId id) const { return wars.try_get(id); }
    [[nodiscard]] Character* character(CharacterId id) { return characters.try_get(id); }
    [[nodiscard]] const Character* character(CharacterId id) const {
        return characters.try_get(id);
    }

    [[nodiscard]] CountryId province_owner(ProvinceId id) const {
        const Province* p = province(id);
        return p ? p->owner : CountryId{};
    }
    [[nodiscard]] CountryId province_controller(ProvinceId id) const {
        const Province* p = province(id);
        return p ? p->controller : CountryId{};
    }

    [[nodiscard]] Relation& relation(CountryId a, CountryId b) {
        uint32_t lo = a.v < b.v ? a.v : b.v;
        uint32_t hi = a.v < b.v ? b.v : a.v;
        return relations[{lo, hi}];
    }
    [[nodiscard]] const Relation* find_relation(CountryId a, CountryId b) const {
        uint32_t lo = a.v < b.v ? a.v : b.v;
        uint32_t hi = a.v < b.v ? b.v : a.v;
        auto it = relations.find({lo, hi});
        return it == relations.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool at_war(CountryId a, CountryId b) const;

    // True when `country` may move through / draw supply from the province.
    [[nodiscard]] bool has_access(CountryId c, ProvinceId p) const;
};

}  // namespace hoi
