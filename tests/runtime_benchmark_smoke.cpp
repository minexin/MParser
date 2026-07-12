#include "mparser/bytecode.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_benchmark.h"
#include "mparser/semantic.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

const mparser::RuntimeValue* findVariable(
    const std::vector<mparser::RuntimeVariable>& variables,
    std::string_view name) {
    for (const auto& variable : variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const mparser::BytecodeTypedRegionExecutionProfile* findTypedExecution(
    const mparser::BytecodeVmResult& result, std::string_view target) {
    for (const auto& execution : result.typedRegionExecutions) {
        if (execution.kind == "scalar-loop" &&
            execution.target == target) {
            return &execution;
        }
    }
    return nullptr;
}

void runRuntimeBenchmarkSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * x + 1;
end
)";

    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());

    mparser::RuntimeBenchmarkRunner runner;
    const auto benchmark = runner.run(
        semantic, bytecode,
        mparser::RuntimeBenchmarkOptions{1, 3});

    assert(benchmark.interpreter.completedIterations == 3);
    assert(benchmark.profiledBytecodeVm.completedIterations == 3);
    assert(benchmark.bytecodeVm.completedIterations == 3);
    assert(benchmark.typedBytecodeVm.completedIterations == 3);
    assert(benchmark.outputsComparable);
    assert(benchmark.outputsMatch);
    assert(benchmark.interpreterProfiledBytecodeOutputsMatch);
    assert(benchmark.profiledSteadyBytecodeOutputsMatch);
    assert(benchmark.typedBytecodeOutputsMatch);
    assert(benchmark.comparisonMessage == "all runtime outputs match");
    assert(benchmark.typedRegionCount > 0);
    assert(benchmark.lastBytecodeVmResult.typedRegionExecutions.empty());
    assert(!benchmark.lastTypedBytecodeVmResult
                .typedRegionExecutions.empty());
    assert(benchmark.bytecodeVm.meanExecutedInstructionCount > 0.0);
    assert(benchmark.profiledBytecodeVm.meanExecutedInstructionCount ==
           benchmark.bytecodeVm.meanExecutedInstructionCount);
    assert(benchmark.lastProfiledBytecodeVmResult.profile.collected);
    assert(!benchmark.lastBytecodeVmResult.profile.collected);
    assert(!benchmark.lastTypedBytecodeVmResult.profile.collected);
    assert(benchmark.lastBytecodeVmResult.profile.instructions.empty());

    const auto* output =
        findVariable(benchmark.lastBytecodeVmResult.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(output->number == 662.0);

    bool rejectedZeroIterations = false;
    try {
        (void)runner.run(
            semantic, bytecode,
            mparser::RuntimeBenchmarkOptions{0, 0});
    } catch (const std::invalid_argument&) {
        rejectedZeroIterations = true;
    }
    assert(rejectedZeroIterations);
}

void runTypedRuntimeBenchmarkSmoke() {
    const std::string source = R"(function y = main()
y = 0;
for i = 1:12
    y = y + i * i;
end
end
)";

    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());

    mparser::RuntimeBenchmarkRunner runner;
    const auto benchmark = runner.run(
        semantic, bytecode,
        mparser::RuntimeBenchmarkOptions{1, 3});

    assert(benchmark.outputsComparable);
    assert(benchmark.outputsMatch);
    assert(benchmark.interpreterProfiledBytecodeOutputsMatch);
    assert(benchmark.profiledSteadyBytecodeOutputsMatch);
    assert(benchmark.typedBytecodeOutputsMatch);
    assert(benchmark.profiledBytecodeVm.completedIterations == 3);
    assert(benchmark.bytecodeVm.completedIterations == 3);
    assert(benchmark.typedBytecodeVm.completedIterations == 3);
    assert(benchmark.bytecodeVm.meanExecutedInstructionCount >
           benchmark.typedBytecodeVm.meanExecutedInstructionCount);
    assert(benchmark.typedBytecodeVm.meanTypedRegionAttemptCount == 1.0);
    assert(benchmark.typedBytecodeVm.meanTypedRegionExecutionCount == 1.0);
    assert(benchmark.typedBytecodeVm.meanTypedRegionFallbackCount == 0.0);
    assert(benchmark.typedBytecodeVm.meanTypedInstructionCount == 72.0);
    assert(benchmark.lastProfiledBytecodeVmResult.profile.collected);
    assert(!benchmark.lastBytecodeVmResult.profile.collected);
    assert(!benchmark.lastTypedBytecodeVmResult.profile.collected);

    const auto* execution =
        findTypedExecution(benchmark.lastTypedBytecodeVmResult, "i");
    assert(execution != nullptr);
    assert(execution->executionCount == 1);
    assert(execution->fallbackCount == 0);

    const auto* output = findVariable(
        benchmark.lastTypedBytecodeVmResult.variables, "y");
    assert(output != nullptr);
    assert(output->kind == mparser::RuntimeValueKind::Number);
    assert(output->number == 650.0);
}

} // namespace

int main() {
    runRuntimeBenchmarkSmoke();
    runTypedRuntimeBenchmarkSmoke();
    std::cout << "runtime benchmark smoke tests passed\n";
    return 0;
}
