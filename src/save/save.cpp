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
//              supply_bottleneck), state store (factories, script flags, occupation
//              counters),
//              region store (weather, per-country air and naval control)
//   Countries  country identity and ownership bookkeeping (id, tag, name, alive,
//              ideology, overlord, puppets, capital), equipment_stockpile,
//              law_levels, the four Modifier sets, the per-resource
//              produced/consumed/imported/exported aggregates, starting_factories,
//              the general roster (Country::generals), the air wing roster
//              (Country::wings), the fleet roster (Country::fleets) and the
//              character store
//   Economy    the content snapshot - equipment, division templates, technologies,
//              laws, buildings, focuses, events, decisions, equipment components
//              (Content::components, the parts a country may fit), equipment designs
//              (Content::designs, the variants countries have created - a design's
//              computed statistics live in the EquipmentDef it produced, which sits
//              in the same equipment table, so `produced` resolves after a load),
//              national spirits, political advisors and SimConstants, i.e. every table
//              the simulation reads - plus per country: production
//              lines, construction queue, research state and the template roster
//              (Country::templates), and the world-level World::trade_routes
//              (resources move, industry consumes them, and the routes tie up
//              civilian factories and convoys - all economic state; they live here
//              rather than in Diplomacy because they are flows, not relations).
//              Content lives here because it is what industry
//              and research consume, and it travels with the save so a
//              default-constructed Game can continue from a file without help from
//              data/; each script block (FocusDef/EventDef/DecisionDef/SpiritDef/
//              AdvisorDef/ComponentDef Json fields) travels structurally and the
//              derived key -> id maps are rebuilt on load instead of being stored twice
//   Military   per country: division/army rosters and the training list; then the
//              army store, the division store, the air wing store, and the naval
//              area: the ship store, the task force store, the fleet store and the
//              naval invasion list (Country::fleets lives in the Countries section
//              with the other id rosters)
//   Battles    the battle store, including both sides, the debug breakdown lines
//              and the last-tick markers
//   Diplomacy  the war store, factions, the ordered relation map, and per country
//              the war list and faction membership
//   Politics   per-country scalars and flags not carried by another section:
//              political_power, stability, war_support, manpower, fuel,
//              fuel_capacity, consumer_goods_ratio, army_experience, at_war,
//              fuel_priority, last_capitulation_check, and the whole national
//              focus / event / decision state (completed_focuses, selected_focus,
//              focus_progress, pending_events, fired_events, active_decisions,
//              decision_days_left, decision_cooldown, country_flags,
//              timed_modifiers, national_spirits, spirit_keys, advisors,
//              spirit_slots, advisor_slots, designs); then the world-level script
//              variables (World::script_vars, an ordered map) and the scheduled events
//              (World::delayed_events). Politics owns them because focuses, events,
//              decisions, spirits and advisors are the political layer and these
//              are the fields they mutate; Country::designs sits with spirit_keys
//              and advisors because it is the same kind of roster - the country's
//              indices into a content table (Content::designs, in Economy)
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
    w.i32(p.anti_air);
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
    if (!r.i32(&p->anti_air)) return false;
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

// Defined below with the other content helpers; State::flags uses them here.
void write_strs(ByteWriter& w, const std::vector<std::string>& v);
bool read_strs(ByteReader& r, std::vector<std::string>* out);

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
    write_strs(w, s.flags);
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
    if (!read_strs(r, &s->flags)) return false;
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
    // Air control is derived each tick by the air phase, but it is authoritative
    // between ticks and the land phase reads it: it is written in region order so
    // the bytes are deterministic.
    w.u32(static_cast<uint32_t>(r.air_control.size()));
    for (const std::pair<CountryId, double>& e : r.air_control) {
        w.u32(e.first.v);
        w.f64(e.second);
    }
    // Naval control is the sea counterpart, written by the naval phase and read by
    // missions, the AI and the UI the same way: same vector-of-pairs shape.
    w.u32(static_cast<uint32_t>(r.naval_control.size()));
    for (const std::pair<CountryId, double>& e : r.naval_control) {
        w.u32(e.first.v);
        w.f64(e.second);
    }
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
    uint32_t n = 0;
    if (!read_count(r, 12, &n)) return false;  // country id + share
    g->air_control.clear();
    g->air_control.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t country = INVALID_ID;
        double share = 0.0;
        if (!r.u32(&country) || !r.f64(&share)) return false;
        g->air_control.emplace_back(CountryId(country), share);
    }
    if (!read_count(r, 12, &n)) return false;  // country id + share
    g->naval_control.clear();
    g->naval_control.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t country = INVALID_ID;
        double share = 0.0;
        if (!r.u32(&country) || !r.f64(&share)) return false;
        g->naval_control.emplace_back(CountryId(country), share);
    }
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

// uint32 vectors are the shared shape of the focus/event/decision id lists, the
// per-country politics indexes and the content key indexes.
void write_u32s(ByteWriter& w, const std::vector<uint32_t>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (uint32_t e : v) w.u32(e);
}

bool read_u32s(ByteReader& r, std::vector<uint32_t>* out) {
    uint32_t n = 0;
    if (!read_count(r, 4, &n)) return false;
    out->clear();
    out->resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.u32(&(*out)[i])) return false;
    }
    return true;
}

// Script blocks (triggers and effects) travel structurally, not as text: numbers keep
// their exact IEEE bits (so a negative zero or a value whose text form is not
// reversible cannot slip a hash comparison) and object key order is preserved.
constexpr int MAX_JSON_DEPTH = 64;

