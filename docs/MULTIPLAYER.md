# Multiplayer (MP-001)

A session is **deterministic lockstep over TCP**: every peer runs the same simulation and
applies the same commands on the same tick. The network moves *commands and hashes*,
never simulation state, so the only thing multiplayer has to get right is the order, the
tick barrier, and what happens when two peers disagree.

| Piece | File | Owns |
| --- | --- | --- |
| Ordering rule, tick barrier, desync report | `src/net/lockstep.h`, `src/net/lockstep.cpp` | which commands a tick contains and in what order |
| Sockets and framing | `src/net/transport.h`, `src/net/transport.cpp` | bytes: connect/accept, buffer until a whole message decodes |
| Lobby and the host/client loops | `src/main.cpp` (`--host`, `--join`) | who is seated, who broadcasts the order, when to halt |

## Hosting and joining

```sh
# host: seat 0, simulates locally and broadcasts each tick's ordered command list
build/game --host --port 5000 --players 2 --player VEL --days 5

# joiner: gets the next free seat, applies exactly the list the host hands it
build/game --join 127.0.0.1:5000 --player KOR --days 5
```

| Flag | Meaning |
| --- | --- |
| `--host` | listen and run a session; seat 0 is the host |
| `--join HOST:PORT` | join a hosted session |
| `--port N` | session port (`--host`; `--port 0` picks an ephemeral one and prints it) |
| `--players N` | seats in the session, the host included. The host waits for `N-1` joiners. A joiner may pass it as a cross-check; a mismatch is refused during the handshake |
| `--player TAG` | the country this peer holds; on the host it is also the country of seat 0 |
| `--net-issue-at <tick>` | tick at which this peer issues its one command (default: half the session). Both peers may pick different ticks; the barrier only cares that every seat submits |

`--days`/`--ticks` on the host decide the session length; a joiner that asks for a
different length is told so and follows the host. `--scenario`, `--seed`, `--data` and
`--mods` **must match on every peer** (see *Determinism requirements*).

Both peers run the same loop, so the logs are directly comparable: each tick that carried
commands prints one line, and the same line appears on every peer with the seats in
application order.

```text
$ build/game --host --port 45801 --players 2 --player VEL --days 5     # host log
listening on port 45801 for 2 player(s)
seat 1: KOR joined from 127.0.0.1:47820
session: 2 seat(s) [0:VEL 1:KOR], local seat 0 (VEL) host
session: 120 ticks, hash exchange every 24 ticks, 1 remote peer(s)
tick 60: VEL submits select_focus "arm_doctrine"
tick 60: applied 2 command(s) [seat0/VEL select_focus "arm_doctrine"; seat1/KOR select_focus "arm_doctrine"]
session ticks 120
session commands 2 applied over 1 tick(s) with commands
session ended 1936-01-06 (date of tick 120)
desync false
world hash 2f15639dd02b0b4d

$ build/game --join 127.0.0.1:45801 --player KOR --days 5              # joiner log
connected to 127.0.0.1:45801 as KOR
session: 2 seat(s) [0:VEL 1:KOR], local seat 1 (KOR) client
session: 120 ticks, hash exchange every 24 ticks, 1 remote peer(s)
tick 60: KOR submits select_focus "arm_doctrine"
tick 60: applied 2 command(s) [seat0/VEL select_focus "arm_doctrine"; seat1/KOR select_focus "arm_doctrine"]
session ticks 120
session commands 2 applied over 1 tick(s) with commands
session ended 1936-01-06 (date of tick 120)
desync false
world hash 2f15639dd02b0b4d
```

A run is auditable from the log alone: `session ticks` says how far it got, `desync
false` says the peers agreed at every hash exchange, and `world hash` is the oracle -
identical lines on both peers mean the two simulations are the same state. Both peers
exit 0 when that holds.

## The ordering rule

