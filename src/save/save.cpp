// Save/load, subsystem hashing and deterministic replay (ARCHITECTURE.md section 8).
//
// A save file is a versioned container of self-describing sections. Each section is
// hashed on its own, so a load that reproduces every section hash provably
// reproduced the state, and a mismatch names the subsystem that diverged (the
// desync diagnostic). Hashing and saving share the exact same canonical bytes:
// `subsystem_hash` is `fnv1a(serialize_subsystem(...))`.
//
// Section split - every field lives in exactly one section, so sections can be
// reordered, compared and hashed independently:
//
//   Map        geography: province store (adjacency, sea_adj, resources, grid
//              coordinates, the supply cache fields supply_level/supply_source/
//              supply_bottleneck), state store (factories, occupation counters),
//              region store (weather)
//   Countries  country identity and ownership bookkeeping (id, tag, name, alive,
//              ideology, overlord, puppets, capital), equipment_stockpile,
//              law_levels, the four Modifier sets, the per-resource
//              produced/consumed/imported/exported aggregates, starting_factories,
//              the general roster (Country::generals) and the character store
//   Economy    the content snapshot - equipment, division templates, technologies,
//              laws, buildings and SimConstants, i.e. every table the simulation
//              reads - plus per country: production lines, construction queue,
//              research state and the template roster (Country::templates). Content
//              lives here because it is what industry and research consume, and it
//              travels with the save so a default-constructed Game can continue from
//              a file without help from data/; the derived key -> id maps are rebuilt
//              on load instead of being stored twice
//   Military   per country: division/army rosters and the training list; then the
//              army store and the division store
//   Battles    the battle store, including both sides, the debug breakdown lines
//              and the last-tick markers
//   Diplomacy  the war store, factions, the ordered relation map, and per country
//              the war list and faction membership
//   Politics   per-country scalars and flags not carried by another section:
//              political_power, stability, war_support, manpower, fuel,
//              fuel_capacity, consumer_goods_ratio, army_experience, at_war,
//              fuel_priority, last_capitulation_check
//   Ai         the six AI layer states (last run tick, interval, reasons), the
//              strategic posture vector, the decision/command counters, the
//              per-country AI control bitmap and the player country
//   Rng        the deterministic reproduction context: game seed, world seed, world
//              tick/date, ticks_run, start date, every RNG stream state and the
//              not-yet-applied command queue (the remaining input that decides the
//              next tick)
//
// The file header additionally repeats the seed, scenario path, tick, date, ticks_run,
// start date, player country and AI control bitmap. Sections are authoritative (only
// they are hashed) and load_game rejects a header that disagrees with them; the copy
// exists so a tool can describe a file without parsing sections.
//
// Deliberately not saved because they are not gameplay state: SimMetrics
// (wall-clock timings), SimEvents (UI history), the command log (appended as its own
// file trailer so a replay is extractable without touching sections) and
// Game::data_root (a load path, not state).

#include "save/save.h"

#include <cassert>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/hash.h"
#include "game/game.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/units.h"
#include "sim/world.h"

namespace hoi {
namespace {

// Magic of the replay container; distinct from SAVE_MAGIC so that loading the wrong
// kind of file is reported instead of parsed.
constexpr uint32_t REPLAY_MAGIC = 0x50455248u;  // "HREP"

constexpr int MOD_COUNT = static_cast<int>(ModifierKind::Count);
constexpr int AI_LAYER_COUNT = static_cast<int>(AiLayer::Count);
constexpr int RNG_STREAM_COUNT = static_cast<int>(RngStream::Count);

bool fail(std::string* err, const std::string& msg) {
    if (err != nullptr) *err = msg;
    return false;
}

std::string hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

uint64_t hash_bytes(const std::vector<uint8_t>& b) {
    return b.empty() ? FNV_OFFSET : fnv1a(b.data(), b.size());
}

// --------------------------------------------------------------- primitives --

// Counts coming from a file are untrusted: reject any count that cannot possibly fit
// in the bytes that remain before allocating for it, so a truncated or corrupt file
// fails cleanly instead of exhausting memory.
bool read_count(ByteReader& r, uint32_t min_elem_bytes, uint32_t* out) {
    uint32_t n = 0;
    if (!r.u32(&n)) return false;
    if (min_elem_bytes != 0 && n > r.remaining() / min_elem_bytes) return false;
    *out = n;
    return true;
}

void write_date(ByteWriter& w, const GameDate& d) {
    w.i32(d.year);
    w.u8(d.month);
    w.u8(d.day);
    w.u8(d.hour);
}

bool read_date(ByteReader& r, GameDate* d) {
    int32_t year = 0;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    if (!r.i32(&year) || !r.u8(&month) || !r.u8(&day) || !r.u8(&hour)) return false;
    d->year = year;
    d->month = month;
    d->day = day;
    d->hour = hour;
    return true;
}

template <typename E>
void write_enum(ByteWriter& w, E e) {
    w.u8(static_cast<uint8_t>(e));
}

template <typename E>
bool read_enum(ByteReader& r, E* out, int count) {
    uint8_t v = 0;
    if (!r.u8(&v)) return false;
    if (static_cast<int>(v) >= count) return false;
    *out = static_cast<E>(v);
    return true;
}

template <typename T>
void write_id(ByteWriter& w, Id<T> id) {
    w.u32(id.v);
}

// Id lists are written as a length followed by raw u32 ids so that the payload does
// not depend on the pointer size of the reading process.
template <typename T>
void write_ids(ByteWriter& w, const std::vector<Id<T>>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (Id<T> id : v) w.u32(id.v);
}

template <typename T>
bool read_ids(ByteReader& r, std::vector<Id<T>>* out) {
    uint32_t n = 0;
    if (!read_count(r, 4, &n)) return false;
    out->clear();
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t raw = INVALID_ID;
        if (!r.u32(&raw)) return false;
        out->emplace_back(raw);
    }
    return true;
}

void write_f64s(ByteWriter& w, const std::vector<double>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (double e : v) w.f64(e);
}

bool read_f64s(ByteReader& r, std::vector<double>* out) {
    uint32_t n = 0;
    if (!read_count(r, 8, &n)) return false;
    out->clear();
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        double v = 0.0;
        if (!r.f64(&v)) return false;
        out->push_back(v);
    }
    return true;
}

void write_bytes(ByteWriter& w, const std::vector<uint8_t>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    if (!v.empty()) w.raw(v.data(), v.size());
}

bool read_bytes(ByteReader& r, std::vector<uint8_t>* out) {
    uint32_t n = 0;
    if (!read_count(r, 1, &n)) return false;
    out->assign(n, 0);
    if (n != 0 && !r.raw(out->data(), n)) return false;
    return true;
}

void write_modifiers(ByteWriter& w, const Modifiers& m) {
    for (int i = 0; i < MOD_COUNT; ++i) w.f64(m.v[i]);
}

bool read_modifiers(ByteReader& r, Modifiers* m) {
    for (int i = 0; i < MOD_COUNT; ++i) {
        if (!r.f64(&m->v[i])) return false;
    }
    return true;
}

void write_resources(ByteWriter& w, const double v[RESOURCE_COUNT]) {
    for (int i = 0; i < RESOURCE_COUNT; ++i) w.f64(v[i]);
}

bool read_resources(ByteReader& r, double v[RESOURCE_COUNT]) {
    for (int i = 0; i < RESOURCE_COUNT; ++i) {
        if (!r.f64(&v[i])) return false;
    }
    return true;
}

// Stores are written as (slot count, one alive flag byte per slot, free slot list,
// then every alive element in ascending slot order). Slot indices are preserved
// exactly, so entity ids survive a save/load round trip, and the free list is kept so
// that ids handed out *after* a load match the ids the saved run would have handed
// out.
template <typename T, typename F>
void write_store(ByteWriter& w, const Store<T>& st, F&& write_one) {
    const std::vector<uint8_t>& flags = st.alive_flags();
    w.u32(static_cast<uint32_t>(flags.size()));
    if (!flags.empty()) w.raw(flags.data(), flags.size());
    const std::vector<uint32_t>& free_list = st.raw_free_list();
    w.u32(static_cast<uint32_t>(free_list.size()));
    for (uint32_t slot : free_list) w.u32(slot);
    st.for_each([&](auto, const T& e) { write_one(w, e); });
}

template <typename T, typename F>
bool read_store(ByteReader& r, Store<T>& st, F&& read_one) {
    uint32_t slot_count = 0;
    if (!read_count(r, 1, &slot_count)) return false;  // one flag byte per slot
    std::vector<uint8_t> flags(slot_count, 0);
    if (slot_count != 0 && !r.raw(flags.data(), slot_count)) return false;

    uint32_t free_count = 0;
    if (!read_count(r, 4, &free_count)) return false;
    std::vector<uint32_t> free_list;
    free_list.reserve(free_count);
    for (uint32_t i = 0; i < free_count; ++i) {
        uint32_t slot = 0;
        if (!r.u32(&slot)) return false;
        // A free slot must exist in the store and must not also be alive.
        if (slot >= slot_count || flags[slot] != 0) return false;
        free_list.push_back(slot);
    }

    st.clear();
    st.raw_items().resize(slot_count);
    for (uint32_t slot = 0; slot < slot_count; ++slot) {
        if (flags[slot] == 0) continue;
        if (!read_one(r, &st.raw_items()[slot])) return false;
    }
    for (uint8_t& f : flags) f = f != 0 ? 1 : 0;
    st.raw_free_list() = std::move(free_list);
    st.set_alive_flags(std::move(flags));
    return r.ok();
}

// ------------------------------------------------------------------- world ---

