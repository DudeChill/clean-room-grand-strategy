# Scripting: triggers, effects, focuses, events and decisions

Status: implemented, tested (`tests/test_script.cpp`).
Confidence: MODERATE. The trigger/effect vocabulary below is a **design choice made
for this engine**, not a reverse-engineered copy of any shipping game's script
format. Keys were chosen to cover exactly the mechanics this engine simulates
(political power, stability, war support, manpower, fuel, factories, divisions,
technology, laws, wars, focuses, events, decisions, variables and flags). Where a
concept is not yet a first-class world field the implementation says so explicitly
(see "Storage notes" and "Known uncertainty").

Focus trees (POL-001), events/decisions (POL-002) and scripted modifiers (POL-003)
are all expressed with this one vocabulary, so adding content never means touching
engine code.

## Where the code lives

| Piece | File |
| --- | --- |
| Evaluator + effect application | `src/sim/script.cpp` (`src/sim/script.h`) |
| Focus selection/progress/completion | `src/sim/focus.cpp` |
| Events, decisions, timers, AI layers | `src/sim/events.cpp` |
| Content structs and lookups | `src/data/content.h` |
| Command plumbing | `src/sim/commands.cpp` |

`ScriptScope { game, country, state, province, target_country }` carries the
evaluation context. A country trigger only needs `country`; a state-scoped trigger
or a state-targeted decision fills `state`; `target_country` is the other side of a
diplomatic interaction (it is the default target of `opinion` and `add_opinion`).

## Triggers

`eval_trigger(scope, trigger)` returns a `bool`. Rules that apply everywhere:

* A `null`/absent trigger, or an empty object `{}`, is **true**.
* A trigger that is not a JSON object is a content error and evaluates to **false**.
* Every key of a multi-key object must hold (implicit AND).
* An unknown key is a content error and evaluates to **false** — never to true. It
  is reported once through the game log, and it never mutates state.
* Comparison values are objects: `{"gte": 50}`. A bare number is accepted as `eq`.
  A string where a number is expected is a content error (false), so a typo can
  never silently pass.

### Boolean combinators and randomness

| Key | Form | True when |
| --- | --- | --- |
| `all` | array of triggers | every element is true |
| `any` | array of triggers | at least one element is true |
| `not` | trigger object, or array | the inner trigger (array ANDed) is false |
| `chance` | number 0..1 | a draw on `RngStream::Events` is below the value |

`chance` is the only trigger that touches the RNG. `eval_random_chance(game, p)` is
the same rule: `p <= 0` is false, `p >= 1` is true, otherwise it draws from
`RngStream::Events`. Same seed and same call sequence therefore give the same
result.

### Country statistics (comparator)

`political_power`, `stability`, `war_support`, `manpower`, `fuel`, `factories`,
`num_divisions`, `year`.

```json
{ "political_power": { "gte": 150 } }
{ "factories": { "gt": 20 } }
{ "year": { "eq": 1939 } }
```

* `factories` counts civilian + military factories + dockyards in states the
  country **controls** (`count_factories`).
* `num_divisions` counts the country's alive divisions.
* `year` uses the current world date.

### Identity and ideology

| Key | Form | True when |
| --- | --- | --- |
| `is_ai` | bool | the country's controller type matches |
| `ideology` | string | the country's ideology name matches (case/separator-insensitive) |
| `at_war` | bool | the country is at war |
| `at_war_with` | country tag | the country is at war with that tag |

### Diplomacy

| Key | Form | True when |
| --- | --- | --- |
| `opinion` | `{"target": "TAG", "gte": 25}` | relation value with the target passes the comparator |

`target` may be omitted when `ScriptScope.target_country` is set. If neither is
available the trigger is a content error (false).

### Politics, technology and flags

| Key | Form | True when |
| --- | --- | --- |
| `has_tech` | technology key | the technology is completed |
| `completed_focus` | focus key | the focus is in the country's completed list |
| `has_flag` / `has_country_flag` | flag name | the country flag is set |
| `has_decision` | decision key | the decision is currently active (taken) |
| `var` | `{"name": "x", "gte": 1}` | the `World::script_vars` entry passes the comparator (missing = 0) |

### Territory and dates

| Key | Form | True when |
| --- | --- | --- |
| `owns_state` | state key | the state's owner is this country |
| `controls_state` | state key | the state's controller is this country |
| `date_after` | `"YYYY-MM-DD"` | the current date is on or after it |
| `date_before` | `"YYYY-MM-DD"` | the current date is on or before it |

