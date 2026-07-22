#include "mparser/adaptive_module_runtime.h"
#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

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
    std::string_view message) {
    if (diagnostics.empty()) {
        return;
    }
    std::string detail(message);
    for (const auto& diagnostic : diagnostics) {
        detail += " | ";
        detail += diagnostic.message;
    }
    throw std::runtime_error(std::move(detail));
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

bool containsSyntaxKind(const mparser::SyntaxNode& node,
                        mparser::SyntaxKind kind) {
    if (node.kind == kind) {
        return true;
    }
    for (const auto& child : node.children) {
        if (containsSyntaxKind(*child, kind)) {
            return true;
        }
    }
    return false;
}

bool containsHirKind(const mparser::HirNode& node, mparser::HirKind kind) {
    if (node.kind == kind) {
        return true;
    }
    for (const auto& child : node.children) {
        if (containsHirKind(*child, kind)) {
            return true;
        }
    }
    return false;
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

void requireOutput(const mparser::BytecodeVmResult& result, size_t index,
                   double expected) {
    require(index < result.outputs.size(), "expected runtime output");
    require(result.outputs[index].kind == mparser::RuntimeValueKind::Number,
            "expected numeric runtime output");
    require(std::fabs(result.outputs[index].number - expected) < 1e-9,
            "runtime output has an unexpected value");
}

mparser::RuntimeValue scalar(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

mparser::RuntimeValue text(std::string value) {
    return mparser::makeRuntimeStringScalarUtf8(value);
}

void syntaxAndRuntimeCoverage() {
    constexpr std::string_view source = R"([modern, modernNargin] = configure(2, Scale=4, Offset=3);
[legacy, legacyNargin] = configure(2, "Offset", 5, "Scale", 2);
[defaults, defaultNargin] = configure(2);
[partial, partialNargin] = configure(2, Sc=6);
[duplicate, duplicateNargin] = configure(2, Scale=2, Scale=7);
[optionalText, optionalTextNargin] = choose("Scale", 5);
[groupedValue, groupedNargin] = grouped(10, 1, 2, Scale=4);
[multiValue, multiNargin] = multi(Beta=4, Alpha=3);
[missingField, assigned] = sparseOptions(Offset=2);

function [y, seen] = configure(x, options)
arguments
    x (1,1) double
    options.Scale (1,1) double {mustBePositive} = 3
    options.Offset (1,1) double = 1
end
y = x * options.Scale + options.Offset;
seen = nargin;
end

function [y, seen] = choose(label, options)
arguments
    label string = "base"
    options.Scale (1,1) double = 1
end
y = options.Scale;
seen = nargin;
end

function [y, seen] = grouped(seed, x, options)
arguments
    seed (1,1) double
end
arguments (Repeating)
    x (1,1) double
end
arguments
    options.Scale (1,1) double = 1
end
y = seed + numel(x) + options.Scale;
seen = nargin;
end

function [y, seen] = multi(options, flags)
arguments
    options.Alpha (1,1) double = 1
    flags.Beta (1,1) double = 2
end
y = options.Alpha + flags.Beta;
seen = nargin;
end

function [hasMissing, y] = sparseOptions(options)
arguments
    options.Missing double
    options.Offset (1,1) double = 1
end
hasMissing = isfield(options, "Missing");
options.Offset = options.Offset + 1;
y = options.Offset;
end
)";

    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "name-value runtime source must parse");
    require(compilation.semantic.diagnostics.empty(),
            "name-value runtime source must pass semantic analysis");
    require(containsSyntaxKind(*compilation.parsed.root,
                               mparser::SyntaxKind::NameValueArgumentExpr),
            "modern name=value syntax needs a dedicated syntax node");
    require(containsHirKind(*compilation.semantic.root,
                            mparser::HirKind::NameValueArgument),
            "modern name=value syntax needs a dedicated HIR node");
    bool hasNameValueBytecode = false;
    for (const auto& instruction : compilation.bytecode.instructions) {
        hasNameValueBytecode =
            hasNameValueBytecode ||
            instruction.op == mparser::BytecodeOp::MakeNameValueArgument;
    }
    require(hasNameValueBytecode,
            "modern name=value syntax must lower to bytecode");

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(compilation.semantic);
    requireNoDiagnostics(interpreted.diagnostics,
                         "interpreter name-value execution must succeed");
    requireNumber(interpreted.variables, "modern", 11);
    requireNumber(interpreted.variables, "modernNargin", 1);
    requireNumber(interpreted.variables, "legacy", 9);
    requireNumber(interpreted.variables, "legacyNargin", 1);
    requireNumber(interpreted.variables, "defaults", 7);
    requireNumber(interpreted.variables, "defaultNargin", 1);
    requireNumber(interpreted.variables, "partial", 13);
    requireNumber(interpreted.variables, "partialNargin", 1);
    requireNumber(interpreted.variables, "duplicate", 15);
    requireNumber(interpreted.variables, "duplicateNargin", 1);
    requireNumber(interpreted.variables, "optionalText", 5);
    requireNumber(interpreted.variables, "optionalTextNargin", 0);
    requireNumber(interpreted.variables, "groupedValue", 16);
    requireNumber(interpreted.variables, "groupedNargin", 3);
    requireNumber(interpreted.variables, "multiValue", 7);
    requireNumber(interpreted.variables, "multiNargin", 0);
    requireNumber(interpreted.variables, "missingField", 0);
    requireNumber(interpreted.variables, "assigned", 3);

    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(bytecode.diagnostics,
                         "bytecode VM name-value execution must succeed");
    requireNumber(bytecode.variables, "modern", 11);
    requireNumber(bytecode.variables, "modernNargin", 1);
    requireNumber(bytecode.variables, "legacy", 9);
    requireNumber(bytecode.variables, "legacyNargin", 1);
    requireNumber(bytecode.variables, "defaults", 7);
    requireNumber(bytecode.variables, "defaultNargin", 1);
    requireNumber(bytecode.variables, "partial", 13);
    requireNumber(bytecode.variables, "partialNargin", 1);
    requireNumber(bytecode.variables, "duplicate", 15);
    requireNumber(bytecode.variables, "duplicateNargin", 1);
    requireNumber(bytecode.variables, "optionalText", 5);
    requireNumber(bytecode.variables, "optionalTextNargin", 0);
    requireNumber(bytecode.variables, "groupedValue", 16);
    requireNumber(bytecode.variables, "groupedNargin", 3);
    requireNumber(bytecode.variables, "multiValue", 7);
    requireNumber(bytecode.variables, "multiNargin", 0);
    requireNumber(bytecode.variables, "missingField", 0);
    requireNumber(bytecode.variables, "assigned", 3);
}

void runtimeDiagnostics() {
    const auto runBoth = [](std::string_view source) {
        auto compilation = compile(source);
        require(compilation.parsed.diagnostics.empty(),
                "diagnostic source must parse");
        require(compilation.semantic.diagnostics.empty(),
                "diagnostic source must pass semantic analysis");
        mparser::Interpreter interpreter;
        mparser::BytecodeVm vm;
        return std::pair{interpreter.run(compilation.semantic),
                         vm.run(compilation.bytecode, compilation.semantic)};
    };

    const auto ambiguous = runBoth(R"(answer = configured(S=2);
function y = configured(options)
arguments
    options.Scale double = 1
    options.Shape double = 2
end
y = options.Scale + options.Shape;
end
)");
    require(hasDiagnostic(ambiguous.first.diagnostics,
                          "ambiguous name-value argument: S"),
            "interpreter must reject ambiguous partial names");
    require(hasDiagnostic(ambiguous.second.diagnostics,
                          "ambiguous name-value argument: S"),
            "VM must reject ambiguous partial names");

    const auto unknown = runBoth(R"(answer = configured(Missing=2);
function y = configured(options)
arguments
    options.Scale double = 1
end
y = options.Scale;
end
)");
    require(hasDiagnostic(unknown.first.diagnostics,
                          "unknown name-value argument: Missing"),
            "interpreter must reject unknown names");
    require(hasDiagnostic(unknown.second.diagnostics,
                          "unknown name-value argument: Missing"),
            "VM must reject unknown names");

    const auto missingValue = runBoth(R"(answer = configured("Scale");
function y = configured(options)
arguments
    options.Scale double = 1
end
y = options.Scale;
end
)");
    require(hasDiagnostic(missingValue.first.diagnostics,
                          "name-value argument is missing a value: Scale"),
            "interpreter must reject a missing legacy value");
    require(hasDiagnostic(missingValue.second.diagnostics,
                          "name-value argument is missing a value: Scale"),
            "VM must reject a missing legacy value");

    const auto positionalAfterName = runBoth(R"(answer = configured(Scale=2, 3);
function y = configured(options)
arguments
    options.Scale double = 1
end
y = options.Scale;
end
)");
    require(hasDiagnostic(positionalAfterName.first.diagnostics,
                          "positional argument cannot follow"),
            "interpreter must reject positional values after name=value");
    require(hasDiagnostic(positionalAfterName.second.diagnostics,
                          "positional argument cannot follow"),
            "VM must reject positional values after name=value");

    const auto suppliedValidation = runBoth(R"(answer = configured(Scale=-1);
function y = configured(options)
arguments
    options.Scale double {mustBePositive} = 1
end
y = options.Scale;
end
)");
    require(hasDiagnostic(suppliedValidation.first.diagnostics,
                          "argument validation failed for configured.options.Scale"),
            "interpreter must validate supplied name-value arguments");
    require(hasDiagnostic(suppliedValidation.second.diagnostics,
                          "argument validation failed for configured.options.Scale"),
            "VM must validate supplied name-value arguments");

    const auto defaultValidation = runBoth(R"(answer = configured();
function y = configured(options)
arguments
    options.Scale double {mustBePositive} = -1
end
y = options.Scale;
end
)");
    require(hasDiagnostic(defaultValidation.first.diagnostics,
                          "argument validation failed for configured.options.Scale"),
            "interpreter must validate defaulted name-value arguments");
    require(hasDiagnostic(defaultValidation.second.diagnostics,
                          "argument validation failed for configured.options.Scale"),
            "VM must validate defaulted name-value arguments");
}

void semanticDiagnostics() {
    auto duplicate = compile(R"(function y = configured(options, flags)
arguments
    options.Scale double = 1
    flags.Scale double = 2
end
y = 1;
end
)");
    require(hasDiagnostic(duplicate.semantic.diagnostics,
                          "field must be globally unique: Scale"),
            "name-value fields must be unique across structures");

    auto collision = compile(R"(function y = configured(Scale, options)
arguments
    Scale double
    options.Scale double = 1
end
y = Scale;
end
)");
    require(hasDiagnostic(collision.semantic.diagnostics,
                          "conflicts with a positional or repeating parameter: Scale"),
            "name-value fields must not collide with positional inputs");

    auto defaultReference = compile(R"(function y = configured(options)
arguments
    options.Scale double = 1
    options.Offset double = options.Scale
end
y = options.Offset;
end
)");
    require(hasDiagnostic(defaultReference.semantic.diagnostics,
                          "default cannot reference a name-value structure"),
            "name-value defaults must be order independent");

    auto validatorReference = compile(R"(function y = configured(options)
arguments
    options.Scale double = 1
    options.Offset double {mustBeGreaterThan(options.Scale)} = 2
end
y = options.Offset;
end
)");
    require(hasDiagnostic(validatorReference.semantic.diagnostics,
                          "validator cannot reference a name-value structure"),
            "name-value validators must be order independent");
}

void staticMethodCoverage() {
    constexpr std::string_view source = R"(answer = NamedMath.scale(3, Scale=4);
created = NamedMath(2, Scale=5);
createdValue = created.Value;
methodValue = created.apply(1, Scale=3);
classdef NamedMath
properties
    Value
end
methods
    function obj = NamedMath(x, options)
    arguments
        x (1,1) double
        options.Scale (1,1) double = 2
    end
    obj.Value = x * options.Scale;
    end

    function y = apply(obj, x, options)
    arguments
        obj
        x (1,1) double
        options.Scale (1,1) double = 2
    end
    y = obj.Value + x * options.Scale;
    end
end
methods (Static)
    function y = scale(x, options)
    arguments
        x (1,1) double
        options.Scale (1,1) double = 2
    end
    y = x * options.Scale;
    end
end
end
)";
    auto compilation = compile(source);
    require(compilation.parsed.diagnostics.empty(),
            "name-value static method must parse");
    require(compilation.semantic.diagnostics.empty(),
            "name-value static method must pass semantic analysis");
    mparser::BytecodeVm vm;
    const auto result = vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(result.diagnostics,
                         "name-value static method must execute in the VM");
    requireNumber(result.variables, "answer", 12);
    requireNumber(result.variables, "createdValue", 10);
    requireNumber(result.variables, "methodValue", 13);
}

