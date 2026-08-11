# Versioning And Development Policy

MParser has one product version shared by the engine, CLI, libraries, headers,
CMake package, and installed SDK. Technical boundaries also carry independent
contract metadata where a consumer must negotiate a binary or wire format.
Those numbers are never SDK product versions.

## Product And SDK

MParser uses semantic versions for release tags and packages. The active
source tree, product metadata, and installed SDK report development version
`1.2.0`. This identifies the in-progress v1.2 train; it is not a tagged
release until the complete milestone reaches its release-candidate gate.

This project is not currently using the v1.2 interfaces in production.
Implementation, in-repository consumers, tests, samples, and documentation may
change together without compatibility wrappers for superseded development
interfaces. This keeps the current design small while its numeric, embedding,
and function surfaces are still settling.

The published MParser 1.0.0 contracts remain immutable historical evidence.
They do not turn an unreleased v1.2 interface into a backward-compatibility
requirement.

## Current Contract Identifiers

| Boundary | Current identifier | Meaning |
| --- | --- | --- |
| MParser product and SDK | `1.2.0` development snapshot | User-facing release identity |
| Production CLI | 1.0 | Command, option, channel, and exit contract |
| C source API | 1.2 | Header-level source contract for C hosts |
| C ABI | generation 2, revision 0 | Binary layout, symbols, ownership, and calling convention |
| C++ source API | 1.2 | Header-level source contract over the C ABI |
| Machine result protocol | `mparser.result` 1.1 | JSON producer/consumer contract |
| Builtin source contract | 1.0 | Registry/descriptor/call semantics compiled with the engine |

## CLI 1.0

The production command is `--run`. Its `--jit=auto|off|portable|native`
selector, invocation arguments, machine-result selector, source-path rules,
channel behavior, and exit classes are the normal user boundary. Automation
uses `--run --result-format=json-v1` instead of parsing human output.

Diagnostic token, HIR, bytecode, planning, profiling, and historical
`--run-*` modes may change during development. They are engineering tools, not
separate execution products.

## C ABI Generation 2

`MPARSER_C_API_VERSION_MAJOR/MINOR/PATCH` report source API `1.2.0`.
`MPARSER_C_ABI_GENERATION` contains generation `2`, and
`MPARSER_C_ABI_REVISION` contains revision `0`.
`mparser_c_abi_generation()` and `mparser_c_abi_revision()` expose the same
binary compatibility identifiers at runtime. The generation terminology is
deliberately distinct from the MParser product version and source API.

Generation 2 directly transports every core numeric class and separate
complex real/imaginary buffers. The current SONAME/install-name major is 2.
Caller-sized root records permit future tails; fixed-stride records remain
sealed. Exact rules and validation are in `c-abi-compatibility.md`.

Until the v1.2 candidate freeze, the current ABI may change with all repository
consumers updated in the same change. After a freeze, an incompatible binary
change advances the generation; an additive frozen-generation change advances
the revision.

## C++ Source API 1.2

`include/mparser/cpp_api.hpp` is a header-only C++20 facade over C ABI
generation 2. Its source API follows the v1.2 product line to avoid presenting
an unrelated SDK 2.0 identity. It does not promise a C++ binary ABI, and no STL
object or C++ class layout crosses the shared-library boundary.

Before the milestone freeze, callers compile the current header. At the
candidate gate, the header and relocated multi-translation-unit consumer are
snapshotted together.

## Machine Result Protocol 1.1

`mparser.result` has an independent wire major/minor pair. Protocol 1.1 adds
numeric-class metadata, exact fixed-width integer JSON values, and separate
imaginary data for complex double/single values. Existing major-1 framing,
status, diagnostics, summaries, and emergency behavior remain defined by the
schema and current 1.1 snapshot.

Consumers check the protocol major and tolerate documented additive minor
fields. A change that removes a required field or changes its meaning requires
a protocol-major change.

## Builtin Source Contract 1.0

`BuiltinRegistry`, `BuiltinDescriptor`, `BuiltinCall`, and `BuiltinResult`
form a source-integration contract for builtins compiled with the engine. They
are not an installed plugin ABI and do not expose stable C++ object layouts.

Contract 1.0 defines naming, aliases, registry freezing, arity, value/shape
constraints, context permissions, side effects, determinism, exception
conversion, diagnostics, ownership, and typed-lowering eligibility. The
archived v1 catalog contains 118 descriptors. The active v1.2 catalog has 164
descriptors and remains intentionally unsnapshotted until the complete
milestone function surface settles.

An independently compiled external C/C++ callback table requires its own
future pure-C ABI. It cannot expose `RuntimeValue`, STL containers, registry
classes, or VM pointers.

## Freeze And Deprecation

A contract snapshot is created at a release-candidate gate, not after every
internal batch. Once a contract is released, incompatible changes require an
explicit replacement contract and migration record. Unreleased development
interfaces do not require a deprecation window; repository callers are simply
updated to the cleaner current form.

Every candidate-boundary change updates the applicable version metadata,
snapshot, tests, documentation, and independent consumer evidence. Historical
snapshots are not rewritten to make a current test pass.
