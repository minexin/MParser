#include "mparser/semantic/argument_contract.h"
#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
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
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [&](const mparser::Diagnostic& diagnostic) {
            return diagnostic.message.find(text) != std::string::npos;
        });
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

const mparser::SyntaxNode* sourceDeclaration(
    const mparser::SyntaxNode& node) {
    if (!node.nameValueSourceClass.empty()) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = sourceDeclaration(*child)) {
            return found;
        }
    }
    return nullptr;
}

const mparser::HirNode* sourceDeclaration(const mparser::HirNode& node) {
    if (!node.nameValueSourceClass.empty()) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = sourceDeclaration(*child)) {
            return found;
        }
    }
    return nullptr;
}

const mparser::HirNode* functionNamed(const mparser::HirNode& node,
                                     std::string_view name) {
    if (node.kind == mparser::HirKind::Function &&
        node.label == name) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = functionNamed(*child, name)) {
            return found;
        }
    }
    return nullptr;
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

constexpr std::string_view kRuntimeSource = R"(
answer = configure(Width=3, Height=0);
partial = configure(Wid=4, Height=2);
duplicate = configure(Width=1, Height=2, Width=5);
[hasWidth, hasHeight] = omitted();

function y = configure(options)
arguments
    options.Height (1,1) double {mustBeNonnegative}
    options.?PlotOptions
end
y = options.Width * 10 + options.Height;
end

function [widthPresent, heightPresent] = omitted(options)
arguments
    options.?PlotOptions
end
widthPresent = isfield(options, "Width");
heightPresent = isfield(options, "Height");
end

classdef BaseOptions
properties
    Width (1,1) double {mustBePositive} = 100
    Weight (1,1) double {mustBeNonnegative} = 5
end
properties (SetAccess = private)
    Secret (1,1) double = 9
end
properties (SetAccess = protected)
    Internal (1,1) double = 8
end
end

classdef PlotOptions < BaseOptions
properties
    Height (1,1) double {mustBePositive} = 200
end
properties (Constant)
    Version = 1
end
end
)";

void syntaxHirAndCatalogCoverage() {
    auto compilation = compile(kRuntimeSource);
    requireNoDiagnostics(compilation.parsed.diagnostics,
                         "class-property argument syntax must parse");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "class-property argument source must be semantic");

    const auto* syntaxSource =
        sourceDeclaration(*compilation.parsed.root);
    require(syntaxSource != nullptr,
            "syntax tree must retain the class-property source");
    require(syntaxSource->label == "options",
            "syntax source must retain the structure root");
    require(syntaxSource->nameValueSourceClass == "PlotOptions",
            "syntax source must retain the class name");

    const auto* hirSource =
        sourceDeclaration(*compilation.semantic.root);
    require(hirSource != nullptr,
            "HIR must retain the class-property source");
    require(hirSource->label == "options" &&
                hirSource->nameValueSourceClass == "PlotOptions",
            "HIR source metadata is incomplete");

    const auto catalog =
        mparser::buildArgumentContractCatalog(*compilation.semantic.root);
    const auto* configure =
        functionNamed(*compilation.semantic.root, "configure");
    require(configure != nullptr, "configure HIR function is missing");
    const auto resolution =
        mparser::resolveArgumentContracts(*configure, catalog);
    requireNoDiagnostics(resolution.diagnostics,
                         "class-property contracts must resolve");

    const auto findContract =
        [&](std::string_view name)
        -> const mparser::ResolvedArgumentContract* {
        const auto found = std::find_if(
            resolution.contracts.begin(), resolution.contracts.end(),
            [&](const mparser::ResolvedArgumentContract& contract) {
                return contract.name == name;
            });
        return found == resolution.contracts.end() ? nullptr : &*found;
    };
    const auto* width = findContract("options.Width");
    const auto* weight = findContract("options.Weight");
    const auto* height = findContract("options.Height");
    require(width != nullptr && width->classDerived,
            "inherited public Width contract must be expanded");
    require(weight != nullptr && weight->classDerived,
            "inherited public Weight contract must be expanded");
    require(height != nullptr && !height->classDerived,
            "explicit Height contract must override the class property");
    require(findContract("options.Secret") == nullptr,
            "private properties must not become name-value arguments");
    require(findContract("options.Internal") == nullptr,
            "protected properties must not become name-value arguments");
    require(findContract("options.Version") == nullptr,
            "constant properties must not become name-value arguments");
    require(!width->property.hasExplicitDefault,
            "class property defaults must not populate name-value structs");
    require(height->property.validators.size() == 1 &&
                height->property.validators.front().name ==
                    "mustBeNonnegative",
            "explicit field validation must override class validation");
}

