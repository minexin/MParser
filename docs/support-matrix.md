# v1.x Support Matrix

MParser v1.x is a MATLAB-like subset runtime, not a complete MATLAB
replacement. This page summarizes the released v1.0 baseline, frozen v1.2
candidate, and active v1.3 development line. Development interfaces are not
production compatibility promises until their milestone candidate is frozen.
The machine-readable
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

## v1.1 Additions

v1.1 preserves every frozen v1 host contract and adds four MATLAB-compatible
runtime corrections:

- chained `^` and `.^` operators associate left while power remains above
  unary minus;
- commas delimit one-line `if`, `switch`, `for`, and `while` statements only
  at top-level delimiter depth;
- a matrix `for` range yields one column per iteration, with typed scalar
  regions falling back when a runtime column is nonscalar;
- `A(:)` always returns a `numel(A)`-by-1 value, including for row vectors.

The combined executable sample is
`samples/v1_1_core_compatibility_demo.m`. Exact tier and limitation claims are
recorded under `SYN-001`, `SYN-002`, `ARR-001`, and `ARR-002`.

## v1.2 Candidate Additions

The current v1.2 train is building one end-to-end numeric foundation:

- dense `double`, `single`, logical, and all eight fixed-width integer classes;
- exact `int64`/`uint64` storage and transport without conversion through
  `double`;
- precision-preserving `int64`/`uint64` arithmetic with scalar `double`,
  including both operand orders, N-D expansion, binary80 intermediate
  rounding, integer conversion, non-finite values, and saturation;
- dense complex `double` and `single` literals, operators, transpose,
  reductions, scans, predicates, and elementary math;
- exact hexadecimal/binary integer literals with default-width inference and
  signed/unsigned width suffixes;
- typed constructors and class-aware `colon`, `size`, `linspace`, `sum`, and
  `prod` behavior;
- shape-preserving `double(string)` plus registry-backed `str2double` for
  decimal, scientific, grouped, complex, Inf/NaN, and hexadecimal text;
- first-class shaped missing arrays and row-structured cell literals through
  parser, HIR, interpreter, bytecode, and VM fallback;
- shared `mod`/`rem`, `nextpow2`, scalar/vector/row/column/matrix predicates,
  and recursive `isequal`/`isequaln` semantics;
- `disp`, bounded `fprintf`/`sprintf`, host output sinks, ordered retained
  output events, and suppressed/unsuppressed top-level expression results;
- in-memory source compilation with search paths plus script/function/class,
  primary-function, pure-function-file, and top-level-statement metadata;
- guarded portable/native fallback for types not represented by Typed IR;
- C/C++ source API 1.2, C ABI generation 2, and machine protocol 1.1
  transport.

The runnable coverage is split between `samples/numeric_types_demo.m`,
`samples/complex_numeric_demo.m`, and
`samples/core_numeric_builtins_demo.m`; shaped missing/text behavior is in
`samples/text_runtime_demo.m`, and host behavior is exercised by
`samples/host_integration_demo.m` and the C/C++ embedding demos. The source tree and installed SDK
report candidate version `1.2.0`; ABI generation and protocol numbers are
independent contract metadata, not SDK product versions. Its
166-descriptor/168-name catalog and current public surfaces are frozen by the
v1.2 candidate contract.

## v1.3 Development Additions

The current tree has two end-to-end system and standard-library batches:

- one session-owned capability context and injectable host adapter for current
  directory, search paths, environment, filesystem read/write, process,
  clock, sleep, and random services;
- command-form parsing and workspace-aware `clear`, `who`, `whos`, `exist`,
  display `format`, and MATLAB-like implicit-output rules;
- `pwd`, `cd`, `tempdir`, `path`, `addpath`, `rmpath`, `which`, `dir`,
  `getenv`, `date`, `clock`, `computer`, `version`, `pause`, and `system`;
- session-reproducible `rand`, `randn`, `randi`, and `rng`, including shaped
  arrays, single output, state save/restore, and guarded VM fallback;
- native-aware `fullfile`, `filesep`, and `pathsep`, including string-array
  shape and missing-string-as-empty component behavior;
- session-scoped `fopen`, `fclose`, `fseek`, `ftell`, `frewind`, file-targeted
  `fprintf`, and repeated `fscanf` with update-stream barriers, translated
  Windows text positions, bounded widths/output, scalar or matrix size,
  column-major zero padding, mixed text/numeric conversion, and exact long
  `int64`/`uint64` input;
- parser diagnostics for illegal bare newlines in calls/indexing while matrix
  and cell newlines remain row separators and `...` remains the legal
  continuation form;
- whitespace-aware single-quote disambiguation in array literals, so
  `['ab' 'cd']` constructs `'abcd'` while adjacent `x'` remains transpose;
- shape-preserving case conversion and trimming, numeric-to-character
  conversion, splitting, and a documented regular-expression subset;
- numeric, complex, exact-integer, text, Cell-text, and shape-only missing
  ordering/set operations, including N-dimensional `sort`, up to three
  `unique` outputs, row mode, stable/first/last choices, and missing policy;
- `iscell`, dimension-aware `struct2cell`/`cell2struct`, and synchronous
  `cellfun` callbacks with multiple inputs/outputs, `UniformOutput`, and
  `ErrorHandler` through the shared dynamic-call context.

