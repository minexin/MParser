# Migrating From v0.x To v1.0

MParser v0.x was an explicitly evolving candidate series. v1.0 freezes a
stable MATLAB-like subset runtime and its public integration boundaries. This
guide identifies changes that pre-v1 scripts, launchers, and embedding hosts
must review before adopting 1.0.0.

The current source version is still the v0.90 release candidate. The final
1.0.0 release changes the package version only after the remaining release
gates pass; it does not use that version bump to redesign the frozen
interfaces.

## 1. Use The Production Command

Use:

```text
mparser --run script.m
```

for editor Run buttons, user execution, and application launches.

Do not use `--run-hir`, `--run-bytecode`, `--run-jit`,
`--run-typed-bytecode`, or adaptive/profile modes as a generic production
alias. They remain available as diagnostic tools, but their human output is
not stable and some intentionally execute a smaller subset or run the script
multiple times.

The undocumented pre-v1 `--run-interpreter` alias is removed. Replace it with
`--run-hir` only when the deliberately smaller reference interpreter is
actually required; otherwise migrate to `--run`.

## 2. Select JIT Through `--jit`

Production execution uses:

```text
--jit={auto|off|portable|native}
```

`--jit=auto` is the default. The diagnostic
`--typed-backend={auto|portable|native}` option is rejected by `--run`.
Launchers that previously assembled diagnostic typed commands should choose a
production JIT policy instead.

Do not treat native eligibility as a language support check. Legal but
unoptimized target-subset code falls back to portable typed execution or the
bytecode VM.

## 3. Stop Parsing Human Output

Automation must use:

```text
mparser --run --result-format=json-v1 script.m
```

The command emits one `mparser.result` 1.x JSON document plus LF on stdout,
keeps stderr empty, and uses the machine exit-code mapping. Diagnostic and
ordinary human output may change formatting during v1.x.

Consumers must:

- verify the protocol major before interpretation;
- inspect both exit code and top-level `status`;
- tolerate documented additive fields within protocol major 1;
- preserve 64-bit counters without converting them through lossy floating
  point;
- reject incomplete output, especially after exit code 4.

Validate against `machine-result-v1.schema.json`. See
[Machine Result Protocol](machine-result-protocol.md).

## 4. Adopt Strict CLI Validation

The v1 CLI rejects:

- more than one execution or inspection mode;
- repeated single-occurrence options;
- options supplied to a mode that does not consume them;
- more than one input file;
- unknown JIT/backend/result-format values;
- `--native-cache-stats` combined with JSON machine mode.

`--argument`, `--module-call`, `--path`, `--class-path`, and
`--adaptive-workspace` are repeatable in their valid contexts. Insert `--`
before a source path beginning with `-`.

Test launchers against [CLI Reference](cli-reference.md) instead of relying on
pre-v1 ignored-option behavior.

## 5. Rebuild Against C ABI 1.1

The stable binary boundary is C ABI major 1 revision 1. Hosts must include the
installed `mparser/c_api.h`, link to `MParser::c_api`, and initialize
extensible root structures with the provided sized initializer macros or
functions.

Review these rules:

- opaque handles use retain/release ownership;
- caller-sized extensible roots may gain compatible tail fields;
- sealed array-element records keep a fixed stride in ABI major 1;
- borrowed strings, arrays, and diagnostics live only as long as their owner;
- every thread keeping a handle owns a retained reference;
- host-created arrays are copy-in and returned spans are read-only views.

Do not embed private `RuntimeValue`, STL containers, or engine C++ layouts
across a shared-library boundary. Old ABI-major-1 caller prefixes remain
accepted, but rebuilding is recommended so the host can use revision-1
resource and lifecycle fields.

See [C ABI Compatibility](c-abi-compatibility.md) and
[C Embedding API](embedding-c-api.md).

## 6. Use The C++20 Source Facade Correctly

The public C++ API is `mparser/cpp_api.hpp`, a header-only C++20 RAII facade
over the C ABI. Link `MParser::cpp_api`.

It promises source compatibility within C++ API major 1, not a C++ binary
ABI. Recompile the host when updating the header or C runtime. Language
compile/runtime failures are returned as module/result diagnostics;
`mparser::ApiError` represents host API misuse or boundary failure.

