# Versioning And Development Policy

MParser has one product version shared by the engine, CLI, libraries, headers,
CMake package, and installed SDK. Technical boundaries also carry independent
contract metadata where a consumer must negotiate a binary or wire format.
Those numbers are never SDK product versions.

## Product And SDK

MParser uses semantic versions for release tags and packages. The active
source tree, product metadata, and installed SDK report candidate version
`1.3.0`. The complete fifteen-batch v1.3 train has reached its internal
candidate gate. Subsequent v1.4-v1.9 internal development batches extend that
candidate without changing the product or public C/C++ API identity.

This project is not currently using the v1.3 interfaces in production.
Implementation, in-repository consumers, tests, samples, and documentation may
change together without compatibility wrappers for superseded development
interfaces. This keeps the current design small while its numeric, embedding,
and function surfaces are still settling.

The published MParser 1.0.0 and frozen v1.2 contracts remain immutable
historical evidence. They do not require compatibility wrappers for superseded
unreleased development interfaces.

## Current Contract Identifiers

| Boundary | Current identifier | Meaning |
| --- | --- | --- |
| MParser product and SDK | `1.3.0` candidate snapshot | User-facing release identity |
| Production CLI | 1.0 | Command, option, channel, and exit contract |
| C source API | 1.3 | Header-level source contract for C hosts |
| C ABI | generation 2, revision 2; archived v1.3 revision 1 and v1.2 revision 0 | Binary layout, symbols, ownership, and calling convention |
| C++ source API | 1.3 | Header-level source contract over the C ABI |
| Machine result protocol | `mparser.result` 1.1 | JSON producer/consumer contract |
| Builtin source contract | 1.17 | Registry/descriptor/call semantics compiled with the engine |

## CLI 1.0

The production command is `--run`. Its `--jit=auto|off|portable|native`
selector, invocation arguments, machine-result selector, source-path rules,
channel behavior, and exit classes are the normal user boundary. Automation
uses `--run --result-format=json-v1` instead of parsing human output.

Diagnostic token, HIR, bytecode, planning, profiling, and historical
`--run-*` modes may change during development. They are engineering tools, not
separate execution products.

## C ABI Generation 2

`MPARSER_C_API_VERSION_MAJOR/MINOR/PATCH` report source API `1.3.0`.
`MPARSER_C_ABI_GENERATION` contains generation `2`, and
the active header's `MPARSER_C_ABI_REVISION` contains revision `3`.
`mparser_c_abi_generation()` and `mparser_c_abi_revision()` expose the same
binary compatibility identifiers at runtime. The generation terminology is
deliberately distinct from the MParser product version and source API.

Generation 2 directly transports every core numeric class and separate
complex real/imaginary buffers. The current SONAME/install-name major is 2.
Caller-sized root records permit future tails; fixed-stride records remain
sealed. Exact rules and validation are in `c-abi-compatibility.md`.

The frozen v1.2 candidate is generation 2 revision 0 with 109 exports. The
v1.3 candidate added eight context-related exports and froze revision 1 with
117 exports. v1.10 added seven shared-Runtime exports (revision 2, 124 exports).
The current v1.11 development tree adds ten debugger exports (revision 3, 134
exports) and an optional caller-sized invocation tail. It retains
generation/SOVERSION 2 because all earlier prefixes and symbols remain present. The v1.2 and v1.3
snapshots are archive evidence, not live header inputs.

Repository consumers move with unreleased development headers. An incompatible
binary change advances the generation; an additive change within a frozen
generation advances the revision. The product/source API level is updated at
the milestone gate rather than for each internal batch.

## C++ Source API 1.3

`include/mparser/cpp_api.hpp` is a header-only C++20 facade over C ABI
generation 2. Its source API follows the v1.3 product line to avoid presenting
an unrelated SDK 2.0 identity. It does not promise a C++ binary ABI, and no STL
object or C++ class layout crosses the shared-library boundary.

The v1.3 candidate header, C dependency snapshot, and relocated
multi-translation-unit consumer are frozen together. They include rooted
system contexts over ABI revision 1. The current v1.x header additionally
exposes shared Runtime ownership over revision 2. Unreleased development interfaces may
move together without compatibility wrappers.