void write_json_value(ByteWriter& w, const Json& j) {
    write_enum(w, j.type());
    switch (j.type()) {
        case Json::Type::Null:
            return;
        case Json::Type::Bool:
            w.boolean(j.as_bool());
            return;
        case Json::Type::Number:
            w.f64(j.as_double());
            return;
        case Json::Type::String:
            w.str(j.as_string());
            return;
        case Json::Type::Array:
            w.u32(static_cast<uint32_t>(j.size()));
            for (const Json& e : j.array_items()) write_json_value(w, e);
            return;
        case Json::Type::Object:
            w.u32(static_cast<uint32_t>(j.size()));
            for (const std::pair<std::string, Json>& kv : j.object_items()) {
                w.str(kv.first);
                write_json_value(w, kv.second);
            }
            return;
    }
}

bool read_json_value(ByteReader& r, Json* out, int depth) {
    if (depth > MAX_JSON_DEPTH) return false;
    Json::Type type;
    if (!read_enum(r, &type, static_cast<int>(Json::Type::Object) + 1)) return false;
    switch (type) {
        case Json::Type::Null:
            *out = Json();
            return true;
        case Json::Type::Bool: {
            bool b = false;
            if (!r.boolean(&b)) return false;
            *out = Json(b);
            return true;
        }
        case Json::Type::Number: {
            double d = 0.0;
            if (!r.f64(&d)) return false;
            *out = Json(d);
            return true;
        }
        case Json::Type::String: {
            std::string s;
            if (!r.str(&s)) return false;
            *out = Json(std::move(s));
            return true;
        }
        case Json::Type::Array: {
            uint32_t n = 0;
            if (!read_count(r, 1, &n)) return false;
            Json arr = Json::array();
            for (uint32_t i = 0; i < n; ++i) {
                Json e;
                if (!read_json_value(r, &e, depth + 1)) return false;
                arr.push_back(std::move(e));
            }
            *out = std::move(arr);
            return true;
        }
        case Json::Type::Object: {
            uint32_t n = 0;
            if (!read_count(r, 5, &n)) return false;  // key length + value type
            Json obj = Json::object();
            for (uint32_t i = 0; i < n; ++i) {
                std::string key;
                Json value;
                if (!r.str(&key)) return false;
                if (!read_json_value(r, &value, depth + 1)) return false;
                obj.set(key, std::move(value));
            }
            *out = std::move(obj);
            return true;
        }
    }
    return false;
}

void write_json(ByteWriter& w, const Json& j) { write_json_value(w, j); }

bool read_json(ByteReader& r, Json* out) { return read_json_value(r, out, 0); }

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
    w.f64(e.air_defence);
    w.f64(e.ground_attack);
    w.f64(e.agility);
    w.f64(e.range);
    w.f64(e.naval_attack);
    w.f64(e.torpedo_attack);
    w.f64(e.sub_detection);
    w.f64(e.detection);
    w.f64(e.visibility);
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
    if (!r.f64(&e->air_defence) || !r.f64(&e->ground_attack) || !r.f64(&e->agility)) return false;
    if (!r.f64(&e->range)) return false;
    if (!r.f64(&e->naval_attack) || !r.f64(&e->torpedo_attack) || !r.f64(&e->sub_detection)) return false;
    if (!r.f64(&e->detection) || !r.f64(&e->visibility)) return false;
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

// Focuses, events and decisions are content: their triggers and effects are stored
// structurally, so a save carries the exact script blocks the run used and a loaded
// Game continues without help from data/.
void write_focus(ByteWriter& w, const FocusDef& f) {
    w.u32(f.index);
    w.str(f.key);
    w.str(f.name);
    w.str(f.tree);
    w.i32(f.x);
    w.i32(f.y);
    w.f64(f.days);
    write_strs(w, f.prerequisites);
    write_strs(w, f.mutually_exclusive);
    write_json(w, f.available);
    write_json(w, f.bypass);
    write_json(w, f.effects);
    w.f64(f.ai_weight);
}

bool read_focus(ByteReader& r, FocusDef* f) {
    if (!r.u32(&f->index)) return false;
    if (!r.str(&f->key) || !r.str(&f->name) || !r.str(&f->tree)) return false;
    if (!r.i32(&f->x) || !r.i32(&f->y) || !r.f64(&f->days)) return false;
    if (!read_strs(r, &f->prerequisites)) return false;
    if (!read_strs(r, &f->mutually_exclusive)) return false;
    if (!read_json(r, &f->available)) return false;
    if (!read_json(r, &f->bypass)) return false;
    if (!read_json(r, &f->effects)) return false;
    return r.f64(&f->ai_weight);
}

void write_event_option(ByteWriter& w, const EventOptionDef& o) {
    w.str(o.name);
    write_json(w, o.effects);
    w.f64(o.ai_weight);
}

bool read_event_option(ByteReader& r, EventOptionDef* o) {
    if (!r.str(&o->name)) return false;
    if (!read_json(r, &o->effects)) return false;
    return r.f64(&o->ai_weight);
}

void write_event(ByteWriter& w, const EventDef& e) {
    w.u32(e.index);
    w.str(e.key);
    w.str(e.title);
    w.str(e.description);
    w.boolean(e.fire_only_once);
    w.boolean(e.major);
    write_json(w, e.trigger);
    w.u32(static_cast<uint32_t>(e.options.size()));
    for (const EventOptionDef& o : e.options) write_event_option(w, o);
    write_json(w, e.immediate);
}

bool read_event(ByteReader& r, EventDef* e) {
    if (!r.u32(&e->index)) return false;
    if (!r.str(&e->key) || !r.str(&e->title) || !r.str(&e->description)) return false;
    if (!r.boolean(&e->fire_only_once) || !r.boolean(&e->major)) return false;
    if (!read_json(r, &e->trigger)) return false;
    uint32_t n = 0;
    if (!read_count(r, 12, &n)) return false;  // empty option is >= 12 bytes
    e->options.clear();
    e->options.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_event_option(r, &e->options[i])) return false;
    }
    return read_json(r, &e->immediate);
}

