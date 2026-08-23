# Bytecode VM Ownership

This directory owns the bytecode execution engines and their VM-specific
runtime integration:

- `bytecode_vm.*` contains the reference bytecode dispatch, call-frame
  handling, diagnostics, and typed-region integration.
- `adaptive_bytecode_vm.*` contains profiling, promotion, invalidation, and
  adaptive-session behavior built on the bytecode VM.

The bytecode representation, verifier, and region metadata remain in the
parent `execution/bytecode` directory. The headers at the parent level are
small forwarding headers retained for source-path continuity; new internal
code should include the headers from this directory directly.