void write_province(ByteWriter& w, const Province& p) {
    write_id(w, p.id);
    w.str(p.name);
    write_id(w, p.state);
    write_id(w, p.region);
    write_enum(w, p.terrain);
    write_id(w, p.owner);
    write_id(w, p.controller);
    write_ids(w, p.adj);
    write_ids(w, p.sea_adj);
    w.boolean(p.coastal);
    w.boolean(p.is_sea);
    w.i32(p.victory_points);
    w.i32(p.infrastructure);
    w.i32(p.fort_level);
    w.i32(p.air_base);
    w.i32(p.naval_base);
    w.i32(p.radar);
    w.boolean(p.supply_hub);
    w.i32(p.railway_level);
    w.f64(p.population);
    write_resources(w, p.resource_yield);
    w.boolean(p.is_capital);
    w.f64(p.supply_level);
    write_id(w, p.supply_source);
    write_id(w, p.supply_bottleneck);
    w.i32(p.x);
    w.i32(p.y);
}

bool read_province(ByteReader& r, Province* p) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    p->id = ProvinceId(id);
    if (!r.str(&p->name)) return false;
    uint32_t state = INVALID_ID;
    uint32_t region = INVALID_ID;
    if (!r.u32(&state) || !r.u32(&region)) return false;
    p->state = StateId(state);
    p->region = RegionId(region);
    if (!read_enum(r, &p->terrain, static_cast<int>(Terrain::Count))) return false;
    uint32_t owner = INVALID_ID;
    uint32_t controller = INVALID_ID;
    if (!r.u32(&owner) || !r.u32(&controller)) return false;
    p->owner = CountryId(owner);
    p->controller = CountryId(controller);
    if (!read_ids(r, &p->adj)) return false;
    if (!read_ids(r, &p->sea_adj)) return false;
    if (!r.boolean(&p->coastal) || !r.boolean(&p->is_sea)) return false;
    if (!r.i32(&p->victory_points) || !r.i32(&p->infrastructure) || !r.i32(&p->fort_level)) return false;
    if (!r.i32(&p->air_base) || !r.i32(&p->naval_base) || !r.i32(&p->radar)) return false;
    if (!r.boolean(&p->supply_hub) || !r.i32(&p->railway_level)) return false;
    if (!r.f64(&p->population)) return false;
    if (!read_resources(r, p->resource_yield)) return false;
    if (!r.boolean(&p->is_capital)) return false;
    if (!r.f64(&p->supply_level)) return false;
    uint32_t source = INVALID_ID;
    uint32_t bottleneck = INVALID_ID;
    if (!r.u32(&source) || !r.u32(&bottleneck)) return false;
    p->supply_source = ProvinceId(source);
    p->supply_bottleneck = ProvinceId(bottleneck);
    if (!r.i32(&p->x) || !r.i32(&p->y)) return false;
    return true;
}

void write_state(ByteWriter& w, const State& s) {
    write_id(w, s.id);
    w.str(s.name);
    write_id(w, s.owner);
    write_id(w, s.controller);
    write_id(w, s.region);
    write_ids(w, s.provinces);
    write_ids(w, s.core_owners);
    w.i32(s.civilian_factories);
    w.i32(s.military_factories);
    w.i32(s.dockyards);
    w.i32(s.synthetic_refineries);
    w.i32(s.building_slots);
    w.f64(s.manpower_pool);
    w.boolean(s.impassable);
    w.f64(s.resistance);
    w.f64(s.compliance);
    w.f64(s.garrison_required);
}

bool read_state(ByteReader& r, State* s) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    s->id = StateId(id);
    if (!r.str(&s->name)) return false;
    uint32_t owner = INVALID_ID;
    uint32_t controller = INVALID_ID;
    uint32_t region = INVALID_ID;
    if (!r.u32(&owner) || !r.u32(&controller) || !r.u32(&region)) return false;
    s->owner = CountryId(owner);
    s->controller = CountryId(controller);
    s->region = RegionId(region);
    if (!read_ids(r, &s->provinces)) return false;
    if (!read_ids(r, &s->core_owners)) return false;
    if (!r.i32(&s->civilian_factories) || !r.i32(&s->military_factories) || !r.i32(&s->dockyards)) return false;
    if (!r.i32(&s->synthetic_refineries)) return false;
    if (!r.i32(&s->building_slots)) return false;
    if (!r.f64(&s->manpower_pool)) return false;
    if (!r.boolean(&s->impassable)) return false;
    if (!r.f64(&s->resistance) || !r.f64(&s->compliance) || !r.f64(&s->garrison_required)) return false;
    return true;
}

void write_region(ByteWriter& w, const Region& r) {
    write_id(w, r.id);
    w.str(r.name);
    w.boolean(r.is_sea);
    write_ids(w, r.provinces);
    w.f64(r.temperature);
    w.boolean(r.rain);
    w.boolean(r.snow);
    w.boolean(r.mud);
    w.boolean(r.sandstorm);
}

bool read_region(ByteReader& r, Region* g) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    g->id = RegionId(id);
    if (!r.str(&g->name)) return false;
    if (!r.boolean(&g->is_sea)) return false;
    if (!read_ids(r, &g->provinces)) return false;
    if (!r.f64(&g->temperature)) return false;
    if (!r.boolean(&g->rain) || !r.boolean(&g->snow) || !r.boolean(&g->mud) || !r.boolean(&g->sandstorm)) return false;
    return true;
}

// ---------------------------------------------------------------- content ----
//
// The content database (equipment statistics, technologies, laws, buildings and
// SimConstants) is read by every simulation phase, so a save that omitted it would
// continue differently from the run it came from: a save is self-contained and a
// default-constructed Game can continue from it. It travels inside the Economy
// section and is therefore part of the world hash. The by-key index maps are derived
// state, rebuilt from the vectors on load rather than stored twice.

// Division templates are shared with the per-country template rosters below.
void write_template(ByteWriter& w, const DivisionTemplate& t);
bool read_template(ByteReader& r, DivisionTemplate* t);

void write_strs(ByteWriter& w, const std::vector<std::string>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const std::string& s : v) w.str(s);
}

bool read_strs(ByteReader& r, std::vector<std::string>* out) {
    uint32_t n = 0;
    if (!read_count(r, 4, &n)) return false;
    out->clear();
    out->resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.str(&(*out)[i])) return false;
    }
    return true;
}

void write_equipment(ByteWriter& w, const EquipmentDef& e) {
    write_id(w, e.id);
    w.str(e.key);
    w.str(e.name);
    write_enum(w, e.category);
    w.i32(e.year);
    w.str(e.archetype);
    w.f64(e.soft_attack);
    w.f64(e.hard_attack);
    w.f64(e.air_attack);
    w.f64(e.defense);
    w.f64(e.breakthrough);
    w.f64(e.armor);
    w.f64(e.piercing);
    w.f64(e.hardness);
    w.f64(e.reliability);
    w.f64(e.speed);
    w.f64(e.max_strength);
    w.f64(e.organization);
    w.f64(e.build_cost);
    write_resources(w, e.resources);
    w.f64(e.fuel_use);
    w.f64(e.supply_use);
    w.f64(e.manpower);
    w.boolean(e.is_archetype);
}

bool read_equipment(ByteReader& r, EquipmentDef* e) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    e->id = EquipmentId(id);
    if (!r.str(&e->key) || !r.str(&e->name)) return false;
    if (!read_enum(r, &e->category, static_cast<int>(EquipmentCategory::Count))) return false;
    if (!r.i32(&e->year)) return false;
    if (!r.str(&e->archetype)) return false;
    if (!r.f64(&e->soft_attack) || !r.f64(&e->hard_attack) || !r.f64(&e->air_attack)) return false;
    if (!r.f64(&e->defense) || !r.f64(&e->breakthrough) || !r.f64(&e->armor)) return false;
    if (!r.f64(&e->piercing) || !r.f64(&e->hardness) || !r.f64(&e->reliability)) return false;
    if (!r.f64(&e->speed) || !r.f64(&e->max_strength) || !r.f64(&e->organization)) return false;
    if (!r.f64(&e->build_cost)) return false;
    if (!read_resources(r, e->resources)) return false;
    if (!r.f64(&e->fuel_use) || !r.f64(&e->supply_use) || !r.f64(&e->manpower)) return false;
    return r.boolean(&e->is_archetype);
}

void write_tech(ByteWriter& w, const TechDef& t) {
    write_id(w, t.id);
    w.str(t.key);
    w.str(t.name);
    w.str(t.category);
    w.i32(t.year);
    w.f64(t.cost_days);
    write_ids(w, t.prerequisites);
    write_strs(w, t.unlock_equipment);
    write_strs(w, t.unlock_buildings);
    write_modifiers(w, t.modifiers);
}

bool read_tech(ByteReader& r, TechDef* t) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    t->id = TechId(id);
    if (!r.str(&t->key) || !r.str(&t->name) || !r.str(&t->category)) return false;
    if (!r.i32(&t->year) || !r.f64(&t->cost_days)) return false;
    if (!read_ids(r, &t->prerequisites)) return false;
    if (!read_strs(r, &t->unlock_equipment)) return false;
    if (!read_strs(r, &t->unlock_buildings)) return false;
    return read_modifiers(r, &t->modifiers);
}

void write_law(ByteWriter& w, const LawDef& l) {
    w.str(l.key);
    w.str(l.name);
    w.i32(l.kind);
    w.i32(l.level);
    w.f64(l.cost);
    write_modifiers(w, l.modifiers);
    w.str(l.requires_law);
    w.i32(l.requires_level);
}

bool read_law(ByteReader& r, LawDef* l) {
    if (!r.str(&l->key) || !r.str(&l->name)) return false;
    if (!r.i32(&l->kind) || !r.i32(&l->level) || !r.f64(&l->cost)) return false;
    if (!read_modifiers(r, &l->modifiers)) return false;
    if (!r.str(&l->requires_law)) return false;
    return r.i32(&l->requires_level);
}

