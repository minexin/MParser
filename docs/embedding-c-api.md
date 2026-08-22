# MParser C Embedding API

`include/mparser/c_api.h` defines MParser's narrow pure-C embedding boundary.
It is implemented by the `mparser_c_api` CMake shared-library target, whose
output name is `mparser_c`. It supports explicit source graphs, compile-once
execution, sessions, structured diagnostics, resource controls, typed numeric
values, host-owned output routing, and explicitly capability-gated system
contexts.

The header exposes no C++ standard-library type, exception, class layout, or
`RuntimeValue` representation. All state crosses the boundary through opaque
handles, fixed-width constants, byte/code-unit views, and versioned plain C
structures.

The frozen v1.2 candidate uses C ABI generation 2 revision 0 and 109 exports.
The live v1.3 development header advances that generation additively to
revision 1 and 117 exports for public system-context injection. These are
binary-contract identifiers, not SDK product versions. MParser, the installed
SDK, and the C/C++ source APIs still report development version `1.2.0`/`1.2`
until the whole v1.3 train reaches its milestone gate. Applications can query
`mparser_c_abi_generation()`, `mparser_c_abi_revision()`, and the three
MParser component-version functions rather than assuming that product, ABI,
and machine protocol levels advance together. The released ABI 1.1 contract
is retained only in the v1.0 archive and is not a compatibility gate for this
non-production line.

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

On Linux the link name is `libmparser_c.so` and its current ABI-generation SONAME is
`libmparser_c.so.2`. On macOS the corresponding install name is
`libmparser_c.2.dylib`. On Windows it is `mparser_c.dll` plus the toolchain
import library. The current shared-library ABI implementation version is
`1.2.0`; its loader compatibility identity remains generation 2.

For a production-only installed SDK:

```powershell
cmake -S . -B build-sdk -DBUILD_TESTING=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix C:\mparser-sdk
```

An independent CMake project can then use:

```cmake
find_package(MParser CONFIG REQUIRED COMPONENTS C CLI)
target_link_libraries(host PRIVATE MParser::c_api)
```

`MParser::cli` is the imported matching CLI executable. The package also
exports project-version and C API version components,
`MParser_C_ABI_GENERATION`, `MParser_C_ABI_REVISION`,
`MParser_C_INCLUDE_DIR`, `MParser_CLI_DIR`, C++ source API `1.2`, machine
protocol `1.1`, CLI contract `1.0`, builtin source contract `1.1`, and checked
paths to the license, notices, public/CLI contracts, protocol schema, builtin
catalog/author guide, and versioning policy. Its paths are relative to the
package prefix, so the installed tree may be moved as a unit before consumer
configuration. On Windows, deploy `mparser_c.dll` beside the host executable
or expose the SDK `bin` directory to the runtime loader.

The C package remains independently consumable. The same MParser SDK also
exports `MParser::cpp_api`; its API level is metadata for source-contract
checks rather than a separate SDK version.

## Handles And Ownership

The API defines opaque handles for modules, sessions, results, values,
cancellation tokens, and system contexts. Each successful constructor or
owned getter returns one reference. Release it with the matching function:

| Handle | Retain/release |
| --- | --- |
| `mparser_module` | `mparser_module_retain/release` |
| `mparser_session` | `mparser_session_retain/release` |
| `mparser_result` | `mparser_result_retain/release` |
| `mparser_value` | `mparser_value_retain/release` |
| `mparser_cancel_token` | `mparser_cancel_token_retain/release` |
| `mparser_system_context` | `mparser_system_context_retain/release` |

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

## System Contexts

Initialize `mparser_system_context_options` with
`MPARSER_SYSTEM_CONTEXT_OPTIONS_INIT`, set a nonempty existing
`root_directory`, select explicit capability bits, and call
`mparser_system_context_create_rooted_native`. Optional current, temporary,
and search directories must resolve to existing directories inside the root.
The context owns mutable current-directory, search-path, random, and open-file
state used by an invocation or reusable session.

