#include "mparser/embedding/compiled_module.h"

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

void runDefinitionDirectorySmoke() {
    const std::string source =
        "% UTF-8: \xc3\xa9\r\n"
        "function out=outer(x)\r\n"
        "out=inner(x);\r\n"
        "function y=inner(x)\r\n"
        "y=x+1;\r\nend\r\nend\r\n"
        "function y=local(x)\r\ny=x;\r\nend\r\n";
    auto module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{{"ranges.m", source}});
    assert(module.valid());
    const auto& definitions = module.definitions();
    assert(definitions.size() == 3);
    assert(definitions[0].name == "outer");
    assert(definitions[0].kind == mparser::CompiledDefinitionKind::PrimaryFunction);
    assert(!definitions[0].parentFunction);
    assert(definitions[0].span.begin.sourceId == 0);
    assert(definitions[0].span.begin.line == 2);
    assert(definitions[0].span.begin.offset == source.find("function out"));
    assert(definitions[1].name == "inner");
    assert(definitions[1].kind == mparser::CompiledDefinitionKind::NestedFunction);
    assert(definitions[1].parentFunction == 0);
    assert(definitions[1].span.begin.line == 4);
    assert(definitions[1].span.begin.offset == source.find("function y=inner"));
    assert(definitions[1].span.end.offset <= definitions[0].span.end.offset);
    assert(definitions[2].name == "local");
    assert(definitions[2].kind == mparser::CompiledDefinitionKind::LocalFunction);
    assert(!definitions[2].parentFunction);
    assert(definitions[2].span.begin.line == 8);
    assert(definitions[2].span.end.offset <= source.size());

    const std::string classSource =
        "classdef RangeClass\nmethods\n"
        "function y=first(obj)\ny=1;\nend\n"
        "function y=second(obj)\ny=2;\nend\nend\nend\n";
    const auto graph = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"ranges.m", source}, {"RangeClass.m", classSource}});
    assert(graph.valid());
    const auto& graphDefinitions = graph.definitions();
    assert(graphDefinitions.size() == 5);
    for (size_t index = 3; index < 5; ++index) {
        const auto& method = graphDefinitions[index];
        assert(method.kind == mparser::CompiledDefinitionKind::Method);
        assert(method.className == "RangeClass");
        assert(!method.parentFunction && !method.declarationOnly);
        assert(method.span.begin.sourceId == 1);
        assert(graph.sourceName(method.span) == "RangeClass.m");
        assert(method.span.end.offset <= classSource.size());
    }
    assert(graphDefinitions[3].name == "first");
    assert(graphDefinitions[4].name == "second");

    const std::string declarationSource =
        "classdef ExternalRange\nmethods\ny=read(obj)\nend\nend\n";
    const std::string methodSource =
        "function y=read(obj)\ny=helper();\n"
        "function z=helper()\nz=1;\nend\nend\n";
    const auto external = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"ExternalRange.m", declarationSource},
            {"read.m", methodSource, "", "", "ExternalRange"}});
    assert(external.valid());
    const auto& externalDefinitions = external.definitions();
    assert(externalDefinitions.size() == 3);
    assert(externalDefinitions[0].name == "read");
    assert(externalDefinitions[0].declarationOnly);
    assert(externalDefinitions[0].span.begin.sourceId == 0);
    assert(externalDefinitions[1].name == "read");
    assert(!externalDefinitions[1].declarationOnly);
    assert(externalDefinitions[1].kind == mparser::CompiledDefinitionKind::Method);
    assert(externalDefinitions[1].className == "ExternalRange");
    assert(externalDefinitions[1].span.begin.sourceId == 1);
    assert(externalDefinitions[1].span.begin.offset == 0);
    assert(externalDefinitions[2].parentFunction == 1);
    assert(externalDefinitions[2].name == "helper");

    for (const auto& method : std::vector<mparser::SourceUnit>{
             {"read.m", "x=1;", "", "", "ExternalRange"},
             {"wrong.m", "function y=read(obj)\ny=1;\nend", "", "", "ExternalRange"},
             {"ExternalRange.m", "function obj=ExternalRange()\nobj=1;\nend", "", "", "ExternalRange"},
             {"read.m", "function y=read(obj)\ny=1;\nend", "", "", "MissingOwner"}}) {
        const auto invalidOwner = mparser::CompiledModule::compile(
            std::vector<mparser::SourceUnit>{{"ExternalRange.m", declarationSource},method});
        assert(!invalidOwner.valid());
        assert(!invalidOwner.diagnostics().empty());
    }

    const auto repeated = mparser::CompiledModule::compile(
        "function y=first()\ny=helper();\n"
        "function y=helper()\ny=1;\nend\nend\n"
        "function y=second()\ny=helper();\n"
        "function y=helper()\ny=2;\nend\nend\n");
    assert(repeated.valid());
    const auto& repeatedDefinitions = repeated.definitions();
    assert(repeatedDefinitions.size() == 4);
    assert(repeatedDefinitions[1].name == repeatedDefinitions[3].name);
    assert(repeatedDefinitions[1].parentFunction == 0);
    assert(repeatedDefinitions[3].parentFunction == 2);
    assert(repeatedDefinitions[1].span.begin.offset <
           repeatedDefinitions[3].span.begin.offset);
}

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
    runDefinitionDirectorySmoke();
    runCatalogSmoke();
    runRepeatedInvocationSmoke();
    runAdaptiveFactorySmoke();
    runInvalidCompilationSmoke();
    runClassCatalogBoundarySmoke();
    std::cout << "compiled module smoke tests passed\n";
    return 0;
}
