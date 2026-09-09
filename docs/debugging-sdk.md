# Debugging Through The SDK

The development C ABI generation 2 revision 4 and C++ source API expose a
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

## Selected-Frame Evaluation

The v1.13 development interface adds `mparser_debug_event_evaluate` and C++
`DebugEvent::evaluate(frameIndex, source, requestedOutputCount = 1)`. Call them
on the execution thread before the pause callback returns. A C++ event copy
retains its snapshot but rejects evaluation after resume, callback failure,
or from another thread. Raw C event pointers must never outlive their callback.

The invocation must explicitly grant `DynamicEvaluation` capability through its
SystemContext. Expressions return ordinary owned result outputs; pass zero
requested outputs for assignment or other script commands. Results and captured
output can outlive the callback. Assignments affect the selected live workspace,
including nested captures and global/persistent bindings. Existing event
snapshots remain unchanged. Evaluation errors are returned in the result and
do not force resume or abort; effects executed before an error are not rolled
back. Normal execution controls still apply, and nested debug pauses are
suppressed during evaluation.

## Conditional Breakpoints

C hosts use `mparser_debugger_set_conditional_breakpoints` with an array of
`mparser_conditional_breakpoint`. C++ hosts set `Breakpoint::condition` before
calling `Debugger::setBreakpoints`. Both replace the entire configured set;
empty conditions remain unconditional. The original C setter is unchanged.

Conditions execute in the current live frame with the invocation's capabilities
and resource controls. A false numeric/logical condition skips that breakpoint,
but does not suppress pause or stepping requests. Matching conditions run in
configuration order until one is true, empty, or fails. Expression failures and
invalid truth values pause with diagnostics rather than silently skipping.
Use the C event condition-diagnostic count/accessor functions or the owned C++
`DebugEvent::conditionDiagnostics` vector. Evaluated side effects are not rolled
back, and snapshots reflect the workspace after condition evaluation.

## Source Breakpoint Commands

An invocation with an attached debugger accepts the following source forms,
including ordinary command syntax and equivalent string-argument calls:

```matlab
dbstop in demo.m at 7
dbstop('in','demo.m','at',7,'if','k==2');
s = dbstatus();
dbclear in demo.m at 7
dbclear in demo.m
dbclear all
```

Source names match the debugger's exact compilation source names. Lines are
positive integers (numeric scalars or decimal text). `dbstop` replaces entries
at the same source/line, retaining unrelated entries; with an explicit output
it returns the configured line. It does not rebind to the next executable line.
`dbstatus()` returns a struct array with `name`, `file`, `line`, and `expression`
fields, one entry per configured breakpoint in configuration order. With no
output it prints the configuration. `name` and `file` both identify the exact
source in this subset. Invalid commands leave configuration unchanged.

These commands share the host SDK's configuration and may also execute through
pause-event evaluation. They do not create a debugger or a hidden interactive
host; absent an attached debugger they report `MParser:Debugger:Unavailable`.
Explicit pause-event evaluation accepts `dbcont`, `dbquit`, `dbstep`,
`dbstep in`, and `dbstep out` (or equivalent function calls). They queue
continue, stop, step-over, step-into, or step-out for when the host callback
returns. The callback is still synchronous; the command does not unwind host
code. Host stop always wins, and a queued source stop cannot be undone by a
later continue. Other source actions use the last accepted request. Evaluation
continues normally after the command, retaining ordinary partial effects.
Outside explicit pause-event evaluation, including breakpoint conditions,
these commands report `MParser:Debugger:NotPausedEvaluation`. Missing debugger
and missing dynamic-evaluation capability retain their existing diagnostics.
`dbstack()` during explicit pause-event evaluation returns the original paused
stack as a column struct array (`file`, `name`, `line`), innermost frame first.
Temporary source-evaluation frames are excluded. With no output it prints that
stack. The query expires with the pause callback and rejects ordinary execution
and breakpoint-condition evaluation. This initial form takes no arguments and
does not provide a second selected-frame output.
The returned struct is an ordinary owned SDK result: its schema and line
values remain readable after resume or callback exception. This is separate
from the pause-event evaluation capability, which expires at callback exit.
`dbup` and `dbdown` move one frame toward the caller or current function.
Each pause starts at the innermost frame. Subsequent evaluations use the
selection when the host passes `MPARSER_DEBUG_SELECTED_FRAME` (C) or
`DebugEvent::selectedFrame` (C++). Explicit indices still address the requested
frame without changing the selection. Navigation applies to the next evaluation
request; it does not move an already executing multi-statement evaluation to
another workspace. Out-of-range navigation leaves selection unchanged. These
initial commands accept no arguments. The selection expires on resume.
Breakpoint rebinding and exception breakpoints remain unimplemented.
The existing keyboard input loop's `dbcont`/`dbquit` behavior is unchanged.

## Threading, Errors, And Resources

The callback runs synchronously on the execution thread. A UI host may copy the
event, signal its UI, wait on its own command queue, and return a selected action.
There is no kernel-created UI thread or implicit network transport.

A debugger supports one active invocation. Concurrent attempts are rejected
with `INVALID_ARGUMENT`; it may be reused after the invocation ends. Independent
debuggers may serve independent executions. Breakpoint replacement, pause
requests, and cancellation requests are permitted from another thread using
independently retained handles/wrappers.

Only the dedicated event evaluation API may execute in a paused frame. Do not
otherwise execute or mutate a module/session/runtime from a debug callback. Such
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

The native, no-JIT, sanitizer, installed-consumer, and applicable platform CI
gates passed for the original v1.11 inspection batch; see [v1.11.md](v1.11.md)
for its exact commit and evidence. The new evaluation and conditional-breakpoint
work has local native/no-JIT and installed-consumer validation; see
[v1.13.md](v1.13.md) for exact runs and remaining platform gates. Neither
batch claims completion of host UI integration.
