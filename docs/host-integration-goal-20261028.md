# Host Integration Kernel Goal

Status: active. Baseline: MParser 1.14.0, commit `9be51f9`.
Scope agreed on 2026-10-28: prioritize kernel requirements raised by
MExecServer/MExecClient, not unrelated MATLAB compatibility expansion.

## Evidence And Ownership

Inputs are the host's `docs/mparser-kernel-iteration-notes.md`,
`docs/v114-host-acceptance-status.md`, `docs/v114-host-iteration.md`,
`docs/v114-graphics-rendering.md` and `docs/v114-figure-export.md`.
Historical observations must be reproduced against the selected SDK.
Kernel execution semantics belong here; REST/UI rendering and interaction
belong to the host. No host regex parser, keyword blacklist or fake graphics
success may substitute for missing kernel contracts.

## Ordered Acceptance Matrix

| ID | Work | Acceptance | State |
| --- | --- | --- | --- |
| H01 | Reject empty assignment RHS and invalid keyword assignment | Parser and public compile API reject malformed source with useful source locations; positive syntax controls pass; compile has no workspace effects | Focused kernel and Windows host REST compile-only cases verified; shipped host package unchanged |
| H02 | Public variable-name validation | Side-effect-free SDK validation with explicit character, keyword and length rules; shared rules with supported source builtins; host rename can validate without executing assignment | SDK and Windows REST builtin checks passed; host rename adoption not shipped or verified |
| H03 | Definition metadata | C/C++ API exposes function/local/nested/method definitions, hierarchy, source identity and ranges with documented offset units; repeated names, UTF-8 and line endings tested | SDK and isolated host compile REST checked, including repeated nested names, CRLF/UTF-8 and class methods; deployed navigation and cross-file REST acceptance pending |
| H04 | Historical metadata export reconciliation | Re-run DynamicProperty descriptor/nested value/unrelated summary export and follow-up invocation through installed SDK and host; do not assume historical failure remains | REST replay passed with both host-adjacent and development DLLs; not a newly fixed kernel defect |
| H05 | Graphics lifecycle and matrix plots | Reproduce current Figure/Axes, replacement/hold/close behavior; implement missing agreed subset and column-wise multiple lines with handle arrays | Focused kernel and REST matrix/annotation/close probes passed; client v2 rendering and full downstream acceptance pending |
| H06 | Graphics identity and properties | Specify stable identities or snapshot mapping before property writeback; implement agreed title/grid/Legend/Clipping object semantics and serialization | Text/Legend/Clipping focused tests and opt-in v3 SDK identity checks passed; host selection, renderer adoption and writeback pending |
| H07 | Graphics export contract | Define figure/format/path validation and renderer adapter boundary; real format/file semantics or explicit unsupported errors, never relabel PNG as PDF | Explicit Figure file requests now have typed headless rejection; seven REST cases checked, no renderer or real encoding implemented |
| H08 | Downstream and release acceptance | Installed SDK clean rebuild, kernel regression, host REST and browser acceptance, current version/contracts/docs and GitHub state synchronized | Kernel/candidate 347/347 and clean installed-SDK host build/REST/67 regressions passed; browser, deployment and successor-version release acceptance pending |

H01 downstream evidence: `tools/probe-host-compile.ps1` ran the existing
Windows MExecServer executable in an isolated directory with the development
DLL, verified the loaded module path and SHA-256
`7143A2DE0362A3D7A9AB14437602966728C87DB127917858A8112053CD080277`.
REST compile-only accepted `a=1;` and rejected the missing RHS (line 2,
column 2) and reserved LHS (line 1, column 1) with compilation-phase errors
and the submitted source names. Execution was null and the workspace remained
empty for all three requests. Raw responses for the assertion-enabled run:
`build/host-compile-probe/c2fa4d0bcfab44b6834dffbae2627ef6`. This tests
the isolated host/kernel pair, not an installed release or browser UI.

## Delivery Gates

H04 replay: `tools/probe-host-compile.ps1 -MetadataOnly` ran the original
dynamic_properties_demo.m with an appended cell containing the live Computed
descriptor. REST exported both descriptors as opaque DynamicProperty handles
and the separate numeric summary as 68; a later command in the same session
read the nested descriptor's class/name and summary. No workspace export
failure on either DLL. Raw responses under `build/host-compile-probe/`:
`3634595c8c3944208da97c82c3d04037` (development DLL, SHA-256 above) and
`f3e9f9f5eed2423db371b7f273eaf6f9` (host-adjacent DLL, SHA-256
`9A01B73DED6AE6D30642DA209D40D5B29349345E571C967734B71C9D0AB83B24`).
This narrows the historical failure to an older configuration; it does not
identify the first fixing commit or imply metadata is fully serialized.

