# TODO

Rules (spec sections 149-150): every item needs an acceptance condition and a
done condition. No open-ended items.

## Blocker / major

* [ ] **AIR-001: air warfare simulation**
  Acceptance: air wings exist as entities with base, region, mission, range, sorties,
  detection; air superiority per region is computed from real sorties and modifies
  land combat; losses produce equipment demand; AI assigns wings.
  Done: golden test flying a wing, taking losses, receiving replacements.

* [ ] **NAVY-001: naval warfare simulation**
  Acceptance: ships, task forces, fleets, sea regions, detection, positioning,
  screening, convoy raiding/escort; naval invasion transports divisions and
  establishes supply.
  Done: golden test for naval invasion end to end.

* [ ] **DIPLO-001: peace conference**
  Acceptance: peace resolution assigns territory and puppets from actual control,
  propagates through ownership, supply, industry and AI goals.
  Done: golden test where a won war redistributes states deterministically.

* [ ] **POL-001: focus trees**
  Acceptance: data-driven focuses with prerequisites, exclusions, availability,
  completion, bypass, effects, AI weighting.
  Done: mod-fixture focus tree completes in game and applies effects.

* [ ] **POL-002: events and decisions**
  Acceptance: data-driven triggers, options, effects, scopes, timers, targeting.
  Done: an event fires from a decision and mutates state; no C++ per event.

## Moderate

* [ ] **ECON-001: trade and convoys** - resource imports/exports, convoy demand,
  blockade effects on imports and supply.
* [ ] **ECON-002: equipment designers** - tank/aircraft/ship variants with components
  changing stats and cost.
* [ ] **MIL-001: battle plan execution** - offensive plans advance along the plan's
  target line automatically rather than requiring per-division orders.
* [ ] **MIL-002: garrison and resistance suppression** - garrison divisions assigned
  to states, suppressing resistance.
* [ ] **SUPPLY-001: motorisation** - army motorisation level consuming trucks to
  extend hub radius.
* [ ] **AI-001: naval and air layers** - AI builds and uses fleets and air wings.
* [ ] **AI-008** explicit AI crisis recovery behaviour: posture scoring reacts to force
  ratios; no dedicated recovery planning.
* [ ] **AI-009** AI aggression tuning: one observer year of the shipped scenario yields
  31 wars and 3 capitulations. Acceptance: with default constants a 2-year observer run
  produces 4-10 wars, at least two peace settlements, and no country eliminated before
  year 2; the tuned constants and their measurements go into
  `docs/mechanics/ai_scoring.md` and `docs/benchmarks/history.tsv`.

## Tooling / process

* [ ] **TOOL-001: headless stress ladder** - STRESS-1..5 scenarios with recorded
  tick time, memory, save size.
* [ ] **TOOL-002: long-run observer automation** - scripted 1/6/12/36/60-month runs
  with crash, NaN, leak and runaway-value detection.
* [ ] **MOD-001: mod loading** - deterministic load order, overrides, conflict
  diagnostics, mod test pack.
* [ ] **NET-001: multiplayer determinism** - two instances, command stream exchange,
  desync report naming the first differing subsystem and command.
* [ ] **UI-001: save/load from client**, **UI-002: casualty and war summary screens**.

## Verification debt

* [ ] **VER-001: evidence packets** for every VALIDATED feature (spec section 12).
* [ ] **VER-002: adversarial suite** - entity deletion, annexation mid-order, save
  during combat, ownership change during route calculation.
