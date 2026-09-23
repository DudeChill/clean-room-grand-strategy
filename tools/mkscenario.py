#!/usr/bin/env python3
"""Builds data/scenarios/1936.json from the generated map (deterministic).

Every land state is assigned to exactly one of the ten countries:
  1. multi-source BFS over the state graph from each country's capital state,
  2. states are handed out in (distance, tag, key) order to the nearest country
     that still has capacity, so the two large powers really do end up larger,
  3. one majority-vote smoothing pass makes borders contiguous,
  4. a bounded repair pass moves border states from overfull to underfull
     countries.
The nineteen historical core states keep their original owners.

Same map + same seed => byte-identical output.
"""

import json
import os
from collections import Counter, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "data", "maps", "world.json")
OUT = os.path.join(ROOT, "data", "scenarios", "1936.json")

world = json.load(open(MAP))
provinces = {p["key"]: p for p in world["provinces"]}
land = [p for p in world["provinces"] if not p["is_sea"]]
state_region = {st["key"]: st["region"] for st in world["states"]}
state_slots = {st["key"]: st["building_slots"] for st in world["states"]}
state_name = {st["key"]: st["name"] for st in world["states"]}

states = {}
for p in land:
    s = states.setdefault(p["state"], {"provinces": [], "pop": 0, "coastal": 0,
                                       "desert": 0, "urban": 0, "vp": 0})
    s["provinces"].append(p)
    s["pop"] += p["population"]
    s["coastal"] += 1 if p["coastal"] else 0
    s["desert"] += 1 if p["terrain"] == "desert" else 0
    s["urban"] += 1 if p["terrain"] == "urban" else 0
    s["vp"] += p["victory_points"]

for s in states.values():
    s["provinces"].sort(key=lambda p: p["key"])
    s["is_coastal"] = s["coastal"] > 0

# State adjacency over shared land province edges.
neighbours = {key: set() for key in states}
for key, st in states.items():
    for p in st["provinces"]:
        for n in p["adj"]:
            q = provinces[n]
            if q["is_sea"] or q["state"] == key:
                continue
            neighbours[key].add(q["state"])
for key in neighbours:
    neighbours[key] = sorted(neighbours[key])


def best_province(state_key):
    ps = sorted(states[state_key]["provinces"],
                key=lambda p: (-p["victory_points"], -p["population"], p["key"]))
    return ps[0]["key"]


# ------------------------------------------------------------------ countries --

COUNTRIES = [
    ("VEL", "Veldoria", "democratic"),
    ("KOR", "Korethia", "fascist"),
    ("THA", "Thalmark", "neutrality"),
    ("SUD", "Sundara", "neutrality"),
    ("AVA", "Avalonia", "democratic"),
    ("MER", "Mervath", "fascist"),
    ("OST", "Ostmark", "neutrality"),
    ("CAL", "Caldon", "democratic"),
    ("NOR", "Norvane", "communist"),
    ("TIR", "Tirolith", "neutrality"),
]
TAGS = [c[0] for c in COUNTRIES]
# Share of the world's states each power ends up with.
WEIGHTS = {"VEL": 0.190, "KOR": 0.150, "THA": 0.090, "SUD": 0.080, "AVA": 0.080,
           "MER": 0.090, "OST": 0.090, "CAL": 0.090, "NOR": 0.070, "TIR": 0.070}

by_pop = lambda s: (-states[s]["pop"], s)
by_desert = lambda s: (-states[s]["desert"], -states[s]["pop"], s)
by_small_landless = lambda s: (states[s]["coastal"] != 0, -states[s]["pop"], s)

taken = set()


def pick(pred, count, sort_key):
    chosen = []
    for skey in sorted(states, key=sort_key):
        if skey in taken or len(chosen) >= count:
            continue
        if not pred(skey):
            continue
        chosen.append(skey)
        taken.add(skey)
    return chosen


# Core states: the historical starting territory of each power.
home_region = state_region[sorted(states, key=by_pop)[0]]
vel = pick(lambda s: state_region[s] == home_region and states[s]["coastal"] == 0, 3, by_pop)
if len(vel) < 3:
    vel += pick(lambda s: True, 3 - len(vel), by_pop)
