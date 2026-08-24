# MParser v1.x Roadmap

The v1.x line grows the MATLAB-like subset after the 1.0.0 baseline. The
current line is active development rather than a production deployment: v1
release contracts remain historical evidence, while current CLI, C/C++ API,
machine protocol, and builtin contracts may move forward together when a
cleaner runtime model requires it. In-repository users are updated in the same
change; compatibility adapters for unreleased development interfaces are not
added by default.

## Release Discipline

Every v1.x change follows these rules:

- legal code outside an optimized region must continue through verified
  bytecode/VM fallback;
- prefer one coherent current contract over wrappers for obsolete development
  interfaces; freeze a new public snapshot only at a milestone release gate;
- each language or builtin addition updates the compatibility matrix,
  representative interpreter/VM/typed tests, samples when user-facing, and
  extension/user documentation;
- performance changes include correctness equivalence and measured evidence,
  not timing-only acceptance;
- release-candidate archives retain checksums, provenance, relocation,
  independent consumer, and platform validation; internal batches do not pay
  that packaging cost repeatedly.

A `0.1` release is a complete release train rather than a single narrow fix.
Several implementation batches may land under the same development milestone,
with focused local regression at each batch boundary. The product/source SDK
is stamped once with that train's development version, while the final
milestone narrative, full differential rerun, release packaging, and complete
platform CI are updated together only when the whole milestone is a release
candidate. Cross-platform-risk changes may still use intermediate CI, but do
not create an extra public version.

The C API 1.2, C ABI generation 2, and C++ source API 1.2 work in v1.2 intentionally
replace the archived v1 host surface so exact integer and complex arrays have one direct
transport model. MExecServer and other downstream projects consume the
resulting milestone API after it settles; preserving their current adapters is
not a kernel release requirement during this iteration.

## Workstream Boundaries

The remaining v1.x work is tracked in four related but distinct streams:

- **Language and data semantics** are prerequisites. Core MATLAB numeric
  classes, complex values, conversion and promotion, array shape, indexing,
  operators, and diagnostics must be correct in the VM before functions or JIT
  paths claim support.
- **Function-library coverage** grows through `BuiltinRegistry` and `.m`
  libraries. Mathematical and array functions share the same value, calling,
  error, and multi-output contracts as user functions.
- **System and host services** require explicit session-scoped capabilities for
  output, workspace, paths, environment, files, processes, time, and random
  state. Embedded execution must not silently mutate process-global state.
- **Optimization coverage** is evidence-driven. Legal code always retains a
  bytecode authority and guarded fallback; unsupported types or shapes are not
  coerced merely to enter a typed/native path.

## v1.1: Core Compatibility Corrections

**Status: implemented; validation evidence is recorded in
[v1.1.md](v1.1.md).**

This milestone closes six structural mismatches found by the 223-point
external differential suite: left-associated power, comma-separated one-line
control forms, matrix-column `for`, and column-shaped `A(:)`. It adds shared
runtime authorities and guarded typed fallback without changing the frozen v1
host contracts.

## v1.2: Core Runtime And Numeric Foundation

**Status: implemented; candidate evidence and the frozen current contract are
recorded in [v1.2.md](v1.2.md) and
[public-contract-v1.2.json](public-contract-v1.2.json).**

v1.2 is a broad vertical milestone, not one constructor per release. It closes
the public host foundation and the core numeric representation needed by later
function growth:

- compile in-memory source with explicit search/class paths, expose source
  metadata, return top-level expression values with stable `ans` locations,
  and route output through a host-owned sink;
- support MATLAB core numeric classes end to end: `double`, `single`,
  `logical`, `int8`/`uint8`, `int16`/`uint16`, `int32`/`uint32`, and
  `int64`/`uint64`;
- support real and complex `double`/`single` scalars and dense arrays,
  imaginary literals, conjugating versus nonconjugating transpose, and exact
  embedding/machine-protocol transport;
