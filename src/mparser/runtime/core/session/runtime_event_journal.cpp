#include "mparser/runtime/core/session/runtime_event_journal.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"

namespace mparser {

RuntimeJournalEvent* RuntimeEventJournal::find(const ExecutionEventStamp& stamp) {
    if (stamp.identity != identity_ || stamp.sequence >= events_.size()) { return nullptr; }
    return &events_[static_cast<size_t>(stamp.sequence)];
}

void RuntimeEventJournal::text(RuntimeOutputEvent& event, std::uint64_t context, size_t captureDepth, std::string sourceName) {
    if (event.transcriptReplay) { event.sequence = cursor(); return; }
    if (const auto recorded = find(event.eventStamp)) { event.sequence = recorded->sequence; return; }
    event.sequence = nextSequence_++;
    event.eventStamp = {identity_,event.sequence};
    events_.push_back({RuntimeJournalKind::Text,event.sequence,context,captureDepth,RuntimeJournalDisposition::Visible,
        RuntimeJournalText{event.kind,event.text,event.span,event.sourceName.empty() ? std::move(sourceName) : event.sourceName}});
}

void RuntimeEventJournal::display(RuntimeAssignmentDisplay& event, RuntimeJournalKind kind,
    std::uint64_t context, size_t captureDepth) {
    if (kind != RuntimeJournalKind::Assignment && kind != RuntimeJournalKind::Expression) {
        throw std::invalid_argument("journal display requires an assignment or expression kind");
    }
    event.sequence = nextSequence_++;
    events_.push_back({kind,event.sequence,context,captureDepth,RuntimeJournalDisposition::Visible,event});
}

void RuntimeEventJournal::expression(RuntimeExpressionResult& expression, std::uint64_t context,
    size_t captureDepth, std::string sourceName, RuntimeSnapshotPropertyResolver properties, RuntimeDisplayFormat format) {
    format.spacing = expression.lineSpacing;
    auto event = runtimeCaptureAssignmentDisplay(expression.label,expression.value,expression.span,std::move(sourceName),
        expression.outputSuppressed,0,format,{},std::move(properties),true);
    event.text = expression.displayText;
    event.semanticTextAvailable = true;
    display(event,RuntimeJournalKind::Expression,context,captureDepth);
    expression.sequence = event.sequence;
}

void RuntimeEventJournal::diagnostic(Diagnostic& diagnostic, std::uint64_t context,
    size_t captureDepth, std::string sourceName) {
    if (find(diagnostic.eventStamp)) { return; }
    const auto sequence = nextSequence_++;
    diagnostic.eventStamp = {identity_,sequence};
    auto copy = diagnostic;
    if (copy.sourceName.empty()) { copy.sourceName = std::move(sourceName); }
    const bool warning = diagnostic.severity == DiagnosticSeverity::Warning;
    events_.push_back({warning ? RuntimeJournalKind::Warning : RuntimeJournalKind::Error,
        sequence,context,captureDepth,warning ? RuntimeJournalDisposition::Visible : RuntimeJournalDisposition::Raised,
        std::move(copy)});
}

void RuntimeEventJournal::caught(const Diagnostic& diagnostic) {
    if (auto event = find(diagnostic.eventStamp); event && event->kind == RuntimeJournalKind::Error) {
        event->disposition = RuntimeJournalDisposition::Caught;
    }
}
void RuntimeEventJournal::unhandled(const Diagnostic& diagnostic) {
    if (auto event = find(diagnostic.eventStamp); event && event->kind == RuntimeJournalKind::Error) {
        event->disposition = RuntimeJournalDisposition::Unhandled;
    }
}
void RuntimeEventJournal::suppressed(const Diagnostic& diagnostic) {
    if (auto event = find(diagnostic.eventStamp)) { event->disposition = RuntimeJournalDisposition::Suppressed; }
}
void RuntimeEventJournal::complete() {
    for (auto& event : events_) {
        if (event.disposition == RuntimeJournalDisposition::Raised) { event.disposition = RuntimeJournalDisposition::Unhandled; }
    }
}
std::vector<RuntimeJournalEvent> RuntimeEventJournal::snapshot(size_t begin) const {
    if (begin > events_.size()) { throw std::out_of_range("journal snapshot start is outside the transcript"); }
    return {events_.begin() + static_cast<std::ptrdiff_t>(begin),events_.end()};
}

RuntimeEventJournalScope::RuntimeEventJournalScope(RuntimeExecutionControl* control, bool requested) : control_(control) {
    if (!control_) { return; }
    journal_ = control_->eventJournal_;
    if (!journal_ && requested) {
        journal_ = std::make_shared<RuntimeEventJournal>();
        control_->eventJournal_ = journal_;
        owner_ = true;
    }
    if (journal_) { begin_ = journal_->size(); context_ = journal_->sourceContext(); }
}
RuntimeEventJournalScope::~RuntimeEventJournalScope() {
    if (owner_) { control_->eventJournal_.reset(); }
}
RuntimeEventJournalBorrow::RuntimeEventJournalBorrow(RuntimeExecutionControl* control,
    std::shared_ptr<RuntimeEventJournal>& journal,bool& recording,std::uint64_t& context)
    : journal_(journal),recording_(recording),context_(context),previous_(journal),
      previousRecording_(recording),previousContext_(context) {
    if (control && control->eventJournal() && journal != control->eventJournal()) {
        journal_ = control->eventJournal(); recording_ = true; context_ = journal_->sourceContext();
    }
}
RuntimeEventJournalBorrow::~RuntimeEventJournalBorrow() {
    journal_ = std::move(previous_); recording_ = previousRecording_; context_ = previousContext_;
}
std::vector<RuntimeJournalEvent> RuntimeEventJournalScope::snapshot() const {
    return journal_ ? journal_->snapshot(begin_) : std::vector<RuntimeJournalEvent>{};
}

RuntimeFormatResult runtimeRenderEventJournal(const std::vector<RuntimeJournalEvent>& events, size_t captureDepth) {
    std::string text;
    for (const auto& event : events) {
        if (event.captureDepth != captureDepth) { continue; }
        if (const auto output = std::get_if<RuntimeJournalText>(&event.payload)) {
            text += output->text;
        } else if (const auto display = std::get_if<RuntimeAssignmentDisplay>(&event.payload)) {
            if (display->outputSuppressed) { continue; }
            if (display->snapshotFailure && !display->semanticTextAvailable) { return {false,{},display->snapshotError}; }
            text += display->label + " = " + display->text + "\n";
            if (display->lineSpacing == RuntimeLineSpacing::Loose) { text += '\n'; }
        } else if (event.kind == RuntimeJournalKind::Warning && event.disposition == RuntimeJournalDisposition::Visible) {
            text += "warning: " + std::get<Diagnostic>(event.payload).message + "\n";
        }
    }
    return {true,std::move(text),{}};
}

} // namespace mparser
