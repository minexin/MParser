#pragma once

#include "mparser/builtin_registry.h"
#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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
}

inline void requireBuiltinVariableParity(
    const BuiltinConformanceRun& run,
    std::string_view name) {
    const RuntimeValue* hir =
        findVariable(run.hir.variables, name);
    const RuntimeValue* vm =
        findVariable(run.vm.variables, name);
    if (!hir || !vm ||
        runtimeValueToString(*hir) !=
            runtimeValueToString(*vm)) {
        throw std::runtime_error(
            "builtin variable parity failed: " +
            std::string(name));
    }
}

} // namespace mparser::test