void write_decision(ByteWriter& w, const DecisionDef& d) {
    w.u32(d.index);
    w.str(d.key);
    w.str(d.name);
    w.str(d.description);
    w.i32(d.category);
    w.boolean(d.targets_state);
    w.f64(d.cost_pp);
    w.i32(d.days_remove);
    w.i32(d.days_cooldown);
    write_json(w, d.visible);
    write_json(w, d.available);
    write_json(w, d.effects);
    write_json(w, d.remove_effect);
    w.f64(d.ai_weight);
}

bool read_decision(ByteReader& r, DecisionDef* d) {
    if (!r.u32(&d->index)) return false;
    if (!r.str(&d->key) || !r.str(&d->name) || !r.str(&d->description)) return false;
    if (!r.i32(&d->category) || !r.boolean(&d->targets_state)) return false;
    if (!r.f64(&d->cost_pp)) return false;
    if (!r.i32(&d->days_remove) || !r.i32(&d->days_cooldown)) return false;
    if (!read_json(r, &d->visible)) return false;
    if (!read_json(r, &d->available)) return false;
    if (!read_json(r, &d->effects)) return false;
    if (!read_json(r, &d->remove_effect)) return false;
    return r.f64(&d->ai_weight);
}

// National spirits and political advisors travel with the rest of the content
// database: the simulation reads their modifiers and availability triggers, and the
// AI plans appointments from them, so a save that omitted them would continue from a
// different rule set. Derived key -> index maps are rebuilt on load like the rest.
void write_spirit(ByteWriter& w, const SpiritDef& s) {
    w.u32(s.index);
    w.str(s.key);
    w.str(s.name);
    w.str(s.description);
    w.i32(s.slots);
    write_json(w, s.available);
    write_modifiers(w, s.modifiers);
    write_json(w, s.effects);
}

bool read_spirit(ByteReader& r, SpiritDef* s) {
    if (!r.u32(&s->index)) return false;
    if (!r.str(&s->key) || !r.str(&s->name) || !r.str(&s->description)) return false;
    if (!r.i32(&s->slots)) return false;
    if (!read_json(r, &s->available)) return false;
    if (!read_modifiers(r, &s->modifiers)) return false;
    return read_json(r, &s->effects);
}

void write_advisor(ByteWriter& w, const AdvisorDef& a) {
    w.u32(a.index);
    w.str(a.key);
    w.str(a.name);
    w.str(a.description);
    w.f64(a.cost_pp);
    write_json(w, a.available);
    write_modifiers(w, a.modifiers);
}

bool read_advisor(ByteReader& r, AdvisorDef* a) {
    if (!r.u32(&a->index)) return false;
    if (!r.str(&a->key) || !r.str(&a->name) || !r.str(&a->description)) return false;
    if (!r.f64(&a->cost_pp)) return false;
    if (!read_json(r, &a->available)) return false;
    return read_modifiers(r, &a->modifiers);
}

// Equipment designers. The component table is the set of parts a country may fit and
// the design table is the variants it has created; both are content the simulation
// reads (availability gates, statistics and production), so they travel with the save
// like every other table. A design's computed statistics live in the EquipmentDef it
// produced, already serialized in the equipment table, so `produced` resolves to the
// same model after a load. ComponentDef carries a JSON availability trigger like a
// spirit, which travels structurally. Derived key -> index maps are rebuilt on load.
void write_component(ByteWriter& w, const ComponentDef& c) {
    w.u32(c.index);
    w.str(c.key);
    w.str(c.name);
    write_enum(w, c.slot);
    write_enum(w, c.category);
    w.i32(c.year);
    w.f64(c.soft_attack);
    w.f64(c.hard_attack);
    w.f64(c.air_attack);
    w.f64(c.air_defence);
    w.f64(c.ground_attack);
    w.f64(c.agility);
    w.f64(c.armor);
    w.f64(c.piercing);
    w.f64(c.defense);
    w.f64(c.breakthrough);
    w.f64(c.hardness);
    w.f64(c.max_strength);
    w.f64(c.organization);
    w.f64(c.speed);
    w.f64(c.reliability);
    w.f64(c.range);
    w.f64(c.detection);
    w.f64(c.sub_detection);
    w.f64(c.naval_attack);
    w.f64(c.torpedo_attack);
    w.f64(c.visibility);
    w.f64(c.build_cost_add);
    w.f64(c.cost_multiplier);
    write_resources(w, c.resources);
    w.f64(c.fuel_use);
    w.f64(c.supply_use);
    w.f64(c.manpower);
    write_json(w, c.available);
}

bool read_component(ByteReader& r, ComponentDef* c) {
    if (!r.u32(&c->index)) return false;
    if (!r.str(&c->key) || !r.str(&c->name)) return false;
    if (!read_enum(r, &c->slot, static_cast<int>(ComponentSlot::Count))) return false;
    if (!read_enum(r, &c->category, static_cast<int>(EquipmentCategory::Count))) return false;
    if (!r.i32(&c->year)) return false;
    if (!r.f64(&c->soft_attack) || !r.f64(&c->hard_attack) || !r.f64(&c->air_attack)) return false;
    if (!r.f64(&c->air_defence) || !r.f64(&c->ground_attack) || !r.f64(&c->agility)) return false;
    if (!r.f64(&c->armor) || !r.f64(&c->piercing)) return false;
    if (!r.f64(&c->defense) || !r.f64(&c->breakthrough)) return false;
    if (!r.f64(&c->hardness)) return false;
    if (!r.f64(&c->max_strength) || !r.f64(&c->organization) || !r.f64(&c->speed)) return false;
    if (!r.f64(&c->reliability) || !r.f64(&c->range)) return false;
    if (!r.f64(&c->detection) || !r.f64(&c->sub_detection)) return false;
    if (!r.f64(&c->naval_attack) || !r.f64(&c->torpedo_attack) || !r.f64(&c->visibility)) return false;
    if (!r.f64(&c->build_cost_add) || !r.f64(&c->cost_multiplier)) return false;
    if (!read_resources(r, c->resources)) return false;
    if (!r.f64(&c->fuel_use) || !r.f64(&c->supply_use) || !r.f64(&c->manpower)) return false;
    return read_json(r, &c->available);
}

