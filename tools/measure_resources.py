#!/usr/bin/env python3
"""Measures resource production versus production-line demand, per country.

Runs `build/game --days N --inspect-country TAG` for every country in the scenario
and reconstructs produced = net + required, where required is the line demand from
data/common/equipment.json. Used to size data/maps/world.json resource yields.

usage: tools/measure_resources.py [days] [scenario-path]
"""

import json
import re
import subprocess
import sys

DAYS = sys.argv[1] if len(sys.argv) > 1 else "30"
BIN = sys.argv[3] if len(sys.argv) > 3 else "build/game"
SCENARIO = sys.argv[2] if len(sys.argv) > 2 else "data/scenarios/1936.json"

RESOURCES = ["oil", "steel", "aluminium", "rubber", "chromium", "tungsten"]
EQUIPMENT = {e["key"]: e.get("resources", {})
             for e in json.load(open("data/common/equipment.json"))["equipment"]}
TAGS = [c["tag"] for c in json.load(open(SCENARIO))["countries"]]

LINES_RE = re.compile(r"^\s{4}(\S+)\s+factories\s+(\d+)\s")
BALANCE_RE = re.compile(r"resource balance \(per day\):(.*)$")

prod_total = {r: 0.0 for r in RESOURCES}
req_total = {r: 0.0 for r in RESOURCES}
deficits = {}

print("%-4s %-10s %9s %9s %9s" % ("tag", "resource", "produced", "required", "net"))
for tag in TAGS:
    out = subprocess.run([BIN, "--days", DAYS, "--scenario", SCENARIO,
                          "--inspect-country", tag],
                         capture_output=True, text=True).stdout
    balance = {}
    required = {r: 0.0 for r in RESOURCES}
    for line in out.splitlines():
        m = BALANCE_RE.search(line)
        if m:
            for name, value in re.findall(r"(\w+)\s+([+-][\d.]+)", m.group(1)):
                balance[name] = float(value)
        m = LINES_RE.match(line)
        if m and m.group(1) in EQUIPMENT and int(m.group(2)) > 0:
            for r, amount in EQUIPMENT[m.group(1)].items():
                if r in required:
                    required[r] += amount * int(m.group(2))
    country_deficits = 0
    for r in RESOURCES:
        if r not in balance:
            continue
        net = balance[r]
        produced = net + required[r]
        prod_total[r] += produced
        req_total[r] += required[r]
        if net < -0.01:
            country_deficits += 1
        print("%-4s %-10s %9.1f %9.1f %+9.1f%s" % (tag, r, produced, required[r], net,
                                                   "  DEFICIT" if net < -0.01 else ""))
    surplus = sum(1 for r in RESOURCES if balance.get(r, 0.0) > 0.01)
    deficits[tag] = (country_deficits, surplus)

print()
print("global: production / requirement (ratio), countries with a deficit / with a surplus")
for r in RESOURCES:
    req = req_total[r]
    prod = prod_total[r]
    ratio = (prod / req) if req > 0.0 else float("inf")
    print("%-10s production %9.1f required %9.1f net %+9.1f ratio %s" %
          (r, prod, req, prod - req, ("%.2f" % ratio) if req > 0.0 else "n/a (unused)"))
print()
print("per country: (resources in deficit, resources in surplus)")
for tag in TAGS:
    print("  %-4s deficit %d surplus %d" % (tag, deficits[tag][0], deficits[tag][1]))