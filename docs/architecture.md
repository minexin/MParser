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
  -> Source dependency graph
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

Attribute syntax also preserves structured meta-class lists. A single
`?Class`, an empty cell list, or a comma-separated list such as
`{?Reader, ?pkg.Writer}` is retained as normalized class names alongside the
lossless raw value. Runtime policy resolution therefore does not need to
re-tokenize attribute strings, and namespace-qualified names can feed the
source loader and class registry directly.

Ordinary statements are parsed into a first expression tree when the syntax is
unambiguous enough to preserve. `AssignmentStatement` keeps left and right
children; bracketed multi-output assignments are represented as `OutputList`.
Expression nodes use neutral syntax for MATLAB runtime ambiguities. In
particular, `A(...)` becomes `CallOrIndexExpr` because only semantic analysis
can decide whether `A` is a variable, function, constructor, or overloaded
object.

`SourceLoader` builds the executable multi-file boundary without coupling path
lookup to semantic lowering. It starts with the requested entry file, inspects
structured syntax for class/function references and imports, and recursively
resolves matching source files. A simple class or ordinary function maps to a
same-named `.m` file; a qualified `pkg.inner.member` maps to
`+pkg/+inner/member.m` below a search root. Classes and namespaces prefer the
referring source root, followed by the entry root and repeatable search paths.
Ordinary function lookup is caller-sensitive: an eligible immediate
`private` folder wins, followed by the entry directory and search paths in
command-line order. Candidate chains are tried longest-first so
`pkg.Class.staticMethod()` first resolves `pkg.Class`, not a spurious `pkg`
class. A candidate is added only when its top-level class or first top-level
function and physical namespace produce the requested full name, so unrelated
files outside the dependency closure do not affect compilation. Every loaded
`SourceUnit` receives a stable source index and namespace name. Dependency-loaded
ordinary and private function files also receive an internal primary identity,
and each caller stores alias-to-identity edges. This prevents same-named private
helpers in different libraries from collapsing into one module symbol. Explicit
imports add direct dependency candidates. Wildcard imports combine their
namespace prefix with names actually referenced in the source, avoiding an
eager folder scan. Import inspection and ordinary call discovery are
intentionally conservative and file-wide for graph loading; semantic visibility
and precedence remain scope-correct.

Class-folder discovery adds `@ClassName/ClassName.m` as an alternative class
candidate at each root, including nested `+namespace` roots. Once selected, all
valid sibling `.m` function files are loaded in deterministic filename order
and tagged with the canonical declaring class. The parent folder, not the
`@ClassName` folder itself, is the search-path root. Method files remain distinct
source units so diagnostics and bytecode spans retain their physical file.

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

`import` has dedicated syntax and HIR nodes. Imports are precollected for the
whole containing script or function, including statements textually before the
declaration, and never leak into nested functions or classes. Variable-like
bindings win first, followed by explicit imports, functions local to the same
source, wildcard imports, class-private function bindings, other lexical
symbols, caller-specific external function bindings, and builtins. Explicit imports
can bind namespace functions, classes, and static class methods. Importing an
instance method is rejected during semantic validation, conflicting explicit
aliases and ambiguous wildcard matches receive diagnostics, and a fully
qualified namespace function wins over interpreting the same dotted spelling as
a class/static-method chain.

Namespace function identity is independent of its source-level short spelling.
The first function in `+pkg/work.m` is public as `pkg.work`; later functions in
that file are private to the source graph as `pkg.work>helper`. Local functions
in namespace scripts and class files use the same owner-qualified internal
form. The invocable module catalog filters these internal identities while
source-local aliases bind calls inside their owning file.

An ordinary function dependency uses an internal identity such as
`$path0>work`, while a private function uses an identity such as
`$private1>adjust`. The prefix is compiler metadata, not MATLAB syntax. Every
source resolves its own short alias through the binding edge selected during
loading, so path order and private visibility remain stable throughout HIR,
bytecode, and runtime execution. Entry-source functions retain the existing
public `CompiledModule` entry catalog contract.

