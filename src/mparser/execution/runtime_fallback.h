#pragma once

#include <string_view>

namespace mparser {

enum class RuntimeFallbackKind {
    None,
    RegionUnavailable,
    RegionNotClosed,
    ContainsCall,
    UnsupportedMutation,
    UnsupportedControlFlow,
    UnsupportedOperation,
    InvalidContract,
    MissingStackValue,
    UnsupportedRuntimeValue,
    UnsupportedRange,
    MissingInput,
    UnsupportedInput,
    KernelRejected,
    BackendUnavailable,
    BackendUnsupported,
    CompilationFailed,
    RuntimeFailed,
    AdaptiveRetrainingRejected,
};

std::string_view runtimeFallbackKindName(RuntimeFallbackKind kind);

} // namespace mparser
