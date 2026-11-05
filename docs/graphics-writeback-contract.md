# Identity-Targeted Graphics SDK

Development product 1.15.0; introduced in C ABI 2.11 (157 total exports).
The subsequent layout follow-up advances to ABI 2.12/158 with a separate host
environment setter and opt-in graphics v4; see axes-layout-development.md.
The three identity-writeback functions are additive. Old symbols, invocation layouts, graphics
versions and frozen release contracts are unchanged.

## Operations

- `mparser_runtime_graphics_json_versioned` / `Runtime::graphicsJson(version)`
  capture the current graph without executing source or reconstructing it
  from a previous Result. Versions 0 and 3 are accepted; ABI 2.12 also accepts
  opt-in version 4 with explicit layout environments. The C output is an
  owned character JSON value with ASCII escapes, including Unicode labels.
- `mparser_runtime_graphics_value` / `Runtime::graphicsObject(graph,object)`
  resolve canonical positive uint64 decimal identity strings only in the
  current owning runtime graph. The returned Value retains that graph, not
  the Runtime. It is a live handle, not an immutable historical snapshot.
- `mparser_runtime_graphics_set_property` / `Runtime::setGraphicsProperty`
  validate the graph and live object, then reuse the kernel's normal property
  validation/assignment under the same runtime and graph locks. Lookup and
  assignment are not separated by an unlocked interval. No source code,
  user getter, callback or workspace assignment is executed.

Malformed/noncanonical identities return INVALID_ARGUMENT. A well-formed
foreign or reset graph returns OWNER_MISMATCH; a missing/deleted object in
the current graph returns OUT_OF_RANGE. Invalid property/value combinations
return TYPE_MISMATCH. Read failures null their output handles. Allocation
and internal errors retain their standard C status. Input values and runtime
handles follow the existing independent-reference/threading rules.

Snapshot-local numeric IDs are never mapped by record position. The required
graph/object identity pair is not authentication: the host must bind requests
to an authenticated session and its generation. Re-resolve on each write;
an exported old handle may still retain an old graph after Runtime reset, but
the new identity-targeted write rejects that graph. The SDK does not implement
an optimistic per-property revision check or an all-properties transaction.

## Host Candidate

`mexecserver-graphics-writeback.patch` is applied only to source copies in the
MParser build directory. It defaults OFF and requires ABI 2.11 plus explicit
graphics v3 when enabled. The candidate POST session graphics-property route
requires string graph/object identities, a property, a normal JSON value and
an expected workspace revision as an unsigned decimal string. Both revision
requests and responses preserve uint64 values without JavaScript rounding.
It rejects busy/conflicting requests, calls
the SDK directly, advances the workspace revision for alias invalidation and
refreshes the last completed graphics export. It does not increment execution
run_count or compile a property-assignment script. It is not deployed to the
original MExecServer/MExecClient sources or production service.
After an uncertain transport/server failure, refresh the revision/snapshot;
do not assume a failed response proves the write was not committed.

## Acceptance

`graphics_identity_api_smoke` covers four requested execution modes, real
mutation/readback, Unicode, malformed/overflowing IDs, invalid-write
preservation, partial deletion/local-ID renumbering, cross-runtime rejection,
concurrent readers/writers and stale reset targets while an old graph remains
externally retained. The installed C++ consumer calls all three new symbols.
`test-graphics-writeback.cjs` adds independent REST revision, stale/local ID,
foreign session, reset and no-source-execution checks. Two concurrent requests
with the same expected revision must produce exactly one successful commit.

Final rebuilt kernel/package regression, including the strengthened reset-race
checks and a concurrent-read identity assertion, passed 358/358 in 94.06 seconds
at `build/graphics-writeback-ci-fix-full-test.log`. The identity assertion also
consumes the JSON parser result as required by GCC's warnings-as-errors build.
Fresh source-only installed
SDK host acceptance passed 148 build actions, all dedicated probes and 67/67
host regressions (19.48 seconds). Stage:
`build/clean-host-probe/4ac83d4ddaf646ceac291e9b935ffac9`; aggregate log:
`build/graphics-writeback-final-clean-host.log`. The loaded DLL SHA256 is
`B746E1C27DFA0F6C7035611D8168219FD0EF77573460107255D7DCB4436BB1F3`.
The strengthened REST race probe also passed at `build/graphics-writeback-race-probe`.

`probe-graphics-writeback-compat.ps1` verifies that an ABI 2.10 SDK build with
the feature OFF exposes neither the capability nor route, while legacy
execution/graphics works. The same old-header host also passed that bounded
probe with the new ABI 2.11 DLL. Old SDK + feature ON failed configuration as
required. The stage was then restored to the new SDK/ON configuration and
passed the race probe plus 67/67 host regressions (19.93 seconds). Evidence:
`build/graphics-writeback-compat.log`, with separate raw responses in the stage.
This is not a full legacy-host regression with the new DLL.

Actual-commit GitHub CI remains a separate gate. Browser
object selection, UI editing/negotiation, property-specific conflict policy,
DLL reload generations, rendering/export and production adoption are not
established by the SDK or a successful REST transport.