H02 downstream evidence: the same isolated host/kernel probe also asserts the
actual predicate results for an ordinary name, reserved keyword, contextual
name, invalid first character, namelengthmax and the 63/64-character boundary.
After seeding original=42, all five REST predicate requests left that value
and the singleton workspace intact. The three H01 controls passed in the same
run. Raw responses: `build/host-compile-probe/a1a4937706914e288ceb2a1586683a5b`.
This verifies the executable builtin path, not the REST host calling the C SDK
validator directly. The host's v114-workspace-conditional-delete.md explicitly
records that rename was not shipped; no rename operation or UI is claimed.

1. Establish minimal reproductions and distinguish missing implementation,
   missing host adoption and stale records.
2. Deliver H01/H02 first, then H03; reconcile H04 separately.
3. Implement verified H05/H06 gaps in dependency order, then H07.
4. For each slice record source revision, commands, actual results and remaining
   limitations. Kernel tests alone do not close downstream integration.
5. Preserve historical public contracts; version new public APIs and protocols
   consistently, with ABI/installed consumer/archive checks as appropriate.
6. Close the goal only when all agreed requirements have acceptance evidence;
   explicitly report any unavailable browser/platform/renderer evidence.

## Progress

- 2026-10-28: Read current host records and public SDK. Confirmed definition
  ranges are absent from SourceMetadata. Found existing DynamicProperty export
  coverage in `tests/cpp_api_smoke.cpp::runDynamicPropertyExportSmoke`; host
  failure remains a retest item, not a presumed unimplemented feature.
- 2026-10-28: Goal and acceptance matrix established. H01 investigation started;
  no new implementation or test pass is claimed at this point.
- 2026-10-28: Added malformed assignment parser regression; observed failure
  before the fix. Assignment fallback now emits diagnostics for missing RHS
  and invalid LHS. Rebuilt `mparser_smoke_tests` and
  `parser_semantic_fuzz_tests` in `build/v1.12-native-fresh`; parser smoke and
  CTest `parser_semantic_fuzz` passed. Positive controls include empty array
  assignment, indexed `end`, multiple outputs and keyword member access.
  H01 remains open pending public compile API, broader regression and host
  verification. These builds do not update the installed downstream SDK.
- 2026-10-29: Added public C++ compile regression for empty RHS, `end` as
  assignment target, absent LHS and numeric LHS. Each must produce an invalid
  module and a compilation diagnostic identifying line 2 in the named source.
  Positive indexed-`end` and keyword-member syntax remains accepted. Rebuilt
  all targets and ran `ctest --test-dir build/v1.12-native-fresh
  --output-on-failure -j 4`: 345/345 passed in 91.47 seconds, including installed
  C/C++ consumers and release archive tests. Log:
  `build/v1.12-native-fresh/Testing/Temporary/LastTest.log`.
  DynamicProperty public C++ export regression passed again as part of this
  run. H01 still needs downstream REST verification; H04 still needs host
  reconciliation. No downstream rebuild or new release is claimed.
- 2026-10-29: Executed the identifier baseline probe using local MATLAB R2024b
  (sandbox startup failed; approved outside-sandbox run exited successfully).
  Reproducer: `tools/probe_identifier_rules.m`. Observed maximum length 63,
  20 reserved keywords, rejection of leading underscore and char(233), and
  acceptance of arguments/properties/methods/events/enumeration/import.
  Current MathWorks documentation records the length increase to 2048 in
  R2025a: https://www.mathworks.com/help/matlab/ref/namelengthmax.html.
  The goal retains the project's R2024b baseline rather than adopting that
  newer rule silently.
- 2026-10-29: Added allocation-free internal identifier validator with explicit
  failure categories and locale-independent ASCII rules. Parser smoke tests
  pass for all reserved keywords, contextual names, embedded NUL, UTF-8 bytes
  and 63/64 length boundaries. Public ABI, builtins and contextual-name parser
  consistency remain to implement; this helper alone does not unblock host
  rename. The earlier 345/345 result predates this helper addition.
- 2026-10-29: Added C/C++ stateless variable-name validation and length query.
  ABI revision is now 2.6 with a new 142-symbol manifest; revision 2.5 symbols
  and snapshots remain intact. Rebuilt C/C++ API targets; `c_api_smoke`,
  `cpp_api_smoke` and `c_api_shared_library_abi` passed. Corrected stale CTest
  output regexes to use configured ABI values. Contract and product version
  synchronization, complete rebuild/regression, builtin integration, contextual
  name parser alignment and downstream acceptance remain open. The old
  v1.14 contract's live-header equality gate is expected to require a new
  versioned successor before release; do not modify historical snapshots to
  hide that difference. See `variable-name-validation.md` for the API boundary.