### State scope (needs `ScriptScope.state`)

| Key | Form | True when |
| --- | --- | --- |
| `state_controller_is_owner` | bool | the state's controller equals its owner |
| `state_factories` | comparator | `State::total_factories()` passes |
| `state_has_flag` | flag name | the state flag is set (see Storage notes) |

Without a state scope these are content errors and evaluate to false.

### State keys

A state key resolves in this order:

1. an exact `State::name` match (ascending id, first hit);
2. `"s<digits>"` or bare `"<digits>"` → `StateId(digits - 1)`, the map-file key
   convention (map state `s1` is the first state created, and so on).

The scenario loader stores the map's display name in `State::name`, so content
should use the `sN` form unless it deliberately matches a display name. World has no
runtime state-key index yet; see "Known uncertainty".

## Effects

`apply_effects(scope, effects)` applies an object of effects. Rules:

* A `null`/absent block does nothing.
* Each **effect is atomic**: the handler resolves every input first and then applies
  it once, or reports and skips it. Effects never throw and never leave a partial
  change behind.
* An unknown key is reported once through the game log and ignored; it never aborts
  the rest of the block.
* Numbers must be finite. `NaN`/`Inf` (or a string) is a content error and the
  effect is skipped, so no non-finite value ever reaches state.
* Values that cannot go below zero are clamped (`political_power`, `manpower`,
  `fuel`); shares (`stability`, `war_support`) are clamped to `[0, 1]`; relations
  are clamped to `[-100, 100]`.

| Key | Form | Effect |
| --- | --- | --- |
| `add_political_power` | number | adds to the pool (floor 0) |
| `add_stability` | number | adds to stability (clamp 0..1) |
| `add_war_support` | number | adds to war support (clamp 0..1) |
| `add_manpower` | number | adds to the recruitable pool (floor 0) |
| `add_fuel` | number | adds to the fuel stockpile (floor 0) |
| `add_opinion` | `{"target": "TAG", "value": 10}` | adds to the relation value (clamp ±100) |
| `declare_war` | `{"target": "TAG", "annex": false, "puppet": false}` | declares war (existing diplomacy API) |
| `add_tech` | technology key | completes the technology and propagates its modifiers/unlocks (`apply_tech_effects`) |
| `set_law` | law key | enacts the law (`apply_law_change`; no political-power cost) |
| `add_modifier` | `{"kind": "DivisionAttack", "value": 0.05, "days": 90, "source": "focus_x"}` | pushes a `TimedModifier` |
| `complete_focus` | focus key | completes the focus and applies its effects (`focus_complete`) |
| `trigger_event` | `{"key": "ev", "days": 7}` | fires the event now, or schedules it |
| `set_flag` / `clear_flag` | flag name | adds/removes a country flag |
| `set_state_flag` / `clear_state_flag` | flag name | adds/removes a `State::flags` entry (needs a state scope) |
| `set_variable` | `{"name": "x", "value": 1}` | sets a `World::script_vars` entry |
| `add_to_variable` | `{"name": "x", "value": 1}` | adds to a variable (missing = 0) |
| `add_claim` | state key | adds the claimant to `State::core_owners` (once) |

Details:

* `declare_war` uses `scope.state` as the war goal claim when the scope is
  state-scoped, otherwise the target's capital. `annex` and `puppet` mark the goal.
* `add_tech` is idempotent: re-applying an owned technology is a no-op and never
  doubles its modifiers.
* `add_modifier`: `kind` is a `ModifierKind` name resolved case- and
  separator-insensitively (`"DivisionAttack"`, `"division_attack"` and
  `"division attack"` all match). `days` absent or negative means permanent
  (`days_left = -1`); `days: 0` is an immediate zero-length modifier. `source`
  defaults to `"script"` and is what tooltips and eventual removal key on.
* `trigger_event`: `days > 0` pushes a `DelayedEvent` at
  `tick + days * 24`; `days` absent, `0` or negative fires immediately through
  `fire_event`.
* `set_state_flag` / `clear_state_flag` write `State::flags` and require a state
  scope; without one they are a reported no-op.
* `add_claim` adds the claimant to `State::core_owners`, the list the war and peace
  code reads, and never adds a duplicate.
