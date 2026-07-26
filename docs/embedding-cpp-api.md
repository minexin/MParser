# MParser C++ Embedding SDK

MParser v0.88 provides a public C++20 facade in
`include/mparser/cpp_api.hpp`. The facade is header-only and delegates to the
narrow C ABI shared library. It does not expose Parser, HIR, Bytecode,
`RuntimeValue`, VM, SLJIT, or C++ standard-library layouts from the shared
library.

The facade is a source-level API candidate for the v0.90 freeze. Its binary
boundary remains C ABI major 1 revision 1, so no C++ exception or standard
library object crosses the shared-library boundary.

## Install And Link

Build and install the SDK:

```powershell
cmake -S . -B build-sdk -DBUILD_TESTING=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix C:\mparser-sdk
```

Consume it from a C++20 CMake project:

```cmake
find_package(MParser 0.88.0 EXACT CONFIG REQUIRED COMPONENTS CPP CLI)
target_link_libraries(host PRIVATE MParser::cpp_api)
```

`MParser::cpp_api` supplies the include directory, C++20 requirement, and a
transitive link to `MParser::c_api`. `MParser::cli` is the matching imported
executable. The package exports `MParser_CPP_FOUND`,
`MParser_CPP_INCLUDE_DIR`, engine version components, and C ABI major/revision
metadata. On Windows, deploy `mparser_c.dll` beside the host executable or add
the installed `bin` directory to the runtime loader path.

## Minimal Host

```cpp
#include "mparser/cpp_api.hpp"

mparser::sdk::Module module = mparser::sdk::Module::compile(R"(
function out = twice(value)
out = value * 2;
end
)", "host.m");

if (!module.isValid()) {
    for (const auto& diagnostic : module.diagnostics()) {
        // Report diagnostic.message and diagnostic.source when present.
    }
}

mparser::sdk::Invocation request;
request.entryFunction = "twice";
request.arguments = {mparser::sdk::Value::scalar(21)};
request.requestedOutputCount = 1;

mparser::sdk::Result result = module.execute(request);
if (result.succeeded()) {
    double answer = result.output(0).numericData().front();
}
```

The complete runnable example is `samples/cpp_embedding_demo.cpp`.

## Ownership

`Module`, `Session`, `Result`, `Value`, and `CancellationToken` are copyable
RAII handles. Copying retains the opaque C handle, moving transfers the
reference, and destruction releases it. An extracted output, workspace value,
Cell element, or Struct field remains valid after its parent wrapper is
destroyed because the C API returns an owned reference.

Module-bound objects and function handles retain their compiled module. They
can be passed back to that module after the original result is destroyed.
Passing a module-bound value to another module raises `ApiError` with
`MPARSER_API_STATUS_OWNER_MISMATCH`. Module ownership also propagates through
Cell and Struct values.

The wrapper performs no global initialization and holds no process-wide C++
state. Stateless `Module::execute` calls have isolated runtime state. A
`Session` preserves globals and function-persistent values and serializes its
own calls through the underlying C session. Concurrent access rules otherwise
follow the C API: every thread must own its own retained handle reference.

## Compilation And Loading

`Module::compile(source, name)` is the one-source convenience form.
`Module::compile(span<SourceUnit>)` compiles an ordered in-memory source graph.
`Module::loadFile(path, SourceLoadOptions)` uses the production UTF-8
filesystem loader for package, private, path, and class-folder semantics.

Language compilation and filesystem loading failures return an inspectable
invalid `Module`. `Module::isValid()` is false and `Module::diagnostics()`
contains source-aware compilation diagnostics. Invalid API arguments,
allocation failures, ownership mismatches, and internal boundary failures
throw `ApiError` instead of returning a module.

`sourceNames()` and `functionNames()` return copied strings. The module may be
compiled once and used for any number of stateless or session invocations.

## Invocation And Results

`Invocation` controls:

- entry function and positional arguments;
- optional requested output count and initial workspace;
- automatic, bytecode, portable, or native backend selection;
- profile collection;
- instruction, wall-time, call-depth, per-value array-byte, and diagnostic
  limits;
- cooperative cancellation through `CancellationToken`.

All numeric limits use zero for unlimited. `Module::execute()` and
`Session::execute()` without an explicit request execute the top-level script
using automatic backend selection.

A successful C API call always returns a `Result`, even when MATLAB-like
execution fails. Inspect `Result::status()` or `Result::succeeded()`, then read
outputs, workspace variables, diagnostics, and `ExecutionSummary`. Language
validation/runtime errors and resource stops do not throw `ApiError`.

`ExecutionSummary` reports the requested backend, effective tier, fallback,
profiling, typed/native counters, resource observations, stop reason, and
elapsed time. Correct VM execution remains available when a request is not
eligible for portable or native optimization.

## Values

`Value` supports the current external transport model:

- double and logical scalars or N-dimensional arrays;
- UTF-16 character arrays;
- UTF-16 string arrays with missing elements;
- N-dimensional Cell arrays;
- scalar Struct construction and returned Struct arrays;
- returned opaque object arrays and function handles;
- an explicit missing value.

Dimensions, numeric payloads, text payloads, and Cell/Struct children are
copied on construction. Array payload and linear element order is MATLAB
column-major. `numericData()` and `characterData()` return immutable spans
whose lifetime is tied to that `Value`. `stringElement`, `cellElement`, and
`structField` return copied or independently retained values.

Structure field indexes follow the order supplied to `Value::structure`; that
order survives a module round trip. The current public constructor creates a
scalar Struct. General external Struct-array construction and zero-copy
external array views remain open design decisions for v0.89/v0.90.

## Errors And Diagnostics

`ApiError` represents host-side contract or transport failure and preserves
the underlying `mparser_api_status`. It derives from `std::runtime_error` and
contains the operation context plus stable C status name. No exception leaves
the shared library; the header-only facade creates host exceptions after a C
call returns.

Compilation, validation, warning, and runtime diagnostics are ordinary copied
data. Each `Diagnostic` includes phase, severity, identifier, message,
optional source range, stack frames, and recursive causes.

Applications should catch `ApiError` for boundary failures and inspect
`Module`/`Result` diagnostics for language failures. Catching
`std::bad_alloc` remains the host's responsibility when the facade allocates
copied strings, vectors, or diagnostics.

## Current Boundary

v0.88 validates this SDK on Windows x64, Linux x64, and native-JIT plus
portable Linux AArch64 builds. Source-tree and relocated installed consumers
exercise compile-once invocation, multi-output results, composite values,
retained lifetimes, diagnostics, sessions, cancellation, resource limits, and
UTF-8 source graphs.

The API is not frozen until the v0.90 candidate review. v0.89 still needs
lifecycle/concurrency stress, macOS x64/ARM64 consumers, external-adapter and
optional array-view decisions, distribution licensing, and shared-library
versioning evidence.
