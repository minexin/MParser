#pragma once

#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_output.h"
#include "mparser/runtime_value.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

class RuntimeSystemContext;

enum class ModuleExecutionBackend {
    Automatic,
    Bytecode,
    Portable,
    Native,
};

enum class ModuleExecutionTier {
    Bytecode,
    Portable,
    Native,
    Mixed,
};

enum class ModuleInvocationStatus {
    Succeeded,
    CompilationFailed,
    RequestRejected,
    RuntimeFailed,
};

enum class ModuleDiagnosticPhase {
    Compilation,
    Validation,
    Execution,
};

enum class ModuleDiagnosticSeverity {
    Error,
    Warning,
};

std::string_view moduleExecutionBackendName(
    ModuleExecutionBackend backend);
std::string_view moduleExecutionTierName(ModuleExecutionTier tier);
std::string_view moduleInvocationStatusName(
    ModuleInvocationStatus status);
std::string_view moduleDiagnosticPhaseName(
    ModuleDiagnosticPhase phase);
std::string_view moduleDiagnosticSeverityName(
    ModuleDiagnosticSeverity severity);

enum class ModuleOutputKind {
    Display,
    StandardOutput,
};

std::string_view moduleOutputKindName(ModuleOutputKind kind);

struct ModuleSourcePosition {
    size_t offset = 0;
    int line = 1;
    int column = 1;
};

struct ModuleSourceRange {
    bool available = false;
    std::string sourceName;
    ModuleSourcePosition begin;
    ModuleSourcePosition end;
};

struct ModuleOutputEvent {
    ModuleOutputKind kind = ModuleOutputKind::StandardOutput;
    std::string text;
    ModuleSourceRange source;
    std::uint64_t sequence = 0;
};

struct ModuleTopLevelExpression {
    RuntimeValue value;
    ModuleSourceRange source;
    bool outputSuppressed = false;
    std::uint64_t sequence = 0;
    std::string displayText = {};
    RuntimeLineSpacing lineSpacing = RuntimeLineSpacing::Loose;
};

using ModuleOutputSink =
    std::function<bool(const ModuleOutputEvent& event)>;

struct ModuleDiagnosticFrame {
    std::string sourceName;
    std::string functionName;
    int line = 1;
};

struct ModuleDiagnosticCause {
    std::string identifier;
    std::string message;
    std::vector<ModuleDiagnosticFrame> stack;
    std::vector<ModuleDiagnosticCause> causes;
};

struct ModuleDiagnostic {
    ModuleDiagnosticPhase phase = ModuleDiagnosticPhase::Execution;
    ModuleDiagnosticSeverity severity =
        ModuleDiagnosticSeverity::Error;
    std::string identifier;
    std::string message;
    ModuleSourceRange source;
    std::vector<ModuleDiagnosticFrame> stack;
    std::vector<ModuleDiagnosticCause> causes;
};

struct ModuleInvocationRequest {
    std::string entryFunction;
    std::vector<RuntimeValue> arguments;
    std::optional<size_t> requestedOutputCount;
    std::vector<RuntimeVariable> initialWorkspace;
    ModuleExecutionBackend backend =
        ModuleExecutionBackend::Automatic;
    bool collectProfile = false;
    RuntimeExecutionLimits limits;
    std::optional<RuntimeCancellationToken> cancellationToken;
    ModuleOutputSink outputSink;
    std::shared_ptr<RuntimeSystemContext> systemContext = {};
};

struct ModuleExecutionSummary {
    ModuleExecutionBackend requestedBackend =
        ModuleExecutionBackend::Automatic;
    ModuleExecutionTier effectiveTier =
        ModuleExecutionTier::Bytecode;
    bool profilingCollected = false;
    bool fallbackOccurred = false;
    bool resourceControlsActive = false;
    bool optimizedExecutionSuppressed = false;
    RuntimeExecutionStopReason stopReason =
        RuntimeExecutionStopReason::None;
    size_t executedInstructionCount = 0;
    size_t typedRegionCount = 0;
    size_t typedRegionAttemptCount = 0;
    size_t typedRegionExecutionCount = 0;
    size_t typedRegionFallbackCount = 0;
    size_t nativeCompilationCount = 0;
    size_t nativeCacheHitCount = 0;
    size_t maximumCallDepth = 0;
    size_t maximumArrayBytes = 0;
    size_t maximumDiagnosticCount = 0;
    std::uint64_t elapsedNanoseconds = 0;
};

struct ModuleInvocationResult {
    ModuleInvocationStatus status =
        ModuleInvocationStatus::CompilationFailed;
    std::string entryFunction;
    size_t requestedOutputCount = 0;
    std::vector<std::string> outputNames;
    std::vector<RuntimeValue> outputs;
    std::vector<ModuleOutputEvent> outputEvents;
    std::vector<ModuleTopLevelExpression> topLevelExpressions;
    std::vector<RuntimeVariable> variables;
    std::vector<ModuleDiagnostic> diagnostics;
    ModuleExecutionSummary execution;

    bool succeeded() const noexcept;
    bool hasWarnings() const noexcept;
};

} // namespace mparser
