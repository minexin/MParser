# Definition Metadata

Development API introduced in ABI generation 2 revision 6 and retained in the
unpublished revision 10 SDK. Retrieve the
syntax definition directory with C++ `Module::definitions()` or C
`mparser_module_definition_count` / `mparser_module_definition_info`.
Unlike `functionNames()`, this includes nested functions and class methods;
directory membership does not imply a function is externally callable.

Each record contains the compiler's definition name, owning class when
applicable, primary/local/nested/method kind, source index and name, source
range, optional enclosing-function index, and declaration-only flag.
Names may include source-graph qualification; they are not unique identifiers.
Indices are module-local syntax traversal indices, not persistent IDs and not
indices in the callable-function catalog. No parent is SIZE_MAX in C and
nullopt in C++. Class ownership is separate from enclosing-function ownership.

Ranges originate in the parsed source, before semantic lowering. Offsets are
zero-based UTF-8 byte offsets into the corresponding source's content. Lines
and columns are one-based; columns count source bytes, not UTF-16 code units
or visual glyph cells. End positions are exclusive. A host using UTF-16 editor
positions must translate against the exact compiled source, not reinterpret
byte offsets as JavaScript string indices. CRLF line endings and leading
UTF-8 comments have focused tests. Ranges cover entire definitions, not just
the function name token; trailing separators may belong to the parser span.

C strings are borrowed for the lifetime of the module. C++ results own copies
and remain usable after module release. C query failures do not modify output;
null pointers or insufficient output size return INVALID_ARGUMENT, and an
invalid index returns OUT_OF_RANGE. Querying never executes code. Invalid
modules may have an empty or partial directory and must not be treated as a
complete navigation index.

Current evidence: internal compiled-module tests cover nested/local functions,
class methods and multiple source units. Separate method declarations and
implementations are both retained before compiler merging, with distinct source
indices and declaration-only flags. Repeated nested names retain distinct parent
indices and source ranges. Public SDK tests cover nested ranges,
ownership after module release, C argument failures and DLL exports.
The installed C++ consumer also calls name validation and reads definition
metadata from the relocated SDK. Real host navigation remains to be verified
before release.

An opt-in MExecServer integration candidate is supplied in
`tools/host-integration/mexecserver-definition-metadata.patch`. With
MEXEC_DEFINITION_METADATA enabled, /compile exposes a versioned definition
directory with explicit position units and exact submitted source text. A
successful compile is marked complete; invalid modules expose an empty,
incomplete directory rather than a partial navigation index. This is an
in-memory, single-request compile endpoint, not cross-file browser navigation.
Before navigating, the host must compare the directory's source name/text with
the exact current document and reject stale responses. Definition indices are
not stable across requests. The candidate does not implement that UI policy.

## Multi-Source Host Candidate

Apply `mexecserver-definition-sources.patch` after the other clean-host patches.
With `MEXEC_DEFINITION_METADATA` enabled, `/compile` additionally accepts
`{"sources":[{"name":"first.m","source":"..."},{"name":"second.m","source":"..."}]}`.
This form excludes `source` and `source_name`, accepts 1 to 64 units and requires
unique nonempty names without NUL. Names are source identities, not paths to
read from disk. All units are submitted to the SDK source-graph compiler in
request order; compilation does not execute them.

The directory's additive `sources` array binds every `source_index` to the
exact submitted name and UTF-8 text. Parent indices refer to the combined
definition directory, not a per-file index. The original top-level source fields
describe the first submitted unit for compatibility; consumers of a graph must
use the array and per-definition source index. Invalid graphs expose an empty,
incomplete directory. Legacy single-source requests remain supported.

`test-definition-sources.cjs` checks repeated nested names, parent indices,
UTF-8/CRLF ranges, invalid graphs, malformed requests and legacy controls.
Initial isolated replay passed at `build/definition-sources-probe`. The complete
eight-patch clean-host rebuild, all REST probes and 67/67 host regressions then
passed at `build/clean-host-probe/bc0e56b4c35e484d851b69ef48e1056a`;
the aggregate log is `build/definition-sources-clean-host.log`. The selected
installed DLL SHA256 is
`5E202651C50AFBF11BFFD2DBB333E7EA88D6F0E567E0CA8CBFDFA00756D2CBC2`.
Browser cross-file navigation,
stale-document rejection and deployment are not implemented by this candidate.

The probe accepts a third `on`/`off` argument (default `on`). Disabled builds
must reject every request containing `sources`, including mixed legacy input,
instead of silently validating only `source`. A negative replay initially
returned HTTP 200 for that mixed request (`build/definition-off-red.log`).
The candidate now rejects it with HTTP 400 while retaining legacy single-source
compile; the corrected OFF replay passed at `build/definition-off-green`.

### Explicit Source-Graph Ownership

Extended REST replay at `build/definition-external-method-probe` compiles
`@ExternalRange/ExternalRange.m` plus `@ExternalRange/read.m`. The declaration
and implementation both retain their source indices, but the implementation
is reported as `primary-function` rather than `method`, without class ownership.
The failing assertion is retained in `build/definition-external-method-test.log`.
This case is not covered by the earlier passing plain-function source graphs.

ABI 2.10 introduces `mparser_module_compile_source_graph` and the C++
`Module::compile(span<const SourceGraphUnit>)` overload. The old source-unit
interface still carries only name and source and remains unchanged. Initialize
each new C descriptor with `MPARSER_SOURCE_GRAPH_UNIT_INIT`, fill its UTF-8 views
and optional `class_method_owner`, then pass an array of descriptor pointers.
Keep descriptors and strings alive through compilation. Input is copied; source
views and definitions follow the existing Module lifetime rules. C++ definition
copies remain valid after module release. Future descriptor tails are ignored;
undersized descriptors or incompatible generations are rejected.

Source names are identities, not permission to infer filesystem layout or read
files. An explicit owner connects a primary method implementation to a class
declared in the graph; the compiler validates filename, signature and ownership.
The host candidate now accepts a string `class_method_owner` on each unit and
requires ABI 2.10. It must not relabel functions or guess ownership from text.
Public SDK metadata and object-form method invocation tests pass. The ABI 2.10
clean installed-SDK host build and expanded external-method REST replay passed
at `build/clean-host-probe/3e274527adcc4cb29f4a6d0ac7bb8218`, followed by 67/67
host regressions (`build/source-owner-clean-host.log`). The loaded DLL SHA256 is
`8DAFC92670588B8BC85B9F0E0EF4C8CE932CAA6A8A3955465968306BBA7BDAEC`.
Browser method navigation remains unverified. The subsequent same-module
function-form dispatch fix has separate R2024b, SDK and host execution checks;
see [function-form dispatch](function-method-dispatch.md). Cross-module automatic
dispatch and explicit class-precedence attributes remain outside that proof.
