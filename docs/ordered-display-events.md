# Ordered Execution Events

Development contract: MParser 1.15, introduced in C ABI 2.9 and retained in 2.10. This additive
API returns an ordered, read-only collection after execution. It is not a live
callback API or a browser-rendering acceptance claim.

## Enable Capture

C++ callers set `Invocation::eventCapture` to `EventCaptureOptions{}` before
execution. Read `Result::executionEvents()` and `Result::eventCaptureSummary()`
afterwards. Both return owning copies that survive destruction of the result.
Leaving the option unset preserves the existing disabled-mode result interface.

C callers initialize `mparser_event_capture_options` with
`mparser_event_capture_options_init`, then assign its address to the
`event_capture` tail of an initialized `mparser_invocation`. Keep the options
alive through the execution call. Read the count with
`mparser_result_execution_event_count`; retrieve each record with
`mparser_result_execution_event_info` and the summary with
`mparser_result_event_capture_summary`, passing the output structure size.
Strings and diagnostic pointers in C records are borrowed until result release.
Copy them before releasing the result. No returned snapshot is a mutable Value
handle. Older invocation prefixes do not expose or read the new tail.

## Interpret Records

Assignment, expression, explicit text, warning and error records retain execution
order, including the prefix before failure. Suppression, capture depth and
diagnostic disposition are separate fields: a recorded suppressed assignment
is not an instruction to print it, and a caught error is not an unhandled error.
Source identity and ranges belong to each record; do not reconstruct them by
parsing output text or by inspecting the final workspace.

Automatic inspection text and structured snapshots protect private, protected
and hidden object storage and callable captures. Public dependent properties
and custom getters are unavailable rather than executed during inspection.
Unknown class ownership is opaque. Explicit user `disp`/`fprintf` text and
diagnostics remain user output, not a general-purpose redaction service.

`value_snapshot_json` uses `mparser.value-snapshot`, version 1. Node IDs are local
to one frozen graph; they cannot identify live objects or authorize writeback.
Numeric bit strings preserve exact values without JSON number rounding.
Snapshots expose only approved reachable nodes. A per-value failure is reported
as unavailable, never substituted with an empty value or the final workspace.

## Limits And Incomplete Results

Defaults are 4096 records, 16 MiB accounted collection bytes, 1 MiB per value,
10000 value nodes and depth 128. Zero means zero budget, not unlimited.
Count or collection-byte exhaustion retains a prefix and reports truncation.
Check the summary even after successful computation. Per-value limits can make
one value unavailable without truncating the collection or failing execution.

Byte accounting charges 256 bytes per record plus UTF-8 string payloads, 128
bytes per diagnostic/cause plus its strings, and 64 bytes per stack frame plus
its strings. It is a defined payload accounting unit, not measured process RAM.
It excludes allocation overhead, diagnostic caches, serializer temporaries,
legacy result arrays, workspace storage, the in-flight journal and semantic
`evalc` buffers. Projection and collection truncation happen after execution.
Live delivery, bounded in-flight recording and backpressure need separate work.

## Validation Boundary

Focused kernel tests cover ordered records, privacy, value snapshots, limits,
old invocation prefixes and recording-independent `evalc` behavior. Installed
SDK consumers and source-only host candidates require their own current runs.
Do not infer full host, browser, deployment or release acceptance from these
kernel tests. See `ordered-display-api-design.md` for the remaining gates.

### Local Development Evidence (2026-11-04)

- `build/ordered-events-expanded-test.log`: 355/355 kernel, SDK and package
  checks passed before the subsequent documentation-only synchronization.
- Clean source-only host build:
  `build/clean-host-probe/f46e75f750a8435890110dafed48abc9`, 148 build actions.
  The ordered-events REST probe passed the six original display cases,
  frozen history, failure ordering, suppression, evalc, UTF-8 and limits.
- The aggregate script stopped after those probes because Windows PowerShell
  could not resolve `Get-FileHash`. Remaining kernel checks were run against
  the same executable using PowerShell 7, with evidence at
  `build/host-compile-probe/a434885c86e7460784800e7edbaf0557` and
  `build/ordered-events-host-kernel.log`.
- Loaded SDK DLL SHA256:
  `5E202651C50AFBF11BFFD2DBB333E7EA88D6F0E567E0CA8CBFDFA00756D2CBC2`.
- Text, clipping, grid, legend and color candidate checks passed in simulated
  Canvas tests. These do not prove real-browser rendering or deployment.
  Graphics export still reports renderer unavailable; export is not complete.

The same source copy was rebuilt with `MEXEC_ORDERED_EVENTS=OFF` and passed
the legacy-request/new-option-rejection probe in `ordered-events-off-probe`.
ON executables and DLLs were retained in `on-binaries` before rebuilding;
the stage's main build directory now contains the OFF binary. The ON host
regression passed 67/67 in `build/ordered-events-host-ctest.log`.
Final GitHub CI and deployment remain to be checked.

### Retained Listener Follow-Up

The cross-command listener integration has a dedicated public-event regression:
`tests/ordered_events_api_smoke.cpp` covers four requested backends, and
`tools/host-integration/test-ordered-events.cjs` replays the installed SDK through
the isolated host. The source is created with recording disabled. A later
command inspects the listener, source, callback and nested container, then fires
the callback. Both event-listener `Source` and property-listener `Object{1}`
paths are covered; the retained PostSet callback also runs on each mutation.
Automatic text and snapshot JSON exclude private storage and
captured variable names/values, including the secret's encoded numeric bits.
The actual callback write precedes the consuming command's result and retains
its owner's source file/context. Reading history leaves the next callback
functional; earlier snapshots remain frozen after later mutation and exported
live-value release.

The expanded SDK test passed in `build/listener-property-public-events-test.log`;
installed-SDK REST evidence is `build/listener-property-public-events-rest` against
the source-only host stage `build/clean-host-probe/5881f098e00348d49173395e8711a484`.
This extends local result-collection acceptance, not live streaming, browser
rendering or production deployment. Explicit user output is still not redacted.
