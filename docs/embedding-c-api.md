# MParser C Embedding API

MParser v0.83 introduced a narrow, pure C embedding boundary in
`include/mparser/c_api.h`. It is implemented by the `mparser_c_api` CMake
shared-library target, whose output name is `mparser_c`. v0.84 extends that
same boundary with explicit multi-source compilation and UTF-8 filesystem
source-graph loading.

The header exposes no C++ standard-library type, exception, class layout, or
`RuntimeValue` representation. All state crosses the boundary through opaque
handles, fixed-width constants, byte/code-unit views, and versioned plain C
structures.

This is ABI candidate 1, not the final v1.0 binary freeze. Until the v0.90 API
gate, an incompatible correction may increment `MPARSER_C_ABI_VERSION`.
Applications must query `mparser_c_abi_version()` and the three MParser
component-version functions rather than assuming that the project version and
C ABI version advance together.

## Build

From the source tree:

```powershell
cmake -S . -B build
cmake --build build --target mparser_c_api mparser_c_embedding_demo `
  mparser_c_source_graph_demo
build\mparser_c_embedding_demo.exe
build\mparser_c_source_graph_demo.exe `
  samples\class_folders\app\run_demo.m `
  samples\class_folders\lib
```

On Linux the shared-library output is `libmparser_c.so`; on Windows it is
`mparser_c.dll` plus the toolchain import library. Installation and exported
package targets remain a v0.90 task. v0.84 consumers therefore build against
this source tree and its CMake target.

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
retained reference. Stateless module invocations have isolated runtime state.
Calls through one C session are serialized by the wrapper and share that
session's persistent/global state.

## Compilation And Invocation

`mparser_module_compile_utf8` compiles one owned copy of a UTF-8 source buffer
and source name. It remains the one-source convenience API.

`mparser_module_compile_sources` accepts one or more versioned
`mparser_source_unit` descriptors. Initialize each descriptor with
`mparser_source_unit_init`. The module copies every name and source buffer,
preserves the supplied order, and treats the first unit as the entry source.
This route supports explicit top-level scripts, functions, and classes, but it
does not infer package/private/class-folder identity from source names.

For MATLAB filesystem semantics, initialize `mparser_source_load_options` and
call `mparser_module_load_file_utf8` with an entry `.m` file and ordered search
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

Initialize every invocation with `mparser_invocation_options_init`. The
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
There is no zero-copy external buffer or mutable view in v0.84.

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

## Structure Versioning

`mparser_invocation_options` and `mparser_execution_summary` start with
`struct_size` and `abi_version`. Initialize them through their API functions;
do not use aggregate literals as a substitute. ABI candidate 1 currently
requires the complete current structure size. A later candidate may add
tail-compatible negotiation before the v0.90 freeze.

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
- retain/release and ABI-request validation.

`c_api_utf8_source_graph_smoke` creates non-ASCII entry and library
directories. `c_embedding_demo_smoke` runs the single-source C host and
`c_source_graph_demo_smoke` loads a real `+package/@Class` graph. All are
included in Linux AArch64 native-JIT and portable-only QEMU jobs in addition
to the complete Windows x64 and Linux x64 suites.

## Remaining Freeze Work

The v0.90 embedding gate still requires:

1. installed C/C++ consumer packages;
2. the versioned machine-readable CLI/result protocol;
3. explicit forward-compatible structure and symbol-version policy;
4. external adapter and optional array-view validation;
5. repeated library load/unload, stress, sanitizer, and allocation-failure
   evidence;
6. final supported-platform ABI and package-consumer evidence.
