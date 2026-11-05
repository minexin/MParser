# Isolated Browser Acceptance

This is a bounded desktop smoke check, not full browser, deployment or release
acceptance. Production MExecServer/MExecClient sources and services were not
modified. The captured runs below used development 1.15.0, ABI 2.10 (154 exports),
not the later ABI 2.12 layout-environment SDK. Current SDK/REST tests do not
replace these separate browser-pixel acceptance gates.

## Selected Pair

- Source-only host stage:
  `build/clean-host-probe/5881f098e00348d49173395e8711a484`.
- Its installed kernel DLL SHA256:
  `89004D8A8A702854A2770AF2C838754BFFD9370F4F64590DC1E2D5745C753DBB`.
- An isolated loopback host used an OS-assigned port and a new file root.
  Health reported product 1.15.0 and ABI 2.10. The process was identified by
  exact executable path and start time and stopped after the check.
- Browser evidence:
  `build/browser-acceptance/a47ca21a-cb09-4200-b6ed-ba1e9df51b33`.
  `session.json` binds the selected binary/hash and `observations.json` records
  the bounded result and failed control paths.

## Observed Desktop Result

The real Chrome command input executed:

```matlab
h=plot([1,4;2,2;3,5]);
title('Kernel Browser'); xlabel('Time'); ylabel('Power'); grid on;
h(1).DisplayName='Measured'; h(2).DisplayName='Reference';
h(1).Color=[1,0,0,0.5]; h(2).Color='b';
ax=gca; ax.YLim=[0,6]; legend('show');
```

The page reported success. Its screenshot visibly showed both matrix columns
as curves, the title and axis labels, grid lines, ordered legend labels, and
pink alpha-composited/red and blue strokes. Browser current-view PNG export
produced `figure.png`, 1080 x 377 pixels, SHA256
`791134D1F9DBE9831EDB0A18B79BA0CE5DC8A95BD7748BAD27B33CF585FB0AAD`.
Decoded pixels included 2401 red-dominant and 2806 blue-dominant pixels; the
image was also visually inspected. This is a real browser canvas PNG, not an
MParser print/saveas renderer, a PDF encoder or a server-side export result.

The download-event wait timed out even though the PNG reached disk. The browser
then reported an unattached debugger; native window approval also timed out.
That initial run did not establish close/recreation or responsive interaction. The full-page
screenshot is present in the chat evidence, not as a local persisted artifact.

## Runtime Identity Candidate

The page exposed a separate host display defect: the initial banner hard-coded
`MParser V1.14`, and the status bar preferred the `development` milestone over
the runtime product version. The health/API kernel identity itself was correct.

`tools/host-integration/mexecclient-runtime-identity-candidate.patch` removes the
initial asserted version and takes the connected version from `/info.runtime.version`.
The status bar displays the milestone separately. Missing version fields remain
unknown rather than being inferred from the milestone or client build.
All values are written through textContent, not HTML.

The new bootstrap test first failed on the original static V1.14 banner in
`build/runtime-identity-red.log`, then passed for development/legacy/missing
fields and literal text. Source-copy patch replay passed in
`build/runtime-identity-candidate-probe.log` at
`build/client-rendering-probe/dc0e39b5b4f24da68d34eed33059ac41`.
The desktop image above predates the correction. The source-only clean-host
runner applies this candidate separately from the five graphics-renderer patches.
The later real-browser follow-up below verifies the corrected banner/status.

Fresh source-only host acceptance with all six client candidates passed at
`build/clean-host-probe/dce947552b47427ab10b1da1c78f0256`: 148 build actions,
all REST/client probes (including the expanded listener privacy replay and
runtime-identity bootstrap checks), and 67/67 host regressions in 21.66 seconds.
Aggregate log: `build/runtime-identity-clean-host.log`. Kernel/package tests
also passed 355/355 at `build/host-browser-final-test.log` in 47.33 seconds.
These source-copy checks remain separate from actual browser interaction.

## Corrected Browser Follow-Up

A fresh isolated host from the six-candidate stage above was checked in Chrome.
Evidence directory:
`build/browser-acceptance/ea1e3027-3435-4b3d-b7a1-158b0c864f19`.
Health/DLL identity is recorded in `session.json`; detailed observations are in
`observations.json`. No production host/client source was changed.

- The actual page banner displayed `MParser 1.15.0`; its status bar displayed
  product 1.15.0 and `development` separately.
- Closing the Figure cleared the canvas, disabled plot/export controls and
  showed `old_valid=false`. Plotting again restored the new curve/title with
  `new_valid=true` and `stale_valid=false` for the old Line.
- Creating a second Figure selected it. Selecting Figure 1 restored the first
  curve; zoom changed the limits and reset restored the original limits.
- The plot and controls remained visible at 1280 x 800. At 390 x 844 the layout
  cropped horizontally; mobile layout acceptance fails for this tested state.
  The temporary viewport override was reset.
- Two coincident Lines over `YLim=[0.5,1.5]` demonstrated per-Line clipping:
  the red `Clipping=off` stroke extended outside the axes, whereas the blue
  `Clipping=on` stroke stopped at the boundary. The exported `clipping.png`
  is 1080 x 377, SHA256
  `F0AF95709920909CC6F397D84E1B221ACAB33420B1939FC8A5EEA33E1263FF00`.
  Pixel inspection counted 420 red-dominant and zero blue-dominant pixels
  outside the axes' vertical span. The image was also visually inspected.
- After `seed_browser=42;`, malformed `broken=;` produced visible
  `compilation-failed` and the submitted command's line-1 diagnostic. The
  workspace still showed 42 and the existing Figure remained visible.

The browser screenshots are retained in the chat, while the PNG and selected
host/observation records are local files. The isolated host was stopped through
its owning PTY and no server process remained. These cases do not establish
every lifecycle/selection race or a deployed client.

## Remaining Gates

The v4 environment/multi-Axes source-copy candidate now has a separate bounded
actual Tabbit measurement/pixel/resize/selection/zoom/reload run in
[the Canvas layout guide](host-canvas-layout-candidate.md). The inspected dual
Axes screenshot and Canvas PNG are retained under build/browser-layout-20261105.
That historical run confirmed 390px cropping failed and predates the new
Legend-creation kernel. The latest ten-candidate current-SDK follow-up closes
that measured cropping failure at five tested viewports and checks actual
repeated/cross-Axes Legend rename/delete/invalid-call preservation. See the
[compact candidate guide](host-compact-layout-candidate.md) for screenshots,
Canvas pixels, keyboard/draft/filter workflows and exact limitations. Its fresh
installed-SDK host replay passed 67/67; original host/client sources and services
remain unchanged. Neither run establishes download UI or production adoption.

- Broader lifecycle/selection races beyond the tested close/recreate/switch cases.
- Responsive interactions beyond the five compact-candidate viewports/workflows,
  touch/hardware keyboards and full screen-reader acceptance.
- Exact submitted-source navigation, document freshness and user-facing failures.
- Host property writeback, protocol negotiation and deployment.
- Kernel file export remains explicit renderer-unavailable; browser PNG export
  is not a substitute for that contract.
- GitHub synchronization and CI for the actual current commit remain separate.
