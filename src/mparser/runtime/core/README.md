# Runtime Core Layout

Runtime core owns value and execution semantics shared by the interpreter,
bytecode VM, Typed/JIT paths, builtins, and embedding layer. It must remain
independent of builtin registration and engine-specific execution policy.

| Directory | Ownership |
| --- | --- |
| `value` | `RuntimeValue`, shapes, numeric classes and dense algorithms, text, Cell/Struct containers, ranges, and generic array reshape |
| `indexing` | Index normalization, indexed mutation, deletion, and lvalue copyback |
| `object_model` | Object-array behavior, metadata/reflection values, and argument/property validation |
| `session` | Call frames, session state, execution limits, exceptions, warnings, output events, and formatting |

Dependencies flow from execution engines and builtin families into runtime
core. Core files must not include `mparser/runtime/builtins/*`. Shared MATLAB-
like value semantics belong here even when first needed by one builtin. For
example, generic reshape is value behavior, while dense matrix division uses
the repository-owned `value/runtime_native_numeric.*` backend; builtin files
only adapt those operations to registry call contracts.

`runtime_value.h` and `runtime_output.h` at this directory root are deliberate
facades used by the frozen BuiltinRegistry source contract. They forward to
their owning headers and must not accumulate declarations or implementation.
All other new `.cpp`/`.h` pairs belong in one of the four ownership
directories.
