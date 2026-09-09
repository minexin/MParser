#include "mparser/runtime/core/session/runtime_debugger.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/runtime/core/value/runtime_numeric.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mparser {

void appendRuntimeDebugFrame(std::vector<RuntimeDebugFrame>& destination,
    const RuntimeCallFrame& frame,
    const std::function<std::string(size_t)>& sourceName,
    const RuntimeSessionState& session,
    const std::deque<RuntimeCallFrame>& activeFrames) {
    if (!frame.debugLocation) {
        return;
    }
    RuntimeDebugFrame snapshot;
    snapshot.kind = frame.kind;
    snapshot.functionName = frame.callable.empty() ? "<script>"
                                                   : frame.callable;
    snapshot.location = *frame.debugLocation;
    const auto sourceId = snapshot.location.begin.sourceId;
    snapshot.sourceName = sourceName(sourceId);
    snapshot.suppliedArgumentCount = frame.suppliedArgumentCount;
    snapshot.requestedOutputCount = frame.requestedOutputCount;
    snapshot.variables = frame.debugWorkspaceAlias ? *frame.debugWorkspaceAlias : frame.workspace;
    // Nested captures commit on return. Inspect their live copies while paused.
    std::map<std::string, const RuntimeCallFrame*> liveOwners;
    bool afterFrame = false;
    for (const auto& nested : activeFrames) {
        if (&nested == &frame) {
            afterFrame = true;
            continue;
        }
        if (!afterFrame || !nested.debugCaptureOwners) {
            continue;
        }
        for (const auto& [name, owner] : *nested.debugCaptureOwners) {
            const auto previous = liveOwners.find(name);
            const auto* expected = previous == liveOwners.end()
                ? &frame : previous->second;
            if (owner >= activeFrames.size() || &activeFrames[owner] != expected) {
                continue;
            }
            if (const auto value = nested.workspace.find(name);
                value != nested.workspace.end()) {
                snapshot.variables[name] = value->second;
                liveOwners[name] = &nested;
            }
        }
    }
    for (const auto& name : frame.globalBindings) {
        if (const auto value = session.findGlobal(name)) {
            snapshot.variables[name] = *value;
        }
    }
    if (frame.persistentScope) {
        for (const auto& name : frame.persistentBindings) {
            if (const auto value = session.findPersistent(
                    frame.persistentScope->contextIdentity,
                    frame.persistentScope->function, name)) {
                snapshot.variables[name] = *value;
            }
        }
    }
    destination.push_back(std::move(snapshot));
}

RuntimeDebugEvaluationResult evaluateRuntimeDebugFrame(
    RuntimeCallFrame& frame, const RuntimeSessionState& session,
    std::deque<RuntimeCallFrame>& activeFrames,
    const std::function<RuntimeDebugEvaluationResult()>& evaluate) {
    if (frame.debugWorkspaceAlias) {
        frame.workspace = *frame.debugWorkspaceAlias;
    }
    // Paused outer frames may have a newer captured value in a nested frame.
    std::map<std::string, RuntimeCallFrame*> liveOwners;
    bool afterFrame = false;
    for (auto& nested : activeFrames) {
        if (&nested == &frame) {
            afterFrame = true;
            continue;
        }
        if (!afterFrame || !nested.debugCaptureOwners) {
            continue;
        }
        for (const auto& [name, owner] : *nested.debugCaptureOwners) {
            const auto previous = liveOwners.find(name);
            const auto* expected = previous == liveOwners.end() ? &frame : previous->second;
            if (owner < activeFrames.size() && &activeFrames[owner] == expected &&
                nested.workspace.contains(name)) {
                liveOwners[name] = &nested;
            }
        }
    }
    for (const auto& [name, owner] : liveOwners) {
        frame.workspace[name] = owner->workspace.at(name);
    }
    for (const auto& name : frame.globalBindings) {
        if (const auto value = session.findGlobal(name)) {
            frame.workspace[name] = *value;
        }
    }
    if (frame.persistentScope) {
        for (const auto& name : frame.persistentBindings) {
            if (const auto value = session.findPersistent(
                    frame.persistentScope->contextIdentity, frame.persistentScope->function, name)) {
                frame.workspace[name] = *value;
            }
        }
    }
    const auto writeBack = [&] {
        if (frame.debugWorkspaceAlias) {
            *frame.debugWorkspaceAlias = frame.workspace;
        }
        for (const auto& [name, owner] : liveOwners) {
            if (const auto value = frame.workspace.find(name); value != frame.workspace.end()) {
                owner->workspace[name] = value->second;
            } else {
                owner->workspace.erase(name);
            }
        }
    };
    RuntimeDebugEvaluationResult result;
    try {
        result = evaluate();
    } catch (...) {
        // Completed edits are not transactional, including host exceptions.
        // Keep live captures and a borrowed workspace consistent before rethrow.
        writeBack();
        throw;
    }
    writeBack();
    return result;
}

