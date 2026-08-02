# v0.90.1 Release Authentication Evidence

This immutable evidence set records the successful tag-scoped authentication
run for the MParser v1 release candidate. GitHub Actions run
[30743014345](https://github.com/minexin/MParser/actions/runs/30743014345)
completed with seven successful execution jobs and one successful release
authentication job. The run used tag `v0.90.1`, source revision
`5763b4752657c54ee5baeaf645a4249b4c5cc8ba`, and the explicit
`authenticate_release=true` workflow input.

The authentication job first required the exact tag, downloaded the five
same-run release artifacts, and applied the project authentication-input
validator to each four-file package set. It then used Cosign 3.0.6, installed
through the commit-pinned `cosign-installer` action, to sign and immediately
verify every archive and every local provenance statement. All ten subjects
were accepted with this identity and issuer:

```text
https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v0.90.1
https://token.actions.githubusercontent.com
```

## Retained Evidence

The downloaded authenticated Actions artifact had ID `8832142356`, name
`mparser-0.90.1-authenticated-v0.90.1`, size `14858931` bytes, and local
download SHA-256
`7460347da20b7b0a4b4cf041c965485c09d7622c7e349c5459f480daed2460d0`.
Its exact contents were five platform directories and 30 files: one archive,
one archive checksum, one provenance statement, one `SHA256SUMS`, and two
Sigstore bundles per platform. There were no duplicate archives, extra files,
or CPack staging directories.

Large platform archives are not checked into Git. This directory retains each
archive checksum, local provenance statement, platform `SHA256SUMS`, and both
Sigstore bundles. `manifest.json` records the omitted archive sizes and
digests plus every retained-file digest. The root `SHA256SUMS` protects the
exact checked-in evidence set.

| Platform | Archive SHA-256 | Provenance SHA-256 |
| --- | --- | --- |
| Linux AArch64 | `7a6919e2376197b760be279b07232c90bedffb2bb0a9819608905c9f4d12b79e` | `27e63c6f9f9d2c8b56e2d713bd984e932175194b6efd8fd5cf084174668ab17c` |
| Linux x86-64 | `6acfabbb4bc83f86162e95260e0567b6843caeefd83d544629bfa937f7ab6572` | `c3dfa7991d8ba5f8b141a4db165d40be0732e3ba09e538c60f769207f7a1d1c2` |
| macOS ARM64 | `e043a232f7240a595de96b728cb3a33b87c988e07527c67075748e165893fa3f` | `48a762ef4eebfb5fe436ad03a3247b52a8674105a922af16abfecd95a9d27029` |
| macOS x86-64 | `dee5abd30a59bc47ccc7cacfb862354d99a57e660670b36dd9d3e134f915eec6` | `b06df45d1772fd9dcd14ee4959fc1761fe57059c9f6379e11246696ea1758986` |
| Windows x86-64 | `004a87b81be47ccd6e06ad9cc715cb7a0b67e1bf0b604a57e8ec3c7a7d16a617` | `e885a29b59dc9c7a70c7a23fd60cdc3c4c508ff9b488891ca99edd930efc9f44` |

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
python -m sigstore verify identity --offline \
  --bundle <archive>.sigstore.json \
  --cert-identity https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v0.90.1 \
  --cert-oidc-issuer https://token.actions.githubusercontent.com \
  sha256:<archive-sha256>
```

The checked-in CTest gate validates the evidence manifest, exact retained file
set, SHA-256 identities, provenance semantics, bundle subject digests, and the
fixed run/tag/identity contract. Cryptographic trust-chain and transparency-log
verification remains the responsibility of a Sigstore verifier; it was run
both inside the authentication job and independently after download.

