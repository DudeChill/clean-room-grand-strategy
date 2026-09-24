// Persistence tests: save/load round trips, subsystem hashing, the desync
// diagnostic, save versions and deterministic replays (ARCHITECTURE.md section 8).
//
// The world under test is built by hand rather than loaded from data/: the test then
// covers every subsystem with non-empty, deliberately varied content, and it does not
// depend on which scenario data happens to be installed.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/binio.h"
#include "core/hash.h"
#include "core/rng.h"
#include "data/content.h"
#include "game/game.h"
#include "save/save.h"
#include "test.h"

namespace {

using namespace hoi;

constexpr uint64_t kSeed = 0x5EED1234ull;
constexpr uint64_t kTicks = 200;

// AI planning is left off while the world is ticked: this test hands the AI layers a
// hand-built (and therefore sparse) content database, and the AI's own tests cover
// its planning. AiState is still filled in and hashed.
constexpr bool kEnableAiDuringTicks = false;

template <typename T>
T* add(Store<T>& st) {
    return st.try_get(st.create());
}

// --------------------------------------------------------------- temp files --

// Temp files are owned by one static object so that removal happens while the path
// list is still alive: a second static function holding the vector would be destroyed
// before (or after) this one depending on the translation unit, and touching a
// destroyed vector in a destructor aborts the process at exit.
struct TempFiles {
    std::vector<std::string> paths;

    ~TempFiles() {
        std::error_code ec;
        for (const std::string& p : paths) std::filesystem::remove(p, ec);
    }
};

TempFiles& temp_files() {
    static TempFiles files;
    return files;
}

std::string temp_path(const std::string& name) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path(ec);
    const std::string path = (dir / ("hoi_persistence_" + name)).string();
    temp_files().paths.push_back(path);
    return path;
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void require(bool ok, const std::string& what, const std::string& err) {
    if (!ok) ::hoi_test::fail(__FILE__, __LINE__, what + ": " + err);
}

// Container layout, used to patch specific bytes of a save file in the tests below.
// The header is skipped by scanning for the section table instead of re-encoding the
// header layout: the table is identifiable because it is nine records in tag order,
// each (tag, byte length, hash, payload), that consume the file up to the world hash.
struct SaveLayout {
    std::vector<size_t> payload_offset;
    std::vector<uint32_t> payload_length;
    size_t world_hash_offset = 0;
    size_t log_count_offset = 0;
};

uint32_t peek_u32(const std::vector<uint8_t>& bytes, size_t off) {
    return static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8) |
           (static_cast<uint32_t>(bytes[off + 2]) << 16) |
           (static_cast<uint32_t>(bytes[off + 3]) << 24);
}

SaveLayout parse_save(const std::vector<uint8_t>& bytes) {
    const size_t kinds = static_cast<size_t>(Subsystem::Count);
    for (size_t start = 4; start + 13 * kinds + 12 < bytes.size(); ++start) {
        std::vector<size_t> offsets(kinds, 0);
        std::vector<uint32_t> lengths(kinds, 0);
        size_t p = start;
        bool ok = true;
        for (size_t i = 0; i < kinds; ++i) {
            if (bytes[p] != static_cast<uint8_t>(i)) {
                ok = false;
                break;
            }
            const uint32_t length = peek_u32(bytes, p + 1);
            if (p + 13 + length > bytes.size()) {
                ok = false;
                break;
            }
            offsets[i] = p + 13;
            lengths[i] = length;
            p += 13 + static_cast<size_t>(length);
        }
        if (!ok || p + 12 > bytes.size()) continue;
        const uint32_t log_count = peek_u32(bytes, p + 8);
        if (p + 12 + static_cast<uint64_t>(log_count) * 14 > bytes.size()) continue;

        SaveLayout layout;
        layout.payload_offset = offsets;
        layout.payload_length = lengths;
        layout.world_hash_offset = p;
        layout.log_count_offset = p + 8;
        return layout;
    }
    ::hoi_test::fail(__FILE__, __LINE__, "parse_save: no valid section table found");
    return SaveLayout{};
}

// ----------------------------------------------------------- hand-built world --

EquipmentId add_equipment(Content& c, const char* key, const char* name, EquipmentCategory cat,
                          double cost, double soft, double def, double org, double speed) {
    EquipmentDef d;
    d.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    d.key = key;
    d.name = name;
    d.category = cat;
    d.year = 1936;
    d.archetype = key;
    d.soft_attack = soft;
    d.defense = def;
    d.organization = org;
    d.speed = speed;
    d.build_cost = cost;
    d.max_strength = 1.0;
    d.manpower = 100.0;
    d.supply_use = 0.1;
    d.reliability = 0.9;
    d.resources[static_cast<int>(Resource::Steel)] = 2.0;
    c.equipment.push_back(d);
    c.equipment_by_key[key] = d.id;
    return d.id;
}

