# MParser

Current milestone: v0.34.0. See [docs/v0.34.md](docs/v0.34.md) for the
current bytecode VM scope, supported subset, validation commands, and next
iteration plan. Previous boundaries are kept in [docs/v0.33.md](docs/v0.33.md),
[docs/v0.32.md](docs/v0.32.md),
[docs/v0.31.md](docs/v0.31.md),
[docs/v0.30.md](docs/v0.30.md),
[docs/v0.29.md](docs/v0.29.md),
[docs/v0.28.md](docs/v0.28.md),
[docs/v0.27.md](docs/v0.27.md),
[docs/v0.26.md](docs/v0.26.md),
[docs/v0.25.md](docs/v0.25.md),
[docs/v0.24.md](docs/v0.24.md),
[docs/v0.23.md](docs/v0.23.md),
[docs/v0.22.md](docs/v0.22.md),
[docs/v0.21.md](docs/v0.21.md),
[docs/v0.20.md](docs/v0.20.md),
[docs/v0.19.md](docs/v0.19.md),
[docs/v0.18.md](docs/v0.18.md),
[docs/v0.17.md](docs/v0.17.md),
[docs/v0.16.md](docs/v0.16.md),
[docs/v0.15.md](docs/v0.15.md),
[docs/v0.14.md](docs/v0.14.md),
[docs/v0.13.md](docs/v0.13.md),
[docs/v0.12.md](docs/v0.12.md),
[docs/v0.11.md](docs/v0.11.md),
[docs/v0.10.md](docs/v0.10.md),
[docs/v0.9.md](docs/v0.9.md),
[docs/v0.8.md](docs/v0.8.md), [docs/v0.7.md](docs/v0.7.md),
[docs/v0.6.md](docs/v0.6.md), [docs/v0.5.md](docs/v0.5.md),
[docs/v0.4.md](docs/v0.4.md), [docs/v0.3.md](docs/v0.3.md),
[docs/v0.2.md](docs/v0.2.md), and [docs/v0.1.md](docs/v0.1.md).

MParser is the first iteration of a MATLAB-compatible language frontend. The
current focus is syntax coverage and stable compiler boundaries:

```text
source -> lossless tokens -> syntax tree -> semantic HIR -> interpreter/bytecode -> JIT IR
```

The implementation starts with a portable C++20 core and CMake build. The first
milestone is to parse modern MATLAB source, including `classdef`, class member
blocks, functions, and control-flow blocks, while preserving enough source
information for later diagnostics, formatting, semantic analysis, and JIT
profiling.

The parser also builds an initial expression tree for ordinary statements:
assignments, output lists, member access, neutral call/index nodes, brace
indexing, unary/binary operators, function handles, matrix/cell literals, and
meta-class expressions. Dynamic MATLAB ambiguities are intentionally preserved;
for example, `A(x)` is represented as `CallOrIndexExpr` until semantic analysis
can decide whether it is indexing or a function/constructor call.

Control-flow headers are structured too. For example, `for i = 1:10` contains a
`ControlHeader` with an `AssignmentStatement`, while `if x > 0` contains a
header expression.

The semantic layer lowers syntax into a HIR tree with scopes and symbols. It
predeclares local functions/classes and class members, resolves local names,
forward local function calls, anonymous function parameters, knowable class
member access including same-file inherited members declared later, and a small
lazy registry of common MATLAB builtins. Dynamic
calls, indexing, package/static lookup, and unknown member access remain delayed
bindings for later name and type resolution.

