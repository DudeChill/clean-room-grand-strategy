#!/usr/bin/env bash
# Content validation gate (MOD-002, MOD-003).
#
#   scripts/validate-content.sh
#
# Validates, in order:
#   1. the shipped content tree with no mods,
#   2. every shipped mod under data/mods/ (each one alone, then all of them together),
#   3. the mod test pack under tests/mods/, which contains deliberately broken mods and
#      therefore expects a non-zero exit *and* a specific diagnostic for each of them.
#
# Exit status is non-zero when any expectation is not met, so this can be wired into
# scripts/verify.sh or CI directly.
#
# Environment:
#   GAME=<binary>        game binary (default build/game)
#   SCENARIO=<file>      scenario for the one-day audit run (default data/scenarios/1936.json)
#   VALIDATE_SKIP_PACK=1 skip section 3 (for packaging a release without the test fixtures)
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

game="${GAME:-build/game}"
scenario="${SCENARIO:-data/scenarios/1936.json}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ ! -x "$game" ]; then
  echo "validate-content: $game is not executable (build first, or set GAME=<binary>)" >&2
  exit 2
fi

pass=0
fail=0
skipped=0
declare -a results

# check <label> <ok|error> <expected substring|-> <mods spec|->
# A mods spec is a comma-separated list of mod roots; "-" means load no mods at all.
check() {
  local label="$1" expect="$2" want="$3" mods="$4"
  local out="$tmp/out.txt"
  local -a args=(--validate-content --scenario "$scenario")
  [ "$mods" != "-" ] && args+=(--mods "$mods")
  local got="ok"
  if ! "$game" "${args[@]}" > "$out" 2>&1; then
    got="error"
  fi
  local why=""
  if [ "$got" != "$expect" ]; then
    why="expected exit=$expect, got exit=$got"
  elif [ "$want" != "-" ] && ! grep -qF -- "$want" "$out"; then
    why="missing diagnostic: $want"
  fi
  if [ -z "$why" ]; then
    results+=("PASS  $label")
    pass=$((pass + 1))
  else
    results+=("FAIL  $label -- $why")
    tail -20 "$out" | sed 's/^/        /'
    fail=$((fail + 1))
  fi
}

# stage <mod dir> <staging root>: expose one mod as <root>/<mod> so it can be validated
# on its own, and print the staging root.
stage() {
  local mod_dir="$1" stage_root="$2"
  mkdir -p "$stage_root"
  ln -sfn "$(realpath "$mod_dir")" "$stage_root/$(basename "$mod_dir")"
  printf '%s' "$stage_root"
}

# The deterministic part of a run's report: subsystem hashes plus the world hash, with
# the wall-clock performance lines removed (they differ every run by design).
hashes() {
  local -a args=(--scenario "$scenario" --seed 4242 --days 5 --hashes --quiet)
  [ "$1" != "-" ] && args+=(--mods "$1")
  "$game" "${args[@]}" 2>&1 | grep -Ev '^(performance|tick p|   ai detail)'
}

# check_stable <label> <mods spec>: the same inputs must produce the same hashes twice.
check_stable() {
  local label="$1"
  hashes "$2" > "$tmp/stable1.txt"
  hashes "$2" > "$tmp/stable2.txt"
  if diff -q "$tmp/stable1.txt" "$tmp/stable2.txt" > /dev/null; then
    results+=("PASS  $label")
    pass=$((pass + 1))
  else
    results+=("FAIL  $label -- hashes differ between two identical runs")
    diff "$tmp/stable1.txt" "$tmp/stable2.txt" | head -10 | sed 's/^/        /'
    fail=$((fail + 1))
  fi
}

echo "== 1. shipped content =="
check "shipped data, no mods" ok "content validation: OK" -
check_stable "shipped data is deterministic across two runs" -

echo "== 2. shipped mods under data/mods =="
if [ ! -d data/mods ]; then
  results+=("FAIL  data/mods is missing")
  fail=$((fail + 1))
else
  found=0
  for mod_dir in data/mods/*/; do
    [ -d "$mod_dir" ] || continue
    [ -f "$mod_dir/mod.json" ] || continue
    found=$((found + 1))
    check "shipped mod $(basename "$mod_dir") alone" ok "content validation: OK" \
      "$(stage "$mod_dir" "$tmp/alone/$found")"
  done
  if [ "$found" -eq 0 ]; then
    results+=("FAIL  no mod with a mod.json under data/mods")
    fail=$((fail + 1))
  else
    check "all shipped mods together" ok "content validation: OK" data/mods
    check_stable "shipped mods produce identical hashes across two runs" data/mods
  fi
fi

echo "== 3. mod test pack (tests/mods) =="
if [ "${VALIDATE_SKIP_PACK:-0}" = "1" ]; then
  results+=("SKIP  mod test pack")
  skipped=$((skipped + 1))
else
  check "pack: clean override replaces a shipped law" ok \
    "mod clean_override: laws conscription_limited: replaced" tests/mods/clean
  check "pack: dependency chain loads the base first" ok \
    "mod alpha_top: laws mod_chain_law: replaced" tests/mods/chain
  check "pack: load_after hint loads the hint first" ok \
    "mod gamma_late: laws mod_hint_law: replaced" tests/mods/hint
  check "pack: add_ prefix appends a new definition" ok \
    "mod append_mod: laws mod_appended_law: added" tests/mods/append
  check "pack: malformed json fails with the file named" error \
    "mod broken_json: common/equipment.json: parse error" tests/mods/broken_json
  check "pack: unknown table fails with the file named" error \
    "common/hovertanks.json" tests/mods/unknown_table
  check "pack: unknown key fails with the reason" error \
    "unknown modifier 'NoSuchModifier'" tests/mods/unknown_key
  check "pack: appending onto an existing key is a conflict" error \
    "mod conflict_mod: laws conscription_limited" tests/mods/conflict
fi

echo
echo "==== content validation summary ===="
printf '%s\n' "${results[@]}"
echo "passed: $pass   failed: $fail   skipped: $skipped"
[ "$fail" -eq 0 ] || exit 1