- parse exact hexadecimal/binary integer literals with MATLAB class suffixes,
  and share one strict decimal/complex text grammar across array-aware
  `double(string)` and registry-backed `str2double`;
- define conversion, rounding, saturation/overflow, mixed-class promotion,
  comparison, concatenation, indexing, assignment, reduction, and diagnostic
  rules against the selected MATLAB reference release;
- deliver the first coherent pure-math tranche, including elementary binary
  and unary functions, rounding and finite predicates, numeric/type/shape
  predicates, `mod`/`rem`, `nextpow2`, `isequaln`, `eps`,
  `real`/`imag`/`conj`/`isreal`, and related constructors;
- preserve double-specialized typed/native execution and add explicit guards,
  deoptimization, and VM equivalence for every newly legal class before wider
  typed lowering is attempted.

The host-foundation slice is implemented in the v1.2 candidate. One shared
runtime output model now serves `disp`, `fprintf`, and `sprintf`; script-level
output events and expression results carry one monotonic sequence; source
metadata classifies script/function/class units; and in-memory source can use
the production search-path loader. C API 1.2, the header-only C++ API, ordinary
CLI output, and machine protocol 1.1 project that same authority. Focused
interpreter/VM/module/API/protocol tests, runnable CLI and embedding samples,
and relocated installed consumers cover the slice.

The numeric audit also removed a previously accepted failure for mixed
64-bit-integer arithmetic. `int64`/`uint64` arrays now combine with scalar
`double` without first rounding the integer through binary64; a portable dyadic
path owns operand order, implicit expansion, software-emulated binary80
rounding, `mod`/`rem`, division, integer conversion, non-finite values, and
saturation. Typed/native regions continue to reject these values before
mutation and use VM fallback.

Character/string, cell, struct, function-handle, and object values remain part
of the stable value model. The v1.2 correctness inventory includes first-class
shaped missing arrays and row-structured multidimensional cell literals, so
forms such as `[missing missing]` and `{'a'; 'b'}` use ordinary array/cell
construction rather than scalar-only exceptions. Large domain families require
separate storage and container semantics beyond a numeric class tag. The later
v1.5-v1.8 batches now provide datetime/duration, CSC sparse, table,
categorical, and timetable slices.

## v1.3: System And Broad Standard Library

**Status: implemented; candidate scope and current contract are recorded in
[v1.3.md](v1.3.md) and
[public-contract-v1.3.json](public-contract-v1.3.json). Subsequent v1.4-v1.8
internal batches are recorded below.**

v1.3 builds on the v1.2 host boundary and closes a substantial system-function
and general-library bundle:

- session-scoped output, workspace inspection/mutation, search paths and
  current directory, warnings, environment, clock/sleep, random state, and
  bounded filesystem services;
- permission-gated process execution and dynamic evaluation, with safe CLI
  defaults and explicit embedding capabilities;
- `who`/`whos`, `which`, `path`, `pwd`/`cd`, `dir`, `getenv`, `system`,
  `pause`, date/clock, computer/version, `evalc`, `evalin`, and `assignin`
  families;
- formatted text/output, ordering/set, array-construction and manipulation,
  number-theory, and random functions that do not require the later
  high-performance backend;
- publish enough host/session behavior for downstream command modules to adopt
  it without kernel-side service-specific shims.

System services are capability checked and testable with injected deterministic
adapters. The CLI may provide an explicit native host adapter, while embedded
sessions retain isolation and resource controls.