The next layer is an initial bytecode path. It linearizes HIR into stack-style
instructions for module/class/function boundaries, assignments, control
headers, literals, names, member access, neutral call/index operations, and
expression operators. v0.34 executes the core numeric/string subset,
`if`/`for`/`while` control flow, same-file local function calls, multi-output
call assignment, MATLAB-style numeric indexing/mutation with `end`, `:`,
vector subscripts, indexed assignment into existing arrays,
`switch/case/otherwise`, and `try/catch` diagnostic recovery. It also records
bytecode VM execution profiles for functions, loops, instructions,
call/index sites, and assignment sites, including runtime value kind and shape
observations. Profile collection is now an explicit VM policy: analysis and
planning commands collect the complete profile, while ordinary bytecode and
post-specialization execution skip per-PC, function, loop, call-site, and
assignment observations. A first optimization planner consumes full profiles
and emits hot, stable candidates with explicit guard records for future typed
IR and JIT work. A small typed IR builder now lowers those candidates into
scalar or typed regions with guard and deoptimization operations. Guard
evaluation can check eligible typed regions against real runtime values. A
portable benchmark runner now measures the HIR interpreter, profiled bytecode
VM, profile-off bytecode VM, and profile-off typed-region VM with common warmup
and rotating measurement order. It reports timing distributions, VM dispatch
counts, typed region activity, and typed instruction counts while requiring all
four runtime outputs to match.
Optimization candidates also carry concrete bytecode region contracts with PC
ranges, stack boundaries, read/write/call summaries, and conservative typed
execution eligibility. Eligible closed scalar `for` loops can now execute in a
transactional typed region path and automatically fall back to unchanged-state
bytecode execution when an entry value is not scalar numeric. A long-lived
adaptive VM session can accumulate profiles across invocations, install an
eligible typed module when a configurable loop threshold is reached, and use
profile-off typed execution on later invocations. Consecutive typed fallbacks
can now invalidate a module, clear stale observations, and resume profiling.
An invalidated region must then re-establish stable scalar evidence for every
external input before it may be installed again. A deterministic event history
records promotions, executions, fallbacks, invalidations, and retraining
rejections. Bytecode runs can now receive an initial workspace, and full
profiles record the entry kind and shape of each injected variable. Adaptive
sessions can optionally preserve the complete result workspace across
invocations, allowing changing runtime state to invalidate and later retrain a
specialization. A bytecode run may also select a same-file entry function by
name, bind positional runtime arguments, and return declared outputs separately
from the diagnostic variable snapshot. Full profiles record function parameter
and result kind/shape observations, and adaptive sessions can retrain from
changing argument values.
The frontend pipeline is also available through `CompiledModule`: it owns the
source set, semantic HIR, lowered bytecode, compile diagnostics, and a catalog of
invocable top-level function signatures. Embedders can compile once, validate
an entry, invoke the ordinary VM repeatedly, or create an adaptive session over
the same immutable artifacts. Class methods remain excluded from the top-level
entry catalog because module entries do not yet carry a class receiver or
constructor-dispatch contract.
`AdaptiveModuleRuntime` adds a longer-lived module execution layer above those
artifacts. It creates one adaptive session per named function, so invocation
heat, arguments, workspaces, promotions, typed executions, fallbacks,
invalidations, retraining, and event histories evolve independently. A failed
specialization in one entry no longer discards another entry's installed typed
regions.
Named function calls carry an explicit requested output count. Function frames
expose numeric `nargin` and `nargout` values, callers may request zero through
all declared outputs, and excessive output requests fail before execution.
v0.23 extends that contract with one-dimensional heterogeneous Cell values,
cell literals, scalar `C{n}` reads/writes, automatic Cell growth for brace
assignment, `varargin`, and `varargout`. Excess positional inputs are collected
in a Cell; excess requested outputs are read from `varargout{n}`. The same
contract is shared by the HIR interpreter's local calls, the bytecode VM, typed
execution, adaptive sessions, compiled modules, and the module runtime.

