#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_object.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_source_evaluation.h"
#include "mparser/runtime_session_state.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_system.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

RuntimePair runBoth(
    std::string_view source,
    mparser::RuntimeSystemCapability capabilities =
        mparser::RuntimeSystemCapability::DynamicEvaluation) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "dynamic-workspace source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "dynamic-workspace source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "dynamic-workspace source did not lower");

    const auto runtimeSession = [capabilities] {
        mparser::RuntimeSystemContextOptions options;
        options.capabilities = capabilities;
        return std::make_shared<mparser::RuntimeSessionState>(
            std::make_shared<mparser::RuntimeSystemContext>(
                std::move(options)));
    };

    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = runtimeSession();
    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic, interpreterOptions);
    mparser::BytecodeVmOptions vmOptions;
    vmOptions.sessionState = runtimeSession();
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic, vmOptions);
    return {std::move(interpreted), std::move(executed)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing dynamic-workspace variable: " +
                             std::string(name));
}

template <typename Result>
bool hasVariable(const Result& result, std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return true;
        }
    }
    return false;
}

template <typename Result>
void requireNumber(const Result& result, std::string_view name,
                   double expected) {
    const auto& value = variable(result, name);
    const auto numeric = mparser::runtimeNumericElement(value, 0);
    if (!numeric || *numeric != expected) {
        throw std::runtime_error(
            "dynamic-workspace numeric value mismatch for " +
            std::string(name) + ": expected " +
            std::to_string(expected) + ", received " +
            (numeric ? std::to_string(*numeric) : std::string("nonnumeric")));
    }
}

template <typename Result>
void verifyResult(const Result& result) {
    if (!result.diagnostics.empty()) {
        std::string message = "dynamic-workspace runtime diagnostic: ";
        message += result.diagnostics.front().identifier;
        message += " ";
        message += result.diagnostics.front().message;
        message += " at line ";
        message += std::to_string(
            result.diagnostics.front().span.begin.line);
        throw std::runtime_error(message);
    }

    requireNumber(result, "value", 7.0);
    requireNumber(result, "nested", 5.0);
    requireNumber(result, "string_value", 7.0);
    requireNumber(result, "rows", 2.0);
    requireNumber(result, "columns", 3.0);
    requireNumber(result, "evalc_rows", 2.0);
    requireNumber(result, "evalc_columns", 3.0);
    requireNumber(result, "base_rows", 4.0);
    requireNumber(result, "base_columns", 5.0);
    requireNumber(result, "shadowed_builtin", 20.0);
    requireNumber(result, "shadowed_builtin_end", 30.0);
    requireNumber(result, "shadowed_builtin_all", 60.0);
    requireNumber(result, "runtime_shadowed_end", 50.0);
    requireNumber(result, "runtime_shadowed_all", 90.0);
    requireNumber(result, "runtime_shadowed_nested", 40.0);
    requireNumber(result, "nested_builtin_end", 50.0);
    requireNumber(result, "nested_shadow_end", 70.0);
    requireNumber(result, "caught", 10.0);
    requireNumber(result, "partial", 9.0);
    requireNumber(result, "syntax_caught", 41.0);
    requireNumber(result, "diagnostic_location_caught", 1.0);
    requireNumber(result, "internal_name", 100.0);
    requireNumber(result, "internal_name_result", 2.0);
    requireNumber(result, "caller_value", 42.0);
    requireNumber(result, "local_value", 15.0);
    requireNumber(result, "base_value", 77.0);
    requireNumber(result, "eval_caller_read", 30.0);
    requireNumber(result, "eval_caller_after", 31.0);

    const auto& dynamicWarningState =
        variable(result, "dynamic_warning_state");
    const auto* dynamicWarningStateText =
        mparser::runtimeStructField(dynamicWarningState, "state");
    require(dynamicWarningStateText &&
                mparser::runtimeTextScalarUtf8(*dynamicWarningStateText) ==
                    std::optional<std::string>{"on"},
            "nested eval did not retain warning state in its session");
    require(mparser::runtimeTextScalarUtf8(
                variable(result, "dynamic_warning_message")) ==
                std::optional<std::string>{"nested warning"} &&
                mparser::runtimeTextScalarUtf8(
                    variable(result, "dynamic_warning_identifier")) ==
                std::optional<std::string>{"Dynamic:Nested"},
            "nested eval did not retain lastwarn state in its session");

    const auto& assigned = variable(result, "assigned_matrix");
    require(mparser::runtimeDimensions(assigned) ==
                std::vector<size_t>({2, 3}),
            "eval did not preserve assigned matrix shape");
    const auto assignedLast =
        mparser::runtimeNumericElement(assigned, 5);
    require(assignedLast && *assignedLast == 99.0,
            "eval did not preserve indexed assignment");

    const auto& missingRow = variable(result, "missing_row");
    require(missingRow.kind == mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(missingRow) ==
                    std::vector<size_t>({1, 2}),
            "assignin/evalin did not preserve a missing row");

    const auto captured =
        mparser::runtimeTextScalarUtf8(variable(result, "captured"));
    require(captured && captured->find('7') != std::string::npos,
            "evalc did not capture disp output");
    const auto expressionCapture =
        mparser::runtimeTextScalarUtf8(
            variable(result, "expression_capture"));
    require(expressionCapture &&
                expressionCapture->find("ans =") != std::string::npos &&
                expressionCapture->find('5') != std::string::npos,
            "evalc did not capture an unsuppressed expression");
    const auto quiet =
        mparser::runtimeTextScalarUtf8(variable(result, "quiet"));
    require(quiet && quiet->empty(),
            "evalc multi-output expression unexpectedly printed text");
    const auto recoveredCapture =
        mparser::runtimeTextScalarUtf8(
            variable(result, "recovered_capture"));
    require(recoveredCapture &&
                recoveredCapture->find('1') != std::string::npos &&
                recoveredCapture->find('2') != std::string::npos,
            "evalc catch expression did not retain ordered output");
    const auto implicitCapture =
        mparser::runtimeTextScalarUtf8(
            variable(result, "implicit_capture"));
    require(implicitCapture &&
                implicitCapture->find("13") != std::string::npos,
            "suppressed evalc did not assign its implicit output to ans");

    requireNumber(result, "bad_scope_caught", 1.0);
    requireNumber(result, "bad_name_caught", 1.0);
    requireNumber(result, "handle_escape_caught", 1.0);
    requireNumber(result, "nested_handle_escape_caught", 1.0);
    requireNumber(result, "declaration_caught", 1.0);
    requireNumber(result, "caller_handle_escape_caught", 1.0);
    requireNumber(result, "escaped_caller_exists", 0.0);
    require(!hasVariable(result, "escaped"),
            "dynamic anonymous handle escaped its module");
    require(!hasVariable(result, "nested_escaped"),
            "nested dynamic anonymous handle escaped its module");
    requireNumber(result, "preserved_handle_result", 5.0);
    requireNumber(result, "temp_exists", 0.0);

    std::string emitted;
    for (const auto& event : result.outputEvents) {
        emitted += event.text;
    }
    require(emitted.find('8') != std::string::npos,
            "eval did not forward ordinary output");
    require(emitted.find('7') == std::string::npos,
            "evalc leaked captured output");
}

