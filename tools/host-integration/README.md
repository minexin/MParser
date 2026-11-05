# MExec Host Integration Candidates

The explicit-layout renderer/synchronization candidates and their controlled
Canvas, app/API and real SDK/REST probes are described in
[the Canvas layout guide](../../docs/host-canvas-layout-candidate.md).
The clean-host runner now applies twelve client candidates. The Legend-reference
candidate resolves explicit entries through the whole snapshot, including
duplicate/cross-Axes references; it never draws the referenced foreign curve
as an associated-Axes series. Bounded actual v4 browser evidence is recorded
in the guide. The final compact-layout candidate closes the measured 390px
cropping failure and retains live panels/drafts without source execution.
Five viewport and current-SDK Legend workflows, the two new simulated
DOM/Canvas probes and remaining device/deployment gates are documented in
[the compact candidate guide](../../docs/host-compact-layout-candidate.md).
The ordered-events candidate adds an explicit default-OFF checkbox and
consumes real kernel history without grouped-result duplication. See
[the ordered display guide](../../docs/host-ordered-display-candidate.md) for
REST/client/browser evidence and the screenshot/live/deployment boundaries.
The definition-navigation candidate uses the server's explicit compile
capabilities and compiler ranges. See [the navigation guide](../../docs/host-definition-navigation-candidate.md)
for application order, browser checks and remaining boundaries.

The clean-host probe also enables `-GraphicsExportContract`: validate the
headless Figure target and PNG/PDF request policy over real REST, verify the
scene stays unchanged and no files are created or overwritten. This is not
renderer adoption; see [the export contract](../../docs/graphics-export-contract.md).

To exercise the ABI 2.13 renderer callback with real file output separately,
run `python probe-graphics-export-render.py <mparser_c-library> <fresh-evidence-dir> <pdftoppm>`
from this directory. This test-only probe needs Pillow, ReportLab and Poppler;
none is linked into MParser or MExecServer. It supplies explicit offscreen
layout, writes two different Figures with native PNG/vector PDF encoders,
checks rasterized pixels and verifies destination rejection and atomic failure
preservation in a private allowlisted output directory.

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
directory, applies the five patches in order, and runs the existing suite plus
Text, clipping/grid and Legend tests. Git's whitespace-only matching handles
Windows CRLF/LF differences; any substantive source mismatch fails the run.

## Runtime Version Display

The source-copy runners also apply `mexecclient-runtime-identity-candidate.patch`.
It removes the initial hard-coded V1.14 banner and displays the runtime product
version returned by `/info`; the milestone is shown separately. It does not
change the kernel version or infer it from SDK/graphics schema versions.
`test-client-runtime-identity.cjs <client-source-copy>` runs the actual app
bootstrap with development, legacy, missing-version and literal-text controls.
The corrected banner/status also passed a later real Chrome check. See
`docs/host-browser-acceptance.md` for the bounded desktop lifecycle/Clipping
evidence, the confirmed narrow-screen cropping and remaining browser gates.
Production sibling sources remain unchanged.

## Definition Directory Candidate

Apply the graphics/SDK-selection patch first, then
mexecserver-definition-metadata.patch with --unidiff-zero and
--ignore-space-change to the separate host checkout. Configure with
MEXEC_DEFINITION_METADATA=ON and the 1.15.0 development SDK (currently ABI 2.12).

The clean-host probe also applies `mexecserver-ordered-events.patch` and enables
`MEXEC_ORDERED_EVENTS`. This candidate requires ABI revision 9 and defaults OFF
when configured independently. Requests opt in with `include_execution_events`;
`max_execution_events` bounds the returned prefix, not in-flight memory.
`test-ordered-events.cjs` accepts `on`/`off` and a fresh evidence directory.
See `docs/ordered-display-events.md` for current proof and open boundaries.
Use an x64 MSVC environment with PowerShell 7 for the aggregate probe; a run
under Windows PowerShell stopped at unavailable `Get-FileHash` after the REST
probes, and its remaining checks were completed separately on the same binary.