void write_design(ByteWriter& w, const EquipmentDesign& d) {
    w.u32(d.index);
    w.str(d.key);
    w.str(d.name);
    write_id(w, d.country);
    write_id(w, d.archetype);
    w.i32(d.year);
    w.u32(static_cast<uint32_t>(d.components.size()));
    for (const std::pair<ComponentSlot, uint32_t>& part : d.components) {
        write_enum(w, part.first);
        w.u32(part.second);
    }
    write_id(w, d.produced);
}

bool read_design(ByteReader& r, EquipmentDesign* d) {
    if (!r.u32(&d->index)) return false;
    if (!r.str(&d->key) || !r.str(&d->name)) return false;
    uint32_t country = INVALID_ID;
    uint32_t archetype = INVALID_ID;
    if (!r.u32(&country) || !r.u32(&archetype)) return false;
    d->country = CountryId(country);
    d->archetype = EquipmentId(archetype);
    if (!r.i32(&d->year)) return false;
    uint32_t n = 0;
    if (!read_count(r, 5, &n)) return false;  // slot byte + component index
    d->components.clear();
    d->components.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ComponentSlot slot = ComponentSlot::Special;
        uint32_t component = 0;
        if (!read_enum(r, &slot, static_cast<int>(ComponentSlot::Count))) return false;
        if (!r.u32(&component)) return false;
        d->components.emplace_back(slot, component);
    }
    uint32_t produced = INVALID_ID;
    if (!r.u32(&produced)) return false;
    d->produced = EquipmentId(produced);
    return true;
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
        k.construction_level_scaling, k.max_factories_per_project, k.research_base_days,
        k.research_year_penalty, k.research_speed_base, k.manpower_growth_per_year_fraction,
        k.recruitable_base, k.base_hours_per_province, k.min_division_speed, k.river_crossing_penalty,
        k.combat_width_base, k.damage_scale, k.org_damage_share, k.strength_damage_share,
        k.armor_advantage_multiplier, k.armor_disadvantage_multiplier, k.org_recovery_base,
        k.entrenchment_per_day, k.planning_per_day, k.planning_max_attack_bonus,
        k.battle_retreat_org_threshold, k.max_battles_per_province, k.supply_hub_radius,
        k.supply_range_penalty, k.supply_demand_per_width, k.supply_rail_bonus_per_level,
        k.supply_infrastructure_bonus_per_level, k.fuel_demand_per_day,
        k.air_base_capacity_per_level, k.air_sortie_hours, k.air_cas_effect,
        k.air_superiority_effect, k.air_bombing_industry_damage, k.air_logistics_strike_damage,
        k.air_anti_air_bombing_reduction, k.air_anti_air_combat_loss_factor, k.air_combat_scale,
        k.air_aircraft_durability, k.air_agility_weight, k.air_combat_defence_floor,
        k.air_combat_roll_base, k.air_experience_per_combat_hour, k.air_experience_per_mission_hour,
        k.air_cas_organisation_damage, k.air_cas_strength_damage, k.air_bombing_power_unit,
        k.air_logistics_power_unit, k.air_support_min_modifier, k.air_support_max_modifier,
        k.air_mission_weight_contested, k.air_mission_weight_support,
        k.naval_base_capacity_per_level, k.naval_detection_scale, k.naval_combat_roll_base,
        k.naval_combat_scale, k.naval_org_damage_scale, k.naval_torpedo_large_hull_bonus,
        k.naval_sub_detection_penalty, k.naval_aa_carrier_air_factor, k.naval_air_attacks_per_hour,
        k.naval_screen_share_cap, k.naval_retreat_strength_threshold, k.naval_retreat_org_threshold,
        k.naval_repair_per_hour, k.naval_repair_org_per_hour, k.naval_repair_cost_fuel,
        k.naval_repair_cost_stockpile_share, k.naval_fuel_use_per_hour,
        k.naval_training_experience_per_hour, k.naval_combat_experience_per_hour,
        k.naval_raid_convoy_damage, k.naval_raid_control_cut, k.naval_escort_protection,
        k.naval_max_engagement_ships, k.naval_large_hull_hp, k.naval_base_supply_per_level,
        k.naval_supply_sea_range_penalty, k.naval_supply_convoy_use_per_capacity,
        k.naval_supply_raid_threshold,
        k.naval_invasion_convoys_per_division, k.naval_invasion_hours_per_sea_hop,
        k.naval_invasion_interception_base, k.naval_invasion_interception_threat_scale,
        k.naval_invasion_escort_mitigation,
        k.political_power_per_day, k.focus_progress_speed, k.stability_drift, k.war_support_drift,
        k.weather_change_chance,
        k.trade_convoy_use_per_unit, k.trade_factory_cost_per_unit, k.trade_factory_cost_law_scale};
    static_assert(sizeof(values) / sizeof(values[0]) == 115,
                  "SimConstants changed: update write_constants/read_constants and the test drift guard");
    w.u32(static_cast<uint32_t>(sizeof(values) / sizeof(values[0])));
    for (double v : values) w.f64(v);
}

