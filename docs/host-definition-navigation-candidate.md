# Host Definition Navigation Candidate

Development candidate, 2026-11-05. Apply the server definition-capabilities
patch after the existing server candidates and the client definition-navigation
patch after the eleven earlier client candidates in `tools/host-integration`.
These patches have been tested on source copies only. The original MExecServer
and MExecClient projects and deployed services are unchanged. SDK 1.15.0,
C ABI 2.12/158 and existing definition-directory version 1 are unchanged.

## Source And Navigation Contract

With `MEXEC_DEFINITION_METADATA=ON` the server advertises
`compile-definition-directory` and `compile-source-graph`. The current-document
check remains a single-source compile. The open-document scope is offered only
with the graph capability and explicitly submits selected open buffers, up to
64. Bound documents from other sessions are excluded. Full virtual paths or
document-specific untitled names identify source units; the client does not
discover dependencies, read additional files, or guess an external method's
owner. The owner is an explicit optional input passed through to the compiler.
The legacy host still supports its original single-document check.

The client checks the directory schema, units, completeness, exact source
identities and contents, parent indices, cycles and ranges before presenting
navigation. It converts UTF-8 byte offsets to UTF-16 selections on the exact
submitted text, preserving a clean file's original line endings for compilation
and mapping CRLF/CR to the editor's LF. Invalid offsets cannot split a UTF-8
scalar or CRLF pair. Definitions are not inferred from function names or source
patterns. Labels distinguish methods, declarations, nested parents and source
locations. Large directories are paged in groups of 100 without dropping records.

Navigation rechecks the entire selected source graph, document identities,
paths, edit revisions, owners and session. An edit followed by undo does not
revive an old result. Closing a source does not reopen it. Selection activates
the existing editor buffer and changes its selection/scroll only, leaving
other drafts, undo history and breakpoint state in place. Nothing executes
source code or updates the execution workspace. This is definition-directory
navigation, not cursor-based symbol resolution or an automatic project index.

## Acceptance Evidence

- Frozen twelve-client-patch replay includes the original editor-analysis test
  and new `test-client-definitions.cjs`: raw CRLF/CR, astral UTF-8/UTF-16,
  repeated names, ranges, graph and explicit ownership, foreign/stale/closed
  sources, revisions, legacy capability fallback and pagination.
- Fresh installed-SDK source-only host: all 148 build actions, REST/client
  probes and 67/67 host tests in 24.30 seconds at
  `build/clean-host-probe/5ddf2d27d38e44c6a27ad1cc54d19334`.
  `build/definition-client-clean-host.log` includes three real compiler
  directories replayed through the actual client scripts with simulated DOM.
  The corpus exercises repeated nested names, external declarations and
  implementations and a compile-only source that would fail if executed.
  DLL SHA256: `9689A754300DF313D828F7FD9692C6D9447426AC05D5CFC2EF85AAD1A67AE298`.
- Tabbit actual interaction: `build/browser-definition-20261105/basic.json`
  and `methods-final.json` preserve compile requests, responses and selections.
  Two separate `helper` definitions, a method declaration, its implementation
  and its nested helper select the exact expected text in the correct existing
  document. The CRLF document contains a non-ASCII and astral prefix. An
  edit/undo leaves stale links disabled, a new check replaces them, and the
  separate draft survives. Session run count and workspace revision stay 0.
- At 390x844 the document width remained 390 and the analysis panel had no
  horizontal overflow after compact mode settled. The viewport was restored to
  1904x915. Native screenshot capture timed out; this is interaction/DOM
  evidence, not screenshot or pixel acceptance. Tabbit finished without forced
  cleanup, and the owned isolated server was stopped. The earlier failed
  fixture upload and immediate post-resize assertion are not counted as passes.

Production adoption, general project-wide source resolution, broader browser
and accessibility coverage, and a formal release remain separate gates.
