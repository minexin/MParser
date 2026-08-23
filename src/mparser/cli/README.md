# CLI Ownership

The `cli` directory owns the `mparser` executable entry point and its
command-line execution policy. It selects execution modes, connects the
front-end and embedding layers, and presents diagnostics and machine results.

Runtime behavior belongs in `src/mparser`; the CLI should remain a thin host
boundary so the same compiled module can be used through the C and C++ APIs.
