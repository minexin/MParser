#pragma once
#include "mparser/runtime/core/session/runtime_event_journal.h"

namespace mparser {
struct RuntimeEventCollectionOptions {
    size_t maximumEvents = 4096;
    size_t maximumBytes = 16 * 1024 * 1024;
    RuntimeSnapshotLimits valueLimits{128,10000,1024 * 1024};
};
enum class RuntimeEventTruncation { None, EventCount, ByteLimit };
struct RuntimeReadonlyEvent {
    RuntimeJournalKind kind;
    RuntimeJournalDisposition disposition;
    std::uint64_t sequence, sourceContext;
    size_t captureDepth;
    SourceSpan span;
    std::string sourceName,label,text,valueJson,valueUnavailable;
    bool outputSuppressed = false;
    RuntimeLineSpacing lineSpacing = RuntimeLineSpacing::Loose;
    std::optional<RuntimeSnapshotFailure> valueFailure;
    std::optional<Diagnostic> diagnostic;
};
struct RuntimeEventCollection {
    bool enabled = false, started = false, truncated = false;
    RuntimeEventTruncation truncation = RuntimeEventTruncation::None;
    std::uint64_t generatedCount = 0, retainedBytes = 0;
    std::vector<RuntimeReadonlyEvent> events;
};
// Bounds the owned public result, not the execution journal or evalc buffers.
RuntimeEventCollection runtimeCollectReadonlyEvents(const std::vector<RuntimeJournalEvent>& events,
    RuntimeEventCollectionOptions options,bool started);
}
