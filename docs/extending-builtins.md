# Extending MParser Builtins

This guide defines the v0.80 source-level rules for extending MParser. The
interfaces below are intentionally usable by an embedding C++ application,
but their binary layout is not frozen until the v0.90 embedding gate.

## Choose The Smallest Extension Level

Use an ordinary `.m` function whenever the implementation can be expressed in
the supported language subset. It participates in the source graph and normal
local, private, package, path, and builtin shadowing without engine changes.

Use a C++ builtin for functionality that needs host code, a runtime primitive,
or a performance kernel. Register it once and pass the registry into module
compilation. Do not add parallel name lists or dispatch branches to semantic
analysis, the HIR interpreter, the bytecode VM, or the typed planner.

An external C ABI adapter is not part of v0.80. Keep independently compiled
plugins behind an application-owned C++ boundary for now. The future adapter
will define version negotiation, array ownership, error conversion, threading,
and lifecycle rules rather than exposing C++ standard-library types.

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
The current context fields are workspace, warning state, object-array policy,
and a reserved dynamic invoker. A context handler must not retain raw pointers
from `BuiltinCallContext` after returning.

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
- follow the descriptor's exact requested-output convention;
- avoid pointers to stack data or unowned external buffers.

The registry checks these rules after the handler returns. Contract violations
become `MParser:BuiltinContractViolation`.

Use `BuiltinResult::failure(call.span, message, identifier)` for expected host
validation errors. Return a specific, stable identifier. Standard and unknown
C++ exceptions are caught at the registry boundary and converted to
`MParser:BuiltinHostException`, but exception conversion is a last-resort
safety boundary rather than ordinary control flow.

## Context And Threading

A `Reentrant` descriptor must be safe for concurrent calls with independent
arguments and contexts. `ContextBound` means callers must respect the owning
session's serialization rules. `Serialized` reserves behavior that requires a
host-managed global lock; the v0.80 runtime does not insert one automatically.

Workspace and warning state belong to one runtime invocation or session. Do
not store their addresses globally. Object-array policy is immutable for one
call. The dynamic invoker field is reserved and currently absent from engine
calls, so declaring it as required produces a deterministic missing-context
diagnostic.

## Conformance Tests

Include `tests/builtin_conformance.h` from a project smoke executable. Compile
the same source with the custom registry, run HIR and bytecode execution, and
compare every observable output. At minimum, test:

- canonical name and every alias;
- minimum, maximum, and invalid input/output counts;
- each declared value/shape constraint;
- zero-output and multi-output behavior when declared;
- every required context and its missing-context diagnostic;
- host exceptions and malformed outputs;
- source-function shadowing and function-handle invocation;
- portable typed execution when a typed lowering is declared;
- native execution when SLJIT is available, plus portable-only fallback.

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
