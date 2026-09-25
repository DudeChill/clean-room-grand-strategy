// Scenario loading: map file + starting world state (spec section 6 data formats).
//
// A scenario names a map file (relative to its own directory) and the countries,
// industry, armies, factions and wars of the start date. Everything created here
// goes through the same structures the simulation uses later; there is no
// scenario-only state.

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "data/content.h"
#include "game/game.h"
#include "sim/navy.h"
#include "sim/world.h"

namespace hoi {
namespace {

// Path surgery goes through std::filesystem, not string cutting: a Windows path uses
// backslashes ("C:\\dir\\scenario.json"), and a helper that only looks for '/' silently
// resolves the map relative to the working directory instead of the scenario's own folder.
std::string dirname_of(const std::string& path) {
    const std::filesystem::path p(path);
    const std::filesystem::path parent = p.parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

std::string join_path(const std::string& dir, const std::string& rel) {
    const std::filesystem::path r(rel);
    if (r.is_absolute()) return rel;  // "/abs/x" or "C:\\abs\\x"
    if (dir.empty() || dir == ".") return rel;
    return (std::filesystem::path(dir) / r).string();
}

bool parse_date(const std::string& s, GameDate* out) {
    if (s.size() < 10) return false;
    int vals[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        if (pos == start) return false;
        long v = 0;
        for (size_t k = start; k < pos; ++k) v = v * 10 + (s[k] - '0');
        vals[i] = static_cast<int>(v);
        if (i < 2 && (pos >= s.size() || s[pos] != '-')) return false;
        ++pos;
    }
    if (vals[0] < 1 || vals[1] < 1 || vals[1] > 12 || vals[2] < 1 || vals[2] > 31) return false;
    out->year = vals[0];
    out->month = static_cast<uint8_t>(vals[1]);
    out->day = static_cast<uint8_t>(vals[2]);
    out->hour = 0;
    return true;
}

int match_ideology(const std::string& key) {
    // Case/separator-insensitive against the engine names, matching content.cpp.
    std::string want;
    for (char ch : key) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') want.push_back(static_cast<char>(c));
    }
    for (int i = 0; i < static_cast<int>(Ideology::Count); ++i) {
        std::string have;
        for (const char* p = ideology_name(static_cast<Ideology>(i)); *p != '\0'; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c >= 'a' && c <= 'z') have.push_back(static_cast<char>(c));
        }
        if (have == want) return i;
    }
    return -1;
}

int match_terrain(const std::string& key) {
    std::string want;
    for (char ch : key) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') want.push_back(static_cast<char>(c));
    }
    for (int i = 0; i < static_cast<int>(Terrain::Count); ++i) {
        std::string have;
        for (const char* p = terrain_name(static_cast<Terrain>(i)); *p != '\0'; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c >= 'a' && c <= 'z') have.push_back(static_cast<char>(c));
        }
        if (have == want) return i;
    }
    return -1;
}

int match_resource(const std::string& key) {
    std::string want;
    for (char ch : key) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') want.push_back(static_cast<char>(c));
    }
    for (int i = 0; i < RESOURCE_COUNT; ++i) {
        std::string have;
        for (const char* p = resource_name(static_cast<Resource>(i)); *p != '\0'; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c >= 'a' && c <= 'z') have.push_back(static_cast<char>(c));
        }
        if (have == want) return i;
    }
    return -1;
}

// Distributes `count` factories of a kind across `states` in list order, never
// exceeding a state's building slots. Deterministic round robin.
template <typename GetCount, typename GetSlot, typename AddOne>
void distribute_factories(int count, const std::vector<StateId>& states, GetCount total,
                          GetSlot slots, AddOne add) {
    if (count <= 0 || states.empty()) return;
    size_t i = 0;
    int guard = 0;
    const int guard_cap = static_cast<int>(states.size()) * 16 + 16;
    while (count > 0 && guard++ < guard_cap) {
        const StateId sid = states[i % states.size()];
        if (total(sid) < slots(sid)) {
            add(sid);
            --count;
        }
        ++i;
    }
}

struct MapKeyIndex {
    std::map<std::string, RegionId> regions;
    std::map<std::string, StateId> states;
    std::map<std::string, ProvinceId> provinces;
};

// Best land province of a state: highest victory points, then population, then
// lowest id, so the pick never depends on container order. With `coastal_only` the
// search skips provinces that do not touch a sea zone.
ProvinceId best_land_province(const World& w, const State& state, bool coastal_only) {
    ProvinceId best;
    int best_vp = -1;
    double best_pop = -1.0;
    for (ProvinceId pid : state.provinces) {
        const Province* prov = w.provinces.try_get(pid);
        if (prov == nullptr || prov->is_sea) continue;
        if (coastal_only && !prov->coastal) continue;
        const bool better =
            prov->victory_points > best_vp ||
            (prov->victory_points == best_vp && prov->population > best_pop) ||
            (prov->victory_points == best_vp && prov->population == best_pop &&
             (!best.valid() || pid.v < best.v));
        if (better) {
            best_vp = prov->victory_points;
            best_pop = prov->population;
            best = pid;
        }
    }
    return best;
}

// The port a country bases its navy at, derived from the loaded world so no
// scenario has to hand-maintain a province list: the coastal capital province when
// the capital reaches the sea, otherwise the best coastal province of the country's
// largest coastal state (most factories, lowest state id on a tie).
ProvinceId main_naval_port(const World& w, CountryId cid) {
    const Country* country = w.country(cid);
    if (country == nullptr) return ProvinceId{};
    const State* capital = w.state(country->capital);
    if (capital != nullptr) {
        const ProvinceId best = best_land_province(w, *capital, true);
        if (best.valid()) return best;
    }
    StateId largest;
    int largest_factories = -1;
    w.states.for_each([&](StateId sid, const State& state) {
        if (state.owner != cid) return;
        if (!best_land_province(w, state, true).valid()) return;
        if (state.total_factories() > largest_factories) {
            largest_factories = state.total_factories();
            largest = sid;
        }
    });
    const State* big = w.state(largest);
    return big != nullptr ? best_land_province(w, *big, true) : ProvinceId{};
}

