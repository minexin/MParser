# Function-Form Method Dispatch

Development work for the external-method source-graph integration gap. Product
and source API remain 1.15.0; ABI remains 2.10 (154 exports). No new public
entry point or machine-result field is introduced by this runtime change.

## Runtime Contract

An unresolved name in a call position can be resolved after its arguments are
evaluated. Bare name reads retain the existing unknown-variable behavior.
Same-named workspace values, nested functions and source-local functions keep
their precedence. A selected method receives the original arguments in their
original order, including an object appearing after a numeric argument.

For same-module class objects without explicit class-priority attributes, the
leftmost class object determines method selection; a missing method on that
class does not authorize selecting an unrelated later argument's method.
The existing method machinery enforces visibility and requested output counts.
Function-form dispatch also overrides a same-named builtin or a primary
function in a different submitted source file. No method body is executed
during lookup, and argument expressions are not retried during dispatch.

Unknown calls with no selected method preserve the original callee span and
unknown-variable diagnostic. The class-private helper regression guards against
accidental access or misleading fallback diagnostics.

MATLAB permits a function-form method to receive a deleted handle if its body
does not access invalid object state. This path does not impose an extra
pre-call lifetime rejection: property access continues to enforce handle
validity. The existing dot-call lifetime policy is unchanged by this work.

## Evidence

- Red reproduction: `build/function-method-red-test.log`, public SDK invocation
  fails with `unknown bytecode runtime variable: read` before the fix.
- Reference source: `tools/reference/function-method/`. Run MATLAB R2024b with
  that directory on the path and call `run_function_method_reference`.
- `build/function-method-matlab-r2024b-final.log` records MATLAB
  `24.2.0.2712019 (R2024b)` returning direct/second-argument values 7/11,
  multi-output 7/8, post-mutation 8, builtin overload 17, inherited value 7,
  private in-class call 31, left/right/mixed selection 1/2/1, local/variable
  precedence 91/55 and rejection of private, numeric and wrong-dominant calls.
  Deleted-handle method calls not accessing state are permitted.
- Public SDK tests exercise Automatic, Bytecode, Portable and Native requests.
  This does not assert that every method body executes as native machine code.
- The first broad run exposed a private-helper diagnostic regression; preserving
  the deferred name span fixes it. Targeted class-private, handle-lifecycle and
  SDK checks passed in `build/function-method-boundary-test.log`.
- A read-only review identified that empty object indexing could drop module
  ownership. The shared-Runtime negative first failed in
  `build/function-method-empty-red-test.log`: a foreign empty object selected
  a same-named local method. Empty indexing now preserves the owner context,
  and the selector requires matching contexts (including rejecting missing
  ownership when the current executor has an owner). The foreign-empty negative
  and same-module-empty positive pass in `build/function-method-empty-green-test.log`.
- Initial installed-SDK REST invocation and 67 host regressions passed on the
  clean host at `build/clean-host-probe/cc64e3a7a60d49b19cf65f7b27e02b14`.
  The aggregate run stopped when the first test used a stateless file request;
  the corrected test uploads through a session file API and passes in
  `build/function-method-rest-session.log`. The same host's remaining kernel
  probes and regressions passed in `build/function-method-host-kernel.log` and
  `build/function-method-host-regression.log`. This predates the final empty
  object ownership fix and must be rechecked with its selected DLL.
- Final kernel/package regression with the ownership fix passed 355/355 in
  `build/function-method-acceptance-test.log` (132.26 seconds). The installed
  C++ consumer executes both function and dot forms and checks their values.
- The same clean host binary was revalidated after selecting the final
  installed DLL; the intermediate DLL remains in the stage's
  `intermediate-kernel` directory. Expanded REST invocation passed in
  `build/function-method-rest-final.log`, remaining kernel probes in
  `build/function-method-host-final-kernel.log`, and 67/67 host regressions in
  `build/function-method-host-final-test.log`. Loaded DLL SHA256:
  `924DF5D40F9B46E2EB8C13BBF37885DBCA059DB589A04CD3778E59935C21BEE3`.
  Raw kernel evidence: `build/host-compile-probe/4344128ea08b4099aa80826f1ce97ce3`.

## Remaining Boundaries

### Cross-Request Follow-Up In Development

An added session replay reproduced `read(obj)` failing in the next command,
despite the first source-file invocation succeeding. See
`build/function-method-followup-red.log` and the public shared-Runtime negative
at `build/cross-invocation-method-red-test.log`. This is a real REPL integration
gap, not evidence against the earlier same-module checks.