void verifySharedObjectHandleRollback() {
    auto registry = mparser::createBuiltinRegistryWithDefaults();
    mparser::BuiltinDescriptor stash;
    stash.name = "stash_handle";
    stash.inputs = mparser::BuiltinArity::fixed(2);
    stash.outputs = mparser::BuiltinArity::fixed(0);
    stash.implementation =
        mparser::BuiltinImplementationKind::Shared;
    stash.purity = mparser::BuiltinPurity::Impure;
    stash.sideEffects = mparser::BuiltinSideEffect::ObjectState;
    stash.handler = [](const mparser::BuiltinCall& call) {
        if (!call.arguments[0].sharedFields) {
            return mparser::BuiltinResult::failure(
                call.span, "stash_handle requires a shared object",
                "DynamicTest:InvalidObject");
        }
        (*call.arguments[0].sharedFields)["Callback"] =
            call.arguments[1];
        return mparser::BuiltinResult::success();
    };
    const auto registration = registry->registerBuiltin(std::move(stash));
    require(registration.succeeded,
            "shared-object test builtin registration failed");
    registry->freeze();

    auto box = mparser::makeRuntimeObjectScalar(
        "HandleBox",
        {{"Callback", mparser::makeRuntimeNumberValue(7)}}, true);
    const auto fields = box.sharedFields;
    mparser::RuntimeWorkspace workspace{{"box", std::move(box)}};

    mparser::RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities =
        mparser::RuntimeSystemCapability::DynamicEvaluation;
    mparser::RuntimeSourceEvaluationOptions options;
    options.builtinRegistry = registry;
    options.sessionState =
        std::make_shared<mparser::RuntimeSessionState>(
            std::make_shared<mparser::RuntimeSystemContext>(
                std::move(systemOptions)));

    mparser::BuiltinSourceEvaluationRequest request;
    request.source =
        "stash_handle(box, @(x) x + 1); clear box;";
    const auto result = mparser::evaluateRuntimeSource(
        request, workspace, options);
    require(!result.succeeded,
            "dynamic handle escaped through a cleared shared object");
    bool foundEscape = false;
    for (const auto& diagnostic : result.diagnostics) {
        foundEscape = foundEscape ||
            diagnostic.identifier ==
                "MParser:DynamicEvaluationHandleEscape";
    }
    require(foundEscape,
            "shared-object handle escape diagnostic is missing");
    require(!workspace.contains("box"),
            "dynamic clear side effect was unexpectedly rolled back");
    require(fields && fields->contains("Callback") &&
                fields->at("Callback").kind ==
                    mparser::RuntimeValueKind::Number &&
                fields->at("Callback").number == 7,
            "shared object retained a temporary-module handle");
}

