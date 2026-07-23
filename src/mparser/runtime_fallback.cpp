#include "mparser/runtime_fallback.h"

namespace mparser {

std::string_view runtimeFallbackKindName(RuntimeFallbackKind kind) {
    switch (kind) {
    case RuntimeFallbackKind::None:
        return "none";
    case RuntimeFallbackKind::RegionUnavailable:
        return "region-unavailable";
    case RuntimeFallbackKind::RegionNotClosed:
        return "region-not-closed";
    case RuntimeFallbackKind::ContainsCall:
        return "contains-call";
    case RuntimeFallbackKind::UnsupportedMutation:
        return "unsupported-mutation";
    case RuntimeFallbackKind::UnsupportedControlFlow:
        return "unsupported-control-flow";
    case RuntimeFallbackKind::UnsupportedOperation:
        return "unsupported-operation";
    case RuntimeFallbackKind::InvalidContract:
        return "invalid-contract";
    case RuntimeFallbackKind::MissingStackValue:
        return "missing-stack-value";
    case RuntimeFallbackKind::UnsupportedRuntimeValue:
        return "unsupported-runtime-value";
    case RuntimeFallbackKind::UnsupportedRange:
        return "unsupported-range";
    case RuntimeFallbackKind::MissingInput:
        return "missing-input";
    case RuntimeFallbackKind::UnsupportedInput:
        return "unsupported-input";
    case RuntimeFallbackKind::KernelRejected:
        return "kernel-rejected";
    case RuntimeFallbackKind::BackendUnavailable:
        return "backend-unavailable";
    case RuntimeFallbackKind::BackendUnsupported:
        return "backend-unsupported";
    case RuntimeFallbackKind::CompilationFailed:
        return "compilation-failed";
    case RuntimeFallbackKind::RuntimeFailed:
        return "runtime-failed";
    case RuntimeFallbackKind::AdaptiveRetrainingRejected:
        return "adaptive-retraining-rejected";
    }
    return "none";
}

} // namespace mparser
