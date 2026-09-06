#include "mparser/runtime/core/session/runtime_debugger.h"
#include "mparser/runtime/core/session/runtime_session_state.h"

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
    snapshot.variables = frame.workspace;
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

void RuntimeDebugger::requestPause() {
    const std::lock_guard lock(mutex_);
    pauseRequested_ = true;
}

void RuntimeDebugger::enterScope(RuntimeDebugFrameProvider provider) {
    const std::lock_guard lock(mutex_);
    if ((!scopes_.empty() &&
         executionThread_ != std::this_thread::get_id()) || inCallback_) {
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
    {
        const std::lock_guard lock(mutex_);
        if (action_ == RuntimeDebugAction::Stop) {
            return false;
        }
        breakpoint = std::any_of(
            breakpoints_.begin(), breakpoints_.end(), [&](const auto& point) {
                return point.sourceName == sourceName &&
                       point.line == location.begin.line;
            });
        pause = std::exchange(pauseRequested_, false);
        if (!breakpoint && !pause &&
            action_ == RuntimeDebugAction::Continue) {
            return true;
        }
        for (const auto& provider : scopes_) {
            provider(event.frames);
        }
        const bool step = action_ == RuntimeDebugAction::StepInto ||
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
        inCallback_ = true;
    }

    RuntimeDebugAction action;
    try {
        // No configuration lock is held while the host pauses or inspects.
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
        action_ = RuntimeDebugAction::Stop;
        throw;
    }
    const std::lock_guard lock(mutex_);
    inCallback_ = false;
    action_ = action;
    stepDepth_ = event.frames.size();
    return action != RuntimeDebugAction::Stop;
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
