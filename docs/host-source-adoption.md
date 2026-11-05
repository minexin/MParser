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
| `D:/project/MExecServer` | [minexin/MExecServer](https://github.com/minexin/MExecServer) | `5481e691d14e0b76379d1eead95242ca3c91f308` |
| `D:/project/MExecClient` | [minexin/MExecClient](https://github.com/minexin/MExecClient) | `6995bcae92cfd6fa06b77d22bf9123745de6390f` |

Both repositories and their default `main` branches were verified directly
through GitHub's authenticated API. They are separate from MParser's tree and
require an authorized login to view. These are the verified CI revisions;
the existing local deployment is a separately identified snapshot below.

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
  The subsequent 2026-11-06 full local replay also passed 364/364, including
  installed C/C++ consumers and the relocated Windows archive:
  `build/mparser-115-host-sync-local.log`.
- Original-source host: 148 build actions and 85/85 in 23.61 seconds,
  `build/host-adoption-20261105/adopted-host-build-test-corrected.log`.
- A fresh source-only copy of the adopted originals, without reapplying the
  legacy candidates: dedicated REST/client probes and 85/85 in 24.98 seconds,
  `build/clean-host-probe/2bfecdd13c9244248e3ffd4168f23cab` and
  `build/host-adoption-20261105/integrated-source-copy-regression.log`.
- All 27 copied frontend assets match the original client byte-for-byte.
  The development build's DLL matches the installed SDK SHA256:
  `C749ACFC0D0FD5128F2554D211A88E587FF2CDA72EA000877287ED98AE11D0F0`.
- A later isolated source-only replay used the downloaded Windows x64 SDK
  from [unsigned run 42082588791](https://github.com/minexin/MParser/actions/runs/42082588791),
  source `7fb29e7`, and the exact host/client revisions above. All 85 CTests
  and dedicated REST/SDK probes passed. The host DLL matched that archive's
  SHA256 `CF0EEFD4AE481ECD76C47F33291F7A05BEF11A2E54BB6EA428B6741006041C60`.
  See `build/unsigned-v115-host-replay-42082588791.log`. This archive was not
  installed into the running service.

The first original-source configure correctly rejected a remaining 1.14.0
requirement. Inspection found that Git had skipped candidate paths inside a
parent working tree. It was not counted as successful adoption. Corrected
path isolation, actual implementation checks and per-file hashes precede the
passing run above. Before files and final normalized/raw hashes are retained
in `build/host-adoption-20261105`; this also records checkout newline changes.
The adopted preset and source-copy tests are not a new full browser acceptance
run. Earlier browser evidence remains scoped to the DLL/source-copy pair
recorded on its own page.

## GitHub Actions

- [Frontend run 42075178497](https://github.com/minexin/MExecClient/actions/runs/42075178497):
  all four Windows/Linux and Node 22/24 jobs passed on the client revision
  above. Each artifact reports 24 module syntax checks, 51 unit tests and 11
  integration tests. These are automated DOM/Canvas replays, not Tabbit.
- [Backend run 42075623084](https://github.com/minexin/MExecServer/actions/runs/42075623084):
  Windows x64, CMake 4.4.5, Node 24.22.2. The configured read credential
  checked out the private frontend revision above. The workflow installed
  the SDK built from MParser `a541767682788bb68bc6deb95b5ddd9c246da85b` and
  passed 85/85 host/client CTests without failures, skips or disabled cases.
  The downloaded report, JUnit and CTest log agree on the clean checkout,
  SDK 1.15.0/ABI 2.13 and test inventory; see
  `build/private-host-ci-verified-42075623084`. This includes the actual
  PowerShell pipeline, exact UTF-8 inputs with/without BOM, stdin error
  location, the 31-case behavior probe with kernel `rand`/`rng`, and the
  v3/v4 view continuity regression.
- The pinned kernel revision passed
  [Documentation CI 42073260611](https://github.com/minexin/MParser/actions/runs/42073260611).
  Its unchanged kernel sources also passed
  [Cross-platform CI 42026807929](https://github.com/minexin/MParser/actions/runs/42026807929)
  at `8af9e74151766a13b831ddf5d86a6c3dddff1869`. Later documentation-only
  commits do not change the pinned kernel code.

## Local Service Deployment

After deployment authorization, the tested original-source build was copied
to a new versioned deployment directory. The managed background process serves
`http://127.0.0.1:8080`, keeps the existing host workspace and records its
exact executable, PID, start time, loaded DLL/hash and source build in the
host's `build/development-service.json`. It is a loopback local process, not
an installed Windows SCM or automatic-boot service. Older deployment/build
directories are retained. Host tools provide explicit start/stop operations
with executable/start-time ownership checks.

The running backend remains the initial private import
`00682e1350eb4caa02d756a85bfb3d7254610707`. The CI/CLI follow-up does not
change the served HTTP implementation. The initial frontend import was
`6dda80db39ff94f26a0608d5c08dae95b79c6ab0`; on 2026-11-06 its only changed
managed asset, `js/plot.js`, was atomically replaced with the tested client
`6995bcae92cfd6fa06b77d22bf9123745de6390f`. All 27 managed assets matched
that checkout by HTTP SHA256; 16 additional files were not modified or
included in the match. The old asset and verified replacement receipt are
retained in `build/deployed-client-6995bca-c84efb97b2444c41ba11effbfc920a9a`.
Process identity, uptime, loaded DLL and a temporary session's workspace and
graph survived the update. No backend restart or renderer install occurred.

`build/host-adoption-20261105/deployed-service-evidence/verified.json` records
actual HTTP readback hashes for all 27 frontend resources and REST acceptance
for invalid-assignment diagnostics, compile-only definition metadata,
matrix/Legend graphics, opt-in ordered events and explicit headless export
failure without a fake PDF. The temporary verification session was removed
through the API; no existing user script was executed. Service deployment is
not publication of an MParser release.

The later deployed REST checks verified failed compile/execute preservation,
kernel reason codes on rename, and compile-only function ranges. Actual
Tabbit replay on this same service verified definition navigation and
bounded graphics cases, and found the v4/v3 selection issue fixed in the
[isolated follow-up](host-browser-acceptance.md#identity-continuity-follow-up)
and then verified on the [updated live assets](host-browser-acceptance.md#live-asset-replay).

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
`tools/host-integration/probe-clean-host.ps1 -IntegratedSources` with the
original roots and installed SDK prefix. This copies current code without
reapplying historical patches.
Later changes are made directly in the two host Git working directories,
tested, committed and pushed to their respective private `origin/main`.
