#pragma once
// Data-driven scripting: triggers, effects and the scope they evaluate against.
//
// Focus trees, events and decisions are all expressed with this one vocabulary, so
// adding content never means touching engine code (spec sections 57, 58, 59, 91).

#include <string>
#include <vector>

#include "core/json.h"
#include "core/types.h"

namespace hoi {

struct Game;

// What a trigger or effect is evaluated against. Only `country` is required; the other
// scopes are filled where the content asks for them (a decision targeted at a state, an
// event fired for a province).
struct ScriptScope {
    Game* game = nullptr;
    CountryId country;
    StateId state;
    ProvinceId province;
    CountryId target_country;  // the other side of a diplomatic interaction
};

// Evaluates a trigger. `trigger` is a JSON object; an empty or null trigger is true.
// Unknown keys are a content error and evaluate to false, never to true.
bool eval_trigger(const ScriptScope& scope, const Json& trigger);

// Applies an effect block. Unknown keys are ignored after being reported once through
// the game's log; effects never throw and never leave partial state (each effect is
// applied atomically).
void apply_effects(const ScriptScope& scope, const Json& effects);

// Convenience: evaluate a `{"chance": p}`-style random trigger with the game's RNG.
bool eval_random_chance(Game& g, double chance);

// Formats a trigger or effect failure for `Content::load_errors` style reporting.
std::string describe_script_error(const std::string& key, const std::string& reason);

}  // namespace hoi
