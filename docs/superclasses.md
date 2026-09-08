# Superclass Reflection

The v1.12 development builtin `superclasses(value)` returns a column cell array
of character vectors naming visible ancestors. A character vector or string
scalar names the class to inspect; other values supply their runtime class,
including object arrays. A class metadata object describes its own metadata
class for this call: use `superclasses(info.Name)` to inspect the represented
class instead.

Traversal follows declaration order, visits each ancestor once, and descends
through hidden ancestors without including their names. This preserves visible
ancestors behind hidden intermediate classes and removes duplicate ancestors in
multiple inheritance. A class with no ancestors, or an unavailable class name,
returns a 0-by-1 cell array. Numeric values have no visible ancestors; `numeric`
is a type predicate category rather than a superclass of `double`.

The interpreter and bytecode VM share the traversal implementation. User class
definitions come from the loaded module's class catalog. Builtin metadata
aliases are normalized through the runtime metadata descriptors. This function
does not load arbitrary classes outside the configured source graph.

See [the runnable sample](../samples/superclasses_demo.m) and the source-loader
fixtures under `tests/fixtures/superclasses`. The fixture verifier runs in MATLAB
R2024b as well as MParser, and checks multiple inheritance, hidden parents,
object arrays, metadata objects, empty shape, and unknown class names.

The behavior is based on the [MathWorks superclasses reference](https://www.mathworks.com/help/matlab/ref/superclasses.html)
and local MATLAB R2024b probes on 2026-09-08. The probes confirmed depth-first
ordering for the fixture hierarchy and the metadata ancestor chain. The
repository regression also compares interpreter and VM results.
