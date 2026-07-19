#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_exception.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"
#include "mparser/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

mparser::SemanticResult analyze(std::string_view source,
                                std::string sourceName) {
    mparser::Lexer lexer(source, 0);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.diagnostics.empty(), "exception source did not parse");

    mparser::SourceUnit unit;
    unit.name = std::move(sourceName);
    unit.content = std::string(source);
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root, {unit});
    require(semantic.diagnostics.empty(),
            "exception source did not pass semantic analysis");
    return semantic;
}

mparser::InterpreterResult runInterpreter(std::string_view source,
                                          std::string sourceName) {
    auto semantic = analyze(source, std::move(sourceName));
    mparser::Interpreter interpreter;
    return interpreter.run(semantic);
}

mparser::BytecodeVmResult runBytecode(std::string_view source,
                                      std::string sourceName) {
    auto semantic = analyze(source, std::move(sourceName));
    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "exception source did not lower to bytecode");
    mparser::BytecodeVm vm;
    return vm.run(bytecode, semantic);
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

const mparser::RuntimeValue& requiredProperty(
    const mparser::RuntimeValue& exception, std::string_view name) {
    const auto* value = mparser::runtimeExceptionProperty(exception, name);
    require(value != nullptr,
            "missing MException property: " + std::string(name));
    return *value;
}

mparser::RuntimeValue stringValue(std::string text) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::String;
    result.text = std::move(text);
    mparser::setRuntimeDimensions(result, {1, result.text.size()});
    return result;
}

mparser::RuntimeValue numberValue(double number) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = number;
    mparser::setRuntimeDimensions(result, {1, 1});
    return result;
}

template <typename Result>
void verifyCaughtExceptions(const Result& result) {
    require(result.diagnostics.empty(),
            "caught exception escaped the runtime");
    const auto* summary = findVariable(result, "summary");
    require(summary != nullptr &&
                summary->kind == mparser::RuntimeValueKind::Number &&
                std::fabs(summary->number - 111111111.0) < 1e-9,
            "exception demo summary mismatch");

    const auto* caught = findVariable(result, "caught");
    require(caught != nullptr && mparser::isRuntimeException(*caught),
            "catch variable is not an MException");
    require(mparser::runtimeExceptionFrameCount(*caught) == 1,
            "caught exception did not retain one throw-site frame");

    const auto& identifier = requiredProperty(*caught, "identifier");
    const auto& message = requiredProperty(*caught, "message");
    const auto& stack = requiredProperty(*caught, "stack");
    const auto& cause = requiredProperty(*caught, "cause");
    require(identifier.kind == mparser::RuntimeValueKind::String &&
                identifier.text == "MParserDemo:BadValue",
            "caught exception identifier mismatch");
    require(message.kind == mparser::RuntimeValueKind::String &&
                message.text == "Value 7 for item",
            "caught exception message mismatch");
    require(stack.kind == mparser::RuntimeValueKind::Struct,
            "caught exception stack is not a structure");
    require(stack.fields.at("file").text == "exception_runtime_smoke.m",
            "caught exception source file mismatch");
    require(stack.fields.at("name").text == "exception_runtime_demo",
            "caught exception function name mismatch");
    require(stack.fields.at("line").number == 3.0,
            "caught exception source line mismatch");
    require(cause.kind == mparser::RuntimeValueKind::Cell &&
                mparser::runtimeDimensions(cause) ==
                    std::vector<size_t>({0, 1}),
            "caught exception cause is not an empty column Cell");

    const auto* thrown = findVariable(result, "thrown");
    const auto* reraised = findVariable(result, "reraised");
    require(thrown != nullptr && reraised != nullptr &&
                mparser::isRuntimeException(*thrown) &&
                mparser::isRuntimeException(*reraised),
            "throw/rethrow did not bind MException objects");
    require(requiredProperty(*thrown, "stack").fields.at("line").number ==
                requiredProperty(*reraised, "stack").fields.at("line").number,
            "rethrow did not preserve the original throw site");
}

const std::string kExceptionSource = R"(function summary = exception_runtime_demo()
try
    error("MParserDemo:BadValue", "Value %d for %s", 7, "item");
catch caught
    caught_id = caught.identifier;
    caught_message = caught.message;
    caught_class = class(caught);
    caught_line = caught.stack.line;
    caught_stack_is_struct = isstruct(caught.stack);
    caught_cause_is_empty = isempty(caught.cause);
end
manual = MException("MParserDemo:Manual", "Manual %s", "failure");
try
    throw(manual);
