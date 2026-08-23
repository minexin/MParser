#include "mparser/execution/runtime_source_evaluation.h"

#include "mparser/embedding/compiled_module.h"
#include "mparser/frontend/lexer.h"
#include "mparser/runtime/core/session/runtime_output.h"
#include "mparser/runtime/io/runtime_system.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace mparser {

class RuntimeSourceEvaluationAccess {
public:
    static BytecodeVmResult invoke(
        const CompiledModule& module,
        const BytecodeVmOptions& options) {
        return module.invokeInternal(options, true);
    }
};

namespace {

constexpr size_t kMaximumDynamicSourceBytes = 16U * 1024U * 1024U;
constexpr size_t kMaximumDynamicOutputCount = 1024U;

Diagnostic dynamicDiagnostic(SourceSpan span, std::string message,
                             std::string identifier) {
    return Diagnostic{span, std::move(message), std::move(identifier)};
}

Diagnostic projectDiagnostic(Diagnostic diagnostic, SourceSpan callSpan,
                             size_t firstLineColumnOffset) {
    const int line = diagnostic.span.begin.line;
    int column = diagnostic.span.begin.column;
    if (line == 1 && firstLineColumnOffset != 0 && column > 0) {
        const size_t sourceColumn = static_cast<size_t>(column);
        column = sourceColumn > firstLineColumnOffset
                     ? static_cast<int>(sourceColumn -
                                        firstLineColumnOffset)
                     : 1;
    }
    std::ostringstream message;
    message << "dynamic source line " << line << ", column " << column
            << ": " << diagnostic.message;
    diagnostic.span = callSpan;
    diagnostic.message = message.str();
    if (diagnostic.identifier.empty()) {
        diagnostic.identifier = "MParser:DynamicEvaluationFailed";
    }
    diagnostic.stack.clear();
    return diagnostic;
}

std::vector<RuntimeVariable> workspaceVariables(
    const RuntimeWorkspace& workspace) {
    std::vector<RuntimeVariable> variables;
    variables.reserve(workspace.size());
    for (const auto& [name, value] : workspace) {
        variables.push_back(RuntimeVariable{name, value});
    }
    return variables;
}

RuntimeWorkspace workspaceMap(
    std::vector<RuntimeVariable> variables) {
    RuntimeWorkspace workspace;
    for (auto& variable : variables) {
        workspace[std::move(variable.name)] = std::move(variable.value);
    }
    return workspace;
}

struct PreparedSource {
    std::string source;
    std::vector<std::string> outputNames;
    size_t firstLineColumnOffset = 0;
};

PreparedSource prepareSource(const BuiltinSourceEvaluationRequest& request,
                             const RuntimeWorkspace& workspace) {
    PreparedSource prepared;
    if (request.requestedOutputCount == 0) {
        prepared.source = request.source;
        return prepared;
    }

    std::unordered_set<std::string> sourceIdentifiers;
    for (const Token& token : Lexer(request.source).lex()) {
        if (token.kind == TokenKind::Identifier) {
            sourceIdentifiers.insert(token.text);
        }
    }

    size_t generation = 0;
    for (;;) {
        prepared.outputNames.clear();
        bool collision = false;
        for (size_t index = 0; index < request.requestedOutputCount;
             ++index) {
            std::string name = "__mparser_eval_output_" +
                               std::to_string(generation) + "_" +
                               std::to_string(index);
            collision = collision || workspace.contains(name) ||
                        sourceIdentifiers.contains(name);
            prepared.outputNames.push_back(std::move(name));
        }
        if (!collision) {
            break;
        }
        ++generation;
    }

    prepared.source.push_back('[');
    for (size_t index = 0; index < prepared.outputNames.size(); ++index) {
        if (index != 0) {
            prepared.source.push_back(',');
        }
        prepared.source += prepared.outputNames[index];
    }
    prepared.source += "] = ";
    prepared.firstLineColumnOffset = prepared.source.size();
    prepared.source += request.source;
    prepared.source.push_back('\n');
    return prepared;
}

bool hasIllegalDynamicDefinition(const CompiledModule& module) {
    if (!module.functions().empty()) {
        return true;
    }
    for (const auto& info : module.sourceInfo()) {
        if (info.kind == CompiledSourceKind::Function ||
            info.kind == CompiledSourceKind::Class) {
            return true;
        }
    }
    return false;
}

struct HandleTraversal {
    std::unordered_set<const RuntimeFunctionHandle*> handles;
    std::unordered_set<const RuntimeWorkspace*> sharedFields;
};

struct SharedFieldSnapshot {
    std::shared_ptr<RuntimeWorkspace> fields;
    RuntimeWorkspace values;
};

struct BorrowedWorkspaceSnapshot {
    RuntimeWorkspace* workspace = nullptr;
    RuntimeWorkspace values;
};

struct RuntimeSessionSnapshot {
    std::vector<RuntimeVariable> globals;
    std::vector<RuntimePersistentVariable> persistentVariables;
};

void collectSharedFieldSnapshots(
    const RuntimeValue& value,
    std::vector<SharedFieldSnapshot>& snapshots,
    HandleTraversal& traversal) {
    if (value.functionHandle &&
        traversal.handles.insert(value.functionHandle.get()).second) {
        if (value.functionHandle->receiver) {
            collectSharedFieldSnapshots(
                *value.functionHandle->receiver, snapshots, traversal);
        }
        for (const auto& [name, captured] :
             value.functionHandle->capturedVariables) {
            (void)name;
            collectSharedFieldSnapshots(captured, snapshots, traversal);
        }
    }
    for (const auto& cell : value.cells) {
        collectSharedFieldSnapshots(cell, snapshots, traversal);
    }
    for (const auto& [name, field] : value.fields) {
        (void)name;
        collectSharedFieldSnapshots(field, snapshots, traversal);
    }
    for (const auto& element : value.structElements) {
        for (const auto& [name, field] : element) {
            (void)name;
            collectSharedFieldSnapshots(field, snapshots, traversal);
        }
    }
    for (const auto& element : value.objectElements) {
        collectSharedFieldSnapshots(element, snapshots, traversal);
    }
    if (value.sharedFields &&
        traversal.sharedFields.insert(value.sharedFields.get()).second) {
        snapshots.push_back(
            SharedFieldSnapshot{value.sharedFields, *value.sharedFields});
        for (const auto& [name, field] : *value.sharedFields) {
            (void)name;
            collectSharedFieldSnapshots(field, snapshots, traversal);
        }
    }
}

void collectCallableContexts(
    const RuntimeValue& value,
    std::set<const RuntimeCallableContext*>& contexts,
    HandleTraversal& traversal) {
    if (value.functionHandle &&
        traversal.handles.insert(value.functionHandle.get()).second) {
        if (value.functionHandle->context) {
            contexts.insert(value.functionHandle->context.get());
        }
        if (value.functionHandle->receiver) {
            collectCallableContexts(*value.functionHandle->receiver,
                                    contexts, traversal);
        }
        for (const auto& [name, captured] :
             value.functionHandle->capturedVariables) {
            (void)name;
            collectCallableContexts(captured, contexts, traversal);
        }
    }
    for (const auto& cell : value.cells) {
        collectCallableContexts(cell, contexts, traversal);
    }
    for (const auto& [name, field] : value.fields) {
        (void)name;
        collectCallableContexts(field, contexts, traversal);
    }
    for (const auto& element : value.structElements) {
        for (const auto& [name, field] : element) {
            (void)name;
            collectCallableContexts(field, contexts, traversal);
        }
    }
    for (const auto& element : value.objectElements) {
        collectCallableContexts(element, contexts, traversal);
    }
    if (value.sharedFields &&
        traversal.sharedFields.insert(value.sharedFields.get()).second) {
        for (const auto& [name, field] : *value.sharedFields) {
            (void)name;
            collectCallableContexts(field, contexts, traversal);
        }
    }
}

bool containsUnsafeHandle(
    const RuntimeValue& value,
    const std::set<const RuntimeCallableContext*>& allowedContexts,
    HandleTraversal& traversal) {
    if (value.functionHandle &&
        traversal.handles.insert(value.functionHandle.get()).second) {
        const auto& handle = *value.functionHandle;
        if (handle.backend != RuntimeFunctionHandleBackend::Independent &&
            (!handle.context ||
             !allowedContexts.contains(handle.context.get()))) {
            return true;
        }
        if (handle.receiver &&
            containsUnsafeHandle(*handle.receiver, allowedContexts,
                                 traversal)) {
            return true;
        }
        for (const auto& [name, captured] : handle.capturedVariables) {
            (void)name;
            if (containsUnsafeHandle(captured, allowedContexts,
                                     traversal)) {
                return true;
            }
        }
    }
    for (const auto& cell : value.cells) {
        if (containsUnsafeHandle(cell, allowedContexts, traversal)) {
            return true;
        }
    }
    for (const auto& [name, field] : value.fields) {
        (void)name;
        if (containsUnsafeHandle(field, allowedContexts, traversal)) {
            return true;
        }
    }
    for (const auto& element : value.structElements) {
        for (const auto& [name, field] : element) {
            (void)name;
            if (containsUnsafeHandle(field, allowedContexts, traversal)) {
                return true;
            }
        }
    }
    for (const auto& element : value.objectElements) {
        if (containsUnsafeHandle(element, allowedContexts, traversal)) {
            return true;
        }
    }
    if (value.sharedFields &&
        traversal.sharedFields.insert(value.sharedFields.get()).second) {
        for (const auto& [name, field] : *value.sharedFields) {
            (void)name;
            if (containsUnsafeHandle(field, allowedContexts, traversal)) {
                return true;
            }
        }
    }
    return false;
}

bool valueContainsUnsafeHandle(
    const RuntimeValue& value,
    const std::set<const RuntimeCallableContext*>& allowedContexts) {
    HandleTraversal traversal;
    return containsUnsafeHandle(value, allowedContexts, traversal);
}

bool restoreUnsafeSessionValues(
    RuntimeSessionState& session,
    const RuntimeSessionSnapshot& original,
    const std::set<const RuntimeCallableContext*>& allowedContexts) {
    bool restored = false;
    for (const auto& current : session.globals()) {
        if (!valueContainsUnsafeHandle(current.value,
                                       allowedContexts)) {
            continue;
        }
        restored = true;
        const auto previous = std::find_if(
            original.globals.begin(), original.globals.end(),
            [&current](const RuntimeVariable& candidate) {
                return candidate.name == current.name;
            });
        if (previous == original.globals.end()) {
            session.clearGlobal(current.name);
        } else {
            session.storeGlobal(previous->name, previous->value);
        }
    }
    for (const auto& current : session.persistentVariables()) {
        if (!valueContainsUnsafeHandle(current.value,
                                       allowedContexts)) {
            continue;
        }
        restored = true;
        const auto previous = std::find_if(
            original.persistentVariables.begin(),
            original.persistentVariables.end(),
            [&current](const RuntimePersistentVariable& candidate) {
                return candidate.contextIdentity ==
                           current.contextIdentity &&
                       candidate.function == current.function &&
                       candidate.name == current.name;
            });
        if (previous == original.persistentVariables.end()) {
            session.clearPersistent(
                current.contextIdentity, current.function,
                current.name);
        } else {
            session.storePersistent(
                previous->contextIdentity, previous->function,
                previous->name, previous->value);
        }
    }
    return restored;
}

bool workspaceContainsUnsafeHandle(
    const RuntimeWorkspace& workspace,
    const std::set<const RuntimeCallableContext*>& allowedContexts) {
    return std::any_of(
        workspace.begin(), workspace.end(),
        [&allowedContexts](const auto& entry) {
            return valueContainsUnsafeHandle(
                entry.second, allowedContexts);
        });
}

bool restoreUnsafeWorkspaceValues(
    RuntimeWorkspace& workspace,
    const RuntimeWorkspace& original,
    const std::set<const RuntimeCallableContext*>& allowedContexts) {
    bool restored = false;
    for (auto iterator = workspace.begin();
         iterator != workspace.end();) {
        if (!valueContainsUnsafeHandle(iterator->second,
                                       allowedContexts)) {
            ++iterator;
            continue;
        }
        restored = true;
        const auto previous = original.find(iterator->first);
        if (previous == original.end()) {
            iterator = workspace.erase(iterator);
        } else {
            iterator->second = previous->second;
            ++iterator;
        }
    }
    return restored;
}

} // namespace

