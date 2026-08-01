# v1.0 Support Matrix

MParser v1.0 is a stable MATLAB-like subset runtime, not a complete MATLAB
replacement. This page is a release-oriented summary. The machine-readable
[compatibility-matrix.json](compatibility-matrix.json) is authoritative and
links every supported or partial claim to source and executable evidence.

## Status Meanings

| Status | Meaning |
| --- | --- |
| Supported | The stated target-subset behavior has executable evidence. |
| Partial | A useful, explicitly bounded subset is implemented. Read its limits. |
| Unsupported | The behavior is intentionally outside the v1.0 contract. |
| Planned | No release claim may depend on it yet. |

Parser acceptance alone is not a runtime support claim. A feature may also
have different HIR, bytecode, typed, and native tier states. Legal target
subset code always relies on the production bytecode VM as semantic fallback;
typed/native coverage is additive.

## Language And Runtime

| Area | v1.0 summary | Matrix entries |
| --- | --- | --- |
| Lexing and expressions | Lossless tokens, MATLAB-like literals/operators, precedence, member access, and delayed call/index resolution | `LEX-001`, `SYN-001` |
| Control flow | `for`, serial `parfor`, `while`, `if`, `switch`, `try/catch`, `break`, `continue`, `return` | `SYN-002` |
| Functions | Local/cross-file calls, argument contracts, `nargin`/`nargout`, multiple outputs, and supported handles | `FUN-001`, `HANDLE-001` |
| Source graph | Search paths, packages, private functions, and class folders | `SRC-001` |
| Numeric/logical arrays | Dense scalar through N-D shape/index rules, logical/colon/`end` indexing, growth, and deletion within recorded limits | `ARR-001`, `ARR-002` |
| Text | Distinct UTF-16 character and string arrays with shared conversion/formatting rules | `TEXT-001` |
| Cell and Struct | N-D Cells, ordered structures, structure arrays, dynamic fields, and comma-separated field results | `CELL-001`, `STRUCT-001`, `STRUCT-002` |
| Assignment | Transactional nested member/parenthesis/brace copy-back | `LVAL-001` |
| Workspace | Scoped global/persistent bindings and reusable sessions | `WORKSPACE-001` |
| Exceptions | Structured exceptions, causes/stacks, warning state, assertions, and embedding diagnostics within explicit correction limits | `EXC-001` |
| Classes | Target `classdef` syntax, value/handle semantics, properties, events/listeners, reflection, and object arrays | `CLASS-001` through `CLASS-006` |
| Builtins | Representative math, reduction, scan, constructor, and array-transform families through one registry contract | `BUILTIN-001`, `BUILTIN-002` |

Entries marked partial describe intentional subset boundaries, not an
invitation to infer MATLAB behavior. Read each entry's `limits` field before
shipping code that depends on it.

## Execution Engines

| Tier | Contract |
| --- | --- |
| Reference HIR | Diagnostic/reference subset; deliberately narrower than production |
| Bytecode VM | Production semantic authority for legal target-subset code |
| Portable typed | Guarded optimized regions with transactional fallback |
| Native JIT | Bundled SLJIT kernels for eligible scalar, branched, and linear-array regions |
| Adaptive runtime | Repeated module/session execution, guarded promotion, invalidation, and bounded process-local cache |

The `--run` command is the production interface. `--run-hir`,
`--run-bytecode`, `--run-jit`, `--run-typed-bytecode`, and adaptive/profile
commands are diagnostics. Native or typed ineligibility must not make a legal
script fail; it returns to a less specialized tier.

## Embedding And Extension

| Boundary | v1.0 contract |
| --- | --- |
| CLI | Production `--run`, strict options, stable exit classes, JSON protocol selector |
| Machine protocol | `mparser.result` 1.0, one JSON document plus LF |
| C ABI | ABI major 1 revision 1, opaque retained handles, caller-sized roots |
| C++ API | Header-only C++20 source API 1.0 over the C ABI |
| Builtin extension | Source contract 1.0 using registry/descriptors/call/results |
| Packaging | Relocatable C/C++/CLI SDK with CMake targets, schemas, docs, examples, notices, checksums, and unsigned SLSA provenance metadata |

The C ABI supports copied column-major values, source graphs, compile-once
invocation, sessions, diagnostics, cancellation, and resource summaries.
Host-created array payloads are copy-in. Returned views remain owned by their
value/result handles.

## Release Platforms

| Platform | Release target | Native JIT | Current evidence note |
| --- | --- | --- | --- |
| Windows x64 | Yes | Yes | Full native suite and package/upload passed in run `30684969401`; local no-JIT 198/198 and MSVC ASan no-JIT 199/199 also pass |
| Linux x64 | Yes | Yes | Full GCC suite/package/upload and Clang ASan/UBSan/LSan suite passed in run `30684969401` |
| Linux AArch64 | Yes | Yes | Native and portable cross-build/QEMU smokes, installed consumers, package/unpacked SDK, and sample passed in run `30684969401`; physical ARM remains required for performance claims |
| macOS x64 | Yes | Yes | Full suite, relocated production SDK consumers, architecture/version checks, package, and upload passed in run `30684969401` |
| macOS ARM64 | Yes | Yes | Full suite, relocated production SDK consumers, native architecture/version checks, package, and upload passed in run `30684969401` |

The table defines the intended v1.0 release set and points to the accepted
functional candidate snapshot at revision `f34d8d9`. Unsupported operating
systems or architectures may build incidentally but are not release targets.
See [Cross-Platform Candidate Validation](v1.0-cross-platform-validation.md)
for exact lane scope and exclusions.

## Resource And Concurrency Boundaries

Embedding requests may bound instruction count, wall time, call depth,
per-value recursive array payload, and diagnostic count, and may carry a
cooperative cancellation token. Zero means unlimited for each numeric limit.
Resource stops are terminal request outcomes and are not catchable language
exceptions.

Independent stateless calls can run concurrently. Calls carrying module-bound
mutable objects or closures serialize on their owner. Session operations are
also ordered. Retained C/C++ wrappers may cross threads only when each thread
owns its own retained reference; concurrently overwriting one host handle
variable is invalid.

See [Runtime Boundaries](runtime-boundaries.md) for the complete operational
contract.

## Explicitly Unsupported In v1.0

- complete MATLAB language or behavioral compatibility;
- complete MATLAB builtin and toolbox catalogs;
- Live Scripts, P-code, MEX, Simulink, graphics, and MATLAB desktop features;
- complex and sparse numeric arrays;
- GPU arrays, tables, timetables, and unlisted domain value types;
- true parallel `parfor`;
- arbitrary MATLAB metaclass, Java, or .NET behavior;
- independently compiled external builtin callbacks;
- zero-copy borrowed host input arrays;
- persistent native-code disk cache.

Long-tail functions/toolboxes and complete MATLAB compatibility are
Post-v1.0. They do not block v1.0 and must not force silent reinterpretation
of the frozen CLI/API/extension contracts.

## Evidence And Change Policy

Every matrix claim marked supported or partial names at least one registered
test and one source artifact. `compatibility_matrix_smoke` rejects missing
sources, missing test registrations, duplicate IDs, invalid states, and
version drift.

Starting with 1.0.0, additive language features or optimized regions may land
in v1.x, but stable v1 contracts cannot be removed or reinterpreted before
v2. See [Versioning And Deprecation](versioning-and-deprecation.md).
