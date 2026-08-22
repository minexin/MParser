#include "mparser/runtime/core/runtime_call_frame.h"

#include <utility>

namespace mparser {

RuntimeCallFrame makeRuntimeScriptFrame(RuntimeWorkspace workspace) {
    RuntimeCallFrame frame;
    frame.workspace = std::move(workspace);
    return frame;
}

RuntimeCallFrame makeRuntimeFunctionFrame(
    RuntimeCallFrameKind kind, std::string callable, SourceSpan span,
    size_t suppliedArgumentCount, size_t requestedOutputCount,
    RuntimeWorkspace workspace) {
    RuntimeCallFrame frame;
    frame.kind = kind;
    frame.callable = std::move(callable);
    frame.span = span;
    frame.workspace = std::move(workspace);
    setRuntimeCallFrameArity(frame, suppliedArgumentCount,
                             requestedOutputCount);
    return frame;
}

RuntimeCallFrame makeRuntimeInitializerFrame(
    std::string callable, SourceSpan span,
    RuntimeWorkspace workspace) {
    RuntimeCallFrame frame;
    frame.kind = RuntimeCallFrameKind::Initializer;
    frame.callable = std::move(callable);
    frame.span = span;
    frame.workspace = std::move(workspace);
    return frame;
}

void setRuntimeCallFrameArity(RuntimeCallFrame& frame,
                              size_t suppliedArgumentCount,
                              size_t requestedOutputCount) {
    frame.suppliedArgumentCount = suppliedArgumentCount;
    frame.requestedOutputCount = requestedOutputCount;
    frame.workspace["nargin"] = makeRuntimeNumberValue(
        static_cast<double>(suppliedArgumentCount));
    frame.workspace["nargout"] = makeRuntimeNumberValue(
        static_cast<double>(requestedOutputCount));
}

std::string_view runtimeCallFrameKindName(RuntimeCallFrameKind kind) {
    switch (kind) {
    case RuntimeCallFrameKind::Script:
        return "script";
    case RuntimeCallFrameKind::Function:
        return "function";
    case RuntimeCallFrameKind::AnonymousFunction:
        return "anonymous-function";
    case RuntimeCallFrameKind::Initializer:
        return "initializer";
    }
    return "script";
}

} // namespace mparser
