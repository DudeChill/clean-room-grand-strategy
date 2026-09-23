#!/usr/bin/env python3
"""Builds data/scenarios/1936.json from the generated map (deterministic)."""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "data", "maps", "world.json")
OUT = os.path.join(ROOT, "data", "scenarios", "1936.json")

world = json.load(open(MAP))
provinces = {p["key"]: p for p in world["provinces"]}
land = [p for p in world["provinces"] if not p["is_sea"]]
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
state_region = {st["key"]: st["region"] for st in world["states"]}


def best_province(state_key):
    ps = sorted(states[state_key]["provinces"],
                key=lambda p: (-p["victory_points"], -p["population"], p["key"]))
    return ps[0]["key"]


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


by_pop = lambda s: (-states[s]["pop"], s)
by_desert = lambda s: (-states[s]["desert"], -states[s]["pop"], s)
by_small_landless = lambda s: (states[s]["coastal"] != 0, -states[s]["pop"], s)

# 1. Large industrial continental power: the three most populous states in one region.
home_region = state_region[sorted(states, key=by_pop)[0]]
vel = pick(lambda s: state_region[s] == home_region and states[s]["coastal"] == 0, 3, by_pop)
if len(vel) < 3:
    vel += pick(lambda s: True, 3 - len(vel), by_pop)

# 2. Island / naval power: states whose every province is coastal.
kor = pick(lambda s: states[s]["coastal"] == len(states[s]["provinces"]), 2, by_pop)

# 3. Small landlocked power: no coastal province, mid population.
tha = pick(lambda s: states[s]["coastal"] == 0, 1, by_small_landless)

# 4. Desert power: most desert provinces.
sud = pick(lambda s: states[s]["desert"] > 0, 1, by_desert)
if not sud:
    sud = pick(lambda s: True, 1, by_pop)

# 5. Six mid-sized powers, spread over distinct regions where possible.
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
    mid.append([skey])
for group in mid:
    second = pick(lambda s: state_region[s] == state_region[group[0]], 1, by_pop)
    group.extend(second)

countries = [
    dict(tag="VEL", name="Veldoria", ideology="democratic", states=vel,
         civ=22, mil=12, dock=2, divisions=[("infantry_division", 6),
                                            ("motorized_division", 2),
                                            ("armored_division", 2)],
         techs=["infantry_weapons", "support_weapons", "field_artillery",
                "motorization", "light_armor", "production_lines",
                "construction_engineering"],
         stock={"infantry_equipment_1": 9000, "support_equipment_1": 3000,
                "motorized_1": 2400, "armor_1": 1800},
         law="conscription_limited", economy="economy_civilian",
         pp=35.0, stability=0.62, war_support=0.35),

    dict(tag="KOR", name="Korethia", ideology="fascist", states=kor,
         civ=14, mil=7, dock=6, divisions=[("infantry_division", 4),
                                           ("garrison_division", 2),
                                           ("motorized_division", 1)],
         techs=["infantry_weapons", "support_weapons", "field_artillery",
                "basic_naval_design", "construction_engineering",
                "aircraft_design"],
         stock={"infantry_equipment_1": 5200, "support_equipment_1": 1500,
                "motorized_1": 900, "destroyer_1": 40, "convoy_1": 200},
         law="conscription_extensive", economy="economy_partial_mobilization",
         pp=40.0, stability=0.55, war_support=0.55),

    dict(tag="THA", name="Thalmark", ideology="neutrality", states=tha,
         civ=4, mil=2, dock=0, divisions=[("infantry_division", 3),
                                          ("garrison_division", 1)],
         techs=["infantry_weapons", "field_artillery", "production_lines"],
         stock={"infantry_equipment_1": 2200, "support_equipment_1": 400,
                "artillery_1": 200},
         law="conscription_volunteer", economy="economy_undisturbed",
         pp=25.0, stability=0.50, war_support=0.25),

    dict(tag="SUD", name="Sundara", ideology="neutrality", states=sud,
         civ=6, mil=3, dock=0, divisions=[("infantry_division", 4),
                                          ("garrison_division", 1)],
         techs=["infantry_weapons", "support_weapons", "field_artillery",
                "production_lines"],
         stock={"infantry_equipment_1": 3000, "support_equipment_1": 600,
                "artillery_1": 300},
         law="conscription_limited", economy="economy_civilian",
         pp=30.0, stability=0.48, war_support=0.30),
]

MID_NAMES = [
    ("AVA", "Avalonia", "democratic"),
    ("MER", "Mervath", "fascist"),
    ("OST", "Ostmark", "neutrality"),
    ("CAL", "Caldon", "democratic"),
    ("NOR", "Norvane", "communist"),
    ("TIR", "Tirolith", "neutrality"),
]
MID_DIVS = [
    [("infantry_division", 4), ("motorized_division", 1)],
    [("infantry_division", 5), ("armored_division", 1)],
    [("infantry_division", 3), ("garrison_division", 2)],
    [("infantry_division", 4), ("mountain_division", 2)],
    [("infantry_division", 6), ("motorized_division", 2)],
    [("garrison_division", 4), ("infantry_division", 2)],
]
for i, (tag, name, ideology) in enumerate(MID_NAMES):
    countries.append(dict(
        tag=tag, name=name, ideology=ideology, states=mid[i],
        civ=8 + i, mil=4 + (i % 3), dock=(2 if i % 2 == 0 else 0),
        divisions=MID_DIVS[i],
        techs=["infantry_weapons", "support_weapons", "field_artillery",
               "motorization" if i % 2 == 0 else "production_lines",
               "construction_engineering"],
        stock={"infantry_equipment_1": 3600 + 200 * i,
               "support_equipment_1": 800, "motorized_1": 600,
               "artillery_1": 250},
        law="conscription_limited" if i % 2 == 0 else "conscription_volunteer",
        economy="economy_civilian" if i % 2 else "economy_partial_mobilization",
        pp=20.0 + i, stability=0.52, war_support=0.30 + 0.02 * i))

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

for c in countries:
    states_list = list(dict.fromkeys(c["states"]))
    if not states_list:
        raise SystemExit("no state for " + c["tag"])
    capital = max(states_list, key=lambda s: (states[s]["vp"], states[s]["pop"], s))
    divisions = []
    for template, count in c["divisions"]:
        divisions.append({"template": template, "province": best_province(states_list[0]),
                          "count": count})
    country = {
        "tag": c["tag"],
        "name": c["name"],
        "ideology": c["ideology"],
        "capital_state": capital,
        "states": states_list,
        "civilian_factories": c["civ"],
        "military_factories": c["mil"],
        "dockyards": c["dock"],
        "technologies": c["techs"],
        "stockpile": c["stock"],
        "laws": [c["law"], c["economy"], "trade_free"],
        "political_power": c["pp"],
        "stability": c["stability"],
        "war_support": c["war_support"],
        "divisions": divisions,
    }
    out["countries"].append(country)

with open(OUT, "w") as f:
    json.dump(out, f, indent=2, sort_keys=False)
    f.write("\n")

print("wrote", OUT)
for c in out["countries"]:
    print(" ", c["tag"], c["name"], c["states"], "capital", c["capital_state"],
          "divisions", sum(d["count"] for d in c["divisions"]))