Fifteen v1.3 implementation batches are present in the candidate tree. A
session-owned `RuntimeSystemContext` now separates current-directory, path,
environment, filesystem read/write, process, clock, sleep, and random
capabilities behind an injectable host adapter. The CLI supplies the native
adapter; engine tests use a deterministic in-memory adapter. Implemented
vertical slices currently include command-form `clear`/workspace queries,
`pwd`/`cd`/`tempdir`, `path`/`addpath`/`rmpath`, `which`/`dir`/`exist`/`getenv`,
date/platform queries, `pause`/`system`, `rand`/`randn`/`randi`/`rng`, display
format state, and the bounded `fullfile`/`filesep`/`pathsep` plus
`fopen`/`fclose`/`fprintf`/`fscanf`/`fseek`/`ftell`/`frewind` file tranche. The
same tests cover HIR, bytecode, production fallback, denied capabilities,
deterministic adapters, resource limits, file lifetime, update-stream
barriers, Windows text translation positions, multidimensional results, and
exact 64-bit formatted input.

The second batch adds one shared text/ordering/collection layer rather than
engine-specific cases. It covers space-separated character literals,
`lower`/`upper`/`strtrim`, array-aware `num2str`, portable `strsplit` and
`regexp` subsets, N-dimensional `sort`, O(n log n) numeric/text `unique`,
shape-only missing sorting/set behavior, `iscell`, `cellfun`, `struct2cell`,
and `cell2struct`. `cellfun` uses the registry's synchronous dynamic invoker,
so anonymous, named, and builtin handles, multiple inputs/outputs,
`UniformOutput`, callback diagnostics, and `ErrorHandler` share HIR/VM call
semantics. `samples/standard_library_demo.m` and `standard_library_smoke`
cover exact integer, complex, N-dimensional, text, Cell, Struct,
callback-failure, 20,000-element ordering, and billion-element shape-only
missing boundaries.

The third batch adds one shared utility layer for `factorial`, `gcd`, `lcm`,
`isprime`, `primes`, `logspace`, one- to three-dimensional `meshgrid`, generic
N-dimensional `flip`/`flipud`/`fliplr`, UTF-16 `strfind`/`strrep`, and
session-random `randperm`. It generalizes beyond the originating differential
examples: dense class and documented scalar-expansion rules, exact 64-bit primality and
GCD paths, shape-only missing transforms, overlapping supplementary-character
search, string-array/Cell mapping, billion-range sparse permutations, and
cooperative resource/cancellation boundaries are tested in both baseline
engines. Integer-only `primes` limits and fractional/nonpositive `logspace`
point-count rules are covered independently of the imported examples.
`samples/utility_library_demo.m` exercises the same surface through HIR,
bytecode, and production modes.

The fourth batch closes the first dynamic-workspace vertical path rather than
special-casing the three externally observed names. `eval`, `evalc`, `evalin`,
and `assignin` share current/caller/base frame resolution, character/string
source conversion, zero/multiple-output calls, ordered capture, catch-source
execution, compile-versus-runtime workspace commitment, and projected source
diagnostics. Dynamic source is recompiled through the canonical frontend and
verified bytecode VM with the parent registry, session, execution control, and
typed-backend policy. Capability denial, nested evaluation, shaped arrays such
as `[missing missing]`, generated-name collisions, runtime-error side effects,
temporary-module handle escape, and runtime builtin shadowing through ordinary,
`end`, and `:` indexing are covered in both baseline engines. Nested callable
arguments inherit an enclosing array's `end`, while a concrete runtime array
shadow creates the nearer index context. Portable/native typed loops guard
same-named workspace values and fall back to bytecode.
MATLAB `!command` syntax is lexed as one physical command line and lowers to
the existing capability-gated `system` builtin rather than adding a new IR or
VM operation; syntax-level dispatch remains independent of a same-named
workspace variable. `samples/dynamic_workspace_demo.m` and
`samples/system_command_demo.m` run through HIR, bytecode, and production
entry points; builtin source contract 1.4 records 225 descriptors and 227
registered names.

