# v1.4 Numeric Performance Workloads

`manifest.json` is the versioned source of truth for the v1.4 performance
suite. The workloads deliberately span existing optimized loops and currently
unoptimized straight-line, element-wise, reduction, function-call, and dense
linear-algebra paths. Each script leaves one finite scalar named
`baseline_result`; that checksum is compared across bytecode, portable Typed,
and native Typed execution by the existing performance-baseline collector.

The suite characterizes behavior rather than imposing machine-specific timing
thresholds. A workload may legally remain in the VM. Its index entry records
whether Typed regions executed, were rejected by a guard, or were not covered,
along with timing and allocation signals. Optimizer work must be selected from
repeatable evidence and preserve the bytecode result through transactional
fallback.

Run the quick contract after building:

```powershell
ctest --test-dir build/windows-msvc-release `
  -R "^performance_suite_contract_smoke$" --output-on-failure
```

Generate the full local suite with:

```powershell
cmake --build build/windows-msvc-release --config Release `
  --target mparser_v1_4_performance_suite
```

Reports and `suite-index.json` are written below
`<build>/performance-suite-v1.4` unless
`MPARSER_PERFORMANCE_SUITE_OUTPUT_DIR` is configured differently.

The quick contract is a native-host test and is intentionally not registered
for emulator-based cross-compiles. Installed SDKs preserve all manifest paths
below `<prefix>/share/mparser/performance-suite`; that directory is the source
root for the installed `benchmarks/v1.4/manifest.json`.