- 2026-10-30: Integrated isvarname/iskeyword/namelengthmax into the shared
  builtin registry, using the SDK's internal rules; source contract is 1.21.
  R2024b probe confirmed false (not type errors) for numeric, empty numeric,
  cell, character matrix, non-scalar string and missing-string inputs.
  Public SDK smoke passed for Bytecode/Portable/Native/Automatic after fixing
  the test to read logical values as uint8 rather than double. This was a test
  type mismatch, not an engine execution failure. Catalog snapshots,
  contextual-name parsing, full regression and downstream adoption remain
  open; the historical 1.20 catalog must not be overwritten.
- 2026-10-30: Contextual-name parser slice now accepts arguments/import in
  expressions and distinguishes assignment/indexing from declaration syntax.
  Parser tests cover script, function and control-block assignments and retain
  real arguments(Input) and import pkg.* declarations. SDK tests assign all
  six contextual names and verify sum 33 in all four backend modes. Rebuilt
  tests: parser_smoke, parser_semantic_fuzz and cpp_api_smoke passed (3/3).
  This is not complete contextual-name support: function parameters,
  workspace declarations, standalone arguments references and other grammar
  positions still need audit before H02 can close. Full regression and public
  contract synchronization remain open.
- 2026-10-30: Restricted arguments-block detection to function declaration
  sections; script and post-declaration bare arguments references now parse as
  expressions. Contextual names are accepted by workspace-declaration and
  function-name parsing. SDK execution with an arguments parameter and import
  local returned 7 on all four backend modes. Argument-suite rerun caught an
  incomplete modifier-list lookahead; fixed Input,Repeating/Output,Repeating
  handling. Rebuilt and reran parser smoke/fuzz, SDK smoke and six argument
  suites: 9/9 passed. Full grammar/public-contract and downstream audits remain
  open; this is focused evidence only.
- 2026-10-30: Added an internal definition directory separate from the callable
  function catalog. Collected directly from the merged syntax tree before
  semantic lowering, including primary/local/nested functions, class methods,
  source spans, parent function indices and declaration-only flag. Focused
  compiled_module_smoke passed for UTF-8 comment byte offsets, CRLF line
  positions, nested-parent ranges and a two-source graph with class methods.
  Public C/C++ export, prototypes/external-method and repeated-name coverage,
  range-unit documentation and host navigation remain open. No runtime
  execution is needed to obtain this directory.
- 2026-10-30: Exported definition directory through C/C++ SDK. Unpublished ABI
  2.6 now has 144 symbols. Rebuilt C/C++ targets; c_api_smoke,
  cpp_api_smoke and c_api_shared_library_abi passed (3/3). Tests verify C
  argument/range failures leave output untouched, nested ranges carry source
  identity and C++ copies outlive their module. Contract units and lifetime
  are documented in `definition-metadata.md`. Extended source-graph cases,
  installed consumer, host and release-contract acceptance remain open.
- 2026-10-30: Found that class-method attachment replaces prototype syntax.
  Changed definition collection to preserve each source before attachment;
  declarations and implementations now retain separate positions. Added
  external-method/prototype and repeated nested-name tests; compiled-module
  and SDK smoke logs confirm both passed.
- 2026-10-30: Installed consumer initially rejected stale ABI 2.5 expectations.
  Updated current consumer expectations to 2.6, package builtin metadata to
  1.21 and generated a new 343-descriptor catalog snapshot without changing
  historical 1.20. Added installed C++ calls for name validation and definition
  metadata. builtin_catalog_snapshot_smoke, installed_c_consumer_smoke and
  installed_cpp_consumer_smoke passed (3/3). Product version/new public contract,
  full regression, real host acceptance and GitHub synchronization remain open.
- 2026-10-30: Confirmed existing plot always creates a separate Figure/Axes
  tree. Added internal current-Figure and per-Figure current-Axes selection,
  lazy creation, deletion invalidation and reparent cleanup. Rebuilt and ran
  runtime_graphics_smoke: passed, including switching, recursive deletion,
  reparenting and rejection of a deleted Figure. Existing plot creation
  semantics and serialized schema remain unchanged in this slice; source
  commands, hold/replacement, matrix plots and protocol exposure remain open.
- 2026-10-31: Added figure/axes/gcf/gca source builtins for no-argument
  create/query and live same-graph handle selection. Lifecycle test exposed
  missing graphics identity comparison in VM and HIR; fixed graph-plus-ID
  comparison and HIR scalar equality dispatch. runtime_graphics_smoke passed
  for HIR and Bytecode selection plus equal/unequal handles. The unpublished
  1.21 catalog now contains 347 descriptors. Numeric Figure IDs, property-list
  forms, close/hold, matrix plots, array equality and host acceptance remain
  open. This does not claim complete MATLAB graphics command compatibility.
