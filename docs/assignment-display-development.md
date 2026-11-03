# Assignment Display Development

Status: internal recording foundation only. H10 is not complete. The public
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
  deliberately does not expose a live mutable Value/handle as a value snapshot.
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
This checks compatibility, not public assignment-display delivery. The six-case
host display probe still reports the original missing assignment events at
build/clean-host-probe/62fd9467618742b4acc17826f1031833/build/display-probe-3686f84eff124e51a525a0efeebb5bb1,
as expected before a public SDK/event transport exists. That is an open gate,
not a passing feature test. Original sibling sources/services remain unchanged.

## Remaining H10 Gates

1. Immutable structured execution-time value snapshots, including mutable
   object/graphics/cyclic-reference families, without exporting writable history.
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
