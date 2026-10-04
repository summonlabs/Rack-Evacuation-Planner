# Rack Evacuation Planner

Deterministic evacuation planning for racks that must be isolated, drained,
depowered, thermally constrained, or physically serviced.

This repository owns the *plan* that says
what has to leave a rack, in what order, to which destination, what cannot leave
yet and why, and whether the rack is proven safe for the requested isolation.

The core question it answers is:

> Given one exact rack state and the current facility, ASI, and DFI evidence,
> what obligations must leave, which destinations and actions are eligible, in
> what deterministic order should evacuation proceed, what cannot yet be moved,
> and when is the rack proven safe for the requested isolation?

**A plan grants nothing and moves nothing.** It is evidence about a proposed
evacuation, not an evacuation.

---

## Systems boundary

### Owned by this boundary

- evacuation-plan identity, lineage, revision, and plan sequence;
- binding of a plan to one source rack, one exact rack composition revision,
  and the exact generations of every evidence stream it consumed;
- enumeration of the obligations in scope, including the distinction between a
  proven-complete enumeration and an incomplete one;
- typed destination/action candidates *supplied by adjacent authorities*,
  including validation that the authority asserting a candidate is the
  authority the request bound, at the generation and epoch it published;
- deterministic eligibility evaluation with a typed rejection reason for every
  candidate that was considered and refused;
- deterministic dependency ordering and wave assignment;
- protected and non-migratable obligations, and residual obligations with typed
  reasons;
- conservation: every enumerated obligation is assigned exactly once, is
  explicitly residual with a reason, or is proven outside the requested scope;
- generation/digest binding and stale-plan fencing;
- plan status and the safety verdict;
- durable plan history with single-writer exclusion, transactional publication,
  and crash-consistent recovery;
- idempotent replay of an already-committed planning request.

### Explicitly not owned

The planner does not own, model, or perform: rack occupancy truth, capacity
truth, workload scheduling or migration, DFI path migration, storage
replication, power switching, cooling actuation, maintenance execution,
facility-drain authority, placement reservation, or generic recovery execution.

It also does not own the placement policy it evaluates. Rules such as "do not
evacuate into the source failure domain" are decisions of the placement-policy
authority: the planner evaluates the rules it is given and refuses to invent a
default of its own.

Consequently the planner never infers authority from existence, observation,
acknowledgement, an earlier successful decision, recovered state, a cached
value, a matching name, apparent health, or topology alone. An evidence record
that is present in a bundle but that the request does not bind is never used as
evidence.

---

## Quick start

The project is C++20 and builds with CMake. On Windows with the Visual Studio
2022 build tools installed, from a developer shell:

```console
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Or through the presets, which cover Release, Debug, AddressSanitizer, a Visual
Studio multi-config generator, and a CI-style configuration:

```console
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Install and consume:

```console
cmake --install build/release --prefix C:/prefix/rack-evacuation-planner
```

```cmake
find_package(RackEvacuationPlanner 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE rep::rep)
```

CMake options: `REP_BUILD_TESTS`, `REP_BUILD_TOOLS`, `REP_BUILD_EXAMPLES`,
`REP_WARNINGS_AS_ERRORS` (default ON), `REP_ENABLE_ASAN`.

---

## Command line

`rep_cli` reads and writes the canonical scenario text format.

```console
rep_cli version
rep_cli render --scenario rack-a.txt
rep_cli plan   --scenario rack-a.txt [--store DIR] [--planner ID] [--out PLAN.txt]
rep_cli verify --scenario rack-a.txt --plan PLAN.txt
rep_cli inspect --store DIR
```

`plan` exits 0 when the outcome committed *and* the verdict is `safe`, 1 when
the outcome committed but nothing is proven, 2 when the request was refused
(with the typed error), and 3 on an engine or I/O failure. `verify` re-reads a
plan report, recomputes its content digest, re-checks it against the scenario
evidence, and audits it against the request; it exits 0 only when all three
agree.

---

## Using the library

```cpp
#include <rep/rep.hpp>

rep::EngineOptions options;
options.planner = rep::PlannerId::parse("dccp55-01").value();
options.store_directory = "C:/var/lib/rep";     // empty for a non-durable engine

auto engine = rep::PlanEngine::open(std::move(options)).value();
auto outcome = engine->submit(request).value();

if (outcome.committed()) {
  const rep::Plan& plan = outcome.plan;
  if (plan.verdict() == rep::SafetyVerdict::Safe) {
    // Every in-scope obligation is assigned: the rack is proven safe for the
    // isolation this request asked about, at exactly these generations.
  }
}
```