Before semantic lowering, the module merger attaches each class-folder method's
primary function to its declaring class. A matching prototype is replaced only
when input/output signatures agree, and its method-block attributes are copied
to the implementation. An undeclared method enters a default public,
non-static methods block. Later functions in the same method file remain
source-local under an owner-qualified identity such as
`pkg.Counter.scale>localMultiply`. Constructors are rejected as separate files,
matching MATLAB's class-folder rule. This normalized class syntax then uses the
same semantic scopes, access checks, hierarchy fixups, and VM metadata as an
inline method.

A function loaded from `@ClassName/private` keeps its hidden `$privateN>name`
identity and also records the canonical class in
`SourceUnit::classPrivateFunctionOwner`. Semantic lowering copies that owner to
`HirNode::lexicalClassName`. The helper remains a module function rather than a
class method, so it receives only its declared arguments and is excluded from
the public invocation catalog. Its source-specific alias is resolved after
wildcard imports but before class methods, which gives MATLAB's function
notation rule without changing dot-member binding. A helper source can bind
other files in the same private folder, while unrelated files and subclasses
receive no alias edge.

The semantic fixup also canonicalizes a known dotted class reference. A syntax
chain such as `pkg.inner.ClassName` remains ordinary member access until all
loaded classes are declared; it then becomes one class-bound HIR name. Physical
namespace names qualify class scopes and runtime keys, while constructor source
signatures continue to use the final unqualified class segment required by
MATLAB syntax.

After every class in the compiled source graph has been lowered, a
hierarchy-aware fixup revisits
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

Property declarations use a shared `PropertySpec` structure rather than raw
text reparsing. It retains dimensions, a class constraint, ordered validator
descriptors and literal arguments, block attributes, and whether an executable
default expression exists. The default remains a normal expression child, so
name binding and future type/JIT passes use the same HIR as script expressions.

Dynamic MATLAB constructs intentionally remain delayed. `CallOrIndex`,
`BraceIndex`, and `MemberAccess` HIR nodes preserve the surface operation and
carry unresolved bindings until a later name-resolution pass has path, package,
class, overload, and runtime workspace context.

## Reference interpreter

The current executable runtime is a small HIR interpreter. Its purpose is to
define and test execution semantics before the bytecode VM and JIT harden. It
supports scalar double values, N-dimensional numeric arrays, N-dimensional
Cells, string literals, variable assignment, local function calls
with isolated stack frames, namespace, ordinary path, and private function calls,
first-output single-value
calls, multiple-output
destructuring for local functions, ignored outputs with `~`, numeric ranges,
`for` loops over ranges or vectors, `while` loops, `break`/`continue`,
`return`, `if`/`elseif`/`else`, `switch`/`case`/`otherwise`, `try`/`catch`
diagnostic recovery, short-circuit `&&`/`||`, arithmetic and comparison
operators with N-dimensional implicit expansion, string equality comparisons,
MATLAB constants such as `pi`, one-argument math builtins such as `sin`,
`cos`, `sqrt`, `exp`, and `log`, string builtin `strcmp`, dimension-aware
`sum`, `prod`, `mean`, `min`, `max`, `any`, and `all`, one-to-three-output
`find`, full-shape `size` and `ndims` queries as a
single row-vector output, selected dimensions, or multiple scalar outputs,
1-based N-dimensional indexing, colon and vector subscripts, folded trailing
dimensions, `end` expressions inside indexing, shape-checked non-scalar
indexed assignment, scalar expansion, and automatic numeric-array growth,
direct indexed empty deletion for vectors and complete array slices,
logical-mask linear and multi-subscript reads/writes, preserved logical
numeric classes and conversion/query builtins,
N-dimensional `zeros`, `ones`, and `cell` constructors, two-dimensional `eye`,
`linspace` vector generation, and shared numeric/Cell `reshape`, `permute`,
`ipermute`, `squeeze`, `repmat`, `cat`, `horzcat`, and `vertcat` operations,
transpose, and basic numeric matrix multiplication.

Unsupported dynamic features produce runtime diagnostics. That includes class
instances, function handles, other
builtin multi-output conventions beyond the implemented
`size`/`min`/`max`/`find` subset, complex
numbers, sparse arrays, object dispatch, and general dynamic call resolution.
This keeps the first interpreter useful for loop and expression validation
without hiding missing MATLAB semantics behind incorrect fallbacks.

## Bytecode VM

