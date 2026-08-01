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

## v1.1: Function And Array Breadth

Primary work:

- expand common math, statistics, text, shape, set, sorting, searching, and
  array-manipulation builtin families through `BuiltinRegistry`;
- add missing target-subset overloads and edge semantics without scattering
  builtin knowledge through Semantic, HIR, Bytecode, and VM layers;
- broaden real dense numeric element types where `RuntimeValue`, conversion,
  indexing, and C/C++ copying contracts can remain compatible;
- grow `.m` standard-library coverage for functions that do not require a
  privileged runtime intrinsic.

Long-tail functions remain prioritized by representative workloads and
compatibility value rather than raw catalog count.

## v1.2: Typed And JIT Coverage

Candidate work, gated by the v1.0 performance baseline:

- more straight-line numeric regions and common matrix/array kernels;
- typed lowering for high-value pure builtins;
- guarded function-call specialization with stable deoptimization;
- reduced `RuntimeValue`, dispatch, and temporary-allocation overhead;
- improved native cache observability and bounded reuse.

Every specialization must preserve bytecode/portable/native result and
diagnostic equivalence. LLVM, OSR, broad speculative object optimization, and
persistent native-code disk cache require separate evidence and may remain
later work.

## v1.3: External Extension Boundary

Design work may introduce a narrow versioned C callback adapter for compiled
external builtins. It must define:

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
