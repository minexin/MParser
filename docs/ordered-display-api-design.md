# Ordered Display Result API Design

Approved direction: additive, explicit opt-in, read-only ordered execution
results. Existing execution entry points, disabled-mode result groups and
frozen historical headers/schemas stay unchanged. This document is a design,
not a claim that the API or host delivery has passed acceptance.

## Delivery Stages

1. Protect immutable value projection. Freeze external property visibility at
   capture; never query getters or live objects when reading old events. Only
   public non-hidden automatically stored properties expose values. Public
   dependent/custom-getter properties report unavailable, not backing data.
   Unknown foreign class metadata fails closed with an opaque descriptor.
   Callable captures/receivers are not exported. Ordinary structs remain data.
2. Add an optional caller-sized invocation tail, keeping existing entry points.
   New C result getters and owning C++ copies expose the ordered collection,
   source names/UTF-8 ranges, suppression/capture depth, diagnostic disposition,
   frozen text and a versioned read-only snapshot graph. Allocate a new ABI
   revision only with those actual public accessors and compatibility tests.
3. Bound returned event count and accounted owned-result payload. Return the
   retained prefix and an explicit incomplete reason/cursor when exhausted;
   do not stop MATLAB computation solely because optional recording is full.
   Per-value failures remain explicit unavailable values. These are retained
   public-result limits, not a bound on the in-flight internal journal,
   semantic evalc buffers, temporary projection/serializer allocations,
   allocator overhead, legacy result arrays, workspace or all invocation memory.
   Projection takes place after execution so truncation cannot alter computation.
   In-flight memory/backpressure requires the later separate delivery contract.
4. Add an opt-in source-only host candidate. Replay the six original failures
   plus nested/function/dynamic/error cases against the selected installed SDK;
   the host consumes real records, never parses statements or guesses history.
5. A later live callback stage must separately specify cancellation, rejection,
   backpressure, callback borrowing and reentrancy. Result collection is not
   advertised as live delivery, browser acceptance or a release.

## Snapshot Projection

The public graph is distinct from writable SDK Value handles. Each graph owns
copied payloads and graph-local IDs; references cannot be used for execution or
writeback. Only nodes reachable through approved public edges are serialized,
with IDs remapped to avoid leaking hidden nodes or their count. Exact numeric
class, shape, IEEE floating bits/integer bits, complex data, UTF-16 units,
missingness, sparse CSC, tables, categorical/temporal data and frozen graphics
remain readable without acquiring a live runtime. Structured recursion follows
the same privacy policy for nested objects and closures.

Public projection is an inspection contract, not MATLAB overloaded display.
Explicit user-generated disp/fprintf text and diagnostics are retained as
generated; the recorder does not attempt to redact arbitrary user output.
Unavailable access metadata is not permission to export internal storage keys.

## Acceptance

- Red/green tests for private/protected/hidden properties, custom getters,
  inheritance, dynamic properties, foreign module ownership and closure values.
- Verify aliases/cycles and no hidden-node leakage after projection; retain
  precision and immutable data after mutation, module/owner destruction.
- C argument/output preservation, caller-sized old prefixes, old-symbol
  consumers, version negotiation and owning C++ lifetime checks.
- Count/byte/value limits, actual pre-failure ordering, caught errors, evalc
  depth and invocation isolation, including recording disabled controls.
- Full kernel/package tests, clean installed-SDK host rebuild/replay, unchanged
  production sibling sources, synchronized development manifest and main CI.

H03/H05-H08 and full H10 acceptance remain independent open gates until their
actual current evidence proves them.
