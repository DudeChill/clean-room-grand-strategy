// Game orchestration: construction, the fixed tick order, metrics and the auditor.

#include "game/game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "core/log.h"
#include "core/math.h"
#include "sim/combat.h"
#include "sim/industry.h"
#include "sim/map.h"
#include "sim/phases.h"
#include "sim/research.h"

namespace hoi {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof(buf), f, args);
    va_end(args);
    return std::string(buf);
}

}  // namespace

double SimMetrics::percentile_tick_ms(double p) const {
    if (tick_history.empty()) return 0.0;
    std::vector<double> sorted = tick_history;
    std::sort(sorted.begin(), sorted.end());
    const double idx = clamp01(p) * static_cast<double>(sorted.size() - 1);
    const size_t i = static_cast<size_t>(idx);
    return sorted[i];
}

bool Game::create(const std::string& data_root_in, const std::string& scenario_path_in,
                  uint64_t seed_in, Game* out, std::string* err) {
    out->data_root = data_root_in;
    out->scenario_path = scenario_path_in;
    out->seed = seed_in;
    if (!load_content(data_root_in, &out->content, err)) return false;
    if (!load_scenario(scenario_path_in, out->content, &out->world, err)) return false;

    out->rng.seed(seed_in);
    out->world.world_seed = seed_in;
    out->start_date = out->world.date;
    out->ai_controlled.assign(out->world.countries.capacity(), 1);
    out->player_country = CountryId{};
    out->events.reserve(4096);
    return true;
}

void Game::log_event(const std::string& kind, const std::string& text, CountryId c) {
    events.push_back(SimEvent{world.tick, kind, text, c});
    if (events.size() > 50000) {
        events.erase(events.begin(), events.begin() + 10000);
    }
}

void Game::tick_once() {
    const auto t_start = Clock::now();
    auto t_phase = t_start;

#define HOI_PHASE(fn, field)                       \
    do {                                           \
        t_phase = Clock::now();                    \
        fn(*this);                                 \
        const auto t_now = Clock::now();           \
        metrics.field += ms_since(t_phase, t_now); \
        t_phase = t_now;                           \
    } while (0)

    HOI_PHASE(phase_commands, ms_commands);
    HOI_PHASE(phase_diplomacy, ms_diplomacy);
    HOI_PHASE(phase_movement, ms_movement);
    HOI_PHASE(phase_combat, ms_combat);
    HOI_PHASE(phase_territory, ms_territory);
    HOI_PHASE(phase_supply, ms_supply);
    HOI_PHASE(phase_air, ms_air);
    HOI_PHASE(phase_industry, ms_industry);
    HOI_PHASE(phase_research, ms_research);
    HOI_PHASE(phase_training, ms_training);
    HOI_PHASE(phase_politics, ms_politics);
    HOI_PHASE(phase_weather, ms_weather);
    HOI_PHASE(phase_ai, ms_ai);
    HOI_PHASE(phase_cleanup, ms_cleanup);

#undef HOI_PHASE

    ++world.tick;
    ++ticks_run;
    world.date = tick_to_date(world.tick, start_date);
    metrics.note_tick(ms_since(t_start, Clock::now()));
}

void Game::run_ticks(uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) tick_once();
}

// ---------------------------------------------------------------- auditor ----

