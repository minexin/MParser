# Release Authentication

MParser release authentication is a publication operation layered over the
deterministic archive and unsigned SLSA Provenance v1 statement produced by the
local CMake/CPack builder. A successful local package build proves integrity
and records inputs, but it does not authenticate Wang Xin, the repository, or
the hosted workflow that publishes the package.

## Selected Candidate

Repository visibility is not a stable release assumption: the repository may
be temporarily public for cross-platform CI and private for the final release.
GitHub hosted artifact attestations for private repositories require GitHub
Enterprise Cloud, so the stable candidate does not depend on that feature. It
uses Cosign keyless blob signing with the GitHub Actions OIDC identity instead.
This mechanism produced accepted candidate evidence from tag `v0.90.1` in
successful Actions run `30743014345`: every advertised archive and local
provenance statement was signed and verified. The downloaded artifact and all
ten bundles were independently validated, closing `G-PROVENANCE-001`. This is
not the final `1.0.0` tag or GitHub Release publication.

Sigstore keyless signing writes metadata to a public transparency log. The
archive contents and private source tree are not uploaded to that log, but the
repository name, workflow path, release tag, signing identity, timestamps, and
artifact digests become publicly observable. The workflow therefore never
signs on a push, pull request, or ordinary manual CI run.

## Activation Policy

Authentication is available only through `workflow_dispatch` in
`.github/workflows/ci.yml`. The operator must:

1. Select the exact release tag in the workflow ref selector.
2. Set `authenticate_release` to `true`, explicitly accepting public Sigstore
   transparency for that release.
3. Wait for Windows x64, Linux x64, native Linux ARM64, Linux AArch64
   cross/QEMU, macOS x64, macOS ARM64, and the required Linux sanitizer lane
   to succeed in the same workflow run.

The authentication job rejects a branch ref and rejects a tag that does not
equal `v<project-version>`. Its job-level token grants only `contents: read`,
`actions: read`, and `id-token: write`; no pull-request job receives an OIDC
signing token.

## Inputs And Outputs

Each platform artifact allowlists exactly four top-level files: the archive,
its SHA-256 sidecar, the unsigned provenance statement, and `SHA256SUMS`.
CPack staging directories and duplicate nested archives are not uploaded. The
job downloads the five immutable package artifacts produced earlier in the
same run. Before requesting an OIDC certificate, it executes
`tests/validate_release_authentication_input.cmake` for every archive. The
validator requires:

- the exact expected archive name and release version;
- exactly those four top-level files, with no staging tree or extra input;
- a matching per-archive SHA-256 sidecar and two-entry `SHA256SUMS` file;
- one unsigned in-toto Statement v1 using the SLSA Provenance v1 predicate;
- an archive subject with matching name, size, media type, and SHA-256;
- the exact release commit, repository, Release configuration, native-JIT
  setting, local builder ID, and source-clean marker;
- SHA-256 identities for the frozen public contracts, release policy,
  authentication workflow, and authentication-input validator.

Cosign signs both `<archive>` and `<archive>.provenance.json`. Each subject gets
an adjacent `<subject>.sigstore.json` bundle containing the short-lived signing
certificate, signature, timestamp, and transparency-log proof. The job then
verifies each bundle before uploading the authenticated release set as a
workflow artifact. It does not create or publish a GitHub Release.

## Accepted Candidate Evidence

The immutable retained record is
[v0.90.1-authentication](release-evidence/v0.90.1-authentication/README.md).
It binds tag `v0.90.1`, revision
`5763b4752657c54ee5baeaf645a4249b4c5cc8ba`, Actions run `30743014345`,
authenticated artifact `8832142356`, five exact package inputs, and ten
Sigstore v0.3 bundles. The same-run Cosign checks and an independent Sigstore
Python 4.5.0 offline verification both accepted the exact workflow identity
and GitHub Actions issuer. `release_authentication_evidence_smoke` continuously
checks the retained file set, hashes, provenance semantics, and bundle subject
digests.

## Consumer Verification

For a tag such as `v0.90.1`, verify an archive and its local provenance with
the workflow identity and GitHub Actions issuer:

```text
cosign verify-blob mparser-0.90.1-linux-x86_64.tar.gz --bundle mparser-0.90.1-linux-x86_64.tar.gz.sigstore.json --certificate-identity=https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v0.90.1 --certificate-oidc-issuer=https://token.actions.githubusercontent.com
cosign verify-blob mparser-0.90.1-linux-x86_64.tar.gz.provenance.json --bundle mparser-0.90.1-linux-x86_64.tar.gz.provenance.json.sigstore.json --certificate-identity=https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v0.90.1 --certificate-oidc-issuer=https://token.actions.githubusercontent.com
```

After signature verification, validate the archive sidecar, `SHA256SUMS`, and
the local provenance subject and contract digests. A valid Sigstore bundle for
an unexpected repository, workflow path, issuer, branch, or tag must be
rejected even when its cryptographic signature is otherwise valid.

## Trust Boundary

The local statement describes the compiler, platform, configuration, source,
and public-contract inputs observed by the platform build job. The Sigstore
bundle authenticates the release-assembly workflow and tag that accepted and
signed those inputs. Neither mechanism proves that GitHub-hosted runners are
free of compromise, and neither replaces consumer verification.

Changing mechanisms requires an explicit release-policy update, contract-hash
update, negative verification tests, and fresh publication evidence; existing
bundles must not be reinterpreted silently. If the final repository remains
public or moves to GitHub Enterprise Cloud, GitHub hosted artifact attestations
may be evaluated separately, but they are not the portable baseline contract.
