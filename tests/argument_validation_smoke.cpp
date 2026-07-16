#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

struct Compilation {
    mparser::ParseResult parsed;
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

Compilation compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    if (!parsed.diagnostics.empty()) {
        return {std::move(parsed), {}, {}};
    }
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    auto compilation = compile(source);
    assert(compilation.parsed.diagnostics.empty());
    assert(compilation.semantic.diagnostics.empty());
    mparser::BytecodeVm vm;
    return vm.run(compilation.bytecode, compilation.semantic);
}

mparser::InterpreterResult runInterpreter(std::string_view source) {
    auto compilation = compile(source);
    assert(compilation.parsed.diagnostics.empty());
    assert(compilation.semantic.diagnostics.empty());
    mparser::Interpreter interpreter;
    return interpreter.run(compilation.semantic);
}

bool hasDiagnostic(const std::vector<mparser::Diagnostic>& diagnostics,
                   std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::RuntimeValue* variable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return &candidate.value;
        }
    }
    return nullptr;
}

const mparser::RuntimeValue* variable(
    const mparser::InterpreterResult& result, std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return &candidate.value;
        }
    }
    return nullptr;
}

void successfulFunctionContract() {
    constexpr std::string_view source = R"(answer = scale(3);
function y = scale(x)
arguments
    x (1,1) double {mustBePositive, mustBeInteger}
end
y = x * 2;
end
)";
    const auto result = run(source);
    assert(result.diagnostics.empty());
    const auto* answer = variable(result, "answer");
    assert(answer != nullptr);
    assert(answer->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(answer->number - 6.0) < 1e-9);

    const auto interpreted = runInterpreter(source);
    assert(interpreted.diagnostics.empty());
    const auto* interpretedAnswer = variable(interpreted, "answer");
    assert(interpretedAnswer != nullptr);
    assert(std::fabs(interpretedAnswer->number - 6.0) < 1e-9);
}

void rejectedFunctionContracts() {
    constexpr std::string_view negativeSource = R"(answer = scale(-2);
function y = scale(x)
arguments
    x (1,1) double {mustBePositive}
end
y = x;
end
)";
    const auto negative = run(negativeSource);
    assert(hasDiagnostic(negative.diagnostics,
                         "argument validation failed for scale.x"));
    assert(hasDiagnostic(negative.diagnostics, "value must be positive"));
    const auto interpretedNegative = runInterpreter(negativeSource);
    assert(hasDiagnostic(interpretedNegative.diagnostics,
                         "argument validation failed for scale.x"));
    assert(hasDiagnostic(interpretedNegative.diagnostics,
                         "value must be positive"));

    const auto vector = run(R"(answer = scale([1 2]);
function y = scale(x)
arguments
    x (1,1) double
end
y = x;
end
)");
    assert(hasDiagnostic(vector.diagnostics,
                         "argument validation failed for scale.x"));
}

void classMethodContract() {
    const auto result = run(R"(classdef Meter
    properties
        Value
    end
    methods
        function obj = Meter(value)
        arguments
            value (1,1) double {mustBeNonnegative}
        end
            obj.Value = value;
        end
        function y = scale(obj, factor)
        arguments
            factor (1,1) double {mustBePositive}
        end
            y = obj.Value * factor;
        end
    end
end
m = Meter(4);
answer = m.scale(3);
)");
    assert(result.diagnostics.empty());
    const auto* answer = variable(result, "answer");
    assert(answer != nullptr && std::fabs(answer->number - 12.0) < 1e-9);

    const auto rejected = run(R"(classdef Meter
    methods
        function obj = Meter(value)
        arguments
            value (1,1) double {mustBeNonnegative}
        end
        end
    end
end
m = Meter(-1);
)");
    assert(hasDiagnostic(rejected.diagnostics,
                         "argument validation failed for Meter.value"));
}

void semanticContractDiagnostics() {
    const auto unknown = compile(R"(function y = f(x)
arguments
    other (1,1) double
end
y = x;
end
)");
    assert(unknown.parsed.diagnostics.empty());
    assert(hasDiagnostic(unknown.semantic.diagnostics,
                         "is not a function parameter"));

    const auto defaults = compile(R"(function y = f(x)
arguments
    x (1,1) double = 2
end
y = x;
end
)");
    assert(defaults.parsed.diagnostics.empty());
    assert(hasDiagnostic(defaults.semantic.diagnostics,
                         "default argument values are not executable yet"));
}

void entryFunctionContract() {
    auto compilation = compile(R"(function y = checked(x)
arguments
    x (1,1) double {mustBeGreaterThan(x, 4)}
end
y = x;
end
)");
    assert(compilation.parsed.diagnostics.empty());
    assert(compilation.semantic.diagnostics.empty());

    mparser::BytecodeVmOptions options;
    options.entryFunction = "checked";
    mparser::RuntimeValue input;
    input.kind = mparser::RuntimeValueKind::Number;
    input.number = 5;
    input.rows = 1;
    input.columns = 1;
    options.arguments = {input};
    options.requestedOutputCount = 1;
    mparser::BytecodeVm vm;
    const auto accepted = vm.run(
        compilation.bytecode, compilation.semantic, options);
    assert(accepted.diagnostics.empty());
    assert(accepted.outputs.size() == 1);
    assert(std::fabs(accepted.outputs.front().number - 5.0) < 1e-9);

    options.arguments.front().number = 4;
    const auto rejected = vm.run(
        compilation.bytecode, compilation.semantic, options);
    assert(hasDiagnostic(rejected.diagnostics,
                         "argument validation failed for checked.x"));
}

void stringScalarContract() {
    constexpr std::string_view source = R"(answer = checked("hello");
function y = checked(text)
arguments
    text (1,1) string {mustBeScalarOrEmpty, mustBeNonzeroLengthText}
end
y = text;
end
)";
    const auto bytecode = run(source);
    assert(bytecode.diagnostics.empty());
    const auto* bytecodeAnswer = variable(bytecode, "answer");
    assert(bytecodeAnswer != nullptr && bytecodeAnswer->text == "hello");

    const auto interpreted = runInterpreter(source);
    assert(interpreted.diagnostics.empty());
    const auto* interpretedAnswer = variable(interpreted, "answer");
    assert(interpretedAnswer != nullptr && interpretedAnswer->text == "hello");
}

} // namespace

int main() {
    successfulFunctionContract();
    rejectedFunctionContracts();
    classMethodContract();
    semanticContractDiagnostics();
    entryFunctionContract();
    stringScalarContract();
    std::cout << "argument validation smoke tests passed\n";
    return 0;
}