1. **Seats** are ordered ascending by seat, ties broken by tag then country id. Both
   peers build `Lockstep` from the same agreed roster (the host's roster message), never
   from arrival order.
2. **Within a seat**, commands keep the order the seat submitted them in.
3. **Only the host derives the list.** It collects every seat's `Commands` for the tick,
   applies rule 1+2, and broadcasts that list as `Applied`. A joiner applies exactly the
   list it was handed, so two peers cannot disagree about the order even when the
   submissions arrive in different orders.
4. **A seat commands only its own country.** A command for another seat's country is
   refused by the session and never applied.
5. **Every seat must submit for every tick.** A seat with nothing to say still submits an
   empty list; a seat that is behind *stalls* the session rather than letting anyone
   guess. The stall is reported (`stalled at tick N: not every seat has submitted`) and
   the run exits non-zero.
6. **AI seating is part of the state.** `ai_controlled` is hashed, so a session makes
   every seated country human on *every* peer. `player_country` is a per-peer view (which
   country this human holds) and is not hashed, so each peer keeps its own.

Peers exchange `world_hash` every 24 ticks (`kLockstepHashCadence`) and once more after
the last tick. The comparison happens at a barrier: both peers send their hash only after
applying the tick and wait for the others, so a hash is never compared across ticks.

## What the transport guarantees

* **Framing.** One message is `[u32 little-endian payload length][payload]`. A connection
  buffers bytes until a whole record decodes: a partial record is *not* an error, it is a
  message that has not arrived yet.
* **Malformed is fatal, never guessed.** A record whose length or payload is inconsistent
  stops the session with a diagnostic instead of being interpreted.
* **One ordered stream per peer.** TCP preserves the order of one peer's messages, which
  is what makes a seat's per-tick `Commands` message idempotent: receiving it again
  replaces that seat's list for the tick rather than appending twice.
* **No wall clock in the simulation.** Timeouts exist only to bound how long a peer waits
  for a stalled seat (30 s) or for a joiner to appear; they never influence state, order
  or hashes.
* **A peer that leaves is noticed.** A closed connection ends the session with
  `session failed: peer seat N (TAG) left the session` and a non-zero exit, on the peer
  that is still running.
* **Lobby (session state, not simulation state).** A joiner sends `Hello{text = tag}`; the
  host answers with one `Hello` per seat, seat 0 first, carrying the seat count in `hash`
  and the session length in `tick`; `Bye` ends the session, or refuses a joiner (unknown
  tag, duplicate tag or country, `--players` mismatch). After the lobby the traffic is
  only: joiner -> host `Commands`/`Hash`, host -> joiner `Applied`/`Hash`/`Bye`/`Desync`.
  Seat 0 is always the host, because that is the seat a joiner reads `Applied` from.

## When a desync happens

A mismatch stops the session on **both** peers with exit code 1. The report names the
tick, both hashes, the first differing subsystem and the tick's command list, followed by
the local per-subsystem hash report:

```text
$ build/game --host --port 45802 --players 2 --player VEL --days 5 --seed 1 --net-issue-at 0
$ build/game --join 127.0.0.1:45802 --player KOR --days 5 --seed 2 --net-issue-at 0

lockstep desync at tick 1
  local  seat 0 (VEL) hash 0x76a1df4a853c0e3c
  remote seat 1 (KOR) hash 0x4e18b8788c899e18
  first differing subsystem: Map
  commands at tick 1 (seat order, then submission):
    (no commands collected for this tick)
  local hash report:
Map        0x6c663239bdfcd827  457372 bytes
Countries  0xf8ceffa21e9809bb  14118 bytes
...
World      0x76a1df4a853c0e3c  tick=1 date=1936-01-01T01 seed=0x0000000000000001 world_seed=0x0000000000000001

desync true
session ticks 1
world hash 76a1df4a853c0e3c
```

The tick in the report is the world tick, i.e. the number of ticks applied when the hash
was taken: "desync at tick 1" is the state *after* the first tick. The example is a seed
mismatch (`--seed 1` against `--seed 2`), which the `World` line makes obvious.

What to do with it:

1. **Stop and keep the log.** Both peers' reports carry both hashes and the same first
   differing subsystem, so the disagreement is named, not hidden.
2. **Compare the two hash reports line by line.** The first line that differs is the
   subsystem that drifted; the `World` line also prints the seed and world seed.
3. **Check the tick's command list** in the report. It is the list both peers applied for
   that tick, in seat order; a command that should not be there is an ordering bug, an
   empty list where commands were expected is a submission that never arrived.
4. **Check the session inputs** before the code: same scenario, seed, data root, mods and
   binary on every peer. A mismatch in any of them is a divergence at tick 0, not a bug
   in the tick loop.
5. **If the inputs match and the state still diverges, it is a determinism bug in the
   simulation.** The subsystem named first narrows it down; the run is reproducible from
   the seed, the scenario and the command log.

## Determinism requirements for a session

* Same scenario, seed, data root, mods and binary on every peer. `--player` may differ
  (that is the point), and `--days` may differ (the host's length wins).
* Every seated country is human on every peer (the roster decides it); everything else is
  AI. This is part of the hashed state, so it cannot be decided per peer.
* No peer may drive the simulation outside the tick loop while a session runs (a second
  command source would enter one peer's queue and not the other's).

## Tests

`tests/test_transport.cpp` (part of `hoi_tests`):

* `transport_localhost_round_trip` - binds port 0, times out on a peer that never
  connects, refuses a connect to a dead port, carries a `Hello` and a `Commands` message
  through real sockets, and sees the peer's close.
* `transport_memory_link_round_trip` - the same framing over the in-memory `Link`, so
  session logic has a network-free path.
* `transport_truncated_record_is_buffered` - a record split one byte short stays buffered
  (and the header/payload split behaves the same way), then decodes whole.
* `transport_coalesced_and_malformed_records` - two records in one read both decode; a
  record with an out-of-range kind byte is `Malformed`, not a guess.

Manual checks that were run against a two-process localhost session (see the DEVLOG entry
for the full transcript): identical `world hash` on both peers with exit 0; a seed
mismatch halting both peers with exit 1 and a report; killing either peer mid-session
ending the other with `peer seat N (TAG) left the session` and exit 1; a `--players`
mismatch refused during the handshake.

Not covered: machines other than localhost, more than two seats (the CLI accepts up to 8
and seats them in acceptance order), and long-run packet loss (TCP retransmits; a peer
that is simply slow stalls the session, which is the documented behaviour).

## Out of scope

* **NAT traversal and matchmaking.** Peers must reach each other; `--join` takes an
  address that works from the joining machine (LAN or a port forward).
* **Reconnects.** A peer that leaves ends the session for everyone; there is no resuming
  into a running session.
* **Spectators.** Every connection is a seat with a country.
* **Encryption, authentication and cheat resistance.** The transport is plain TCP, and the
  session trusts the host to derive the tick's order; it does check that a seat only
  commands its own country and that every peer agrees on the hash.
* **Saving a session.** `--save` applies to a single-process run; a session has no
  session-file format of its own. Each peer's own run can be saved separately with
  `--save` once the session ends (both peers end on the same state by construction).
* **Late join.** The roster is fixed in the lobby before tick 0.
