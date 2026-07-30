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
- Keep the v1.0 adaptive/native cache bounded and process-local. A future disk
  cache is additive and may ship only with atomic storage, bounded resources,
  corruption recovery, and complete source/architecture/ABI/compiler/options/
  version invalidation.
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

### v0.75: Character And String Runtime Contracts

- Separate `CharacterArray` and `StringArray` storage behind a shared,
  engine-facing `RuntimeValue` header with explicit UTF-16 payloads and UTF-8
  boundaries.
- Align HIR and bytecode behavior for text shape, emptiness, indexing, brace
  access, assignment, growth, deletion, concatenation, implicit expansion,
  conversions, formatting, missing strings, and representative builtins.
- Route array transforms, diagnostics, metadata, dynamic calls, validated class
  properties, and typed/native fallback through the same text contract.
- Close `G-TEXT-001` with differential runtime tests, all public CLI modes,
  a runnable sample, portable-only validation, and focused AArch64 evidence.

### v0.76: Object Array Runtime Contracts

- Add one shared object-array storage and operation boundary for value and
  handle classes, with column-major indexing over portable row-major payloads.
- Execute indexed selection, assignment, default-filled growth, deletion,
  concatenation, array transforms, value copy-back, handle identity, explicit
  lifecycle operations, and object-aware display.
- Support `matlab.mixin.Heterogeneous` class roots and most-specific common
  superclass resolution without silently mixing ordinary unrelated classes.
- Close `G-CLASS-001` with direct runtime tests, end-to-end bytecode tests, a
  production sample, typed/native fallback evidence, portable-only validation,
  and focused AArch64 execution.

### v0.77: Reflection Metadata Contracts

- Centralize the supported metadata type graph, canonical and legacy names,
  direct inheritance, class flags, and public members in one runtime schema.
- Support logical-order metadata-array projection and direct `findobj`
  property filters over inherited, private-owner, static, and dynamic
  descriptors.
- Expose property validation metadata, executable validation probes and
  checks, callable module-bound validator handles, and explicit unsupported
  member diagnostics.
- Close `G-REFLECT-001` with a compatibility table, direct schema tests,
  end-to-end VM tests, a production sample, portable-only validation, and
  focused AArch64 execution.

### v0.78: Workspace And Runtime Session Contracts

- Add scoped `global` and per-function `persistent` declarations across
  Parser, HIR, semantic bindings, bytecode, HIR execution, and VM execution.
- Route shared loads and every supported assignment form through one
  `RuntimeSessionState`, with deterministic empty initialization, snapshots,
  targeted clearing, reset, and explicit concurrency boundaries.
- Add owning `CompiledModuleSession` compile-once/invoke-many state and make
  module sessions, adaptive sessions, and function handles retain immutable
  compiled artifacts independently of the original module object's lifetime.
- Keep shared bindings outside typed/native regions and prove VM fallback,
  differential engine behavior, session isolation, lifetime safety, runnable
  samples, portable-only execution, and focused AArch64 execution.

### v0.79: Remaining Runtime Foundation Freeze

- Separate `RuntimeValue`, `RuntimeWorkspace`, value factories, ownership,
  rendering, handle metadata, and recursive invariant validation from both
  baseline engines.
- Use one `RuntimeCallFrame` contract for HIR and VM script, function,
  anonymous-function, and initializer execution, including canonical
  `nargin`/`nargout` initialization.
- Propagate structured fallback kinds through region analysis, portable typed
  execution, native SLJIT, VM profiles, adaptive events, and CLI detail output.
- Close `G-RUNTIME-001` with copy/alias/cycle tests, malformed-value tests,
  2,048 compile-once/invoke-many calls, three-engine sample parity, linked
  compatibility evidence, and no known target-subset feature requiring
  replacement of the main IR or VM contracts.
- Treat this as an internal source-contract freeze for v0.80. Public C++ ABI,
  narrow C ABI, serialization, and the machine protocol remain v0.90 gates.