// Case/separator-insensitive match of a scenario mission name against the engine's
// NavalMission names, mirroring the other matchers above.
int match_naval_mission(const std::string& key) {
    std::string want;
    for (char ch : key) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') want.push_back(static_cast<char>(c));
    }
    for (int i = 0; i < static_cast<int>(NavalMission::Count); ++i) {
        std::string have;
        for (const char* p = naval_mission_name(static_cast<NavalMission>(i)); *p != '\0'; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
            if (c >= 'a' && c <= 'z') have.push_back(static_cast<char>(c));
        }
        if (have == want) return i;
    }
    return -1;
}

}  // namespace

bool load_scenario(const std::string& scenario_path, Content& content, World* world,
                   std::string* err) {
    if (world == nullptr) {
        if (err) *err = "load_scenario: null world";
        return false;
    }
    std::string fatal;
    // Non-fatal data problems (an unknown content key in the scenario) are reported
    // without failing the load: the rest of the scenario is still playable.
    std::vector<std::string> warnings;
    auto fail = [&](const std::string& msg) {
        if (fatal.empty()) fatal = msg;
        return false;
    };

    // ---- scenario file ---------------------------------------------------
    Json scenario;
    std::string parse_err;
    if (!Json::parse_file(scenario_path, &scenario, &parse_err)) {
        return fail(scenario_path + ": " + (parse_err.empty() ? "cannot read file" : parse_err));
    }
    const std::string map_rel = scenario["map"].as_string();
    if (map_rel.empty()) return fail(scenario_path + ": missing \"map\" key");
    // The map is resolved relative to the scenario's own directory; because the
    // shipped layout keeps scenarios next to maps in the data root, a missing
    // first candidate is retried one directory up before giving up.
    std::string map_path = join_path(dirname_of(scenario_path), map_rel);
    Json map;
    if (!Json::parse_file(map_path, &map, &parse_err)) {
        const std::string alt = join_path(dirname_of(scenario_path), "../" + map_rel);
        if (alt == map_path || !Json::parse_file(alt, &map, &parse_err)) {
            return fail(map_path + ": " + (parse_err.empty() ? "cannot read file" : parse_err));
        }
        map_path = alt;
    }
    const Json& jregions = map["regions"];
    const Json& jstates = map["states"];
    const Json& jprovinces = map["provinces"];
    if (!jregions.is_array() || !jstates.is_array() || !jprovinces.is_array()) {
        return fail(map_path + ": expected regions/states/provinces arrays");
    }

    MapKeyIndex index;

    // ---- regions ---------------------------------------------------------
    for (size_t i = 0; i < jregions.size(); ++i) {
        const Json& r = jregions[i];
        const std::string key = r["key"].as_string();
        if (key.empty()) return fail(map_path + ": region entry without key");
        if (index.regions.count(key) != 0) return fail(map_path + ": duplicate region " + key);
        Region region;
        region.name = r["name"].as_string(key);
        region.is_sea = r["is_sea"].as_bool(false);
        const RegionId id = world->regions.create(region);
        world->regions[id].id = id;
        index.regions[key] = id;
    }

    // ---- states ----------------------------------------------------------
    for (size_t i = 0; i < jstates.size(); ++i) {
        const Json& s = jstates[i];
        const std::string key = s["key"].as_string();
        if (key.empty()) return fail(map_path + ": state entry without key");
        if (index.states.count(key) != 0) return fail(map_path + ": duplicate state " + key);
        State state;
        state.name = s["name"].as_string(key);
        state.building_slots = static_cast<int>(s["building_slots"].as_int(0));
        const std::string region_key = s["region"].as_string();
        auto rit = index.regions.find(region_key);
        if (rit != index.regions.end()) {
            state.region = rit->second;
        } else if (!region_key.empty()) {
            return fail(map_path + ": state " + key + " references unknown region " + region_key);
        }
        const StateId id = world->states.create(state);
        world->states[id].id = id;
        index.states[key] = id;
    }

    // ---- provinces -------------------------------------------------------
    for (size_t i = 0; i < jprovinces.size(); ++i) {
        const Json& p = jprovinces[i];
        const std::string key = p["key"].as_string();
        if (key.empty()) return fail(map_path + ": province entry without key");
        if (index.provinces.count(key) != 0) return fail(map_path + ": duplicate province " + key);
        Province prov;
        prov.name = p["name"].as_string(key);
        prov.is_sea = p["is_sea"].as_bool(false);
        const std::string terrain_key = p["terrain"].as_string("plains");
        const int terrain = match_terrain(terrain_key);
        if (terrain < 0) return fail(map_path + ": province " + key + ": unknown terrain " + terrain_key);
        prov.terrain = static_cast<Terrain>(terrain);
        prov.x = static_cast<int>(p["x"].as_int(0));
        prov.y = static_cast<int>(p["y"].as_int(0));
        prov.victory_points = static_cast<int>(p["victory_points"].as_int(0));
        prov.infrastructure = static_cast<int>(p["infrastructure"].as_int(3));
        prov.population = p["population"].as_double(0.0);
        prov.supply_hub = p["supply_hub"].as_bool(false);
        prov.railway_level = static_cast<int>(p["railway_level"].as_int(0));
        // Air base level is a province attribute; the scenario can also raise it
        // below (the capital and largest-industrial-state guarantees).
        prov.air_base = static_cast<int>(p["air_base"].as_int(0));
        const Json& resources = p["resources"];
        if (resources.is_object()) {
            for (const auto& item : resources.object_items()) {
                const int idx = match_resource(item.first);
                if (idx < 0) {
                    return fail(map_path + ": province " + key + ": unknown resource " + item.first);
                }
                prov.resource_yield[idx] = item.second.as_double(0.0);
            }
        }
        const std::string state_key = p["state"].as_string();
        auto sit = index.states.find(state_key);
        if (sit != index.states.end()) {
            prov.state = sit->second;
        } else if (!state_key.empty()) {
            return fail(map_path + ": province " + key + " references unknown state " + state_key);
        }
        const std::string region_key = p["region"].as_string();
        auto rit = index.regions.find(region_key);
        if (rit != index.regions.end()) {
            prov.region = rit->second;
        } else if (!region_key.empty()) {
            return fail(map_path + ": province " + key + " references unknown region " + region_key);
        }
        const ProvinceId id = world->provinces.create(prov);
        world->provinces[id].id = id;
        index.provinces[key] = id;
    }

    // ---- adjacency -------------------------------------------------------
    // The generator emits one adjacency list per province containing both land and
    // sea neighbours; the loader splits it by the neighbour's is_sea flag.
    for (size_t i = 0; i < jprovinces.size(); ++i) {
        const Json& p = jprovinces[i];
        const std::string key = p["key"].as_string();
        const ProvinceId id = index.provinces.at(key);
        Province& prov = world->provinces[id];
        const Json& adj = p["adj"];
        if (!adj.is_array()) continue;
        for (size_t k = 0; k < adj.size(); ++k) {
            const std::string nk = adj[k].as_string();
            auto it = index.provinces.find(nk);
            if (it == index.provinces.end()) {
                return fail(map_path + ": province " + key + " adjacency references unknown " + nk);
            }
            if (it->second == id) {
                return fail(map_path + ": province " + key + " lists itself as a neighbour");
            }
            const Province& other = world->provinces[it->second];
            if (prov.is_sea) {
                // Sea zones keep the full generated neighbour list (land + sea):
                // land systems never start from a sea zone, but naval code wants
                // the complete coastline relationship in one place.
                prov.adj.push_back(it->second);
                if (other.is_sea) prov.sea_adj.push_back(it->second);
            } else if (other.is_sea) {
                // Land adjacency is land-only; sea neighbours are sea zones.
                prov.sea_adj.push_back(it->second);
            } else {
                prov.adj.push_back(it->second);
            }
        }
        std::sort(prov.adj.begin(), prov.adj.end());
        prov.adj.erase(std::unique(prov.adj.begin(), prov.adj.end()), prov.adj.end());
        std::sort(prov.sea_adj.begin(), prov.sea_adj.end());
        prov.sea_adj.erase(std::unique(prov.sea_adj.begin(), prov.sea_adj.end()),
                           prov.sea_adj.end());
        if (!prov.is_sea) prov.coastal = !prov.sea_adj.empty();
    }

    // Land adjacency must be symmetric; a violation means the map file is broken.
    {
        int violations = 0;
        std::string first_violation;
        world->provinces.for_each([&](ProvinceId id, const Province& prov) {
            if (prov.is_sea) return;
            for (ProvinceId nb : prov.adj) {
                const Province* other = world->provinces.try_get(nb);
                if (other == nullptr || other->is_sea) {
                    ++violations;
                    if (first_violation.empty()) first_violation = "non-land neighbour";
                    continue;
                }
                if (std::find(other->adj.begin(), other->adj.end(), id) == other->adj.end()) {
                    ++violations;
                    if (first_violation.empty()) first_violation = "asymmetric edge";
                }
            }
        });
        if (violations > 0) {
            return fail(map_path + ": " + std::to_string(violations) +
                        " adjacency violation(s), first: " + first_violation);
        }
    }

    // Region and state membership, derived from the province table.
    world->states.for_each([&](StateId, State& state) {
        state.provinces.clear();
    });
    world->regions.for_each([&](RegionId, Region& region) {
        region.provinces.clear();
    });
    world->provinces.for_each([&](ProvinceId id, const Province& prov) {
        State* state = world->states.try_get(prov.state);
        if (state != nullptr) state->provinces.push_back(id);
        Region* region = world->regions.try_get(prov.region);
        if (region != nullptr) region->provinces.push_back(id);
    });

    // ---- world header ----------------------------------------------------
    world->tick = 0;
    GameDate start;
    const std::string start_str = scenario["start_date"].as_string("1936-01-01");
    if (!parse_date(start_str, &start)) {
        return fail(scenario_path + ": invalid start_date " + start_str);
    }
    world->date = start;
    world->world_seed = static_cast<uint64_t>(scenario["seed"].as_int(12345));

    // ---- countries -------------------------------------------------------
    std::map<std::string, CountryId> by_tag;
    const Json& jcountries = scenario["countries"];
    if (!jcountries.is_array()) return fail(scenario_path + ": expected a countries array");

    // Law kind count decides the size of every country's law_levels vector.
    int law_kinds = 0;
    for (const LawDef& law : content.laws) law_kinds = std::max(law_kinds, law.kind + 1);
    // The scenario's template clones are appended to content.templates, so the
    // library size is captured before any country is processed.
    const size_t library_templates = content.templates.size();

    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const std::string tag = c["tag"].as_string();
        if (tag.empty()) return fail(scenario_path + ": country entry without tag");
        if (by_tag.count(tag) != 0) return fail(scenario_path + ": duplicate country tag " + tag);

        Country country;
        country.tag = tag;
        country.name = c["name"].as_string(tag);
        const std::string ideology_key = c["ideology"].as_string("neutrality");
        const int ideology = match_ideology(ideology_key);
        if (ideology < 0) {
            return fail(scenario_path + ": country " + tag + ": unknown ideology " + ideology_key);
        }
        country.ideology = static_cast<Ideology>(ideology);
        country.law_levels.assign(static_cast<size_t>(law_kinds), 0);
        country.equipment_stockpile.assign(content.equipment.size(), 0.0);
        country.political_power = c["political_power"].as_double(country.political_power);
        country.stability = c["stability"].as_double(country.stability);
        country.war_support = c["war_support"].as_double(country.war_support);
        // Research slots must exist before the first tick, otherwise StartResearch
        // has nowhere to put a technology.
        const int unlocked = country.research.slots_unlocked > 0 ? country.research.slots_unlocked : 0;
        country.research.slots.assign(static_cast<size_t>(unlocked), ResearchSlot{});
        const CountryId id = world->countries.create(country);
        world->countries[id].id = id;
        by_tag[tag] = id;
    }

    // ---- territorial ownership -------------------------------------------
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        Country& country = world->countries[cid];

        const Json& states = c["states"];
        if (states.is_array()) {
            for (size_t k = 0; k < states.size(); ++k) {
                const std::string sk = states[k].as_string();
                auto sit = index.states.find(sk);
                if (sit == index.states.end()) {
                    return fail(scenario_path + ": country " + country.tag +
                                " references unknown state " + sk);
                }
                State& state = world->states[sit->second];
                state.owner = cid;
                state.controller = cid;
                if (std::find(state.core_owners.begin(), state.core_owners.end(), cid) ==
                    state.core_owners.end()) {
                    state.core_owners.push_back(cid);
                }
                for (ProvinceId pid : state.provinces) {
                    Province& prov = world->provinces[pid];
                    if (prov.is_sea) continue;
                    prov.owner = cid;
                    prov.controller = cid;
                }
            }
        }

        const std::string cap_key = c["capital_state"].as_string();
        if (!cap_key.empty()) {
            auto sit = index.states.find(cap_key);
            if (sit == index.states.end()) {
                return fail(scenario_path + ": country " + country.tag +
                            " references unknown capital_state " + cap_key);
            }
            State& capital = world->states[sit->second];
            if (!capital.owner.valid()) {
                return fail(scenario_path + ": country " + country.tag +
                            " capital state " + cap_key + " is not owned by it");
            }
            country.capital = sit->second;
            // The capital province is the highest-VP province of the capital state.
            ProvinceId best;
            int best_vp = -1;
            for (ProvinceId pid : capital.provinces) {
                const Province& prov = world->provinces[pid];
                if (prov.is_sea) continue;
                if (prov.victory_points > best_vp) {
                    best_vp = prov.victory_points;
                    best = pid;
                }
            }
            if (best.valid()) {
                world->provinces[best].is_capital = true;
            } else {
                return fail(scenario_path + ": country " + country.tag +
                            " capital state has no land province");
            }
        } else {
            return fail(scenario_path + ": country " + country.tag + " has no capital_state");
        }
    }

    // ---- starting industry ------------------------------------------------
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        Country& country = world->countries[cid];
        const int civ = static_cast<int>(c["civilian_factories"].as_int(0));
        const int mil = static_cast<int>(c["military_factories"].as_int(0));
        const int dock = static_cast<int>(c["dockyards"].as_int(0));

        std::vector<StateId> states;
        const Json& jstates_c = c["states"];
        if (jstates_c.is_array()) {
            for (size_t k = 0; k < jstates_c.size(); ++k) {
                auto sit = index.states.find(jstates_c[k].as_string());
                if (sit != index.states.end()) states.push_back(sit->second);
            }
        }
        auto slot_of = [&](StateId sid) {
            const State* s = world->states.try_get(sid);
            return s != nullptr ? s->building_slots : 0;
        };
        auto total_of = [&](StateId sid) {
            const State* s = world->states.try_get(sid);
            return s != nullptr ? s->total_factories() : 0;
        };
        // Military industry is placed first: a scenario's production lines must
        // always have factories behind them, and a state can run out of building
        // slots.
        distribute_factories(mil, states, total_of, slot_of, [&](StateId sid) {
            world->states[sid].military_factories += 1;
        });
        distribute_factories(civ, states, total_of, slot_of, [&](StateId sid) {
            world->states[sid].civilian_factories += 1;
        });
        distribute_factories(dock, states, total_of, slot_of, [&](StateId sid) {
            world->states[sid].dockyards += 1;
        });

        // Capitulation thresholds and production lines are based on what actually
        // fits into the country's states, not on what the scenario asked for.
        int placed_military = 0;
        int placed_dockyards = 0;
        int placed_total = 0;
        for (StateId sid : states) {
            const State* s = world->states.try_get(sid);
            if (s == nullptr) continue;
            placed_military += s->military_factories;
            placed_dockyards += s->dockyards;
            placed_total += s->total_factories();
        }
        country.starting_factories = placed_total;

        // Starting production lines. Ships and convoys draw on dockyards, everything
        // else on military factories (the one factory-pool rule the command layer and
        // the industry phase use); the clamp keeps each pool's sum honest so the
        // "unassigned factories" figure in the UI starts at zero for a planned
        // scenario.
        const Json& jlines = c["production_lines"];
        if (jlines.is_array()) {
            int military_left = placed_military;
            int dockyards_left = placed_dockyards;
            for (size_t k = 0; k < jlines.size(); ++k) {
                const Json& entry = jlines[k];
                const std::string eq_key = entry["equipment"].as_string();
                const EquipmentId eq = content.equipment_id(eq_key);
                if (!eq.valid()) {
                    if (!eq_key.empty()) {
                        warnings.push_back(scenario_path + ": country " + country.tag +
                                           ": production_lines references unknown equipment " +
                                           eq_key);
                    }
                    continue;
                }
                const bool dockyard =
                    equipment_factory_pool(content, eq) == FactoryPool::Dockyard;
                int& factories_left = dockyard ? dockyards_left : military_left;
                int factories = static_cast<int>(entry["factories"].as_int(0));
                if (factories > factories_left) factories = factories_left;
                if (factories <= 0) continue;
                ProductionLine line;
                line.equipment = eq;
                line.factories = factories;
                line.efficiency = content.constants.efficiency_start;
                line.efficiency_cap = content.constants.efficiency_cap_base;
                line.started = 0;
                country.lines.push_back(line);
                factories_left -= factories;
            }
        }
    }

    // ---- air bases -------------------------------------------------------
    // A province may declare an air base level directly in the map. On top of any
    // declared levels, every country is guaranteed a usable air arm at scenario
    // start: its capital province hosts at least a level-2 air base, and the
    // province of its largest industrial state at least a level-1 base. Both are
    // derived from data (capital_state and the factories placed above), so any
    // scenario with a capital and industry gets the same treatment and wings can be
    // formed from the first tick.
    {
        world->countries.for_each([&](CountryId cid, const Country& country) {
            const State* capital = world->states.try_get(country.capital);
            if (capital != nullptr) {
                Province* p = world->provinces.try_get(best_land_province(*world, *capital, false));
                if (p != nullptr && p->air_base < 2) p->air_base = 2;
            }
            StateId largest;
            int largest_factories = -1;
            world->states.for_each([&](StateId sid, const State& state) {
                if (state.owner != cid) return;
                if (state.total_factories() > largest_factories) {
                    largest_factories = state.total_factories();
                    largest = sid;
                }
            });
            const State* big = world->states.try_get(largest);
            if (big != nullptr) {
                Province* p = world->provinces.try_get(best_land_province(*world, *big, false));
                if (p != nullptr && p->air_base < 1) p->air_base = 1;
            }
        });
    }

    // ---- naval bases -----------------------------------------------------
    // Ports are derived from the map, never hand-listed in the scenario. A coastal
    // capital and the derived main naval port get a level-2 base (so a starting task
    // force fits: capacity is level * capacity per level), and every country whose
    // largest coastal state is elsewhere still gets a level-1 base in it.
    world->countries.for_each([&](CountryId cid, const Country& country) {
        Province* port = world->provinces.try_get(main_naval_port(*world, cid));
        if (port != nullptr && port->coastal && port->naval_base < 2) port->naval_base = 2;
        StateId largest;
        int largest_factories = -1;
        world->states.for_each([&](StateId sid, const State& state) {
            if (state.owner != cid) return;
            if (!best_land_province(*world, state, true).valid()) return;
            if (state.total_factories() > largest_factories) {
                largest_factories = state.total_factories();
                largest = sid;
            }
        });
        const State* big = world->states.try_get(largest);
        if (big != nullptr) {
            const ProvinceId best = best_land_province(*world, *big, true);
            Province* p = world->provinces.try_get(best);
            if (p != nullptr && p->naval_base < 1) p->naval_base = 1;
        }
    });

    // ---- starting research, laws, stockpile -------------------------------
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        Country& country = world->countries[cid];

        const Json& techs = c["technologies"];
        if (techs.is_array()) {
            for (size_t k = 0; k < techs.size(); ++k) {
                const std::string tk = techs[k].as_string();
                const TechId tid = content.tech_id(tk);
                if (!tid.valid()) {
                    warnings.push_back(scenario_path + ": country " + country.tag +
                                       ": unknown technology " + tk);
                    continue;
                }
                if (!country.research.has_tech(tid)) {
                    country.research.completed.push_back(tid);
                }
                const TechDef* def = content.tech_def(tid);
                if (def != nullptr) country.tech_modifiers.add(def->modifiers);
                // Equipment/buildings unlocked by a completed tech are derived from
                // ResearchState::completed by research.h, so nothing else to copy.
            }
        }

        const Json& laws = c["laws"];
        if (laws.is_array()) {
            for (size_t k = 0; k < laws.size(); ++k) {
                const std::string lk = laws[k].as_string();
                const LawDef* def = content.law(lk);
                if (def == nullptr) {
                    warnings.push_back(scenario_path + ": country " + country.tag +
                                       ": unknown law " + lk);
                    continue;
                }
                if (def->kind >= 0 && static_cast<size_t>(def->kind) < country.law_levels.size()) {
                    country.law_levels[static_cast<size_t>(def->kind)] =
                        static_cast<uint8_t>(def->level);
                }
                country.law_modifiers.add(def->modifiers);
            }
        }

        // Starting national spirits. The scenario lists content keys; each is
        // resolved through the spirit index and added with the same bookkeeping the
        // AddNationalSpirit command uses (the content index plus one permanent
        // TimedModifier keyed by the spirit's own source key). Effects are not run
        // in this pass, so scenario starts use spirits whose value is entirely in
        // their modifiers.
        const Json& spirits = c["spirits"];
        if (spirits.is_array()) {
            for (size_t k = 0; k < spirits.size(); ++k) {
                const std::string skey = spirits[k].as_string();
                const uint32_t sidx = content.spirit_id(skey);
                const SpiritDef* def = content.spirit(sidx);
                if (def == nullptr) {
                    warnings.push_back(scenario_path + ": country " + country.tag +
                                       ": unknown national spirit " + skey);
                    continue;
                }
                if (std::find(country.spirit_keys.begin(), country.spirit_keys.end(), sidx) !=
                    country.spirit_keys.end()) {
                    continue;
                }
                // Same slot invariant spirit_add enforces: a scenario cannot stack
                // more spirit slots than the country's capacity.
                int used_slots = 0;
                for (uint32_t held : country.spirit_keys) {
                    const SpiritDef* held_def = content.spirit(held);
                    if (held_def != nullptr) used_slots += held_def->slots;
                }
                if (used_slots + def->slots > country.spirit_slots) {
                    warnings.push_back(scenario_path + ": country " + country.tag +
                                       ": no free spirit slots for " + def->key);
                    continue;
                }
                country.spirit_keys.push_back(sidx);
                TimedModifier tm;
                tm.source = def->key;
                tm.mods = def->modifiers;
                tm.days_left = -1;
                country.national_spirits.push_back(std::move(tm));
            }
        }

        const Json& stock = c["stockpile"];
        if (stock.is_object()) {
            for (const auto& item : stock.object_items()) {
                const EquipmentId eq = content.equipment_id(item.first);
                if (!eq.valid()) {
                    warnings.push_back(scenario_path + ": country " + country.tag +
                                       ": unknown equipment " + item.first);
                    continue;
                }
                if (eq.v < country.equipment_stockpile.size()) {
                    country.equipment_stockpile[eq.v] += item.second.as_double(0.0);
                }
            }
        }
    }

    // ---- starting armies --------------------------------------------------
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        Country& country = world->countries[cid];

        // Country-owned copies of the template library. The command layer requires
        // template->country == issuer, and the library entries stay resolvable by
        // their plain keys, so each country gets clones keyed "<TAG>_<key>".
        std::vector<TemplateId> owned;
        owned.reserve(library_templates);
        for (size_t ti = 0; ti < library_templates; ++ti) {
            DivisionTemplate clone = content.templates[ti];
            clone.country = cid;
            clone.key = country.tag + "_" + clone.key;
            auto existing = content.template_by_key.find(clone.key);
            if (existing != content.template_by_key.end()) {
                owned.push_back(existing->second);
                continue;
            }
            clone.id = TemplateId(static_cast<uint32_t>(content.templates.size()));
            const TemplateId clone_id = clone.id;
            const std::string clone_key = clone.key;
            content.templates.push_back(std::move(clone));
            content.template_by_key[clone_key] = clone_id;
            owned.push_back(clone_id);
        }
        country.templates = owned;

        const Json& divisions = c["divisions"];
        if (!divisions.is_array()) continue;

        Army army;
        army.country = cid;
        army.name = country.tag + " Field Army";
        const ArmyId army_id = world->armies.create(army);
        world->armies[army_id].id = army_id;

        Character general;
        general.country = cid;
        general.name = country.name + " Staff";
        general.is_general = true;
        general.skill = 2;
        general.attack = 2;
        general.defense = 2;
        general.planning = 2;
        general.logistics = 2;
        general.army = army_id;
        const CharacterId general_id = world->characters.create(general);
        world->characters[general_id].id = general_id;
        world->armies[army_id].general = general_id;
        country.generals.push_back(general_id);

        int ordinal = 0;
        for (size_t k = 0; k < divisions.size(); ++k) {
            const Json& d = divisions[k];
            const std::string tkey = d["template"].as_string();
            // Divisions use the country's own copy of the library template.
            TemplateId tid = content.template_id(country.tag + "_" + tkey);
            if (!tid.valid()) tid = content.template_id(tkey);
            if (!tid.valid()) {
                return fail(scenario_path + ": country " + country.tag +
                            " references unknown template " + tkey);
            }
            const DivisionTemplate* tmpl = content.template_def(tid);
            if (tmpl == nullptr) {
                return fail(scenario_path + ": template " + tkey + " has no definition");
            }
            const std::string pk = d["province"].as_string();
            auto pit = index.provinces.find(pk);
            if (pit == index.provinces.end()) {
                return fail(scenario_path + ": country " + country.tag +
                            " references unknown province " + pk);
            }
            if (world->provinces[pit->second].is_sea) {
                return fail(scenario_path + ": country " + country.tag +
                            " deploys a division into sea province " + pk);
            }
            if (world->provinces[pit->second].controller != cid) {
                return fail(scenario_path + ": country " + country.tag +
                            " deploys a division into province " + pk +
                            " which it does not control");
            }
            const int count = static_cast<int>(d["count"].as_int(1));
            for (int n = 0; n < count; ++n) {
                Division div;
                div.country = cid;
                div.template_id = tid;
                div.name = country.tag + " " + tmpl->name + " " + std::to_string(++ordinal);
                div.location = pit->second;
                div.organization = tmpl->max_organization;
                div.max_organization = tmpl->max_organization;
                div.strength = 1.0;
                div.supply = 1.0;
                div.fuel = 1.0;
                div.manpower = tmpl->manpower;
                div.equipment.assign(content.equipment.size(), 0.0);
                // Equipment is counted in battalions, matching the deficit model in
                // sim/industry.cpp: a template needing 9 infantry battalions wants
                // 9 units of that equipment.
                for (const BattalionSlot& slot : tmpl->battalions) {
                    if (!slot.equipment.valid()) continue;
                    if (slot.equipment.v >= div.equipment.size()) continue;
                    div.equipment[slot.equipment.v] += static_cast<double>(slot.count);
                }
                div.army = army_id;
                const DivisionId did = world->divisions.create(div);
                world->divisions[did].id = did;
                country.divisions.push_back(did);
                world->armies[army_id].divisions.push_back(did);
            }
        }
        country.armies.push_back(army_id);
    }

    // ---- starting air wings ----------------------------------------------
    // A country may list starting wings. Aircraft are drawn from the country's
    // stockpile exactly as CreateAirWing does (through the same stockpile draw), so
    // a wing is never created with planes that do not exist.
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        Country& country = world->countries[cid];
        const Json& jwings = c["wings"];
        if (!jwings.is_array()) continue;
        for (size_t k = 0; k < jwings.size(); ++k) {
            const Json& wj = jwings[k];
            const std::string eq_key = wj["equipment"].as_string();
            const EquipmentId eq = content.equipment_id(eq_key);
            const EquipmentDef* def = content.equipment_def(eq);
            if (def == nullptr || def->is_archetype ||
                def->category != EquipmentCategory::Aircraft) {
                warnings.push_back(scenario_path + ": country " + country.tag +
                                   ": wing equipment " + eq_key +
                                   " is not an aircraft model");
                continue;
            }
            const std::string pk = wj["province"].as_string();
            auto pit = index.provinces.find(pk);
            const Province* base =
                pit == index.provinces.end() ? nullptr : world->provinces.try_get(pit->second);
            if (base == nullptr || base->is_sea || base->controller != cid) {
                warnings.push_back(scenario_path + ": country " + country.tag +
                                   ": wing base " + pk + " is not controlled by it");
                continue;
            }
            const int want = static_cast<int>(wj["planes"].as_int(0));
            if (want <= 0) {
                warnings.push_back(scenario_path + ": country " + country.tag +
                                   ": wing has non-positive planes");
                continue;
            }
            // Draw the aircraft from the stockpile (the same operation
            // reinforce_air_wing performs for a command-created wing).
            int delivered = want;
            if (eq.v < country.equipment_stockpile.size()) {
                const double stock = country.equipment_stockpile[eq.v];
                if (stock < static_cast<double>(delivered)) {
                    delivered = static_cast<int>(stock);
                }
                country.equipment_stockpile[eq.v] = stock - delivered;
            } else {
                delivered = 0;
            }
            if (delivered <= 0) {
                warnings.push_back(scenario_path + ": country " + country.tag +
                                   ": no " + eq_key + " in stockpile for a starting wing");
                continue;
            }
            if (delivered < want) {
                warnings.push_back(scenario_path + ": country " + country.tag +
                                   ": only " + std::to_string(delivered) + " of " +
                                   std::to_string(want) + " " + eq_key +
                                   " available for a starting wing");
            }
            AirWing wing;
            wing.country = cid;
            wing.equipment = eq;
            wing.base = pit->second;
            wing.region = base->region;
            wing.max_planes = want;
            wing.planes = delivered;
            wing.mission = AirMission::AirSuperiority;
            wing.name =
                def->name + " wing " + std::to_string(country.wings.size() + 1);
            const AirWingId id = world->air_wings.create(wing);
            world->air_wings[id].id = id;
            country.wings.push_back(id);
        }
    }

    // ---- puppets and overlords --------------------------------------------
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const CountryId cid = by_tag.at(c["tag"].as_string());
        const Json& puppets = c["puppets"];
        if (puppets.is_array()) {
            for (size_t k = 0; k < puppets.size(); ++k) {
                const std::string pt = puppets[k].as_string();
                auto it = by_tag.find(pt);
                if (it == by_tag.end()) {
                    return fail(scenario_path + ": country " + c["tag"].as_string() +
                                " lists unknown puppet " + pt);
                }
                world->countries[it->second].overlord = cid;
                world->countries[cid].puppets.push_back(it->second);
            }
        }
        const std::string overlord_tag = c["overlord"].as_string();
        if (!overlord_tag.empty()) {
            auto it = by_tag.find(overlord_tag);
            if (it == by_tag.end()) {
                return fail(scenario_path + ": country " + c["tag"].as_string() +
                            " lists unknown overlord " + overlord_tag);
            }
            world->countries[cid].overlord = it->second;
            world->countries[it->second].puppets.push_back(cid);
        }
    }

    // ---- factions ---------------------------------------------------------
    const Json& jfactions = scenario["factions"];
    if (jfactions.is_array()) {
        for (size_t i = 0; i < jfactions.size(); ++i) {
            const Json& f = jfactions[i];
            Faction faction;
            faction.id = static_cast<uint32_t>(i + 1);  // 0 means "no faction"
            faction.name = f["name"].as_string("Faction");
            const std::string leader_tag = f["leader"].as_string();
            auto lit = by_tag.find(leader_tag);
            if (lit == by_tag.end()) {
                return fail(scenario_path + ": faction " + faction.name +
                            " has unknown leader " + leader_tag);
            }
            faction.leader = lit->second;
            const Json& members = f["members"];
            if (members.is_array()) {
                for (size_t k = 0; k < members.size(); ++k) {
                    const std::string mt = members[k].as_string();
                    auto it = by_tag.find(mt);
                    if (it == by_tag.end()) {
                        return fail(scenario_path + ": faction " + faction.name +
                                    " has unknown member " + mt);
                    }
                    faction.members.push_back(it->second);
                }
            }
            if (std::find(faction.members.begin(), faction.members.end(), faction.leader) ==
                faction.members.end()) {
                faction.members.push_back(faction.leader);
            }
            for (CountryId member : faction.members) {
                world->countries[member].faction = faction.id;
            }
            world->factions.push_back(std::move(faction));
        }
    }

    // ---- wars -------------------------------------------------------------
    const Json& jwars = scenario["wars"];
    if (jwars.is_array()) {
        for (size_t i = 0; i < jwars.size(); ++i) {
            const Json& wj = jwars[i];
            War war;
            war.start_tick = 0;
            war.active = true;
            auto add_side = [&](const Json& tags, std::vector<WarParticipant>* side) {
                if (!tags.is_array()) return true;
                for (size_t k = 0; k < tags.size(); ++k) {
                    const std::string t = tags[k].as_string();
                    auto it = by_tag.find(t);
                    if (it == by_tag.end()) return false;
                    side->push_back(WarParticipant{it->second, 0.0, 0.0, 0.0});
                }
                return true;
            };
            if (!add_side(wj["attackers"], &war.attackers)) {
                return fail(scenario_path + ": war entry has an unknown attacker");
            }
            if (!add_side(wj["defenders"], &war.defenders)) {
                return fail(scenario_path + ": war entry has an unknown defender");
            }
            if (war.attackers.empty() || war.defenders.empty()) {
                return fail(scenario_path + ": war entry needs attackers and defenders");
            }
            war.aggressor = war.attackers.front().country;
            const Json& goals = wj["goals"];
            if (goals.is_array()) {
                for (size_t k = 0; k < goals.size(); ++k) {
                    const Json& gj = goals[k];
                    auto cit = by_tag.find(gj["claimant"].as_string());
                    auto tit = by_tag.find(gj["target"].as_string());
                    if (cit == by_tag.end() || tit == by_tag.end()) {
                        return fail(scenario_path + ": war goal references an unknown country");
                    }
                    WarGoal goal;
                    goal.claimant = cit->second;
                    goal.target = tit->second;
                    goal.annex_country = gj["annex_country"].as_bool(false);
                    goal.puppet = gj["puppet"].as_bool(false);
                    const std::string sk = gj["state"].as_string();
                    if (!sk.empty()) {
                        auto sit = index.states.find(sk);
                        if (sit == index.states.end()) {
                            return fail(scenario_path + ": war goal references unknown state " + sk);
                        }
                        goal.state = sit->second;
                    }
                    war.goals.push_back(goal);
                }
            }
            const WarId wid = world->wars.create(war);
            world->wars[wid].id = wid;
            for (const WarParticipant& p : world->wars[wid].attackers) {
                world->countries[p.country].at_war = true;
                world->countries[p.country].wars.push_back(wid);
            }
            for (const WarParticipant& p : world->wars[wid].defenders) {
                world->countries[p.country].at_war = true;
                world->countries[p.country].wars.push_back(wid);
            }
            for (const WarParticipant& a : world->wars[wid].attackers) {
                for (const WarParticipant& d : world->wars[wid].defenders) {
                    world->relation(a.country, d.country).at_war = true;
                }
            }
        }
    }

    if (err) {
        err->clear();
        for (size_t i = 0; i < warnings.size(); ++i) {
            if (i != 0) *err += "; ";
            *err += warnings[i];
        }
    }
    return true;
}

