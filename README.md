# MParser

Current milestone: v0.90.0. See [docs/v0.90.md](docs/v0.90.md) for the
current bytecode VM scope, supported subset, validation commands, and next
iteration plan,
[docs/public-contract-v1.json](docs/public-contract-v1.json) for the
machine-validated C/C++/protocol candidate freeze,
[docs/embedding-cpp-api.md](docs/embedding-cpp-api.md) for the public C++20
RAII embedding SDK,
[docs/c-abi-compatibility.md](docs/c-abi-compatibility.md) for C ABI evolution
and structure-versioning rules,
[docs/machine-result-protocol.md](docs/machine-result-protocol.md) for the
versioned CLI automation contract,
[docs/embedding-c-api.md](docs/embedding-c-api.md) for the narrow pure C
embedding contract, and
[docs/extending-builtins.md](docs/extending-builtins.md)
for the source-level C++ builtin extension contract. Previous boundaries are
kept in [docs/v0.89.md](docs/v0.89.md),
[docs/v0.88.md](docs/v0.88.md),
[docs/v0.87.md](docs/v0.87.md),
[docs/v0.86.md](docs/v0.86.md),
[docs/v0.85.md](docs/v0.85.md),
[docs/v0.84.md](docs/v0.84.md),
[docs/v0.83.md](docs/v0.83.md),
[docs/v0.82.md](docs/v0.82.md),
[docs/v0.81.md](docs/v0.81.md),
[docs/v0.80.md](docs/v0.80.md),
[docs/v0.79.md](docs/v0.79.md),
[docs/v0.78.md](docs/v0.78.md),
[docs/v0.77.md](docs/v0.77.md),
[docs/v0.76.md](docs/v0.76.md),
[docs/v0.75.md](docs/v0.75.md),
[docs/v0.74.md](docs/v0.74.md),
[docs/v0.73.md](docs/v0.73.md),
[docs/v0.72.md](docs/v0.72.md),
[docs/v0.71.md](docs/v0.71.md),
[docs/v0.70.md](docs/v0.70.md),
[docs/v0.69.md](docs/v0.69.md),
[docs/v0.68.md](docs/v0.68.md),
[docs/v0.67.md](docs/v0.67.md),
[docs/v0.66.md](docs/v0.66.md),
[docs/v0.65.md](docs/v0.65.md),
[docs/v0.64.md](docs/v0.64.md),
[docs/v0.63.md](docs/v0.63.md),
[docs/v0.62.md](docs/v0.62.md),
[docs/v0.61.md](docs/v0.61.md),
[docs/v0.60.md](docs/v0.60.md),
[docs/v0.59.md](docs/v0.59.md),
[docs/v0.58.md](docs/v0.58.md),
[docs/v0.57.md](docs/v0.57.md), [docs/v0.56.md](docs/v0.56.md),
[docs/v0.55.md](docs/v0.55.md),
[docs/v0.54.md](docs/v0.54.md),
[docs/v0.53.md](docs/v0.53.md),
[docs/v0.52.md](docs/v0.52.md),
[docs/v0.51.md](docs/v0.51.md),
[docs/v0.50.md](docs/v0.50.md),
[docs/v0.49.md](docs/v0.49.md),
[docs/v0.48.md](docs/v0.48.md),
[docs/v0.47.md](docs/v0.47.md),
[docs/v0.46.md](docs/v0.46.md),
[docs/v0.45.md](docs/v0.45.md),
[docs/v0.44.md](docs/v0.44.md),
[docs/v0.43.md](docs/v0.43.md),
[docs/v0.42.md](docs/v0.42.md),
[docs/v0.41.md](docs/v0.41.md),
[docs/v0.40.md](docs/v0.40.md),
[docs/v0.39.md](docs/v0.39.md),
[docs/v0.38.md](docs/v0.38.md),
[docs/v0.37.md](docs/v0.37.md),
[docs/v0.36.md](docs/v0.36.md),
[docs/v0.35.md](docs/v0.35.md),
[docs/v0.34.md](docs/v0.34.md),
[docs/v0.33.md](docs/v0.33.md),
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

The release definition and staged path to v1.0 are maintained in
[docs/roadmap-v1.0.md](docs/roadmap-v1.0.md). v1.0 targets a stable,
documented, embeddable MATLAB-like subset runtime. It does not claim complete
MATLAB compatibility, but it does require the language and engine foundations
needed to extend ordinary functions after v1.0 without repeatedly redesigning
the frontend or VM. The current source-linked support claims and prioritized
gaps are recorded in the machine-validated
[compatibility matrix](docs/compatibility-matrix.json).

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

Cross-platform compatibility is a release constraint rather than a later
porting task. CI builds and tests the SLJIT backend on Windows x64 and Linux
x64, cross-compiles it for Linux AArch64, and exercises focused native and
portable AArch64 runtime paths under QEMU. Platform-specific machine-code and
calling-convention details remain behind SLJIT's public API; the portable typed
kernel stays available when native JIT support is disabled.

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
member access across the loaded source graph, qualified namespace
functions/classes/static methods, structured explicit and wildcard imports,
caller-scoped ordinary and `private` function files, and a small lazy registry
of common MATLAB builtins. Class methods implemented in separate
`@ClassName/*.m` files are merged into their declaring class before semantic
lowering. Dynamic calls, indexing, and unknown member access remain delayed
bindings for later name and type resolution.

