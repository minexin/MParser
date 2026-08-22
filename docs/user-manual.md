# MParser User Manual

MParser is an embeddable MATLAB-like subset runtime. The published v1.0
baseline and frozen v1.2 candidate cover a documented language subset,
a production bytecode VM, guarded typed/JIT execution, a command-line
interface, and C/C++ embedding APIs. It does not claim complete MATLAB or toolbox compatibility.
Later unreleased milestones may replace their own
development interfaces without requiring adapters for superseded drafts.

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

The script console supports `disp`, `fprintf`, and pure `sprintf`.
Unsuppressed expression statements print `ans = value` using the session's
`format` and line-spacing state; semicolon-suppressed expressions update `ans`
without display. The frozen v1.2 embedding surface routes console output only.
The active v1.3 CLI additionally gives each invocation a native system context
and allows `fprintf(fid,...)` for a file opened in that same session.

The active v1.3 low-level file slice is:

```matlab
name = fullfile(tempdir, 'values.txt');
fid = fopen(name, 'w');
fprintf(fid, '%d %d %d', 1, 2, 3);
fclose(fid);

fid = fopen(name, 'r');
[values, count] = fscanf(fid, '%d', [2 2]);
frewind(fid);
firstLine = fgetl(fid);
atEnd = feof(fid);
fclose(fid);

fid = fopen(name, 'r+');
[value, count] = fscanf(fid, '%d', 1);
position = ftell(fid);
fseek(fid, 0, 'cof');
fprintf(fid, '%d', value + 1);
frewind(fid);
fclose(fid);

binaryName = fullfile(tempdir, 'values.bin');
fid = fopen(binaryName, 'w+', 'ieee-be', 'UTF-8');
written = fwrite(fid, uint16([1 258 65535]), 'uint16');
frewind(fid);
[binaryValues, readCount] = fread(fid, [2 2], 'uint16=>uint16');
fclose(fid);
```

`fopen` currently accepts `r`, `w`, or `a`, optional `+`, and optional `b` or
`t`. Its optional machine format accepts native, little-endian, and big-endian
spellings; UTF-8 is the only accepted encoding. `fscanf` supports `%c`, `%s`, `%d`, `%i`,
`%u`, `%o`, `%x`, `%e`, `%f`, `%g`, the corresponding documented long integer
forms, assignment suppression, field widths, literals, repeated formats, and
scalar or two-dimensional sizes. Finite numeric shapes are zero padded in
column order. Reads are bounded to 16 MiB, scan output to 16 million elements,
and one session defaults to 256 open files. `fclose('all')` and `fopen('all')`
operate only on that session.

`fgetl` removes CR, LF, CRLF, or LFCR terminators. `fgets` retains the
terminator, accepts an optional positive character limit, and can return its
terminator code as a second output. Both return numeric `-1` when no input
remains. `feof` becomes true only after a read consumes the logical remainder;
successful `fseek` and `frewind` clear it. `ferror` returns the most recent
per-file message and MParser error number, and `ferror(fid,'clear')` returns
then clears that state.

`fread` and `fwrite` support fixed-width logical, signed/unsigned 8/16/32/64-bit
integer, single, and double precisions with common MATLAB aliases. `fread`
accepts `source`, `source=>output`, `*source`, and `N*source` forms, scalar or
two-dimensional sizes, byte skips, and per-call byte-order overrides. Finite
shapes are zero padded in column-major order and exact 64-bit output classes
retain their payload bits. `fwrite` converts numeric arrays in column-major
order and applies skips between precision blocks without overwriting existing
gap bytes on ordinary update streams.

`fseek` accepts signed integer byte offsets with `bof`/`cof`/`eof` or numeric
origins `-1`/`0`/`1`; `ftell` reports a zero-based byte position and `frewind`
is `fseek(fid,0,'bof')`. As in MATLAB, a `+` update stream requires `fseek` or
`frewind` between a read and a write. Scanner prefetch remains invisible to
`ftell`; Windows text streams retain a compact CRLF-to-byte mapping so a
current-relative seek uses the physical file position rather than the
translated string length.

