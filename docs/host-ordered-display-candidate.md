# Ordered Display Client Candidate

Status: opt-in source-copy integration candidate, 2026-11-05. Product/SDK
1.15.0 remains development, C ABI 2.12/158, builtin contract 1.22 and machine
protocol 1.2 unchanged. This candidate consumes the existing read-only result
API; it adds no kernel semantics, live callback, public accessor or schema.

## Selection And Ownership

Apply `mexecclient-ordered-events-candidate.patch` after the ten preceding client
candidates. The clean-host, original-client and frozen-fixture replay runners
now apply eleven patches. The server must separately enable
`MEXEC_ORDERED_EVENTS` and advertise `ordered-display-events`.

The command panel's checkbox is labelled `有序内核回显`. It is unchecked on load,
not persisted, and hidden/cleared if the host does not advertise support.
Capability advertisement alone never requests capture. Checking it adds
`include_execution_events:true` and `max_execution_events:4096` to subsequent
ordinary or conditional execute requests. An explicit caller option takes
precedence. Compile-only requests do not acquire capture options. Debug start
and paused-frame evaluation retain their existing protocol and display path.
Nothing is retried automatically after an ambiguous execution response.

The client renders `mexec.execution-events` version 1 in its original sequence,
not grouped by record kind. Assignment/expression labels and protected text
come directly from the kernel. It does not parse source, query final workspace
values, evaluate getters, format snapshot graphs or invent missing history.
Existing literal-text command primitives retain safe text insertion. Compact/
loose row spacing uses the record's hint, not a claim of full MATLAB display.
Explicit entry-function returned outputs retain their existing separate return
display after the captured records; they are not reconstructed assignment events.

Suppressed output, captured evalc depth and caught/raised diagnostic records do
not become ordinary visible output. Visible/unhandled diagnostics reuse the
existing detail renderer and exact submitted-document navigation guards.
Complete started collections suppress the legacy text/expression/diagnostic
groups, preventing duplicate display. With no collection, old behavior remains.
Pre-execution compilation diagnostics remain visible when capture did not start.
The outer execution-status line is host status, not an execution event.

Malformed/future schemas, noncanonical or out-of-order sequences and invalid
counts produce an explicit protocol warning before any event is displayed;
legacy value/text groups are not used to fill that history. Uint64 counts are
checked without converting to JavaScript Number. Per-value unavailable records
are explicit, not empty values. A truncated collection shows its retained prefix
and exact counts/reason. Aggregate diagnostics follow a labelled boundary
outside that prefix, without guessing which diagnostics were omitted.

## Evidence

`test-client-ordered-events.cjs` exercises actual patched api.js/app.js and the
new consumer with a simulated DOM. Coverage includes explicit/default/disabled
capture, conditional/compile/debug boundaries, request/input immutability,
historical values, interleaving, suppression, evalc/caught/unhandled errors,
legacy-group duplication, truncation, unavailable values, exact counts beyond
2^53, invalid/future protocols, function returns and stale-source navigation.
Frozen eleven-patch replay passes at
`build/host-graphics-candidate-smoke/run-Hqsput`. The final compact test also
rejects panel maximization from a checkbox-label double-click. These are not
browser pixels.

`test-ordered-events.cjs` accepts an optional fourth client-root argument. It
then sends real requests through that patched API, renders real SDK records
through the consumer and checks the original six display cases plus checkbox
defaults, two historical writes, truncation, evalc capture, private-object/
closure summaries and an explicit function return. Raw focused evidence:
`build/ordered-client-return-rest-20261105`; log:
`build/ordered-client-return-rest.log`. No hidden source or closure values appear
in the automatic summaries. Explicit user output is not a redaction service.

Fresh source-only installed-SDK host replay:
`build/clean-host-probe/069f5ae6d50b4b83932e862cc0c79a8a`. All 148 build actions,
REST probes, eleven client candidates and host CTest 67/67 passed (23.15 seconds)
at `build/ordered-client-clean-host.log`. The final focused return-value case was
added afterwards and passed against that same executable separately. SDK DLL
SHA256 is `9689A754300DF313D828F7FD9692C6D9447426AC05D5CFC2EF85AAD1A67AE298`.

The final guarded candidate and expanded return-value probe were then replayed
from original sources in a second fresh installed-SDK host at
`build/clean-host-probe/0d6110aac1ca4a15bf72b4b9f8b4dd7a`. All 148 build actions,
REST/client checks and 67/67 host tests passed (19.65 seconds), recorded in
`build/ordered-client-final-clean-host.log`. Final original-client replay passed
at `build/client-rendering-probe/917360c42ccd4fe5a6afa8649ef89b85`.

Complete rebuilt kernel/package regression passed 362/362 in 62.61 seconds at
`build/ordered-client-full-test.log`. A separately built current-SDK host with
`MEXEC_ORDERED_EVENTS=OFF` passed the bounded capability-absence, explicit-option
rejection and real patched-client legacy execute controls at
`build/ordered-client-off-probe-20261105` (`build/ordered-client-off.log`). That
is not a full OFF-mode host regression. No original ON binary was overwritten.
The subsequent six candidate/documentation/installed-consumer/archive checks
passed in 67.03 seconds at `build/ordered-client-package-test.log`; final
evidence-note synchronization has its own rerun in
`build/ordered-client-sync-package-test.log`.

Actual Tabbit interaction evidence is in `build/browser-ordered-20261105`:
`results.json`, `layout.json` and `final-state.json`. Native checkbox/command
actions prove default OFF, ON interleaved writes/text/warning order, evalc and
caught-error suppression, one unhandled error after the pre-failure value,
compile-error visibility and return to legacy OFF. The 390x844 DOM check has
document width 390 and the checkbox within bounds. Desktop dimensions were
restored to 1904x915, checkbox OFF and no dirty editor; no test source was
replayed after capture failure. Tabbit finished without forced cleanup and
only the verified isolated server was stopped. Original sibling projects and
production services were not changed.
The final label double-click exclusion followed this browser run and has the
separate simulated compact-layout check described above.

Native page screenshots (portrait/desktop) and an element screenshot timed out.
This run therefore establishes actual interaction/DOM evidence, not screenshot
or pixel-level visual acceptance. It is not production deployment, full browser
coverage, overloaded MATLAB display, debugger adoption, live streaming or bounded
in-flight journal memory. Actual-commit GitHub CI and final installed-package
checks remain separate gates until their matching run results are recorded.
