# Architecture Notes

The main design constraint is to keep MATLAB syntax, dynamic name resolution,
and execution optimization as separate layers. MATLAB has many expressions that
cannot be fully classified during parsing. For example, `A(x)` can be indexing,
function invocation, or constructor invocation depending on runtime bindings.
Likewise, `A.B` can be package access, static method access, property access, or
an enumeration member.

## Frontend stages

```text
Source text
  -> Lossless token stream
  -> Syntax tree
  -> Semantic HIR
  -> Reference interpreter
  -> Bytecode IR
  -> Optimizing/JIT IR
```

The lexer keeps whitespace, comments, and spans so the parser and later tooling
can emit source-aware diagnostics. The initial syntax tree intentionally stores
raw text for declarations and statements that are not yet fully lowered. This
lets the parser accept more MATLAB surface syntax while the semantic layer
grows incrementally.

Ordinary statements are parsed into a first expression tree when the syntax is
unambiguous enough to preserve. `AssignmentStatement` keeps left and right
children; bracketed multi-output assignments are represented as `OutputList`.
Expression nodes use neutral syntax for MATLAB runtime ambiguities. In
particular, `A(...)` becomes `CallOrIndexExpr` because only semantic analysis
can decide whether `A` is a variable, function, constructor, or overloaded
object.

Control-flow headers use the same expression machinery. A `for` range header is
represented through a `ControlHeader` child that can contain an assignment-like
node, while `if`, `elseif`, `while`, `switch`, and `case` headers can contain
expression nodes. This keeps loop analysis and future JIT profiling from having
to reparse raw header text.

## Semantic HIR

The first semantic layer lowers syntax into HIR and builds scopes and symbols.
It records module, class, and function scopes; predeclares local functions,
classes, and class members; declares methods, properties, function parameters,
outputs, and local assignment targets; and resolves local name references where
the answer is syntactically knowable.

Name resolution is deliberately layered. Local scopes win first, which lets
MATLAB shadowing rules work for names like `sin = 1`. If no local binding is
known, the analyzer can materialize a small set of common MATLAB builtins on
demand, so expressions like `sin(x)` and `pi` have stable HIR bindings without
filling every symbol table dump with unused builtins. The current type hints are
minimal but useful: constructors and first method parameters can carry class
type names, allowing member access such as `obj.Value` to bind to a known class
property when the receiver type is clear.

After every same-file class has been lowered, a hierarchy-aware fixup revisits
unresolved member and call nodes. It walks superclass graphs, merges repeated
paths to the same declaration, and prefers a declaration from a strictly more
specific class. This makes inherited bindings independent of source order while
leaving genuinely ambiguous multiple-inheritance members unresolved for the
runtime class registry to diagnose with full executable metadata.

Superclass-qualified calls have their own syntax and HIR node. The semantic
pass binds an object-output form to a direct superclass constructor and a
same-named method form to the exact declaration on the named superclass. A
post-lowering validation pass rejects indirect or repeated constructor calls,
conditional construction, construction after an object reference, object
references in superclass arguments, and malformed qualified method calls.

Dynamic MATLAB constructs intentionally remain delayed. `CallOrIndex`,
`BraceIndex`, and `MemberAccess` HIR nodes preserve the surface operation and
carry unresolved bindings until a later name-resolution pass has path, package,
class, overload, and runtime workspace context.

## Reference interpreter

The current executable runtime is a small HIR interpreter. Its purpose is to
define and test execution semantics before the bytecode VM and JIT harden. It
supports scalar double values, one-dimensional numeric vectors,
two-dimensional numeric matrices, string literals, variable assignment, local function calls
with isolated stack frames, first-output single-value calls, multiple-output
destructuring for local functions, ignored outputs with `~`, numeric ranges,
`for` loops over ranges or vectors, `while` loops, `break`/`continue`,
`return`, `if`/`elseif`/`else`, `switch`/`case`/`otherwise`, `try`/`catch`
diagnostic recovery, short-circuit `&&`/`||`, arithmetic and comparison
operators with scalar/vector/matrix broadcasting, string equality comparisons,
MATLAB constants such as `pi`, one-argument math builtins such as `sin`,
`cos`, `sqrt`, `exp`, and `log`, string builtin `strcmp`, reductions such as
`sum`, `min`, `max`, and `mean`, `size` queries as a single row-vector output
or multiple scalar outputs, 1-based vector and matrix indexing, colon and
vector subscripts, `end` expressions inside indexing, scalar-fill indexed
assignment into existing numeric arrays,
`zeros`, `ones`, and `eye` array constructors, `linspace` vector generation,
transpose, and basic numeric matrix multiplication.

