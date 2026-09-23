// Command line entry point.
//
// Modes:
//   game --headless --days 365            run a campaign and report
//   game --scenario <file> --seed <n>     choose scenario and deterministic seed
//   game --save <file> / --load <file>    persistence round trip
//   game --hashes                         print subsystem hashes (determinism oracle)
//   game --audit                          run the world auditor
//   game --player TAG                     hand a country to the player (AI plays the rest)

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/types.h"
#include "game/game.h"
#include "game/server.h"
#include "save/save.h"
#include "sim/industry.h"
#include "sim/research.h"
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
    uint64_t inspect_province = 0;
    uint64_t inspect_supply = 0;
    uint64_t seed = 12345;
    uint64_t days = 30;
    uint64_t ticks = 0;
    uint64_t save_every_days = 0;
    uint64_t autosave_days = 0;
    uint16_t port = 8080;
    bool use_ticks = false;
    bool hashes = false;
    bool audit = false;
    bool summary = false;
    bool quiet = false;
    bool verbose = false;
    bool serve = false;
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
        "  --port <n>            server port (default 8080)\n"
        "  --web <dir>           static web root (default web)\n"
        "  --autosave-days <n>   autosave interval while serving\n"
        "  --hashes              print subsystem hashes\n"
        "  --audit               run the world auditor\n"
        "  --summary             print per-country summary\n"
        "  --inspect-country TAG print full state of one country\n"
        "  --inspect-province ID print province state and supply\n"
        "  --inspect-supply ID   print the supply route of a province\n"
        "  --inspect-battle ID   print a battle breakdown (attacker/defender/debug)\n"
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
        } else if (a == "--serve") {
            o->serve = true;
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
        } else if (a == "--inspect-battle") {
            if (!next(&o->inspect_battle)) return false;
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
                "movement %.3f | combat %.3f | territory %.3f | supply %.3f | industry %.3f | "
                "research %.3f | training %.3f | politics %.3f | weather %.3f | ai %.3f | "
                "cleanup %.3f\n",
                m.average_tick_ms(), m.ms_commands, m.ms_diplomacy, m.ms_movement, m.ms_combat,
                m.ms_territory, m.ms_supply, m.ms_industry, m.ms_research, m.ms_training,
                m.ms_politics, m.ms_weather, m.ms_ai, m.ms_cleanup);
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
    std::printf("  stockpile:\n");
    for (size_t i = 0; i < c.equipment_stockpile.size(); ++i) {
        if (c.equipment_stockpile[i] <= 0.0) continue;
        const EquipmentDef* def = g.content.equipment_def(EquipmentId(static_cast<uint32_t>(i)));
        if (def) std::printf("    %-24s %.1f\n", def->key.c_str(), c.equipment_stockpile[i]);
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

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) return 2;

    log_set_level(opt.quiet ? LogLevel::Error : (opt.verbose ? LogLevel::Debug : LogLevel::Info));

    Game game;
    std::string err;
    if (!opt.load_path.empty()) {
        if (!load_game(game, opt.load_path, &err)) {
            std::fprintf(stderr, "load failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("loaded %s: %s at %04d-%02u-%02u (tick %llu)\n", opt.load_path.c_str(),
                    game.scenario_path.c_str(), game.world.date.year, game.world.date.month,
                    game.world.date.day, static_cast<unsigned long long>(game.world.tick));
    } else {
        if (!Game::create(opt.data_root, opt.scenario, opt.seed, &game, &err)) {
            std::fprintf(stderr, "startup failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("scenario %s loaded: %zu provinces, %zu states, %zu countries, %zu divisions\n",
                    opt.scenario.c_str(), game.world.provinces.size(), game.world.states.size(),
                    game.world.countries.size(), game.world.divisions.size());
        for (const auto& e : game.content.load_errors) {
            std::printf("content warning: %s\n", e.c_str());
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