The fifth batch makes warning state session-correct and extends the existing
file abstraction instead of adding engine-specific shortcuts. `warning` and
`lastwarn` now persist across repeated calls in one module session, reset with
that session, remain isolated for stateless calls, and follow MATLAB-like
implicit-output rules. `fgetl`/`fgets`, `feof`/`ferror`, and common numeric
`fread`/`fwrite` share one unread-suffix, physical-position, EOF/error, and
update-stream contract. Fixed-width logical/integer/single/double precisions,
exact 64-bit payloads, finite/Inf shapes, repeat blocks, byte skips, and
native/little/big-endian conversion are covered by deterministic adapter tests
and the HIR/bytecode/production file sample. Builtin source contract 1.5 records
231 descriptors and 233 registered names while retaining frozen 1.4 evidence.

The sixth batch closes the reusable host-injection boundary requested by
downstream embedding work. C ABI generation 2 revision 1 adds an opaque rooted
system-context handle, caller-sized options, retain/release/capability queries,
and context-bound stateless/session entry points. The C++ facade adds the same
contract as copyable RAII `SystemContext` and `SystemContextOptions`. The
native adapter validates root/current/temporary/search directories, rejects
resolved path and symbolic-link escapes, preserves random/path/file state
across a retained session, and keeps environment/process authority explicit
and host-wide. It is a deterministic path policy rather than an OS sandbox;
filesystem-link races require host process isolation. Source-tree and relocated
C/C++ consumers, exact 117-symbol validation, allocation-failure translation,
and a runnable C++ sample cover the public boundary without changing the
frozen v1.2 revision-0 snapshots.

The seventh batch closes the general local-filesystem management slice through
the same capability boundary. `isfile` and `isfolder` preserve string-array or
character-cell shape and map missing strings to false; `fileparts` preserves
character/string/cell container type without touching the host; `fileread`
performs bounded UTF-8 reads from the current directory or search path; and
`tempname` creates a nonmaterialized session-safe candidate. `mkdir`, `rmdir`,
`copyfile`, and `movefile` share zero-output diagnostic behavior and up to
three MATLAB-style status outputs, including recursive removal, source
wildcards, directory-copy contents, and directory-move nesting. Rooted tests
cover normal operations, independent source/destination escape rejection,
root/current-directory protection, and retained context state. The runnable
`samples/filesystem_management_demo.m` agrees through HIR, bytecode, and
production entry points. Builtin source contract 1.6 records 240 descriptors
and 242 registered names while preserving every earlier snapshot.

The eighth batch brings forward the portable part of the advanced numerical
train where it already has useful vertical coverage. One repository-owned
C++20 backend implements dense LU/QR solves, Hermitian and general
eigensystems, FFT/IFFT, convolution, statistics, norms, rank, polynomial fit,
and related functions without depending on Eigen. HIR and bytecode share the
same implementation; typed/native-ineligible calls retain guarded VM fallback.

The ninth batch closes four high-value dynamic language gaps together. Cell
case expressions in `switch` test their elements in order. Nested functions
receive qualified lexical identities, exact semantic free-variable capture,
shared parent-frame updates, sibling lookup, multi-level capture, and named
handle invocation while the parent remains active. Bytecode Cell brace reads
carry requested output arity so comma-separated contents expand consistently
in destructuring, calls, and literals. The shared lvalue transaction seeds an
undefined indexed Struct root, grows gaps, aligns schemas, and supports nested
or dynamic fields. Escaping a nested-function handle after its lexical parent
returns remains an explicit unsupported closure-lifetime boundary.
`samples/dynamic_language_semantics_demo.m` and
`dynamic_language_semantics_smoke` cover both baseline engines and production
fallback.

The tenth batch closes the four remaining class/reflection/event differential
cases as one runtime slice. Literal class names passed to enumeration and
metadata/member queries enter the source graph uniformly. `enumeration`
returns an N-by-1 same-class object array plus an N-by-1 names Cell. Anonymous
root calls inherit implicit, zero, or explicit-one output context, allowing
zero-output listener methods without weakening assigned-call diagnostics.
Shared `strcmp`/`strcmpi` accepts shape-preserving Cell arrays of text scalars
with scalar expansion, so reflection lists use the same comparison contract in
HIR and bytecode. Focused tests and runnable enumeration/event samples cover
the generalized mechanisms; full differential results remain the closure
authority.