void write_building(ByteWriter& w, const BuildingDef& b) {
    write_enum(w, b.kind);
    w.str(b.key);
    w.str(b.name);
    w.f64(b.base_cost);
    w.boolean(b.per_state);
    w.i32(b.max_level);
}

bool read_building(ByteReader& r, BuildingDef* b) {
    if (!read_enum(r, &b->kind, static_cast<int>(BuildingKind::Count))) return false;
    if (!r.str(&b->key) || !r.str(&b->name)) return false;
    if (!r.f64(&b->base_cost)) return false;
    if (!r.boolean(&b->per_state)) return false;
    return r.i32(&b->max_level);
}

// SimConstants in declaration order; every balance number the simulation reads is
// part of the state, so a data change cannot slip past a hash comparison.
void write_constants(ByteWriter& w, const SimConstants& k) {
    const double values[] = {
        k.ic_per_military_factory, k.ic_per_civilian_factory, k.ic_per_dockyard,
        k.efficiency_start, k.efficiency_cap_base, k.efficiency_cap_growth_per_day,
        k.efficiency_growth_per_day, k.switch_same_archetype_retention, k.resource_shortage_floor,
        k.consumer_goods_base, k.fuel_per_oil, k.fuel_storage_per_factory,
        k.synthetic_oil_per_refinery_per_day, k.synthetic_rubber_per_refinery_per_day,
        k.construction_cost_factory, k.construction_cost_infrastructure, k.construction_cost_railway,
        k.construction_cost_supply_hub, k.construction_cost_air_base, k.construction_cost_naval_base,
        k.construction_cost_fort, k.construction_cost_radar, k.construction_cost_synthetic,
        k.construction_level_scaling, k.research_base_days, k.research_year_penalty,
        k.research_speed_base, k.manpower_growth_per_year_fraction, k.recruitable_base,
        k.base_hours_per_province, k.min_division_speed, k.river_crossing_penalty,
        k.combat_width_base, k.damage_scale, k.org_damage_share, k.strength_damage_share,
        k.armor_advantage_multiplier, k.armor_disadvantage_multiplier, k.org_recovery_base,
        k.entrenchment_per_day, k.planning_per_day, k.planning_max_attack_bonus,
        k.battle_retreat_org_threshold, k.max_battles_per_province, k.supply_hub_radius,
        k.supply_range_penalty, k.supply_demand_per_width, k.supply_rail_bonus_per_level,
        k.supply_infrastructure_bonus_per_level, k.fuel_demand_per_day, k.political_power_per_day,
        k.stability_drift, k.war_support_drift, k.weather_change_chance};
    w.u32(static_cast<uint32_t>(sizeof(values) / sizeof(values[0])));
    for (double v : values) w.f64(v);
}

bool read_constants(ByteReader& r, SimConstants* k) {
    uint32_t n = 0;
    if (!read_count(r, 8, &n)) return false;
    if (n != 54) return false;  // a different count means a different SimConstants layout
    double v[54] = {0.0};
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.f64(&v[i])) return false;
    }
    k->ic_per_military_factory = v[0];
    k->ic_per_civilian_factory = v[1];
    k->ic_per_dockyard = v[2];
    k->efficiency_start = v[3];
    k->efficiency_cap_base = v[4];
    k->efficiency_cap_growth_per_day = v[5];
    k->efficiency_growth_per_day = v[6];
    k->switch_same_archetype_retention = v[7];
    k->resource_shortage_floor = v[8];
    k->consumer_goods_base = v[9];
    k->fuel_per_oil = v[10];
    k->fuel_storage_per_factory = v[11];
    k->synthetic_oil_per_refinery_per_day = v[12];
    k->synthetic_rubber_per_refinery_per_day = v[13];
    k->construction_cost_factory = v[14];
    k->construction_cost_infrastructure = v[15];
    k->construction_cost_railway = v[16];
    k->construction_cost_supply_hub = v[17];
    k->construction_cost_air_base = v[18];
    k->construction_cost_naval_base = v[19];
    k->construction_cost_fort = v[20];
    k->construction_cost_radar = v[21];
    k->construction_cost_synthetic = v[22];
    k->construction_level_scaling = v[23];
    k->research_base_days = v[24];
    k->research_year_penalty = v[25];
    k->research_speed_base = v[26];
    k->manpower_growth_per_year_fraction = v[27];
    k->recruitable_base = v[28];
    k->base_hours_per_province = v[29];
    k->min_division_speed = v[30];
    k->river_crossing_penalty = v[31];
    k->combat_width_base = v[32];
    k->damage_scale = v[33];
    k->org_damage_share = v[34];
    k->strength_damage_share = v[35];
    k->armor_advantage_multiplier = v[36];
    k->armor_disadvantage_multiplier = v[37];
    k->org_recovery_base = v[38];
    k->entrenchment_per_day = v[39];
    k->planning_per_day = v[40];
    k->planning_max_attack_bonus = v[41];
    k->battle_retreat_org_threshold = v[42];
    k->max_battles_per_province = v[43];
    k->supply_hub_radius = v[44];
    k->supply_range_penalty = v[45];
    k->supply_demand_per_width = v[46];
    k->supply_rail_bonus_per_level = v[47];
    k->supply_infrastructure_bonus_per_level = v[48];
    k->fuel_demand_per_day = v[49];
    k->political_power_per_day = v[50];
    k->stability_drift = v[51];
    k->war_support_drift = v[52];
    k->weather_change_chance = v[53];
    return true;
}

void write_content(ByteWriter& w, const Content& c) {
    w.u32(static_cast<uint32_t>(c.equipment.size()));
    for (const EquipmentDef& e : c.equipment) write_equipment(w, e);
    w.u32(static_cast<uint32_t>(c.templates.size()));
    for (const DivisionTemplate& t : c.templates) write_template(w, t);
    w.u32(static_cast<uint32_t>(c.techs.size()));
    for (const TechDef& t : c.techs) write_tech(w, t);
    w.u32(static_cast<uint32_t>(c.laws.size()));
    for (const LawDef& l : c.laws) write_law(w, l);
    w.u32(static_cast<uint32_t>(c.buildings.size()));
    for (const BuildingDef& b : c.buildings) write_building(w, b);
    write_constants(w, c.constants);
}

// Rebuilds the derived key -> id maps so the loaded database behaves exactly like a
// designed one: commands and the AI look definitions up by key.
void rebuild_content_index(Content& c) {
    c.equipment_by_key.clear();
    for (const EquipmentDef& e : c.equipment) c.equipment_by_key[e.key] = e.id;
    c.template_by_key.clear();
    for (const DivisionTemplate& t : c.templates) c.template_by_key[t.key] = t.id;
    c.tech_by_key.clear();
    for (const TechDef& t : c.techs) c.tech_by_key[t.key] = t.id;
    c.law_index.clear();
    for (size_t i = 0; i < c.laws.size(); ++i) c.law_index[c.laws[i].key] = static_cast<int>(i);
    c.load_errors.clear();
}

bool read_content(ByteReader& r, Content* c) {
    uint32_t n = 0;
    if (!read_count(r, 28, &n)) return false;
    c->equipment.clear();
    c->equipment.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_equipment(r, &c->equipment[i])) return false;
    }
    if (!read_count(r, 28, &n)) return false;
    c->templates.clear();
    c->templates.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_template(r, &c->templates[i])) return false;
    }
    if (!read_count(r, 24, &n)) return false;
    c->techs.clear();
    c->techs.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_tech(r, &c->techs[i])) return false;
    }
    if (!read_count(r, 32, &n)) return false;
    c->laws.clear();
    c->laws.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_law(r, &c->laws[i])) return false;
    }
    if (!read_count(r, 24, &n)) return false;
    c->buildings.clear();
    c->buildings.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_building(r, &c->buildings[i])) return false;
    }
    if (!read_constants(r, &c->constants)) return false;
    rebuild_content_index(*c);
    return true;
}

// ----------------------------------------------------------------- economy ---

void write_line(ByteWriter& w, const ProductionLine& l) {
    write_id(w, l.equipment);
    w.i32(l.factories);
    w.f64(l.efficiency);
    w.f64(l.efficiency_cap);
    w.f64(l.output_today);
    w.f64(l.output_total);
    w.f64(l.resource_shortage);
    w.u64(l.started);
    write_ids(w, l.previous);
}

bool read_line(ByteReader& r, ProductionLine* l) {
    uint32_t equipment = INVALID_ID;
    if (!r.u32(&equipment)) return false;
    l->equipment = EquipmentId(equipment);
    if (!r.i32(&l->factories)) return false;
    if (!r.f64(&l->efficiency) || !r.f64(&l->efficiency_cap)) return false;
    if (!r.f64(&l->output_today) || !r.f64(&l->output_total)) return false;
    if (!r.f64(&l->resource_shortage)) return false;
    if (!r.u64(&l->started)) return false;
    return read_ids(r, &l->previous);
}

void write_project(ByteWriter& w, const ConstructionProject& p) {
    write_enum(w, p.kind);
    write_id(w, p.province);
    write_id(w, p.state);
    w.i32(p.target_level);
    w.f64(p.progress);
    w.f64(p.cost);
    w.boolean(p.repair);
}

bool read_project(ByteReader& r, ConstructionProject* p) {
    if (!read_enum(r, &p->kind, static_cast<int>(BuildingKind::Count))) return false;
    uint32_t province = INVALID_ID;
    uint32_t state = INVALID_ID;
    if (!r.u32(&province) || !r.u32(&state)) return false;
    p->province = ProvinceId(province);
    p->state = StateId(state);
    if (!r.i32(&p->target_level)) return false;
    if (!r.f64(&p->progress) || !r.f64(&p->cost)) return false;
    return r.boolean(&p->repair);
}