- 2026-10-31: Changed plot to target the current Axes, retaining Figure/Axes
  identity and invalidating replaced Line handles. Added Axes.NextPlot values
  replace, replacechildren and add, plus hold(), hold(ax), hold on/off and
  hold(ax, 'on'/'off'). Add mode derives limits from retained lines. Updates
  are staged before committing graph state; invalid lengths/properties preserve
  the existing graph and next ID. The initial staging implementation copies
  the graph; performance optimization remains separate from semantic acceptance.
  HIR/VM lifecycle, hold, reset/preserve properties, stale handles and atomic
  validation tests pass. Catalog 1.21 now has 348 descriptors; the catalog and
  graphics focused tests passed (2/2). Full regression is being re-run.
  Figure.NextPlot, manual limits, matrix plots, close, broader hold syntax,
  protocol identity, downstream acceptance and release synchronization remain
  open; these tests do not establish full MATLAB graphics compatibility.
- 2026-10-31: Full rebuild exposed an outdated C ABI layout revision assertion.
  Updated it to 2.6 and added size/offset checks for mparser_definition_info.
  The subsequent full CTest run passed 339/345 in 105.53 seconds. Failures were
  v1_14_public_contract_smoke, builtin_registry_smoke, embedding_unload_stress,
  graphics_api_smoke, c_abi_compat_demo_smoke and release_archive_smoke.
  Updated current builtin/ABI expectations and the SDK repeated-plot test to
  verify replacement data while preserving earlier immutable snapshots.
  Focused rerun of those four corrected tests plus c_api_layout_contract passed
  5/5. The v1.14 live-artifact hash gate and release archive's stale catalog
  metadata remain unresolved pending successor release/version synchronization.
  Historical release snapshots have not been rewritten. This is not a claim
  that the latest complete suite or downstream host acceptance is green.
- 2026-10-31: Implemented real dense matrix plot expansion: one input plots
  columns, two matrices require equal shapes, vector/matrix pairs select the
  matching matrix dimension (columns take precedence for square matrices).
  Rules were checked against https://www.mathworks.com/help/matlab/ref/plot.html.
  Added graph plotLines staging so replacement occurs once per batch, with
  one column-vector handle array returned for multiple lines. Invalid batch
  coordinate lengths and incompatible input dimensions preserve existing lines.
  New tests exposed HIR graphics arrays falling through to numeric indexing;
  connected graphics indexing to the existing runtime object index operation.
  HIR/VM matrix data, orientation, square-matrix precedence, array dimensions,
  invalid-input preservation, and public SDK graph/reference export tests pass.
  Latest focused CTest: builtin_catalog_snapshot_smoke, graphics_api_smoke and
  runtime_graphics_smoke passed 3/3. Catalog remains 348 descriptors (summary
  updated). The complete suite has not been rerun after these matrix changes.
  Empty-shape MATLAB parity, complex/sparse data, explicit Axes plot arguments,
  LineSpec/property forms, output-allocation fault injection, resource limits,
  rendering acceptance and successor release gates remain open.
- 2026-10-31: Added Axes/Line Clipping (default on), canonical on/off snapshot
  properties, text and real numeric/logical scalar 0/1 writes, and invalid-value
  rejection without mutation. Original coordinates and axis limits are not
  clipped or rewritten. HIR/VM tests cover defaults, write forms, rejected
  values, retained samples, hold preservation and replacement reset; SDK tests
  verify independent Axes/Line states and out-of-range sample export. Native
  runtime_graphics_smoke and graphics_api_smoke passed 2/2. See
  graphics-clipping-contract.md for host composition and pending acceptance.
  The source value remains a character vector, not MATLAB OnOffSwitchState.
  No browser/host renderer changes or pixel acceptance are claimed; Legend,
  real downstream tests, full regression and release synchronization remain open.
- 2026-10-31: Added close(), close(Figure or Figure array), and close all for
  current headless figures. Validates every explicit handle and runtime owner
  before deletion; recursively invalidates axes/line handles. With no figures,
  close succeeds without creating one. Optional status output is numeric 1.
  Source syntax references https://www.mathworks.com/help/matlab/ref/close.html;
  CloseRequestFcn, HandleVisibility/hidden/force, numeric Figure IDs and names
  are not implemented, so full MATLAB close compatibility is not claimed.
  HIR/VM tests cover current selection, fallback, recursive invalidation,
  invalid targets, arrays, all, and replot after close. Direct registry tests
  reject mixed-runtime arrays before deleting any figure. SDK tests confirm
  an empty post-close graph and unchanged previously captured result snapshots.
  Native runtime_graphics_smoke, graphics_api_smoke, builtin_registry_smoke and
  builtin_catalog_snapshot_smoke passed 4/4. Catalog 1.21 now contains 349
  descriptors / 351 names. Whole-suite, actual host and release acceptance
  remain open, as do callback semantics and allocation-fault rollback coverage.