bool read_constants(ByteReader& r, SimConstants* k) {
    uint32_t n = 0;
    if (!read_count(r, 8, &n)) return false;
    if (n != 115) return false;  // a different count means a different SimConstants layout
    double v[115] = {0.0};
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
    k->max_factories_per_project = v[24];
    k->research_base_days = v[25];
    k->research_year_penalty = v[26];
    k->research_speed_base = v[27];
    k->manpower_growth_per_year_fraction = v[28];
    k->recruitable_base = v[29];
    k->base_hours_per_province = v[30];
    k->min_division_speed = v[31];
    k->river_crossing_penalty = v[32];
    k->combat_width_base = v[33];
    k->damage_scale = v[34];
    k->org_damage_share = v[35];
    k->strength_damage_share = v[36];
    k->armor_advantage_multiplier = v[37];
    k->armor_disadvantage_multiplier = v[38];
    k->org_recovery_base = v[39];
    k->entrenchment_per_day = v[40];
    k->planning_per_day = v[41];
    k->planning_max_attack_bonus = v[42];
    k->battle_retreat_org_threshold = v[43];
    k->max_battles_per_province = v[44];
    k->supply_hub_radius = v[45];
    k->supply_range_penalty = v[46];
    k->supply_demand_per_width = v[47];
    k->supply_rail_bonus_per_level = v[48];
    k->supply_infrastructure_bonus_per_level = v[49];
    k->fuel_demand_per_day = v[50];
    k->air_base_capacity_per_level = v[51];
    k->air_sortie_hours = v[52];
    k->air_cas_effect = v[53];
    k->air_superiority_effect = v[54];
    k->air_bombing_industry_damage = v[55];
    k->air_logistics_strike_damage = v[56];
    k->air_anti_air_bombing_reduction = v[57];
    k->air_anti_air_combat_loss_factor = v[58];
    k->air_combat_scale = v[59];
    k->air_aircraft_durability = v[60];
    k->air_agility_weight = v[61];
    k->air_combat_defence_floor = v[62];
    k->air_combat_roll_base = v[63];
    k->air_experience_per_combat_hour = v[64];
    k->air_experience_per_mission_hour = v[65];
    k->air_cas_organisation_damage = v[66];
    k->air_cas_strength_damage = v[67];
    k->air_bombing_power_unit = v[68];
    k->air_logistics_power_unit = v[69];
    k->air_support_min_modifier = v[70];
    k->air_support_max_modifier = v[71];
    k->air_mission_weight_contested = v[72];
    k->air_mission_weight_support = v[73];
    k->naval_base_capacity_per_level = v[74];
    k->naval_detection_scale = v[75];
    k->naval_combat_roll_base = v[76];
    k->naval_combat_scale = v[77];
    k->naval_org_damage_scale = v[78];
    k->naval_torpedo_large_hull_bonus = v[79];
    k->naval_sub_detection_penalty = v[80];
    k->naval_aa_carrier_air_factor = v[81];
    k->naval_air_attacks_per_hour = v[82];
    k->naval_screen_share_cap = v[83];
    k->naval_retreat_strength_threshold = v[84];
    k->naval_retreat_org_threshold = v[85];
    k->naval_repair_per_hour = v[86];
    k->naval_repair_org_per_hour = v[87];
    k->naval_repair_cost_fuel = v[88];
    k->naval_repair_cost_stockpile_share = v[89];
    k->naval_fuel_use_per_hour = v[90];
    k->naval_training_experience_per_hour = v[91];
    k->naval_combat_experience_per_hour = v[92];
    k->naval_raid_convoy_damage = v[93];
    k->naval_raid_control_cut = v[94];
    k->naval_escort_protection = v[95];
    k->naval_max_engagement_ships = v[96];
    k->naval_large_hull_hp = v[97];
    k->naval_base_supply_per_level = v[98];
    k->naval_supply_sea_range_penalty = v[99];
    k->naval_supply_convoy_use_per_capacity = v[100];
    k->naval_supply_raid_threshold = v[101];
    k->naval_invasion_convoys_per_division = v[102];
    k->naval_invasion_hours_per_sea_hop = v[103];
    k->naval_invasion_interception_base = v[104];
    k->naval_invasion_interception_threat_scale = v[105];
    k->naval_invasion_escort_mitigation = v[106];
    k->political_power_per_day = v[107];
    k->focus_progress_speed = v[108];
    k->stability_drift = v[109];
    k->war_support_drift = v[110];
    k->weather_change_chance = v[111];
    k->trade_convoy_use_per_unit = v[112];
    k->trade_factory_cost_per_unit = v[113];
    k->trade_factory_cost_law_scale = v[114];
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
    w.u32(static_cast<uint32_t>(c.focuses.size()));
    for (const FocusDef& f : c.focuses) write_focus(w, f);
    w.u32(static_cast<uint32_t>(c.events.size()));
    for (const EventDef& e : c.events) write_event(w, e);
    w.u32(static_cast<uint32_t>(c.decisions.size()));
    for (const DecisionDef& d : c.decisions) write_decision(w, d);
    w.u32(static_cast<uint32_t>(c.components.size()));
    for (const ComponentDef& comp : c.components) write_component(w, comp);
    w.u32(static_cast<uint32_t>(c.designs.size()));
    for (const EquipmentDesign& d : c.designs) write_design(w, d);
    w.u32(static_cast<uint32_t>(c.spirits.size()));
    for (const SpiritDef& s : c.spirits) write_spirit(w, s);
    w.u32(static_cast<uint32_t>(c.advisors.size()));
    for (const AdvisorDef& a : c.advisors) write_advisor(w, a);
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
    c.focus_index.clear();
    for (size_t i = 0; i < c.focuses.size(); ++i) c.focus_index[c.focuses[i].key] = static_cast<uint32_t>(i);
    c.event_index.clear();
    for (size_t i = 0; i < c.events.size(); ++i) c.event_index[c.events[i].key] = static_cast<uint32_t>(i);
    c.decision_index.clear();
    for (size_t i = 0; i < c.decisions.size(); ++i) {
        c.decision_index[c.decisions[i].key] = static_cast<uint32_t>(i);
    }
    c.design_of_equipment.clear();
    for (size_t i = 0; i < c.designs.size(); ++i) {
        c.design_of_equipment[c.designs[i].produced.v] = static_cast<uint32_t>(i);
    }
    c.component_index.clear();
    for (size_t i = 0; i < c.components.size(); ++i) {
        c.component_index[c.components[i].key] = static_cast<uint32_t>(i);
    }
    c.design_index.clear();
    for (size_t i = 0; i < c.designs.size(); ++i) {
        c.design_index[c.designs[i].key] = static_cast<uint32_t>(i);
    }
    c.spirit_index.clear();
    for (size_t i = 0; i < c.spirits.size(); ++i) c.spirit_index[c.spirits[i].key] = static_cast<uint32_t>(i);
    c.advisor_index.clear();
    for (size_t i = 0; i < c.advisors.size(); ++i) c.advisor_index[c.advisors[i].key] = static_cast<uint32_t>(i);
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
    if (!read_count(r, 32, &n)) return false;
    c->focuses.clear();
    c->focuses.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_focus(r, &c->focuses[i])) return false;
    }
    if (!read_count(r, 24, &n)) return false;
    c->events.clear();
    c->events.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_event(r, &c->events[i])) return false;
    }
    if (!read_count(r, 32, &n)) return false;
    c->decisions.clear();
    c->decisions.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_decision(r, &c->decisions[i])) return false;
    }
    if (!read_count(r, 266, &n)) return false;  // index + 2 strings + enums + 32 doubles + json
    c->components.clear();
    c->components.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_component(r, &c->components[i])) return false;
    }
    if (!read_count(r, 32, &n)) return false;  // index + 2 strings + 4 ids + int + part count
    c->designs.clear();
    c->designs.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_design(r, &c->designs[i])) return false;
    }
    // A design's archetype, fitted components and produced model must all resolve:
    // design_create guarantees it, so a save where they do not is malformed.
    for (const EquipmentDesign& d : c->designs) {
        if (!d.archetype.valid() || d.archetype.v >= c->equipment.size()) return false;
        if (!d.produced.valid() || d.produced.v >= c->equipment.size()) return false;
        for (const std::pair<ComponentSlot, uint32_t>& part : d.components) {
            if (part.second >= c->components.size()) return false;
        }
    }
    if (!read_count(r, 40, &n)) return false;
    c->spirits.clear();
    c->spirits.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_spirit(r, &c->spirits[i])) return false;
    }
    if (!read_count(r, 40, &n)) return false;
    c->advisors.clear();
    c->advisors.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_advisor(r, &c->advisors[i])) return false;
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