void write_construction(ByteWriter& w, const ConstructionState& c) {
    w.i32(c.max_queue_size);
    w.u32(static_cast<uint32_t>(c.queue.size()));
    for (const ConstructionProject& p : c.queue) write_project(w, p);
}

bool read_construction(ByteReader& r, ConstructionState* c) {
    if (!r.i32(&c->max_queue_size)) return false;
    uint32_t n = 0;
    if (!read_count(r, 8, &n)) return false;
    c->queue.clear();
    c->queue.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_project(r, &c->queue[i])) return false;
    }
    return true;
}

void write_research(ByteWriter& w, const ResearchState& s) {
    write_ids(w, s.completed);
    w.u32(static_cast<uint32_t>(s.slots.size()));
    for (const ResearchSlot& slot : s.slots) {
        write_id(w, slot.tech);
        w.f64(slot.progress);
        w.boolean(slot.active);
    }
    w.i32(s.slots_unlocked);
}

bool read_research(ByteReader& r, ResearchState* s) {
    if (!read_ids(r, &s->completed)) return false;
    uint32_t n = 0;
    if (!read_count(r, 13, &n)) return false;  // tech id + progress + active
    s->slots.clear();
    s->slots.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t tech = INVALID_ID;
        if (!r.u32(&tech)) return false;
        s->slots[i].tech = TechId(tech);
        if (!r.f64(&s->slots[i].progress)) return false;
        if (!r.boolean(&s->slots[i].active)) return false;
    }
    return r.i32(&s->slots_unlocked);
}

void write_template(ByteWriter& w, const DivisionTemplate& t) {
    write_id(w, t.id);
    w.str(t.key);
    w.str(t.name);
    write_id(w, t.country);
    w.u32(static_cast<uint32_t>(t.battalions.size()));
    for (const BattalionSlot& b : t.battalions) {
        write_id(w, b.equipment);
        w.i32(b.count);
        w.boolean(b.support);
    }
    w.f64(t.combat_width);
    w.f64(t.max_organization);
    w.f64(t.max_strength);
    w.f64(t.soft_attack);
    w.f64(t.hard_attack);
    w.f64(t.defense);
    w.f64(t.breakthrough);
    w.f64(t.armor);
    w.f64(t.piercing);
    w.f64(t.hardness);
    w.f64(t.speed);
    w.f64(t.supply_use);
    w.f64(t.fuel_use);
    w.f64(t.manpower);
    w.f64(t.build_cost);
    w.f64(t.train_days);
}

bool read_template(ByteReader& r, DivisionTemplate* t) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    t->id = TemplateId(id);
    if (!r.str(&t->key) || !r.str(&t->name)) return false;
    uint32_t country = INVALID_ID;
    if (!r.u32(&country)) return false;
    t->country = CountryId(country);
    uint32_t n = 0;
    if (!read_count(r, 9, &n)) return false;
    t->battalions.clear();
    t->battalions.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t equipment = INVALID_ID;
        if (!r.u32(&equipment)) return false;
        t->battalions[i].equipment = EquipmentId(equipment);
        if (!r.i32(&t->battalions[i].count)) return false;
        if (!r.boolean(&t->battalions[i].support)) return false;
    }
    if (!r.f64(&t->combat_width) || !r.f64(&t->max_organization) || !r.f64(&t->max_strength)) return false;
    if (!r.f64(&t->soft_attack) || !r.f64(&t->hard_attack) || !r.f64(&t->defense)) return false;
    if (!r.f64(&t->breakthrough) || !r.f64(&t->armor) || !r.f64(&t->piercing)) return false;
    if (!r.f64(&t->hardness) || !r.f64(&t->speed) || !r.f64(&t->supply_use)) return false;
    if (!r.f64(&t->fuel_use) || !r.f64(&t->manpower) || !r.f64(&t->build_cost)) return false;
    return r.f64(&t->train_days);
}

// ---------------------------------------------------------------- military ---

void write_division(ByteWriter& w, const Division& d) {
    write_id(w, d.id);
    write_id(w, d.country);
    write_id(w, d.template_id);
    w.str(d.name);
    write_id(w, d.location);
    write_id(w, d.previous_location);
    w.f64(d.organization);
    w.f64(d.max_organization);
    w.f64(d.strength);
    w.f64(d.experience);
    w.f64(d.entrenchment);
    w.f64(d.planning);
    w.f64(d.supply);
    w.f64(d.fuel);
    w.f64(d.manpower);
    write_f64s(w, d.equipment);
    write_id(w, d.army);
    write_enum(w, d.order);
    write_id(w, d.order_target);
    w.boolean(d.moving);
    write_id(w, d.move_from);
    write_id(w, d.move_to);
    w.f64(d.move_progress);
    write_ids(w, d.path);
    w.boolean(d.retreating);
    write_id(w, d.battle);
    w.f64(d.combat_attack_modifier);
    w.f64(d.training_days_left);
    w.f64(d.losses_manpower);
    w.f64(d.losses_equipment);
    w.u64(d.created_tick);
}

bool read_division(ByteReader& r, Division* d) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    uint32_t tmpl = INVALID_ID;
    if (!r.u32(&id)) return false;
    d->id = DivisionId(id);
    if (!r.u32(&country)) return false;
    d->country = CountryId(country);
    if (!r.u32(&tmpl)) return false;
    d->template_id = TemplateId(tmpl);
    if (!r.str(&d->name)) return false;
    uint32_t location = INVALID_ID;
    uint32_t previous = INVALID_ID;
    if (!r.u32(&location) || !r.u32(&previous)) return false;
    d->location = ProvinceId(location);
    d->previous_location = ProvinceId(previous);
    if (!r.f64(&d->organization) || !r.f64(&d->max_organization) || !r.f64(&d->strength)) return false;
    if (!r.f64(&d->experience) || !r.f64(&d->entrenchment) || !r.f64(&d->planning)) return false;
    if (!r.f64(&d->supply) || !r.f64(&d->fuel) || !r.f64(&d->manpower)) return false;
    if (!read_f64s(r, &d->equipment)) return false;
    uint32_t army = INVALID_ID;
    if (!r.u32(&army)) return false;
    d->army = ArmyId(army);
    if (!read_enum(r, &d->order, static_cast<int>(OrderKind::Count))) return false;
    uint32_t target = INVALID_ID;
    if (!r.u32(&target)) return false;
    d->order_target = ProvinceId(target);
    if (!r.boolean(&d->moving)) return false;
    uint32_t from = INVALID_ID;
    uint32_t to = INVALID_ID;
    if (!r.u32(&from) || !r.u32(&to)) return false;
    d->move_from = ProvinceId(from);
    d->move_to = ProvinceId(to);
    if (!r.f64(&d->move_progress)) return false;
    if (!read_ids(r, &d->path)) return false;
    if (!r.boolean(&d->retreating)) return false;
    uint32_t battle = INVALID_ID;
    if (!r.u32(&battle)) return false;
    d->battle = BattleId(battle);
    if (!r.f64(&d->combat_attack_modifier)) return false;
    if (!r.f64(&d->training_days_left)) return false;
    if (!r.f64(&d->losses_manpower) || !r.f64(&d->losses_equipment)) return false;
    return r.u64(&d->created_tick);
}

void write_army(ByteWriter& w, const Army& a) {
    write_id(w, a.id);
    write_id(w, a.country);
    w.str(a.name);
    write_id(w, a.general);
    write_ids(w, a.divisions);
    write_enum(w, a.order.kind);
    write_ids(w, a.order.line);
    write_ids(w, a.order.target_line);
    w.f64(a.order.progress);
    w.u64(a.order.started);
    w.u8(a.stance);
    w.i32(a.motorization);
}

bool read_army(ByteReader& r, Army* a) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    if (!r.u32(&id)) return false;
    a->id = ArmyId(id);
    if (!r.u32(&country)) return false;
    a->country = CountryId(country);
    if (!r.str(&a->name)) return false;
    uint32_t general = INVALID_ID;
    if (!r.u32(&general)) return false;
    a->general = CharacterId(general);
    if (!read_ids(r, &a->divisions)) return false;
    if (!read_enum(r, &a->order.kind, static_cast<int>(OrderKind::Count))) return false;
    if (!read_ids(r, &a->order.line)) return false;
    if (!read_ids(r, &a->order.target_line)) return false;
    if (!r.f64(&a->order.progress)) return false;
    if (!r.u64(&a->order.started)) return false;
    if (!r.u8(&a->stance)) return false;
    return r.i32(&a->motorization);
}

void write_training(ByteWriter& w, const TrainingDivision& t) {
    write_id(w, t.division);
    write_id(w, t.template_id);
    w.f64(t.days_left);
}

bool read_training(ByteReader& r, TrainingDivision* t) {
    uint32_t division = INVALID_ID;
    uint32_t tmpl = INVALID_ID;
    if (!r.u32(&division) || !r.u32(&tmpl)) return false;
    t->division = DivisionId(division);
    t->template_id = TemplateId(tmpl);
    return r.f64(&t->days_left);
}

// ----------------------------------------------------------------- battles ---

void write_side(ByteWriter& w, const BattleSideState& s) {
    write_ids(w, s.divisions);
    write_f64s(w, s.width_used);
    w.f64(s.total_soft_attack);
    w.f64(s.total_hard_attack);
    w.f64(s.total_defense);
    w.f64(s.total_breakthrough);
    w.f64(s.total_armor);
    w.f64(s.total_piercing);
}

bool read_side(ByteReader& r, BattleSideState* s) {
    if (!read_ids(r, &s->divisions)) return false;
    if (!read_f64s(r, &s->width_used)) return false;
    if (!r.f64(&s->total_soft_attack) || !r.f64(&s->total_hard_attack)) return false;
    if (!r.f64(&s->total_defense) || !r.f64(&s->total_breakthrough)) return false;
    if (!r.f64(&s->total_armor) || !r.f64(&s->total_piercing)) return false;
    return true;
}