// Second scenario pass (declared in content.h, called by Game::create right after
// load_scenario). It needs the finished Game because the content it creates must
// go through the same helpers the commands use. Starting navies are formed with
// form_task_force and the fleet bookkeeping of CreateFleet, so no Ship or TaskForce
// field is guessed here; only a missing/unreadable scenario file is a hard error.
bool load_scenario_forces(const std::string& scenario_path, Game& g, std::string* err) {
    Json scenario;
    std::string parse_err;
    if (!Json::parse_file(scenario_path, &scenario, &parse_err)) {
        if (err) {
            *err = scenario_path + ": " + (parse_err.empty() ? "cannot read file" : parse_err);
        }
        return false;
    }
    std::vector<std::string> warnings;
    auto report = [&](const std::string& msg) { warnings.push_back(msg); };

    const Json& jcountries = scenario["countries"];
    if (!jcountries.is_array()) {
        if (err) err->clear();
        return true;
    }
    for (size_t i = 0; i < jcountries.size(); ++i) {
        const Json& c = jcountries[i];
        const Json& navy = c["navy"];
        if (!navy.is_object()) continue;
        const std::string tag = c["tag"].as_string();
        CountryId cid;
        g.world.countries.for_each([&](CountryId id, const Country& cc) {
            if (cc.tag == tag) cid = id;
        });
        Country* country = g.world.country(cid);
        if (country == nullptr) {
            report(scenario_path + ": navy references unknown country " + tag);
            continue;
        }
        // The port is derived, not listed by the scenario (see main_naval_port).
        const ProvinceId port = main_naval_port(g.world, cid);
        if (!port.valid()) {
            report(scenario_path + ": country " + tag +
                   " has no coastal port to base its starting navy at");
            continue;
        }
        // Fleet, created with the same bookkeeping as the CreateFleet command.
        Fleet fleet;
        fleet.country = cid;
        fleet.name = navy["fleet"].as_string(country->tag + " Fleet");
        const FleetId fleet_id = g.world.fleets.create(fleet);
        country->fleets.push_back(fleet_id);

        const Json& task_forces = navy["task_forces"];
        if (!task_forces.is_array()) continue;
        for (size_t k = 0; k < task_forces.size(); ++k) {
            const Json& t = task_forces[k];
            const std::string eq_key = t["equipment"].as_string();
            const EquipmentId eq = g.content.equipment_id(eq_key);
            const EquipmentDef* def = g.content.equipment_def(eq);
            if (def == nullptr || def->is_archetype ||
                def->category != EquipmentCategory::Ship) {
                report(scenario_path + ": country " + tag + ": navy equipment " + eq_key +
                       " is not a ship model");
                continue;
            }
            const int ships = static_cast<int>(t["ships"].as_int(0));
            if (ships <= 0) {
                report(scenario_path + ": country " + tag +
                       ": task force with non-positive ship count");
                continue;
            }
            const std::string name = t["name"].as_string(def->name + " task force");
            const TaskForceId tf_id = form_task_force(g, cid, port, eq, ships, name);
            if (!tf_id.valid()) {
                report(scenario_path + ": country " + tag + ": cannot form '" + name +
                       "' from " + eq_key);
                continue;
            }
            TaskForce* tf = g.world.task_force(tf_id);
            // form_task_force attaches the task force (and each ship) to the
            // country's first fleet -- the fleet created just above -- and lists it
            // in that fleet's roster, so nothing else to wire here.
            const std::string mission = t["mission"].as_string();
            if (!mission.empty()) {
                const int m = match_naval_mission(mission);
                if (m < 0) {
                    report(scenario_path + ": country " + tag + ": unknown naval mission " +
                           mission);
                } else {
                    // The same assignment SetNavalMission makes; the task force's sea
                    // region is already its port's sea zone.
                    tf->mission = static_cast<NavalMission>(m);
                }
            }
        }
    }

    if (err) {
        for (const std::string& w : warnings) {
            if (!err->empty()) *err += "; ";
            *err += w;
        }
    }
    return true;
}

}  // namespace hoi