The eighth server candidate, `mexecserver-definition-sources.patch`, adds the
explicit `sources` request form to `/compile` under MEXEC_DEFINITION_METADATA.
It now requires ABI 2.10 and accepts optional explicit `class_method_owner`
per unit; names alone do not confer ownership. The expanded external-method
case and 67 host regressions passed in the clean ABI 2.10 build at
`build/clean-host-probe/3e274527adcc4cb29f4a6d0ac7bb8218`.
The clean-host probe runs `test-definition-sources.cjs` after the ordered-events
probe. Eight-patch build/REST/67-test acceptance passed at
`build/clean-host-probe/bc0e56b4c35e484d851b69ef48e1056a` using PowerShell 7.
See `docs/definition-metadata.md` for source binding and navigation boundaries.
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

The runner includes `test-axes-layout.cjs` for actual kernel normalized Axes
positions, identity-targeted InnerPosition writes, later-command readback,
replacement preservation and explicit host layout-environment injection.
`MEXEC_GRAPHICS_LAYOUT_ENVIRONMENT` defaults OFF and requires ABI 2.12. Its
separate graphics-layout route explicitly returns v4 with environment/revision;
legacy graphics/execution routes retain v3. The probe checks real source unit
conversion, invalid/stale requests and one concurrent commit, not browser pixels.
`probe-axes-layout-compat.ps1` verifies ABI 2.11 OFF, bounded old-header/new-DLL
layout-OFF and legacy-writeback probes, and rejects old SDK layout ON. Its
finally block restores the new SDK/ON before the final probe and host suite.

The source-copy runner enables `MEXEC_GRAPHICS_WRITEBACK` with the ABI 2.12 SDK
and explicit graphics v3. `test-graphics-writeback.cjs` checks real property
writes/readback, lossless revision strings, stale/deleted/reset/foreign targets,
invalid-write preservation and exactly one winner for concurrent requests at
one revision, without incrementing run_count. This does not implement UI editing.
The compatibility probe verifies the OFF route/capability boundary with the
older SDK, a bounded old-header/new-DLL replay and the required old-SDK ON
configuration failure, then restores ON and runs host regression. See
`docs/graphics-writeback-contract.md` for exact evidence and limits.

The runner also includes `test-legend-state.cjs`: 14 R2024b-backed membership
and label phases plus independent later-command String reads and invalid-write
preservation. It compares real kernel entries and Line DisplayName values,
without host-side membership/label reconstruction. Full layout and production
adoption remain separate.
This follow-up passed clean source-only build/REST/client checks and 67 host
regressions at `build/clean-host-probe/3c666eb6ad85436895207643b88a4f13`, with
the selected DLL recorded in `docs/graphics-legend-contract.md`.

The clean runner includes `test-graphics-colon.cjs`: full ascending/descending,
single and nondividing XData arrays are compared to R2024b, then retained handles
are read in later commands. It also checks source-file execution, 2412 nested
iterations with the final value 10, and a floating zero-step empty row. This is
numeric/REST evidence, not a host-added endpoint or pixel/rendering acceptance.
Final acceptance passed 148 clean build actions, all dedicated probes and
67/67 regressions at
`build/clean-host-probe/d8deab22f170429482bc2a2218a77f3c` (19.88 seconds).
The selected installed DLL hash and exact numeric scope are in
`docs/graphics-colon-contract.md`; no original host/client sources were changed.

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
libraries from source. Graphics v3, definition metadata, conditional rename and
authoritative kernel path synchronization
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

Earlier complete clean installed-SDK run:
`build/clean-host-probe/104cdbe5a9af4f3b8e320e834cc421db` (148 build actions,
all five host candidates, rename, contextual/diagnostic and graphics/definition
REST probes, client candidate checks and host CTest 67/67 in 21.50 seconds).
The loaded DLL SHA256 was
`09C5D9D24E7C24B77F7D107F925664909A14F43A387EA397867B0DFB7E223F5D`;
raw REST evidence is `build/host-compile-probe/c0c09bb20e1b4b03a916cc380eda5150`.

