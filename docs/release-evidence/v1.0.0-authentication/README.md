# v1.0.0 Release Authentication Evidence

This immutable evidence set records the successful tag-scoped authentication
run for MParser 1.0.0. GitHub Actions run
[30780391460](https://github.com/minexin/MParser/actions/runs/30780391460)
completed with seven successful execution jobs and one successful release
authentication job. The run used tag `v1.0.0`, source revision
`d8075575403bf30828a928a83bbbbfb9706ba902`, and the explicit
`authenticate_release=true` workflow input.

The authentication job required the exact final tag, downloaded the five
same-run release artifacts, and applied the project authentication-input
validator to each four-file package set. It then used Cosign 3.0.6, installed
through the commit-pinned `cosign-installer` action, to sign and immediately
verify every archive and every local provenance statement. All ten subjects
were accepted with this identity and issuer:

```text
https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v1.0.0
https://token.actions.githubusercontent.com
```

## Retained Evidence

The authenticated Actions artifact has ID `8843808799`, name
`mparser-1.0.0-authenticated-v1.0.0`, API-reported size `15104142` bytes, and
GitHub-recorded ZIP SHA-256
`9224c5466e39386e9831f6641bc608b49326cc9a6834a6be999e57078d1a74e9`.
Its downloaded contents were independently checked as five exact platform
directories and 30 files: one archive, one archive checksum, one provenance
statement, one `SHA256SUMS`, and two Sigstore bundles per platform. There were
no duplicate archives, extra files, or CPack staging directories.

Large platform archives are not checked into Git. This directory retains each
archive checksum, local provenance statement, platform `SHA256SUMS`, and both
Sigstore bundles. `manifest.json` records the omitted archive sizes and
digests plus every retained-file digest. The root `SHA256SUMS` protects the
exact checked-in evidence set.

| Platform | Archive SHA-256 | Provenance SHA-256 |
| --- | --- | --- |
| Linux AArch64 | `77e8bdcdc13f626537a141be39f1e3c759150ae7a47f3e0e67bec83fb38eb0ab` | `c7fb4f11f5cda84ab3fbe8a083ac64768b929694eaa207c8cc4d113b79f5a601` |
| Linux x86-64 | `08fca95bdbf10ebcffa50e47f30235f81cdac8f5bea9916eb02f6567f63aa740` | `9cba5282b59d491994f43016d95e40a3a9d2cd04f1e2c112fd129d169e0cda75` |
| macOS ARM64 | `a61cf44973da3810422a7519814e8fd5661ecbac340f346a34216c6f7fcccd61` | `ed6ebb8787603694383103efd21f791f00ad03b07113909c8647f785cd6ec4c9` |
| macOS x86-64 | `12e0f64d8c0f350e8ed4cac2e4b4d60b9d3ceb79b004c553b24d77cf11a5e2f0` | `c2291ba3ee419369f7d35b7e3ac425ebe8f5a5ab987b8891217f7f44454bbe95` |
| Windows x86-64 | `bbe1d57b26ac97bc176c91c3bfe804a2a99763fd47ab2bf88f8a6b0c7028e410` | `a2c44344bf2b6638f70a1e7be2f78f176c7b1f496bdb4eeac2d1fda6b595ae0f` |

## Independent Verification

After the workflow completed, the authenticated artifact was downloaded and
checked independently on Windows. All five package inputs passed
`tests/validate_release_authentication_input.cmake`, including checksum,
provenance, source revision, repository, Release configuration, native-JIT,
and frozen-contract checks. Sigstore Python 4.5.0 then verified all ten
subjects in offline bundle mode against the production trust configuration,
the exact workflow identity, and the GitHub Actions issuer.

An archive bundle can be rechecked without retaining the large archive by
passing its recorded digest to Sigstore Python:

```text
sigstore verify identity --offline \
  --bundle <archive>.sigstore.json \
  --cert-identity https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v1.0.0 \
  --cert-oidc-issuer https://token.actions.githubusercontent.com \
  sha256:<archive-sha256>
```

The checked-in CTest gate validates the evidence manifest, exact retained file
set, SHA-256 identities, provenance semantics, bundle subject digests, and the
fixed run/tag/identity contract. Cryptographic trust-chain and transparency-log
verification remains the responsibility of a Sigstore verifier; it was run
both inside the authentication job and independently after download.