void interpreterAndVmCoverage() {
    auto compilation = compile(kRuntimeSource);
    requireNoDiagnostics(compilation.parsed.diagnostics,
                         "runtime source must parse");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "runtime source must pass semantic analysis");
    requireNoDiagnostics(compilation.bytecode.diagnostics,
                         "runtime source must lower");

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(compilation.semantic);
    requireNoDiagnostics(interpreted.diagnostics,
                         "interpreter class-property arguments must execute");
    requireNumber(interpreted.variables, "answer", 30);
    requireNumber(interpreted.variables, "partial", 42);
    requireNumber(interpreted.variables, "duplicate", 52);
    requireNumber(interpreted.variables, "hasWidth", 0);
    requireNumber(interpreted.variables, "hasHeight", 0);

    mparser::BytecodeVm vm;
    const auto bytecode =
        vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(bytecode.diagnostics,
                         "VM class-property arguments must execute");
    requireNumber(bytecode.variables, "answer", 30);
    requireNumber(bytecode.variables, "partial", 42);
    requireNumber(bytecode.variables, "duplicate", 52);
    requireNumber(bytecode.variables, "hasWidth", 0);
    requireNumber(bytecode.variables, "hasHeight", 0);
}

std::pair<mparser::InterpreterResult, mparser::BytecodeVmResult> runBoth(
    std::string_view invocation) {
    std::string source(invocation);
    source += R"(
function y = configure(options)
arguments
    options.?PlotOptions
end
y = options.Width;
end

classdef PlotOptions
properties
    Width (1,1) double {mustBePositive}
    Weight (1,1) double {mustBeNonnegative}
end
properties (SetAccess = private)
    Secret double
end
properties (Constant)
    Version = 1
end
end
)";
    auto compilation = compile(source);
    requireNoDiagnostics(compilation.parsed.diagnostics,
                         "diagnostic source must parse");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "diagnostic source must be semantically valid");
    mparser::Interpreter interpreter;
    mparser::BytecodeVm vm;
    return {interpreter.run(compilation.semantic),
            vm.run(compilation.bytecode, compilation.semantic)};
}

void runtimeDiagnostics() {
    const auto invalid =
        runBoth("answer = configure(Width=-1);\n");
    require(hasDiagnostic(invalid.first.diagnostics,
                          "argument validation failed for configure.options.Width"),
            "interpreter must execute derived property validation");
    require(hasDiagnostic(invalid.second.diagnostics,
                          "argument validation failed for configure.options.Width"),
            "VM must execute derived property validation");

    const auto ambiguous =
        runBoth("answer = configure(W=2);\n");
    require(hasDiagnostic(ambiguous.first.diagnostics,
                          "ambiguous name-value argument: W"),
            "interpreter must detect derived partial-name ambiguity");
    require(hasDiagnostic(ambiguous.second.diagnostics,
                          "ambiguous name-value argument: W"),
            "VM must detect derived partial-name ambiguity");

    const auto privateProperty =
        runBoth("answer = configure(Secret=2);\n");
    require(hasDiagnostic(privateProperty.first.diagnostics,
                          "unknown name-value argument: Secret"),
            "interpreter must hide private properties");
    require(hasDiagnostic(privateProperty.second.diagnostics,
                          "unknown name-value argument: Secret"),
            "VM must hide private properties");

    const auto constantProperty =
        runBoth("answer = configure(Version=2);\n");
    require(hasDiagnostic(constantProperty.first.diagnostics,
                          "unknown name-value argument: Version"),
            "interpreter must hide constant properties");
    require(hasDiagnostic(constantProperty.second.diagnostics,
                          "unknown name-value argument: Version"),
            "VM must hide constant properties");
}