The next layer is an initial bytecode path. It linearizes HIR into stack-style
instructions for module/class/function boundaries, assignments, control
headers, literals, names, member access, neutral call/index operations, and
expression operators. v0.54 executes the core numeric/string subset,
`if`/`for`/`while` control flow, same-file local function calls, multi-output
call assignment, MATLAB-style N-dimensional numeric indexing/mutation with
`end`, `:`, vector subscripts, folded trailing dimensions, shape-checked
non-scalar indexed assignment, scalar expansion, automatic numeric-array
growth, and direct `A(...)=[]` vector/slice deletion,
first-class logical scalars and arrays, logical-mask reads and writes,
`logical`/`double` conversion, `class`/`isa`/`islogical`, session commands
`clear`, `clc`, `tic`, and `toc`,
dimension-aware `sum`, `prod`, `mean`, `min`, `max`, `any`, and `all`, plus
one-to-three-output `find`,
shape-preserving `cumsum`, `cumprod`, `cummin`, and `cummax`, and numeric
first- or higher-order `diff`,
`switch/case/otherwise`, and `try/catch` recovery with `MException` catch
values, identifiers, messages, throw-site stacks, and `error`/`throw`/
`rethrow`. It also records
bytecode VM execution profiles for functions, loops, instructions,
call/index sites, and assignment sites, including runtime value kind, numeric
class, and shape observations. Profile collection is now an explicit VM
policy: analysis and
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
counts, typed region activity, source bytecode work, and predecoded kernel
instruction counts while requiring all four runtime outputs to match.
Optimization candidates also carry concrete bytecode region contracts with PC
ranges, stack boundaries, read/write/call summaries, and conservative typed
execution eligibility. Eligible closed scalar `for` loops, including
well-nested loop trees, can now execute in a transactional predecoded
register-kernel path and automatically fall back to unchanged-state bytecode
execution when an entry value is not scalar numeric. Two- and three-term colon
ranges share one runtime planner across the interpreter, bytecode VM, and typed
kernel.
Statically bound
scalar calls to `abs`, `acos`, `asin`, `atan`, `cos`, `exp`, `log`, `sin`,
`sqrt`, and `tan` execute directly in that typed region. Structured forward
`if`/`elseif`/`else` branches now execute in both the portable and native
typed kernels; general builtins, user functions, dynamic indexing, mutation,
backward jumps, loop control, and exception regions retain their conservative
fallback boundary. A long-lived
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
The frontend pipeline is also available through `CompiledModule`: shared
immutable storage owns the source set, semantic HIR, lowered bytecode, compile
diagnostics, and a catalog of invocable top-level function signatures.
Embedders can compile once, validate an entry, invoke the ordinary VM
repeatedly, create an adaptive session, or create an isolated
`CompiledModuleSession` over the same artifacts. Sessions and module-bound
function handles keep those artifacts alive even after the original
`CompiledModule` object is destroyed. Class methods remain excluded from the
top-level entry catalog because module entries do not yet carry a class
receiver or constructor-dispatch contract.
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
v0.35 extends that source graph to MATLAB namespace folders. A reference such
as `pkg.inner.ClassName` resolves from a class-path root through
`+pkg/+inner/ClassName.m`; each source unit records its namespace and each class
uses the full name as its semantic and runtime identity. Constructors keep the
short name required inside `classdef` source while compiling to the qualified
class key. Qualified construction, static access, cross-namespace inheritance,
superclass calls, private member identity, and dynamic dispatch share the same
class registry. Classes with the same short name can coexist in different
namespaces, while duplicate full names are diagnosed. Multiple namespace roots
merge naturally and the first matching class member in class-path order wins.
v0.36 adds functions to the same namespace graph. A public function file such
as `+pkg/scale.m` receives the canonical identity `pkg.scale`, while later
functions in that file remain source-local under identities such as
`pkg.scale>helper`. Structured `import` statements are collected for the whole
containing script or function scope before name binding. Explicit imports can
name namespace functions, classes, or static methods; wildcard imports are
resolved lazily when an unqualified name is referenced. Explicit imports win
over source-local functions, while source-local functions win over wildcard
imports. Variables and parameters retain highest precedence. The loader follows
qualified calls and imported names through nested `+namespace` folders with the
same deterministic class-path ordering used for classes.
v0.37 extends the graph to ordinary function files and caller-scoped `private`
folders. For each caller, lookup checks its eligible `private` folder, the
entry file directory, and repeated `--path` roots in order. Current-directory
functions therefore beat search-path functions, while `private` functions are
visible only to files in their parent directory. Dependency-loaded ordinary
and private functions receive stable internal identities, so two libraries can
use different private helpers with the same filename without colliding. The
source graph records each caller's alias-to-identity binding and `--module-info`
prints those edges. Explicit and wildcard imports retain their v0.36 semantic
precedence. `--class-path` remains a compatibility alias for `--path`.
v0.38 adds MATLAB class folders. A class definition can live at
`@Counter/Counter.m` or inside a namespace such as
`+pkg/@Counter/Counter.m`; the loader deterministically collects sibling `.m`
method files and records their declaring class. A separate implementation
replaces its matching method prototype while preserving block attributes such
as `Static` and `Access`. A method without a prototype is added with MATLAB's
default public, non-static attributes. Instance, static, and private methods,
method-local helper functions, inheritance, and virtual overrides reuse the
existing class HIR and bytecode dispatch. Signature mismatches, duplicate
inline/separate implementations, malformed method files, and missing owners
are compile diagnostics. Constructors remain in the `classdef` file as MATLAB
requires.
v0.39 completes the `.m` class-folder helper model for functions under
`@ClassName/private`. Each helper remains an ordinary hidden function, but its
source unit and HIR function carry the canonical lexical class identity. This
lets the bytecode VM read and write private class members without turning the
helper into a method or inserting an object receiver. Private helpers are
visible only to methods of the defining class, are not inherited by subclasses,
and can call one another. For a same-named helper and method, function notation
selects the private helper while dot notation selects the method. Source-local
functions and wildcard imports retain their higher MATLAB precedence. Every VM
function call now installs its own lexical access context, so an ordinary path
function called by a method cannot inherit the caller's private privilege.
v0.40 makes MATLAB enumeration classes executable. Enumeration declarations
retain same-line comma separation, `...` continuations, block attributes, and
structured constructor arguments through syntax, HIR, and dedicated bytecode
initializer regions. `Class.Member`, explicit member imports, package-qualified
members, default and string conversion constructors, methods, equality, and
`switch` all share lazy one-time member construction. Value-enumeration
properties become immutable after construction, while enumerations derived
from `handle` expose one shared mutable singleton. Enumeration classes are
implicitly sealed, conflicting member names are diagnosed, private
constructors remain usable by member initialization, and recursive member
initialization fails deterministically. The VM also implements `isenum`,
`enumeration`, enum-aware `class`, `isa`, `char`, and `string`; `Hidden` members
remain directly addressable but are filtered from enumeration queries. Until
object arrays exist, `enumeration` returns visible values and names as Cells,
which support brace indexing and general `length`, `numel`, `size`, and
`isempty` queries.
v0.41 makes function handles and class events executable in the bytecode VM.
Anonymous handles retain their parameter list, bytecode body range, lexical
class privilege, and a value snapshot of referenced free variables. Named
handles can target builtins, local/path/package functions, static methods, and
bound object methods, and may be called through the same neutral call/index
operation as an ordinary function. Event declarations now have distinct
symbols, bindings, HIR nodes, inherited runtime metadata, and executable
`ListenAccess`,
`NotifyAccess`, and `Hidden` policies. `addlistener` creates a listener coupled
to its source, while `listener` creates an independently retained listener;
`notify` invokes callbacks synchronously with source and `event.EventData`
arguments. Listener `Enabled`, `Recursive`, `Callback`, `delete`, and `isvalid`
behavior is implemented, including recursion suppression and source-coupled
lifetime. Custom `event.EventData` subclasses receive `Source` and `EventName`,
and named callback handles participate in package/path dependency loading.

v0.42 replaces the runtime's two-field shape assumption with canonical
N-dimensional metadata shared by the HIR interpreter, bytecode VM, profiler,
optimization planner, typed IR, guard evaluator, benchmark comparator, and CLI.
Numeric arrays and Cells support N-dimensional `zeros`, `ones`, and `cell`
construction; `size`, `size(A,dim)`, dimension-vector `size`, multi-output
`size`, `ndims`, `numel`, `length`, and `isempty`; MATLAB column-major linear
indexing; folded trailing dimensions; and N-dimensional `end` evaluation.
Numeric subscript reads and scalar-fill writes accept arbitrary dimensions,
while scalar Cell brace reads and writes use the same mapping. Elementwise
numeric operations retain the complete shape. Class property size declarations
can now contain more than two dimensions, create typed N-dimensional defaults,
and reshape numeric or Cell assignments while preserving MATLAB linear order.
Runtime profiles and JIT guards carry the complete dimension vector, so a
`2x3x2` observation cannot be confused with a two-dimensional `2x3` value.

v0.43 builds a shared numeric indexed-assignment engine on that shape model.
Both baseline runtimes now accept shape-checked non-scalar right-hand values,
apply repeated indices and matrix-shaped subscript arrays in MATLAB
column-major order, grow scalar, vector, matrix, and N-dimensional targets,
and zero-fill new elements while preserving existing coordinates. Basic
numeric elementwise operators also implement MATLAB dimension-wise implicit
expansion, including singleton expansion across significant third and later
dimensions.

v0.44 adds shared N-dimensional array transformations for numeric arrays and
Cells. `reshape` preserves MATLAB column-major logical order and supports one
inferred `[]` dimension; `permute` and `ipermute` validate complete dimension
orders; `squeeze` removes significant singleton dimensions; and `repmat`
performs checked multidimensional tiling. `cat`, `horzcat`, and `vertcat`
perform coordinate-preserving concatenation with strict shape validation.
The HIR interpreter, bytecode VM, and class-property reshape path now use the
same transformation contract.

v0.45 makes logical data a preserved runtime numeric class instead of an
untyped double `0`/`1` convention. Comparisons, logical operators, predicates,
and `true`/`false` constructors produce logical values; ordinary arithmetic
produces doubles. Linear and multi-subscript logical masks work in both
baseline runtimes, including MATLAB-compatible column-major selection,
vector-orientation rules, shorter masks, ignored out-of-range `false` entries,
and diagnostics for out-of-range `true` entries. Logical-mask assignment is
transactional, never grows the target, and coerces right-hand values to the
target numeric class before mutation. Runtime transformations preserve the
logical class, while profiler observations, optimization guards, typed IR, and
adaptive retraining distinguish `logical` from `double`. The scalar typed
loop tier also preserves logical intermediates produced inside a specialized
double loop.

v0.46 adds two runtime contracts that close visible MATLAB gaps. Direct empty
syntax on an indexed target now deletes vector elements, complete matrix rows
or columns, and complete N-dimensional slices in both baseline runtimes.
Numeric and logical deletion subscripts are resolved transactionally, repeated
indices are removed once, the target numeric class is preserved, and direct
colon syntax is retained in bytecode metadata. An empty value stored in a
variable or produced by `zeros(0,0)` remains an ordinary assignment value and
does not trigger deletion. Literal `[]` now has its MATLAB 0-by-0 shape.

The v0.46 scalar typed loop tier also introduced direct execution for ten
statically bound pure unary math builtins. At that milestone,
`samples/timing_loop_demo.m` entered the typed path with no fallback and moved
from roughly 14.1 seconds in the baseline VM to roughly 1.8 seconds in the
typed stack executor. `clear`, `clc`, `tic`, and `toc` are executable in
command form. Typed baseline comparison excludes only values assigned from
nondeterministic `toc` expressions while still checking every deterministic
variable.

