# Graphics Color Contract

Development product 1.15.0, ABI 2.7. These rules apply to the existing Line and
Text Color properties, not to every MATLAB graphics class or to plot LineSpec
arguments. No new C ABI symbol or builtin is introduced.

## Source Values

Color accepts case-insensitive short/full names and their R2024b prefixes for
red, green, blue, cyan, magenta, yellow, black and white. The black short name is
k; ambiguous b/bl selects blue. Leading/trailing ASCII whitespace is trimmed.
Character vectors and scalar strings accept #RGB/#RRGGBB hexadecimal colors.
The case-insensitive none name/prefix is stored and read back as character none.
The auto keyword, unknown names, malformed hex and string/character arrays are
rejected. Names/hex are normalized inside the kernel, not by a host renderer.

Numeric Color accepts three or four elements in logical column-major order,
including column vectors, higher-rank shapes, sparse storage and complex values.
Only real components are used, matching the measured R2024b behavior. Floating
and logical channels use [0,1]. Unsigned integers are scaled by their type's
maximum; signed integers are shifted by -minimum and divided by the full type
range. This is not equivalent to treating every integer input as a double RGB
triplet. Nonfinite or out-of-range normalized channels, including alpha, fail.

Source Color reads return a 1-by-3 double RGB row or character none. Line retains
a fourth-channel alpha for rendering; Text accepts but discards it, as observed
in the R2024b print baseline. Three-channel/name assignments restore opaque
rendering. Invalid assignment is atomic: previous RGB, alpha, graph identity,
object identity and other graph data remain unchanged.

## Snapshot Values

Rendering snapshots are not the same shape as source getter results. Their
properties.Color is canonical RGB, RGBA for a non-opaque Line, or string none.
An opaque fourth alpha is omitted; Text always exports RGB/none. Legacy automatic
serialization selects graphics v2 for RGBA/none instead of silently giving a
v1-only host an extended color. Ordinary RGB-only Line scenes retain v1 output.
Explicit identity snapshots remain v3. The existing schemas permit these value
shapes; no frozen v1.14 schema or snapshot is rewritten.

A renderer must suppress a none Line and its Legend sample without removing the
Legend label. RGBA applies to both Line and Legend sample. Bound Text labels
use their Color; none suppresses drawing without deleting the String. The
isolated client color candidate implements these rules, but real-browser visual
acceptance and deployed-client adoption remain separate gates.

## Evidence

tests/graphics_color_cases.json fixes 75 inputs. tools/probe_graphics_colors.m
replays the same corpus in MATLAB R2024b for Line/Text, saving the exact version,
input expressions, accepted values, rejection identifiers and unchanged values.
The probe also saves PNGs for alpha 0/0.5/1 on white backgrounds. In the measured
Line center pixels, green was 255/127/0 respectively; Text remained opaque red
for all three alpha inputs. These reference images are not MParser rendering.

graphics_color_smoke uses the public C++ SDK in four requested
backend modes for 600 executions, checking getter type/shape, rendering values,
aliases, graph/object identities, frozen results and invalid-write atomicity.
Native requests may use normal fallback. Simulated-canvas candidate tests and
a real kernel v2 scene cover RGBA/none, Text colors and Legend samples. REST
probe switch GraphicsColors adds 14 focused Line/Text requests. These fixed
corpora do not imply full graphics-class, backend-codegen or browser parity.
