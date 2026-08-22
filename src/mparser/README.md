# Internal Source Layout

`src/mparser` contains MParser implementation details. Installed SDK headers
remain under `include/mparser`; paths in this tree are not public API or ABI.

| Directory | Ownership |
| --- | --- |
| `frontend` | Source loading, tokens, lexer, parser, syntax tree, and diagnostics |
| `semantic` | Semantic HIR, signatures, argument contracts, and property specifications |
| `execution` | Reference interpreter and execution behavior shared across engines |
| `execution/bytecode` | Bytecode IR, verifier, VM, regions, and adaptive bytecode execution |
| `execution/jit` | Typed IR, optimization planning, portable kernels, and native lowering |
| `runtime/core` | Runtime values, shapes, indexing, assignment, objects, text, and session state |
| `runtime/builtins` | Builtin registry plus builtin-family implementations |
| `runtime/io` | Filesystem, file, MAT-file, and operating-system integration |
| `embedding` | Compiled modules, C ABI implementation, machine protocol, and module execution |

Keep a `.cpp`/`.h` pair in the same owning directory. New code should have one
clear owner instead of adding files directly under `src/mparser`. Include
internal headers from the `src` include root, for example
`mparser/runtime/core/runtime_value.h`; do not depend on relative `../` paths.
