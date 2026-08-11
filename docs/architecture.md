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

Function argument declarations reuse `PropertySpec`, but their enclosing group
is explicit in both syntax and HIR. `ArgumentBlockKind` distinguishes ordinary
input, repeating input, output, and repeating output blocks; a dedicated
`ArgumentBlock` HIR node preserves that boundary. Dotted declarations such as
`options.Scale` remain intact, and `FunctionSignature` classifies each named
parameter as positional, repeating, or name-value. It also records the number
of required positional parameters after trailing defaults are considered. A
shared argument-count check combines that value with the fixed parameter count,
repeating-group width, and `varargin` flag, so interpreters and reusable module
entries do not reconstruct calling conventions from raw source.

The interpreter and bytecode VM execute repeating and name-value input groups
with the same layout. Fixed positional parameters bind first and may evaluate
trailing defaults. Remaining positional inputs must contain zero or more
complete repetitions of the declared group. Every repeating parameter receives
a one-dimensional Cell, and each Cell element is converted and validated
independently before the function body runs. A repeating `varargin`
declaration validates each excess input while retaining the ordinary
`varargin` Cell interface.

Modern `Name=value` call operands lower through a dedicated syntax/HIR node and
`MakeNameValueArgument` bytecode operation. A shared invocation normalizer also
accepts legacy string/value tails, resolves exact or unambiguous partial field
names across all declared structures, applies last-value-wins duplicate
semantics, and separates the positional prefix from named values. Each
name-value root is bound as a Struct. Supplied or defaulted fields run through
the same conversion, size, and validator pipeline as positional inputs; an
omitted field without a default is absent. `nargin` remains the caller-provided
positional count. `CompiledModule` preflight and adaptive module sessions use
the value-aware normalizer, including optional fixed inputs, repeating groups,
unknown/ambiguous names, and malformed legacy tails.

An argument declaration of the form `options.?ClassName` remains an ordinary
`Argument` node with an explicit `nameValueSourceClass` field; the structure
root is not encoded into a synthetic dotted field. After HIR lowering, a shared
`ArgumentContractCatalog` projects the loaded class graph into the subset
needed by function invocation. It resolves inherited property order, keeps
public `SetAccess` properties, excludes constants and non-public policies, and
admits an immutable property only in the constructor of its declaring class.
The resulting contracts are materialized as `options.Property` names before
normal invocation normalization. Explicit dotted declarations are collected
first and override a class-derived property regardless of source order. Only
the property's class, shape, and validator contract is copied; its class
default is deliberately removed, so omitted fields remain absent from the
Struct. Semantic validation uses the same expansion to diagnose unavailable
source classes, multiple class-property sources, field collisions, and
positional-parameter collisions. The interpreter, bytecode function table,
and `CompiledModule` entry catalog consume this one resolved contract view.

Output argument groups run after the function body and reuse the same runtime
class conversion, shape adaptation, and validator service. Assigned fixed
outputs are validated and any converted value is written back into the
completed frame before result collection. Missing, unassigned outputs are not
validated. A repeating output is represented by one explicit
`FunctionSignature::repeatingOutput` name and a Cell in the function frame.
The fixed output prefix is followed by zero or more elements from that Cell,
so a named repeating output and `varargout` share one collection convention.
Validation applies to each assigned Cell element and identifies failures with
an occurrence suffix such as `values{2}`. Output initialization, validation,
collection, and public slot naming are shared by the HIR interpreter and VM.

Dynamic MATLAB constructs intentionally remain delayed. `CallOrIndex`,
`BraceIndex`, and `MemberAccess` HIR nodes preserve the surface operation and
carry unresolved bindings until a later name-resolution pass has path, package,
class, overload, and runtime workspace context.

The deterministic frontend fuzz regression feeds bounded curated and mutated
sources through Lexer and Parser twice, fingerprints the complete token/tree
and diagnostic results, and runs Semantic twice only when Parser diagnostics
are empty. It then fingerprints HIR, scopes, symbols, and diagnostics. The
fixed seed and complete failing source make every regression reproducible;
this is a CI regression harness rather than a claim of coverage-guided fuzz
saturation.

## Reference interpreter

The secondary reference engine is a small HIR interpreter. Its purpose is to
define and differentially test execution semantics alongside the production
bytecode VM and JIT. It
supports core real and complex numeric classes, N-dimensional numeric arrays, N-dimensional
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
`find`, shape-preserving `cumsum`, `cumprod`, `cummin`, and `cummax`, numeric
`diff`, full-shape `size` and `ndims` queries as a
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
instances and method handles, anonymous-function text parsing, lazy source
discovery for computed function-name strings, other builtin multi-output
conventions beyond the implemented
`size`/`min`/`max`/`find` subset, sparse arrays, complex integer arrays, and
object dispatch.
This keeps the first interpreter useful for loop and expression validation
without hiding missing MATLAB semantics behind incorrect fallbacks.

## Bytecode VM

The bytecode layer lowers HIR into stack-style instructions while preserving
source spans and semantic bindings. Function, class, and module nodes remain
boundary instructions; expressions become load/operator/call instructions;
assignments lower right-hand values before explicit store instructions; and
core control flow lowers to jump-target instructions.

Before any untrusted `BytecodeProgram` reaches VM metadata collection,
optimization planning, region analysis, or typed execution, the verifier
checks opcode/metadata contracts, nested execution ownership (including
anonymous bodies), jump confinement, `for`/`switch`/`try` structure, index and
transactional-lvalue contexts, and reachable stack depth. Every branch merge
and cycle must preserve the complete operand and structured-context state;
every executable range must leave or return with its declared result depth.
Typed modules are accepted only when their source/header/target identity and
complete region contract equal a contract re-derived from the validated
bytecode. Rejection uses
`MParser:Bytecode:InvalidProgram`, is capped at 64 deterministic diagnostics,
and has no execution side effects. Compiled modules, adaptive sessions, and
benchmarks validate owned immutable snapshots once and then use explicit
trusted internal entry points; typed loop entries do not repeat a
whole-program scan. Legal jumps out of `switch` or `try` scopes unwind those
runtime contexts before execution resumes. Bytecode remains an internal
representation, not a serialized public ABI.

The executable bytecode VM handles every core real numeric class, complex
double/single values, logical values, strings, N-dimensional numeric arrays
and heterogeneous Cells, matrix/cell literals,
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
attempts/executions/fallbacks, source typed instruction counts, and predecoded
kernel instruction counts; exact four-way output equivalence is a required
correctness gate. Two- and three-term colon expressions now lower as one
bytecode range operation and use the same range planner as the HIR interpreter
and typed executor. This keeps positive, negative, zero-step, and empty-range
behavior aligned across tiers.

`runtime_reduction` is the common reduction boundary for both baseline
runtimes. It selects the first non-singleton dimension by default, accepts a
scalar dimension, dimension vector, or all dimensions, and maps every input
and output through the canonical column-major shape helpers. The same boundary
implements empty identities, NaN policies, numeric output-class selection,
extrema value/index pairs, elementwise extrema with implicit expansion, and
`find` orientation and N-dimensional trailing-subscript folding. Keeping this
logic below both dispatchers prevents interpreter and VM semantics from
drifting before native lowering is introduced.

`runtime_numeric` is the canonical numeric-class boundary. It stores
fixed-width integers without a double round trip, carries separate real and
imaginary channels for complex floating values, applies checked MATLAB-like
conversion and saturation, and centralizes scalar element operations used by
operators, ranges, indexing, reductions, scans, embedding, and display.
Typed/native regions currently specialize dense real doubles; numeric-class,
shape, and complexity guards return unsupported values to the VM before an
optimized region can publish mutation.

`runtime_scan` owns cumulative and finite-difference array traversal. The four
cumulative operations retain the complete input shape and walk independent
lines along a selected dimension in either direction. Each line carries an
explicit accumulator and missing-value state, including the different default
NaN policies of arithmetic scans and extrema scans. Numeric `diff` applies a
positive order repeatedly along one fixed dimension and constructs each
smaller result in column-major logical order. Both facilities use
`runtimeNumericValueFromLogicalOrder`, which is shared with reductions and
centralizes logical-class coercion plus row-major payload mapping.

`RuntimeValue::dimensions` is the canonical shape. It always exposes at least
two dimensions and drops trailing singleton dimensions beyond the second,
matching MATLAB's `ndims` convention. The legacy `rows` and `columns` fields
mirror the first two entries for compatibility with existing scalar and matrix
operations. Numeric and Cell payloads retain the runtime's established
row-major physical representation, while `runtime_shape` maps every logical
linear or multidimensional access through MATLAB column-major coordinates.
This isolates storage compatibility from language semantics and provides one
shape contract for both baseline runtimes and optimization metadata.

