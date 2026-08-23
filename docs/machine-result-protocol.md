# MParser Machine Result Protocol

The machine result protocol is the automation boundary for one-shot CLI
execution. It is separate from MParser's human-readable output and from the C
embedding ABI.

Invoke protocol version 1 with:

```powershell
build\mparser.exe --run --result-format=json-v1 samples\machine_protocol_demo.m
```

The Linux spelling is identical apart from the executable path:

```bash
./build/mparser --run --result-format=json-v1 samples/machine_protocol_demo.m
```

`--jit=auto|off|portable|native`, `--entry-function`, `--argument`,
`--outputs`, `--path`, and `--class-path` retain their normal production
meaning.

## Channel And Process Contract

Once `--result-format=json-v1` has been requested and stdout accepts the
complete write:

- stdout contains exactly one compact UTF-8 JSON document followed by one
  LF byte (`0x0A`) on every platform;
- there are no bytes before the opening `{` or after that LF byte;
- stderr remains empty for every protocol exit;
- compilation, request, source-load, and language runtime failures are encoded
  in that document and do not write human diagnostics to stderr;
- `--native-cache-stats` is rejected because its human stdout would corrupt
  the document;
- the ordinary human `--run` format is unchanged;
- `--help` and `--version` remain human-only operations; combining either with
  a result format produces a protocol `request-rejected` document, independent
  of argument order.

An output-producing MATLAB-like builtin must use the host output sink, be
represented by a protocol extension, or be rejected in machine mode; it must
not write unframed bytes into protocol stdout.

Process exit codes are stable within protocol major 1:

| Exit | `status` | Meaning |
| ---: | --- | --- |
| 0 | `succeeded` | Compilation, validation, and execution succeeded |
| 1 | `compilation-failed` | The source graph did not compile |
| 2 | `request-rejected` | CLI, source-load, or invocation validation failed |
| 3 | `runtime-failed` | Language execution or a resource control failed |
| 4 | `request-rejected` when writable | Emergency serialization or output-transport failure |

Consumers must inspect both the exit code and `status`. A nonzero exit still
has a framed result document for exits 1 through 3. If ordinary result
serialization fails before output begins, exit 4 uses a static
allocation-free document with `MParser:ProtocolFailure`, engine version
`unknown`, and default execution fields. If stdout itself rejects or only
partially accepts bytes, exit 4 cannot guarantee a parseable document; the
consumer must treat missing or malformed stdout as a transport failure.

## Top-Level Document

The producer emits fields in this deterministic order, although consumers
must treat JSON object member order as insignificant:

```json
{
  "protocol": {"name": "mparser.result", "major": 1, "minor": 1},
  "engine": {"name": "MParser", "version": "1.3.0"},
  "status": "succeeded",
  "entry_function": "",
  "requested_output_count": 0,
  "outputs": [],
  "output_events": [],
  "top_level_expressions": [],
  "workspace": [],
  "diagnostics": [],
  "execution": {}
}
```

`outputs` contains `{ "name", "value" }` entries. `name` is null when the
runtime has no declared name for that output. `workspace` contains
`{ "name", "value" }` entries in the runtime result order.

`output_events` contains retained `disp`/stdout records:

```json
{
  "sequence": 0,
  "kind": "display",
  "text": "value=42\n",
  "source": {
    "name": "script.m",
    "begin": {"offset": 0, "line": 1, "column": 1},
    "end": {"offset": 15, "line": 1, "column": 16}
  }
}
```

`kind` is `display` for `disp` and `stdout` for the current `fprintf` form.
The text is framed inside JSON and therefore never writes unstructured bytes
to protocol stdout. `source` may be null when no language source range exists.

`top_level_expressions` contains script expression-statement results:

```json
{
  "sequence": 1,
  "output_suppressed": false,
  "source": null,
  "value": {"kind":"numeric","class":"double","dimensions":[1,1],"data":[42]}
}
```

Semicolon-suppressed expressions remain present with
`output_suppressed: true` and still update `ans`; assignments are represented
through `workspace`. Both arrays use the same zero-based monotonically
increasing unsigned 64-bit sequence. Sequences are strictly increasing inside
each array and unique across both arrays, so consumers recover original
execution order with a two-way merge. Array order alone does not establish
cross-array order.

Current ordinary producer documents emit both arrays, including when empty.
The tolerant major-1 schema makes them optional so the earlier protocol-1.1
snapshot and the allocation-free emergency document remain valid; a consumer
must treat an absent array as empty.

`execution` projects every `ModuleExecutionSummary` field:

- requested backend and effective tier;
- profile, fallback, resource-control, and optimization-suppression flags;
- structured stop reason;
- instruction and typed-region counts;
- native compilation and cache-hit counts;
- call-depth, array-byte, and diagnostic high-water marks;
- elapsed nanoseconds.

Before execution, fields that have no observed runtime value retain their
zero/default summary value.

Every count, byte total, and nanosecond field is an unsigned 64-bit JSON
integer. Consumers must parse these fields with an exact `uint64` or
arbitrary-precision integer representation. Converting through binary64 can
lose values above `2^53 - 1`; the protocol fixture deliberately contains
`18446744073709551615`.

## Value Encoding

All dimensions have at least two entries and all array payloads are emitted in
MATLAB column-major linear order. Internal C++ storage order is not exposed.

### Numeric And Logical

```json
{"kind":"numeric","class":"double","dimensions":[2,2],"data":[1,3,2,4]}
```