Wrappers are copyable retained handles. Give each thread its own wrapper copy
instead of concurrently assigning or destroying the same wrapper object.

See [C++ Embedding SDK](embedding-cpp-api.md).

## 7. Move Builtins Into The Registry

Pre-v1 builtins may have been wired separately into semantic analysis, the HIR
interpreter, bytecode VM, or typed execution. v1 extensions register one
`BuiltinDescriptor` and one invocation implementation through the unified
registry/call/result contract.

Each descriptor declares:

- canonical name and aliases;
- input/output arity;
- value and shape constraints;
- purity, determinism, and side effects;
- workspace/context permissions;
- diagnostic and exception conversion;
- threading/resource cooperation;
- typed/JIT eligibility.

An extension is incomplete if one runtime path hard-codes behavior outside
the registry. Use the conformance test template and normalized catalog
snapshot described in [Extending Builtins](extending-builtins.md).

The builtin source contract is not an installed binary plugin ABI.
Independently compiled callbacks remain Post-v1.0.

## 8. Review Runtime Values And Outputs

The frozen runtime model distinguishes zero, one, and multiple requested
outputs. A standalone call requests zero outputs; embedding hosts should not
assume a hidden placeholder result.

Review code that assumed:

- row-major external arrays;
- all text values are one legacy character representation;
- structures are always scalar;
- handle and value objects share copy behavior;
- anonymous functions capture an entire workspace by reference;
- nested mutation may partially commit before a failure.

The v1 boundary uses column-major arrays, distinct character/string values,
structure arrays, explicit value/handle semantics, exact by-value anonymous
captures, and transactional nested path/optimized-region commit.

## 9. Add Resource And Cancellation Policies

Embedding hosts can set instruction, wall-time, call-depth, per-value
array-payload, and diagnostic-count limits and can pass a cancellation token.
Zero means unlimited for each numeric limit.

Resource stops are terminal request outcomes, not catchable language
exceptions. Completed earlier side effects are not globally rolled back.
Module/session lock admission time is outside the runtime wall-time budget, so
queue deadlines remain a host responsibility.

Read [Runtime Boundaries](runtime-boundaries.md) before executing untrusted or
multi-tenant workloads. These controls do not make MParser an OS sandbox.

## 10. Consume The Relocatable Package

Prefer the installed CMake package:

```cmake
find_package(MParser CONFIG REQUIRED COMPONENTS C CPP CLI)
```

Use `MParser::c_api`, `MParser::cpp_api`, and `MParser::cli`; do not link
source-tree internal targets. Release archives contain headers, shared
runtime, CLI, schemas, manuals, examples, Apache-2.0 license, and third-party
notices.

Verify both entries in `SHA256SUMS`, then verify the unsigned SLSA provenance
subject, source revision, public-contract inputs, build type, builder, and
platform/toolchain parameters. SHA-256 and an unsigned statement are integrity
and audit evidence, not publisher identity; follow the authenticated
publication policy in [v1 Release Process](release-process.md) when 1.0.0 is
published.

## 11. Recheck Supported MATLAB Behavior

Do not translate "v1.0" into "all MATLAB behavior." Review
[Support Matrix](support-matrix.md) and the authoritative
`compatibility-matrix.json`, especially entries marked partial.

Explicitly unsupported/Post-v1.0 areas include complete MATLAB/toolbox
coverage, Live Scripts, MEX, Simulink, graphics, complex/sparse/GPU/table
values, parallel `parfor`, external builtin ABI, zero-copy host input arrays,
and persistent native disk cache.

## Migration Checklist

- Production launch uses `--run`.
- Production tier selection uses `--jit`, not `--typed-backend`.
- Automation consumes `mparser.result` 1.x, not human text.
- CLI construction passes strict option-context and duplicate checks.
- C hosts use ABI 1.1 initializers and explicit retain/release.
- C++ hosts compile as C++20 and link `MParser::cpp_api`.
- External arrays and indexes are column-major.
- Zero/multi-output behavior is handled explicitly.
- Builtins use the unified registry and conformance template.
- Resource, cancellation, concurrency, and queue policies are documented by
  the host.
- Deployment consumes an installed/unpacked SDK and records package integrity.
- Script dependencies have been checked against partial/unsupported matrix
  entries.

The common compatibility window is defined in
[Versioning And Deprecation](versioning-and-deprecation.md).
