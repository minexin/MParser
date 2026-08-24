# JIT And Fallback

MParser treats optimization as an additive execution tier. The production
bytecode VM remains the semantic authority for every legal program in the
target subset. A region that cannot be optimized must execute correctly in a
less specialized tier.

## Production Policy

The stable production command accepts:

```text
--jit={auto|off|portable|native}
```

| Policy | Behavior |
| --- | --- |
| `auto` | Prefer native SLJIT for eligible regions, then guarded fallback |
| `off` | Execute through bytecode without typed/JIT regions |
| `portable` | Request the C++ portable typed kernel |
| `native` | Request SLJIT machine code for eligible regions |

`auto` is the default. The policy changes optimization selection, not MATLAB-
like language semantics. Native availability depends on build configuration
and target architecture.

The diagnostic `--typed-backend={auto|portable|native}` selector belongs to
`--run-jit`, typed/adaptive/module runtime, and benchmark modes. It is not
accepted by production `--run`.

## Execution Pipeline

```text
source graph
  -> Lexer / Parser / Semantic HIR
  -> verified bytecode
  -> profile and region discovery
  -> typed guards
  -> portable or native execution
  -> bytecode fallback when required
```

Internal bytecode is verified before VM, optimization, or typed execution.
Typed regions describe supported operations and assumptions. Runtime guards
check value kinds, shapes, ranges, and other region-specific invariants before
specialized code commits results.

## Transactional Fallback

Optimized regions follow a guarded transaction:

1. validate the region and runtime assumptions;
2. execute against staged state;
3. commit outputs only after successful completion;
4. discard staged effects and continue in the bytecode VM when a guard or
   supported optimization operation fails.

This prevents partial optimized writes from becoming visible before fallback.
Fallback is an expected control path, not a script error. It may occur because
of a value/shape mismatch, an unsupported call or mutation, an invalidated
adaptive assumption, unavailable native code, or active resource controls.

Fallback cannot make language behavior outside the compatibility matrix
valid. Unsupported source semantics still produce a normal diagnostic.

## Current Coverage

The v1.0 release includes guarded portable regions and bundled SLJIT native
kernels for selected numeric scalar loops, structured branches, nested loops,
and dense-double linear-array operations. It also contains adaptive promotion,
invalidation, repeated module sessions, and a bounded process-local native
cache. The active v1.4 development train additionally specializes proven pure
single-input/single-output top-level local scalar functions called from those
loops. Their private locals are inlined as kernel SSA operands; unsupported
signatures, argument contracts, free variables, side effects, types, or domain
results remain in or return transactionally to bytecode. Exact semantic symbol
identity prevents nested-function shadowing from selecting a same-named local
target. When a call-depth limit is active, regions containing specialized
calls remain in the VM so the resource contract observes each logical call.
The second v1.4 batch adds closed-assignment Dense Typed regions for floating
`double`/`single` scalars and arrays, including complex values. Portable
execution fuses supported element-wise arithmetic and pure unary builtin calls
into one N-dimensional pass with MATLAB column-major implicit expansion,
preserving numeric class and the imaginary channel. Exact-shape real-double
expressions may use the existing SLJIT scalar kernel. Builtin source contract
1.11 also declares `sum` as a Typed reduction: portable supports default,
explicit-dimension, and all-element results for the supported floating and
complex classes, while native lowering remains restricted to real double and
scalar results. Broadcast/native mismatch, domain transitions, changed
shapes, shadowing, unsupported operations, and active checkpoints retain
transactional portable or VM fallback.

Coverage is intentionally conservative. Objects, Cells, Struct values,
dynamic operations, arbitrary calls, unsupported mutations, and unrecognized
control flow commonly remain in the VM. The
[v1.0 JIT scope decision](v1.0-jit-scope-decision.md) found no measured v1.0
release workload that required another specialization. v1.4 re-evaluates that
historical decision with the versioned seven-workload suite described in
[v1.4.md](v1.4.md). New coverage still requires a representative bottleneck,
native/no-JIT equivalence, and unchanged fallback parity.

## Native Backend

SLJIT is pinned and vendored under `third_party/sljit`; normal builds do not
download it. Configure:

```text
-DMPARSER_ENABLE_NATIVE_JIT=ON
```

to build native code generation, or:

```text
-DMPARSER_ENABLE_NATIVE_JIT=OFF
```

to omit it. A no-JIT build still contains `--run`, bytecode execution, and the
portable typed kernel. "No native JIT" means no machine-code generation; it
does not mean the interpreter or optimizer framework was removed.

Native kernels are currently release-targeted on x64 and AArch64. Cross-build
and emulator runs establish functional behavior, while publishable
performance claims require native hardware.

## Adaptive Execution

Adaptive execution observes repeated bytecode loops, promotes eligible hot
regions, and invalidates regions after bounded fallback behavior. Persistent
module runtime and sessions avoid reparsing/recompiling each invocation and
can preserve an explicitly requested workspace.

The production one-shot `--run` does not run a hidden baseline execution.
Diagnostic modes such as `--run-typed-bytecode` may execute twice to compare
results, and adaptive modes may execute repeatedly. Do not use those modes as
an editor Run command for scripts with side effects.

## Native Cache

The native cache is bounded and process-local. Configure its resident limits:

```text
--native-cache-entries=<count>
--native-cache-bytes=<count>
```

A zero entry or byte limit permits compile-and-execute but retains no native
code. `--native-cache-stats` prints human-readable activity and lifecycle
data. It cannot be combined with JSON machine mode.

Cache keys include the code and contract inputs needed to avoid reusing a
kernel for incompatible source, architecture, ABI, compiler options, or
runtime assumptions. Runtime values and raw pointers are not persistent cache
identity.

MParser v1.0 does not promise a disk cache. A future persistent cache must add
atomic storage, corruption recovery, bounds, and complete source,
architecture, ABI, compiler/options, and engine-version invalidation rules.

## Resources And Cancellation

Embedding requests with active instruction, wall-time, call-depth,
array-payload, diagnostic, or cancellation controls may suppress optimized
execution when the optimized tier cannot preserve the requested accounting.
The execution summary reports requested/effective tier, fallback,
optimization suppression, counters, and stop reason.

Correct resource enforcement takes precedence over optimization. Queue wait
for a module/session graph lock is outside the runtime wall-time budget;
accounting begins after admission.

## Measuring Performance

Separate these boundaries:

- source parse and semantic compilation;
- fresh-process startup;
- bytecode execution;
- portable typed execution;
- first native execution including compilation;
- warm native execution and cache hits.

`tic`/`toc` measures the language-visible interval, not necessarily shell
startup, source loading, compilation, output formatting, or process teardown.
The host can therefore appear slower than the value printed by the script.

The engineering collector and schema in
[v1.0 Performance Baseline](v1.0-performance-baseline.md) record raw samples,
hardware, OS, compiler, build type, backend, source/binary hashes,
allocations, peak RSS, binary size, and cache transitions. The project does
not use one machine's timings as universal pass/fail thresholds.

## Diagnosing Tier Selection

Use the production interface for real work:

```text
mparser --run --jit=auto script.m
```

Use diagnostic modes only when investigating execution:

```text
mparser --run-bytecode script.m
mparser --run-jit --typed-backend=native script.m
mparser --run-typed-bytecode --typed-backend=portable script.m
mparser --run-adaptive-bytecode --typed-backend=auto script.m
```

For stable automation, request `--result-format=json-v1` and inspect the
versioned execution summary rather than parsing diagnostic text.
