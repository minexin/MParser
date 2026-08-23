#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/session/runtime_call_frame.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/execution/jit/typed_region_executor.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

enum class RuntimeSourceStorageKind {
    Global,
    Persistent,
};

struct RuntimeSourceStorageBinding {
    RuntimeSourceStorageKind kind = RuntimeSourceStorageKind::Global;
    std::optional<RuntimePersistentScope> persistentScope;
};

struct RuntimeSourceStorageDeclarationResult {
    bool succeeded = false;
    std::optional<RuntimeSourceStorageBinding> binding;
    RuntimeValue value;
    std::vector<Diagnostic> diagnostics;
};

using RuntimeSourceStorageResolver = std::function<
    std::optional<RuntimeSourceStorageBinding>(
        RuntimeWorkspace* ownerWorkspace, std::string_view name)>;

using RuntimeSourceStorageDeclarer = std::function<
    RuntimeSourceStorageDeclarationResult(
        RuntimeWorkspace* ownerWorkspace, RuntimeSourceStorageKind kind,
        std::string_view name, const RuntimeValue* localValue,
        SourceSpan span)>;

using RuntimeSourceStorageClearer = std::function<void(
    RuntimeWorkspace* ownerWorkspace, std::string_view name)>;

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
    RuntimeSourceStorageResolver inheritedStorageResolver;
    RuntimeSourceStorageDeclarer inheritedStorageDeclarer;
    RuntimeSourceStorageClearer inheritedStorageClearer;
    RuntimeWorkspace* inheritedStorageWorkspace = nullptr;
};

BuiltinSourceEvaluationResult evaluateRuntimeSource(
    const BuiltinSourceEvaluationRequest& request,
    RuntimeWorkspace& workspace,
    const RuntimeSourceEvaluationOptions& options);

} // namespace mparser
