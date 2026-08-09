#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_exception.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_struct.h"
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

const mparser::RuntimeValue& requiredStructField(
    const mparser::RuntimeValue& structure, std::string_view name) {
    const auto* value = mparser::runtimeStructField(structure, name);
    require(value != nullptr,
            "missing structure field: " + std::string(name));
    return *value;
}

template <typename Result>
const mparser::RuntimeValue& requiredVariable(
    const Result& result, std::string_view name) {
    const auto* value = findVariable(result, name);
    require(value != nullptr,
            "missing runtime variable: " + std::string(name));
    return *value;
}

mparser::RuntimeValue stringValue(std::string text) {
    return mparser::makeRuntimeCharacterVectorUtf8(text);
}

std::string textValue(const mparser::RuntimeValue& value) {
    return mparser::runtimeTextScalarUtf8(value).value_or(
        std::string("<non-text>"));
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
    require(textValue(identifier) == "MParserDemo:BadValue",
            "caught exception identifier mismatch");
    require(textValue(message) == "Value 7 for item",
            "caught exception message mismatch");
    require(stack.kind == mparser::RuntimeValueKind::Struct,
            "caught exception stack is not a structure");
    require(textValue(requiredStructField(stack, "file")) ==
                "exception_runtime_smoke.m",
            "caught exception source file mismatch");
    require(textValue(requiredStructField(stack, "name")) ==
                "exception_runtime_demo",
            "caught exception function name mismatch");
    require(requiredStructField(stack, "line").number == 3.0,
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
    require(requiredStructField(requiredProperty(*thrown, "stack"),
                                "line")
                .number ==
                requiredStructField(requiredProperty(*reraised, "stack"),
                                    "line")
                    .number,
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

const std::string kNestedStackSource = R"(function value = stack_contract_demo()
value = outer_stack();
end
function value = outer_stack()
value = inner_stack();
end
function value = inner_stack()
error("Stack:Nested", "nested failure");
value = 0;
end
)";

template <typename Result>
void verifyNestedStack(const Result& result) {
    require(result.diagnostics.size() == 1,
            "nested failure did not produce one diagnostic");
    const auto& diagnostic = result.diagnostics.front();
    require(diagnostic.identifier == "Stack:Nested" &&
                diagnostic.message == "nested failure" &&
                diagnostic.severity ==
                    mparser::DiagnosticSeverity::Error,
            "nested failure diagnostic metadata mismatch");
    require(diagnostic.stack.size() == 3,
            "nested failure did not retain three stack frames");
    require(diagnostic.stack[0].name == "inner_stack" &&
                diagnostic.stack[0].line == 8 &&
                diagnostic.stack[1].name == "outer_stack" &&
                diagnostic.stack[1].line == 5 &&
                diagnostic.stack[2].name == "stack_contract_demo" &&
                diagnostic.stack[2].line == 2,
            "nested failure stack order or call-site lines mismatch");
    for (const auto& frame : diagnostic.stack) {
        require(frame.file == "nested_stack_contract.m",
                "nested failure stack source file mismatch");
    }
}

void runNestedStackParitySmoke() {
    verifyNestedStack(runInterpreter(
        kNestedStackSource, "nested_stack_contract.m"));
    verifyNestedStack(runBytecode(
        kNestedStackSource, "nested_stack_contract.m"));
}

const std::string kCauseSource = R"(function report_ok = cause_contract_demo()
try
    leaf_cause();
catch root
    wrapped = MException("Cause:Wrapper", "wrapper failure");
    wrapped = wrapped.addCause(root);
    try
        throw(wrapped);
    catch captured
        basic_report = captured.getReport("basic");
        extended_report = captured.getReport("extended", "hyperlinks", "off");
        report_ok = strcmp(basic_report, "wrapper failure");
    end
end
end
function leaf_cause()
error("Cause:Root", "root failure");
end
)";

template <typename Result>
void verifyCauseContract(const Result& result) {
    require(result.diagnostics.empty(),
            "caught cause chain escaped the runtime");
    const auto& captured = requiredVariable(result, "captured");
    require(mparser::isRuntimeException(captured) &&
                mparser::runtimeExceptionCauseCount(captured) == 1,
            "MException cause chain was not retained");
    const auto& causes = requiredProperty(captured, "cause");
    require(causes.kind == mparser::RuntimeValueKind::Cell &&
                causes.cells.size() == 1 &&
                mparser::isRuntimeException(causes.cells.front()),
            "public MException cause property mismatch");
    const auto& causeIdentifier =
        requiredProperty(causes.cells.front(), "identifier");
    const auto& causeMessage =
        requiredProperty(causes.cells.front(), "message");
    require(textValue(causeIdentifier) == "Cause:Root" &&
                textValue(causeMessage) == "root failure",
            "nested MException cause metadata mismatch: id=" +
                textValue(causeIdentifier) + ", message=" +
                textValue(causeMessage));
    const auto& basic = requiredVariable(result, "basic_report");
    const auto& extended = requiredVariable(result, "extended_report");
    require(textValue(basic) == "wrapper failure",
            "basic exception report mismatch");
    const std::string extendedText = textValue(extended);
    require(extendedText.find("Cause:Wrapper: wrapper failure") !=
                    std::string::npos &&
                extendedText.find("Cause:Root: root failure") !=
                    std::string::npos &&
                extendedText.find("Caused by:") != std::string::npos,
            "extended exception report omitted stack or cause details");
}

void runCauseParitySmoke() {
    verifyCauseContract(runInterpreter(
        kCauseSource, "cause_contract.m"));
    verifyCauseContract(runBytecode(
        kCauseSource, "cause_contract.m"));
}

const std::string kUncaughtCauseSource = R"(function uncaught_cause_demo()
try
    error("Cause:Root", "root failure");
catch root
    middle = MException("Cause:Middle", "middle failure");
    middle = middle.addCause(root);
    outer = MException("Cause:Outer", "outer failure");
    outer = outer.addCause(middle);
    throw(outer);
end
end
)";