Text has a separate engine-facing contract in `runtime_value.h` and
`runtime_text`. `CharacterArray` stores a rectangular UTF-16 code-unit payload;
`StringArray` stores a shaped array of independently sized UTF-16 elements plus
an explicit missing bit. A single-quoted literal is therefore a character
array, while a double-quoted literal is always a string scalar. `''` is 0-by-0
char and `""` is 1-by-1 string. UTF-8 decoding and encoding are explicit at
source, CLI, diagnostic, and embedding-facing text boundaries, so neither
`wchar_t` width nor the process locale affects Windows, Linux, or AArch64
behavior.

Both baseline engines delegate text indexing, string brace access, indexed
assignment, growth, deletion, concatenation, comparison, implicit expansion,
conversion, missing masks, and Cell conversion to `runtime_text`. General
array transforms reuse `runtime_array_ops` and preserve logical column-major
order while retaining row-major physical payloads. Typed/native recognition
does not lower text operations and falls back to the bytecode VM before
mutation or output publication.

Object arrays use the same visible shape contract through `runtime_object`, but
retain scalar objects as the established fast representation. A nonscalar
object stores scalar `RuntimeValue` elements in row-major physical order;
`runtimeObjectLogicalElement` and the shared index-selection planner perform
all MATLAB-visible column-major mapping. Empty arrays retain class, handle/value
category, and dimensions without manufacturing placeholder elements. Metadata
and `MException` objects remain outside this class-object path.

The object-array policy supplied by the bytecode VM owns two class-dependent
operations: resolving a most-specific common class and invoking a default
constructor for growth. Ordinary arrays require one exact class. A hierarchy
whose root derives from `matlab.mixin.Heterogeneous` may contain multiple
subclasses and records their most-specific shared user superclass as the array
class. Value and handle categories cannot mix. Failed class resolution,
default construction, shape validation, or element assignment leaves the root
value unchanged.

Indexing, assignment, deletion, concatenation, transpose, `reshape`,
`permute`, `ipermute`, `squeeze`, `repmat`, and `cat` reconstruct arrays from
logical element order and therefore share one layout invariant. Value elements
copy their field maps; handle elements retain shared field identity unless
growth creates a distinct default element. Nonscalar property reads produce a
comma-separated list in logical order, while methods receive the complete
array. Direct `array.Property = value` is rejected; indexed scalar writes use
the lvalue transaction and copy modified value objects back through the array.
`delete` and `isvalid` traverse handle arrays elementwise and tolerate repeated
aliases without running destruction twice.

Typed/native executors accept only their declared scalar-double and dense
linear-double inputs. An object observed at a typed boundary fails the guard or
executor input contract before mutation and resumes in the bytecode VM. Object
addresses and handle storage never enter native cache keys or generated code.
The remaining object-array compatibility limits are default-constructor
single-call side-effect equivalence, heterogeneous
`getDefaultScalarElement`/`convertObject`, ordinary class dominance conversion,
custom `subsref`/`subsasgn`/concatenation overrides, and automatic
reachability-based handle destruction.

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

Function handles are first-class runtime values backed by shared
`RuntimeFunctionHandle` descriptors rather than a VM-local identifier table.
Each descriptor has an immutable identity, callable kind, backend kind,
display name, source data, and an optional `RuntimeCallableContext`. Anonymous
handles retain an explicit parameter vector, the HIR or half-open bytecode body
range, lexical class identity, and a value snapshot of only the free variables
referenced by the body. Nested anonymous bodies contribute their external free
variables, while parameters at either level remain local. HIR and bytecode use
the same semantic capture analysis.
Named handles resolve once at creation to a builtin, ordinary function,
package function, static method, or bound object method. Builtin descriptors
are backend-independent; source-backed descriptors remain tied to the engine
and compiled source graph that owns their executable body.

`CompiledModule`, module sessions, adaptive sessions, and module-bound handles
retain one callable context across invocations. The context carries a shared
lifetime anchor to the immutable compiled artifacts, so a returned handle or
session remains valid after the original `CompiledModule` object is destroyed.
The handle can be passed back to later invocations of that same compiled
identity. Passing it to another compiled module fails deterministically instead
of using foreign instruction or HIR addresses. Context-free builtin handles
may cross that boundary. This is an in-process lifetime contract, not a
serialized or cross-process handle ABI.

Both baseline engines share the MATLAB-like dynamic-call contract. A neutral
`CallOrIndexExpr` evaluates an unresolved target once, invokes it when it is a
function handle, and otherwise applies indexing. `feval` preserves the
caller's requested output count; `str2func`, `func2str`, and `functions` use the
same descriptor helpers, including anonymous closure workspace metadata.
Literal `feval('name', ...)` and `str2func('name')` targets participate in
source-graph discovery. Resolution prefers a loaded public path/package
function, then a public static method, then a builtin. Private path targets
cannot be obtained by text, while lexical `@name` creation retains private
visibility. A computed string can address an already compiled target but does
not lazily load a new source file. Anonymous function text parsing and HIR
method-handle execution remain explicit unsupported boundaries.

Event declarations use their own HIR and binding kind but emit no top-level
runtime instruction. Instead, class loading builds inherited event tables with
stable declaration order, access policies, and hidden flags. Listener records
hold weak links to source and listener field storage. The active VM registry
strongly retains `addlistener` results for source-coupled lifetime without
placing a listener back-edge in source fields; `listener` relies on the caller
to retain its result. `notify` snapshots matching listener identities, then
invokes each enabled callback synchronously with the source and an
`event.EventData` object. Per-listener active state suppresses recursive
delivery unless `Recursive` is enabled. Custom event-data subclasses reuse
normal class construction and receive the built-in `Source` and `EventName`
fields at notification time.

`runtime_metadata` gives metadata values a runtime representation that does not
hold pointers into VM-private class tables. A scalar descriptor stores a
canonical `matlab.metadata.*` class and a stable textual identity; a descriptor
array stores scalar descriptors in `RuntimeValue::cells` and uses the shared
shape contract. Member access resolves that identity back through the current
VM class catalog. This keeps values copyable across frames and reusable module
invocations while avoiding lifetime coupling to `ClassInfo`, `PropertyInfo`,
or `FunctionInfo` addresses. Current `meta.*` compatibility names are
canonicalized at the boundary, so equality, `isa`, display, and metadata
queries do not fork into two type systems.

Optimization candidates also carry bytecode region
contracts: half-open PC
ranges, body boundaries, stack inputs/outputs, variable reads/inputs/writes,
observable outputs, call targets, and conservative side-effect flags. Only
closed scalar loops without unsupported calls, mutation, unstructured control
flow, or operations are currently eligible for a typed execution path.
Perfectly structured nested `for` loops are part of the enclosing contract;
the analyzer records their count and maximum depth and validates every header
and latch boundary. Structured `if`/`elseif`/`else` bytecode is accepted when
each jump is forward, closed by the selected region, and remains at the same
loop depth. A control-flow dataflow pass intersects incoming definition sets,
so a variable assigned by every arm is local to the kernel while a value that
may be read before assignment remains an external guarded input. Backward
jumps, `break`, `continue`, `return`, and exception control flow retain the
bytecode boundary. Statically
bound one-argument calls to `abs`, `acos`, `asin`, `atan`, `cos`, `exp`,
`log`, `sin`, `sqrt`, and `tan` are scalar operations rather than generic
calls; every other call retains the rejection boundary. The VM
can now hand eligible scalar `for` loop trees to a transactional predecoded
scalar kernel. Kernel preparation resolves variable names to contiguous slots,
converts supported operations and pure math calls to enum opcodes, assigns
expression temporaries to indexed registers, fuses the final producer with its
destination store, lowers nested boundaries to structured loop operations,
and patches bytecode branch targets to closed scalar-kernel instruction labels.
Its span executor runs arbitrary well-nested scalar loops and uses a direct
path for leaf bodies. The executor commits scalar slots only after the
complete loop succeeds; a failed entry type or kernel compilation check leaves
the VM state untouched and resumes the original bytecode loop. Its entry
specialization remains double-only, while typed stack values retain the
logical class produced by comparisons and logical operators inside the loop.
Pure math dispatch is shared by the interpreter, baseline VM, region analyzer,
and typed executor, preventing the optimized tier from acquiring a different
function allowlist or numerical implementation. Profile-off execution also
skips reconstruction of per-iteration loop observations; aggregate source and
kernel work remains available in the typed execution summary.

