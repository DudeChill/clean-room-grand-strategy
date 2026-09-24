#pragma once
// Game: the authoritative aggregate. Owns world state, content, RNG streams,
// command queue/log, AI state and metrics. Everything the simulation needs to run
// headless lives here; the renderer and network layer are optional observers.

#include <cstdint>
#include <string>
#include <vector>

#include "core/rng.h"
#include "core/types.h"
#include "data/content.h"
#include "save/save.h"
#include "sim/ai/ai.h"
#include "sim/commands.h"
#include "sim/world.h"

namespace hoi {

struct SimEvent {
    Tick tick = 0;
    std::string kind;  // "war", "capitulation", "production", ...
    std::string text;
    CountryId country;
};

struct SimMetrics {
    double ms_commands = 0.0;
    double ms_diplomacy = 0.0;
    double ms_movement = 0.0;
    double ms_combat = 0.0;
    double ms_territory = 0.0;
    double ms_supply = 0.0;
    double ms_air = 0.0;
    double ms_naval = 0.0;
    double ms_trade = 0.0;
    double ms_industry = 0.0;
    double ms_research = 0.0;
    double ms_training = 0.0;
    double ms_politics = 0.0;
    double ms_focuses = 0.0;
    double ms_events = 0.0;
    double ms_spirits = 0.0;
    double ms_weather = 0.0;
    double ms_ai = 0.0;
    double ms_ai_design = 0.0;   // the equipment-design layer inside the AI phase
    double ms_ai_industry = 0.0;
    double ms_ai_trade = 0.0;
    double ms_ai_production = 0.0;
    double ms_ai_military = 0.0;
    double ms_ai_politics = 0.0;
    double ms_ai_research = 0.0;
    double ms_ai_diplomacy = 0.0;
    double ms_cleanup = 0.0;
    double ms_tick_total = 0.0;
    std::vector<double> tick_history;  // capped ring of recent tick durations

    void note_tick(double ms) {
        ms_tick_total += ms;
        tick_history.push_back(ms);
        if (tick_history.size() > 4096) tick_history.erase(tick_history.begin());
    }
    [[nodiscard]] double average_tick_ms() const {
        if (tick_history.empty()) return 0.0;
        double sum = 0.0;
        for (double v : tick_history) sum += v;
        return sum / static_cast<double>(tick_history.size());
    }
    [[nodiscard]] double percentile_tick_ms(double p) const;
};

struct Game {
    World world;
    Content content;
    RngSet rng;
    CommandQueue queue;
    CommandLog log;
    AiState ai;
    SimMetrics metrics;
    std::vector<SimEvent> events;
    std::vector<uint8_t> ai_controlled;  // per country id: 1 = AI plays it
    CountryId player_country;            // INVALID in observer mode
    GameDate start_date;
    std::string scenario_path;
    std::string data_root;
    uint64_t seed = 0;
    uint64_t ticks_run = 0;

    // Builds a game from data + scenario, seeding RNG streams deterministically.
    static bool create(const std::string& data_root, const std::string& scenario_path,
                       uint64_t seed, Game* out, std::string* err);

    // Advances exactly one simulation hour through the fixed phase order.
    void tick_once();
    void run_ticks(uint64_t count);

    void log_event(const std::string& kind, const std::string& text, CountryId c = CountryId{});

    [[nodiscard]] bool is_ai(CountryId c) const {
        return c.valid() && c.v < ai_controlled.size() && ai_controlled[c.v] != 0;
    }
    void set_ai(CountryId c, bool enabled) {
        if (!c.valid()) return;
        if (ai_controlled.size() <= c.v) ai_controlled.resize(c.v + 1, 1);
        ai_controlled[c.v] = enabled ? 1 : 0;
    }
};

// World auditor (spec section 81): checks invariants that must never break.
// Returns true when the world is consistent; `report` lists every violation.
bool audit_world(const Game& g, std::string* report);

// Deterministic guard used by tests and the CLI: returns a list of violations of
// simulation invariants (negative stockpiles, dangling references, ...).
std::vector<std::string> check_invariants(const Game& g);

}  // namespace hoi
