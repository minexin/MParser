#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace mparser {

enum class RuntimeCallFrameKind {
    Script,
    Function,
    AnonymousFunction,
    Initializer,
};

struct RuntimeCallFrame {
    RuntimeCallFrameKind kind = RuntimeCallFrameKind::Script;
    std::string callable;
    SourceSpan span;
    size_t suppliedArgumentCount = 0;
    size_t requestedOutputCount = 0;
    RuntimeWorkspace workspace;
};

RuntimeCallFrame makeRuntimeScriptFrame(
    RuntimeWorkspace workspace = {});

RuntimeCallFrame makeRuntimeFunctionFrame(
    RuntimeCallFrameKind kind, std::string callable, SourceSpan span,
    size_t suppliedArgumentCount, size_t requestedOutputCount,
    RuntimeWorkspace workspace = {});

RuntimeCallFrame makeRuntimeInitializerFrame(
    std::string callable, SourceSpan span,
    RuntimeWorkspace workspace = {});

void setRuntimeCallFrameArity(RuntimeCallFrame& frame,
                              size_t suppliedArgumentCount,
                              size_t requestedOutputCount);

std::string_view runtimeCallFrameKindName(RuntimeCallFrameKind kind);

} // namespace mparser