### v0.80: Function Extension Infrastructure

- Status: closed by the v0.80 registry implementation and linked evidence in
  `docs/v0.80.md` and `BUILTIN-002`.
- Land the unified builtin registry, descriptor, call/result, and error model.
- Migrate representative math, reduction, scan, array, multi-output, and
  context builtins with cross-tier consistency tests.
- Publish the extension-author rules and a reusable builtin conformance-test
  template.
- Preserve `.m` functions as the smallest extension level, define the
  source-level C++ registry path, and reserve the narrow external C/C++ adapter
  and stable ABI for v0.90.
- Keep broad long-tail function growth in v1.x; v0.80 proves that ordinary
  additions no longer require parallel semantic, HIR, VM, and JIT name lists.

### v0.81: Engine-Neutral Embedding Execution

- Status: closed by the v0.81 request/result implementation and linked evidence
  in `docs/v0.81.md` and `EMBED-003`.
- Add one request/result contract shared by stateless `CompiledModule` and
  persistent `CompiledModuleSession` execution.
- Separate compilation, validation, warning, and runtime-failure diagnostics
  from bytecode profiles and preserve owned source/cause information.
- Cache registry-aware static Typed IR in each compiled module and expose
  automatic, bytecode, portable, and native backend preferences with mandatory
  VM fallback.
- Reject malformed arguments/workspaces before execution and preserve the old
  VM-specific APIs as compatibility paths.
- Defer public binary layout, external value ownership, resource controls,
  C ABI, machine protocol, install/export consumers, and final freeze to later
  embedding milestones.

### v0.82: Resource-Controlled Embedding

- Status: closed by the v0.82 execution-control implementation and linked
  evidence in `docs/v0.82.md`, `EMBED-004`, and `G-RESOURCE-001`.
- Add request-level instruction, steady-clock wall-time, call-depth,
  per-value recursive array-payload, and diagnostic limits plus copyable
  cross-thread cancellation.
- Make resource stops uncatchable terminal runtime failures with stable
  identifiers, stop reasons, high-water marks, and deterministic retention of
  one terminal diagnostic.
- Suppress typed/native regions for controls that require instruction polling,
  preserve optimized execution for compatible controls, and report the policy
  fallback explicitly.
- Expose declared execution-control context to cooperative C++ builtins,
  preserve ordinary host exception containment, and continue propagating
  `std::bad_alloc` until allocation-safe reporting is available.
- Prove exact termination, session reuse, non-rollback semantics, live
  cancellation, runnable host behavior, and focused native/portable AArch64
  execution.

### v0.83: Narrow C Embedding ABI

- Status: closed by the v0.83 shared-library implementation and linked
  evidence in `docs/v0.83.md`, `docs/embedding-c-api.md`, and `EMBED-005`.
- Add one pure C header and shared library with version/status queries,
  opaque retained module/session/result/value/cancellation handles, and no
  exposed C++ layout or exception.
- Project compile-once/invoke-many, staged diagnostics, multiple outputs,
  result workspaces, resource controls, cancellation, and summaries through
  the engine-neutral request/result boundary.
- Define MATLAB column-major external arrays, copied numeric/logical/UTF-16/
  Cell/Struct inputs, and returned object/function-handle lifetimes.
- Retain producing modules for module-bound values, reject cross-module use,
  and preserve independent builtin handles across modules.
- Prove the boundary with a C11 regression and runnable C sample in Windows,
  Linux, native AArch64, and portable AArch64 jobs.
- Keep ABI candidate 1 explicitly pre-freeze; multi-source loading, machine
  protocol, install/export consumers, optional views/adapters, and final
  compatibility policy remain v0.90 gates.

### v0.84: C Source Graph Embedding

- Status: closed by the v0.84 source/load implementation and linked evidence
  in `docs/v0.84.md`, `docs/embedding-c-api.md`, and `EMBED-006`.
- Add versioned, copied C source descriptors for deterministic in-memory
  multi-source compilation with retained source order and diagnostics.