The bytecode layer lowers HIR into stack-style instructions while preserving
source spans and semantic bindings. Function, class, and module nodes remain
boundary instructions; expressions become load/operator/call instructions;
assignments lower right-hand values before explicit store instructions; and
core control flow lowers to jump-target instructions.

v0.47 has an executable bytecode VM for scalar doubles, logical values, strings,
N-dimensional numeric arrays and heterogeneous Cells, matrix/cell literals,
core arithmetic, selected builtins, scripts,
named entry functions with positional arguments, `if`/`for`/`while` control flow with
`break`, `continue`, and `return`, same-file local function calls, isolated
call frames, multi-output call assignment, MATLAB-style numeric indexing with
`end`, `:`, and vector subscripts, plus shape-checked non-scalar indexed
assignment, logical-mask reads/writes, and automatic numeric-array growth.
Direct `A(...)=[]` syntax carries a null-assignment bit and direct-colon mask
on `StoreIndex`, allowing the shared assignment runtime to distinguish slice
deletion from an ordinary empty right-hand value. Vector element deletion and
complete row, column, or N-dimensional slice deletion are transactional and
preserve the target numeric class. Literal `[]` is represented as 0-by-0.
Linear and multi-subscript indexing use MATLAB column-major
order, fold trailing dimensions when fewer subscripts are supplied, and expose
trailing singleton dimensions when more subscripts are supplied. It also
executes coordinate-preserving numeric/Cell array transformations and
concatenation through the same checked implementation as the reference
interpreter, plus `switch`/`case`/`otherwise` dispatch and
`try`/`catch` diagnostic recovery. When enabled, runtime profiles record
instruction PCs, functions, loops, call/index sites, and assignment sites,
including hot-loop marking plus structured runtime kind/numeric-class/shape
observations.
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

`runtime_reduction` is the common reduction boundary for both baseline
runtimes. It selects the first non-singleton dimension by default, accepts a
scalar dimension, dimension vector, or all dimensions, and maps every input
and output through the canonical column-major shape helpers. The same boundary
implements empty identities, NaN policies, numeric output-class selection,
extrema value/index pairs, elementwise extrema with implicit expansion, and
`find` orientation and N-dimensional trailing-subscript folding. Keeping this
logic below both dispatchers prevents interpreter and VM semantics from
drifting before native lowering is introduced.

`RuntimeValue::dimensions` is the canonical shape. It always exposes at least
two dimensions and drops trailing singleton dimensions beyond the second,
matching MATLAB's `ndims` convention. The legacy `rows` and `columns` fields
mirror the first two entries for compatibility with existing scalar and matrix
operations. Numeric and Cell payloads retain the runtime's established
row-major physical representation, while `runtime_shape` maps every logical
linear or multidimensional access through MATLAB column-major coordinates.
This isolates storage compatibility from language semantics and provides one
shape contract for both baseline runtimes and optimization metadata.

VM member access is lexical rather than inherited from the dynamic caller.
Every function invocation pushes an access frame. Methods and class-private
helpers place their canonical class in that frame; ordinary, namespace, path,
and unrelated private functions place an empty class identity. This lets a
helper use private properties and methods while preventing an ordinary function
called by a method from borrowing the method's access rights. The frame is
restored across nested calls, constructor chains, failures, and returns.

Enumeration members use declaration-owned bytecode regions rather than class
top-level execution. Each class stores the ordered member metadata, attributes,
constructor argument range, recursion state, and cached runtime object. Class
member access, explicit imports, and string conversion all enter one lazy
construction function. Value-enumeration identity freezes property writes
after construction; handle enumerations keep shared member storage. Equality
and switch matching compare the class-qualified member identity. Enumeration
classes are implicitly sealed, and member collisions are rejected during
semantic predeclaration. Until object arrays exist, `enumeration` exposes its
ordered visible values and names as Cells.

Function handles are first-class runtime values backed by VM-owned callable
descriptors. Anonymous handles retain an explicit parameter vector, a half-open
bytecode body range, the lexical class identity, and a value snapshot of the
defining frame. Named handles resolve once at creation to a builtin, ordinary
function, package function, static method, or bound object method. The public
runtime value carries only an opaque VM-local identity, so closures and private
method metadata do not leak through the embedding boundary.

