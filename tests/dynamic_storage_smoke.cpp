#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/diagnostic.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_session_state.h"
#include "mparser/runtime/io/runtime_system.h"
#include "mparser/semantic/semantic.h"

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

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "dynamic-storage source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "dynamic-storage source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "dynamic-storage source did not lower");

    const auto makeSession = [] {
        mparser::RuntimeSystemContextOptions options;
        options.capabilities =
            mparser::RuntimeSystemCapability::DynamicEvaluation;
        return std::make_shared<mparser::RuntimeSessionState>(
            std::make_shared<mparser::RuntimeSystemContext>(
                std::move(options)));
    };

    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = makeSession();
    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic, interpreterOptions);

    mparser::BytecodeVmOptions vmOptions;
    vmOptions.sessionState = makeSession();
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
    throw std::runtime_error("missing dynamic-storage variable: " +
                             std::string(name));
}

template <typename Result>
void requireNumber(const Result& result, std::string_view name,
                   double expected) {
    const auto numeric =
        mparser::runtimeNumericElement(variable(result, name), 0);
    if (!numeric || *numeric != expected) {
        throw std::runtime_error(
            "dynamic-storage numeric mismatch for " +
            std::string(name) + ": expected " +
            std::to_string(expected) + ", received " +
            (numeric ? std::to_string(*numeric)
                     : std::string("nonnumeric")));
    }
}

template <typename Result>
void verifyResult(const Result& result) {
    if (mparser::hasErrorDiagnostics(result.diagnostics)) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error(
            "dynamic-storage runtime diagnostic: " +
            diagnostic.identifier + " " + diagnostic.message);
    }

    size_t declarationWarnings = 0;
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier ==
            "MParser:GlobalDeclarationAfterUse") {
            ++declarationWarnings;
        }
    }
    require(declarationWarnings == 1,
            "dynamic global-after-local warning count differs");

    requireNumber(result, "global_first", 11.0);
    requireNumber(result, "global_second", 22.0);
    requireNumber(result, "global_stored", 22.0);
    requireNumber(result, "persistent_first", 11.0);
    requireNumber(result, "persistent_second", 22.0);
    requireNumber(result, "caller_persistent_first", 11.0);
    requireNumber(result, "caller_persistent_second", 22.0);
    requireNumber(result, "caller_global_value", 15.0);
    requireNumber(result, "caller_global_stored", 15.0);
    requireNumber(result, "base_global_value", 12.0);
    requireNumber(result, "base_global_stored", 12.0);
    requireNumber(result, "nested_eval_first", 11.0);
    requireNumber(result, "nested_eval_second", 22.0);
    requireNumber(result, "global_failure_value", 8.0);
    requireNumber(result, "persistent_failure_first", 11.0);
    requireNumber(result, "persistent_failure_second", 22.0);
    requireNumber(result, "nested_global_value", 14.0);
    requireNumber(result, "nested_persistent_caught", 1.0);
    requireNumber(result, "static_outer_persistent_caught", 1.0);
    requireNumber(result, "local_global_value", 3.0);
    requireNumber(result, "local_global_stored", 3.0);
    requireNumber(result, "local_persistent_caught", 1.0);
    requireNumber(result, "local_persistent_value", 3.0);
    requireNumber(result, "storage_kind_conflict_caught", 1.0);
    requireNumber(result, "global_handle_escape_caught", 1.0);
    requireNumber(result, "global_handle_escape_value", 7.0);
    requireNumber(result, "persistent_handle_escape_caught", 1.0);
    requireNumber(result, "persistent_handle_escape_value", 7.0);
    requireNumber(result, "base_declaration_succeeded", 1.0);
    requireNumber(result, "base_declaration_empty", 1.0);
    requireNumber(result, "base_redeclaration_caught", 1.0);
    requireNumber(result, "script_persistent_use_caught", 1.0);
    requireNumber(result, "global_clear_local", 8.0);
    requireNumber(result, "global_clear_stored", 5.0);
    requireNumber(result, "persistent_clear_first", 5.0);
    requireNumber(result, "persistent_clear_second", 6.0);
    requireNumber(result, "persistent_clear_first_caught", 1.0);
    requireNumber(result, "persistent_clear_second_caught", 1.0);
    requireNumber(result, "clear_all_first", 6.0);
    requireNumber(result, "clear_all_second", 7.0);
    requireNumber(result, "clear_all_first_caught", 1.0);
    requireNumber(result, "clear_all_second_caught", 1.0);
}

