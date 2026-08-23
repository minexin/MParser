#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace mparser {

enum class RuntimeCallFrameKind {
    Script,
    Function,
    AnonymousFunction,
    Initializer,
};

struct RuntimePersistentScope {
    size_t contextIdentity = 0;
    std::string function;
};

struct RuntimeCallFrame {
    RuntimeCallFrameKind kind = RuntimeCallFrameKind::Script;
    std::string callable;
    SourceSpan span;
    size_t suppliedArgumentCount = 0;
    size_t requestedOutputCount = 0;
    RuntimeWorkspace workspace;
    std::set<std::string> globalBindings;
    std::set<std::string> persistentBindings;
    std::optional<RuntimePersistentScope> persistentScope;
    bool dynamicPersistentDeclarationsAllowed = false;
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