v0.47 replaces the earlier scalar/global reduction shortcuts with a shared
dimension-aware implementation used by the HIR interpreter and bytecode VM.
`sum`, `prod`, `mean`, `min`, `max`, `any`, and `all` reduce the first
non-singleton dimension by default and accept a scalar dimension, dimension
vector, or `"all"`. Numeric reductions support missing-value policy flags;
ordinary reductions support `"default"`, `"double"`, and `"native"` output
selection. `min` and `max` add value/index outputs, optional linear indices,
and elementwise two-input forms with N-dimensional implicit expansion.

`find` now supports an optional result limit, `"first"` or `"last"`
selection, and one to three outputs. It preserves row-vector orientation for a
single linear-index output and folds trailing N-dimensional coordinates into
the second subscript for multi-output calls. Reductions and `find` traverse
the canonical shape in MATLAB column-major logical order, while their result
payloads are mapped back into the runtime's existing storage representation.
Historical demos that intentionally need a global total now spell that intent
as `sum(A, "all")`.

v0.48 builds shape-preserving scans and shape-reducing differences on the same
logical-order substrate. `cumsum`, `cumprod`, `cummin`, and `cummax` select the
first non-singleton dimension by default, accept one explicit positive scalar
dimension, preserve arbitrary N-dimensional shapes, and support `"forward"`
or `"reverse"` traversal. Include/omit missing-value policies are shared by
both baseline runtimes; sums and products include NaN by default, while
cumulative extrema omit NaN by default. Logical cumulative sums and products
return double values, while cumulative extrema preserve the logical class.

Numeric `diff` supports default first differences, a positive order, an
explicit dimension, `[]` as the default order placeholder, empty inputs,
logical-to-double results, and dimensions beyond the current rank. Its
two-input form keeps every order on the initially selected dimension, matching
the current MATLAB behavior instead of carrying residual orders into a later
dimension. The shared `runtime_numeric` result builder maps scan and difference
outputs from MATLAB column-major logical order into the runtime payload once.

v0.50 extends the v0.49 predecoded register kernel across complete structured
scalar loop nests. The region analyzer validates matching `ForBegin` and
`ForNext` boundaries, and the kernel lowers them to structured loop operations
instead of entering the inner typed region once per outer iteration. A shared
colon-range runtime gives the interpreter, bytecode VM, and typed executor the
same positive-step, negative-step, and empty-range behavior. Leaf loops use a
direct scalar span path while preserving transactional commit and fallback.

The kernel replaces per-iteration bytecode decoding, string operation
dispatch, dynamic stacks, map lookups, and temporary `RuntimeValue`
construction. Loads are bound to contiguous scalar slots, expression
temporaries use indexed registers, and the final expression producer writes
directly to its destination slot. The VM also skips synthetic loop-profile
reconstruction when profiling is disabled.

On the development machine (Intel Core i5-1135G7), five consecutive final
Release runs of the original nested `1000 x 1000` loop in
`samples/timing_loop_demo.m` ranged from `0.0806` to `0.0949` seconds, with a
`0.0828` second median. The whole nest enters one outer typed region, covers
1,000 outer and 1,000,000 nested iterations, represents 15,004,000 source
instructions, and dispatches 4,001,000 kernel instructions with no fallback.
A single comparable v0.49 run was about `0.0786` seconds, which is not enough
evidence to claim either a speedup or a regression at this noise level. The
user-reported MATLAB reference of about `0.02` seconds was measured on a
different Intel Core i7-12700 machine, so it is a useful target scale rather
than a valid cross-machine speed ratio. This path is still a portable C++
register interpreter; the structured nest is primarily the required boundary
for native-code JIT compilation.

v0.51 adds that first native-code boundary. Eligible structured scalar loop
kernels can now be compiled through SLJIT into executable machine code and
cached by structural kernel identity. Native execution preserves the same
entry guards, nested colon-range semantics, transactional workspace commit,
output-equivalence check, counters, and bytecode fallback contract as the
portable register kernel. `--typed-backend=auto` is the default and selects
native code when available; `portable` provides a same-binary comparison path,
while `native` requires a successful native entry and otherwise resumes at the
unchanged bytecode boundary. The backend is optional at build time so targets
that cannot provide executable memory can retain the portable runtime.

In one eight-invocation Release session on the same Intel Core i5-1135G7,
`samples/adaptive_native_jit_benchmark.m` produced a `0.0330` second median
across six native cache hits versus a `0.1108` second median across seven
portable typed runs, a same-process `3.36x` improvement. Both paths produced
`finalValue = 834.32145639679`. The separate user-reported MATLAB result of
about `0.02` seconds came from an Intel Core i7-12700 and remains a target
scale, not a direct cross-machine ratio.

v0.52 extends the same typed loop boundary across structured
`if`/`elseif`/`else` statements. Region analysis accepts only closed forward
jumps that remain at the same loop depth, and a control-flow dataflow pass
computes variables that are definitely assigned on every incoming path.
Portable kernels execute explicit `Jump` and `JumpIfFalse` operations; SLJIT
emits closed machine-code labels and branches for the identical kernel. Native
branch kernels also collect path-accurate source and kernel instruction counts,
so backend comparisons remain meaningful when different arms execute. The
sample `samples/branched_typed_loop_demo.m` produces `summary = 23520` through
both backends and exercises native code caching.

v0.53 turns that process-wide native code cache into a bounded, thread-safe
LRU. The default limits are 256 generated kernels and 16 MiB of SLJIT code;
either limit may be changed before execution, and zero disables retention
without disabling native compilation. Oversized kernels execute uncached,
runtime limit reductions evict immediately, and explicit clear/reset/statistics
APIs make cache lifecycle observable. Compilations remain outside the cache
mutex, concurrent same-key compilations converge on one resident entry, and an
active execution keeps its generated code alive even if another thread clears
or evicts it.

v0.54 broadens the shared typed-loop contract from scalar workspaces to
preallocated dense double vectors. Closed `for` regions can now read and write
scalar elements through MATLAB 1-based linear indexing, including write-then-
read data flow and pure scalar math around each access. The portable kernel and
SLJIT backend execute the same `LoadArrayElement` and `StoreArrayElement` IR.
Both work on private array copies and commit only after the entire region
succeeds; a non-integer or out-of-bounds index, an unsupported shape, or a
request for growth resumes the unchanged bytecode frame. General matrices,
logical/vector/colon subscripts, multiple subscripts, deletion, and growth stay
on the full VM path. The native helper calls use SLJIT's public mixed integer/
floating-point call ABI and are exercised in Linux AArch64 QEMU CI.

v0.59 makes ordinary, repeating, output, and name-value `arguments` groups
explicit in syntax, HIR, and `FunctionSignature`, and executes complete
repeating input groups in both runtimes. v0.60 completes the name-value input
calling convention. Modern `Name=value` expressions and legacy
`"Name", value` pairs share exact and unambiguous-prefix matching, defaults,
validation, last-value-wins duplicate handling, and structure-backed field
access. Name-value inputs can follow fixed or repeating positional inputs in
local/path/package functions, constructors and methods, named bytecode entry
functions, compiled modules, and adaptive module sessions. `nargin` counts
only positional values, omitted fields without defaults remain absent, and
compiled-module preflight validates actual argument values rather than only
their count.

v0.61 executes fixed and repeating output `arguments` blocks after the
function body completes. Assigned fixed outputs pass through the same class,
shape, and validator pipeline as inputs, and converted values are written back
before they cross the call boundary. A named repeating output or validated
`varargout` remains a Cell inside the function and expands into the requested
output tail at the call site. Fixed outputs can precede one repeating output;
the repeating declaration must be last. The HIR interpreter, bytecode VM,
constructors and methods, named entries, compiled modules, and adaptive module
runtime share the same output initialization, validation, collection, and
slot-naming helpers.

v0.62 executes MATLAB class-derived name-value declarations such as
`options.?PlotOptions`. The source root and qualified class survive syntax and
HIR lowering, then a shared argument-contract catalog expands inherited public
settable properties for the HIR interpreter, bytecode VM, compiled-module
preflight, and adaptive/module call paths. Constant, private, protected, and
selectively accessible properties are excluded. Constructor-only immutable
properties are included only in their declaring class's constructor. An
explicit declaration such as `options.Height` overrides the class-derived
validation regardless of declaration order, while class property defaults are
never copied into the options Struct. Exact names, partial matching,
last-value-wins duplicates, validation, and `nargin` retain the v0.60 calling
convention. The Windows CLI also disables operating-system crash dialogs at
startup while preserving nonzero exits and terminal diagnostics.