* `complete_focus` guards against already-completed focuses and against content
  recursion (effect depth limit).

## Worked examples

### A focus: industry investment

```json
{
  "key": "invest_industry",
  "tree": "VLA",
  "x": 0, "y": 0,
  "days": 70,
  "prerequisites": [],
  "mutually_exclusive": ["invest_army"],
  "available": { "all": [ { "political_power": { "gte": 50 } }, { "not": { "at_war": true } } ] },
  "bypass": { "at_war": true },
  "effects": {
    "add_modifier": { "kind": "ConstructionSpeed", "value": 0.05, "days": 180, "source": "invest_industry" },
    "add_political_power": 25,
    "set_flag": "industry_drive"
  }
}
```

`focus_available` is true while `available` passes and no prerequisite or mutually
exclusive focus is completed; selecting it starts the timer; on completion the
engine applies `effects`. `bypass` lets the AI or a player skip it (completed
without `effects`) when the situation has moved on.

### An event with a choice

```json
{
  "key": "dockyard_expansion",
  "title": "Dockyard Expansion",
  "fire_only_once": true,
  "trigger": { "all": [ { "year": { "gte": 1937 } }, { "factories": { "gte": 15 } } ] },
  "immediate": { "add_political_power": -10 },
  "options": [
    { "name": "Fund it", "ai_weight": 1.2, "effects": { "add_modifier": { "kind": "DockyardOutput", "value": 0.10, "days": 365 } } },
    { "name": "Decline", "ai_weight": 0.8, "effects": { "add_war_support": -0.02 } }
  ]
}
```

The country receives the event in its pending list and must choose an option (a
human by command, the AI by weight). `fire_only_once` events are tracked per
country in `Country::fired_events`. `trigger_event` lets any effect fire an event
by key, optionally delayed.

### A state-targeted decision

```json
{
  "key": "fortify_border",
  "name": "Fortify the Border",
  "targets_state": true,
  "cost_pp": 50,
  "days_remove": 0,
  "visible": { "controls_state": "s3" },
  "available": { "all": [ { "var": { "name": "fort_budget", "gte": 1 } }, { "state_factories": { "gte": 2 } } ] },
  "effects": {
    "set_variable": { "name": "fort_budget", "value": 0 },
    "add_modifier": { "kind": "DivisionDefense", "value": 0.05, "source": "fortify_border" },
    "add_claim": "s3"
  }
}
```

`decision_visible` and `decision_available` are pure: they evaluate triggers and
report to the UI/AI without mutating anything. Taking a decision deducts
`cost_pp`, applies `effects` and starts the removal timer; when the timer expires
the engine applies `remove_effect` and starts the cooldown.

## Determinism

* All content lookups go through `Content` maps keyed by string; iteration over the
  world is by ascending id (`Store::for_each`). No unordered-container iteration
  touches gameplay state.
* `chance` draws only from `RngStream::Events`, so adding a `chance` in one system
  cannot shift the numbers another system sees.
* `script_vars` is a `std::map`, so variable order is deterministic.
* Non-finite numbers are rejected at the boundary, so no `NaN`/`Inf` can enter
  state.

## Failure states / edge cases

* unknown trigger key → false (reported once); unknown effect key → ignored
  (reported once)
* non-object trigger/effect block → false / no-op, reported
* a string used as a number, or a comparator without a bound → content error
* `complete_focus` on a missing or already-completed focus → reported no-op
* `declare_war` on self or an unknown/dead country → reported no-op
* `trigger_event` with an unknown event key → reported no-op
* `all`/`any`/`not` nesting deeper than the engine limit (`32`) → reported false
* `complete_focus` recursion through effects is bounded by the same depth limit

## Storage notes

State flags and claims are first-class world state:

* `state_has_flag` reads `State::flags`; `set_state_flag` / `clear_state_flag`
  write it (state-scoped effects).
* `add_claim` adds the country to `State::core_owners`, the list the war and peace
  code already reads to settle goals. No separate claim registry exists.

## Known uncertainty

* The vocabulary is our design choice, not a match to another game's script syntax.
* State keys resolve by display name or `sN` index because `World` has no state-key
  index; a proper index mapping the map file's `key` to `StateId` should be added.
* `chance` inside a `visible`/`available` trigger would consume the events RNG
  during command validation, which is otherwise pure. Keep randomness in event
  `trigger` blocks and focus/decision `effects`, not in visibility triggers.