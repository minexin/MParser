#pragma once

#include "mparser/frontend/diagnostic.h"
#include "mparser/runtime/core/session/runtime_output.h"

#include <variant>

namespace mparser {
class RuntimeExecutionControl;

enum class RuntimeJournalKind { Assignment, Expression, Text, Warning, Error };
enum class RuntimeJournalDisposition { Visible, Raised, Caught, Suppressed, Unhandled };

struct RuntimeJournalText {
    RuntimeOutputKind kind = RuntimeOutputKind::StandardOutput;
    std::string text;
    SourceSpan span;
    std::string sourceName;
};

struct RuntimeJournalEvent {
    RuntimeJournalKind kind = RuntimeJournalKind::Assignment;
    std::uint64_t sequence = 0;
    std::uint64_t sourceContext = 0;
    size_t captureDepth = 0;
    RuntimeJournalDisposition disposition = RuntimeJournalDisposition::Visible;
    std::variant<RuntimeAssignmentDisplay, RuntimeJournalText, Diagnostic> payload;
};

// One invocation, one serial producer chain. This is an internal transcript,
// not a thread-safe live sink or a public wire contract.
class RuntimeEventJournal {
public:
    std::uint64_t sourceContext() { return ++nextContext_; }
    size_t size() const noexcept { return events_.size(); }
    std::uint64_t cursor() const noexcept { return nextSequence_; }
    void text(RuntimeOutputEvent& event, std::uint64_t context, size_t captureDepth, std::string sourceName);
    void display(RuntimeAssignmentDisplay& event, RuntimeJournalKind kind, std::uint64_t context, size_t captureDepth);
    void expression(RuntimeExpressionResult& expression, std::uint64_t context, size_t captureDepth, std::string sourceName);
    void diagnostic(Diagnostic& diagnostic, std::uint64_t context, size_t captureDepth, std::string sourceName);
    void caught(const Diagnostic& diagnostic);
    void unhandled(const Diagnostic& diagnostic);
    void suppressed(const Diagnostic& diagnostic);
    void complete();
    std::vector<RuntimeJournalEvent> snapshot(size_t begin = 0) const;
private:
    RuntimeJournalEvent* find(const ExecutionEventStamp& stamp);
    std::shared_ptr<const ExecutionEventIdentity> identity_ = std::make_shared<const ExecutionEventIdentity>();
    std::uint64_t nextSequence_ = 0, nextContext_ = 0;
    std::vector<RuntimeJournalEvent> events_;
};

class RuntimeEventJournalScope {
public:
    RuntimeEventJournalScope(RuntimeExecutionControl* control, bool requested);
    ~RuntimeEventJournalScope();
    RuntimeEventJournalScope(const RuntimeEventJournalScope&) = delete;
    RuntimeEventJournalScope& operator=(const RuntimeEventJournalScope&) = delete;
    const std::shared_ptr<RuntimeEventJournal>& journal() const noexcept { return journal_; }
    std::uint64_t sourceContext() const noexcept { return context_; }
    std::vector<RuntimeJournalEvent> snapshot() const;
private:
    RuntimeExecutionControl* control_;
    std::shared_ptr<RuntimeEventJournal> journal_;
    bool owner_ = false;
    size_t begin_ = 0;
    std::uint64_t context_ = 0;
};

RuntimeFormatResult runtimeRenderEventJournal(const std::vector<RuntimeJournalEvent>& events, size_t captureDepth);

} // namespace mparser