catch thrown
    throw_line = thrown.stack.line;
    try
        rethrow(thrown);
    catch reraised
        rethrow_line = reraised.stack.line;
    end
end
try
    missing_runtime_name;
catch automatic
    automatic_id = automatic.identifier;
end
id_ok = strcmp(caught_id, "MParserDemo:BadValue");
message_ok = strcmp(caught_message, "Value 7 for item");
class_ok = strcmp(caught_class, "MException");
isa_ok = isa(caught, "MException");
line_ok = caught_line > 0;
preserved_ok = throw_line == rethrow_line;
automatic_ok = strcmp(automatic_id, "MParser:RuntimeError");
summary = id_ok * 100000000 + message_ok * 10000000 + ...
          class_ok * 1000000 + isa_ok * 100000 + line_ok * 10000 + ...
          caught_stack_is_struct * 1000 + caught_cause_is_empty * 100 + ...
          preserved_ok * 10 + automatic_ok;
end
)";

void runCaughtParitySmoke() {
    verifyCaughtExceptions(runInterpreter(
        kExceptionSource, "exception_runtime_smoke.m"));
    verifyCaughtExceptions(runBytecode(
        kExceptionSource, "exception_runtime_smoke.m"));
}

template <typename Result>
void verifyUncaught(const Result& result) {
    require(result.diagnostics.size() == 1,
            "uncaught error did not produce one diagnostic");
    const auto& diagnostic = result.diagnostics.front();
    require(diagnostic.identifier == "MParserDemo:Uncaught",
            "uncaught diagnostic identifier mismatch");
    require(diagnostic.message == "bad 9",
            "uncaught diagnostic message mismatch");
    require(diagnostic.span.begin.line == 1,
            "uncaught diagnostic source line mismatch");
}

void runUncaughtParitySmoke() {
    constexpr std::string_view source =
        "error(\"MParserDemo:Uncaught\", \"bad %d\", 9);";
    verifyUncaught(runInterpreter(source, "uncaught_hir.m"));
    verifyUncaught(runBytecode(source, "uncaught_vm.m"));
}

void runSharedContractSmoke() {
    require(mparser::isRuntimeExceptionIdentifier("Component:Mnemonic"),
            "valid exception identifier was rejected");
    require(mparser::isRuntimeExceptionIdentifier("A:B:C_2"),
            "multi-component exception identifier was rejected");
    require(!mparser::isRuntimeExceptionIdentifier("NoColon"),
            "identifier without a mnemonic separator was accepted");
    require(!mparser::isRuntimeExceptionIdentifier("A:bad value"),
            "identifier containing whitespace was accepted");

    const auto formatted = mparser::runtimeConstructMException(
        {stringValue("Format:Scalar"),
         stringValue("%d %.1f %% %c"), numberValue(7),
         numberValue(2.25), stringValue("Z")});
    require(formatted.succeeded &&
                requiredProperty(formatted.value, "message").text ==
                    "7 2.2 % Z",
            "scalar exception formatting mismatch");

    auto errorStruct = mparser::makeRuntimeStructValue();
    mparser::runtimeSetStructField(
        errorStruct, "identifier", stringValue("Struct:Failure"));
    mparser::runtimeSetStructField(
        errorStruct, "message", stringValue("structured failure"));
    const auto structured =
        mparser::runtimeCreateErrorException({errorStruct});
    require(structured.succeeded &&
                requiredProperty(structured.value, "identifier").text ==
                    "Struct:Failure" &&
                requiredProperty(structured.value, "message").text ==
                    "structured failure",
            "error structure construction mismatch");

    const auto invalidIdentifier = mparser::runtimeConstructMException(
        {stringValue("invalid"), stringValue("message")});
    require(!invalidIdentifier.succeeded,
            "invalid exception identifier was accepted");
    const auto unusedReplacement = mparser::runtimeCreateErrorException(
        {stringValue("plain message"), numberValue(1)});
    require(!unusedReplacement.succeeded,
            "unused exception replacement was accepted");
    const auto unthrownRethrow = mparser::runtimePrepareExceptionForThrow(
        structured.value, {}, true);
    require(!unthrownRethrow.succeeded,
            "rethrow accepted an exception without a prior throw stack");
}

} // namespace

int main() {
    try {
        runCaughtParitySmoke();
        runUncaughtParitySmoke();
        runSharedContractSmoke();
        std::cout << "exception runtime smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Exception runtime smoke failure: " << error.what()
                  << '\n';
        return 1;
    }
}
