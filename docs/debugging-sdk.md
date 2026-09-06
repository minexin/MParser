# Debugging Through The SDK

The development C ABI generation 2 revision 3 and C++ source API expose a
host-driven debugger. This is an embedding interface, not a terminal debugger,
a debug-adapter protocol server, or MATLAB `dbstop`/`keyboard` command support.
See [cpp_debugger_demo.cpp](../samples/cpp_debugger_demo.cpp) for a runnable host.

## Execution And Stepping

Create `mparser_debugger` with a callback, or C++ `Debugger(DebugSink)`, and
attach it to `mparser_invocation_options.debugger` / `Invocation::debugger`.
The same options work with Module, Session, rooted SystemContext, and shared
Runtime execution. In C, use `MPARSER_INVOCATION_OPTIONS_INIT` so the negotiated
structure size includes the debugger tail. The historical unsized initializer
initializes only the original prefix and does not enable tail fields.

`setBreakpoints` replaces the entire breakpoint set atomically. Each point
contains the exact compiled source name and a positive, one-based line number.
Unresolved points remain pending; they are not moved to a nearby line.
Comments, function declarations, and closing `end` tokens are not statements.
Several executable statements on one line produce separate column locations
and each can stop. A loop header is revisited on its backedge.

`requestPause` stops at the next executable statement, before its side effects.
Requests made while idle are retained for the next invocation. At a pause:

- `Continue` resumes until a breakpoint or pause request;
- `StepInto` stops at the next statement, including a called source function;
- `StepOver` runs deeper calls and stops at the same or a shallower stack depth;
- `StepOut` stops only after the current frame returns to a shallower depth;
- `Stop` cancels the invocation and cannot be caught by script `try/catch`.

Breakpoints and pause requests take priority over stepping depth. Stepping out
of the outermost frame simply finishes execution. There is no synthetic final
pause after the last statement. Every attached debugger disables Typed/native
regions for that invocation; production execution without a debugger keeps its
ordinary optimization behavior. Debug timings are not performance baselines.

## Frames And Values

Frames run from outermost to current and carry function/source names, source
range, frame kind, `nargin`/`nargout`, and visible workspace bindings. Callers
retain the call-site location while callees run. Anonymous captures, shared
globals, persistent and nested shared bindings, default arguments, class initializers/methods,
dynamic `eval` callbacks, and cross-module Runtime calls use the same frame path.
No internal VM operand stack or object-layout pointers are exposed.

The C event, frame info strings, and variable names are borrowed, valid only on
the callback thread while the callback is active. `mparser_debug_event_variable`
returns an owned normal SDK value; release it normally. C++ copies event/frame
metadata and owns all exported Values, so an event copy may be passed to a UI
thread. Numeric/text/value-semantic snapshots remain independently readable.
Handle objects and function handles retain their usual identity and owner
domain; this is not a deep historical snapshot of a mutable handle graph.

The interface is read-only inspection. Assigning locals, evaluating arbitrary
expressions in a selected frame, conditional breakpoints, breakpoint rebinding,
exception breakpoints, and source-level `db*` commands are not implemented.

## Threading, Errors, And Resources

The callback runs synchronously on the execution thread. A UI host may copy the
event, signal its UI, wait on its own command queue, and return a selected action.
There is no kernel-created UI thread or implicit network transport.

A debugger supports one active invocation. Concurrent attempts are rejected
with `INVALID_ARGUMENT`; it may be reused after the invocation ends. Independent
debuggers may serve independent executions. Breakpoint replacement, pause
requests, and cancellation requests are permitted from another thread using
independently retained handles/wrappers.

Do not execute or mutate a module/session/runtime from a debug callback. Such
same-thread SDK attempts are rejected. A paused invocation retains the ordinary
module/session/runtime execution locks; do not wait for another thread's call
that requires those locks. Resume the callback first. Event inspection and
debugger configuration do not release the language execution locks.

Cancellation is checked before the paused statement resumes. Wall-clock resource
budgets include time spent paused. A blocked host callback must cooperate with
its UI/cancellation channel; the kernel cannot forcibly unwind host code.
Invalid resume actions and C++ callback exceptions produce
`MParser:DebugCallbackFailed` and cancel execution. Exceptions must not escape
a raw C callback. The callback's user data must outlive active invocations.

## Validation

`runtime_debugger_smoke` compares HIR/bytecode stepping and checks loop
backedges, column locations, stop propagation, anonymous captures, live session
and nested shared bindings, callee-stop assignment/side-effect isolation,
class/default-argument frames, dynamic call ordering, cross-module
calls, and malformed source metadata. `debugger_api_smoke` exercises public
Module/Session/Runtime entry points, retained Values, threaded pause/resume,
reentry, callback failure, and cancellation. `debugger_c_api_smoke` independently
builds as C11 and checks the raw callback and caller-sized tail contract.

The development batch is not a cross-platform release claim until the native,
no-JIT, sanitizer, installed-consumer, and applicable platform CI gates pass.
