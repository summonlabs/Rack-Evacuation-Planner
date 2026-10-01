# Architecture

This document describes how the Rack Evacuation Planner is put together: the
module layering, the canonical encoding that everything is built on, the durable
format, and the shape of the planning algorithm. The behaviour itself is
described in the [README](../README.md).

---

## 1. Layering

The public headers form a strict dependency DAG. Nothing below depends on
anything above it, and there are no cycles:

```
status.hpp          errors and Result<T>
  └── digest.hpp    SHA-256 and Digest
        └── canonical.hpp   CanonicalWriter, domain tags, encode_sequence
              └── types.hpp ids, strong counters, checked arithmetic, enums, resources
                    ├── obligation.hpp   Obligation and its catalog
                    ├── policy.hpp       policy rules, maintenance, failure domains
                    ├── candidate.hpp    candidates and their compatibility tables
                    │     └── adjacent.hpp   capacity, ASI and DFI payloads
                    │           └── evidence.hpp  stamps, records, bundles, source bindings
                    │                 └── request.hpp  isolation request, plan request
                    │                       └── plan.hpp     the sealed artifact and the auditor
                    │                             ├── store.hpp   durable state and publication
                    │                             └── engine.hpp  identity, epochs, submission
                    └── scenario.hpp     the canonical text format
```

`rep/rep.hpp` is the umbrella header.

Implementation is split the same way. Two internal header trees are not
installed and are not part of the public API:

- `src/detail/` — `text.hpp` (strict integer parsing and escaping),
  `containers.hpp` (canonical sorting and duplicate refusal), `platform.hpp`
  (durable file publication, exclusive locks, strict reads), `canonical_reader.hpp`
  (the strict decoder), `store_format.hpp` (the on-disk format), and
  `planner.hpp` (the internal planning entry point).
- `tests/` — a small registration-based test framework (`testkit.hpp`) and
  shared fixtures.

`PlanEngine` is the only thing that assigns identity, sequence, revision, and
epoch. The planner in `src/planner.cpp` never invents an identity: it is handed
one and stamps it onto the artifact.

## 2. The canonical encoding

Everything that is hashed or persisted goes through `CanonicalWriter`. The rules
are fixed and versioned by a domain tag:

- integers are fixed-width little endian; signed values are written as their
  two's-complement bit pattern of the same width;
- booleans are a single byte, `0x00` or `0x01`;
- enums are their stable 16-bit code;
- every variable-length byte sequence is preceded by its length as a 64-bit
  little-endian value;
- containers are written in their canonical order (sorted, deduplicated) and
  preceded by their element count;
- nothing is written that depends on host endianness, pointer width, locale, or
  struct padding.

The digest of a record is

```
SHA-256( u64le(len(domain_tag)) || domain_tag || encoded_bytes )
```

so two record kinds can never produce the same digest even when their encodings
coincide. `CanonicalWriter::finish()` is `const` and does not disturb the buffer,
so a caller can take the digest and the bytes of the same value.

Decoding is the mirror image and is deliberately strict: `CanonicalReader`
either consumes exactly the bytes it was asked for or fails with
`Truncated`/`Corrupt`/`LimitExceeded`. Containers are length-bounded, and every
length is checked against the bytes that actually remain before anything is
allocated.

## 3. The durable format

A store directory contains exactly three kinds of file:

```
rep-store.lock          the exclusive writer lock (one open handle, no sharing)
rep-current             the commit point: names one state file and its digest
rep-state-<16 digits>.bin   one whole authoritative generation
```

### State file

```
header (104 bytes)
  magic            8   "REPSTOR1"
  format_version   4   currently 1
  header_size      4   104
  sequence         8   last committed plan sequence
  epoch            8   control epoch that published this generation
  payload_length   8
  payload_digest  32   SHA-256 of the payload bytes as they appear below
  header_digest   32   SHA-256 of the preceding 72 header bytes
payload           N   canonical encoding: u64-length-prefixed domain tag,
                      then the store body (sequence, epoch, writer, opened_at,
                      plans, idempotency records)
trailer_digest   32   SHA-256 of header || payload
```

The payload is self-describing: it opens with its own domain tag, so a reader
knows what it decoded before it decodes it. Decoding requires the file length to
be exactly `104 + payload_length + 32`, all three digests to verify, every
enumeration code to be a known value, every identifier to be valid, every nested
plan to pass its own `verify()`, and the payload to end exactly at its declared
boundary with no trailing bytes.

### Pointer file

```
header (96 bytes)
  magic            8   "REPCURNT"
  format_version   4
  header_size      4   96
  sequence         8
  name_length      8
  state_digest    32   SHA-256 of the entire referenced state file
  header_digest   32   SHA-256 of the preceding 64 header bytes
name              N   "rep-state-<16 digits>.bin"
```