The eleventh batch closes the first portable workspace-persistence vertical
slice through `save` and `load`. One repository-owned MAT v5 codec preserves
all dense numeric classes, logical values, complex double/single arrays,
UTF-16 character arrays, N-dimensional Cell arrays, and Struct arrays. The
zero-output `load` form commits a fully decoded selection transactionally to
the current workspace, while one-output `load` returns a scalar Struct without
mutating that workspace. Compressed and uncompressed files share the same
resource-bounded parser; checked-in miniz supplies only DEFLATE compression
and is not a numeric or runtime dependency. Hand-built endian fixtures,
malformed/resource-limit tests, HIR/bytecode/production samples, and two-way
MATLAB R2024b compressed/uncompressed interchange validate the format boundary.
Builtin source contract 1.8 records 259 descriptors and 261 registered names.
MAT v4/v7.3, strict v6 output, append/ASCII modes, sparse/table/object values,
and arbitrary object persistence remain explicit later boundaries.

The twelfth batch closes the file deletion and metadata convenience vertical
slice. Source contract 1.9 contains 260 descriptors and 262 registered names:
text `delete` is a capability-bound system builtin while object, listener, and
dynamic-property deletion retain their VM lifecycle intrinsic. `fileattrib`
implements query/status/display forms, basename wildcards, Windows
archive/system/hidden/write flags, UNIX write/execute user scopes, and
recursive updates without following symbolic links. `dir.datenum` now carries
the local MATLAB serial modification date instead of a placeholder. In-memory
adapter tests, rooted C++ API escape/link tests, object-lifecycle regressions,
and `samples/file_metadata_demo.m` cover HIR, bytecode, and production paths.
Recycle-bin integration, parent-component wildcards, selectable link policy,
and broader platform-specific attributes remain later boundaries.

The latest MATLAB R2024b external differential rerun at
`MParserV1.0Test/results/20260824-232112-v1.7-table-final` records 222
matches and one gap across all 223 MATLAB-accepted cases, with no prior match
regressing. Datetime, table, and sparse now pass; graphics is the only imported
gap. The thirteenth batch closes dynamic
parent-module local, nested, path, package, private, and module-bound-handle invocation across HIR
and bytecode parents, including recursive evaluation, output capture, captured
workspace updates, and source-scoped private isolation. MATLAB R2024b reference
probes confirm that function and class definitions are themselves illegal in
`eval`; the fourteenth and fifteenth batches subsequently closed dynamic
storage declarations and the planned conversion/callback/set/text-query
standard-library slice. Scansets,
bit/character/complex binary-I/O corners,
selectable non-UTF-8 encodings, remote URLs, extended MAT variants, full
MATLAB regular-expression syntax, locale-wide Unicode case conversion, and
long-tail overloads such as extended GCD coefficients are also not implied by
these slices.

The fourteenth batch closes the dynamic storage-declaration gap without
creating a second execution model. A short-lived source-storage bridge binds
`global` and `persistent` declarations in evaluated text to the selected real
current/caller/base frame. Persistent state uses compiled callable identity,
survives compile-once/invoke-many sessions, and rejects script use or static
nested workspaces; global-after-local behavior is warning-bearing and preserves
existing-global precedence. Nested eval, ordinary-error side effects, named and
whole-workspace clear association, and HIR/bytecode parent equivalence have
focused coverage. Temporary-module handle rollback now includes session
globals, persistent variables, and shared objects reachable only through those
stores. `samples/dynamic_workspace_demo.m` exercises the supported path in HIR,
bytecode, and production modes. The fifteenth batch below closes the remaining
planned v1.3 standard-library breadth; explicitly recorded system/text/I/O
boundaries stay assigned to later milestones rather than this workspace
contract.