kor = pick(lambda s: states[s]["coastal"] == len(states[s]["provinces"]), 2, by_pop)
tha = pick(lambda s: states[s]["coastal"] == 0, 1, by_small_landless)
sud = pick(lambda s: states[s]["desert"] > 0, 1, by_desert)
if not sud:
    sud = pick(lambda s: True, 1, by_pop)
mid = []
used_regions = set()
for skey in sorted(states, key=by_pop):
    if len(mid) >= 6 or skey in taken:
        continue
    region = state_region[skey]
    if region in used_regions:
        continue
    used_regions.add(region)
    taken.add(skey)
    mid.append(skey)
for skey in mid:
    second = pick(lambda s: state_region[s] == state_region[skey], 1, by_pop)
    for s in second:
        if s not in mid:
            mid.append(s)
            break

CORE = {
    "VEL": vel,
    "KOR": kor,
    "THA": tha,
    "SUD": sud,
    "AVA": [mid[0]],
    "MER": [mid[1]],
    "OST": [mid[2]],
    "CAL": [mid[3]],
    "NOR": [mid[4]],
    "TIR": [mid[5]],
}
capital_of = {tag: max(core, key=lambda s: (states[s]["vp"], states[s]["pop"], s))
              for tag, core in CORE.items()}

# Distinct capital provinces: the BFS sources.
sources = {tag: best_province(capital_of[tag]) for tag in TAGS}

# Breadth-first hop distance from each capital over the land province graph.
province_neighbours = {p["key"]: [n for n in p["adj"] if not provinces[n]["is_sea"]]
                       for p in land}


def bfs_from(start):
    dist = {start: 0}
    queue = deque([start])
    while queue:
        cur = queue.popleft()
        for nxt in province_neighbours[cur]:
            if nxt in dist:
                continue
            dist[nxt] = dist[cur] + 1
            queue.append(nxt)
    return dist


cap_dist = {tag: bfs_from(prov) for tag, prov in sources.items()}

# Distance from a capital to a state = distance to its nearest province.
INF = 10 ** 9
state_dist = {}
for skey, st in states.items():
    state_dist[skey] = {}
    for tag in TAGS:
        best = INF
        for p in st["provinces"]:
            d = cap_dist[tag].get(p["key"])
            if d is not None and d < best:
                best = d
        state_dist[skey][tag] = best

# Capacity-aware multi-source assignment.
total_states = len(states)
caps = {tag: max(1, int(round(WEIGHTS[tag] * total_states))) for tag in TAGS}
assign = {}
remaining = dict(caps)
order = sorted(states, key=lambda s: (min(state_dist[s].values()),
                                      min(COUNTRIES, key=lambda c: (state_dist[s][c[0]], c[0]))[0],
                                      s))
for skey in order:
    ranked = sorted(COUNTRIES, key=lambda c: (state_dist[skey][c[0]], c[0]))
    chosen = None
    for tag, _, _ in ranked:
        if remaining[tag] > 0:
            chosen = tag
            break
    if chosen is None:
        chosen = ranked[0][0]
    assign[skey] = chosen
    remaining[chosen] -= 1

# One majority-vote smoothing pass; the nineteen core states never move.
core_owner = {}
for tag, core in CORE.items():
    for skey in core:
        core_owner[skey] = tag

counts = Counter(assign.values())
for skey in sorted(states):
    if skey in core_owner:
        assign[skey] = core_owner[skey]
        continue
    votes = Counter()
    votes[assign[skey]] += 1  # the state votes for its current owner
    for nb in neighbours[skey]:
        votes[assign[nb]] += 1
    ranked = sorted(votes.items(),
                    key=lambda kv: (-kv[1], min(state_dist[skey].values()), kv[0]))
    assign[skey] = ranked[0][0]

# Bounded repair: move border states from overfull to underfull countries, which
# keeps every country's territory contiguous and the intended size ordering.
counts = Counter(assign.values())
floor = {tag: max(1, int(caps[tag] * 0.55)) for tag in TAGS}
for _ in range(4000):
    over = [tag for tag in TAGS if counts[tag] > caps[tag]]
    under = [tag for tag in TAGS if counts[tag] < floor[tag]]
    if not over or not under:
        break
    moved = False
    for tag in sorted(over, key=lambda t: (-(counts[t] - caps[t]), t)):
        for skey in sorted([s for s in states if assign[s] == tag]):
            if skey in core_owner:
                continue
            candidates = sorted({assign[nb] for nb in neighbours[skey] if nb not in core_owner})
            for target in sorted(candidates, key=lambda t: (counts[t], t)):
                if target == tag or counts[target] >= floor[target]:
                    continue
                assign[skey] = target
                counts[tag] -= 1
                counts[target] += 1
                moved = True
                break
            if moved:
                break
        if moved:
            break
    if not moved:
        break

