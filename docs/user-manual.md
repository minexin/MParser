# MParser User Manual

MParser is an embeddable MATLAB-like subset runtime. Its v1.0 contract covers
a documented language subset, a production bytecode VM, guarded typed/JIT
execution, a command-line interface, and C/C++ embedding APIs. It does not
claim complete MATLAB or toolbox compatibility.

The machine-readable [compatibility matrix](compatibility-matrix.json) is the
authority for individual support claims. This manual describes the normal
user workflow.

## Quick Start

After building from source, run a script through the stable production
interface:

```powershell
build\windows-msvc-release\mparser.exe --run samples\production_run_demo.m
```

```bash
./build/linux-release/mparser --run samples/production_run_demo.m
```

An installed or unpacked release places `mparser` under its `bin` directory.
The equivalent command is:

```text
mparser --run path/to/script.m
```

`--run` compiles the source graph once, executes the full bytecode semantics
once, and uses eligible typed or native regions according to `--jit`. It is
the interface intended for terminals, editor Run buttons, and applications.
The historical `--run-*` commands are diagnostic interfaces.

See [Build And Install](build-and-install.md) for compiler and packaging
instructions and [CLI Reference](cli-reference.md) for every mode and option.

## Production Execution

The default policy is `--jit=auto`:

```text
mparser --run --jit=auto script.m
mparser --run --jit=off script.m
mparser --run --jit=portable script.m
mparser --run --jit=native script.m
```

All four policies preserve the same language contract. Unsupported optimized
regions continue in the bytecode VM; optimization coverage is not a
correctness requirement. A build configured with
`MPARSER_ENABLE_NATIVE_JIT=OFF` still supports `--run`, the VM, and the
portable typed kernel.

`--jit=native` requests the native backend. If native code cannot be used for
a legal region, guarded fallback preserves execution. It does not turn
otherwise unsupported language behavior into supported behavior.

For the exact tier and cache rules, read [JIT And Fallback](jit-and-fallback.md).

## Scripts And Source Graphs

A command accepts exactly one entry source. MParser may load additional
functions, packages, private functions, and class-folder sources from the
entry directory and repeatable search paths:

```text
mparser --run --path=lib --class-path=classes app/main.m
```

`--path` and `--class-path` are spelling-compatible aliases at the CLI
boundary. Both add source-graph search directories. Use `--` before an entry
path that begins with `-`:

```text
mparser --run -- -generated-name.m
```

Local functions use isolated call frames. Script workspace, function local
workspace, `global`, and per-function `persistent` bindings follow the
supported subset recorded in the compatibility matrix. Reusable embedding
sessions preserve their explicit session workspace; one-shot `--run` does
not persist state between processes.

## Functions And Arguments

The target subset includes:

- local and cross-file functions;
- positional, repeating, and name-value arguments;
- `nargin`, `nargout`, multiple outputs, and function handles;
- named, anonymous, builtin, and supported method handles;
- dynamic invocation through the documented call/handle rules.

Invoke a function entry instead of the script body with:

```text
mparser --run --entry-function=calculate --argument=2 \
  --argument=[1,2,3] --outputs=2 functions.m
```

CLI argument values are intentionally narrow: numeric scalars, numeric row
vectors, quoted UTF-8 string scalars, and `name=value` arguments. Rich values,
initial workspaces, reusable modules, cancellation, and resource limits are
available through the C or C++ embedding API.

## Arrays And Values

MParser uses MATLAB column-major linear order at language, C, C++, and machine
protocol boundaries. The v1.0 target subset includes:

- dense double and logical scalars, vectors, matrices, and N-dimensional
  arrays;
- colon and `end` indexing, logical indexing, indexed growth, and deletion
  within the matrix's documented limits;
- UTF-16 character arrays and string arrays;
- N-dimensional Cell arrays;
- ordered structures, structure arrays, dynamic fields, and comma-separated
  field results;
- value, handle, and supported heterogeneous object arrays.