v0.24 makes a focused subset of the existing class frontend executable in the
bytecode VM: class objects own named scalar runtime fields, constructors
initialize objects through their regular function frame, direct property reads
and writes work, instance methods receive the object as their first argument,
and methods declared in `methods (Static)` can be called through the class.
v0.25 adds the first ownership policy: ordinary classes retain independent
field state when copied, while classes declared with `< handle` share property
storage across aliases. Calls used as standalone statements now request zero
outputs, so outputless methods execute without stack residue and observe
`nargout == 0`.
v0.26 resolves same-file class hierarchies recursively. Derived objects include
inherited properties, dispatch inherited instance and static methods, honor
subclass overrides, and inherit handle ownership transitively. Multiple
inheritance accepts shared declarations through a common ancestor, selects a
strictly more specific override, and diagnoses unresolved member conflicts,
missing superclasses, and cyclic hierarchies.
v0.27 adds executable superclass initialization. Dedicated syntax, HIR, and
bytecode nodes retain `obj@Superclass(args)` and
`method@Superclass(obj, args)` without confusing either form with indexing.
Derived constructors can call direct superclass constructors explicitly,
uncalled direct bases receive implicit zero-argument construction, and a
default derived constructor forwards its arguments to its first executable
base. A shared construction context preserves the most-derived object through
value and handle constructors and initializes common diamond ancestors once.
Qualified superclass methods bypass the current override while retaining
normal dynamic dispatch for calls made inside the selected base method.
v0.28 makes structured property declarations executable. Property dimensions,
class constraints, ordered validator functions, block attributes, and default
expressions survive syntax and HIR lowering. Explicit defaults execute once on
first class use within a bytecode run and are cached at the declaring property,
so handle-object
defaults are shared exactly as class-load defaults while ordinary values are
copied into each object. The VM creates supported implicit defaults, performs
class conversion before size adaptation and validators, and applies the same
pipeline to later direct property assignments. Inherited declarations retain
source order and common diamond properties keep one shared default cache.
v0.29 executes class-member encapsulation. Property `Access`, `GetAccess`, and
`SetAccess` enforce public, protected, private, and constructor-only immutable
writes; method visibility also covers static methods and constructors.
`Constant` properties use class-level cached values, while `Dependent`
properties dispatch through qualified `get.Property` and `set.Property`
methods without allocating fields. Validation runs before setters, recursive
same-property access is suppressed, handle setters may omit outputs, and
`AbortSet` skips runtime-equal handle assignments.
v0.30 makes abstract and sealed class contracts executable. Method prototypes
and abstract properties become inherited implementation requirements, and a
class with unresolved requirements is automatically abstract even without an
explicit class attribute. Concrete properties preserve abstract `GetAccess`
and `SetAccess` and inherit abstract validation; abstract methods are satisfied
by name without imposing argument-name, signature, or attribute equality.
Abstract classes cannot be instantiated, sealed classes cannot be subclassed,
and sealed methods cannot be redefined. Multiple inheritance can satisfy an
abstract requirement with a concrete member supplied by another base.
v0.31 adds selective class visibility and restricted inheritance. Meta-class
references in attributes are retained structurally, and `Access`, `GetAccess`,
and `SetAccess` accept a class or cell array of classes for methods,
constructors, and properties. Listed classes and their descendants receive
access, while the defining class always retains access. `AllowedSubclasses`
restricts direct subclass edges, with an empty or fully unresolved list acting
as a sealed class. Class-list method overrides require both authorization and
an exactly preserved access policy.
v0.32 gives every stored property a declaring-class-qualified identity. A
subclass may now declare a property with the same surface name as inherited
properties only when every inherited candidate has both `GetAccess` and
`SetAccess` set to private. Base methods, subclass methods, external access,
constructors, defaults, validation, constants, dependent accessors, value
copies, handle aliases, and compatible multiple inheritance all resolve the
correct independent property slot. More than one inherited non-private
candidate remains an ambiguity.
v0.33 applies declaring-class identity to private methods. Explicitly private
and empty-list private methods can coexist by surface name across subclasses
and unrelated bases. Calls made by a declaring class select its own private
candidate, while public/protected methods retain most-specific dynamic
dispatch. Method references carry the chosen declaring class through the
operand stack, static private methods use the same rule, diamond paths dedupe
one definition, and named access-list override restrictions remain intact.
v0.34 removes the single-file class boundary. A `SourceLoader` follows simple
class dependencies into sibling directories and repeatable CLI class paths,
then `CompiledModule` parses and merges the dependency closure while retaining
a source ID on every token, syntax node, HIR node, bytecode span, and
diagnostic. Cross-file inheritance, constructors, private property/method
identity, selective access lists, and dynamic dispatch therefore reuse the
same runtime metadata as same-file classes. Only referenced class files enter
the module, deterministic path precedence selects one definition, and
`--module-info` reports the complete source set.

