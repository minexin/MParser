# Browser Acceptance

These are bounded checks of separately identified host/client pairs, not
full browser, production or release acceptance. The ABI 2.13 PNG download
replay is isolated; the deployed loopback replay does not install a renderer.
The historical desktop check starting at Selected Pair used ABI 2.10
(154 exports), not the later layout-environment SDK.

## ABI 2.13 Browser PNG Replay

On 2026-11-05, actual Tabbit interaction exercised the source-copy host at
`build/clean-host-probe/38989fc5d01243869d276c8ba540df5d/build` against the
installed 1.15.0 ABI 2.13 SDK. Its loaded DLL path and SHA256
`C749ACFC0D0FD5128F2554D211A88E587FF2CDA72EA000877287ED98AE11D0F0`
were verified from the running process. This invocation had no renderer port
configured; the optional REST encoder from the separate H07 proof was absent.
The isolated service listened on loopback port 6373 with a fresh file root.

Evidence root:
`build/browser-acceptance/9c7257347f924fd48c1337f0a61574f6`.
`session.json` binds the executable, start time, DLL/hash and actual `/info`.
Real command input created red and blue Figures. The selector chose the first
red Figure while the kernel current Figure was the second blue one. Clicking
the panel export button produced `figure (2).png`; selecting the blue Figure
and clicking the ribbon export button produced `figure (3).png`.

The native Tabbit download list identified both completed files from the
isolated host. The user also confirmed the first file. Copied bytes were
decoded and inspected, not inferred from Canvas pixels or a clicked button:

| Download | Dimensions | Bytes | Red/Blue Dominant Pixels | SHA256 |
| --- | --- | --- | --- | --- |
| Red selected Figure | 1071 x 375 | 25983 | 1635 / 0 | `81fcca0f5f7079698f3fc06ce909ca3d54f1e9d837eb784ceeb77494589685c7` |
| Blue selected Figure | 1071 x 375 | 26451 | 0 / 1704 | `3cfb124851949ce249b2809606f6acd8ac75900e493fbbaec07e81c64ea2d898` |

`verified.json`, `download-red.png`, `download-blue.png` and the saved native
download observations retain this proof. The lifecycle receipt asserts that
hiding the current Figure selects the remaining visible Figure, hiding both
and `close all` disable export, and creating a visible empty Figure re-enables
it. It does not claim a separate empty-Figure download. A temporary controlled
`toDataURL` failure displayed an error and dispatched zero download anchors;
the original encoder was restored. This is fault injection, not a naturally
occurring browser encoder failure.

The real browser command `print('export-browser','-dpdf')` returned
`MParser:GraphicsRendererUnavailable`; no corresponding session file existed.
Final readback was graphics v4 with explicit host layout. `final-page.png`
(913 x 887, SHA256
`46a4d7cd9f9253059790d66fed4ff4edcb94452bc3decbb082cf6fbad3a4546c`)
was inspected and shows the recreated Figure and the headless `print` error.
It is one desktop view, not another responsive viewport matrix.

Tabbit's API did not expose download events and rejected navigation to its
internal download page. Native Ctrl+J and accessibility observations supplied
the download confirmation; no internal browser database was read. The created
test page was closed, the automation task finished without forced cleanup,
and only the process with the verified executable/start time was stopped.
Browser PNG is still a current-view download, not kernel `print`/`saveas`, a
PDF download route, or production renderer adoption.

## Deployed Loopback Replay

