#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_session_state.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    result.dimensions = {1, 1};
    return result;
}

void assertNumber(const mparser::RuntimeValue& value, double expected) {
    assert(value.kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value.number - expected) < 1e-9);
}

void assertNoDiagnostics(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.message << "\n";
    }
    assert(diagnostics.empty());
}

const mparser::RuntimeValue* findVariable(
    const std::vector<mparser::RuntimeVariable>& variables,
    std::string_view name) {
    for (const auto& variable : variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

mparser::BytecodeVmResult invoke(
    const mparser::CompiledModuleSession& session,
    std::string entryFunction,
    std::vector<mparser::RuntimeValue> arguments = {}) {
    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    options.entryFunction = std::move(entryFunction);
    options.arguments = std::move(arguments);
    options.requestedOutputCount = 1;
    return session.invoke(options);
}

void invokeNumber(
    const mparser::CompiledModuleSession& session,
    std::string entryFunction, double expected,
    std::vector<mparser::RuntimeValue> arguments = {}) {
    const auto result = invoke(
        session, std::move(entryFunction),
        std::move(arguments));
    assertNoDiagnostics(result.diagnostics);
    assert(result.outputs.size() == 1);
    assertNumber(result.outputs.front(), expected);
}

void assertEmptyMatrix(const mparser::RuntimeValue& value) {
    assert(value.kind == mparser::RuntimeValueKind::Matrix);
    assert(value.rows == 0);
    assert(value.columns == 0);
    assert(value.dimensions ==
           std::vector<size_t>({0, 0}));
    assert(value.elements.empty());
    assert(value.numericClass ==
           mparser::RuntimeNumericClass::Double);
}

void runRawStateSmoke() {
    mparser::RuntimeSessionState state;

    assertEmptyMatrix(state.declareGlobal("shared"));
    assertEmptyMatrix(state.declarePersistent("counter", "count"));
    state.storeGlobal("shared", number(7));
    state.storePersistent("counter", "count", number(3));

    assertNumber(*state.findGlobal("shared"), 7);
    assertNumber(*state.findPersistent("counter", "count"), 3);
    assert(state.globals().size() == 1);
    assert(state.persistentVariables().size() == 1);

    assert(state.clearPersistent("counter", "count"));
    assert(!state.findPersistent("counter", "count"));
    assert(!state.clearPersistent("counter", "count"));
    assert(state.clearGlobal("shared"));
    assert(!state.findGlobal("shared"));
    assert(!state.clearGlobal("shared"));

    state.storePersistent(17, "counter", "count", number(4));
    state.storePersistent(18, "counter", "count", number(9));
    assertNumber(*state.findPersistent(
                     17, "counter", "count"),
                 4);
    assertNumber(*state.findPersistent(
                     18, "counter", "count"),
                 9);
    assert(state.persistentVariables(17).size() == 1);
    assert(state.clearFunction(17, "counter") == 1);
    assert(!state.findPersistent(17, "counter", "count"));
    assertNumber(*state.findPersistent(
                     18, "counter", "count"),
                 9);
}

const std::string kSessionModule = R"(function out = nextCounter(step)
persistent count values
if isempty(count)
    count = 0;
    values = zeros(1, 2);
end
count = count + step;
values(1) = values(1) + step;
out = count * 10 + values(1);
end

function out = setShared(value)
global shared_state
shared_state = value;
out = shared_state;
end

function out = readShared()
global shared_state
out = shared_state;
end
)";

void runCompiledSessionSmoke() {
    auto module = mparser::CompiledModule::compile(kSessionModule);
    assert(module.valid());

    auto firstSession = module.createSession();
    const auto first =
        invoke(firstSession, "nextCounter", {number(1)});
    assertNoDiagnostics(first.diagnostics);
    assertNumber(first.outputs.front(), 11);

    const auto second =
        invoke(firstSession, "nextCounter", {number(2)});
    assertNoDiagnostics(second.diagnostics);
    assert(second.outputs.size() == 1);
    assertNumber(second.outputs.front(), 33);

    const auto persistent = firstSession.persistentVariables();
    assert(persistent.size() == 2);
    assert(persistent[0].function == "nextCounter");
    assert(persistent[1].function == "nextCounter");
    assert(firstSession.clearFunction("nextCounter") == 2);
    assert(firstSession.persistentVariables().empty());
    (void)invokeNumber(firstSession, "nextCounter", 44,
                       {number(4)});

    (void)invokeNumber(firstSession, "setShared", 9, {number(9)});
    (void)invokeNumber(firstSession, "readShared", 9);
    assert(firstSession.globals().size() == 1);
    assert(firstSession.clearGlobal("shared_state"));
    const auto cleared = invoke(firstSession, "readShared");
    assertNoDiagnostics(cleared.diagnostics);
    assert(cleared.outputs.size() == 1);
    assertEmptyMatrix(cleared.outputs.front());

    auto secondSession = module.createSession();
    const auto isolated = invoke(secondSession, "readShared");
    assertNoDiagnostics(isolated.diagnostics);
    assert(isolated.outputs.size() == 1);
    assertEmptyMatrix(isolated.outputs.front());
    (void)invokeNumber(secondSession, "nextCounter", 22,
                       {number(2)});

    (void)invokeNumber(firstSession, "setShared", 8, {number(8)});
    firstSession.clearGlobals();
    assert(firstSession.globals().empty());
    firstSession.reset();
    assert(firstSession.globals().empty());
    assert(firstSession.persistentVariables().empty());
}

void runCrossEngineStateSmoke() {
    const std::string source = R"(global shared
if isempty(shared)
    shared = 0;
end
shared = shared + 1;
count = nextCount();
summary = shared * 10 + count;

function out = nextCount()
persistent count
if isempty(count)
    count = 0;
end
count = count + 1;
out = count;
end
)";

    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());
    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    auto state = std::make_shared<mparser::RuntimeSessionState>();
    auto callableContext = mparser::makeRuntimeCallableContext();
    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = state;
    interpreterOptions.callableContext = callableContext;
    mparser::Interpreter interpreter;
    const auto interpreted =
        interpreter.run(semantic, interpreterOptions);
    assert(interpreted.diagnostics.empty());
    assertNumber(*findVariable(interpreted.variables, "summary"), 11);

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    mparser::BytecodeVmOptions vmOptions;
    vmOptions.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    vmOptions.sessionState = state;
    vmOptions.callableContext = callableContext;
    mparser::BytecodeVm vm;
    const auto executed = vm.run(bytecode, semantic, vmOptions);
    assert(executed.diagnostics.empty());
    assertNumber(*findVariable(executed.variables, "summary"), 22);
}

