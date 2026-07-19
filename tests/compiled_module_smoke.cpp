#include "mparser/compiled_module.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

void assertNumber(const mparser::RuntimeValue& value, double expected) {
    assert(value.kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value.number - expected) < 1e-9);
}

const std::string kModuleSource = R"(function y = compiled_module_demo()
y = 999;
end

function [s, p] = kernel(x, n)
s = 0;
for i = 1:n
    s = s + x * i;
end
p = x * n;
end
)";

void runCatalogSmoke() {
    auto module = mparser::CompiledModule::compile(kModuleSource);
    assert(module.valid());
    assert(module.diagnostics().empty());
    assert(module.source() == kModuleSource);
    assert(module.semantic().root != nullptr);
    assert(!module.bytecode().instructions.empty());
    assert(module.functions().size() == 2);

    const auto* kernel = module.findFunction("kernel");
    assert(kernel != nullptr);
    assert(kernel->signature.parameters.size() == 2);
    assert(kernel->signature.parameters[0] == "x");
    assert(kernel->signature.parameters[1] == "n");
    assert(kernel->signature.outputs.size() == 2);
    assert(kernel->signature.outputs[0] == "s");
    assert(kernel->signature.outputs[1] == "p");
    assert(module.findFunction("missing") == nullptr);
}

void runRepeatedInvocationSmoke() {
    auto module = mparser::CompiledModule::compile(kModuleSource);

    mparser::BytecodeVmOptions firstOptions;
    firstOptions.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    firstOptions.entryFunction = "kernel";
    firstOptions.arguments = {number(2), number(4)};
    const auto first = module.invoke(firstOptions);
    assert(first.diagnostics.empty());
    assert(first.entryFunction == "kernel");
    assert(first.outputNames.size() == 2);
    assert(first.outputs.size() == 2);
    assertNumber(first.outputs[0], 20);
    assertNumber(first.outputs[1], 8);

    mparser::BytecodeVmOptions secondOptions = firstOptions;
    secondOptions.arguments = {number(3), number(5)};
    const auto second = module.invoke(secondOptions);
    assert(second.diagnostics.empty());
    assertNumber(second.outputs[0], 45);
    assertNumber(second.outputs[1], 15);

    firstOptions.entryFunction = "missing";
    const auto missing = module.invoke(firstOptions);
    assert(missing.diagnostics.size() == 1);
    assert(missing.diagnostics[0].message ==
           "entry function is not available: missing");

    secondOptions.arguments.pop_back();
    const auto wrongArity = module.invoke(secondOptions);
    assert(wrongArity.diagnostics.size() == 1);
    assert(wrongArity.diagnostics[0].message ==
           "function invocation failed for kernel: function argument count "
           "mismatch");
}

void runAdaptiveFactorySmoke() {
    auto module = mparser::CompiledModule::compile(kModuleSource);
    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 10;
    options.entryFunction = "kernel";
    options.arguments = {number(3), number(6)};
    auto session = module.createAdaptiveSession(options);

    const auto cold = session.run();
    const auto promoted = session.run();
    const auto typed = session.run();
    assert(cold.tier == mparser::AdaptiveBytecodeTier::Profiling);
    assert(promoted.promotionOccurred);
    assert(session.hasTypedModule());
    assert(typed.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(typed.runtime.diagnostics.empty());
    assertNumber(typed.runtime.outputs[0], 63);
    assertNumber(typed.runtime.outputs[1], 18);

    options.entryFunction = "missing";
    bool rejected = false;
    try {
        auto invalidSession = module.createAdaptiveSession(options);
        (void)invalidSession;
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()) ==
                   "entry function is not available: missing";
    }
    assert(rejected);
}

void runInvalidCompilationSmoke() {
    auto module = mparser::CompiledModule::compile(
        "function y = broken(\ny = 1;\n");
    assert(!module.valid());
    assert(!module.diagnostics().empty());

    const auto runtime = module.invoke();
    assert(runtime.diagnostics.size() == module.diagnostics().size());
}

void runClassCatalogBoundarySmoke() {
    auto module = mparser::CompiledModule::compile(R"(classdef Sample
methods
function y = method(obj)
y = 1;
end
end
end
)");
    assert(module.valid());
    assert(module.functions().empty());
    assert(module.findFunction("method") == nullptr);
}

} // namespace

int main() {
    runCatalogSmoke();
    runRepeatedInvocationSmoke();
    runAdaptiveFactorySmoke();
    runInvalidCompilationSmoke();
    runClassCatalogBoundarySmoke();
    std::cout << "compiled module smoke tests passed\n";
    return 0;
}
