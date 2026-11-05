# Compact Host Layout Candidate

Status: source-copy integration candidate, 2026-11-05. Product/SDK 1.15.0
remains development, C ABI 2.12/158 exports, builtin contract 1.22. No kernel,
public API or graphics schema change is introduced by this candidate.

## Scope And Replay

Apply `mexecclient-compact-layout-candidate.patch` after the preceding nine
client candidates in `tools/host-integration`. The clean-host, client-rendering
and frozen-fixture runners now apply all ten in order. Original MExecClient
and MExecServer sources and deployed services are unchanged.

The former desktop grid had a 650px minimum width. Compact mode instead uses
the existing live panels in one column at `(max-width: 720px)` or
`(max-height: 500px) and (max-width: 1000px)`. Workbench shows the editor/plot
and command panels together in portrait; short screens expose commands on
their own tab. Files, workspace and commands can each use the available area.
No editor DOM, drafts, workspace state or pending command is reconstructed.

The panel tabs support roving arrows/Home/End with IME/modifier guards. Hidden
panels are inert; focus moves out of a concealed pane to its selected tab. Compact
tabpanel labels are assigned and original desktop roles/labels restored.
Viewport changes do not overwrite saved desktop proportions. Entering compact
mode clears transient desktop maximization; returning to desktop restores the
ordinary four-panel layout, not that transient maximized state.

Menus, directory controls and plot selectors fit the compact layout; the ribbon
keeps its own internal horizontal scrolling. Tick density depends on actual
Axes pixel size. Explicit v4 Legend entries wrap within the associated Axes
width rather than spanning its neighbor. This is host presentation only:
kernel Position/Units, ranges, Line data, member order and labels are unchanged.
This is not MATLAB's full text/inset/Legend layout solver. Arbitrarily long
labels or too many entries for a tiny Axes are not certified by these cases.

`test-client-compact-layout.cjs` checks panel selection/reveal, short mode,
inert/focus, keyboard/IME, desktop role restoration, maximization/reset, live
DOM/drafts and saved proportions without kernel API calls. The companion
Canvas test checks ticks, Axes-local wrapping, repeated/foreign references,
DPR and immutable inputs. These Node tests use simulated DOM/Canvas, not pixels.

## Actual Browser Evidence

Tabbit used the current Legend-creation SDK DLL, SHA256
`9689A754300DF313D828F7FD9692C6D9447426AC05D5CFC2EF85AAD1A67AE298`,
with an isolated source-copy host. Raw observations and inspected PNGs are in
`build/browser-compact-20261105`; the browser web root was
`build/compact-client-20261105/web`. The following is bounded acceptance, not
production adoption or every mobile/browser configuration.

| Viewport | Document Width | Canvas CSS Size | Compact |
| --- | --- | --- | --- |
| 320 x 800 | 320 | 314 x 214.39 | Yes |
| 390 x 844 | 390 | 384 x 243.88 | Yes |
| 768 x 1024 | 768 | 403.92 x 415.75 | No |
| 844 x 390 | 844 | 838 x 84 | Yes |
| 1904 x 915 | 1904 | 1071 x 374.72 | No |

`viewport-matrix.json` checks document/control/Canvas bounds and nonzero
red/green/blue Canvas pixels for every row. The actual 390px page screenshot
was inspected: plot controls wrap, commands remain reachable and the four
Legend labels fit inside the first of two Axes. Final `final-views.json` and
portrait/landscape Canvas PNGs check the last role/short-Legend changes at
390 x 844 and 844 x 390, including desktop role/viewport restoration. Canvas
PNG encoding is not proof of the browser's download-button workflow. A native
full-page landscape screenshot timed out; it is not counted as a screenshot
pass. The native landscape Canvas PNG was inspected separately.

The actual Legend scene used ordered `[foreign, blue, red, foreign]` members
with explicit labels and AutoUpdate off. The repeated foreign Line displayed
its final kernel name in both positions, with green keys. Native command
interactions then proved both entries rename together, a new Line does not
join with AutoUpdate off, deleting the foreign Line removes both references,
and an invalid Location preserves the existing displayed Legend with a visible
runtime error. `workflows.json` retains the responses. Workspace filtering,
files/workbench switching, keyboard navigation, pending-command/draft retention
and landscape command-to-plot navigation passed. Pure view/filter/draft actions
left execution run_count at 1. One concurrent layout GET returned the expected
busy 409 during execution; synchronization recovered and final state passed.

The final page had no dirty editor and returned to 1904 x 915. Tabbit finished
with `forcedCleanup=false`; the exact owned isolated server was stopped. No
source commands were replayed merely because a screenshot timed out.

## Installed-SDK And Remaining Gates

Fresh source-only host replay:
`build/clean-host-probe/5be4563d5be948e28ce08e9c0046df10`, aggregate log
`build/compact-clean-host.log`: 148 build actions, all REST probes including
80 Legend creation cases, all ten client candidates and host CTest 67/67
(20.26 seconds). The loaded DLL matches the browser hash above.

Complete rebuilt kernel/package regression passed 362/362 in 85.96 seconds at
`build/compact-full-test.log`. Final original-source client replay passed at
`build/client-rendering-probe/3d6109f2c914472f9b22ad1f214f4742`, including both
new compact probes. These are separate from actual-commit GitHub CI.

The historical 390px failure in the earlier
[Canvas run](host-canvas-layout-candidate.md) is closed for this latest tested
candidate, not retroactively for its old source/SDK. Touch/hardware keyboards,
screen readers, all browser/device sizes, long-label/full Legend layout,
broader lifecycle races, navigation/property editing, download UI completion,
production deployment and release acceptance remain separate gates.