void verifyCompiledSessionPersistence() {
    auto module = mparser::CompiledModule::compile(R"MATLAB(
function value = dynamic_session_counter()
    eval(['persistent count; if isempty(count), count = 0; end; ' ...
        'count = count + 1;']);
    value = count;
end
)MATLAB");
    require(module.valid(),
            "dynamic persistent session module did not compile");

    mparser::RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities =
        mparser::RuntimeSystemCapability::DynamicEvaluation;
    auto state = std::make_shared<mparser::RuntimeSessionState>(
        std::make_shared<mparser::RuntimeSystemContext>(
            std::move(systemOptions)));
    auto session = module.createSession(state);

    mparser::BytecodeVmOptions options;
    options.entryFunction = "dynamic_session_counter";
    options.requestedOutputCount = 1;
    const auto first = session.invoke(options);
    const auto second = session.invoke(options);
    require(first.diagnostics.empty() && second.diagnostics.empty(),
            "dynamic persistent session invocation failed");
    require(first.outputs.size() == 1 && second.outputs.size() == 1,
            "dynamic persistent session output count differs");
    const auto firstValue =
        mparser::runtimeNumericElement(first.outputs.front(), 0);
    const auto secondValue =
        mparser::runtimeNumericElement(second.outputs.front(), 0);
    require(firstValue && *firstValue == 1.0 &&
                secondValue && *secondValue == 2.0,
            "dynamic persistent did not survive repeated invocation");
    const auto persistent = session.persistentVariables();
    require(persistent.size() == 1 &&
                persistent.front().function ==
                    "dynamic_session_counter" &&
                persistent.front().name == "count",
            "dynamic persistent session metadata differs");
    require(session.clearFunction("dynamic_session_counter") == 1,
            "dynamic persistent session clear did not remove state");
    const auto reset = session.invoke(options);
    require(reset.diagnostics.empty() && reset.outputs.size() == 1,
            "dynamic persistent session did not run after clear");
    const auto resetValue =
        mparser::runtimeNumericElement(reset.outputs.front(), 0);
    require(resetValue && *resetValue == 1.0,
            "dynamic persistent session did not reset after clear");
}

void verifyOrdinaryScriptPersistentRejected() {
    const auto module = mparser::CompiledModule::compile(
        "persistent ordinary_script_persistent;");
    require(!module.valid(),
            "ordinary script persistent declaration was accepted");
}

} // namespace

