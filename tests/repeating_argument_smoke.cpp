#include "mparser/adaptive_module_runtime.h"
#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/compiled_module.h"
#include "mparser/function_signature.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Compilation {
    mparser::ParseResult parsed;
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
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

Compilation compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    if (!parsed.root || !parsed.diagnostics.empty()) {
        return {std::move(parsed), {}, {}};
    }
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

const mparser::RuntimeValue* variable(
    const std::vector<mparser::RuntimeVariable>& variables,
    std::string_view name) {
    for (const auto& candidate : variables) {
        if (candidate.name == name) {
            return &candidate.value;
        }
    }
    return nullptr;
}

void requireNumber(const std::vector<mparser::RuntimeVariable>& variables,
                   std::string_view name, double expected) {
    const auto* value = variable(variables, name);
    require(value != nullptr, "expected runtime variable");
    require(value->kind == mparser::RuntimeValueKind::Number,
            "expected numeric runtime variable");
    require(std::fabs(value->number - expected) < 1e-9,
            "runtime variable has an unexpected value");
}

mparser::RuntimeValue scalar(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

void requireOutput(const mparser::BytecodeVmResult& result, size_t index,
                   double expected) {
    require(index < result.outputs.size(), "expected runtime output");
    require(result.outputs[index].kind == mparser::RuntimeValueKind::Number,
            "expected numeric runtime output");
    require(std::fabs(result.outputs[index].number - expected) < 1e-9,
            "runtime output has an unexpected value");
}

void explicitGroupModel() {
    constexpr std::string_view source = R"(function y = grouped(seed,x,options)
arguments (Input)
    seed (1,1) double
end
arguments (Input,Repeating)
    x (1,1) double
end
arguments
    options.Scale (1,1) double = 1
end
arguments (Output)
    y (1,1) double
end
y = seed;
end
)";
    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "group model source must parse");
    require(compilation.semantic.diagnostics.empty(),
            "group model source must pass semantic analysis");

    const auto& syntaxFunction = *compilation.parsed.root->children.front();
    require(syntaxFunction.children.size() == 5,
            "function must retain four argument blocks and its body");
    require(syntaxFunction.children[0]->argumentBlock.kind ==
                mparser::ArgumentBlockKind::Input,
            "explicit Input block kind was not retained");
    require(syntaxFunction.children[1]->argumentBlock.kind ==
                mparser::ArgumentBlockKind::RepeatingInput,
            "repeating input block kind was not retained");
    require(syntaxFunction.children[2]->children.front()->label ==
                "options.Scale",
            "name-value declaration must retain its dotted name");
    require(syntaxFunction.children[3]->argumentBlock.kind ==
                mparser::ArgumentBlockKind::Output,
            "output block kind was not retained");

    const auto& hirFunction = *compilation.semantic.root->children.front();
    require(hirFunction.children[0]->kind == mparser::HirKind::ArgumentBlock,
            "arguments block must have a dedicated HIR node");
    const auto signature = mparser::parseFunctionSignature(hirFunction);
    require(signature.parameterKinds.size() == 3,
            "signature must classify every named parameter");
    require(signature.parameterKinds[0] ==
                mparser::FunctionParameterKind::Positional,
            "seed must be positional");
    require(signature.parameterKinds[1] ==
                mparser::FunctionParameterKind::Repeating,
            "x must be repeating");
    require(signature.parameterKinds[2] ==
                mparser::FunctionParameterKind::NameValue,
            "options must be name-value");
    require(mparser::functionRequiredPositionalParameterCount(signature) == 1,
            "seed must remain the only required positional parameter");
}