// Three countries (ALB/BRV/CDA) on eight provinces (seven land, one sea), two
// regions, four states, one war, one faction, one battle, one army, two divisions
// and one air wing per country: enough to exercise every subsystem with real
// content.
void build_world(Game* g) {
    g->seed = kSeed;
    g->scenario_path = "scenarios/persistence_test.json";
    g->data_root = "data";
    g->start_date = GameDate{1936, 1, 1, 0};
    g->player_country = CountryId(0);
    g->world.world_seed = 7777;
    g->world.date = g->start_date;
    g->rng.seed(g->seed);

    Content& c = g->content;
    const EquipmentId inf = add_equipment(c, "infantry_equipment_1", "Infantry Equipment I",
                                          EquipmentCategory::Infantry, 4.0, 6.0, 8.0, 20.0, 4.0);
    const EquipmentId art = add_equipment(c, "artillery_1", "Artillery I", EquipmentCategory::Artillery,
                                          8.0, 25.0, 6.0, 10.0, 4.0);
    const EquipmentId mot = add_equipment(c, "motorized_1", "Motorized Transport",
                                          EquipmentCategory::Motorized, 12.0, 2.0, 2.0, 15.0, 12.0);

    // An aircraft model: the air slice serializes its statistics like any other
    // equipment, and the wings below reference it by id.
    EquipmentDef fighter;
    fighter.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    fighter.key = "fighter_1";
    fighter.name = "Fighter I";
    fighter.category = EquipmentCategory::Aircraft;
    fighter.year = 1936;
    fighter.archetype = "fighter";
    fighter.air_attack = 18.0;
    fighter.air_defence = 14.0;
    fighter.ground_attack = 4.0;
    fighter.agility = 20.0;
    fighter.range = 3.0;
    fighter.reliability = 0.9;
    fighter.max_strength = 1.0;
    fighter.build_cost = 24.0;
    fighter.manpower = 5.0;
    fighter.supply_use = 0.2;
    fighter.fuel_use = 0.5;
    fighter.resources[static_cast<int>(Resource::Aluminium)] = 3.0;
    c.equipment.push_back(fighter);
    c.equipment_by_key[fighter.key] = fighter.id;
    const EquipmentId fighter_id = fighter.id;

    // A ship model: the naval equipment statistics (guns, torpedoes, detection,
    // visibility) travel in the Economy content snapshot like every other table, and
    // the ships and task forces below reference the model by id.
    EquipmentDef destroyer;
    destroyer.id = EquipmentId(static_cast<uint32_t>(c.equipment.size()));
    destroyer.key = "destroyer_1";
    destroyer.name = "Destroyer I";
    destroyer.category = EquipmentCategory::Ship;
    destroyer.year = 1936;
    destroyer.archetype = "destroyer";
    destroyer.naval_attack = 12.0;
    destroyer.torpedo_attack = 8.0;
    destroyer.sub_detection = 6.0;
    destroyer.detection = 5.0;
    destroyer.visibility = 0.7;
    destroyer.defense = 10.0;
    destroyer.armor = 3.0;
    destroyer.piercing = 5.0;
    destroyer.speed = 35.0;
    destroyer.max_strength = 1.0;
    destroyer.reliability = 0.9;
    destroyer.build_cost = 40.0;
    destroyer.manpower = 250.0;
    destroyer.supply_use = 0.3;
    destroyer.fuel_use = 0.4;
    destroyer.resources[static_cast<int>(Resource::Steel)] = 5.0;
    c.equipment.push_back(destroyer);
    c.equipment_by_key[destroyer.key] = destroyer.id;

    DivisionTemplate infantry;
    infantry.id = TemplateId(0);
    infantry.key = "infantry_division";
    infantry.name = "Infantry Division";
    infantry.country = CountryId(0);
    infantry.battalions.push_back(BattalionSlot{inf, 9, false});
    infantry.battalions.push_back(BattalionSlot{art, 3, false});
    infantry.combat_width = 20.0;
    infantry.max_organization = 60.0;
    infantry.max_strength = 100.0;
    infantry.soft_attack = 60.0;
    infantry.defense = 80.0;
    infantry.speed = 4.0;
    infantry.manpower = 12000.0;
    infantry.build_cost = 800.0;
    infantry.train_days = 90.0;
    c.templates.push_back(infantry);
    c.template_by_key[infantry.key] = infantry.id;

    DivisionTemplate motorized = infantry;
    motorized.id = TemplateId(1);
    motorized.key = "motorized_division";
    motorized.name = "Motorized Division";
    motorized.battalions.clear();
    motorized.battalions.push_back(BattalionSlot{mot, 6, false});
    c.templates.push_back(motorized);
    c.template_by_key[motorized.key] = motorized.id;

    for (const char* key : {"infantry_weapons", "motorized_infantry", "improved_artillery"}) {
        TechDef td;
        td.id = TechId(static_cast<uint32_t>(c.techs.size()));
        td.key = key;
        td.name = key;
        td.category = "land";
        td.year = 1936;
        td.cost_days = 120.0;
        td.modifiers.set(ModifierKind::DivisionAttack, 0.05);
        c.techs.push_back(td);
        c.tech_by_key[key] = td.id;
    }

    for (const char* key : {"conscription_law", "economy_law"}) {
        LawDef law;
        law.key = key;
        law.name = key;
        law.kind = static_cast<int>(c.laws.size());
        law.level = 0;
        law.cost = 100.0;
        c.laws.push_back(law);
        c.law_index[key] = static_cast<int>(c.laws.size()) - 1;
    }

    for (int i = 0; i < static_cast<int>(BuildingKind::Count); ++i) {
        BuildingDef b;
        b.kind = static_cast<BuildingKind>(i);
        b.key = building_kind_name(b.kind);
        b.name = b.key;
        b.base_cost = 1000.0 + 100.0 * i;
        b.max_level = 10;
        c.buildings.push_back(b);
    }

    World& w = g->world;

    Region* north = add(w.regions);
    north->id = RegionId(0);
    north->name = "Northland";
    Region* south = add(w.regions);
    south->id = RegionId(1);
    south->name = "Southland";
    south->temperature = 22.0;
    south->rain = true;
    // A sea region: naval control is a per-region map like air control, and the
    // ships created below operate in a real sea zone.
    Region* sea_zone = add(w.regions);
    sea_zone->id = RegionId(2);
    sea_zone->name = "The Narrows";
    sea_zone->is_sea = true;

    const char* state_names[4] = {"Northwest", "Northeast", "Southwest", "Southeast"};
    for (int i = 0; i < 4; ++i) {
        State* s = add(w.states);
        s->id = StateId(static_cast<uint32_t>(i));
        s->name = state_names[i];
        s->region = RegionId(static_cast<uint32_t>(i < 2 ? 0 : 1));
        s->civilian_factories = 3 + i;
        s->military_factories = 2 + i;
        s->dockyards = i % 2;
        s->synthetic_refineries = i % 3;
        s->building_slots = 6 + i;
        s->manpower_pool = 100000.0 * (i + 1);
        s->flags.push_back(std::string("state_flag_") + std::to_string(i));
    }

    const int state_of[7] = {0, 0, 0, 1, 1, 2, 3};
    const double population[7] = {250000.0, 120000.0, 90000.0, 300000.0, 40000.0, 180000.0, 60000.0};
    for (int i = 0; i < 7; ++i) {
        Province* p = add(w.provinces);
        p->id = ProvinceId(static_cast<uint32_t>(i));
        p->name = "Province " + std::to_string(i);
        p->state = StateId(static_cast<uint32_t>(state_of[i]));
        p->region = RegionId(static_cast<uint32_t>(state_of[i] < 2 ? 0 : 1));
        p->terrain = i == 3 ? Terrain::Mountain : (i == 5 ? Terrain::Forest : Terrain::Plains);
        p->population = population[i];
        p->infrastructure = 2 + (i % 4);
        p->victory_points = i == 0 ? 20 : i;
        p->resource_yield[static_cast<int>(Resource::Steel)] = 1.5 * i;
        p->resource_yield[static_cast<int>(Resource::Oil)] = i == 2 ? 4.0 : 0.0;
        p->x = i * 3;
        p->y = i * 2;
        p->railway_level = i % 3;
        p->air_base = (i % 3 == 0) ? 3 : 1;
        p->naval_base = (i == 5 || i == 6) ? 2 : 0;
        p->anti_air = i % 2;
        p->supply_hub = (i == 2);
        p->fort_level = i == 4 ? 2 : 0;
        p->is_capital = (i == 0 || i == 3 || i == 5);
        p->supply_level = 1.0 - 0.05 * i;
        p->supply_source = ProvinceId(0);
        p->supply_bottleneck = ProvinceId(static_cast<uint32_t>(i));
        p->coastal = (i == 5 || i == 6);
        w.states[StateId(static_cast<uint32_t>(state_of[i]))].provinces.push_back(p->id);
    }
    Province* sea = add(w.provinces);
    sea->id = ProvinceId(7);
    sea->name = "Inner Sea";
    sea->is_sea = true;
    sea->terrain = Terrain::ShallowSea;
    sea->state = StateId{};
    sea->region = RegionId(2);
    sea->supply_source = ProvinceId{};
    sea->supply_bottleneck = ProvinceId{};
    sea_zone->provinces.push_back(sea->id);

    const int chain[7][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 0}};
    for (const auto& edge : chain) {
        w.provinces[ProvinceId(static_cast<uint32_t>(edge[0]))].adj.push_back(
            ProvinceId(static_cast<uint32_t>(edge[1])));
        w.provinces[ProvinceId(static_cast<uint32_t>(edge[1]))].adj.push_back(
            ProvinceId(static_cast<uint32_t>(edge[0])));
    }
    w.provinces[ProvinceId(5)].sea_adj.push_back(ProvinceId(7));
    w.provinces[ProvinceId(6)].sea_adj.push_back(ProvinceId(7));
    w.provinces[ProvinceId(7)].sea_adj.push_back(ProvinceId(5));
    w.provinces[ProvinceId(7)].sea_adj.push_back(ProvinceId(6));

    for (int i = 0; i < 7; ++i) {
        Region* reg = w.regions.try_get(RegionId(static_cast<uint32_t>(state_of[i] < 2 ? 0 : 1)));
        reg->provinces.push_back(ProvinceId(static_cast<uint32_t>(i)));
    }
    // Air control would otherwise be empty until the air phase runs: seed it so the
    // round trip has to carry it even if this world never flies a sortie.
    w.regions[RegionId(0)].air_control = {{CountryId(0), 0.6}, {CountryId(1), 0.4}};
    w.regions[RegionId(1)].air_control = {{CountryId(1), 0.35}, {CountryId(2), 0.65}};

    struct CountrySetup {
        const char* tag;
        const char* name;
        Ideology ideology;
        int capital_state;
        int provinces[3];
        int province_count;
    };
    const CountrySetup setups[3] = {
        {"ALB", "Albania", Ideology::Democratic, 0, {0, 1, 2}, 3},
        {"BRV", "Brevonia", Ideology::Fascist, 1, {3, 4, 4}, 2},
        {"CDA", "Celdaria", Ideology::Neutrality, 2, {5, 6, 6}, 2},
    };
    const int state_owner[4] = {0, 1, 2, 0};

    for (uint32_t i = 0; i < 3; ++i) {
        Country* co = add(w.countries);
        co->id = CountryId(i);
        co->tag = setups[i].tag;
        co->name = setups[i].name;
        co->ideology = setups[i].ideology;
        co->capital = StateId(static_cast<uint32_t>(setups[i].capital_state));
        co->political_power = 10.0 + 5.0 * i;
        co->stability = 0.5 + 0.05 * i;
        co->war_support = 0.4 + 0.1 * i;
        co->manpower = 500000.0 + 1000.0 * i;
        co->fuel = 1000.0;
        co->fuel_capacity = 20000.0;
        co->equipment_stockpile.assign(c.equipment.size(), 100.0 + 10.0 * i);
        co->law_levels = {1, 0, 2};
        co->base_modifiers.set(ModifierKind::FactoryOutput, 0.05 * i);
        co->tech_modifiers.set(ModifierKind::ResearchSpeed, 0.1);
        co->law_modifiers.set(ModifierKind::ConstructionSpeed, 0.1);
        co->national_modifiers.set(ModifierKind::Stability, 0.05);
        co->consumer_goods_ratio = 0.30 + 0.01 * i;
        co->army_experience = 0.2 * i;
        co->starting_factories = 8 + static_cast<int>(i);
        co->fuel_priority = i == 1;
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            co->resources_produced[r] = 10.0 * r;
            co->resources_consumed[r] = 5.0 * r;
            co->resources_imported[r] = r;
            co->resources_exported[r] = 0.5 * r;
        }
        ProductionLine line;
        line.equipment = inf;
        line.factories = 3;
        line.efficiency = 0.4;
        line.efficiency_cap = 0.7;
        line.output_today = 1.5;
        line.output_total = 120.0;
        line.resource_shortage = 0.1;
        line.started = 10 + i;
        line.previous.push_back(art);
        co->lines.push_back(line);
        ConstructionProject project;
        project.kind = BuildingKind::MilitaryFactory;
        project.province = ProvinceId(static_cast<uint32_t>(setups[i].provinces[0]));
        project.state = StateId(static_cast<uint32_t>(setups[i].capital_state));
        project.target_level = 2;
        project.progress = 300.0;
        project.cost = 1200.0;
        co->construction.queue.push_back(project);
        co->research.completed.push_back(TechId(0));
        co->research.slots.push_back(ResearchSlot{TechId(1), 33.0, true});
        co->research.slots_unlocked = 4;
        co->templates = {TemplateId(0)};

        for (int k = 0; k < setups[i].province_count; ++k) {
            Province* p = w.provinces.try_get(ProvinceId(static_cast<uint32_t>(setups[i].provinces[k])));
            p->owner = CountryId(i);
            p->controller = CountryId(i);
        }
        for (int s = 0; s < 4; ++s) {
            if (state_owner[s] != static_cast<int>(i)) continue;
            State* st = w.states.try_get(StateId(static_cast<uint32_t>(s)));
            st->owner = CountryId(i);
            st->controller = CountryId(i);
            st->core_owners.push_back(CountryId(i));
        }

        Character* general = add(w.characters);
        general->id = CharacterId(i);
        general->country = CountryId(i);
        general->name = std::string("General ") + setups[i].tag;
        general->skill = 2 + static_cast<int>(i);
        co->generals.push_back(general->id);

        Army* army = add(w.armies);
        army->id = ArmyId(i);
        army->country = CountryId(i);
        army->name = std::string("1st Army ") + setups[i].tag;
        army->general = general->id;
        army->order.kind = OrderKind::Offensive;
        army->order.line.push_back(ProvinceId(static_cast<uint32_t>(setups[i].provinces[0])));
        army->order.target_line.push_back(
            ProvinceId(static_cast<uint32_t>((setups[i].provinces[0] + 3) % 7)));
        army->order.progress = 0.25;
        army->order.started = 5;
        army->stance = static_cast<uint8_t>(i % 3);
        army->motorization = static_cast<int>(i) * 2;
        general->army = army->id;
        co->armies.push_back(army->id);

        for (int k = 0; k < 2; ++k) {
            Division* d = add(w.divisions);
            d->id = DivisionId(i * 2 + k);
            d->country = CountryId(i);
            d->template_id = k == 0 ? TemplateId(0) : TemplateId(1);
            d->name = std::string("1st Division ") + std::to_string(i * 2 + k);
            d->location = ProvinceId(static_cast<uint32_t>(setups[i].provinces[k % setups[i].province_count]));
            d->previous_location = ProvinceId(static_cast<uint32_t>(setups[i].provinces[0]));
            d->organization = 50.0;
            d->max_organization = 60.0;
            d->strength = 0.9;
            d->experience = 0.1 * i;
            d->entrenchment = 0.2;
            d->planning = 0.15;
            d->supply = 0.9;
            d->fuel = 1.0;
            d->manpower = 11000.0;
            d->equipment.assign(c.equipment.size(), 100.0);
            d->army = army->id;
            d->order = OrderKind::FrontLine;
            d->order_target = ProvinceId(1);
            d->moving = k == 1;
            d->move_from = ProvinceId(static_cast<uint32_t>(setups[i].provinces[0]));
            d->move_to = ProvinceId(static_cast<uint32_t>(setups[i].provinces[1 % setups[i].province_count]));
            d->move_progress = 0.4;
            d->path.push_back(ProvinceId(static_cast<uint32_t>(setups[i].provinces[1 % setups[i].province_count])));
            d->combat_attack_modifier = 0.05;
            d->losses_manpower = 12.0;
            d->losses_equipment = 3.0;
            d->created_tick = 4;
            co->divisions.push_back(d->id);
            army->divisions.push_back(d->id);
        }

        // One air wing per country. The wings are placed so no two hostile wings share
        // a region (1 and 2 are not at war with each other), because the test wants
        // them to survive the tick loop: the round trip must carry a live wing's
        // planes, efficiency, experience, losses and last sortie tick.
        AirWing* wing = add(w.air_wings);
        wing->country = CountryId(i);
        wing->equipment = fighter_id;
        wing->name = std::string("1st Air Wing ") + setups[i].tag;
        wing->planes = 60 + static_cast<int>(i) * 10;
        wing->max_planes = 100;
        wing->base = ProvinceId(static_cast<uint32_t>(setups[i].provinces[0]));
        wing->region = RegionId(static_cast<uint32_t>(i == 1 ? 1 : 0));
        wing->mission = AirMission::AirSuperiority;
        wing->efficiency = 0.8 - 0.1 * i;
        wing->experience = 0.05 * i;
        wing->losses = static_cast<int>(i) * 3;
        wing->last_sortie = 7 + i;
        co->wings.push_back(wing->id);
    }

    Battle* battle = add(w.battles);
    battle->id = BattleId(0);
    battle->province = ProvinceId(3);
    battle->start_tick = 30;
    battle->attacker.divisions.push_back(DivisionId(0));
    battle->attacker.width_used.push_back(20.0);
    battle->attacker.total_soft_attack = 120.0;
    battle->attacker.total_hard_attack = 10.0;
    battle->attacker.total_defense = 80.0;
    battle->attacker.total_breakthrough = 40.0;
    battle->attacker.total_armor = 5.0;
    battle->attacker.total_piercing = 7.0;
    battle->defender.divisions.push_back(DivisionId(2));
    battle->defender.width_used.push_back(20.0);
    battle->defender.total_defense = 150.0;
    battle->defender.total_soft_attack = 60.0;
    battle->progress = 0.3;
    battle->terrain = Terrain::Mountain;
    battle->river_crossing = true;
    battle->attacker_lead = CountryId(0);
    battle->defender_lead = CountryId(1);
    battle->last_tick = 31;
    BattleDebugLine debug;
    debug.division = DivisionId(0);
    debug.base_attack = 120.0;
    debug.planning_mod = 0.05;
    debug.terrain_mod = -0.2;
    debug.supply_mod = 0.9;
    debug.commander_mod = 0.1;
    debug.experience_mod = 0.02;
    debug.final_attack = 100.0;
    debug.enemy_defense = 0.6;
    debug.damage = 60.0;
    debug.org_damage = 45.0;
    debug.strength_damage = 15.0;
    battle->debug.push_back(debug);

    War* war = add(w.wars);
    war->id = WarId(0);
    war->attackers.push_back(WarParticipant{CountryId(0), 100.0, 5.0, 0.1});
    war->defenders.push_back(WarParticipant{CountryId(1), 80.0, 4.0, 0.05});
    WarGoal goal;
    goal.claimant = CountryId(0);
    goal.target = CountryId(1);
    goal.state = StateId(1);
    war->goals.push_back(goal);
    war->start_tick = 20;
    war->active = true;
    war->aggressor = CountryId(0);

    Faction faction;
    faction.id = 1;
    faction.name = "Northern Compact";
    faction.leader = CountryId(0);
    faction.members = {CountryId(0), CountryId(2)};
    w.factions.push_back(faction);
    w.countries[CountryId(0)].faction = 1;
    w.countries[CountryId(2)].faction = 1;

    Relation r01;
    r01.value = -60.0;
    r01.at_war = true;
    w.relations[{0, 1}] = r01;
    Relation r02;
    r02.value = 25.0;
    r02.non_aggression = true;
    r02.military_access = true;
    w.relations[{0, 2}] = r02;
    Relation r12;
    r12.value = -10.0;
    r12.guarantee = true;
    w.relations[{1, 2}] = r12;

    w.countries[CountryId(0)].wars.push_back(WarId(0));
    w.countries[CountryId(1)].wars.push_back(WarId(0));
    w.countries[CountryId(0)].at_war = true;
    w.countries[CountryId(1)].at_war = true;
    w.countries[CountryId(0)].last_capitulation_check = 12;

    if (kEnableAiDuringTicks) {
        g->ai_controlled.assign(3, 1);
    } else {
        for (uint32_t i = 0; i < 3; ++i) g->set_ai(CountryId(i), false);
    }
}

