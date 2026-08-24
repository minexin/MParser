# MParser C++ Embedding SDK

`include/mparser/cpp_api.hpp` provides the header-only MParser C++20 embedding
facade. The v1.3 candidate reports source API 1.3 over C ABI generation 2
revision 1 and includes the public `SystemContext` facade. MParser and the
installed SDK report product version `1.3.0`; source API, ABI, and protocol
identifiers remain independently queryable contract metadata.

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
`MParser_CPP_INCLUDE_DIR`, engine/C API version components, and C ABI
generation/revision metadata. It also exports C++ source API `1.3`, machine
result protocol `1.1`,
CLI contract `1.0`, builtin source contract `1.13`, and checked paths to the
public/CLI contracts, protocol schema, builtin catalog/author guide, and
versioning policy. On Windows, deploy `mparser_c.dll` beside the host
executable or add the installed `bin` directory to the runtime loader path.

Hosts can query the header declaration directly:

```cpp
static_assert(mparser::sdk::kSourceApiVersionMajor == 1);
static_assert(mparser::sdk::kSourceApiVersionMinor == 3);
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

`Module`, `Session`, `Result`, `Value`, `CancellationToken`, and
`SystemContext` are copyable RAII handles. Copying retains the opaque C handle,
moving transfers the reference, and destruction releases it. An extracted
output, workspace value, Cell element, or Struct field remains valid after its
parent wrapper is destroyed because the C API returns an owned reference.

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

## System Contexts

`SystemContext::rootedNative(SystemContextOptions)` exposes the v1.3 native
host services without leaking `RuntimeSystemContext` or filesystem types
through the shared-library ABI. The host chooses a root directory, optional
current/temporary/search directories, random seed, open/read limits, and an
explicit `SystemCapability` mask. Bind it with
`Module::execute(invocation, context)` or `Module::createSession(context)`:

```cpp
mparser::sdk::SystemContextOptions options;
options.rootDirectory = sandboxPath;
options.temporaryDirectory = sandboxPath + "/tmp";
options.capabilities =
    mparser::sdk::SystemCapability::CurrentDirectory |
    mparser::sdk::SystemCapability::FileSystemRead |
    mparser::sdk::SystemCapability::FileSystemWrite |
    mparser::sdk::SystemCapability::Random;