`examples/plan_walkthrough.cpp` is a complete worked example that also
demonstrates stale fencing: republishing one bound stream turns a previously
safe plan into a stale one.

---

## Data model

| Concept | Meaning |
| --- | --- |
| **Obligation** | One unit of work that must leave the rack: a workload, a storage replica, a network path, a service endpoint, a reservation, or an appliance. Defined by an obligation-catalog stream, never invented by the planner. |
| **Enumeration** | The rack authority's statement of which obligations occupy the rack, together with whether that statement is *complete* for an exact composition revision. |
| **Evidence stream** | A `(authority, stream)` pair publishing records at a generation and an epoch, each carrying a digest over its payload. |
| **Candidate** | A typed `(action, destination)` an adjacent authority asserts is available, with the resources it provisions, a cost, and whether it needs a maintenance window. |
| **Plan** | The sealed artifact: identity, lineage, revision, sequence, epoch, bindings, status, assignments, residuals, scope exclusions, and a content digest. |

### Evidence kinds

`rack_composition`, `enumeration`, `obligation_catalog`, `capacity`,
`placement_policy`, `maintenance`, `failure_domain`,
`asi_workload_state`, `dfi_obligation`, `candidate_offers`.

A request binds a *set of streams* to these kinds. More than one stream may be
bound to one kind, in which case their contents are merged. Two bound records
that describe the same subject with different content are a contradiction: the
request is refused rather than resolved.

---

## Planning semantics

### Coverage first

The planner establishes the obligation set before it plans anything:

- the bound rack-composition stream must describe the requested rack at the
  requested composition revision;
- the bound enumeration stream must exist, declare `complete`, name the same
  composition revision, and enumerate exactly the occupant set;
- every occupant must have exactly one definition in the bound catalog, and that
  definition must name the same source rack.

If any of that fails, the plan is **indeterminate**: it carries the typed
reasons and no per-obligation outcome at all, because a plan that cannot state
the obligation set must not state anything else either.

An empty obligation list is only reported as **proven empty** when the
enumeration source proved completeness for the exact bound revision. A plan is
never allowed to conclude "nothing to evacuate" from missing evidence.

### Plan status

| Status | Meaning | Verdict |
| --- | --- | --- |
| `indeterminate` | The obligation set could not be established. No assignments, no residuals, no scope exclusions. | `indeterminate` |
| `empty_safe` | Coverage proven, and no occupant is in the requested scope. | `safe` |
| `complete` | Coverage proven, and every in-scope obligation is assigned. | `safe` |
| `partial` | Coverage proven, and at least one in-scope obligation is residual with a typed reason. | `not_proven` |

### Eligibility

Each in-scope obligation is evaluated against its candidates. The checks run in
a fixed order, and the first failure is the candidate's recorded reason:

1. duplicate claim: another candidate with a smaller id already claimed this
   `(action, destination)`;
2. the action can express this obligation kind;
3. the action can target this destination kind;
4. the destination is not the source rack itself;
5. the authority asserting the candidate is one of the bound candidate streams;
6. the candidate's declared generation and epoch are the ones that stream
   published;
7. the provision covers what the obligation actually holds;
8. when the fabric authority published a permitted endpoint set for this
   obligation, the destination is in it;
9. no matching policy `deny_action` rule;
10. when the obligation kind has `allow_action` rules, one of them matches;
11. when the policy authority requires failure-domain spread for this kind, the
    destination is *proven* to be in a different domain;
12. no blocking incident covers the source or destination domain;
13. a closed maintenance window does not freeze this action, and a candidate
    that needs a window has an open one;
14. the destination is described by a bound capacity stream;
15. the capacity still available for that destination covers the provision.

Among the eligible candidates the planner selects the one with the lowest
`(cost, action, destination, candidate id)`. Capacity is consumed as obligations
are assigned, in evacuation order, so a destination is never double-booked
inside one plan.

### Ordering and waves

Dependencies form a graph over the in-scope obligations. Strongly connected
components are found with Tarjan's algorithm (iteratively, so an adversarial
chain cannot exhaust the stack); every member of a multi-node component is
residual with `dependency_cycle`. The remaining graph is ordered by Kahn's
algorithm with a ready set ordered by the canonical obligation key
`(kind, id)`, so the evacuation order never depends on container iteration
order or on how the input was assembled.