void semanticDiagnostics() {
    const auto unknown = compile(R"(
function y = configure(options)
arguments
    options.?MissingOptions
end
y = 1;
end
)");
    require(hasDiagnostic(unknown.semantic.diagnostics,
                          "source class is not available: MissingOptions"),
            "unknown class-property sources must be diagnosed");

    const auto multiple = compile(R"(
function y = configure(options, flags)
arguments
    options.?FirstOptions
    flags.?SecondOptions
end
y = 1;
end
classdef FirstOptions
properties
    First double
end
end
classdef SecondOptions
properties
    Second double
end
end
)");
    require(hasDiagnostic(multiple.semantic.diagnostics,
                          "only one class-property name-value source"),
            "a function must reject multiple class-property sources");

    const auto missingRoot = compile(R"(
function y = configure(x)
arguments
    options.?PlotOptions
end
y = x;
end
classdef PlotOptions
properties
    Width double
end
end
)");
    require(hasDiagnostic(missingRoot.semantic.diagnostics,
                          "name-value structure is not a function parameter: options"),
            "class-property source roots must be parameters");

    const auto positionalCollision = compile(R"(
function y = configure(Width, options)
arguments
    Width double
    options.?PlotOptions
end
y = Width;
end
classdef PlotOptions
properties
    Width double
end
end
)");
    require(hasDiagnostic(
                positionalCollision.semantic.diagnostics,
                "conflicts with a positional or repeating parameter: Width"),
            "derived fields must not collide with positional parameters");

    const auto fieldCollision = compile(R"(
function y = configure(options, flags)
arguments
    options.?PlotOptions
    flags.Width double
end
y = 1;
end
classdef PlotOptions
properties
    Width double
end
end
)");
    require(hasDiagnostic(fieldCollision.semantic.diagnostics,
                          "field must be globally unique: Width"),
            "derived fields must remain globally unique");

    const auto repeatingBlock = compile(R"(
function y = configure(options)
arguments (Repeating)
    options.?PlotOptions
end
y = 1;
end
classdef PlotOptions
properties
    Width double
end
end
)");
    require(hasDiagnostic(
                repeatingBlock.semantic.diagnostics,
                "class-property name-value source cannot appear in a Repeating block"),
            "class-property sources must stay in ordinary input blocks");

    const auto outputBlock = compile(R"(
function options = configure()
arguments (Output)
    options.?PlotOptions
end
end
classdef PlotOptions
properties
    Width double
end
end
)");
    require(hasDiagnostic(
                outputBlock.semantic.diagnostics,
                "output arguments cannot use a class-property name-value source"),
            "output blocks must reject class-property sources");
}

void compiledModuleCoverage() {
    constexpr std::string_view source = R"(
function y = kernel(options)
arguments
    options.?KernelOptions
    options.Offset (1,1) double {mustBeNonnegative}
end
y = options.Scale * 10 + options.Offset;
end

classdef KernelOptions
properties
    Scale (1,1) double {mustBePositive} = 2
    Offset (1,1) double {mustBePositive} = 3
end
properties (SetAccess = private)
    Hidden double
end
end
)";
    auto module = mparser::CompiledModule::compile(std::string(source));
    require(module.valid(), "compiled class-property module must be valid");
    const auto* kernel = module.findFunction("kernel");
    require(kernel != nullptr, "compiled module must catalog kernel");
    require(kernel->nameValueArguments.size() == 2,
            "compiled module must expand class-property arguments");
    require(std::find(kernel->nameValueArguments.begin(),
                      kernel->nameValueArguments.end(),
                      "options.Scale") !=
                kernel->nameValueArguments.end(),
            "compiled module must retain derived Scale metadata");
    require(std::find(kernel->nameValueArguments.begin(),
                      kernel->nameValueArguments.end(),
                      "options.Offset") !=
                kernel->nameValueArguments.end(),
            "compiled module must retain explicit Offset metadata");

    mparser::BytecodeVmOptions options;
    options.entryFunction = "kernel";
    options.arguments = {
        mparser::makeRuntimeNameValueArgument("Scale", scalar(4)),
        mparser::makeRuntimeNameValueArgument("Offset", scalar(0))};
    const auto result = module.invoke(options);
    requireNoDiagnostics(result.diagnostics,
                         "compiled class-property invocation must execute");
    require(result.outputs.size() == 1 &&
                result.outputs.front().kind ==
                    mparser::RuntimeValueKind::Number &&
                std::fabs(result.outputs.front().number - 40) < 1e-9,
            "compiled class-property invocation returned the wrong value");

    mparser::SourceUnit functionSource;
    functionSource.name = "qualified_kernel.m";
    functionSource.primaryFunctionIdentity = "qualified_kernel";
    functionSource.content = R"(
