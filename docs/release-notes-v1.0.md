# MParser 1.0.0 Release Notes

Publication status: **release candidate documentation**. The source project
remains version `0.90.0` until the final release-platform validation window
passes and the public artifacts are generated from the frozen release commit.

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

## Embedding And Automation

The release candidate freezes:

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

Windows local evidence currently includes:

- native Release: **202/202**;
- no-JIT Release: **196/196**;
- MSVC AddressSanitizer no-JIT: **197/197** with no sanitizer report;
- reproducible fixed-payload ZIP generation, relocation, independent C11 and
  C++20 consumers, and unpacked CLI protocol execution.

Final Linux x64/AArch64 and macOS x64/ARM64 candidate confirmation is deferred
to the shared validation window and is not represented here as passed.

Release packaging emits the archive, its CPack `.sha256` sidecar, an unsigned
in-toto Statement v1 using the SLSA Provenance v1 predicate, and
`SHA256SUMS` binding both the archive and statement. The local statement is
audit metadata, not publisher authentication and not a SLSA level claim.
Final publication must attach the selected signing or hosted-provenance
identity. See [v1 Release Process](release-process.md).

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
