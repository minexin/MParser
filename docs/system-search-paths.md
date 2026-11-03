# System Search Paths

Development SDK 1.15.0, C ABI generation 2 revision 8. Two additive C exports
expose the actual SystemContext path list without executing source:

- mparser_system_context_visit_search_paths(context, visitor, user_data)
- mparser_system_context_set_search_paths(context, paths, path_count)

C++ SystemContext::searchPaths() returns an owning vector of UTF-8 strings;
setSearchPaths(vector<string>) replaces the list. These operate on the same
state used by path/addpath/rmpath in a Runtime retaining that context, not a
separate host cache. Product/source API stays 1.15.0/1.15; ABI is now 2.8 with
148 exports. Old revision-5/revision-7 symbol manifests remain preserved.

## Read Contract

SearchPaths capability is required; denied access returns SYSTEM_CONTEXT_FAILED,
not an empty path. Empty is a successful, truly empty list. A getter copies one
complete ordered snapshot under the context lock and performs no source
compilation, execution, cwd/random/workspace mutation or filesystem writes.

The C visitor receives sequential indices and canonical absolute UTF-8 paths.
Each view is borrowed only during its callback. Copy bytes before returning.
Callbacks run after releasing context locks and may reenter context APIs; they
still observe the original detached snapshot. Nonzero continues, zero aborts
with SYSTEM_CONTEXT_FAILED after any already-visited prefix. Snapshot/encoding
failure calls no visitor. Do not throw exceptions across a C callback. The C++
wrapper owns copied strings and rethrows its own allocation exceptions safely.

## Replace Contract

All entries are validated before one locked list replacement. Directory
normalization and rooted-adapter constraints are reused. Relative paths resolve
against one cwd captured for the whole call, not the process cwd. First
normalized occurrence wins; order is otherwise retained. Empty list clears the
path; empty entries, embedded NUL, malformed UTF-8 and null-data/nonzero-size
views are INVALID_ARGUMENT. Missing/non-directory/outside-root entries or
denied capability return SYSTEM_CONTEXT_FAILED. Failure preserves the old list.
Allocation failures return ALLOCATION_FAILED; no C++ exception escapes the ABI.

Reads/writes publish complete lists. They are not a multi-call transaction,
compare-and-swap API, filesystem transaction, or automatic module recompiler.
Existing compiled source graphs and escaped handles retain their original
bindings. Hosts must serialize path snapshot plus compilation if they require
one session action to use a stable search-path state.

## Compiler Integration

SourceLoadOptions remains an explicit loader input. A host can populate its
searchPaths from context.searchPaths() for each new command/file compile;
the owning snapshot remains valid after subsequent context mutation. Prepend
the appropriate checked source directory/current directory if the host's lookup
contract requires them. Current directory and the path list are distinct state.

This does not silently bind SourceLoader filesystem access to SystemContext.
The host must validate entry paths/source identities and loader search roots;
an arbitrary user-supplied absolute source label is not a rooted file grant.
The host integration candidate changes the existing command/source_path loader
options only. Its self-contained in-memory source endpoint stays unchanged;
cross-file in-memory indexing/navigation remains a separate acceptance gate.
Within an already compiled script, a path mutation does not retroactively load
a previously unresolved direct function call. That is not promised here.

The opt-in MEXEC_SYSTEM_PATH_SYNC host candidate uses these APIs for compilation,
virtual REST path display and REST add/remove. It preserves Runtime/Context
identity and does not execute hidden path-query scripts or parse addpath text.
REST add retains its prior append semantics; source addpath keeps kernel
prepend/end semantics. Default OFF preserves the old host behavior. SDK and
focused host tests must not be reported as deployed-client acceptance.