function y = qualified_kernel(options)
arguments
    options.?pkg.QualifiedOptions
end
y = options.Scale;
end
)";
    mparser::SourceUnit classSource;
    classSource.name = "+pkg/QualifiedOptions.m";
    classSource.namespaceName = "pkg";
    classSource.content = R"(
classdef QualifiedOptions
properties
    Scale (1,1) double {mustBePositive}
end
end
)";
    auto qualified = mparser::CompiledModule::compile(
        {std::move(functionSource), std::move(classSource)});
    require(qualified.valid(),
            "qualified class-property module must be valid");
    const auto* qualifiedKernel =
        qualified.findFunction("qualified_kernel");
    require(qualifiedKernel != nullptr &&
                qualifiedKernel->nameValueArguments.size() == 1 &&
                qualifiedKernel->nameValueArguments.front() ==
                    "options.Scale",
            "qualified source classes must expand their properties");
    options.entryFunction = "qualified_kernel";
    options.arguments = {
        mparser::makeRuntimeNameValueArgument("Scale", scalar(6))};
    const auto qualifiedResult = qualified.invoke(options);
    requireNoDiagnostics(
        qualifiedResult.diagnostics,
        "qualified class-property invocation must execute");
    require(qualifiedResult.outputs.size() == 1 &&
                qualifiedResult.outputs.front().kind ==
                    mparser::RuntimeValueKind::Number &&
                std::fabs(qualifiedResult.outputs.front().number - 6) < 1e-9,
            "qualified class-property invocation returned the wrong value");
}

void immutableConstructorCoverage() {
    constexpr std::string_view source = R"(
created = ImmutableOptions(Code=7);
createdCode = created.Code;

classdef ImmutableOptions
properties (SetAccess = immutable)
    Code (1,1) double {mustBePositive} = 1
end
methods
    function obj = ImmutableOptions(options)
    arguments
        options.?ImmutableOptions
    end
    obj.Code = options.Code;
    end
end
end
)";
    auto compilation = compile(source);
    requireNoDiagnostics(compilation.parsed.diagnostics,
                         "immutable constructor source must parse");
    requireNoDiagnostics(compilation.semantic.diagnostics,
                         "immutable constructor source must be semantic");
    mparser::BytecodeVm vm;
    const auto result =
        vm.run(compilation.bytecode, compilation.semantic);
    requireNoDiagnostics(
        result.diagnostics,
        "constructor class-property source must include immutable properties");
    requireNumber(result.variables, "createdCode", 7);

    constexpr std::string_view plainFunctionSource = R"(
answer = configure(Code=3);
function y = configure(options)
arguments
    options.?ImmutableOptions
end
y = 1;
end
classdef ImmutableOptions
properties (SetAccess = immutable)
    Code (1,1) double {mustBePositive} = 1
end
end
)";
    auto plainFunction = compile(plainFunctionSource);
    requireNoDiagnostics(plainFunction.parsed.diagnostics,
                         "plain immutable source must parse");
    requireNoDiagnostics(plainFunction.semantic.diagnostics,
                         "plain immutable source must be semantic");
    mparser::Interpreter interpreter;
    const auto interpreted =
        interpreter.run(plainFunction.semantic);
    require(hasDiagnostic(interpreted.diagnostics,
                          "unknown name-value argument: Code"),
            "immutable properties must stay hidden outside constructors");
    const auto bytecode =
        vm.run(plainFunction.bytecode, plainFunction.semantic);
    require(hasDiagnostic(bytecode.diagnostics,
                          "unknown name-value argument: Code"),
            "VM must hide immutable properties outside constructors");
}

} // namespace

int main() {
    try {
        syntaxHirAndCatalogCoverage();
        interpreterAndVmCoverage();
        runtimeDiagnostics();
        semanticDiagnostics();
        compiledModuleCoverage();
        immutableConstructorCoverage();
        std::cout << "class-property argument smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "class-property argument smoke test failed: "
                  << error.what() << "\n";
        return 1;
    }
}
