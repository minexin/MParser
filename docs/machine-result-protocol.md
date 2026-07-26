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

Once `--result-format=json-v1` has been requested:

- stdout contains exactly one compact UTF-8 JSON document followed by one
  newline;
- compilation, request, source-load, and language runtime failures are encoded
  in that document and do not write human diagnostics to stderr;
- `--native-cache-stats` is rejected because its human stdout would corrupt
  the document;
- the ordinary human `--run` format is unchanged;
- `--help` and `--version` remain human-only early-exit operations and should
  not be combined with a result format.

Output-producing MATLAB-like builtins are not implemented as of v0.87. A future
output builtin must be captured, represented by a protocol extension, or
rejected in machine mode; it must not write unframed bytes into protocol
stdout.

Process exit codes are stable within protocol major 1:

| Exit | `status` | Meaning |
| ---: | --- | --- |
| 0 | `succeeded` | Compilation, validation, and execution succeeded |
| 1 | `compilation-failed` | The source graph did not compile |
| 2 | `request-rejected` | CLI, source-load, or invocation validation failed |
| 3 | `runtime-failed` | Language execution or a resource control failed |
| 4 | reserved | Host/protocol failure not representable by the statuses above |

Consumers must inspect both the exit code and `status`. A nonzero exit still
has a normal result document.

## Top-Level Document

The producer emits fields in this deterministic order, although consumers
must treat JSON object member order as insignificant:

```json
{
  "protocol": {"name": "mparser.result", "major": 1, "minor": 0},
  "engine": {"name": "MParser", "version": "0.87.0"},
  "status": "succeeded",
  "entry_function": "",
  "requested_output_count": 0,
  "outputs": [],
  "workspace": [],
  "diagnostics": [],
  "execution": {}
}
```

`outputs` contains `{ "name", "value" }` entries. `name` is null when the
runtime has no declared name for that output. `workspace` contains
`{ "name", "value" }` entries in the runtime result order.

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

## Value Encoding

All dimensions have at least two entries and all array payloads are emitted in
MATLAB column-major linear order. Internal C++ storage order is not exposed.

### Numeric And Logical

```json
{"kind":"numeric","class":"double","dimensions":[2,2],"data":[1,3,2,4]}
```

Logical payload elements are JSON booleans and use `"class":"logical"`.
Finite doubles are JSON numbers formatted for round-trip binary64 recovery.
Non-finite values are the strings `"NaN"`, `"Infinity"`, and `"-Infinity"`;
this keeps every document valid JSON.

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
{"kind":"cell","dimensions":[1,2],"data":[{"kind":"missing"},{"kind":"missing"}]}
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
{"kind":"name-value-argument","name":"Scale","value":{"kind":"missing"}}
```

A missing runtime value is `{ "kind": "missing" }`.

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

- A minor revision may add optional object members or new enum values.
- Consumers must ignore unknown members and preserve unknown enum values as
  unsupported rather than guessing.
- Existing members do not change meaning within major 1.
- Removing a member, changing its type or meaning, or changing array ordering
  requires a new major protocol and a new CLI format name.
- `engine.version` identifies runtime behavior; it is not the protocol
  negotiation field.

`tests/golden/machine_result_v1.json` freezes producer spelling and ordering
for all RuntimeValue kinds, diagnostic trees, non-finite numbers, UTF
replacement, and execution fields. CLI integration tests independently parse
success, compilation, validation, source-load, CLI, and runtime-failure
documents.
