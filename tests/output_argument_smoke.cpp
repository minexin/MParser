#include "mparser/embedding/adaptive_module_runtime.h"
#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/semantic/function_signature.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/semantic/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Compilation {
    mparser::ParseResult parsed;
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

struct BothResults {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult bytecode;
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

void requireNoDiagnostics(
    const std::vector<mparser::Diagnostic>& diagnostics,
    std::string_view context) {
    if (diagnostics.empty()) {
        return;
    }
    std::string message(context);
    for (const auto& diagnostic : diagnostics) {
        message += " | ";
        message += diagnostic.message;
    }
    throw std::runtime_error(message);
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
    if (!semantic.root || !semantic.diagnostics.empty()) {
        return {std::move(parsed), std::move(semantic), {}};
    }
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

BothResults runBoth(std::string_view source) {
    auto compilation = compile(source);
    requireNoDiagnostics(compilation.parsed.diagnostics,
                         "source did not parse");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "source did not lower to HIR");
    requireNoDiagnostics(compilation.bytecode.diagnostics,
                         "source did not lower to bytecode");
    mparser::Interpreter interpreter;
    mparser::BytecodeVm vm;
    return {interpreter.run(compilation.semantic),
            vm.run(compilation.bytecode, compilation.semantic)};
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
    require(value != nullptr, "expected runtime variable is missing");
    require(value->kind == mparser::RuntimeValueKind::Number,
            "expected a numeric runtime variable");
    require(std::fabs(value->number - expected) < 1e-9,
            "numeric runtime variable has the wrong value");
}

void requireOutput(const mparser::BytecodeVmResult& result, size_t index,
                   double expected) {
    require(index < result.outputs.size(), "expected output is missing");
    require(result.outputs[index].kind == mparser::RuntimeValueKind::Number,
            "expected a numeric output");
    require(std::fabs(result.outputs[index].number - expected) < 1e-9,
            "numeric output has the wrong value");
}

mparser::RuntimeValue scalar(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    result.dimensions = {1, 1};
    return result;
}

const mparser::HirNode* findFunction(const mparser::HirNode& node,
                                     std::string_view name) {
    if (node.kind == mparser::HirKind::Function && node.label == name) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = findFunction(*child, name)) {
            return found;
        }
    }
    return nullptr;
}

void fixedOutputConversionAndValidation() {
    constexpr std::string_view source = R"(
[flag,row] = checked(2,[1;2;3]);
summary = flag + sum(row);

function [flag,row] = checked(x,column)
arguments (Output)
    flag (1,1) logical
    row (1,:) double {mustBePositive}
end
flag = x;
row = column;
end
)";
    const auto results = runBoth(source);
    requireNoDiagnostics(results.interpreter.diagnostics,
                         "interpreter fixed output validation failed");
    requireNoDiagnostics(results.bytecode.diagnostics,
                         "VM fixed output validation failed");
    requireNumber(results.interpreter.variables, "summary", 7);
    requireNumber(results.bytecode.variables, "summary", 7);

    for (const auto* flag :
         {variable(results.interpreter.variables, "flag"),
          variable(results.bytecode.variables, "flag")}) {
        require(flag != nullptr, "converted logical output is missing");
        require(flag->numericClass ==
                    mparser::RuntimeNumericClass::Logical &&
                    flag->number == 1,
                "output class conversion was not retained");
    }
    for (const auto* row :
         {variable(results.interpreter.variables, "row"),
          variable(results.bytecode.variables, "row")}) {
        require(row != nullptr, "reshaped output is missing");
        require(mparser::runtimeDimensions(*row) ==
                    std::vector<size_t>({1, 3}),
                "output size adaptation was not retained");
    }

    const auto rejected = runBoth(R"(
answer = positive(-1);
function y = positive(x)
arguments (Output)
    y (1,1) double {mustBePositive}
end
y = x;
end
)");
    require(hasDiagnostic(
                rejected.interpreter.diagnostics,
                "output argument validation failed for positive.y"),
            "interpreter must reject an invalid fixed output");
    require(hasDiagnostic(
                rejected.bytecode.diagnostics,
                "output argument validation failed for positive.y"),
            "VM must reject an invalid fixed output");

    const auto assignedTail = runBoth(R"(
answer = pair();
function [first,second] = pair()
arguments (Output)
    first (1,1) double {mustBePositive}
    second (1,1) double {mustBePositive}
end
first = 1;
second = -1;
end
)");
    require(hasDiagnostic(
                assignedTail.interpreter.diagnostics,
                "output argument validation failed for pair.second"),
            "interpreter must validate assigned outputs beyond the requested prefix");
    require(hasDiagnostic(
                assignedTail.bytecode.diagnostics,
                "output argument validation failed for pair.second"),
            "VM must validate assigned outputs beyond the requested prefix");

    const auto unassignedTail = runBoth(R"(
answer = pair();
function [first,second] = pair()
arguments (Output)
    first (1,1) double {mustBePositive}
    second (1,1) double {mustBePositive}
end
first = 1;
end
)");
    requireNoDiagnostics(
        unassignedTail.interpreter.diagnostics,
        "interpreter must skip an unassigned output contract");
    requireNoDiagnostics(unassignedTail.bytecode.diagnostics,
                         "VM must skip an unassigned output contract");
    requireNumber(unassignedTail.interpreter.variables, "answer", 1);
    requireNumber(unassignedTail.bytecode.variables, "answer", 1);
}

