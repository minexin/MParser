# Native Linux ARM64 Candidate Performance Evidence

This immutable evidence set was downloaded from successful GitHub Actions run
[30732814590](https://github.com/minexin/MParser/actions/runs/30732814590).
Both reports bind source revision
`3ec2bc074387283785068fe6ac98b4472a109a3a`, project version `0.90.0`,
the current scalar/array workload hashes, and Release binaries produced by the
same native Linux ARM64 job. `SHA256SUMS` records the downloaded report
identities.

## Environment

| Artifact | OS | Architecture | CPU label | Logical CPUs | Memory | Compiler |
| --- | --- | --- | --- | ---: | ---: | --- |
| `linux-aarch64` | Linux 6.17.0-1020-azure | aarch64 | ARM implementer `0x41`, architecture `8`, variant `0x0`, part `0xd49`, revision `0` | 4 | 15.57 GiB | GCC 13.3.0 |

Both reports record `emulated=false`. The `ubuntu-24.04-arm` hosted runner
executes the ARM64 binary natively. The kernel does not expose a generic model
name, so the collector records the raw ARM MIDR fields instead of publishing
an ambiguous `unknown` label. This is hosted-runner evidence, not a bare-metal
timing claim. Linux AArch64/QEMU remains independent functional/package
evidence and is intentionally absent from this set.

## Host-Wall Medians

Times are milliseconds. These observations characterize this named host and
the documented protocol boundaries; they are not portable release thresholds.

| Workload | Parse | Compile | Process cold | Bytecode | Portable | Native cold | Native warm | Bytecode/native warm |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| scalar loop | 0.015 | 0.059 | 835.990 | 827.026 | 5.698 | 1.883 | 1.801 | 459.21x |
| linear array | 0.021 | 0.070 | 39.105 | 33.962 | 0.214 | 0.237 | 0.143 | 237.05x |

Both reports pass the protocol-1.0 Draft-7 and semantic validator. Each records
matching bytecode, portable, and native results; zero runtime and typed-region
fallback; one cold native compilation; and 20 measured warm cache hits. The
checked-in `linux_arm64_performance_evidence_smoke` independently enforces the
exact report pair, manifest, revision, workload hashes, GNU/aarch64 native
identity, non-emulation marker, non-unknown CPU identity, measured boundaries,
fallback behavior, correctness, and cache transition.