On 2026-11-05, Tabbit used a new temporary session on the original-source
1.15.0/ABI 2.13 deployment at port 8080. The
[adoption record](host-source-adoption.md#local-service-deployment) identifies
the deployed revisions and DLL. The running service was not replaced.

- Clicking a nested definition in an unsaved UTF-8 document selected the
  exact compiler-supplied range. Editing and undoing did not revive the stale
  navigation result. Compile-only left the workspace revision and execution
  count at zero. Evidence: `build/tabbit-deployed-definitions.json`.
- A 3-by-2 matrix produced two Lines with distinct column data and identities.
  Changing one DisplayName updated the kernel Legend label while retaining
  both Line identities. The rendered red and blue Lines were visible at
  1904 x 915 and 390 x 844; no horizontal overflow in the compact case.
  The inspected `build/tabbit-deployed-matrix-page.png` shows the two clipping
  modes, updated Legend and source-located compile error. Pixel sampling
  outside the axes found 227 red-dominant pixels and zero blue-dominant
  pixels (`build/tabbit-deployed-clipping-pixels.json`).
- Selecting the first of two Figures worked. Malformed source reported
  compilation failure. Closing all cleared the graph and disabled the
  selector and PNG export; recreation enabled them. `isvalid` distinguished
  the new Line from the stale one
  (`build/tabbit-deployed-lifecycle-confirm.json`).
- `print` without a renderer explicitly failed with
  `MParser:GraphicsRendererUnavailable`, leaving no PDF in the session.
  **Deployed baseline defect:** the selected first Figure fell back to the
  second when a v3 execution result arrived while the v4 layout view was
  active. The result's v3 graph was identical to the REST readback before
  the failure. This is a selection-continuity problem, not a deleted or
  overwritten kernel Figure. Evidence: `build/tabbit-deployed-lifecycle.json`,
  `selectionRegression`. The source fix was verified in isolation and on the
  updated live assets below.

Only the audit tab and its temporary API session were removed; the service
was not restarted. This run did not perform a new native PNG download.

## Identity Continuity Follow-Up

On 2026-11-06, the frontend fix at
`6995bcae92cfd6fa06b77d22bf9123745de6390f` was verified against the same
ABI 2.13 contract on a new isolated loopback host. The actual HTTP-served
`plot.js` matched the working file by SHA256. The view now uses kernel graph
and object identities across v3 execution and v4 layout snapshots, not the
snapshot-local selector references. A different graph does not reuse the
selection. Real additions, hiding and deletion still update selection;
authoritative Axes limit changes supersede a stale local zoom.

Tabbit selected the first of two Figures, its second Axes and a zoomed range.
Renderer-unavailable `print` returned the same v3 graph and did not disturb
the choice or zoom. A second failed export followed a real Line color
mutation; the Canvas still drew the changed Line (539 magenta pixels),
retained the same view and wrote no PDF. Changed Axes limits, hidden Figure
fallback, close/recreate and old/new handle validity also passed. The
1280 x 800 screenshot was inspected. Evidence:
`build/browser-selection-fix-10d7d4cbbe804485a2b18ff950083c26/replay.json`
and `verified-selection.png` in the same directory. The isolated session,
tab and server were cleaned up. This run verifies the source fix independently
of deployment.

## Live Asset Replay

The [single-asset update](host-source-adoption.md#local-service-deployment)
served the verified `plot.js` on port 8080 without restarting the backend.
On 2026-11-06 a fresh Tabbit tab and API session replayed the same two-Figure,
two-Axes case at 1280 x 800. The rejected export preserved the selected Figure,
Axes 2 and zoom. A color mutation preceding a second rejected export drew
539 magenta pixels without changing that view or producing a PDF. Changed
limits, hiding, close/recreate, disabled export with no Figure and old/new
handle validity passed. One layout read returned the recoverable HTTP 409
session-busy response; the selection and layout assertions still passed.

The inspected `verified-selection.png`, response `replay.json` and HTTP
asset `verified.json` are in
`build/deployed-client-6995bca-c84efb97b2444c41ba11effbfc920a9a`.
The audit tab and its session were removed; backend PID and uptime remained
continuous. This was not another native PNG download or a production renderer
test.

## Workspace Rename And Source Graph

On 2026-11-06, an isolated host using the verified Windows SDK from unsigned
run 42082588791 served the exact MExecClient `577dea9` assets. Tabbit verified
kernel keyword rejection, duplicate-name refusal and stale-revision recovery.
The valid contextual name `arguments` retained the updated value of 64;
focus moved to its row. A closure renamed in the 390px layout returned the
captured result 14 on a later explicit execution. The renames themselves
preserved the run count and graph. Compact DOM geometry fit the viewport;
compact screenshot capture timed out.

In the same isolated session, four files were opened through the editor.
Including a malformed source made the graph compile fail; excluding it and
explicitly binding the external method owner compiled the three selected
units. Four jumps selected exact ranges across files: two same-named nested
helpers, a declaration, and its separate implementation. The browser
converted the compiler's UTF-8 byte ranges over Unicode/CRLF text to editor
selections. Editing and undoing did not revive stale navigation. The
workspace, graph and run count were unchanged by compile and navigation.

`replay.json`, `multisource.json`, `verified-rename-dialog.png` and
`verified-source-graph.png` are retained under
`build/workspace-rename-browser-a73c3d824ab344d4a062358669094294`. The
scratch tab, session and service were removed. This was not a new 8080
deployment.

## Versioned Live Client

The next 2026-11-06 replay ran on the [updated 8080 entrypoint](host-source-adoption.md#local-service-deployment),
client `577dea9`. Tabbit loaded all 25 script/style URLs from the same client
prefix. The replay confirmed keyword, duplicate and stale revision refusal;
F2 and Enter committed a contextual rename with value 64 and restored focus.
A renamed closure returned 14 on later explicit execution. The renames
themselves did not execute source. The four cross-document definition jumps,
explicit external owner, exclusion of malformed source and edit/undo
invalidation also passed without changing workspace, graph or run count.

The actual Canvas supplied a 715 x 298 content size, 96 CSS pixels per inch
and measured character dimensions. Those values were read back from the
kernel v4 Figure, and the kernel converted Axes Position to pixels using
them. Revision-checked REST writes updated a real Line's DisplayName and
Clipping and the bound title Text. Browser refresh rendered the new Legend
label and title; the sample outside the Axes changed from zero red pixels
to 270 while blue remained zero. Stale revision and closed-object writes
were rejected without graph mutation. `print` still refused PDF explicitly
and wrote no file. An unsupported `LineWidth` fixture was also rejected; it
was not counted as implemented. The writeback screenshot timed out, so the
rendering proof is the actual Canvas pixel sample and view state.

The scratch session was removed without restarting the backend or replacing
the other sessions. Evidence is in
`build/live-client-577dea9-a65effd3c24c4bc58523215dd4f1fda0`, including
`live-browser.json`, `live-multisource.json`, `live-graphics-writeback.json`,
and the inspected rename and source graph screenshots.

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

## Scope Limits

The later [ordered display candidate](host-ordered-display-candidate.md) has
actual default-OFF/ON/OFF command/checkbox and event-order DOM evidence with
the current SDK. Screenshot capture timed out; it is not a visual acceptance
claim. That replay did not modify the deployment.

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
installed-SDK host replay passed 67/67; that historical replay did not change
original host/client sources or services. Neither run establishes download UI
or production adoption.

- Broader lifecycle/selection races beyond the tested close/recreate/switch cases.
- Responsive interactions beyond the five compact-candidate viewports/workflows,
  touch/hardware keyboards and full screen-reader acceptance.
- Source navigation, document freshness and failure paths beyond the bounded
  single-source and selected-open-document cases above.
- A general UI property editor, arbitrary property support and production
  rollout beyond the verified local REST writeback and Canvas refresh.
- Kernel file export remains explicit renderer-unavailable; browser PNG export
  is not a substitute for that contract.
- [Source adoption and exact-revision CI](host-source-adoption.md#github-actions)
  and [signed release verification](release-evidence/v1.15.0-publication/README.md)
  are separate from this browser evidence.
