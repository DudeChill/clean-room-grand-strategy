#!/usr/bin/env bash
# Release gate (spec sections 100, 211): build, tests, golden scenarios, determinism,
# save/load round trip, world audit, inspector smoke tests and a timing report.
# Fails (non-zero) on the first check that does not pass, and prints a summary.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

game="build/game"
tests="build/hoi_tests"
scenario="${SCENARIO:-data/scenarios/1936.json}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0
declare -a results

check() {
  local name="$1"; shift
  if "$@" > "$tmp/out.txt" 2>&1; then
    results+=("PASS  $name")
    pass=$((pass + 1))
  else
    results+=("FAIL  $name")
    tail -20 "$tmp/out.txt" | sed 's/^/        /'
    fail=$((fail + 1))
  fi
}

echo "== 1. build =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > "$tmp/build.log" 2>&1 || { echo "configure failed"; tail -20 "$tmp/build.log"; exit 1; }
cmake --build build -j"$(nproc)" >> "$tmp/build.log" 2>&1 || { echo "build failed"; tail -40 "$tmp/build.log"; exit 1; }
echo "   ok"

echo "== 2. unit / integration / golden tests =="
check "hoi_tests" "$tests"

echo "== 3. world audit on the shipped scenario =="
check "scenario loads and audits" "$game" --scenario "$scenario" --days 3 --audit --quiet

echo "== 4. deterministic replay of an AI-driven run =="
"$game" --scenario "$scenario" --seed 4242 --days 10 --hashes --quiet 2>&1 | grep -Ev '^(performance|tick p|   ai detail)' > "$tmp/h1.txt"
"$game" --scenario "$scenario" --seed 4242 --days 10 --hashes --quiet 2>&1 | grep -Ev '^(performance|tick p|   ai detail)' > "$tmp/h2.txt"
if diff -q "$tmp/h1.txt" "$tmp/h2.txt" > /dev/null; then
  results+=("PASS  determinism (identical subsystem + world hashes across two runs)")
  pass=$((pass + 1))
else
  results+=("FAIL  determinism")
  diff "$tmp/h1.txt" "$tmp/h2.txt" | head -20 | sed 's/^/        /'
  fail=$((fail + 1))
fi

echo "== 5. save / load round trip inside a live run =="
check "save every 20 days, hash must match after reload" \
  "$game" --scenario "$scenario" --days 60 --save "$tmp/session.save" --save-every-days 20 --quiet

echo "== 6. long observer run with metrics =="
check "365 days, all countries AI" \
  "$game" --scenario "$scenario" --days 365 --summary --audit --hashes

echo "== 7. inspectors =="
check "inspect-country" sh -c "'$game' --scenario '$scenario' --days 5 --quiet --inspect-country VEL | grep -q 'COUNTRY VEL'"
check "inspect-province" sh -c "'$game' --scenario '$scenario' --days 5 --quiet --inspect-province 3 | grep -q 'PROVINCE 3'"
check "inspect-battle" sh -c "'$game' --scenario '$scenario' --days 5 --quiet --inspect-battle 0 | grep -qE 'no battle|BATTLE'"

echo "== 8. content validation (MOD-002, MOD-003) =="
check "content validator: base tree, data/mods, tests/mods fixtures" env GAME="$game" scripts/validate-content.sh

echo
echo "== 9. documentation gate (parity matrix + discrepancy database) =="
if scripts/audit-docs.sh > /tmp/verify_docs.txt 2>&1; then
  results+=("PASS  documentation gate (no OPEN BLOCKER/MAJOR, vocabularies intact)")
  pass=$((pass + 1))
else
  results+=("FAIL  documentation gate")
  sed 's/^/        /' /tmp/verify_docs.txt | tail -12
  fail=$((fail + 1))
fi

echo "==== verification summary ===="
printf '%s\n' "${results[@]}"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ] || exit 1
