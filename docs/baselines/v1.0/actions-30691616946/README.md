# Cross-Platform Candidate Performance Evidence

This immutable evidence set was downloaded from successful GitHub Actions run
[30691616946](https://github.com/minexin/MParser/actions/runs/30691616946).
All reports bind source revision
`85685b88f8f8eb4e89b03abf53aa16dbbe60c68c`, project version `0.90.0`,
the current scalar/array workload hashes, and Release binaries produced by the
same platform job. `SHA256SUMS` records the downloaded report identities.

## Environments

| Artifact | OS | Architecture | CPU label | Logical CPUs | Memory | Compiler |
| --- | --- | --- | --- | ---: | ---: | --- |
| `windows-x86_64` | Windows 10.0.26100 | x86_64 | AMD64 Family 25 Model 1 Stepping 1, AuthenticAMD | 4 | 15.99 GiB | MSVC 19.51.36252.0 |
| `linux-x86_64` | Linux 6.17.0-1020-azure | x86_64 | AMD EPYC 7763 64-Core Processor | 4 | 15.61 GiB | GCC 13.3.0 |
| `macos-x86_64` | Darwin 24.6.0 | x86_64 | Intel Core i7-8700B at 3.20 GHz | 4 | 14.00 GiB | Apple Clang 17.0.0.17000013 |
| `macos-arm64` | Darwin 24.6.0 | aarch64 | Apple M1 (Virtual) | 3 | 7.00 GiB | Apple Clang 17.0.0.17000013 |

Every report records `emulated=false`. The macOS ARM64 lane executes the
ARM64 binary on an Apple Silicon, native-architecture hosted runner. Its CPU
label explicitly says `Virtual`, so this is hardware-backed virtual-runner
evidence, not a bare-metal timing claim. Linux AArch64/QEMU remains functional
evidence only and is intentionally absent from this set.

## Host-Wall Medians

Times are milliseconds. These observations characterize the named hosts and
boundaries; they are not portable release thresholds.

| Platform | Workload | Parse | Compile | Process cold | Bytecode | Portable | Native cold | Native warm | Bytecode/native warm |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows x86_64 | scalar loop | 0.030 | 0.117 | 1767.300 | 1663.630 | 6.710 | 2.839 | 2.738 | 607.69x |
| Windows x86_64 | linear array | 0.038 | 0.169 | 92.559 | 63.280 | 0.326 | 0.344 | 0.244 | 259.72x |
| Linux x86_64 | scalar loop | 0.017 | 0.068 | 930.635 | 890.491 | 6.311 | 2.568 | 2.460 | 361.92x |
| Linux x86_64 | linear array | 0.023 | 0.080 | 45.570 | 37.381 | 0.328 | 0.310 | 0.249 | 150.18x |
| macOS x86_64 | scalar loop | 0.025 | 0.106 | 1071.398 | 1080.252 | 12.090 | 2.007 | 1.793 | 602.45x |
| macOS x86_64 | linear array | 0.033 | 0.119 | 61.319 | 57.954 | 0.543 | 0.413 | 0.264 | 219.82x |
| macOS ARM64 | scalar loop | 0.011 | 0.042 | 659.654 | 674.666 | 5.887 | 1.402 | 1.326 | 508.98x |
| macOS ARM64 | linear array | 0.014 | 0.052 | 47.818 | 35.381 | 0.272 | 0.414 | 0.129 | 274.76x |

All eight reports pass the protocol-1.0 Draft-7 and semantic validator. Each
records result equality, zero runtime and typed-region fallback, one cold
native-cache population, and warm native-cache hits. The checked-in
`cross_platform_performance_evidence_smoke` independently enforces the exact
report set, manifest, revision, workload hashes, platform/compiler identity,
non-emulation marker, measured boundaries, fallback behavior, correctness,
and cache transition.