`AdaptiveBytecodeVmSession` owns immutable bytecode and semantic snapshots
alongside its longer-lived tiering state, so caller mutation after construction
cannot invalidate its one-time verification. Before promotion, it merges
instruction, function, loop, call-site, assignment, kind, and shape
observations across complete VM invocations. When cumulative loop heat reaches
the configured threshold, the session builds and installs a typed module. The
VM re-derives each typed-region contract from the validated bytecode before
installation. The next invocation runs that module with full profiling
disabled. Installation is restricted to modules containing at least one
executable scalar loop; hot but ineligible call-heavy loops stay in the
profiling tier. This is invocation-boundary tiering, not loop-midpoint on-stack
replacement.

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
Its shared immutable data owns an ordered set of named, namespace-aware
sources, caller-specific function bindings, class-method ownership, semantic
HIR, bytecode, diagnostics, and the invocable top-level function catalog.
Compilation stops after a failed parse, semantic analysis, or lowering phase.
Valid modules can preflight a named entry, execute independent ordinary VM
invocations, or construct adaptive and stateful module sessions over the same
artifacts. These derived objects retain the shared data, so the original module
object does not impose their lifetime. Class methods remain excluded from the
entry catalog because module entries do not yet carry a class receiver or
constructor-dispatch contract. Source IDs survive the merge, so parser,
semantic, bytecode, and runtime diagnostics can map a span back to the owning
file. Duplicate top-level classes are rejected before semantic analysis.
Public namespace functions are exposed by canonical name; source-local helper
and dependency-loaded path/private identities remain available to bytecode
binding but are omitted from the public entry catalog. `--module-info` exposes
the source identity and caller binding graph for diagnostics.

`RuntimeSessionState` is the mutable workspace boundary shared by the HIR
interpreter, bytecode VM, production fallback, and adaptive execution. Global
bindings use the declared variable name. Persistent bindings use the canonical
compiled callable identity, owning function identity, and variable name; class
methods use a class-qualified function key. This preserves global sharing while
preventing same-named functions in different compiled modules from sharing
persistent storage accidentally. First declaration installs a 0-by-0 double
matrix, matching the empty-value checks expected by initialization idioms.
Reads and every supported write form route through this state rather than
through an engine private map. Individual map operations and snapshots are
mutex-protected;
compound script invocation and read-modify-write sequences are not atomic, so
an embedding caller must serialize concurrent use of one intentionally shared
state.

`CompiledModule::createSession()` creates an isolated state by default and
returns a `CompiledModuleSession` that owns both the module identity and state.
It supports repeated invocation, global and persistent snapshots, clearing one
global, clearing all persistent values for one function, clearing all globals,
and resetting all shared state. A caller may inject a state to share globals
deliberately; persistent values remain scoped to the compiled callable
identity. A session's persistent snapshot and targeted function clear are
filtered to that identity. Bare `CompiledModule::invoke()` remains an
independent invocation unless options provide a state. Adaptive-session
`reset()` resets tiering observations and workspaces but deliberately preserves
the runtime state; module-session or `RuntimeSessionState` reset is the explicit
state-erasure operation.

`AdaptiveModuleRuntime` partitions mutable tiering state by named entry
function. Each lazily created function session owns its cumulative profiles,
arguments, optional persistent workspace, installed typed module, fallback
counters, invalidations, retraining state, and event history. Alternating calls
therefore warm independently, and invalidating one function leaves every other
function's specialization installed. The runtime exposes compact per-function
state summaries and supports targeted or complete state reset.

Function invocation carries a requested output count separately from the
declared signature. Functions without a repeating output accept zero through
all declared outputs; functions with a final named repeating output or
`varargout` accept an expanded result count. Assignment lowering supplies the
same count for local calls. Every active function frame initializes numeric
`nargin` and `nargout` values before executing its body. A function with
`varargin` receives all positional excess arguments in a one-dimensional Cell.
A repeating output receives a Cell and satisfies requested outputs beyond the
fixed prefix from its elements. Results expose only the requested slots, while
the diagnostic frame snapshot retains all declared outputs and
call-introspection variables. Post-body output contracts can convert or reject
assigned values before result collection. The HIR interpreter and all bytecode
tiers share this contract.

The VM also has an initial executable class object model. A class declaration
registers its properties and method bytecode ranges; a constructor call creates
an object with named fields and initializes its first declared output to that
object. Direct `obj.Property` reads and direct variable-backed
`obj.Property = value` writes are executable. Instance method calls receive the
receiver as their first positional argument, while class-qualified calls are
accepted only for methods declared in a `methods (Static)` block. Objects use
the ordinary function-frame contract, so method inputs, outputs, `nargin`, and
`nargout` retain the same runtime behavior as local functions. Constructor,
instance, and static-method outputs also pass through ordinary or repeating
output argument validation before they leave the class call boundary.

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
supports `double`, `logical`, `char`, string arrays, `cell`, and same-file user
classes. N-dimensional positive-integer/colon size constraints support numeric
scalar expansion and numeric/Cell/text reshaping that preserves MATLAB
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
set-membership/range functions,
`HandleCompatible` enforcement, cross-file function discovery from command-form
calls, anonymous-function text parsing, lazy source discovery from computed
function-name strings, `.mlx`, `.p`, and MEX precedence, class-folder Live
Code/P-code/MEX methods, automatic handle
destruction at scope/workspace teardown, cyclic object collection,
listener/source arrays, numeric/logical/character built-in enumeration bases,
enumeration object arrays, class methods as `CompiledModule` entry targets,
sparse arrays, and complex values until the IR grows richer mutation, layout,
and dynamic dispatch conventions. Structures support ordered heterogeneous
fields, N-dimensional element arrays, static/dynamic direct member access,
indexed reads, whole-element assignment, common growth, vector deletion, and
comma-separated field results. General nested value updates such as
`S(index).field = value` remain outside the current lvalue contract. Cell
execution supports N-dimensional scalar brace reads/writes, but Cell
parenthesis indexing, vector-valued brace selections, and comma-separated-list
expansion are future work.

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
eligible regions may enter a typed path. v0.50 provides a portable predecoded
register kernel for complete structured scalar loop nests. v0.51 compiles the
same kernel contract to native machine code through optional SLJIT. v0.52 adds
closed forward branch operations and path-sensitive definite-input analysis to
that shared kernel. v0.53 adds a bounded, thread-safe native-code LRU with
explicit limits, lifecycle operations, and statistics, while retaining the
portable executor as a build-time and runtime fallback. v0.54 adds backend-
neutral linear array load/store operations for preallocated dense double
vectors. Both backends copy array state before entry and publish it only after
the complete region succeeds. The
structured kernel IR remains backend-neutral so a future LLVM ORC backend can
target richer regions without changing profiling, guards, or deoptimization:

The pinned SLJIT source is vendored under `third_party/sljit`. CMake compiles
its single `sljitLir.c` entry translation unit into a private static target, so
the default native build does not fetch dependencies or expose SLJIT types in
MParser's public execution contract.

For branch-bearing kernels, both executors use the same scalar truth contract
and forward jump targets. The native emitter owns one label table for the
kernel, binds jumps when their target is emitted, and rejects unresolved or
cross-span targets before code installation. Per-instruction counters are
enabled for branch kernels so source and kernel work reflect the arms actually
executed rather than a static loop multiplier.

For linear-array kernels, the region analyzer admits only one direct numeric
subscript at each indexed read or write. Runtime specialization then requires a
dense double `Vector` or `Matrix` whose shape has at most one non-singleton
dimension, so the existing row-major payload offset is identical to MATLAB's
column-major linear offset. Indexes must be finite positive integers and must
remain within the preallocated payload. The typed kernel owns copied elements
and a written-array bitmap; failure returns no workspace, while success rebuilds
the original runtime kind and dimensions. The SLJIT emitter calls checked load
and store helpers through `SLJIT_ARGS3(F64, P, W, F64)` and
`SLJIT_ARGS4V(P, W, F64, F64)`, leaving platform register assignment to SLJIT.
The structural cache key includes array-slot count and each instruction's slot
identity, but excludes runtime pointers, values, dimensions, and lengths.

The native cache defaults to 256 entries and 16 MiB of generated code. Lookup
and LRU mutation are serialized by a standard C++ mutex, while expensive SLJIT
compilation occurs outside that critical section. Concurrent misses for the
same structural key may compile redundantly, but installation converges on the
first resident kernel and records the duplicate work. Cache entries hold shared
ownership of generated code, so eviction, dynamic shrinking, or explicit clear
cannot invalidate an execution already in flight. Displaced ownership is
released after unlocking, avoiding executable-memory destruction under the
cache mutex. A zero entry or byte limit means compile-and-execute without
retention; it does not disable the native backend.

