# Assignment Display Development

Status: internal recording and immutable-value foundation only. H10 is not complete. The public
1.15.0 development SDK remains ABI 2.8; no assignment-display export, protocol
field, host feature or deployed UI has been advertised by this change.

## Evidence

The selected ABI 2.8 host still reproduced all six original display failures
before implementation: ordinary/repeated/comment-semicolon assignments had no
events, interleaving retained only disp, and a partial failure lost assignment
history. Raw responses, health and selected binary hashes are retained at
build/clean-host-probe/9c2059d6af5946f2aa1931cb27dcd001/build/display-probe-3a3e55ff445842738cc62b12c258faea.
The diagnostic tool's exit status alone is not a compatibility pass.

tools/host-integration/collect_display_reference.m captures actual MATLAB
R2024b console output using evalc, including repeated writes, real suppression,
indexed/field/cell roots, multi-output/ignored/repeated targets, loops,
function bodies, bare names, warnings and pre-failure output. The reference
run used 24.2.0.2712019 (R2024b), 17 cases, at
build/display-matlab-38ebd9e7d194465ea47a48bac6a24d1f/reference.json; SHA256:
CF920FBC3A2D38A6A19830B96C1359AA16A46F63B3D26483FF101AEDDA77DE54.
An earlier run used a filename inconsistent with its MATLAB function name and
failed before collection; it is not acceptance evidence.

