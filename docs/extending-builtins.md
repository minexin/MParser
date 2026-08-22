# Extending MParser Builtins

This guide defines the source-level rules for extending MParser. The
interfaces below are engine source-integration APIs, not installed C++ ABI.
Their C++ layout may evolve with the engine even after the public v1.0
embedding boundary freezes.

## Choose The Smallest Extension Level

Use an ordinary `.m` function whenever the implementation can be expressed in
the supported language subset. It participates in the source graph and normal
local, private, package, path, and builtin shadowing without engine changes.

Use a C++ builtin for functionality that needs host code, a runtime primitive,
or a performance kernel. Register it once and pass the registry into module
compilation. Do not add parallel name lists or dispatch branches to semantic
analysis, the HIR interpreter, the bytecode VM, or the typed planner.

An external native callback adapter is not part of the v1.0 contract. Keep
independently compiled integrations behind an application-owned boundary or
build their descriptors into the engine. This prevents an unreviewed plugin
ABI from freezing `RuntimeValue`, registry, VM, or C++ standard-library
layouts.

## External Adapter Decision

The three extension levels are:

1. `.m` functions loaded through the ordinary source graph;
2. source-integrated C++ builtins registered through `BuiltinRegistry`;
3. a future independently compiled native adapter, explicitly Post-v1.0.

The future third level must be a separately versioned pure C function table.
It must negotiate structure sizes and ABI generation/revision, use opaque handles
or copied plain-C values, contain every exception at the boundary, and declare
thread safety, determinism, side effects, workspace/context permissions, and
resource-control cooperation. It must also define callback and allocator
lifetime, array ownership, alignment, mutability, teardown order, and module
unload behavior. It must not expose C++ classes, `RuntimeValue`,
`BuiltinDescriptor`, STL containers, or VM pointers.

v1.0 host-created arrays therefore remain copy-in. Public result accessors
provide readonly runtime-owned spans tied to a retained value handle. A future
borrowed-input adapter must use a new additive descriptor carrying a release
callback and explicit lifetime/threading contract; it cannot reinterpret the
existing constructors.

## Minimal C++ Registration

```cpp
#include "mparser/builtin_registry.h"
#include "mparser/compiled_module.h"

#include <stdexcept>

auto registry = mparser::createBuiltinRegistryWithDefaults();

mparser::BuiltinDescriptor twice;
twice.name = "host_twice";
twice.inputs = mparser::BuiltinArity::fixed(1);
twice.outputs = mparser::BuiltinArity::range(0, 1);
twice.argumentConstraints = {{
    mparser::BuiltinValueConstraint::ScalarNumeric,
    mparser::BuiltinShapeConstraint::Scalar,
}};
twice.outputConstraints = {{
    mparser::BuiltinValueConstraint::ScalarNumeric,
    mparser::BuiltinShapeConstraint::Scalar,
}};
twice.implementation = mparser::BuiltinImplementationKind::Shared;
twice.purity = mparser::BuiltinPurity::Pure;
twice.determinism = mparser::BuiltinDeterminism::Deterministic;
twice.threadSafety = mparser::BuiltinThreadSafety::Reentrant;
twice.summary = "Return twice one scalar numeric input.";
twice.handler = [](const mparser::BuiltinCall& call) {
    if (call.requestedOutputCount == 0) {
        return mparser::BuiltinResult::success();
    }
    return mparser::BuiltinResult::success({
        mparser::makeRuntimeNumberValue(
            call.arguments.front().number * 2.0),
    });
};

const auto registration =
    registry->registerBuiltin(std::move(twice));
if (!registration.succeeded) {
    throw std::runtime_error(registration.error);
}
registry->freeze();

mparser::CompiledModuleCompileOptions options;
options.builtinRegistry = registry;
auto module = mparser::CompiledModule::compile(
    "answer = host_twice(21);", options);
```

Freeze the registry before sharing it or compiling modules. A
`CompiledModule` retains the exact registry pointer used during semantic
analysis, so function resolution and all later execution tiers see the same
catalog.

`CompiledModule::execute()` also builds and retains static typed regions from
that registry. It is the preferred v0.82 embedding path: automatic, portable,
and native requests use guarded optimized execution, while unsupported code
continues in the VM. Request-level resource controls may deliberately suppress
optimized regions that lack safe checkpoints. The older
`invoke(BytecodeVmOptions)` entry remains the low-level compatibility path.

## Descriptor Rules

`name` is the canonical spelling. Each alias resolves to the same descriptor.
Names and aliases must be unique across the registry. Source functions still
shadow builtins according to the documented language resolution order.

`inputs` and `outputs` describe caller-visible arity. A handler must return
exactly `call.requestedOutputCount` outputs, including an empty vector for zero
outputs. Use `BuiltinArity::fixed`, `range`, or `variadic`; do not infer arity
inside multiple engine dispatchers.

