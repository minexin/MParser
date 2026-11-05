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

Creation/subset and array-lifecycle reference (80 cases):

```matlab
run_legend_creation_reference('D:/project/MParser/build/legend-creation-checked.json', ...
    'D:/project/MParser/tests/legend_creation_reference.json');
```

This separately records labels, ordered/duplicate/cross-Axes members, actual
DisplayNames, current/associated Axes, warning/error identity, name-value
forms, empty inputs and delete/isvalid array shapes. The test uses PlotChildren
only as MATLAB-side reference inspection; the runtime does not expose that
undocumented property as a new public interface. Error identifiers and all
legacy ambiguity warnings are retained as evidence, not all matched by MParser.