The fifteenth batch adds source contract 1.10 with 275 descriptors and 277
registered names. Conversion coverage now includes `int2str`, class-aware
`mat2str`, isolated numeric `str2num`, N-dimensional `num2cell`/`cell2mat`,
and `iscellstr`. Exact fixed-width formatting does not round through `double`;
class-preserving wide-integer text uses MATLAB-readable `s64`/`u64` literals,
and `cell2mat` assembles variable-segment rectangular N-dimensional grids.
`str2num` parses a restricted expression graph and admits
only deterministic pure registry calls; it cannot assign, reach caller
variables, or invoke workspace/system services. `arrayfun` shares the dynamic
invoker, exact input-shape rule, multi-output behavior, `UniformOutput`, and
`ErrorHandler` contract with direct calls. `ismember`, `union`, `intersect`,
`setdiff`, and `setxor` preserve numeric classes and MATLAB column-major
shapes, support element or numeric/character `rows` operation, sorted/stable
ordering, first indices, Cell text, string missing, complex values, and the
MATLAB rule that NaN or missing set elements do not match. `contains`,
`startsWith`, and `endsWith` accept multiple patterns and `IgnoreCase` while
preserving string/Cell input shape. One runnable sample and paired HIR/bytecode
tests cover normal, malformed, exact 64-bit, variable-block N-dimensional,
20,000-element, payload-free billion-element, and cancellation paths. Legacy
set ordering, Pattern objects, locale-wide Unicode case folding, and broad
long-tail conversion overloads remain explicit later work.

## v1.4: Advanced Mathematics And Typed Performance

v1.4 groups the heavier numerical families with the optimization work needed
to make them useful:

- statistics and reductions, sort/unique variants, dot/cross and matrix norms,
  determinant/inverse/rank/eigenvalue families, convolution, and FFT;
- a canonical repository-owned C++ implementation for dense linear algebra,
  polynomial fitting, and FFT, without an Eigen dependency; any later optional
  BLAS/LAPACK/FFT acceleration must document license, threading, determinism,
  feature detection, and preserve this portable semantic fallback;
- the same repository-owned native C++20 rule applies to every new core
  mathematical function: Eigen must not be linked, vendored, or copied;
  published algorithms or external implementations may inform design, while
  MParser retains its own implementation, provenance, and regression tests;
- representative MATLAB/MParser benchmarks covering parse/compile/cold/warm,
  bytecode/portable/native, allocation, peak memory, cache, and fallback;
- straight-line numeric regions, general dense-array shape/stride guards,
  element-wise fusion, common reduction lowering, and guarded function-call
  specialization where profiling evidence shows value.

Every optimized family must pass native/no-JIT result equivalence, guard
failure and transactional fallback tests, and applicable x64/ARM64 validation.
Optimization coverage may improve release value, but cannot turn a legal VM
program into an error.

The first implementation batch is recorded in [v1.4.md](v1.4.md). It adds a
versioned seven-workload suite and uses its largest uncovered call-overhead
signal to add guarded pure local scalar-function specialization through the
existing portable/SLJIT kernel. The same suite keeps straight-line scalar,
dense element-wise, reduction, and dense-linear-algebra coverage visibly
measured. The second implementation batch closes the first high-value array
slice with guarded N-dimensional element-wise fusion for floating
`double`/`single` values, real/complex portable execution, MATLAB
column-major implicit expansion, exact/profiled versus dynamic
type/complexity/shape guards, and source-contract-1.12 terminal `sum`, `prod`,
and `mean` lowering. Native lowering remains restricted to real-double exact-shape
element-wise kernels and scalar reductions, with native-to-portable or VM
fallback preserving results. The measured Dense workload moves from about
79.7 ms in bytecode to 4.05 ms in portable execution on the recorded host.
Straight-line scalar, nested reductions, general matrix kernels, broader
numeric classes, and dense linear algebra remain visible candidates; the v1.4
milestone stays open and the project version is not advanced for either
internal batch. A behavior-neutral source-ownership batch also places the VM
implementation under `execution/bytecode/vm` and the CLI entry point under
`cli`, with forwarding headers and a layout smoke preserving the existing
internal include boundary; larger monolith splits remain separately gated.

