# MParser C ABI Development Contract

MParser and its installed SDK use one product version. The current source tree
is the v1.2 development line and reports product/SDK version `1.2.0`. This is
development metadata until the complete milestone reaches its tagged release
gate.

The embedding boundary has an independent technical contract level:

- C source API: `MPARSER_C_API_VERSION_MAJOR/MINOR/PATCH == 1/2/0`;
- C ABI generation: `MPARSER_C_ABI_GENERATION == 2`;
- C ABI revision: `MPARSER_C_ABI_REVISION == 0`;
- shared-library full version: `1.2.0`, with SOVERSION/install-name generation
  `2`.

The API version follows the MParser/SDK development line. The ABI generation
and revision are binary negotiation data rather than another SDK version.
`mparser_c_abi_generation()` and `mparser_c_abi_revision()` report them at
runtime. The three `mparser_version_*()` functions report product version
`1.2.0`.

## Development Policy

The project is not yet using the v1.2 SDK in production. Current headers,
implementation, in-repository callers, tests, examples, and documentation move
together. Superseded development headers and binaries are not compatibility
targets, and no adapters are added solely to preserve them.

At the v1.2 candidate gate, the current header, layouts, symbols, package
metadata, and independent consumers will be frozen as one reviewed contract.
After that freeze, an incompatible correction requires a new ABI generation.
An additive change within one frozen ABI generation requires a revision
increase and
updated consumer evidence.

The released MParser 1.0.0 ABI 1.1 contract remains immutable historical
evidence in `docs/public-contract-v1.json` and
`tests/public_contract/c_abi/1.1`. It is not the active ABI 2 test target.

## Library Identity

The current ABI-generation library names are:

- ELF: `libmparser_c.so.2`;
- macOS: `libmparser_c.2.dylib`;
- Windows: `mparser_c.dll` plus its import library.

Internal compiler, VM, C++ facade, and SLJIT symbols have hidden visibility.
The current public export set is the 89-name manifest in
`tests/c_api_generation2_symbols.txt`.

## Extensible Roots

These root structures start with `struct_size` and `abi_generation` and may gain
tail fields within ABI 2:

- `mparser_invocation_options`;
- `mparser_execution_summary`;
- `mparser_source_load_options`.

Initialize current-source storage with the uppercase macros:

```c
mparser_invocation_options options;
MPARSER_INVOCATION_OPTIONS_INIT(&options);

mparser_execution_summary summary;
MPARSER_EXECUTION_SUMMARY_INIT(&summary);

mparser_source_load_options load_options;
MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options);
```

The macros call the matching `*_init_sized` function with the caller's
complete `sizeof` and current ABI generation. A host wrapper may reserve tail
storage explicitly:

```c
struct extended_request {
    mparser_invocation_options value;
    unsigned char future_tail[32];
};

struct extended_request request;
mparser_api_status status = mparser_invocation_options_init_sized(
    &request, sizeof(request), MPARSER_C_ABI_GENERATION);
```

A successful sized initializer validates the ABI generation and minimum prefix,
clears the caller-declared storage, records its capacity in `struct_size`, and
sets known defaults. Input readers consume known fields and ignore an unknown
tail. Output writers never exceed the recorded capacity.

The public minimum-size constants are `MPARSER_INVOCATION_OPTIONS_SIZE`,
`MPARSER_EXECUTION_SUMMARY_SIZE`, `MPARSER_SOURCE_LOAD_OPTIONS_SIZE`, and
`MPARSER_SOURCE_UNIT_SIZE`. They describe the current minimum accepted record
or fixed stride; they are not API or product versions.

## Sealed Records

`mparser_source_unit`, `mparser_named_value`, `mparser_numeric_buffer`, the
UTF views, and source positions are fixed-stride records in the current ABI.
In particular, source units are passed as an array, so
`mparser_source_unit.struct_size` must equal `MPARSER_SOURCE_UNIT_SIZE`.
An oversized source-unit descriptor is rejected rather than interpreted with
an ambiguous stride.

Opaque module, session, result, value, cancellation, and diagnostic handles
may change internally. Their retain/release, ownership, concurrency, and
borrowed-view rules are documented in `embedding-c-api.md`.

## Numeric Transport

ABI 2 replaces the double-only numeric transport with
`mparser_numeric_buffer`. It carries a numeric class, complexity flag, element
count, and typed real/imaginary pointers. `int64_t` and `uint64_t` therefore
cross the boundary exactly instead of passing through `double`; complex double
and single arrays preserve separate components. Logical and integer values
must be real.

Input buffers are copied. Output buffers are immutable views owned by the
returned `mparser_value`. No external C++ layout, writable runtime storage, or
borrowed-input lifetime crosses the C boundary.

## Validation

`c_api_smoke` validates ABI negotiation, caller-sized roots, sealed records,
every numeric class, complex buffers, source graphs, sessions, ownership,
resources, and failure boundaries. `c_api_layout_contract` checks current
64-bit sizes, offsets, and constant values.

`c_api_shared_library_abi` inspects the dynamic library with the platform
toolchain, compares its exports with `tests/c_api_generation2_symbols.txt`, and checks
SONAME/install-name major 2. Lifecycle, unload, allocation-failure, named-fault,
concurrency, and relocated C/C++ consumer tests exercise the same current
headers. The complete platform matrix runs once the v1.2 train reaches its
candidate gate; internal batches use focused local regression unless a change
has unusual cross-platform risk.