void repeatingLocalFunction() {
    constexpr std::string_view source = R"([zeroCount, zeroFirst, zeroLast, zeroNargin] = summarize(10);
[count, firstX, lastY, observedNargin] = summarize(10, 1, 2, 3, 4);

function [count, firstX, lastY, observedNargin] = summarize(seed, x, y)
arguments
    seed (1,1) double {mustBePositive}
end
arguments (Repeating)
    x (1,1) double {mustBePositive}
    y (1,1) double {mustBeInteger}
end
count = numel(x);
firstX = seed;
lastY = seed;
if count > 0
    firstX = x{1};
    lastY = y{count};
end
observedNargin = nargin;
end
)";
    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "repeating local function must parse");
    require(compilation.semantic.diagnostics.empty(),
            "repeating local function must pass semantic analysis");

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(compilation.semantic);
    require(interpreted.diagnostics.empty(),
            "HIR interpreter repeating call failed");
    requireNumber(interpreted.variables, "zeroCount", 0);
    requireNumber(interpreted.variables, "zeroFirst", 10);
    requireNumber(interpreted.variables, "zeroLast", 10);
    requireNumber(interpreted.variables, "zeroNargin", 1);
    requireNumber(interpreted.variables, "count", 2);
    requireNumber(interpreted.variables, "firstX", 1);
    requireNumber(interpreted.variables, "lastY", 4);
    requireNumber(interpreted.variables, "observedNargin", 5);

    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(compilation.bytecode, compilation.semantic);
    require(bytecode.diagnostics.empty(), "bytecode repeating call failed");
    requireNumber(bytecode.variables, "zeroCount", 0);
    requireNumber(bytecode.variables, "zeroNargin", 1);
    requireNumber(bytecode.variables, "count", 2);
    requireNumber(bytecode.variables, "firstX", 1);
    requireNumber(bytecode.variables, "lastY", 4);
    requireNumber(bytecode.variables, "observedNargin", 5);
}

void repeatingDiagnostics() {
    constexpr std::string_view incompleteSource = R"(answer = collect(1, 2);
function count = collect(seed, x, y)
arguments
    seed (1,1) double
end
arguments (Repeating)
    x (1,1) double
    y (1,1) double
end
count = numel(x);
end
)";
    auto incomplete = compile(incompleteSource);
    require(incomplete.semantic.diagnostics.empty(),
            "incomplete-call source must be semantically valid");
    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(incomplete.semantic);
    require(hasDiagnostic(interpreted.diagnostics,
                          "incomplete repeating argument group"),
            "interpreter must diagnose an incomplete repeating group");
    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(incomplete.bytecode, incomplete.semantic);
    require(hasDiagnostic(bytecode.diagnostics,
                          "incomplete repeating argument group"),
            "bytecode VM must diagnose an incomplete repeating group");

    constexpr std::string_view invalidValueSource = R"(answer = collect(1, 2, 3, -4, 5);
function count = collect(seed, x, y)
arguments
    seed (1,1) double
end
arguments (Repeating)
    x (1,1) double {mustBePositive}
    y (1,1) double {mustBeInteger}
end
count = numel(x);
end
)";
    auto invalid = compile(invalidValueSource);
    require(invalid.semantic.diagnostics.empty(),
            "invalid-value call must compile before runtime validation");
    const auto interpretedInvalid = interpreter.run(invalid.semantic);
    require(hasDiagnostic(interpretedInvalid.diagnostics, "collect.x{2}"),
            "interpreter must identify the failing repetition");
    const auto bytecodeInvalid = vm.run(invalid.bytecode, invalid.semantic);
    require(hasDiagnostic(bytecodeInvalid.diagnostics, "collect.x{2}"),
            "bytecode VM must identify the failing repetition");
}

void repeatingVarargin() {
    constexpr std::string_view source = R"(count = collect(1, 2, 3);
function count = collect(varargin)
arguments (Repeating)
    varargin (1,1) double {mustBePositive}
end
count = numel(varargin);
end
)";
    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "repeating varargin source must parse");
    require(compilation.semantic.diagnostics.empty(),
            "repeating varargin source must pass semantic analysis");

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(compilation.semantic);
    require(interpreted.diagnostics.empty(),
            "interpreter repeating varargin failed");
    requireNumber(interpreted.variables, "count", 3);

    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(compilation.bytecode, compilation.semantic);
    require(bytecode.diagnostics.empty(), "bytecode repeating varargin failed");
    requireNumber(bytecode.variables, "count", 3);
}

void repeatingStaticMethod() {
    constexpr std::string_view source = R"(answer = PairMath.combine(1, 2, 3, 4);
classdef PairMath
    methods (Static)
        function total = combine(left, right)
        arguments (Repeating)
            left (1,1) double
            right (1,1) double
        end
        total = 0;
        for i = 1:numel(left)
            total = total + left{i} * right{i};
        end
        end
    end
end
)";
    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "repeating static method must parse");
    require(compilation.semantic.diagnostics.empty(),
            "repeating static method must pass semantic analysis");
    mparser::BytecodeVm vm;
    const auto result = vm.run(compilation.bytecode, compilation.semantic);
    require(result.diagnostics.empty(), "repeating static method failed");
    requireNumber(result.variables, "answer", 14);
}