The follow-up asks the object's registered owner module to
resolve only a method, not a general member. It does not run property getters
or method bodies during lookup. A successful lookup returns an unbound callable
with owner-supplied implicit-output metadata. Actual invocation retains the
original argument order and output request, and checks public access again.
Implicit-expression status is forwarded per invocation without mutating the
stored callable. No public C ABI or source-graph descriptor changes are needed.
Expanded SDK checks exercise four requested backends, private-method rejection,
same-named consumer classes, empty-object ownership, multiple/zero outputs,
implicit nargout, evalc and ordinary-function fallback. A dependent getter that
would mutate the object is not executed by lookup. Existing dot-member method
handles now also retain owner-supplied implicit-output metadata; the output
count mismatch was first reproduced in
`build/cross-invocation-method-dot-red-test.log` and corrected in
`build/cross-invocation-method-expanded-test.log`.
Current full regression, installed consumers and session REST evidence is
recorded below, separately from the earlier same-module acceptance.

Commit review identified two additional cross-request issues. Destructors
resolved as method handles must use the existing deletion lifecycle, not call
only the destructor body. The negative at
`build/cross-invocation-review-red-test.log` reproduced a still-valid object.
Routing through deletion then exposed a per-invocation listener registry:
destructors ran, but the destruction event was lost. Listener records now belong
to RuntimeSessionState, use a recursive execution lock, and survive owner VM
recreation. Reset replaces the registry and invalidates old listeners; listener
deletion checks field identity as well as the numeric ID. SDK tests cover
function/dot deletion, inherited destructors, a single destruction event,
listener invalidation, repeated deletion and reset ID reuse.

Method-lookup rejection previously attached a consumer-local source ID to the
owner source table. The negative at `build/cross-invocation-listener-test.log`
proved incorrect file attribution. Lookup refusals now return structured status
data; the consumer records the diagnostic and event at the original call span.
Tests assert `denied.m`, byte offset 4 and exactly one error event.
Targeted session/lifecycle tests passed at
`build/cross-invocation-review-green-test.log`; reset and event/property listener
tests passed at `build/cross-invocation-state-test.log`. These focused runs do
not replace the final complete regression and installed-host follow-up replay.

The shared registry also needs exception-safe callback activation. A test
invoker throws std::runtime_error on its first callback, retains the session,
then notifies again. It first failed with `callbackActive remained set` in
`build/listener-exception-red-test.log`. A noexcept RAII scope now restores the
previous value by record ID and listener-fields identity, without retaining a
map iterator across callbacks. Targeted event/SDK checks pass in
`build/listener-exception-green-test.log`.

#### Follow-Up Review Regressions

The follow-up includes these reviewed regressions and changes:

- `build/private-delete-capability-red-test.log` reproduces rejection of a
  private destructor handle created legally inside its class. The validated
  capability now reaches the complete lifecycle route, including object arrays
  (array red: `build/private-delete-array-red-test.log`). Direct external name,
  dot and forged-handle requests still reject; an exported authorised handle
  invalidates the object exactly once even with repeated elements/calls.
- `build/foreign-destruction-scope-red-test.log` reproduces an invalid-object
  error while a foreign callback reads the source during destruction. A shared
  per-session destruction scope now spans nested owner routing and uses RAII
  cleanup. Tests also reject the same source's property access after destruction.
  The positive replay passed in `build/foreign-destruction-scope-green-test.log`.
- `build/listener-weak-source-red-test.log` proves that persistent session
  records kept source objects alive via Source/Object. Those listener fields
  now carry weak internal object storage, materialized by public member getters.
  Event and property listener identity/release cases pass. The SDK validator
  initially rejected the new representation with INTERNAL_ERROR; weak storage
  is now admitted only in the two listener source fields and rejected elsewhere.
  Runtime contract, event/property listener and SDK checks pass together in
  `build/lifecycle-review-expanded-green-test.log` (4/4).
- `build/foreign-event-permission-red-test.log` confirms a same-named consumer
  class could authorise an owner's private event after registry sharing. Event
  operations now query permissions and execute through the source owner, while
  the consumer records a refusal. Public notification and legitimate owner-class
  notification remain positive controls.

- `build/listener-captured-source-red-test.log` reproduced callback closure
  retention through Session -> listener -> callback -> source. At invocation
  entry, the session now compares strong owners against the listener-retained
  graph without weakening closure capture or coupled callback retention. The
  graph traverses object fields, function receivers/captures, cell/struct/object
  containers and tabular values. External source, callback, listener and cell
  roots remain live; internal shared callback aliases do not become false roots.
  Expanded ownership tests pass in
  `build/listener-reachability-expanded-test.log`.