# Deterministic ordering of each country's states (keys ascending).
member_states = {tag: sorted([s for s in states if assign[s] == tag])
                 for tag in TAGS}
assert all(member_states[tag] for tag in TAGS)
assert sum(len(v) for v in member_states.values()) == total_states

# ------------------------------------------------------------------- industry --

LINE_PRIORITY = {
    "VEL": [("infantry_equipment_1", 0.42), ("support_equipment_1", 0.12),
            ("artillery_1", 0.12), ("motorized_1", 0.14), ("armor_1", 0.20)],
    "KOR": [("infantry_equipment_1", 0.40), ("support_equipment_1", 0.10),
            ("destroyer_1", 0.30), ("convoy_1", 0.20)],
    "THA": [("infantry_equipment_1", 0.60), ("support_equipment_1", 0.20),
            ("artillery_1", 0.20)],
    "SUD": [("infantry_equipment_1", 0.60), ("support_equipment_1", 0.20),
            ("artillery_1", 0.20)],
}
DEFAULT_LINES = [("infantry_equipment_1", 0.50), ("support_equipment_1", 0.18),
                 ("artillery_1", 0.16), ("motorized_1", 0.16)]

TECHS = {
    "VEL": ["infantry_weapons", "support_weapons", "field_artillery", "motorization",
            "light_armor", "production_lines",
            "aircraft_design", "fighter_airframe", "cas_airframe"],
    "KOR": ["infantry_weapons", "support_weapons", "field_artillery",
            "basic_naval_design", "aircraft_design", "construction_engineering",
            "fighter_airframe", "cas_airframe"],
    "THA": ["infantry_weapons", "field_artillery", "production_lines"],
    "SUD": ["infantry_weapons", "support_weapons", "field_artillery", "production_lines"],
}
DEFAULT_TECHS = ["infantry_weapons", "support_weapons", "field_artillery",
                 "construction_engineering", "production_lines"]

# Starting air arm: the two largest powers field a fighter wing (100 planes) and a
# CAS wing (50 planes) based on their capital, with the aircraft already in the
# stockpile the loader draws them from.
AIR_POWERS = {
    "VEL": {"fighter_1": 100, "cas_1": 50},
    "KOR": {"fighter_1": 100, "cas_1": 50},
}

LAW = {"VEL": ("conscription_limited", "economy_civilian", 0.62, 0.35),
       "KOR": ("conscription_extensive", "economy_partial_mobilization", 0.55, 0.55),
       "THA": ("conscription_volunteer", "economy_undisturbed", 0.50, 0.25),
       "SUD": ("conscription_limited", "economy_civilian", 0.48, 0.30)}

# Starting industry: (civilian, military, dockyards). Sized so that a campaign is
# playable from day one: a factory costs ~10800 capacity-days, so a power with
# ~38 civilian factories finishes one in well under three months even after the
# consumer-goods share.
FACTORIES = {
    "VEL": (38, 15, 5),
    "KOR": (23, 9, 4),
    "AVA": (14, 6, 2),
    "CAL": (13, 6, 2),
    "MER": (12, 5, 1),
    "OST": (11, 5, 1),
    "NOR": (10, 4, 2),
    "TIR": (9, 4, 1),
    "THA": (6, 3, 0),
    "SUD": (5, 2, 0),
}


def fit_factories(civ, mil, dock, slots):
    """Scales requested industry down until it fits the states' building slots."""
    total = civ + mil + dock
    if total <= slots:
        return civ, mil, dock
    scale = float(slots) / float(total)
    civ = int(civ * scale)
    mil = int(mil * scale)
    dock = int(dock * scale)
    while civ + mil + dock > slots:
        if civ >= mil and civ >= dock and civ > 0:
            civ -= 1
        elif mil >= dock and mil > 0:
            mil -= 1
        elif dock > 0:
            dock -= 1
        else:
            break
    return civ, mil, dock