RuntimeDebugger::RuntimeDebugger(RuntimeDebugSink sink)
    : sink_(std::move(sink)) {
    if (!sink_) {
        throw std::invalid_argument("debugger requires a pause callback");
    }
}

void RuntimeDebugger::setBreakpoints(
    std::vector<RuntimeBreakpoint> breakpoints) {
    for (const auto& breakpoint : breakpoints) {
        if (breakpoint.sourceName.empty() || breakpoint.line <= 0) {
            throw std::invalid_argument(
                "breakpoint requires a source name and a positive line");
        }
    }
    const std::lock_guard lock(mutex_);
    breakpoints_ = std::move(breakpoints);
}

void RuntimeDebugger::setSourceBreakpoint(RuntimeBreakpoint breakpoint) {
    if (breakpoint.sourceName.empty() || breakpoint.line <= 0) {
        throw std::invalid_argument("breakpoint requires a source name and a positive line");
    }
    const std::lock_guard lock(mutex_);
    auto replacement = breakpoints_;
    std::erase_if(replacement, [&](const auto& existing) {
        return existing.sourceName == breakpoint.sourceName && existing.line == breakpoint.line;
    });
    replacement.push_back(std::move(breakpoint));
    breakpoints_.swap(replacement);
}

void RuntimeDebugger::clearSourceBreakpoints(const std::string& sourceName, int line) {
    if (sourceName.empty() || line < 0) {
        throw std::invalid_argument("invalid source breakpoint removal");
    }
    const std::lock_guard lock(mutex_);
    std::erase_if(breakpoints_, [&](const auto& existing) {
        return existing.sourceName == sourceName && (line == 0 || existing.line == line);
    });
}

std::vector<RuntimeBreakpoint> RuntimeDebugger::breakpoints() {
    const std::lock_guard lock(mutex_);
    return breakpoints_;
}

std::optional<std::vector<RuntimeDebugFrame>> RuntimeDebugger::sourceStack() {
    const std::lock_guard lock(mutex_);
    if (!hostCallbackActive_ || !evaluating_ ||
        executionThread_ != std::this_thread::get_id()) {
        return std::nullopt;
    }
    return pausedFrames_;
}

bool RuntimeDebugger::requestSourceAction(RuntimeDebugAction action) {
    const std::lock_guard lock(mutex_);
    if (!hostCallbackActive_ || !evaluating_ ||
        executionThread_ != std::this_thread::get_id()) {
        return false;
    }
    switch (action) {
    case RuntimeDebugAction::Continue:
    case RuntimeDebugAction::StepInto:
    case RuntimeDebugAction::StepOver:
    case RuntimeDebugAction::StepOut:
    case RuntimeDebugAction::Stop:
        break;
    default:
        return false;
    }
    // A queued stop cannot be cancelled by a later source command.
    if (sourceAction_ != RuntimeDebugAction::Stop) { sourceAction_ = action; }
    return true;
}

bool RuntimeDebugger::moveSourceFrame(bool towardCaller, size_t count) {
    const std::lock_guard lock(mutex_);
    if (!hostCallbackActive_ || !evaluating_ || pausedFrames_.empty() ||
        executionThread_ != std::this_thread::get_id()) {
        return false;
    }
    if (towardCaller) {
        if (count > selectedFrame_) { return false; }
        selectedFrame_ -= count;
    } else {
        if (count > pausedFrames_.size() - 1 - selectedFrame_) { return false; }
        selectedFrame_ += count;
    }
    return true;
}

