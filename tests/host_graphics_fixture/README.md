# Frozen Client Renderer Fixture

These are test-only snapshots of the existing MExecClient renderer and its
graphics_snapshot.test.js suite. They were captured from the sibling local
MExecClient checkout before applying the integration candidates. The manifest
pins their LF-normalized contents. They are not a deployed client or a claim
that the current host still has these exact sources.

The replay verifies the hashes, copies the inputs to a new build directory,
applies eight renderer/bootstrap candidates and runs their tests. On 2026-11-05
the unchanged sibling plot-view.js, api.js, app.js and index.html were
additionally captured for per-Axes view and capability/bootstrap tests; the
original plot.js/test snapshots were not replaced.
Do not edit this baseline just to make a candidate patch apply. If the target
host changes, recapture its inputs intentionally, update the provenance
and hashes, and verify both the live-source probe and frozen replay.

All Canvas calls use a simulated context. The CTest variant also passes a
fresh snapshot from the built MParser executable to this patched renderer.
Neither test establishes real-browser layout, visible pixels, interactions,
download behavior, deployment or full MATLAB compatibility.
