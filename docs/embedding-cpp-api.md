# MParser C++ Embedding SDK

`include/mparser/cpp_api.hpp` provides the header-only MParser C++20 embedding
facade. The current v1.2 development line reports source API 1.2 and delegates
to C ABI generation 2. The product and installed SDK still share one MParser
version; source API 1.2 is contract metadata, not a separately versioned SDK.

The facade does not expose Parser, HIR, Bytecode, `RuntimeValue`, VM, SLJIT,
or C++ standard-library layouts from the shared library. No C++ exception or
standard-library object crosses that binary boundary. MParser does not promise
a C++ binary ABI; hosts compile the current header facade and link only to the
narrow C library.

## Install And Link

Build and install the SDK:

```powershell
cmake -S . -B build-sdk -DBUILD_TESTING=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix C:\mparser-sdk
```

Consume it from a C++20 CMake project:

```cmake
find_package(MParser CONFIG REQUIRED COMPONENTS CPP CLI)
target_link_libraries(host PRIVATE MParser::cpp_api)
```

`MParser::cpp_api` supplies the include directory, C++20 requirement, and a
transitive link to `MParser::c_api`. `MParser::cli` is the matching imported
executable. The package exports `MParser_CPP_FOUND`,
`MParser_CPP_INCLUDE_DIR`, engine version components, and C ABI major/revision
metadata. It also exports C++ source API `1.2`, machine result protocol `1.1`,
CLI contract `1.0`, builtin source contract `1.0`, and checked paths to the
public/CLI contracts, protocol schema, builtin catalog/author guide, and
versioning policy. On Windows, deploy `mparser_c.dll` beside the host
executable or add the installed `bin` directory to the runtime loader path.

Hosts can query the header declaration directly:

```cpp
static_assert(mparser::sdk::kSourceApiVersionMajor == 1);
static_assert(mparser::sdk::kSourceApiVersionMinor == 2);
const auto version = mparser::sdk::sourceApiVersion();
```

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
    mparser::sdk::Value output = result.output(0);
    double answer = output.numericData().front();
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
state. Copy a wrapper before handing it to another thread so each thread owns
an independent retained reference. Concurrent assignment, move, reset, or
destruction of the same wrapper object is not supported.

Pure stateless `Module::execute` calls have isolated runtime state and may run
concurrently. Calls carrying module-bound objects or closures serialize on
their producing module. A `Session` preserves globals and
function-persistent values; every session operation from one module is
serialized at the module graph boundary and then at the individual session.
This also protects objects that escape from session state. Wall-time and
cancellation accounting begins after this lock admission, so queue wait is
not part of a request's wall-time budget.

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

- double, single, logical, and every fixed-width signed/unsigned integer
  scalar or N-dimensional array;
- complex double and single arrays with separate real and imaginary spans;
- UTF-16 character arrays;
- UTF-16 string arrays with missing elements;
- N-dimensional Cell arrays;
- scalar Struct construction and returned Struct arrays;
- returned opaque object arrays and function handles;
- an explicit missing value.

Dimensions, numeric payloads, text payloads, and Cell/Struct children are
copied on construction. Array payload and linear element order is MATLAB
column-major. `numericClass()` and `isComplex()` describe numeric storage;
`numericData<Element>()` and `numericImaginaryData<Element>()` return typed
immutable spans and reject an element type that does not match the numeric
class. Their lifetime is tied to that `Value`; retain the `Value` instead of
keeping a span returned through a temporary wrapper. `characterData()`,
`stringElement`, `cellElement`, and `structField` return borrowed spans,
copies, or independently retained values as documented by their types.

Structure field indexes follow the order supplied to `Value::structure`; that
order survives a module round trip. The current public constructor creates a
scalar Struct. General external Struct-array construction remains additive
future work.

The current ownership contract uses copy-in for host-created array payloads.
Numeric and character spans are readonly runtime-owned
views tied to one `Value`; no host buffer is borrowed and no writable external
view exists. A later borrowed-input API must use a new C descriptor with
explicit lifetime, alignment, mutability, and thread-safety fields.

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

A boundary failure never returns a partially initialized wrapper. It does not
make invocation globally transactional: a publication/allocation failure
after runtime execution can leave session or object side effects committed.
Do not blindly retry such a call unless the invoked operation is idempotent.
`Invocation::limits.maximumArrayBytes` limits one recursively measured runtime
value, not aggregate heap or RSS. Wall-time starts after module/session lock
admission, so queue deadlines remain a host responsibility.

## Current Boundary

Source-tree and relocated installed consumers exercise source API 1.2 through
compile-once invocation, exact typed and complex numeric values, multi-output
results, composite values, retained lifetimes, diagnostics, sessions,
cancellation, resource limits, and UTF-8 source graphs. The installed
reference consumer spans multiple translation units and checks product, C ABI,
C++ API, and protocol metadata after relocation.

Lifecycle and concurrency stress covers pure calls, shared handle mutation,
same and independent sessions, cross-session escaped objects, shared
cancellation, isolated limits, and concurrent retain/release. The shared C
library carries ABI generation 2 and an exact exported-symbol manifest; the
C++ facade remains header-only rather than a C++ binary ABI.

The current development header and consumers move together. A source API 1.2
snapshot is created when the complete v1.2 milestone reaches its candidate
gate; the source API 1.0 snapshot remains historical v1.0 evidence only. The
full Windows/Linux/macOS x64/ARM64 SDK matrix is likewise run at the milestone
gate instead of after every internal batch.

Distribution licensing is Apache-2.0 with `Copyright 2026 Wang Xin`.
Candidate archives include the current headers, contract metadata, schema,
examples, and notices and are validated through their unpacked C/C++ SDK.