Event declarations use their own HIR and binding kind but emit no top-level
runtime instruction. Instead, class loading builds inherited event tables with
stable declaration order, access policies, and hidden flags. Listener records
hold weak links to source and listener field storage. `addlistener` also stores
the listener on the source object, while `listener` relies on the caller to
retain its result. `notify` snapshots matching listener identities, then invokes
each enabled callback synchronously with the source and an `event.EventData`
object. Per-listener active state suppresses recursive delivery unless
`Recursive` is enabled. Custom event-data subclasses reuse normal class
construction and receive the built-in `Source` and `EventName` fields at
notification time.

Optimization candidates also carry bytecode region
contracts: half-open PC
ranges, body boundaries, stack inputs/outputs, variable reads/inputs/writes,
observable outputs, call targets, and conservative side-effect flags. Only
closed scalar loops without unsupported calls, mutation, control flow, or
operations are currently eligible for a typed execution path. Statically
bound one-argument calls to `abs`, `acos`, `asin`, `atan`, `cos`, `exp`,
`log`, `sin`, `sqrt`, and `tan` are scalar operations rather than generic
calls; every other call retains the rejection boundary. The VM
can now hand eligible scalar `for` loops to a transactional typed stack
executor. The executor works on a temporary variable frame and commits only
after the complete loop succeeds; a failed runtime type or stack check leaves
the VM state untouched and resumes the original bytecode loop. Its entry
specialization remains double-only, while typed stack values retain the
logical class produced by comparisons and logical operators inside the loop.
Pure math dispatch is shared by the interpreter, baseline VM, region analyzer,
and typed executor, preventing the optimized tier from acquiring a different
function allowlist or numerical implementation.

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
It owns an ordered set of named, namespace-aware sources, caller-specific
function bindings, class-method ownership, semantic HIR,
bytecode, diagnostics, and the invocable
top-level function catalog. Compilation stops after a failed parse, semantic
analysis, or lowering phase. Valid modules can preflight a named entry, execute
independent ordinary VM invocations, or construct adaptive sessions that point
at the same immutable HIR and bytecode. The module must therefore outlive every
adaptive session created from it. Class methods remain excluded from the entry
catalog because module entries do not yet carry a class receiver or
constructor-dispatch contract. Source IDs survive the merge, so parser,
semantic, bytecode, and runtime diagnostics can map a span back to the owning
file. Duplicate top-level classes are rejected before semantic analysis.
Public namespace functions are exposed by canonical name; source-local helper
and dependency-loaded path/private identities remain available to bytecode
binding but are omitted from the public entry catalog. `--module-info` exposes
the source identity and caller binding graph for diagnostics.

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

An effective property name maps to an ordered candidate set rather than one
descriptor. Each descriptor has a stable `DeclaringClass::Property` storage
identity. Repeated diamond paths merge that identity once. Multiple distinct
same-name candidates are compatible only when at most one has non-private
`GetAccess` or `SetAccess`; a class may add its own same-name property only
when every inherited candidate has both access values private. Runtime member
selection first chooses a descriptor declared by the currently executing
method's class, then the unique non-private descriptor. This preserves lexical
base-class access while exposing a subclass's separate public property.

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

Each explicit property default lowers into an
`EnterPropertyInitializer`/`LeavePropertyInitializer` bytecode range. Runtime
class descriptors retain declaration order and share property descriptors
through inherited and diamond layouts. On first use within a VM run, a
descriptor evaluates its default in an isolated class-initialization frame,
validates it, and caches
the resulting value. Object allocation copies cached value data while nested
handle objects retain shared storage. This mirrors MATLAB's evaluate-once class
default behavior and ensures all effective defaults exist before any base or
derived constructor body runs. Stored fields use the descriptor's qualified
identity, so base and subclass defaults with the same surface name coexist in
both value objects and shared handle storage.

Property writes use a class, size, validator pipeline. The current class subset
supports `double`, `logical`, `char`, scalar `string`, `cell`, and same-file
user classes. N-dimensional positive-integer/colon size constraints support
numeric scalar expansion and numeric/Cell reshaping that preserves MATLAB
column-major linear order. Built-in numeric, shape,
text, comparison, and missing-value validators execute left to right. The same
pipeline validates explicit and implicit defaults and direct member writes;
failed validation leaves the object unchanged.

