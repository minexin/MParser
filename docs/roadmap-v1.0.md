# MParser v1.0 Roadmap

## Release Definition

MParser v1.0 is a stable, documented, embeddable, and sustainably extensible
MATLAB-like subset runtime. It is not a claim of complete MATLAB compatibility.

The subset qualification is not permission to leave foundational syntax or
runtime semantics ambiguous. Before v1.0, MParser must finish the language and
engine contracts required by its declared subset. It must also make ordinary
function extension routine enough that adding a conventional function after
v1.0 normally does not require redesigning Parser, HIR, Semantic, Bytecode, VM,
or `RuntimeValue`.

Long-tail function-library growth is a v1.x activity that will continue well
after v1.0. The v1.0 gate is a stable extension system proven by representative
function families, not an unbounded builtin count.

## Priority Labels

- **Must-have** is a v1.0 release gate. Missing evidence blocks the release.
- **Should-have** materially improves the release but may be deferred only with
  a documented compatibility-preserving extension point and rationale.
- **Post-v1.0** is intentionally outside the v1.0 gate and must not silently
  expand the release scope.

## A. Language And Runtime Boundary

### Must-have

- Publish a machine-maintained compatibility matrix covering syntax, semantic
  analysis, execution tiers, diagnostics, platform notes, and explicit
  unsupported behavior. Every supported claim must point to executable tests.
- Close high-frequency gaps inside the target subset, including dynamic
  call/index disambiguation, array and type behavior, builtin invocation,
  control flow, functions, exceptions, workspace effects, and source locations.
- Define the supported numeric, logical, character, string, Cell, and object
  shape/index/assignment rules, including scalar expansion, linear and
  N-dimensional indexing, deletion, multiple outputs, `nargin`, and `nargout`.
- Stabilize function, local/private/path/package dispatch, function handles,
  dynamic calls, name shadowing, and error behavior across the source graph.
- Finish the declared `classdef` subset: value/handle identity, constructors,
  inheritance, properties, methods, access, events, metadata/reflection,
  dynamic properties, listeners, and explicit object lifecycle.
- Stabilize the Lexer -> Parser -> HIR -> Semantic -> Bytecode -> VM/source
  graph chain and its contracts for `RuntimeValue`, shape, indexing, call
  frames, output arity, diagnostics, and source spans.
- Record unsupported MATLAB features as deliberate matrix entries rather than
  accepting them partially or failing without a stable diagnostic.

### Should-have

- Reduce differences between the reference HIR interpreter and bytecode VM for
  the pure/function subset, or explicitly retire duplicate behavior where one
  implementation cannot remain contract-equivalent.
- Add differential fixtures against MATLAB or Octave where licensing and
  semantics permit, with tolerances and expected incompatibilities documented.
- Expand reflection and dynamic-dispatch coverage beyond the minimum subset
  when it does not destabilize the frozen object/runtime representation.

### Post-v1.0

- Complete MATLAB language compatibility, Live Scripts, P-code, MEX loading,
  Simulink, toolboxes, and undocumented MATLAB behavior.
- Every specialized array storage class, graphics object, table/timetable, or
  domain object not selected by the v1.0 compatibility matrix.

## B. Typed Execution And JIT

### Must-have

- Preserve one semantic authority: legal code unsupported by typed or native
  lowering must execute correctly in the bytecode VM.
- Freeze guarded region entry, transactional commit, deoptimization/fallback,
  cache ownership, diagnostics, and backend-selection contracts.
- Keep portable and SLJIT-native backends behaviorally equivalent for every
  advertised typed region on Windows, Linux, x64, and ARM64 targets.
- Define typed eligibility in stable metadata rather than scattering backend
  assumptions through semantic analysis and VM dispatch.
- Maintain correctness tests that force JIT off, portable typed execution,
  native execution, guard failure, fallback, cache hit, and cache invalidation.

### Should-have

- Broaden typed regions beyond closed loops where profiling shows value,
  including straight-line numeric regions and more general arrays/matrices.
- Add representative typed builtin lowering, function-call specialization,
  direct bounds checks, reduced `RuntimeValue`/VM dispatch overhead, and
  vector/SIMD kernels where portable behavior remains clear.
- Establish repeatable performance and memory baselines that distinguish
  compilation, warm execution, cache behavior, and end-to-end wall time.
- Define source, target architecture, ABI, compiler, option, and MParser-version
  invalidation keys before exposing any persistent native-code cache.

### Post-v1.0

