# Figure File Export

Development contract for MParser 1.15.0, builtin source contract 1.23. C ABI
2.13 adds optional SDK callback registration without new symbols or changes
to machine/graphics wire formats. The CLI and current MExecServer have no
renderer installed. An SDK invocation is headless unless the caller supplies one.

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

## SDK Renderer Callback

With ABI generation 2 revision 13 or newer, initialize `mparser_invocation_options`
using `MPARSER_INVOCATION_OPTIONS_INIT` and fill `graphics_export` and
`graphics_export_user_data`. In C++, set `Invocation::graphicsExport`. The
registration applies to one execution (module, session or shared Runtime),
not to later invocations. The C++ wrapper rejects registration with an old
DLL. C hosts must check generation and revision before using the new tail.
Older-sized callers, partial tails and null callbacks retain headless behavior.

The synchronous callback receives a size-tagged request with a typed PNG/PDF
format, selected Figure identity, destination and borrowed v4 scene JSON.
The request and views are valid only until the callback returns; copy them
to retain evidence. The whole graph is included to
retain cross-Axes Legend references; render only the selected Figure, not all
Figures. No graph lock is held across the adapter call and no live object or
workspace pointer is supplied. A missing Figure layout environment remains
missing; an adapter must not claim real display metrics it does not have.

The host owns root/cwd policy, access checks, encoding and atomic publication.
It must treat the path as untrusted, must not reenter the invoking runtime, and
must not report `Written` until the actual selected format is committed. The
callback runs on the invoking thread; the runtime/session execution lock can
still be held. Do not release its handles or wait for work needing the lock.
Same-thread reentry into execution/reset and the guarded runtime mutation APIs
is rejected. Renderer callbacks are not dispatched from inside another guarded
execution callback. Separate invocations can run concurrently, so shared host
callback state needs its own synchronization. No renderer userdata is retained
by the SDK after the invocation.

Unsupported format becomes `MParser:UnsupportedExportFormat`; policy rejection
becomes `MParser:GraphicsExportDestinationRejected`. Failure, unknown status and
C++ callback exceptions become `MParser:GraphicsExportFailed`. C callbacks must
not propagate exceptions. The kernel does not write or
inspect output bytes here. Cancellation is checked before dispatch, not a
promise of interrupting the adapter or rolling back an already committed file.
No built-in encoder, production deployment or browser download behavior is
introduced by this callback.

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
`graphics_api_smoke` checks C++ registration, frozen scenes and callback failure;
`graphics_export_c_api_smoke` checks all three execution paths, old/partial
option buffers and guarded reentry. These ABI tests use test doubles. The
relocated SDK consumer also exercises the optional C++ registration.
The isolated host probe adds real REST checks for target and format rules,
empty-scene preservation and unchanged files, without configuring an encoder.

`tools/host-integration/probe-graphics-export-render.py` is a separate real-file
SDK probe using Pillow and ReportLab. It renders only a deliberately bounded
normalized Axes/solid RGB Line subset with an explicitly supplied offscreen
environment. The PNG and vector PDF select different Figures; Poppler raster
readback and pixel counts verify the output. In its private output root an
exact flat allowlist rejects a parent-relative path; same-directory atomic
replacement preserves bytes on an injected pre-commit failure. A Figure
without explicit layout is rejected before encoding. This is not a general
filesystem sandbox or a production renderer. MExecServer adoption, full
graphics rendering and browser download acceptance remain open.
