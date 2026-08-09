# MParser v1.x Roadmap

The v1.x line grows the released MATLAB-like subset while preserving the
public CLI, C ABI major 1, C++ source API 1.x, machine protocol major 1, and
builtin source contract 1.x. This roadmap starts after 1.0.0 publication; it
does not expand the remaining 1.0 release gate.

## Release Discipline

Every v1.x change follows these rules:

- legal code outside an optimized region must continue through verified
  bytecode/VM fallback;
- compatible additive fields are allowed in tolerant machine/API structures,
  while required field removal or reinterpretation waits for v2;
- each language or builtin addition updates the compatibility matrix,
  representative interpreter/VM/typed tests, samples when user-facing, and
  extension/user documentation;
- performance changes include correctness equivalence and measured evidence,
  not timing-only acceptance;
- release archives retain checksums, provenance, relocation, independent
  consumer, and platform validation.

## v1.1: Core Compatibility Corrections

**Status: implemented; validation evidence is recorded in
[v1.1.md](v1.1.md).**

This milestone closes six structural mismatches found by the 223-point
external differential suite: left-associated power, comma-separated one-line
control forms, matrix-column `for`, and column-shaped `A(:)`. It adds shared
runtime authorities and guarded typed fallback without changing the frozen v1
host contracts.

## v1.2: Command-Module Integration

The next milestone closes the highest-value MExecServer integration gaps:

- compile in-memory source with explicit search and class paths;
- expose source-graph main-function, pure-function-file, and top-level
  statement metadata;
- return top-level expression values and stable `ans` source locations;
- route `disp` and formatted output through a host-owned sink;
- preserve current file-based and CLI behavior through additive defaults.

## v1.3-v1.4: Function Breadth

v1.3 prioritizes pure numeric, rounding, finite/shape predicate, text-format,
ordering, set, and array-manipulation families through `BuiltinRegistry`.
v1.4 follows with explicitly contextual services such as random state,
workspace/path queries, warnings, environment access, and bounded file I/O.
`.m` standard-library implementations remain preferred where no privileged
runtime context is required. Long-tail functions are ranked by representative
workloads and compatibility value, not raw catalog count.

## v1.5: Ownership And Inspection

Close the remaining Cell, Struct, class, enum, event, and reflection gaps that
fit the frozen representation contracts. Define safe cross-module ownership
for module-bound handles, objects, and globals. Debugger stack/local inspection
is an additive public contract and must be gated separately from ordinary
execution.

## v1.6+: Typed Coverage And Remaining Language Gaps

The [v1.0 JIT scope decision](v1.0-jit-scope-decision.md) defers broader
`G-JIT-001` work until a representative workload justifies it. Candidates
include straight-line numeric regions, common matrix kernels, high-value pure
builtin lowering, guarded function specialization, and reduced
`RuntimeValue`/temporary allocation overhead. Every specialization must
preserve bytecode/portable/native result and diagnostic equivalence.

Remaining in-scope syntax, type, nested-function, linear-algebra,
FFT/convolution, persistence, and dynamic-evaluation gaps continue in coherent
0.1 milestones. LLVM, OSR, speculative object optimization, and persistent
native-code disk cache require separate evidence and may remain later work.

## External Extension Boundary

A later v1.x milestone may introduce a narrow versioned C callback adapter for
compiled external builtins. It must define:

- descriptor/version negotiation and capability discovery;
- copied versus owned array/value transfer and optional view rules;
- callback lifetime, unload, reentrancy, thread safety, and cancellation;
- deterministic exception/diagnostic conversion;
- namespace, precedence, shadowing, purity, side-effect, and JIT eligibility;
- compatibility tests against older headers and independently built modules.

No internal Parser, HIR, Bytecode, VM, or `RuntimeValue` layout becomes ABI as
part of this work.

## Later v1.x Candidates

Subject to demand and architecture fit:

- complex and sparse numeric values;
- tables/timetables and richer domain value types;
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