// Fills in the state a hand-built world cannot reach without the simulation and the
// AI: this runs after the tick loop, so no phase can act on it.
void enrich(Game& g) {
    World& w = g.world;

    // Content tables for the political layer: focuses, events and decisions. Built
    // here (after the tick loop) so the phases never act on this hand-built world,
    // and each entry carries non-empty script blocks so the canonical-JSON round
    // trip is exercised. Event triggers are never-true conditions: an event only
    // fires when content or a test asks it to.
    {
        Content& c = g.content;
        FocusDef focus;
        focus.index = 0;
        focus.key = "industrial_effort";
        focus.name = "Industrial Effort";
        focus.tree = "ALB";
        focus.x = 1;
        focus.y = 2;
        focus.days = 70.0;
        focus.prerequisites = {"army_effort"};
        focus.mutually_exclusive = {"naval_effort"};
        focus.available = Json::parse(R"({"political_power": {"gte": 10}})");
        focus.bypass = Json::parse(R"({"year": {"gte": 1938}})");
        focus.effects = Json::parse(R"({"add_political_power": 50, "add_tech": "infantry_weapons"})");
        focus.ai_weight = 1.5;
        c.focuses.push_back(focus);

        FocusDef second = focus;
        second.index = 1;
        second.key = "army_effort";
        second.name = "Army Effort";
        second.tree = "shared";
        second.days = 50.0;
        second.prerequisites.clear();
        c.focuses.push_back(second);
        c.focus_index[focus.key] = 0;
        c.focus_index[second.key] = 1;

        EventDef event;
        event.index = 0;
        event.key = "the_landing";
        event.title = "The Landing";
        event.description = "Troops come ashore.";
        event.fire_only_once = true;
        event.major = true;
        event.trigger = Json::parse(R"({"year": {"gte": 9999}})");  // never fires on its own
        EventOptionDef option;
        option.name = "Fight";
        option.effects = Json::parse(R"({"add_war_support": 0.05})");
        option.ai_weight = 2.0;
        event.options.push_back(option);
        event.immediate = Json::parse(R"({"set_flag": "landing"})");
        c.events.push_back(event);
        c.event_index[event.key] = 0;

        DecisionDef decision;
        decision.index = 0;
        decision.key = "rearm";
        decision.name = "Rearm";
        decision.description = "Rush the factories.";
        decision.category = 1;
        decision.targets_state = false;
        decision.cost_pp = 50.0;
        decision.days_remove = 30;
        decision.days_cooldown = 10;
        decision.visible = Json::parse(R"({"has_tech": "infantry_weapons"})");
        decision.available = Json::parse(R"({"political_power": {"gte": 50}})");
        decision.effects = Json::parse(R"({"add_stability": 0.02})");
        decision.remove_effect = Json::parse(R"({"add_stability": -0.02})");
        decision.ai_weight = 0.8;
        c.decisions.push_back(decision);
        c.decision_index[decision.key] = 0;

        // National spirits and political advisors: the political layer reads their
        // modifiers and availability triggers, so they travel with the save like the
        // focus/event/decision tables. Non-empty script blocks exercise the
        // canonical-JSON round trip for both.
        SpiritDef spirit;
        spirit.index = 0;
        spirit.key = "war_economy";
        spirit.name = "War Economy";
        spirit.description = "Industry on a war footing.";
        spirit.slots = 2;
        spirit.available = Json::parse(R"({"at_war": true})");
        spirit.modifiers.set(ModifierKind::FactoryOutput, 0.15);
        spirit.effects = Json::parse(R"({"add_stability": -0.02})");
        c.spirits.push_back(spirit);

        SpiritDef second_spirit = spirit;
        second_spirit.index = 1;
        second_spirit.key = "national_unity";
        second_spirit.name = "National Unity";
        second_spirit.slots = 1;
        second_spirit.available = Json::parse(R"({"stability": {"gte": 0.5}})");
        second_spirit.effects = Json();
        c.spirits.push_back(second_spirit);
        c.spirit_index[spirit.key] = 0;
        c.spirit_index[second_spirit.key] = 1;

        AdvisorDef advisor;
        advisor.index = 0;
        advisor.key = "armaments_minister";
        advisor.name = "Armaments Minister";
        advisor.description = "Speeds up military production.";
        advisor.cost_pp = 150.0;
        advisor.available = Json::parse(R"({"political_power": {"gte": 150}})");
        advisor.modifiers.set(ModifierKind::PoliticalPowerGain, 0.10);
        c.advisors.push_back(advisor);

        AdvisorDef second_advisor = advisor;
        second_advisor.index = 1;
        second_advisor.key = "propaganda_minister";
        second_advisor.name = "Propaganda Minister";
        second_advisor.cost_pp = 75.0;
        second_advisor.available = Json();
        second_advisor.modifiers.set(ModifierKind::Stability, 0.05);
        c.advisors.push_back(second_advisor);
        c.advisor_index[advisor.key] = 0;
        c.advisor_index[second_advisor.key] = 1;
    }

    // Political state that only the scripted layer produces: focus progression, an
    // event awaiting a choice, the fired-event and decision bookkeeping, script
    // flags, the modifiers those grant, the world script variables and one scheduled
    // event. Built here so no phase can clear it before the round trip hashes it.
    w.countries.for_each([&](CountryId id, Country& c) {
        c.completed_focuses = {0};
        c.selected_focus = 1;
        c.focus_progress = 12.5 + static_cast<double>(id.v);
        c.pending_events = {0};
        c.fired_events = {0, 1};
        c.active_decisions = {0};
        c.decision_days_left = {7.5 + static_cast<double>(id.v)};
        c.decision_cooldown.assign(g.content.decisions.size(), 0.0);
        if (!c.decision_cooldown.empty()) c.decision_cooldown[0] = 3.0 + static_cast<double>(id.v);
        c.country_flags = {std::string("flag_") + c.tag};
        TimedModifier timed;
        timed.source = "industrial_effort";
        timed.days_left = 20 + static_cast<int>(id.v);
        timed.mods.set(ModifierKind::FactoryOutput, 0.1 + 0.01 * id.v);
        c.timed_modifiers.push_back(timed);

        // The held national spirits are permanent (days_left = -1), keyed into the
        // content roster by index; the advisor roster and both slot capacities are
        // part of the political state too.
        TimedModifier spirit;
        spirit.source = g.content.spirits.empty() ? "war_economy" : g.content.spirits.front().key;
        spirit.days_left = -1;
        spirit.mods.set(ModifierKind::FactoryOutput, 0.15);
        c.national_spirits.push_back(spirit);
        if (g.content.spirits.size() > 1) {
            TimedModifier second;
            second.source = g.content.spirits[1].key;
            second.days_left = -1;
            second.mods.set(ModifierKind::Stability, 0.05);
            c.national_spirits.push_back(second);
        }
        c.spirit_keys.clear();
        for (uint32_t i = 0; i < g.content.spirits.size(); ++i) c.spirit_keys.push_back(i);
        c.advisors.push_back(id.v % (g.content.advisors.empty() ? 1u : g.content.advisors.size()));
        c.spirit_slots = 6 + static_cast<int>(id.v);
        c.advisor_slots = 3 + static_cast<int>(id.v);
    });
    w.script_vars["war_effort"] = 3.5;
    w.script_vars["preparedness"] = 0.25;
    DelayedEvent delayed;
    delayed.country = CountryId(0);
    delayed.event = 0;
    delayed.due = w.tick + 10;
    w.delayed_events.push_back(delayed);

    for (int i = 0; i < static_cast<int>(AiLayer::Count); ++i) {
        AiLayerState& layer = g.ai.layers[i];
        layer.last_run_tick = 10 + static_cast<uint32_t>(i);
        layer.interval_ticks = 24;
        AiReason reason;
        reason.what = std::string("layer_") + ai_layer_name(static_cast<AiLayer>(i));
        reason.score = 0.5 + 0.1 * i;
        reason.factors.push_back({"need", 0.4});
        reason.factors.push_back({"cost", -0.2});
        layer.last_reasons.push_back(reason);
    }
    g.ai.posture.assign(w.countries.capacity(), 1);
    if (g.ai.posture.size() > 2) g.ai.posture[1] = 2;
    g.ai.decisions_made = 42;
    g.ai.commands_issued = 17;

    // A training queue entry per country, pointing at a division that exists.
    uint32_t first_division = INVALID_ID;
    w.divisions.for_each([&](DivisionId id, const Division&) {
        if (first_division == INVALID_ID) first_division = id.v;
    });
    if (first_division != INVALID_ID) {
        w.countries.for_each([&](CountryId, Country& c) {
            TrainingDivision training;
            training.division = DivisionId(first_division);
            training.template_id = TemplateId(0);
            training.days_left = 30.0;
            c.training.push_back(training);
        });
    }

    // Naval state: a fleet, a task force and a ship per country, an invasion and the
    // per-region naval control map. It is built here (after the tick loop, so no
    // naval phase has acted on it) to give the Military/Countries/Map sections
    // non-empty naval content: a store that serialised empty would prove nothing.
    const EquipmentId destroyer = g.content.equipment_id("destroyer_1");
    const NavalMission missions[3] = {NavalMission::ConvoyRaid, NavalMission::Patrol,
                                      NavalMission::InvasionSupport};
    w.countries.for_each([&](CountryId id, Country& c) {
        Fleet* fleet = add(w.fleets);
        fleet->country = id;
        fleet->name = std::string("Home Fleet ") + c.tag;
        TaskForce* task_force = add(w.task_forces);
        task_force->country = id;
        task_force->fleet = fleet->id;
        task_force->name = std::string("Task Force ") + c.tag;
        task_force->port = ProvinceId(5);
        task_force->sea_region = RegionId(2);
        task_force->mission = missions[id.v % 3];
        task_force->at_sea = true;
        task_force->detection = 0.4 + 0.1 * id.v;
        task_force->last_engagement = 3 + id.v;
        Ship* ship = add(w.ships);
        ship->country = id;
        ship->equipment = destroyer;
        ship->name = std::string("DD ") + c.tag;
        ship->fleet = fleet->id;
        ship->task_force = task_force->id;
        ship->strength = 0.9 - 0.05 * id.v;
        ship->organisation = 0.8 - 0.1 * id.v;
        ship->experience = 0.05 * id.v;
        ship->fuel = 0.7 - 0.05 * id.v;
        ship->port = ProvinceId(5);
        ship->sea_region = RegionId(2);
        ship->at_sea = true;
        task_force->ships.push_back(ship->id);
        fleet->task_forces.push_back(task_force->id);
        c.fleets.push_back(fleet->id);
    });

    // A naval invasion crossing to a hostile coast: the list is dense, so one entry
    // gives it a non-empty round trip.
    NavalInvasion invasion;
    invasion.army = w.countries[CountryId(2)].armies.front();
    invasion.country = CountryId(2);
    invasion.origin = ProvinceId(5);
    invasion.target = ProvinceId(0);
    invasion.sea_region = RegionId(2);
    invasion.progress = 0.35;
    invasion.started = 20;
    invasion.landed = false;
    w.invasions.push_back(invasion);

    // Naval control per region, seeded so the Map round trip carries it even though
    // the naval phase never ran on this hand-built world.
    w.regions[RegionId(2)].naval_control = {{CountryId(0), 0.55}, {CountryId(2), 0.45}};
    w.regions[RegionId(0)].naval_control = {{CountryId(1), 0.30}};

    // A battle per country pair is too much: keep exactly one, with debug lines so
    // the Battles section is never empty even if the simulation ended every battle.
    if (w.battles.size() == 0) {
        Battle* battle = add(w.battles);
        battle->id = BattleId(static_cast<uint32_t>(w.battles.capacity() - 1));
        battle->province = ProvinceId(3);
        battle->start_tick = g.world.tick;
        battle->last_tick = g.world.tick;
        battle->attacker_lead = CountryId(0);
        battle->defender_lead = CountryId(1);
        BattleDebugLine debug;
        debug.base_attack = 1.0;
        debug.final_attack = 1.0;
        battle->debug.push_back(debug);
    }

    // A recycled slot leaves a hole in a store: the save must keep both the dead slot
    // and the free list, otherwise a later create() hands out a different id after a
    // load and the run diverges silently.
    Character* spare = add(w.characters);
    const CharacterId spare_id = spare->id;
    w.characters.destroy(spare_id);

    // Commands in the log: the save trailer and the replay both carry it.
    Command command;
    command.type = CommandType::StartResearch;
    command.country = CountryId(0);
    command.tech = TechId(1);
    command.value = 3;
    command.value_f = 1.25;
    command.text = "persistence test command";
    command.issued_tick = g.world.tick;
    command.divisions.push_back(DivisionId(0));
    g.log.record(g.world.tick, command, CommandResult::Applied);
    Command queued = command;
    queued.type = CommandType::SetLaw;
    g.log.record(g.world.tick, queued, CommandResult::Applied);
    // A command still waiting in the queue is part of the saved state: it decides the
    // next tick, so it is hashed with the RNG streams.
    g.queue.push(queued);
}

// ---------------------------------------------------------------- fixture -----

struct Fixture {
    Game source;
    Game fresh;
    std::string save_path;
    std::string replay_path;
    uint64_t hash = 0;
    size_t log_count = 0;
    uint32_t log_next_sequence = 0;
};

