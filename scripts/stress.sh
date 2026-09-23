#!/usr/bin/env bash
# Stress ladder and long-run observer automation (spec sections 96-97).
#
# Runs the headless simulation at increasing scale and records the measurements that
# matter: tick time distribution, save size, world-audit result and wall-clock cost.
# Results append to docs/benchmarks/history.tsv so regressions are visible over time.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

game="${GAME:-build/game}"
scenario="${SCENARIO:-data/scenarios/1936.json}"
out_dir="docs/benchmarks"
mkdir -p "$out_dir"
history="$out_dir/history.tsv"
[ -f "$history" ] || printf 'stamp\ttier\tdays\tticks\ttick_ms_avg\ttick_ms_p95\ttick_ms_p99\tsave_bytes\taudit\twall_s\n' > "$history"

# STRESS-1..5: small conflict -> late-game global war. All tiers currently run the
# same 1936 scenario because the world is a single scenario for this milestone; the
# tiers differ by duration, which is what exposes long-run drift.
run_tier() {
  local tier="$1" days="$2" label="$3"
  local log="$out_dir/${tier}.log"
  local save="$out_dir/${tier}.save"
  echo "== $tier ($label): $days days =="
  local start end wall
  start=$(date +%s.%N)
  if ! "$game" --scenario "$scenario" --days "$days" --quiet --audit --save "$save" > "$log" 2>&1; then
    echo "  FAILED (see $log)"; tail -5 "$log" | sed 's/^/  /'
    return 1
  fi
  end=$(date +%s.%N)
  wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.1f", b - a }')

  local avg p95 p99 save_bytes audit
  avg=$(grep -o 'total [0-9.]*' "$log" | head -1 | awk '{print $2}')
  p95=$(grep -o 'p95 [0-9.]*' "$log" | head -1 | awk '{print $2}')
  p99=$(grep -o 'p99 [0-9.]*' "$log" | head -1 | awk '{print $2}')
  save_bytes=$(stat -c %s "$save" 2>/dev/null || echo 0)
  audit=$(grep -c 'world audit: OK' "$log" || true)
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$(date -Iseconds)" "$tier" "$days" "$((days * 24))" "${avg:-}" "${p95:-}" "${p99:-}" \
    "$save_bytes" "$audit" "$wall" >> "$history"
  printf '  avg %s ms/tick, p95 %s ms, p99 %s ms, save %s bytes, audit_ok=%s, wall %ss\n' \
    "${avg:-?}" "${p95:-?}" "${p99:-?}" "$save_bytes" "$audit" "$wall"
}

run_tier STRESS-1 30 "small conflict"
run_tier STRESS-2 180 "regional war"
run_tier STRESS-3 365 "continental war"
run_tier STRESS-4 730 "global war"
run_tier STRESS-5 1825 "late-game global war"

echo
echo "history written to $history"
column -t -s $'\t' "$history" | tail -8