void write_debug_line(ByteWriter& w, const BattleDebugLine& d) {
    write_id(w, d.division);
    w.f64(d.base_attack);
    w.f64(d.planning_mod);
    w.f64(d.terrain_mod);
    w.f64(d.supply_mod);
    w.f64(d.commander_mod);
    w.f64(d.experience_mod);
    w.f64(d.final_attack);
    w.f64(d.enemy_defense);
    w.f64(d.damage);
    w.f64(d.org_damage);
    w.f64(d.strength_damage);
}

bool read_debug_line(ByteReader& r, BattleDebugLine* d) {
    uint32_t division = INVALID_ID;
    if (!r.u32(&division)) return false;
    d->division = DivisionId(division);
    if (!r.f64(&d->base_attack) || !r.f64(&d->planning_mod) || !r.f64(&d->terrain_mod)) return false;
    if (!r.f64(&d->supply_mod) || !r.f64(&d->commander_mod) || !r.f64(&d->experience_mod)) return false;
    if (!r.f64(&d->final_attack) || !r.f64(&d->enemy_defense) || !r.f64(&d->damage)) return false;
    if (!r.f64(&d->org_damage) || !r.f64(&d->strength_damage)) return false;
    return true;
}

void write_battle(ByteWriter& w, const Battle& b) {
    write_id(w, b.id);
    write_id(w, b.province);
    w.u64(b.start_tick);
    write_side(w, b.attacker);
    write_side(w, b.defender);
    w.f64(b.progress);
    write_enum(w, b.terrain);
    w.boolean(b.river_crossing);
    w.boolean(b.encirclement);
    write_id(w, b.attacker_lead);
    write_id(w, b.defender_lead);
    w.u32(static_cast<uint32_t>(b.debug.size()));
    for (const BattleDebugLine& d : b.debug) write_debug_line(w, d);
    w.u64(b.last_tick);
}

bool read_battle(ByteReader& r, Battle* b) {
    uint32_t id = INVALID_ID;
    uint32_t province = INVALID_ID;
    if (!r.u32(&id)) return false;
    b->id = BattleId(id);
    if (!r.u32(&province)) return false;
    b->province = ProvinceId(province);
    if (!r.u64(&b->start_tick)) return false;
    if (!read_side(r, &b->attacker)) return false;
    if (!read_side(r, &b->defender)) return false;
    if (!r.f64(&b->progress)) return false;
    if (!read_enum(r, &b->terrain, static_cast<int>(Terrain::Count))) return false;
    if (!r.boolean(&b->river_crossing) || !r.boolean(&b->encirclement)) return false;
    uint32_t attacker_lead = INVALID_ID;
    uint32_t defender_lead = INVALID_ID;
    if (!r.u32(&attacker_lead) || !r.u32(&defender_lead)) return false;
    b->attacker_lead = CountryId(attacker_lead);
    b->defender_lead = CountryId(defender_lead);
    uint32_t n = 0;
    if (!read_count(r, 92, &n)) return false;  // one debug line is 16 doubles + id
    b->debug.clear();
    b->debug.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_debug_line(r, &b->debug[i])) return false;
    }
    return r.u64(&b->last_tick);
}

// --------------------------------------------------------------- diplomacy ---

void write_war(ByteWriter& w, const War& war) {
    write_id(w, war.id);
    w.u32(static_cast<uint32_t>(war.attackers.size()));
    for (const WarParticipant& p : war.attackers) {
        write_id(w, p.country);
        w.f64(p.casualties_manpower);
        w.f64(p.casualties_equipment);
        w.f64(p.occupation_share);
    }
    w.u32(static_cast<uint32_t>(war.defenders.size()));
    for (const WarParticipant& p : war.defenders) {
        write_id(w, p.country);
        w.f64(p.casualties_manpower);
        w.f64(p.casualties_equipment);
        w.f64(p.occupation_share);
    }
    w.u32(static_cast<uint32_t>(war.goals.size()));
    for (const WarGoal& g : war.goals) {
        write_id(w, g.claimant);
        write_id(w, g.target);
        write_id(w, g.state);
        w.boolean(g.annex_country);
        w.boolean(g.puppet);
    }
    w.u64(war.start_tick);
    w.boolean(war.active);
    write_id(w, war.aggressor);
}

bool read_participant(ByteReader& r, WarParticipant* p) {
    uint32_t country = INVALID_ID;
    if (!r.u32(&country)) return false;
    p->country = CountryId(country);
    if (!r.f64(&p->casualties_manpower) || !r.f64(&p->casualties_equipment)) return false;
    return r.f64(&p->occupation_share);
}

bool read_war(ByteReader& r, War* war) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    war->id = WarId(id);
    uint32_t n = 0;
    if (!read_count(r, 28, &n)) return false;
    war->attackers.clear();
    war->attackers.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_participant(r, &war->attackers[i])) return false;
    }
    if (!read_count(r, 28, &n)) return false;
    war->defenders.clear();
    war->defenders.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_participant(r, &war->defenders[i])) return false;
    }
    if (!read_count(r, 14, &n)) return false;
    war->goals.clear();
    war->goals.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t claimant = INVALID_ID;
        uint32_t target = INVALID_ID;
        uint32_t state = INVALID_ID;
        if (!r.u32(&claimant) || !r.u32(&target) || !r.u32(&state)) return false;
        war->goals[i].claimant = CountryId(claimant);
        war->goals[i].target = CountryId(target);
        war->goals[i].state = StateId(state);
        if (!r.boolean(&war->goals[i].annex_country)) return false;
        if (!r.boolean(&war->goals[i].puppet)) return false;
    }
    if (!r.u64(&war->start_tick)) return false;
    if (!r.boolean(&war->active)) return false;
    uint32_t aggressor = INVALID_ID;
    if (!r.u32(&aggressor)) return false;
    war->aggressor = CountryId(aggressor);
    return true;
}

// ---------------------------------------------------------------- countries ---

void write_country_core(ByteWriter& w, const Country& c) {
    write_id(w, c.id);
    w.str(c.tag);
    w.str(c.name);
    w.boolean(c.alive);
    write_enum(w, c.ideology);
    write_id(w, c.overlord);
    write_ids(w, c.puppets);
    write_id(w, c.capital);
    write_f64s(w, c.equipment_stockpile);
    write_bytes(w, c.law_levels);
    write_modifiers(w, c.base_modifiers);
    write_modifiers(w, c.tech_modifiers);
    write_modifiers(w, c.law_modifiers);
    write_modifiers(w, c.national_modifiers);
    write_ids(w, c.generals);
    w.i32(c.starting_factories);
    write_resources(w, c.resources_produced);
    write_resources(w, c.resources_consumed);
    write_resources(w, c.resources_imported);
    write_resources(w, c.resources_exported);
}

bool read_country_core(ByteReader& r, Country* c) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) return false;
    c->id = CountryId(id);
    if (!r.str(&c->tag) || !r.str(&c->name)) return false;
    if (!r.boolean(&c->alive)) return false;
    if (!read_enum(r, &c->ideology, static_cast<int>(Ideology::Count))) return false;
    uint32_t overlord = INVALID_ID;
    if (!r.u32(&overlord)) return false;
    c->overlord = CountryId(overlord);
    if (!read_ids(r, &c->puppets)) return false;
    uint32_t capital = INVALID_ID;
    if (!r.u32(&capital)) return false;
    c->capital = StateId(capital);
    if (!read_f64s(r, &c->equipment_stockpile)) return false;
    if (!read_bytes(r, &c->law_levels)) return false;
    if (!read_modifiers(r, &c->base_modifiers)) return false;
    if (!read_modifiers(r, &c->tech_modifiers)) return false;
    if (!read_modifiers(r, &c->law_modifiers)) return false;
    if (!read_modifiers(r, &c->national_modifiers)) return false;
    if (!read_ids(r, &c->generals)) return false;
    if (!r.i32(&c->starting_factories)) return false;
    if (!read_resources(r, c->resources_produced)) return false;
    if (!read_resources(r, c->resources_consumed)) return false;
    if (!read_resources(r, c->resources_imported)) return false;
    return read_resources(r, c->resources_exported);
}

void write_character(ByteWriter& w, const Character& ch) {
    write_id(w, ch.id);
    write_id(w, ch.country);
    w.str(ch.name);
    w.boolean(ch.is_general);
    w.i32(ch.skill);
    w.i32(ch.attack);
    w.i32(ch.defense);
    w.i32(ch.planning);
    w.i32(ch.logistics);
    write_id(w, ch.army);
}

bool read_character(ByteReader& r, Character* ch) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    if (!r.u32(&id)) return false;
    ch->id = CharacterId(id);
    if (!r.u32(&country)) return false;
    ch->country = CountryId(country);
    if (!r.str(&ch->name)) return false;
    if (!r.boolean(&ch->is_general)) return false;
    if (!r.i32(&ch->skill) || !r.i32(&ch->attack) || !r.i32(&ch->defense)) return false;
    if (!r.i32(&ch->planning) || !r.i32(&ch->logistics)) return false;
    uint32_t army = INVALID_ID;
    if (!r.u32(&army)) return false;
    ch->army = ArmyId(army);
    return true;
}

// ------------------------------------------------------------ country parts --

void write_country_economy(ByteWriter& w, const Country& c) {
    w.u32(static_cast<uint32_t>(c.lines.size()));
    for (const ProductionLine& l : c.lines) write_line(w, l);
    write_construction(w, c.construction);
    write_research(w, c.research);
    write_ids(w, c.templates);
}

bool read_country_economy(ByteReader& r, Country* c) {
    uint32_t n = 0;
    if (!read_count(r, 40, &n)) return false;
    c->lines.clear();
    c->lines.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_line(r, &c->lines[i])) return false;
    }
    if (!read_construction(r, &c->construction)) return false;
    if (!read_research(r, &c->research)) return false;
    return read_ids(r, &c->templates);
}

