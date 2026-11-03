#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/execution/jit/optimization_plan.h"
#include "mparser/execution/jit/typed_ir.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/io/runtime_system.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace mparser;
using Json = nlohmann::json;
enum class Engine { Hir, Bytecode, Portable, Native, Automatic };

constexpr const char* helpers = R"(
function value=expression_probe_value()
value=40+nargout;
end
function expression_probe_no_output()
local=22
end
function varargout=expression_probe_varargout()
if nargout>0, varargout={70}; end
end
function varargout=expression_probe_optional()
varargout={70};
end
function value=expression_probe_missing()
if nargout>0, value=80; end
end
function expression_probe_global()
global ans;
3+4
end
function result=expression_probe_scope()
ans=100;
named=9;
named
before=ans;
2+3
after=ans;
result=[before,after,nargout];
end
)";

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

BytecodeVmResult run(const std::string& source, Engine engine, bool capture,
    std::vector<RuntimeVariable> initial = {}, std::shared_ptr<RuntimeExecutionControl> control = {}) {
    auto module = CompiledModule::compile(std::vector<SourceUnit>{{"expression_probe.m",source+helpers}});
    require(module.valid(), "expression source must compile");
    RuntimeSystemContextOptions context;
    context.capabilities = RuntimeSystemCapability::DynamicEvaluation;
    const auto session = std::make_shared<RuntimeSessionState>(std::make_shared<RuntimeSystemContext>(context));
    if (engine == Engine::Hir) {
        InterpreterOptions options;
        options.sessionState = session;
        options.captureAssignmentDisplays = capture;
        options.executionControl = control;
        require(initial.empty(),"HIR has no initial-workspace option");
        auto interpreted = Interpreter{}.run(module.semantic(),options);
        BytecodeVmResult result;
        result.variables = std::move(interpreted.variables);
        result.diagnostics = std::move(interpreted.diagnostics);
        result.expressionResults = std::move(interpreted.expressionResults);
        result.outputEvents = std::move(interpreted.outputEvents);
        result.journalEvents = std::move(interpreted.journalEvents);
        return result;
    }
    BytecodeVmOptions options;
    options.sessionState = session;
    options.captureAssignmentDisplays = capture;
    options.executionControl = control;
    options.initialWorkspace = std::move(initial);
    if (engine == Engine::Bytecode) { return BytecodeVm{}.run(module.bytecode(),module.semantic(),options); }
    options.typedRegionBackend = engine == Engine::Portable ? TypedRegionBackend::Portable :
        engine == Engine::Native ? TypedRegionBackend::Native : TypedRegionBackend::Auto;
    const auto typed = BytecodeTypedIrBuilder{}.build(
        BytecodeOptimizationPlanner{}.planStaticRegions(module.bytecode(),module.semantic().builtinRegistry));
    return BytecodeVm{}.run(module.bytecode(),module.semantic(),typed,options);
}

const RuntimeValue* variable(const BytecodeVmResult& result, const std::string& name) {
    const auto found = std::find_if(result.variables.begin(),result.variables.end(),
        [&](const auto& item) { return item.name == name; });
    return found == result.variables.end() ? nullptr : &found->value;
}

std::vector<std::string> labels(const std::string& console) {
    std::vector<std::string> result;
    std::istringstream lines(console);
    std::string line;
    while (std::getline(lines,line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const auto equal = line.find(" =");
        if (equal != std::string::npos && equal != 0) { result.push_back(line.substr(0,equal)); }
    }
    return result;
}

void flatten(const Json& value, std::vector<double>& values) {
    if (value.is_array()) { for (const auto& item : value) { flatten(item,values); } }
    else if (value.is_number()) { values.push_back(value.get<double>()); }
}

void numeric(const RuntimeValue* actual, const Json& reference, const std::string& message) {
    require(actual != nullptr, message+" missing value");
    std::vector<double> expected;
    flatten(reference,expected);
    require(runtimeShapeElementCount(*actual) == expected.size(),message+" element count");
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto value = runtimeNumericElement(*actual,index);
        require(value && std::abs(*value-expected[index]) <= std::max(1e-30,std::abs(expected[index])*2e-15),
            message+" numeric value");
    }
}

void corpus(const Json& reference, Engine engine, bool capture) {
    for (const auto& test : reference.at("cases")) {
        const auto id = test.at("name").get<std::string>()+" engine="+std::to_string(static_cast<int>(engine))+
            " capture="+std::to_string(capture);
        const auto result = run(test.at("source"),engine,capture);
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.severity == DiagnosticSeverity::Error) { std::cerr << id << ": " << diagnostic.message << '\n'; }
        }
        require(!hasErrorDiagnostics(result.diagnostics),id+" execution");
        const auto answer = variable(result,"ans");
        require((answer != nullptr) == test.at("ansPresent").get<bool>(),id+" ans presence");
        if (answer) {
            numeric(answer,test.at("answer").at("real"),id+" ans");
            require(runtimeDimensions(*answer) == test.at("answer").at("dimensions").get<std::vector<size_t>>(),
                id+" ans dimensions");
        }
        if (!test.at("report").empty()) { numeric(variable(result,"report"),test.at("report"),id+" report"); }
        const auto expected = labels(test.at("console"));
        if (capture) {
            std::vector<std::string> actual;
            for (const auto& event : result.journalEvents) {
                if (const auto display = std::get_if<RuntimeAssignmentDisplay>(&event.payload);
                    display && !display->outputSuppressed && event.captureDepth == 0) {
                    require(!display->snapshotFailure,id+" immutable expression snapshot");
                    actual.push_back(display->label);
                }
            }
            require(actual == expected,id+" journal labels and order");
        } else {
            require(result.journalEvents.empty(),id+" recording remains opt-in");
            // Legacy console still excludes assignment displays; compare expression labels only.
            auto expressionLabels = expected;
            expressionLabels.erase(std::remove(expressionLabels.begin(),expressionLabels.end(),"local"),expressionLabels.end());
            require(labels(runtimeRenderConsole(result.outputEvents,result.expressionResults)) == expressionLabels,
                id+" expression console labels");
        }
        if (!test.at("captured").get<std::string>().empty()) {
            const auto text = variable(result,"text");
            require(text != nullptr,id+" captured value exists");
            const auto actual = runtimeTextScalarUtf8(*text);
            require(actual && labels(*actual) == labels(test.at("captured")),id+" evalc labels");
        }
    }
}