- 2026-10-31: Added grid on/off, grid() toggle and grid(ax, mode) for current
  2-D Axes, including scalar numeric/logical 0/1 inputs. Axes XGrid/YGrid default
  off, export canonical strings and support individual property writes. HIR/VM
  tests cover default, toggle, explicit non-current axes, invalid-mode rejection,
  hold preservation and plot replacement reset. Registry/catalog/runtime tests
  passed 3/3; a separate rebuilt graphics_api_smoke passed with assertions for
  exported XGrid/YGrid. Catalog 1.21 now contains 350 descriptors / 352 names.
  Reference: https://www.mathworks.com/help/matlab/ref/grid.html. Minor grids,
  ZGrid, mixed-state MATLAB toggle parity, grid style/tick contracts, host
  rendering and full regression remain open. Title/xlabel/ylabel were not
  implemented as string-only shims: their Text return objects and Legend
  require an explicit object/serialization extension before semantic acceptance.
- 2026-10-31: Added the internal Text node and runtime graphics class
  matlab.graphics.primitive.Text, with String/Visible/Color properties and
  Axes parenting, reparenting and recursive invalidation. Graph snapshots that
  contain Text use graphics version 2; graphs containing only the existing
  Figure/Axes/Line types retain version 1. This version is per snapshot, not
  a graph identity or a session capability negotiation mechanism.
  Extended the development machine-result schema to accept Text in graphics
  v2 while rejecting Text mislabeled v1 and unknown graphics version 3.
  runtime_graphics_smoke passed, including runtime-value validation, property
  mutation, machine serialization and version rejection. Rebuilt machine
  protocol/SDK tests and current/historical schema tests passed 4/4.
  This is foundational work only: title/xlabel/ylabel source entry points,
  Axes label bindings, mixed child-array behavior, Legend, graph stable IDs,
  host v2 capability/renderer support, protocol/release version negotiation,
  complete regression and publication remain unfinished. Historical released
  schema snapshots have not been changed.
- 2026-11-01: Added internal Axes label bindings for Title/XLabel/YLabel.
  setLabel stages the graph update and reuses the existing Text identity for
  repeated writes to a role. Graphics v2 serializes an explicit labels map of
  snapshot-local Text references; bound roles omit the legacy string property
  to avoid two competing wire values. Reparent/delete clears the previous
  binding. Development schema rejects labels in graphics v1. Source title
  builtins and Axes label-property access are not connected yet; plot reset
  semantics for bound labels, host v2 support and release gates remain open.
  Rebuilt runtime_graphics_smoke passed, covering repeated identity, String
  mutation, label-reference schema validation, reparent/delete cleanup and
  invalid-role preservation. No full-suite or downstream acceptance is claimed.
- 2026-11-01: Connected title/xlabel/ylabel builtins for scalar text and
  optional explicit Axes handles, returning real Text objects. Explicit Axes
  calls do not change current selection. Invalid text/target validation occurs
  before label mutation; repeated calls reuse the role's Text identity.
  HIR/VM tests verify all three commands, repeated identity, String alias
  mutation, parent, non-current target, invalid input preservation and close
  invalidation. Registry/catalog/runtime tests passed 3/3. Rebuilt SDK test
  passed 1/1, checking graphics v2 Title/XLabel references, Text contents and
  returned handle serialization. Catalog 1.21 now has 353 descriptors/355 names.
  This does not close label compatibility: Axes.Title/XLabel/YLabel source
  property access still uses legacy values, plot replacement/replacechildren
  handling for labels is unfinished, and multi-line/numeric labels, formatting
  options, mixed children, host v2 rendering, Legend, whole-suite and release
  acceptance remain open. No title rendering in the actual host is claimed.
- 2026-11-01: Axes.Title/XLabel/YLabel reads now return bound Text handles,
  lazily creating an empty label if needed. Label function calls reuse that
  identity; alias.String mutations are visible through the function result.
  Direct legacy string assignments to Axes label properties are rejected
  rather than silently updating a second, unused string. This is a deliberate
  behavior change requiring release documentation. replacechildren preserves
  bound labels; replace resets their Text properties without replacing handles.
  HIR/VM tests now assert lazy identity, member reads, alias mutation, rejected
  assignment preservation, label retention/reset and identity after plotting.
  Rebuilt runtime_graphics_smoke and graphics_api_smoke passed 2/2.
  Nested member writes such as ax.Title.String, assigning replacement Text
  handles, mixed child arrays, label hidden-handle parity and MATLAB lifecycle
  comparison still need validation. Legend, host v2 rendering, full regression,
  release protocol synchronization and publication remain unfinished.
- 2026-11-02: Nested label writes initially failed in two paths: VM copyback
  rejected the same Text handle, and HIR lvalue transactions had no object
  member hooks. Added graphics-only HIR member hooks and same-bound-handle
  copyback support. Replacement Text handles remain explicitly unsupported.
  Copyback identity checks inspect existing bindings without lazily creating
  a label, so a rejected replacement does not mutate the graph. Initial
  runtime_graphics_smoke and lvalue_runtime_smoke rerun passed 2/2. Expanded
  tests cover ax.Title.String, ax.Title.Color, invalid nested colors and
  rejected replacement preserving an unbound label. Full suite, host rendering,
  Legend and release synchronization remain open.
  Expanded native rerun passed 3/3: lvalue_runtime_smoke, graphics_api_smoke
  and runtime_graphics_smoke. This is focused evidence, not full acceptance.