## Machine Result Protocol 1.1

`mparser.result` has an independent wire major/minor pair. Protocol 1.1 adds
numeric-class metadata, exact fixed-width integer JSON values, and separate
imaginary data for complex double/single values. Existing major-1 framing,
status, diagnostics, summaries, and emergency behavior remain defined by the
schema and current 1.1 snapshot.

Consumers check the protocol major and tolerate documented additive minor
fields. A change that removes a required field or changes its meaning requires
a protocol-major change.

## Builtin Source Contract 1.17

`BuiltinRegistry`, `BuiltinDescriptor`, `BuiltinCall`, and `BuiltinResult`
form a source-integration contract for builtins compiled with the engine. They
are not an installed plugin ABI and do not expose stable C++ object layouts.

Contract 1.0 defined naming, aliases, registry freezing, arity, value/shape
constraints, context permissions, side effects, determinism, exception
conversion, diagnostics, ownership, and typed-lowering eligibility. Contract
1.1 adds the host output and execution-context permissions used by the v1.2
runtime without creating an external binary ABI. The archived 1.0 catalog has
118 descriptors. The frozen v1.2 catalog has 166 descriptors and 168
registered names, including the `Inf` and `NaN` aliases. Incremental v1.3
source-contract revisions add deterministic system contexts, dynamic source
evaluation, callbacks, file/MAT services, native C++20 numerical families,
conversion, set, and text-query metadata. The v1.3 contract 1.10 catalog has
275 descriptors and 277 registered names. Contract 1.11 added the declarative
`sum` Typed reduction identity. Contract 1.12 added the declarative `prod` and
`mean` Typed reduction identities used by guarded Dense regions. Active contract
1.13 adds the shared native C++ datetime/duration family: explicit civil-date
and H/M/S construction, `NaT` arrays, component and unit operations, temporal
predicates, ISO formatting, and VM/interpreter arithmetic. Contract 1.14 adds
the native C++ CSC sparse family (`sparse`, `spalloc`, `speye`, `spones`,
`full`, `nonzeros`, `nnz`, and `issparse`) with shared RuntimeValue access and
opaque embedding transport. Contract 1.15 adds ordered table storage,
metadata, indexing and assignment, `height`, `width`, `istable`, and
array/structure conversions through the same registry. The active catalog
at 1.15 contained 306 descriptors and 308 registered names. Contract 1.16 adds
N-dimensional categorical storage and category-management builtins, closes
table row deletion/multi-variable assignment/concatenation/sorting, and adds
datetime/duration RowTimes plus table/timetable conversion. The active catalog
contained 324 descriptors and 326 registered names. Contract 1.17 adds
`innerjoin`, `outerjoin`, `groupcounts`, and `groupsummary` through the same
context, execution-control, RuntimeValue, and registry paths. Its active
catalog contains 328 descriptors and 330 registered names. Temporal, sparse,
categorical, table, and timetable values remain VM/portable values with no
Typed/JIT lowering in these batches; rich tabular aggregations, advanced
timetable operations, timezone databases, calendar-month arithmetic,
rich-data MAT persistence, and graphics remain outside this contract.

An independently compiled external C/C++ callback table requires its own
future pure-C ABI. It cannot expose `RuntimeValue`, STL containers, registry
classes, or VM pointers.

## Freeze And Deprecation

A contract snapshot is created at a release-candidate gate, not after every
internal batch. The v1.3 candidate is frozen in `public-contract-v1.3.json`; the
v1.2 artifact remains archived. The active v1.10 Runtime extension is still an
unreleased development contract and is validated by its current header,
manifest, tests, and consumers rather than by mutating the v1.3 snapshot. Once
a contract is released, incompatible changes require an explicit replacement
contract and migration record. Unreleased development interfaces do not
require a deprecation window; repository callers are simply updated to the
cleaner current form.

Every candidate-boundary change updates the applicable version metadata,
snapshot, tests, documentation, and independent consumer evidence. Historical
snapshots are not rewritten to make a current test pass.