`wave(o)` is 0 when the obligation has no in-scope dependency and otherwise
`1 + max(wave(dependency))`. Ordering and waves are both recorded in the plan.
An obligation whose dependency is residual is itself residual with
`dependency_unsatisfied`.

### Residual reasons

An obligation that cannot be assigned carries one typed reason, chosen by fixed
priority from the rejections actually recorded: capacity exhaustion, active
incident, maintenance block, failure-domain conflict, policy denial, then a
generic "candidates were rejected". Every candidate that was considered is
listed with its own reason, so a residual always explains itself.

`protected` obligations need an explicit `allow_protected_move` rule, and an
obligation the ASI or DFI authority reports as non-migratable, or as having an
*unknown* capability, is residual — the planner never treats silence or an
explicit "unknown" as permission.

### Conservation

Every enumerated occupant appears exactly once across the three outcome lists:
assigned, residual, or excluded by scope. The plan refuses to seal if that does
not hold, and `rep::audit_plan` re-derives conservation, ordering, capacity, and
structural validity from the request, independently of the planner's own
bookkeeping.

---

## Identity, generations, and fencing

Every plan records a `plan_id`, a `lineage`, a lineage `revision`, a global
`sequence`, and the control `epoch` of the incarnation that sealed it. Opening a
durable engine claims a **new** epoch: a successor never inherits the previous
incarnation's mutation authority, and a plan sealed by an earlier incarnation is
reported as stale rather than current.

A plan binds the exact `(stream, generation, epoch, digest)` of every stream it
consumed. A plan is stale when any bound stream has moved generation, changed
epoch, changed digest, or disappeared. Staleness outranks the status: a stale
plan proves nothing, whatever its status says.

`PlanEngine::assess` is the supported way to ask whether a stored plan still
proves anything; it reports the incarnation check, supersession by a later
revision of the same lineage, every staleness finding, and the resulting verdict.

## Idempotency and replay

A request carries an idempotency key. The key and the digest of the request
identify one operation. Submitting the same operation again returns the
already-committed plan as a **replay** and commits nothing, even when the
caller's epoch view has moved on: replay is resolved *before* any
stale-precondition rejection, so a lost response can never cause a second
commit.

The idempotency index is durable, so replay also works across a restart. The
same key with a different request is a conflict, and a refused request does not
consume its key.

---

## Persistence and recovery

The durable store keeps plan history, the idempotency index, the sequence, and
the control epoch.

Publication is transactional: the new generation is written to a staging file,
flushed to the device, read back and verified byte for byte, decoded and
compared against the state that produced it, renamed into place, and only then
referenced by a small `rep-current` pointer that carries the digest of the whole
state file. `rep-current` is the single commit point. Recovery reads exactly the
generation that pointer names, verifies every digest, and decodes strictly;
anything corrupt, truncated, ambiguous, or from an unsupported format version is
refused rather than guessed at. A generation that is not referenced by the
pointer, or residue from an interrupted publication, is removed when the store
is opened.

One process holds the writer role at a time through an OS-level exclusive file
lock with no sharing. A second process is refused, abrupt holder death releases
the lock in the kernel, and the successor claims the next epoch rather than
inheriting the dead one's authority.

A control epoch of 0 means "no prior incarnation"; epoch fencing only applies to
non-zero expectations, so an in-memory engine's first incarnation can be
addressed explicitly.

## Concurrency model

One mutex guards all mutable engine state. Every public method acquires it
exactly once, and none of them re-enters it: the only public method that another
public method reaches — `summarise`, called by `history` — performs no locking
and reads only its argument, and the store's publication path uses a kernel file
lock rather than a mutex. No callback runs while the mutex is held: the engine's
single caller-supplied callback, the optional clock used to stamp an epoch
claim, is invoked during `open`, before the engine object exists. Nothing
returns a reference into engine state; plans, summaries, statistics, and reports
are returned by value.

The audit of the call graph found no read-to-write reacquisition, no write-lock
re-entry, no nested acquisition with inconsistent ordering, no lock inversion,
no callback under lock, no join while holding a lock the workers need, and no
cancellation path with reversed lock order.

The store's exclusion is a kernel lock, not a mutex, and is independent of the
in-process mutex, so it cannot participate in an in-process ordering cycle.

## Determinism