template <typename Result>
void verifyUncaughtDiagnosticCauses(const Result& result) {
    require(result.diagnostics.size() == 1 &&
                mparser::hasErrorDiagnostics(result.diagnostics),
            "uncaught cause chain did not produce one error diagnostic");
    const auto& diagnostic = result.diagnostics.front();
    require(diagnostic.identifier == "Cause:Outer" &&
                diagnostic.message == "outer failure" &&
                diagnostic.causes.size() == 1,
            "outer diagnostic cause metadata mismatch");
    const auto& middle = diagnostic.causes.front();
    require(middle.identifier == "Cause:Middle" &&
                middle.message == "middle failure" &&
                middle.causes.size() == 1,
            "middle diagnostic cause metadata mismatch");
    const auto& root = middle.causes.front();
    require(root.identifier == "Cause:Root" &&
                root.message == "root failure" &&
                root.stack.size() == 1 &&
                root.stack.front().name == "uncaught_cause_demo" &&
                root.stack.front().line == 3,
            "nested diagnostic cause stack mismatch");
}

void runUncaughtCauseParitySmoke() {
    verifyUncaughtDiagnosticCauses(runInterpreter(
        kUncaughtCauseSource, "uncaught_cause_contract.m"));
    verifyUncaughtDiagnosticCauses(runBytecode(
        kUncaughtCauseSource, "uncaught_cause_contract.m"));
}

const std::string kThrowPolicySource = R"(function ok = throw_policy_demo()
manual = MException("Policy:Manual", "manual failure");
try
    do_throw(manual);
catch thrown
end
try
    do_rethrow();
catch reraised
end
try
    do_throw_as_caller(manual);
catch as_caller
end
ok = 1;
end
function do_throw(exception)
throw(exception);
end
function do_rethrow()
try
    leaf_rethrow();
catch caught
    rethrow(caught);
end
end
function leaf_rethrow()
error("Policy:Leaf", "leaf failure");
end
function do_throw_as_caller(exception)
throwAsCaller(exception);
end
)";

