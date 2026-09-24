#pragma once
// AI: a first-class player (spec sections 66-70, 163-166).
//
// The AI is layered (industrial, production, research, military, diplomatic) and
// acts exclusively through the same commands players use. It never edits world
// state directly and never receives information a player could not have.

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"
#include "sim/commands.h"

namespace hoi {

struct Game;

enum class AiLayer : uint8_t {
    Industry = 0,
    Research,
    Production,
    Military,
    Politics,
    Diplomacy,
    Count
};

const char* ai_layer_name(AiLayer l);

// One scored decision, kept for the AI debugger (spec section 68).
struct AiReason {
    std::string what;
    double score = 0.0;
    std::vector<std::pair<std::string, double>> factors;
};

struct AiLayerState {
    uint32_t last_run_tick = 0;
    uint32_t interval_ticks = 24;  // how often this layer plans
    std::vector<AiReason> last_reasons;
};

struct AiState {
    AiLayerState layers[static_cast<int>(AiLayer::Count)];
    // Per-country strategic posture, recomputed by the strategic layer.
    std::vector<uint8_t> posture;  // indexed by country id: 0=peace,1=defensive,2=offensive
    uint64_t decisions_made = 0;
    uint64_t commands_issued = 0;

    [[nodiscard]] AiLayerState& layer(AiLayer l) { return layers[static_cast<int>(l)]; }
};

// Plans for every AI-controlled country and pushes commands into `g.queue`.
// Runs in phase 11 so the commands apply at the start of the next tick.
void phase_ai(Game& g);

// Politics layer: focus selection, event choices and decisions. Declared here and
// defined in sim/focus.cpp and sim/events.cpp.
void ai_politics_layer(Game& g, Country& c);

// Single-country entry points, used by tests to exercise one layer in isolation.
void ai_industry_layer(Game& g, Country& c);
void ai_research_layer(Game& g, Country& c);
void ai_production_layer(Game& g, Country& c);
void ai_military_layer(Game& g, Country& c);
void ai_diplomacy_layer(Game& g, Country& c);

}  // namespace hoi
