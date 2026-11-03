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
The patch also selects product/SDK 1.15.0 explicitly, so it cannot silently
link the v1.14 baseline package.

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
The later clean-build procedure below supersedes that limitation when it passes;
the earlier evidence remains a record of the original isolated test.

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

## Definition Directory Candidate

Apply the graphics/SDK-selection patch first, then
mexecserver-definition-metadata.patch with --unidiff-zero and
--ignore-space-change to the separate host checkout. Configure with
MEXEC_DEFINITION_METADATA=ON and the 1.15.0 development SDK (ABI 2.7).
Graphics v3 may remain OFF if only definition metadata is needed.
The default remains OFF. This patch can be combined with the graphics patch;
they modify separate compile definitions and REST/serialization call sites.

The /compile response adds definition_directory: version, completeness, UTF-8
byte units, source name, exact submitted source text and compiler definition
records. Invalid source produces an incomplete empty directory. Source echo
lets a host bind ranges to the exact request text, but the client still must
check document identity and request freshness before navigating.

Run probe-host-compile.ps1 with -DefinitionMetadata for nested repeated names,
CRLF/UTF-8 position checks, class methods, compile-without-execution and invalid
source. Use -GraphicsV3 as well only if that server also enables graphics v3.
An isolated binary with both options passed those REST checks; raw evidence is
build/host-compile-probe/56610d516503487ba260eca175f3c484. It reused previously
compiled unchanged host objects and third-party libraries, so this is not
evidence of a clean downstream CMake build or deployed navigation support.

## Clean Installed-SDK Acceptance

Run probe-clean-host.ps1 from an x64 MSVC developer environment with CMake,
Ninja, PowerShell 7, Node.js, Git and robocopy available:

```powershell
./tools/host-integration/probe-clean-host.ps1 `
  -ServerRoot D:/project/MExecServer `
  -ClientRoot D:/project/MExecClient `
  -SdkPrefix <installed-development-SDK>
```

The script copies source directories only into a unique repository build
directory, excludes compiled artifacts, and builds the host and third-party
libraries from source. Both new features are enabled with warnings-as-errors.
The installed SDK supplies headers, import library and DLL; no host objects
or libraries are reused from an existing server build. The original host,
client and deployed service remain unchanged.

The third host patch, mexecserver-development-acceptance.patch, supplies the
configured SDK's exact ABI generation/revision to the REST regression (instead
of hard-coded 2.5), and declares graphics v3 in /info only when enabled. It does
not accept arbitrary newer revisions at runtime. The REST probe verifies the
v3 capability declaration before executing graphics requests.

Configuration/build/CTest output and REST probe results are retained under
build/clean-host-probe. Any build, probe or regression failure stops the script;
passing does not establish real-browser rendering or production deployment.

Verified clean run: build/clean-host-probe/ddb1df19182d40db93f346e910a252c2
(148 build actions, REST and candidate checks, host CTest 67/67). The installed
DLL SHA256 was 655A551409F5860B48CBC9C693DE5AF34BA4778825C663C00AFA3A927CF99F76.

## Repository Regression

`node tests/host_graphics_candidate_smoke.cjs` replays the renderer candidates
against the pinned, test-only host snapshots in tests/host_graphics_fixture.
It does not require the sibling MExecClient checkout and runs in a separate
GitHub CI job. The original live-source replay remains available to detect
changes in the actual integration target.

CTest adds host_graphics_candidate_smoke when Node.js and Git are found.
Configure MPARSER_REQUIRE_HOST_GRAPHICS_TESTS=ON to reject missing tools
instead of omitting the test; Node.js 18+ is required. The CTest variant also
executes tests/host_graphics_scene.m with the built MParser and verifies the
real v2 snapshot's matrix data, labels, grids, clipping and Legend in the
candidate renderer. The standalone CI job covers patch replay only, while
normal platform CTest runs cover the real kernel scene when tools are present.
This remains simulated-canvas, not real-browser, acceptance.