## Color Candidate

Apply mexecclient-color-candidate.patch after the Legend candidate. It consumes
kernel-normalized RGB/RGBA/none: transparent Line colors are used by both the
Line and Legend sample; none suppresses strokes but preserves Legend labels.
Bound Text labels render their Color and skip drawing for none without losing
their String. The client does not parse names/hex or scale numeric types.

test-client-colors.cjs checks both v2/v3 snapshots, mutation/restoration and
unchanged inputs with a simulated canvas. The CTest real-kernel scene now also
contains an RGBA Line, a none Line and a green Text title. These tests are not
pixel-level browser acceptance. All replay runners apply this fifth candidate;
the deployed renderer remains unchanged.

probe-host-compile.ps1 -GraphicsColors replays 14 focused Line/Text mutations
through the selected executable/DLL, checking canonical getters and snapshots,
integer scaling, RGBA readback and unchanged graphs after invalid inputs. It
requires the updated development kernel. The clean-host runner enables it.

Color baseline clean installed-SDK run with five client candidates:
`build/clean-host-probe/9d7d0dee19914d4bbdd7ca0e103693cc` (148 build actions,
all REST/candidate probes and host CTest 67/67). The loaded DLL SHA256 was
`D8EE7C21B46138B10EBFA0EC3358C63173854D90FF86472019DFB31E9C616DBA`;
raw REST evidence is `build/host-compile-probe/3249318bbaec4e398dd3418579ebcdb0`.
These tests leave the deployed client/service unchanged and do not prove
real-browser compositing or product-release acceptance.

## Authoritative System Paths Candidate

Apply mexecserver-system-path-sync.patch after the other five server patches.
MEXEC_SYSTEM_PATH_SYNC defaults OFF; ON requires SDK ABI generation 2 revision 8
and advertises kernel-search-path-sync. Existing command/source_path compilation
reads SystemContext::searchPaths() for each request under the session lock.
Checked source-directory/cwd entries precede the actual ordered path list.
Source addpath/rmpath changes therefore reach later compilation even after a
script fails. Existing compiled source graphs retain their original bindings.

GET session/path maps the kernel list to virtual paths, including its real
ordering/emptiness. It does not unconditionally invent a root entry. REST add
keeps its existing append behavior; source addpath has kernel prepend/end
behavior. REST updates call setSearchPaths instead of recreating Runtime or
SystemContext, so handles, globals, file state and random streams are retained.
Session snapshots use this same list. The old path_ behavior remains only in
the disabled configuration. No path-query source or source-text parser is used.

test-system-path-sync.cjs runs an isolated selected server, stores raw JSON and
checks next-command/file resolution, shadow order, remove/end/clear, cwd, root
and failure rules, GET side-effect checks, reset/isolation, closure/graphics and
RNG continuity. The clean-host runner builds with the option ON and runs it
before all other existing REST/candidate/67-host-test checks. The self-contained
in-memory source endpoint is not widened to untrusted filesystem loading;
cross-file in-memory navigation and same-compiled-script dynamic resolution
remain separate work, as documented in system-search-paths.md.

Clean installed-SDK path acceptance passed at
`build/clean-host-probe/9c2059d6af5946f2aa1931cb27dcd001`: all path/rename/kernel
REST/client-candidate checks and host CTest 67/67 (20.04 seconds), with host
warnings-as-errors enabled. Raw path JSON is in that run's `path-probe`;
other kernel REST evidence is `build/host-compile-probe/f85e180cb48840029f0c18b65dde6cab`.
The selected ABI 2.8 DLL SHA256 is
`8FE54E9108F05BFC3B2B38A9AF73C1C3827CD289FF54356EF9608A07A455E269`.
The disabled option was rebuilt and passed 67/67 (20.74 seconds) separately;
the original diagnostic probe retained the old next-command failure and host
path positive control (`build/path-host-off-probe.log`). This is a compatibility
control, not a claim that the disabled host now synchronizes paths.
