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
