#include "mparser/module_execution.h"

namespace mparser {

std::string_view moduleExecutionBackendName(
    ModuleExecutionBackend backend) {
    switch (backend) {
    case ModuleExecutionBackend::Automatic:
        return "automatic";
    case ModuleExecutionBackend::Bytecode:
        return "bytecode";
    case ModuleExecutionBackend::Portable:
        return "portable";
    case ModuleExecutionBackend::Native:
        return "native";
    }
    return "unknown";
}

std::string_view moduleExecutionTierName(ModuleExecutionTier tier) {
    switch (tier) {
    case ModuleExecutionTier::Bytecode:
        return "bytecode";
    case ModuleExecutionTier::Portable:
        return "portable";
    case ModuleExecutionTier::Native:
        return "native";
    case ModuleExecutionTier::Mixed:
        return "mixed";
    }
    return "unknown";
}

std::string_view moduleInvocationStatusName(
    ModuleInvocationStatus status) {
    switch (status) {
    case ModuleInvocationStatus::Succeeded:
        return "succeeded";
    case ModuleInvocationStatus::CompilationFailed:
        return "compilation-failed";
    case ModuleInvocationStatus::RequestRejected:
        return "request-rejected";
    case ModuleInvocationStatus::RuntimeFailed:
        return "runtime-failed";
    }
    return "unknown";
}

std::string_view moduleDiagnosticPhaseName(
    ModuleDiagnosticPhase phase) {
    switch (phase) {
    case ModuleDiagnosticPhase::Compilation:
        return "compilation";
    case ModuleDiagnosticPhase::Validation:
        return "validation";
    case ModuleDiagnosticPhase::Execution:
        return "execution";
    }
    return "unknown";
}

std::string_view moduleDiagnosticSeverityName(
    ModuleDiagnosticSeverity severity) {
    switch (severity) {
    case ModuleDiagnosticSeverity::Error:
        return "error";
    case ModuleDiagnosticSeverity::Warning:
        return "warning";
    }
    return "unknown";
}

bool ModuleInvocationResult::succeeded() const noexcept {
    return status == ModuleInvocationStatus::Succeeded;
}

bool ModuleInvocationResult::hasWarnings() const noexcept {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity ==
            ModuleDiagnosticSeverity::Warning) {
            return true;
        }
    }
    return false;
}

} // namespace mparser