Class member attributes are normalized into executable runtime descriptors.
Property `Access`, `GetAccess`, and `SetAccess` select public, protected, or
private visibility or a selective class list, with immutable writes restricted
to the defining constructor. Method access applies to instance, static,
constructor, and qualified superclass calls. Access checks use the currently
executing method's declaring class, so inherited base implementations retain
base privileges and subclass implementations receive protected access. A
class list always permits the defining class, permits every resolved listed
class, and permits descendants of listed classes. It does not implicitly grant
access to descendants of the defining class unless that class is itself in the
list. Unresolved names are removed; an empty effective list has private
semantics. Constructor checks also carry an explicit requesting class through
implicit and explicit superclass construction.

Constant properties have no per-instance field. Their explicit initializer
uses the declaring-class context, enters the existing one-time property cache,
and is available through class-qualified or object reads. Dependent properties
also have no field; they bind qualified `get.Name` and `set.Name` function
ranges during class resolution. Reads invoke getters, while writes validate
before invoking setters. Active access methods bypass recursive dispatch for
their own property. Value setters return the updated object; handle setters may
mutate shared storage without an output. Handle-only `AbortSet` compares the
current exposed value and skips an equal assignment.

Abstract and sealed contracts are resolved with the same class graph. Method
prototypes and abstract properties remain separate from executable method and
field tables, then propagate as unresolved requirements through every base.
A concrete declaration with the same name satisfies a method requirement;
MATLAB does not require the implementation to preserve the prototype's
signature or attributes. An abstract property implementation must preserve
`GetAccess` and `SetAccess`. Validation declared by the abstract property is
materialized on the effective concrete descriptor, while validation on the
implementation itself is rejected. This lets constructors and later writes
reuse the existing validation pipeline without creating abstract storage.

A class is effectively abstract when it explicitly sets `Abstract` or retains
any unresolved method/property requirement. Direct construction reports the
remaining names, while abstract base constructors still participate in a
concrete most-derived construction chain. A `Sealed` class rejects every
subclass edge, a sealed method rejects direct redefinition, and a sealed class
cannot retain abstract members. Multiple inheritance can satisfy a requirement
with a concrete member supplied by another base before the final abstractness
decision is made.

`AllowedSubclasses` uses the same structured meta-class representation to
validate each direct superclass edge. Only classes in the resolved list may
name the restricted class as a direct base; descendants below an allowed class
remain governed by that allowed class's own policy. An empty or fully
unresolved list is equivalent to sealing the class. A class-list method may be
overridden only by an authorized subclass, and the override must preserve the
complete resolved access policy.

The VM intentionally still rejects non-`handle` built-in superclass
construction, full MATLAB property conversion, custom validators, validator
set-membership/range functions, non-scalar string arrays,
`HandleCompatible` enforcement, cross-file function discovery from command-form
calls, dynamic or text-created function handles, `.mlx`, `.p`, and MEX
precedence, class-folder Live Code/P-code/MEX methods, general handle deletion,
`ObjectBeingDestroyed`, cyclic object collection, property change listeners,
listener/source arrays, numeric/logical/character built-in enumeration bases,
enumeration object arrays, class methods as `CompiledModule` entry targets,
structs, sparse arrays, and complex values until the IR grows
richer mutation, layout, and dynamic dispatch conventions. Cell execution
supports N-dimensional scalar brace reads/writes, but Cell parenthesis
indexing, vector-valued brace selections, and comma-separated-list expansion
are future work.

This shape is intentionally close to an interpreter dispatch loop, but still
abstract enough for MATLAB's delayed decisions. `CallOrIndex` remains a neutral
operation unless semantic analysis has already bound it to a local function,
method, class, or builtin. That makes bytecode suitable as the handoff point
for future runtime name lookup, profiling, and hot-loop specialization.

## JIT direction