void namedRepeatingOutputs() {
    constexpr std::string_view source = R"(
[first,second,count] = repeated(2);
summary = first * 100 + second * 10 + count;

function values = repeated(seed)
arguments (Output,Repeating)
    values (1,1) double {mustBePositive}
end
values{1} = seed;
values{2} = seed + 1;
values{3} = nargout;
end
)";
    auto compilation = compile(source);
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "named repeating output source did not compile");
    const auto* function =
        findFunction(*compilation.semantic.root, "repeated");
    require(function != nullptr, "repeated function is missing from HIR");
    const auto signature = mparser::parseFunctionSignature(*function);
    require(signature.repeatingOutput == "values",
            "signature did not retain the repeating output name");
    require(mparser::functionFixedOutputCount(signature) == 0,
            "named repeating function must have no fixed outputs");
    require(mparser::functionOutputCountIsValid(signature, 4),
            "named repeating output must accept expanded result counts");

    mparser::Interpreter interpreter;
    mparser::BytecodeVm vm;
    const auto interpreted = interpreter.run(compilation.semantic);
    const auto bytecode =
        vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(interpreted.diagnostics,
                         "interpreter named repeating output failed");
    requireNoDiagnostics(bytecode.diagnostics,
                         "VM named repeating output failed");
    requireNumber(interpreted.variables, "summary", 233);
    requireNumber(bytecode.variables, "summary", 233);

    const auto rejected = runBoth(R"(
[first,second] = repeated(2);
function values = repeated(seed)
arguments (Output,Repeating)
    values (1,1) double {mustBePositive}
end
values{1} = seed;
values{2} = -seed;
end
)");
    require(hasDiagnostic(
                rejected.interpreter.diagnostics,
                "output argument validation failed for repeated.values{2}"),
            "interpreter must identify the failing output occurrence");
    require(hasDiagnostic(
                rejected.bytecode.diagnostics,
                "output argument validation failed for repeated.values{2}"),
            "VM must identify the failing output occurrence");

    const auto nonCell = runBoth(R"(
answer = repeated();
function values = repeated()
arguments (Output,Repeating)
    values double
end
values = 3;
end
)");
    require(hasDiagnostic(nonCell.interpreter.diagnostics,
                          "repeating output must be a Cell"),
            "interpreter must preserve repeating output Cell storage");
    require(hasDiagnostic(nonCell.bytecode.diagnostics,
                          "repeating output must be a Cell"),
            "VM must preserve repeating output Cell storage");
}

void fixedAndVariadicOutputComposition() {
    const auto mixed = runBoth(R"(
[head,first,second] = partition(5);
summary = head + first + second;
function [head,tail] = partition(seed)
arguments (Output)
    head (1,1) double
end
arguments (Output,Repeating)
    tail (1,1) double {mustBePositive}
end
head = seed;
tail{1} = seed + 1;
tail{2} = seed + 2;
end
)");
    requireNoDiagnostics(mixed.interpreter.diagnostics,
                         "interpreter fixed/repeating outputs failed");
    requireNoDiagnostics(mixed.bytecode.diagnostics,
                         "VM fixed/repeating outputs failed");
    requireNumber(mixed.interpreter.variables, "summary", 18);
    requireNumber(mixed.bytecode.variables, "summary", 18);

    const auto varargout = runBoth(R"(
[first,second] = spread(4);
summary = first * 10 + second;
function varargout = spread(seed)
arguments (Output,Repeating)
    varargout (1,1) double {mustBePositive}
end
varargout{1} = seed;
varargout{2} = nargout;
end
)");
    requireNoDiagnostics(varargout.interpreter.diagnostics,
                         "interpreter validated varargout failed");
    requireNoDiagnostics(varargout.bytecode.diagnostics,
                         "VM validated varargout failed");
    requireNumber(varargout.interpreter.variables, "summary", 42);
    requireNumber(varargout.bytecode.variables, "summary", 42);
}

