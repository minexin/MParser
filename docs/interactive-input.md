# Interactive Script Input

The v1.12 development implementation supplies `input` and `keyboard` through
an invocation-owned input source. The host supplies lines; the runtime evaluates
them in the workspace where the builtin was called.

## Script Behavior

`input(prompt)` evaluates a supplied line and returns its result. An empty or
whitespace-only line returns an empty matrix. Evaluation errors are sent to the
output sink and the runtime requests another line. `input(prompt,'s')` returns
the supplied UTF-8 text as a character vector, preserving whitespace without
evaluating it. The prompt accepts `\n` and `\\` escapes.

`keyboard` requests commands with the `K>> ` prompt. Commands can inspect and
change the current workspace. `dbcont` or `return` resumes the interrupted
script; `dbquit` cancels execution. Empty commands request another line.
Expression and command evaluation require the SystemContext dynamic-evaluation
capability. A capability denial is returned immediately rather than retried.

End of input produces `MParser:EndOfInput`; an unavailable or failed source
produces `MParser:InputFailed`. End of input does not implicitly resume a
`keyboard` session.

## CLI

Human-readable execution modes use standard input and display prompts and
runtime output as execution proceeds. Try
`mparser --run samples/interactive_input_demo.m` and enter `7`, `demo`, then
`dbcont`; the sample produces `summary = 32`. Redirected line input works too.

Machine output mode (`--result-format=json-v1`) does not attach the console
input source. An interactive request fails with a structured runtime diagnostic
instead of reading standard input or mixing prompts into the JSON result.

## C And C++ Hosts

Set `mparser_invocation_options.input_source` and `input_user_data`, initializing
the options with `mparser_invocation_options_init_sized(&options, sizeof(options),
MPARSER_C_ABI_GENERATION)`. The older unsized initializer retains the minimum
structure size and does not enable these tail fields. These are optional sized-structure tail
fields: callers whose `struct_size` excludes them do not supply an input source.
The callback receives an expression, text, or command mode and a borrowed
UTF-8 prompt. It returns one of:

| Status | Meaning |
| --- | --- |
| `MPARSER_INPUT_READY` | A complete line, without its line terminator |
| `MPARSER_INPUT_PENDING` | No line yet; poll again after checking execution control |
| `MPARSER_INPUT_END` | The source has ended |
| `MPARSER_INPUT_ERROR` | The host failed to obtain a line |

Return promptly from the callback. A blocking callback prevents the execution
thread from checking cancellation or deadlines until it returns. Cancellation
is checked again after the callback and takes precedence over a returned line.
The runtime waits briefly between pending polls.

The prompt is borrowed only for the callback duration. Returned text and error
buffers must stay valid until the next callback or execution completion,
whichever occurs first; the runtime copies the selected buffer. Text must be
valid UTF-8. Keep callback state alive for the entire invocation. Do not throw
exceptions across the C callback boundary or reenter the active invocation.

The C++ facade exposes `Invocation::inputSource`, taking an `InputRequest` and
returning an owning `InputResult`. A host can return
`{InputStatus::Ready, "7", {}}`, or `{InputStatus::Pending, {}, {}}` while waiting
for its UI. Callback exceptions are captured by the bridge and rethrown after
the C call returns. Prompts arrive through the input callback; execution output
continues through the invocation's existing output sink.

[cpp_input_demo.cpp](../samples/cpp_input_demo.cpp) is a runnable session host
that handles pending input, evaluates a local expression, and updates the
function workspace through keyboard commands. It returns 42 and is built as
`mparser_cpp_input_demo`; the source is included in installed SDK examples.

## Validation Status

Focused coverage lives in `runtime_input_smoke`, `input_c_api_smoke`,
`cpp_api_smoke`, and `interactive_input_cli_smoke`. Milestone closure still
requires full native/no-JIT suites, installed consumers, the external catalog,
and applicable platform CI as specified by the v1.x roadmap.

On 2026-09-08, the Windows MSVC Release no-JIT build completed in
`build/v1.12-input-nojit-fresh`. The five focused input, CLI, C/C++, and C ABI
layout tests passed, including function-handle invocation with local expression
input and keyboard assignment while preserving the outer workspace. The full
suite passed 319/321 tests, including installed C and C++ consumers. The two
failures were `builtin_registry_smoke` and `builtin_catalog_snapshot_smoke`:
the active registry includes the interaction descriptors while its count and
source-contract snapshot still describe 1.17. The next catalog revision must
record the reviewed descriptor changes without modifying historical snapshots.
This result does not close the v1.12 milestone.

Subsequent validation on 2026-09-08 supersedes those failures: the active
1.18 catalog contains 331 descriptors and 333 registered names. The full
Windows MSVC Release no-JIT suite passed 322/322 tests. The fresh native
Release suite passed 339/340 initially; its sole failure was the release
archive verifier still requiring the historical 1.17 active catalog. Updating
the active version, counts, and frozen SHA-256 and adding 1.18 to the archive
inventory made the release archive retest pass (1/1). All 340 native tests
have therefore passed across the full run and focused archive retest.
Both configurations passed installed and relocated C/C++ consumers.
Historical snapshot hashes remain unchanged. External catalog revalidation
and applicable platform CI remain pending.
