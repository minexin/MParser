# Runtime Boundaries

This document collects the operational rules shared by the CLI, C ABI, C++
facade, bytecode VM, and typed/JIT tiers. Detailed declarations remain in
[C Embedding API](embedding-c-api.md),
[C++ Embedding SDK](embedding-cpp-api.md), and
[Machine Result Protocol](machine-result-protocol.md).

## Failure Classes

MParser separates failures by phase:

| Phase | Examples | Host outcome |
| --- | --- | --- |
| Request validation | invalid option, argument, entry, output count, or descriptor | rejected request |
| Source loading | unreadable entry/search path, invalid source graph | source-aware load diagnostic |
| Compilation | lexer, parser, semantic, class, or bytecode validation failure | invalid module or compilation failure |
| Execution | language error, unhandled exception, builtin error | runtime failure with diagnostics |
| Resource stop | cancellation or configured limit reached | terminal runtime failure |
| API misuse | null/invalid handle, incompatible struct, allocation fault at ABI boundary | stable C status or C++ `ApiError` |

Language compilation and execution failures are ordinary result diagnostics;
they are not C++ API exceptions. The C++ facade reserves `ApiError` for host
API misuse or boundary failures.

## Diagnostics

A structured diagnostic may contain:

- phase and severity;
- stable identifier and human-readable message;
- optional source name and begin/end positions;
- stack source/function/line frames;
- nested causes.

CLI human mode writes diagnostics to stderr. Machine mode embeds diagnostics
in its one JSON result and keeps stderr empty. C diagnostics are borrowed from
their module or result; C++ wrappers copy them into owning values.

`maximumDiagnosticCount` bounds collected diagnostics. Truncation is reported
through the result/summary contract. Diagnostic messages may improve in v1.x;
automation should branch on protocol status, phase, and stable identifiers
rather than exact English prose.

## Resource Limits

Embedding invocations expose:

| Limit | Meaning |
| --- | --- |
| maximum instruction count | Bytecode accounting budget |
| maximum wall time | Runtime interval after lock admission |
| maximum call depth | Active runtime call-frame depth |
| maximum array bytes | Per-`RuntimeValue` recursive payload bound |
| maximum diagnostic count | Collected diagnostic bound |

Zero means unlimited for each numeric limit. The array limit is not a process
heap quota and does not include every allocator or engine data structure.
The execution summary records observed instruction count, elapsed time,
maximum call depth, maximum array bytes, and maximum diagnostics.

Resource stops are uncatchable terminal request failures. Side effects that
completed before a stop are not transactionally rolled back. A session
remains reusable after a handled request failure unless the host discards it.

Resource accounting cooperates with builtins through the builtin call context.
An extension must not claim bounded behavior if it performs uninterruptible
work or allocates unreported payloads.

## Cancellation

Cancellation tokens use cooperative observation. A token may be cancelled
from another thread and can be shared by retained handle copies. Cancellation
does not forcibly terminate a native thread or unwind arbitrary external
code.

Pre-cancelled requests stop before useful execution. A long builtin or future
external adapter must poll at documented boundaries. Hosts that need queue
deadlines or admission limits enforce those outside MParser because module
graph lock wait is not part of the request wall-time budget.

## Ownership

C ABI modules, sessions, values, results, and cancellation tokens are opaque
reference-counted handles. Every thread that keeps a handle while another
thread may release it must own a retained reference.

Borrowed pointers follow their owner:

- module source names and compilation diagnostics live until module release;
- result outputs, workspace values, summaries, and execution diagnostics live
  until result release;
- value array/string/field accessors live until value release or an operation
  documented to replace that value.

Do not free borrowed pointers. Do not use a borrowed view after releasing its
owner.

The C++20 facade wraps the same handles with RAII. Wrappers are copyable, and
each copy owns a retained reference. Copy a wrapper before handing it to
another thread; do not concurrently assign or destroy the same wrapper
object.

## Values And Arrays

External array shapes and linear indexes use MATLAB column-major order.
Host-created numeric, logical, character, string, Cell, and scalar Struct
values copy their input payload into runtime-owned storage. Returned accessors
are immutable views into value-owned storage.

The v1.0 contract intentionally does not expose:

- a borrowed zero-copy host input buffer;
- a mutable raw runtime array view;
- a general host constructor for structure arrays;
- STL or C++ class layouts across the shared-library ABI.

Future zero-copy support requires explicit allocator, alignment, mutability,
lifetime, threading, and error rules and is therefore additive Post-v1.0 work.

## Function Handles And Objects

Anonymous functions capture the exact semantic free-variable set by value.
Escaped function handles and objects retain the module/runtime state required
for invocation. Handle objects preserve shared identity; value objects copy
according to their value semantics.

Listener ownership does not form a source/listener/callback reference cycle.
Explicit deletion invalidates handle objects and performs supported lifecycle
notification. Using an invalid or deleted object produces a diagnostic.

Returned opaque object/function-handle values remain tied to their owning
module graph. Passing them back to another unrelated module is outside the
contract.

## Concurrency

Independent stateless invocations with no module-bound values may run
concurrently. Invocations carrying mutable objects or closures from a module
serialize on that module's graph lock so shared fields cannot race.

Session operations are ordered. Different sessions from one module also take
the module graph lock because escaped objects may connect their state. The
lock order is module graph first, then session, preventing an inversion
between normal invocation and session paths.

Retain/release counters and cancellation state are atomic. This does not make
one host handle variable safe for concurrent assignment, replacement, or
release. Give each thread its own retained C handle or copied C++ wrapper.

## JIT And State Commitment

Typed/native regions validate guards and stage writes before commit. A failed
optimization attempt discards staged region effects and resumes in bytecode.
Language-visible effects completed before an unrelated later runtime or
resource failure are not globally rolled back.

When active limits cannot be observed precisely by an optimized region,
MParser suppresses that optimization and records the decision. Correct VM
execution and resource enforcement take precedence over optimized coverage.

## Security Boundary

MParser validates source and internal bytecode and applies checked runtime
shape/index/resource rules. It is not an operating-system sandbox. Resource
limits are defense-in-depth controls, not a replacement for process
isolation, filesystem policy, memory limits, CPU quotas, or a host watchdog
when executing untrusted input.

An embedding application remains responsible for:

- controlling which source files and search paths are exposed;
- setting process and operating-system resource limits;
- enforcing queue/admission deadlines;
- isolating untrusted extensions;
- validating machine protocol size before accepting unbounded external input.

## Current Development Boundaries

C source API 1.2 and ABI generation 2 ownership, sealed/extensible structure
rules, and symbol meanings are checked against current headers and consumers.
The C++ source API is 1.2 and promises no C++ binary ABI. Machine protocol 1.1 carries exact
typed and complex numeric values. Builtin source contract 1.1 is compiled with
the engine and is not an external plugin ABI. These current interfaces are
frozen by the v1.2 candidate snapshot; archived v1.0 contracts remain
historical evidence rather than compatibility gates for development changes.

See [Versioning And Deprecation](versioning-and-deprecation.md) for the common
policy.
