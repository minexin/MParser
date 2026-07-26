# MParser C ABI Compatibility

MParser v0.89 enforces the evolution rules for C ABI candidate major 1. The
candidate is still pre-v1.0, but its compatibility behavior is executable and
must be reviewed before any public layout or symbol changes.

The current tuple is:

- ABI major: `MPARSER_C_ABI_VERSION_MAJOR == 1`;
- ABI revision: `MPARSER_C_ABI_REVISION == 1`;
- engine release: `0.89.0`.

`mparser_c_abi_version()` returns the ABI major.
`mparser_c_abi_revision()` returns the additive feature revision. The engine
version functions describe language/runtime behavior and do not replace ABI
negotiation.

## Compatibility Policy

Within one ABI major, MParser may:

- add exported functions;
- add status or enum-like integer constants without reusing old values;
- append fields to explicitly extensible structures;
- add new opaque handle types;
- strengthen diagnostics while preserving status and ownership meaning.

Within one ABI major, MParser must not:

- remove or change an existing exported function signature;
- change an existing constant value;
- reorder, remove, or reinterpret an existing structure field;
- change ownership of an existing returned handle or borrowed view;
- expose a formerly opaque handle layout;
- append fields to a sealed structure used in an array.

An incompatible correction requires a new ABI major and, once the v1.0
binary boundary is frozen, a matching shared-library ABI name.

The current shared-library implementation version is `1.1.0`, with ABI-major
SONAME/install name 1:

- ELF: `libmparser_c.so.1`;
- macOS: `libmparser_c.1.dylib`;
- Windows: `mparser_c.dll` plus its import library.

The engine release remains independent. Internal compiler, VM, C++ facade,
and SLJIT symbols have hidden visibility. The complete public export set is
the 90-name manifest in `tests/c_api_abi1_symbols.txt`.

## Extensible Structures

These root structures may gain tail fields:

- `mparser_invocation_options`;
- `mparser_execution_summary`;
- `mparser_source_load_options`.

Each starts with `struct_size` and `abi_version`. New code initializes the
complete storage available in the compiling host with:

```c
mparser_invocation_options options;
MPARSER_INVOCATION_OPTIONS_INIT(&options);

mparser_execution_summary summary;
MPARSER_EXECUTION_SUMMARY_INIT(&summary);

mparser_source_load_options load_options;
MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options);
```

The macros call the corresponding `*_init_sized` function with the caller's
`sizeof` and ABI major. A host wrapper with reserved future storage may call
the sized entry directly:

```c
struct future_request {
    mparser_invocation_options value;
    unsigned char future_tail[32];
};

struct future_request request;
mparser_api_status status = mparser_invocation_options_init_sized(
    &request, sizeof(request), MPARSER_C_ABI_VERSION);
```

A successful sized initializer:

1. requires storage at least as large as the ABI-major-1 prefix;
2. rejects an unsupported ABI major with `ABI_MISMATCH`;
3. clears every byte in the caller-declared storage;
4. records that storage capacity in `struct_size`;
5. initializes known defaults such as the automatic backend.

The storage must be writable for the declared size and naturally aligned for
the corresponding public structure. Making that structure the first member
of a host wrapper, as above, satisfies both the prefix-address and alignment
requirements.

Input consumers require the ABI-major-1 prefix, read only fields known to the
library, and ignore a larger unknown tail. This lets a newer-header host call
an older revision-1-or-later library when it uses only fields understood by
that library.

For output structures, the host initializes its storage before the getter.
`mparser_result_execution_summary` writes only fields known to the library and
never writes beyond the recorded capacity. Bytes belonging to a future tail
remain at their initialized default.

The fixed minimum constants are:

- `MPARSER_INVOCATION_OPTIONS_V1_SIZE`;
- `MPARSER_EXECUTION_SUMMARY_V1_SIZE`;
- `MPARSER_SOURCE_LOAD_OPTIONS_V1_SIZE`.

## Legacy Initializers

The original v0.83-v0.86 functions remain exported:

- `mparser_invocation_options_init`;
- `mparser_execution_summary_init`;
- `mparser_source_load_options_init`.

They always clear and report exactly the frozen ABI-major-1 prefix, even if a
future library was compiled with a larger current structure. That fixed write
range is required so an old binary cannot be overwritten by a newer library.

New source should use the uppercase safe-initialization macros. The legacy
functions exist for already-built ABI-major-1 consumers and for compatibility
tests.

## Sealed Structures

The following records have fixed layout within ABI major 1:

- `mparser_source_unit`;
- `mparser_named_value`;
- `mparser_utf8_view` and `mparser_utf16_view`;
- `mparser_source_position`.

In particular, `mparser_module_compile_sources` receives an array of
`mparser_source_unit`, so the compiled library uses the frozen structure size
as the array stride. `mparser_source_unit.struct_size` must equal
`MPARSER_SOURCE_UNIT_V1_SIZE`; an oversized descriptor is rejected rather
than ambiguously indexed. A future source descriptor needs a new type and a
new function that carries an explicit stride or pointer list.

Opaque module, session, result, value, cancellation, and diagnostic handles
may change internally without changing this ABI. Their retain/release and
borrowed-view rules remain those in `embedding-c-api.md`.

## Compatibility Matrix

| Host binary | Runtime library | Contract |
| --- | --- | --- |
| v0.86 ABI-major-1 header | v0.89 | Supported through frozen old symbols and prefixes |
| v0.89 current header | v0.89 | Supported, including sized initialization |
| Future ABI-major-1 header | v0.89 | Supported for known prefix fields when revision-1 symbols are available |
| v0.89 code using revision-1 symbols | v0.86 | Not load-compatible because those additive symbols do not exist |
| Different ABI major | Any | Rejected or requires a separately named ABI/library |

Use package-version constraints when deploying code that depends on a newer
additive symbol. Runtime ABI revision checks describe capabilities after the
binary has loaded; they cannot make a missing dynamic symbol loadable.

## Validation

`c_api_smoke` covers undersized and wrong-major rejection, old-prefix write
bounds, oversized request execution, oversized source-load execution,
oversized summary output, and sealed source descriptors.

`c_api_v1_compat_smoke` is compiled only against the frozen v0.86 header
snapshot. It links the current library, compiles a two-source graph, executes
it, and reads an execution summary without any revision-1 declarations.

`c_abi_compat_demo_smoke` runs `samples/c_abi_compat_demo.c`, which demonstrates
future-tail request and summary storage. The installed consumer checks ABI
`1.1` after package relocation. Focused Linux AArch64 native and portable jobs
run the same evidence under QEMU.

`c_api_shared_library_abi` inspects the built dynamic library with
`dumpbin`, `nm`, `readelf`, or `otool` as appropriate. It rejects missing or
unexpected `mparser_*` exports and rejects an ELF SONAME or macOS install name
whose major differs from ABI major 1.

SOVERSION 1 names the current ABI-major candidate but does not declare the
v1.0 freeze early. The v0.90 review may still require a new ABI major for an
incompatible correction; after v1.0, such a change requires the documented
major-version transition.