Canonical outputs never depend on map or hash iteration order, on thread timing,
on locale, or on host endianness. Every container has one canonical form (sorted
and deduplicated) that is enforced by its factory. Integers are encoded
little-endian at a fixed width; variable-length data is length-prefixed; digests
are SHA-256 over a domain-separated encoding, so two different record kinds can
never collide. The planner reads no clock: maintenance windows and incidents are
evaluated against the request's own `evaluation_time`.

The test suite pins this: the property suite rebuilds each request from a
reversed record order and requires a byte-identical evidence bundle, an identical
request digest, and an identical plan digest.

---

## Failure semantics

The public API is error-code based; no exception crosses the boundary.

```
Ok InvalidArgument InvalidIdentifier InvalidEncoding Overflow Underflow
NotFound AlreadyExists DuplicateIdentity LimitExceeded DigestMismatch Corrupt
Truncated UnsupportedVersion IoError Locked ReparsePoint StaleEpoch
StaleGeneration IdempotencyConflict PreconditionFailed Indeterminate
CycleDetected CapacityExhausted PolicyDenied Internal VersionMismatch
NotADirectory
```

`PlanEngine::submit` distinguishes a *domain outcome* (planned, replayed, or
refused for a stated reason, reported inside `PlanOutcome`) from an *engine
failure* (returned as an `Error`), so a caller can always tell "the planner
decided" from "the engine could not".

---

## Validation performed

Everything below was executed on Windows 11 (build 26300) with MSVC 14.44
(toolset 14.44.35207, x64), the Windows SDK 10.0.26100.0, CMake 4.3.2, Ninja,
and Git 2.52.0.

### Builds

- Release and Debug configure and build with the project's strict first-party
  warning policy (`/W4 /permissive- /WX`, C++20). The first-party warning count
  is zero in both configurations.
- AddressSanitizer builds and runs; see *Sanitizer* below for exactly what it
  covers.

### Test suite

204 tests and 8611 assertions across 14 suites, all passing in Release through
CTest, and the same 14 suites pass in Debug:

| Suite | Tests | Assertions |
| --- | ---: | ---: |
| `digest` | 10 | 1332 |
| `canonical` | 7 | 66 |
| `types` | 19 | 1619 |
| `domain` | 20 | 436 |
| `evidence` | 11 | 326 |
| `scenario` | 11 | 482 |
| `planner` | 45 | 235 |
| `plan` | 14 | 161 |
| `store` | 17 | 705 |
| `engine` | 16 | 174 |
| `property` | 5 | 2352 |
| `concurrency` | 4 | 215 |
| `multiprocess` | 5 | 128 |
| `hardening` | 20 | 380 |
| **total** | **204** | **8611** |

```console
100% tests passed, 0 tests failed out of 14
```


The suites are:

| Suite | What it proves |
| --- | --- |
| `digest`, `canonical`, `types` | SHA-256 against published vectors, canonical encoding byte-for-byte (including a regression pin), checked arithmetic at every boundary, identifier and enum contracts |
| `domain`, `evidence` | factory canonicalisation and refusal rules, compatibility tables, payload digest enforcement, kind/payload agreement |
| `scenario` | the text format round-trips, and every diagnostic is exact and located |
| `planner` | coverage, eligibility, ordering, waves, conservation, and the typed reason for every refusal |
| `plan` | sealing, structural invariants, staleness fencing, verdicts, and the independent auditor |
| `store` | transactional publication, and refusal of truncated, bit-flipped, trailing-garbage, substituted, and mismatched state |
| `engine` | epoch claiming, replay before stale-epoch rejection, lineage revisions, recovery, and fencing |
| `property` | randomized scenarios against an independent reference model |
| `concurrency` | many threads on one engine, concurrent readers and writers, one commit per key |
| `multiprocess` | real independent OS processes |
| `hardening` | adversarial and boundary input |

### Randomized and property testing

The property suite generates scenarios programmatically (never through the text
format, so a parser defect cannot mask a planner defect) from a fixed printed
seed. For each scenario it builds an independent reference model that re-derives
the in-scope set, capacity, and candidate eligibility from the request, and then
checks:

- conservation over the three outcome lists;
- dependency ordering, re-derived with a different algorithm from the planner's;
- that no destination is overbooked;
- that every assignment is structurally valid for its obligation kind;
- that every residual explains itself;
- that a plan claiming completeness is only published when the reference's
  brute-force search finds a complete assignment at all (soundness);
- that `audit_plan` reports nothing;
- determinism and input-order independence.

Failures print the seed and the iteration index, so any failure reproduces
exactly.