v0.63 makes the loaded class catalog introspectable from executable MATLAB
code. The bytecode VM now evaluates `?ClassName`, `metaclass(obj)`,
`matlab.metadata.Class.fromName`, and the legacy `meta.class.fromName` alias.
Read-only `matlab.metadata.Class`, `Property`, `Method`, `Event`,
`EnumerationMember`, and `Namespace` values expose class attributes, direct
superclasses, inherited member lists, defining classes, access policies,
method signatures, property defaults, and namespace membership. Metadata
arrays use the normal N-dimensional shape and column-major indexing contract.
Scalar class metadata supports equality plus subclass and superclass
comparisons. `properties`, `methods`, `events`, `isprop`, and `ismethod`
apply MATLAB visibility rules to loaded classes, and method-form
`metafunction("Class/Method")` returns a method descriptor. Current
`matlab.metadata.*` names and the renamed `meta.*` aliases share one canonical
runtime identity.

v0.64 extends that reflection boundary from classes to callable signatures.
`metafunction` resolves loaded functions, namespace functions, same-file local
functions, explicit constructors, slash-form methods, dotted static methods,
and instance methods selected with `Arguments=` or `ArgumentTypes=`. Function
and method descriptors expose typed `matlab.metadata.CallSignature` values.
Their input and output arrays retain argument identifiers, group names,
required/repeating/name-value classification, validation classes, dimensions,
validators, referenced arguments, defaults, and class-derived name-value
sources. Namespace `FunctionList` now returns executable function metadata,
and source-unit names survive semantic analysis as platform-neutral
`FullPath` values.

v0.65 adds MATLAB per-instance dynamic properties to the bytecode class
runtime. Classes can derive from `dynamicprops`, create properties with direct
or method-form `addprop`, and discover declared or dynamic descriptors through
`findprop`. Dynamic values follow handle aliases while remaining isolated
between instances. Writable `matlab.metadata.DynamicProperty` attributes
control access, visibility, persistence hints, equality short-circuiting, and
function-handle `GetMethod`/`SetMethod` callbacks. `properties`, `isprop`,
`methods`, `ismethod`, `metaclass`, `class`, and `isa` understand the new
runtime type and its legacy `meta.DynamicProperty` alias. Deleting a descriptor
removes the property from its owner and invalidates all descriptor aliases;
plain dynamic values and descriptors can also be rebound from an initial VM
workspace without losing their IDs or instance storage.

v0.66 makes `GetObservable` and `SetObservable` executable through MATLAB-style
property listeners. Declared and dynamic properties emit `PreGet`, `PostGet`,
`PreSet`, and `PostSet` around their accessors. The runtime supports
`addlistener`, uncoupled `listener`, and `event.proplistener`, including their
method forms, source-coupled retention, deletion, enabled and recursive state,
and `AbortSet` suppression. Callbacks receive the matching
`matlab.metadata.Property` or `DynamicProperty` descriptor and an
`event.PropertyEvent` carrying `AffectedObject`, `Source`, and `EventName`.
Inherited property descriptors and handle aliases retain source identity, and
deleting a dynamic property invalidates listeners bound to that descriptor.

v0.67 establishes `--run` as the stable production execution interface. It
executes the full bytecode VM semantics once, installs statically eligible
typed loop regions, prefers the native SLJIT backend when available, and keeps
guarded portable or bytecode fallback inside the runtime. `--jit` selects
`auto`, `off`, `portable`, or `native` policy without changing script
semantics. Diagnostic execution modes remain available, while the deliberately
smaller reference interpreter now has the explicit `--run-hir` name.

v0.67.1 makes the native JIT benchmark directly executable through the
production interface. The separate adaptive benchmark retains the original
eight-run persistent-workspace cache experiment.

v0.68 implements explicit MATLAB handle-object destruction as a complete
runtime lifecycle. Every handle class inherits `ObjectBeingDestroyed`,
`delete(obj)` and `obj.delete()` invalidate all aliases before callbacks,
invoke destruction listeners, execute every valid class destructor from the
most-derived class through its superclasses, and then invalidate coupled
listeners and dynamic-property descriptors. Repeated deletion is idempotent;
`isvalid`, `events`, `methods`, and class metadata expose the corresponding
state and inherited members.

v0.69 establishes ordered scalar structures as a shared runtime contract.
`struct(field, value, ...)`, static and dynamic `s.field`/`s.(name)` reads and
writes, implicit creation by member assignment, `fieldnames`, collection-form
`isfield`, `rmfield`, and `isstruct` now agree between the HIR interpreter and
bytecode VM. Field definition order is retained independently from map lookup,
and dynamic member names also work for bytecode class objects. Structure arrays
remain an explicit unsupported boundary rather than being approximated as a
scalar value.

v0.70 turns the v1.0 language and engine audit into an executable release
contract. A machine-readable compatibility matrix links supported and partial
claims to registered CTest evidence and source files, while an automated
validator rejects stale tests, missing sources, malformed tier states, and
unclassified gaps. The runtime now exposes `MException` values through both
baseline engines: `catch` binds an object with `identifier`, `message`,
`stack`, and `cause`; `error`, `throw`, and `rethrow` share construction,
formatting, validation, and diagnostic conversion rules. Optimized Release
tests explicitly keep assertion-based smoke contracts enabled.

v0.71 replaces scalar-only structure storage with a canonical structure-array
representation shared by both baseline engines. Nonscalar Cell values in
`struct(field, value, ...)` determine the result shape, scalar values broadcast,
and empty Cell values create empty typed structures. Parenthesis indexing uses
the common MATLAB-visible column-major index contract; whole-element indexed
assignment supports replacement, scalar expansion, common growth, and linear
vector deletion. Nonscalar field access now produces an internal
comma-separated list that expands in function calls, output lists, numeric and
Cell literals, while ordinary single-value assignment reports an arity error.
Typed and native tiers safely fall back to the VM for these operations.

v0.72 introduces a shared root-and-path lvalue transaction across the HIR
interpreter and bytecode VM. Mixed member, `()`, and `{}` paths evaluate each
dynamic field or subscript once, mutate a detached leaf, and copy the result
back through every parent before committing the root variable. The contract
covers numeric arrays, structure arrays, Cells, dynamic fields, indexed
growth, deletion, schema extension, and VM value/handle object properties.
Failed paths leave value roots unchanged; typed and native regions reject the
new path opcodes and execute them in the bytecode VM.

v0.73 establishes a shared exception and diagnostic contract. Both baseline
engines expose full source-graph stack arrays, recursive causes, explicit
`throw`/`rethrow`/`throwAsCaller` policies, basic/extended reports,
severity-tagged warnings, `lastwarn`, and catchable `assert`. Warnings remain
observable without terminating execution, while correction objects stay an
explicit unsupported boundary.

v0.74 replaces VM-local function-handle identifiers with first-class callable
descriptors and a stable per-`CompiledModule` context. Anonymous closure
snapshots, named and builtin handles, dynamic call-versus-index dispatch,
multi-output `feval`, `str2func`, `func2str`, and `functions` now agree across
the HIR interpreter and bytecode VM. Literal text targets participate in path
and package dependency loading, private functions retain lexical-only access,
and ordinary path functions consistently shadow builtins. Source-backed
handles can be passed back to repeated invocations of their owning module;
builtin handles may cross modules. Serialization, computed-string lazy source
loading, anonymous text parsing, and HIR method handles remain explicit later
boundaries.

v0.75 separates character arrays from string arrays in the engine-facing
`RuntimeValue` contract. Both use UTF-16 code units but retain distinct shape,
empty-value, indexing, assignment, concatenation, conversion, comparison, and
display behavior. Shared text operations now serve the HIR interpreter and
bytecode VM, including string brace access, indexed growth/deletion, implicit
expansion, array transforms, `char`/`string`/`strings`/`cellstr`/`strlength`,
type predicates, missing-string masks, and UTF-8 CLI boundaries. Validated
`char` and `string` class properties use the same shape-preserving path. Typed
and native tiers conservatively fall back for text values.