- A subsequent review found that a live source must also keep its coupled
  listener reachable. `build/listener-transitive-root-red-test.log` reproduces
  lost listening through an A -> B -> C callback chain with only A held outside
  the registry. Conditional source-to-listener liveness edges now propagate to
  a fixed point without increasing internal strong-owner counts. The chain
  still notifies C while A is live and releases after A is dropped. Five focused
  SDK, contract, snapshot and event/property listener tests pass in
  `build/listener-transitive-root-green-test.log`; read-only re-review found no
  further confirmed collector issue.
- `build/foreign-event-data-red-test.log` reproduced acceptance of an unrelated
  foreign handle whose class name matched EventData in the source owner's
  table. Validation now asks the data object's own registered owner whether its
  actual class derives from event.EventData. The negative and a positive
  third-module payload callback pass in `build/foreign-event-data-green-test.log`.
- `build/weak-source-snapshot-red-test.log` reproduced lost weak Source/Object
  alias identity. Internal immutable snapshot capture now materializes those
  edges before alias lookup. The frozen source retains its original fields and
  alias identity without keeping live runtime storage; expired source capture
  fails explicitly. Both snapshot/contract tests pass in
  `build/weak-source-snapshot-green-test.log`.
- `build/event-review-origin-graphics-red-test.log` reproduced two further
  errors: owned event-operation diagnostics used the wrong source table, and
  listener weak-source validation rejected a legitimate graphics handle. Direct
  operation diagnostics now carry the caller's explicit file and journal
  context before exception/journal creation. Callback-owned diagnostics keep
  their original source, including a third-module callback with capture off/on
  and catch-stack checks. Weak-source validation preserves the original handle
  type, validating live storage or an empty expired-reference prototype.
  SDK graphics listener export and immutable graphics source capture are covered.
- `build/destruction-native-exception-red-test.log` reproduced a coupled
  listener remaining valid after a native destruction callback exception.
  Lifecycle-stage exception handling now cleans destroyed-handle state before
  propagating the original exception. Tests also create a replacement registry
  inside a callback's session reset, then verify the old callback scope cannot
  alter the new listener and that the new callback still fires. All five focused
  SDK, contract, snapshot and listener tests pass in
  `build/event-review-final-green-test.log`.

#### Current Local Acceptance

- Complete rebuild and kernel/package regression passed 355/355 in 112.64
  seconds at `build/cross-invocation-complete-test.log`. Strengthened callback
  error uniqueness/disposition, destroying-scope cleanup and same-ID reset
  assertions passed in the subsequent five-test run at
  `build/event-review-strengthened-test.log`.
- Fresh source-only installed-SDK host acceptance completed all 148 build
  actions, REST probes, renderer candidate checks and 67/67 host regressions
  (22.58 seconds): `build/clean-host-probe/5881f098e00348d49173395e8711a484`.
  The expanded definition-source probe executes methods in the next session
  command, notifies a retained listener, then deletes its source and checks both
  object and listener invalidation. Source host/client directories and deployed
  services were not modified.
- Selected installed DLL SHA256:
  `89004D8A8A702854A2770AF2C838754BFFD9370F4F64590DC1E2D5745C753DBB`.
  Aggregate evidence: `build/cross-invocation-complete-clean-host.log`.

Remaining acceptance gates are not hidden by those green tests:

- The private-destructor R2024b fixture under
  `tools/reference/private-delete` was not executed: automatic approval review
  failed due its usage limit before MATLAB started. Native positive/negative
  results are not an R2024b compatibility claim for that fixture.

- GitHub CI for this follow-up must be checked against its actual commit;
  passing local tests does not establish remote CI or a published release.

This is not complete MATLAB class dispatch. `InferiorClasses` precedence,
operator syntax and arbitrary unregistered-module dispatch remain separate work.
Registered objects are routed through their original owner module. Missing
ownership or an owner not registered in the active Runtime is not authorization
to use a same-named local class. Browser and deployment gates remain open.

Neither kernel SDK checks, isolated host acceptance nor the R2024b corpus prove
a deployed host, browser navigation, graphics rendering or export.

Reference rules: MathWorks [method invocation](https://www.mathworks.com/help/matlab/matlab_oop/method-invocation.html)
and [class precedence](https://www.mathworks.com/help/matlab/matlab_oop/class-precedence.html).
The installed R2024b replay, not the current online release label, anchors the
tested examples above.
