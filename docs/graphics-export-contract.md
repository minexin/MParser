# Figure File Export

Development contract for MParser 1.15.0, builtin source contract 1.23. C ABI
2.12 and the machine/graphics wire formats are unchanged. The public SDK,
CLI and current MExecServer have no file renderer installed.

## Source Requests

| Form | Target | Format |
| --- | --- | --- |
| `print(fig, filename, device)` | Live scalar Figure in the current graph | `-dpng` or `-dpdf` |
| `print(filename, device)` | Existing current Figure, without creating one | `-dpng` or `-dpdf` |
| `saveas(object, filename, format)` | Live scalar graphics object, resolved through ownership to its Figure | `png` or `pdf` |
| `saveas(object, filename)` | Same as above | Inferred only from `.png` or `.pdf` |

Formats, devices and extensions are case-sensitive. With an explicit format,
the corresponding extension is appended if the final filename component has
none. Dots in parent directories do not count. An existing extension must
match the selected format, otherwise `MParser:GraphicsExportExtensionMismatch`
is returned. The deliberate safety difference from MATLAB is that bytes of
one format must not be published under another format's filename.

Filenames must be scalar text without control characters and have a nonempty
final component, no trailing dot or space, and no print option in place of the
filename. This is lexical validation only. A syntactically accepted path is
not an authorization or a resolved filesystem location. Numeric figure
numbers, handle arrays, native FIG, printer/clipboard/device options beyond
the table, DPI, paper/layout options and implicit format for `print` remain
unsupported. A missing, deleted, foreign or invalid graphics target is an
`InvalidExportFigure` error. Invalid path syntax and unknown formats have
separate `InvalidGraphicsExport` and `UnsupportedExportFormat` errors.

Valid requests without a renderer return `MParser:GraphicsRendererUnavailable`.
This path performs no file I/O, does not create a missing Figure, and does not
change the selected Figure or any scene object. An explicit expression such as
`gcf()` is still evaluated normally before the export call.

## Internal Renderer Adapter

An internal, synchronous `RuntimeGraphicsExportAdapter` can be supplied by an
execution control. The selected Figure identity, PNG/PDF enum, destination and
owning v4 JSON snapshot are prepared together. The whole graph is included to
retain cross-Axes Legend references; render only the selected Figure, not all
Figures. No graph lock is held across the adapter call and no live object or
workspace pointer is supplied. A missing Figure layout environment remains
missing; an adapter must not claim real display metrics it does not have.

The adapter owns root/cwd policy, access checks, encoding and atomic publication.
It must treat the path as untrusted, must not reenter the invoking runtime, and
must not report `Written` until the actual selected format is committed.
Unsupported encoding, destination rejection, reported failure and exceptions
have distinct typed or execution-error results. The kernel does not write or
inspect output bytes here. Cancellation is checked before dispatch, not a
promise of interrupting the adapter or rolling back an already committed file.
No public callback, built-in encoder, production deployment or browser download
behavior is introduced by this adapter.

## Reference And Tests

`tools/reference/graphics-export/run_graphics_export_reference.m` replayed 21
assertions in installed MATLAB R2024b `24.2.0.2712019`, checking output
existence, diagnostic identifiers, filenames and PNG/PDF/FIG signatures.
Evidence: `build/graphics-export-r2024b-checked.log`. The explicit mismatch
cases really produced PNG bytes with `.pdf` and `.xyz` suffixes in MATLAB.
Those are negative controls for MParser, not parity claims. MATLAB also accepts
numeric Figure numbers and defaults `saveas` without a format/extension to FIG;
neither is supported here.

`graphics_export_smoke` exercises HIR/VM dispatch, owning and current Figure
selection, immutable request snapshots and adapter failure mapping. Its
recording adapter is a test double, not an encoder or file-success proof.
`graphics_api_smoke` checks the installed public ABI's headless behavior.
The isolated host probe adds real REST checks for target and format rules,
empty-scene preservation and unchanged files, without configuring an encoder.
Actual PNG/PDF rendering and commit under a rooted host policy, a supported
public SDK registration boundary, and deployed acceptance remain open.