Unsupported dynamic features produce runtime diagnostics. That includes class
instances, automatic growth during indexed assignment, non-scalar
right-hand-side indexed assignment shape matching, function handles, other
builtin multi-output conventions beyond the scalar runtime subset, complex
numbers, sparse arrays, object dispatch, and general dynamic call resolution.
This keeps the first interpreter useful for loop and expression validation
without hiding missing MATLAB semantics behind incorrect fallbacks.

## Bytecode VM

The bytecode layer lowers HIR into stack-style instructions while preserving
source spans and semantic bindings. Function, class, and module nodes remain
boundary instructions; expressions become load/operator/call instructions;
assignments lower right-hand values before explicit store instructions; and
core control flow lowers to jump-target instructions.

v0.27 has an executable bytecode VM for scalar doubles, strings, numeric
vectors/matrices, one-dimensional heterogeneous Cells, matrix/cell literals,
core arithmetic, selected builtins, scripts,
named entry functions with positional arguments, `if`/`for`/`while` control flow with
`break`, `continue`, and `return`, same-file local function calls, isolated
call frames, multi-output call assignment, MATLAB-style numeric indexing with
`end`, `:`, and vector subscripts, and indexed assignment into existing
numeric arrays. It also executes `switch`/`case`/`otherwise` dispatch and
`try`/`catch` diagnostic recovery. When enabled, runtime profiles record
instruction PCs, functions, loops, call/index sites, and assignment sites,
including hot-loop marking plus structured runtime kind/shape observations.
`BytecodeVmOptions` can disable those detailed observations after planning;
aggregate dispatcher counts and typed-region execution summaries remain
available for auditability. The same options can inject an initial workspace
into the outer VM frame. Full profiling records each injected value's kind and
shape as an entry observation. The optimization
planner consumes those profiles and emits hot, stable candidates with explicit
guard records. The typed IR builder lowers candidates into scalar or typed
regions with guard and deoptimization operations. Guard evaluation can check
eligible typed regions against real runtime values before an optimized
execution path enters them. A runtime benchmark layer measures the HIR
interpreter, profiled bytecode VM, profile-off bytecode VM, and profile-off
typed-region VM after parsing/lowering. One unmeasured profiled run creates the
typed module, then all four paths share warmup and rotate measurement order.
The report includes timing distributions, VM dispatch counts, typed
attempts/executions/fallbacks, and typed instruction counts; exact four-way
output equivalence is a required correctness gate.
Optimization candidates also carry bytecode region
contracts: half-open PC
ranges, body boundaries, stack inputs/outputs, variable reads/inputs/writes,
observable outputs, call targets, and conservative side-effect flags. Only
closed scalar loops without unsupported calls, mutation, control flow, or
operations are currently eligible for a typed execution path. The VM
can now hand eligible scalar `for` loops to a transactional typed stack
executor. The executor works on a temporary variable frame and commits only
after the complete loop succeeds; a failed runtime type or stack check leaves
the VM state untouched and resumes the original bytecode loop.

`AdaptiveBytecodeVmSession` owns a longer-lived tiering state. Before promotion,
it merges instruction, function, loop, call-site, assignment, kind, and shape
observations across complete VM invocations. When cumulative loop heat reaches
the configured threshold, the session builds and installs a typed module. The
next invocation runs that module with full profiling disabled. Installation is
restricted to modules containing at least one executable scalar loop; hot but
ineligible call-heavy loops stay in the profiling tier. This is
invocation-boundary tiering, not loop-midpoint on-stack replacement.

Typed executions feed a deterministic tiering policy. Successful regions reset
their consecutive fallback count. Repeated fallback can invalidate the complete
installed module, discard the stale accumulated profile, and return the session
to profiling. A failed source region remains under retraining: it can be
reinstalled only after all of its external inputs have stable scalar numeric
assignment observations. This conservative proof prevents a vector-incompatible
region from oscillating between promotion and immediate fallback. Promotion,
typed execution, fallback, invalidation, and retraining rejection are retained
as session events.

