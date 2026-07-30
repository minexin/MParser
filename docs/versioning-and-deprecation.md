# Versioning And Deprecation Policy

This policy covers the MParser engine/package, command-line interface, C ABI,
C++ source facade, machine result protocol, and source-integrated builtin
extension contract. Their version numbers describe different compatibility
boundaries and must not be substituted for one another.

## Engine And Packages

MParser uses semantic versioning for release numbers. Before `1.0.0`, minor
releases may change pre-v1 interfaces when the milestone document and
compatibility matrix describe the migration. Starting with `1.0.0`:

- a patch release fixes defects without intentionally changing a documented
  public contract;
- a minor release may add backward-compatible language features, APIs,
  protocol fields, builtin metadata, and optimizations;
- a major release is required to remove or reinterpret a stable v1 contract.

The CMake package version file uses `SameMinorVersion` for the `0.x`
candidate series. The `1.0.0` release switches it to `SameMajorVersion`, so a
consumer requesting a compatible v1 package can accept later v1 minor and
patch releases. Consumers that need an exact engine build may continue to use
`EXACT`.

## CLI 1.0

The production command is `--run`. Its `--jit=auto|off|portable|native`
selector, invocation arguments, machine-result selector, source path rules,
channel behavior, and exit-code classes are stable in v1. The complete
machine-readable boundary is recorded in `cli-contract-v1.json`.

Historical `--run-*`, profiling, planning, token, HIR, and bytecode modes are
supported diagnostic tools. Their option names remain accepted throughout
v1.x, but their human-readable text is not a parsing protocol and may gain
fields or formatting changes. Automation must use
`--run --result-format=json-v1`.

The undocumented pre-v1 `--run-interpreter` alias is removed before the v1
freeze. Its documented replacement is `--run-hir`.

## C ABI 1.1

`MPARSER_C_ABI_VERSION_MAJOR` identifies the binary ABI family.
`MPARSER_C_ABI_REVISION` identifies backward-compatible additions within that
family. C ABI major 1 revision 1 obeys these rules:

- existing exported symbols, enum values, sealed record layouts, ownership
  rules, and error meanings are immutable;
- extensible root records may add tail fields only, and callers initialize
  them through the sized initializer functions;
- a library accepts every documented older caller prefix in ABI major 1;
- new functionality uses new symbols or tail fields and increments the ABI
  revision when required;
- a breaking layout, calling convention, ownership, or symbol change requires
  ABI major 2 and a new shared-library `SOVERSION`.

The engine semantic version and C ABI revision are independent. A v1 engine
minor release may retain C ABI 1.1 when it adds no C ABI surface.

## C++ Source API 1.0

`include/mparser/cpp_api.hpp` is a header-only C++20 facade over the narrow C
ABI. It promises source compatibility within major 1, not a C++ binary ABI.
Hosts link to `mparser_c_api`; they do not exchange STL objects or C++ class
layouts across the shared-library boundary.

Backward-compatible declarations may increment the C++ source API minor.
Removing a declaration, changing ownership, or changing an existing operation
in a source-incompatible way requires source API major 2.

## Machine Result Protocol 1.0

`mparser.result` has an independent major/minor pair. Producers currently emit
exact protocol 1.0. Within major 1:

- existing fields retain their spelling, type, and meaning;
- a minor revision may add fields or enum values only where the consumer
  contract explicitly permits them;
- tolerant consumers ignore unknown additive fields and check the protocol
  major before interpretation;
- removing a required field or changing an existing meaning requires protocol
  major 2.

The JSON Schema is the tolerant major-1 consumer profile. Exact 1.0 producer
shape is frozen by the golden and public-contract snapshots. Machine mode
writes one JSON document plus one LF to stdout, leaves stderr empty, and uses
the protocol exit-code mapping.

## Builtin Source Contract 1.0

`BuiltinRegistry`, `BuiltinDescriptor`, `BuiltinCall`, and `BuiltinResult`
form a source-integration contract for builtins compiled together with the
engine. They are not an installed plugin ABI and do not promise stable C++
object layout.

Major 1 freezes the semantic rules for naming and aliases, registry freezing,
input/output arity, positional value/shape constraints, context permissions,
side-effect and determinism metadata, exception conversion, output ownership,
diagnostics, and typed-lowering eligibility. Additive enum cases, descriptor
fields, or constraints increment the source-contract minor and require default
behavior that preserves existing descriptors. Removing a field or changing
an existing semantic rule requires source-contract major 2.

The normalized default catalog snapshot is review evidence, not a promise
that every catalog name is implemented. `Unsupported` remains an explicit
state, and conservative intrinsic metadata may be refined compatibly when it
does not make an existing extension invalid. The compatibility matrix remains
authoritative for language and builtin implementation coverage.

An independently compiled external C/C++ callback table is Post-v1.0. It will
need a separately versioned pure-C ABI and cannot expose `RuntimeValue`, STL
containers, registry classes, or VM pointers.

## Deprecation Window

A stable v1 element is not removed or reinterpreted in v1.x. When a
replacement is needed:

1. document the old element as deprecated, its replacement, and the first
   release carrying the notice;
2. emit a warning where doing so does not corrupt machine output or normal
   program semantics;
3. retain the old behavior for at least one subsequent minor release and for
   the remainder of major v1;
4. remove it only in the next major release and record the migration.

Security, data-corruption, or undefined-behavior defects may require immediate
containment. Such an exception needs a release note, compatibility-matrix
entry, and the narrowest practical replacement path.

## Contract Review

Every public-boundary change is classified as compatible, additive,
deprecated, or breaking. The change must update the applicable version,
snapshot, tests, documentation, and `public-contract-v1.json` hash only after
old-client or golden compatibility evidence passes. A hash mismatch is a
review gate, not an instruction to refresh snapshots automatically.