void runAssignmentRouteSmoke() {
    const std::string source = R"(global numeric_values cell_values record nested loop_value
numeric_values = zeros(1, 2);
numeric_values(2) = 3;
cell_values = {1, 2};
cell_values{2} = 4;
record = struct();
record.value = 5;
nested = struct();
nested.inner = {0, 0};
nested.inner{2} = 6;
for loop_value = 1:7
end
first = nextNested();
second = nextNested();
summary = numeric_values(2) * 10000 + cell_values{2} * 1000 + ...
    record.value * 100 + nested.inner{2} * 10 + loop_value + ...
    first * 100 + second;

function out = nextNested()
persistent state
if isempty(state)
    state = struct();
    state.inner = {0, 0};
end
state.inner{2} = state.inner{2} + 1;
out = state.inner{2};
end
)";

    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());
    mparser::SemanticAnalyzer analyzer;
    const auto semantic = analyzer.analyze(*parseResult.root);
    assertNoDiagnostics(semantic.diagnostics);

    auto interpretedState =
        std::make_shared<mparser::RuntimeSessionState>();
    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = interpretedState;
    mparser::Interpreter interpreter;
    const auto interpreted =
        interpreter.run(semantic, interpreterOptions);
    assertNoDiagnostics(interpreted.diagnostics);
    assertNumber(*findVariable(interpreted.variables, "summary"),
                 34669);
    assert(interpretedState->globals().size() == 5);
    assert(interpretedState->persistentVariables().size() == 1);

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    assertNoDiagnostics(bytecode.diagnostics);
    auto executedState =
        std::make_shared<mparser::RuntimeSessionState>();
    mparser::BytecodeVmOptions vmOptions;
    vmOptions.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    vmOptions.sessionState = executedState;
    mparser::BytecodeVm vm;
    const auto executed = vm.run(bytecode, semantic, vmOptions);
    assertNoDiagnostics(executed.diagnostics);
    assertNumber(*findVariable(executed.variables, "summary"),
                 34669);
    assert(executedState->globals().size() == 5);
    assert(executedState->persistentVariables().size() == 1);
}