template <typename Result>
void verifyThrowPolicies(const Result& result) {
    require(result.diagnostics.empty(),
            "caught throw-policy exception escaped the runtime");
    const auto throwFrames = mparser::runtimeExceptionFrames(
        requiredVariable(result, "thrown"));
    const auto rethrowFrames = mparser::runtimeExceptionFrames(
        requiredVariable(result, "reraised"));
    const auto callerFrames = mparser::runtimeExceptionFrames(
        requiredVariable(result, "as_caller"));
    require(throwFrames.size() == 2 &&
                throwFrames[0].name == "do_throw" &&
                throwFrames[1].name == "throw_policy_demo",
            "throw did not replace the stack at its throw site");
    require(rethrowFrames.size() == 3 &&
                rethrowFrames[0].name == "leaf_rethrow" &&
                rethrowFrames[1].name == "do_rethrow" &&
                rethrowFrames[2].name == "throw_policy_demo",
            "rethrow did not preserve the original stack");
    require(callerFrames.size() == 1 &&
                callerFrames[0].name == "throw_policy_demo",
            "throwAsCaller did not remove its own stack frame");
}

void runThrowPolicyParitySmoke() {
    verifyThrowPolicies(runInterpreter(
        kThrowPolicySource, "throw_policy_contract.m"));
    verifyThrowPolicies(runBytecode(
        kThrowPolicySource, "throw_policy_contract.m"));
}

const std::string kWarningSource = R"(function ok = warning_contract_demo()
warning("Warn:Visible", "visible %d", 1);
[visible_message, visible_id] = lastwarn();
saved = warning("off", "Warn:Muted");
warning("Warn:Muted", "muted message");
[muted_message, muted_id] = lastwarn();
muted_state = warning("query", "Warn:Muted");
warning(saved);
warning("Warn:Muted", "restored message");
[restored_message, restored_id] = lastwarn();
[manual_message, manual_id] = lastwarn("manual message", "Warn:Manual");
lastwarn("stable message", "Warn:Stable");
try
    lastwarn("corrupt message", "InvalidIdentifier");
catch invalid_lastwarn
end
[stable_message, stable_id] = lastwarn();
backtrace_saved = warning("off", "backtrace");
backtrace_off = warning("query", "backtrace");
warning(backtrace_saved);
backtrace_on = warning("query", "backtrace");
verbose_saved = warning("on", "verbose");
verbose_on = warning("query", "verbose");
warning(verbose_saved);
verbose_off = warning("query", "verbose");
all_settings = warning();
warning("Warn:Reset", "");
[reset_message, reset_id] = lastwarn();
ok = strcmp(visible_message, "visible 1") && ...
     strcmp(visible_id, "Warn:Visible") && ...
     strcmp(muted_message, "muted message") && ...
     strcmp(muted_id, "Warn:Muted") && ...
     strcmp(restored_message, "restored message") && ...
     strcmp(restored_id, "Warn:Muted") && ...
     strcmp(manual_message, "manual message") && ...
     strcmp(manual_id, "Warn:Manual") && ...
     strcmp(stable_message, "stable message") && ...
     strcmp(stable_id, "Warn:Stable") && ...
     strcmp(backtrace_off.state, "off") && ...
     strcmp(backtrace_on.state, "on") && ...
     strcmp(verbose_on.state, "on") && ...
     strcmp(verbose_off.state, "off") && ...
     numel(all_settings) == 1 && ...
     strcmp(reset_message, "") && strcmp(reset_id, "");
end
)";