void write_country_military(ByteWriter& w, const Country& c) {
    write_ids(w, c.divisions);
    write_ids(w, c.armies);
    w.u32(static_cast<uint32_t>(c.training.size()));
    for (const TrainingDivision& t : c.training) write_training(w, t);
}

bool read_country_military(ByteReader& r, Country* c) {
    if (!read_ids(r, &c->divisions)) return false;
    if (!read_ids(r, &c->armies)) return false;
    uint32_t n = 0;
    if (!read_count(r, 12, &n)) return false;  // id + division list + army list + training list
    c->training.clear();
    c->training.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_training(r, &c->training[i])) return false;
    }
    return true;
}

void write_country_diplomacy(ByteWriter& w, const Country& c) {
    write_ids(w, c.wars);
    w.u32(c.faction);
}

bool read_country_diplomacy(ByteReader& r, Country* c) {
    if (!read_ids(r, &c->wars)) return false;
    return r.u32(&c->faction);
}

void write_country_politics(ByteWriter& w, const Country& c) {
    w.f64(c.political_power);
    w.f64(c.stability);
    w.f64(c.war_support);
    w.f64(c.manpower);
    w.f64(c.fuel);
    w.f64(c.fuel_capacity);
    w.f64(c.consumer_goods_ratio);
    w.f64(c.army_experience);
    w.boolean(c.at_war);
    w.boolean(c.fuel_priority);
    w.u64(c.last_capitulation_check);
}

bool read_country_politics(ByteReader& r, Country* c) {
    if (!r.f64(&c->political_power) || !r.f64(&c->stability) || !r.f64(&c->war_support)) return false;
    if (!r.f64(&c->manpower) || !r.f64(&c->fuel) || !r.f64(&c->fuel_capacity)) return false;
    if (!r.f64(&c->consumer_goods_ratio) || !r.f64(&c->army_experience)) return false;
    if (!r.boolean(&c->at_war) || !r.boolean(&c->fuel_priority)) return false;
    return r.u64(&c->last_capitulation_check);
}

// Per-country blocks are keyed by country id so that a section can be read back
// without assuming the order sections are applied in.
void write_country_block(ByteWriter& w, CountryId id, const Country& c,
                         void (*write_part)(ByteWriter&, const Country&)) {
    w.u32(id.v);
    write_part(w, c);
}

// Returns nullptr and reports false when the id has no country in the world.
Country* country_block_target(ByteReader& r, Game& g, bool* ok) {
    uint32_t id = INVALID_ID;
    if (!r.u32(&id)) {
        *ok = false;
        return nullptr;
    }
    Country* c = g.world.country(CountryId(id));
    if (c == nullptr) *ok = false;
    return c;
}

// ---------------------------------------------------------------------- ai ---

void write_ai(ByteWriter& w, const Game& g) {
    for (int i = 0; i < AI_LAYER_COUNT; ++i) {
        const AiLayerState& l = g.ai.layers[i];
        w.u32(l.last_run_tick);
        w.u32(l.interval_ticks);
        w.u32(static_cast<uint32_t>(l.last_reasons.size()));
        for (const AiReason& reason : l.last_reasons) {
            w.str(reason.what);
            w.f64(reason.score);
            w.u32(static_cast<uint32_t>(reason.factors.size()));
            for (const std::pair<std::string, double>& f : reason.factors) {
                w.str(f.first);
                w.f64(f.second);
            }
        }
    }
    write_bytes(w, g.ai.posture);
    w.u64(g.ai.decisions_made);
    w.u64(g.ai.commands_issued);
    write_bytes(w, g.ai_controlled);
    w.u32(g.player_country.v);
}

bool read_ai(ByteReader& r, Game& g) {
    for (int i = 0; i < AI_LAYER_COUNT; ++i) {
        AiLayerState& l = g.ai.layers[i];
        if (!r.u32(&l.last_run_tick) || !r.u32(&l.interval_ticks)) return false;
        uint32_t n = 0;
        if (!read_count(r, 16, &n)) return false;  // empty reason is >= 16 bytes
        l.last_reasons.clear();
        l.last_reasons.resize(n);
        for (uint32_t k = 0; k < n; ++k) {
            AiReason& reason = l.last_reasons[k];
            if (!r.str(&reason.what) || !r.f64(&reason.score)) return false;
            uint32_t fn = 0;
            if (!read_count(r, 12, &fn)) return false;  // factor key + score
            reason.factors.clear();
            reason.factors.resize(fn);
            for (uint32_t f = 0; f < fn; ++f) {
                if (!r.str(&reason.factors[f].first) || !r.f64(&reason.factors[f].second)) return false;
            }
        }
    }
    if (!read_bytes(r, &g.ai.posture)) return false;
    if (!r.u64(&g.ai.decisions_made) || !r.u64(&g.ai.commands_issued)) return false;
    if (!read_bytes(r, &g.ai_controlled)) return false;
    uint32_t player = INVALID_ID;
    if (!r.u32(&player)) return false;
    g.player_country = CountryId(player);
    return true;
}

// --------------------------------------------------------------------- rng ---

void write_rng(ByteWriter& w, const Game& g) {
    w.u64(g.seed);
    w.u64(g.world.world_seed);
    w.u64(g.world.tick);
    write_date(w, g.world.date);
    w.u64(g.ticks_run);
    write_date(w, g.start_date);
    w.u64(g.rng.master_seed());
    for (int i = 0; i < RNG_STREAM_COUNT; ++i) {
        uint64_t state[4] = {0, 0, 0, 0};
        g.rng.get(static_cast<RngStream>(i)).serialize_state(state);
        for (int k = 0; k < 4; ++k) w.u64(state[k]);
    }
    w.u32(static_cast<uint32_t>(g.queue.pending.size()));
    for (const Command& c : g.queue.pending) serialize_command(w, c);
}

bool read_rng(ByteReader& r, Game& g) {
    if (!r.u64(&g.seed) || !r.u64(&g.world.world_seed) || !r.u64(&g.world.tick)) return false;
    if (!read_date(r, &g.world.date)) return false;
    if (!r.u64(&g.ticks_run) || !read_date(r, &g.start_date)) return false;
    uint64_t master = 0;
    if (!r.u64(&master)) return false;
    g.rng.seed(master);
    for (int i = 0; i < RNG_STREAM_COUNT; ++i) {
        uint64_t state[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            if (!r.u64(&state[k])) return false;
        }
        g.rng.get(static_cast<RngStream>(i)).deserialize_state(state);
    }
    uint32_t n = 0;
    if (!read_count(r, 4, &n)) return false;
    g.queue.pending.clear();
    g.queue.pending.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.ok()) return false;
        g.queue.pending.push_back(deserialize_command(r));
    }
    return r.ok();
}

// ----------------------------------------------------------- migration hook --

// A section layout only changes with a SAVE_VERSION bump, so reading file version N
// as version SAVE_VERSION needs a translation step living here. The table is keyed
// by the version found in the file; there is no older released format yet, so it is
// empty and every older file is rejected with an actionable message. Adding a field
// means: bump SAVE_VERSION, then append the step that upgrades (N -> N+1).
using MigrationFn = bool (*)(Game& g, const std::string& path, std::string* err);

struct Migration {
    uint32_t from_version;
    MigrationFn fn;
};

const std::vector<Migration>& migration_table() {
    static const std::vector<Migration> table;  // no released older versions yet
    return table;
}

bool migrate_from(uint32_t found, Game& g, const std::string& path, std::string* err) {
    for (const Migration& m : migration_table()) {
        if (m.from_version == found) return m.fn(g, path, err);
    }
    return fail(err, "no migration path from version " + std::to_string(found) + " to version " +
                         std::to_string(SAVE_VERSION) + " (file: " + path + ")");
}

// The save header carries a redundant, human-readable copy of the run's identity and
// control routing next to the seed so tools can describe a file without parsing
// sections. Sections stay authoritative (only they are hashed); load_game checks the
// two against each other and rejects a file where they disagree.
struct SaveHeader {
    uint64_t seed = 0;
    std::string scenario;
    uint64_t tick = 0;
    GameDate date;
    uint64_t ticks_run = 0;
    GameDate start_date;
    uint32_t player_country = INVALID_ID;
    std::vector<uint8_t> ai_controlled;
};

void write_header(ByteWriter& w, const Game& g) {
    w.u32(SAVE_MAGIC);
    w.u32(SAVE_VERSION);
    w.u64(g.seed);
    w.str(g.scenario_path);
    w.u64(g.world.tick);
    write_date(w, g.world.date);
    w.u64(g.ticks_run);
    write_date(w, g.start_date);
    w.u32(g.player_country.v);
    write_bytes(w, g.ai_controlled);
}

bool read_header(ByteReader& r, SaveHeader* h, const std::string& path, std::string* err) {
    uint32_t magic = 0;
    if (!r.u32(&magic)) return fail(err, "save file is truncated: no magic in " + path);
    if (magic != SAVE_MAGIC) {
        return fail(err, "not a save file: found magic " + hex64(magic) + ", expected " + hex64(SAVE_MAGIC) +
                             " (file: " + path + ")");
    }
    uint32_t version = 0;
    if (!r.u32(&version)) return fail(err, "save file is truncated: no version in " + path);
    if (version != SAVE_VERSION) {
        if (version > SAVE_VERSION) {
            return fail(err, "unsupported save version " + std::to_string(version) +
                                 ": this build supports version " + std::to_string(SAVE_VERSION) +
                                 " and cannot read newer files (file: " + path + ")");
        }
        return fail(err, "no migration path from version " + std::to_string(version) + " to version " +
                             std::to_string(SAVE_VERSION) + " (file: " + path + ")");
    }
    if (!r.u64(&h->seed) || !r.str(&h->scenario) || !r.u64(&h->tick) || !read_date(r, &h->date) ||
        !r.u64(&h->ticks_run) || !read_date(r, &h->start_date) || !r.u32(&h->player_country) ||
        !read_bytes(r, &h->ai_controlled)) {
        return fail(err, "save file is truncated in its header: " + path);
    }
    return true;
}