There is also a small reference interpreter over HIR. It executes scalar double
expressions, one-dimensional numeric vectors, two-dimensional numeric matrices,
string literals,
assignments, local function calls with isolated stack frames, first-output
single-value calls, multiple-output destructuring such as `[a, b] = f(x)`,
ignored outputs with `~`, `for` ranges, `while` loops, `break`/`continue`,
`return`,
`if`/`elseif`/`else` blocks, `switch`/`case`/`otherwise`, `try`/`catch`
diagnostic recovery, short-circuit `&&`/`||`, string equality comparisons, MATLAB
constants such as `pi`, one-argument math builtins such as `sin` and `sqrt`,
string builtin `strcmp`, reductions such as `sum`, row/column shape queries
through `size` including `[rows, cols] = size(A)`, 1-based vector and matrix
indexing such as `A(2)` and `A(2, 1)`, colon and vector subscripts, `end`
expressions inside indexing, scalar-fill indexed assignment into existing
numeric arrays, array constructors `zeros`, `ones`, and `eye`, `linspace`
vector generation, transpose, basic numeric matrix multiplication, and
one-dimensional Cell literals plus scalar brace indexing/mutation. It is
intentionally not a full MATLAB runtime yet: classes, automatic growth during
indexed assignment, non-scalar right-hand-side indexed assignment shape
matching, function handles, other builtin multi-output conventions beyond the
scalar runtime subset, complex numbers, sparse arrays, and object dispatch
still report runtime diagnostics instead of guessing.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Try the parser

Check the current CLI version:

```powershell
build\mparser.exe --version
```

```powershell
build\Debug\mparser.exe samples\basic_class.m
```

With the Ninja generator used by the current build:

```powershell
build\mparser.exe samples\expression_demo.m
```

Use `--tokens` to inspect the lossless token stream:

```powershell
build\Debug\mparser.exe --tokens samples\basic_class.m
```

Use `--hir` to inspect semantic scopes, symbols, and bindings:

```powershell
build\mparser.exe --hir samples\expression_demo.m
```

Use `--bytecode` to inspect the current linear IR skeleton:

```powershell
build\mparser.exe --bytecode samples\expression_demo.m
```

Use `--run-bytecode` to execute the current bytecode VM subset:

```powershell
build\mparser.exe --run-bytecode samples\bytecode_vm_demo.m
```

The bytecode VM control-flow subset can be tried with:

```powershell
build\mparser.exe --run-bytecode samples\bytecode_control_demo.m
```

The bytecode VM local-function and multi-output subset can be tried with:

```powershell
build\mparser.exe --run-bytecode samples\bytecode_call_demo.m
```

The bytecode VM indexing and indexed-assignment subset can be tried with:

```powershell
build\mparser.exe --run-bytecode samples\bytecode_indexing_demo.m
```

The bytecode VM profiling and type/shape observation subset can be tried with:

```powershell
build\mparser.exe --profile-bytecode samples\bytecode_profile_demo.m
build\mparser.exe --profile-bytecode samples\bytecode_indexing_demo.m
```

The profile-driven bytecode optimization planner can be tried with:

```powershell
build\mparser.exe --plan-bytecode samples\bytecode_profile_demo.m
```

The typed optimization IR handoff can be tried with:

```powershell
build\mparser.exe --typed-ir-bytecode samples\bytecode_profile_demo.m
```

Typed IR guard evaluation can be tried with:

