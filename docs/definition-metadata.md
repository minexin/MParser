# Definition Metadata

Development API in ABI generation 2 revision 6 (unpublished). Retrieve the
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