Fixture build_fixture() {
    Fixture f;
    build_world(&f.source);
    f.source.run_ticks(kTicks);
    enrich(f.source);
    f.hash = world_hash(f.source);
    f.log_count = f.source.log.records.size();
    f.log_next_sequence = f.source.log.next_sequence;

    // A second, identical game: state comes from the save, content from the builder.
    build_world(&f.fresh);

    f.save_path = temp_path("save.bin");
    f.replay_path = temp_path("replay.bin");
    std::string err;
    require(save_game(f.source, f.save_path, &err), "save_game", err);
    require(save_replay(f.source, f.replay_path, &err), "save_replay", err);
    CHECK_GT(f.hash, 0u);
    CHECK_GT(f.log_count, 0u);
    // The air slice must actually be exercised: a fixture whose wings did not survive
    // the tick loop would serialise an empty store and prove nothing.
    CHECK_GT(f.source.world.air_wings.size(), 0u);
    CHECK(!f.source.world.regions[RegionId(0)].air_control.empty());
    // The naval slice must be exercised too: an empty ship store or invasion list
    // would serialise nothing and prove nothing.
    CHECK_GT(f.source.world.ships.size(), 0u);
    CHECK_GT(f.source.world.task_forces.size(), 0u);
    CHECK_GT(f.source.world.fleets.size(), 0u);
    CHECK(!f.source.world.invasions.empty());
    CHECK(!f.source.world.regions[RegionId(2)].naval_control.empty());
    // The political slice must be exercised too: empty focus/event/decision tables or
    // an empty script-variable map would serialise nothing and prove nothing.
    CHECK_GT(f.source.content.focuses.size(), 0u);
    CHECK_GT(f.source.content.events.size(), 0u);
    CHECK_GT(f.source.content.decisions.size(), 0u);
    // The spirit/advisor slice must be exercised too: an empty content table or an
    // empty roster would serialise nothing and prove nothing.
    CHECK_GT(f.source.content.spirits.size(), 0u);
    CHECK_GT(f.source.content.advisors.size(), 0u);
    CHECK(!f.source.world.countries[CountryId(0)].national_spirits.empty());
    CHECK(!f.source.world.countries[CountryId(0)].spirit_keys.empty());
    CHECK(!f.source.world.countries[CountryId(0)].advisors.empty());
    CHECK(!f.source.world.script_vars.empty());
    CHECK(!f.source.world.delayed_events.empty());
    CHECK(!f.source.world.countries[CountryId(0)].completed_focuses.empty());
    CHECK(!f.source.world.countries[CountryId(0)].timed_modifiers.empty());
    CHECK(!f.source.world.countries[CountryId(0)].country_flags.empty());
    return f;
}

const Fixture& fixture() {
    static Fixture f = build_fixture();
    return f;
}

Game load_into_fresh(const Fixture& f) {
    Game g = f.fresh;
    std::string err;
    require(load_game(g, f.save_path, &err), "load_game", err);
    return g;
}

