# MParser Documentation

MParser v1.0 is a stable, documented, embeddable, and sustainably extensible
MATLAB-like subset runtime. It does not claim complete MATLAB compatibility.

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

- [C Embedding API](embedding-c-api.md): stable C ABI 1.1, opaque handles,
  values, sessions, diagnostics, cancellation, and limits.
- [C ABI Compatibility](c-abi-compatibility.md): structure evolution,
  symbols, layouts, ownership, and old-header support.
- [C++ Embedding SDK](embedding-cpp-api.md): header-only C++20 source API 1.0.
- [Machine Result Protocol](machine-result-protocol.md): one-shot JSON
  protocol 1.0.

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
- [v1 Release Process](release-process.md)
- [Release Authentication](release-authentication.md): opt-in release-tag
  signing, public-transparency boundary, and consumer verification.
- [v1.0 Contract Freeze Candidate](v1.0-contract-freeze.md)
- [v1.0 Roadmap](roadmap-v1.0.md)
- [v1.x Roadmap](roadmap-v1.x.md)

## Machine-Readable Authorities

These files are release contracts, not generated prose:

| Artifact | Authority |
| --- | --- |
| [compatibility-matrix.json](compatibility-matrix.json) | Feature status, tier coverage, limits, executable evidence, and gaps |
| [public-contract-v1.json](public-contract-v1.json) | Combined public contract hashes and versions |
| [cli-contract-v1.json](cli-contract-v1.json) | CLI 1.0 modes, options, channels, and compatibility |
| [machine-result-v1.schema.json](machine-result-v1.schema.json) | Tolerant `mparser.result` major-1 consumer schema |
| [performance-baseline-v1.schema.json](performance-baseline-v1.schema.json) | Engineering baseline protocol schema |
| `default_catalog.json` | Installed normalized builtin catalog snapshot |

When prose and a machine-readable contract appear to disagree, treat it as a
documentation defect and use the versioned machine contract until corrected.

## Release Evidence

- [v0.90 Milestone](v0.90.md): embedding and release-candidate API boundary.
- [v0.90.1 Authentication Hardening](v0.90.1.md): immutable-tag handling,
  exact package upload sets, and the pre-signing rejection regression.
- [v0.90.1 Authentication Evidence](release-evidence/v0.90.1-authentication/README.md):
  retained provenance, checksums, ten Sigstore bundles, and independent
  verification for Actions run `30743014345`.
- [v1.0 Cross-Platform Candidate Validation](v1.0-cross-platform-validation.md):
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

Actions run `30684969401` is the accepted cross-platform candidate snapshot.
It does not turn emulated AArch64 execution into physical-hardware performance
evidence or unsigned provenance into publisher authentication.

## Historical Milestones

Files named `v0.xx.md` record the contract and evidence at each pre-v1
milestone. They explain design history but do not override the current
compatibility matrix, public contract, or v1 manuals.

The source repository's top-level `README.md` remains the project overview
and sample catalog.
