#include "mparser/runtime/core/session/runtime_execution_control.h"

#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/runtime/core/session/runtime_debugger.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace mparser {

std::string_view runtimeExecutionStopReasonName(
    RuntimeExecutionStopReason reason) {
    switch (reason) {
    case RuntimeExecutionStopReason::None:
        return "none";
    case RuntimeExecutionStopReason::Cancelled:
        return "cancelled";
    case RuntimeExecutionStopReason::InstructionLimit:
        return "instruction-limit";
    case RuntimeExecutionStopReason::WallTimeLimit:
        return "wall-time-limit";
    case RuntimeExecutionStopReason::CallDepthLimit:
        return "call-depth-limit";
    case RuntimeExecutionStopReason::ArrayByteLimit:
        return "array-byte-limit";
    case RuntimeExecutionStopReason::DiagnosticLimit:
        return "diagnostic-limit";
    }
    return "unknown";
}

bool RuntimeExecutionLimits::active() const noexcept {
    return maxInstructionCount != 0 ||
           maxWallTime.count() != 0 ||
           maxCallDepth != 0 ||
           maxArrayBytes != 0 ||
           maxDiagnosticCount != 0;
}

bool RuntimeExecutionLimits::requiresInstructionCheckpoints()
    const noexcept {
    return maxInstructionCount != 0 ||
           maxWallTime.count() != 0;
}

RuntimeCancellationToken::RuntimeCancellationToken()
    : state_(std::make_shared<std::atomic_bool>(false)) {}

void RuntimeCancellationToken::requestCancellation() const noexcept {
    state_->store(true, std::memory_order_release);
}

bool RuntimeCancellationToken::cancellationRequested() const noexcept {
    return state_->load(std::memory_order_acquire);
}

RuntimeExecutionControl::RuntimeExecutionControl(
    RuntimeExecutionLimits limits,
    std::optional<RuntimeCancellationToken> cancellation,
    std::shared_ptr<RuntimeDebugger> debugger)
    : limits_(limits),
      cancellation_(std::move(cancellation)),
      debugger_(std::move(debugger)),
      startedAt_(std::chrono::steady_clock::now()) {}

RuntimeDebugger* RuntimeExecutionControl::debugger() const noexcept {
    return debugger_.get();
}

void RuntimeExecutionControl::stopFromDebugger() noexcept {
    stop(RuntimeExecutionStopReason::Cancelled);
}

const RuntimeExecutionLimits&
RuntimeExecutionControl::limits() const noexcept {
    return limits_;
}

bool RuntimeExecutionControl::active() const noexcept {
    return limits_.active() || cancellation_.has_value() || debugger_;
}

bool RuntimeExecutionControl::requiresInstructionCheckpoints()
    const noexcept {
    return limits_.requiresInstructionCheckpoints() ||
           cancellation_.has_value() || debugger_;
}

bool RuntimeExecutionControl::hasArrayByteLimit() const noexcept {
    return limits_.maxArrayBytes != 0;
}

bool RuntimeExecutionControl::hasDiagnosticLimit() const noexcept {
    return limits_.maxDiagnosticCount != 0;
}

bool RuntimeExecutionControl::checkpoint() {
    if (stopReason_ != RuntimeExecutionStopReason::None) {
        return false;
    }
    if (cancellation_ &&
        cancellation_->cancellationRequested()) {
        stop(RuntimeExecutionStopReason::Cancelled);
        return false;
    }
    if (limits_.maxWallTime.count() < 0 ||
        (limits_.maxWallTime.count() > 0 &&
         std::chrono::steady_clock::now() - startedAt_ >=
             limits_.maxWallTime)) {
        stop(RuntimeExecutionStopReason::WallTimeLimit);
        return false;
    }
    return true;
}

bool RuntimeExecutionControl::beforeInstruction() {
    if (!checkpoint()) {
        return false;
    }
    if (limits_.maxInstructionCount != 0 &&
        executedInstructionCount_ >=
            limits_.maxInstructionCount) {
        stop(RuntimeExecutionStopReason::InstructionLimit);
        return false;
    }
    return true;
}

bool RuntimeExecutionControl::completeInstruction() {
    if (executedInstructionCount_ !=
        std::numeric_limits<size_t>::max()) {
        ++executedInstructionCount_;
    }
    return checkpoint();
}

bool RuntimeExecutionControl::enterCall() {
    if (!checkpoint()) {
        return false;
    }
    if (limits_.maxCallDepth != 0 &&
        currentCallDepth_ >= limits_.maxCallDepth) {
        stop(RuntimeExecutionStopReason::CallDepthLimit);
        return false;
    }
    if (currentCallDepth_ !=
        std::numeric_limits<size_t>::max()) {
        ++currentCallDepth_;
    }
    maximumCallDepth_ =
        std::max(maximumCallDepth_, currentCallDepth_);
    return true;
}

void RuntimeExecutionControl::leaveCall() noexcept {
    if (currentCallDepth_ != 0) {
        --currentCallDepth_;
    }
}

bool RuntimeExecutionControl::observeArrayBytes(size_t bytes) {
    maximumArrayBytes_ = std::max(maximumArrayBytes_, bytes);
    if (limits_.maxArrayBytes != 0 &&
        bytes > limits_.maxArrayBytes) {
        stop(RuntimeExecutionStopReason::ArrayByteLimit);
        return false;
    }
    return stopReason_ == RuntimeExecutionStopReason::None;
}

bool RuntimeExecutionControl::observeValue(
    const RuntimeValue& value) {
    const auto bytes = runtimeValueArrayBytes(value);
    return observeArrayBytes(
        bytes.value_or(std::numeric_limits<size_t>::max()));
}

bool RuntimeExecutionControl::observeDiagnosticCount(size_t count) {
    maximumDiagnosticCount_ =
        std::max(maximumDiagnosticCount_, count);
    if (limits_.maxDiagnosticCount != 0 &&
        count > limits_.maxDiagnosticCount) {
        stop(RuntimeExecutionStopReason::DiagnosticLimit);
        return false;
    }
    return stopReason_ == RuntimeExecutionStopReason::None;
}

void RuntimeExecutionControl::markOptimizedExecutionSuppressed()
    noexcept {
    optimizedExecutionSuppressed_ = true;
}

RuntimeExecutionStopReason
RuntimeExecutionControl::stopReason() const noexcept {
    return stopReason_;
}

RuntimeExecutionSnapshot
RuntimeExecutionControl::snapshot() const noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startedAt_);
    const auto elapsedCount = elapsed.count();
    const auto maximum =
        std::numeric_limits<std::uint64_t>::max();

    RuntimeExecutionSnapshot result;
    result.controlsActive = active();
    result.optimizedExecutionSuppressed =
        optimizedExecutionSuppressed_;
    result.stopReason = stopReason_;
    result.executedInstructionCount =
        executedInstructionCount_;
    result.currentCallDepth = currentCallDepth_;
    result.maximumCallDepth = maximumCallDepth_;
    result.maximumArrayBytes = maximumArrayBytes_;
    result.maximumDiagnosticCount =
        maximumDiagnosticCount_;
    result.elapsedNanoseconds =
        elapsedCount <= 0
            ? 0
            : static_cast<std::uint64_t>(
                  static_cast<unsigned long long>(elapsedCount) >
                          maximum
                      ? maximum
                      : static_cast<unsigned long long>(
                            elapsedCount));
    return result;
}

void RuntimeExecutionControl::stop(
    RuntimeExecutionStopReason reason) noexcept {
    if (stopReason_ == RuntimeExecutionStopReason::None) {
        stopReason_ = reason;
    }
}

} // namespace mparser