// Air wings are entities like divisions: their id is their store slot, so every field
// is written and the store payload carries the alive flags.
void write_air_wing(ByteWriter& w, const AirWing& a) {
    write_id(w, a.id);
    write_id(w, a.country);
    write_id(w, a.equipment);
    w.str(a.name);
    w.i32(a.planes);
    w.i32(a.max_planes);
    write_id(w, a.base);
    write_id(w, a.region);
    write_enum(w, a.mission);
    w.f64(a.efficiency);
    w.f64(a.experience);
    w.i32(a.losses);
    w.u64(a.last_sortie);
}

bool read_air_wing(ByteReader& r, AirWing* a) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    uint32_t equipment = INVALID_ID;
    if (!r.u32(&id)) return false;
    a->id = AirWingId(id);
    if (!r.u32(&country) || !r.u32(&equipment)) return false;
    a->country = CountryId(country);
    a->equipment = EquipmentId(equipment);
    if (!r.str(&a->name)) return false;
    if (!r.i32(&a->planes) || !r.i32(&a->max_planes)) return false;
    uint32_t base = INVALID_ID;
    uint32_t region = INVALID_ID;
    if (!r.u32(&base) || !r.u32(&region)) return false;
    a->base = ProvinceId(base);
    a->region = RegionId(region);
    if (!read_enum(r, &a->mission, static_cast<int>(AirMission::Count))) return false;
    if (!r.f64(&a->efficiency) || !r.f64(&a->experience)) return false;
    if (!r.i32(&a->losses)) return false;
    return r.u64(&a->last_sortie);
}

// The naval entities live in the Military section's "naval" area, next to the land
// and air stores: they are military state keyed by country, and the Country::fleets
// roster in the Countries section names the fleets written here. Ships, task forces
// and fleets are stores (their id is their slot, so the store payload carries the
// alive flags and free list); invasions are a plain vector with no identity.
void write_ship(ByteWriter& w, const Ship& s) {
    write_id(w, s.id);
    write_id(w, s.country);
    write_id(w, s.equipment);
    w.str(s.name);
    write_id(w, s.fleet);
    write_id(w, s.task_force);
    w.f64(s.strength);
    w.f64(s.organisation);
    w.f64(s.experience);
    w.f64(s.fuel);
    write_id(w, s.port);
    write_id(w, s.sea_region);
    w.boolean(s.at_sea);
}

bool read_ship(ByteReader& r, Ship* s) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    uint32_t equipment = INVALID_ID;
    if (!r.u32(&id)) return false;
    s->id = ShipId(id);
    if (!r.u32(&country) || !r.u32(&equipment)) return false;
    s->country = CountryId(country);
    s->equipment = EquipmentId(equipment);
    if (!r.str(&s->name)) return false;
    uint32_t fleet = INVALID_ID;
    uint32_t task_force = INVALID_ID;
    if (!r.u32(&fleet) || !r.u32(&task_force)) return false;
    s->fleet = FleetId(fleet);
    s->task_force = TaskForceId(task_force);
    if (!r.f64(&s->strength) || !r.f64(&s->organisation) || !r.f64(&s->experience)) return false;
    if (!r.f64(&s->fuel)) return false;
    uint32_t port = INVALID_ID;
    uint32_t sea_region = INVALID_ID;
    if (!r.u32(&port) || !r.u32(&sea_region)) return false;
    s->port = ProvinceId(port);
    s->sea_region = RegionId(sea_region);
    return r.boolean(&s->at_sea);
}