void additional(Engine engine) {
    RuntimeValue nine; nine.kind = RuntimeValueKind::Number; nine.number = 9;
    for (const std::string name : {"pi","disp"}) {
        const auto result = engine == Engine::Hir ?
            run(name+"=9; ans=100; (("+name+"))",engine,true) :
            run("ans=100; (("+name+"))",engine,true,{{name,nine}});
        require(!hasErrorDiagnostics(result.diagnostics),"runtime variable shadows builtin including zero-output builtin");
        numeric(variable(result,"ans"),Json(100),"shadow preserves ans");
        require(std::get<RuntimeAssignmentDisplay>(result.journalEvents.back().payload).label == name,
            "label follows actual runtime load, not static builtin binding");
    }
    const auto result = run("ans=100; named=9; named\nnamed(99)\n42",engine,true);
    require(hasErrorDiagnostics(result.diagnostics),"failed expression is rejected");
    numeric(variable(result,"ans"),Json(100),"failed expression leaves previous ans");
    require(std::count_if(result.journalEvents.begin(),result.journalEvents.end(),
        [](const auto& event) { return event.kind == RuntimeJournalKind::Expression; }) == 1,
        "failure produces no fabricated expression and does not execute following statements");
    const auto history = run("ans=100; named=9;\nnamed\nc={7,8};\nc{:}\nnamed=20; c{1}=90;",engine,true);
    std::vector<const RuntimeAssignmentDisplay*> expressions;
    for (const auto& event : history.journalEvents) {
        if (event.kind == RuntimeJournalKind::Expression) {
            expressions.push_back(&std::get<RuntimeAssignmentDisplay>(event.payload));
        }
    }
    require(expressions.size() == 3,"comma-list values are separate expression records");
    for (size_t index = 0; index < expressions.size(); ++index) {
        const auto& expression = *expressions[index];
        require(expression.valueSnapshot.available() && expression.valueSnapshot.node().numeric.size() == 1 &&
            expression.valueSnapshot.node().numeric[0].real == (index == 0 ? 9 : index == 1 ? 7 : 8),
            "each expression freezes its actual intermediate value before later mutation");
        require(expression.span.begin.line == (index == 0 ? 2 : 4) && expression.sourceName == "expression_probe.m",
            "expanded values keep original statement provenance");
    }
    size_t calls = 0;
    std::string streamed;
    auto control = std::make_shared<RuntimeExecutionControl>(RuntimeExecutionLimits{},std::nullopt,nullptr,
        RuntimeInputSource{},[&](std::string_view text) { streamed += text; return ++calls < 2; });
    const auto rejected = run("ans=100; named=9; named\nc={7,8}; c{:}\n42",engine,true,{},control);
    require(hasErrorDiagnostics(rejected.diagnostics) && calls == 2 && labels(streamed) == std::vector<std::string>{"named","ans"},
        "console sees real labels and can reject midway through an expanded expression");
    numeric(variable(rejected,"ans"),Json(7),"rejected sink stops before later comma-list values and statements");
    require(std::count_if(rejected.journalEvents.begin(),rejected.journalEvents.end(),
        [](const auto& event) { return event.kind == RuntimeJournalKind::Expression; }) == 2,
        "rejection keeps only the actual generated expression prefix");
}

void legacyProjection() {
    const auto module = CompiledModule::compile(std::vector<SourceUnit>{{"expression_projection.m",
        std::string("ans=100; expression_probe_scope();\n21")+helpers}});
    require(module.valid(),"legacy projection source compiles");
    for (const auto backend : {ModuleExecutionBackend::Bytecode,ModuleExecutionBackend::Portable,
            ModuleExecutionBackend::Native,ModuleExecutionBackend::Automatic}) {
        for (const bool capture : {false,true}) {
            ModuleInvocationRequest request;
            request.backend = backend;
            request.captureEventJournal = capture;
            const auto result = module.execute(request);
            require(result.succeeded() && result.topLevelExpressions.size() == 2,
                "legacy top-level array must not acquire function-body expressions");
            require(result.topLevelExpressions[0].source.begin.line == 1 && result.topLevelExpressions[1].source.begin.line == 2,
                "legacy projection keeps script provenance");
            if (capture) {
                require(std::count_if(result.journalEvents.begin(),result.journalEvents.end(),
                    [](const auto& event) { return event.kind == RuntimeJournalKind::Expression; }) == 4,
                    "new internal journal still contains function-body expressions");
            }
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2,"expected actual MATLAB reference path");
        std::ifstream file(argv[1]);
        const auto reference = Json::parse(file);
        require(reference.at("release") == "2024b" && reference.at("cases").size() == 39,
            "expected fixed 39-case R2024b corpus");
        for (const auto engine : {Engine::Hir,Engine::Bytecode,Engine::Portable,Engine::Native,Engine::Automatic}) {
            corpus(reference,engine,false);
            corpus(reference,engine,true);
            additional(engine);
        }
        legacyProjection();
        std::cout << "Expression display: 39 R2024b cases x 5 requested engines x 2 recording modes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Expression display failure: " << error.what() << '\n';
        return 1;
    }
}
