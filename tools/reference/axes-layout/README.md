# Axes Layout R2024b Reference

```matlab
addpath('D:/project/MParser/tools/reference/axes-layout');
run_axes_layout_reference('D:/project/MParser/build/axes-layout-r2024b-checked.json', ...
    'D:/project/MParser/tests/axes_layout_reference.json');
```

The optional second argument asserts the complete JSON-normalized report before
writing it. Fifty cases record a controlled 800-by-600 Figure, six Axes units,
conversion round trips, numeric validation, plot replacement, resizing,
reparenting and native Figure sizing controls. The report includes the actual
MATLAB version, platform, screen dimensions and DPI. Character metrics are
derived from the recorded pixel/character positions, not assumed portable.

`axes_layout_smoke` supplies an explicit internal layout environment and replays
43 Axes-only sources in HIR/four requested VM modes (215 outcomes). The
seven native Figure source cases are not counted as implemented source syntax.
Separate internal graph checks exercise environment resize/reparent conversion,
missing/invalid metrics, aliasing, overflow rejection and frozen snapshots.
`axes_layout_api_smoke` separately replays 172 four-mode outcomes through the
public ABI 2.12 injection interface and checks immutable v4 environments,
legacy isolation and invalid/stale/concurrent requests. Neither layer alone
establishes browser measurement/rendering or native Figure window semantics. See
`docs/axes-layout-development.md` for the current public boundary.
