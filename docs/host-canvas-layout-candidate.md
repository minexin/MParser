# Host Canvas Layout Candidate

Status: source-copy integration candidate, 2026-11-05. Product 1.15.0 remains
development; C ABI 2.12/158 and machine protocol 1.2 are unchanged. Original
MExecServer/MExecClient sources and deployed services are not modified.

## Selection And Measurement

Apply `mexecclient-axes-layout-candidate.patch` after the five renderer patches,
then `mexecclient-layout-sync-candidate.patch` after the runtime-identity patch.
The latter loads plot-layout.js before plot.js/app.js and adds the two explicit
REST methods. `/info.features` must advertise `graphics-layout-environment`.
Without it, the existing graphics route and v1/v2/v3 renderer remain in use;
the client does not probe unsupported routes or invent a capability.

The candidate measures the connected, visible Canvas content area in logical
CSS pixels (client dimensions minus padding). Zero/hidden/unmounted areas and
fonts still loading defer injection. No 600x400, screen-size or test-fixture
fallback is submitted. A temporary CSS `1in` ruler measures the browser's
logical pixels-per-inch, not physical monitor DPI. Character units use the
width of ten zero glyphs divided by ten and the line-box height of an inline
measurement with the Canvas computed font. Probes have no padding/border,
zero letter spacing and are removed in finally. These are documented host
metrics, not a claim to reproduce MATLAB's desktop default font.