void compiledAndAdaptiveInvocation() {
    constexpr std::string_view source = R"(function [y, seen] = kernel(x, options)
arguments
    x (1,1) double
    options.Scale (1,1) double {mustBePositive} = 2
end
y = x * options.Scale;
seen = nargin;
end
)";
    auto module = mparser::CompiledModule::compile(std::string(source));
    require(module.valid(), "compiled name-value module must be valid");
    const auto* kernel = module.findFunction("kernel");
    require(kernel != nullptr, "compiled module must catalog the entry");
    require(kernel->nameValueArguments.size() == 1 &&
                kernel->nameValueArguments.front() == "options.Scale",
            "compiled module must retain name-value metadata");

    mparser::BytecodeVmOptions options;
    options.entryFunction = "kernel";
    options.arguments = {
        scalar(3),
        mparser::makeRuntimeNameValueArgument("Sc", scalar(4))};
    const auto modern = module.invoke(options);
    requireNoDiagnostics(
        modern.diagnostics,
        "compiled module must accept modern name-value wrappers");
    requireOutput(modern, 0, 12);
    requireOutput(modern, 1, 1);

    options.arguments = {scalar(3), text("Scale"), scalar(5)};
    const auto legacy = module.invoke(options);
    requireNoDiagnostics(
        legacy.diagnostics,
        "compiled module must accept legacy name/value pairs");
    requireOutput(legacy, 0, 15);
    requireOutput(legacy, 1, 1);

    options.arguments = {
        scalar(3),
        mparser::makeRuntimeNameValueArgument("Unknown", scalar(5))};
    const auto unknown = module.invoke(options);
    require(hasDiagnostic(unknown.diagnostics,
                          "unknown name-value argument: Unknown"),
            "compiled module preflight must inspect argument values");

    mparser::AdaptiveModuleRuntime runtime(module);
    const auto adaptive = runtime.invoke(
        "kernel",
        {scalar(2),
         mparser::makeRuntimeNameValueArgument("Scale", scalar(6))});
    requireNoDiagnostics(
        adaptive.adaptive.runtime.diagnostics,
        "adaptive module runtime must accept name-value arguments");
    requireOutput(adaptive.adaptive.runtime, 0, 12);
    requireOutput(adaptive.adaptive.runtime, 1, 1);
}

} // namespace

int main() {
    try {
        syntaxAndRuntimeCoverage();
        runtimeDiagnostics();
        semanticDiagnostics();
        staticMethodCoverage();
        compiledAndAdaptiveInvocation();
        std::cout << "name-value argument smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "name-value argument smoke test failed: "
                  << error.what() << "\n";
        return 1;
    }
}
