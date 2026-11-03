# Variable Name Validation

Development API, introduced in C ABI generation 2 revision 6 and retained in
the 1.15.0 development SDK (ABI 2.8). Product identity and the active development
manifest are synchronized; this checkout is not a published release candidate.

`mparser_validate_variable_name(mparser_utf8_view, out_status)` performs a pure,
allocation-free check without compiling source or creating a runtime. The C++
wrapper is `mparser::sdk::validateVariableName(std::string_view)`.
Query the supported limit using `mparser_variable_name_max_length()` or
`mparser::sdk::variableNameMaxLength()` instead of hard-coding it in a host.

## Rules

The compatibility baseline is MATLAB R2024b: 63 ASCII characters maximum,
an English letter first, then English letters, decimal digits or underscore.
Names are case-sensitive. Reserved language keywords are rejected. Contextual
words including `arguments`, `properties`, `methods`, `events`, `enumeration`
and `import` are not reserved keywords. Builtin names are not reserved either.
This is validation, not normalization: no trimming, truncation or replacement
is performed. Embedded NUL and non-ASCII UTF-8 bytes are invalid.

Reasons are checked in this order: empty, too long, invalid initial character,
invalid character, reserved keyword. Otherwise the result is valid. C API
argument errors (null output, or null input with nonzero size) return
`MPARSER_API_STATUS_INVALID_ARGUMENT` and leave the output untouched. Invalid
names themselves return `MPARSER_API_STATUS_OK` with a reason code.

The validator does not check workspace collisions, revision conflicts, value
ownership or rename atomicity; these remain host workspace concerns. Validation
is not a promise that a name is legal in every source grammar position. R2024b
itself imposes additional context-sensitive declaration restrictions below.

Source builtins `isvarname`, `iskeyword` and `namelengthmax` use the same
internal rules (builtin source contract 1.21; catalog snapshot included in the
development manifest, formal release pending).
`isvarname(value)` and `iskeyword(value)` return scalar logical false for
non-text, non-scalar string, missing string or non-row-character inputs.
`iskeyword()` returns the 20 reserved keywords as a 20-by-1 cell array of
character vectors. `namelengthmax()` returns double 63. Invalid argument
counts are rejected by the builtin registry.

## Evidence

`tools/probe_identifier_rules.m` reproduces the local R2024b baseline.
`tests/parser_smoke.cpp` tests the internal rules; `tests/cpp_api_smoke.cpp`
exercises both the public wrapper and C entrypoint argument failures.
The shared-library ABI test verifies the two added exports alongside the
previous 140 exports. Historical revision-5 manifests remain unchanged.

## Source Contexts

`tests/contextual_identifiers.json` fixes 90 R2024b positive/negative cases for
the six contextual names across transpose, parameters/outputs, anonymous and
named handles, input/output argument declarations, name-value arguments,
for/global/persistent bindings and class properties. The reference probe
`tools/probe_contextual_identifiers.m` runs the same corpus in MATLAB, saving
the actual release/version, corpus and raw outcomes under build. CTest
`contextual_identifier_smoke` replays all cases in four requested backend modes
(off/portable/native/auto), saving raw JSON and input hashes. A requested native
backend may use the kernel's normal fallback; this is not proof that every
expression was compiled to native machine code.

The corpus includes 83 successful executions and seven required rejections:
`arguments=...` at the start of a function declaration section is interpreted
as a malformed arguments-block header, even when arguments is a parameter or
output. Once an executable statement ends that section, the same variable
assignment is legal. In class property declarations, arguments/import are
accepted but properties/methods/events/enumeration remain reserved there.
Those restrictions must not be removed just because isvarname returns true.
Other grammar contexts still require separate acceptance; this fixed corpus is
not a full language-compatibility claim.

## Host Rename Candidate

`tools/host-integration/mexecserver-workspace-rename.patch` uses this validator
for an opt-in, revision-checked REST operation. The host checks collisions and
renames only the binding under its existing session lock, leaving the SDK Value
untouched. It does not compile or execute an assignment and does not reconstruct
the value from JSON. The default route remains disabled; this is not a deployed
host or client update. See `tools/host-integration/README.md` for the candidate
contract, reproduction and acceptance boundaries.