void repeatingNamedEntry() {
    constexpr std::string_view source = R"(function count = collect(x)
arguments (Repeating)
    x (1,1) double {mustBePositive}
end
count = numel(x);
end
)";
    auto compilation = compile(source);
    require(compilation.semantic.diagnostics.empty(),
            "named repeating entry must compile");
    mparser::BytecodeVmOptions options;
    options.entryFunction = "collect";
    options.arguments = {scalar(2), scalar(3), scalar(4)};
    options.requestedOutputCount = 1;
    mparser::BytecodeVm vm;
    const auto result =
        vm.run(compilation.bytecode, compilation.semantic, options);
    require(result.diagnostics.empty(), "named repeating entry failed");
    require(result.outputs.size() == 1, "named entry must return one output");
    require(std::fabs(result.outputs.front().number - 3) < 1e-9,
            "named entry returned the wrong repetition count");

    options.arguments.clear();
    const auto empty =
        vm.run(compilation.bytecode, compilation.semantic, options);
    require(empty.diagnostics.empty(), "zero-repetition named entry failed");
    require(empty.outputs.size() == 1 &&
                std::fabs(empty.outputs.front().number) < 1e-9,
            "zero-repetition named entry must return zero");
}

void reusableModuleInvocation() {
    constexpr std::string_view source = R"(function [count,total] = collect(seed,x,y)
arguments
    seed (1,1) double = 10
end
arguments (Repeating)
    x (1,1) double {mustBePositive}
    y (1,1) double {mustBeInteger}
end
count = numel(x);
total = seed;
for i = 1:count
    total = total + x{i} * y{i};
end
end
)";
    const auto module = mparser::CompiledModule::compile(std::string(source));
    require(module.valid(), "reusable repeating module must compile");

    const auto* function = module.findFunction("collect");
    require(function != nullptr, "compiled function catalog is missing collect");
    require(mparser::functionRequiredPositionalParameterCount(
                function->signature) == 0,
            "compiled signature must retain the optional seed");
    require(mparser::functionPositionalParameterCount(function->signature) == 1,
            "compiled signature must retain one positional parameter");
    require(mparser::functionRepeatingParameterCount(function->signature) == 2,
            "compiled signature must retain a two-value repeating group");

    require(module.validateInvocation("collect", 0, 2).empty(),
            "compiled module must accept defaults with zero repetitions");
    require(module.validateInvocation("collect", 5, 2).empty(),
            "compiled module must accept complete repeating groups");
    const auto incomplete = module.validateInvocation("collect", 2, 2);
    require(hasDiagnostic(incomplete, "incomplete repeating argument group"),
            "compiled module must reject an incomplete repeating group");

    mparser::BytecodeVmOptions options;
    options.entryFunction = "collect";
    options.requestedOutputCount = 2;
    const auto empty = module.invoke(options);
    require(empty.diagnostics.empty(),
            "compiled module zero-repetition invocation failed");
    requireOutput(empty, 0, 0);
    requireOutput(empty, 1, 10);

    options.arguments =
        {scalar(2), scalar(1), scalar(3), scalar(4), scalar(5)};
    const auto repeated = module.invoke(options);
    require(repeated.diagnostics.empty(),
            "compiled module repeating invocation failed");
    requireOutput(repeated, 0, 2);
    requireOutput(repeated, 1, 25);

    mparser::AdaptiveModuleRuntime adaptive(module);
    const auto adaptiveEmpty = adaptive.invoke("collect", {}, 2);
    require(adaptiveEmpty.adaptive.runtime.diagnostics.empty(),
            "adaptive zero-repetition invocation failed");
    requireOutput(adaptiveEmpty.adaptive.runtime, 0, 0);
    requireOutput(adaptiveEmpty.adaptive.runtime, 1, 10);
    const auto adaptiveRepeated = adaptive.invoke(
        "collect", {scalar(2), scalar(1), scalar(3), scalar(4), scalar(5)}, 2);
    require(adaptiveRepeated.adaptive.runtime.diagnostics.empty(),
            "adaptive repeating invocation failed");
    requireOutput(adaptiveRepeated.adaptive.runtime, 0, 2);
    requireOutput(adaptiveRepeated.adaptive.runtime, 1, 25);
}