CSS absolute units and physical display measurements differ; see the
[W3C absolute-length definition](https://www.w3.org/TR/css-values-4/#absolute-lengths).
DPR scales the backing bitmap only. It does not multiply the environment,
source Position, logical DPI or character size. ResizeObserver, window resize,
font completion, opening the plot view and selecting a Figure request a
debounced refresh. Only the currently selected Figure is configured; hidden
Figures are configured when selected and measurable.

## Mutation And Recovery

The coordinator serializes refreshes and coalesces concurrent requests. It
reads GET graphics-layout, renders that v4 snapshot, then compares measured
metrics with the selected Figure's frozen environment. An identical profile
does not advance either revision. A changed profile is sent with exact decimal
graph/Figure identities and the GET's exact string workspace revision. No
source command, property-parser emulation, Number conversion of uint64 values
or implicit Units assignment is used. A successful mutation refreshes workspace
aliases/revision without executing source.

Every awaited result is checked against the captured session and graphics
epoch. Session switches, reset and newly handled execution results discard
late results. A failed/uncertain POST first rereads the authoritative snapshot
and refreshes workspace state; it does not assume rollback. Busy/conflict
responses do not spin or blindly replay the mutation. Another explicit refresh
can retry from current identities/revision. Transport errors remain visible.
This is request-driven refresh, not subscription-based live synchronization.

## Rendering

For v4 the selected Figure's Axes are all drawn, in snapshot order. Position
is converted from bottom-left origin into Canvas top-left coordinates. Six
kernel-supported units are consumed: normalized, pixels (one-based origin),
inches, centimeters, points and characters. Environment-dependent units with
missing/invalid metrics are not interpreted using guessed values. Nonfinite
projected rectangles are skipped. Off-Figure origins remain off-Figure, and
zero-sized Axes do not acquire a fabricated minimum size.

Lines retain their data, per-Line Clipping, colors and styles. Labels follow
their own Axes rectangle. An outside Legend no longer shrinks an explicit
kernel Position. Decoration-inset/Legend overlap solving is not implemented.
The Axes selector chooses the target of wheel/keyboard/pan controls; it does
not hide other Axes. Zoom/pan ranges are keyed by exact graph/object identity
and remain host-only. Environment-only updates retain selection and ranges;
changed object membership or a new graph clears stale view state. Retained
selector callbacks cannot act on a replacement scene. Legacy snapshots retain
the old selected-Axes/fixed-margin behavior.

PNG export rejects an unmeasurable v4 Canvas or incomplete environment-dependent
layout rather than downloading stale backing pixels or a partial Figure.

## Evidence And Limits

`test-client-axes-layout.cjs` checks the six conversions, missing metrics,
measured-probe cleanup, hidden/font-loading deferral, two-Axes drawing, DPR,
independent zoom, immutable snapshots, exact large identities/revisions,
session/epoch staleness, uncertain mutation and bounded conflict recovery.
`test-client-layout-bootstrap.cjs` checks the actual API/app wiring, capability
fallback, JSON body, workspace refresh and deduplication. The frozen fixture
adds the unchanged sibling plot-view.js; existing plot.js/test snapshots remain
unchanged. CI replays these renderer candidates with a simulated Canvas.

`test-client-layout-rest.cjs` uses the actual selected server/SDK plus the
candidate coordinator. It verifies static-module delivery, source conversion
readback, resize/reset/reinjection, unchanged run_count and legacy v3 isolation.
Its metrics are explicitly controlled inputs, not browser measurements.

Initial bounded REST evidence is under
`build/client-layout-rest-retest-20261105`; source-copy replay is under
`build/client-rendering-probe/a0c0a1cf74c841ada5b5d9a62ceac2d1`.
The initial stable Tabbit launcher exited 1 before attachment. After the user
reinstalled/opened Tabbit, the stable launcher connected successfully. No
browser runtime, registry or security settings were modified by this work.

Fresh installed-SDK source-only host acceptance passed all 148 build actions,
the dedicated REST/controller/bootstrap probes and 67/67 regressions in 20.06
seconds at `build/clean-host-probe/bf8b468cec354136812c3dae24f0c940`.
Aggregate log: `build/client-layout-clean-host.log`; independent kernel evidence:
`build/host-compile-probe/7c80b6bd19b1406d91ebdc3d02c1cba4`. Loaded DLL SHA256:
`EC3756F5B69A5D363955DB07A00E1E8917A4A401E54B22689DB04EF84A399C9B`.
Complete kernel/package regression passed 360/360 in 65.05 seconds at
`build/client-layout-kernel-test.log`. Final export-guard replay passed under
`build/client-rendering-probe/22721241bc3c4695a4424bb128c5d9ca`; that same
renderer was copied to the owned host evidence stage for the final host rerun.
This does not modify either original sibling project. Exact submitted-commit
GitHub CI is checked separately from these local results.

## Actual Browser Follow-Up

The same SDK/host stage above was served from an isolated process, with the
exact EC3756F5 DLL. Tabbit Browser stable loaded the actual client. Evidence:
`build/browser-layout-20261105/session.json`, `observations.json`, the inspected
`desktop-dual-axes.png` and inspected `recreated-canvas.png`.

- At 1904x915, actual Canvas logical size was 1071x375, logical DPI 96 and
  measured character size 7.0078125x17. The v4 Figure contained these exact
  metrics. Both Axes were visible with separate red/blue pixel ranges: red
  x=107..427, blue x=642..963, 697 classified pixels each. The screenshot was
  visually inspected, not inferred from a successful transport response.
- A temporary 1280x800/DPR=2 page emulation produced a 715x298 logical Canvas
  and 1430x596 backing bitmap. Source conversion to pixels read back
  [108.1,76,321.3,225] (floating representation retained); resize did not change
  those pixel-unit numbers. The injected environment used logical dimensions.
- Actual selection/zoom controls changed Axes 2 ranges to [0.1,0.9], left Axes 1
  at [0,1], retained the independent view on switching back and reset correctly.
- Close/recreate invalidated the old Line and changed Figure identity from 1
  to 8. Reloading the same page/session restored the current Figure with
  run_count unchanged at 3, without executing hidden source.
- At 390x844, document width was 650 and Canvas right edge 462: narrow-screen
  acceptance still fails. Canvas itself had nonwhite pixels; that does not
  excuse horizontal clipping. The original 1904x915 page size was restored.
- Actual browser Canvas PNG encoding was saved and visually checked at
  1071x375 (`recreated-canvas.png`). This is not evidence that the browser's
  download UI completed a button-triggered download. Page screenshots during
  viewport emulation timed out; those failed captures are not counted as
  visual passes. DOM geometry and pixel/encoding results remain separate.

The Tabbit task finished successfully without forced cleanup. The owned server
was stopped after verifying its exact executable path; no production service
was touched. This bounded run predates the newer Legend-creation kernel and
ninth client candidate, and does not establish every stale-response/resize
race, native Figure layout, release acceptance or deployed host adoption.