```powershell
build\mparser.exe --check-typed-ir-bytecode samples\bytecode_profile_demo.m
```

Inspect an eligible closed scalar loop region with:

```powershell
build\mparser.exe --plan-bytecode samples\typed_region_demo.m
build\mparser.exe --typed-ir-bytecode samples\typed_region_demo.m
build\mparser.exe --check-typed-ir-bytecode samples\typed_region_demo.m
```

Execute the scalar typed region and verify its output against the baseline VM:

```powershell
build\mparser.exe --run-typed-bytecode samples\typed_region_demo.m
build\mparser.exe --run-typed-bytecode `
  samples\typed_region_fallback_demo.m
```

Observe cross-invocation warmup and automatic typed-tier promotion with:

```powershell
build\mparser.exe --run-adaptive-bytecode --adaptive-runs=3 `
  --adaptive-hot-loop=10 samples\adaptive_tiering_demo.m
```

Observe fallback-driven invalidation and conservative retraining with:

```powershell
build\mparser.exe --run-adaptive-bytecode --adaptive-runs=4 `
  --adaptive-hot-loop=10 --adaptive-fallback-limit=2 `
  samples\typed_region_fallback_demo.m
```

Run the full promotion, invalidation, retraining, and re-promotion cycle with:

```powershell
build\mparser.exe --run-adaptive-bytecode --adaptive-runs=7 `
  --adaptive-hot-loop=10 --adaptive-fallback-limit=2 `
  --adaptive-persist-workspace --adaptive-workspace=phase=0 `
  samples\adaptive_workspace_demo.m
```

Select a named function, pass positional arguments, and return its declared
outputs with:

```powershell
build\mparser.exe --run-bytecode --entry-function=kernel `
  --argument=2 --argument=4 samples\function_entry_demo.m
```

Request a prefix of declared outputs and inspect `nargin`/`nargout` with:

```powershell
build\mparser.exe --run-bytecode `
  --entry-function=function_contract_demo --argument=2 --argument=4 `
  --outputs=2 samples\function_contract_demo.m
```

Run a variadic function using Cell-backed `varargin` and `varargout` with:

```powershell
build\mparser.exe --run-bytecode `
  --entry-function=cell_varargs_demo --argument=1 --argument=2 --argument=3 `
  --outputs=4 samples\cell_varargs_demo.m
```

Run the executable class subset with:

```powershell
build\mparser.exe --run-bytecode samples\class_runtime_demo.m
```

Observe handle aliases, value-copy isolation, and zero-output method calls with:

```powershell
build\mparser.exe --run-bytecode samples\handle_semantics_demo.m
```

Run inherited property layout, method override, static inheritance, and
transitive handle behavior with:

```powershell
build\mparser.exe --run-bytecode samples\class_inheritance_demo.m
```

Run explicit and implicit superclass constructors, default constructor
forwarding, and qualified superclass methods with:

```powershell
build\mparser.exe --run-bytecode samples\superclass_construction_demo.m
```

Run structured property defaults, type/size validation, ordered validators,
scalar expansion, and class-level handle default caching with:

```powershell
build\mparser.exe --run-bytecode samples\property_validation_demo.m
```

Run member access control, constants, immutable construction, and dependent
property get/set dispatch with:

```powershell
build\mparser.exe --run-bytecode samples\class_access_demo.m
```

Run abstract method/property implementation, inherited validation, dynamic
dispatch, and sealed members with:

```powershell
build\mparser.exe --run-bytecode samples\abstract_contract_demo.m
```

Run selective property/method/constructor access and restricted inheritance
with:

```powershell
build\mparser.exe --run-bytecode samples\class_access_policy_demo.m
```

Run declaring-class-local private property slots across value and handle
inheritance with:

```powershell
build\mparser.exe --run-bytecode samples\class_property_identity_demo.m
```

Run declaring-class-local private method dispatch while preserving public
virtual dispatch with:

```powershell
build\mparser.exe --run-bytecode samples\class_method_identity_demo.m
```