auto context = mparser::sdk::SystemContext::rootedNative(options);
auto session = module.createSession(context);
context = {};
auto result = session.execute();
```

The session retains the context. Reusing one context across calls shares its
current directory, paths, random sequence, and open files; use separate
contexts for isolation. Path-oriented operations reject resolved paths outside
the root, including existing escaping symbolic links. Environment/process
capabilities remain host-wide, and the root is not an OS sandbox or protection
against a hostile process racing link changes during a call. The complete
runnable example is `samples/cpp_system_context_demo.cpp`.

## Compilation And Loading

`Module::compile(source, name)` is the one-source convenience form.
`Module::compile(span<SourceUnit>)` compiles an ordered in-memory source graph.
`Module::compile(source, name, SourceLoadOptions)` treats the in-memory source
as a production entry path and discovers dependencies through its directory
and ordered search paths. Relative names are normalized from the host
process's current directory.
`Module::loadFile(path, SourceLoadOptions)` uses the production UTF-8
filesystem loader for package, private, path, and class-folder semantics.

Language compilation and filesystem loading failures return an inspectable
invalid `Module`. `Module::isValid()` is false and `Module::diagnostics()`
contains source-aware compilation diagnostics. Invalid API arguments,
allocation failures, ownership mismatches, and internal boundary failures
throw `ApiError` instead of returning a module.

`sourceNames()` and `functionNames()` return copied strings.
`sourceMetadata()` additionally returns each unit's `SourceKind`, primary
function, top-level-statement flag, and pure-function-file flag. The module may
be compiled once and used for any number of stateless or session invocations.

## Invocation And Results

`Invocation` controls:

- entry function and positional arguments;
- optional requested output count and initial workspace;
- automatic, bytecode, portable, or native backend selection;
- profile collection;
- instruction, wall-time, call-depth, per-value array-byte, and diagnostic
  limits;
- cooperative cancellation through `CancellationToken`;
- a synchronous `OutputSink` receiving copied `OutputEvent` records.

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

`Result::outputEvents()` returns the retained display/stdout records, while
`Result::topLevelExpressions()` returns script expression values, source
ranges, suppression flags, and sequence numbers. The two arrays share one
zero-based sequence so they can be merged into original execution order.
Semicolon-suppressed expressions update `ans` and remain available to the
host, but ordinary human output omits them. Assignments are represented in
`Result::variables()` rather than as expression records.

Expression values follow normal strong `Result`/`Value` ownership. Object and
function-handle expressions therefore retain their runtime graphs until the
result and copied values are destroyed, which can extend handle/listener
lifetimes across later statements. Use explicit language-level `delete` or
short result lifetimes when deterministic teardown is required.

Returning `false` from `Invocation::outputSink` produces
`MParser:OutputSinkRejected`. If a C++ output sink throws, the facade catches
the exception at the C callback boundary, rejects runtime output, and rethrows
the original exception after the C call returns; no C++ exception crosses the
shared-library ABI. A sink is invoked synchronously while execution owns its
module/session lock and must not re-enter that module/session.

The isolated `disp`, stdout-only `fprintf`, and pure `sprintf` formatter is the
same bounded subset described in [C Embedding API](embedding-c-api.md).
File-targeted formatting and stream I/O require a bound system context with
filesystem capability; neither surface claims complete MATLAB formatting.

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
- scalar and N-dimensional missing values.

`Value::missing()` creates a 1-by-1 language value. Use
`Value::missingArray(dimensions)` for a shaped missing array; `dimensions()`
and `elementCount()` report its logical shape even though it has no per-element
payload exposed to the host.

Dimensions, numeric payloads, text payloads, and Cell/Struct children are
copied on construction. Array payload and linear element order is MATLAB
column-major. `numericClass()` and `isComplex()` describe numeric storage;
`numericData<Element>()` and `numericImaginaryData<Element>()` return typed
immutable spans and reject an element type that does not match the numeric
class. Their lifetime is tied to that `Value`, and the live API permits these
borrowed-view methods only on lvalue wrappers. Store a returned `Value` before
requesting its span; calls such as `result.output(0).numericData()` are rejected
at compile time. `characterData()` has the same lvalue-only rule, while
`stringElement`, `cellElement`, and `structField` return copies or independently
retained values as documented by their types.

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

Source-tree and relocated installed consumers exercise source API 1.3 through
compile-once invocation, exact typed and complex numeric values, multi-output
results, composite values, retained lifetimes, diagnostics, sessions,
cancellation, resource limits, UTF-8 source graphs and metadata, synchronous
output sinks, ordered output/expression results, and rooted system-context
creation/retention. The installed
reference consumer spans multiple translation units and checks product, C ABI,
C++ API, host output behavior, and protocol metadata after relocation.

Lifecycle and concurrency stress covers pure calls, shared handle mutation,
same and independent sessions, cross-session escaped objects, shared
cancellation, isolated limits, and concurrent retain/release. The v1.3
candidate library contract is revision 1 with an exact 117-symbol manifest;
the archived v1.2 boundary remains revision 0 with 109 exports. The
C++ facade remains header-only rather than a C++ binary ABI.

The current header and consumers move together. Source API 1.3 is frozen in
the v1.3 candidate snapshot; source API 1.2 and 1.0 remain archived v1.2 and
v1.0 evidence. The full Windows/Linux/macOS x64/ARM64 SDK matrix is likewise
run at the milestone gate instead of after every internal batch.

Distribution licensing is Apache-2.0 with `Copyright 2026 Wang Xin`.
Candidate archives include the current headers, contract metadata, schema,
examples, and notices and are validated through their unpacked C/C++ SDK.
