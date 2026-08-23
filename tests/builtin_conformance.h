#pragma once

#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"

#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mparser::test {

struct BuiltinConformanceRun {
    CompiledModule module;
    InterpreterResult hir;
    BytecodeVmResult vm;
};

inline BuiltinConformanceRun runBuiltinConformance(
    std::string source,
    std::shared_ptr<const BuiltinRegistry> builtinRegistry =
        defaultBuiltinRegistry()) {
    CompiledModuleCompileOptions options;
    options.builtinRegistry = std::move(builtinRegistry);
    CompiledModule module =
        CompiledModule::compile(std::move(source), options);
    if (!module.valid()) {
        throw std::runtime_error(
            module.diagnostics().empty()
                ? "builtin conformance compilation failed"
                : module.diagnostics().front().message);
    }

    Interpreter interpreter;
    InterpreterResult hir = interpreter.run(module.semantic());
    BytecodeVmResult vm = module.invoke();
    return BuiltinConformanceRun{
        std::move(module), std::move(hir), std::move(vm)};
}

inline const RuntimeValue* findVariable(
    const std::vector<RuntimeVariable>& variables,
    std::string_view name) {
    for (const RuntimeVariable& variable : variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

using ComparedRuntimeHandles =
    std::set<std::pair<const void*, const void*>>;

inline bool builtinNumberEqual(double left, double right) {
    return left == right ||
           (std::isnan(left) && std::isnan(right));
}

inline bool builtinRuntimeValueEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    ComparedRuntimeHandles& comparedHandles);

inline bool builtinRuntimeWorkspaceEqual(
    const RuntimeWorkspace& left, const RuntimeWorkspace& right,
    ComparedRuntimeHandles& comparedHandles) {
    if (left.size() != right.size()) {
        return false;
    }
    for (const auto& [name, value] : left) {
        const auto candidate = right.find(name);
        if (candidate == right.end() ||
            !builtinRuntimeValueEqual(
                value, candidate->second, comparedHandles)) {
            return false;
        }
    }
    return true;
}

inline bool builtinRuntimeValuesEqual(
    const std::vector<RuntimeValue>& left,
    const std::vector<RuntimeValue>& right,
    ComparedRuntimeHandles& comparedHandles) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (!builtinRuntimeValueEqual(
                left[index], right[index], comparedHandles)) {
            return false;
        }
    }
    return true;
}

inline bool builtinRuntimeValueEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    ComparedRuntimeHandles& comparedHandles) {
    if (left.kind != right.kind ||
        left.numericClass != right.numericClass ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }

    switch (left.kind) {
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::MissingArray:
        return true;
    case RuntimeValueKind::Number:
        return builtinNumberEqual(left.number, right.number);
    case RuntimeValueKind::CharacterArray:
        return left.characterElements == right.characterElements;
    case RuntimeValueKind::StringArray:
        return left.stringElements == right.stringElements;
    case RuntimeValueKind::Vector:
    case RuntimeValueKind::Matrix:
        if (left.elements.size() != right.elements.size()) {
            return false;
        }
        for (size_t index = 0; index < left.elements.size(); ++index) {
            if (!builtinNumberEqual(
                    left.elements[index], right.elements[index])) {
                return false;
            }
        }
        return true;
    case RuntimeValueKind::Cell:
    case RuntimeValueKind::CommaSeparatedList:
        return builtinRuntimeValuesEqual(
            left.cells, right.cells, comparedHandles);
    case RuntimeValueKind::NameValueArgument:
        return left.text == right.text &&
               builtinRuntimeValuesEqual(
                   left.cells, right.cells, comparedHandles);
    case RuntimeValueKind::Struct:
        if (runtimeStructFieldOrder(left) !=
                runtimeStructFieldOrder(right) ||
            runtimeStructElementCount(left) !=
                runtimeStructElementCount(right)) {
            return false;
        }
        for (size_t offset = 0;
             offset < runtimeStructElementCount(left); ++offset) {
            const auto* leftElement =
                runtimeStructElement(left, offset);
            const auto* rightElement =
                runtimeStructElement(right, offset);
            if (!leftElement || !rightElement ||
                !builtinRuntimeWorkspaceEqual(
                    *leftElement, *rightElement,
                    comparedHandles)) {
                return false;
            }
        }
        return true;
    case RuntimeValueKind::Object: {
        if (left.className != right.className ||
            left.enumerationMemberName !=
                right.enumerationMemberName ||
            left.handleObject != right.handleObject ||
            (left.handleObject &&
             static_cast<bool>(left.sharedFields) !=
                 static_cast<bool>(right.sharedFields)) ||
            !builtinRuntimeValuesEqual(
                left.objectElements, right.objectElements,
                comparedHandles)) {
            return false;
        }
        const RuntimeWorkspace& leftFields =
            left.handleObject && left.sharedFields
                ? *left.sharedFields
                : left.fields;
        const RuntimeWorkspace& rightFields =
            right.handleObject && right.sharedFields
                ? *right.sharedFields
                : right.fields;
        if (left.handleObject && left.sharedFields &&
            right.sharedFields) {
            const auto identity =
                std::pair<const void*, const void*>{
                    left.sharedFields.get(),
                    right.sharedFields.get()};
            if (!comparedHandles.insert(identity).second) {
                return true;
            }
        }
        return builtinRuntimeWorkspaceEqual(
            leftFields, rightFields, comparedHandles);
    }
    case RuntimeValueKind::FunctionHandle: {
        if (!left.functionHandle || !right.functionHandle) {
            return left.functionHandle == right.functionHandle;
        }
        const auto identity = std::pair<const void*, const void*>{
            left.functionHandle.get(), right.functionHandle.get()};
        if (!comparedHandles.insert(identity).second) {
            return true;
        }
        const RuntimeFunctionHandle& leftHandle =
            *left.functionHandle;
        const RuntimeFunctionHandle& rightHandle =
            *right.functionHandle;
        if (leftHandle.kind != rightHandle.kind ||
            leftHandle.display != rightHandle.display ||
            leftHandle.targetName != rightHandle.targetName ||
            leftHandle.className != rightHandle.className ||
            leftHandle.methodName != rightHandle.methodName ||
            leftHandle.declaringClass !=
                rightHandle.declaringClass ||
            leftHandle.lexicalClassName !=
                rightHandle.lexicalClassName ||
            leftHandle.sourceFile != rightHandle.sourceFile ||
            leftHandle.parameters != rightHandle.parameters ||
            leftHandle.receiver.has_value() !=
                rightHandle.receiver.has_value()) {
            return false;
        }
        if (leftHandle.receiver &&
            !builtinRuntimeValueEqual(
                *leftHandle.receiver, *rightHandle.receiver,
                comparedHandles)) {
            return false;
        }
        return builtinRuntimeWorkspaceEqual(
            leftHandle.capturedVariables,
            rightHandle.capturedVariables, comparedHandles);
    }
    }
    return false;
}

