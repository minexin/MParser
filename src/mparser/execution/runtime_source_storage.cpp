#include "mparser/execution/runtime_source_storage.h"

#include <string>

namespace mparser {

std::optional<RuntimeSourceStorageBinding> runtimeSourceStorageBinding(
    const RuntimeCallFrame& frame, std::string_view name) {
    const std::string variable(name);
    if (frame.globalBindings.contains(variable)) {
        return RuntimeSourceStorageBinding{
            RuntimeSourceStorageKind::Global, std::nullopt};
    }
    if (frame.persistentBindings.contains(variable) &&
        frame.persistentScope) {
        return RuntimeSourceStorageBinding{
            RuntimeSourceStorageKind::Persistent,
            frame.persistentScope};
    }
    return std::nullopt;
}

RuntimeSourceStorageDeclarationResult runtimeDeclareSourceStorage(
    RuntimeCallFrame& frame, RuntimeSessionState& session,
    RuntimeSourceStorageKind kind, std::string_view name,
    const RuntimeValue* localValue, SourceSpan span) {
    RuntimeSourceStorageDeclarationResult result;
    const std::string variable(name);
    const auto existing = runtimeSourceStorageBinding(frame, name);
    if (existing && existing->kind != kind) {
        result.diagnostics.push_back(Diagnostic{
            span,
            "variable cannot be both global and persistent: " + variable,
            "MParser:ConflictingWorkspaceDeclaration"});
        return result;
    }
    if (existing) {
        result.succeeded = true;
        result.binding = existing;
        if (kind == RuntimeSourceStorageKind::Global) {
            result.value = session.declareGlobal(name);
        } else {
            const auto& scope = *existing->persistentScope;
            result.value = session.declarePersistent(
                scope.contextIdentity, scope.function, name);
        }
        frame.workspace[variable] = result.value;
        return result;
    }

    if (kind == RuntimeSourceStorageKind::Global) {
        const auto stored = session.findGlobal(name);
        if (localValue) {
            if (!stored) {
                session.storeGlobal(variable, *localValue);
            }
            result.diagnostics.push_back(Diagnostic{
                span,
                "global declaration follows use of local variable: " +
                    variable,
                "MParser:GlobalDeclarationAfterUse",
                DiagnosticSeverity::Warning});
        }
        result.value = session.declareGlobal(name);
        result.binding = RuntimeSourceStorageBinding{
            RuntimeSourceStorageKind::Global, std::nullopt};
        result.succeeded = true;
        frame.globalBindings.insert(variable);
        frame.workspace[variable] = result.value;
        return result;
    }

    if (frame.kind == RuntimeCallFrameKind::Script) {
        if (localValue) {
            result.diagnostics.push_back(Diagnostic{
                span,
                "persistent variable already exists in workspace: " +
                    variable,
                "MParser:PersistentVariableAlreadyInWorkspace"});
            return result;
        }
        result.value = makeRuntimeMatrixValue(0, 0, {});
        result.succeeded = true;
        frame.workspace[variable] = result.value;
        return result;
    }
    if (!frame.persistentScope) {
        result.diagnostics.push_back(Diagnostic{
            span,
            "persistent declaration is only valid in a function: " +
                variable,
            "MParser:PersistentNotInFunction"});
        return result;
    }
    if (!frame.dynamicPersistentDeclarationsAllowed) {
        result.diagnostics.push_back(Diagnostic{
            span,
            "persistent declaration cannot modify a static nested "
            "workspace: " + variable,
            "MParser:StaticWorkspaceViolation"});
        return result;
    }
    if (localValue) {
        result.diagnostics.push_back(Diagnostic{
            span,
            "persistent variable already exists in workspace: " +
                variable,
            "MParser:PersistentVariableAlreadyInWorkspace"});
        return result;
    }

    result.binding = RuntimeSourceStorageBinding{
        RuntimeSourceStorageKind::Persistent, frame.persistentScope};
    result.value = session.declarePersistent(
        frame.persistentScope->contextIdentity,
        frame.persistentScope->function, name);
    result.succeeded = true;
    frame.persistentBindings.insert(variable);
    frame.workspace[variable] = result.value;
    return result;
}

void runtimeClearSourceStorage(RuntimeCallFrame& frame,
                               std::string_view name) {
    if (name.empty()) {
        frame.globalBindings.clear();
        frame.persistentBindings.clear();
        return;
    }
    const std::string variable(name);
    frame.globalBindings.erase(variable);
    frame.persistentBindings.erase(variable);
}

} // namespace mparser
