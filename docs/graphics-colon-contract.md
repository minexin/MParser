# Floating Colon And Graphics Data

Development product 1.15.0. This fixes the host-reported `0:0.05:10` endpoint
loss in numeric range construction, not in rendering. Public C/C++ headers,
ABI 2.10, builtin inventory and graphics snapshot versions are unchanged.

## Construction

Floating ranges plan their length once. A candidate endpoint within the
scale-relative rounding tolerance is snapped to the requested stop; a genuinely
nondividing stop does not add another sample. Elements are generated from the
nearer endpoint with a symmetric midpoint, not by repeatedly adding the step.
Single operands select single-precision planning and output; element arithmetic
uses the stored rounded endpoints/step before conversion to the output class.
An increment below the spacing at a large offset can produce repeated values,
without prematurely ending the sequence.

The same plan/element functions drive HIR/bytecode materialization, Portable
nested loops and SLJIT Native nested loops. Native loops retain an index and
numeric plan in per-loop stack storage; they do not allocate a range array.
Floating zero-step or wrong-direction ranges are empty 1-by-0 rows. NaN and
infinite inputs follow the measured reference cases, with impossible array
lengths rejected before allocation. Existing exact integer range construction
is unchanged, including its zero-step rejection; this is not complete colon
compatibility for every input class or extreme allocation.

`plot` receives the actual computed array. Neither the graph serializer nor the
host appends a missing endpoint, alters XData, or substitutes `linspace`.

## Reference And Verification

The source observation is `MExecServer/docs/mparser-kernel-iteration-notes.md`,
2026-10-29 title/graphics section. The pre-fix CLI reproduction in
`build/colon-host-red.json` has 200 XData values ending at
9.950000000000006. MATLAB R2024b and the rebuilt CLI both have 201 values ending
at exactly 10; the latter is retained at `build/colon-host-green.json`.

`tools/reference/colon/cases.json` supplies 38 inputs. MATLAB
24.2.0.2712019 (R2024b) produced `tests/colon_reference_cases.json`, including
class, dimensions, every value's hexadecimal representation and error IDs.
The assertion-enabled replay at `build/colon-reference-r2024b-checked.log`
exited successfully and matched the frozen structured reference. Cases include
ascending/descending ranges, nondividing stops, near-endpoint rounding, single
and mixed single inputs, large offsets/repeated samples, tiny/subnormal scales,
overflowing spans and nonfinite controls.

`colon_range_smoke` checks the 38 cases through HIR and four requested SDK
backend modes (190 checks), with exact finite-value bits and NaN classification.
It also checks four real kernel graphics snapshots. Requested backend mode is
not proof of optimized execution. Separately, `typed_region_executor_smoke`
requires actual Portable/Native nested execution, zero fallback and correct
iteration/last-value results for ascending, descending, repeated-value and
zero-step ranges. The final rebuilt kernel/package regression passed 356/356 in
92.32 seconds at `build/colon-final-full-test.log`.

`tools/host-integration/test-graphics-colon.cjs` compares complete REST XData
arrays against the reference, then reads the retained graphics handle in a
later request. It also covers source-file execution and nested-loop results.
Final clean source-only installed-SDK host acceptance passed all 148 build
actions, dedicated REST/client probes and 67/67 regressions in 19.88 seconds.
Stage: `build/clean-host-probe/d8deab22f170429482bc2a2218a77f3c`;
aggregate log: `build/colon-final-clean-host.log`. The selected and loaded
DLL SHA256 is
`47AD32B94966A59BB2DC52B81BC4AD402FD588DC71E20E526ED6F9156CB926C9`.
Raw general kernel probes are under
`build/host-compile-probe/387095bee1d240e5854f39a8d66ba857`, with the new numeric
responses in the stage's `colon-graphics-probe` directory. The original host
and client sources and production service were not changed.

Actual-commit GitHub CI remains a separate gate. These checks do not prove
browser pixels, production adoption or a published release.