The JIT should specialize hot bytecode regions, not raw AST nodes. The current
runtime profiler can identify frequently executed loops, functions, and
call/index sites, then attach conservative runtime
kind/numeric-class/full-shape observations to
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
initialization, and exact qualified base-method calls. v0.28 adds structured
property metadata, executable class-level default caches, implicit defaults,
class/size conversion, and ordered assignment validation. v0.29 adds
access-aware class metadata, constructor visibility, constant and dependent
properties, qualified get/set dispatch, immutable construction, and
handle-only `AbortSet`. v0.30 adds method prototypes as inherited contracts,
abstract-property implementation and validation inheritance, automatic
abstract-class detection, instantiation diagnostics, sealed classes, and
sealed methods. v0.31 adds structured meta-class attribute lists, selective
property/method/constructor access, authorized class-list overrides, and
direct-edge `AllowedSubclasses` enforcement. v0.32 adds candidate-based
property layout, declaring-class-local storage identity, legal private
property redeclaration, lexical property selection, and compatible private
property merging across multiple inheritance. v0.33 adds declaring-class-local
private method candidates, lexical private dispatch, exact selected-method
references, private static-method identity, and compatible private method
merging without weakening visible virtual dispatch. v0.34 adds ordered source
units, recursive class dependency discovery, deterministic class-path lookup,
multi-source compilation, duplicate-class rejection, and file-aware CLI
diagnostics while preserving the existing class identity and runtime tables.
v0.35 adds physical `+namespace` discovery, nested namespaces, canonical full
class identities, qualified constructor/static binding, cross-namespace
inheritance, same-short-name isolation, duplicate-full-name diagnostics, and
first-class-path-member precedence. v0.36 adds public namespace function
discovery, source-local helper identity, structured import scopes, explicit and
wildcard import precedence, imported static-method validation, and namespace
function execution in both baseline runtimes. v0.37 adds ordinary `.m` function
discovery, caller-scoped private folders, current-directory and ordered search
path precedence, stable external function identities, source binding graph
inspection, and execution through both baseline runtimes. v0.38 adds
`@ClassName` definition discovery, nested namespace class folders,
deterministic separate-method loading, prototype/implementation signature
validation, inherited method attributes, unlisted default-public methods,
method-local helper identity, and ordinary/static/private method execution with
inheritance and virtual overrides. v0.39 adds class-private helper ownership,
helper-to-helper discovery, MATLAB function-versus-dot dispatch precedence,
subclass isolation, explicit lexical VM access frames, and source/HIR ownership
inspection without exposing helpers as methods. v0.40 adds structured
enumeration-member symbols and initializer regions, lazy cached construction,
value immutability, handle singleton state, implicit sealing, enum equality and
switch behavior, member imports, package loading, conversions, and visible
member queries. v0.41 adds executable closure and named function handles,
package callback discovery, event metadata and inheritance, listener access and
lifecycle policies, synchronous notification, recursive callback control, and
default/custom event data. v0.42 adds canonical N-dimensional runtime shapes,
column-major logical indexing, numeric and Cell construction/query/mutation,
N-dimensional class property constraints, and full-shape profile and guard
metadata. v0.43 adds shared column-major numeric-array assignment, checked
automatic growth, repeated-index ordering, and N-dimensional implicit expansion
to both baseline runtimes. v0.44 adds shared logical-order reshape, dimension
permutation and inversion, singleton squeezing, checked replication, and
general N-dimensional concatenation for numeric arrays and Cells. v0.45 adds
preserved logical numeric classes, conversion and class queries,
MATLAB-style logical-mask selection and transactional assignment in both
baseline runtimes, and numeric-class-aware profiling, guards, typed IR, and
adaptive retraining. v0.46 adds syntax-sensitive empty deletion through the
shared assignment engine, true 0-by-0 empty literals, command-form session
builtins, and direct scalar typed execution for ten pure unary math builtins.
v0.47 adds a shared dimension-aware reduction runtime, first-non-singleton,
explicit-dimension, dimension-vector, and all-dimension selection, extrema
indices and implicit expansion, missing-value policies, and one-to-three-output
`find` in both baseline runtimes. The next steps are cumulative and moving
array operations, object/listener arrays,
property event listeners,
comma-separated-list Cell
semantics, persistent code
caches, native lowering, and eventual on-stack replacement while preserving
the same commit/fallback contract.

When dynamic features invalidate assumptions, execution should deopt back to
the interpreter. This is the path that keeps full MATLAB semantics compatible
with aggressive optimization.
