# Legend Membership And Labels

Development product 1.15.0; public ABI 2.12/158 and existing graphics formats
are unchanged by this follow-up. Builtin source contract is now 1.22. This
extends the existing 2-D Line Legend object, not all MATLAB
legend syntax or layout. Rendering remains a host responsibility.

## Kernel State

Each Legend retains an ordered set of live Line IDs associated with its Axes.
Serialization does not reconstruct membership from every current Axes child.
`AutoUpdate` defaults to on and accepts canonical on/off text or scalar numeric
or logical 0/1. Off blocks automatic inclusion of newly added lines, not updates
to existing members. Deletion removes obsolete members in both modes. Automatic
membership drops Lines moved out of its Axes; explicitly supplied membership
retains those references across reparenting, matching the creation reference.
Turning on does not retroactively include lines added while off.

At inclusion, an unnamed Line receives the first unused dataN name among the
Legend's members. This writes its actual DisplayName; deleting another member
does not renumber it. A later DisplayName write, including empty text, is read
back and serialized as the real label. Mixed named/unnamed examples produce
measured, data1 rather than a guessed data2 based on child ordinal.

`String` reads return a cell row of character vectors. Writes accept text,
cell text or nonmissing string arrays in logical order. They update the
corresponding member Lines' DisplayName properties. Shorter lists reduce the
membership to that prefix; longer lists ignore excess labels, without adding
excluded Lines. An empty cell list leaves membership unchanged, matching the
reference. Validation precedes mutation, and multi-label changes are staged
before committing. Internal membership uses IDs only, never strong handles.

Snapshots continue to use explicit entries with snapshot-local Line references
and label strings; v3 retains persistent graph/object identities separately.
The computed String list is not duplicated as a new wire property type.
AutoUpdate is a string property in existing extensible property maps.

## Creation And Explicit Membership

`legend(label1,...,labelN)`, cell/string label arrays and character-matrix rows
are accepted, optionally preceded by a scalar Axes and/or Line array. Line
arrays use logical column-major order, including repeated references and Lines
in other Axes/Figures of the same runtime. Without an explicit Axes, the first
subset Line identifies the associated Axes. This does not change current Axes
selection. Empty numeric/typed subsets mean the default candidate set, as in
R2024b. A bare/show call preserves an existing subset; nonempty creation labels
without a subset rebuild membership from the associated Axes' Lines.

Creation labels update the corresponding Lines' DisplayNames. Short lists
select a prefix; extra labels are ignored and emit
`MATLAB:legend:IgnoringExtraEntries` through the real warning context. Empty
labels exclude their corresponding Lines without overwriting an existing
name. An empty cell list preserves/retrieves default membership. These are
creation rules, not a redefinition of every `String` setter rule above.

Automatic dataN names are tracked separately from explicit names. Repeated
unnamed handles consume successive names in one creation call; both repeated
entries then read that Line's final name. Generated names are cleared when no
Legend retains them, while user-assigned names survive. Explicit members are
ID references only, not ownership edges. Removing a Line deletes all repeated
entries; deleting an associated Axes deletes its Legend, not foreign Lines.

Supported creation properties are Location, Orientation, AutoUpdate and Visible,
using name/value pairs or `Name=Value`; property names are case-insensitive.
Documented compass abbreviations and measured h/v orientation aliases are
canonicalized. Duplicate property assignments apply in order. Creation makes
the resulting Legend visible, even after a Visible='off' argument, as measured
in R2024b. `toggle` and case-insensitive show/hide/off modes are supported.
All argument and property validation is staged on a temporary graph before
commit: invalid creation does not manufacture a Figure/Axes or change an
existing Legend, names, selection, object IDs or snapshots.

The graph record keeps the same ordered entries format. An entry can reference
any live Line in the graph and may repeat a Line; containment is still acyclic.
The companion client candidate resolves entry styles through the whole object
table, not only associated-Axes children. This is not permission to draw a
foreign curve as an Axes series or infer membership from DisplayName.

