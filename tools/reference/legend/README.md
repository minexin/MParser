# Legend R2024b Reference

```matlab
addpath('D:/project/MParser/tools/reference/legend');
run_legend_reference('D:/project/MParser/build/legend-r2024b-checked.json', ...
    'D:/project/MParser/tests/legend_reference.json');
```

The optional second argument asserts the complete JSON-normalized report before
writing it. Fourteen states include auto-update off/on, name mutation, deletion,
non-retroactive inclusion, custom/short/excess/empty labels and mixed names.
The hidden MATLAB Figure is closed on success or error; no MParser rendering
is involved. See docs/graphics-legend-contract.md for scope and evidence.