v0.76 adds a dedicated object-array payload and shared object operation layer.
Value and handle objects now preserve MATLAB-visible column-major indexing over
the runtime's row-major storage, including logical/vector/N-dimensional
selection, transactional assignment and growth, deletion, concatenation,
transpose, reshape, permutation, squeezing, and replication. Value-object
nested writes copy back through the lvalue transaction, while handle elements
retain aliases and independent default-filled identities. Classes deriving
from `matlab.mixin.Heterogeneous` form arrays whose class is the most specific
shared superclass; ordinary unlike classes remain a diagnostic. Nonscalar
property reads produce comma-separated lists, methods receive the whole array,
and direct nonscalar property assignment is rejected as MATLAB requires.
Explicit `delete` and `isvalid` operate elementwise on handle arrays. Typed and
native tiers treat objects as unsupported optimized inputs and safely execute
the legal code in the bytecode VM.

v0.77 freezes the supported reflection graph behind one
`RuntimeMetadataTypeDescriptor` table. Canonical `matlab.metadata.*` names,
legacy `meta.*` aliases, direct superclasses, class flags, and public member
lists now drive `class`, `isa`, `metaclass`, `properties`, `methods`, and VM
member dispatch from the same source. Metadata arrays project properties as
logical-order comma-separated lists, while `findobj` filters metadata or
handle arrays by one or more property-name/value pairs. Property descriptors
expose `PropertyValidation` (including the `matlab.metadata.Validation` and
`meta.Validation` aliases), class and dimension restrictions, callable
validation-function handles, `isValidValue`, and `validateValue`. Dynamic
properties now default `NonCopyable` to true and expose empty validation
metadata. Module-bound validation handles retain the existing
`CompiledModule` identity guard. Reflection remains a documented VM subset;
typed and native execution safely fall back.

v0.78 adds explicit MATLAB-like `global` and `persistent` declarations to the
syntax, semantic HIR, bytecode, reference interpreter, and production VM.
`RuntimeSessionState` is the shared state boundary: globals are keyed by name,
persistent values by compiled callable identity, canonical function, and
variable name, and first declaration produces an empty 0-by-0 double matrix.
Every supported assignment path, including indexed, brace, member, nested
lvalue, and `for` target writes, uses the same binding route.
`CompiledModuleSession` provides isolated
compile-once/invoke-many state, snapshots, targeted clearing, and reset while
retaining the module artifacts it executes. Callers may inject one state when
intentional global sharing is required without merging same-named persistent
functions from different compiled modules. Shared-state accesses remain VM operations;
typed and native regions reject them during region selection and preserve
correctness through ordinary fallback.

v0.79 freezes the engine-facing runtime foundation used by both baseline
engines and the optimization tiers. `RuntimeValue` now has shared factories,
one `RuntimeWorkspace` type, explicit immediate/value/shared-handle/callable/
transient ownership categories, recursive shape/payload validation, and
cycle-safe validation for handle graphs and function closures. HIR and
bytecode execution use the same `RuntimeCallFrame` kinds and `nargin`/
`nargout` initialization. Typed-region selection, portable execution, native
SLJIT, adaptive invalidation, and CLI detail output carry a
`RuntimeFallbackKind` in addition to explanatory text. The source contract is
stable enough for the v0.80 builtin registry; the public C++ ABI and future C
ABI remain intentionally pre-freeze until the v0.90 embedding gate.

v0.80 makes `BuiltinRegistry` and `BuiltinDescriptor` the shared source of
truth for semantic builtin resolution, function handles, HIR/bytecode
execution, typed-region selection, and portable/native lowering. Descriptors
declare arity, value/shape constraints, purity, determinism, thread safety,
side effects, context permissions, diagnostics, and typed/JIT eligibility.
Representative math, reduction, scan, array-transform, multi-output, and
warning-state families now execute through shared `BuiltinCall`/
`BuiltinResult` handlers; the displaced interpreter and VM dispatch branches
are gone. Embedders may compile a module with a frozen custom registry, and the
module retains it across all tiers. Registration, host exceptions, context,
output ownership, HIR/VM parity, handles, shadowing, portable typed execution,
and native SLJIT execution are covered by one reusable conformance suite.
This stabilizes how the function library grows; it does not claim broad
long-tail MATLAB or toolbox function coverage.

v0.81 adds an engine-neutral C++ embedding execution contract.
`ModuleInvocationRequest` carries the entry, arguments, output count, initial
workspace, backend preference, and profiling choice.
`ModuleInvocationResult` separates compilation failure, request rejection,
runtime failure, and success while returning owned phase-aware diagnostics,
outputs, workspace values, and a compact execution summary. `CompiledModule`
builds and retains registry-aware static Typed IR once; `execute()` defaults to
guarded automatic JIT, while explicit bytecode, portable, and native requests
all preserve VM fallback. `CompiledModuleSession::execute()` uses the same
contract with persistent state. The old VM-specific `invoke()` API remains
available. This is a source-level API candidate, not yet the frozen C++ ABI,
narrow C ABI, resource contract, or versioned machine protocol.

v0.82 adds cooperative cancellation and bounded production execution to that
embedding contract. Each request may set instruction, steady-clock wall-time,
call-depth, per-value recursive array-payload, and retained-diagnostic limits,
or carry a copyable cross-thread cancellation token. Resource stops are
uncatchable terminal runtime failures with stable identifiers and an explicit
stop reason in `ModuleExecutionSummary`; persistent sessions remain reusable
but do not roll back effects completed before a stop. Instruction, deadline,
and cancellation checks suppress typed/native regions until those kernels
have safe polling points, while call-depth, array, and diagnostic controls
retain guarded optimized execution. Context builtins can declare
`ExecutionControl` permission and cooperate before long host operations.
`std::bad_alloc` still propagates to the embedder because no allocation-safe
reporting reserve has been frozen.

v0.83 adds the first narrow pure C ABI over the same engine-neutral contract.
`mparser_c_api` builds a shared library named `mparser_c`, while
`include/mparser/c_api.h` exposes only opaque retained handles, fixed-width
constants, borrowed UTF-8/UTF-16 views, and versioned C structures. A C host
can compile one UTF-8 source, invoke statelessly or through a serialized
persistent session, pass column-major numeric/logical/text/Cell/Struct values,
return object and function-handle values, inspect diagnostics and execution
summaries, enforce resource limits, and cancel from another thread.
Module-defined objects and closures retain their producing module and are
rejected before cross-module use; independent builtin handles can cross
modules. No C++ exception crosses this boundary, including allocation failure.
ABI candidate 1 remains pre-freeze until v0.90.

v0.84 extends the C boundary to complete source-graph ingestion.
`mparser_module_compile_sources` copies an ordered set of versioned in-memory
source descriptors, while `mparser_module_load_file_utf8` loads an entry file
and ordered search paths through the same `SourceLoader` used by the CLI.
Filesystem loading therefore preserves ordinary/private/path functions,
`+package` namespaces, `@Class` folders, separated methods, imports, and
dependency discovery. Paths and retained source names are UTF-8 on every
platform. Hosts can enumerate module sources, and load failures return a
stable status plus an inspectable module diagnostic. The original one-source
entry remains compatible.

v0.85 packages that C boundary as a relocatable SDK. A production build may
set standard CMake `BUILD_TESTING=OFF`, install the pure C header, shared
library, CLI, examples, notices, and package metadata, then expose
`MParser::c_api` and `MParser::cli` through
`find_package(MParser CONFIG)`. The regression installs to one prefix, moves
the entire tree, and configures a separate C11 project that has no source-tree
access; it verifies ABI/version queries, a two-input/two-output invocation,
and the imported CLI. Windows x64, Linux x64, and both native-JIT and portable
Linux AArch64 paths validate the installed package. This is still C ABI
candidate 1: public C++ packaging, machine protocol, final compatibility
policy, macOS evidence, and release archives remain v0.90 work.

v0.86 adds a versioned machine result protocol to production `--run`.
`--result-format=json-v1` routes the normal engine-neutral invocation result
to one UTF-8 JSON document containing status, outputs, workspace, staged
diagnostics, and execution summary. Every current runtime value has a stable
shape-aware encoding; array payloads use MATLAB column-major order, function
handles expose descriptors without captures, and objects remain explicit
opaque values. Success and every failure stage use documented process exit
codes while keeping stderr empty. A complete golden fixture and parsed CLI
regressions lock the protocol independently from human display output.

v0.87 makes C ABI candidate major 1 safely evolvable. Caller-sized
initializers clear and record the complete host storage for extensible request,
summary, and source-load records; old initializer symbols now have a frozen v1
write range, so a newer library cannot overwrite a v0.86 host object. Input
consumers accept the known prefix and ignore future tails, while output getters
respect caller capacity. Fixed-stride `mparser_source_unit` is explicitly
sealed. A frozen v0.86 header consumer, future-tail execution/load/summary
tests, an ABI compatibility sample, relocated installed consumer, and focused
AArch64 QEMU paths enforce the policy.