### Multiprocess and crash consistency

Real child processes, observed through pipes, with no shared memory:

- a second process is refused the writer lock while the first holds it;
- killing the holder abruptly releases the lock, and a new process acquires it;
- a child commits generations that an independent parent then recovers and
  verifies;
- a child is killed at each publication boundary (after the staged write, after
  read-back verification, after the state file is renamed into place, before the
  pointer moves, and after it moves) and the parent proves that recovery yields
  exactly one whole generation — either the one before the commit point or the
  one after, never a blend — and that the recovered store is still writable;
- a child that claims an incarnation and dies leaves an epoch that the successor
  must advance, and a request carrying the dead incarnation's epoch is refused.

### Sanitizer

AddressSanitizer (`/fsanitize=address`) builds and the complete suite runs under
it. It is a memory-safety check on the process, not a race detector and not a
power-loss simulation: the crash tests kill processes at the OS level.

### Static analysis

MSVC static analysis (`/analyze`, the Microsoft Native Recommended Rules) runs
over the whole tree — library, tools, example, and tests — with **zero
findings**. The analysis runs with the same `/EHsc` and warning settings as the
normal build, so the result reflects how the code is actually compiled.

### Packaging and downstream consumption

The install tree is exercised end to end: configure, build, install to a clean
prefix, an independent out-of-tree consumer that locates the package with
`find_package(RackEvacuationPlanner 1.0 REQUIRED)` and links `rep::rep`, then
compiles, links, and runs against the installed artifact only.

---

## REAL / SYNTHETIC / UNSUPPORTED

**REAL** — behaviour genuinely exercised by the host OS:

- the SHA-256 implementation, canonical encoding, and every digest in the system;
- the durable store, its file formats, its atomic publication sequence, and its
  recovery from truncation, bit flips, substitution, and trailing garbage;
- OS-level single-writer exclusion, including refusal of a second process and
  release of the lock when the holder dies;
- crash consistency proven with real processes killed at publication boundaries;
- filesystem behaviour that the store relies on: directory creation, reparse
  points, path handling, and the package/install/consumer path;
- multithreaded execution of the engine.

**SYNTHETIC** — modelled evidence from adjacent authorities:

- every evidence stream in this repository is a synthetic contract. Rack
  composition, enumeration, capacity, placement policy, maintenance state,
  failure domains, ASI workload state, DFI obligations, and candidate offers are
  all supplied by the caller as typed records with generations and digests.
- No BMS, DCIM, PDU, UPS, generator, cooling, accelerator, RDMA, InfiniBand,
  NVLink, hypervisor, scheduler, or storage-array integration exists here, and
  none is exercised.
- Nothing in this repository observes a real rack, moves a real workload, or
  switches real power.

**UNSUPPORTED** — not implemented and not validated:

- executing an evacuation, in whole or in part; a plan is a proposal;
- reserving a destination, migrating a workload, rebinding a fabric path,
  releasing a reservation, or switching anything;
- deriving capacity, occupancy, or failure domains from observation; every one
  of them is an input the caller must supply with an authority, a generation,
  and a digest;
- non-Windows platforms: the code contains a POSIX path for the platform layer,
  but it has not been built or exercised in this release, and the multiprocess
  and crash proofs are Windows-only;
- authentication of evidence. Digests detect corruption and accidental
  substitution; they do not authenticate an untrusted publisher.

---

## Genuine limitations

- **Greedy allocation.** Capacity is consumed in evacuation order. A plan can
  therefore be `partial` because an earlier obligation took a destination that a
  later one needed, even though some other assignment would have covered
  everything. The reference model in the property suite checks *soundness*, not
  global optimality; the planner does not claim to minimise residuals.
- **Cost is a tie-break, not an objective.** Selection minimises cost locally
  among eligible candidates for one obligation; there is no global cost
  optimisation.
- **Dependencies are acyclic-or-refused.** A cycle makes every member residual;
  the planner will not break a cycle on its own.
- **The plan is not a reservation.** Capacity is checked against the reported
  availability at plan time and is never held. Between planning and execution
  the world can change, which is exactly what staleness fencing is for.
- **State streams are per-kind.** A workload depends on the ASI stream and a
  path, replica, or service endpoint depends on the DFI stream; a kind with no
  stream bound is residual, never assumed migratable.
- **Store retention is bounded** by `max_plans`, and a request that would exceed
  it is refused rather than silently pruning history.
- **Digests are integrity, not authenticity.** See above.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
