# MParser v1.15.0 Prerelease

This host-integration milestone is a MATLAB-like subset runtime, not a claim
of full MATLAB compatibility or a replacement for the latest stable release.
It provides the CLI and installed C/C++ SDK for Windows x64, Linux x64,
Linux AArch64, macOS x64 and macOS ARM64.

## What's Included

- Compile diagnostics reject empty assignment right-hand sides and reserved
  keyword assignments. Hosts can validate variable names without compiling or
  executing source, and use compiler-owned function, nested-function and class
  method ranges for navigation across UTF-8 source units.
- Headless Figure/Axes/Line lifecycle and column-wise matrix plotting, with
  real Text, Legend and Clipping data. Opt-in graphics v3 identifies live
  objects across snapshots. Graphics v4 freezes a Figure's explicitly supplied
  host layout metrics; missing metrics are not guessed.
- `print` and `saveas` validate figure, format and destination before invoking
  an optional host renderer. The SDK does not ship an encoder. A default
  headless runtime reports an explicit unsupported result instead of producing
  a file with the wrong format. Browser PNG download remains a separate action.
- Ordered, opt-in completed-result display events and an authoritative
  session search-path API. Existing output remains unchanged when the new
  event collection is not enabled; there is no live event stream.

SDK identity: C/C++ source API 1.15, C ABI generation 2 revision 13 (158
exports), builtin source contract 1.23, CLI 1.0 and machine result 1.2.
See the [integration guide](https://github.com/minexin/MParser/blob/v1.15.0/docs/v1.15.md)
for verified scope and limitations. MExecServer and MExecClient are separate
projects; their binaries, browser assets and deployments are not part of this
release.

## Verify The Downloads

The release-wide `mparser-1.15.0-release-assets.SHA256SUMS` covers all other
assets. Each platform has its own archive, archive checksum, local provenance,
platform checksums and Sigstore bundles for the archive and provenance. The
bundles bind the GitHub Actions signing identity to the exact `v1.15.0` tag.
For example, verify the Windows archive with:

```text
cosign verify-blob mparser-1.15.0-windows-x86_64.zip --bundle mparser-1.15.0-windows-x86_64.zip.sigstore.json --certificate-identity=https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v1.15.0 --certificate-oidc-issuer=https://token.actions.githubusercontent.com
```
