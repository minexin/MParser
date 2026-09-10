# Headless Graphics Object Model

The V1.x graphics target is a kernel object model. It is independent of any
window system and does not promise complete MATLAB desktop graphics behavior.

The first slice will support `plot(y)` with implicit x coordinates `1:numel(y)`
and `plot(x,y)`, returning a handle-identity `Line`.
The call creates a `Figure` containing an `Axes`, with the line parented by the
axes. Each object has a stable identity for the life of the owning runtime;
repeated references compare as the same handle. Parent and child properties
are navigable, deletion recursively detaches descendants, and runtime cleanup
releases the graph without retaining a session or module owner.

The initial property set is intentionally small and validated by the kernel:
figure visibility and name; axes limits, labels, and title; line data, color,
line style, and display name; and each object's `Parent` and `Children` links.
Invalid property names, values, and cross-runtime handles produce deterministic
diagnostics. No native window, event loop, rasterizer, or GUI dependency belongs
in the kernel.

Serialization is a versioned, deterministic graph record. Objects are emitted
in creation order, references use stable per-record IDs, children are ordered,
and properties use canonical names and scalar/array encodings. Parent and child
references use IDs instead of recursively expanding the graph: the normal
bidirectional links are valid. The serializer must reject dangling references,
cycles in the containment hierarchy, foreign-runtime references, or unsupported
values rather than silently dropping them. Deserialization is an explicit backend boundary;
it must not create executable callbacks or arbitrary system resources.

MExecClient or another plugin can translate this record to Canvas, SVG, an image,
or a native window. Such rendering is a separate compatibility track. The
kernel track closes `cap_291_out_graphics` only when the object graph, handle
identity, lifetime, property access, and deterministic serialization pass in
interpreter and bytecode execution and through the public embedding and
machine protocol. The imported `plot(1:3)` case alone is insufficient evidence:
regressions must also inspect the returned Line, traverse Axes/Figure parents,
modify and read back properties through aliases, delete the graph, and compare
deterministic serialized records across repeated equivalent runs. Until that
milestone, the graphics compatibility gap stays open. The working-tree source
slice is tracked in [v1.14.md](v1.14.md); successful `plot` execution alone does
not establish the full capability.

## Implementation Constraints From The Current Runtime

`runtime_object.cpp` stores ordinary handle fields in a shared map, and
`runtime_value_ops.cpp` compares scalar handle identity using that map's
address. Graph parent/child links must not be stored as strong `RuntimeValue`
references in those maps: Figure-to-Axes and Axes-to-Figure would keep each
other alive after the owning session is destroyed. A graph-owned node table
should instead store IDs for containment edges and expose handles through
validated property access. Ordinary value properties may reuse existing
numeric/text representations, but relation properties need graph lookup.

The graph must not retain its Runtime or compiled module. A runtime/session
owns the graph, and exported handles may retain graph state independently
without creating a back-reference to that owner. Nodes must not retain those
exported handles. Deletion needs explicit tombstones or equivalent validity
state so aliases remain identifiable while operations reject deleted objects.
Creating a fresh node must not reuse the identity of a deleted live alias.
Cross-runtime operations must validate graph ownership before changing edges.

`machine_protocol.cpp::writeObjectValue` currently emits only opaque object
metadata. Existing object output therefore cannot serve as the graphics graph
record: V1.14 must add and test the versioned graph serialization path, including
ordered nodes, ordered children, canonical properties and validated references.
The public SDK must expose that same semantic record, not an internal field-map
pointer or a recursive expansion of Parent/Children.

These are implementation constraints, not evidence of implemented graphics.
Acceptance must explicitly cover graph release after owner destruction, handles
retained by SDK results, alias invalidation after recursive deletion, rejected
foreign parents, and equivalent serialized output from interpreter and VM runs.
