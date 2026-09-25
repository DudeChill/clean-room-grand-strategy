// Command line entry point.
//
// Modes:
//   game --headless --days 365            run a campaign and report
//   game --scenario <file> --seed <n>     choose scenario and deterministic seed
//   game --save <file> / --load <file>    persistence round trip
//   game --hashes                         print subsystem hashes (determinism oracle)
//   game --audit                          run the world auditor
//   game --mods <dirs>                    apply content mods (see docs/MODDING.md)
//   game --validate-content               validate the content tree and exit non-zero on errors
//   game --player TAG                     hand a country to the player (AI plays the rest)

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <thread>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/socket_compat.h"
#if defined(_WIN32)
// socket_compat.h pulls winsock2.h with WIN32_LEAN_AND_MEAN; windows.h then skips
// shellapi.h, which is where ShellExecuteA lives.
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif
#include "core/types.h"
#include "data/mod.h"
#include "game/game.h"
#include "game/server.h"
#include "net/lockstep.h"
#include "net/transport.h"
#include "sim/design.h"
#include "save/save.h"
#include "sim/industry.h"
#include "sim/research.h"
#include "sim/spirits.h"
#include "sim/trade.h"
#include "sim/supply.h"
#include "sim/units.h"

using namespace hoi;

namespace {

volatile bool g_stop_requested = false;

void on_signal(int) { g_stop_requested = true; }

struct Options {
    std::string data_root = "data";
    std::string scenario = "data/scenarios/1936.json";
    std::string save_path;
    std::string load_path;
    std::string player_tag;
    std::string web_root = "web";
    std::string inspect_country;
    std::string inspect_battle;
    std::vector<std::string> mod_roots;  // --mods: roots holding <root>/<mod>/mod.json
    uint64_t inspect_province = 0;
    uint64_t inspect_supply = 0;
    uint64_t seed = 12345;
    uint64_t days = 30;
    uint64_t ticks = 0;
    uint64_t save_every_days = 0;
    uint64_t autosave_days = 0;
    uint16_t port = 8080;
    uint32_t players = 1;         // --players: seats in a networked session (host: required)
    bool players_set = false;     // --players was given explicitly (joiners cross-check it)
    std::string join_addr;        // --join HOST:PORT
    int64_t net_issue_at = -1;    // --net-issue-at: tick this peer submits its one command
    bool use_ticks = false;
    bool host = false;            // --host: listen and run the session
    bool hashes = false;
    bool audit = false;
    bool summary = false;
    bool quiet = false;
    bool verbose = false;
    bool serve = false;
    bool play = false;   // one-click start: serve on a free port and open the browser
    bool inspect_trade = false;
    bool validate_content = false;
};

void usage() {
    std::printf(
        "usage: game [options]\n"
        "  --data <dir>          content root (default data)\n"
        "  --scenario <file>     scenario json (default data/scenarios/1936.json)\n"
        "  --seed <n>            deterministic seed\n"
        "  --days <n>            simulated days (default 30)\n"
        "  --ticks <n>           simulated hours (overrides --days)\n"
        "  --player <TAG>        player-controlled country tag\n"
        "  --save <file>         save after the run\n"
        "  --load <file>         load a save instead of a scenario\n"
        "  --save-every-days <n> periodic save/load round trip (stress test)\n"
        "  --serve               run the playable browser client\n"
        "  --play                one-click start: serve on a free port and open the browser\n"
        "  --port <n>            server port (default 8080); the session port with --host\n"
        "  --host                host a networked lockstep session (see docs/MULTIPLAYER.md)\n"
        "  --players <n>         seats in a networked session, including the host (host: default 1)\n"
        "  --join HOST:PORT      join a hosted session\n"
        "  --net-issue-at <tick> tick at which this peer issues its one command (default: half\n"
        "                        the session); both peers may pick different ticks\n"
        "  --web <dir>           static web root (default web)\n"
        "  --autosave-days <n>   autosave interval while serving\n"
        "  --hashes              print subsystem hashes\n"
        "  --audit               run the world auditor\n"
        "  --mods <dirs>         content mod roots, comma separated (see docs/MODDING.md)\n"
        "  --validate-content    load content with --mods, report, audit one day, exit non-zero "
        "on any error\n"
        "  --summary             print per-country summary\n"
        "  --inspect-country TAG print full state of one country\n"
        "  --inspect-province ID print province state and supply\n"
        "  --inspect-supply ID   print the supply route of a province\n"
        "  --inspect-battle ID   print a battle breakdown (attacker/defender/debug)\n"
        "  --inspect-trade       print every country's resource balance and all trade routes\n"
        "  --verbose             debug logging\n"
        "  --quiet               errors only\n");
}

bool parse_args(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (a == "--data") {
            if (!next(&o->data_root)) return false;
        } else if (a == "--scenario") {
            if (!next(&o->scenario)) return false;
        } else if (a == "--seed") {
            std::string v;
            if (!next(&v)) return false;
            o->seed = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--days") {
            std::string v;
            if (!next(&v)) return false;
            o->days = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--ticks") {
            std::string v;
            if (!next(&v)) return false;
            o->ticks = std::strtoull(v.c_str(), nullptr, 10);
            o->use_ticks = true;
        } else if (a == "--player") {
            if (!next(&o->player_tag)) return false;
        } else if (a == "--save") {
            if (!next(&o->save_path)) return false;
        } else if (a == "--load") {
            if (!next(&o->load_path)) return false;
        } else if (a == "--save-every-days") {
            std::string v;
            if (!next(&v)) return false;
            o->save_every_days = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--play") {
            o->play = true;
            o->serve = true;
        } else if (a == "--serve") {
            o->serve = true;
        } else if (a == "--host") {
            o->host = true;
        } else if (a == "--join") {
            if (!next(&o->join_addr)) return false;
        } else if (a == "--net-issue-at") {
            std::string v;
            if (!next(&v)) return false;
            o->net_issue_at = static_cast<int64_t>(std::strtoll(v.c_str(), nullptr, 10));
        } else if (a == "--players") {
            std::string v;
            if (!next(&v)) return false;
            o->players = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
            o->players_set = true;
        } else if (a == "--port") {
            std::string v;
            if (!next(&v)) return false;
            o->port = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--web") {
            if (!next(&o->web_root)) return false;
        } else if (a == "--autosave-days") {
            std::string v;
            if (!next(&v)) return false;
            o->autosave_days = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--inspect-country") {
            if (!next(&o->inspect_country)) return false;
        } else if (a == "--inspect-province") {
            std::string v;
            if (!next(&v)) return false;
            o->inspect_province = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--inspect-supply") {
            std::string v;
            if (!next(&v)) return false;
            o->inspect_supply = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--inspect-trade") {
            o->inspect_trade = true;
        } else if (a == "--inspect-battle") {
            if (!next(&o->inspect_battle)) return false;
        } else if (a == "--mods") {
            std::string v;
            if (!next(&v)) return false;
            size_t start = 0;
            while (start <= v.size()) {
                const size_t comma = v.find(',', start);
                const std::string one = v.substr(start, comma == std::string::npos
                                                             ? std::string::npos
                                                             : comma - start);
                if (!one.empty()) o->mod_roots.push_back(one);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (a == "--validate-content") {
            o->validate_content = true;
        } else if (a == "--hashes") {
            o->hashes = true;
        } else if (a == "--audit") {
            o->audit = true;
        } else if (a == "--summary") {
            o->summary = true;
        } else if (a == "--quiet") {
            o->quiet = true;
        } else if (a == "--verbose") {
            o->verbose = true;
        } else if (a == "--headless") {
            // explicit no-op: the binary is always headless
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

void print_country_summary(const Game& g) {
    std::printf("\n%-6s %-22s %6s %6s %6s %6s %6s %6s %6s %6s\n", "tag", "name", "states",
                "civ", "mil", "div", "manp", "pp", "stab", "ws");
    g.world.countries.for_each([&](CountryId cid, const Country& c) {
        if (!c.alive) return;
        int civ = 0, mil = 0, dock = 0;
        count_factories(g.world, cid, &civ, &mil, &dock);
        int states = 0;
        g.world.states.for_each([&](StateId, const State& s) {
            if (s.controller == cid) ++states;
        });
        std::printf("%-6s %-22s %6d %6d %6d %6zu %6.0f %6.1f %6.2f %6.2f\n", c.tag.c_str(),
                    c.name.c_str(), states, civ, mil, c.divisions.size(), c.manpower,
                    c.political_power, c.stability, c.war_support);
    });
}

void print_metrics(const Game& g) {
    const SimMetrics& m = g.metrics;
    std::printf("\nperformance (avg ms/tick): total %.3f | commands %.3f | diplomacy %.3f | "
                "movement %.3f | combat %.3f | territory %.3f | supply %.3f | air %.3f | naval %.3f | industry %.3f | "
                "research %.3f | training %.3f | politics %.3f | weather %.3f | ai %.3f | "
                "cleanup %.3f\n",
                m.average_tick_ms(), m.ms_commands, m.ms_diplomacy, m.ms_movement, m.ms_combat,
                m.ms_territory, m.ms_supply, m.ms_air, m.ms_naval, m.ms_industry, m.ms_research, m.ms_training,
                m.ms_politics, m.ms_weather, m.ms_ai, m.ms_cleanup);
    std::printf("   ai detail: industry %.1f trade %.1f design %.1f production %.1f military %.1f "
                "politics %.1f research %.1f diplomacy %.1f intel %.1f\n",
                m.ms_ai_industry, m.ms_ai_trade, m.ms_ai_design, m.ms_ai_production,
                m.ms_ai_military, m.ms_ai_politics, m.ms_ai_research, m.ms_ai_diplomacy,
                m.ms_ai_intelligence);
    std::printf("tick p50 %.3f ms, p95 %.3f ms, p99 %.3f ms over %zu samples\n",
                m.percentile_tick_ms(0.50), m.percentile_tick_ms(0.95), m.percentile_tick_ms(0.99),
                m.tick_history.size());
}

// Inspectors (spec section 82): answer "why is this happening" from the CLI, using
// the same authoritative state the simulation uses.
void inspect_country(const Game& g, const std::string& tag) {
    const World& w = g.world;
    const Country* found = nullptr;
    CountryId cid;
    w.countries.for_each([&](CountryId id, const Country& c) {
        if (c.tag == tag) { found = &c; cid = id; }
    });
    if (!found) {
        std::printf("no country with tag %s\n", tag.c_str());
        return;
    }
    const Country& c = *found;
    int civ = 0, mil = 0, dock = 0;
    count_factories(w, cid, &civ, &mil, &dock);
    std::printf("COUNTRY %s %s (%s)\n", c.tag.c_str(), c.name.c_str(),
                c.alive ? "alive" : "defeated");
    std::printf("  political power %.1f  stability %.2f  war support %.2f  manpower %.0f  "
                "fuel %.1f/%.1f  consumer goods %.2f\n",
                c.political_power, c.stability, c.war_support, c.manpower, c.fuel, c.fuel_capacity,
                c.consumer_goods_ratio);
    std::printf("  factories: civilian %d, military %d, dockyards %d (assigned %d)\n", civ, mil,
                dock,
                [&] {
                    int n = 0;
                    for (const auto& l : c.lines) n += l.factories;
                    return n;
                }());
    std::printf("  modifiers applied at %.4f factor summary\n",
                c.total_modifiers().factor(ModifierKind::FactoryOutput));
    std::printf("  divisions %zu (training %zu), armies %zu, templates %zu, wars %zu\n",
                c.divisions.size(), c.training.size(), c.armies.size(), c.templates.size(),
                c.wars.size());
    std::printf("  production lines:\n");
    for (const auto& l : c.lines) {
        const EquipmentDef* def = g.content.equipment_def(l.equipment);
        std::printf("    %-24s factories %3d  efficiency %.3f (cap %.3f)  total %.1f  "
                    "resource shortage %.2f\n",
                    def ? def->key.c_str() : "(idle/retired)", l.factories, l.efficiency,
                    l.efficiency_cap, l.output_total, l.resource_shortage);
    }
    if (!c.construction.queue.empty()) {
        std::printf("  construction queue:\n");
        for (const auto& p : c.construction.queue) {
            const Province* prov = w.province(p.province);
            std::printf("    %-20s level %d  progress %.0f/%.0f  %s\n", building_kind_name(p.kind),
                        p.target_level, p.progress, p.cost,
                        prov ? prov->name.c_str() : "(state-wide)");
        }
    }
    std::printf("  research:\n");
    for (size_t i = 0; i < c.research.slots.size(); ++i) {
        const ResearchSlot& s = c.research.slots[i];
        const TechDef* def = g.content.tech_def(s.tech);
        if (s.active && def) {
            std::printf("    slot %zu: %-24s %.1f/%.1f days\n", i, def->key.c_str(), s.progress,
                        tech_cost_days(g, cid, s.tech));
        } else {
            std::printf("    slot %zu: idle\n", i);
        }
    }
    std::printf("  politics: pp %.0f  focus '",
                c.political_power);
    if (const FocusDef* sel = g.content.focus(c.selected_focus)) {
        std::printf("%s' %.0f/%.0f days", sel->key.c_str(), c.focus_progress, sel->days);
    } else {
        std::printf("none'");
    }
    std::printf("  completed focuses %zu\n", c.completed_focuses.size());
    if (!c.completed_focuses.empty()) {
        std::printf("    completed:");
        for (uint32_t f : c.completed_focuses) {
            const FocusDef* def = g.content.focus(f);
            std::printf(" %s", def ? def->key.c_str() : "?");
        }
        std::printf("\n");
    }
    if (!c.pending_events.empty()) {
        std::printf("    pending events:");
        for (uint32_t e : c.pending_events) {
            const EventDef* def = g.content.event(e);
            std::printf(" %s", def ? def->key.c_str() : "?");
        }
        std::printf("\n");
    }
    if (!c.active_decisions.empty()) {
        std::printf("    decisions:");
        for (size_t i = 0; i < c.active_decisions.size(); ++i) {
            const DecisionDef* def = g.content.decision(c.active_decisions[i]);
            const double left = i < c.decision_days_left.size() ? c.decision_days_left[i] : 0.0;
            std::printf(" %s(%s)", def ? def->key.c_str() : "?",
                        left > 0 ? std::to_string(static_cast<int>(left)).c_str() : "permanent");
        }
        std::printf("\n");
    }
    if (!c.spirit_keys.empty() || !c.national_spirits.empty()) {
        std::printf("    spirits:");
        for (uint32_t idx : c.spirit_keys) {
            const SpiritDef* def = g.content.spirit(idx);
            std::printf(" %s", def ? def->key.c_str() : "?");
        }
        std::printf("   (slots free %d of %d)\n", spirit_slots_free(g, cid), c.spirit_slots);
    }
    if (!c.advisors.empty()) {
        std::printf("    advisors:");
        for (uint32_t idx : c.advisors) {
            const AdvisorDef* def = g.content.advisor(idx);
            std::printf(" %s", def ? def->key.c_str() : "?");
        }
        std::printf("   (slots free %d of %d)\n", advisor_slots_free(g, cid), c.advisor_slots);
    }
    if (!c.timed_modifiers.empty()) {
        std::printf("    timed modifiers:");
        for (const TimedModifier& tm : c.timed_modifiers) {
            std::printf(" %s(%s)", tm.source.c_str(),
                        tm.days_left < 0 ? "permanent" : std::to_string(tm.days_left).c_str());
        }
        std::printf("\n");
    }
    std::printf("  resource balance (per day):");
    for (int r = 0; r < RESOURCE_COUNT; ++r) {
        const double bal = resource_balance(g, cid, static_cast<Resource>(r));
        if (bal > 0.01 || bal < -0.01) {
            std::printf(" %s %+.2f", resource_name(static_cast<Resource>(r)), bal);
        }
    }
    std::printf("\n");
    if (!w.trade_routes.empty()) {
        std::printf("  trade routes:\n");
        for (const TradeRoute& r : w.trade_routes) {
            if (r.importer != cid && r.exporter != cid) continue;
            const Country* other = w.country(r.importer == cid ? r.exporter : r.importer);
            std::printf("    %s %s %-9s %.1f/day (delivered %.1f)%s%s\n",
                        r.importer == cid ? "import" : "export", other ? other->tag.c_str() : "?",
                        resource_name(r.resource), r.amount, r.delivered,
                        r.sea_route ? " sea" : " land", r.active ? "" : " INACTIVE");
        }
    }
    std::printf("  stockpile:\n");
    for (size_t i = 0; i < c.equipment_stockpile.size(); ++i) {
        if (c.equipment_stockpile[i] <= 0.0) continue;
        const EquipmentDef* def = g.content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
        if (def) std::printf("    %-24s %.1f\n", def->key.c_str(), c.equipment_stockpile[i]);
    }
    std::printf("  designs:\n");
    for (uint32_t idx : c.designs) {
        const EquipmentDesign* d = g.content.design(idx);
        if (!d) continue;
        const EquipmentDef* produced = g.content.equipment_def(d->produced);
        const EquipmentDef* base = g.content.equipment_def(d->archetype);
        if (!produced) continue;
        double cost_delta = 0.0;
        if (base && base->build_cost > 0.0) {
            cost_delta = (produced->build_cost / base->build_cost - 1.0) * 100.0;
        }
        std::printf("    %-20s %-10s year %d  cost %.2f (%+.0f%% vs %s)  soft %.0f hard %.0f "
                    "armor %.0f pierce %.0f speed %.1f  slots %zu\n",
                    d->name.c_str(), d->key.c_str(), d->year, produced->build_cost, cost_delta,
                    base ? base->key.c_str() : "?", produced->soft_attack, produced->hard_attack,
                    produced->armor, produced->piercing, produced->speed, d->components.size());
    }
}

void inspect_province(const Game& g, ProvinceId pid) {
    const Province* p = g.world.province(pid);
    if (!p) {
        std::printf("no province %u\n", pid.v);
        return;
    }
    const State* s = g.world.state(p->state);
    std::printf("PROVINCE %u '%s' terrain %s%s\n", pid.v, p->name.c_str(), terrain_name(p->terrain),
                p->is_sea ? " (sea)" : "");
    std::printf("  state '%s'  owner %s  controller %s  vp %d  infra %d  rail %d  hub %s\n",
                s ? s->name.c_str() : "?",
                p->owner.valid() ? g.world.country(p->owner)->tag.c_str() : "-",
                p->controller.valid() ? g.world.country(p->controller)->tag.c_str() : "-",
                p->victory_points, p->infrastructure, p->railway_level, p->supply_hub ? "yes" : "no");
    std::printf("  fort %d  air base %d  naval base %d  radar %d  population %.0f\n", p->fort_level,
                p->air_base, p->naval_base, p->radar, p->population);
    const Province* src = g.world.province(p->supply_source);
    const Province* bottleneck = g.world.province(p->supply_bottleneck);
    std::printf("  supply level %.3f  source %s  bottleneck %s\n", p->supply_level,
                src ? src->name.c_str() : "-", bottleneck ? bottleneck->name.c_str() : "-");
    std::printf("  neighbours:");
    for (ProvinceId n : p->adj) {
        const Province* np = g.world.province(n);
        std::printf(" %u(%s)", n.v, np ? np->name.c_str() : "?");
    }
    std::printf("\n  divisions here:\n");
    g.world.divisions.for_each([&](DivisionId did, const Division& d) {
        if (d.location != pid) return;
        const Country* dc = g.world.country(d.country);
        std::printf("    #%u %-24s %s  org %.1f/%.1f  strength %.2f  supply %.2f  "
                    "entrenchment %.2f  planning %.2f\n",
                    did.v, d.name.c_str(), dc ? dc->tag.c_str() : "?", d.organization,
                    d.max_organization, d.strength, d.supply, d.entrenchment, d.planning);
    });
}

void inspect_supply(const Game& g, ProvinceId pid) {
    const Province* p = g.world.province(pid);
    if (!p) {
        std::printf("no province %u\n", pid.v);
        return;
    }
    const CountryId holder = p->controller.valid() ? p->controller : p->owner;
    std::vector<SupplyRouteStep> route;
    ProvinceId bottleneck;
    const double delivered =
        holder.valid() ? explain_supply_route(g, holder, pid, &route, &bottleneck) : 0.0;
    std::printf("SUPPLY for %s (%s): delivered %.2f/hr, level %.3f\n", p->name.c_str(),
                g.world.country(holder) ? g.world.country(holder)->tag.c_str() : "-", delivered,
                p->supply_level);
    for (const SupplyRouteStep& step : route) {
        const Province* sp = g.world.province(step.province);
        std::printf("  -> %-24s capacity %.2f\n", sp ? sp->name.c_str() : "?", step.capacity);
    }
    const Province* bp = g.world.province(bottleneck);
    std::printf("  bottleneck: %s\n", bp ? bp->name.c_str() : "none");
}

void inspect_trade(const Game& g) {
    const World& w = g.world;
    std::printf("TRADE\n");
    w.countries.for_each([&](CountryId cid, const Country& c) {
        if (!c.alive) return;
        std::printf("  %-4s", c.tag.c_str());
        for (int r = 0; r < RESOURCE_COUNT; ++r) {
            const Resource res = static_cast<Resource>(r);
            const double balance = resource_balance(g, cid, res);
            if (balance > 0.01 || balance < -0.01) {
                std::printf("  %s %+.2f", resource_name(res), balance);
            }
        }
        std::printf("\n");
    });
    std::printf("  routes: %zu\n", w.trade_routes.size());
    for (const TradeRoute& r : w.trade_routes) {
        const Country* i = w.country(r.importer);
        const Country* e = w.country(r.exporter);
        std::printf("    %s -> %s %-9s amount %.2f delivered %.2f %s %s\n",
                    e ? e->tag.c_str() : "?", i ? i->tag.c_str() : "?", resource_name(r.resource),
                    r.amount, r.delivered, r.sea_route ? "sea" : "land",
                    r.active ? "" : "INACTIVE");
    }
}

void inspect_battle(const Game& g, BattleId bid) {
    const Battle* b = g.world.battle(bid);
    if (!b) {
        std::printf("no battle %u\n", bid.v);
        return;
    }
    const Province* p = g.world.province(b->province);
    std::printf("BATTLE %u in '%s' (%s)%s%s  progress %.2f  since tick %llu\n", bid.v,
                p ? p->name.c_str() : "?", terrain_name(b->terrain),
                b->river_crossing ? " river" : "", b->encirclement ? " ENCIRCLED" : "", b->progress,
                static_cast<unsigned long long>(b->start_tick));
    auto side = [&](const char* label, const BattleSideState& s) {
        std::printf("  %s: soft %.1f  hard %.1f  defence %.1f  breakthrough %.1f  armour %.1f  "
                    "piercing %.1f\n",
                    label, s.total_soft_attack, s.total_hard_attack, s.total_defense,
                    s.total_breakthrough, s.total_armor, s.total_piercing);
        for (size_t i = 0; i < s.divisions.size(); ++i) {
            const Division* d = g.world.division(s.divisions[i]);
            if (!d) continue;
            const Country* dc = g.world.country(d->country);
            std::printf("    #%u %-22s %s org %.1f/%.1f str %.2f supply %.2f width %.1f\n",
                        s.divisions[i].v, d->name.c_str(), dc ? dc->tag.c_str() : "?",
                        d->organization, d->max_organization, d->strength, d->supply,
                        i < s.width_used.size() ? s.width_used[i] : 0.0);
        }
    };
    side("attackers", b->attacker);
    side("defenders", b->defender);
    if (!b->debug.empty()) {
        std::printf("  last tick damage breakdown:\n");
        for (const BattleDebugLine& l : b->debug) {
            std::printf("    div #%u base %.1f terrain x%.2f supply x%.2f planning +%.2f commander "
                        "+%.2f exp +%.2f -> %.1f vs defence %.1f = dmg %.2f (org %.2f str %.3f)\n",
                        l.division.v, l.base_attack, l.terrain_mod, l.supply_mod, l.planning_mod,
                        l.commander_mod, l.experience_mod, l.final_attack, l.enemy_defense, l.damage,
                        l.org_damage, l.strength_damage);
        }
    }
}

void print_content_diagnostics(const ModLoadReport& report, const Content& content,
                               bool as_errors) {
    // One line per distinct diagnostic, whichever list carries it: the report renders
    // its events, its error list and Content::load_errors overlap by design.
    std::vector<std::string> seen;
    const std::string text = report.text();
    if (!text.empty()) std::printf("%s", text.c_str());
    auto shown = [&](const std::string& raw) {
        if (std::find(seen.begin(), seen.end(), raw) != seen.end()) return true;
        seen.push_back(raw);
        return false;
    };
    for (const std::string& e : report.errors) {
        // A diagnostic the report already rendered is printed exactly once.
        if (!text.empty() && text.find(e) != std::string::npos) {
            shown(e);
            continue;
        }
        if (!shown(e)) {
            std::printf("%s: %s\n", as_errors ? "content error" : "content warning", e.c_str());
        }
    }
    for (const std::string& e : content.load_errors) {
        if (shown(e)) continue;
        std::printf("%s: %s\n", as_errors ? "content error" : "content warning", e.c_str());
    }
}

// --validate-content (MOD-002): load the content tree with the requested mods through
// the same code path a real run uses, print the deterministic load report, audit a
// one-day run, and exit non-zero on any diagnostic. Nothing is written.
int run_content_validation(const Options& opt) {
    Game game;
    std::string err;
    ModLoadReport report;
    if (!Game::create(opt.data_root, opt.scenario, opt.seed, &game, &err, opt.mod_roots, &report)) {
        print_content_diagnostics(report, game.content, true);
        std::fflush(stdout);
        std::fprintf(stderr, "content validation: FAILED: %s\n", err.c_str());
        return 1;
    }
    print_content_diagnostics(report, game.content, true);
    game.run_ticks(static_cast<uint64_t>(TICKS_PER_DAY));
    std::string audit;
    const bool audit_ok = audit_world(game, &audit);
    std::printf("\n%s\n", audit.c_str());
    size_t replaced = 0;
    size_t added = 0;
    for (const ModLoadEvent& e : report.events) {
        if (e.action == "replaced") ++replaced;
        if (e.action == "added") ++added;
    }
    if (!audit_ok || !report.errors.empty() || !game.content.load_errors.empty()) {
        // Keep the summary after the diagnostics it summarizes even when the two streams
        // are interleaved (stdout is block buffered when redirected to a file).
        std::fflush(stdout);
        std::fprintf(stderr, "content validation: FAILED: %zu mod error(s), %zu content error(s)\n",
                     report.errors.size(), game.content.load_errors.size());
        return 1;
    }
    std::printf("content validation: OK (%zu replaced, %zu added, %zu mod root(s))\n", replaced,
                added, opt.mod_roots.size());
    return 0;
}

// ------------------------------------------------------------------ multiplayer ----
//
// MP-001: a deterministic lockstep session over the transport in src/net/transport.h.
//
// One peer hosts, the rest join. The host is the only peer that derives a tick's
// application order: it collects every seat's `Commands` for the tick, asks `Lockstep`
// for the ordered list (by seat, then submission order), broadcasts that list as
// `Applied` and applies it locally. A joiner applies exactly the list it was handed, so
// two peers cannot disagree about the order even when the arrivals interleave
// differently. Every `kLockstepHashCadence` ticks the peers compare `world_hash`; a
// mismatch halts the session with the tick, the first differing subsystem and that
// tick's command list, because a silent divergence is the one outcome that is not
// allowed.
//
// Lobby protocol (session state, never simulation state - the roster is agreed, then
// every peer builds `Lockstep` from it and Hello messages are ignored from then on):
//   joiner -> host : Hello{text = tag}
//   host -> joiner : one Hello per seat (seat, country, text = tag). The seat-0 Hello
//                    carries the seat count in `hash` and the session length in `tick`,
//                    so a joiner can size the roster and the run before tick 0.
//   host -> joiner : Bye when the session ends, or when the lobby refused the joiner.
// After the lobby the only traffic is: joiner -> host `Commands`/`Hash`; host -> joiner
// `Applied`/`Hash`/`Bye`/`Desync`.
//
// The wall clock below bounds how long a peer waits for a stalled seat; it never
// touches simulation state or the application order.

constexpr int kNetPollSliceMs = 200;
constexpr int kNetLobbyTimeoutMs = 20000;
constexpr int kNetConnectTimeoutMs = 5000;
constexpr uint64_t kNetStallMillis = 30000;

std::string net_fmt(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
std::string net_fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

int64_t ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

const char* net_result_name(LockstepResult r) {
    switch (r) {
        case LockstepResult::Ok: return "ok";
        case LockstepResult::Waiting: return "waiting";
        case LockstepResult::Desynced: return "desynced";
        case LockstepResult::NotMySeat: return "not-my-seat";
        case LockstepResult::Unknown: return "unknown";
    }
    return "?";
}

bool find_country_by_tag(const Game& g, const std::string& tag, CountryId* out) {
    CountryId found;
    bool duplicate = false;
    g.world.countries.for_each([&](CountryId cid, const Country& c) {
        if (c.tag != tag) return;
        if (found.valid()) duplicate = true;
        found = cid;
    });
    if (!found.valid() || duplicate) return false;
    *out = found;
    return true;
}

std::string country_tag(const Game& g, CountryId cid) {
    const Country* c = g.world.country(cid);
    return c != nullptr ? c->tag : std::string("?");
}

bool parse_host_port(const std::string& text, std::string* host, uint16_t* port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    const std::string digits = text.substr(colon + 1);
    char* end = nullptr;
    const unsigned long value = std::strtoul(digits.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value == 0 || value > 65535) return false;
    *host = text.substr(0, colon);
    *port = static_cast<uint16_t>(value);
    return true;
}

std::string describe_command(const Command& c) {
    std::string text = command_type_name(c.type);
    if (!c.text.empty()) text += " \"" + c.text + "\"";
    if (c.state.valid()) text += net_fmt(" state=%u", c.state.raw());
    if (c.province.valid()) text += net_fmt(" province=%u", c.province.raw());
    if (c.equipment.valid()) text += net_fmt(" equipment=%u", c.equipment.raw());
    if (c.tech.valid()) text += net_fmt(" tech=%u", c.tech.raw());
    if (c.value != 0) text += net_fmt(" value=%d", c.value);
    return text;
}

// The one command this peer issues for its own country, chosen deterministically: the
// first candidate that validates, scanned in content order. Both peers scan the same
// state, so two peers pick the same command - the point is that the *order* of two
// seats' commands, not the choice, is what the session has to agree on.
bool pick_player_command(const Game& g, CountryId cid, Tick tick, Command* out) {
    if (!cid.valid()) return false;

    // 1. The first national focus the country may start.
    for (const FocusDef& focus : g.content.focuses) {
        Command c;
        c.type = CommandType::SelectFocus;
        c.country = cid;
        c.issued_tick = tick;
        c.text = focus.key;
        if (validate_command(g, c) == CommandResult::Applied) {
            *out = c;
            return true;
        }
    }
    // 2. The first technology with a free research slot.
    for (const TechDef& tech : g.content.techs) {
        Command c;
        c.type = CommandType::StartResearch;
        c.country = cid;
        c.issued_tick = tick;
        c.tech = tech.id;
        if (validate_command(g, c) == CommandResult::Applied) {
            *out = c;
            return true;
        }
    }
    // 3. A civilian factory in the first state the country controls.
    StateId state;
    g.world.states.for_each([&](StateId id, const State& s) {
        if (!state.valid() && s.controller == cid) state = id;
    });
    if (state.valid()) {
        Command c;
        c.type = CommandType::StartConstruction;
        c.country = cid;
        c.issued_tick = tick;
        c.state = state;
        c.value = static_cast<int32_t>(BuildingKind::CivilianFactory);
        if (validate_command(g, c) == CommandResult::Applied) {
            *out = c;
            return true;
        }
    }
    // 4. The first law the country can afford and does not already hold.
    for (const LawDef& law : g.content.laws) {
        Command c;
        c.type = CommandType::SetLaw;
        c.country = cid;
        c.issued_tick = tick;
        c.text = law.key;
        c.value = law.level;
        if (validate_command(g, c) == CommandResult::Applied) {
            *out = c;
            return true;
        }
    }
    // 5. The first national spirit it is allowed to hold.
    for (const SpiritDef& spirit : g.content.spirits) {
        Command c;
        c.type = CommandType::AddNationalSpirit;
        c.country = cid;
        c.issued_tick = tick;
        c.text = spirit.key;
        if (validate_command(g, c) == CommandResult::Applied) {
            *out = c;
            return true;
        }
    }
    return false;
}

// One remote seat and its channel. Peers are kept in ascending seat order so a
// diagnostic names the same seat on every peer.
struct RemotePeer {
    uint32_t seat = 0;
    std::string tag;
    std::unique_ptr<net::Connection> conn;
    std::unique_ptr<net::Channel> channel;
    bool alive = true;
};

// One lockstep session. The same loop drives both roles: `host` only decides who
// derives a tick's order, who broadcasts it and who waits for it.
struct NetSession {
    Game* game = nullptr;
    Lockstep lock;
    std::vector<PeerSeat> seats;
    std::vector<RemotePeer> peers;
    uint32_t local_seat = 0;
    std::string local_tag;
    bool host = false;
    Tick total_ticks = 0;
    Tick issue_tick = 0;

    std::deque<NetMessage> inbox;
    std::map<Tick, std::map<uint32_t, NetMessage>> hashes;
    bool bye = false;
    Tick bye_tick = 0;
    bool stopped = false;
    std::string stop_reason;
    bool desync = false;
    std::string desync_text;
    uint64_t ticks_applied = 0;
    uint64_t commands_applied = 0;
    uint64_t ticks_with_commands = 0;

    void stop(const std::string& reason) {
        if (stopped) return;
        stopped = true;
        stop_reason = reason;
    }

    [[nodiscard]] bool is_hash_tick(Tick t) const {
        return t % kLockstepHashCadence == 0 || t + 1 == total_ticks;
    }

    [[nodiscard]] std::string seat_list() const {
        std::string text;
        for (const PeerSeat& s : seats) {
            if (!text.empty()) text += " ";
            text += net_fmt("%u:%s", s.seat, s.tag.c_str());
        }
        return text;
    }

    [[nodiscard]] uint32_t seat_of_country(CountryId cid) const {
        for (const PeerSeat& s : seats) {
            if (s.country == cid) return s.seat;
        }
        return 0xFFFFFFFFu;
    }

    void send_all(const NetMessage& msg) {
        for (RemotePeer& p : peers) {
            if (!p.alive) continue;
            std::string err;
            if (!p.channel->send(msg, &err)) {
                p.alive = false;
                HOI_WARN("seat %u (%s): send failed: %s", p.seat, p.tag.c_str(), err.c_str());
            }
        }
    }

    // Starts a tick: this seat's commands (if it has any) go out, and - commands or not -
    // this seat takes part in the tick's barrier. A seat with nothing to say still
    // submits an empty list: the session must never guess that a silent seat had
    // nothing to say, and the host cannot advance without a submission from every seat.
    void begin_tick(Tick tick) {
        if (tick == issue_tick) issue_command(tick);
        // A seat with nothing to say still has to vote for the tick: the session must
        // never guess that a silent seat had nothing to say, and the host cannot advance
        // without a submission from every seat. Real commands are submitted first, so
        // `submit_empty` only fills the tick when this seat has none.
        if (lock.submit_empty(tick) == LockstepResult::Desynced) {
            desync = true;
            desync_text = lock.desync_report();
        }
        // This seat's Commands message belongs to the host; the host already holds its
        // own submission, so it only forwards anything else that was queued.
        for (const NetMessage& msg : lock.outgoing()) {
            if (host && msg.kind == NetMessageKind::Commands) continue;
            send_all(msg);
        }
    }

    // Reads and dispatches everything the peers have sent. False once the session cannot
    // continue. `progress` counts messages, which is what resets the stall clock.
    bool pump(int timeout_ms, uint64_t* progress) {
        for (RemotePeer& p : peers) {
            if (!p.alive) continue;
            std::vector<NetMessage> got;
            std::string err;
            const net::LinkStatus status = p.channel->receive(&got, timeout_ms, &err);
            if (status == net::LinkStatus::Closed) {
                p.alive = false;
                stop(net_fmt("peer seat %u (%s) left the session", p.seat, p.tag.c_str()));
                return false;
            }
            if (status == net::LinkStatus::Malformed) {
                stop(net_fmt("malformed message framing from seat %u (%s): %s", p.seat,
                             p.tag.c_str(), err.c_str()));
                return false;
            }
            if (progress != nullptr) *progress += got.size();
            for (NetMessage& msg : got) inbox.push_back(std::move(msg));
        }
        dispatch();
        return !stopped;
    }

    void dispatch() {
        while (!inbox.empty()) {
            NetMessage msg = std::move(inbox.front());
            inbox.pop_front();
            switch (msg.kind) {
                case NetMessageKind::Hello:
                    break;  // the lobby is closed; the agreed roster is authoritative
                case NetMessageKind::Commands:
                    // A joiner only ever hears the host's `Applied`: a Commands message
                    // would let a peer push this peer's tick to ready before the host
                    // has derived the order.
                    if (!host) {
                        stop(net_fmt("seat %u sent Commands to a joiner: only the host derives a "
                                     "tick's order",
                                     msg.seat));
                        break;
                    }
                    if (lock.receive(msg) == LockstepResult::Desynced) {
                        desync = true;
                        desync_text = lock.desync_report();
                    }
                    break;
                case NetMessageKind::Applied:
                    // Only the host derives a tick's order, so only the host may broadcast
                    // one; a joiner that sends `Applied` is trying to choose this peer's
                    // application order.
                    if (host) {
                        stop(net_fmt("seat %u sent Applied to the host: only the host derives a "
                                     "tick's order",
                                     msg.seat));
                        break;
                    }
                    if (lock.receive(msg) == LockstepResult::Desynced) {
                        desync = true;
                        desync_text = lock.desync_report();
                    }
                    break;
                case NetMessageKind::Hash:
                    // Held until the barrier for that tick: a peer's hash for tick T can
                    // arrive before this peer has applied T, and comparing then would
                    // report a divergence that does not exist.
                    hashes[msg.tick][msg.seat] = std::move(msg);
                    break;
                case NetMessageKind::Desync:
                    desync = true;
                    desync_text = msg.text.empty()
                                      ? net_fmt("seat %u reported a desync at tick %llu", msg.seat,
                                                static_cast<unsigned long long>(msg.tick))
                                      : msg.text;
                    break;
                case NetMessageKind::Bye:
                    bye = true;
                    bye_tick = msg.tick;
                    break;
                default:
                    break;
            }
            if (desync || stopped) {
                inbox.clear();
                return;
            }
        }
    }

    // Waits until every seat has submitted for `tick`; `ordered` then holds the list in
    // application order. On the host that is the seats' own submissions; on a joiner it
    // is the host's `Applied` message.
    bool wait_ready(Tick tick, std::vector<Command>* ordered) {
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        while (!lock.ready(tick, ordered)) {
            uint64_t progress = 0;
            if (!pump(kNetPollSliceMs, &progress)) return false;
            const int64_t idle = ms_since(last);
            if (progress > 0) {
                last = std::chrono::steady_clock::now();
            } else if (idle > static_cast<int64_t>(kNetStallMillis)) {
                stop(net_fmt("stalled at tick %llu: not every seat has submitted (seats %s); a "
                             "seat that is behind stalls the session rather than guessing",
                             static_cast<unsigned long long>(tick), seat_list().c_str()));
                return false;
            }
        }
        return true;
    }

    // The hash barrier: after applying tick `t` every peer sends its hash and waits for
    // the others, so the comparison always happens with both peers on the same tick.
    bool barrier(Tick applied_tick) {
        const NetMessage mine = lock.hash_message(*game);
        const Tick key = mine.tick;  // whatever the message calls it, both peers agree
        send_all(mine);

        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        while (hashes[key].size() < peers.size()) {
            uint64_t progress = 0;
            if (!pump(kNetPollSliceMs, &progress)) return false;
            const int64_t idle = ms_since(last);
            if (progress > 0) {
                last = std::chrono::steady_clock::now();
            } else if (idle > static_cast<int64_t>(kNetStallMillis)) {
                stop(net_fmt("stalled at tick %llu waiting for %zu peer hash(es) (seats %s)",
                             static_cast<unsigned long long>(applied_tick),
                             peers.size() - hashes[key].size(), seat_list().c_str()));
                return false;
            }
        }

        for (const auto& entry : hashes[key]) {
            const NetMessage& msg = entry.second;
            if (lock.check_hash(*game, msg) == LockstepResult::Desynced) {
                desync = true;
                desync_text = lock.desync_report();
                NetMessage report;
                report.kind = NetMessageKind::Desync;
                report.seat = local_seat;
                report.tick = applied_tick;
                report.text = desync_text;
                send_all(report);  // tell the others to stop too, never to keep running
                return false;
            }
        }
        hashes.erase(key);
        return true;
    }

    void issue_command(Tick tick) {
        const CountryId cid = lock.local_country();
        Command cmd;
        if (!pick_player_command(*game, cid, tick, &cmd)) {
            std::printf("tick %llu: %s has no command that validates; issuing none\n",
                        static_cast<unsigned long long>(tick), local_tag.c_str());
            return;
        }
        const LockstepResult result = lock.submit(tick, cmd);
        if (result != LockstepResult::Ok) {
            std::printf("tick %llu: %s (%s) command refused by the session: %s\n",
                        static_cast<unsigned long long>(tick), local_tag.c_str(),
                        country_tag(*game, cid).c_str(), net_result_name(result));
            return;
        }
        std::printf("tick %llu: %s submits %s\n", static_cast<unsigned long long>(tick),
                    local_tag.c_str(), describe_command(cmd).c_str());
    }

    void print_tick_line(Tick tick, const std::vector<Command>& ordered) {
        std::string rendered;
        for (const Command& c : ordered) {
            if (!rendered.empty()) rendered += "; ";
            rendered += net_fmt("seat%u/%s %s", seat_of_country(c.country),
                                country_tag(*game, c.country).c_str(), describe_command(c).c_str());
        }
        std::printf("tick %llu: applied %zu command(s) [%s]\n",
                    static_cast<unsigned long long>(tick), ordered.size(), rendered.c_str());
    }

    void print_summary() {
        std::printf("session ticks %llu\n", static_cast<unsigned long long>(ticks_applied));
        std::printf("session commands %llu applied over %llu tick(s) with commands\n",
                    static_cast<unsigned long long>(commands_applied),
                    static_cast<unsigned long long>(ticks_with_commands));
        std::printf("session ended %04d-%02u-%02u (date of tick %llu)\n", game->world.date.year,
                    game->world.date.month, game->world.date.day,
                    static_cast<unsigned long long>(ticks_applied));
        std::printf("desync false\n");
        std::printf("world hash %016llx\n", static_cast<unsigned long long>(world_hash(*game)));
    }

    int finish() {
        if (desync) {
            if (host) {
                NetMessage report;
                report.kind = NetMessageKind::Desync;
                report.seat = local_seat;
                report.tick = ticks_applied;
                report.text = desync_text;
                send_all(report);
            }
            std::printf("\n%s\n", desync_text.c_str());
            std::printf("desync true\n");
            std::printf("session ticks %llu\n", static_cast<unsigned long long>(ticks_applied));
            std::printf("world hash %016llx\n", static_cast<unsigned long long>(world_hash(*game)));
            return 1;
        }
        if (stopped) {
            std::fflush(stdout);
            std::fprintf(stderr, "session failed: %s\n", stop_reason.c_str());
            std::printf("session ticks %llu\n", static_cast<unsigned long long>(ticks_applied));
            std::printf("desync false\n");
            std::printf("world hash %016llx\n", static_cast<unsigned long long>(world_hash(*game)));
            return 1;
        }
        if (host) {
            NetMessage end;
            end.kind = NetMessageKind::Bye;
            end.seat = local_seat;
            end.tick = total_ticks == 0 ? 0 : total_ticks - 1;
            end.text = "session complete";
            send_all(end);
        }
        print_summary();
        return 0;
    }

    int run() {
        // Every seat's country must be non-AI on *every* peer, or the AI layers would
        // issue different commands on different peers: the roster decides it, so both
        // peers derive the same `ai_controlled` from the same seats. `player_country` is
        // a per-peer view (which country this human holds) and is not part of the
        // compared hash, so each peer keeps its own seat there.
        for (const PeerSeat& s : seats) game->set_ai(s.country, false);
        if (lock.local_country().valid()) game->player_country = lock.local_country();

        std::printf("session: %zu seat(s) [%s], local seat %u (%s) %s\n", seats.size(),
                    seat_list().c_str(), local_seat, local_tag.c_str(), host ? "host" : "client");
        std::printf("session: %llu ticks, hash exchange every %u ticks, %zu remote peer(s)\n",
                    static_cast<unsigned long long>(total_ticks), kLockstepHashCadence,
                    peers.size());

        for (Tick tick = 0; tick < total_ticks; ++tick) {
            begin_tick(tick);

            std::vector<Command> ordered;
            if (!wait_ready(tick, &ordered)) break;

            if (host) {
                NetMessage applied;
                applied.kind = NetMessageKind::Applied;
                applied.seat = local_seat;
                applied.tick = tick;
                applied.commands = ordered;  // the authoritative order for this tick
                send_all(applied);
            }

            for (const Command& cmd : ordered) game->queue.push(cmd);
            game->tick_once();
            ++ticks_applied;
            commands_applied += ordered.size();
            if (!ordered.empty()) {
                ++ticks_with_commands;
                print_tick_line(tick, ordered);
            }

            // The hash barrier comes before `mark_applied`: the tick's command list is
            // what the desync report has to name, so it stays stored until the peers
            // agree about the tick.
            if (is_hash_tick(tick) && !barrier(tick)) break;
            lock.mark_applied(tick);
            if (bye) break;
        }

        if (!host && !stopped && !desync && !bye) {
            // The host ends the session. A joiner never infers the end from a missing
            // message: it waits for the Bye, so a session that stopped early is a
            // failure and not a quiet success.
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
            while (!bye) {
                uint64_t progress = 0;
                if (!pump(kNetPollSliceMs, &progress)) break;
                const int64_t idle = ms_since(last);
                if (progress > 0) {
                    last = std::chrono::steady_clock::now();
                } else if (idle > static_cast<int64_t>(kNetStallMillis)) {
                    stop(net_fmt("host went quiet after tick %llu without ending the session",
                                 static_cast<unsigned long long>(ticks_applied - 1)));
                    break;
                }
            }
            if (bye && bye_tick + 1 != total_ticks) {
                stop(net_fmt("host ended the session after tick %llu, expected %llu",
                             static_cast<unsigned long long>(bye_tick + 1),
                             static_cast<unsigned long long>(total_ticks)));
            }
        }
        return finish();
    }
};

// The tick at which this peer submits its one command. Both peers may choose different
// ticks; the barrier does not care when a submission arrives, only that it does.
Tick issue_tick_for(const Options& opt, Tick total) {
    if (opt.net_issue_at < 0) return total / 2;
    const Tick want = static_cast<Tick>(opt.net_issue_at);
    if (want >= total) {
        std::printf("note: --net-issue-at %llu is past the last tick (%llu); using the last tick\n",
                    static_cast<unsigned long long>(want),
                    static_cast<unsigned long long>(total - 1));
        return total - 1;
    }
    return want;
}

// The host: bind, seat the joiners, agree the roster, then run the session.
int run_net_host(const Options& opt, Game& game) {
    if (opt.player_tag.empty()) {
        std::fprintf(stderr, "--host needs --player TAG (the host's country)\n");
        return 2;
    }
    if (opt.players < 1 || opt.players > 8) {
        std::fprintf(stderr, "--players must be between 1 and 8 (this is a LAN transport)\n");
        return 2;
    }

    std::string err;
    net::Listener listener;
    if (!listener.bind_port(opt.port, &err)) {
        std::fprintf(stderr, "host: cannot listen: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening on port %u for %u player(s)\n", listener.port(), opt.players);

    // The roster is built in ascending seat order and is the only definition of the
    // seats; both peers construct `Lockstep` from it.
    std::vector<PeerSeat> seats;
    CountryId host_country;
    if (!find_country_by_tag(game, opt.player_tag, &host_country)) {
        std::fprintf(stderr, "host: no country with tag %s in this scenario\n",
                     opt.player_tag.c_str());
        return 1;
    }
    seats.push_back(PeerSeat{0, host_country, opt.player_tag});

    std::vector<RemotePeer> joiners;
    for (uint32_t seat = 1; seat < opt.players; ++seat) {
        std::string aerr;
        auto conn = listener.accept(-1, &g_stop_requested, &aerr);
        if (conn == nullptr) {
            std::fprintf(stderr, "host: %s\n", aerr.c_str());
            return 1;
        }
        RemotePeer peer;
        peer.seat = seat;
        peer.conn = std::move(conn);
        peer.channel = std::make_unique<net::Channel>(peer.conn.get());

        // The joiner announces itself with its tag, and - when it was told a session size -
        // how many players it expects; the seat is assigned here, so the assignment is
        // identical on every peer.
        const std::string from = peer.conn->peer();
        std::string hello_err;
        bool greeted = false;
        uint32_t announced_players = 0;
        for (int attempt = 0; attempt < 100 && !greeted; ++attempt) {
            std::vector<NetMessage> got;
            const net::LinkStatus status =
                peer.channel->receive(&got, kNetLobbyTimeoutMs / 100, &hello_err);
            if (status == net::LinkStatus::Closed || status == net::LinkStatus::Malformed) break;
            for (NetMessage& msg : got) {
                if (msg.kind == NetMessageKind::Hello && !greeted) {
                    peer.tag = msg.text;
                    announced_players = static_cast<uint32_t>(msg.hash);
                    greeted = true;
                }
            }
        }
        if (!greeted) {
            std::fprintf(stderr, "host: %s connected but never announced a tag (%s)\n", from.c_str(),
                         hello_err.c_str());
            return 1;
        }

        CountryId country;
        std::string refusal;
        if (peer.tag.empty()) {
            refusal = "empty tag";
        } else if (announced_players != 0 && announced_players != opt.players) {
            refusal = "this peer asked for " + std::to_string(announced_players) +
                      " players, the session has " + std::to_string(opt.players);
        } else if (!find_country_by_tag(game, peer.tag, &country)) {
            refusal = "no country with tag " + peer.tag + " in this scenario";
        } else {
            for (const PeerSeat& s : seats) {
                if (s.tag == peer.tag) refusal = "tag " + peer.tag + " is already seated";
                if (s.country == country) refusal = "country " + peer.tag + " is already seated";
            }
        }
        if (!refusal.empty()) {
            NetMessage bye_msg;
            bye_msg.kind = NetMessageKind::Bye;
            bye_msg.text = "refused: " + refusal;
            std::string send_err;
            peer.channel->send(bye_msg, &send_err);
            std::fflush(stdout);
            std::fprintf(stderr, "host: refused %s from %s: %s\n", peer.tag.c_str(), from.c_str(),
                         refusal.c_str());
            return 1;
        }

        seats.push_back(PeerSeat{seat, country, peer.tag});
        std::printf("seat %u: %s joined from %s\n", seat, peer.tag.c_str(), from.c_str());
        joiners.push_back(std::move(peer));
    }
    listener.close();

    // Ascending seat order, ties broken by tag: the sort is explicit so the roster does
    // not depend on message arrival order.
    std::sort(seats.begin(), seats.end(), [](const PeerSeat& a, const PeerSeat& b) {
        if (a.seat != b.seat) return a.seat < b.seat;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.country < b.country;
    });

    const Tick total = opt.use_ticks ? opt.ticks
                                     : opt.days * static_cast<uint64_t>(TICKS_PER_DAY);
    if (total == 0) {
        std::fprintf(stderr, "host: --days/--ticks asks for a zero-tick session\n");
        return 2;
    }

    // The agreed roster: one Hello per seat, seat 0 first, carrying the seat count and
    // the session length so a joiner can size the run before tick 0.
    for (RemotePeer& peer : joiners) {
        std::string send_err;
        for (const PeerSeat& s : seats) {
            NetMessage hello;
            hello.kind = NetMessageKind::Hello;
            hello.seat = s.seat;
            hello.tick = total;
            hello.text = s.tag;  // the tag is the seat's identity; the country follows from it
            if (s.seat == 0) hello.hash = seats.size();
            if (!peer.channel->send(hello, &send_err)) {
                std::fprintf(stderr, "host: cannot send the roster to seat %u: %s\n", peer.seat,
                             send_err.c_str());
                return 1;
            }
        }
    }

    NetSession session;
    session.game = &game;
    session.seats = seats;
    session.peers = std::move(joiners);
    session.local_seat = 0;
    session.local_tag = opt.player_tag;
    session.host = true;
    session.total_ticks = total;
    session.issue_tick = issue_tick_for(opt, total);
    session.lock = Lockstep(seats, 0);
    if (session.peers.empty()) {
        std::printf("session: no joiners; the session has no peer to verify against\n");
    }
    return session.run();
}

// The joiner: connect, be seated, then follow the host's ordered list tick by tick.
int run_net_client(const Options& opt, Game& game) {
    if (opt.player_tag.empty()) {
        std::fprintf(stderr, "--join needs --player TAG (this peer's country)\n");
        return 2;
    }
    std::string host;
    uint16_t port = 0;
    if (!parse_host_port(opt.join_addr, &host, &port)) {
        std::fprintf(stderr, "--join wants HOST:PORT (got \"%s\")\n", opt.join_addr.c_str());
        return 2;
    }

    std::string err;
    auto conn = net::Connection::connect(host, port, kNetConnectTimeoutMs, &err);
    if (conn == nullptr) {
        std::fprintf(stderr, "join failed: %s\n", err.c_str());
        return 1;
    }
    auto channel = std::make_unique<net::Channel>(conn.get());
    std::printf("connected to %s:%u as %s\n", host.c_str(), static_cast<unsigned>(port),
                opt.player_tag.c_str());

    NetMessage hello;
    hello.kind = NetMessageKind::Hello;
    hello.seat = 0;
    // Announce the tag, and the session size when this peer was given one, so the host
    // can refuse a mismatch before tick 0 instead of stalling later.
    hello.hash = opt.players_set ? opt.players : 0;
    hello.text = opt.player_tag;
    if (!channel->send(hello, &err)) {
        std::fprintf(stderr, "join failed: cannot announce this peer: %s\n", err.c_str());
        return 1;
    }

    // The host's own Hello (seat 0, sent first) says how many seats the session has and
    // how long it runs; the rest of the roster follows.
    std::deque<NetMessage> lobby;
    std::vector<PeerSeat> seats;
    size_t expected = 0;
    bool seated = false;
    bool refused = false;
    std::string refusal;
    Tick total = 0;
    uint32_t my_seat = 0;
    bool lobby_done = false;
    std::string roster_error;
    while (!lobby_done) {
        while (!lobby.empty()) {
            NetMessage msg = std::move(lobby.front());
            lobby.pop_front();
            if (msg.kind == NetMessageKind::Bye) {
                refused = true;
                refusal = msg.text;
                lobby_done = true;
                break;
            }
            if (msg.kind != NetMessageKind::Hello) continue;
            if (msg.seat == 0) expected = static_cast<size_t>(msg.hash);
            total = msg.tick;
            // The seat's country is the scenario's country for that tag: both peers
            // resolve it the same way, and an unknown tag is a scenario mismatch.
            CountryId country;
            if (!find_country_by_tag(game, msg.text, &country)) {
                roster_error = "the host seated " + msg.text +
                               ", which is not a country in this peer's scenario";
                lobby_done = true;
                break;
            }
            seats.push_back(PeerSeat{msg.seat, country, msg.text});
            if (msg.text == opt.player_tag) {
                my_seat = msg.seat;
                seated = true;
            }
        }
        if (lobby_done) break;
        if (expected != 0 && seats.size() >= expected) break;
        std::vector<NetMessage> got;
        const net::LinkStatus status = channel->receive(&got, kNetLobbyTimeoutMs, &err);
        if (status == net::LinkStatus::Closed || status == net::LinkStatus::Malformed) {
            std::fprintf(stderr, "lobby: the host closed the connection%s%s\n",
                         err.empty() ? "" : ": ", err.c_str());
            return 1;
        }
        for (NetMessage& msg : got) lobby.push_back(std::move(msg));
        if (status == net::LinkStatus::Idle && got.empty()) {
            std::fprintf(stderr, "lobby: no roster from the host within %d ms\n",
                         kNetLobbyTimeoutMs);
            return 1;
        }
    }

    if (!roster_error.empty()) {
        std::fprintf(stderr, "lobby: %s; both peers need the same scenario, seed, data root and "
                             "mods\n",
                     roster_error.c_str());
        return 1;
    }
    if (refused) {
        std::fprintf(stderr, "lobby: the host did not seat this peer: %s\n", refusal.c_str());
        return 1;
    }
    if (!seated || expected == 0 || seats.size() < expected) {
        std::fprintf(stderr,
                     "lobby: %s was not seated (got %zu of %zu seat(s); the host assigns seats "
                     "by tag)\n",
                     opt.player_tag.c_str(), seats.size(), expected);
        return 1;
    }
    if (opt.players_set && opt.players != expected) {
        std::fprintf(stderr, "lobby: --players %u disagrees with the host's %zu seats\n",
                     opt.players, expected);
        return 1;
    }

    const Tick local_total = opt.use_ticks ? opt.ticks
                                           : opt.days * static_cast<uint64_t>(TICKS_PER_DAY);
    if (local_total != total) {
        std::printf("session: the host runs %llu ticks, this peer's --days/--ticks asks for %llu; "
                    "following the host\n",
                    static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(local_total));
    }

    std::sort(seats.begin(), seats.end(), [](const PeerSeat& a, const PeerSeat& b) {
        if (a.seat != b.seat) return a.seat < b.seat;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.country < b.country;
    });

    RemotePeer host_peer;
    host_peer.seat = 0;
    host_peer.tag = seats.empty() ? std::string("host") : seats.front().tag;
    host_peer.channel = std::move(channel);
    host_peer.conn = std::move(conn);

    NetSession session;
    session.game = &game;
    session.seats = seats;
    session.peers.push_back(std::move(host_peer));
    session.local_seat = my_seat;
    session.local_tag = opt.player_tag;
    session.host = false;
    session.total_ticks = total;
    session.issue_tick = issue_tick_for(opt, total);
    session.lock = Lockstep(seats, my_seat);
    return session.run();
}

int run_net_session(const Options& opt, Game& game) {
    if (opt.host && !opt.join_addr.empty()) {
        std::fprintf(stderr, "--host and --join are mutually exclusive\n");
        return 2;
    }
    if (opt.serve) {
        std::fprintf(stderr, "--serve and --host/--join are mutually exclusive\n");
        return 2;
    }
    if (!opt.save_path.empty() || opt.audit || opt.hashes || opt.summary ||
        opt.save_every_days > 0) {
        std::printf("note: --save/--audit/--hashes/--summary/--save-every-days do not apply to a "
                    "networked session and are ignored\n");
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    return opt.host ? run_net_host(opt, game) : run_net_client(opt, game);
}

}  // namespace

// ---------------------------------------------------------------- play mode --

// True when something is already listening on this port, so `--play` can step past it
// instead of failing on a machine that runs other servers.
bool port_in_use(uint16_t port) {
    net::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == net::kInvalidSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool taken = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                                 static_cast<net::socklen_compat>(sizeof(addr))) == 0;
    net::close_socket(fd);
    return taken;
}

// Launches the default browser without waiting for it: a blocking call here would keep
// the server from ever starting (a browser that hangs on a headless machine is exactly
// the case that must not stall the game).
void open_browser_async(const std::string& url) {
#if defined(_WIN32)
    // ShellExecute returns as soon as the browser process is created.
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();  // detach: the browser must outlive nothing in particular
        ::close(STDIN_FILENO);
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
#if defined(__APPLE__)
        ::execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        ::execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        ::_exit(127);  // nothing sensible left to do in the child
    }
#endif
}

// Opens the browser once the server has had a moment to bind, so the page loads first try.
void open_browser_soon(const std::string& url) {
    std::thread([url]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        open_browser_async(url);
    }).detach();
}

// One-click start: pick a free port, serve, open the browser, and stay in the foreground
// so closing the window stops the game.
int run_play_mode(const Options& opt, Game& game) {
    // A double-click should hand the player a country, not a spectator seat: with no
    // `--player`, take the first living country of the scenario (the shipped one is the
    // recommended power) and hand it to the human on this machine.
    if (!game.player_country.valid()) {
        CountryId pick;
        game.world.countries.for_each([&](CountryId id, const Country& c) {
            if (!pick.valid() && c.alive) pick = id;
        });
        if (pick.valid()) {
            game.player_country = pick;
            game.set_ai(pick, false);
            const Country* c = game.world.country(pick);
            if (c) {
                std::printf("no --player given, so you are playing %s (%s); pass --player TAG to choose another\n",
                            c->tag.c_str(), c->name.c_str());
            }
        }
    }
    std::string err;
    if (!net::socket_layer_init(&err)) {
        std::fprintf(stderr, "cannot start networking: %s\n", err.c_str());
        return 1;
    }
    uint16_t port = opt.port == 0 ? 8080 : opt.port;
    for (int attempt = 0; attempt < 20 && port_in_use(port); ++attempt) {
        ++port;
    }
    char url[64];
    std::snprintf(url, sizeof(url), "http://127.0.0.1:%u", static_cast<unsigned>(port));

    std::printf("\n");
    std::printf("  Clean Room Grand Strategy\n");
    std::printf("  -------------------------\n");
    {
        const Country* me = game.world.country(game.player_country);
        std::printf("  playing as %s   (%s)\n", me ? me->tag.c_str() : "observer",
                    opt.scenario.c_str());
    }
    std::printf("  open %s in your browser (it should open by itself)\n", url);
    std::printf("  keep this window open; close it to stop the game\n\n");
    std::fflush(stdout);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    ServerOptions sopts;
    sopts.port = port;
    sopts.web_root = opt.web_root;
    sopts.autosave_days = opt.autosave_days;
    sopts.verbose = opt.verbose;
    if (!opt.save_path.empty()) sopts.save_path = opt.save_path;
    open_browser_soon(url);
    return run_server(game, sopts, &g_stop_requested);
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) return 2;

    log_set_level(opt.quiet ? LogLevel::Error : (opt.verbose ? LogLevel::Debug : LogLevel::Info));

    Game game;
    std::string err;
    if (opt.validate_content) return run_content_validation(opt);
    if (!opt.load_path.empty()) {
        if (!load_game(game, opt.load_path, &err)) {
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("loaded %s: %s at %04d-%02u-%02u (tick %llu)\n", opt.load_path.c_str(),
                    game.scenario_path.c_str(), game.world.date.year, game.world.date.month,
                    game.world.date.day, static_cast<unsigned long long>(game.world.tick));
    } else {
        ModLoadReport report;
        if (!Game::create(opt.data_root, opt.scenario, opt.seed, &game, &err, opt.mod_roots,
                          &report)) {
            std::fprintf(stderr, "startup failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("scenario %s loaded: %zu provinces, %zu states, %zu countries, %zu divisions\n",
                    opt.scenario.c_str(), game.world.provinces.size(), game.world.states.size(),
                    game.world.countries.size(), game.world.divisions.size());
        print_content_diagnostics(report, game.content, false);
        if (!report.errors.empty()) {
            // A mod that fails to apply must not be silently absent from the run.
            std::fflush(stdout);
            std::fprintf(stderr, "startup failed: %zu mod error(s); fix them or drop --mods\n",
                         report.errors.size());
            return 1;
        }
    }

    if (!opt.player_tag.empty()) {
        game.world.countries.for_each([&](CountryId cid, const Country& c) {
            if (c.tag == opt.player_tag) {
                game.player_country = cid;
                game.set_ai(cid, false);
                std::printf("player controls %s (%s)\n", c.tag.c_str(), c.name.c_str());
            }
        });
        if (!game.player_country.valid()) {
            std::fprintf(stderr, "no country with tag %s\n", opt.player_tag.c_str());
            return 1;
        }
    }

    const uint64_t ticks = opt.use_ticks ? opt.ticks : opt.days * static_cast<uint64_t>(TICKS_PER_DAY);

    // Every mode that opens a socket (serve, play, host, join) needs the socket layer up
    // first; on Windows that means Winsock, on Unix it is a no-op. Doing it here means no
    // socket path can forget it.
    {
        std::string net_err;
        if (!net::socket_layer_init(&net_err)) {
            std::fprintf(stderr, "cannot start networking: %s\n", net_err.c_str());
            return 1;
        }
    }

    if (opt.host || !opt.join_addr.empty()) return run_net_session(opt, game);

    if (opt.play) return run_play_mode(opt, game);

    if (opt.serve) {
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        ServerOptions sopts;
        sopts.port = opt.port;
        sopts.web_root = opt.web_root;
        sopts.autosave_days = opt.autosave_days;
        sopts.verbose = opt.verbose;
        if (!opt.save_path.empty()) sopts.save_path = opt.save_path;
        std::printf("starting server; the simulation pauses until the client unpauses\n");
        return run_server(game, sopts, &g_stop_requested);
    }
    const uint64_t save_interval =
        opt.save_every_days > 0 ? opt.save_every_days * static_cast<uint64_t>(TICKS_PER_DAY) : 0;

    const uint64_t batch = 24;  // report daily
    uint64_t done = 0;
    while (done < ticks) {
        const uint64_t step = std::min(batch, ticks - done);
        game.run_ticks(step);
        done += step;
        if (save_interval > 0 && done % save_interval == 0) {
            const std::string path = opt.save_path.empty() ? "build/roundtrip.save" : opt.save_path;
            const uint64_t before = world_hash(game);
            if (!save_game(game, path, &err)) {
                std::fprintf(stderr, "periodic save failed: %s\n", err.c_str());
                return 1;
            }
            Game reloaded;
            if (!load_game(reloaded, path, &err)) {
                std::fprintf(stderr, "periodic load failed: %s\n", err.c_str());
                return 1;
            }
            const uint64_t after = world_hash(reloaded);
            if (before != after) {
                std::fprintf(stderr, "SAVE ROUND TRIP MISMATCH at tick %llu: %016llx vs %016llx\n",
                             static_cast<unsigned long long>(game.world.tick),
                             static_cast<unsigned long long>(before),
                             static_cast<unsigned long long>(after));
                return 1;
            }
            std::printf("save/load round trip OK at day %llu (hash %016llx)\n",
                        static_cast<unsigned long long>(done / 24),
                        static_cast<unsigned long long>(before));
        }
    }

    std::printf("simulated %llu ticks (%llu days) ending %04d-%02u-%02u\n",
                static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(ticks / 24),
                game.world.date.year, game.world.date.month, game.world.date.day);
    std::printf("countries alive %zu, wars %zu, battles %zu, divisions %zu\n",
                game.world.countries.size(), game.world.wars.size(), game.world.battles.size(),
                game.world.divisions.size());
    std::printf("commands applied %zu (of %zu recorded)\n",
                [&] {
                    size_t applied = 0;
                    for (const auto& r : game.log.records)
                        if (r.result == CommandResult::Applied) ++applied;
                    return applied;
                }(),
                game.log.records.size());

    if (opt.summary) print_country_summary(game);
    if (!opt.inspect_country.empty()) inspect_country(game, opt.inspect_country);
    if (opt.inspect_province != 0) inspect_province(game, ProvinceId(static_cast<uint32_t>(opt.inspect_province)));
    if (opt.inspect_supply != 0) inspect_supply(game, ProvinceId(static_cast<uint32_t>(opt.inspect_supply)));
    if (opt.inspect_trade) inspect_trade(game);
    if (!opt.inspect_battle.empty()) {
        inspect_battle(game, BattleId(static_cast<uint32_t>(std::strtoul(opt.inspect_battle.c_str(), nullptr, 10))));
    }
    if (opt.hashes) std::printf("\n%s", hash_report(game).c_str());
    print_metrics(game);

    if (opt.audit) {
        std::string report;
        const bool ok = audit_world(game, &report);
        std::printf("\n%s", report.c_str());
        if (!ok) return 1;
    }

    if (!opt.save_path.empty()) {
        if (!save_game(game, opt.save_path, &err)) {
            std::fprintf(stderr, "save failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("saved %s\n", opt.save_path.c_str());
    }
    return 0;
}
