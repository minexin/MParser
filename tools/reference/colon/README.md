# Colon Reference

Run on MATLAB R2024b:

```matlab
addpath('D:/project/MParser/tools/reference/colon');
run_colon_reference('D:/project/MParser/build/colon-reference-r2024b-checked.json', ...
    'D:/project/MParser/tests/colon_reference_cases.json');
```

The 38 expressions come from `cases.json`. The optional second argument checks
every structured result against the frozen reference before writing output.
JSON round-trip normalization makes MATLAB's row/column struct-array orientation
irrelevant; it does not remove classes, dimensions, hex values or errors.
The reference was collected on MATLAB 24.2.0.2712019 (R2024b) and the assertion
run exited zero. NaN payload bits are recorded, but kernel regressions require
NaN classification rather than identical platform-specific NaN payloads.

The host's `0:0.05:10` observation is the primary reproduction. Additional
expressions exercise endpoint tolerance, single, descending, repeated values,
nonfinite and overflow boundaries. This is a finite graphics-data regression,
not all MATLAB colon types or a claim of renderer compatibility. Integer ranges
are not changed by this follow-up. See `docs/graphics-colon-contract.md`.