v0.88 adds an installed header-only C++20 facade over that narrow C ABI.
`mparser::sdk` supplies copyable RAII modules, sessions, results, values, and
cancellation tokens together with source compilation/loading, invocation,
diagnostic, resource-limit, and execution-summary types. The facade exports no
internal compiler or VM layout and keeps the shared-library boundary in C.
`MParser::cpp_api` is exercised from the source tree and from a separate C++20
project after the installed prefix is moved; focused native and portable Linux
AArch64 jobs cross-build and run the same consumer under QEMU.

v0.89 hardens that embedding boundary under concurrency and across release
platforms. Pure stateless calls remain concurrent, while module-bound mutable
objects and every session operation use one fixed module-then-session lock
order. New stress tests cover shared handle mutation, retain/release,
cancellation, isolated resource budgets, and 256 dynamic load/unload cycles.
The C library now has ABI implementation version `1.1.0`, SOVERSION/install
name 1, hidden internal symbols, and an exact 90-symbol public manifest.
macOS x64 and ARM64 jobs build and test native SLJIT, relocate a production
install, and consume both public C and C++ SDKs. The v1.0 transport candidate
uses copy-in arrays plus readonly output spans; a stable external native
callback table is explicitly Post-v1.0.

v0.90 freezes the combined embedding and automation boundary as a
machine-checkable v1 candidate. The public contract manifest locks C ABI
`1.1`, the 90-symbol export set, 64-bit public record layouts, C++ source API
`1.0`, machine result protocol `1.0`, their immutable snapshots, and the
review policy for later changes. The C++ facade remains header-only and has no
C++ binary ABI. Machine mode now publishes a JSON Schema, exact single-line
framing, structured rejection of human-only options, exact unsigned 64-bit
counters, and an allocation-free exit-4 emergency document.

The same milestone adds deterministic C-boundary allocation/internal-failure
injection, pre/post-execution commit tests, concurrent native-cache churn, and
a Linux Clang ASan/UBSan CI lane. The v1.0 native cache is explicitly bounded
and process-local; a disk cache remains deferred until atomic persistence,
corruption recovery, bounds, and complete invalidation keys exist.
Anonymous closures now retain only semantic free variables in both baseline
engines. Source-coupled listeners are kept alive by the active VM registry
without a source-to-listener ownership back-edge, and cross-platform lifetime
regressions keep Linux LeakSanitizer enabled for the full suite.
Platform/architecture-named ZIP or TGZ release archives carry Apache-2.0,
`Copyright 2026 Wang Xin`, checksums, installed public contracts, and the
relocatable SDK. Their smoke test packages a fixed payload twice, unpacks it,
builds independent C11 and multi-translation-unit C++20 consumers, and runs
the installed CLI protocol. Checksums prove integrity, not publisher identity;
signing or provenance attestation remains a v1.0 release operation.

Install and consume the C SDK:

```powershell
cmake -S . -B build-sdk -DBUILD_TESTING=OFF
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix C:\mparser-sdk
```

```cmake
find_package(MParser 0.90.0 EXACT CONFIG REQUIRED COMPONENTS CPP CLI)
target_link_libraries(host PRIVATE MParser::cpp_api)
```

Create a checksummed release archive:

```powershell
cmake -S . -B build-package `
  -DMPARSER_ENABLE_RELEASE_PACKAGING=ON
cmake --build build-package --config Release `
  --target mparser_release_package
```

Build and run the resource-control embedding example:

```powershell
cmake --build build --target mparser_embedding_resource_control_demo
build\mparser_embedding_resource_control_demo.exe
```

Build and run the pure C embedding example:

```powershell
cmake --build build --target mparser_c_embedding_demo
build\mparser_c_embedding_demo.exe
```

Build and run the C++20 embedding example:

```powershell
cmake --build build --target mparser_cpp_embedding_demo
build\mparser_cpp_embedding_demo.exe
```

Build and run the C ABI compatibility example:

```powershell
cmake --build build --target mparser_c_abi_compat_demo
build\mparser_c_abi_compat_demo.exe
```

Build and run the pure C source-graph example:

```powershell
cmake --build build --target mparser_c_source_graph_demo
build\mparser_c_source_graph_demo.exe `
  samples\class_folders\app\run_demo.m `
  samples\class_folders\lib
```

There is also a small reference interpreter over HIR. It executes scalar double
expressions, N-dimensional numeric arrays,
distinct UTF-16 character and string arrays,
assignments, local, namespace, ordinary path, and private function calls with
isolated stack frames,
named, anonymous, and builtin function handles with closure snapshots,
dynamic handle invocation, `feval`, `str2func`, `func2str`, and `functions`,
first-output
single-value calls, multiple-output destructuring such as `[a, b] = f(x)`,
ignored outputs with `~`, `for` ranges, `while` loops, `break`/`continue`,
`return`,
`if`/`elseif`/`else` blocks, `switch`/`case`/`otherwise`, `try`/`catch`
diagnostic recovery, short-circuit `&&`/`||`, text comparisons and string
implicit expansion/append, MATLAB
constants such as `pi`, one-argument math builtins such as `sin` and `sqrt`,
text builtins and conversions including `strcmp`, `strcmpi`, `char`, `string`,
`strings`, `cellstr`, and `strlength`, dimension-aware reductions through `sum`, `prod`,
`mean`, `min`, `max`, `any`, and `all`, multi-output `find`, cumulative
`cumsum`, `cumprod`, `cummin`, and `cummax`, numeric `diff`, shape queries
through `size` and `ndims` including multi-output and dimension-vector forms,
1-based
N-dimensional numeric indexing such as `A(2)` and `A(2, 1, 3)`, colon and
vector subscripts, `end`
expressions inside indexing, non-scalar and scalar-expanded indexed assignment
with automatic numeric-array growth, direct indexed `[]` deletion,
logical-mask indexing and assignment,
logical/double conversion and class queries, array constructors `zeros`,
`ones`, and
two-dimensional `eye`, `linspace` vector generation, numeric/text/Cell `reshape`,
`permute`, `ipermute`, `squeeze`, `repmat`, `cat`, `horzcat`, and `vertcat`,
transpose, basic numeric matrix multiplication, and
N-dimensional Cells with parenthesis indexing/mutation and brace
indexing/mutation, and ordered scalar or array structures with static/dynamic
member access, parenthesis indexing, whole-element assignment, field queries,
comma-separated field results, and transactional nested path copy-back. It is
intentionally not a full MATLAB runtime yet: classes,
method handles, other builtin multi-output conventions beyond the
implemented `size`/`min`/`max`/`find` subset, complex numbers, sparse arrays,
class reflection, and object dispatch
still report runtime diagnostics instead of guessing.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

For MSVC with Ninja, configure through the checked-in wrapper from a VS
Developer Shell. It switches the configure process to UTF-8 so CMake records
the localized `/showIncludes` prefix without corruption:

```powershell
.\cmake\configure-windows-msvc.cmd
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

When replacing an older Ninja tree that was configured under a different code
page, use `--fresh` with CMake 3.24 or newer, or choose a new build directory.
After a public runtime-header layout change, use `--clean-first` once before
trusting test results.

Native scalar-loop JIT support is enabled by default. The pinned SLJIT source
is vendored under `third_party/sljit`, so a default configure does not require
network access. An existing source checkout can still be supplied explicitly,
or the native backend can be omitted:

```powershell
cmake -S . -B build-local-sljit `
  -DMPARSER_SLJIT_SOURCE_DIR=C:\path\to\sljit
cmake -S . -B build-nojit -DMPARSER_ENABLE_NATIVE_JIT=OFF
```

Disabling the native backend does not remove the interpreter, bytecode VM, or
portable typed kernel. It only removes machine-code generation and the SLJIT
dependency from that build.

On a Debian or Ubuntu x64 host with the GNU AArch64 cross compiler installed,
the checked-in toolchain builds the same sources for Linux AArch64:

```bash
cmake -S . -B build-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake \
  -DMPARSER_ENABLE_NATIVE_JIT=ON
cmake --build build-arm64 --parallel
```

The CI workflow additionally runs focused AArch64 native-JIT and portable
tests under QEMU. Physical ARM hardware remains the authority for performance
measurements.

Run the structured-branch sample with either backend:

```powershell
build\mparser.exe --run-typed-bytecode --typed-backend=portable `
  samples\branched_typed_loop_demo.m
build\mparser.exe --run-typed-bytecode --typed-backend=native `
  samples\branched_typed_loop_demo.m
```

Run the typed linear-array sample with either backend:

```powershell
build\mparser.exe --run-typed-bytecode --typed-backend=portable `
  samples\typed_linear_array_demo.m
build\mparser.exe --run-typed-bytecode --typed-backend=native `
  samples\typed_linear_array_demo.m
```

Both executions produce `first = 2.8414709848079`,
`last = 40.9129452507276`, and `checksum = 420.99822188442`, then report that
their outputs match the baseline bytecode VM.

Inspect or tune the native cache with `--native-cache-entries=N`,
`--native-cache-bytes=N`, and `--native-cache-stats`. This module-runtime
sample promotes three distinct loop kernels into a two-entry cache and
therefore reports one LRU eviction:

```powershell
build\mparser.exe --run-module-runtime --adaptive-hot-loop=5 `
  --typed-backend=native --native-cache-entries=2 `
  --native-cache-bytes=1048576 --native-cache-stats `
  --module-call=cache_add:2 --module-call=cache_add:2 `
  --module-call=cache_add:2 --module-call=cache_multiply:2 `
  --module-call=cache_multiply:2 --module-call=cache_multiply:2 `
  --module-call=cache_absolute:2 --module-call=cache_absolute:2 `
  --module-call=cache_absolute:2 samples\native_cache_demo.m
```

## Run scripts

`--run` is the stable one-shot interface for applications, terminals, and
editor Run buttons. It compiles the source graph, executes the full bytecode
semantics once, and automatically uses eligible typed/JIT loop regions:

```powershell
build\mparser.exe --run samples\production_run_demo.m
```

On Linux, use the same options with the platform executable:

```bash
./build/mparser --run samples/production_run_demo.m
```

The default `--jit=auto` policy prefers native SLJIT code when that backend is
available. Runtime guards preserve transactional fallback to the portable
typed kernel or bytecode VM. The policy can be selected explicitly without
changing script semantics:

```powershell
build\mparser.exe --run --jit=off samples\production_run_demo.m
build\mparser.exe --run --jit=portable samples\production_run_demo.m
build\mparser.exe --run --jit=native samples\production_run_demo.m
```

For automation, request protocol v1 instead of parsing the human variable
display:

```powershell
build\mparser.exe --run --result-format=json-v1 `
  samples\machine_protocol_demo.m
```

The command writes exactly one JSON document to stdout for success,
compilation failure, request rejection, runtime failure, and serialization
emergencies while stdout remains writable. Arrays are column-major at this
boundary. Exit codes are respectively `0`, `1`, `2`, `3`, and `4`; an output
transport failure can make exit-4 stdout incomplete. See
[docs/machine-result-protocol.md](docs/machine-result-protocol.md) for the
complete schema and compatibility rules.

The public and diagnostic execution modes have separate contracts:

| Mode | Purpose |
| --- | --- |
| `--run` | Production one-shot execution with automatic typed/JIT regions |
| `--run-bytecode` | Baseline bytecode VM execution with JIT disabled |
| `--run-hir` | Deliberately smaller reference HIR interpreter |
| `--run-jit` | Static-JIT diagnostics and region reporting |
| `--run-typed-bytecode` | Profile, rerun, and compare two executions |
| `--run-adaptive-bytecode` | Repeated adaptive-session diagnostics |

Unlike `--run-typed-bytecode`, production `--run` does not execute a profiling
baseline first, so script side effects occur once. Use `--help` for the full
CLI and place `--` before a source path that begins with a hyphen.

With the VS Code CMake Tools extension, a cross-platform task can resolve the
selected `mparser` launch target and run the active MATLAB file:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "MParser: Run current file",
      "type": "process",
      "command": "${command:cmake.launchTargetPath}",
      "args": ["--run", "${file}"],
      "options": { "cwd": "${workspaceFolder}" },
      "problemMatcher": []
    }
  ]
}
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

Run non-scalar assignment, numeric-array growth, repeated indices, and
N-dimensional implicit expansion through either baseline runtime with:

```powershell
build\mparser.exe --run-bytecode samples\array_assignment_demo.m
build\mparser.exe --run-hir samples\array_assignment_demo.m
```

Run first-class logical arrays, mask selection, mask assignment, conversion,
and transformed logical shapes through both baseline runtimes with:

```powershell
build\mparser.exe --run-bytecode samples\logical_index_demo.m
build\mparser.exe --run-hir samples\logical_index_demo.m
```

Run direct empty deletion for vectors, rows, columns, logical selections, and
N-dimensional slices through both baseline runtimes with:

```powershell
build\mparser.exe --run-bytecode samples\array_deletion_demo.m
build\mparser.exe --run-hir samples\array_deletion_demo.m
```

Run dimension-aware reductions, extrema indices, NaN policies, and `find`
through both baseline runtimes with:

```powershell
build\mparser.exe --run-bytecode samples\reduction_find_demo.m
build\mparser.exe --run-hir samples\reduction_find_demo.m
```

Run N-dimensional cumulative operations, reverse traversal, NaN policies, and
first- or higher-order differences through both baseline runtimes with:

```powershell
build\mparser.exe --run-bytecode samples\scan_diff_demo.m
build\mparser.exe --run-hir samples\scan_diff_demo.m
```

The bytecode VM profiling and type/shape observation subset can be tried with:

```powershell
build\mparser.exe --profile-bytecode samples\bytecode_profile_demo.m
build\mparser.exe --profile-bytecode samples\bytecode_indexing_demo.m
build\mparser.exe --profile-bytecode samples\nd_arrays_demo.m
```

The profile-driven bytecode optimization planner can be tried with:

```powershell
build\mparser.exe --plan-bytecode samples\bytecode_profile_demo.m
build\mparser.exe --plan-bytecode samples\nd_arrays_demo.m
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
build\mparser.exe --run-typed-bytecode --typed-backend=native `
  samples\nested_typed_loop_demo.m
build\mparser.exe --run-typed-bytecode --typed-backend=portable `
  samples\nested_typed_loop_demo.m
build\mparser.exe --run-typed-bytecode `
  samples\typed_region_fallback_demo.m
```

Run the million-iteration math loop with `tic`/`toc` and pure-math typed
specialization with:

```powershell
build-release\mparser.exe --run-bytecode samples\timing_loop_demo.m
build-release\mparser.exe --run-typed-bytecode --typed-backend=native `
  samples\timing_loop_demo.m
build-release\mparser.exe --run-typed-bytecode --typed-backend=portable `
  samples\timing_loop_demo.m
build-release\mparser.exe --run-typed-bytecode samples\timing_flat_loop_demo.m

build-release\mparser.exe --run --jit=native `
  samples\native_jit_benchmark.m

build-release\mparser.exe --run-adaptive-bytecode `
  --typed-backend=native --adaptive-runs=8 --adaptive-hot-loop=1 `
  --adaptive-persist-workspace --adaptive-workspace=runCount=0 `
  samples\adaptive_native_jit_benchmark.m
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

Run modern and legacy name-value calls through both baseline runtimes with:

```powershell
build\mparser.exe --run-hir samples\name_value_arguments_demo.m
build\mparser.exe --run-bytecode samples\name_value_arguments_demo.m
```

Run inherited class-property name-value contracts with:

```powershell
build\mparser.exe --run-hir samples\class_property_arguments_demo.m
build\mparser.exe --run-bytecode samples\class_property_arguments_demo.m
```

Named entries accept `--argument=Name=value`, and module-runtime calls accept
the same form after a colon:

```powershell
build\mparser.exe --run-bytecode --entry-function=configure `
  --argument=2 --argument=Scale=4 --outputs=2 `
  samples\name_value_arguments_demo.m
build\mparser.exe --run-module-runtime --module-call=configure:2:Scale=4 `
  samples\name_value_arguments_demo.m
```

Run the compile-once C++ embedding request/result and persistent-session sample
with:

```powershell
cmake --build build --target mparser_embedding_execution_demo
build\mparser_embedding_execution_demo.exe
```

Run fixed output conversion and named repeating output expansion through both
baseline runtimes with:

```powershell
build\mparser.exe --run-hir samples\output_arguments_demo.m
build\mparser.exe --run-bytecode samples\output_arguments_demo.m
build\mparser.exe --run-bytecode --entry-function=repeatedOutputs `
  --argument=4 --outputs=2 samples\output_arguments_demo.m
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

`--path=DIR` may be repeated and is the general source search-path option.
`--class-path=DIR` remains a compatibility alias. Both participate in semantic,
bytecode, runtime, benchmark, and module modes; token and syntax-only inspection
remain limited to the requested entry file.

Run qualified classes from multiple and nested namespace folders with:

```powershell
build\mparser.exe --run-bytecode `
  --class-path=samples\package_classes `
  samples\package_classes\app\run_demo.m