Indexing and nested assignment use transactional root-and-path semantics. A
failed nested mutation does not commit a partially modified root value.
Complex numbers, sparse arrays, tables, timetables, GPU arrays, and arbitrary
MATLAB domain objects are outside the v1.0 contract.

## Control Flow And Exceptions

The target subset includes `for`, serial `parfor`, `while`, `if`, `switch`,
`try`/`catch`, `break`, `continue`, and `return`. `parfor` is accepted with
serial semantics; it is not a parallel execution promise.

Structured diagnostics retain an identifier, phase, severity, source range,
stack frames, and nested causes where available. The supported exception
surface includes `MException` construction and reporting, throw/rethrow
policies, warnings and `lastwarn`, and assertion failures. Unsupported
exception-correction or object behavior is diagnosed instead of approximated.

Human CLI diagnostics are written to stderr. Machine mode emits structured
diagnostics in its single JSON result. Embedding hosts receive copied
diagnostic records owned by the module or result. See
[Runtime Boundaries](runtime-boundaries.md).

## Classes And Objects

The parser and production runtime support the target `classdef` subset:

- properties, methods, events, enumeration blocks, arguments blocks, and
  inheritance syntax;
- value and handle construction, method dispatch, superclass calls, and
  access checks;
- dependent, constant, abstract, observable, validated, and accessor-backed
  properties;
- listeners, dynamic properties, explicit handle deletion, and validity;
- class/member/function/signature/argument/namespace metadata queries;
- supported value, handle, and `matlab.mixin.Heterogeneous` object arrays.

This is a deliberately bounded object model. MATLAB metaclass identity,
dynamic loading behavior, undocumented reflection details, Java/.NET
interoperation, and every built-in MATLAB class are not implied by syntax
acceptance. Consult `CLASS-001` through `CLASS-006` in the compatibility
matrix before depending on a specific object behavior.

## Automation

Do not parse the human variable display. Request the versioned protocol:

```text
mparser --run --result-format=json-v1 script.m
```

Machine mode writes one `mparser.result` 1.x JSON document followed by one LF
to stdout and keeps stderr empty. Its exit classes are:

| Code | Outcome |
| ---: | --- |
| 0 | succeeded |
| 1 | compilation failed |
| 2 | request rejected |
| 3 | runtime failed |
| 4 | emergency serialization or output transport failure |

Read [Machine Result Protocol](machine-result-protocol.md) and validate
against [machine-result-v1.schema.json](machine-result-v1.schema.json).

## Embedding

Choose the narrowest boundary that fits the host:

| Host need | Interface |
| --- | --- |
| One process invocation and JSON | CLI `--run --result-format=json-v1` |
| Stable binary boundary from C or another FFI | C ABI 1.1 |
| C++20 RAII and copied STL-facing values | Header-only C++ source API 1.0 |
| Builtin compiled into the engine | Builtin source contract 1.0 |

The C and C++ APIs compile once and invoke many times, expose sessions,
structured values, diagnostics, cancellation, limits, and execution summaries.
The builtin registry is a source-integration mechanism, not an external plugin
ABI. Independently compiled native callbacks and borrowed zero-copy input
arrays are Post-v1.0.

Start with [C Embedding API](embedding-c-api.md),
[C++ Embedding SDK](embedding-cpp-api.md), or
[Extending Builtins](extending-builtins.md).

## Explicit Boundaries

The following are not v1.0 release claims:

- complete MATLAB compatibility;
- Live Scripts, P-code, MEX, Simulink, graphics, or desktop UI integration;
- MATLAB toolboxes and their long-tail function catalogs;
- complex, sparse, GPU, table, timetable, or every domain-specific value;
- parallel `parfor`;
- an external binary plugin ABI or zero-copy borrowed array ABI;
- a persistent on-disk native-code cache.

Long-tail builtin and toolbox growth is v1.x work performed through the
frozen extension rules. See [Support Matrix](support-matrix.md) for the
release-level summary and [Migration To v1.0](migration-v1.0.md) before
upgrading a pre-v1 integration.