- 2026-11-02: A new Axes.Children test after title/xlabel/ylabel reproduced
  an incompatible-common-class error from mixing Line and bound Text objects.
  Checked MathWorks' preventing-access-to-figures-and-axes documentation:
  bound titles/axis labels are accessed through dedicated Axes properties,
  not public Children. Changed public handle children enumeration to omit
  bound labels while retaining internal ownership edges and complete snapshot
  objects/labels references for rendering and recursive deletion. This is not
  a general heterogeneous-graphics-array implementation. Detached/reparented
  ordinary Text mixed with Line and general HandleVisibility remain open.
  Tests cover public children with plotted lines, label-only axes and deleting
  an owning Axes; the public machine graph remains an ownership graph rather
  than a literal copy of the source Children property.
  Rebuilt runtime_graphics_smoke and graphics_api_smoke passed 2/2. Reference:
  https://www.mathworks.com/help/matlab/creating_plots/preventing-access-to-figures-and-axes.html.
  Actual MATLAB R2024b replay, full regression, host and release acceptance
  remain pending.
- 2026-11-02: Added internal matlab.graphics.illustration.Legend nodes with
  Figure parenting, Visible, Location and Orientation properties. Location
  defaults northeast; validates named 2-D inside/outside/best positions and
  orientation vertical/horizontal. Numeric/logical scalar 0/1 Visible writes
  now normalize to on/off for existing graphics types as well. Legend records
  require graphics v2 and are rejected when labeled v1. Native graph/runtime
  tests verify value contracts, parent rejection, properties, error preservation,
  serialization and recursive Figure deletion; rebuilt runtime_graphics_smoke
  and graphics_api_smoke passed 2/2. Reference:
  https://www.mathworks.com/help/matlab/ref/matlab.graphics.illustration.legend-properties.html.
  This is not a completed legend feature: legend() entry point, associated
  Axes reference, entries/labels, auto-update, Axes reparent/delete tracking,
  mixed Figure children, custom Position/layout, host rendering and full
  release acceptance remain unfinished. No host-generated legend is authorized
  solely by Line.DisplayName. Historical schema snapshots remain unchanged.
- Subsequent development: associated each requested Legend with one Axes while
  retaining its Figure parent. Repeated requests reuse identity; moving/deleting
  the Axes moves/deletes its Legend without affecting other axes. Graphics v2
  exports the snapshot-local axes reference and ordered entries (line reference,
  label), using current Line.DisplayName or a numbered fallback. Exposed
  legend(), legend(ax), show/hide/off with optional Axes. SDK script checks
  hide/show identity, removal without deleting the Line, and recreated entries.
  Runtime graph tests cover rename/delete refresh and Axes lifecycle. Registry,
  catalog, graphics SDK and runtime focused tests passed 4/4; catalog 1.21 has
  354 descriptors / 356 names. Explicit label lists, selected Line subsets,
  AutoUpdate control, cross-snapshot identity, downstream rendering, full
  regression and successor release acceptance are not established by this run.
- H05/H06 host transport (2026-11-02): the real MExecServer executable with
  development DLL SHA256 `7143A2DE0362A3D7A9AB14437602966728C87DB127917858A8112053CD080277`
  passed the matrix, annotated and closed graphics assertions in
  `tools/probe-host-compile.ps1` over loopback REST. The three raw responses
  are in local evidence directory `build/host-compile-probe/1facc90683da4e2f820919a31de720d3`.
  This verifies transport for this pairing, not browser rendering, persistent
  object identity across snapshots, or the published package.
- H06 identity counterexample: in the same REST session, the first Line's
  snapshot id was 3 before and after annotation. After close all the graph
  was empty; recreating the matrix plot produced a new valid Line with id 3
  while the original handle remained invalid. Raw responses:
  `build/host-compile-probe/9ea83f51f35e4d64bba256b3756f06fb`.
  Version 1/2 record ids are assigned afresh by serialize() and must not be
  used as cross-request write targets. A successor identity contract must
  distinguish replacement even with identical plot data and reject stale
  targets; the existing numerical ids remain graph-local references.
- H06 native lifetime regression (2026-11-03): rebuilt
  `runtime_graphics_smoke_tests` and ran `runtime_graphics_smoke` (1/1 passed).
  New coverage verifies alias identity across property mutation, invalidation
  on Figure deletion, replacement identity, rejected stale writes with an
  unchanged graph, null serialized references for deleted handles, and distinct
  identities for equal numeric IDs in separate graphs. It also deliberately
  demonstrates reference reuse in legacy snapshots. Native handle safety is
  verified here; persistent wire identity and a host writeback API remain open.
