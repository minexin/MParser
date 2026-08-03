# MParser 1.0.0 Release Notes

Publication contract: **frozen v1**. The source project version is `1.0.0`.
The immutable release commit still requires its exact `v1.0.0` tag, final
tag-scoped authentication, artifact verification, and release-page
publication. Candidate authenticated-provenance evidence is already closed by
tag run `30743014345`.

MParser 1.0.0 defines a stable, documented, embeddable, and sustainably
extensible MATLAB-like subset runtime. It does not claim complete MATLAB,
toolbox, desktop, graphics, Simulink, or MEX compatibility.

## Language And Runtime

The v1 target subset includes:

- MATLAB-like expressions, control flow, local and cross-file functions,
  multiple outputs, `nargin`/`nargout`, and supported function handles;
- dense real numeric/logical N-D arrays, column-major indexing, logical and
  colon indexing, `end`, growth, deletion, reductions, scans, and transforms;
- character/string arrays, cells, ordered structures, structure arrays, and
  transactional nested assignment;
- scoped global/persistent workspace state and reusable runtime sessions;
- structured exceptions, causes/stacks, warnings, assertions, resource stops,
  and source-located diagnostics;
- the documented `classdef` subset, including value/handle classes,
  inheritance, properties, methods, validation, events/listeners,
  enumerations, object arrays, and bounded reflection;
- package, private-function, class-folder, and source-search-path resolution.

The machine-readable
[compatibility matrix](compatibility-matrix.json) is authoritative for exact
coverage and limits. Parser acceptance alone is not a runtime support claim.

## Execution

`mparser --run script.m` is the production command. Legal target-subset code
uses the verified bytecode VM as its semantic authority. Portable typed and
SLJIT native execution are guarded optimizations; unsupported or invalidated
regions fall back without changing program meaning.

Native JIT remains optional at build time. A no-JIT build retains production
execution and portable typed regions. Historical `--run-*` modes are
diagnostic interfaces with the stability policy recorded in
[CLI Reference](cli-reference.md) and
[Versioning And Deprecation](versioning-and-deprecation.md).

The committed scalar and dense-array baselines show matching results, zero
typed fallback, and large gains for the current optimized families. They do
not identify another specialization required for release, so
[`G-JIT-001`](v1.0-jit-scope-decision.md) remains Should-have but is deferred
to v1.x. Legal unoptimized code continues through the VM.

## Embedding And Automation

The release freezes:

- C ABI major 1, revision 1 with opaque retained handles and caller-sized
  structure roots;
- header-only C++20 source API 1.0 over the C ABI;
- compile-once/invoke-many modules, sessions, source graphs, cancellation,
  instruction/time/depth/array/diagnostic limits, diagnostics, and resource
  summaries;
- `mparser.result` machine protocol major 1 with one JSON document followed
  by one LF byte;
- `BuiltinRegistry`/`BuiltinDescriptor` source extension contract 1.0.

Installed CMake targets are `MParser::c_api`, `MParser::cpp_api`, and
`MParser::cli`. See the C and C++ embedding guides for ownership, copying,
threading, unload, error, and compatibility rules.

## Reliability And Distribution

The candidate includes deterministic parser/semantic fuzz regression,
mandatory malformed-bytecode verification, an 8,012-invocation soak,
allocation-failure containment, handle/listener lifecycle checks,
load/unload and concurrent embedding stress, native-cache churn, and resource
boundary tests.

Current Windows local evidence includes:

- native Release: **209/209**;
- clean no-JIT Release: **202/202**;
- the previously accepted MSVC AddressSanitizer no-JIT suite with no
  sanitizer report;
- reproducible fixed-payload ZIP generation, relocation, independent C11 and
  C++20 consumers, and unpacked CLI protocol execution.

Revision `f34d8d9e00816341a01df406c2e315164886c1cc` subsequently passed all six
jobs in [Actions run 30684969401](https://github.com/minexin/MParser/actions/runs/30684969401):
Linux GCC, Linux Clang ASan/UBSan/LeakSanitizer, Windows MSVC, macOS x64,
macOS ARM64, and Linux AArch64 native/portable QEMU. The run includes strict
packages and uploaded artifacts for every release target plus relocated or
unpacked public SDK consumers. See
[Cross-Platform Validation](v1.0-cross-platform-validation.md).

Revision `fed5476841293dcdf74be69aef62faa6bc75e3cf` then passed all seven
execution jobs in Actions run `30746822213`, including the retained release
authentication evidence gate, native Linux ARM64, AArch64 native/portable
QEMU, relocated SDK consumers, release archives, and Linux ASan/UBSan.

Release packaging emits the archive, its CPack `.sha256` sidecar, an unsigned
in-toto Statement v1 using the SLSA Provenance v1 predicate, and
`SHA256SUMS` binding both the archive and statement. The local statement is
audit metadata, not publisher authentication and not a SLSA level claim.
A default-off manual-tag Sigstore job revalidated the five platform package
sets from tag `v0.90.1`, signed each archive and statement, and verified the
exact workflow and tag identity in successful Actions run `30743014345`.
The retained evidence and independent ten-subject verification are recorded
under `docs/release-evidence/v0.90.1-authentication`. The version and public
contracts are now frozen at 1.0.0; publication still requires the final tag
authentication and release-page assembly.
See [v1 Release Process](release-process.md) and
[Release Authentication](release-authentication.md).

The release-labeled candidate-readiness gate now requires zero open Must-have
gaps and rejects any newly introduced or framework-impacting blocker before
publication. Performance and authenticated-provenance evidence are both
closed and machine-validated.

## Compatibility And Migration

Public v1 contracts follow semantic compatibility rules:

- compatible additions may land in v1.x;
- removals or incompatible reinterpretations require the documented
  deprecation window or a v2 boundary;
- internal Parser, HIR, bytecode, VM, and `RuntimeValue` layouts are not
  public ABI;
- serialized bytecode is not a public file format.

Pre-v1 command and embedding migration is documented in
[Migrating From v0.x To v1.0](migration-v1.0.md). Consumers should stop using
removed aliases, select JSON mode explicitly, query ABI/runtime versions,
respect caller-sized structures, and retain/release every returned handle.

## Explicit Non-Goals

The following do not block 1.0.0:

- complete MATLAB syntax, behavior, builtin, or toolbox parity;
- complex/sparse/GPU/table/timetable value families;
- true parallel `parfor`;
- Live Scripts, P-code, MEX, Simulink, graphics, and desktop integration;
- arbitrary MATLAB metaclass, Java, or .NET behavior;
- external compiled builtin callback ABI and zero-copy borrowed host arrays;
- persistent native-code disk cache, LLVM, or OSR.

Long-tail function growth and broader optimization continue under the
[v1.x Roadmap](roadmap-v1.x.md) without destabilizing the frozen v1
contracts.