void check_all_sections_equal(const Game& a, const Game& b) {
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        const Subsystem s = static_cast<Subsystem>(i);
        if (subsystem_hash(a, s) != subsystem_hash(b, s)) {
            ::hoi_test::fail(__FILE__, __LINE__,
                             std::string("subsystem hash differs after load: ") + subsystem_name(s));
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------- tests -

HOI_TEST(save_round_trip_preserves_world_and_subsystem_hashes) {
    const Fixture& f = fixture();
    Game loaded = load_into_fresh(f);

    CHECK_EQ(world_hash(loaded), f.hash);
    check_all_sections_equal(f.source, loaded);
    // The header and the Rng section must agree with what was saved.
    CHECK_EQ(loaded.seed, f.source.seed);
    CHECK_EQ(loaded.world.tick, f.source.world.tick);
    CHECK_EQ(loaded.world.world_seed, f.source.world.world_seed);
    CHECK_EQ(loaded.ticks_run, f.source.ticks_run);
    CHECK(loaded.world.date == f.source.world.date);
    CHECK(loaded.player_country == f.source.player_country);
    CHECK(loaded.scenario_path == f.source.scenario_path);
}

HOI_TEST(save_round_trip_preserves_the_command_log_and_queue) {
    const Fixture& f = fixture();
    Game loaded = load_into_fresh(f);

    CHECK_EQ(loaded.log.records.size(), f.log_count);
    CHECK_EQ(loaded.log.next_sequence, f.log_next_sequence);
    CHECK(loaded.queue.pending.size() == f.source.queue.pending.size());
    for (size_t i = 0; i < loaded.log.records.size(); ++i) {
        CHECK_EQ(static_cast<int>(loaded.log.records[i].command.type),
                 static_cast<int>(f.source.log.records[i].command.type));
        CHECK(loaded.log.records[i].tick == f.source.log.records[i].tick);
        CHECK(loaded.log.records[i].command.text == f.source.log.records[i].command.text);
        CHECK(loaded.log.records[i].result == f.source.log.records[i].result);
    }
}

HOI_TEST(save_round_trip_preserves_id_allocation_for_new_entities) {
    const Fixture& f = fixture();
    Game loaded = load_into_fresh(f);

    // Destroying and re-creating entities must hand out the same slot in both runs:
    // the store free list is part of the saved state.
    uint32_t victim = INVALID_ID;
    f.source.world.divisions.for_each([&](DivisionId id, const Division&) {
        if (victim == INVALID_ID) victim = id.v;
    });
    CHECK(victim != INVALID_ID);

    Game before = f.source;
    before.world.divisions.destroy(DivisionId(victim));
    loaded.world.divisions.destroy(DivisionId(victim));
    const Division* a = add(before.world.divisions);
    const Division* b = add(loaded.world.divisions);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK_EQ(a->id, b->id);
    CHECK_EQ(world_hash(before), world_hash(loaded));
}

HOI_TEST(save_detects_corrupted_section_payload_and_names_it) {
    const Fixture& f = fixture();
    std::vector<uint8_t> bytes = read_bytes(f.save_path);
    CHECK(!bytes.empty());

    const SaveLayout layout = parse_save(bytes);
    const size_t payload_offset = layout.payload_offset[static_cast<size_t>(Subsystem::Military)];
    const uint32_t payload_length = layout.payload_length[static_cast<size_t>(Subsystem::Military)];
    CHECK(payload_offset != 0);
    CHECK_GT(payload_length, 4u);

    bytes[payload_offset + payload_length / 2] ^= 0x5A;
    const std::string corrupt_path = temp_path("corrupt.bin");
    write_bytes(corrupt_path, bytes);

    Game loaded = f.fresh;
    std::string err;
    CHECK(!load_game(loaded, corrupt_path, &err));
    CHECK(err.find("Military") != std::string::npos);
    CHECK(err.find("section hash mismatch") != std::string::npos);

    // A corrupted section length must be reported as a truncated file, not parsed.
    std::vector<uint8_t> bad_length = read_bytes(f.save_path);
    const uint32_t huge = 0xFFFF0000u;
    const size_t length_offset = payload_offset - 12;  // tag (1) + length (4) + hash (8)
    bad_length[length_offset + 0] = static_cast<uint8_t>(huge & 0xFF);
    bad_length[length_offset + 1] = static_cast<uint8_t>((huge >> 8) & 0xFF);
    bad_length[length_offset + 2] = static_cast<uint8_t>((huge >> 16) & 0xFF);
    bad_length[length_offset + 3] = static_cast<uint8_t>((huge >> 24) & 0xFF);
    const std::string bad_length_path = temp_path("bad_length.bin");
    write_bytes(bad_length_path, bad_length);
    err.clear();
    CHECK(!load_game(loaded, bad_length_path, &err));
    CHECK(err.find("Military") != std::string::npos);
    CHECK(err.find("truncated") != std::string::npos);
}

HOI_TEST(save_rejects_unknown_versions_and_garbage_files) {
    const Fixture& f = fixture();
    std::vector<uint8_t> bytes = read_bytes(f.save_path);
    CHECK_GT(bytes.size(), 8u);

    const std::string newer_path = temp_path("newer.bin");
    std::vector<uint8_t> newer = bytes;
    newer[4] = 99;  // SAVE_VERSION lives right after the magic
    write_bytes(newer_path, newer);

    const std::string older_path = temp_path("older.bin");
    std::vector<uint8_t> older = bytes;
    older[4] = 1;
    write_bytes(older_path, older);

    const std::string garbage_path = temp_path("garbage.bin");
    write_bytes(garbage_path, std::vector<uint8_t>(64, 0xAB));

    Game loaded = f.fresh;
    std::string err;
    CHECK(!load_game(loaded, newer_path, &err));
    CHECK(err.find("99") != std::string::npos);
    CHECK(err.find(std::to_string(SAVE_VERSION)) != std::string::npos);

    err.clear();
    CHECK(!load_game(loaded, older_path, &err));
    CHECK(err.find("no migration path from version 1") != std::string::npos);

    err.clear();
    CHECK(!load_game(loaded, garbage_path, &err));
    CHECK(err.find("not a save file") != std::string::npos);

    // Truncating a valid save must be reported, not parsed.
    err.clear();
    const std::string truncated_path = temp_path("truncated.bin");
    write_bytes(truncated_path, std::vector<uint8_t>(bytes.begin(), bytes.begin() + bytes.size() / 2));
    CHECK(!load_game(loaded, truncated_path, &err));
    CHECK(!err.empty());

    // Bytes appended after the command log mean the file is not what it claims.
    err.clear();
    std::vector<uint8_t> extended = bytes;
    extended.insert(extended.end(), 8, 0x7F);
    const std::string extended_path = temp_path("extended.bin");
    write_bytes(extended_path, extended);
    CHECK(!load_game(loaded, extended_path, &err));
    CHECK(err.find("trailing bytes") != std::string::npos);

    // An absurd command-log count is rejected by the size check before any parse.
    err.clear();
    std::vector<uint8_t> absurd = bytes;
    const SaveLayout layout = parse_save(bytes);
    absurd[layout.log_count_offset] = 0xFF;
    absurd[layout.log_count_offset + 1] = 0xFF;
    absurd[layout.log_count_offset + 2] = 0xFF;
    absurd[layout.log_count_offset + 3] = 0x7F;
    const std::string absurd_path = temp_path("absurd_log.bin");
    write_bytes(absurd_path, absurd);
    CHECK(!load_game(loaded, absurd_path, &err));
    CHECK(err.find("command log") != std::string::npos);
}

HOI_TEST(save_hash_report_lists_every_subsystem) {
    const Fixture& f = fixture();
    const std::string report = hash_report(f.source);

    size_t line_count = 0;
    size_t pos = 0;
    std::vector<std::string> lines;
    while (pos <= report.size()) {
        const size_t end = report.find('\n', pos);
        if (end == std::string::npos) break;
        lines.push_back(report.substr(pos, end - pos));
        pos = end + 1;
    }
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        const std::string name = subsystem_name(static_cast<Subsystem>(i));
        int matches = 0;
        for (const std::string& line : lines) {
            if (line.rfind(name, 0) == 0 && line.size() > name.size() && line[name.size()] == ' ') {
                ++matches;
            }
        }
        if (matches != 1) {
            ::hoi_test::fail(__FILE__, __LINE__,
                             "hash report has " + std::to_string(matches) + " lines for " + name);
        }
        ++line_count;
    }
    CHECK_EQ(line_count, static_cast<size_t>(Subsystem::Count));
    CHECK(report.find("tick=") != std::string::npos);
    CHECK(report.find("date=") != std::string::npos);
    CHECK(report.find("seed=") != std::string::npos);
    CHECK_EQ(hash_report(f.source), report);  // deterministic
}

// Every gameplay field must be part of the world hash: a field that is not hashed is
// a field the save forgets.
HOI_TEST(save_hash_covers_every_gameplay_field) {
    const Fixture& f = fixture();
    const uint64_t before = f.hash;

    struct Flip {
        const char* what;
        void (*fn)(Game&);
    };
    const std::vector<Flip> flips = {
        {"province.adj", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.adj.push_back(ProvinceId(9)); }); }},
        {"province.sea_adj", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.sea_adj.push_back(ProvinceId(9)); }); }},
        {"province.coastal", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.coastal = !p.coastal; }); }},
        {"province.is_sea", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.is_sea = !p.is_sea; }); }},
        {"province.victory_points", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.victory_points += 1; }); }},
        {"province.infrastructure", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.infrastructure += 1; }); }},
        {"province.fort_level", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.fort_level += 1; }); }},
        {"province.air_base", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.air_base += 1; }); }},
        {"province.naval_base", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.naval_base += 1; }); }},
        {"province.radar", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.radar += 1; }); }},
        {"province.anti_air", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.anti_air += 1; }); }},
        {"province.supply_hub", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.supply_hub = !p.supply_hub; }); }},
        {"province.railway_level", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.railway_level += 1; }); }},
        {"province.population", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.population += 1.0; }); }},
        {"province.resource_yield", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.resource_yield[static_cast<int>(Resource::Oil)] += 1.0; }); }},
        {"province.is_capital", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.is_capital = !p.is_capital; }); }},
        {"province.supply_level", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.supply_level *= 0.5; }); }},
        {"province.supply_source", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.supply_source = ProvinceId(6); }); }},
        {"province.supply_bottleneck", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.supply_bottleneck = ProvinceId(6); }); }},
        {"province.x", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.x += 1; }); }},
        {"province.y", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.y += 1; }); }},
        {"province.terrain", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.terrain = Terrain::Jungle; }); }},
        {"province.owner", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.owner = CountryId(2); }); }},
        {"province.controller", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.controller = CountryId(2); }); }},
        {"province.name", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.name += "x"; }); }},
        {"province.state", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.state = StateId(1); }); }},
        {"province.region", +[](Game& g) { g.world.provinces.for_each([](ProvinceId, Province& p) { p.region = RegionId(1); }); }},
        {"state.provinces", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.provinces.push_back(ProvinceId(6)); }); }},
        {"state.core_owners", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.core_owners.push_back(CountryId(2)); }); }},
        {"state.civilian_factories", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.civilian_factories += 1; }); }},
        {"state.military_factories", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.military_factories += 1; }); }},
        {"state.dockyards", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.dockyards += 1; }); }},
        {"state.synthetic_refineries", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.synthetic_refineries += 1; }); }},
        {"state.building_slots", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.building_slots += 1; }); }},
        {"state.manpower_pool", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.manpower_pool += 1.0; }); }},
        {"state.impassable", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.impassable = !s.impassable; }); }},
        {"state.flags", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.flags.push_back("x"); }); }},
        {"state.resistance", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.resistance += 1.0; }); }},
        {"state.compliance", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.compliance += 1.0; }); }},
        {"state.garrison_required", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.garrison_required += 1.0; }); }},
        {"state.owner", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.owner = CountryId(2); }); }},
        {"state.controller", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.controller = CountryId(2); }); }},
        {"state.name", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.name += "x"; }); }},
        {"state.region", +[](Game& g) { g.world.states.for_each([](StateId, State& s) { s.region = RegionId(1); }); }},
        {"region.temperature", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.temperature += 1.0; }); }},
        {"region.rain", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.rain = !r.rain; }); }},
        {"region.snow", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.snow = !r.snow; }); }},
        {"region.mud", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.mud = !r.mud; }); }},
        {"region.sandstorm", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.sandstorm = !r.sandstorm; }); }},
        {"region.is_sea", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.is_sea = !r.is_sea; }); }},
        {"region.provinces", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.provinces.push_back(ProvinceId(6)); }); }},
        {"region.name", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.name += "x"; }); }},
        {"region.air_control.size", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.air_control.push_back({CountryId(1), 0.25}); }); }},
        {"region.air_control.country", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { for (auto& e : r.air_control) e.first = CountryId(2); }); }},
        {"region.air_control.share", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { for (auto& e : r.air_control) e.second -= 0.05; }); }},
        {"region.naval_control.size", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { r.naval_control.push_back({CountryId(1), 0.25}); }); }},
        {"region.naval_control.country", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { for (auto& e : r.naval_control) e.first = CountryId(2); }); }},
        {"region.naval_control.share", +[](Game& g) { g.world.regions.for_each([](RegionId, Region& r) { for (auto& e : r.naval_control) e.second -= 0.05; }); }},
        {"country.tag", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.tag += "x"; }); }},
        {"country.name", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.name += "x"; }); }},
        {"country.alive", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.alive = !c.alive; }); }},
        {"country.ideology", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.ideology = Ideology::Communist; }); }},
        {"country.overlord", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.overlord = CountryId(2); }); }},
        {"country.puppets", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.puppets.push_back(CountryId(2)); }); }},
        {"country.capital", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.capital = StateId(3); }); }},
        {"country.equipment_stockpile", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.equipment_stockpile[0] += 1.0; }); }},
        {"country.law_levels", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.law_levels[0] += 1; }); }},
        {"country.base_modifiers", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.base_modifiers.v[0] += 0.01; }); }},
        {"country.tech_modifiers", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.tech_modifiers.v[1] += 0.01; }); }},
        {"country.law_modifiers", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.law_modifiers.v[2] += 0.01; }); }},
        {"country.national_modifiers", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.national_modifiers.v[3] += 0.01; }); }},
        {"country.generals", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.generals.push_back(CharacterId(0)); }); }},
        {"country.wings", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.wings.push_back(AirWingId(0)); }); }},
        {"country.fleets", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.fleets.push_back(FleetId(0)); }); }},
        {"country.starting_factories", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.starting_factories += 1; }); }},
        {"country.resources_produced", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.resources_produced[1] += 1.0; }); }},
        {"country.resources_consumed", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.resources_consumed[1] += 1.0; }); }},
        {"country.resources_imported", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.resources_imported[1] += 1.0; }); }},
        {"country.resources_exported", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.resources_exported[1] += 1.0; }); }},
        {"character.name", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.name += "x"; }); }},
        {"character.is_general", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.is_general = !c.is_general; }); }},
        {"character.skill", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.skill += 1; }); }},
        {"character.attack", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.attack += 1; }); }},
        {"character.defense", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.defense += 1; }); }},
        {"character.planning", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.planning += 1; }); }},
        {"character.logistics", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.logistics += 1; }); }},
        {"character.army", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.army = ArmyId(0); }); }},
        {"character.country", +[](Game& g) { g.world.characters.for_each([](CharacterId, Character& c) { c.country = CountryId(1); }); }},
        {"line.equipment", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.equipment = EquipmentId(1); }); }},
        {"line.factories", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.factories += 1; }); }},
        {"line.efficiency", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.efficiency += 0.01; }); }},
        {"line.efficiency_cap", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.efficiency_cap += 0.01; }); }},
        {"line.output_today", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.output_today += 1.0; }); }},
        {"line.output_total", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.output_total += 1.0; }); }},
        {"line.resource_shortage", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.resource_shortage += 0.01; }); }},
        {"line.started", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.started += 1; }); }},
        {"line.previous", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& l : c.lines) l.previous.push_back(EquipmentId(0)); }); }},
        {"construction.queue", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.construction.queue.push_back(ConstructionProject{}); }); }},
        {"construction.max_queue_size", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.construction.max_queue_size += 1; }); }},
        {"project.kind", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.kind = BuildingKind::Fort; }); }},
        {"project.province", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.province = ProvinceId(4); }); }},
        {"project.state", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.state = StateId(2); }); }},
        {"project.target_level", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.target_level += 1; }); }},
        {"project.progress", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.progress += 1.0; }); }},
        {"project.cost", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.cost += 1.0; }); }},
        {"project.repair", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& p : c.construction.queue) p.repair = !p.repair; }); }},
        {"research.completed", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.research.completed.push_back(TechId(2)); }); }},
        {"research.slots.tech", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& s : c.research.slots) s.tech = TechId(2); }); }},
        {"research.slots.progress", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& s : c.research.slots) s.progress += 1.0; }); }},
        {"research.slots.active", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& s : c.research.slots) s.active = !s.active; }); }},
        {"research.slots_unlocked", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.research.slots_unlocked += 1; }); }},
        {"country.templates", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.templates.push_back(TemplateId(1)); }); }},
        {"template.key", +[](Game& g) { for (auto& t : g.content.templates) t.key += "x"; }},
        {"template.name", +[](Game& g) { for (auto& t : g.content.templates) t.name += "x"; }},
        {"template.country", +[](Game& g) { for (auto& t : g.content.templates) t.country = CountryId(1); }},
        {"template.battalions", +[](Game& g) { for (auto& t : g.content.templates) t.battalions.push_back(BattalionSlot{EquipmentId(0), 1, true}); }},
        {"template.combat_width", +[](Game& g) { for (auto& t : g.content.templates) t.combat_width += 1.0; }},
        {"template.max_organization", +[](Game& g) { for (auto& t : g.content.templates) t.max_organization += 1.0; }},
        {"template.stats", +[](Game& g) { for (auto& t : g.content.templates) t.soft_attack += 1.0; }},
        {"template.hardness", +[](Game& g) { for (auto& t : g.content.templates) t.hardness += 0.1; }},
        {"template.supply_use", +[](Game& g) { for (auto& t : g.content.templates) t.supply_use += 0.1; }},
        {"template.fuel_use", +[](Game& g) { for (auto& t : g.content.templates) t.fuel_use += 0.1; }},
        {"template.build_cost", +[](Game& g) { for (auto& t : g.content.templates) t.build_cost += 1.0; }},
        {"template.train_days", +[](Game& g) { for (auto& t : g.content.templates) t.train_days += 1.0; }},
        {"equipment.id", +[](Game& g) { for (auto& e : g.content.equipment) e.id = EquipmentId(0); }},
        {"equipment.key", +[](Game& g) { for (auto& e : g.content.equipment) e.key += "x"; }},
        {"equipment.name", +[](Game& g) { for (auto& e : g.content.equipment) e.name += "x"; }},
        {"equipment.category", +[](Game& g) { for (auto& e : g.content.equipment) e.category = EquipmentCategory::Armor; }},
        {"equipment.year", +[](Game& g) { for (auto& e : g.content.equipment) e.year += 1; }},
        {"equipment.archetype", +[](Game& g) { for (auto& e : g.content.equipment) e.archetype += "x"; }},
        {"equipment.soft_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.soft_attack += 1.0; }},
        {"equipment.hard_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.hard_attack += 1.0; }},
        {"equipment.air_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.air_attack += 1.0; }},
        {"equipment.air_defence", +[](Game& g) { for (auto& e : g.content.equipment) e.air_defence += 1.0; }},
        {"equipment.ground_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.ground_attack += 1.0; }},
        {"equipment.agility", +[](Game& g) { for (auto& e : g.content.equipment) e.agility += 1.0; }},
        {"equipment.range", +[](Game& g) { for (auto& e : g.content.equipment) e.range += 1.0; }},
        {"equipment.naval_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.naval_attack += 1.0; }},
        {"equipment.torpedo_attack", +[](Game& g) { for (auto& e : g.content.equipment) e.torpedo_attack += 1.0; }},
        {"equipment.sub_detection", +[](Game& g) { for (auto& e : g.content.equipment) e.sub_detection += 1.0; }},
        {"equipment.detection", +[](Game& g) { for (auto& e : g.content.equipment) e.detection += 1.0; }},
        {"equipment.visibility", +[](Game& g) { for (auto& e : g.content.equipment) e.visibility += 0.1; }},
        {"equipment.defense", +[](Game& g) { for (auto& e : g.content.equipment) e.defense += 1.0; }},
        {"equipment.breakthrough", +[](Game& g) { for (auto& e : g.content.equipment) e.breakthrough += 1.0; }},
        {"equipment.armor", +[](Game& g) { for (auto& e : g.content.equipment) e.armor += 1.0; }},
        {"equipment.piercing", +[](Game& g) { for (auto& e : g.content.equipment) e.piercing += 1.0; }},
        {"equipment.hardness", +[](Game& g) { for (auto& e : g.content.equipment) e.hardness += 0.1; }},
        {"equipment.reliability", +[](Game& g) { for (auto& e : g.content.equipment) e.reliability -= 0.1; }},
        {"equipment.speed", +[](Game& g) { for (auto& e : g.content.equipment) e.speed += 1.0; }},
        {"equipment.max_strength", +[](Game& g) { for (auto& e : g.content.equipment) e.max_strength += 1.0; }},
        {"equipment.organization", +[](Game& g) { for (auto& e : g.content.equipment) e.organization += 1.0; }},
        {"equipment.build_cost", +[](Game& g) { for (auto& e : g.content.equipment) e.build_cost += 1.0; }},
        {"equipment.resources", +[](Game& g) { for (auto& e : g.content.equipment) e.resources[static_cast<int>(Resource::Steel)] += 1.0; }},
        {"equipment.fuel_use", +[](Game& g) { for (auto& e : g.content.equipment) e.fuel_use += 0.1; }},
        {"equipment.supply_use", +[](Game& g) { for (auto& e : g.content.equipment) e.supply_use += 0.1; }},
        {"equipment.manpower", +[](Game& g) { for (auto& e : g.content.equipment) e.manpower += 1.0; }},
        {"equipment.is_archetype", +[](Game& g) { for (auto& e : g.content.equipment) e.is_archetype = true; }},
        {"tech.id", +[](Game& g) { for (auto& t : g.content.techs) t.id = TechId(0); }},
        {"tech.key", +[](Game& g) { for (auto& t : g.content.techs) t.key += "x"; }},
        {"tech.name", +[](Game& g) { for (auto& t : g.content.techs) t.name += "x"; }},
        {"tech.category", +[](Game& g) { for (auto& t : g.content.techs) t.category += "x"; }},
        {"tech.year", +[](Game& g) { for (auto& t : g.content.techs) t.year += 1; }},
        {"tech.cost_days", +[](Game& g) { for (auto& t : g.content.techs) t.cost_days += 1.0; }},
        {"tech.prerequisites", +[](Game& g) { for (auto& t : g.content.techs) t.prerequisites.push_back(TechId(0)); }},
        {"tech.unlock_equipment", +[](Game& g) { for (auto& t : g.content.techs) t.unlock_equipment.push_back("x"); }},
        {"tech.unlock_buildings", +[](Game& g) { for (auto& t : g.content.techs) t.unlock_buildings.push_back("x"); }},
        {"tech.modifiers", +[](Game& g) { for (auto& t : g.content.techs) t.modifiers.v[0] += 0.01; }},
        {"law.key", +[](Game& g) { for (auto& l : g.content.laws) l.key += "x"; }},
        {"law.name", +[](Game& g) { for (auto& l : g.content.laws) l.name += "x"; }},
        {"law.kind", +[](Game& g) { for (auto& l : g.content.laws) l.kind += 1; }},
        {"law.level", +[](Game& g) { for (auto& l : g.content.laws) l.level += 1; }},
        {"law.cost", +[](Game& g) { for (auto& l : g.content.laws) l.cost += 1.0; }},
        {"law.modifiers", +[](Game& g) { for (auto& l : g.content.laws) l.modifiers.v[0] += 0.01; }},
        {"law.requires_law", +[](Game& g) { for (auto& l : g.content.laws) l.requires_law += "x"; }},
        {"law.requires_level", +[](Game& g) { for (auto& l : g.content.laws) l.requires_level += 1; }},
        {"building.kind", +[](Game& g) { for (auto& b : g.content.buildings) b.kind = BuildingKind::Fort; }},
        {"building.key", +[](Game& g) { for (auto& b : g.content.buildings) b.key += "x"; }},
        {"building.name", +[](Game& g) { for (auto& b : g.content.buildings) b.name += "x"; }},
        {"building.base_cost", +[](Game& g) { for (auto& b : g.content.buildings) b.base_cost += 1.0; }},
        {"building.per_state", +[](Game& g) { for (auto& b : g.content.buildings) b.per_state = !b.per_state; }},
        {"building.max_level", +[](Game& g) { for (auto& b : g.content.buildings) b.max_level += 1; }},
        {"country.divisions", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.divisions.push_back(DivisionId(0)); }); }},
        {"country.armies", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.armies.push_back(ArmyId(0)); }); }},
        {"country.training", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.training.push_back(TrainingDivision{DivisionId(0), TemplateId(0), 5.0}); }); }},
        {"training.days_left", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& t : c.training) t.days_left += 1.0; }); }},
        {"training.template_id", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& t : c.training) t.template_id = TemplateId(1); }); }},
        {"training.division", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& t : c.training) t.division = DivisionId(1); }); }},
        {"division.organization", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.organization += 1.0; }); }},
        {"division.max_organization", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.max_organization += 1.0; }); }},
        {"division.strength", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.strength -= 0.01; }); }},
        {"division.experience", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.experience += 0.01; }); }},
        {"division.entrenchment", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.entrenchment += 0.01; }); }},
        {"division.planning", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.planning += 0.01; }); }},
        {"division.supply", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.supply -= 0.01; }); }},
        {"division.fuel", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.fuel -= 0.01; }); }},
        {"division.manpower", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.manpower += 1.0; }); }},
        {"division.equipment", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { if (!d.equipment.empty()) d.equipment[0] += 1.0; }); }},
        {"division.army", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.army = ArmyId(1); }); }},
        {"division.order", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.order = OrderKind::Garrison; }); }},
        {"division.order_target", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.order_target = ProvinceId(5); }); }},
        {"division.moving", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.moving = !d.moving; }); }},
        {"division.move_from", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.move_from = ProvinceId(4); }); }},
        {"division.move_to", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.move_to = ProvinceId(4); }); }},
        {"division.move_progress", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.move_progress += 0.01; }); }},
        {"division.path", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.path.push_back(ProvinceId(2)); }); }},
        {"division.retreating", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.retreating = !d.retreating; }); }},
        {"division.battle", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.battle = BattleId(0); }); }},
        {"division.combat_attack_modifier", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.combat_attack_modifier += 0.01; }); }},
        {"division.training_days_left", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.training_days_left += 1.0; }); }},
        {"division.losses_manpower", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.losses_manpower += 1.0; }); }},
        {"division.losses_equipment", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.losses_equipment += 1.0; }); }},
        {"division.created_tick", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.created_tick += 1; }); }},
        {"division.previous_location", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.previous_location = ProvinceId(3); }); }},
        {"division.location", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.location = ProvinceId(3); }); }},
        {"division.template_id", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.template_id = TemplateId(1); }); }},
        {"division.name", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.name += "x"; }); }},
        {"division.country", +[](Game& g) { g.world.divisions.for_each([](DivisionId, Division& d) { d.country = CountryId(2); }); }},
        {"army.name", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.name += "x"; }); }},
        {"army.country", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.country = CountryId(2); }); }},
        {"army.general", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.general = CharacterId(1); }); }},
        {"army.divisions", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.divisions.push_back(DivisionId(0)); }); }},
        {"army.order.kind", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.order.kind = OrderKind::Fallback; }); }},
        {"army.order.line", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.order.line.push_back(ProvinceId(2)); }); }},
        {"army.order.target_line", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.order.target_line.push_back(ProvinceId(2)); }); }},
        {"army.order.progress", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.order.progress += 0.05; }); }},
        {"army.order.started", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.order.started += 1; }); }},
        {"army.stance", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.stance = static_cast<uint8_t>(2); }); }},
        {"army.motorization", +[](Game& g) { g.world.armies.for_each([](ArmyId, Army& a) { a.motorization += 1; }); }},
        {"wing.country", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.country = CountryId(2); }); }},
        {"wing.equipment", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.equipment = EquipmentId(1); }); }},
        {"wing.name", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.name += "x"; }); }},
        {"wing.planes", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.planes += 1; }); }},
        {"wing.max_planes", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.max_planes += 1; }); }},
        {"wing.base", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.base = ProvinceId(5); }); }},
        {"wing.region", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.region = RegionId(1); }); }},
        {"wing.mission", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.mission = AirMission::LogisticsStrike; }); }},
        {"wing.efficiency", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.efficiency -= 0.01; }); }},
        {"wing.experience", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.experience += 0.01; }); }},
        {"wing.losses", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.losses += 1; }); }},
        {"wing.last_sortie", +[](Game& g) { g.world.air_wings.for_each([](AirWingId, AirWing& a) { a.last_sortie += 1; }); }},
        {"ship.country", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.country = CountryId(2); }); }},
        {"ship.equipment", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.equipment = EquipmentId(0); }); }},
        {"ship.name", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.name += "x"; }); }},
        {"ship.fleet", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.fleet = FleetId(0); }); }},
        {"ship.task_force", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.task_force = TaskForceId(0); }); }},
        {"ship.strength", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.strength -= 0.01; }); }},
        {"ship.organisation", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.organisation -= 0.01; }); }},
        {"ship.experience", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.experience += 0.01; }); }},
        {"ship.fuel", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.fuel -= 0.01; }); }},
        {"ship.port", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.port = ProvinceId(6); }); }},
        {"ship.sea_region", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.sea_region = RegionId(0); }); }},
        {"ship.at_sea", +[](Game& g) { g.world.ships.for_each([](ShipId, Ship& s) { s.at_sea = !s.at_sea; }); }},
        {"task_force.country", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.country = CountryId(2); }); }},
        {"task_force.fleet", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.fleet = FleetId(0); }); }},
        {"task_force.name", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.name += "x"; }); }},
        {"task_force.ships", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.ships.push_back(ShipId(0)); }); }},
        {"task_force.port", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.port = ProvinceId(6); }); }},
        {"task_force.sea_region", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.sea_region = RegionId(0); }); }},
        {"task_force.mission", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.mission = NavalMission::Training; }); }},
        {"task_force.at_sea", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.at_sea = !t.at_sea; }); }},
        {"task_force.detection", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.detection += 0.05; }); }},
        {"task_force.last_engagement", +[](Game& g) { g.world.task_forces.for_each([](TaskForceId, TaskForce& t) { t.last_engagement += 1; }); }},
        {"fleet.country", +[](Game& g) { g.world.fleets.for_each([](FleetId, Fleet& fl) { fl.country = CountryId(2); }); }},
        {"fleet.name", +[](Game& g) { g.world.fleets.for_each([](FleetId, Fleet& fl) { fl.name += "x"; }); }},
        {"fleet.task_forces", +[](Game& g) { g.world.fleets.for_each([](FleetId, Fleet& fl) { fl.task_forces.push_back(TaskForceId(0)); }); }},
        {"invasion.army", +[](Game& g) { for (auto& i : g.world.invasions) i.army = ArmyId(1); }},
        {"invasion.country", +[](Game& g) { for (auto& i : g.world.invasions) i.country = CountryId(1); }},
        {"invasion.origin", +[](Game& g) { for (auto& i : g.world.invasions) i.origin = ProvinceId(6); }},
        {"invasion.target", +[](Game& g) { for (auto& i : g.world.invasions) i.target = ProvinceId(1); }},
        {"invasion.sea_region", +[](Game& g) { for (auto& i : g.world.invasions) i.sea_region = RegionId(0); }},
        {"invasion.progress", +[](Game& g) { for (auto& i : g.world.invasions) i.progress += 0.01; }},
        {"invasion.started", +[](Game& g) { for (auto& i : g.world.invasions) i.started += 1; }},
        {"invasion.landed", +[](Game& g) { for (auto& i : g.world.invasions) i.landed = !i.landed; }},
        {"battle.province", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.province = ProvinceId(2); }); }},
        {"battle.start_tick", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.start_tick += 1; }); }},
        {"battle.attacker.divisions", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.divisions.push_back(DivisionId(1)); }); }},
        {"battle.attacker.width_used", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.width_used.push_back(5.0); }); }},
        {"battle.attacker.total_soft_attack", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_soft_attack += 1.0; }); }},
        {"battle.attacker.total_hard_attack", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_hard_attack += 1.0; }); }},
        {"battle.attacker.total_defense", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_defense += 1.0; }); }},
        {"battle.attacker.total_breakthrough", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_breakthrough += 1.0; }); }},
        {"battle.attacker.total_armor", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_armor += 1.0; }); }},
        {"battle.attacker.total_piercing", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker.total_piercing += 1.0; }); }},
        {"battle.defender", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.defender.total_defense += 1.0; }); }},
        {"battle.progress", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.progress += 0.01; }); }},
        {"battle.terrain", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.terrain = Terrain::Urban; }); }},
        {"battle.river_crossing", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.river_crossing = !b.river_crossing; }); }},
        {"battle.encirclement", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.encirclement = !b.encirclement; }); }},
        {"battle.attacker_lead", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.attacker_lead = CountryId(2); }); }},
        {"battle.defender_lead", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.defender_lead = CountryId(2); }); }},
        {"battle.debug", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.debug.push_back(BattleDebugLine{}); }); }},
        {"battle.debug.division", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { for (auto& d : b.debug) d.division = DivisionId(2); }); }},
        {"battle.debug.attack_mods", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { for (auto& d : b.debug) { d.planning_mod += 1.0; d.terrain_mod += 1.0; d.supply_mod += 1.0; d.commander_mod += 1.0; d.experience_mod += 1.0; } }); }},
        {"battle.debug.air_mod", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { for (auto& d : b.debug) d.air_mod += 1.0; }); }},
        {"battle.debug.damage", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { for (auto& d : b.debug) { d.base_attack += 1.0; d.final_attack += 1.0; d.enemy_defense += 1.0; d.damage += 1.0; d.org_damage += 1.0; d.strength_damage += 1.0; } }); }},
        {"battle.last_tick", +[](Game& g) { g.world.battles.for_each([](BattleId, Battle& b) { b.last_tick += 1; }); }},
        {"war.attackers", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.attackers.push_back(WarParticipant{CountryId(2), 1.0, 2.0, 0.3}); }); }},
        {"war.defenders", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.defenders.push_back(WarParticipant{CountryId(2), 1.0, 2.0, 0.3}); }); }},
        {"war.participant.casualties_manpower", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& p : w.attackers) p.casualties_manpower += 1.0; }); }},
        {"war.participant.casualties_equipment", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& p : w.attackers) p.casualties_equipment += 1.0; }); }},
        {"war.participant.occupation_share", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& p : w.attackers) p.occupation_share += 1.0; }); }},
        {"war.defender.casualties", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& p : w.defenders) p.casualties_manpower += 1.0; }); }},
        {"war.goals", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.goals.push_back(WarGoal{CountryId(0), CountryId(1), StateId(2), true, true}); }); }},
        {"war.goal.claimant", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& x : w.goals) x.claimant = CountryId(2); }); }},
        {"war.goal.target", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& x : w.goals) x.target = CountryId(2); }); }},
        {"war.goal.state", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& x : w.goals) x.state = StateId(3); }); }},
        {"war.goal.annex_country", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& x : w.goals) x.annex_country = !x.annex_country; }); }},
        {"war.goal.puppet", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { for (auto& x : w.goals) x.puppet = !x.puppet; }); }},
        {"war.start_tick", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.start_tick += 1; }); }},
        {"war.active", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.active = !w.active; }); }},
        {"war.aggressor", +[](Game& g) { g.world.wars.for_each([](WarId, War& w) { w.aggressor = CountryId(2); }); }},
        {"faction.id", +[](Game& g) { for (auto& fa : g.world.factions) fa.id += 1; }},
        {"faction.name", +[](Game& g) { for (auto& fa : g.world.factions) fa.name += "x"; }},
        {"faction.leader", +[](Game& g) { for (auto& fa : g.world.factions) fa.leader = CountryId(1); }},
        {"faction.members", +[](Game& g) { for (auto& fa : g.world.factions) fa.members.push_back(CountryId(1)); }},
        {"relation.value", +[](Game& g) { for (auto& e : g.world.relations) e.second.value += 1.0; }},
        {"relation.non_aggression", +[](Game& g) { for (auto& e : g.world.relations) e.second.non_aggression = !e.second.non_aggression; }},
        {"relation.military_access", +[](Game& g) { for (auto& e : g.world.relations) e.second.military_access = !e.second.military_access; }},
        {"relation.guarantee", +[](Game& g) { for (auto& e : g.world.relations) e.second.guarantee = !e.second.guarantee; }},
        {"relation.at_war", +[](Game& g) { for (auto& e : g.world.relations) e.second.at_war = !e.second.at_war; }},
        {"country.wars", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.wars.push_back(WarId(0)); }); }},
        {"country.faction", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.faction += 1; }); }},
        {"country.political_power", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.political_power += 1.0; }); }},
        {"country.stability", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.stability += 0.01; }); }},
        {"country.war_support", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.war_support += 0.01; }); }},
        {"country.manpower", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.manpower += 1.0; }); }},
        {"country.fuel", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.fuel += 1.0; }); }},
        {"country.fuel_capacity", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.fuel_capacity += 1.0; }); }},
        {"country.consumer_goods_ratio", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.consumer_goods_ratio += 0.01; }); }},
        {"country.army_experience", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.army_experience += 0.01; }); }},
        {"country.at_war", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.at_war = !c.at_war; }); }},
        {"country.fuel_priority", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.fuel_priority = !c.fuel_priority; }); }},
        {"country.last_capitulation_check", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.last_capitulation_check += 1; }); }},
        {"ai.layer.last_run_tick", +[](Game& g) { g.ai.layers[0].last_run_tick += 1; }},
        {"ai.layer.interval_ticks", +[](Game& g) { g.ai.layers[0].interval_ticks += 1; }},
        {"ai.layer.reasons", +[](Game& g) { g.ai.layers[0].last_reasons.push_back(AiReason{"x", 1.0, {}}); }},
        {"ai.reason.what", +[](Game& g) { for (auto& r : g.ai.layers[0].last_reasons) r.what += "x"; }},
        {"ai.reason.score", +[](Game& g) { for (auto& r : g.ai.layers[0].last_reasons) r.score += 1.0; }},
        {"ai.reason.factors", +[](Game& g) { for (auto& r : g.ai.layers[0].last_reasons) r.factors.push_back({"k", 1.0}); }},
        {"ai.posture", +[](Game& g) { g.ai.posture.push_back(2); }},
        {"ai.decisions_made", +[](Game& g) { g.ai.decisions_made += 1; }},
        {"ai.commands_issued", +[](Game& g) { g.ai.commands_issued += 1; }},
        {"ai_controlled", +[](Game& g) { g.ai_controlled.push_back(1); }},
        {"player_country", +[](Game& g) { g.player_country = CountryId(1); }},
        {"game.seed", +[](Game& g) { g.seed += 1; }},
        {"world.world_seed", +[](Game& g) { g.world.world_seed += 1; }},
        {"world.tick", +[](Game& g) { g.world.tick += 1; }},
        {"world.date", +[](Game& g) { g.world.date.hour = static_cast<uint8_t>((g.world.date.hour + 1) % 24); }},
        {"game.ticks_run", +[](Game& g) { g.ticks_run += 1; }},
        {"game.start_date", +[](Game& g) { g.start_date.day = 2; }},
        {"rng.master_seed", +[](Game& g) { g.rng.seed(g.rng.master_seed() + 1); }},
        {"rng.stream_state", +[](Game& g) { g.rng.get(RngStream::Events).next_u64(); }},
        {"queue.pending", +[](Game& g) { g.queue.push(Command{}); }},
        {"focus.index", +[](Game& g) { for (auto& t : g.content.focuses) t.index += 1; }},
        {"focus.key", +[](Game& g) { for (auto& t : g.content.focuses) t.key += "x"; }},
        {"focus.name", +[](Game& g) { for (auto& t : g.content.focuses) t.name += "x"; }},
        {"focus.tree", +[](Game& g) { for (auto& t : g.content.focuses) t.tree += "x"; }},
        {"focus.x", +[](Game& g) { for (auto& t : g.content.focuses) t.x += 1; }},
        {"focus.y", +[](Game& g) { for (auto& t : g.content.focuses) t.y += 1; }},
        {"focus.days", +[](Game& g) { for (auto& t : g.content.focuses) t.days += 1.0; }},
        {"focus.prerequisites", +[](Game& g) { for (auto& t : g.content.focuses) t.prerequisites.push_back("x"); }},
        {"focus.mutually_exclusive", +[](Game& g) { for (auto& t : g.content.focuses) t.mutually_exclusive.push_back("x"); }},
        {"focus.available", +[](Game& g) { for (auto& t : g.content.focuses) t.available = Json(true); }},
        {"focus.bypass", +[](Game& g) { for (auto& t : g.content.focuses) t.bypass = Json(true); }},
        {"focus.effects", +[](Game& g) { for (auto& t : g.content.focuses) t.effects = Json(true); }},
        {"focus.ai_weight", +[](Game& g) { for (auto& t : g.content.focuses) t.ai_weight += 0.1; }},
        {"event.index", +[](Game& g) { for (auto& e : g.content.events) e.index += 1; }},
        {"event.key", +[](Game& g) { for (auto& e : g.content.events) e.key += "x"; }},
        {"event.title", +[](Game& g) { for (auto& e : g.content.events) e.title += "x"; }},
        {"event.description", +[](Game& g) { for (auto& e : g.content.events) e.description += "x"; }},
        {"event.fire_only_once", +[](Game& g) { for (auto& e : g.content.events) e.fire_only_once = !e.fire_only_once; }},
        {"event.major", +[](Game& g) { for (auto& e : g.content.events) e.major = !e.major; }},
        {"event.trigger", +[](Game& g) { for (auto& e : g.content.events) e.trigger = Json(true); }},
        {"event.immediate", +[](Game& g) { for (auto& e : g.content.events) e.immediate = Json(true); }},
        {"event.options", +[](Game& g) { for (auto& e : g.content.events) e.options.push_back(EventOptionDef{}); }},
        {"event.option.name", +[](Game& g) { for (auto& e : g.content.events) { for (auto& o : e.options) o.name += "x"; } }},
        {"event.option.effects", +[](Game& g) { for (auto& e : g.content.events) { for (auto& o : e.options) o.effects = Json(true); } }},
        {"event.option.ai_weight", +[](Game& g) { for (auto& e : g.content.events) { for (auto& o : e.options) o.ai_weight += 1.0; } }},
        {"decision.index", +[](Game& g) { for (auto& d : g.content.decisions) d.index += 1; }},
        {"decision.key", +[](Game& g) { for (auto& d : g.content.decisions) d.key += "x"; }},
        {"decision.name", +[](Game& g) { for (auto& d : g.content.decisions) d.name += "x"; }},
        {"decision.description", +[](Game& g) { for (auto& d : g.content.decisions) d.description += "x"; }},
        {"decision.category", +[](Game& g) { for (auto& d : g.content.decisions) d.category += 1; }},
        {"decision.targets_state", +[](Game& g) { for (auto& d : g.content.decisions) d.targets_state = !d.targets_state; }},
        {"decision.cost_pp", +[](Game& g) { for (auto& d : g.content.decisions) d.cost_pp += 1.0; }},
        {"decision.days_remove", +[](Game& g) { for (auto& d : g.content.decisions) d.days_remove += 1; }},
        {"decision.days_cooldown", +[](Game& g) { for (auto& d : g.content.decisions) d.days_cooldown += 1; }},
        {"decision.visible", +[](Game& g) { for (auto& d : g.content.decisions) d.visible = Json(true); }},
        {"decision.available", +[](Game& g) { for (auto& d : g.content.decisions) d.available = Json(true); }},
        {"decision.effects", +[](Game& g) { for (auto& d : g.content.decisions) d.effects = Json(true); }},
        {"decision.remove_effect", +[](Game& g) { for (auto& d : g.content.decisions) d.remove_effect = Json(true); }},
        {"decision.ai_weight", +[](Game& g) { for (auto& d : g.content.decisions) d.ai_weight += 0.1; }},
        {"spirit.index", +[](Game& g) { for (auto& s : g.content.spirits) s.index += 1; }},
        {"spirit.key", +[](Game& g) { for (auto& s : g.content.spirits) s.key += "x"; }},
        {"spirit.name", +[](Game& g) { for (auto& s : g.content.spirits) s.name += "x"; }},
        {"spirit.description", +[](Game& g) { for (auto& s : g.content.spirits) s.description += "x"; }},
        {"spirit.slots", +[](Game& g) { for (auto& s : g.content.spirits) s.slots += 1; }},
        {"spirit.available", +[](Game& g) { for (auto& s : g.content.spirits) s.available = Json(true); }},
        {"spirit.modifiers", +[](Game& g) { for (auto& s : g.content.spirits) s.modifiers.v[0] += 0.01; }},
        {"spirit.effects", +[](Game& g) { for (auto& s : g.content.spirits) s.effects = Json(true); }},
        {"advisor.index", +[](Game& g) { for (auto& a : g.content.advisors) a.index += 1; }},
        {"advisor.key", +[](Game& g) { for (auto& a : g.content.advisors) a.key += "x"; }},
        {"advisor.name", +[](Game& g) { for (auto& a : g.content.advisors) a.name += "x"; }},
        {"advisor.description", +[](Game& g) { for (auto& a : g.content.advisors) a.description += "x"; }},
        {"advisor.cost_pp", +[](Game& g) { for (auto& a : g.content.advisors) a.cost_pp += 1.0; }},
        {"advisor.available", +[](Game& g) { for (auto& a : g.content.advisors) a.available = Json(true); }},
        {"advisor.modifiers", +[](Game& g) { for (auto& a : g.content.advisors) a.modifiers.v[0] += 0.01; }},
        {"country.completed_focuses", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.completed_focuses.push_back(1); }); }},
        {"country.selected_focus", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.selected_focus += 1; }); }},
        {"country.focus_progress", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.focus_progress += 1.0; }); }},
        {"country.pending_events", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.pending_events.push_back(0); }); }},
        {"country.fired_events", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.fired_events.push_back(0); }); }},
        {"country.active_decisions", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.active_decisions.push_back(0); }); }},
        {"country.decision_days_left", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.decision_days_left.push_back(1.0); }); }},
        {"country.decision_cooldown", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { if (!c.decision_cooldown.empty()) c.decision_cooldown[0] += 1.0; }); }},
        {"country.country_flags", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.country_flags.push_back("x"); }); }},
        {"country.timed_modifiers", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.timed_modifiers.push_back(TimedModifier{}); }); }},
        {"country.timed_modifier.source", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.timed_modifiers) m.source += "x"; }); }},
        {"country.timed_modifier.mods", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.timed_modifiers) m.mods.v[0] += 0.01; }); }},
        {"country.timed_modifier.days_left", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.timed_modifiers) m.days_left += 1; }); }},
        {"country.national_spirits", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.national_spirits.push_back(TimedModifier{}); }); }},
        {"country.national_spirit.source", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.national_spirits) m.source += "x"; }); }},
        {"country.national_spirit.mods", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.national_spirits) m.mods.v[0] += 0.01; }); }},
        {"country.national_spirit.days_left", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { for (auto& m : c.national_spirits) m.days_left -= 1; }); }},
        {"country.spirit_keys", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.spirit_keys.push_back(1); }); }},
        {"country.advisors", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.advisors.push_back(1); }); }},
        {"country.spirit_slots", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.spirit_slots += 1; }); }},
        {"country.advisor_slots", +[](Game& g) { g.world.countries.for_each([](CountryId, Country& c) { c.advisor_slots += 1; }); }},
        {"world.script_vars.value", +[](Game& g) { for (auto& e : g.world.script_vars) e.second += 1.0; }},
        {"world.script_vars.key", +[](Game& g) { g.world.script_vars["added"] = 1.0; }},
        {"world.delayed_events", +[](Game& g) { g.world.delayed_events.push_back(DelayedEvent{}); }},
        {"delayed_event.country", +[](Game& g) { for (auto& d : g.world.delayed_events) d.country = CountryId(1); }},
        {"delayed_event.event", +[](Game& g) { for (auto& d : g.world.delayed_events) d.event += 1; }},
        {"delayed_event.due", +[](Game& g) { for (auto& d : g.world.delayed_events) d.due += 1; }},
    };

    for (const Flip& flip : flips) {
        Game mutated = f.source;
        flip.fn(mutated);
        if (world_hash(mutated) == before) {
            ::hoi_test::fail(__FILE__, __LINE__,
                             std::string("field is not covered by the world hash: ") + flip.what);
        }
    }

    // SimConstants is one flat record of balance numbers: every member is part of the
    // hash individually, so a data tune cannot slip past a save/load comparison.
    double SimConstants::*const constant_members[] = {
        &SimConstants::ic_per_military_factory,
        &SimConstants::ic_per_civilian_factory,
        &SimConstants::ic_per_dockyard,
        &SimConstants::efficiency_start,
        &SimConstants::efficiency_cap_base,
        &SimConstants::efficiency_cap_growth_per_day,
        &SimConstants::efficiency_growth_per_day,
        &SimConstants::switch_same_archetype_retention,
        &SimConstants::resource_shortage_floor,
        &SimConstants::consumer_goods_base,
        &SimConstants::fuel_per_oil,
        &SimConstants::fuel_storage_per_factory,
        &SimConstants::synthetic_oil_per_refinery_per_day,
        &SimConstants::synthetic_rubber_per_refinery_per_day,
        &SimConstants::construction_cost_factory,
        &SimConstants::construction_cost_infrastructure,
        &SimConstants::construction_cost_railway,
        &SimConstants::construction_cost_supply_hub,
        &SimConstants::construction_cost_air_base,
        &SimConstants::construction_cost_naval_base,
        &SimConstants::construction_cost_fort,
        &SimConstants::construction_cost_radar,
        &SimConstants::construction_cost_synthetic,
        &SimConstants::construction_level_scaling,
        &SimConstants::max_factories_per_project,
        &SimConstants::research_base_days,
        &SimConstants::research_year_penalty,
        &SimConstants::research_speed_base,
        &SimConstants::manpower_growth_per_year_fraction,
        &SimConstants::recruitable_base,
        &SimConstants::base_hours_per_province,
        &SimConstants::min_division_speed,
        &SimConstants::river_crossing_penalty,
        &SimConstants::combat_width_base,
        &SimConstants::damage_scale,
        &SimConstants::org_damage_share,
        &SimConstants::strength_damage_share,
        &SimConstants::armor_advantage_multiplier,
        &SimConstants::armor_disadvantage_multiplier,
        &SimConstants::org_recovery_base,
        &SimConstants::entrenchment_per_day,
        &SimConstants::planning_per_day,
        &SimConstants::planning_max_attack_bonus,
        &SimConstants::battle_retreat_org_threshold,
        &SimConstants::max_battles_per_province,
        &SimConstants::supply_hub_radius,
        &SimConstants::supply_range_penalty,
        &SimConstants::supply_demand_per_width,
        &SimConstants::supply_rail_bonus_per_level,
        &SimConstants::supply_infrastructure_bonus_per_level,
        &SimConstants::fuel_demand_per_day,
        &SimConstants::air_base_capacity_per_level,
        &SimConstants::air_sortie_hours,
        &SimConstants::air_cas_effect,
        &SimConstants::air_superiority_effect,
        &SimConstants::air_bombing_industry_damage,
        &SimConstants::air_logistics_strike_damage,
        &SimConstants::air_anti_air_bombing_reduction,
        &SimConstants::air_anti_air_combat_loss_factor,
        &SimConstants::air_combat_scale,
        &SimConstants::air_aircraft_durability,
        &SimConstants::air_agility_weight,
        &SimConstants::air_combat_defence_floor,
        &SimConstants::air_combat_roll_base,
        &SimConstants::air_experience_per_combat_hour,
        &SimConstants::air_experience_per_mission_hour,
        &SimConstants::air_cas_organisation_damage,
        &SimConstants::air_cas_strength_damage,
        &SimConstants::air_bombing_power_unit,
        &SimConstants::air_logistics_power_unit,
        &SimConstants::air_support_min_modifier,
        &SimConstants::air_support_max_modifier,
        &SimConstants::air_mission_weight_contested,
        &SimConstants::air_mission_weight_support,
        &SimConstants::naval_base_capacity_per_level,
        &SimConstants::naval_detection_scale,
        &SimConstants::naval_combat_roll_base,
        &SimConstants::naval_combat_scale,
        &SimConstants::naval_org_damage_scale,
        &SimConstants::naval_torpedo_large_hull_bonus,
        &SimConstants::naval_sub_detection_penalty,
        &SimConstants::naval_aa_carrier_air_factor,
        &SimConstants::naval_air_attacks_per_hour,
        &SimConstants::naval_screen_share_cap,
        &SimConstants::naval_retreat_strength_threshold,
        &SimConstants::naval_retreat_org_threshold,
        &SimConstants::naval_repair_per_hour,
        &SimConstants::naval_repair_org_per_hour,
        &SimConstants::naval_repair_cost_fuel,
        &SimConstants::naval_repair_cost_stockpile_share,
        &SimConstants::naval_fuel_use_per_hour,
        &SimConstants::naval_training_experience_per_hour,
        &SimConstants::naval_combat_experience_per_hour,
        &SimConstants::naval_raid_convoy_damage,
        &SimConstants::naval_raid_control_cut,
        &SimConstants::naval_escort_protection,
        &SimConstants::naval_max_engagement_ships,
        &SimConstants::naval_large_hull_hp,
        &SimConstants::naval_base_supply_per_level,
        &SimConstants::naval_supply_sea_range_penalty,
        &SimConstants::naval_supply_convoy_use_per_capacity,
        &SimConstants::naval_supply_raid_threshold,
        &SimConstants::naval_invasion_convoys_per_division,
        &SimConstants::naval_invasion_hours_per_sea_hop,
        &SimConstants::naval_invasion_interception_base,
        &SimConstants::naval_invasion_interception_threat_scale,
        &SimConstants::naval_invasion_escort_mitigation,
        &SimConstants::political_power_per_day,
        &SimConstants::focus_progress_speed,
        &SimConstants::stability_drift,
        &SimConstants::war_support_drift,
        &SimConstants::weather_change_chance,
    };
    // Drift guard: SimConstants is a plain struct, so nothing forces a new field to be
    // added here and to write_constants. The Economy payload ends with the flat
    // constants record (a count followed by one double per member), so the count the
    // serializer actually wrote is compared with this table.
    constexpr size_t constant_count = sizeof(constant_members) / sizeof(constant_members[0]);
    static_assert(constant_count == 112,
                  "SimConstants changed: update constant_members and save.cpp write/read_constants");
    ByteWriter economy;
    serialize_subsystem(f.source, Subsystem::Economy, &economy);
    const std::vector<uint8_t>& economy_bytes = economy.data();
    const size_t constants_bytes = 4 + 8 * constant_count;
    CHECK_GT(economy_bytes.size(), constants_bytes);
    CHECK_EQ(peek_u32(economy_bytes, economy_bytes.size() - constants_bytes),
             static_cast<uint32_t>(constant_count));
    for (double SimConstants::*member : constant_members) {
        Game mutated = f.source;
        mutated.content.constants.*member += 1.0;
        if (world_hash(mutated) == before) {
            ::hoi_test::fail(__FILE__, __LINE__, "SimConstants member is not covered by the world hash");
        }
    }
}