- On-stack replacement, speculative whole-program optimization, tiered LLVM
  ORC integration, GPU lowering, and broad automatic vectorization unless a
  later audit proves one is essential to the declared v1.0 performance floor.
- Performance parity with MATLAB across arbitrary scripts and toolboxes.

## C. Function Extension, Embedding, And Persistence

### Must-have

- Replace distributed builtin name/signature switches with one
  `BuiltinRegistry`/`BuiltinDescriptor` system or an equivalent single source
  of truth.
- Describe each builtin's name, aliases, input/output arity, type and shape
  constraints, purity, determinism, side effects, workspace/context access,
  error contract, and typed/JIT eligibility.
- Introduce one shared `BuiltinCall`/`BuiltinResult` contract or equivalent over
  `RuntimeValue`, supporting zero, one, and multiple outputs, diagnostics,
  function handles, dynamic invocation, and source context.
- Distinguish pure functions, context/workspace functions, and VM intrinsics.
  Only true control/runtime primitives remain VM-specific.
- Migrate representative families through the registry: scalar mathematics,
  reductions, scans, array operations, at least one multiple-output builtin,
  and at least one context/workspace builtin. Verify interpreter, VM, portable
  typed fallback, and native paths wherever each path applies.
- Document three extension levels: `.m` functions, built-in C++ functions, and
  a future external C/C++ adapter/ABI. Specify naming and shadowing, lifecycle,
  thread safety, array ownership/copy/view rules, exception conversion, and
  version compatibility.
- Freeze an embeddable C++ API and narrow C ABI for compile/load, invoke many
  times, pass scalar/array/object values, collect multiple outputs, inspect
  diagnostics, and release resources.
- Provide a versioned machine-readable CLI/result protocol separate from human
  display output, with stable success, diagnostic, value, and version fields.
- Make compiled-module and persistent-runtime lifetimes explicit, including
  source ownership, workspace isolation, handle cleanup, limits, cancellation,
  and cross-thread rules.

### Should-have

- Provide a validated external adapter prototype without promising a fully
  open plugin ABI until ownership, exception, and version rules are proven.
- Add an adaptive/native disk cache only if it has atomic storage, bounded
  resources, corruption recovery, and complete source/architecture/ABI/version
  invalidation. Otherwise keep the v1.0 cache process-local and document it.
- Offer zero-copy or view-based arrays where lifetime and mutability can be
  expressed safely; copying remains the compatibility fallback.

### Post-v1.0

- A broad third-party binary plugin ecosystem, stable C++ compiler ABI across
  arbitrary toolchains, distributed execution, and transparent remote caches.
- Large-scale long-tail builtin and toolbox accumulation after the registry and
  authoring rules have passed the v1.0 gate.

## D. v1.0 Engineering Closure

### Must-have

- Declare and validate a platform matrix covering Windows, Linux, macOS, x64,
  and ARM64. Every advertised tuple must have native or documented emulated
  configure, build, test, install, and sample evidence.
- Run parser/semantic fuzzing, malformed-bytecode checks, long-running runtime
  stress, repeated module invocation, cache churn, handle/listener lifecycle,
  and resource-limit/cancellation tests.
- Establish CPU, wall-time, peak-memory, allocation, binary-size, cold-start,
  and warm-cache baselines for representative scripts. Performance claims must
  identify hardware, build type, backend, and timing boundary.
- Define resource and failure boundaries: recursion/call depth, array size,
  instruction or time budget, cancellation, allocation failure, diagnostics
  volume, and host exception containment.
- Ship installable packages and consumer validation for every supported OS,
  including headers/libraries for embedding, CLI runtime assets, licenses,
  version metadata, and reproducible build instructions.
- Publish a user manual, CLI/API reference, compatibility/support matrix,
  embedding guide, extension-author guide, diagnostics guide, and release
  migration policy.
- Freeze the public CLI, C++ API, narrow C ABI, machine protocol, registry
  descriptor contract, and compatibility policy before tagging v1.0.
- Require every meaningful milestone to include implementation, broad
  regression, a runnable sample, README/architecture/milestone documentation,
  and cross-platform CI evidence.

### Should-have

- Add sanitizer jobs, coverage trend reporting, reproducible benchmarks, and
  deterministic artifact provenance to release CI.
- Test compatibility with multiple supported compiler versions and host
  embedding processes, including repeated load/unload where the platform
  permits it.

### Post-v1.0

- Certification for safety-critical or hard real-time use, all historical OS
  versions, every compiler/standard-library combination, and every CPU ISA.

## Stage Gates

### v0.70: Subset And Engine Audit