// ------------------------------------------------------------------- files ---

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes, std::string* err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return fail(err, "cannot open file for writing: " + path);
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) return fail(err, "failed to write " + std::to_string(bytes.size()) + " bytes to " + path);
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(err, "cannot open file for reading: " + path);
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) return fail(err, "failed to read file: " + path);
    if (out->empty()) return fail(err, "file is empty: " + path);
    return true;
}

// A command record trailer (tick + sequence + command + result) is never smaller
// than this, which bounds the allocation for a corrupt count.
constexpr uint32_t MIN_RECORD_BYTES = 14;  // 8 tick + 4 sequence + 1 command + 1 result

void write_command_log(ByteWriter& w, const CommandLog& log) {
    w.u32(static_cast<uint32_t>(log.records.size()));
    w.u32(log.next_sequence);
    for (const CommandRecord& rec : log.records) {
        w.u64(rec.tick);
        w.u32(rec.sequence);
        serialize_command(w, rec.command);
        w.u8(static_cast<uint8_t>(rec.result));
    }
}

bool read_command_log(ByteReader& r, CommandLog* log, std::string* err) {
    uint32_t count = 0;
    if (!read_count(r, MIN_RECORD_BYTES, &count)) return fail(err, "truncated or corrupt command log");
    uint32_t next_sequence = 0;
    if (!r.u32(&next_sequence)) return fail(err, "truncated command log header");
    log->clear();
    log->records.reserve(count);
    log->next_sequence = next_sequence;
    for (uint32_t i = 0; i < count; ++i) {
        CommandRecord rec;
        if (!r.u64(&rec.tick) || !r.u32(&rec.sequence)) {
            return fail(err, "command log truncated at record " + std::to_string(i));
        }
        // The record trailer is not covered by a hash, so `deserialize_command` is fed
        // bytes that may be arbitrary: a corrupt length field must surface as "malformed
        // file", not as an exception escaping load_game.
        try {
            rec.command = deserialize_command(r);
        } catch (const std::exception&) {
            return fail(err, "command log record " + std::to_string(i) + " is malformed");
        }
        uint8_t result = 0;
        if (!r.u8(&result) || !r.ok()) {
            return fail(err, "command log truncated at record " + std::to_string(i));
        }
        if (result >= static_cast<uint8_t>(CommandResult::Count)) {
            return fail(err, "command log record " + std::to_string(i) + " has an invalid result code");
        }
        rec.result = static_cast<CommandResult>(result);
        log->records.push_back(std::move(rec));
    }
    return true;
}

}  // namespace

const char* subsystem_name(Subsystem s) {
    switch (s) {
        case Subsystem::Map: return "Map";
        case Subsystem::Countries: return "Countries";
        case Subsystem::Economy: return "Economy";
        case Subsystem::Military: return "Military";
        case Subsystem::Battles: return "Battles";
        case Subsystem::Diplomacy: return "Diplomacy";
        case Subsystem::Politics: return "Politics";
        case Subsystem::Ai: return "Ai";
        case Subsystem::Rng: return "Rng";
        case Subsystem::Count: break;
    }
    return "Unknown";
}

void serialize_subsystem(const Game& g, Subsystem s, ByteWriter* out) {
    assert(out != nullptr);
    if (out == nullptr) return;
    ByteWriter& w = *out;
    switch (s) {
        case Subsystem::Map:
            write_store(w, g.world.provinces, write_province);
            write_store(w, g.world.states, write_state);
            write_store(w, g.world.regions, write_region);
            break;

        case Subsystem::Countries:
            write_store(w, g.world.countries, write_country_core);
            write_store(w, g.world.characters, write_character);
            break;

        case Subsystem::Economy: {
            w.u32(static_cast<uint32_t>(g.world.countries.size()));
            g.world.countries.for_each([&](CountryId id, const Country& c) {
                write_country_block(w, id, c, write_country_economy);
            });
            write_content(w, g.content);
            break;
        }

        case Subsystem::Military: {
            w.u32(static_cast<uint32_t>(g.world.countries.size()));
            g.world.countries.for_each([&](CountryId id, const Country& c) {
                write_country_block(w, id, c, write_country_military);
            });
            write_store(w, g.world.armies, write_army);
            write_store(w, g.world.divisions, write_division);
            break;
        }

        case Subsystem::Battles:
            write_store(w, g.world.battles, write_battle);
            break;

        case Subsystem::Diplomacy: {
            write_store(w, g.world.wars, write_war);
            w.u32(static_cast<uint32_t>(g.world.factions.size()));
            for (const Faction& f : g.world.factions) {
                w.u32(f.id);
                w.str(f.name);
                write_id(w, f.leader);
                write_ids(w, f.members);
            }
            // std::map iteration is ordered: relations round-trip byte-identically.
            w.u32(static_cast<uint32_t>(g.world.relations.size()));
            for (const auto& entry : g.world.relations) {
                w.u32(entry.first.first);
                w.u32(entry.first.second);
                w.f64(entry.second.value);
                w.boolean(entry.second.non_aggression);
                w.boolean(entry.second.military_access);
                w.boolean(entry.second.guarantee);
                w.boolean(entry.second.at_war);
            }
            w.u32(static_cast<uint32_t>(g.world.countries.size()));
            g.world.countries.for_each([&](CountryId id, const Country& c) {
                write_country_block(w, id, c, write_country_diplomacy);
            });
            break;
        }

        case Subsystem::Politics: {
            w.u32(static_cast<uint32_t>(g.world.countries.size()));
            g.world.countries.for_each([&](CountryId id, const Country& c) {
                write_country_block(w, id, c, write_country_politics);
            });
            break;
        }

        case Subsystem::Ai:
            write_ai(w, g);
            break;

        case Subsystem::Rng:
            write_rng(w, g);
            break;

        case Subsystem::Count:
            break;
    }
}

bool deserialize_subsystem(Game& g, Subsystem s, ByteReader* in) {
    assert(in != nullptr);
    if (in == nullptr) return false;
    ByteReader& r = *in;
    switch (s) {
        case Subsystem::Map:
            if (!read_store(r, g.world.provinces, read_province)) return false;
            if (!read_store(r, g.world.states, read_state)) return false;
            return read_store(r, g.world.regions, read_region);

        case Subsystem::Countries:
            if (!read_store(r, g.world.countries, read_country_core)) return false;
            return read_store(r, g.world.characters, read_character);

        case Subsystem::Economy: {
            uint32_t n = 0;
            if (!read_count(r, 8, &n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                bool ok = true;
                Country* c = country_block_target(r, g, &ok);
                if (c == nullptr || !ok) return false;
                if (!read_country_economy(r, c)) return false;
            }
            return read_content(r, &g.content);
        }

        case Subsystem::Military: {
            uint32_t n = 0;
            if (!read_count(r, 8, &n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                bool ok = true;
                Country* c = country_block_target(r, g, &ok);
                if (c == nullptr || !ok) return false;
                if (!read_country_military(r, c)) return false;
            }
            if (!read_store(r, g.world.armies, read_army)) return false;
            return read_store(r, g.world.divisions, read_division);
        }

        case Subsystem::Battles:
            return read_store(r, g.world.battles, read_battle);

        case Subsystem::Diplomacy: {
            if (!read_store(r, g.world.wars, read_war)) return false;
            uint32_t n = 0;
            if (!read_count(r, 12, &n)) return false;
            g.world.factions.clear();
            g.world.factions.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                Faction& f = g.world.factions[i];
                if (!r.u32(&f.id) || !r.str(&f.name)) return false;
                uint32_t leader = INVALID_ID;
                if (!r.u32(&leader)) return false;
                f.leader = CountryId(leader);
                if (!read_ids(r, &f.members)) return false;
            }
            if (!read_count(r, 16, &n)) return false;
            g.world.relations.clear();
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t lo = 0;
                uint32_t hi = 0;
                Relation rel;
                if (!r.u32(&lo) || !r.u32(&hi)) return false;
                if (lo > hi) return false;  // relations always key the lower id first
                if (!r.f64(&rel.value)) return false;
                if (!r.boolean(&rel.non_aggression) || !r.boolean(&rel.military_access)) return false;
                if (!r.boolean(&rel.guarantee) || !r.boolean(&rel.at_war)) return false;
                g.world.relations[{lo, hi}] = rel;
            }
            if (!read_count(r, 8, &n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                bool ok = true;
                Country* c = country_block_target(r, g, &ok);
                if (c == nullptr || !ok) return false;
                if (!read_country_diplomacy(r, c)) return false;
            }
            return true;
        }

        case Subsystem::Politics: {
            uint32_t n = 0;
            if (!read_count(r, 8, &n)) return false;
            for (uint32_t i = 0; i < n; ++i) {
                bool ok = true;
                Country* c = country_block_target(r, g, &ok);
                if (c == nullptr || !ok) return false;
                if (!read_country_politics(r, c)) return false;
            }
            return true;
        }

        case Subsystem::Ai:
            return read_ai(r, g);

        case Subsystem::Rng:
            return read_rng(r, g);

        case Subsystem::Count:
            break;
    }
    return false;
}

uint64_t subsystem_hash(const Game& g, Subsystem s) {
    ByteWriter w;
    serialize_subsystem(g, s, &w);
    return hash_bytes(w.data());
}

uint64_t world_hash(const Game& g) {
    Hasher h;
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        h.u64(subsystem_hash(g, static_cast<Subsystem>(i)));
    }
    return h.value();
}