BuiltinSourceEvaluationResult evaluateRuntimeSource(
    const BuiltinSourceEvaluationRequest& request,
    RuntimeWorkspace& workspace,
    const RuntimeSourceEvaluationOptions& options) {
    BuiltinSourceEvaluationResult result;
    if (request.source.size() > kMaximumDynamicSourceBytes) {
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span, "dynamic source exceeds the 16 MiB limit",
            "MParser:DynamicSourceLimitExceeded"));
        return result;
    }
    if (request.requestedOutputCount > kMaximumDynamicOutputCount) {
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span,
            "dynamic source requested more than 1024 outputs",
            "MParser:DynamicOutputLimitExceeded"));
        return result;
    }
    const auto systemContext = options.sessionState
                                   ? options.sessionState->systemContext()
                                   : nullptr;
    if (!systemContext ||
        !systemContext->hasCapability(
            RuntimeSystemCapability::DynamicEvaluation)) {
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span,
            "dynamic source evaluation capability is disabled",
            "MParser:SystemCapabilityDenied"));
        return result;
    }

    const RuntimeWorkspace originalWorkspace = workspace;
    RuntimeSessionSnapshot originalSession;
    if (options.sessionState) {
        originalSession.globals = options.sessionState->globals();
        originalSession.persistentVariables =
            options.sessionState->persistentVariables();
    }
    std::vector<BorrowedWorkspaceSnapshot> borrowedWorkspaces;
    std::unordered_set<const RuntimeWorkspace*> seenWorkspaces{&workspace};
    for (RuntimeWorkspace* inherited :
         options.inheritedWorkspaceFrames) {
        if (inherited && seenWorkspaces.insert(inherited).second) {
            borrowedWorkspaces.push_back(
                BorrowedWorkspaceSnapshot{inherited, *inherited});
        }
    }
    std::set<const RuntimeCallableContext*> allowedContexts;
    HandleTraversal contextTraversal;
    for (const auto& [name, value] : originalWorkspace) {
        (void)name;
        collectCallableContexts(value, allowedContexts, contextTraversal);
    }
    for (const auto& inherited : borrowedWorkspaces) {
        for (const auto& [name, value] : inherited.values) {
            (void)name;
            collectCallableContexts(
                value, allowedContexts, contextTraversal);
        }
    }
    for (const auto& inherited : options.inheritedCallables) {
        collectCallableContexts(
            inherited.callable, allowedContexts, contextTraversal);
    }
    for (const auto& variable : originalSession.globals) {
        collectCallableContexts(
            variable.value, allowedContexts, contextTraversal);
    }
    for (const auto& variable :
         originalSession.persistentVariables) {
        collectCallableContexts(
            variable.value, allowedContexts, contextTraversal);
    }
    std::vector<SharedFieldSnapshot> sharedFieldSnapshots;
    HandleTraversal snapshotTraversal;
    for (const auto& [name, value] : originalWorkspace) {
        (void)name;
        collectSharedFieldSnapshots(
            value, sharedFieldSnapshots, snapshotTraversal);
    }
    for (const auto& inherited : borrowedWorkspaces) {
        for (const auto& [name, value] : inherited.values) {
            (void)name;
            collectSharedFieldSnapshots(
                value, sharedFieldSnapshots, snapshotTraversal);
        }
    }
    for (const auto& variable : originalSession.globals) {
        collectSharedFieldSnapshots(
            variable.value, sharedFieldSnapshots, snapshotTraversal);
    }
    for (const auto& variable :
         originalSession.persistentVariables) {
        collectSharedFieldSnapshots(
            variable.value, sharedFieldSnapshots, snapshotTraversal);
    }

    const PreparedSource prepared = prepareSource(request, workspace);
    CompiledModuleCompileOptions compileOptions;
    compileOptions.builtinRegistry =
        options.builtinRegistry ? options.builtinRegistry
                                : defaultBuiltinRegistry();
    compileOptions.externalFunctionNames.reserve(
        options.inheritedCallables.size());
    compileOptions.allowTopLevelPersistentDeclarations = true;
    for (const auto& inherited : options.inheritedCallables) {
        compileOptions.externalFunctionNames.push_back(inherited.name);
    }
    CompiledModule module = CompiledModule::compile(
        std::vector<SourceUnit>{
            SourceUnit{"<eval>", prepared.source}},
        compileOptions);
    if (!module.valid()) {
        for (const auto& diagnostic : module.diagnostics()) {
            result.diagnostics.push_back(
                projectDiagnostic(diagnostic, request.span,
                                  prepared.firstLineColumnOffset));
        }
        return result;
    }
    if (hasIllegalDynamicDefinition(module)) {
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span,
            "dynamic source cannot define functions or classes",
            "MParser:UnsupportedDynamicDeclaration"));
        return result;
    }

    BytecodeVmOptions vmOptions;
    vmOptions.profiling = BytecodeVmProfilingMode::Disabled;
    vmOptions.sessionState = options.sessionState;
    vmOptions.initialWorkspace = workspaceVariables(workspace);
    vmOptions.typedRegionBackend = options.typedRegionBackend;
    vmOptions.executionControl = options.executionControl;
    vmOptions.inheritedWorkspaceFrames =
        options.inheritedWorkspaceFrames;
    vmOptions.inheritedCallables = options.inheritedCallables;
    vmOptions.inheritedCallableScopes =
        options.inheritedCallableScopes;
    vmOptions.inheritedCallableInvoker =
        options.inheritedCallableInvoker;
    vmOptions.inheritedCallableWorkspace =
        options.inheritedCallableWorkspace
            ? options.inheritedCallableWorkspace
            : &workspace;
    vmOptions.inheritedStorageResolver =
        options.inheritedStorageResolver;
    vmOptions.inheritedStorageDeclarer =
        options.inheritedStorageDeclarer;
    vmOptions.inheritedStorageClearer =
        options.inheritedStorageClearer;
    vmOptions.inheritedStorageWorkspace =
        options.inheritedStorageWorkspace
            ? options.inheritedStorageWorkspace
            : &workspace;
    BytecodeVmResult runtime =
        options.enableTypedRegions
            ? RuntimeSourceEvaluationAccess::invoke(module, vmOptions)
            : module.invoke(vmOptions);

    RuntimeWorkspace updatedWorkspace =
        workspaceMap(std::move(runtime.variables));
    for (const std::string& name : prepared.outputNames) {
        const auto output = updatedWorkspace.find(name);
        if (output != updatedWorkspace.end()) {
            result.outputs.push_back(output->second);
            updatedWorkspace.erase(output);
        }
    }

    bool unsafeHandle = restoreUnsafeWorkspaceValues(
        updatedWorkspace, originalWorkspace, allowedContexts);
    for (const auto& inherited : borrowedWorkspaces) {
        unsafeHandle =
            restoreUnsafeWorkspaceValues(
                *inherited.workspace, inherited.values,
                allowedContexts) ||
            unsafeHandle;
    }
    if (options.sessionState) {
        unsafeHandle = restoreUnsafeSessionValues(
                           *options.sessionState, originalSession,
                           allowedContexts) ||
                       unsafeHandle;
    }
    for (const auto& output : result.outputs) {
        unsafeHandle = unsafeHandle ||
            valueContainsUnsafeHandle(output, allowedContexts);
    }
    for (const auto& snapshot : sharedFieldSnapshots) {
        unsafeHandle = unsafeHandle ||
            workspaceContainsUnsafeHandle(
                *snapshot.fields, allowedContexts);
    }
    if (unsafeHandle) {
        for (const auto& snapshot : sharedFieldSnapshots) {
            *snapshot.fields = snapshot.values;
        }
        result.outputs.clear();
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span,
            "a function handle created by dynamic source cannot escape its "
            "temporary compiled module",
            "MParser:DynamicEvaluationHandleEscape"));
    }

    workspace = std::move(updatedWorkspace);
    result.capturedOutput = runtimeRenderConsole(
        runtime.outputEvents, runtime.expressionResults);
    for (auto& diagnostic : runtime.diagnostics) {
        result.diagnostics.push_back(
            projectDiagnostic(std::move(diagnostic), request.span,
                              prepared.firstLineColumnOffset));
    }

    if (!request.captureOutput && !result.capturedOutput.empty()) {
        if (!options.outputSink) {
            result.diagnostics.push_back(dynamicDiagnostic(
                request.span,
                "dynamic source produced output without an output sink",
                "MParser:MissingBuiltinContext"));
        } else if (!options.outputSink(RuntimeOutputEvent{
                       RuntimeOutputKind::StandardOutput,
                       result.capturedOutput, request.span})) {
            result.diagnostics.push_back(dynamicDiagnostic(
                request.span,
                "host output sink rejected dynamic source output",
                "MParser:OutputSinkRejected"));
        }
    }

    result.succeeded = !hasErrorDiagnostics(result.diagnostics);
    if (!result.succeeded) {
        result.outputs.clear();
    } else if (result.outputs.size() != request.requestedOutputCount) {
        result.diagnostics.push_back(dynamicDiagnostic(
            request.span,
            "dynamic expression did not produce the requested outputs",
            "MParser:DynamicOutputCountMismatch"));
        result.outputs.clear();
        result.succeeded = false;
    }
    return result;
}

} // namespace mparser