Adaptive sessions may preserve the result workspace between invocations. The
next VM run receives that workspace as its initial frame, while normal scope
and function-frame rules still apply. Retraining first examines assignments
executed before a region; when no such definition exists, it uses the workspace
entry observation. This lets a persistent script move from scalar to vector
state, invalidate an incompatible specialization, return to scalar state, and
install a replacement specialization without manual pipeline orchestration.

`BytecodeVmOptions` can also select a same-file entry function and supply
positional runtime arguments. The VM skips unselected function declarations,
binds arguments to the selected signature, executes that function in the outer
entry frame, and returns declared outputs in signature order alongside the
full variable snapshot. Full profiles retain parameter names, output names,
argument observations, and result observations. Adaptive sessions preserve
these entry profiles across calls and may replace their argument vector between
invocations, so retraining can prove a region input from a stable function
parameter when no preceding assignment defines it.

`CompiledModule` is the reusable embedding boundary above those runtime paths.
It owns source text, semantic HIR, bytecode, diagnostics, and the invocable
top-level function catalog. Compilation stops after a failed parse, semantic
analysis, or lowering phase. Valid modules can preflight a named entry, execute
independent ordinary VM invocations, or construct adaptive sessions that point
at the same immutable HIR and bytecode. The module must therefore outlive every
adaptive session created from it. Class methods remain excluded from the entry
catalog because module entries do not yet carry a class receiver or
constructor-dispatch contract.

`AdaptiveModuleRuntime` partitions mutable tiering state by named entry
function. Each lazily created function session owns its cumulative profiles,
arguments, optional persistent workspace, installed typed module, fallback
counters, invalidations, retraining state, and event history. Alternating calls
therefore warm independently, and invalidating one function leaves every other
function's specialization installed. The runtime exposes compact per-function
state summaries and supports targeted or complete state reset.

Function invocation carries a requested output count separately from the
declared signature. Top-level callers can request zero through all declared
outputs; assignment lowering supplies the same count for local calls. Every
active function frame initializes numeric `nargin` and `nargout` values before
executing its body. A function with `varargin` receives all positional excess
arguments in a one-dimensional Cell; a function with `varargout` may satisfy
requested outputs beyond its fixed output names from `varargout{n}`. Results
expose only the requested output prefix, while the diagnostic frame snapshot
retains all declared outputs and call-introspection variables. The HIR
interpreter and all bytecode tiers share this contract.

The VM also has an initial executable class object model. A class declaration
registers its properties and method bytecode ranges; a constructor call creates
an object with named fields and initializes its first declared output to that
object. Direct `obj.Property` reads and direct variable-backed
`obj.Property = value` writes are executable. Instance method calls receive the
receiver as their first positional argument, while class-qualified calls are
accepted only for methods declared in a `methods (Static)` block. Objects use
the ordinary function-frame contract, so method inputs, outputs, `nargin`, and
`nargout` retain the same runtime behavior as local functions.

Superclass names are retained explicitly in HIR instead of being flattened
into generic statements. A class whose superclass list contains the built-in
`handle` marker allocates shared property storage, so assignment copies an
object reference and mutations are visible through every alias. Other classes
keep field maps by value, so assignment produces independent property state.
Standalone call statements lower with a requested result count of zero; the
callee therefore observes `nargout == 0`, and the VM leaves no unused result on
its operand stack.

Class declarations and executable method ranges are collected before hierarchy
resolution, so same-file superclasses may appear before or after subclasses.
The resolver recursively builds each class's effective property layout and
instance/static method tables. Constructors remain local to their declaring
class. A subclass declaration overrides an inherited method; in a diamond,
the same common-ancestor declaration is merged once and a strictly more
specific override dominates its ancestor definition. Unrelated inherited
definitions require a subclass override, otherwise the hierarchy receives an
ambiguity diagnostic. Missing and cyclic same-file hierarchies are rejected.
Handle ownership propagates through the resolved superclass graph.