void semanticRestrictions() {
    auto repeatingDefault = compile(R"(function y = f(x)
arguments (Repeating)
    x double = 1
end
y = 0;
end
)");
    require(hasDiagnostic(repeatingDefault.semantic.diagnostics,
                          "repeating input argument cannot define a default"),
            "repeating defaults must be rejected");

    auto mismatchedOrder = compile(R"(function y = f(x,y)
arguments (Repeating)
    y double
    x double
end
y = 0;
end
)");
    require(hasDiagnostic(mismatchedOrder.semantic.diagnostics,
                          "order must match the function signature"),
            "repeating declaration order must match the signature");

    auto forbiddenFunction = compile(R"(function y = f(x)
arguments
    x double = nargin + 1
end
y = x;
end
)");
    require(hasDiagnostic(forbiddenFunction.semantic.diagnostics,
                          "function is not allowed in an arguments block: nargin"),
            "workspace-sensitive functions must be rejected in defaults");

    auto undeclaredVarargin = compile(R"(function y = f(x,varargin)
arguments (Repeating)
    x double
end
y = numel(x);
end
)");
    require(hasDiagnostic(
                undeclaredVarargin.semantic.diagnostics,
                "varargin must be declared as the only argument"),
            "varargin cannot coexist beside a different repeating group");

    auto unvalidatedVarargin = compile(R"(function y = f(x,varargin)
arguments
    x double
end
y = x;
end
)");
    require(hasDiagnostic(
                unvalidatedVarargin.semantic.diagnostics,
                "varargin must be declared as the only argument"),
            "varargin must participate in input argument validation");

    auto nonRepeatingVarargout = compile(R"(function varargout = f()
arguments (Output)
    varargout
end
end
)");
    require(hasDiagnostic(
                nonRepeatingVarargout.semantic.diagnostics,
                "varargout must be declared in a Repeating output"),
            "varargout must use a repeating output block");
}

void nameValueGroupsExecuteAndOutputGroupsRemainExplicit() {
    constexpr std::string_view nameValueSource = R"(answer = configured(2);
function y = configured(x,options)
arguments
    x (1,1) double
    options.Scale (1,1) double = 3
end
y = x;
end
)";
    auto nameValue = compile(nameValueSource);
    require(nameValue.semantic.diagnostics.empty(),
            "name-value metadata source must compile");
    mparser::Interpreter interpreter;
    const auto interpretedNameValue = interpreter.run(nameValue.semantic);
    require(interpretedNameValue.diagnostics.empty(),
            "interpreter must execute defaulted name-value contracts");
    requireNumber(interpretedNameValue.variables, "answer", 2);
    mparser::BytecodeVm vm;
    const auto bytecodeNameValue =
        vm.run(nameValue.bytecode, nameValue.semantic);
    require(bytecodeNameValue.diagnostics.empty(),
            "bytecode VM must execute defaulted name-value contracts");
    requireNumber(bytecodeNameValue.variables, "answer", 2);

    constexpr std::string_view outputSource = R"(answer = checkedOutput(2);
function y = checkedOutput(x)
arguments
    x (1,1) double
end
arguments (Output)
    y (1,1) double {mustBePositive}
end
y = x;
end
)";
    auto output = compile(outputSource);
    require(output.semantic.diagnostics.empty(),
            "output metadata source must compile");
    const auto interpretedOutput = interpreter.run(output.semantic);
    require(hasDiagnostic(interpretedOutput.diagnostics,
                          "output arguments blocks are not executable yet"),
            "interpreter must not silently ignore output contracts");
    const auto bytecodeOutput = vm.run(output.bytecode, output.semantic);
    require(hasDiagnostic(bytecodeOutput.diagnostics,
                          "output arguments blocks are not executable yet"),
            "bytecode VM must not silently ignore output contracts");
}

} // namespace

int main() {
    try {
        explicitGroupModel();
        repeatingLocalFunction();
        repeatingDiagnostics();
        repeatingVarargin();
        repeatingStaticMethod();
        repeatingNamedEntry();
        reusableModuleInvocation();
        semanticRestrictions();
        nameValueGroupsExecuteAndOutputGroupsRemainExplicit();
        std::cout << "repeating argument smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "repeating argument smoke test failed: " << error.what()
                  << "\n";
        return 1;
    }
}