- Add a UTF-8 entry/search-path loader over the shared `SourceLoader`, covering
  ordinary/private functions, packages, class folders, separated methods, and
  discovered source dependencies without exposing internal `SourceUnit`
  metadata.
- Return inspectable invalid modules for stable source-load failures and expose
  borrowed module source enumeration under the existing ownership model.
- Prove source-buffer independence, cross-source diagnostics, real
  `+package/@Class` loading, native/no-JIT parity, and focused AArch64
  native/portable execution with a C11 regression and runnable C host.
- Keep ABI candidate 1 pre-freeze; install/export consumers, machine protocol,
  forward-compatible layout/symbol policy, stress, and final platform
  evidence remain later gates.

### v0.85: Installable C SDK

- Status: closed by the v0.85 install/export implementation and linked
  evidence in `docs/v0.85.md`, `docs/embedding-c-api.md`, and `EMBED-007`.
- Install the C header, shared runtime, CLI, examples, documentation, and
  third-party notices using standard GNU directory variables.
- Export relocatable `MParser::c_api` and `MParser::cli` CMake targets plus
  package, project-version, and C ABI metadata.
- Prove isolation with a separate C11 project configured only against a moved
  installation prefix; require version/ABI parity, multi-output execution, and
  imported CLI execution.
- Validate Windows x64 and Linux x64 through the complete native suite, and
  validate both native-JIT and portable Linux AArch64 installations through
  explicit cross-consumer builds and QEMU execution.
- Keep public C++ packaging, machine protocol, final ABI policy, macOS
  consumers, distribution licensing, and release archives as later gates.

### v0.86: Versioned Machine Result Protocol

- Status: closed by the v0.86 protocol implementation and linked evidence in
  `docs/v0.86.md`, `docs/machine-result-protocol.md`, and `EMBED-008`.
- Add `--result-format=json-v1` to production `--run` as a separate automation
  channel over `ModuleInvocationResult`, without changing human output.
- Define stable status-specific process exits, one-document stdout, staged
  diagnostics, complete execution summaries, and deterministic CLI/source-load
  failure projection.
- Serialize every current RuntimeValue kind with MATLAB column-major arrays,
  round-trip finite doubles, valid non-finite tokens, exact UTF-16 characters,
  missing strings, recursive composite values, stable function descriptors,
  and opaque objects.
- Freeze protocol major 1 spelling with an all-types golden result plus parsed
  success, compilation, validation, runtime, source-load, and CLI regressions.
- Include native and portable AArch64 protocol execution while leaving public
  C++ packaging, final C ABI policy, macOS consumers, stress, and release
  archives for later v0.90 work.

### v0.87: Evolvable C ABI Candidate

- Status: closed by the v0.87 ABI implementation and linked evidence in
  `docs/v0.87.md`, `docs/c-abi-compatibility.md`, and `EMBED-009`.
- Separate ABI major, additive revision, and engine versions without changing
  the existing ABI-major-1 query or symbols.
- Add caller-sized initialization for extensible request, execution-summary,
  and source-load roots; freeze old initializer write ranges to the v1 prefix.
- Accept known prefixes and ignore unknown input tails, bound output writes by
  caller capacity, and seal array-strided source-unit descriptors.
- Compile a real consumer against the frozen v0.86 header snapshot, execute
  future-tail request/load/summary paths, and ship a runnable ABI sample.
- Include relocated installed-consumer ABI `1.1` checks plus focused native
  and portable AArch64 evidence.

### v0.88: Installable C++ Embedding SDK

- Status: closed by the v0.88 SDK implementation and linked evidence in
  `docs/v0.88.md`, `docs/embedding-cpp-api.md`, and `EMBED-010`.
- Add a C++20 RAII facade over the narrow C ABI rather than exporting
  internal Parser/HIR/Bytecode/RuntimeValue layouts.
- Export an installed `MParser::cpp_api` target with compile/load/invoke-many,
  sessions, values, diagnostics, cancellation, and resource summaries.