```text
bytecode -> cumulative profiling -> tier promotion -> typed IR
         -> guard check -> structured scalar kernel
                        -> SLJIT native code + bounded process LRU
                        -> portable register-kernel fallback
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
`find` in both baseline runtimes. v0.48 adds shared N-dimensional forward and
reverse cumulative scans, operation-specific NaN defaults, logical output-class
rules, and fixed-dimension higher-order numeric differences. v0.49 replaces
per-iteration typed stack interpretation with a predecoded scalar register
kernel, fuses expression stores, removes disabled-profile loop reconstruction,
and reports source-versus-kernel instruction work. v0.50 forms complete
well-nested scalar loop regions, unifies colon-range semantics across runtime
tiers, preserves zero-trip definite-initialization behavior, and adds a leaf
loop dispatch fast path. v0.51 adds optional native SLJIT lowering, executable
capability reporting, structural process-wide code caching, backend selection,
and transaction-safe portable or bytecode fallback. v0.52 adds structured
`if`/`elseif`/`else` regions, control-flow-aware definite assignment, portable
kernel jumps, native SLJIT labels, exact branch-path counters, and branch-cache
regressions. v0.53 adds entry and code-byte limits, LRU eviction, safe dynamic
shrink and clear operations, cache activity/lifecycle metrics, CLI controls,
and concurrent cache regression coverage. v0.54 adds transactional dense-double
vector element loads and stores to portable and native typed loop kernels,
checked 1-based indexing, structural cache reuse, observable index counts, and
AArch64 QEMU execution coverage. v0.55 adds guarded first-invocation JIT
execution. v0.56 makes positional `arguments` blocks executable in the VM,
v0.57 shares their validation service with the HIR interpreter, and v0.58
executes trailing positional defaults. v0.59 stabilizes all argument-group
kinds and executes repeating inputs. v0.60 adds modern and legacy name-value
invocation with Struct-backed roots. v0.61 completes post-body fixed and
repeating output validation, including named repeating output expansion across
baseline, compiled-module, adaptive, and class call paths. v0.62 adds
`options.?ClassName` class-property contract expansion across syntax, HIR,
semantic validation, both baseline runtimes, compiled modules, constructors,
and reusable runtime tiers. v0.63 executes class metadata lookup, read-only
descriptor access, inherited member lists, visibility queries, metadata
indexing, class-relation operators, current and legacy metadata names, and
method introspection in the bytecode VM. v0.64 makes loaded callable
signatures executable metadata. Semantic results
retain source-unit names and namespaces, while the bytecode VM assigns each
ordinary, namespace, local, constructor, and method function a stable textual
identity. `matlab.metadata.Function`, `CallSignature`, `Argument`,
`ArgumentIdentifier`, `ArgumentValidation`, `ArgumentValidator`,
`DefaultArgumentValue`, and array-dimension descriptors resolve their fields
on demand from that identity. No descriptor stores a pointer into the VM's
function or class maps. `metafunction` applies the existing invocation
normalizer and argument validator when selecting by runtime values, so
reflection and execution agree on positional, repeating, and name-value
calling conventions without executing the selected body.

v0.65 layers per-instance dynamic properties onto the existing handle-object
storage model. A `dynamicprops` superclass marks the class as handle-based,
while each owner stores reserved descriptor and value entries in its shared
field map. `matlab.metadata.DynamicProperty` descriptors carry a stable
run-local ID and shared mutable attribute state without retaining their owner,
so owner-to-descriptor cycles are avoided. A VM registry keeps only weak owner
and descriptor references, is reconstructed from initial workspace objects,
and drives deletion. Member reads and writes first resolve a valid dynamic
descriptor, enforce its access policy, and then use stored data or invoke its
`GetMethod`/`SetMethod` function handle. Static property and method dispatch is
unchanged. The metadata hierarchy treats DynamicProperty as a Property and
accepts the legacy `meta.DynamicProperty` spelling through canonicalization.

v0.66 connects observable declared and dynamic properties to the existing
event-listener runtime. Property listeners normalize a source property name or
metadata descriptor into a stable static storage key or dynamic descriptor ID.
Declared access dispatches `PreGet` before the getter and `PostGet` after it;
write dispatches `PreSet` after validation and `AbortSet` comparison but before
the setter, then `PostSet` after the mutation commits. Dynamic access follows
the same ordering around its optional `GetMethod` and `SetMethod`. The shared
listener callback path supplies the property descriptor and a read-only
`event.PropertyEvent`, while existing enabled, recursive, retained, delete, and
validity behavior remains common with ordinary `event.listener` values.
Property listener records contain no native pointers and are invalidated when
their dynamic descriptor is deleted.

v0.67 places a stable production facade in front of these runtime tiers.
`mparser --run file.m` compiles the source graph and performs one bytecode-VM
execution with the full currently implemented language semantics. Before that
execution, the static optimization planner discovers eligible structured loop
regions and lowers them to typed IR; the VM then applies the same guards,
transactional commit, and fallback rules used by the diagnostic JIT paths.
This avoids a profiling execution and therefore preserves one-shot script side
effects.

The production `--jit=auto|off|portable|native` option is a policy selector,
not a separate language runtime. `auto` prefers the native SLJIT backend when
available, `portable` fixes typed regions to the C++ kernel, and `off` bypasses
typed regions. Guard or backend failure resumes safely in a lower tier. The
reference HIR interpreter is exposed as `--run-hir`; `--run-bytecode`,
`--run-jit`, `--run-typed-bytecode`, and `--run-adaptive-bytecode` remain
explicit diagnostic interfaces. This separation lets editors and embedders
depend on `--run` while optimization strategy evolves behind it.

CLI contract 1.0 makes this separation machine-checkable. Every execution or
inspection mode and every scalar option is accepted at most once; options used
outside the modes that consume them are rejected instead of silently ignored.
Production `--run` accepts `--jit` and rejects the diagnostic
`--typed-backend` selector. The undocumented pre-v1 `--run-interpreter` alias
is removed. Human usage errors return 2, while automation uses the independent
`mparser.result` exit mapping and an empty stderr channel.

v0.68 adds explicit lifecycle state to scalar handle-object storage. A valid
destructor is a non-static, non-abstract, non-sealed `delete` method with one
ordinary object parameter, no outputs, no variadic parameters, and no
arguments blocks. Destructors are retained per declaring class instead of
participating in ordinary override inheritance. Explicit free- or method-form
deletion marks the shared object state invalid, notifies the inherited
`handle.ObjectBeingDestroyed` event, invokes the most-derived destructor and
then each superclass destructor in declaration order, and finally removes
dynamic properties and invalidates source-coupled listeners. Diamond paths are
deduplicated by declaring class. A diagnostic in one callback or destructor is
deferred until later lifecycle stages and cleanup have run.

All aliases observe the same validity bit. Lifecycle callbacks may still read
the object while `isvalid` reports false, but ordinary access after destruction
is rejected. Repeated deletion is a no-op. An explicitly retained uncoupled
listener remains valid after its source is destroyed, while its source is
invalid. `events`, `methods`, `ismethod`, `metaclass`, and `?handle` expose the
inherited event and handle methods. Automatic reachability- or scope-driven
destruction remains separate future work.

v0.69 introduced scalar structures through an explicit runtime representation
shared by the HIR interpreter and bytecode VM. Its original structure payload
used `RuntimeValue::fields`, while `fieldOrder` preserved MATLAB field
definition order across construction, assignment, copying, `fieldnames`,
`rmfield`, name-value roots, and display. Keeping the behavior behind the
shared `runtime_struct` layer made the v0.71 array migration local to runtime
value consumers instead of requiring a new parser, HIR, or bytecode model.

Parser and HIR already preserved `s.(expression)` as a two-child member node.
Bytecode now consumes the dynamic name before the receiver, resolves it to the
same ordinary `MemberAccess`/`StoreMember` path, and therefore supports dynamic
declared or runtime-added object properties as well as structure fields.
Direct variable member stores resolve the receiver from the current frame; an
absent variable becomes an empty scalar structure, matching `s.field = value`
creation. v0.72 extends this rule to nested paths through a transactional
root-and-path layer instead of silently dropping copied parent updates.

v0.70 makes exception recovery a shared runtime contract instead of exposing
an engine-specific message string. `Diagnostic` carries an optional stable
identifier, and `runtime_exception` owns identifier validation, limited
message formatting, `MException` construction, throw/rethrow preparation, and
conversion between runtime exceptions and source diagnostics. Both the HIR
interpreter and bytecode VM retain a pending exception while unwinding to the
nearest `catch`; the catch variable is a read-only scalar `MException` object
with `identifier`, `message`, `stack`, `cause`, and `Correction` properties.
Typed and native regions continue to reject exception control flow and fall
back to the bytecode VM.

v0.73 completes the engine-facing diagnostic contract. `Diagnostic` now has
an explicit Error/Warning severity, a complete vector of source frames, and
recursive causes. Each runtime records the current error site followed by
call-site frames from the innermost caller to the entry script or function.
The same frame collector covers local, path, private, package, class-method,
handle, and source-graph calls. The public `MException.stack` is an N-by-1
structure array whose elements expose `file`, `name`, and `line`; embedding
results retain the same data even when the exception is uncaught across files.
This public structure array is the canonical frame store; the exception value
does not maintain a second hidden stack representation.

Stack mutation is an enum contract rather than a boolean convention: `throw`
replaces the stack at the current site, `rethrow` preserves a previously
captured stack, and `throwAsCaller` replaces it after removing the current
frame. `addCause` returns a copied value with an appended `MException` in its
column Cell, and diagnostic conversion recursively preserves those causes.
`getReport` provides stable MParser `basic` and `extended` text and accepts the
MATLAB hyperlinks option for compatibility without emitting terminal links.
`Correction` is a readable Missing placeholder; `addCorrection` fails with
`MParser:UnsupportedExceptionCorrection`, making the unsupported object model
explicit.

`runtime_warning` owns per-invocation warning settings, identifier overrides,
`backtrace`/`verbose` flags, save/restore structures, and `lastwarn` state.
Warnings use a separate internal queue so they cannot trigger `catch`, abort a
VM instruction stream, block adaptive workspace publication, or turn a CLI
run into failure. Public results merge them as severity-tagged diagnostics,
and consumers use `hasErrorDiagnostics` when deciding success. Suppressed
warnings still update `lastwarn`. `assert` uses the shared truth and exception
formatting rules and raises a catchable default or caller-specified exception.
The HIR statement path now requests zero outputs for standalone calls, matching
bytecode lowering and preserving no-output function/builtin semantics.

The machine-readable `compatibility-matrix.json` records this boundary and
every other audited feature by parser, semantic, HIR, bytecode, production,
typed, and native tier. Its CTest validator checks source paths, registered
evidence, allowed states, unique IDs, and gap classification. Release builds
also undefine `NDEBUG` for smoke executables so assertion-based evidence
remains active under optimization.

v0.71 replaces the structure side of `RuntimeValue::fields` with one canonical
array representation: `structElements` stores one ordered field map per
element, `fieldOrder` is the schema and display order even for an empty typed
structure, and ordinary runtime dimensions describe the N-dimensional shape.
Object and exception property maps continue to use `fields`; scalar structures
are simply 1-by-1 structure arrays. Runtime storage follows the existing
row-major container convention, while every MATLAB-visible linear traversal is
converted through the shared column-major index helpers. Structure construction
accepts same-shaped nonscalar Cell field values and broadcasts scalar values.
Indexed reads preserve selection shape; whole-element stores require matching
schemas, support scalar expansion and common growth, and linear vector
deletion preserves MATLAB-visible order.

Nonscalar structure field access produces an internal
`RuntimeValueKind::CommaSeparatedList`. This is a transient evaluation result,
not a storable MATLAB value. The shared `runtime_value_ops` layer expands it in
function arguments, matrix/Cell literals, index arguments, and output lists,
while scalar-only contexts diagnose zero or multiple results. HIR and bytecode
use the same structure and list helpers, including exact requested-output
handling for `[a,b] = S.field`; typed and native execution conservatively fall
back to the bytecode VM for these values. Cell brace comma-separated lists in
all direct output contexts remain a distinct language/runtime boundary.

v0.72 adds `runtime_lvalue` as the shared mutation coordinator. A transaction
owns a detached root value, the current child, and one frame for every
successfully traversed parent. Each frame records the parent value and an
already-evaluated member, parenthesis, or brace segment. The leaf operation is
applied once, then frames are replayed in reverse order. The caller publishes
the new root only after that replay succeeds. Failed numeric/member/type
checks, nonscalar intermediate results, and failed growth therefore leave
value roots unchanged.

`runtime_index`, `runtime_cell`, and `runtime_struct` own container behavior;
the transaction does not reproduce shape or offset calculations. Numeric
reads now share one indexing helper between HIR and VM. Cell `()` preserves a
Cell result and selection shape, while `{}` returns contents or an internal
comma-separated list. Cell writes support contents replacement, Cell subset
replacement, common growth, and linear vector deletion. Structure
intermediate indexing can grow a detached array, and reverse copy-back may
append a newly introduced field to the common schema with empty-double values
for untouched elements.

The HIR interpreter collects a neutral root plus path from existing HIR nodes.
The bytecode lowerer keeps direct `StoreIndex` instructions unchanged for JIT
recognition and uses `BeginLvalue`, descent instructions, and one final
`StorePath*` only for paths containing multiple segments. Separate index
contexts make `end` observe the current transaction child. Dynamic field names
and every subscript are evaluated exactly once before their segment executes.
Try recovery records both index-context and lvalue-stack depth, so a caught
diagnostic discards an unfinished transaction.

Class execution remains a VM feature. The lvalue layer receives object member
read/write hooks backed by the existing property access, validation, accessor,
event, and handle-lifecycle machinery. Reverse replay gives value objects
copy-back behavior and preserves handle-object shared identity. Property
setters and handle writes retain their ordinary observable side effects; the
transaction does not attempt to undo user code. Typed and native region
analysis classifies all path setup, descent, and store instructions as
unsupported dynamic operations and resumes in the VM without changing script
semantics.

v0.77 replaces the VM's duplicated metadata class tests, member-name lists,
and superclass assumptions with one `RuntimeMetadataTypeDescriptor` graph in
`runtime_metadata`. Each descriptor declares its canonical class, recognized
legacy names, direct metadata superclass, public properties and methods, and
the class flags needed by `metaclass`. Canonicalization, runtime class names,
`isa`, `properties`, `methods`, and built-in reflection all consume that graph.
The semantic graph deliberately differs from storage: R2026a signature,
argument, validation, and dimension support objects still use object-shaped
`RuntimeValue` storage, but only the class-related descriptors through
`Function` inherit `MetaData` and `handle`.

Metadata arrays keep their canonical descriptor class and Cell-backed element
payload. VM member access projects a declared property over each scalar in
logical order and returns the existing internal comma-separated-list value;
requested-output expansion therefore uses the same mechanics as structure and
object-array property reads. `findobj` performs direct property-pair filtering
through scalar descriptor resolvers, preserving identities rather than copying
resolved field snapshots. Unsupported properties and scalar-only method calls
have separate deterministic diagnostics.

`Property.Validation` is a lazy identity referencing the same `PropertyInfo`
and `PropertySpec` used by assignment. Its class, dimensions, and validation
functions cannot drift from executable property checks. `isValidValue`
temporarily runs the assignment validator and restores the pending exception
and diagnostics, while `validateValue` keeps the ordinary error. Reflected
validator handles encode the property identity and validator index, carry the
owning `RuntimeCallableContext`, and pass through the standard cross-module
handle guard before invoking one validator. No raw property pointer or class
table address is stored in a handle or native cache key.

v0.78 represents `global` and `persistent` as explicit syntax, HIR binding
kinds, and bytecode declarations. Semantic predeclaration is scoped to the
containing script or function, does not descend into nested functions or
classes, rejects conflicting declaration roles, and requires a workspace
declaration before a reference in the supported subset. A no-argument MATLAB
`clear` still clears the current execution frame's local association without
erasing `RuntimeSessionState`. Command forms such as `clear name`,
`clear global name`, and `clear functionName` are not yet executable language
syntax; embedders use the explicit session APIs above.

Typed-region discovery treats shared binding declarations, loads, stores, and
session-bound loop targets as unsupported optimization operations. It does not
capture mutable session pointers in a portable or native specialization.
Legal workspace code therefore executes in the VM through the existing guarded
fallback contract, including store-only loop variables.

v0.79 separates the canonical runtime value contract from either baseline
engine. `runtime_value` owns the value representation, factories, display and
function-handle metadata helpers, the `RuntimeWorkspace` alias, ownership
classification, storable-value rule, and recursive contract validator.
Runtime headers that only consume values no longer include `interpreter.h`.
This prevents the v0.80 builtin layer from depending on an execution engine
merely to exchange values.

The ownership categories describe copy and lifetime behavior:

- immediate scalar values carry their payload directly;
- value arrays, text, Cells, structures, and value objects copy their value
  containers, while contained handles and callables retain their own shared
  identities;
- handle objects share property storage and may form cycles;
- function handles share one identity-bearing callable descriptor and retain
  their module context;
- Missing, comma-separated-list, and name-value transport values are transient
  and are not ordinary workspace storage values.

Shape remains part of each `RuntimeValue`. Explicit dimensions are normalized,
their product must fit `size_t`, rows and columns must match the first two
dimensions, and each payload must match the resulting element count.
Structures additionally validate one ordered schema, object arrays validate
scalar element storage and value/handle consistency, and module-backed
function handles require a callable context. Validation is cycle-safe for
shared handle fields and function-handle captures. It is an invariant checker,
not a serializer or a claim that every MATLAB storage class exists.

HIR and bytecode execution now share `RuntimeCallFrame`, with distinct script,
function, anonymous-function, and initializer kinds. Function frames record
the callable and source span, supplied positional argument count, requested
output count, workspace, and canonical `nargin`/`nargout` values.
Initializers deliberately do not synthesize function arity variables.

Optimization fallback has a parallel machine-readable contract.
`RuntimeFallbackKind` distinguishes region-shape rejection, calls, unsupported
mutation or control flow, malformed contracts, missing or unsupported runtime
inputs, typed-kernel rejection, backend availability/compilation/runtime
failure, and adaptive retraining rejection. Region analysis, typed execution,
native fallback, VM profiles, adaptive events, and CLI details propagate these
codes while retaining human-readable reasons. A successful portable execution
after an automatic native rejection records the native fallback separately
without turning the overall execution into a failure.

This is an internal source-level freeze for the builtin registry and remaining
engine work. Struct layout, final public symbol/version policy, serialization,
and the versioned machine protocol remain v0.90 release gates. v0.83 projects
this internal value model through an opaque candidate C ABI without exposing
the C++ layout, and v0.84 feeds complete source graphs into that same module
boundary.

v0.80 places one `BuiltinRegistry` between semantic name resolution and every
runtime tier. A `SemanticResult` retains a shared immutable registry, so
compiled artifacts cannot resolve a custom name with one catalog and execute
it with another. The default catalog is frozen; embedders construct a mutable
copy with defaults, add descriptors, freeze it, and supply it through
`CompiledModuleCompileOptions`.

Each `BuiltinDescriptor` owns canonical and alias names, input/output arity,
positional input and output value/shape constraints, implementation kind,
purity, determinism, thread-safety, side-effect and context flags, diagnostic
identity, and optional typed lowering. `BuiltinCall` borrows arguments and
invocation context;
`BuiltinResult` returns exact requested outputs plus diagnostics. The registry
validates required context, converts host exceptions, rejects transient or
malformed output values, checks declared output constraints, and enforces the
same arity contract before either baseline engine observes the result.

Shared and context handlers run identically from HIR and bytecode. Intrinsics
remain explicit engine operations, and unsupported catalog entries produce a
deterministic diagnostic. Representative math, reduction, scan,
array-transform, multi-output, and warning-state builtins have moved to shared
handlers; their old baseline-engine dispatch branches no longer exist.
Semantic resolution, builtin handles, typed-region analysis, optimization
planning, portable execution, native SLJIT, adaptive planning, and benchmark
planning all consume the retained descriptor metadata.

Typed lowering is a closed set of audited kernel identities, not an arbitrary
native callback. A descriptor lacking a valid pure lowering executes in the
VM. This keeps optimization coverage additive and preserves the v1.0 rule that
legal but unoptimized target-subset code must remain correct. Extension levels,
ownership rules, diagnostics, threading, conformance tests, and the future C
adapter boundary are specified in
[extending-builtins.md](extending-builtins.md).

The semantic source-integration boundary is versioned independently as builtin
source contract 1.0. A normalized snapshot records all 118 default descriptors
and every compatibility-relevant metadata field. A generator-backed smoke
test compares the live registry to that snapshot; it does not serialize
handlers or claim a C++ binary ABI. Conformance tests compare recursive runtime
values and diagnostics across HIR and bytecode rather than comparing display
strings.

v0.81 adds an engine-neutral host boundary above the bytecode VM.
`ModuleInvocationRequest` contains entry selection, arguments, output arity,
initial workspace, backend preference, and profiling policy.
`ModuleInvocationResult` owns status, outputs, workspace, projected
diagnostics, and an execution summary without exposing the full VM profile.
Compilation, validation, and execution diagnostics are distinct phases;
warnings do not change a successful status.

After bytecode lowering, `CompiledModule` builds one static
`BytecodeTypedIrModule` with the retained builtin registry and stores it beside
the semantic and bytecode artifacts. `execute()` uses that cached module for
automatic, portable, or native guarded execution and uses the ordinary VM for
an explicit bytecode request. Unsupported optimized regions always fall back
to the VM. `CompiledModuleSession::execute()` injects its existing
`RuntimeSessionState` into the same path, so stateless and persistent hosts
share one request/result contract.

`ModuleExecutionSummary` folds engine details into requested and effective
tiers, instruction count, typed attempts/executions/fallbacks, and native
compile/cache-hit counts. The low-level `invoke(BytecodeVmOptions)` APIs remain
for profiling tools and source compatibility. v0.86 projects this result into
the CLI machine protocol. v0.88 packages the header-only C++ facade. v0.90
freezes C ABI 1.1, C++ source API 1.0, and machine protocol 1.0 as one
machine-validated v1 candidate without exposing internal engine layouts.

v0.82 attaches one `RuntimeExecutionControl` to each engine-neutral
invocation. `RuntimeExecutionLimits` defines zero-as-unlimited instruction,
steady-clock wall-time, call-depth, per-value recursive array-payload, and
diagnostic budgets. A copyable `RuntimeCancellationToken` shares atomic
one-shot cancellation state between the host and execution thread. The VM
checks cancellation and time around instruction dispatch, counts completed
instructions exactly, guards root/function/anonymous calls, and observes live
runtime values and diagnostic volume after controlled instructions.

Resource stops are terminal engine events, not language exceptions. They
produce one stable identifier, bypass bytecode `try` recovery, set
`RuntimeFailed`, and preserve a machine-readable `RuntimeExecutionStopReason`
plus resource high-water marks in `ModuleExecutionSummary`. A session remains
usable after a stop, but side effects completed before the stop are not rolled
back.

Strict checkpoint controls currently suppress portable and native typed
regions because those kernels do not yet contain safe cancellation polls.
Call-depth, payload, and diagnostic controls preserve eligible optimized
regions. This policy is explicit in the execution summary and keeps resource
correctness additive: the guarded VM remains the semantic authority.

`runtimeValueArrayBytes()` counts recursive dynamic payload for one
`RuntimeValue`, with cycle guards for shared object fields and function-handle
captures. It is not an aggregate process-heap meter. Host inputs are rejected
before entry and live VM values are observed at checkpoints; allocation-heavy
context builtins declare `ExecutionControl` permission and must preflight or
checkpoint their own host work. Ordinary host exceptions remain contained.
The internal source-level C++ execution API can still propagate
`std::bad_alloc`; the public C boundary maps it to
`MPARSER_API_STATUS_ALLOCATION_FAILED`, and machine output has a static
allocation-free exit-4 result when ordinary serialization cannot complete.

v0.83 adds a narrow C projection in `include/mparser/c_api.h` and
`src/mparser/c_api.cpp`. `mparser_c_api` is a shared library; the static core
and optional SLJIT dependency are position-independent inputs. The public
header contains no C++ layout. Modules, sessions, results, values, and
cancellation tokens are atomically retained opaque handles, while diagnostic
pointers and string/data views borrow storage from one of those owners.

All C value handles own validated `RuntimeValue` copies. Constructor and
accessor payloads use MATLAB column-major order and are converted at the
boundary, so internal row-major physical storage is not an ABI promise.
Numeric/logical, UTF-16 character/string, Cell, and scalar Struct values can be
constructed externally. Objects and function handles can be returned and
re-injected. Module-defined callable/object graphs retain the producing module;
the request builder rejects a different module identity before execution and
propagates ownership through Cell/Struct composition. Independent builtin
handles do not retain a module.

The C request/result layer maps directly to the v0.81-v0.82 execution
contract, including backend selection, initial workspaces, multiple outputs,
diagnostic trees, resource controls, cancellation, and summaries. Stateless
calls remain isolated. One C session serializes calls around its persistent
state. API misuse returns fixed-width status codes, language failure remains a
result status, and all C++ exceptions are contained; allocation failure maps to
an explicit C status.

ABI candidate major 1 revision 1 distinguishes extensible root records from
sealed array/view records. Request, execution-summary, and source-load roots
carry caller capacity and are initialized through revision-1 sized exports.
The library requires only the frozen v1 prefix, ignores unknown input tails,
and bounds output writes by the recorded capacity. The old initializer symbols
always write the old prefix rather than the library's current `sizeof`, which
keeps v0.86 binaries safe when later headers append fields.

`mparser_source_unit` is sealed because an array parameter makes its compiled
size the descriptor stride. Oversized units are rejected; a future extensible
descriptor requires a new stride-aware API. Status values and symbols are
additive within the ABI major, while incompatible changes require a new major.
The v0.90 compatibility review freezes this behavior through the public
contract manifest, exact header/export snapshots, and 64-bit record-layout
tests. See
[embedding-c-api.md](embedding-c-api.md) and
[c-abi-compatibility.md](c-abi-compatibility.md).

Sized initializers clear only the caller-provided byte range and then populate
the frozen prefix. They do not assign a current-library C++ aggregate over
caller storage, so a future larger library record cannot overwrite an older
or deliberately smaller host prefix.

v0.84 adds two C ingestion paths without creating a second execution model.
`mparser_module_compile_sources` copies an ordered array of versioned
name/source descriptors and compiles it directly through `CompiledModule`.
This is the deterministic host-supplied graph path: the first descriptor is
the entry and source-linked diagnostics retain descriptor identity.

`mparser_module_load_file_utf8` is the filesystem-semantic path. It converts
length-delimited UTF-8 entry/search paths directly to
`std::filesystem::path`, then delegates to `SourceLoader`. The loader discovers
ordinary and private functions, package functions/classes, class folders and
separated methods, imports, superclasses, property types, meta references,
function handles, and call dependencies. It emits the existing `SourceUnit`
metadata for namespace, external function identity, class ownership, private
ownership, and alias bindings; none of that internal metadata is exposed as C
ABI layout.

Both paths converge before Lexer -> Parser -> HIR -> Semantic -> Bytecode and
therefore share cached Typed IR, guarded native/portable execution, VM
fallback, diagnostics, values, sessions, and resource controls. Modules own
all source data. Borrowed source-name views are exposed only through module
enumeration. Filesystem failures create an invalid module with a stable
`MParser:SourceLoadFailed` diagnostic so the host retains one inspection
pattern for compilation and loading failures.

v0.85 gives the narrow C projection a relocatable installation boundary.
`mparser_c_api` and the CLI are exported as `MParser::c_api` and
`MParser::cli`; `MParserConfig.cmake` derives header, library, and executable
locations from its containing prefix. The exported C target carries only the
public C include path and shared-library import contract. The static core and
its C++ headers remain implementation details rather than a premature public
C++ SDK.

The installed-consumer regression configures a separate C11 project after
renaming the installation prefix. It links only the imported C target,
verifies the package/header/runtime versions and C ABI `1.1`, invokes a
two-output function, and runs the imported CLI. Cross builds do not register a
host-side nested CTest; the AArch64 CI explicitly installs and cross-builds
both SLJIT-enabled and portable packages, then executes their consumers under
QEMU. `BUILD_TESTING=OFF` leaves only the core, shared C library, header-only
C++ interface, and CLI production targets.

v0.88 layers `include/mparser/cpp_api.hpp` over the narrow C ABI as a
header-only C++20 facade. Its RAII wrappers retain opaque module, session,
result, value, and cancellation handles; no internal compiler/VM layout or C++
binary ABI is exported by the shared library. `cpp_api_smoke` covers all
current external value kinds, compile/load/invoke-many, multi-output,
diagnostic trees, limits, cancellation, sessions, retained values, and the
production UTF-8 source graph. The package exports `MParser::cpp_api`, and a
separate C++20 consumer builds after the install prefix is renamed. Focused
AArch64 native/portable jobs cross-build and execute both C and C++ consumers.

v0.89 adds a module-owned recursive graph lock at the public C boundary.
Stateless requests without module-bound values remain concurrent. Requests
carrying module-owned objects or closures serialize while the runtime may
read or mutate their shared graphs. Every session execute, clear, and reset
operation acquires the module graph lock before its session lock; this fixed
order protects handle objects retained in persistent/global state and objects
that escape into another host call. The conservative policy serializes
distinct sessions from one module and leaves finer object-grained locking as
a compatible future optimization. Runtime wall-time accounting starts after
lock admission.

The same milestone gives the shared library implementation version `1.1.0`
and ABI-major SONAME/install name 1. Core, SLJIT, and non-C boundary symbols
compile with hidden visibility; `tests/c_api_abi1_symbols.txt` is the exact
90-symbol public manifest checked with platform object tools. Repeated dynamic
load/unload, concurrent retain/release, pure invocation, shared handle,
session, cancellation, and resource-isolation stress run beside relocated
macOS x64/ARM64 C and C++ consumers.

Array ownership is closed for v1.0: host-created payloads copy
into `RuntimeValue`, while result spans are immutable runtime-owned views tied
to one retained value handle. A stable external native callback interface is
Post-v1.0 and must be a new versioned pure-C function table; it will not
expose the source-level registry or C++ runtime layout.

v0.90 adds deterministic named C-boundary faults at source, module, session,
execution, result, diagnostic, value, Cell/Struct, and cancellation-token
publication points. Failures never publish partial handles. Execution is not
globally transactional: a failure before core entry does not commit, while a
failure after the runtime returns may leave session/object effects committed.
This boundary is tested explicitly so host retry policy does not depend on an
unstated rollback assumption.

The same milestone freezes the generated-code cache as bounded and
process-local for v1.0. Concurrent compilation, lookup, limit changes, and
clears share the existing mutex/lifetime contract and are stress-tested. Disk
persistence remains additive until atomic writes, bounds, corruption recovery,
and source/architecture/C ABI/compiler/options/version invalidation are all
defined.

v0.87 adds two independent ABI evolution probes. `c_api_smoke` places the
extensible prefixes inside larger host records, verifies old and sized write
ranges, and executes invocation, source loading, and summary output through
those records. `c_api_v1_compat_smoke` includes only the frozen v0.86 public
header snapshot and links it against the current shared library. The
compatibility demo and relocated consumer exercise revision-1 symbols on x64
and focused AArch64 native/portable paths.

v0.86 adds `machine_protocol.cpp` as an independent projection over
`ModuleInvocationResult`. Production `--run --result-format=json-v1` maps the
selected JIT policy to `ModuleExecutionBackend`, invokes
`CompiledModule::execute()`, and serializes status, outputs, workspace,
diagnostics, and `ModuleExecutionSummary`. It does not serialize a raw
`BytecodeVmResult` or duplicate execution semantics in the CLI.

The serializer owns a structured JSON writer with deterministic UTF-8
escaping and no external JSON dependency. Runtime arrays are traversed by
logical column-major index and mapped through the shared shape helpers, so
internal row-major storage remains private. Character arrays expose UTF-16
code units, missing strings use JSON null, and non-finite doubles use named
string tokens. Cells, structures, comma-separated lists, and name-value
arguments recurse under a fixed nesting guard.

Function handles expose only stable callable metadata; captures, receivers,
addresses, and process identities are omitted. Objects remain opaque class,
shape, handle/value, and enumeration descriptors. This avoids cycles and does
not invent an object-property ownership contract outside the C API.

Compilation, validation, source-load, CLI, and execution failures all produce
one protocol document with stable status-specific exit codes. Human output is
unchanged. Machine mode rejects native-cache statistics, and future
output-producing builtins must be captured or represented rather than writing
unframed stdout. Combining machine mode with `--help` or `--version` produces
a structured request rejection in either argument order. The exact producer
contract is locked by a complete golden fixture, a recursive JSON Schema, an
immutable 1.0 snapshot, exact unsigned-64-bit fixtures, a reference-consumer
contract test, and parsed end-to-end CLI regressions; see
[machine-result-protocol.md](machine-result-protocol.md).

The published schema is a tolerant Draft-7 major-1 consumer profile, while the
golden fixture and snapshot freeze exact producer 1.0. Test-only vendored JSON
and JSON Schema libraries validate the golden, emergency, snapshot, negative,
and every dynamically emitted CLI document. A supplemental semantic pass
checks exact unsigned 64-bit bounds that common signed-number Draft-7
implementations cannot represent directly.

Release packaging uses the ordinary install graph through CPack and emits one
platform/architecture archive, SHA-256 sidecars, and an unsigned in-toto
Statement v1 with the SLSA Provenance v1 predicate. The statement binds the
archive, base Git commit, public contract inputs, toolchain/platform, build
parameters, and local builder trust boundary. A fixed `SOURCE_DATE_EPOCH` and
single-thread archive pass make the same built payload and statement
reproducible. The release smoke test packages twice, checks paths, hashes, and
provenance semantics, proves modified archives and statements are rejected,
unpacks the SDK, builds independent C11 and
multi-translation-unit C++20 consumers, and runs the installed machine
protocol without source-tree or loader-path access. The publication target
requires a clean worktree before CPack runs. This does not claim reproducible
compilation, publisher identity, or a SLSA level; authenticated publication
remains a release operation. The final `v1.0.0` tag completed that operation
in Actions run `30780391460`; the v1.0.0 source snapshot and frozen v1
contracts are retained with the ten authenticated subjects. The final
32-asset Release was published without digest drift and independently
downloaded and verified. Compatible v1.x engine minors build on those frozen
public contracts without rewriting the historical release evidence.
See [release-process.md](release-process.md).

A CMake-only candidate-readiness gate cross-checks the engine version, frozen
public-contract state, release-note status, roadmap status, and the exact open
gap set. It rejects an unexpected Must-have or any remaining Must-have with
non-`none` framework impact, keeping the final validation window focused on
external evidence rather than new runtime architecture. Isolated mutated
matrix fixtures prove Must-have/Should-have framework-impact and blocker-set
drift are denied. The same gate includes the fixed-report JIT scope validator:
`G-JIT-001` stays an additive Should-have deferred to v1.x, and an unreviewed
v1.0 reactivation is rejected.

Revision `f34d8d9` passed all six release lanes in Actions run `30684969401`.
That evidence closes the reliability and documentation Must-haves without an
engine change. Performance evidence subsequently closed in runs `30691616946`
and `30732814590`, and final-tag authenticated provenance closed in run
`30780391460`; the readiness gate now permits no open Must-haves.

The v1.0 performance gate is implemented as a non-installed engineering
executable rather than another public runtime mode. It calls `Parser`,
`CompiledModule`, forced bytecode/portable/native backends, and the bounded
native-cache API directly, then emits versioned
`mparser.performance-baseline` JSON. This preserves the frozen CLI and
embedding contracts while making parse, compile, process-cold, runtime,
allocation, peak-resident-memory, binary-size, and cache boundaries explicit.
Raw timing and allocation samples remain in every report; source and binary
SHA-256 values bind evidence to exact inputs. A Draft-7 schema plus semantic
validator recomputes statistics and checks backend/result/cache invariants,
while CMake independently recomputes the hashes, all without freezing
host-specific timing thresholds. The native-only
`mparser_performance_evidence` target generates and validates the scalar and
dense-array release reports with explicit source revision, OS, architecture,
and `emulated=false` checks. Cross-compilation emulator prefixes remain
available to contract tests, but emulated reports are marked as functional
evidence rather than native performance evidence. See
[v1.0-performance-baseline.md](v1.0-performance-baseline.md).

Windows sanitizer validation is an opt-in build boundary rather than a
runtime feature. `MPARSER_ENABLE_MSVC_ASAN` applies AddressSanitizer only to a
no-JIT, non-packaging configuration, discovers the matching dynamic runtime
beside the selected compiler, and stages it into local main and
relocated-consumer output directories. The DLL is not part of the installed
SDK. A CMake smoke test checks its hash and starts the instrumented CLI before
the broader lifecycle, fuzz, bytecode, soak, and embedding regressions run.

First-party compiler diagnostics use one CMake policy across libraries, the
CLI, tools, demos, and test executables. Checked-in presets and every CI
configure path enable `MPARSER_WARNINGS_AS_ERRORS`; ad hoc builds leave it off
by default, and bundled SLJIT remains outside the policy. Optimized tests
force-include a tiny `NDEBUG` cleanup header before `<cassert>`, preserving
their contracts without MSVC's conflicting-option `D9025` noise.

After v0.90 the v1.0 mainline avoids Parser, HIR, Bytecode, `RuntimeValue`, or
embedding-framework redesign unless release evidence proves a correctness
defect. Reliability, documentation, candidate packaging, performance, and
authenticated-provenance evidence are closed; final 1.0.0 publication is also
closed, leaving bounded sanitizer evaluation outside the release gate. The
measured v1.0 JIT scope audit found no required new specialization and defers
broader typed work to v1.x; any later addition still requires representative
evidence. Long-tail
functions, toolboxes, disk caches, external callback ABIs, zero-copy borrowed
arrays, LLVM/OSR, and full MATLAB compatibility remain v1.x or Post-v1.0 work.

v1.1 closes externally observed core semantic defects without reopening those
framework contracts. Top-level comma handling is a Parser collection policy,
not a new syntax-node shape. Numeric `for` iteration is centralized in
`runtimeNumericForLoopColumns`, which projects each logical column through the
shared shape/index helpers and preserves numeric class. Interpreter and VM use
the same projected `RuntimeValue` sequence. Typed scalar regions preflight the
sequence and return `UnsupportedRuntimeValue` to the VM before mutating the
range stack when any column is nonscalar.

Single-colon linear indexing carries one explicit shape bit from HIR or
bytecode into the shared numeric, Cell, Struct, text, object, and nested-lvalue
read helpers. This distinguishes `A(:)`, which must be column-shaped, from an
ordinary vector subscript whose orientation follows the existing target and
subscript rules. Existing overloads retain the old no-colon entry points, so
the implementation correction does not invent a new public ABI.

v1.2 deliberately advances the host value transport while the project remains
in active development. C source API 1.2, C ABI generation 2, and C++ source
API 1.2 expose
typed numeric buffers and separate complex components; machine protocol 1.1
carries the same classes without losing 64-bit integer precision. The product
and installed SDK both report development version 1.2.0. Historical v1.0
snapshots remain unchanged, but they do not require compatibility adapters in
the current kernel. The active contracts freeze together only after the full
v1.2 numeric, function, sample, regression, and platform train is complete.

The v1.2 host-console slice keeps one semantic authority across all engine and
embedding projections. `runtime_output` owns bounded display/printf formatting
and output records; `BuiltinRegistry` classifies `disp` and `fprintf` as
context-bound console operations and `sprintf` as a pure formatter. The
interpreter and bytecode VM inject an invocation-local sink. Each sink appends
an immutable event and assigns a shared monotonically increasing sequence
before calling an optional external host sink. Rejection becomes the ordinary
`MParser:OutputSinkRejected` runtime diagnostic rather than an exception that
crosses the engine boundary.

Parser expression statements retain their semicolon-suppression bit. Semantic
HIR preserves it, bytecode emits `CaptureExpression`, and both baseline engines
capture only script-level expression statements, never function-body
expressions. Captured values update `ans` whether suppressed or not and share
the output-event sequence. `ModuleInvocationResult` therefore owns two stable
arrays that the CLI and machine protocol merge/project without rerunning or
reinterpreting source. Assignments continue through the workspace snapshot.

`CompiledSourceInfo` classifies every source as script, function, class, or
unknown and records primary-function, pure-function-file, and top-level flags.
`SourceLoader::loadSource` combines an owned in-memory entry with the same
normalized search/package/private/class-folder discovery as a filesystem
entry. C API 1.2 exposes borrowed metadata views, synchronous callback fields,
and owned expression values; the C++ facade copies metadata/events and contains
callback exceptions. Machine protocol 1.1 frames optional event/expression
arrays, while ordinary CLI mode prints the merged human stream. Typed/native
regions remain guarded: output or capture semantics outside their eligibility
continue through the bytecode authority.

The v1.2 core numeric builtin tranche also removes tier-specific equality
behavior. `BuiltinRegistry` routes `mod`/`rem` and `nextpow2` through shared
numeric helpers, shape predicates through `runtime_shape`, and recursive
`isequal`/`isequaln` through `runtime_value_ops`. Interpreter and bytecode VM
therefore share class, shape, NaN, Cell, Struct, and object-lifetime semantics;
typed or native ineligibility returns to that same VM authority. The combined
execution sample is `samples/core_numeric_builtins_demo.m`.

The release gates, compatibility-matrix requirement, builtin-extension
architecture, embedding boundary, platform matrix, and explicit v1.0
non-goals are defined in [roadmap-v1.0.md](roadmap-v1.0.md). That roadmap is
the authority for milestone selection; this document remains the detailed
description of the architecture already implemented.

The release-documentation layer is intentionally derived from those
versioned contracts rather than becoming another semantic authority.
[README.md](README.md) routes users to the manual, build/install guide,
human-readable support matrix, CLI reference, JIT/fallback guide, runtime
boundaries, embedding/API references, builtin author guide, and migration
policy. `release_documentation_smoke` reads `cli-contract-v1.json`, requires
every frozen production option and diagnostic mode in the CLI reference and
live `--help`, then runs both the documented human production sample and the
`mparser.result` machine sample. Installed-consumer and release-archive gates
require the same manual set from a relocated SDK.

When dynamic features invalidate assumptions, execution should deopt back to
the baseline bytecode VM. This preserves the declared MATLAB-like subset
semantics while allowing guarded optimization.
