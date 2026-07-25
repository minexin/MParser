#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace mparser {

struct RuntimeValue;

enum class RuntimeExecutionStopReason {
    None,
    Cancelled,
    InstructionLimit,
    WallTimeLimit,
    CallDepthLimit,
    ArrayByteLimit,
    DiagnosticLimit,
};

std::string_view runtimeExecutionStopReasonName(
    RuntimeExecutionStopReason reason);

struct RuntimeExecutionLimits {
    size_t maxInstructionCount = 0;
    std::chrono::nanoseconds maxWallTime{};
    size_t maxCallDepth = 0;
    size_t maxArrayBytes = 0;
    size_t maxDiagnosticCount = 0;

    bool active() const noexcept;
    bool requiresInstructionCheckpoints() const noexcept;
};

class RuntimeCancellationToken {
public:
    RuntimeCancellationToken();

    void requestCancellation() const noexcept;
    bool cancellationRequested() const noexcept;

private:
    std::shared_ptr<std::atomic_bool> state_;
};

struct RuntimeExecutionSnapshot {
    bool controlsActive = false;
    bool optimizedExecutionSuppressed = false;
    RuntimeExecutionStopReason stopReason =
        RuntimeExecutionStopReason::None;
    size_t executedInstructionCount = 0;
    size_t currentCallDepth = 0;
    size_t maximumCallDepth = 0;
    size_t maximumArrayBytes = 0;
    size_t maximumDiagnosticCount = 0;
    std::uint64_t elapsedNanoseconds = 0;
};

class RuntimeExecutionControl {
public:
    explicit RuntimeExecutionControl(
        RuntimeExecutionLimits limits = {},
        std::optional<RuntimeCancellationToken> cancellation =
            std::nullopt);

    const RuntimeExecutionLimits& limits() const noexcept;
    bool active() const noexcept;
    bool requiresInstructionCheckpoints() const noexcept;
    bool hasArrayByteLimit() const noexcept;
    bool hasDiagnosticLimit() const noexcept;

    bool checkpoint();
    bool beforeInstruction();
    bool completeInstruction();
    bool enterCall();
    void leaveCall() noexcept;
    bool observeArrayBytes(size_t bytes);
    bool observeValue(const RuntimeValue& value);
    bool observeDiagnosticCount(size_t count);
    void markOptimizedExecutionSuppressed() noexcept;

    RuntimeExecutionStopReason stopReason() const noexcept;
    RuntimeExecutionSnapshot snapshot() const noexcept;

private:
    void stop(RuntimeExecutionStopReason reason) noexcept;

    RuntimeExecutionLimits limits_;
    std::optional<RuntimeCancellationToken> cancellation_;
    std::chrono::steady_clock::time_point startedAt_;
    RuntimeExecutionStopReason stopReason_ =
        RuntimeExecutionStopReason::None;
    size_t executedInstructionCount_ = 0;
    size_t currentCallDepth_ = 0;
    size_t maximumCallDepth_ = 0;
    size_t maximumArrayBytes_ = 0;
    size_t maximumDiagnosticCount_ = 0;
    bool optimizedExecutionSuppressed_ = false;
};

} // namespace mparser
