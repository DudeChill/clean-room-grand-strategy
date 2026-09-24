#!/usr/bin/env bash
# Documentation gate (spec sections 9-12, 121-122): the milestone claim is only honest if
# the parity matrix and the discrepancy database agree with it. This script is mechanical
# on purpose - it does not read prose, it reads the tables.
#
# Checks
#   1. every row in docs/DISCREPANCIES.md uses a known severity
#   2. no OPEN row is BLOCKER or MAJOR (that is the feature-complete core criterion)
#   3. every parity status is from the allowed vocabulary
#   4. the histogram is printed, so a reviewer sees what "done" currently means
#
# Exits non-zero on the first failing check.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

discrepancies="docs/DISCREPANCIES.md"
parity="docs/PARITY_MATRIX.md"

fail=0
note() { printf '        %s\n' "$1"; }

echo "== discrepancies: severity vocabulary =="
bad_sev=$(grep -E '^\| [A-Z]+-[0-9]+ \|' "$discrepancies" | grep -vE '\| (BLOCKER|MAJOR|MODERATE|MINOR) \|' | wc -l)
if [ "$bad_sev" -eq 0 ]; then
  echo "PASS  every row uses a known severity"
else
  echo "FAIL  $bad_sev row(s) use an unknown severity"
  grep -E '^\| [A-Z]+-[0-9]+ \|' "$discrepancies" | grep -vE '\| (BLOCKER|MAJOR|MODERATE|MINOR) \|' | head -5 | while read -r l; do note "${l:0:110}"; done
  fail=1
fi

echo "== discrepancies: no open blocker or major =="
open_major=$(grep -E '^\| [A-Z]+-[0-9]+ \| (BLOCKER|MAJOR) \|' "$discrepancies" | grep -vc 'CLOSED')
if [ "$open_major" -eq 0 ]; then
  echo "PASS  no OPEN BLOCKER/MAJOR discrepancy"
else
  echo "FAIL  $open_major OPEN BLOCKER/MAJOR discrepancy(ies)"
  grep -E '^\| [A-Z]+-[0-9]+ \| (BLOCKER|MAJOR) \|' "$discrepancies" | grep -v 'CLOSED' | head -10 | while read -r l; do note "${l:0:120}"; done
  fail=1
fi

echo "== parity matrix: status vocabulary =="
bad_status=$(grep -E '^\| [A-Z]+-[0-9]+ \|' "$parity" | grep -vcE '\| (UNRESEARCHED|RESEARCHING|SPECIFIED|NOT_IMPLEMENTED|PROTOTYPE|PARTIAL|FUNCTIONAL|PARITY_TESTING|VALIDATED) \|')
if [ "$bad_status" -eq 0 ]; then
  echo "PASS  every parity row uses an allowed status"
else
  echo "FAIL  $bad_status parity row(s) use a status outside the allowed vocabulary"
  grep -E '^\| [A-Z]+-[0-9]+ \|' "$parity" | grep -vE '\| (UNRESEARCHED|RESEARCHING|SPECIFIED|NOT_IMPLEMENTED|PROTOTYPE|PARTIAL|FUNCTIONAL|PARITY_TESTING|VALIDATED) \|' | head -5 | while read -r l; do note "${l:0:110}"; done
  fail=1
fi

echo "== histogram (informational) =="
printf '        discrepancies: '; grep -oE '\| (BLOCKER|MAJOR|MODERATE|MINOR) \|' "$discrepancies" | sort | uniq -c | tr '\n' ' '; echo
printf '        closed rows:   %s of %s\n' \
  "$(grep -E '^\| [A-Z]+-[0-9]+ \|' "$discrepancies" | grep -c 'CLOSED')" \
  "$(grep -cE '^\| [A-Z]+-[0-9]+ \|' "$discrepancies")"
printf '        parity status: '; grep -oE '\| (UNRESEARCHED|RESEARCHING|SPECIFIED|NOT_IMPLEMENTED|PROTOTYPE|PARTIAL|FUNCTIONAL|PARITY_TESTING|VALIDATED) \|' "$parity" | sort | uniq -c | tr '\n' ' '; echo

exit "$fail"