std::optional<size_t> RuntimeDebugger::selectedSourceFrame() {
    const std::lock_guard lock(mutex_);
    if (!hostCallbackActive_ || pausedFrames_.empty() ||
        executionThread_ != std::this_thread::get_id()) {
        return std::nullopt;
    }
    return selectedFrame_;
}

void RuntimeDebugger::requestPause() {
    const std::lock_guard lock(mutex_);
    pauseRequested_ = true;
}

void RuntimeDebugger::enterScope(RuntimeDebugFrameProvider provider) {
    const std::lock_guard lock(mutex_);
    if ((!scopes_.empty() &&
         executionThread_ != std::this_thread::get_id()) ||
        (inCallback_ && !evaluating_)) {
        throw std::logic_error(
            "a debugger cannot execute concurrently or from its pause callback");
    }
    scopes_.push_back(std::move(provider));
    executionThread_ = std::this_thread::get_id();
}

void RuntimeDebugger::leaveScope() noexcept {
    const std::lock_guard lock(mutex_);
    scopes_.pop_back();
    if (scopes_.empty()) {
        executionThread_ = {};
        action_ = RuntimeDebugAction::Continue;
        stepDepth_ = 0;
    }
}

bool RuntimeDebugger::statement(const std::string& sourceName,
                                const SourceSpan& location) {
    RuntimeDebugEvent event;
    bool breakpoint = false;
    bool pause = false;
    bool step = false;
    std::vector<std::string> conditions;
    {
        const std::lock_guard lock(mutex_);
        if (action_ == RuntimeDebugAction::Stop) {
            return false;
        }
        if (evaluating_) {
            return true;
        }
        for (const auto& point : breakpoints_) {
            if (point.sourceName == sourceName && point.line == location.begin.line) {
                conditions.push_back(point.condition);
            }
        }
        breakpoint = !conditions.empty();
        pause = std::exchange(pauseRequested_, false);
        if (!breakpoint && !pause &&
            action_ == RuntimeDebugAction::Continue) {
            return true;
        }
        for (const auto& provider : scopes_) {
            provider(event.frames);
        }
        step = action_ == RuntimeDebugAction::StepInto ||
            (action_ == RuntimeDebugAction::StepOver &&
             event.frames.size() <= stepDepth_) ||
            (action_ == RuntimeDebugAction::StepOut &&
             event.frames.size() < stepDepth_);
        if (!breakpoint && !pause && !step) {
            return true;
        }
        event.reason = pause ? RuntimeDebugReason::PauseRequest
                             : breakpoint ? RuntimeDebugReason::Breakpoint
                                          : RuntimeDebugReason::Step;
        event.sequence = sequence_++;
        pausedSequence_ = event.sequence;
        pausedEvaluators_.clear();
        for (auto& frame : event.frames) {
            pausedEvaluators_.push_back(std::move(frame.evaluator_));
        }
        inCallback_ = true;
        sourceAction_.reset();
    }

    RuntimeDebugAction action;
    try {
        if (breakpoint) {
            breakpoint = false;
            for (const auto& condition : conditions) {
                if (condition.empty()) {
                    breakpoint = true;
                    break;
                }
                auto result = evaluate(event.sequence,
                    event.frames.empty() ? 0 : event.frames.size() - 1, {condition, 1});
                if (!result.succeeded) {
                    event.conditionDiagnostics = std::move(result.diagnostics);
                    if (event.conditionDiagnostics.empty()) {
                        event.conditionDiagnostics.push_back(Diagnostic{location,
                            "breakpoint condition evaluation failed",
                            "MParser:Debugger:InvalidCondition"});
                    }
                    breakpoint = true;
                    break;
                }
                const auto truth = result.outputs.size() == 1
                    ? runtimeNumericTruthValue(result.outputs.front()) : std::nullopt;
                if (!truth) {
                    event.conditionDiagnostics.push_back(Diagnostic{location,
                        "breakpoint condition must return a numeric or logical truth value",
                        "MParser:Debugger:InvalidCondition"});
                    breakpoint = true;
                    break;
                }
                if (*truth) {
                    breakpoint = true;
                    break;
                }
            }
        }
        if (!breakpoint && !pause && !step) {
            const std::lock_guard lock(mutex_);
            inCallback_ = false;
            pausedEvaluators_.clear();
            return true;
        }
        for (auto& diagnostic : event.conditionDiagnostics) {
            if (diagnostic.sourceName.empty()) {
                diagnostic.sourceName = sourceName;
            }
        }
        event.reason = pause ? RuntimeDebugReason::PauseRequest
                             : breakpoint ? RuntimeDebugReason::Breakpoint
                                          : RuntimeDebugReason::Step;
        if (!conditions.empty()) {
            const std::lock_guard lock(mutex_);
            event.frames.clear();
            pausedEvaluators_.clear();
            for (const auto& provider : scopes_) { provider(event.frames); }
            for (auto& frame : event.frames) {
                pausedEvaluators_.push_back(std::move(frame.evaluator_));
            }
        }
        // No configuration lock is held while the host pauses or inspects.
        {
            const std::lock_guard lock(mutex_);
            hostCallbackActive_ = true;
            pausedFrames_ = event.frames;
            selectedFrame_ = event.frames.empty() ? 0 : event.frames.size() - 1;
        }
        action = sink_(event);
        switch (action) {
        case RuntimeDebugAction::Continue:
        case RuntimeDebugAction::StepInto:
        case RuntimeDebugAction::StepOver:
        case RuntimeDebugAction::StepOut:
        case RuntimeDebugAction::Stop:
            break;
        default:
            throw std::invalid_argument("invalid debugger resume action");
        }
    } catch (...) {
        const std::lock_guard lock(mutex_);
        inCallback_ = false;
        hostCallbackActive_ = false;
        pausedFrames_.clear();
        sourceAction_.reset();
        pausedEvaluators_.clear();
        action_ = RuntimeDebugAction::Stop;
        throw;
    }
    const std::lock_guard lock(mutex_);
    inCallback_ = false;
    hostCallbackActive_ = false;
    pausedFrames_.clear();
    if (action != RuntimeDebugAction::Stop && sourceAction_) { action = *sourceAction_; }
    sourceAction_.reset();
    pausedEvaluators_.clear();
    action_ = action;
    stepDepth_ = event.frames.size();
    return action != RuntimeDebugAction::Stop;
}

