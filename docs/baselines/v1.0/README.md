# MParser v1.0 Baseline Evidence

This directory contains source-tree performance and resource evidence for the
v1.0 candidate. Reports are immutable observations tied to an implementation
revision and exact source/binary SHA-256 values. They are not cross-machine
performance promises and are not installed in release SDKs.

## Windows x86-64 Candidate

The first local evidence set measures implementation commit
`da4b010791590a235dbcbf3be5a8dc853b790963`.

| Field | Value |
| --- | --- |
| OS | Windows 10.0.19045 |
| Architecture | x86_64, native execution |
| CPU label | Intel64 Family 6 Model 140 Stepping 1, GenuineIntel |
| Logical CPUs | 8 |
| Physical memory | 15.80 GiB |
| Compiler | MSVC 19.44.35228.0 |
| Build | Release |

All timing columns below are host-wall medians in milliseconds. Exact raw
samples, engine timings, allocation samples, cache counters, boundaries,
hashes, and results remain in each JSON report.

| Report | Parse | Compile | Process cold | Bytecode | Portable | Native cold | Native warm | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| [native scalar loop](windows-x86_64-msvc-release/native-scalar-loop-v1.json) | 0.034 | 0.165 | 1978.449 | 2342.359 | 11.533 | 3.557 | 2.571 | 5.45 MiB |
| [native linear array](windows-x86_64-msvc-release/native-linear-array-v1.json) | 0.063 | 0.270 | 141.698 | 90.290 | 0.555 | 0.710 | 0.541 | 5.46 MiB |
| [no-JIT linear array](windows-x86_64-msvc-release/nojit-linear-array-v1.json) | 0.044 | 0.199 | 131.174 | 98.938 | 0.814 | unavailable | unavailable | 5.32 MiB |

The scalar and array native reports both record cold compilation/insertion and
later native-cache hits. The no-JIT report records both native phases as
`unavailable` while preserving bytecode/portable result equality. Every report
passed the protocol-1.0 Draft-7 schema, semantic timing/allocation/cache
validator, and an independent PowerShell SHA-256/size check.

The measured comparison is reviewed in the
[v1.0 JIT scope decision](../../v1.0-jit-scope-decision.md). It records median
speedups of 911.12x for the native scalar loop, 166.94x for the native array
loop, and 121.52x for the no-JIT portable array loop. The candidate-readiness
gate also preserves conservative worst-sample evidence above 100x, 100x, and
80x respectively. Those checks bind this fixed decision evidence; they are not
portable performance promises or thresholds for future hosts.

Local release evidence for the same implementation:

- Windows native Release: 201/201 tests passed.
- Windows no-JIT Release: 195/195 tests passed.
- `BUILD_TESTING=OFF`: SDK runtime built without the engineering collector
  target.
- `release_archive_smoke`: reproducible relocated C11/C++20/CLI consumers
  passed; the final archive SHA-256 was
  `8a84bf772b0d7e94302f38c065562d1bda909b54185ae971e4499c9455efcc1a`.

The archive hash is recorded here because this source-only index is not part of
the archive payload. Functional cross-platform validation later passed at
revision `f34d8d9` in Actions run `30684969401`, but no comparable platform
reports were committed by that workflow. The subsequent native-only
`mparser_performance_evidence` target and CI upload steps make those reports
repeatable on Windows, Linux, macOS x64, and native-architecture macOS ARM64.
The accepted eight-report set from revision `85685b8` and successful Actions
run `30691616946` is indexed under
[`actions-30691616946`](actions-30691616946/README.md). Its ARM64 CPU label is
`Apple M1 (Virtual)`, so the evidence is explicitly hosted and non-emulated,
not a bare-metal timing claim. Linux AArch64/QEMU results remain functional
evidence only.