Use `mparser_module_execute_with_system_context` for a stateless invocation or
`mparser_module_create_session_with_system_context` for persistent execution.
A session retains the context, so the host may release its original context
handle after session creation. Calls that share one context intentionally
share its mutable system state; create separate contexts when requests require
isolation. The original `mparser_module_execute` and
`mparser_module_create_session` continue to create isolated runtime state with
no process-facing capabilities.

The rooted native adapter canonicalizes the longest existing path ancestor
before every path-oriented directory, lookup, listing, and open operation. It
rejects `..` and existing symbolic-link targets outside the configured root,
filters directory entries that resolve outside it, limits simultaneously open
files and one read window, and closes retained files with the context.
Environment and process capabilities are host-wide and must be granted
separately; the root does not constrain a command launched through `system`.
This path policy is not a complete OS sandbox and does not defend against a
hostile local process racing filesystem links during a call. Use an operating
system sandbox when adversarial filesystem mutation is in scope.

## Compilation And Invocation

`mparser_module_compile_utf8` compiles one owned copy of a UTF-8 source buffer
and source name. It remains the one-source convenience API.

`mparser_module_compile_utf8_with_options` combines an in-memory entry source
with the production `SourceLoader`. The source name acts as the entry path,
its directory and ordered `mparser_source_load_options.search_paths` define
dependency lookup, and discovered package/private/class-folder/function files
join the graph. A relative source name is resolved from the host process's
current directory and returned loaded names are normalized UTF-8 filesystem
paths. The module owns a copy of the entry text before the call returns.

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
Each unit also exposes `mparser_module_source_kind`, primary-function name,
top-level-statement presence, and pure-function-file classification. Unknown
or out-of-range units return neutral values rather than borrowed storage.
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
- a cross-thread cancellation token;
- an optional synchronous host output callback and opaque user pointer.

Zero means unlimited for every numeric resource limit. The execution behavior
and JIT suppression rules are the same as the v0.82 C++ contract.

`mparser_module_execute` is stateless. A session created by
`mparser_module_create_session` preserves global and persistent state across
calls and supports targeted global clearing, clearing all globals, and reset.
Both paths return an owned `mparser_result`.

The corresponding `*_with_system_context` entry points use the host-provided
context described above. Execution-tier selection and VM fallback are
unchanged; system builtins remain ordinary context-aware registry calls.

An API status of `OK` means the host request was accepted and a result was
created. It does not by itself mean that MATLAB-like execution succeeded.
Inspect `mparser_result_status` or `mparser_result_succeeded`, then inspect
outputs, variables, diagnostics, and the execution summary.

## Output And Top-Level Expressions

`disp` emits a display event. The current `fprintf(format, ...)` form emits a
standard-output event and, when requested, returns the emitted UTF-8 byte
count. `sprintf(format, ...)` returns a character row vector without emitting
an event. The frozen v1.2 host surface does not expose file ownership; the
active v1.3 `*_with_system_context` calls expose session/context-owned file
identifiers when the host grants filesystem capabilities.

The `fprintf`/`sprintf` formatter accepts static flags/width/precision and
`d`, `i`, `u`, `f`, `F`, `e`, `E`, `g`, `G`, `s`, and `c` conversions. It
formats numeric arrays in column-major order by repeating the format, treats
text precision/counting by Unicode code point, and accepts a valid Unicode
scalar value for numeric `%c`. Dynamic `*` width/precision, length modifiers,
complex numeric conversions, and unsupported conversions produce
`MParser:InvalidFormattedOutput`. Width is capped at 1 MiB, precision at 4096,
and one formatted result at 16 MiB.

Set `mparser_invocation_options.output_sink` to receive events as they occur.
The callback's text and source pointers are borrowed only for the call. Return
`MPARSER_OUTPUT_ACCEPT` to continue or `MPARSER_OUTPUT_REJECT` to produce a
runtime failure with `MParser:OutputSinkRejected`. The callback runs
synchronously while the module/session execution lock is held; it must return
promptly and must not re-enter the executing module or session. Every event is
also retained in the result and can be read through the
`mparser_result_output_event_*` getters.

