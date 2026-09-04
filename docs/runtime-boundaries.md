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

## System Capabilities And Files

The active v1.3 engine keeps process-facing state in one
`RuntimeSystemContext` per reusable session. Current-directory, path,
environment, filesystem read, filesystem write, process, clock, sleep, and
random rights are separate capability bits. Dynamic source evaluation has its
own capability and does not implicitly grant process or filesystem access. An isolated context grants none;
the CLI creates a native context for ordinary local script execution; tests
can inject a deterministic `RuntimeHostAdapter` without touching the process.

Open file identifiers begin at 3, belong to one context, and cannot be reused
across sessions. The context defaults to 256 open files and a 16 MiB read
window; formatted scanning independently caps output at 16 million elements.
Closing the context closes every retained host file. A failed parse restores
the unread suffix, and a read-limit failure attempts to restore the native
stream position before reporting an error.

`save` and `load` use the same filesystem capabilities and host adapter rather
than bypassing the session. `save` requires write authority; `load` requires
read authority and searches the current directory followed by the session
search path for a simple relative name. A missing extension becomes `.mat`.
Encoding completes before the destination is opened, and decoding plus
variable filtering completes before a zero-output `load` replaces workspace
entries. An explicitly assigned `loaded = load(path)` returns a scalar Struct
and does not mutate the caller workspace.

The MAT v5 codec defaults to a 256 MiB aggregate input, expansion, and decoded
allocation bound, plus 64 levels of Cell/Struct or compressed-element nesting.
The context's file-read window is enforced before decode; shape-driven
allocations and compressed expansion are separately checked by the codec.
Supported persisted
values are dense numeric arrays in every runtime numeric class, complex
`double`/`single`, logical arrays, UTF-16 character arrays, N-dimensional Cell arrays, and
ordered Struct arrays. String/missing values, objects, function handles,
sparse arrays, tables, and unknown MAT classes fail explicitly. The current
writer accepts default v7-style output, `-v7`, `-mat`, and
`-nocompression`; `-v6`, `-append`, `-ascii`, object serialization, and MAT
v7.3/HDF5 are not silently approximated.

`dir`, file-form `delete`, and `fileattrib` also remain behind the adapter.
Directory timestamps expose both local display text and MATLAB serial dates.
Attribute queries report unsupported platform fields as `NaN`; updates map
`a/h/s/w` to Windows flags and `w/x` plus `u/g/o/a` scopes to UNIX permission
bits. Recursive updates do not follow symbolic links. Text `delete` removes
files only, expands basename wildcards, and leaves directories to `rmdir`;
non-text object deletion remains an engine lifecycle intrinsic.

Formatted reads may prefetch beyond the value they return. Each entry keeps a
logical unread suffix plus sparse physical-byte correction points, so `ftell`
and current-relative `fseek` observe consumed bytes rather than the prefetch
cursor. The correction list is empty for binary streams and Unix text streams;
on Windows it records CRLF contractions without allocating a byte-sized map.
Read/write update streams require an explicit successful `fseek` or `frewind`
between operation directions.

Host adapter calls are synchronous and serialized by the owning context.
Adapters must not re-enter that same context. Process execution and native
filesystem access are capabilities, not sandboxing: an embedding host should
grant them only when its own isolation policy allows the operation.

The current v1.10 development C ABI revision 2 and header-only C++ facade
expose a rooted native context plus an explicit shared Runtime. The host
selects capability bits, root/current/temporary/search directories, a random
seed, and open/read limits, then binds that context to a stateless invocation,
reusable session, or Runtime. A Runtime shares one session state and callable
graph across explicitly registered modules; all Runtime calls are serialized
by its recursive graph lock. Ordinary module/session entry points remain
isolated owner domains and reject foreign module-bound values.

Path-oriented adapter calls canonicalize the longest existing ancestor and
reject `..` or existing symbolic-link targets that leave the configured root.
Directory listings omit entries whose resolved targets escape. Environment and
process capabilities remain host-wide, and a launched command is not confined
to the root. Rooted write operations also reject a path whose canonical form
differs from its lexical form, so `mkdir`, writable `fopen`, `copyfile`
destinations, and `movefile`/`rmdir` sources cannot silently mutate through an
existing symbolic link or path alias. The same rule applies to file deletion
and attribute updates. Read-only operations may resolve an
in-root link. Filesystem checks followed by open remain susceptible to a
hostile local process racing link changes, so this is a deterministic host
policy boundary rather than a complete OS security sandbox.

## Dynamic Source Evaluation

`eval`, `evalc`, and `evalin` re-enter the canonical Lexer, Parser, semantic,
bytecode, and VM pipeline through a borrowed source-evaluator callback in
`BuiltinCallContext`. The outer HIR interpreter and bytecode VM use the same
implementation. `eval` selects the current frame; `evalin` selects caller or
base; `assignin` uses the same frame resolver without compiling source.
Evaluation is frame-transparent: a borrowed base-to-caller workspace chain is
passed into the temporary VM, so `evalin` or `assignin('caller',...)` inside
evaluated text addresses the caller of the original function rather than the
temporary script frame. The chain is valid only for the synchronous evaluator
call and is never retained.

