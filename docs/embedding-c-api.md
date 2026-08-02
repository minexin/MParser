# MParser C Embedding API

MParser v0.83 introduced a narrow, pure C embedding boundary in
`include/mparser/c_api.h`. It is implemented by the `mparser_c_api` CMake
shared-library target, whose output name is `mparser_c`. v0.84 extends that
same boundary with explicit multi-source compilation and UTF-8 filesystem
source-graph loading. v0.85 installs it as a relocatable CMake package. v0.86
adds a separate machine-readable CLI result protocol over the same
engine-neutral invocation result. v0.87 adds executable ABI-major-1 evolution
rules, sized structure initialization, and a frozen old-header consumer.
v0.88 adds a separate public C++20 RAII facade over this same boundary; see
`embedding-cpp-api.md`. v0.89 adds the shared-object concurrency boundary,
ABI-major shared-library naming, an exact public symbol manifest, repeated
lifecycle/unload stress, and macOS package consumers.
v0.90 freezes ABI candidate 1.1 through exact header/layout/export snapshots,
adds deterministic allocation/internal-failure containment tests, publishes
the combined public contract, and validates unpacked checksummed SDK archives.

The header exposes no C++ standard-library type, exception, class layout, or
`RuntimeValue` representation. All state crosses the boundary through opaque
handles, fixed-width constants, byte/code-unit views, and versioned plain C
structures.

This is the frozen v1 candidate for ABI major 1 revision 1, not the final v1.0
release promise. An incompatible correction changes the ABI major rather than
rewriting candidate major 1 in place. Applications must query
`mparser_c_abi_version()`, `mparser_c_abi_revision()`, and the three MParser
component-version functions rather than assuming that the project, ABI major,
and additive ABI revision advance together.

## Build

From the source tree:

```powershell
cmake -S . -B build
cmake --build build --target mparser_c_api mparser_c_embedding_demo `
  mparser_c_source_graph_demo mparser_c_abi_compat_demo
build\mparser_c_embedding_demo.exe
build\mparser_c_abi_compat_demo.exe
build\mparser_c_source_graph_demo.exe `
  samples\class_folders\app\run_demo.m `
  samples\class_folders\lib
