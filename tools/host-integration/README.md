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
With GraphicsV3 enabled it also checks partial Line deletion/reference
renumbering, mutation stability, rejected deleted-handle writes, interleaved
session isolation and fresh graph identity on session reset. It does not claim
to verify an identity-targeted REST mutation API or concurrent request races.

The isolated v3 binary was compiled from current MExecServer sources plus the
two patched serialization call sites, linked with existing third-party Oat++
libraries and the development MParser import library. This proves the selected
binary's REST behavior, not a clean downstream CMake build or deployment.
The later clean-build procedure below supersedes that limitation when it passes;
the earlier evidence remains a record of the original isolated test.

Earlier paired local evidence:
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

Add `-ContextualIdentifiers` when using the updated 1.15.0 development kernel.
It replays 36 non-class source cases from the shared R2024b contextual-name
corpus through fresh REST sessions: 34 expected executions and two expected
compile rejections with no execution/workspace effects. This switch is explicit
so an older selected DLL is not accidentally treated as the updated kernel.
The clean-host runner enables it alongside the existing feature probes.

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
libraries from source. Graphics v3, definition metadata and conditional rename
are enabled with warnings-as-errors.
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

Earlier verified clean run: build/clean-host-probe/ddb1df19182d40db93f346e910a252c2
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

## Conditional Workspace Rename Candidate

Apply `mexecserver-workspace-rename.patch` after the graphics, definition and
development-acceptance server patches above,
using `git apply --unidiff-zero --ignore-space-change`. Configure
`MEXEC_WORKSPACE_RENAME=ON` to advertise `conditional-workspace-rename` in `/info`
and enable `POST /sessions/{id}/workspace/{name}/rename`. The option defaults to
OFF; no unconditional rename API is added. The existing conditional-delete
regression now checks the configured rename capability in either mode.

The JSON body requires `new_name` (string) and `expected_revision` (unsigned
decimal string). The session checks the revision, calls SDK
`validateVariableName`, finds the exact case-sensitive source binding and
rejects destination collisions under one lock. A successful rename changes
only `NamedValue.name` and increments the revision exactly once. Renaming an
existing name to itself is a no-op with an unchanged revision. No SDK Value is
copied/reconstructed, no source is replaced and no script is executed.

Responses: success includes `renamed` and the revision captured under the lock;
stale revision is HTTP 409 `workspace_conflict`, an occupied destination is
409 `variable_name_conflict`, and a missing source is 404 `variable_not_found`.
Kernel name rejection is 400 `invalid_variable_name`, including the public
`name_status` enum value and kernel-reported `max_length`. Malformed JSON,
missing/wrong-type fields and malformed/overflowing revisions return 400
without mutation. Stale revisions take precedence over name validation and
binding lookup; clients must refresh instead of guessing the current state.

The clean-host script runs `test-workspace-rename.cjs` with the built server.
It retains raw request/response JSON and checks kernel validation reasons,
63/64-byte boundaries, embedded NUL/non-ASCII, contextual and builtin names,
case sensitivity, collisions, missing source, no-op, no execution/source change,
numeric values, captured function handles, graphics aliases, dynamic objects
and descriptors, rename/rename and rename/PUT races, and stale requests after
reset. Contextual-name acceptance here concerns binding validation, not a
claim that every parser grammar position accepts those names.

For a host configured with the option OFF, run the same Node test with a new
empty evidence directory and the final argument `off`; it checks the missing
capability/route and unchanged workspace. The ON probe uses `on`. The tests
stop their own isolated server and never update the deployed host or browser.

Earlier complete one-command run:
`build/clean-host-probe/752a6a87a7984b61b469ccd1c1f6da32` (148 clean build
actions, rename/REST/client candidates and host CTest 67/67). The selected
installed 1.15.0 DLL SHA256 was
`503D91D4AADE837C7A5E9CCDF995D8BD4C703E0382A8466AFF21250F6EF29E8B`.
The separately configured OFF-mode probe and conditional-delete test passed
at `build/clean-host-probe/070e28f719224c6e8b4ba13704ee3ec0/rename-probe-off`.
This establishes focused server-side adoption, not a deployed rename UI.

## Compile Diagnostic Propagation

Apply `mexecserver-compile-diagnostics.patch` after the other server candidates.
The host must check `moduleToUse.isValid()` before calling the shared Runtime:
the SDK rejects execution of an invalid module with an API status, not a runtime
Result. Without this check, a normal compile failure was mislabeled as
`MExec:ExecutionBridgeError` and its source range was lost. With the check, the
host returns the module's original diagnostics and `compilation-failed` status
before `clear_workspace`, input/debug setup or execution. The existing conservative
request revision/run-count accounting is unchanged; no claim of unchanged
revision is made for failed execute requests.

`probe-host-compile.ps1 -CompileDiagnostics` seeds a numeric value, captured
closure and graphics handle, then submits missing-RHS and reserved-LHS errors
through source, command and source_path requests, each with clear_workspace=true.
All six cases must preserve workspace values and the last graphics snapshot,
emit the correct compiler identifier/source/line/column with null execution and
no output events, and permit the existing closure and graphics handle to work
afterwards. These are separate checks from compile_only; valid-but-runtime-failing
scripts retain the existing runtime behavior. The clean-host runner enables
this probe. Original deployed host sources remain unchanged.

Latest complete clean installed-SDK run:
`build/clean-host-probe/104cdbe5a9af4f3b8e320e834cc421db` (148 build actions,
all five host candidates, rename, contextual/diagnostic and graphics/definition
REST probes, client candidate checks and host CTest 67/67 in 21.50 seconds).
The loaded DLL SHA256 was
`09C5D9D24E7C24B77F7D107F925664909A14F43A387EA397867B0DFB7E223F5D`;
raw REST evidence is `build/host-compile-probe/c0c09bb20e1b4b03a916cc380eda5150`.
