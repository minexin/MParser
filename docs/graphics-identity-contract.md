# Graphics Identity Development Contract

Status: opt-in serializer, standalone schema and C/C++ value SDK implemented.
ABI 2.11 adds current-runtime lookup and atomic property writes; see
[writeback contract](graphics-writeback-contract.md). Host negotiation,
UI selection/writeback and deployment require separate acceptance.

C ABI 2.7 adds `mparser_value_graphics_json_versioned(value, version, out_json)`.
C++ values expose `graphicsJson(version)`. Version 0 retains legacy behavior;
version 3 enables persistent identities in each scalar/array graph. Other
versions fail with INVALID_ARGUMENT and null output. The existing unversioned
C function remains available. Ownership and deleted-handle behavior are unchanged.

The same development ABI adds `mparser_result_graphics_json_versioned` and
`Result::graphicsJson(3)` for no-output plots and whole-session snapshots.
Results retain both representations captured under one graph lock at completion.
Later mutations, runtime reset and runtime destruction cannot change either
representation. A valid result without a snapshot yields an empty view with OK;
unsupported versions or null arguments yield INVALID_ARGUMENT. Returned views
are borrowed until result release. The development ABI 2.7 inventory contains
146 symbols; it has not been released.

`RuntimeGraphicsGraph::serialize(true)` emits graphics version 3. Existing
callers use the default `false` and retain version 1/2 output unchanged.

- `id`, parent, children, label and legend references remain snapshot-local.
- `graphIdentity` is a decimal string allocated once per graph.
- Each record's `objectIdentity` is a decimal string allocated once per node.
- The pair identifies an object across snapshots within one loaded kernel
  instance. Mutation and reparenting preserve it; deletion and recreation do not.
- Different graphs have different graph identities, including empty graphs.
- Strings avoid loss of integer precision in JavaScript consumers.
- These are identity labels, not authentication tokens or writable handles.
  No process addresses are exported.

## Lifetime and Host Boundary

Graph identity allocation is thread-safe and refuses counter exhaustion.
Identities are not durable across process restart or kernel unload/reload.
The host must additionally bind caches and requests to its runtime/session
generation and discard them when that generation changes.

The existing public machine-result schema intentionally does not accept v3 yet.
Do not insert this opt-in payload into existing public responses until a
downstream negotiation is implemented.

The standalone `graphics-v3.schema.json` checks record structure and canonical
positive decimal identity strings (up to 20 digits). It does not resolve
references, enforce identity uniqueness across records, or authorize writes.
Consumers must not mistake successful schema validation for live-target
validation. The kernel serializer validates graph topology separately.

## Remaining Acceptance

Before property writeback is exposed, resolve identity only against the owning
live session and graph under the graph lock. Reject foreign, expired, malformed
or deleted targets before mutation; never resolve a legacy snapshot id as an
object identity. The isolated REST probe now verifies partial deletion and
reference renumbering, rejected writes through a deleted script handle,
interleaved session isolation and namespace replacement on session reset.
The subsequent ABI 2.11 SDK adds a current-runtime identity-targeted write path;
the earlier probe alone did not expose it. Concurrent
sessions under contention, renderer selection refresh and DLL reload remain
separate downstream acceptance requirements.