Script expression statements are retained through
`mparser_result_top_level_expression_*`. Both unsuppressed and semicolon-
suppressed expressions update `ans`; the suppression bit controls human
display but does not remove the value from the result. Assignments remain
available through result workspace variables rather than this expression
list. Output events and top-level expressions share a zero-based monotonically
increasing `uint64_t sequence`, so a host can merge the two arrays in original
execution order. Source names/ranges remain borrowed from the result; values
returned by the expression getter are owned handles.

Top-level expression values use the same strong ownership as ordinary result
values. Object and function-handle expressions retain their runtime graphs
until the result and any derived value handles are released; this can extend
handle/listener lifetimes across later script statements. Release results
promptly, or use explicit language-level `delete`, when deterministic handle
teardown matters.

## Values

The external value model supports:

- double, single, logical, and all fixed-width signed/unsigned integer
  scalars/arrays;
- complex double and single scalars/arrays;
- UTF-16 character arrays;
- UTF-16 string arrays, including missing elements;
- N-dimensional Cell arrays;
- scalar structures;
- returned object arrays;
- named, anonymous, and builtin function handles;
- scalar and N-dimensional missing values.

Array constructors require a rank of at least two and a dimension product
equal to the supplied element count. Dimensions and payloads are copied.
Numeric values use `mparser_numeric_buffer`. `numeric_class` selects `double`,
`float`, `int8_t`/`uint8_t`, `int16_t`/`uint16_t`, `int32_t`/`uint32_t`, or
`int64_t`/`uint64_t` storage. Logical data uses `uint8_t`. Complex values are
limited to double/single and carry equal-length real and imaginary buffers;
integer and logical values must be real. Character and string payloads use
Unicode code units represented by `uint16_t`.

`mparser_value_create_missing()` creates the language-visible 1-by-1 missing
value. `mparser_value_create_missing_array(dimensions, rank, ...)` creates a
shape-only missing array with rank at least two; its checked dimension product
is exposed by the ordinary rank/dimension/element-count accessors. Both return
`MPARSER_VALUE_MISSING`. The internal absent-result sentinel is not exposed by
either constructor.

All external array payloads and linear element indexes use MATLAB column-major
order. MParser converts to and from its internal storage without exposing that
storage layout. Accessor pointers are immutable and owned by the value handle.
There is no zero-copy external input buffer or mutable view in the current
API. Host-created arrays are copied.
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

`mparser_invocation_options`, `mparser_execution_summary`,
`mparser_source_load_options`, and `mparser_system_context_options` start with
`struct_size` and `abi_generation`.
Initialize current source with the uppercase `MPARSER_*_INIT` macros; do not
use aggregate literals. The macros pass the caller's complete storage size and
current ABI generation to the sized initializers. Input readers ignore unknown
tails, and output writers stop at the caller's recorded capacity. The direct
initializer functions initialize exactly the current known record size.

The public minimum-size constants use neutral `MPARSER_*_SIZE` names. They
describe record sizes, not the active ABI or SDK product version.

`mparser_source_unit` is sealed within ABI generation 2 because arrays use its fixed
size as the descriptor stride. Oversized source-unit descriptors are rejected.
Current structure and symbol rules are in
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
- in-memory source loading with search paths and source classification;
- non-ASCII entry/search paths and retained UTF-8 source names;
- scalar and multiple-output invocation;
- bounded formatting, synchronous output callbacks, retained ordered events,
  and suppressed/unsuppressed top-level expression values;