- Prove source-tree isolation and relocation with an external C++ consumer on
  the release platform matrix.
- Preserve language failures as inspectable modules/results, reserve
  `ApiError` for host-boundary failures, and validate independent value/module
  ownership plus caller-defined Struct field order.

### v0.89: Embedding Stress And Platform Evidence

- Status: closed by the v0.89 implementation and linked evidence in
  `docs/v0.89.md`, the embedding guides, and `EMBED-011`.
- Add repeated load/unload and retain/release stress plus concurrent pure
  invocation, shared-handle mutation, session, cancellation, and independent
  resource-pressure tests.
- Serialize module-bound mutable graphs and conservative session operations,
  with a documented lock-admission and wall-time boundary.
- Add macOS x64/ARM64 build, test, production install, relocation, and C/C++
  consumer evidence.
- Assign shared-library ABI version `1.1.0`/SOVERSION 1, hide internal
  symbols, and machine-check the exact public C export manifest.
- Freeze v1.0 array transport at copy-in plus readonly runtime-owned output
  views. Defer a versioned pure-C external callback table to Post-v1.0.
- At milestone closure, redistribution remained disabled pending the
  project-owner license decision. The following v0.90 groundwork closes that
  gate with Apache-2.0 and `Copyright 2026 Wang Xin`.

### v0.90: Embedding And Release Candidate APIs

- Status: implementation complete. A v0.90 commit is closed only when its
  corresponding cross-platform Actions run satisfies every release lane.
- Freeze C ABI 1.1, C++ source API 1.0, and machine protocol 1.0 through one
  machine-readable public-contract manifest, immutable snapshots, normalized
  hashes, old-client consumers, layout checks, and an explicit change-review
  policy.
- Publish the protocol JSON Schema, exact stdout/stderr/newline framing,
  unsigned-64-bit integer policy, human-option rejection, and allocation-free
  emergency exit-4 result.
- Prove allocation/internal-failure containment, pre/post-execution commit
  boundaries, concurrent session mutation/clearing, and bounded native-cache
  churn. Keep the v1.0 native cache process-local.
- Produce checksummed platform/architecture archives, reproduce an archive
  from one fixed payload, unpack it, and consume only its C11, multi-TU C++20,
  and machine-protocol SDK surface.
- Run Linux Clang ASan/UBSan and upload release packages for Windows x64,
  Linux x64/AArch64, and macOS x64/ARM64.
- Require an explicit compatibility review for every later candidate change;
  v1.0 still owns final CLI/API policy, support/manual closure, fuzz/performance
  characterization, provenance, and publication.

### v1.0: Contract Freeze And Release

The following sequence starts only after the v0.90 sanitizer and platform gate
is green. It refines, rather than replaces, the four work areas above.

1. **Must-have: close v0.90 ownership and sanitizer evidence.** Anonymous
   functions capture the exact semantic free-variable set by value; listener
   ownership has no source/listener/callback reference cycle; HIR and bytecode
   behavior is equivalent. Require focused lifecycle tests, native and clean
   no-JIT full regression, unpacked release-package validation, Linux Clang
   ASan/UBSan, and every existing cross-platform CI lane before advancing.
   **Status: complete.** Commit `d658002` passed all six release lanes in
   Actions run `30423248652`.
2. **Must-have: freeze the final public contracts.** Freeze production
   `--run`, `--jit`, machine mode, and the stability/deprecation boundary of
   historical `--run-*` modes; C ABI 1.1; header-only C++ API 1.0; machine
   result protocol 1.0; and the `BuiltinRegistry`/`BuiltinDescriptor`
   extension contract. Publish one versioning and deprecation policy for all
   of them.
   **Status: candidate implemented and Windows-validated at `cfd59b7`.**
   Candidate cross-platform confirmation is deferred until CI capacity is
   restored, so this is not yet the final v1.0 publication freeze.