Graphics `delete` and `isvalid` now share scalar/array dispatch in HIR and VM.
Matrix/empty shapes are retained by isvalid's logical result; duplicate/deleted
handles are safely handled by delete. Graphics arrays no longer fall into
file-delete dispatch. No host source parser or filename rewrite is involved.

## Evidence And Boundaries

`tools/reference/legend/run_legend_reference.m` records 14 actual states on
MATLAB 24.2.0.2712019 (R2024b). `tests/legend_reference.json` freezes labels,
AutoUpdate and Line DisplayName values. An assertion-enabled second run passed
at `build/legend-r2024b-checked.log`. The online
[Legend properties](https://www.mathworks.com/help/matlab/ref/matlab.graphics.illustration.legend-properties.html)
describe the update/deletion rules, but the installed R2024b run anchors these
examples rather than the current online release label.

`legend_state_smoke` replays all states across four requested SDK backend modes,
checks String getter shape/values, immutable historical snapshots and invalid
write atomicity. `runtime_graphics_smoke` checks member removal/reparenting and
snapshot topology. The clean-host runner includes `test-legend-state.cjs`,
which repeats the 14 phases and independent later-command String reads via REST.
Complete rebuilt kernel/package regression passed 357/357 in 46.55 seconds at
`build/legend-final-test.log`. The installed SDK selected DLL SHA256 is
`C56151B5299A037413A7BB7529A86F788EFD9050217392955F150D027C5330CC`.
Fresh source-only installed-SDK host acceptance passed 148 build actions,
dedicated REST/client probes and 67/67 regressions (19.85 seconds). Stage:
`build/clean-host-probe/3c666eb6ad85436895207643b88a4f13`; aggregate log:
`build/legend-clean-host.log`. Its loaded DLL matches the selected hash above.
Actual-commit GitHub CI remains a separate gate.

The new `run_legend_creation_reference.m` records 80 cases on the same actual
R2024b installation; `tests/legend_creation_reference.json` was independently
assertion-replayed at `build/legend-creation-r2024b-80-checked.log`.
`legend_creation_smoke` checks HIR/VM graph state, names, selection, warnings
and v3/v4 schemas. `legend_creation_api_smoke` replays the corpus in four
requested SDK backend modes and checks frozen results/invalid/foreign targets.
`test-legend-creation.cjs` is the source-copy installed-SDK REST gate, including
independent later-command String reads. The complete rebuilt kernel/package
regression passed 362/362 in 65.88 seconds at
`build/legend-creation-final-full-test.log`. The fresh source-only installed-SDK
host passed all 148 build actions, all REST/client probes and 67/67 tests in
27.61 seconds at `build/clean-host-probe/a3cce8c27a944401ab82b4d4789a3257`.
Aggregate log: `build/legend-creation-clean-host.log`; loaded DLL SHA256:
`9689A754300DF313D828F7FD9692C6D9447426AC05D5CFC2EF85AAD1A67AE298`.
Bounded old SDK ABI 2.11 OFF and old-header/current-DLL layout-absence/writeback
controls passed, old SDK ON was rejected and restored current SDK ON passed
layout plus 67/67 in 20.62 seconds at `build/legend-creation-compat.log`. This
is not a full old-header host regression with the new DLL. Final selected
documentation/installed/archive checks passed 10/10 in 65.98 seconds at
`build/legend-creation-final-package-test.log`. The later
[actual compact/Legend browser follow-up](host-compact-layout-candidate.md)
checks repeated/cross-Axes keys, rename, AutoUpdate off, delete and invalid-call
preservation with this DLL. Full Legend layout, deployed adoption and
actual-commit GitHub CI remain separate gates.

AutoUpdate reads use canonical character on/off, not MATLAB's OnOffSwitchState
enum identity. Categorical/missing labels, rich text interpretation, unlisted
creation properties, boxon/boxoff, Legend title objects, custom Position/Units
and full layout are not implemented by this follow-up. Error identifiers and
legacy argument-ambiguity warning wording are not all MATLAB-identical. The
extra-label warning is checked explicitly. Simulated Canvas tests are not
browser-pixel evidence; the bounded actual run above is not production
adoption. No tag or published release is created merely by this work.
