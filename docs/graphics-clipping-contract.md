# Graphics Clipping Contract (Development)

This document describes the unpublished kernel implementation. MExecClient
rendering acceptance is still pending; the v1.14 published artifacts do not
include these changes.

## Kernel Properties

Axes and Line nodes have `Clipping`, initially `on`. Figure has no Clipping
property. Assignments accept character/string scalars `on` and `off`, or real
dense numeric/logical scalar 0 and 1. Other values are rejected before property
mutation. Snapshots encode the canonical string `on` or `off`.

Source reads currently return a character vector, as with the existing Visible
property. This is not MATLAB's `matlab.lang.OnOffSwitchState` enumeration;
enum identity/conversion compatibility is not implemented.

Setting Clipping does not change XData, YData, XLim or YLim. The kernel does not
discard out-of-range points or calculate pixel-space line intersections.
`NextPlot=add` preserves the Axes setting; `replacechildren` preserves it while
deleting the lines; `replace` restores the Axes default. Newly created lines
start with Clipping on independently of the Axes setting.

## Host Rendering

For the current 2-D Line model, clip a line to the Axes plot rectangle only when
both Axes.Clipping and Line.Clipping are on. Axes.Clipping off disables clipping
for its lines; Line.Clipping off disables it for that individual line. Apply
the clipping state while drawing the line, without affecting axis labels or
titles. Retain all original samples in the host data model.

Old snapshots lacking these properties can be interpreted as on, matching the
previous fixed-clipping rendering behavior. This fallback does not authorize a
host to simulate property writes or invent kernel state. Future 3-D objects and
ClippingStyle require a separate contract.

References: [Line properties](https://www.mathworks.com/help/matlab/ref/matlab.graphics.chart.primitive.line-properties.html)
and [Axes properties](https://www.mathworks.com/help/matlab/ref/matlab.graphics.axis.axes-properties.html).
These establish the intended property semantics, not pixel-level MATLAB parity.

## Evidence and Open Acceptance

`runtime_graphics_smoke` exercises HIR and Bytecode defaults, text/numeric/logical
writes, invalid-value preservation, unchanged samples, and hold/replacement.
`graphics_api_smoke` checks exported Axes and Line values and retains samples
outside explicit limits. Both tests passed in the native Windows build.

Subsequent development added isolated MExecServer transport, per-line client
candidate checks and bounded real-browser Clipping pixels; see
[browser acceptance](host-browser-acceptance.md) for the exact scene, DLL and
remaining boundaries. This does not establish full pixel parity, deployed
client adoption or a release. Basic Legend creation/visibility/location/deletion
is also implemented and covered by kernel/candidate tests; explicit labels,
AutoUpdate controls and full layout remain outside that validated subset.
