# MParser v1.15.0 Publication Evidence

[MParser v1.15.0](https://github.com/minexin/MParser/releases/tag/v1.15.0)
was published as a GitHub prerelease on 2026-11-06. The exact tag resolves to
`6b2f0d666964a8a9b46bbba51a9ecb4eb46b4c4b`. It did not replace v1.0.0 as
the latest stable release. This evidence is recorded after the tag, not by
changing the tagged source.

[Actions run 42141719375](https://github.com/minexin/MParser/actions/runs/42141719375)
built the five archives and authenticated each archive and its local
provenance in the same run. Independent Sigstore Python 4.5.0 offline
verification accepted all ten subjects for the exact identity
`https://github.com/minexin/MParser/.github/workflows/ci.yml@refs/tags/v1.15.0`
and OIDC issuer `https://token.actions.githubusercontent.com`. The raw local
provenance is still marked unsigned; the adjacent bundle authenticates it.

GitHub release ID `435963620` contains 32 assets (32,710,907 bytes):
five archives, five archive checksums, five provenance
statements, ten Sigstore bundles, five per-platform checksum files, the
authentication manifest and one release-wide checksum file. [Manifest](manifest.json)
records the canonical asset set, byte sizes and digests. All 32 staged files
matched GitHub's draft asset sizes and SHA-256 values before publication.
After publication all 32 canonical release URLs were fetched independently;
the downloaded bytes, [31 release-wide checksum entries](mparser-1.15.0-release-assets.SHA256SUMS), ten signatures and
all five package-input sets matched. Each archive was checked for 32 installed
SDK entries and exact active contract manifest binding against the tag.

The canonical Windows and Linux x64 downloads each passed independent C and
C++ installed consumers (two tests per language), the exact 158-symbol ABI
comparison and CLI `mparser.result` 1.2 execution. An isolated Windows
MExecServer/MExecClient rebuild passed 86/86 tests against the final signed
archive's DLL. The DLL matched the canonical Windows download by SHA-256:
`50F5E3D634D69128C0757AE2E79B3D3233AEFDF607083939222FA8CAA01F9048`.

This is bounded release and SDK evidence, not full MATLAB compatibility or
an MExecServer/MExecClient production deployment. The host projects remain
separate from the released assets.
