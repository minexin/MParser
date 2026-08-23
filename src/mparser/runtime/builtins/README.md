# Builtin Source Layout

`builtin_registry.cpp` and `builtin_registry.h` remain in this directory as the
single catalog and dispatch entry point. Implementations are grouped by their
primary MATLAB-like responsibility:

| Directory | Ownership |
| --- | --- |
| `numeric` | Scalar and array math, reductions, scans, and advanced numeric algorithms |
| `array` | Shape-aware array helpers, collection operations, and set operations |
| `text` | Character, string, and text-query operations |
| `conversion` | Conversions that cross numeric, text, Cell, or array families |
| `callback` | Higher-order functions that invoke user or builtin callables |
| `system` | Workspace/context, filesystem/process, and MAT-file operations |

Keep descriptor registration in `builtin_registry.cpp` and executable behavior
in the owning family. Family code may use shared runtime/core helpers; direct
family-to-family dependencies should be limited to reusable operations rather
than registration side effects. New portable math implementations belong in
`numeric` and remain repository-owned C++20 without an Eigen dependency.