`argumentConstraints` are checked positionally before the handler. Omitted
positions mean `Any`. Declare only constraints guaranteed for all accepted
calls. More detailed family-specific validation may remain in a shared runtime
helper and should return one stable diagnostic identifier.

`outputConstraints` are checked after the handler returns and after the
generic `RuntimeValue` ownership/shape contract. Omitted positions mean
`Any`. A mismatch is an extension bug and becomes
`MParser:BuiltinContractViolation`; it is not reported as an ordinary caller
argument error. Typed lowerings require an explicit numeric first input and
first output constraint.

`implementation` selects the ownership boundary:

- `Shared` is a context-free handler shared by HIR and VM.
- `Context` is a shared handler that consumes declared runtime context.
- `Intrinsic` is implemented by engine bytecode or control machinery.
- `Unsupported` is recognized but deliberately not executable.

`purity`, `determinism`, `threadSafety`, and `sideEffects` are behavioral
contracts, not documentation hints. Mark uncertain behavior conservatively.
Optimization must reject an operation whose descriptor does not prove the
required properties.

`contextPermissions` lists every context capability a handler may use.
`requiredContext` is the subset that must be present for every invocation.
The active context fields are structured workspace access, warning state,
object-array policy, execution control, output, system services, display
format, the current registry, and a synchronous dynamic invoker. The invoker
accepts a function handle or text function name and returns the exact requested
outputs plus nested diagnostics through the normal call-frame rules. A context
handler must not retain raw pointers or callbacks from `BuiltinCallContext`
after returning. A descriptor such as `fprintf` that can use either output or
system services lists both in `contextPermissions` but leaves
`requiredContext` empty and validates the selected form in its handler.

`call.callerNargout()` reports the caller-visible output request independently
of the internal result slots used by an expression. `implicitOutputPolicy`
controls statement calls with no explicit target: use `FirstAvailable` for an
ordinary value-returning function, `None` for commands/setters, and
`FirstWhenNoArguments` for query-or-set functions such as `rng`. Do not encode
these distinctions in parser or VM name lists.

`typedLowering` is optional. `None` means the legal call executes in the
baseline VM. A non-`None` value may be used only when the custom function is
semantically identical to the named audited kernel for every value accepted
by the descriptor. It is not a general native callback ABI.

## Value And Ownership Rules

Inputs are borrowed for the duration of the handler. Copy a `RuntimeValue`
when value semantics are intended. Handle objects and function handles retain
their documented shared identities when copied.

Every successful output must:

- be an ordinary storable runtime value rather than Missing,
  comma-separated-list, or name-value transport state;
- satisfy `validateRuntimeValueContract`, including shape and payload size;
- satisfy its positional `outputConstraints` entry when one is declared;
- follow the descriptor's exact requested-output convention;
- avoid pointers to stack data or unowned external buffers.

The registry checks these rules after the handler returns. Contract violations
become `MParser:BuiltinContractViolation`.

Use `BuiltinResult::failure(call.span, message, identifier)` for expected host
validation errors. Return a specific, stable identifier. Standard and unknown
C++ exceptions are caught at the registry boundary and converted to
`MParser:BuiltinHostException`, but exception conversion is a last-resort
safety boundary rather than ordinary control flow. `std::bad_alloc` propagates
to the embedder until MParser has an allocation-safe preallocated reporting
path.

## Context And Threading

A `Reentrant` descriptor must be safe for concurrent calls with independent
arguments and contexts. `ContextBound` means callers must respect the owning
session's serialization rules. `Serialized` reserves behavior that requires a
host-managed global lock; the v0.80 runtime does not insert one automatically.

Workspace, display format, system services, and warning state belong to one
runtime invocation or session. Do not store their addresses globally.
Object-array policy is immutable for one call. System adapters are synchronous
and context-bound. The dynamic invoker is the deliberate path for controlled
synchronous language re-entry: `cellfun` uses it on the owning execution
thread, extracts nested outputs and diagnostics transactionally, and never
retains the callable or callback. Calling a descriptor that requires
`DynamicCall` without an engine context still produces a deterministic
missing-context diagnostic.

Dynamic source re-entry is a separate permission from callable invocation.
`BuiltinContextPermission::SourceEvaluation` exposes one borrowed evaluator
for `eval`-family handlers. It receives source text, current/caller/base
workspace scope, requested output count, capture policy, and call span, then
returns values, captured text, and diagnostics through ordinary runtime-owned
types. A handler must invoke it synchronously and must not retain the callback,
workspace pointers, or any temporary-module value after the call. Declaring
this permission does not itself grant the session's
`RuntimeSystemCapability::DynamicEvaluation` right.
The engine supplies a borrowed logical ancestor-workspace chain so recursive
`caller`/`base` resolution is frame-transparent. Temporary module handles are
checked across the selected and reachable ancestor workspaces, including
pre-existing shared object fields, before the synchronous call returns.