template <typename Result>
void verifyWarningContract(const Result& result) {
    require(!mparser::hasErrorDiagnostics(result.diagnostics),
            "warning-only script produced an error diagnostic");
    require(result.diagnostics.size() == 2,
            "warning suppression did not filter emitted diagnostics");
    require(result.diagnostics[0].severity ==
                mparser::DiagnosticSeverity::Warning &&
                result.diagnostics[0].identifier == "Warn:Visible" &&
                result.diagnostics[0].message == "visible 1" &&
                result.diagnostics[1].severity ==
                    mparser::DiagnosticSeverity::Warning &&
                result.diagnostics[1].identifier == "Warn:Muted" &&
                result.diagnostics[1].message == "restored message",
            "warning diagnostic metadata mismatch");
    require(result.diagnostics[0].stack.size() == 1 &&
                result.diagnostics[0].stack[0].name ==
                    "warning_contract_demo",
            "warning diagnostic did not retain its source frame");
    const auto& ok = requiredVariable(result, "ok");
    require(ok.kind == mparser::RuntimeValueKind::Number &&
                ok.number == 1.0,
            "lastwarn getter/setter contract mismatch");
    const auto& mutedState = requiredVariable(result, "muted_state");
    require(textValue(requiredStructField(mutedState, "identifier")) ==
                "Warn:Muted" &&
                textValue(requiredStructField(mutedState, "state")) == "off",
            "warning query state mismatch");
}

void runWarningParitySmoke() {
    verifyWarningContract(runInterpreter(
        kWarningSource, "warning_contract.m"));
    verifyWarningContract(runBytecode(
        kWarningSource, "warning_contract.m"));
}

const std::string kAssertSource = R"(function ok = assert_contract_demo()
assert(1);
try
    assert(0);
catch defaulted
end
try
    assert(0, "Assert:Custom", "bad %d", 7);
catch custom
end
try
    invalid_output = assert(0);
catch output_error
end
try
    [first_exception, second_exception] = ...
        MException("Constructor:Arity", "invalid arity");
catch constructor_output_error
end
exception = MException("Correction:Boundary", "boundary");
correction = exception.Correction;
try
    exception = exception.addCorrection(1);
catch correction_error
end
ok = 1;
end
)";

template <typename Result>
void verifyAssertAndCorrection(const Result& result) {
    require(result.diagnostics.empty(),
            "caught assertion or correction error escaped the runtime");
    const auto& defaulted = requiredVariable(result, "defaulted");
    const auto& custom = requiredVariable(result, "custom");
    const auto& outputError = requiredVariable(result, "output_error");
    const auto& constructorOutputError =
        requiredVariable(result, "constructor_output_error");
    const auto& correction = requiredVariable(result, "correction");
    const auto& correctionError =
        requiredVariable(result, "correction_error");
    require(textValue(requiredProperty(defaulted, "identifier")) ==
                "MParser:AssertionFailed" &&
                textValue(requiredProperty(defaulted, "message")) ==
                    "Assertion failed.",
            "default assert exception mismatch");
    require(textValue(requiredProperty(custom, "identifier")) ==
                "Assert:Custom" &&
                textValue(requiredProperty(custom, "message")) == "bad 7",
            "custom assert exception mismatch");
    require(textValue(requiredProperty(outputError, "identifier")) ==
                "MParser:InvalidAssertion",
            "assert output-arity exception mismatch");
    require(textValue(requiredProperty(constructorOutputError, "identifier")) ==
                "MParser:InvalidException",
            "MException output-arity exception mismatch");
    require(correction.kind == mparser::RuntimeValueKind::Missing,
            "MException Correction placeholder mismatch");
    require(textValue(requiredProperty(correctionError, "identifier")) ==
                "MParser:UnsupportedExceptionCorrection",
            "unsupported correction boundary diagnostic mismatch");
}