The name is validated character by character (no separators, no traversal), and
its sequence must agree with the pointer's.

### Publication

```
write staged file  ->  flush to device  ->  read back and compare  ->
decode and compare against the input state  ->  rename into place  ->
staged pointer  ->  atomic replace of rep-current
```

`rep-current` is the single commit point. A reader therefore observes either the
whole previous generation or the whole new one; a state file that the pointer
does not name is residue and is removed when the store is opened.

The store refuses to open on anything corrupt, truncated, ambiguous, or from an
unsupported format version. It never falls back to a neighbouring generation and
never repairs a body.

### Writer exclusion and epochs

The writer role is an OS-level lock: the lock file is opened with no sharing, so
the kernel refuses a second process and releases the lock when the holder dies.
Opening a store with a `PlanEngine` claims `previous_epoch + 1` and publishes
that claim before accepting work, so a successor cannot silently inherit the
previous incarnation's authority, and a plan sealed by an earlier incarnation is
detectably not current.

## 4. The planning algorithm

`rep::detail::plan_evacuation` runs in five phases.

**1. Bind evidence.** Only streams the request names are read. A bound stream
whose record is absent counts as missing; a record present in the bundle but
unbound is never read. Conflicting duplicates across bound streams are refused
here, because the adjacent layer has no principled way to choose a side.

**2. Establish coverage.** The composition, enumeration, and catalog are
resolved into exactly one occupant set plus one definition per occupant. Every
failure in this phase produces an indeterminate plan with typed reasons and no
per-obligation output at all — a plan that cannot state the obligation set must
not state anything else.

**3. Order by dependency.** Tarjan's algorithm (iterative, so a long chain
cannot exhaust the stack) finds strongly connected components. Components with
more than one node are cycles and every member becomes residual. What remains is
acyclic, and Kahn's algorithm emits it in an order determined by a ready set
ordered on the canonical obligation key `(kind, id)`. The order is therefore
independent of how the input was assembled and of any container iteration order.

**4. Evaluate eligibility.** Obligations are visited in that order. For each one
the planner applies the fifteen ordered checks documented in the README,
selects the cheapest eligible candidate, and consumes its provision from that
destination's remaining capacity. Capacity is deliberately consumed greedily: it
keeps a destination from being double-booked inside one plan, at the cost of
global optimality.

**5. Seal.** Assignments, residuals, and scope exclusions are sorted, the status
is derived from them, and the artifact is sealed with a content digest. Sealing
verifies the structural invariants — dense order indices, exactly one outcome
per obligation, list ordering, and status/outcome agreement — so an
inconsistent plan cannot be published even by a caller that builds one by hand.

`audit_plan` is a second, independent pass over a sealed plan and the request
that produced it. It re-derives conservation, scope, dependency ordering, the
candidate each assignment names, and per-destination capacity from the request
itself, so a plan that is internally consistent but planned wrongly is still
caught.

## 5. Error discipline

Every fallible operation returns `Result<T>`, which holds either a value or an
`Error` carrying a stable `ErrorCode`, a message, and a field. No exception
crosses the public boundary, and every failure mode has a code: a corrupt file
is `Corrupt`, a short read is `Truncated`, an unsupported format is
`UnsupportedVersion`, an arithmetic limit is `Overflow` or `Underflow`, and a
lock held elsewhere is `Locked`.

`PlanEngine::submit` keeps two channels distinct on purpose. A *planning
decision* — planned, replayed, or refused for a stated reason — is reported
inside `PlanOutcome`. An *engine failure* — the store could not be written, the
lock was lost — is returned as an `Error`. Callers can therefore always tell
"the planner decided" from "the engine could not".

## 6. Concurrency

One mutex guards all mutable engine state. Every public method acquires it
exactly once and drops it before returning; no public method calls another
public method; no callback is invoked while it is held; nothing returns a
reference into internal state. Plans, summaries, and statistics are returned by
value.

The store's exclusion is a kernel file lock, not a mutex, and is independent of
the in-process mutex, so it cannot participate in an in-process lock-ordering
cycle. The audit of the call graph is recorded in the README.

## 7. Determinism

Two properties carry the determinism story:

- **Canonical containers.** Every collection has one canonical form, enforced by
  its factory rather than by convention, so two logically equal inputs always
  encode to identical bytes.
- **No ambient inputs.** The planner reads no clock, no locale, no environment,
  and no global state: the evaluation instant is part of the request, and every
  other input is an explicit argument.

The property suite exploits this by rebuilding each request with its evidence
records supplied in reverse order and requiring a byte-identical bundle,
request digest, and plan digest.