## v1.5: Rich Data, Ownership, And Inspection

v1.5 is organized as complete rich-data vertical slices rather than isolated
class-name registrations. Every slice must define RuntimeValue storage,
shape/index behavior, construction and conversion, BuiltinRegistry metadata,
interpreter/VM parity, embedding and machine-protocol limits, a runnable
sample, and applicable x64/ARM64 evidence.

The first batch is recorded in [v1.5.md](v1.5.md). It adds native C++
`datetime`/`duration`/`NaT` values, shaped component and unit operations,
temporal arithmetic/comparison, ISO-like formatting, and owner-independent C
API transport. It advances the builtin source contract to 1.13 with 291
descriptors and 293 registered names. Temporal values intentionally remain in
the VM/portable semantic path; no Typed/JIT lowering is claimed, and the
machine protocol continues to expose ordinary objects as opaque values.

The next batches close the remaining class and ownership boundaries that fit
the existing contracts. The sparse numeric first batch is recorded in
[v1.6.md](v1.6.md), and the table runtime first batch is recorded in
[v1.7.md](v1.7.md). The full 223-case differential rerun confirms temporal,
sparse, and table closure with graphics as the sole imported gap. The v1.8
batch recorded in [v1.8.md](v1.8.md) then adds a complete categorical vertical
slice, closes table row/multi-variable/concatenation/sorting gaps, and adds the
first timetable RowTimes/conversion slice on shared tabular storage. Advanced
tabular algorithms and debugger stack/local inspection remain additive,
separately gated work.

## v1.6+: Remaining Semantics And Deeper Optimization

Remaining in-scope parser, extended-persistence, nested-function, dynamic-source-graph,
data-family, and standard-library gaps continue in complete functional bundles.
After the v1.4 baseline, later optimization candidates are numeric-type-aware
lowering, copy-on-write/alias and temporary-buffer reuse, x86 SIMD/ARM NEON,
CPU dispatch, loop-invariant and induction optimization, advanced tiering,
OSR, parallel kernels, optional GPU execution, and persistent native cache.

LLVM, broad object speculation, OSR, GPU execution, and disk machine-code cache
remain evidence-gated and do not displace language or function correctness.

## External Extension Boundary

A later v1.x milestone may introduce a narrow versioned C callback adapter for
compiled external builtins. It must define:

- descriptor/version negotiation and capability discovery;
- copied versus owned array/value transfer and optional view rules;
- callback lifetime, unload, reentrancy, thread safety, and cancellation;
- deterministic exception/diagnostic conversion;
- namespace, precedence, shadowing, purity, side-effect, and JIT eligibility;
- conformance tests against the current headers and independently built
  modules once that adapter reaches a release-candidate contract.

No internal Parser, HIR, Bytecode, VM, or `RuntimeValue` layout becomes ABI as
part of this work.

## Later v1.x Candidates

Subject to demand and architecture fit after the scheduled core milestones:

- additional sparse formats and richer domain value types beyond the v1.5
  priority set;
- broader class metadata and reflection inside an explicit compatibility
  boundary;
- package/distribution tooling for reusable `.m` libraries;
- coverage-guided fuzz corpus management and longer stress profiles;
- authenticated hosted provenance, SBOM integration, and stronger release
  policy automation;
- additional operating systems or architectures backed by maintained
  toolchains and recurring evidence.

True MATLAB parity, MATLAB proprietary toolbox reimplementation, desktop UI,
Simulink, MEX binary compatibility, and unrestricted Java/.NET integration
remain outside the v1.x commitment unless the project explicitly adopts a new
scope.
