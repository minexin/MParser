# Axes Layout Development

Development product 1.15.0, ABI 2.12 (158 exports). This is an incremental
implementation of the host's 2026-11-02 multi-Axes layout requirement, not
complete MATLAB layout, subplot, native Figure sizing or browser acceptance.

## Public Properties

Axes now stores `Position=[left bottom width height]` and `Units='normalized'`.
The default position is `[0.13 0.11 0.775 0.815]`. Coordinates refer to the
parent Figure's content area, with a lower-left origin. Position excludes
decoration margins. `InnerPosition` is a read/write alias of Position, not
a second independent rectangle. Source getters return a double row vector.

Writes require a finite numeric four-element row or column vector, with
nonnegative width/height. Zero dimensions and out-of-container origins are
accepted. Single/integer/sparse values are converted to double; complex
positions use their real components. Logical values, matrices, nonfinite
values and negative sizes are rejected before mutation. These choices follow
the measured R2024b cases rather than a generic vector-property coercion.

The normal `plot` replacement path preserves Position and Units while resetting
other ordinary Axes properties. `NextPlot='replacechildren'` retains them and
the other Axes properties. Reparenting keeps the current numeric position and
units; later conversions use the new parent's environment.

Current graphics snapshots carry Position and Units in the existing extensible
property map. The existing ABI 2.11 identity-targeted write accepts Position and
InnerPosition, with its normal session/graph/object validity checks. Existing
public layouts and symbols are retained. ABI 2.12 adds one environment setter
and a new, separately sized environment structure.

## Layout Environment Boundary

The graph receives an explicit per-Figure environment: content
width/height in logical pixels, pixels per inch, and character width/height in
pixels. All five values must be positive and finite, with representable point
scale. No screen, DPI, font metrics or Figure size is guessed by default.
Metrics use logical pixels consistently, not a high-DPI canvas backing-store
resolution. The host is responsible for measurement accuracy and for providing
updated metrics after its viewport/font environment changes; the kernel stores
supplied measurements, not an attestation of the actual desktop state.

With that environment the kernel converts all six canonical Axes units:
normalized, pixels, inches, centimeters, points and characters. Unit names
are case-insensitive and accept unambiguous prefixes. Pixel origins are one
based; the other unit origins are zero based. Changing Units converts Position
atomically instead of relabeling numbers. Missing metrics, malformed units or
overflow leave both properties unchanged. Exponent-space scaling avoids
intermediate overflow for otherwise representable conversions.

The approved policy (2026-11-05) is explicit host injection, with no virtual
defaults. `mparser_runtime_graphics_set_layout_environment` and C++
`Runtime::setGraphicsLayoutEnvironment(graph,figure,environment)` check the
current graph and live Figure under the same locks as assignment. The C
structure carries size and ABI generation plus the five metrics; short or
incompatible structures return ABI_MISMATCH. Invalid metrics/identities return
INVALID_ARGUMENT, foreign/reset graphs OWNER_MISMATCH, deleted targets
OUT_OF_RANGE and non-Figure targets TYPE_MISMATCH. No source, callback, window
creation or implicit Axes Units change occurs. Every successful injection
advances that Figure's non-reused decimal layout revision. Deletion/reset
discards the environment; callers must inject afresh for new identities.

Without an environment, normalized positions and same-unit selection work;
cross-unit conversion fails explicitly without mutation. Hosts must not infer
metrics from test fixtures, current desktop dimensions or object order.

## Opt-In Snapshot V4

Runtime, Result and Value `graphicsJson(4)` explicitly select the identity and
layout format in `graphics-v4.schema.json`. Every Figure has a `layout` field:
null if unconfigured, otherwise revision, contentSizePixels, pixelsPerInch and
characterSizePixels. This is enough to interpret that Figure's supported
physical units without reading mutable host settings later. A null environment
is an explicit unavailable state, not permission to guess rendering dimensions.
Reparenting into an unconfigured Figure preserves numeric properties but does
not manufacture an environment. Hosts must configure it before interpreting
environment-dependent layout. Result snapshots freeze the environment and its
revision; retained live Values continue to reflect their owning live graph.
Result storage currently retains all three serialized representations eagerly;
this does not promise zero-cost opt-in or a per-scene snapshot memory bound.
Versions 0 and 3 never acquire layout fields. Their extensible property maps
still carry the new Position/Units properties. The default export is unchanged.

