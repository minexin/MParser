# MExecServer Graphics V3 Integration

This patch is an opt-in integration candidate, not a deployed server update.
Apply it to a separate MExecServer source checkout after checking the diff:

```powershell
git apply --check D:/project/MParser/tools/host-integration/mexecserver-graphics-v3.patch
git apply D:/project/MParser/tools/host-integration/mexecserver-graphics-v3.patch
```

Configure against the development MParser SDK using MParser_DIR and set
MEXEC_GRAPHICS_V3=ON. The default remains OFF. ON requires ABI generation 2,
revision 7 or later and the installed v3 schema. Both execution-result and
workspace-value serialization select v3 together.

Do not enable this server for the current v1-only browser client. This build
switch is not per-request protocol negotiation and does not implement
identity-targeted property writeback.

## Verification

Run the repository probe with the explicitly selected executable and DLL:

```powershell
./tools/probe-host-compile.ps1 -ServerExe <server-executable> -KernelDll <kernel-dll> -GraphicsV3
```

Omit GraphicsV3 for the legacy server. The probe starts an isolated loopback
session, verifies the loaded DLL path/hash, retains raw REST JSON under build,
and stops its own server. It checks matrix plotting, Text/Legend transport,
close/recreation, result/workspace identity agreement and export failures
without file creation or overwriting an existing file.

The isolated v3 binary was compiled from current MExecServer sources plus the
two patched serialization call sites, linked with existing third-party Oat++
libraries and the development MParser import library. This proves the selected
binary's REST behavior, not a clean downstream CMake build or deployment.

Latest paired local evidence:
- Legacy: build/host-compile-probe/9fbc4681980346279ee78908f117306d
- V3: build/host-compile-probe/e289ee0fbfab40c4b63e37af37ae95f9

Those build directories are local evidence, not committed release artifacts.

## Client Rendering Candidates

mexecclient-text-labels-candidate.patch is an isolated, incomplete renderer
candidate. Apply with git apply --unidiff-zero in a separate client checkout.
Run node tools/host-integration/test-client-text-labels.cjs with that checkout
path from the MParser root. The existing simulated-DOM suite and additional
v2/v3 Text mutation, visibility and parent-binding assertions passed locally.

Do not deploy these candidates yet: accepting v2/v3 is not full support.
Persistent UI selection, protocol negotiation and real browser visual acceptance
still need implementation/verification.
The patch leaves conservative full UI reset on changed snapshots in place.

Apply mexecclient-grid-candidate.patch with --unidiff-zero after the clipping
patch. The clipping test also checks independent XGrid/YGrid, both-on stroke
counts, and toggling off. Only major grids are represented; no minor-grid
support is implied.

Apply mexecclient-line-clipping-candidate.patch after the Text candidate.
Run test-client-clipping.cjs with the isolated client path. This verifies mixed
Line clipping, default clipping, hidden strokes, balanced canvas save/restore
and unchanged input data using a simulated canvas. It is not pixel-level or
browser acceptance. Both candidate patches leave the deployed client untouched.

Apply mexecclient-legend-candidate.patch last, with --unidiff-zero. It renders
only explicit Legend entries, preserves their order and Line styles, and omits
hidden or foreign-Axes legends and dangling references. Horizontal wrapping and
bounded outside-label width are covered by simulated-canvas assertions. The
best/bestoutside locations use a fixed placement heuristic, not MATLAB's
overlap-minimizing algorithm. Dense legends may be clipped vertically; this is
not complete MATLAB layout parity.

To reproduce all candidate checks without changing the original client:

```powershell
./tools/host-integration/probe-client-rendering.ps1 -ClientRoot D:/project/MExecClient
```

The script copies only the renderer and existing test to a unique build
directory, applies the four patches in order, and runs the existing suite plus
Text, clipping/grid and Legend tests. Git's whitespace-only matching handles
Windows CRLF/LF differences; any substantive source mismatch fails the run.
