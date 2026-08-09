# MParser Documentation

MParser is a documented, embeddable, and sustainably extensible MATLAB-like
subset runtime. The published baseline is v1.0; the active source tree is the
v1.2 development train and does not claim complete MATLAB compatibility.

Start with the task that matches your role.

## Run Scripts

1. [User Manual](user-manual.md)
2. [Build And Install](build-and-install.md)
3. [CLI Reference](cli-reference.md)
4. [Support Matrix](support-matrix.md)
5. [JIT And Fallback](jit-and-fallback.md)
6. [Runtime Boundaries](runtime-boundaries.md)

The normal execution command is:

```text
mparser --run script.m
```

Automation uses:

```text
mparser --run --result-format=json-v1 script.m
```

## Embed MParser

- [C Embedding API](embedding-c-api.md): current C ABI generation 2, opaque handles,
  values, sessions, diagnostics, cancellation, and limits.
- [C ABI Compatibility](c-abi-compatibility.md): structure evolution,
  symbols, layouts, ownership, and candidate-freeze rules.
- [C++ Embedding SDK](embedding-cpp-api.md): header-only C++20 source API 1.2.
- [Machine Result Protocol](machine-result-protocol.md): one-shot JSON
  protocol 1.1.

Installed CMake targets are `MParser::c_api`, `MParser::cpp_api`, and
`MParser::cli`.

## Extend The Runtime

- [Extending Builtins](extending-builtins.md): builtin source contract 1.0,
  descriptor rules, registry behavior, ownership, threading, resources, and
  conformance tests.
- [Architecture](architecture.md): Lexer to Parser to semantic HIR to
  verified bytecode to VM/typed/native execution.

`.m` source functions, builtins compiled into the engine, and a future
external C callback ABI are separate extension levels. The external binary
adapter is Post-v1.0.

## Upgrade Or Review Compatibility

- [Migrating From v0.x To v1.0](migration-v1.0.md)
- [Versioning And Deprecation](versioning-and-deprecation.md)
- [v1.0 Release Notes](release-notes-v1.0.md)
- [v1.0 Release Freeze](v1.0.md)
- [v1.1 Core Compatibility](v1.1.md)
- [v1 Release Process](release-process.md)
- [Release Authentication](release-authentication.md): opt-in release-tag
  signing, public-transparency boundary, and consumer verification.
- [MParser 1.0.0 Release](https://github.com/minexin/MParser/releases/tag/v1.0.0):
  published platform SDKs, checksums, provenance, and Sigstore bundles.
- [v1.0 Contract Freeze](v1.0-contract-freeze.md)
- [v1.0 Roadmap](roadmap-v1.0.md)
- [v1.x Roadmap](roadmap-v1.x.md)
- [v1.x External Gap Plan](v1.x-external-gap-plan.md): post-1.0 differential
  findings and MExecServer kernel requests tracked to 0.1 milestones.

## Machine-Readable Authorities

These files are release contracts, not generated prose:

| Artifact | Authority |
| --- | --- |
| [compatibility-matrix.json](compatibility-matrix.json) | Feature status, tier coverage, limits, executable evidence, and gaps |
| [public-contract-v1.json](public-contract-v1.json) | Archived v1.0 public contract hashes and versions |
| [cli-contract-v1.json](cli-contract-v1.json) | CLI 1.0 modes, options, channels, and compatibility |
| [machine-result-v1.schema.json](machine-result-v1.schema.json) | Tolerant `mparser.result` major-1 consumer schema |
| [performance-baseline-v1.schema.json](performance-baseline-v1.schema.json) | Engineering baseline protocol schema |
| `default_catalog.json` | Archived v1.0 normalized builtin catalog snapshot |

For the active development line, the implementation, compatibility matrix,
current protocol snapshot, and milestone documentation move together. The v1
public contract and catalog remain authorities only for their archived release.

## Release Evidence

- [v1.0 Release Freeze](v1.0.md): final engine/package version, frozen public
  contracts, release evidence, and publication outcome.
- [v0.90 Milestone](v0.90.md): embedding and release-candidate API boundary.
- [v0.90.1 Authentication Hardening](v0.90.1.md): immutable-tag handling,
  exact package upload sets, and the pre-signing rejection regression.
- [v0.90.1 Authentication Evidence](release-evidence/v0.90.1-authentication/README.md):
  historical candidate provenance and signing evidence.
- [v1.0.0 Authentication Evidence](release-evidence/v1.0.0-authentication/README.md):
  retained final-tag provenance, checksums, ten Sigstore bundles, and
  independent verification for Actions run `30780391460`.
- [v1.0.0 Publication Evidence](release-evidence/v1.0.0-publication/README.md):
  exact GitHub Release identity, 32-asset manifest, release-wide checksums,
  full redownload, package-input, Sigstore, SDK-consumer, and CLI validation.
- [v1.0 Cross-Platform Validation](v1.0-cross-platform-validation.md):
  revision-bound Windows, Linux, macOS, x64, ARM64, sanitizer, SDK, and package
  evidence.
- [v1.0 Reliability Gate](v1.0-reliability.md): fuzz, malformed bytecode,
  soak, lifecycle, resource, cache, allocation, unload, concurrency, and
  cross-platform closure plus local Windows MSVC ASan evidence.
- [v1.0 Performance Baseline](v1.0-performance-baseline.md): timing,
  allocation, peak-memory, binary-size, and cache methodology.
- [v1.0 JIT Scope Decision](v1.0-jit-scope-decision.md): measured rationale
  for freezing current guarded coverage and deferring broader specialization
  to v1.x.
- [v1.0 Release Documentation Gate](v1.0-documentation.md): manual
  consistency, installed/archive contents, and release-matrix evidence.
- `docs/baselines/v1.0/README.md` in the source repository: committed
  source-bound reports.

Actions run `30780391460` is the accepted final-tag cross-platform and
authentication snapshot. It does not turn emulated AArch64 execution into
physical-hardware performance evidence; native Linux ARM64 remains the
performance authority for that architecture. The final hosted Release keeps
the authenticated asset digests unchanged and has an independently retained
post-publication audit.

## Historical Milestones

Milestones v0.77 through v0.90.1 remain beside the current documentation
because the v1 compatibility matrix cites their contract-boundary evidence.
Earlier notes are retained in the
[v0.1-v0.76 milestone archive](history/v0.1-v0.76/README.md). Historical
milestones do not override the current compatibility matrix, public contract,
or v1 manuals.

The source repository's top-level `README.md` remains the project overview
and sample catalog.