void runAssertAndCorrectionParitySmoke() {
    verifyAssertAndCorrection(runInterpreter(
        kAssertSource, "assert_contract.m"));
    verifyAssertAndCorrection(runBytecode(
        kAssertSource, "assert_contract.m"));
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
                textValue(requiredProperty(formatted.value, "message")) ==
                    "7 2.2 % Z",
            "scalar exception formatting mismatch");

    mparser::RuntimeNumericElementValue exactInteger;
    exactInteger.numericClass = mparser::RuntimeNumericClass::UInt64;
    exactInteger.integerRealBits = 9007199254740993ULL;
    exactInteger.real = 9007199254740992.0;
    auto exactValue = mparser::runtimeNumericValueFromElements(
        {1, 1}, {exactInteger}, exactInteger.numericClass);
    require(exactValue.has_value(),
            "exact integer exception value construction failed");
    mparser::RuntimeNumericElementValue complexElement;
    complexElement.real = 1.0;
    complexElement.imaginary = 2.0;
    complexElement.complex = true;
    auto complexValue = mparser::runtimeNumericValueFromElements(
        {1, 1}, {complexElement}, complexElement.numericClass);
    require(complexValue.has_value(),
            "complex exception value construction failed");
    const auto numericFormatted = mparser::runtimeConstructMException(
        {stringValue("Format:NumericChannels"),
         stringValue("%d %g"), *exactValue, *complexValue});
    require(numericFormatted.succeeded &&
                textValue(requiredProperty(
                    numericFormatted.value, "message")) ==
                    "9007199254740993 1",
            "numeric-channel exception formatting mismatch");

    auto errorStruct = mparser::makeRuntimeStructValue();
    mparser::runtimeSetStructField(
        errorStruct, "identifier", stringValue("Struct:Failure"));
    mparser::runtimeSetStructField(
        errorStruct, "message", stringValue("structured failure"));
    const auto structured =
        mparser::runtimeCreateErrorException({errorStruct});
    require(structured.succeeded &&
                textValue(requiredProperty(structured.value, "identifier")) ==
                    "Struct:Failure" &&
                textValue(requiredProperty(structured.value, "message")) ==
                    "structured failure",
            "error structure construction mismatch");

    mparser::Diagnostic embedded;
    embedded.identifier = "Embedded:Outer";
    embedded.message = "outer diagnostic";
    embedded.stack.push_back({"outer.m", "outer", 9});
    mparser::DiagnosticCause embeddedCause;
    embeddedCause.identifier = "Embedded:Cause";
    embeddedCause.message = "cause diagnostic";
    embeddedCause.stack.push_back({"cause.m", "cause", 4});
    embedded.causes.push_back(std::move(embeddedCause));
    const auto embeddedException =
        mparser::runtimeExceptionFromDiagnostic(embedded, {});
    const auto embeddedRoundTrip =
        mparser::runtimeDiagnosticFromException(embeddedException, {});
    require(mparser::runtimeExceptionFrameCount(embeddedException) == 1 &&
                mparser::runtimeExceptionCauseCount(embeddedException) == 1 &&
                embeddedRoundTrip.stack.size() == 1 &&
                embeddedRoundTrip.stack.front().file == "outer.m" &&
                embeddedRoundTrip.causes.size() == 1 &&
                embeddedRoundTrip.causes.front().identifier ==
                    "Embedded:Cause" &&
                embeddedRoundTrip.causes.front().stack.size() == 1,
            "embedding diagnostic exception round trip lost stack or cause");

    const auto invalidIdentifier = mparser::runtimeConstructMException(
        {stringValue("invalid"), stringValue("message")});
    require(!invalidIdentifier.succeeded,
            "invalid exception identifier was accepted");
    const auto unusedReplacement = mparser::runtimeCreateErrorException(
        {stringValue("plain message"), numberValue(1)});
    require(!unusedReplacement.succeeded,
            "unused exception replacement was accepted");
    const auto unthrownRethrow = mparser::runtimePrepareExceptionForThrow(
        structured.value, {},
        mparser::RuntimeExceptionStackPolicy::Preserve);
    require(!unthrownRethrow.succeeded,
            "rethrow accepted an exception without a prior throw stack");
}

} // namespace

int main() {
    try {
        runCaughtParitySmoke();
        runUncaughtParitySmoke();
        runNestedStackParitySmoke();
        runCauseParitySmoke();
        runUncaughtCauseParitySmoke();
        runThrowPolicyParitySmoke();
        runWarningParitySmoke();
        runAssertAndCorrectionParitySmoke();
        runSharedContractSmoke();
        std::cout << "exception runtime smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Exception runtime smoke failure: " << error.what()
                  << '\n';
        return 1;
    }
}
