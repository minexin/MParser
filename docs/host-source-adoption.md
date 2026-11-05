# Original Host Source Adoption

On 2026-11-05 the user authorized development directly in
`D:/project/MExecServer` and `D:/project/MExecClient`, including installation of
the development SDK. Later in the same session the user authorized local
service deployment and replaced the initial local-only patch policy with two
independent private GitHub repositories. Local edits/tests/commits followed by
push to each `origin/main` are now the authoritative host workflow. No new
MParser feature branch was created.

| Local Source | Private Repository | Verified Remote Main |
| --- | --- | --- |
| `D:/project/MExecServer` | [minexin/MExecServer](https://github.com/minexin/MExecServer) | `00682e1350eb4caa02d756a85bfb3d7254610707` |
| `D:/project/MExecClient` | [minexin/MExecClient](https://github.com/minexin/MExecClient) | `6dda80db39ff94f26a0608d5c08dae95b79c6ab0` |

Both repositories and their default `main` branches were verified directly
through GitHub's authenticated API. They are separate from MParser's tree and
require the owner's authorized login to view. No full cross-repository Actions
pipeline is claimed for these first host imports; the tests below ran locally.

## Adopted Development State

The eleven server and twelve client candidates have been applied to the
original source directories. They connect identifier validation, compiler
definitions/source ownership, graphics identity and layout, revision-checked
writes, authoritative search paths and opt-in ordered events. The frontend
also adopts the verified Text/Legend/Color/Clipping, compact-layout and
definition-navigation consumers. UI event collection remains default OFF.

The server has an independent `windows-msvc-development` configure/build/test
preset. It selects `build/mparser-v1.15-development-package` and explicitly
enables the new host capabilities; generic legacy configurations retain their
explicit opt-in switches. SDK identity is 1.15.0 / ABI 2.13 / builtin 1.23.
Host/client product versions remain 0.1.0 and the connected kernel identity is
displayed separately. The test-only loopback encoder/export bridge is NOT in
the adopted server. `print`/`saveas` remain explicitly headless by default.
Existing V1.14 packages/builds and workspace files were not replaced.

Eighteen integration regressions now belong to the host's own CTest suite:
seven REST/SDK workflows plus eleven client checks. Tests use their own
loopback child processes and unique evidence roots, not a deployed service.

## Verified Evidence

- Kernel full rebuild/regression: 364/364 in 99.23 seconds,
  `build/host-adoption-20261105/kernel-full-regression.log`.
- Original-source host: 148 build actions and 85/85 in 23.61 seconds,
  `build/host-adoption-20261105/adopted-host-build-test-corrected.log`.
- A fresh source-only copy of the adopted originals, without reapplying the
  legacy candidates: dedicated REST/client probes and 85/85 in 24.98 seconds,
  `build/clean-host-probe/2bfecdd13c9244248e3ffd4168f23cab` and
  `build/host-adoption-20261105/integrated-source-copy-regression.log`.
- All 27 copied frontend assets match the original client byte-for-byte.
  The development build's DLL matches the installed SDK SHA256:
  `C749ACFC0D0FD5128F2554D211A88E587FF2CDA72EA000877287ED98AE11D0F0`.

The first original-source configure correctly rejected a remaining 1.14.0
requirement. Inspection found that Git had skipped candidate paths inside a
parent working tree. It was not counted as successful adoption. Corrected
path isolation, actual implementation checks and per-file hashes precede the
passing run above. Before files and final normalized/raw hashes are retained
in `build/host-adoption-20261105`; this also records checkout newline changes.
The adopted preset and source-copy tests are not a new full browser acceptance
run. Earlier browser evidence remains scoped to the DLL/source-copy pair
recorded on its own page.

## Local Service Deployment

After deployment authorization, the tested original-source build was copied
to a new versioned deployment directory. The managed background process serves
`http://127.0.0.1:8080`, keeps the existing host workspace and records its
exact executable, PID, start time, loaded DLL/hash and source build in the
host's `build/development-service.json`. It is a loopback local process, not
an installed Windows SCM or automatic-boot service. Older deployment/build
directories are retained. Host tools provide explicit start/stop operations
with executable/start-time ownership checks.

`build/host-adoption-20261105/deployed-service-evidence/verified.json` records
actual HTTP readback hashes for all 27 frontend resources and REST acceptance
for invalid-assignment diagnostics, compile-only definition metadata,
matrix/Legend graphics, opt-in ordered events and explicit headless export
failure without a fake PDF. The temporary verification session was removed
through the API; no existing user script was executed. Service deployment is
not publication of an MParser release.

## Repository And Patch Policy

A `.patch` is a reviewable source diff, not an installer, deployed binary or
runtime hot patch. Historical candidate patches remain here as the original
integration evidence. Their content has already been adopted; do not apply
them to current host sources a second time. The extra initial-adoption diffs
are preserved only in the local backup directory. New host implementation
changes belong to their private repositories, not new public MParser patches.
MParser retains its SDK contracts, integration tests and verified repository
revision references. This respects the later private-repository decision.

For regression of the already-adopted originals, use
`probe-clean-host.ps1 -IntegratedSources` with the original roots and installed
SDK prefix. This copies current code without reapplying historical patches.
Later changes are made directly in the two host Git working directories,
tested, committed and pushed to their respective private `origin/main`.
