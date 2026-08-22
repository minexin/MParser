#pragma once

#include "mparser/builtin_registry.h"
#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_session_state.h"
#include "mparser/typed_region_executor.h"

#include <memory>

namespace mparser {

struct RuntimeSourceEvaluationOptions {
    std::shared_ptr<const BuiltinRegistry> builtinRegistry;
    std::shared_ptr<RuntimeSessionState> sessionState;
    std::shared_ptr<RuntimeExecutionControl> executionControl;
    RuntimeOutputSink outputSink;
    TypedRegionBackend typedRegionBackend = TypedRegionBackend::Auto;
    bool enableTypedRegions = false;
    std::vector<RuntimeWorkspace*> inheritedWorkspaceFrames;
};

BuiltinSourceEvaluationResult evaluateRuntimeSource(
    const BuiltinSourceEvaluationRequest& request,
    RuntimeWorkspace& workspace,
    const RuntimeSourceEvaluationOptions& options);

} // namespace mparser