The runnable evidence is `samples/system_services_demo.m`,
`samples/random_runtime_demo.m`, `samples/file_io_demo.m`, and
`samples/standard_library_demo.m`. These are
development capabilities, not a claim of complete MATLAB system or file I/O.
Binary `fread`/`fwrite`, line reads, end/error queries, scansets, selectable
encodings, remote files, MAT files, dynamic evaluation, many broad
standard-library families, and full MATLAB regexp/Unicode behavior remain
open. The product and public SDK still
report `1.2.0` until the complete v1.3 milestone is ready to freeze.

## Language And Runtime

| Area | Current v1.x summary | Matrix entries |
| --- | --- | --- |
| Lexing and expressions | Lossless tokens, MATLAB-like literals/operators, precedence, member access, and delayed call/index resolution | `LEX-001`, `SYN-001` |
| Control flow | `for`, serial `parfor`, `while`, `if`, `switch`, `try/catch`, `break`, `continue`, `return` | `SYN-002` |
| Functions | Local/cross-file calls, argument contracts, `nargin`/`nargout`, multiple outputs, and supported handles | `FUN-001`, `HANDLE-001` |
| Source graph | Search paths, packages, private functions, and class folders | `SRC-001` |
| Numeric/logical arrays | Dense core numeric classes and complex double/single through N-D shape/index rules, logical/colon/`end` indexing, growth, and deletion within recorded limits | `ARR-001`, `ARR-002` |
| Text | Distinct UTF-16 character and string arrays with shared conversion/formatting rules | `TEXT-001` |
| Missing values | First-class scalar and N-D `missing` arrays with shape-preserving transforms, indexing/mutation, floating/string promotion, and same-shape `ismissing` masks | `MISSING-001` |
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

| Boundary | Current development contract |
| --- | --- |
| CLI | Production `--run`, strict options, stable exit classes, JSON protocol selector |
| Machine protocol | `mparser.result` 1.1, exact typed/complex JSON values, ordered output/expression records, one document plus LF |
| C API/ABI | C source API 1.2; ABI generation 2 revision 0, typed real/imaginary buffers, source metadata, output sink/results, opaque retained handles, caller-sized roots |
| C++ API | Header-only C++20 source API 1.2 over C ABI generation 2, including RAII source metadata and host output projection |
| Builtin extension | Frozen v1.2 source contract 1.1; active in-tree source contract 1.3 using registry/descriptors/call/results |
| Packaging | Relocatable C/C++/CLI SDK with CMake targets, schemas, docs, examples, notices, checksums, and unsigned SLSA provenance metadata |

The C ABI supports copied column-major values, source graphs, compile-once
invocation, sessions, diagnostics, cancellation, resource summaries,
synchronous output routing, retained output events, and top-level expression
results.
Host-created array payloads are copy-in. Returned views remain owned by their
value/result handles.

## Release Platforms

| Platform | Release target | Native JIT | Current evidence note |
| --- | --- | --- | --- |
| Windows x64 | Yes | Yes | Full suite/package passed in run `30746822213`; exact 1.0.0 local native 209/209, clean no-JIT 202/202, and accepted MSVC ASan no-JIT 199/199 also pass |
| Linux x64 | Yes | Yes | Full GCC suite/package and Clang ASan/UBSan/LSan suite passed in run `30746822213` |
| Linux AArch64 | Yes | Yes | Native `ubuntu-24.04-arm` full suite plus independent cross-build/QEMU native/portable package lane passed in run `30746822213`; accepted non-emulated ARM MIDR performance reports remain bound to run `30732814590` |
| macOS x64 | Yes | Yes | Full suite, relocated production SDK consumers, architecture/version checks, and package passed in run `30746822213` |
| macOS ARM64 | Yes | Yes | Full suite, relocated production SDK consumers, native architecture/version checks, and package passed in run `30746822213` |

The table defines the v1.0 release set and points to the latest accepted
pre-freeze cross-platform snapshot at revision `fed5476`. Unsupported
operating systems or architectures may build incidentally but are not release
targets. See [Cross-Platform Validation](v1.0-cross-platform-validation.md)
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

## Explicitly Unsupported In Current v1.x

- complete MATLAB language or behavioral compatibility;
- complete MATLAB builtin and toolbox catalogs;
- Live Scripts, P-code, MEX, Simulink, graphics, and MATLAB desktop features;
- sparse and complex-integer numeric arrays;
- GPU arrays, tables, timetables, and unlisted domain value types;
- true parallel `parfor`;
- arbitrary MATLAB metaclass, Java, or .NET behavior;
- independently compiled external builtin callbacks;
- zero-copy borrowed host input arrays;
- persistent native-code disk cache.

Long-tail functions/toolboxes and complete MATLAB compatibility remain staged
v1.x or later work. They do not force the current milestone to carry adapters
for superseded development interfaces.

## Evidence And Change Policy

Every matrix claim marked supported or partial names at least one registered
test and one source artifact. `compatibility_matrix_smoke` rejects missing
sources, missing test registrations, duplicate IDs, invalid states, and
version drift.

The v1.0 snapshots remain immutable historical evidence. The current C API
1.2, ABI generation 2, C++ API 1.2, protocol 1.1, and builtin contract 1.1
snapshots are frozen separately at the v1.2 candidate gate. The in-tree
builtin source contract has advanced to 1.3 for v1.3 development and is not a
new frozen SDK release. See
[v1.x Roadmap](roadmap-v1.x.md).