inline bool builtinRuntimeValueEqual(
    const RuntimeValue& left, const RuntimeValue& right) {
    ComparedRuntimeHandles comparedHandles;
    return builtinRuntimeValueEqual(
        left, right, comparedHandles);
}

inline bool builtinDiagnosticCauseEqual(
    const DiagnosticCause& left, const DiagnosticCause& right) {
    if (left.identifier != right.identifier ||
        left.message != right.message ||
        left.stack.size() != right.stack.size() ||
        left.causes.size() != right.causes.size()) {
        return false;
    }
    for (size_t index = 0; index < left.stack.size(); ++index) {
        if (left.stack[index].file != right.stack[index].file ||
            left.stack[index].name != right.stack[index].name ||
            left.stack[index].line != right.stack[index].line) {
            return false;
        }
    }
    for (size_t index = 0; index < left.causes.size(); ++index) {
        if (!builtinDiagnosticCauseEqual(
                left.causes[index], right.causes[index])) {
            return false;
        }
    }
    return true;
}

inline bool builtinDiagnosticsEqual(
    const std::vector<Diagnostic>& left,
    const std::vector<Diagnostic>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const Diagnostic& first = left[index];
        const Diagnostic& second = right[index];
        if (first.identifier != second.identifier ||
            first.message != second.message ||
            first.severity != second.severity ||
            first.span.begin.offset !=
                second.span.begin.offset ||
            first.span.begin.line != second.span.begin.line ||
            first.span.begin.column !=
                second.span.begin.column ||
            first.span.begin.sourceId !=
                second.span.begin.sourceId ||
            first.span.end.offset != second.span.end.offset ||
            first.span.end.line != second.span.end.line ||
            first.span.end.column != second.span.end.column ||
            first.span.end.sourceId != second.span.end.sourceId ||
            first.stack.size() != second.stack.size() ||
            first.causes.size() != second.causes.size()) {
            return false;
        }
        for (size_t frame = 0; frame < first.stack.size();
             ++frame) {
            if (first.stack[frame].file !=
                    second.stack[frame].file ||
                first.stack[frame].name !=
                    second.stack[frame].name ||
                first.stack[frame].line !=
                    second.stack[frame].line) {
                return false;
            }
        }
        for (size_t cause = 0; cause < first.causes.size();
             ++cause) {
            if (!builtinDiagnosticCauseEqual(
                    first.causes[cause],
                    second.causes[cause])) {
                return false;
            }
        }
    }
    return true;
}

inline void requireNoBuiltinErrors(
    const BuiltinConformanceRun& run) {
    if (hasErrorDiagnostics(run.hir.diagnostics)) {
        throw std::runtime_error(
            "HIR builtin conformance run reported an error: " +
            run.hir.diagnostics.front().message);
    }
    if (hasErrorDiagnostics(run.vm.diagnostics)) {
        throw std::runtime_error(
            "VM builtin conformance run reported an error: " +
            run.vm.diagnostics.front().message);
    }
    if (!builtinDiagnosticsEqual(
            run.hir.diagnostics, run.vm.diagnostics)) {
        throw std::runtime_error(
            "HIR and VM builtin diagnostics differ");
    }
}

inline void requireBuiltinVariableParity(
    const BuiltinConformanceRun& run,
    std::string_view name) {
    const RuntimeValue* hir =
        findVariable(run.hir.variables, name);
    const RuntimeValue* vm =
        findVariable(run.vm.variables, name);
    if (!hir || !vm ||
        !builtinRuntimeValueEqual(*hir, *vm)) {
        throw std::runtime_error(
            "builtin variable parity failed: " +
            std::string(name));
    }
}

} // namespace mparser::test