void runSharedStateModuleIsolationSmoke() {
    auto state = std::make_shared<mparser::RuntimeSessionState>();
    auto firstModule =
        mparser::CompiledModule::compile(kSessionModule);
    auto secondModule =
        mparser::CompiledModule::compile(kSessionModule);
    assert(firstModule.valid());
    assert(secondModule.valid());

    auto firstSession = firstModule.createSession(state);
    auto secondSession = secondModule.createSession(state);
    invokeNumber(firstSession, "nextCounter", 11, {number(1)});
    invokeNumber(secondSession, "nextCounter", 11, {number(1)});

    const auto firstPersistent =
        firstSession.persistentVariables();
    const auto secondPersistent =
        secondSession.persistentVariables();
    assert(firstPersistent.size() == 2);
    assert(secondPersistent.size() == 2);
    assert(firstPersistent.front().contextIdentity != 0);
    assert(secondPersistent.front().contextIdentity != 0);
    assert(firstPersistent.front().contextIdentity !=
           secondPersistent.front().contextIdentity);
    assert(state->persistentVariables().size() == 4);

    assert(firstSession.clearFunction("nextCounter") == 2);
    assert(firstSession.persistentVariables().empty());
    assert(secondSession.persistentVariables().size() == 2);
    invokeNumber(secondSession, "nextCounter", 22, {number(1)});
}

void runArtifactLifetimeSmoke() {
    std::optional<mparser::CompiledModuleSession> session;
    std::optional<mparser::AdaptiveBytecodeVmSession> adaptive;
    {
        auto module =
            mparser::CompiledModule::compile(kSessionModule);
        assert(module.valid());
        session.emplace(module.createSession());

        mparser::AdaptiveBytecodeVmOptions options;
        options.hotLoopThreshold = 100;
        options.entryFunction = "nextCounter";
        options.arguments = {number(3)};
        options.requestedOutputCount = 1;
        adaptive.emplace(module.createAdaptiveSession(options));
    }

    const auto sessionResult =
        invoke(*session, "nextCounter", {number(5)});
    assertNoDiagnostics(sessionResult.diagnostics);
    assert(sessionResult.outputs.size() == 1);
    assertNumber(sessionResult.outputs.front(), 55);

    const auto adaptiveResult = adaptive->run();
    assertNoDiagnostics(adaptiveResult.runtime.diagnostics);
    assert(adaptiveResult.runtime.outputs.size() == 1);
    assertNumber(adaptiveResult.runtime.outputs.front(), 33);
    const auto repeatedAdaptiveResult = adaptive->run();
    assertNoDiagnostics(
        repeatedAdaptiveResult.runtime.diagnostics);
    assert(repeatedAdaptiveResult.runtime.outputs.size() == 1);
    assertNumber(repeatedAdaptiveResult.runtime.outputs.front(), 66);
}

void runFunctionHandleLifetimeSmoke() {
    mparser::RuntimeValue handle;
    std::weak_ptr<const void> artifact;
    {
        auto module = mparser::CompiledModule::compile(R"(
function handle = makeHandle()
handle = @squareValue;
end

function out = squareValue(value)
out = value * value;
end
)");
        assert(module.valid());
        auto session = module.createSession();
        const auto result = invoke(session, "makeHandle");
        assertNoDiagnostics(result.diagnostics);
        assert(result.outputs.size() == 1);
        handle = result.outputs.front();
        assert(handle.kind ==
               mparser::RuntimeValueKind::FunctionHandle);
        assert(handle.functionHandle);
        assert(handle.functionHandle->context);
        artifact =
            handle.functionHandle->context->lifetimeAnchor;
        assert(!artifact.expired());
    }

    assert(!artifact.expired());
    handle = {};
    assert(artifact.expired());
}

} // namespace

int main() {
    runRawStateSmoke();
    runCompiledSessionSmoke();
    runCrossEngineStateSmoke();
    runAssignmentRouteSmoke();
    runSharedStateModuleIsolationSmoke();
    runArtifactLifetimeSmoke();
    runFunctionHandleLifetimeSmoke();
    std::cout << "runtime session smoke tests passed\n";
    return 0;
}