Logical payload elements are JSON booleans and use `"class":"logical"`.
Floating classes are `double` and `single`. Integer classes are `int8`,
`uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, and `uint64`.
Finite floating values are JSON numbers; non-finite values are the strings
`"NaN"`, `"Infinity"`, and `"-Infinity"`. Integer payloads are exact JSON
integers, including values outside the binary64 exact range.

Protocol 1.1 adds exact complex floating payloads:

```json
{
  "kind":"numeric",
  "class":"single",
  "dimensions":[1,2],
  "data":[1,3],
  "complex":true,
  "imaginary_data":[2,-4]
}
```

`data` and `imaginary_data` have the same element count and MATLAB
column-major order. Logical values are always real. The runtime currently
rejects complex integer arithmetic even though the recursive schema reserves
an integer imaginary channel for forward numeric transport.

### Character And String

Character arrays preserve exact UTF-16 code units:

```json
{"kind":"character","dimensions":[1,3],"utf16":[65,66,67]}
```

String arrays contain UTF-8 JSON strings. A missing string element is JSON
null and is distinct from an empty string:

```json
{"kind":"string","dimensions":[1,2],"data":["text",null]}
```

Malformed UTF-8 in host-facing names or diagnostics is replaced
deterministically with `\uFFFD`. Runtime UTF-16 conversion follows the same
replacement policy for malformed surrogate input.

### Composite Values

Cells and transient comma-separated lists use recursive `data` arrays:

```json
{"kind":"cell","dimensions":[1,2],"data":[{"kind":"missing","dimensions":[1,1]},{"kind":"missing","dimensions":[1,1]}]}
```

Structures declare field order once and emit one object per column-major
structure element:

```json
{
  "kind":"struct",
  "dimensions":[1,1],
  "fields":["count","label"],
  "data":[{
    "count":{"kind":"numeric","class":"double","dimensions":[1,1],"data":[2]},
    "label":{"kind":"string","dimensions":[1,1],"data":["demo"]}
  }]
}
```

Name-value arguments use:

```json
{"kind":"name-value-argument","name":"Scale","value":{"kind":"missing","dimensions":[1,1]}}
```

A language-visible missing value carries its logical shape, for example
`{ "kind": "missing", "dimensions": [2,3] }`. The producer retains the
dimensionless `{ "kind": "missing" }` form for an internal absent output and
for compatibility with earlier major-1 documents. Consumers must therefore
treat `dimensions` as optional; when present, it is authoritative and follows
the same normalized shape rules as other arrays.

### Handles And Objects

Function handles expose only a stable callable descriptor:

```json
{
  "kind":"function-handle",
  "dimensions":[1,1],
  "handle_kind":"function",
  "backend":"bytecode",
  "display":"@work",
  "target_name":"work",
  "class_name":"",
  "method_name":"",
  "declaring_class":"",
  "module_bound":true
}
```

Captured workspaces, receivers, source addresses, process-local identities,
and native pointers are intentionally omitted.

Objects are opaque because protocol v1 has no property-inspection ownership
contract:

```json
{
  "kind":"object",
  "class":"DemoObject",
  "dimensions":[1,1],
  "handle":false,
  "enumeration_member":null,
  "representation":"opaque"
}
```

Use the C embedding API when a host needs retained object or function-handle
values that can be passed into later invocations.

## Diagnostics

Every diagnostic contains:

- `phase`: `compilation`, `validation`, or `execution`;
- `severity`: `error` or `warning`;
- stable `identifier` and human `message`;
- `source`, either null or a named begin/end range with byte offset, line, and
  column;
- an ordered `stack`;
- recursive `causes`.

Source-load failures use `MParser:SourceLoadFailed`. CLI contract failures use
`MParser:CliError`. Language diagnostics retain their original identifiers.

## Versioning

Consumers select the protocol explicitly with `json-v1` and verify
`protocol.name` plus `protocol.major`.

The normative machine-readable shape is
[`machine-result-v1.schema.json`](machine-result-v1.schema.json). This file
is the tolerant Draft-7 major-1 consumer profile. It permits additive object
members while preserving all current required members, field types, and
recursive value/diagnostic shapes. The current protocol-1.1 producer shape is
checked by the golden document and semantic/schema validators. The earlier
protocol-1.1 snapshot remains an accepted minimal document rather than being
rewritten for additive optional members. A future minor that adds enum values
or value kinds must publish a revised major-1 schema; consumers apply a schema
only up to the minor they support.

- A minor revision may add optional object members or new enum values.
- Consumers must ignore unknown members and preserve unknown enum values as
  unsupported rather than guessing.
- Existing members do not change meaning within major 1.
- Removing a member, changing its type or meaning, or changing array ordering
  requires a new major protocol and a new CLI format name.
- `engine.version` identifies runtime behavior; it is not the protocol
  negotiation field.

`tests/golden/machine_result_v1.json` freezes current producer spelling and ordering
for all RuntimeValue kinds, diagnostic trees, non-finite numbers, UTF
replacement, exact integer and complex values, ordered output/expression
records, and execution fields. The earlier protocol 1.1 snapshot under
`tests/public_contract/protocol/1.1` remains unchanged and schema-valid; the
1.0 directory is retained only as historical release evidence.
`tests/golden/machine_result_emergency_v1.json` independently freezes the
allocation-free exit-4 document used while stdout remains writable.
`machine_protocol_contract_smoke` validates the schema from an independent
consumer perspective, including additive-minor member acceptance and negative
major, required-field, current-minor enum, and type cases. CLI integration
tests independently parse success, compilation, validation, source-load, CLI,
runtime-failure,
human-option rejection, and emergency-framing documents.

`machine_protocol_schema_smoke` uses vendored test-only JSON and Draft-7
libraries to validate the golden, emergency, snapshot, negative, and dynamic
CLI documents. A supplemental semantic pass checks exact unsigned 64-bit
bounds because the validator represents JSON numeric limits with signed host
integers. Runtime protocol generation does not link either test dependency.