RuntimeDebugEvaluationResult RuntimeDebugger::evaluate(
    size_t eventSequence, size_t frameIndex,
    const RuntimeDebugEvaluationRequest& request) {
    const auto rejected = [](const char* message, const char* identifier) {
        RuntimeDebugEvaluationResult result;
        result.diagnostics.push_back(Diagnostic{{}, message, identifier});
        return result;
    };
    RuntimeDebugEvaluator evaluator;
    {
        const std::lock_guard lock(mutex_);
        if (!inCallback_ || eventSequence != pausedSequence_) {
            return rejected("debug event is no longer paused",
                            "MParser:Debugger:StaleEvent");
        }
        if (executionThread_ != std::this_thread::get_id()) {
            return rejected("debug evaluation requires the execution thread",
                            "MParser:Debugger:WrongThread");
        }
        if (evaluating_) {
            return rejected("debug evaluation cannot reenter itself",
                            "MParser:Debugger:ReentrantEvaluation");
        }
        if (frameIndex == selectedFrame && hostCallbackActive_) {
            frameIndex = selectedFrame_;
        }
        if (frameIndex >= pausedEvaluators_.size()) {
            return rejected("debug frame index is out of range",
                            "MParser:Debugger:InvalidFrame");
        }
        evaluator = pausedEvaluators_[frameIndex];
        if (!evaluator) {
            return rejected("debug frame does not support evaluation",
                            "MParser:Debugger:EvaluationUnavailable");
        }
        evaluating_ = true;
    }
    try {
        auto result = evaluator(request);
        const std::lock_guard lock(mutex_);
        evaluating_ = false;
        return result;
    } catch (...) {
        const std::lock_guard lock(mutex_);
        evaluating_ = false;
        throw;
    }
}

RuntimeDebugScope::RuntimeDebugScope(RuntimeDebugger* debugger,
                                   RuntimeDebugFrameProvider provider)
    : debugger_(debugger) {
    if (debugger_) {
        debugger_->enterScope(std::move(provider));
    }
}

RuntimeDebugScope::~RuntimeDebugScope() {
    leave();
}

void RuntimeDebugScope::leave() noexcept {
    if (debugger_) {
        debugger_->leaveScope();
        debugger_ = nullptr;
    }
}

} // namespace mparser