Construction allocates the most-derived object once and carries it through a
shared construction context. Explicit `obj@Superclass(args)` instructions
invoke only direct bases and write the updated value object back to the active
constructor output; handle objects retain the same shared property storage.
Uncalled direct bases are initialized with zero arguments in declaration order.
When a derived class has no constructor, its arguments are forwarded to the
first executable direct base and remaining bases receive no arguments. The
context records initialized classes, so a common ancestor reached through a
diamond runs once. `method@Superclass(obj, ...)` invokes the exact method
declared on the named ancestor. Calls made by that base implementation still
use the runtime object's most-derived dispatch table.

The VM intentionally still rejects cross-file/package superclass lookup,
non-`handle` built-in superclass construction, full independent-property
compatibility checks, access-control and
`Abstract`/`Sealed`/`HandleCompatible` enforcement, handle lifecycle operations
such as `delete` and `isvalid`, cyclic object collection, events, enumerations,
dependent properties, class methods as `CompiledModule` entry targets,
function-handle execution, dynamic function handles, automatic numeric-array
growth, non-scalar right-hand-side indexed assignment shape matching, structs,
sparse arrays, and complex values until the IR grows richer mutation, layout,
and dynamic dispatch conventions. Cell execution is deliberately limited to
one-dimensional scalar brace reads/writes; multi-subscript Cells and
comma-separated-list expansion are future work.

This shape is intentionally close to an interpreter dispatch loop, but still
abstract enough for MATLAB's delayed decisions. `CallOrIndex` remains a neutral
operation unless semantic analysis has already bound it to a local function,
method, class, or builtin. That makes bytecode suitable as the handoff point
for future runtime name lookup, profiling, and hot-loop specialization.

## JIT direction

The JIT should specialize hot bytecode regions, not raw AST nodes. The v0.27
runtime profiler can identify frequently executed loops, functions, and
call/index sites, then attach conservative runtime kind/shape observations to
stable profile positions. The optimization planner converts those observations
into explicit candidates and guards. The typed IR builder now lowers those
candidates into typed regions, and the guard evaluator can decide whether
eligible regions may enter a typed path. Later passes should execute or compile
those regions:

```text
bytecode -> cumulative profiling -> tier promotion -> typed IR
         -> guard check -> LLVM ORC JIT
```

The v0.11 benchmark contract introduced the performance and
output-equivalence baseline. v0.12 supplies the executable region boundary and
requires region eligibility in addition to runtime guards. v0.13 adds the
first transactional executor and VM resume path. v0.14 brings that executor
into a typed benchmark and exposes both dispatch reduction and typed work per
run. v0.15 separates full profiling from steady-state baseline and typed
execution. v0.16 adds cross-invocation profile accumulation and automatic
typed-tier installation. v0.17 adds fallback-driven invalidation, conservative
retraining, and a deterministic event history. v0.18 adds explicit entry
workspaces, entry value profiles, persistent script state, and successful
re-promotion after runtime state stabilizes. v0.19 adds named entry functions,
positional arguments, declared output results, and parameter-driven adaptive
retraining. v0.20 adds a reusable compiled module, function catalog, entry
preflight, repeated VM invocation, and adaptive-session construction over one
immutable frontend result. v0.21 partitions adaptive state and typed modules by
entry function, preserving unaffected specializations across invalidation.
v0.22 adds requested-output semantics plus `nargin` and `nargout` across the
interpreter, baseline, typed, adaptive, and module runtimes. v0.23 adds
Cell-backed `varargin` and `varargout` through those same call boundaries. The
v0.24 adds baseline object values and bytecode class dispatch. v0.25 adds
handle-alias ownership, value-copy isolation, retained superclass metadata, and
true zero-output call statements. v0.26 adds order-independent same-file class
hierarchies, inherited property/method tables, override and diamond resolution,
transitive handle ownership, and invalid-hierarchy diagnostics. v0.27 adds
dedicated superclass-call IR, semantic construction-order checks, explicit and
implicit base construction, default constructor forwarding, one-time diamond
initialization, and exact qualified base-method calls. The next steps are
property defaults and validation, access-aware class metadata,
multidimensional and comma-separated-list Cell semantics, persistent code
caches, native lowering, and eventual on-stack replacement while preserving
the same commit/fallback contract.

When dynamic features invalidate assumptions, execution should deopt back to
the interpreter. This is the path that keeps full MATLAB semantics compatible
with aggressive optimization.