3. **Must-have: close reliability evidence.** Exercise parser/semantic fuzz,
   malformed bytecode, compile-once/invoke-many, long-running execution,
   module/session load-unload, handle/listener lifetimes, cancellation and
   resource limits, native-cache churn, allocation failure, and concurrent
   pressure. Every discovered defect is fixed without weakening an existing
   diagnostic, fallback, sanitizer, or lifecycle gate.
   **Status: implementation and Windows evidence complete at `f8f85f9`.**
   Deterministic frontend fuzz, mandatory malformed-bytecode verification,
   the named 8,012-invocation soak, lifecycle/resource/cache/allocation/
   load-unload/concurrency gates, native and no-JIT full regression, and
   unpacked package validation are tracked in
   [v1.0-reliability.md](v1.0-reliability.md). Candidate cross-platform
   confirmation remains deferred until the shared validation window returns.
4. **Must-have: establish performance and resource baselines.** Measure parse
   and compile time, cold start, bytecode, portable typed execution, native
   cold/warm execution, peak memory, allocations, binary size, and cache
   boundaries. Every result records hardware, OS, compiler, build type,
   backend, workload, and the exact timing boundary.
   **Status: source contract implemented; local closure in progress.** The
   non-installed collector, protocol-1.0 Draft-7 schema, semantic validator,
   quick eligible contract workload, and representative scalar/array
   workloads are defined in
   [v1.0-performance-baseline.md](v1.0-performance-baseline.md). Windows
   native/no-JIT full regression, package evidence, and committed host reports
   close the local candidate; cross-platform and physical-ARM measurements
   remain deferred.
5. **Must-have: close release documentation.** Publish the user manual,
   install/build guide, support matrix, CLI/API references, JIT/fallback
   behavior, C/C++ embedding guides, machine protocol, builtin author guide,
   diagnostics/resource/concurrency/lifecycle rules, the v0.x-to-v1.0
   migration policy, and explicit unsupported/Post-v1.0 lists.
6. **Should-have: evaluate specialized sanitizer CI.** Prefer Linux Clang
   TSan, then Windows MSVC ASan no-JIT, then macOS ARM64 Apple Clang
   ASan+UBSan no-JIT. A toolchain may be deferred only with a recorded
   stability rationale and compensating evidence; this work must never weaken
   Linux ASan/UBSan or the core lifecycle gates.
7. **Must-have: publish 1.0.0.** Freeze the compatibility matrix, public
   contract manifest, snapshots, and version metadata; generate Windows,
   Linux, and macOS x64/ARM64 archives with SHA-256 checksums; build and run
   independent C11 and C++20 consumers plus the CLI machine protocol from each
   unpacked SDK; complete final cross-platform CI, signing or provenance
   attestation, release notes, and the v1.x roadmap.

### v1.0 Scope Controls

- Foundational compatibility-matrix Must-have gaps are closed at the v0.90
  boundary. From this point, Parser, HIR, Bytecode, `RuntimeValue`, and
  embedding framework redesign is out of scope unless release evidence finds
  a correctness or public-contract defect that cannot be fixed locally.
- `G-JIT-001` remains **Should-have**. Select only a small number of
  high-value straight-line, matrix, builtin, or function specializations when
  the v1.0 performance baseline proves they are necessary. Every addition
  retains guarded fallback and bytecode/portable/native equivalence, and
  optimization coverage cannot block correct VM execution.
- `G-LONGTAIL-001` and `G-MATLAB-001` remain **Post-v1.0**. Unbounded builtin,
  toolbox, and complete MATLAB compatibility work moves to the v1.x roadmap
  and does not block v1.0.
- Each sequence item follows the milestone evidence rule below: implementation,
  focused tests, broad regression, a runnable sample, README/architecture/
  milestone documentation, and applicable platform CI.

## Gate Evidence

A milestone is complete only when its claimed behavior is demonstrated by
current source, focused tests, broad regression, a runnable sample, updated
documentation, and the applicable platform CI. Plans, passing unrelated tests,
or the absence of a known bug are not completion evidence.
