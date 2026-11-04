# Explicit Source-Graph Ownership

Status: development implementation, not a shipped release or full goal acceptance.

## Compatibility Boundary

Keep `mparser_source_unit` and `mparser_module_compile_sources` unchanged. The
old entry point checks exact element size and indexes a contiguous array;
appending fields would change its stride for existing callers. Do not infer
class ownership from opaque source names or consult the filesystem.

Add a distinct caller-sized source-graph unit and a distinct compile entry
point. Each unit carries source name, source UTF-8 and optional explicit class
method owner. An empty owner means an ordinary source. Use an array of pointers
to caller-sized units so future trailing fields do not change array stride.
Validate all pointers, lengths, sizes and ABI generations before publication;
copy input strings and preserve the existing null-output failure contract.
Allocate a new ABI revision/export manifest only with the implemented entry
point and its compatibility tests. Product remains unreleased 1.15.0.

The C++ layer uses a distinct graph-unit type and overload, retaining the old
SourceUnit contract. It owns strings through the call and returns the same
owned Module. No runtime execution or mutable reflection handle is introduced.

## Compiler Authority

Pass explicit method ownership into the existing internal SourceUnit field;
the compiler remains responsible for merging implementations with declarations.
Its existing checks reject missing owners, non-function source, filename and
primary-function mismatches, and misplaced constructors. Hosts must not fix
definition kinds or class names after compilation. This ownership associates
sources within a newly compiled module; it is not authority over live objects
from another module.

The initial extension addresses the demonstrated external-method integration
gap. Package namespaces, class-private functions and function-binding aliases
remain separate internal fields and must not be advertised as supported by
this first public descriptor. Do not silently accept unimplemented context.

## Acceptance Work

- Public C/C++ explicit-owner positive tests: declaration and implementation
  remain distinct, source indices and nested parents correct, data survives
  module release, and invoking a class method uses the merged implementation.
- Public negatives: bad sizes/generation/null views, unknown class, signature
  mismatch, wrong method filename, script input and constructor placement.
- Old source arrays/headers/symbol consumers and installed SDK regression.
- Host candidate accepts optional method ownership only through the new SDK,
  validates its JSON type and rejects unsupported context; tests submit explicit
  ownership instead of treating an `@Class` name as implicit permission.
- Repeat the currently failing external-method REST case and all clean-host
  regressions, then update manifests/docs/CI without claiming browser adoption.

Internal ownership-negative checks were added to compiled_module_smoke and
public argument/ownership checks to cpp_api_smoke.

Initial implementation adds `mparser_module_compile_source_graph`, pointer-array
`mparser_source_graph_unit` descriptors and C++ `SourceGraphUnit`. Focused
cpp_api_smoke now verifies declaration/implementation separation, nested parent
indices, owned metadata after module release and `obj.read()` returning 7.
ABI 2.10/154-export and layout checks pass. Installed C and C++ consumers use
the new entry point. The clean installed-SDK host replay passes external-method
metadata and 67/67 regressions at
`build/clean-host-probe/3e274527adcc4cb29f4a6d0ac7bb8218`.
These results do not establish browser navigation, deployment or release
acceptance; final full regression and CI must be checked before publication.

The first execution check using `read(obj)` failed with
`unknown bytecode runtime variable: read`. Object-form `obj.read()` passed.
Function-form external-method dispatch remains a separate demonstrated gap;
the object-form result does not prove parity for that calling syntax.