void write_task_force(ByteWriter& w, const TaskForce& t) {
    write_id(w, t.id);
    write_id(w, t.country);
    write_id(w, t.fleet);
    w.str(t.name);
    write_ids(w, t.ships);
    write_id(w, t.port);
    write_id(w, t.sea_region);
    write_enum(w, t.mission);
    w.boolean(t.at_sea);
    w.f64(t.detection);
    w.u64(t.last_engagement);
}

bool read_task_force(ByteReader& r, TaskForce* t) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    uint32_t fleet = INVALID_ID;
    if (!r.u32(&id)) return false;
    t->id = TaskForceId(id);
    if (!r.u32(&country) || !r.u32(&fleet)) return false;
    t->country = CountryId(country);
    t->fleet = FleetId(fleet);
    if (!r.str(&t->name)) return false;
    if (!read_ids(r, &t->ships)) return false;
    uint32_t port = INVALID_ID;
    uint32_t sea_region = INVALID_ID;
    if (!r.u32(&port) || !r.u32(&sea_region)) return false;
    t->port = ProvinceId(port);
    t->sea_region = RegionId(sea_region);
    if (!read_enum(r, &t->mission, static_cast<int>(NavalMission::Count))) return false;
    if (!r.boolean(&t->at_sea)) return false;
    if (!r.f64(&t->detection)) return false;
    return r.u64(&t->last_engagement);
}

void write_fleet(ByteWriter& w, const Fleet& f) {
    write_id(w, f.id);
    write_id(w, f.country);
    w.str(f.name);
    write_ids(w, f.task_forces);
}

bool read_fleet(ByteReader& r, Fleet* f) {
    uint32_t id = INVALID_ID;
    uint32_t country = INVALID_ID;
    if (!r.u32(&id)) return false;
    f->id = FleetId(id);
    if (!r.u32(&country)) return false;
    f->country = CountryId(country);
    if (!r.str(&f->name)) return false;
    return read_ids(r, &f->task_forces);
}

void write_invasion(ByteWriter& w, const NavalInvasion& i) {
    write_id(w, i.army);
    write_id(w, i.country);
    write_id(w, i.origin);
    write_id(w, i.target);
    write_id(w, i.sea_region);
    w.f64(i.progress);
    w.u64(i.started);
    w.boolean(i.landed);
}