The source-copy host candidate defaults OFF and requires ABI 2.12 plus explicit
graphics v3. Its POST graphics-layout-environment accepts string identities and
an unsigned decimal string expected workspace revision, serializes the call
with execution/reset and advances the host revision without incrementing
run_count. GET graphics-layout is a read-only version-4 current-graph snapshot
paired with that revision. Existing execution and graphics routes retain v3;
consumers explicitly negotiate/use the layout route. No production host source
or browser renderer is changed. After uncertain transport failure, refresh the
revision/frame instead of assuming no commit occurred.

## Evidence And Remaining Work

Actual MATLAB 24.2.0.2712019 (R2024b), PCWIN64, recorded 50 cases in
`tests/axes_layout_reference.json`; a second assertion-enabled run passed at
`build/axes-layout-r2024b-checked.log`. The controlled environment used an
800-by-600 Figure and measured 96 pixels/inch. This is not a portable default
environment. `axes_layout_smoke` checks 215 source outcomes (HIR plus four
requested VM modes) and internal
environment controls. `axes_layout_api_smoke` separately replays the 172
outcomes through the installed public ABI surface and checks frozen v4
environments, legacy isolation, invalid targets and reset/concurrent updates.
The identity SDK smoke also checks normalized Position writes and
missing-environment preservation.

Complete rebuilt kernel/package regression passed 360/360 in 48.56 seconds at
`build/axes-layout-final-full-test.log`; the subsequent HIR-strengthened layout
pair passed at `build/axes-layout-hir-test.log`. Source-only installed-SDK host acceptance
passed all 148 build actions, dedicated REST/client probes and 67/67 regressions
(20.82 seconds). Stage:
`build/clean-host-probe/da813f8efbc74603be061f2d84f945d4`; aggregate log:
`build/axes-layout-clean-host.log`; independent kernel probe:
`build/host-compile-probe/fcaf5d56d088461ab48842bc45ebfcc6`. The selected and
loaded DLL SHA256 is
`EC3756F5B69A5D363955DB07A00E1E8917A4A401E54B22689DB04EF84A399C9B`.

`probe-axes-layout-compat.ps1` passed an ABI 2.11 SDK build with layout OFF,
absent routes/capability and existing writeback. The same old-header host also
passed those bounded probes with the new ABI 2.12 DLL. Old SDK + layout ON
failed configuration as required. The stage was restored to the new SDK/ON,
then passed the layout probe and 67/67 host regressions (19.71 seconds).
Evidence is in `build/axes-layout-compat.log` and separate stage response sets.
This is not a full old-header host regression with the new DLL. Actual-commit
GitHub CI remains a separate gate, including hosted macOS runner availability.

The first feature-commit CI passed Windows, both macOS architectures and
Clang sanitizers, but GCC rejected an inlined SDK numericScalar optional-span
construction with maybe-uninitialized warnings. The scalar helper now separates
real and complex construction explicitly. Added real/complex, signed-zero and
logical scalar checks passed with 360/360 complete regression in 48.36 seconds
at `build/axes-layout-scalar-fix-full-test.log`. A second fresh installed-SDK
source-only host passed 148 actions, all probes and 67/67 regressions (20.71
seconds) at `build/clean-host-probe/98b8df0155804973afe2b53ce5101abd`, aggregate
`build/axes-layout-scalar-fix-clean-host.log`. The DLL hash is unchanged; the
earlier old-header/new-DLL controls still bind that exact DLL. Exact fix-commit
GCC CI remains a separate gate until it completes.

The online [Axes properties](https://www.mathworks.com/help/matlab/ref/matlab.graphics.axis.axes-properties.html)
and [Figure properties](https://www.mathworks.com/help/matlab/ref/matlab.ui.figure.html)
explain unit/position concepts; the installed R2024b run anchors this corpus.

Still open: actual browser environment measurement/rendering and negotiation,
full native Figure Position/Units,
OuterPosition/PositionConstraint and decoration-inset solving, subplot/tiled
layout, browser multi-Axes rendering, production adoption and actual-commit CI.
The native Figure zero-width reference is visibly clamped by MATLAB's window
system; it is not implemented or emulated by this headless Axes foundation.
