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

void check(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::string diagnosticsText(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) {
        text += std::to_string(diagnostic.span.begin.line) + ":" +
                std::to_string(diagnostic.span.begin.column) + ": " +
                diagnostic.message + "\n";
    }
    return text;
}

struct Pipeline {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

Pipeline lower(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    check(parsed.diagnostics.empty(),
          "parse diagnostics:\n" + diagnosticsText(parsed.diagnostics));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    check(semantic.diagnostics.empty(),
          "semantic diagnostics:\n" +
              diagnosticsText(semantic.diagnostics));

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    check(bytecode.diagnostics.empty(),
          "bytecode diagnostics:\n" +
              diagnosticsText(bytecode.diagnostics));
    return Pipeline{std::move(semantic), std::move(bytecode)};
}

template <typename Result>
const mparser::RuntimeValue* findVariable(const Result& result,
                                          std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

template <typename Result>
void checkNumber(const Result& result, std::string_view name,
                 double expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing variable: " + std::string(name));
    check(value->kind == mparser::RuntimeValueKind::Number,
          "variable is not numeric: " + std::string(name));
    check(std::fabs(value->number - expected) < 1e-9,
          "unexpected numeric value: " + std::string(name));
}

template <typename Result>
void checkString(const Result& result, std::string_view name,
                 std::string_view expected) {
    const auto* value = findVariable(result, name);
    check(value != nullptr, "missing variable: " + std::string(name));
    const auto text = mparser::runtimeTextScalarUtf8(*value);
    check(text.has_value(), "variable is not text: " + std::string(name));
    check(*text == expected,
          "unexpected text value for " + std::string(name) + ": " +
              *text);
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

void checkDynamicScriptResult(const auto& result, std::string_view engine) {
    check(result.diagnostics.empty(), std::string(engine) +
                                          " diagnostics:\n" +
                                          diagnosticsText(result.diagnostics));
    checkNumber(result, "anonymous_value", 12.0);
    checkNumber(result, "anonymous_feval", 15.0);
    checkNumber(result, "named_first", 7.0);
    checkNumber(result, "named_second", 8.0);
    checkNumber(result, "text_first", 8.0);
    checkNumber(result, "text_second", 9.0);
    checkNumber(result, "converted_first", 9.0);
    checkNumber(result, "converted_second", 10.0);
    checkNumber(result, "indexed_value", 20.0);
    checkNumber(result, "dynamic_call", 6.0);
    checkNumber(result, "shadow_index", 5.0);
    checkNumber(result, "named_builtin_value", 0.0);
    checkNumber(result, "text_builtin_value", 0.0);
    checkNumber(result, "captured_factor", 3.0);
    checkString(result, "anonymous_name", "@(x)x * factor");
    checkString(result, "named_name", "pair");
    std::string variableNames;
    for (const auto& variable : result.variables) {
        variableNames += variable.name + " ";
    }
    check(findVariable(result, "detail_function") != nullptr,
          std::string(engine) + " did not retain detail_function; variables: " +
              variableNames);
    checkString(result, "detail_function", "@(x)x * factor");
    checkString(result, "detail_type", "anonymous");
}

void runInterpreterAndVmParitySmoke() {
    constexpr std::string_view source = R"(factor = 3;
anonymous = @(x)x * factor;
factor = 10;
anonymous_value = anonymous(4);
anonymous_feval = feval(anonymous, 5);

named = @pair;
[named_first, named_second] = feval(named, 7);
[text_first, text_second] = feval('pair', 8);
converted = str2func('pair');
[converted_first, converted_second] = converted(9);

values = [10 20 30];
dynamic_target = values;
indexed_value = dynamic_target(2);
dynamic_target = anonymous;
dynamic_call = dynamic_target(2);

sin = [4 5];
shadow_index = sin(2);
named_builtin = @sin;
named_builtin_value = named_builtin(0);
feval(named_builtin, 0);
text_builtin = str2func('sin');
text_builtin_value = feval(text_builtin, 0);

anonymous_name = func2str(anonymous);
named_name = func2str(named);
detail = functions(anonymous);
detail_function = detail.function;
detail_type = detail.type;
captured_factor = detail.workspace{1}.factor;

function [first, second] = pair(value)
first = value;
second = value + 1;
end
)";

    auto pipeline = lower(source);
    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(pipeline.bytecode, pipeline.semantic);
    checkDynamicScriptResult(bytecode, "bytecode VM");

    mparser::Interpreter interpreter;
    const auto hir = interpreter.run(pipeline.semantic);
    checkDynamicScriptResult(hir, "HIR interpreter");
}

void runTextDiagnosticParitySmoke() {
    auto pipeline = lower("bad = str2func('@(x)x + 1');\n");

    mparser::BytecodeVm vm;
    const auto bytecode = vm.run(pipeline.bytecode, pipeline.semantic);
    check(hasDiagnostic(bytecode.diagnostics,
                        "does not parse anonymous function text"),
          "bytecode VM accepted anonymous function text");

    mparser::Interpreter interpreter;
    const auto hir = interpreter.run(pipeline.semantic);
    check(hasDiagnostic(hir.diagnostics,
                        "does not parse anonymous function text"),
          "HIR interpreter accepted anonymous function text");
}

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

mparser::BytecodeVmResult invoke(
    const mparser::CompiledModule& module, std::string entry,
    std::vector<mparser::RuntimeValue> arguments) {
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = std::move(entry);
    options.arguments = std::move(arguments);
    return module.invoke(options);
}

void checkScalarOutput(const mparser::BytecodeVmResult& result,
                       double expected, std::string_view context) {
    check(result.diagnostics.empty(), std::string(context) +
                                          " diagnostics:\n" +
                                          diagnosticsText(result.diagnostics));
    check(result.outputs.size() == 1,
          std::string(context) + " did not return one output");
    check(result.outputs.front().kind ==
              mparser::RuntimeValueKind::Number &&
              std::fabs(result.outputs.front().number - expected) < 1e-9,
          std::string(context) + " returned the wrong value");
}

void runCompiledModulePersistenceSmoke() {
    constexpr std::string_view producerSource = R"(function handle = make_closure(factor)
handle = @(value)value * factor;
end

function handle = make_named()
handle = @double_value;
end

function handle = make_builtin()
handle = @sin;
end

function result = apply_handle(handle, value)
result = handle(value);
end

function result = double_value(value)
result = value * 2;
end
)";
    auto producer =
        mparser::CompiledModule::compile(std::string(producerSource));
    check(producer.valid(), "producer module did not compile");

    const auto closure = invoke(producer, "make_closure", {number(4)});
    check(closure.diagnostics.empty() && closure.outputs.size() == 1 &&
              closure.outputs.front().kind ==
                  mparser::RuntimeValueKind::FunctionHandle,
          "closure handle was not returned");
    const auto closureCopy = closure.outputs.front();
    check(closureCopy.functionHandle && closure.outputs.front().functionHandle &&
              closureCopy.functionHandle->identity ==
                  closure.outputs.front().functionHandle->identity,
          "copying a handle changed its descriptor identity");
    const auto secondClosure =
        invoke(producer, "make_closure", {number(4)});
    check(secondClosure.diagnostics.empty() &&
              secondClosure.outputs.size() == 1 &&
              secondClosure.outputs.front().functionHandle &&
              secondClosure.outputs.front().functionHandle->identity !=
                  closure.outputs.front().functionHandle->identity,
          "separate handle creation reused a descriptor identity");
    checkScalarOutput(invoke(producer, "apply_handle",
                             {closure.outputs.front(), number(5)}),
                      20.0, "same-module closure invocation");
    checkScalarOutput(invoke(producer, "apply_handle",
                             {closure.outputs.front(), number(6)}),
                      24.0, "repeated closure invocation");

    mparser::AdaptiveBytecodeVmOptions adaptiveOptions;
    adaptiveOptions.hotLoopThreshold = 100;
    adaptiveOptions.entryFunction = "apply_handle";
    adaptiveOptions.arguments = {closure.outputs.front(), number(2)};
    adaptiveOptions.requestedOutputCount = 1;
    auto adaptive = producer.createAdaptiveSession(adaptiveOptions);
    checkScalarOutput(adaptive.run().runtime, 8.0,
                      "adaptive closure invocation");
    adaptive.setArguments({closure.outputs.front(), number(3)});
    checkScalarOutput(adaptive.run().runtime, 12.0,
                      "repeated adaptive closure invocation");

    const auto named = invoke(producer, "make_named", {});
    check(named.diagnostics.empty() && named.outputs.size() == 1,
          "named handle was not returned");
    checkScalarOutput(invoke(producer, "apply_handle",
                             {named.outputs.front(), number(7)}),
                      14.0, "same-module named invocation");

    auto consumer = mparser::CompiledModule::compile(R"(
function result = apply_external(handle, value)
result = handle(value);
end
)" );
    check(consumer.valid(), "consumer module did not compile");
    const auto foreign = invoke(consumer, "apply_external",
                                {closure.outputs.front(), number(3)});
    check(hasDiagnostic(foreign.diagnostics,
                        "different compiled module"),
          "foreign module accepted a module-bound closure");

    const auto builtin = invoke(producer, "make_builtin", {});
    check(builtin.diagnostics.empty() && builtin.outputs.size() == 1,
          "builtin handle was not returned");
    checkScalarOutput(invoke(consumer, "apply_external",
                             {builtin.outputs.front(), number(0)}),
                      0.0, "cross-module builtin invocation");
}

} // namespace

int main() {
    try {
        runInterpreterAndVmParitySmoke();
        runTextDiagnosticParitySmoke();
        runCompiledModulePersistenceSmoke();
        std::cout << "function handle runtime smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
