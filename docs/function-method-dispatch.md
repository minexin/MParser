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

This is not complete MATLAB class dispatch. `InferiorClasses` precedence,
operator syntax, and argument-directed dispatch to foreign-module object
owners remain separate work. A foreign object cannot be authorized using a
same-named local class: the new selector fails closed at that boundary.
Existing explicit member/callable routing remains unchanged.

Neither kernel SDK checks nor the R2024b corpus prove a deployed host, browser
navigation, graphics rendering or export. Current installed-SDK REST and full
regression runs must be checked independently before marking acceptance.

Reference rules: MathWorks [method invocation](https://www.mathworks.com/help/matlab/matlab_oop/method-invocation.html)
and [class precedence](https://www.mathworks.com/help/matlab/matlab_oop/class-precedence.html).
The installed R2024b replay, not the current online release label, anchors the
tested examples above.