Evaluation begins from a value copy of the selected workspace. A compile
failure leaves the original workspace untouched. Successful execution and
assignments completed before an ordinary runtime error replace the selected
workspace snapshot, matching the language-visible side-effect boundary.
`evalc` retains the dynamically rendered output instead of forwarding it;
ordinary `eval` forwards one ordered output event to its parent invocation.
Multiple requested values are received through collision-free internal names
and removed before workspace commit. Diagnostics retain dynamic line/column
coordinates in their message and project their public span to the call site.
Runtime frame values take precedence over static builtin/function bindings,
including values introduced by `initialWorkspace` or `assignin`. The neutral
call/index bytecode contract preserves `end` and `:` indexing in that case;
typed regions guard callable targets and transactionally fall back when a
workspace value shadows one.

One source string is limited to 16 MiB and one call to 1024 requested outputs.
The nested VM shares the parent execution control, session, builtin registry,
and execution-tier policy. HIR and explicit bytecode modes keep nested source
on bytecode, while production execution supplies the compiled static Typed IR
and selected portable/native backend. Cancellation, instruction/time/array
limits, system capabilities, random state, and open-file ownership therefore
do not reset at an `eval` boundary. MATLAB R2024b rejects function and class
definitions in evaluated text, and MParser rejects the same category.

The selected parent call frame also owns a short-lived storage bridge. Dynamic
`global` declarations associate that frame with session global storage;
dynamic `persistent` declarations use the selected ordinary function's compiled
callable identity. Caller/base routing therefore binds the requested real frame,
not the temporary VM script. Persistent declaration is rejected when the target
is a value-bearing script workspace or a static workspace created by nested
functions. A declaration after a local global candidate emits a deterministic
warning and preserves MATLAB-like existing-global precedence. `clear name` and
`clear` remove runtime associations but do not erase the underlying global or
persistent session value. Ordinary runtime failures retain completed storage
writes and associations.

The temporary module receives a read-only callable catalog for the selected
parent frame. Source-graph-visible local, nested, path, package, and private
functions are represented by their real owner handles, with private aliases
restricted to the source that received the loader binding. Direct calls,
`@name`, permitted string lookup, and pre-existing module-bound handles invoke
synchronously in the active owner engine. The bridge synchronizes the selected
workspace before and after the call and returns output events to the temporary
VM, preserving nested capture updates and `evalc` behavior. It exists only for
the active evaluation and does not grant general cross-module invocation.

A module-bound function handle whose implementation is created inside the
temporary compiled module cannot escape through an output or workspace value.
Inherited owner handles may be invoked or returned and remain valid in their
owner. Pre-existing shared object fields are snapshotted; if a temporary
handle is written into one, those fields are restored in place before the
escape diagnostic is returned. Ancestor workspaces reachable through
`assignin` are included in the same escape scan and restore rule. Session
globals and persistent variables are snapshotted as well, including values not
initially visible in the selected workspace, so temporary handles cannot remain
hidden in session storage after the diagnostic.

## Cancellation

Cancellation tokens use cooperative observation. A token may be cancelled
from another thread and can be shared by retained handle copies. Cancellation
does not forcibly terminate a native thread or unwind arbitrary external
code.

Pre-cancelled requests stop before useful execution. A long builtin or future
external adapter must poll at documented boundaries. Hosts that need queue
deadlines or admission limits enforce those outside MParser because module
graph lock wait is not part of the request wall-time budget.

The active v1.3 numeric utility handlers poll during long sieve and output
construction work and preflight result payloads before allocation. The sparse
`randperm(n,k)` path performs the same preflight before consuming session
random state, so a cancellation or resource rejection does not silently
advance the reproducible random sequence. These checks bound observable
runtime behavior; they do not promise hard preemption inside arbitrary host
adapter calls.

## Ownership

C ABI modules, sessions, values, results, cancellation tokens, and system
contexts are opaque reference-counted handles. Every thread that keeps a
handle while another thread may release it must own a retained reference.

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

Sparse, temporal, and table values use value-owned opaque storage. A table is
module-independent only when every recursively contained variable and its
`UserData` are module-independent; otherwise the retained C/C++ value keeps
the producing module lifetime anchor just like another composite value.

Listener ownership does not form a source/listener/callback reference cycle.
Explicit deletion invalidates handle objects and performs supported lifecycle
notification. Using an invalid or deleted object produces a diagnostic.

Returned opaque object/function-handle values remain tied to their owning
module graph. A Runtime-created value can cross only between modules attached
to that same Runtime; a value from an ordinary module/session or another
Runtime remains rejected with owner mismatch. Scalar user-object public member
access routes through the defining module, while private members remain
private. Cross-module object arrays and dynamic object registries are guarded
until their complete lifetime/indexing contract is implemented.

## Concurrency

Independent stateless invocations with no module-bound values may run
concurrently. Invocations carrying mutable objects or closures from a module
serialize on that module's graph lock so shared fields cannot race. Calls
through one shared Runtime serialize all attached modules and support
reentrant owner callbacks while the recursive Runtime lock is held.

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

## Current Candidate Boundaries

C source API 1.3 and current ABI generation 2 revision 2 ownership,
sealed/extensible structure rules, and symbol meanings are checked against
current headers and consumers. The C++ source API is 1.3 and promises no C++
binary ABI. Machine protocol 1.1 carries exact
typed and complex numeric values. Builtin source contract 1.1 is frozen with
the archived v1.2 candidate; the active candidate descriptor contract is 1.17 and remains
a compiled-in source extension surface, not an external plugin ABI. Archived
v1.0 and frozen v1.2 contracts remain historical evidence rather than
compatibility gates for development changes.

See [Versioning And Deprecation](versioning-and-deprecation.md) for the common
policy.