void reusableAndAdaptiveInvocation() {
    const auto module = mparser::CompiledModule::compile(R"(
function values = repeated(seed)
arguments (Output,Repeating)
    values (1,1) double {mustBePositive}
end
values{1} = seed;
values{2} = seed + 1;
values{3} = nargout;
end
)");
    require(module.valid(), "repeating output module did not compile");
    const auto* function = module.findFunction("repeated");
    require(function != nullptr, "compiled function catalog is incomplete");
    require(function->signature.repeatingOutput == "values",
            "compiled signature lost its repeating output");
    require(module.validateInvocation("repeated", 1, 3).empty(),
            "module preflight rejected an expanded output count");

    mparser::BytecodeVmOptions options;
    options.entryFunction = "repeated";
    options.arguments = {scalar(4)};
    options.requestedOutputCount = 3;
    const auto result = module.invoke(options);
    requireNoDiagnostics(result.diagnostics,
                         "compiled module repeating output invocation failed");
    require(result.outputNames ==
                std::vector<std::string>({"values1", "values2", "values3"}),
            "named repeating output slots have unstable names");
    requireOutput(result, 0, 4);
    requireOutput(result, 1, 5);
    requireOutput(result, 2, 3);

    mparser::AdaptiveModuleRuntime runtime(module);
    const auto adaptive = runtime.invoke("repeated", {scalar(6)}, 3);
    requireNoDiagnostics(adaptive.adaptive.runtime.diagnostics,
                         "adaptive repeating output invocation failed");
    requireOutput(adaptive.adaptive.runtime, 0, 6);
    requireOutput(adaptive.adaptive.runtime, 1, 7);
    requireOutput(adaptive.adaptive.runtime, 2, 3);
}

void classMethodAndConstructorOutputs() {
    auto compilation = compile(R"(
classdef CheckedBox
    properties
        Value
    end
    methods
        function obj = CheckedBox(value)
        arguments (Output)
            obj CheckedBox
        end
        obj.Value = value;
        end
        function y = scale(obj,factor)
        arguments (Output)
            y (1,1) double {mustBePositive}
        end
        y = obj.Value * factor;
        end
    end
    methods (Static)
        function values = sequence(seed)
        arguments (Output,Repeating)
            values (1,1) double {mustBePositive}
        end
        values{1} = seed;
        values{2} = seed + 1;
        end
        function obj = invalid()
        arguments (Output)
            obj CheckedBox
        end
        obj = 1;
        end
    end
end

box = CheckedBox(5);
[first,second] = CheckedBox.sequence(3);
scaled = box.scale(2);
summary = box.Value + first + second + scaled;
)");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "class output contracts did not compile");
    mparser::BytecodeVm vm;
    const auto result =
        vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(result.diagnostics,
                         "class output contracts did not execute");
    requireNumber(result.variables, "summary", 22);

    auto invalid = compile(R"(
classdef CheckedBox
    methods (Static)
        function obj = invalid()
        arguments (Output)
            obj CheckedBox
        end
        obj = 1;
        end
    end
end
box = CheckedBox.invalid();
)");
    requireNoDiagnostics(invalid.semantic.diagnostics,
                         "invalid class output test did not compile");
    const auto rejected = vm.run(invalid.bytecode, invalid.semantic);
    require(hasDiagnostic(
                rejected.diagnostics,
                "output argument validation failed for CheckedBox.invalid.obj"),
            "class output type validation must run after static methods");
}

void semanticRestrictions() {
    auto misplaced = compile(R"(
function [tail,head] = invalid()
arguments (Output,Repeating)
    tail double
end
head = 1;
tail{1} = 2;
end
)");
    require(hasDiagnostic(
                misplaced.semantic.diagnostics,
                "repeating output argument must be last in the function signature"),
            "repeating outputs must form the final output tail");

    auto blockOrder = compile(R"(
function [head,tail] = invalid()
arguments (Output,Repeating)
    tail double
end
arguments (Output)
    head double
end
head = 1;
tail{1} = 2;
end
)");
    require(hasDiagnostic(
                blockOrder.semantic.diagnostics,
                "fixed output block cannot follow a repeating output block"),
            "fixed output contracts must precede repeating output contracts");

    auto reference = compile(R"(
function [low,high] = invalid()
arguments (Output)
    low double
    high double {mustBeGreaterThan(high,low)}
end
low = 1;
high = 2;
end
)");
    require(hasDiagnostic(
                reference.semantic.diagnostics,
                "output argument validator cannot reference an earlier output: low"),
            "output validators must not depend on earlier outputs");
}

} // namespace

int main() {
    try {
        fixedOutputConversionAndValidation();
        namedRepeatingOutputs();
        fixedAndVariadicOutputComposition();
        reusableAndAdaptiveInvocation();
        classMethodAndConstructorOutputs();
        semanticRestrictions();
        std::cout << "output argument smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "output argument smoke test failed: " << error.what()
                  << "\n";
        return 1;
    }
}