## Resource Cooperation

An allocation-heavy or long-running context builtin should declare
`BuiltinContextPermission::ExecutionControl`. Mark it as required only when the
builtin cannot operate without a production VM execution control; the
reference HIR interpreter does not provide this embedding context.

`call.context->executionControl` is borrowed for one handler call. Use
`checkpoint()` between bounded chunks of work and return promptly when it
fails. Before allocating a large result, compute the requested payload with
overflow checks, compare it with `limits().maxArrayBytes`, and call
`observeArrayBytes()` to register the preflight. Successful outputs are still
validated and observed by the VM after the instruction.

Cancellation and wall time are cooperative. MParser cannot interrupt a builtin
blocked in a system call or host library that does not return to a checkpoint.
The array-byte limit measures recursive payload for one `RuntimeValue`; it is
not an aggregate heap quota. Do not advertise a builtin as resource-bounded
unless its internal work obeys these constraints.

## Conformance Tests

Include `tests/builtin_conformance.h` from a project smoke executable. Compile
the same source with the custom registry, run HIR and bytecode execution, and
compare every observable output. At minimum, test:

- canonical name and every alias;
- minimum, maximum, and invalid input/output counts;
- each declared value/shape constraint;
- zero-output and multi-output behavior when declared;
- every required context and its missing-context diagnostic;
- execution-control polling and allocation preflight when applicable;
- host exceptions and malformed outputs;
- source-function shadowing and function-handle invocation;
- portable typed execution when a typed lowering is declared;
- native execution when SLJIT is available, plus portable-only fallback.

The conformance helper compares recursive `RuntimeValue` structure rather than
display strings, including array shape/class, cell and struct contents,
object graphs, function-handle captures, and diagnostics.

Add one runnable `.m` sample and register it in CTest for `--run-hir`,
`--run-bytecode`, and `--run`. Update the compatibility matrix and milestone
document with those exact test names. A builtin change is not complete when
only its direct helper unit test passes.

## Upstream Checklist

1. Reuse or add one engine-independent runtime helper.
2. Add one descriptor to the default catalog.
3. Remove any displaced hardcoded recognition or engine dispatch.
4. Add conformance, error-boundary, shadowing, and applicable typed tests.
5. Add a public CLI sample with one deterministic summary.
6. Run native and portable-only full suites.
7. Update README, architecture, compatibility matrix, milestone docs, and
   focused AArch64 CI.

Long-tail function additions should normally touch the helper, descriptor,
tests, sample, and documentation only. A change that requires Parser, HIR,
Bytecode, or VM representation work should be treated as a language/runtime
contract change and reviewed against the v1.0 roadmap before implementation.

## Source Contract Version

`kBuiltinSourceContractMajor` and `kBuiltinSourceContractMinor` currently
identify active source contract 1.6. Contract 1.0 established
registration/freeze rules, descriptor meaning, call/result behavior,
ownership, diagnostics, context, threading, resource cooperation, and
typed-lowering eligibility. It does not promise a C++ binary ABI or stable
class layout. Frozen contract 1.1 added host output and execution-context
permissions for v1.2. Contract 1.3 additionally records structured
workspace access, caller `nargout`, implicit-output policy, system/display
contexts, random/display side effects, and the expanded descriptor catalog.
Active contract 1.4 adds current/caller/base workspace resolution and
synchronous source evaluation. Contract 1.5 adds the generalized stream-I/O
descriptor family and corrects warning/`lastwarn` implicit-output metadata.
Contract 1.6 adds the capability-bound filesystem query, path decomposition,
whole-file text read, temporary-name, and status-returning mutation families.
The current v1.3 development catalog exercises
callable-based dynamic invocation through `cellfun`, execution-controlled
numeric utilities, session-random `randperm`, and the `eval`/`evalc`/`evalin`/
`assignin` family plus shared text/binary and managed filesystem operations;
it contains 240 descriptors and 242 registered names.

`tests/public_contract/builtin/1.1/default_catalog.json` remains the normalized
v1.2 candidate snapshot. The active development snapshot is
`tests/public_contract/builtin/1.6/default_catalog.json`; earlier files remain
historical evidence.
`builtin_catalog_snapshot_smoke` regenerates the
catalog in memory and compares every name, alias, arity, input/output
constraint, behavioral classification, context permission, error identifier,
and typed lowering. Review a catalog difference rather than refreshing the
snapshot reflexively.

Compatible additive source-contract changes increment the minor version and
provide defaults that preserve existing descriptors. Removing a field or
changing an existing semantic rule requires a new major version. The common
deprecation window and future external-adapter boundary are defined in
`versioning-and-deprecation.md`.