bool read_invasion(ByteReader& r, NavalInvasion* i) {
    uint32_t army = INVALID_ID;
    uint32_t country = INVALID_ID;
    uint32_t origin = INVALID_ID;
    uint32_t target = INVALID_ID;
    uint32_t sea_region = INVALID_ID;
    if (!r.u32(&army) || !r.u32(&country)) return false;
    i->army = ArmyId(army);
    i->country = CountryId(country);
    if (!r.u32(&origin) || !r.u32(&target) || !r.u32(&sea_region)) return false;
    i->origin = ProvinceId(origin);
    i->target = ProvinceId(target);
    i->sea_region = RegionId(sea_region);
    if (!r.f64(&i->progress)) return false;
    if (!r.u64(&i->started)) return false;
    return r.boolean(&i->landed);
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
    w.f64(d.air_mod);
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
    if (!r.f64(&d->air_mod)) return false;
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
    write_ids(w, c.wings);
    write_ids(w, c.fleets);
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
    if (!read_ids(r, &c->wings)) return false;
    if (!read_ids(r, &c->fleets)) return false;
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

// Trade routes are world-level economic state: they move resources into the
// importer's pool, cost civilian factories and consume convoys, and industry consumes
// what they deliver, so they live in the Economy section next to the per-country
// economy blocks (not Diplomacy: a route is a flow, not a relation). They are written
// in the vector's own (importer, exporter, resource) sorted order, so the payload is
// deterministic. Written before the content snapshot, which stays the last record in
// the section so the drift guard can measure the constants record from the section
// end.
void write_trade_route(ByteWriter& w, const TradeRoute& t) {
    write_id(w, t.importer);
    write_id(w, t.exporter);
    write_enum(w, t.resource);
    w.f64(t.amount);
    w.f64(t.delivered);
    w.boolean(t.sea_route);
    write_id(w, t.sea_region);
    w.f64(t.convoy_use);
    w.f64(t.factory_cost);
    w.boolean(t.active);
}

bool read_trade_route(ByteReader& r, TradeRoute* t) {
    uint32_t importer = INVALID_ID;
    uint32_t exporter = INVALID_ID;
    if (!r.u32(&importer) || !r.u32(&exporter)) return false;
    t->importer = CountryId(importer);
    t->exporter = CountryId(exporter);
    if (!read_enum(r, &t->resource, RESOURCE_COUNT)) return false;
    if (!r.f64(&t->amount) || !r.f64(&t->delivered)) return false;
    if (!r.boolean(&t->sea_route)) return false;
    uint32_t region = INVALID_ID;
    if (!r.u32(&region)) return false;
    t->sea_region = RegionId(region);
    if (!r.f64(&t->convoy_use) || !r.f64(&t->factory_cost)) return false;
    return r.boolean(&t->active);
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

void write_timed_modifier(ByteWriter& w, const TimedModifier& m) {
    w.str(m.source);
    write_modifiers(w, m.mods);
    w.i32(m.days_left);
}

bool read_timed_modifier(ByteReader& r, TimedModifier* m) {
    if (!r.str(&m->source)) return false;
    if (!read_modifiers(r, &m->mods)) return false;
    return r.i32(&m->days_left);
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
    // National focus, pending events and decisions, script flags and the modifiers
    // they grant. This is the country's political state exactly once.
    write_u32s(w, c.completed_focuses);
    w.u32(c.selected_focus);
    w.f64(c.focus_progress);
    write_u32s(w, c.pending_events);
    write_u32s(w, c.fired_events);
    write_u32s(w, c.active_decisions);
    write_f64s(w, c.decision_days_left);
    write_f64s(w, c.decision_cooldown);
    write_strs(w, c.country_flags);
    w.u32(static_cast<uint32_t>(c.timed_modifiers.size()));
    for (const TimedModifier& m : c.timed_modifiers) write_timed_modifier(w, m);
    // The held national spirits (permanent named modifiers, days_left = -1), the
    // Content::spirits / Content::advisors rosters by index, and the slot capacities.
    w.u32(static_cast<uint32_t>(c.national_spirits.size()));
    for (const TimedModifier& m : c.national_spirits) write_timed_modifier(w, m);
    write_u32s(w, c.spirit_keys);
    write_u32s(w, c.advisors);
    // The country's equipment designs, indices into Content::designs (Economy); the
    // same kind of roster as spirit_keys and advisors.
    write_u32s(w, c.designs);
    w.i32(c.spirit_slots);
    w.i32(c.advisor_slots);
}

bool read_country_politics(ByteReader& r, Country* c) {
    if (!r.f64(&c->political_power) || !r.f64(&c->stability) || !r.f64(&c->war_support)) return false;
    if (!r.f64(&c->manpower) || !r.f64(&c->fuel) || !r.f64(&c->fuel_capacity)) return false;
    if (!r.f64(&c->consumer_goods_ratio) || !r.f64(&c->army_experience)) return false;
    if (!r.boolean(&c->at_war) || !r.boolean(&c->fuel_priority)) return false;
    if (!r.u64(&c->last_capitulation_check)) return false;
    if (!read_u32s(r, &c->completed_focuses)) return false;
    if (!r.u32(&c->selected_focus)) return false;
    if (!r.f64(&c->focus_progress)) return false;
    if (!read_u32s(r, &c->pending_events)) return false;
    if (!read_u32s(r, &c->fired_events)) return false;
    if (!read_u32s(r, &c->active_decisions)) return false;
    if (!read_f64s(r, &c->decision_days_left)) return false;
    if (!read_f64s(r, &c->decision_cooldown)) return false;
    if (!read_strs(r, &c->country_flags)) return false;
    uint32_t n = 0;
    if (!read_count(r, 16, &n)) return false;  // empty source + modifiers + days_left
    c->timed_modifiers.clear();
    c->timed_modifiers.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_timed_modifier(r, &c->timed_modifiers[i])) return false;
    }
    if (!read_count(r, 16, &n)) return false;  // empty source + modifiers + days_left
    c->national_spirits.clear();
    c->national_spirits.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!read_timed_modifier(r, &c->national_spirits[i])) return false;
    }
    if (!read_u32s(r, &c->spirit_keys)) return false;
    if (!read_u32s(r, &c->advisors)) return false;
    if (!read_u32s(r, &c->designs)) return false;
    if (!r.i32(&c->spirit_slots) || !r.i32(&c->advisor_slots)) return false;
    return true;
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
            // World-level trade routes, written in their stored sorted order; the
            // content snapshot below must stay the last record in this section.
            w.u32(static_cast<uint32_t>(g.world.trade_routes.size()));
            for (const TradeRoute& t : g.world.trade_routes) write_trade_route(w, t);
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
            write_store(w, g.world.air_wings, write_air_wing);
            // Naval area: ships, task forces, fleets and the invasion list.
            write_store(w, g.world.ships, write_ship);
            write_store(w, g.world.task_forces, write_task_force);
            write_store(w, g.world.fleets, write_fleet);
            w.u32(static_cast<uint32_t>(g.world.invasions.size()));
            for (const NavalInvasion& inv : g.world.invasions) write_invasion(w, inv);
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
            // std::map iteration is ordered: script variables round-trip byte-identically.
            w.u32(static_cast<uint32_t>(g.world.script_vars.size()));
            for (const auto& entry : g.world.script_vars) {
                w.str(entry.first);
                w.f64(entry.second);
            }
            w.u32(static_cast<uint32_t>(g.world.delayed_events.size()));
            for (const DelayedEvent& d : g.world.delayed_events) {
                w.u32(d.country.v);
                w.u32(d.event);
                w.u64(d.due);
            }
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
            if (!read_count(r, 47, &n)) return false;  // trade route: 2 ids + resource
            g.world.trade_routes.clear();
            g.world.trade_routes.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (!read_trade_route(r, &g.world.trade_routes[i])) return false;
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
            if (!read_store(r, g.world.divisions, read_division)) return false;
            if (!read_store(r, g.world.air_wings, read_air_wing)) return false;
            if (!read_store(r, g.world.ships, read_ship)) return false;
            if (!read_store(r, g.world.task_forces, read_task_force)) return false;
            if (!read_store(r, g.world.fleets, read_fleet)) return false;
            if (!read_count(r, 37, &n)) return false;  // invasion: 5 ids + double + tick + bool
            g.world.invasions.clear();
            g.world.invasions.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (!read_invasion(r, &g.world.invasions[i])) return false;
            }
            return true;
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
            if (!read_count(r, 8, &n)) return false;  // name length + value
            g.world.script_vars.clear();
            for (uint32_t i = 0; i < n; ++i) {
                std::string name;
                double value = 0.0;
                if (!r.str(&name) || !r.f64(&value)) return false;
                g.world.script_vars[name] = value;
            }
            if (!read_count(r, 16, &n)) return false;  // country + event + due
            g.world.delayed_events.clear();
            g.world.delayed_events.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                DelayedEvent& d = g.world.delayed_events[i];
                uint32_t country = INVALID_ID;
                if (!r.u32(&country)) return false;
                d.country = CountryId(country);
                if (!r.u32(&d.event) || !r.u64(&d.due)) return false;
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