// A save must be self-contained: GOLDEN_010 loads into a default-constructed Game
// (no content, no scenario), and a save that left the content database or the control
// routing behind would continue differently from the run it came from.
HOI_TEST(save_load_into_default_constructed_game_is_self_contained) {
    const Fixture& f = fixture();

    Game empty;  // no content, no world, no scenario path
    std::string err;
    require(load_game(empty, f.save_path, &err), "load_game into empty Game", err);

    CHECK_EQ(world_hash(empty), f.hash);
    check_all_sections_equal(f.source, empty);
    CHECK_EQ(empty.content.equipment.size(), f.source.content.equipment.size());
    CHECK_EQ(empty.content.techs.size(), f.source.content.techs.size());
    CHECK_EQ(empty.content.laws.size(), f.source.content.laws.size());
    CHECK_EQ(empty.content.buildings.size(), f.source.content.buildings.size());
    CHECK_EQ(empty.content.templates.size(), f.source.content.templates.size());
    CHECK_EQ(empty.content.focuses.size(), f.source.content.focuses.size());
    CHECK_EQ(empty.content.events.size(), f.source.content.events.size());
    CHECK_EQ(empty.content.decisions.size(), f.source.content.decisions.size());
    CHECK_EQ(empty.content.spirits.size(), f.source.content.spirits.size());
    CHECK_EQ(empty.content.advisors.size(), f.source.content.advisors.size());
    CHECK(empty.content.constants.ic_per_military_factory ==
          f.source.content.constants.ic_per_military_factory);
    // Derived key -> id maps are rebuilt, not stored: lookups must work after a load.
    for (const EquipmentDef& e : f.source.content.equipment) {
        CHECK(empty.content.equipment_id(e.key) == e.id);
    }
    for (const DivisionTemplate& t : f.source.content.templates) {
        CHECK(empty.content.template_id(t.key) == t.id);
    }
    CHECK(empty.content.laws.size() == f.source.content.laws.size());
    if (!f.source.content.laws.empty()) {
        CHECK(empty.content.law(f.source.content.laws.front().key) != nullptr);
    }
    // Focuses, events and decisions are addressed by key through the rebuilt indexes.
    for (const FocusDef& focus : f.source.content.focuses) {
        CHECK(empty.content.focus_id(focus.key) != 0xFFFFFFFFu);
        CHECK(empty.content.focus(empty.content.focus_id(focus.key)) != nullptr);
    }
    for (const EventDef& event : f.source.content.events) {
        CHECK(empty.content.event_id(event.key) != 0xFFFFFFFFu);
        CHECK(empty.content.event(empty.content.event_id(event.key)) != nullptr);
    }
    for (const DecisionDef& decision : f.source.content.decisions) {
        CHECK(empty.content.decision_id(decision.key) != 0xFFFFFFFFu);
        CHECK(empty.content.decision(empty.content.decision_id(decision.key)) != nullptr);
    }
    // National spirits and advisors are addressed by key through the rebuilt indexes.
    for (const SpiritDef& spirit : f.source.content.spirits) {
        CHECK(empty.content.spirit_id(spirit.key) != 0xFFFFFFFFu);
        CHECK(empty.content.spirit(empty.content.spirit_id(spirit.key)) != nullptr);
    }
    for (const AdvisorDef& advisor : f.source.content.advisors) {
        CHECK(empty.content.advisor_id(advisor.key) != 0xFFFFFFFFu);
        CHECK(empty.content.advisor(empty.content.advisor_id(advisor.key)) != nullptr);
    }
    // The per-country spirit/advisor state survives the load into an empty Game.
    const Country& src_country = f.source.world.countries[CountryId(0)];
    const Country& loaded_country = empty.world.countries[CountryId(0)];
    CHECK_EQ(loaded_country.national_spirits.size(), src_country.national_spirits.size());
    CHECK_EQ(loaded_country.spirit_keys.size(), src_country.spirit_keys.size());
    CHECK_EQ(loaded_country.advisors.size(), src_country.advisors.size());
    CHECK_EQ(loaded_country.spirit_slots, src_country.spirit_slots);
    CHECK_EQ(loaded_country.advisor_slots, src_country.advisor_slots);

    // Continuing both runs must stay identical: this is what a lost field would break.
    Game original = f.source;
    Game loaded = empty;
    original.run_ticks(TICKS_PER_DAY * 5);
    loaded.run_ticks(TICKS_PER_DAY * 5);
    CHECK_EQ(world_hash(original), world_hash(loaded));
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        const Subsystem s = static_cast<Subsystem>(i);
        if (subsystem_hash(original, s) != subsystem_hash(loaded, s)) {
            ::hoi_test::fail(__FILE__, __LINE__,
                             std::string("subsystem diverged after continuing: ") + subsystem_name(s));
        }
    }
}

