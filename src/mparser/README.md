# Internal Source Layout

`src/mparser` contains MParser implementation details. Installed SDK headers
remain under `include/mparser`; paths in this tree are not public API or ABI.

| Directory | Ownership |
| --- | --- |
| `frontend` | Source loading, tokens, lexer, parser, syntax tree, and diagnostics |
| `semantic` | Semantic HIR, signatures, argument contracts, and property specifications |
| `cli` | Command-line entry point, execution mode selection, diagnostics, and result presentation |
| `execution` | Reference interpreter and execution behavior shared across engines |
| `execution/bytecode` | Bytecode IR, verifier, and region metadata |
| `execution/bytecode/vm` | Bytecode dispatch, call frames, adaptive execution, and VM-specific runtime integration |
| `execution/jit` | Typed IR, optimization planning, portable kernels, and native lowering |
| `runtime/core` | Stable internal runtime facades and ownership rules |
| `runtime/core/value` | Runtime values, shapes, text, containers, dense/CSC sparse numeric storage, categorical and shared table/timetable storage, and repository-owned numeric algorithms |
| `runtime/core/indexing` | Index planning, indexed assignment, and lvalue copyback |
| `runtime/core/object_model` | Object arrays, metadata, and argument/property validation |
| `runtime/core/session` | Call frames, session state, execution control, diagnostics, warnings, and output formatting |
| `runtime/builtins` | Builtin registry and family composition entry point |
| `runtime/builtins/numeric` | Native C++ numeric, reduction, and scan builtins |
| `runtime/builtins/array` | Array helpers, collections, and set-family builtins |
| `runtime/builtins/text` | Text construction, transformation, and query builtins |
| `runtime/builtins/conversion` | Cross-family value and text conversion builtins |
| `runtime/builtins/callback` | Higher-order callback builtins such as `arrayfun` |
| `runtime/builtins/datetime` | Native C++ datetime, duration, NaT, component, and temporal-unit builtins |
| `runtime/builtins/sparse` | CSC sparse construction, inspection, pattern operations, and dense conversion |
| `runtime/builtins/categorical` | Categorical construction, category management, predicates, and conversions |
| `runtime/builtins/table` | Table construction, metadata, conversion, and tabular query builtins |
| `runtime/builtins/timetable` | Timetable construction, RowTimes, and table/timetable conversion builtins |
| `runtime/builtins/system` | Context, filesystem, process, and MAT-file builtins |
| `runtime/io` | Filesystem, file, MAT-file, and operating-system integration |
| `embedding` | Compiled modules, C ABI implementation, machine protocol, and module execution |

Keep a `.cpp`/`.h` pair in the same owning directory. The only root core
headers are the registry-facing `runtime_value.h` and `runtime_output.h`
facades; their definitions remain in the owning subdirectories. New code
should have one clear owner instead of adding files directly under
`src/mparser`. Parent-level bytecode VM headers are forwarding headers for
path continuity; new VM code should include `execution/bytecode/vm` headers.
Include internal headers from the `src` include root, for
example `mparser/runtime/core/value/runtime_value.h`; do not depend on
relative `../` paths. Core must not include builtin-family headers. Detailed
runtime and builtin dependency rules are documented in `runtime/core/README.md`
and `runtime/builtins/README.md`.

## Incremental Decomposition

Directory ownership is the primary organization rule; files should not be
moved only because a line-count threshold was crossed. When a large
translation unit has a stable responsibility boundary, split it by behavior
while keeping its public facade and include path stable. The preferred order
is system builtin services, C API implementation sections, builtin registry
catalog helpers, and then VM helper groups. The VM dispatch loop and the
registry entry point should remain easy to locate rather than being scattered
across generic `common` or `utils` directories.

Every physical split must update the CMake source list, this ownership map,
the source-layout validator, and the focused plus full regression suites in
the same change. Runtime semantics, builtin registration, and public SDK
headers must not be duplicated as a side effect of source organization.