Load a derived class beside the script and its base class from an additional
class path with:

```powershell
build\mparser.exe --run-bytecode `
  --class-path=samples\cross_file_classes\lib `
  samples\cross_file_classes\run_demo.m
```

`--class-path=DIR` may be repeated. It participates in semantic, bytecode,
runtime, benchmark, and module modes; token and syntax-only inspection remain
limited to the requested entry file.

Compile once, inspect the reusable module catalog, and validate an entry with:

```powershell
build\mparser.exe --module-info --entry-function=accumulate_scale `
  --argument=2 --argument=4 samples\compiled_module_demo.m
```

The same function-entry contract participates in adaptive tiering:

```powershell
build\mparser.exe --run-adaptive-bytecode --adaptive-runs=3 `
  --adaptive-hot-loop=10 --entry-function=adaptive_function_demo `
  --argument=3 samples\adaptive_function_demo.m
```

Alternate between independently tiered functions in one compiled module with:

```powershell
build\mparser.exe --run-module-runtime --adaptive-hot-loop=10 `
  --adaptive-fallback-limit=2 --module-call=hot_a:2 `
  --module-call=hot_b:3 --module-call=hot_a:2 `
  --module-call=hot_b:3 --module-call=hot_a:2 `
  --module-call=hot_b:3 samples\adaptive_module_runtime_demo.m
```

Compare the HIR interpreter, baseline bytecode VM, and typed-region VM with:

```powershell
build\mparser.exe --benchmark-runtime --benchmark-warmup=3 `
  --benchmark-iterations=20 samples\runtime_benchmark_demo.m
```

Use the closed scalar loop sample to observe typed-region dispatch reduction:

```powershell
build\mparser.exe --benchmark-runtime --benchmark-warmup=3 `
  --benchmark-iterations=20 samples\typed_benchmark_demo.m
```

The bytecode VM switch and try/catch subset can be tried with:

```powershell
build\mparser.exe --run-bytecode samples\switch_demo.m
build\mparser.exe --run-bytecode samples\try_catch_demo.m
```

Use `--run` to execute the current interpreter subset:

```powershell
build\mparser.exe --run samples\run_demo.m
```

The vector subset can be tried with:

```powershell
build\mparser.exe --run samples\vector_demo.m
```

The matrix subset can be tried with:

```powershell
build\mparser.exe --run samples\matrix_demo.m
```

Local function calls can be tried with:

```powershell
build\mparser.exe --run samples\local_function_demo.m
```

Multiple-output local functions can be tried with:

```powershell
build\mparser.exe --run samples\multi_output_demo.m
```

Array constructors can be tried with:

```powershell
build\mparser.exe --run samples\constructor_demo.m
```

`linspace` vector generation can be tried with:

```powershell
build\mparser.exe --run samples\linspace_demo.m
```

Indexed assignment can be tried with:

```powershell
build\mparser.exe --run samples\indexed_assignment_demo.m
```

`end` in indexing expressions can be tried with:

```powershell
build\mparser.exe --run samples\end_indexing_demo.m
```

Colon and vector indexing can be tried with:

```powershell
build\mparser.exe --run samples\colon_indexing_demo.m
```

`while` loops can be tried with:

```powershell
build\mparser.exe --run samples\while_demo.m
```

`switch` blocks can be tried with:

```powershell
build\mparser.exe --run samples\switch_demo.m
```

String comparisons can be tried with:

```powershell
build\mparser.exe --run samples\string_compare_demo.m
```

Short-circuit logical conditions can be tried with:

```powershell
build\mparser.exe --run samples\short_circuit_demo.m
```

Loop control can be tried with:

```powershell
build\mparser.exe --run samples\loop_control_demo.m
```

Function returns can be tried with:

```powershell
build\mparser.exe --run samples\return_demo.m
```

`try`/`catch` diagnostic recovery can be tried with:

```powershell
build\mparser.exe --run samples\try_catch_demo.m
```