- H06 opt-in identity implementation (2026-11-03): the internal graph serializer
  now supports version 3 with string graphIdentity/objectIdentity pairs while
  default public output remains v1/v2. Rebuilt runtime_graphics_smoke passed
  (1/1), checking mutation stability, replacement distinction, graph isolation
  and unchanged legacy serialization. See graphics-identity-contract.md for
  loaded-kernel lifetime boundaries. Public SDK/schema negotiation, host
  adoption and identity-targeted writeback remain pending; the published DLL
  and REST output have not acquired this capability from this focused build.
- H06 v3 validation: added standalone graphics-v3.schema.json and wired it into
  runtime_graphics_smoke. Rebuilt test passed (1/1), including missing identity,
  numeric/zero/leading-zero/negative/fraction identity rejection and legacy
  snapshot rejection by the v3 validator. Partial deletion changes the surviving
  Line snapshot reference but preserves its object identity; an empty graph
  preserves its graph identity. Schema checks are structural, not live-target
  authorization. Public result schema remains unchanged in this step.
- H06 value SDK (2026-11-03): C ABI 2.7 now exports
  mparser_value_graphics_json_versioned; C++ Value::graphicsJson(3) selects
  identity-bearing scalar/array graphs. Version 0 and the existing C function
  retain legacy output. Unsupported versions reject with null output. New
  revision-7 symbol inventory has 145 symbols; older inventories remain intact.
  Rebuilt C/C++ smoke, unload stress, shared ABI, layout, graphics SDK and
  graphics runtime checks passed 7/7. This does not yet expose v3 at Result
  level or select it in MExecServer; host negotiation/writeback, installed
  package acceptance, full regression and release synchronization remain open.
- H06 result SDK: added mparser_result_graphics_json_versioned and
  Result::graphicsJson(3) to the unreleased ABI 2.7 (146 symbols). Both legacy
  and identity snapshots are captured under one graph lock and retained by
  the result. Rebuilt shared ABI, graphics SDK and runtime checks passed 3/3.
  SDK tests verify no-output plots, replacement identity, frozen earlier
  results, close/reset identity behavior, post-runtime lifetime, missing
  snapshots and invalid version/null argument handling. MExecServer still
  calls legacy graphicsJson() in workspace_session.cpp and value_json.cpp;
  downstream version selection and client adoption are not validated.
- Downstream inspection (2026-11-03): MExecClient web/js/plot.js currently
  rejects any snapshot whose version is not 1 at renderGraphicsSnapshot entry.
  Thus successful REST v2 transport does not establish Text/Legend rendering;
  v3 must not be enabled unconditionally for this client. Host/client protocol
  acceptance must cover v2 labels/legends and v3 identity explicitly, with a
  runtime/session generation boundary. This is a downstream gap, not permission
  to strip kernel objects or relabel an extended graph as v1.
- Full development validation (2026-11-03): complete build succeeded (203
  incremental actions); CTest finished 343/345 passed, including installed C
  and C++ consumers. Remaining failures: v1_14_public_contract_smoke detects
  changed builtin_registry.h against historical/current v1.14 artifact hashes;
  release_archive_smoke still requires active builtin catalog 1.20 with
  340 descriptors / 342 names, whereas development is 1.21. The release
  successor metadata and active-package assertions must be synchronized without
  rewriting historical snapshots. Evidence: build/v1.12-native-fresh/Testing/
  Temporary/LastTest.log and LastTestsFailed.log. This is not a green release.
- Follow-up: release_archive_smoke passed after pinning the 1.21 catalog
  (354 descriptors / 356 names), retaining the historical catalog hashes,
  and updating the installed headless graphics example to use Text label
  functions and an explicit second Figure. Direct CLI execution exported one
  Figure, one Axes, one Line and three Text nodes. Archive-only rerun: 1/1;
  the v1.14 public-contract gate and successor release remain unresolved.
- H07 baseline: same-session REST calls after a verified matrix plot used
  print(gcf(), absolutePath, '-dpdf') and saveas(gcf(), absolutePath, 'pdf').
  Each returned runtime-failed with "unknown bytecode runtime variable" naming
  the requested command, and neither target existed afterwards. This proves
  no false PDF success for these cases, not a renderer or an export-specific
  unsupported diagnostic. Local raw responses:
  build/host-compile-probe/1c0ca19f468c40018002e42a8abdebe8.
  Loaded development DLL SHA256:
  1974FE684B10B466229E75FD85094ABE357F56A667B035817EE713BE57419709.
- H07 follow-up: registered print and saveas in the development catalog (1.21,
  356 descriptors / 358 names). Explicit Figure, textual file path, PNG/PDF
  device or format are checked before returning GraphicsRendererUnavailable.
  This headless subset performs no file I/O, has no encoder, and does not claim
  printer/implicit Figure/other MATLAB option support. REST probe verified
  missing and existing PDF targets, unchanged existing bytes, deleted/non-Figure
  handles, empty path and unsupported device. Raw responses:
  build/host-compile-probe/9f673d4457b94205986c65402375992c.
  Registry, catalog snapshot and development contract checks passed 3/3.
