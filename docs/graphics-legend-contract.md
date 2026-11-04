# Legend Membership And Labels

Development product 1.15.0; public ABI 2.10 and graphics versions 1/2/3 are
unchanged. This extends the existing 2-D Line Legend object, not all MATLAB
legend syntax or layout. Rendering remains a host responsibility.

## Kernel State

Each Legend retains an ordered set of live Line IDs associated with its Axes.
Serialization does not reconstruct membership from every current Axes child.
`AutoUpdate` defaults to on and accepts canonical on/off text or scalar numeric
or logical 0/1. Off blocks automatic inclusion of newly added lines, not updates
to existing members. Deletion and reparenting remove obsolete members in both
modes. Turning on does not retroactively include lines added while off.

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

AutoUpdate reads use canonical character on/off, not MATLAB's OnOffSwitchState
enum identity. Categorical labels, rich text interpretation, explicit Line
subsets in legend(...), label creation arguments, name-value syntax, Legend
title objects, custom Position/Units and full layout are not implemented by
this follow-up. Candidate Canvas checks are not browser-pixel or production
adoption evidence. No tag or published release is created merely by this work.