int main() {
    try {
        constexpr std::string_view source = R"MATLAB(
[global_first, global_second, global_stored] = global_roundtrip();
persistent_first = persistent_step();
persistent_second = persistent_step();
caller_persistent_first = caller_persistent_owner();
caller_persistent_second = caller_persistent_owner();
caller_global_value = caller_global_owner();
caller_global_stored = caller_global_reader();
base_global_setter();
base_global_value = DYN_BASE_GLOBAL;
base_global_stored = base_global_reader();
nested_eval_first = nested_eval_persistent();
nested_eval_second = nested_eval_persistent();
global_failure_value = global_failure_step();
persistent_failure_first = persistent_failure_step();
persistent_failure_second = persistent_failure_step();
nested_global_value = nested_global_owner();
nested_persistent_caught = nested_persistent_owner();
static_outer_persistent_caught = static_outer_persistent();
[local_global_value, local_global_stored] = local_before_global();
[local_persistent_caught, local_persistent_value] = ...
    local_before_persistent();
storage_kind_conflict_caught = storage_kind_conflict();
[global_handle_escape_caught, global_handle_escape_value] = ...
    global_handle_escape();
[persistent_handle_escape_caught, persistent_handle_escape_value] = ...
    persistent_handle_escape();

base_declaration_succeeded = false;
try
    evalin('base', 'persistent DYN_BASE_DECLARATION;');
    base_declaration_succeeded = true;
catch
end
base_declaration_empty = isempty(DYN_BASE_DECLARATION);
base_redeclaration_caught = false;
try
    evalin('base', ['persistent DYN_BASE_DECLARATION; ' ...
        'DYN_BASE_DECLARATION = 1;']);
catch err
    base_redeclaration_caught = strcmp(err.identifier, ...
        'MParser:PersistentVariableAlreadyInWorkspace');
end
script_persistent_use_caught = false;
try
    eval(['persistent DYN_SCRIPT_PERSISTENT; ' ...
        'DYN_SCRIPT_PERSISTENT = 1;']);
catch err
    script_persistent_use_caught = strcmp(err.identifier, ...
        'MParser:PersistentNotInFunction');
end

[global_clear_local, global_clear_stored] = global_clear_probe();
[persistent_clear_first, persistent_clear_first_caught] = ...
    persistent_clear_step();
[persistent_clear_second, persistent_clear_second_caught] = ...
    persistent_clear_step();
[clear_all_first, clear_all_first_caught] = clear_all_step();
[clear_all_second, clear_all_second_caught] = clear_all_step();

function [first, second, stored] = global_roundtrip()
    first = global_step();
    second = global_step();
    stored = global_reader();
end

function value = global_step()
    eval(['global DYN_GLOBAL; ' ...
        'if isempty(DYN_GLOBAL), DYN_GLOBAL = 0; end; ' ...
        'DYN_GLOBAL = DYN_GLOBAL + 1;']);
    DYN_GLOBAL = DYN_GLOBAL + 10;
    value = DYN_GLOBAL;
end

function value = global_reader()
    global DYN_GLOBAL;
    value = DYN_GLOBAL;
end

function value = persistent_step()
    eval(['persistent DYN_PERSISTENT; ' ...
        'if isempty(DYN_PERSISTENT), DYN_PERSISTENT = 0; end; ' ...
        'DYN_PERSISTENT = DYN_PERSISTENT + 1;']);
    DYN_PERSISTENT = DYN_PERSISTENT + 10;
    value = DYN_PERSISTENT;
end

function value = caller_persistent_owner()
    caller_persistent_helper();
    DYN_CALLER_PERSISTENT = DYN_CALLER_PERSISTENT + 10;
    value = DYN_CALLER_PERSISTENT;
end

function caller_persistent_helper()
    evalin('caller', ['persistent DYN_CALLER_PERSISTENT; ' ...
        'if isempty(DYN_CALLER_PERSISTENT), ' ...
        'DYN_CALLER_PERSISTENT = 0; end; ' ...
        'DYN_CALLER_PERSISTENT = DYN_CALLER_PERSISTENT + 1;']);
end

function value = caller_global_owner()
    caller_global_helper();
    DYN_CALLER_GLOBAL = DYN_CALLER_GLOBAL + 10;
    value = DYN_CALLER_GLOBAL;
end

function caller_global_helper()
    evalin('caller', ...
        'global DYN_CALLER_GLOBAL; DYN_CALLER_GLOBAL = 5;');
end

function value = caller_global_reader()
    global DYN_CALLER_GLOBAL;
    value = DYN_CALLER_GLOBAL;
end

function base_global_setter()
    evalin('base', ...
        'global DYN_BASE_GLOBAL; DYN_BASE_GLOBAL = 12;');
end

function value = base_global_reader()
    global DYN_BASE_GLOBAL;
    value = DYN_BASE_GLOBAL;
end

function value = nested_eval_persistent()
    eval(['eval(''persistent DYN_NESTED_EVAL; ' ...
        'if isempty(DYN_NESTED_EVAL), DYN_NESTED_EVAL = 0; end; ' ...
        'DYN_NESTED_EVAL = DYN_NESTED_EVAL + 1;'');']);
    DYN_NESTED_EVAL = DYN_NESTED_EVAL + 10;
    value = DYN_NESTED_EVAL;
end

function value = global_failure_step()
    try
        eval(['global DYN_FAILURE_GLOBAL; ' ...
            'DYN_FAILURE_GLOBAL = 7; missing_dynamic_name;']);
    catch
    end
    DYN_FAILURE_GLOBAL = DYN_FAILURE_GLOBAL + 1;
    value = DYN_FAILURE_GLOBAL;
end

function value = persistent_failure_step()
    try
        eval(['persistent DYN_FAILURE_PERSISTENT; ' ...
            'if isempty(DYN_FAILURE_PERSISTENT), ' ...
            'DYN_FAILURE_PERSISTENT = 0; end; ' ...
            'DYN_FAILURE_PERSISTENT = DYN_FAILURE_PERSISTENT + 1; ' ...
            'missing_dynamic_name;']);
    catch
    end
    DYN_FAILURE_PERSISTENT = DYN_FAILURE_PERSISTENT + 10;
    value = DYN_FAILURE_PERSISTENT;
end

function value = nested_global_owner()
    value = 0;
    nested_body();

    function nested_body()
        eval('global DYN_NESTED_GLOBAL; DYN_NESTED_GLOBAL = 4;');
        DYN_NESTED_GLOBAL = DYN_NESTED_GLOBAL + 10;
        value = DYN_NESTED_GLOBAL;
    end
end

function caught = nested_persistent_owner()
    caught = false;
    nested_body();

    function nested_body()
        try
            eval('persistent DYN_NESTED_PERSISTENT;');
        catch err
            caught = strcmp(err.identifier, ...
                'MParser:StaticWorkspaceViolation');
        end
    end
end

function caught = static_outer_persistent()
    caught = false;
    try
        eval('persistent DYN_STATIC_OUTER;');
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:StaticWorkspaceViolation');
    end

    function unused_nested()
    end
end

function [value, stored] = local_before_global()
    DYN_LOCAL_GLOBAL = 3;
    eval('global DYN_LOCAL_GLOBAL;');
    value = DYN_LOCAL_GLOBAL;
    stored = local_global_reader();
end

function value = local_global_reader()
    global DYN_LOCAL_GLOBAL;
    value = DYN_LOCAL_GLOBAL;
end

function [caught, value] = local_before_persistent()
    DYN_LOCAL_PERSISTENT = 3;
    caught = false;
    try
        eval('persistent DYN_LOCAL_PERSISTENT;');
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:PersistentVariableAlreadyInWorkspace');
    end
    value = DYN_LOCAL_PERSISTENT;
end

function caught = storage_kind_conflict()
    eval('global DYN_STORAGE_KIND;');
    caught = false;
    try
        eval('persistent DYN_STORAGE_KIND;');
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:ConflictingWorkspaceDeclaration');
    end
end

function [caught, value] = global_handle_escape()
    global DYN_ESCAPE_GLOBAL;
    DYN_ESCAPE_GLOBAL = 7;
    caught = false;
    try
        eval(['global DYN_ESCAPE_GLOBAL; ' ...
            'DYN_ESCAPE_GLOBAL = @(input) input + 1;']);
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:DynamicEvaluationHandleEscape');
    end
    value = DYN_ESCAPE_GLOBAL;
end

function [caught, value] = persistent_handle_escape()
    eval(['persistent DYN_ESCAPE_PERSISTENT; ' ...
        'DYN_ESCAPE_PERSISTENT = 7;']);
    caught = false;
    try
        eval(['persistent DYN_ESCAPE_PERSISTENT; ' ...
            'DYN_ESCAPE_PERSISTENT = @(input) input + 1;']);
    catch err
        caught = strcmp(err.identifier, ...
            'MParser:DynamicEvaluationHandleEscape');
    end
    value = DYN_ESCAPE_PERSISTENT;
end

function [localValue, stored] = global_clear_probe()
    eval('global DYN_CLEAR_GLOBAL; DYN_CLEAR_GLOBAL = 5;');
    clear DYN_CLEAR_GLOBAL;
    DYN_CLEAR_GLOBAL = 8;
    localValue = DYN_CLEAR_GLOBAL;
    stored = clear_global_reader();
end

function value = clear_global_reader()
    global DYN_CLEAR_GLOBAL;
    value = DYN_CLEAR_GLOBAL;
end

function [before, caught] = persistent_clear_step()
    eval(['persistent DYN_CLEAR_PERSISTENT; ' ...
        'if isempty(DYN_CLEAR_PERSISTENT), ' ...
        'DYN_CLEAR_PERSISTENT = 5; else, ' ...
        'DYN_CLEAR_PERSISTENT = DYN_CLEAR_PERSISTENT + 1; end;']);
    before = DYN_CLEAR_PERSISTENT;
    clear DYN_CLEAR_PERSISTENT;
    caught = false;
    try
        DYN_CLEAR_PERSISTENT = DYN_CLEAR_PERSISTENT + 1;
    catch
        caught = true;
    end
end

function [observed, caught] = clear_all_step()
    eval(['global DYN_CLEAR_ALL_OBSERVED; ' ...
        'persistent DYN_CLEAR_ALL_PERSISTENT; ' ...
        'if isempty(DYN_CLEAR_ALL_PERSISTENT), ' ...
        'DYN_CLEAR_ALL_PERSISTENT = 6; else, ' ...
        'DYN_CLEAR_ALL_PERSISTENT = ' ...
        'DYN_CLEAR_ALL_PERSISTENT + 1; end; ' ...
        'DYN_CLEAR_ALL_OBSERVED = DYN_CLEAR_ALL_PERSISTENT; clear;']);
    observed = clear_all_global_reader();
    caught = false;
    try
        DYN_CLEAR_ALL_PERSISTENT = DYN_CLEAR_ALL_PERSISTENT + 1;
    catch
        caught = true;
    end
end

function value = clear_all_global_reader()
    global DYN_CLEAR_ALL_OBSERVED;
    value = DYN_CLEAR_ALL_OBSERVED;
end
)MATLAB";

        const RuntimePair result = runBoth(source);
        verifyResult(result.interpreter);
        verifyResult(result.vm);
        verifyCompiledSessionPersistence();
        verifyOrdinaryScriptPersistentRejected();
        std::cout << "dynamic storage smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