std::vector<std::string> check_invariants(const Game& g) {
    std::vector<std::string> out;
    const World& w = g.world;

    w.provinces.for_each([&](ProvinceId pid, const Province& p) {
        if (!p.is_sea) {
            // Sea provinces legitimately have no state, owner or controller.
            if (!p.state.valid() || !w.state(p.state)) {
                out.push_back(fmt("province %u references missing state %u", pid.v, p.state.v));
            }
            if (!p.region.valid() || !w.regions.try_get(p.region)) {
                out.push_back(fmt("province %u references missing region %u", pid.v, p.region.v));
            }
            if (p.owner.valid() && !w.country(p.owner)) {
                out.push_back(fmt("province %u owner %u does not exist", pid.v, p.owner.v));
            }
            if (p.controller.valid() && !w.country(p.controller)) {
                out.push_back(fmt("province %u controller %u does not exist", pid.v, p.controller.v));
            }
            // Land adjacency must be symmetric among land provinces.
            for (ProvinceId n : p.adj) {
                const Province* other = w.province(n);
                if (!other) {
                    out.push_back(
                        fmt("province %u adjacency references missing province %u", pid.v, n.v));
                    continue;
                }
                if (other->is_sea) {
                    out.push_back(fmt("province %u land adjacency %u is a sea province", pid.v, n.v));
                    continue;
                }
                if (std::find(other->adj.begin(), other->adj.end(), pid) == other->adj.end()) {
                    out.push_back(fmt("adjacency %u -> %u is not symmetric", pid.v, n.v));
                }
            }
        }
        // Sea zones are referenced from coastal provinces through sea_adj, and the
        // sea province lists the same land provinces in its own adjacency.
        for (ProvinceId n : p.sea_adj) {
            const Province* other = w.province(n);
            if (!other || !other->is_sea) {
                out.push_back(fmt("province %u sea_adj %u is not a sea province", pid.v, n.v));
            }
        }
        if (!std::isfinite(p.population) || p.population < 0.0) {
            out.push_back(fmt("province %u has invalid population", pid.v));
        }
        if (!std::isfinite(p.supply_level) || p.supply_level < -1e-9 || p.supply_level > 1.0 + 1e-9) {
            out.push_back(fmt("province %u supply level out of range (%f)", pid.v, p.supply_level));
        }
    });

    w.states.for_each([&](StateId sid, const State& s) {
        if (s.owner.valid() && !w.country(s.owner)) {
            out.push_back(fmt("state %u owner %u does not exist", sid.v, s.owner.v));
        }
        if (s.civilian_factories < 0 || s.military_factories < 0 || s.dockyards < 0) {
            out.push_back(fmt("state %u has negative factory count", sid.v));
        }
    });

    w.countries.for_each([&](CountryId cid, const Country& c) {
        if (!c.alive) return;  // a defeated country is out of play and owns no industry
        if (c.manpower < -1e-6) out.push_back(fmt("country %u has negative manpower (%f)", cid.v, c.manpower));
        if (c.fuel < -1e-6) out.push_back(fmt("country %u has negative fuel (%f)", cid.v, c.fuel));
        if (c.political_power < -1e-6) {
            out.push_back(fmt("country %u has negative political power", cid.v));
        }
        for (double v : c.equipment_stockpile) {
            if (!std::isfinite(v) || v < -1e-6) {
                out.push_back(fmt("country %u has invalid stockpile entry", cid.v));
                break;
            }
        }
        for (const auto& line : c.lines) {
            if (line.factories < 0) out.push_back(fmt("country %u line has negative factories", cid.v));
            if (!std::isfinite(line.efficiency) || line.efficiency < -1e-9 ||
                line.efficiency > 1.0 + 1e-9) {
                out.push_back(fmt("country %u line efficiency out of range (%f)", cid.v, line.efficiency));
            }
        }
        int assigned = 0;
        for (const auto& line : c.lines) assigned += line.factories;
        int civ = 0, mil = 0, dock = 0;
        count_factories(w, cid, &civ, &mil, &dock);
        if (assigned > mil) {
            out.push_back(fmt("country %u assigned %d factories but controls only %d", cid.v, assigned, mil));
        }
    });

    w.divisions.for_each([&](DivisionId did, const Division& d) {
        if (!w.country(d.country)) {
            out.push_back(fmt("division %u belongs to missing country %u", did.v, d.country.v));
            return;
        }
        if (!g.content.template_def(d.template_id)) {
            out.push_back(fmt("division %u references missing template %u", did.v, d.template_id.v));
        }
        if (d.location.valid() && !w.province(d.location)) {
            out.push_back(fmt("division %u is in missing province %u", did.v, d.location.v));
        }
        if (!std::isfinite(d.strength) || d.strength < -1e-9 || d.strength > 1.0 + 1e-6) {
            out.push_back(fmt("division %u strength out of range (%f)", did.v, d.strength));
        }
        if (!std::isfinite(d.organization) || d.organization < -1e-6) {
            out.push_back(fmt("division %u organisation negative (%f)", did.v, d.organization));
        }
        if (!std::isfinite(d.supply) || d.supply < -1e-9 || d.supply > 1.0 + 1e-9) {
            out.push_back(fmt("division %u supply out of range (%f)", did.v, d.supply));
        }
        if (d.battle.valid() && !w.battle(d.battle)) {
            out.push_back(fmt("division %u references missing battle %u", did.v, d.battle.v));
        }
        for (double e : d.equipment) {
            if (!std::isfinite(e) || e < -1e-6) {
                out.push_back(fmt("division %u has invalid equipment count", did.v));
                break;
            }
        }
    });

    w.battles.for_each([&](BattleId bid, const Battle& b) {
        if (!w.province(b.province)) {
            out.push_back(fmt("battle %u in missing province %u", bid.v, b.province.v));
        }
        if (b.attacker.divisions.empty() && b.defender.divisions.empty()) {
            out.push_back(fmt("battle %u has no participants", bid.v));
        }
        for (DivisionId did : b.attacker.divisions) {
            if (!w.divisions.alive(did)) {
                out.push_back(fmt("battle %u attacker %u missing", bid.v, did.v));
                continue;
            }
            const Division* d = w.division(did);
            if (d && d->battle != bid) {
                out.push_back(fmt("battle %u lists attacker %u but that division is in battle %s",
                                  bid.v, did.v,
                                  d->battle.valid() ? std::to_string(d->battle.v).c_str()
                                                    : "none"));
            }
        }
        for (DivisionId did : b.defender.divisions) {
            if (!w.divisions.alive(did)) {
                out.push_back(fmt("battle %u defender %u missing", bid.v, did.v));
                continue;
            }
            const Division* d = w.division(did);
            if (d && d->battle != bid) {
                out.push_back(fmt("battle %u lists defender %u but that division is in battle %s",
                                  bid.v, did.v,
                                  d->battle.valid() ? std::to_string(d->battle.v).c_str()
                                                    : "none"));
            }
        }
    });

    w.wars.for_each([&](WarId wid, const War& war) {
        if (war.attackers.empty() || war.defenders.empty()) {
            out.push_back(fmt("war %u has an empty side", wid.v));
        }
        for (const auto& p : war.attackers) {
            if (!w.country(p.country)) out.push_back(fmt("war %u attacker %u missing", wid.v, p.country.v));
        }
        for (const auto& p : war.defenders) {
            if (!w.country(p.country)) out.push_back(fmt("war %u defender %u missing", wid.v, p.country.v));
        }
    });

    return out;
}