HOI_TEST(replay_round_trip_restores_log_and_initial_hash) {
    const Fixture& f = fixture();

    Game game;
    build_world(&game);
    std::string err;
    require(load_game(game, f.save_path, &err), "load_game", err);
    // The replay was recorded from this exact state, so its initial hash must be the
    // hash of the state loaded from the save.
    CHECK_EQ(world_hash(game), f.hash);

    require(load_replay(game, f.replay_path, &err), "load_replay", err);
    CHECK_EQ(world_hash(game), f.hash);
    CHECK_EQ(game.log.records.size(), f.log_count);
    CHECK_EQ(game.log.next_sequence, f.log_next_sequence);
    CHECK_EQ(game.seed, f.source.seed);
    CHECK_EQ(game.log.records.front().command.text, f.source.log.records.front().command.text);

    // A replay must not be accepted against a different state at the same tick.
    Game other;
    build_world(&other);
    require(load_game(other, f.save_path, &err), "load_game", err);
    other.world.provinces.for_each([](ProvinceId, Province& p) { p.infrastructure += 1; });
    other.world.tick = game.world.tick;
    std::string mismatch;
    CHECK(!load_replay(other, f.replay_path, &mismatch));
    CHECK(mismatch.find("replay initial hash mismatch") != std::string::npos);
}

HOI_TEST(save_rejects_a_replay_file_and_vice_versa) {
    const Fixture& f = fixture();
    Game game = f.fresh;
    std::string err;
    CHECK(!load_game(game, f.replay_path, &err));
    CHECK(err.find("not a save file") != std::string::npos);

    err.clear();
    CHECK(!load_replay(game, f.save_path, &err));
    CHECK(err.find("not a replay file") != std::string::npos);
}