std::string hash_report(const Game& g) {
    std::string out;
    char line[160];
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        Subsystem s = static_cast<Subsystem>(i);
        ByteWriter w;
        serialize_subsystem(g, s, &w);
        std::snprintf(line, sizeof(line), "%-10s %s  %u bytes\n", subsystem_name(s),
                      hex64(hash_bytes(w.data())).c_str(), static_cast<unsigned>(w.size()));
        out += line;
    }
    std::snprintf(line, sizeof(line), "%-10s %s  tick=%llu date=%04d-%02d-%02dT%02d seed=%s world_seed=%s\n",
                  "World", hex64(world_hash(g)).c_str(),
                  static_cast<unsigned long long>(g.world.tick), static_cast<int>(g.world.date.year),
                  static_cast<int>(g.world.date.month), static_cast<int>(g.world.date.day),
                  static_cast<int>(g.world.date.hour), hex64(g.seed).c_str(),
                  hex64(g.world.world_seed).c_str());
    out += line;
    return out;
}

bool save_game(const Game& g, const std::string& path, std::string* err) {
    ByteWriter w;
    write_header(w, g);

    w.u32(static_cast<uint32_t>(Subsystem::Count));
    for (int i = 0; i < static_cast<int>(Subsystem::Count); ++i) {
        ByteWriter payload;
        serialize_subsystem(g, static_cast<Subsystem>(i), &payload);
        w.u8(static_cast<uint8_t>(i));
        w.u32(static_cast<uint32_t>(payload.size()));
        w.u64(hash_bytes(payload.data()));
        if (!payload.data().empty()) w.raw(payload.data().data(), payload.data().size());
    }

    w.u64(world_hash(g));
    write_command_log(w, g.log);
    return write_file(path, w.data(), err);
}

bool load_game(Game& g, const std::string& path, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes, err)) return false;
    ByteReader r(bytes);

    SaveHeader header;
    if (!read_header(r, &header, path, err)) return false;

    uint32_t section_count = 0;
    if (!r.u32(&section_count)) return fail(err, "save file is truncated before its sections: " + path);
    if (section_count != static_cast<uint32_t>(Subsystem::Count)) {
        return fail(err, "save file declares " + std::to_string(section_count) + " sections, version " +
                             std::to_string(SAVE_VERSION) + " defines " +
                             std::to_string(static_cast<uint32_t>(Subsystem::Count)));
    }

    const int count = static_cast<int>(Subsystem::Count);
    std::vector<std::vector<uint8_t>> payload(static_cast<size_t>(count));
    std::vector<uint64_t> stored_hash(static_cast<size_t>(count), 0);
    std::vector<uint8_t> present(static_cast<size_t>(count), 0);
    for (uint32_t i = 0; i < section_count; ++i) {
        uint8_t tag = 0;
        uint32_t length = 0;
        uint64_t hash = 0;
        if (!r.u8(&tag) || !r.u32(&length) || !r.u64(&hash)) {
            return fail(err, "save file is truncated in a section header: " + path);
        }
        if (tag >= static_cast<uint8_t>(Subsystem::Count)) {
            return fail(err, "save file has an unknown section tag " + std::to_string(tag));
        }
        if (present[tag] != 0) {
            return fail(err, "save file repeats section '" + std::string(subsystem_name(static_cast<Subsystem>(tag))) + "'");
        }
        if (length > r.remaining()) {
            return fail(err, "section '" + std::string(subsystem_name(static_cast<Subsystem>(tag))) +
                                 "' declares " + std::to_string(length) + " bytes but only " +
                                 std::to_string(r.remaining()) + " remain: file is truncated");
        }
        payload[tag].assign(length, 0);
        if (length != 0 && !r.raw(payload[tag].data(), length)) {
            return fail(err, "save file is truncated in section '" +
                                 std::string(subsystem_name(static_cast<Subsystem>(tag))) + "'");
        }
        stored_hash[tag] = hash;
        present[tag] = 1;
    }

    // Hash check first: the desync diagnostic names the first section whose bytes do
    // not match, and nothing from an unverified payload is ever parsed into state.
    for (int i = 0; i < count; ++i) {
        if (present[i] == 0) {
            return fail(err, "save file is missing subsystem '" +
                                 std::string(subsystem_name(static_cast<Subsystem>(i))) + "'");
        }
        const uint64_t actual = hash_bytes(payload[i]);
        if (actual != stored_hash[i]) {
            return fail(err, "section hash mismatch for subsystem '" +
                                 std::string(subsystem_name(static_cast<Subsystem>(i))) + "': file says " +
                                 hex64(stored_hash[i]) + ", payload hashes to " + hex64(actual) +
                                 " (save is corrupt or was produced by a different build)");
        }
    }

    for (int i = 0; i < count; ++i) {
        Subsystem s = static_cast<Subsystem>(i);
        ByteReader in(payload[i]);
        if (!deserialize_subsystem(g, s, &in)) {
            return fail(err, "failed to deserialize subsystem '" + std::string(subsystem_name(s)) +
                                 "' (malformed payload)");
        }
        if (!in.eof()) {
            return fail(err, "subsystem '" + std::string(subsystem_name(s)) + "' payload has " +
                                 std::to_string(in.remaining()) + " trailing bytes");
        }
        payload[i].clear();
        payload[i].shrink_to_fit();
    }

    // The header copy is redundant, so a disagreement means the file was assembled
    // from two different games: report which field diverged.
    if (g.seed != header.seed) {
        return fail(err, "save header seed " + hex64(header.seed) + " disagrees with the Rng section seed " +
                             hex64(g.seed));
    }
    if (g.world.tick != header.tick) {
        return fail(err, "save header tick " + std::to_string(header.tick) + " disagrees with the Rng section tick " +
                             std::to_string(g.world.tick));
    }
    if (!(g.world.date == header.date)) {
        return fail(err, "save header date disagrees with the Rng section date");
    }
    if (g.ticks_run != header.ticks_run) {
        return fail(err, "save header ticks_run " + std::to_string(header.ticks_run) +
                             " disagrees with the Rng section ticks_run " + std::to_string(g.ticks_run));
    }
    if (!(g.start_date == header.start_date)) {
        return fail(err, "save header start_date disagrees with the Rng section start_date");
    }
    if (g.player_country.v != header.player_country) {
        return fail(err, "save header player_country " + std::to_string(header.player_country) +
                             " disagrees with the Ai section player_country " +
                             std::to_string(g.player_country.v));
    }
    if (g.ai_controlled != header.ai_controlled) {
        return fail(err, "save header ai_controlled (" + std::to_string(header.ai_controlled.size()) +
                             " entries) disagrees with the Ai section (" +
                             std::to_string(g.ai_controlled.size()) + " entries)");
    }
    g.scenario_path = header.scenario;

    uint64_t stored_world = 0;
    if (!r.u64(&stored_world)) return fail(err, "save file is truncated before the world hash: " + path);
    const uint64_t actual_world = world_hash(g);
    if (actual_world != stored_world) {
        return fail(err, "world hash mismatch after load: file says " + hex64(stored_world) +
                             ", loaded state hashes to " + hex64(actual_world));
    }

    if (!read_command_log(r, &g.log, err)) return false;
    if (!r.eof()) {
        return fail(err, "save file has " + std::to_string(r.remaining()) +
                             " trailing bytes after the command log");
    }
    return true;
}

bool save_replay(const Game& g, const std::string& path, std::string* err) {
    ByteWriter w;
    w.u32(REPLAY_MAGIC);
    w.u32(SAVE_VERSION);
    w.u64(g.seed);
    w.str(g.scenario_path);
    w.u64(world_hash(g));   // the state the recorded commands must be applied to
    w.u64(g.world.tick);    // tick the recording ends at
    write_command_log(w, g.log);
    return write_file(path, w.data(), err);
}

bool load_replay(Game& g, const std::string& path, std::string* err) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, &bytes, err)) return false;
    ByteReader r(bytes);

    uint32_t magic = 0;
    if (!r.u32(&magic)) return fail(err, "replay file is truncated: no magic in " + path);
    if (magic != REPLAY_MAGIC) {
        return fail(err, "not a replay file: found magic " + hex64(magic) + ", expected " +
                             hex64(REPLAY_MAGIC) + " (file: " + path + ")");
    }
    uint32_t version = 0;
    if (!r.u32(&version)) return fail(err, "replay file is truncated: no version in " + path);
    if (version != SAVE_VERSION) {
        if (version > SAVE_VERSION) {
            return fail(err, "unsupported replay version " + std::to_string(version) +
                                 ": this build supports version " + std::to_string(SAVE_VERSION) +
                                 " and cannot read newer files (file: " + path + ")");
        }
        return migrate_from(version, g, path, err);
    }

    uint64_t seed = 0;
    std::string scenario;
    uint64_t initial_hash = 0;
    uint64_t ticks = 0;
    if (!r.u64(&seed) || !r.str(&scenario) || !r.u64(&initial_hash) || !r.u64(&ticks)) {
        return fail(err, "replay file is truncated in its header: " + path);
    }

    CommandLog log;
    if (!read_command_log(r, &log, err)) return false;
    if (!r.eof()) {
        return fail(err, "replay file has " + std::to_string(r.remaining()) +
                             " trailing bytes after the command log");
    }

    // When the target game is already at the tick the recording ends at, its world
    // hash must be the recorded initial hash: that is the difference between
    // replaying a recording and replaying it against the wrong state.
    if (g.world.tick == ticks && world_hash(g) != initial_hash) {
        return fail(err, "replay initial hash mismatch: recording starts from " + hex64(initial_hash) +
                             ", current state hashes to " + hex64(world_hash(g)) +
                             " (load the matching save before replaying)");
    }

    g.log = std::move(log);
    g.seed = seed;
    if (!scenario.empty()) g.scenario_path = scenario;
    return true;
}

}  // namespace hoi
