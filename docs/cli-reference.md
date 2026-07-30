# CLI Reference

This document describes CLI contract 1.0. The machine-readable authority is
[cli-contract-v1.json](cli-contract-v1.json). Human help text may be
reformatted, but the production command, stable production options,
diagnostic mode names, channel rules, and compatibility policy remain as
specified for v1.x.

## Synopsis

```text
mparser [mode] [mode-options] <file.m>
mparser --help
mparser --version
```

Select at most one execution or inspection mode and provide exactly one input
source for source-processing commands. Repeating a single-occurrence option,
combining modes, using an option outside its accepted mode, or providing
multiple input files is a usage error.

Use `--` before a source path that begins with `-`.

## Utility Commands

| Command | Contract |
| --- | --- |
| `--help` | Stable command name; text layout is not frozen |
| `--version` | Prints `MParser <semver>` |

## Production Mode

`--run` is the stable production command:

```text
mparser --run [production-options] <file.m>
```

It compiles the source graph and executes once with production bytecode
semantics. Eligible optimized regions may run according to the selected JIT
policy. The script is not first executed as a profiling baseline.

### Production Selectors

| Option | Meaning |
| --- | --- |
| `--jit={auto|off|portable|native}` | Select production optimization policy |
| `--result-format=json-v1` | Emit one versioned machine result instead of human output |

`--jit=auto` is the default and prefers native execution when available.
`--jit=off` disables typed/JIT regions. `--jit=portable` requests the portable
typed kernel. `--jit=native` requests SLJIT machine code with guarded fallback.

`--result-format=json-v1` is accepted only with `--run`. It cannot be combined
with `--native-cache-stats`.

### Stable Production Options

The following meanings are stable in CLI 1.0:

| Option | Meaning |
| --- | --- |
| `--entry-function=<name>` | Invoke a named function rather than the script entry |
| `--argument=<value>` | Append a positional or `name=value` argument; repeatable |
| `--outputs=<count>` | Request exactly this many outputs; zero is valid |
| `--path=<directory>` | Append a source-graph search path; repeatable |
| `--class-path=<directory>` | Search-path alias retained for class-folder workflows; repeatable |
| `--native-cache-entries=<count>` | Bound resident native cache entries; zero disables retention |
| `--native-cache-bytes=<count>` | Bound resident native code bytes; zero disables retention |
| `--native-cache-stats` | Print human-readable process-local cache statistics |

CLI values support numeric scalars, numeric row vectors such as `[1,2,3]`,
quoted UTF-8 string scalars, and name-value arguments such as
`Method="fast"`. Shell quoting rules still apply.

Example:

```text
mparser --run --jit=auto --entry-function=calculate \
  --argument=4 --argument=[1,2,3] --outputs=2 \
  --path=lib functions.m
```

## Diagnostic Execution Modes

Diagnostic mode names remain accepted during v1.x. Their human-readable
output may gain fields or change formatting and must not be parsed as a
machine protocol.

| Mode | Purpose |
| --- | --- |
| `--run-hir` | Execute the deliberately smaller reference HIR subset |
| `--run-bytecode` | Execute the production bytecode VM with JIT disabled |
| `--profile-bytecode` | Execute the VM and print profile data |
| `--plan-bytecode` | Print a profile-guided optimization plan |
| `--typed-ir-bytecode` | Print profile-guided typed IR |
| `--check-typed-ir-bytecode` | Evaluate typed guards against runtime values |
| `--run-jit` | Run static-JIT diagnostics and print region information |
| `--run-typed-bytecode` | Profile, rerun with typed regions, and compare results |
| `--run-adaptive-bytecode` | Run a repeated adaptive VM session |
| `--run-module-runtime` | Invoke entries in one persistent compiled module |
| `--benchmark-runtime` | Compare reference, VM, portable, and available native paths |

These modes are engineering tools. In particular,
`--run-typed-bytecode` may execute the workload more than once and is not a
replacement for `--run` when code has side effects.

The following modes accept:

```text
--typed-backend={auto|portable|native}
```

- `--run-jit`
- `--run-typed-bytecode`
- `--run-adaptive-bytecode`
- `--run-module-runtime`
- `--benchmark-runtime`

Production `--run` accepts `--jit` and rejects `--typed-backend`.

### Adaptive Options

| Option | Accepted with | Meaning |
| --- | --- | --- |
| `--adaptive-runs=<count>` | `--run-adaptive-bytecode` | Number of repeated runs; must be positive |
| `--adaptive-hot-loop=<count>` | adaptive or module runtime | Promotion threshold; must be positive |
| `--adaptive-fallback-limit=<count>` | adaptive or module runtime | Invalidation threshold; must be positive |
| `--adaptive-persist-workspace` | adaptive or module runtime | Preserve the explicit session workspace |
| `--adaptive-workspace=<name=value>` | adaptive or module runtime | Add or replace an initial workspace value; repeatable |

### Module Runtime Options

`--run-module-runtime` requires at least one repeatable:

```text
--module-call=<name>[:<value>...]
```

Each call invokes an entry in the same compiled module/runtime. `--outputs`
may select the output count. Arguments use the same narrow CLI value grammar
as `--argument`.

### Benchmark Options

| Option | Meaning |
| --- | --- |
| `--benchmark-warmup=<count>` | Warmup count; zero is valid |
| `--benchmark-iterations=<count>` | Measured iterations; must be positive |

`--benchmark-runtime` is diagnostic characterization. It is not the
versioned performance-baseline protocol and should not be used as a release
threshold without recording hardware, build, workload, and timing boundaries.

## Inspection Modes

| Mode | Purpose |
| --- | --- |
| `--tokens` | Print lossless lexer tokens |
| `--hir` | Print semantic HIR |
| `--bytecode` | Print lowered bytecode |
| `--module-info` | Print the compiled source graph and optionally validate an entry |

`--entry-function`, `--argument`, and `--outputs` are accepted by the
function-invocation diagnostic modes. `--outputs` is also accepted by
`--run-module-runtime`. Search paths are accepted by source-graph modes, not
the token-only lexer view.

Native cache limit options are accepted by execution modes. Cache statistics
are human-readable diagnostics and are deliberately excluded from JSON
machine mode.

## Human Channels And Exit Codes

For ordinary human-readable commands:

| Channel | Content |
| --- | --- |
| stdout | Results or requested inspection output |
| stderr | CLI, source, compilation, or runtime diagnostics |

| Code | Meaning |
| ---: | --- |
| 0 | success |
| 1 | source, compilation, execution, or requested diagnostic failure |
| 2 | CLI usage error |

## JSON-v1 Channels And Exit Codes

For `--run --result-format=json-v1`:

- stdout contains one `mparser.result` 1.x JSON document followed by one LF;
- stderr is empty;
- consumers inspect both the process exit code and top-level `status`.

| Code | Status class |
| ---: | --- |
| 0 | `succeeded` |
| 1 | `compilation-failed` |
| 2 | `request-rejected` |
| 3 | `runtime-failed` |
| 4 | emergency serialization or output-transport failure |

An output-transport failure can leave stdout incomplete. See
[Machine Result Protocol](machine-result-protocol.md).

## Compatibility

Production mode and option meanings are not removed or reinterpreted before
v2. Diagnostic mode names remain available in v1.x, but their text is not a
machine contract. The undocumented pre-v1 `--run-interpreter` alias was
removed; use `--run-hir`.

For the shared compatibility and deprecation rules, see
[Versioning And Deprecation](versioning-and-deprecation.md).
