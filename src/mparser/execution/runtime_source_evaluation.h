#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/runtime_execution_control.h"
#include "mparser/runtime/core/runtime_session_state.h"
#include "mparser/execution/jit/typed_region_executor.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mparser {

struct RuntimeSourceCallable {
    std::string name;
    RuntimeValue callable;
    size_t implicitOutputCount = 1;
    bool textResolutionAllowed = false;
};

struct RuntimeSourceCallableScope {
    RuntimeWorkspace* workspace = nullptr;
    std::vector<RuntimeSourceCallable> callables;
};

struct RuntimeSourceCallableInvocationResult {
    bool succeeded = false;
    std::vector<RuntimeValue> outputs;
    std::vector<Diagnostic> diagnostics;
    std::vector<RuntimeOutputEvent> outputEvents;
};

using RuntimeSourceCallableInvoker = std::function<
    RuntimeSourceCallableInvocationResult(
        const RuntimeValue& callable,
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount, SourceSpan span,
        RuntimeWorkspace* ownerWorkspace)>;

struct RuntimeSourceEvaluationOptions {
    std::shared_ptr<const BuiltinRegistry> builtinRegistry;
    std::shared_ptr<RuntimeSessionState> sessionState;
    std::shared_ptr<RuntimeExecutionControl> executionControl;
    RuntimeOutputSink outputSink;
    TypedRegionBackend typedRegionBackend = TypedRegionBackend::Auto;
    bool enableTypedRegions = false;
    std::vector<RuntimeWorkspace*> inheritedWorkspaceFrames;
    std::vector<RuntimeSourceCallable> inheritedCallables;
    std::vector<RuntimeSourceCallableScope> inheritedCallableScopes;
    RuntimeSourceCallableInvoker inheritedCallableInvoker;
    RuntimeWorkspace* inheritedCallableWorkspace = nullptr;
};

BuiltinSourceEvaluationResult evaluateRuntimeSource(
    const BuiltinSourceEvaluationRequest& request,
    RuntimeWorkspace& workspace,
    const RuntimeSourceEvaluationOptions& options);

} // namespace mparser
