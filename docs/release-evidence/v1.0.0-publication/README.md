# MParser v1.0.0 Publication Evidence

MParser 1.0.0 was published as a non-prerelease GitHub Release at
<https://github.com/minexin/MParser/releases/tag/v1.0.0> on
`2026-08-03T08:59:01Z`. GitHub release ID `RE_kwDOTV8DlM4Vs2eC` binds the
published page to immutable tag `v1.0.0` and source revision
`d8075575403bf30828a928a83bbbbfb9706ba902`.

The release contains exactly 32 assets totalling 15,194,301 bytes: five
platform archives, five archive SHA-256 sidecars, five unsigned SLSA
Provenance v1 statements, ten Sigstore bundles, five platform checksum files,
the authentication manifest, and one release-wide checksum file. Large
release assets are not duplicated in the source repository. `manifest.json`
records every published name, size, and API SHA-256 digest, while
`mparser-1.0.0-release-assets.SHA256SUMS` is the exact published checksum asset
covering the other 31 files.

## Independent Validation

After publication, all 32 canonical
`/releases/download/v1.0.0/` assets were downloaded again. The downloaded set
matched the prepublication set byte for byte, all 31 release-wide checksum
entries passed, and every local size and digest matched the GitHub API record.

The five platform package inputs were reconstructed into their original
four-file layouts and passed
`tests/validate_release_authentication_input.cmake` against the exact tag
source. This checked version `1.0.0`, revision, repository, Release
configuration, native JIT, clean-source state, archive digest, and provenance
semantics. Sigstore Python 4.5.0 independently accepted all ten downloaded
subjects in offline mode with identity
`https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v1.0.0`
and issuer `https://token.actions.githubusercontent.com`.

The downloaded Windows x86-64 SDK separately passed its independent C11 and
multi-translation-unit C++20 consumers, two tests each. Its installed CLI
reported `MParser 1.0.0`, emitted a successful `mparser.result` 1.0 document
for `machine_protocol_demo.m`, and ran the class-folder sample through the
documented `--path` interface.

Final-tag authentication was produced by Actions run `30780391460`.
Post-tag retained-evidence revision
`0ca98aab4f1c14e7d2043fb14ba3a72dc31d2849` passed all seven execution lanes
in Actions run `30796864411`. The tag remains on the immutable release source;
publication evidence is intentionally a later documentation-only record on
`main`.

## Consumer Check

Download the assets needed for one platform and verify the release-wide file:

```text
sha256sum -c mparser-1.0.0-release-assets.SHA256SUMS --ignore-missing
```

The adjacent `.sigstore.json` bundle authenticates each archive or provenance
subject. Verification commands and the public-transparency boundary are
documented in `docs/release-authentication.md`.