- H06 isolated MExecServer integration: compiled and linked the server sources
  with the two v3 SDK call sites enabled and ran probe-host-compile.ps1 with
  -GraphicsV3. REST graph 3 exported Line identity 3 before/after annotation
  and identity 9 after close/recreate, despite local reference 3 in all three
  nonempty snapshots. Workspace array references resolved to the same graph
  and Line identity as the result. Raw responses:
  build/host-compile-probe/831df21e474247e88b4d37b875c239c6.
  This verifies the opt-in server binary, not deployment, browser rendering,
  request negotiation, or property writeback.
- CI/main integration: commit 4016567 passed Documentation CI (41573050568)
  and Cross-platform CI (41573039268) on the milestone branch. Six executed
  platform jobs, including ASan/UBSan, succeeded; QEMU and authentication-candidate
  jobs were skipped by workflow conditions, not verified by these runs. The
  commit was fast-forwarded and pushed to main without including unfinished
  client candidates. Subsequent work continues on main as requested.
  The main runs also succeeded: Documentation CI 41574081644 and
  Cross-platform CI 41574081515, both for 4016567. A subsequent complete local
  rebuild and CTest run finished 346/346 passed (100.59 seconds), including
  frozen/development contracts, installed consumers and release archive checks.
  Test evidence does not constitute a new product release or browser acceptance.
- Client candidate replay: probe-client-rendering.ps1 applies four explicit
  patches to an isolated copy of the existing MExecClient renderer/test. The
  legacy suite and v2/v3 Text, per-Line clipping, independent grids and explicit
  Legend tests passed. Legend checks cover order, styles, rename, visibility,
  deletion, graph references, orientation and long outside labels at 320x240.
  Clean replay evidence: build/client-rendering-probe/0ecc787c41b54960833f6b5f18c947a0.
  This is simulated-canvas evidence only: no deployed-client or real-browser
  rendering acceptance, selection persistence or full Legend layout parity.
- H03 isolated host compile integration: an opt-in patch calls the SDK
  definition directory directly from /compile and exposes units, completeness
  and exact request text alongside the records. REST checks passed for repeated
  nested names with distinct parents, class methods, CRLF and UTF-8 byte offsets,
  every begin/end line/column, a script that must not execute and invalid source
  with an incomplete empty directory. Raw evidence:
  build/host-compile-probe/56610d516503487ba260eca175f3c484. This does not verify
  deployed UI navigation, stale-document rejection or cross-file REST indexing.
- Installed SDK documentation now includes definition metadata, variable-name
  validation and graphics clipping guides, each exposed through a CMake path
  variable and pinned in the development artifact manifest. Frozen v1.14,
  development manifest, installed C++ consumer and relocated archive checks
  passed 4/4 after this packaging change. No historical snapshot was rewritten.
- Clean downstream acceptance: probe-clean-host.ps1 completed in the fresh
  build/clean-host-probe/ddb1df19182d40db93f346e910a252c2 directory. It copied
  source directories only, applied the three host and four client candidates,
  enabled graphics v3 and definition metadata, and built all 148 host/dependency
  actions with MSVC Release and host warnings-as-errors. All SDK inputs came
  from the relocated installed package, not the kernel source include tree or
  old host object files. The selected DLL hash was
  655A551409F5860B48CBC9C693DE5AF34BA4778825C663C00AFA3A927CF99F76.
  REST checks passed (build/host-compile-probe/5c3e664ced3e49c39285dc785af8c2bd),
  the simulated client candidate tests passed, and host CTest finished 67/67
  passed in 19.84 seconds. The initial clean attempt found a hard-coded ABI
  2.5 regression expectation; the acceptance patch now checks the exact
  configured SDK ABI and advertises v3 only when enabled. The original host,
  client and deployed service were not modified. Browser visuals, production
  integration, request negotiation, writeback and new product release are not
  established by this clean-build test.
- Continuous renderer regression: test-only frozen MExecClient inputs are now
  pinned by normalized hashes and replayed by host_graphics_candidate_smoke.
  CTest additionally executes the built kernel on host_graphics_scene.m and
  passes its actual v2 graph to the candidate renderer, checking matrix YData,
  Text labels, independent grids, Line clipping and explicit Legend entries.
  Full local CTest passed 347/347 (45.61 seconds). A standalone Node CI job
  checks patch replay without the sibling host checkout. This adds continuous
  simulated-canvas coverage, not browser visual acceptance. Tabbit diagnose
  returned exit 1 without a usable connection; settings were not changed.
  Previous main commit d637f75 passed Documentation CI 41585463047 and
  Cross-platform CI 41585463024.
