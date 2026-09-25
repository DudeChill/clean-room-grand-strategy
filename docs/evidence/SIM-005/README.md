# Evidence packet: SIM-005 headless execution

**Claim.** The simulation runs to completion headless, with no window, no input and no
developer intervention, and reports enough state (counters, audit, hashes, metrics) for a
reviewer to judge the run from the log alone.

**How to reproduce** (from the repository root, on a Release build):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/game --days 30 --quiet          # a 30-day run with per-phase metrics
build/game --days 30 --hashes         # the same run with the per-subsystem hash report
build/game --days 3 --audit           # world audit over a short run
build/game --days 10 --save r.save    # write a save
build/game --load r.save --days 5     # continue from it
```

**Observed** (captured while writing this packet, this revision):

| Command | Result |
|---|---|
| `--days 30 --quiet` | `simulated 720 ticks (30 days) ending 1936-01-31` |
| `--days 3 --audit` | `world audit: OK` |
| `--days 30 --hashes` | World line: `World      0x07fe3db417454942  tick=720 date=1936-01-31T00 seed=0x0000000000003039 world_seed=0x0000000000003039` |
| `--days 10 --save` then `--load --days 5` | both exit 0; the loaded run continues and prints its own summary |

Raw output is in this directory: `run30.txt`, `hashes.txt`, `audit.txt`, `save.txt`,
`load.txt`.

**What this does not prove.** Nothing about the client, about performance under load, or
about multi-hour wall-clock stability; those are covered by TOOL-002 (determinism oracle)
and by the release reviews.