This does not yet include scansets, bit/ubit or encoding-dependent character
binary precisions, direct complex `fwrite`, non-UTF-8 encodings, remote URLs,
or MAT-file persistence.
See [C Embedding API](embedding-c-api.md#output-and-top-level-expressions) for
the frozen host-output conversions and limits.

Within `(...)`, split an expression across physical lines with `...`; a bare
newline ends the statement and is a parser error if the call is unfinished.
Within `[...]` and `{...}`, a bare newline is a row separator, just like `;`.

Local functions use isolated call frames. Script workspace, function local
workspace, `global`, and per-function `persistent` bindings follow the
supported subset recorded in the compatibility matrix. Reusable embedding
sessions preserve their explicit session workspace; one-shot `--run` does
not persist state between processes.

The active v1.3 tree supports dynamic workspace execution through `eval`,
`evalc`, `evalin`, and `assignin`. Character rows and string scalars are
accepted as source. `eval` uses the current workspace, `evalin` selects
`caller` or `base`, and `assignin` writes one validated variable name to one
of those two workspaces. Expression forms support multiple outputs; `evalc`
returns captured console text first and any requested expression outputs
after it. The legacy catch-expression argument executes in the same selected
workspace, and assignments completed before a runtime error remain visible.
Compilation failure does not modify the workspace. A value already present
in the selected workspace shadows a same-named builtin or source function at
runtime; parenthesis access, `end`, and `:` then use array-index semantics.
An evaluated `evalin`/`assignin('caller',...)` call sees the caller of the
original function; the temporary evaluator does not insert a visible MATLAB
function frame.

```matlab
seed = 40;
value = eval('seed + 2');
[rows, columns] = eval('size(ones(2, 3))');
text = evalc('disp(value)');
assignin('base', 'row', [missing missing]);
row = evalin('base', 'row');
sin = [10 20 30];
last = eval('sin(end)');
```

Dynamic source is limited to 16 MiB and 1024 requested outputs and is always
compiled through the normal frontend and production bytecode VM. The owning
system context must grant `DynamicEvaluation`; isolated sessions grant no
such capability, while the local CLI native context does. Explicit HIR and
bytecode modes keep evaluated source on bytecode; production mode may run
eligible loops through its guarded portable/native backend. Dynamic declarations
of functions, classes, `global`, or `persistent` values are not yet accepted,
and a function handle created by the temporary dynamic module cannot escape
it. Parent-module local/path/package functions and module-bound workspace
handles are not callable from the temporary source graph yet. Pre-existing
handles may pass through unchanged and remain callable after control returns;
builtins and other selected workspace values remain available. If new dynamic
handles are written into a pre-existing shared object, its fields are restored
before the escape error is reported.

A system command may also be written in MATLAB shell-escape form at the start
of a statement:

```matlab
!echo mparser_system_command
```

The lexer preserves the rest of that physical line verbatim and the parser
routes it through zero-output `system(...)`. It therefore has the same host
adapter, output, platform-shell, and `Process` capability boundaries as an
ordinary `system` call. Because `!` is syntax rather than a name lookup, a
workspace variable or function named `system` does not intercept it.

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

The active v1.3 development library includes `lower`, `upper`, `strtrim`,
`num2str`, `strsplit`, a portable `regexp` subset, `sort`, `unique`, `iscell`,
`cellfun`, `struct2cell`, and `cell2struct`. Ordering preserves dense numeric
classes and supports N-dimensional dimensions; missing arrays remain
shape-only. `cellfun` accepts function handles or text names and supports
multiple Cell inputs/outputs, `UniformOutput`, and `ErrorHandler` through the
same call-frame rules used by direct invocation.

The next utility slice adds `factorial`, `gcd`, `lcm`, `isprime`, `primes`,
`logspace`, one- to three-dimensional `meshgrid`, generic N-dimensional
`flip`, `flipud`, `fliplr`, UTF-16 `strfind`/`strrep`, and session-random
`randperm`. Numeric utilities preserve supported input classes and shapes
where MATLAB does; `gcd` currently returns only the common divisor, and
`meshgrid` accepts at most three inputs. `primes` requires a real integer
scalar; `logspace` floors a finite fractional point count and produces an
empty row for a nonpositive count. Text search reports UTF-16 code-unit
positions, including overlaps. A `missing` call constructs the scalar seed,
but the runtime value is shape-carrying: concatenation, `repmat`, indexing,
and transforms such as `flip` operate on arbitrary N-dimensional missing
arrays without allocating an element payload.

See `samples/standard_library_demo.m` and
`samples/utility_library_demo.m` for executable examples. Locale-wide Unicode
case conversion, the complete MATLAB regular-expression dialect, extended
GCD coefficients, and all long-tail overloads remain outside this development
slice.

## Arrays And Values

MParser uses MATLAB column-major linear order at language, C, C++, and machine
protocol boundaries. The current target subset includes:

- dense `double`, `single`, logical, `int8`/`uint8`, `int16`/`uint16`,
  `int32`/`uint32`, and exact `int64`/`uint64` scalars and N-dimensional
  arrays;
- scalar-`double` arithmetic with `int64`/`uint64` preserves integer bits above
  `flintmax`, emulates MATLAB's binary80 intermediate rounding before the
  nearest-with-ties-away integer conversion, and saturates at the target
  bounds for scalar or implicitly expanded array operations;
- dense complex `double` and `single` values with separate real and imaginary
  components;
- colon and `end` indexing, logical indexing, indexed growth, and deletion
  within the matrix's documented limits;
- UTF-16 character arrays and string arrays;
- scalar and N-dimensional `missing` arrays; `[missing missing]` is a 1-by-2
  missing array, floating numeric combinations convert missing elements to
  NaN, and string combinations retain per-element missing state;
- N-dimensional Cell arrays;
- ordered structures, structure arrays, dynamic fields, and comma-separated
  field results;
- value, handle, and supported heterogeneous object arrays.

Indexing and nested assignment use transactional root-and-path semantics. A
failed nested mutation does not commit a partially modified root value.
Sparse arrays, complex integer arrays, tables, timetables, GPU arrays, and
arbitrary MATLAB domain objects are outside the current contract.

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
to stdout and keeps stderr empty. Script output and top-level expression
results are represented by ordered protocol arrays rather than unframed
console bytes. Its exit classes are:

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
| Narrow binary boundary from C or another FFI | C source API 1.2, ABI generation 2 |
| C++20 RAII and copied STL-facing values | Header-only C++ source API 1.2 |
| Builtin compiled into the engine | Frozen v1.2 source contract 1.1; active in-tree contract 1.5 |

The C and C++ APIs compile once and invoke many times, expose sessions,
structured values, diagnostics, cancellation, limits, execution summaries,
source metadata, synchronous output sinks, retained output events, and
top-level expression results.
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
- sparse, complex-integer, GPU, table, timetable, or every domain-specific value;
- parallel `parfor`;
- an external binary plugin ABI or zero-copy borrowed array ABI;
- a persistent on-disk native-code cache.

Long-tail builtin and toolbox growth is staged v1.x work performed through the
shared extension rules. See [Support Matrix](support-matrix.md) for the
current summary. The [Migration To v1.0](migration-v1.0.md) guide applies only
to the archived v1.0 release boundary.