- Publish the first compatibility matrix and source-linked gap inventory.
- Close mandatory target-subset syntax/semantic/runtime gaps found by the audit.
- Prove there is no known required feature that forces replacement of the main
  IR, bytecode, call-frame, indexing, object, diagnostic, or source-graph model.
- Classify every unresolved item as v0.80/v0.90/v1.0, Should-have, or Post-v1.0.

### v0.71: Structure Array And List Foundation

- Replace scalar-only structure storage with one schema-preserving array
  representation shared by both baseline engines.
- Implement representative construction, indexing, whole-element mutation,
  growth, deletion, and comma-separated field-result contexts.
- Preserve VM correctness through typed/native fallback and record deeper
  nested lvalue mutation as a separate framework boundary.

### v0.72: Root-And-Path Lvalue Transactions

- Complete one shared root-and-path transaction for mixed member,
  parenthesis, and brace mutation across numeric arrays, Cells, structures,
  and scalar value/handle objects.
- Evaluate dynamic names and subscripts once, copy updated values back through
  every parent, commit only the root, and preserve VM try/catch recovery.
- Keep direct numeric indexed stores available to typed/native recognition;
  all dynamic path opcodes conservatively fall back to the bytecode VM.
- Validate rollback, deletion, growth, structure schema extension, and
  value/handle copy semantics through HIR, bytecode, production, and AArch64
  focused tests.

### v0.73: Exception And Diagnostic Contracts

- Expose complete N-by-1 `MException.stack` arrays and embedding-visible
  source-graph frames shared by the HIR interpreter and bytecode VM.
- Stabilize cause chaining, basic/extended reports, throw/rethrow/
  throwAsCaller policies, warning severity/state/lastwarn, catchable assert,
  and the explicit unsupported correction boundary.
- Keep warnings nonfatal across CLI, adaptive execution, benchmarking, and
  workspace publication, while legal exception-heavy code remains a VM
  fallback outside typed/native regions.
- Close `G-EXC-001` with differential runtime, cross-file CompiledModule, CLI
  sample, compatibility-matrix, and AArch64 focused evidence.

### v0.74: Dynamic Call And Function Handle Contracts

- Replace VM-local handle identifiers with shared callable descriptors and a
  stable per-module runtime context, allowing source-backed handles to survive
  repeated invocations of their owning `CompiledModule`.
- Align HIR and bytecode behavior for anonymous/named/builtin handles,
  call-versus-index dispatch, `feval`, `str2func`, `func2str`, `functions`,
  output arity, closure snapshots, and diagnostics.
- Define literal text dependency discovery and deterministic
  variable/path/package/private/builtin shadowing without granting text calls
  private lexical access or loading computed targets implicitly.
- Close `G-CALL-001` with differential engine tests, CompiledModule lifetime
  tests, source-graph precedence tests, and one sample checked through every
  public execution mode.

### v0.75-v0.79: Remaining Runtime Foundations

- Close the audited Must-have text, object-array/reflection, workspace, and
  remaining runtime-value boundaries with linked compatibility evidence.
- Freeze the engine-facing value, shape, indexing, call-frame, output-count,
  diagnostic, and fallback contracts needed by the v0.80 extension layer.

### v0.80: Function Extension Infrastructure

- Land the unified builtin registry, descriptor, call/result, and error model.
- Migrate representative math, reduction, scan, array, multi-output, and
  context builtins with cross-tier consistency tests.
- Publish the extension-author rules and a reusable builtin conformance-test
  template.

### v0.90: Embedding And Release Candidate APIs

- Stabilize compile-once/invoke-many C++ and narrow C interfaces plus the
  versioned machine protocol.
- Validate representative function families, persistent runtime behavior,
  resource boundaries, cross-platform consumers, and cache invalidation.
- Freeze v1.0 API/ABI/protocol candidates and require explicit compatibility
  review for every later change.

### v1.0: Contract Freeze And Release

- Satisfy every Must-have item with linked tests, artifacts, and documentation.
- Publish the final support matrix, packages, manuals, extension guide, and
  performance/resource characterization.
- Freeze public CLI/API/extension contracts under a documented versioning and
  deprecation policy.
- Move unbounded long-tail function growth and explicit Post-v1.0 items into
  the v1.x roadmap without blocking the v1.0 release.

## Gate Evidence

A milestone is complete only when its claimed behavior is demonstrated by
current source, focused tests, broad regression, a runnable sample, updated
documentation, and the applicable platform CI. Plans, passing unrelated tests,
or the absence of a known bug are not completion evidence.
