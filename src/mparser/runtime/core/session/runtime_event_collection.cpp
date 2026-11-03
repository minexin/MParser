#include "mparser/runtime/core/session/runtime_event_collection.h"
#include "mparser/runtime/core/value/runtime_snapshot_json.h"

namespace mparser {
namespace {
class Charge {
public:
    explicit Charge(size_t maximum) : maximum_(maximum) {}
    void add(size_t count) {
        if (count > maximum_ - used_) { exceeded_ = true; }
        else { used_ += count; }
    }
    void frame(const DiagnosticFrame& value) { add(64); add(value.file.size()); add(value.name.size()); }
    void cause(const DiagnosticCause& value) {
        add(128); add(value.identifier.size()); add(value.message.size());
        for (const auto& item : value.stack) { frame(item); }
        for (const auto& item : value.causes) { cause(item); }
    }
    bool exceeded() const { return exceeded_; }
    size_t used() const { return used_; }
private:
    size_t maximum_,used_ = 0;
    bool exceeded_ = false;
};
}
RuntimeEventCollection runtimeCollectReadonlyEvents(const std::vector<RuntimeJournalEvent>& events,
    RuntimeEventCollectionOptions options,bool started) {
    RuntimeEventCollection result;
    result.enabled = true; result.started = started; result.generatedCount = events.size();
    for (const auto& event : events) {
        if (result.events.size() == options.maximumEvents) {
            result.truncated = true; result.truncation = RuntimeEventTruncation::EventCount; break;
        }
        RuntimeReadonlyEvent copy{event.kind,event.disposition,event.sequence,event.sourceContext,event.captureDepth,
            {},{},{},{},{},{},false,RuntimeLineSpacing::Loose,std::nullopt,std::nullopt};
        if (const auto display = std::get_if<RuntimeAssignmentDisplay>(&event.payload)) {
            copy.span = display->span; copy.sourceName = display->sourceName; copy.label = display->label;
            copy.text = display->publicText; copy.outputSuppressed = display->outputSuppressed; copy.lineSpacing = display->lineSpacing;
            copy.valueFailure = display->snapshotFailure; copy.valueUnavailable = display->snapshotError;
            if (!copy.valueFailure) {
                try { copy.valueJson = runtimePublicSnapshotJson(display->valueSnapshot,options.valueLimits); }
                catch (const RuntimeSnapshotError& error) { copy.valueFailure = error.reason(); copy.valueUnavailable = error.what(); }
                catch (const std::bad_alloc&) { throw; }
                catch (const std::exception& error) { copy.valueFailure = RuntimeSnapshotFailure::InvalidValue; copy.valueUnavailable = error.what(); }
            }
        } else if (const auto text = std::get_if<RuntimeJournalText>(&event.payload)) {
            copy.span = text->span; copy.sourceName = text->sourceName; copy.text = text->text;
        } else {
            const auto& diagnostic = std::get<Diagnostic>(event.payload);
            copy.span = diagnostic.span; copy.sourceName = diagnostic.sourceName; copy.text = diagnostic.message;
            copy.diagnostic = diagnostic;
        }
        Charge charge(options.maximumBytes - static_cast<size_t>(result.retainedBytes));
        charge.add(256); charge.add(copy.sourceName.size()); charge.add(copy.label.size()); charge.add(copy.text.size());
        charge.add(copy.valueJson.size()); charge.add(copy.valueUnavailable.size());
        if (copy.diagnostic) {
            const auto& diagnostic = *copy.diagnostic;
            charge.add(128); charge.add(diagnostic.identifier.size()); charge.add(diagnostic.message.size()); charge.add(diagnostic.sourceName.size());
            for (const auto& frame : diagnostic.stack) { charge.frame(frame); }
            for (const auto& cause : diagnostic.causes) { charge.cause(cause); }
        }
        if (charge.exceeded()) { result.truncated = true; result.truncation = RuntimeEventTruncation::ByteLimit; break; }
        result.retainedBytes += charge.used();
        result.events.push_back(std::move(copy));
    }
    return result;
}
}