// ---------------------------------------------------------------- cleanup ----

void phase_cleanup(Game& g) {
    World& w = g.world;

    // Finished battles: a side with no divisions means the battle is over.
    std::vector<BattleId> finished;
    w.battles.for_each([&](BattleId bid, const Battle& b) {
        if (b.attacker.divisions.empty() || b.defender.divisions.empty()) finished.push_back(bid);
    });
    for (BattleId bid : finished) {
        Battle* b = w.battles.try_get(bid);
        if (!b) continue;
        for (DivisionId did : b->attacker.divisions) {
            Division* d = w.divisions.try_get(did);
            if (d && d->battle == bid) d->battle = BattleId{};
        }
        for (DivisionId did : b->defender.divisions) {
            Division* d = w.divisions.try_get(did);
            if (d && d->battle == bid) d->battle = BattleId{};
        }
        w.battles.destroy(bid);
    }

    // Destroyed divisions: no strength left and not training.
    std::vector<DivisionId> dead;
    w.divisions.for_each([&](DivisionId did, const Division& d) {
        if (!d.in_training() && d.strength <= 0.0) dead.push_back(did);
    });
    for (DivisionId did : dead) {
        Division* d = w.divisions.try_get(did);
        if (!d) continue;
        const CountryId owner = d->country;
        if (d->battle.valid()) detach_from_battle(g, *d);
        Country* c = w.countries.try_get(owner);
        if (c) {
            c->divisions.erase(std::remove(c->divisions.begin(), c->divisions.end(), did),
                               c->divisions.end());
            for (auto it = c->training.begin(); it != c->training.end(); ++it) {
                if (it->division == did) {
                    c->training.erase(it);
                    break;
                }
            }
        }
        if (d->army.valid()) {
            Army* a = w.armies.try_get(d->army);
            if (a) {
                a->divisions.erase(std::remove(a->divisions.begin(), a->divisions.end(), did),
                                   a->divisions.end());
            }
        }
        w.divisions.destroy(did);
    }

    // Armies of countries that no longer exist are removed with their divisions.
    std::vector<ArmyId> dead_armies;
    w.armies.for_each([&](ArmyId aid, const Army& a) {
        const Country* c = w.country(a.country);
        if (!c || (!c->alive && a.divisions.empty())) dead_armies.push_back(aid);
    });
    for (ArmyId aid : dead_armies) w.armies.destroy(aid);

#if defined(HOI_DEBUG_BUILD) && HOI_DEBUG_BUILD
    if (w.tick % 720 == 0) {  // monthly invariant sweep in debug builds
        std::vector<std::string> violations = check_invariants(g);
        for (const auto& v : violations) HOI_ERROR("invariant: %s", v.c_str());
    }
#endif
}

bool audit_world(const Game& g, std::string* report) {
    std::vector<std::string> violations = check_invariants(g);
    if (report) {
        report->clear();
        if (violations.empty()) {
            *report = "world audit: OK";
        } else {
            *report = "world audit: " + std::to_string(violations.size()) + " violation(s)\n";
            for (size_t i = 0; i < violations.size() && i < 200; ++i) {
                *report += "  " + violations[i] + "\n";
            }
            if (violations.size() > 200) *report += "  ... (truncated)\n";
        }
    }
    return violations.empty();
}

}  // namespace hoi
