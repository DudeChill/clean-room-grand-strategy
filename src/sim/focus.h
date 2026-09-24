#pragma once
// National focus trees (spec section 57).
//
// A focus is data: prerequisites, mutual exclusions, availability and bypass triggers,
// a duration and an effect block, all evaluated by the script engine. The engine only
// owns the selection, the daily progress and the completion.

#include <string>
#include <vector>

#include "core/types.h"
#include "data/content.h"

namespace hoi {

struct Game;

// Index of a focus inside Content::focuses.
using FocusIndex = uint32_t;
inline constexpr FocusIndex INVALID_FOCUS = 0xFFFFFFFFu;

// True when the focus exists, is not completed, its prerequisites are completed, no
// mutually exclusive focus is completed, and its `available` trigger passes.
bool focus_available(const Game& g, CountryId country, FocusIndex focus);

// True when the focus's `bypass` trigger passes: the focus may be skipped (the engine
// marks it completed without its effects when the AI or the player asks for the bypass).
bool focus_bypassable(const Game& g, CountryId country, FocusIndex focus);

// Country-level entry points used by the command layer.
bool focus_select(Game& g, CountryId country, FocusIndex focus);
void focus_cancel(Game& g, CountryId country);
bool focus_complete(Game& g, CountryId country, FocusIndex focus);

// Daily progress for every country with a selected focus; called by phase_focuses.
void phase_focuses(Game& g);

// AI: pick the best available focus per country, scored by its effects and the
// country's situation, recording the reasons in the AI layer log.
void ai_focus_layer(Game& g, Country& c);

// Human-readable description of a focus state, for tooltips and the debugger.
std::string focus_status_text(const Game& g, CountryId country, FocusIndex focus);

}  // namespace hoi