- MATLAB column-major numeric, character, and string round trips;
- exact single/integer buffers and complex real/imaginary round trips;
- Cell and Struct construction with independent child lifetimes;
- script workspace injection and result-variable lookup;
- object pass-through after releasing the producing result;
- same-module closures and cross-module ownership rejection;
- cross-module independent builtin handles;
- resource stops, pre-cancellation, execution summaries, and session recovery;
- retain/release and ABI-request validation;
- caller-capacity write bounds, oversized request/load/summary storage, and
  sealed source-unit rejection.

`c_api_utf8_source_graph_smoke` creates non-ASCII entry and library
directories. `c_embedding_demo_smoke` runs the C host, including output and
top-level result handling, and
`c_source_graph_demo_smoke` loads a real `+package/@Class` graph.
`c_abi_compat_demo_smoke` exercises current caller-sized future-tail storage;
it does not test an older SDK. `c_api_layout_contract` checks the current
64-bit public record layout and constant ranges.
`c_api_allocation_failure` intercepts global allocation across
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

`c_api_shared_library_abi` compares the live dynamic export table against
`tests/c_api_generation2_revision1_symbols.txt` and validates ELF SONAME or
macOS install-name major 2. The frozen revision-0 manifest remains in
`tests/c_api_generation2_symbols.txt`. Internal compiler, VM, C++ facade, and
SLJIT symbols use hidden
visibility. Windows x64, Linux x64, macOS x64/ARM64, and focused Linux
AArch64 jobs execute the applicable ABI and stress evidence.

`installed_c_consumer_smoke` installs the SDK, moves its prefix, configures a
separate C11 project through `find_package`, and verifies C ABI execution,
in-memory source metadata, output callbacks/results, top-level expressions,
and the imported CLI on Windows x64 and Linux x64. The AArch64 job independently
installs and cross-consumes both native-JIT and portable packages, then runs
the installed consumer and CLI under QEMU.

With `MPARSER_ENABLE_RELEASE_PACKAGING=ON`,
`mparser_release_package` creates a platform/architecture-named ZIP or TGZ
plus SHA-256 records and an unsigned SLSA Provenance v1 statement.
`release_archive_smoke` packages one built payload twice with a fixed
timestamp, verifies archive paths, checksums, and provenance semantics,
unpacks it, and consumes only that SDK from independent C11 and C++20
projects. These records provide integrity and audit evidence, not publisher
identity. Tag `v1.0.0` completed the final authenticated operation in Actions
run `30780391460`; the retained evidence binds all ten release subjects.

## Licensing

MParser's first-party source and documentation are licensed under the Apache
License, Version 2.0, with `Copyright 2026 Wang Xin`. The source tree and
installed SDK include `LICENSE`, `NOTICE`, and `THIRD_PARTY_NOTICES.md`.
The relocated CMake package exports `MParser_LICENSE`, `MParser_COPYRIGHT`,
and checked paths to all three files. Vendored SLJIT remains under the
Simplified BSD terms reproduced in the third-party notices.

## Current Development Boundary

The frozen v1.2 candidate host surface is C source API 1.2, C ABI generation 2
revision 0 with 109 exports, header-only C++ source API 1.2, and machine
protocol 1.1. The active v1.3 tree additively advances the live ABI to revision
1 with 117 exports and rooted system-context calls. These contracts are
versioned independently for technical checks, while the CLI, libraries,
headers, and installed SDK still report MParser version 1.2.0 until the v1.3
gate. Current in-repository and relocated consumers move together. The frozen
v1.2 snapshot remains `docs/public-contract-v1.2.json`.

The v1.0 contract, package, hashes, and authentication evidence remain
available as a historical release record in `docs/public-contract-v1.json`
and the v1.0 milestone documents. They do not require ABI 2 to preserve old
development adapters.

The current API excludes an independently compiled native callback ABI and
zero-copy borrowed input arrays. Their future rules are defined in
`extending-builtins.md`; neither blocks `.m` functions, source-integrated C++
builtins, or normal host invocation.

The CLI schema and exit/channel contract are defined separately in
[machine-result-protocol.md](machine-result-protocol.md).