The documented [semicolon rule](https://www.mathworks.com/help/matlab/ref/semicolon.html)
and the actual reference agree that only a statement separator suppresses the
display. A semicolon inside a comment is not a separator. The reference also
shows that indexed/field/cell writes display the updated root variable and
that [a,a]=pair() displays its final a twice, not two RHS values.

## Implemented Foundation

- Parser and HIR preserve assignment capture eligibility and the actual
  separator's suppression bit. Control headers and implicit function/catch
  bindings do not invent assignment displays.
- Internal InterpreterOptions/BytecodeVmOptions.captureAssignmentDisplays is
  opt-in and defaults false. Recording produces RuntimeAssignmentDisplay
  records only after successful assignment commit.
- Each record owns frozen formatted text, the actual root label, source name,
  byte-based source span, suppression, sequence and line-spacing state. It
  also owns an immutable structured value snapshot or an explicit snapshot
  failure. It never exposes a live mutable Value/handle as historical data.
- The recorder reads the committed root directly. It does not evaluate an
  index, dynamic field name or property getter again, parse source text, run a
  query script, or reconstruct earlier values from the final workspace.
- Within one executor, assignment, existing expression and text records share
  the execution sequence. Suppressed records are retained and flagged; they
  are not rendered or sent to a public output sink by this foundation.
- Recording suppresses optimized regions that cannot report each commit, and
  explicitly reports optimizedExecutionSuppressed. Requested Native/Portable/
  Automatic coverage must not be described as native-code display execution.
- Multi-output stores now commit left to right. The VM reverses the returned
  stack segment once, then commits the original LHS order. This also corrects
  the final value of repeated targets when recording is disabled.
- The HIR interpreter now stops on uncaught errors, does not commit failed RHS
  values and does not continue later targets after a failed output-list write.
  Caught failures retain earlier records and resume in the catch arm.

## Immutable Structured Values

Internal RuntimeValueSnapshot owns a const node graph containing only copied
payloads and snapshot-local ID edges. No RuntimeValue, executable callable,
module context, live shared property storage or graphics handle is retained.
Reading a node cannot write back into history or the execution runtime.
Copies retain the immutable data independently of its originating event.

Dense numeric snapshots use logical column-major order and preserve numeric
class, shape, complex components and exact integer bits; binary64 signed zero,
infinity and NaN payloads are not converted to strings or rounded. Character
code units, string missingness, nested cells/structs and field order are copied.
Sparse CSC payloads stay sparse, without expanding their logical zero elements.
Categorical dictionaries/codes/flags, temporal payloads and table/timetable
variables, row axes and stored properties are copied as structured data.

User objects capture stored fields under their internal storage names, not
computed/dependent properties. No getter, listener or user display method is
invoked. Aliases and cycles in shared object fields, callable captures and
tabular UserData become local edges rather than recursive live handles.
Callable snapshots are descriptive and non-invocable; they retain neither HIR
body pointers nor module lifetime anchors. Dynamic-property weak owners record
their class and liveness at capture without strengthening owner lifetime.
Graphics nodes contain a serializer-produced immutable v3 graph/reference at
capture, which remains readable after later mutation, close or destruction.
These IDs are historical references, not authorized write targets.

Capture requires a stable source value, as held by the executor/session action;
it is not an unsynchronized snapshot of concurrently modified raw RuntimeValue
storage or a transaction spanning unrelated graphics graphs. Graph serialization
uses the existing serializer lock. Public projection of private storage names,
captured closure variables or class internals requires its own access/redaction
contract; this internal representation is not exposed directly to a host.

Default limits are depth 128, 100000 nodes and a 64 MiB retained payload budget
(node records, strings, IDs and owned arrays). The byte budget is accounting,
not a claim to cap allocator overhead or temporary serializer allocations.
Invalid data and exceeded limits have distinct typed failures. No partial node
graph is returned. The assignment event retains label/source/suppression/order
and the explicit failure, with unavailable value/text; it does not format an
unbounded rejected value or substitute an empty/final workspace value. Allocation
failure still propagates through the existing execution/API failure boundary.

tests/value_snapshot_smoke.cpp checks the value families, exact bits, large
sparse inputs, alias/cycle edges, owner/code/graphics lifetimes, mutation after
capture, deleted graphics and bounded failures. Assignment tests additionally
exercise actual user-object mutation and plot/mutate/close through four
requested VM modes. The HIR user-constructor fixture is currently unsupported;
its explicit failure/no-invented-commit behavior is checked, not counted as HIR
object execution parity. These are internal tests, not SDK or browser acceptance.

tests/assignment_display_smoke.cpp exercises these paths using the HIR
interpreter and four requested VM backends, with frozen text/history, root
mutation, mixed output sequence, repeated/ignored outputs, side-effecting
indices, loops/functions, uncaught/caught errors, format state, CRLF/UTF-8 and
verifier guards. Formatting assertions use the kernel formatter; they are not
claims of byte-for-byte MATLAB console typography or custom display parity.

The rebuilt kernel/package regression passed 350/350 at
build/display-full-regression.log. A source-only clean host run using the
relocated ABI 2.8 SDK passed all existing REST/candidate probes and host CTest
67/67 (21.16 seconds) at
build/clean-host-probe/62fd9467618742b4acc17826f1031833. Kernel REST evidence:
build/host-compile-probe/ba5919870b8e40438e659fe1ff4cc4aa. Selected DLL SHA256:
11ECDA1CCD88C4EC00898F36E658A7969458227CA24C2BCD8C3FF0FF00BF2329.
That foundation run checks compatibility, not public assignment-display delivery. The six-case
host display probe still reports the original missing assignment events at
build/clean-host-probe/62fd9467618742b4acc17826f1031833/build/display-probe-3686f84eff124e51a525a0efeebb5bb1,
as expected before a public SDK/event transport exists. That is an open gate,
not a passing feature test. Original sibling sources/services remain unchanged.

The subsequent structured-value implementation was rebuilt and passed 351/351
kernel/package tests in 82.33 seconds at build/snapshot-full-regression.log,
including the extended assignment tests and new value_snapshot_smoke. The
selected relocated DLL SHA256 is
524AA94C7F8390448BEF9C660B09906AF8EE2D23931E58BFD623C421696E9AE5.
This does not prove any new public event transport or actual native-code display
execution; the recording mode explicitly suppresses unobservable optimized regions.
An additional source-only clean installed-SDK host run at
build/clean-host-probe/4a21a67db7b94722a0a7d8281536221b passed all existing
REST/candidate checks and 67/67 host tests in 20.74 seconds with
warnings-as-errors enabled. Raw kernel REST evidence is
build/host-compile-probe/6c622813822042888778e6f95e5765fd. These checks preserve
the public compatibility boundary; they do not advertise a new H10 host feature.

## Remaining H10 Gates

1. Public immutable snapshot projection/lifetime and access rules, including
   private/dependent-property policy and explicit unavailable-value handling.
   The internal structure exists; its public SDK/protocol delivery is not done.
2. A single shared execution journal across nested/dynamic/escaped-module calls;
   warning/error/diagnostic ordering and caught-error policy; live delivery and
   rejection/cancellation behavior. Executor-local counters are not that proof.
3. Real expression labels and ans rules, function-scope expressions and
   overloaded display semantics. Existing top-level-expression fields are not
   silently redefined by this foundation.
4. Additive C/C++ accessors and machine protocol contract with old headers and
   frozen schema compatibility, explicit lifetime/error/version rules and package
   consumer tests. No public ABI revision is allocated prematurely.
5. Opt-in host transport and ordered consumption of actual kernel events,
   using no source regex or final-workspace inference; clean host regression,
   real browser acceptance and release/deployment checks.

The full host-integration goal and all other open H03/H05-H08 gates remain in
docs/host-integration-goal-20261028.md.