```

Inspect the resolved namespace attached to each source with the same command
using `--module-info` instead of `--run-bytecode`.

Run a package enumeration with an imported member, constructor arguments,
methods, `switch`, string conversion, and visible-member discovery with:

```powershell
build\mparser.exe --run-bytecode `
  --class-path=samples\enumeration_classes\lib `
  samples\enumeration_classes\app\run_demo.m
```

The demo reports `summary = 56`, prints members as
`<palette.Status.Ready>`, and filters the `Hidden` member from the two Cells
returned by `enumeration`.

Run class metadata lookup, inherited member discovery, visibility filtering,
metadata comparison, and method introspection with:

```powershell
build\mparser.exe --run-bytecode samples\class_metadata_demo.m
```

The demo reports `summary = 18` and displays metadata values with their
canonical class and identity, such as
`<matlab.metadata.Class MetadataChild>`.

Run function, constructor, method, and call-signature reflection with:

```powershell
build\mparser.exe --run-bytecode samples\function_metadata_demo.m
```

The demo reports `summary = 17`. It exercises typed function and method
signatures, name-value defaults, validators, dimensions, dotted static lookup,
ordinary-function selection, and instance-method selection from argument
types.

Run observable declared and dynamic property events with:

```powershell
build\mparser.exe --run-bytecode samples\property_events_demo.m
```

The demo reports `summary = 738`. It verifies the exact get/set event order,
method-form listener creation, metadata and event-data identity, dynamic
property delivery and invalidation, and the `event.proplistener` constructor.

Run the complete explicit handle-destruction lifecycle with:

```powershell
build\mparser.exe --run samples\handle_lifecycle_demo.m
```

The demo reports `summary = 161`. It verifies invalidation before
`ObjectBeingDestroyed`, derived-to-base destructor order, alias identity,
idempotent free- and method-form deletion, listener lifetime, dynamic-property
cleanup, and inherited handle event metadata.

Run ordered scalar structure construction, dynamic fields, field queries, and
copy-preserving field removal through the production interface with:

```powershell
build\mparser.exe --run samples\struct_runtime_demo.m
```

The demo reports `summary = 2193`. The same file is also a checked HIR and
baseline-bytecode sample.

Run identifier-preserving exception creation, catch inspection, explicit
throw, and stack-preserving rethrow through the production interface with:

```powershell
build\mparser.exe --run samples\exception_runtime_demo.m
```

The demo reports `summary = 111111111` and is also checked through the HIR and
baseline-bytecode interfaces.

Run full source-linked stack propagation, cause chaining, reports,
`warning`/`lastwarn`, and catchable `assert` through the production interface:

```powershell
build\mparser.exe --run samples\exception_diagnostics_demo.m
```

The demo emits `MParserDemo:Notice` as a nonfatal warning, exits successfully,
and reports `summary = 111111`. Diagnostics now include severity, identifier,
an N-by-1 `file`/`name`/`line` stack, and recursive causes through the public
runtime result. `throw`, `rethrow`, and `throwAsCaller` respectively replace,
preserve, and caller-trim the stack. Correction objects remain an explicit
unsupported boundary rather than a partially emulated contract.

Run closures, dynamic call/index dispatch, text-created handles, multi-output
`feval`, builtin shadowing, and handle metadata through the production
interface with:

```powershell
build\mparser.exe --run samples\dynamic_call_demo.m
```

The demo reports `summary = 106` and is checked through `--run-hir`,
`--run-bytecode`, and `--run`. `str2func` and text `feval` can address loaded
public functions; a computed function name does not implicitly load a new
source file.

Run the distinct UTF-16 character/string runtime, indexing and mutation,
implicit expansion, conversions, array transforms, and validated text-property
shape behavior through the production interface:

```powershell
build\mparser.exe --run samples\text_runtime_demo.m
```

The demo reports `summary = 36` and is also checked through `--run-hir` and
`--run-bytecode`. Single quotes create character arrays, double quotes create
string scalars or arrays, and legal text code outside optimized regions falls
back to the bytecode VM.

Run value, handle, and heterogeneous object arrays through the production
interface:

```powershell
build\mparser.exe --run samples\object_array_demo.m
```

The demo reports `summary = 14` and is also checked with `--run-bytecode`. It
covers object growth, value-copy isolation, property lists, whole-array method
dispatch, N-dimensional transforms, deletion, handle identity and validity,
and most-specific heterogeneous array classes.

Run a namespace class whose instance, static, private, and default-public
methods live in separate `@Counter` files with:

```powershell
build\mparser.exe --run-bytecode `
  --path=samples\class_folders\lib `
  samples\class_folders\app\run_demo.m
```

Use `--module-info` with the same paths to inspect each method source's
`method-of=folderpkg.Counter` ownership.

Run class-private helper reads, writes, static calls, and function-versus-dot
dispatch with:

```powershell
build\mparser.exe --run-bytecode `
  --path=samples\class_private_functions\lib `
  samples\class_private_functions\app\run_demo.m
```

`--module-info` reports `private-of=securepkg.Vault`, while `--hir` reports
`lexical-class=securepkg.Vault` on the hidden helper functions.

Run qualified namespace functions, explicit and wildcard imports, same-file
private helpers, an imported class, and an imported static method with:

```powershell
build\mparser.exe --run-bytecode `
  --class-path=samples\namespace_functions `
  samples\namespace_functions\app\run_demo.m
```

Run the pure function subset through the reference HIR interpreter with:

```powershell
build\mparser.exe --run-hir `
  --class-path=samples\namespace_functions `
  samples\namespace_functions\app\run_function_demo.m
```

The complete demo uses `--run-bytecode` because the reference interpreter does
not yet implement class objects.

Run ordinary current-directory and search-path functions together with two
caller-local `private/adjust.m` implementations using:

```powershell
build\mparser.exe --run-bytecode `
  --path=samples\function_paths\first_library `
  --path=samples\function_paths\second_library `
  --path=samples\function_paths\global_library `
  samples\function_paths\app\run_demo.m
```

The same pure-function graph executes through the reference HIR interpreter by
replacing `--run-bytecode` with `--run-hir`. Use `--module-info` to inspect the
caller-specific binding edges and internal function identities.

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

Use `--run-hir` to execute the current reference-interpreter subset:

```powershell
build\mparser.exe --run-hir samples\run_demo.m
```

The vector subset can be tried with:

```powershell
build\mparser.exe --run-hir samples\vector_demo.m
```

The matrix subset can be tried with:

```powershell
build\mparser.exe --run-hir samples\matrix_demo.m
```

Local function calls can be tried with:

```powershell
build\mparser.exe --run-hir samples\local_function_demo.m
```

Multiple-output local functions can be tried with:

```powershell
build\mparser.exe --run-hir samples\multi_output_demo.m
```

Array constructors can be tried with:

```powershell
build\mparser.exe --run-hir samples\constructor_demo.m
```

`linspace` vector generation can be tried with:

```powershell
build\mparser.exe --run-hir samples\linspace_demo.m
```

Indexed assignment can be tried with:

```powershell
build\mparser.exe --run-hir samples\indexed_assignment_demo.m
```

`end` in indexing expressions can be tried with:

```powershell
build\mparser.exe --run-hir samples\end_indexing_demo.m
```

Colon and vector indexing can be tried with:

```powershell
build\mparser.exe --run-hir samples\colon_indexing_demo.m
```

`while` loops can be tried with:

```powershell
build\mparser.exe --run-hir samples\while_demo.m
```

`switch` blocks can be tried with:

```powershell
build\mparser.exe --run-hir samples\switch_demo.m
```

String comparisons can be tried with:

```powershell
build\mparser.exe --run-hir samples\string_compare_demo.m
```

Short-circuit logical conditions can be tried with:

```powershell
build\mparser.exe --run-hir samples\short_circuit_demo.m
```

Loop control can be tried with:

```powershell
build\mparser.exe --run-hir samples\loop_control_demo.m
```

Function returns can be tried with:

```powershell
build\mparser.exe --run-hir samples\return_demo.m
```

`try`/`catch` diagnostic recovery can be tried with:

```powershell
build\mparser.exe --run-hir samples\try_catch_demo.m
```

## License

MParser is licensed under the
[Apache License, Version 2.0](LICENSE).
Copyright 2026 Wang Xin.

The project attribution notice is in [NOTICE](NOTICE). Vendored components
remain under their respective licenses and are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