def make_lines(tag, mil):
    priority = LINE_PRIORITY.get(tag, DEFAULT_LINES)
    lines = []
    left = mil
    for key, share in priority:
        take = min(int(round(mil * share)), left)
        if take <= 0:
            continue
        lines.append({"equipment": key, "factories": take})
        left -= take
    if left > 0 and lines:
        lines[0]["factories"] += left
    return lines


def make_divisions(tag, state_list, techs, count):
    coastal = any(states[s]["is_coastal"] for s in state_list)
    mix = []
    mix.append(("infantry_division", max(1, int(round(count * 0.60)))))
    garrison = int(round(count * 0.15))
    if garrison > 0:
        mix.append(("garrison_division", garrison))
    if "motorization" in techs:
        motorized = int(round(count * 0.15))
        if motorized > 0:
            mix.append(("motorized_division", motorized))
    if "light_armor" in techs:
        armor = int(round(count * 0.10))
        if armor > 0:
            mix.append(("armored_division", armor))
    if coastal and tag == "KOR":
        mix.append(("garrison_division", 1))
    divisions = []
    index = 0
    for template, n in mix:
        for _ in range(n):
            skey = state_list[index % len(state_list)]
            divisions.append({"template": template, "province": best_province(skey),
                              "count": 1})
            index += 1
    return divisions


out = {
    "name": "1936",
    "map": "maps/world.json",
    "start_date": "1936-01-01",
    "seed": 12345,
    "countries": [],
    "factions": [{"name": "Veldorian Compact", "leader": "VEL",
                  "members": ["VEL", "AVA", "CAL"]}],
    "wars": [],
}

for tag, name, ideology in COUNTRIES:
    state_list = member_states[tag]
    slots = sum(state_slots[s] for s in state_list)
    civ, mil, dock = FACTORIES[tag]
    civ, mil, dock = fit_factories(civ, mil, dock, slots)
    if slots < civ + mil + dock:
        raise SystemExit("industry does not fit the building slots of " + tag)

    techs = TECHS.get(tag, DEFAULT_TECHS)
    division_count = max(4, min(16, int(round(len(state_list) * 0.30))))
    divisions = make_divisions(tag, state_list, techs, division_count)

    stock = {
        "infantry_equipment_1": 40 * division_count,
        "support_equipment_1": 12 * division_count,
        "artillery_1": 6 * division_count,
    }
    if "motorization" in techs:
        stock["motorized_1"] = 10 * division_count
    if "light_armor" in techs:
        stock["armor_1"] = 5 * division_count
    if tag == "KOR":
        stock["destroyer_1"] = 40
        stock["convoy_1"] = 200

    # Aircraft for the starting wings live in the stockpile; the loader draws them
    # out when it creates the wings.
    wings = []
    for model, planes in AIR_POWERS.get(tag, {}).items():
        stock[model] = stock.get(model, 0) + planes
        wings.append({"equipment": model, "province": sources[tag], "planes": planes})

    law, economy, stability, war_support = LAW.get(
        tag, ("conscription_limited" if len(state_list) % 2 == 0 else
              "conscription_volunteer", "economy_civilian", 0.52, 0.30))

    entry = {
        "tag": tag,
        "name": name,
        "ideology": ideology,
        "capital_state": capital_of[tag],
        "states": state_list,
        "civilian_factories": civ,
        "military_factories": mil,
        "dockyards": dock,
        "technologies": techs,
        "production_lines": make_lines(tag, mil),
        "stockpile": stock,
        "laws": [law, economy, "trade_free"],
        "political_power": 20.0 + len(state_list) * 0.5,
        "stability": stability,
        "war_support": war_support,
        "divisions": divisions,
    }
    if wings:
        entry["wings"] = wings
    out["countries"].append(entry)

with open(OUT, "w") as f:
    json.dump(out, f, indent=2, sort_keys=False)
    f.write("\n")

print("wrote", OUT)
for c in out["countries"]:
    slots = sum(state_slots[s] for s in c["states"])
    print("  %s %-10s states %3d slots %3d civ %3d mil %3d dock %2d div %3d" %
          (c["tag"], c["name"], len(c["states"]), slots, c["civilian_factories"],
           c["military_factories"], c["dockyards"],
           sum(d["count"] for d in c["divisions"])))