void verifyTypedDynamicShadowFallback() {
    mparser::RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities =
        mparser::RuntimeSystemCapability::DynamicEvaluation;
    mparser::RuntimeSourceEvaluationOptions options;
    options.sessionState =
        std::make_shared<mparser::RuntimeSessionState>(
            std::make_shared<mparser::RuntimeSystemContext>(
                std::move(systemOptions)));
    options.typedRegionBackend =
        mparser::TypedRegionBackend::Portable;
    options.enableTypedRegions = true;

    mparser::RuntimeWorkspace workspace{
        {"sin", mparser::makeRuntimeVectorValue({10, 20, 30})}};
    mparser::BuiltinSourceEvaluationRequest request;
    request.source = R"(total = 0;
for index = 1:3
    total = total + sin(index);
end
)";
    const auto result = mparser::evaluateRuntimeSource(
        request, workspace, options);
    require(result.succeeded && result.diagnostics.empty(),
            "typed dynamic source did not use guarded fallback");
    const auto total = workspace.find("total");
    require(total != workspace.end() &&
                total->second.kind ==
                    mparser::RuntimeValueKind::Number &&
                total->second.number == 60,
            "typed dynamic source ignored runtime callable shadowing");
}

} // namespace

int main() {
    try {
        constexpr std::string_view source = R"MATLAB(
seed = 5;
value = eval('seed + 2');
nested = eval('eval(''2 + 3'')');
string_value = eval("6 + 1");
[rows, columns] = eval('size(ones(2, 3))');

captured = evalc('disp(7)');
expression_capture = evalc('2 + 3');
[quiet, evalc_rows, evalc_columns] = ...
    evalc('size(ones(2, 3))');
recovered_capture = evalc( ...
    'disp(1); error(''Dynamic:Probe'', ''boom'')', ...
    'disp(2)');
evalc('disp(13)');
implicit_capture = ans;

eval('assigned_matrix = reshape(1:6, 2, 3);');
eval('assigned_matrix(2, 3) = 99;');
assignin('base', 'missing_row', [missing missing]);
missing_row = evalin('base', 'missing_row');
[base_rows, base_columns] = ...
    evalin('base', 'size(ones(4, 5))');

sin = [10 20 30];
shadowed_builtin = eval('sin(2)');
shadowed_builtin_end = eval('sin(end)');
shadowed_builtin_all = eval('sum(sin(:))');

partial = 1;
try
    eval('partial = 9; error(''Dynamic:Failure'', ''boom'')');
catch
end
caught = eval( ...
    'error(''Dynamic:Failure'', ''boom'')', 'partial + 1');
syntax_caught = eval('1 +', '41');

diagnostic_location_caught = false;
try
    location_output = eval('unknown_dynamic_variable');
catch err
    diagnostic_location_caught = ~isempty(strfind( ...
        err.message, 'dynamic source line 1, column 1:'));
end

__mparser_eval_output_0_0 = 100;
internal_name_result = eval('1 + 1');
internal_name = __mparser_eval_output_0_0;

bad_scope_caught = false;
try
    evalin('current', '1');
catch err
    bad_scope_caught = ...
        strcmp(err.identifier, 'MParser:InvalidWorkspaceScope');
end

bad_name_caught = false;
try
    assignin('base', 'not valid', 1);
catch err
    bad_name_caught = ...
        strcmp(err.identifier, 'MParser:InvalidWorkspaceVariableName');
end

handle_escape_caught = false;
try
    eval('escaped = @(x) x + 1;');
catch err
    handle_escape_caught = ...
        strcmp(err.identifier, 'MParser:DynamicEvaluationHandleEscape');
end

nested_handle_escape_caught = false;
try
    eval('nested_escaped = {@(x) x + 1};');
catch err
    nested_handle_escape_caught = ...
        strcmp(err.identifier, 'MParser:DynamicEvaluationHandleEscape');
end

existing_handle = @parent_increment;
eval('preserved_handle = existing_handle;');
preserved_handle_result = preserved_handle(4);

declaration_caught = false;
try
    eval('global dynamic_global');
catch err
    declaration_caught = ...
        strcmp(err.identifier, 'MParser:UnsupportedDynamicDeclaration');
end

eval('temp_to_clear = 1; clear temp_to_clear;');
temp_exists = exist('temp_to_clear', 'var');
eval('disp(8)');

warning('off', 'Dynamic:Nested');
eval(['warning(''on'', ''Dynamic:Nested''); ' ...
      'lastwarn(''nested warning'', ''Dynamic:Nested'');']);
dynamic_warning_state = warning('query', 'Dynamic:Nested');
[dynamic_warning_message, dynamic_warning_identifier] = lastwarn();

scope_marker = 30;
[caller_value, local_value, eval_caller_read] = caller_roundtrip();
eval_caller_after = scope_marker;
base_value = base_roundtrip();
[runtime_shadowed_end, runtime_shadowed_all, runtime_shadowed_nested] = ...
    runtime_shadow_roundtrip();
[nested_builtin_end, nested_shadow_end] = nested_end_roundtrip();
caller_handle_escape_caught = caller_handle_escape();
escaped_caller_exists = exist('escaped_caller', 'var');

function [caller_result, local_result, eval_caller_read] = caller_roundtrip()
    local_number = 10;
    eval('local_number = local_number + 2;');
    local_result = eval('local_number + 3');
    eval_caller_read = eval( ...
        'evalin(''caller'', ''scope_marker'')');
    eval('assignin(''caller'', ''scope_marker'', 31)');
    assign_to_caller();
    caller_result = read_from_caller();
end

function assign_to_caller()
    assignin('caller', 'caller_number', 41);
end

function value = read_from_caller()
    value = evalin('caller', 'caller_number + 1');
end

function value = base_roundtrip()
    assignin('base', 'base_dynamic', 77);
    value = evalin('base', 'base_dynamic');
end

function [last, total, nested] = runtime_shadow_roundtrip()
    inject_runtime_shadow();
    last = cos(end);
    total = sum(cos(:));
    matrix = [10 20; 30 40];
    nested = matrix(cos(2) / 25, end);
end

function inject_runtime_shadow()
    assignin('caller', 'cos', [40 50]);
end

function [builtin_result, shadow_result] = nested_end_roundtrip()
    values = 10:10:100;
    builtin_result = values(min(end, 5));
    inject_nested_shadow();
    shadow_result = values(min(end));
end

function inject_nested_shadow()
    assignin('caller', 'min', [2 4 7]);
end

function caught = caller_handle_escape()
    caught = false;
    try
        eval(['assignin(''caller'', ''escaped_caller'', ' ...
            '@(x) x + 1)']);
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:DynamicEvaluationHandleEscape');
    end
end

function value = parent_increment(input)
    value = input + 1;
end
)MATLAB";

        const RuntimePair pair = runBoth(source);
        verifyResult(pair.interpreter);
        verifyResult(pair.vm);

        constexpr std::string_view deniedSource = R"MATLAB(
denied = false;
try
    eval('1 + 1');
catch err
    denied = strcmp(err.identifier, 'MParser:SystemCapabilityDenied');
end
)MATLAB";
        const RuntimePair denied = runBoth(
            deniedSource, mparser::RuntimeSystemCapability::None);
        require(denied.interpreter.diagnostics.empty(),
                "interpreter leaked a denied dynamic-evaluation diagnostic");
        require(denied.vm.diagnostics.empty(),
                "VM leaked a denied dynamic-evaluation diagnostic");
        requireNumber(denied.interpreter, "denied", 1.0);
        requireNumber(denied.vm, "denied", 1.0);
        verifySharedObjectHandleRollback();
        verifyTypedDynamicShadowFallback();
        std::cout << "dynamic workspace smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