```

On Linux the link name is `libmparser_c.so` and its ABI-major SONAME is
`libmparser_c.so.1`. On macOS the corresponding install name is
`libmparser_c.1.dylib`. On Windows it is `mparser_c.dll` plus the toolchain
import library. The complete shared-library implementation version is
`1.1.0`, independently of the engine release.

For a production-only installed SDK:

```powershell
cmake -S . -B build-sdk -DBUILD_TESTING=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix C:\mparser-sdk
```

An independent CMake project can then use:

```cmake
find_package(MParser 0.90.1 EXACT CONFIG REQUIRED COMPONENTS C CLI)
target_link_libraries(host PRIVATE MParser::c_api)
```

`MParser::cli` is the imported matching CLI executable. The package also
exports project-version components, `MParser_C_ABI_VERSION`,
`MParser_C_ABI_REVISION`,
`MParser_C_INCLUDE_DIR`, `MParser_CLI_DIR`, C++ source API `1.0`, machine
protocol `1.0`, CLI contract `1.0`, builtin source contract `1.0`, and checked
paths to the license, notices, public/CLI contracts, protocol schema, builtin
catalog/author guide, and versioning policy. Its paths are relative to the
package prefix, so the installed tree may be moved as a unit before consumer
configuration. On Windows, deploy `mparser_c.dll` beside the host executable
or expose the SDK `bin` directory to the runtime loader.

The C package remains independently consumable. v0.88 also exports
`MParser::cpp_api`; v0.90 records C ABI 1.1 and C++ source API 1.0 as the
combined v1 candidates.

## Handles And Ownership

The API defines opaque handles for modules, sessions, results, values, and
cancellation tokens. Each successful constructor or owned getter returns one
reference. Release it with the matching function:

| Handle | Retain/release |
| --- | --- |
| `mparser_module` | `mparser_module_retain/release` |
| `mparser_session` | `mparser_session_retain/release` |
| `mparser_result` | `mparser_result_retain/release` |
| `mparser_value` | `mparser_value_retain/release` |
| `mparser_cancel_token` | `mparser_cancel_token_retain/release` |

`mparser_result_output`, `mparser_result_variable`,
`mparser_value_cell_element`, and `mparser_value_struct_field` return owned
value handles. They remain valid after the parent result or composite handle
is released.

`mparser_utf8_view` and `mparser_utf16_view` are borrowed immutable views.
Module names and compilation diagnostics remain valid until the module is
released; result fields and execution diagnostics remain valid until the
result is released; value payload views remain valid until the value is
released. The host must not free or modify view storage.

Retain/release counters and cancellation state are atomic. A thread that keeps
a handle after another thread may release its reference must first own a
retained reference. Do not concurrently assign, release, or replace the same
host handle variable; retain one reference per thread.

Stateless module invocations have isolated runtime state and may execute
concurrently when their arguments and initial workspace contain no
module-bound value. Requests carrying module-bound objects or closures are
serialized by their producing module so shared handle fields cannot race.
All execute, clear, and reset operations on every session from one module use
that same graph lock and then the session lock. This conservative rule also
serializes different sessions from one module; it preserves escaped handle
objects even when they are retained in persistent or global state. A future
finer-grained object lock may relax this without changing host-visible
semantics.

Invocation wall-time and cancellation accounting starts after lock admission.
Time spent waiting behind an operation on the same module/session is not part
of the requested wall-time limit. Cancellation remains observable once the
queued invocation enters the runtime.

## Compilation And Invocation

`mparser_module_compile_utf8` compiles one owned copy of a UTF-8 source buffer
and source name. It remains the one-source convenience API.

`mparser_module_compile_sources` accepts one or more versioned
`mparser_source_unit` descriptors. Initialize each descriptor with
`mparser_source_unit_init`. The module copies every name and source buffer,
preserves the supplied order, and treats the first unit as the entry source.
This route supports explicit top-level scripts, functions, and classes, but it
does not infer package/private/class-folder identity from source names.

For MATLAB filesystem semantics, initialize `mparser_source_load_options`
with `MPARSER_SOURCE_LOAD_OPTIONS_INIT(&options)` and call
`mparser_module_load_file_utf8` with an entry `.m` file and ordered search
paths. This invokes the same `SourceLoader` used by the CLI and supports:

- entry-directory and ordered search-path precedence;
- ordinary path and private functions;
- `+package` functions and classes;
- `@Class` folders, separated methods, and class-private functions;
- discovered imports, superclasses, typed properties, and callable
  dependencies.

Paths are length-delimited UTF-8 and are converted directly to native
filesystem paths. Loaded source names remain UTF-8. The host can inspect the
ordered graph through `mparser_module_source_count` and
`mparser_module_source_name`; returned names are borrowed from the module.
Empty paths, embedded nulls, and malformed UTF-8 return
`MPARSER_API_STATUS_INVALID_ARGUMENT` without creating a module.

On a language compilation failure either route returns
`MPARSER_API_STATUS_COMPILATION_FAILED` and an inspectable invalid module whose
diagnostics preserve source positions. A filesystem loading failure returns
`MPARSER_API_STATUS_SOURCE_LOAD_FAILED` and an inspectable invalid module with
a stable `MParser:SourceLoadFailed` compilation diagnostic. Descriptor or
option contract errors return without creating a module.

Initialize every current-source invocation with
`MPARSER_INVOCATION_OPTIONS_INIT(&options)` and check its returned status. The
request can select:

- a top-level entry function, or an empty name for script execution;
- positional value arguments and an optional requested output count;
- named initial-workspace values;
- automatic, bytecode, portable, or native execution;
- profile collection;
- instruction, wall-time, call-depth, per-value array-payload, and diagnostic
  limits;
- a cross-thread cancellation token.

Zero means unlimited for every numeric resource limit. The execution behavior
and JIT suppression rules are the same as the v0.82 C++ contract.

`mparser_module_execute` is stateless. A session created by
`mparser_module_create_session` preserves global and persistent state across
calls and supports targeted global clearing, clearing all globals, and reset.
Both paths return an owned `mparser_result`.

An API status of `OK` means the host request was accepted and a result was
created. It does not by itself mean that MATLAB-like execution succeeded.
Inspect `mparser_result_status` or `mparser_result_succeeded`, then inspect
outputs, variables, diagnostics, and the execution summary.

## Values

The external value model supports:

- double and logical scalars/arrays;
- UTF-16 character arrays;
- UTF-16 string arrays, including missing elements;
- N-dimensional Cell arrays;
- scalar structures;
- returned object arrays;
- named, anonymous, and builtin function handles;
- the explicit missing transport value.

Array constructors require a rank of at least two and a dimension product
equal to the supplied element count. Dimensions and payloads are copied.
Numeric/logical payloads use `double`; character and string payloads use
Unicode code units represented by `uint16_t`.

All external array payloads and linear element indexes use MATLAB column-major
order. MParser converts to and from its internal storage without exposing that
storage layout. Accessor pointers are immutable and owned by the value handle.
There is no zero-copy external input buffer or mutable view in the v1.0
candidate. Copy-in is the frozen ownership rule for host-created arrays.
Runtime-owned numeric and character accessor pointers are immutable zero-copy
views whose lifetime is bounded by their `mparser_value`; they must not be
retained after that value is released. A future borrowed-input API requires a
new additive descriptor with explicit deleter, alignment, mutability, and
threading rules.

Cell and structure constructors copy the represented runtime values. Releasing
the child handles after construction is valid. A scalar structure is created
from named fields; structure arrays can be returned and inspected, but a
general external structure-array constructor is not yet exposed.

Objects and module-defined function handles may reference compiled callable
metadata. Such values retain their producing module and report
`mparser_value_is_module_bound() == 1`. They may be passed back to the same
module after their producing result has been released. Passing them to a
different module returns `MPARSER_API_STATUS_OWNER_MISMATCH` before execution.
This ownership propagates through Cell/Struct values. Combining values bound
to different modules is rejected. Independent builtin handles are not
module-bound and can cross modules.

## Diagnostics And Failures

API misuse and boundary failures return `mparser_api_status`, including
invalid arguments, range/type errors, owner mismatch, ABI mismatch,
compilation failure, allocation failure, and internal failure.
`mparser_api_status_name` provides a stable ASCII name.

Language compilation, validation, warning, and execution diagnostics are
owned by modules or results. Each diagnostic exposes:

- phase and severity;
- stable identifier and message;
- source name and begin/end position when available;
- projected call-stack frames;
- nested causes.

The diagnostic pointer itself is borrowed from its module or result.

No C++ exception crosses the C boundary. Ordinary internal exceptions become
`MPARSER_API_STATUS_INTERNAL_ERROR`, and allocation failure becomes
`MPARSER_API_STATUS_ALLOCATION_FAILED`. This differs deliberately from the
v0.82 source-level C++ contract, where `std::bad_alloc` still propagates.

Failure after a C constructor starts never publishes a partial output handle.
Existing module, session, result, value, and cancellation handles remain
valid. Execution itself is not globally transactional: a failure before the
runtime core starts does not commit session state, while allocation or
publication failure after the runtime returns may leave language-visible
side effects committed. Do not blindly retry an invocation that returned a C
boundary error after execution may have begun unless that operation is
idempotent.

`max_array_bytes` is a per-`RuntimeValue` recursive payload limit observed at
runtime checkpoints. It is not a process RSS, aggregate heap, or allocator
quota. Wall-time accounting starts after module/session lock admission; hosts
that need queue deadlines or concurrency admission limits must enforce them
outside MParser.

## Structure Versioning

`mparser_invocation_options`, `mparser_execution_summary`, and
`mparser_source_load_options` start with `struct_size` and `abi_version`.
Initialize current source with the uppercase `MPARSER_*_INIT` macros; do not
use aggregate literals. The macros pass the caller's complete storage size to
the revision-1 sized initializers. New libraries accept the frozen v1 prefix,
ignore unknown input tails, and limit output writes to the caller's recorded
capacity. The old initializer symbols remain available and write only the
frozen v1 prefix for already-built consumers.

`mparser_source_unit` is sealed within ABI major 1 because arrays use its
fixed size as the descriptor stride. Oversized source-unit descriptors are
rejected. Full structure and symbol evolution rules are in
[c-abi-compatibility.md](c-abi-compatibility.md).

The C constants are integer typedefs plus macros rather than compiler enums,
so their width does not depend on the host compiler's enum ABI.

## Validation

`c_api_smoke` is compiled as C11 and links only through the shared C target. It
covers:

- version negotiation, multi-source compilation, source enumeration, and
  source-linked diagnostics;
- copied host source-buffer ownership and versioned load options;
- package/class-folder/search-path loading and stable load failures;
- non-ASCII entry/search paths and retained UTF-8 source names;
- scalar and multiple-output invocation;
- MATLAB column-major numeric, character, and string round trips;
- Cell and Struct construction with independent child lifetimes;
- script workspace injection and result-variable lookup;
- object pass-through after releasing the producing result;
- same-module closures and cross-module ownership rejection;
- cross-module independent builtin handles;
- resource stops, pre-cancellation, execution summaries, and session recovery;
- retain/release and ABI-request validation;
- old-prefix write bounds, oversized request/load/summary storage, and sealed
  source-unit rejection.

`c_api_utf8_source_graph_smoke` creates non-ASCII entry and library
directories. `c_embedding_demo_smoke` runs the single-source C host and
`c_source_graph_demo_smoke` loads a real `+package/@Class` graph.
`c_api_v1_compat_smoke` compiles against the frozen v0.86 header snapshot, and
`c_abi_compat_demo_smoke` executes future-tail storage. All are
included in Linux AArch64 native-JIT and portable-only QEMU jobs in addition
to the complete Windows x64 and Linux x64 suites.

`c_api_v1_1_compat_smoke` compiles against the exact ABI 1.1 snapshot.
`c_api_layout_contract` freezes the 64-bit public record layout and constant
ranges. `c_api_allocation_failure` intercepts global allocation across
protocol serialization and representative C API construction/execution
routes. `c_api_named_fault_smoke` injects deterministic bad-allocation and
internal faults at publication boundaries, proves thread-local isolation, and
checks the pre/post-execution commit contract.

`c_api_lifecycle_stress` performs concurrent retain/release cycles for every
public handle family and proves retained sessions and module-bound values
outlive their original module/result handles. `cpp_api_concurrency_stress`
proves concurrent pure stateless calls, exact shared-handle mutation,
serialized sessions, cross-session escaped-object safety, independent session
state, shared cancellation, and per-invocation resource isolation.
`embedding_unload_stress` dynamically
loads, queries, and unloads the shared library 256 times.

`c_api_shared_library_abi` compares the dynamic export table against
`tests/c_api_abi1_symbols.txt` and validates ELF SONAME or macOS install-name
major 1. Internal compiler, VM, C++ facade, and SLJIT symbols use hidden
visibility. Windows x64, Linux x64, macOS x64/ARM64, and focused Linux
AArch64 jobs execute the applicable ABI and stress evidence.

`installed_c_consumer_smoke` installs the SDK, moves its prefix, configures a
separate C11 project through `find_package`, and verifies C ABI execution plus
the imported CLI on Windows x64 and Linux x64. The AArch64 job independently
installs and cross-consumes both native-JIT and portable packages, then runs
the installed consumer and CLI under QEMU.

With `MPARSER_ENABLE_RELEASE_PACKAGING=ON`,
`mparser_release_package` creates a platform/architecture-named ZIP or TGZ
plus SHA-256 records and an unsigned SLSA Provenance v1 statement.
`release_archive_smoke` packages one built payload twice with a fixed
timestamp, verifies archive paths, checksums, and provenance semantics,
unpacks it, and consumes only that SDK from independent C11 and C++20
projects. These records provide integrity and audit evidence, not publisher
identity. Tag `v0.90.1` completed the separate authenticated candidate
operation in Actions run `30743014345`; final `1.0.0` publication must repeat
it for the final tag.

## Licensing

MParser's first-party source and documentation are licensed under the Apache
License, Version 2.0, with `Copyright 2026 Wang Xin`. The source tree and
installed SDK include `LICENSE`, `NOTICE`, and `THIRD_PARTY_NOTICES.md`.
The relocated CMake package exports `MParser_LICENSE`, `MParser_COPYRIGHT`,
and checked paths to all three files. Vendored SLJIT remains under the
Simplified BSD terms reproduced in the third-party notices.

## Frozen Boundary

The v0.90 embedding gate is closed: C ABI 1.1, header-only C++ source API 1.0,
machine protocol 1.0, allocation/internal-failure containment, sanitizer
coverage, and checksummed unpacked-SDK validation are all active regression
gates. `docs/public-contract-v1.json` also hashes the legacy C ABI 1.0 header
snapshot, and sized initializers preserve a caller-provided prefix without
writing a future library `sizeof` over it.

`docs/versioning-and-deprecation.md` now defines the common v1 evolution
policy. Cross-platform reliability, documentation, and candidate packaging
are confirmed. Remaining v1.0 work concerns performance/resource evidence,
authenticated publication, and the final version/tag operation rather than
redesigning this ABI.

The v1.0 candidate deliberately excludes a stable external native callback
ABI and zero-copy borrowed input arrays. Their future additive rules are
defined in `extending-builtins.md`; neither blocks correct `.m` functions,
source-integrated C++ builtins, or the public host invocation APIs.

The CLI schema and exit/channel contract are defined separately in
[machine-result-protocol.md](machine-result-protocol.md).
