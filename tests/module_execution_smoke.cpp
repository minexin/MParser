#include "mparser/compiled_module.h"
#include "mparser/native_scalar_jit.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value.h"

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

mparser::RuntimeValue number(double value) {
    return mparser::makeRuntimeNumberValue(value);
}

void requireScalar(
    const mparser::RuntimeValue& value, double expected,
    std::string_view label) {
    require(value.kind == mparser::RuntimeValueKind::Number,
            std::string(label) + " is not a scalar number");
    require(std::abs(value.number - expected) < 1e-9,
            std::string(label) + " has an unexpected value");
}

void requireText(const mparser::RuntimeValue& value,
                 std::string_view expected,
                 std::string_view label) {
    const auto text = mparser::runtimeTextScalarUtf8(value);
    require(text && *text == expected,
            std::string(label) + " has an unexpected text value");
}

void requireOutputs(
    const mparser::ModuleInvocationResult& result,
    double first, double second) {
    require(result.succeeded(), "module invocation did not succeed");
    require(result.outputs.size() == 2,
            "module invocation did not return two outputs");
    requireScalar(result.outputs[0], first, "first output");
    requireScalar(result.outputs[1], second, "second output");
}

const mparser::ModuleDiagnostic* findDiagnostic(
    const mparser::ModuleInvocationResult& result,
    std::string_view identifier) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier == identifier) {
            return &diagnostic;
        }
    }
    return nullptr;
}

const mparser::RuntimeValue* findVariable(
    const mparser::ModuleInvocationResult& result,
    std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const std::string kModuleSource = R"(function [total, last] = accumulate(limit)
total = 0;
for i = 1:limit
    total = total + i;
end
last = i;
end

function out = nextCounter(step)
persistent count
if isempty(count)
    count = 0;
end
count = count + step;
out = count;
end

function out = emitWarning()
warning("Embed:Notice", "notice %d", 3);
out = 7;
end

function out = writeWarningState()
warning("off", "Embed:Persistent");
lastwarn("session message", "Embed:Persistent");
out = 1;
end

function [state, message, identifier] = readWarningState()
state = warning("query", "Embed:Persistent");
[message, identifier] = lastwarn();
end

function out = failNow()
error("Embed:Failure", "failed %d", 9);
out = 0;
end
)";

mparser::ModuleInvocationRequest accumulateRequest(
    mparser::ModuleExecutionBackend backend) {
    mparser::ModuleInvocationRequest request;
    request.entryFunction = "accumulate";
    request.arguments = {number(100)};
    request.requestedOutputCount = 2;
    request.backend = backend;
    return request;
}

void runBackendSmoke(const mparser::CompiledModule& module) {
    auto bytecodeRequest = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    bytecodeRequest.collectProfile = true;
    const auto bytecode = module.execute(bytecodeRequest);
    requireOutputs(bytecode, 5050, 100);
    require(bytecode.execution.requestedBackend ==
                mparser::ModuleExecutionBackend::Bytecode,
            "bytecode request backend was not retained");
    require(bytecode.execution.effectiveTier ==
                mparser::ModuleExecutionTier::Bytecode,
            "bytecode request used an optimized tier");
    require(bytecode.execution.profilingCollected,
            "bytecode profiling was not collected");
    require(bytecode.execution.executedInstructionCount > 0,
            "bytecode instruction count is empty");
    require(bytecode.execution.typedRegionAttemptCount == 0,
            "bytecode request attempted a typed region");

    const auto portable = module.execute(accumulateRequest(
        mparser::ModuleExecutionBackend::Portable));
    requireOutputs(portable, 5050, 100);
    require(portable.execution.effectiveTier ==
                mparser::ModuleExecutionTier::Portable,
            "portable request did not execute a portable region");
    require(portable.execution.typedRegionAttemptCount > 0,
            "portable request did not attempt a typed region");
    require(portable.execution.typedRegionExecutionCount > 0,
            "portable request did not execute a typed region");
    require(!portable.execution.fallbackOccurred,
            "portable request unexpectedly fell back");

    const auto native = module.execute(accumulateRequest(
        mparser::ModuleExecutionBackend::Native));
    requireOutputs(native, 5050, 100);
    if (mparser::nativeScalarJitAvailable()) {
        require(native.execution.effectiveTier ==
                    mparser::ModuleExecutionTier::Native,
                "native request did not execute native code");
        require(native.execution.typedRegionExecutionCount > 0,
                "native request did not execute a typed region");
        require(native.execution.nativeCompilationCount +
                    native.execution.nativeCacheHitCount >
                    0,
                "native request recorded neither compilation nor cache hit");
    } else {
        require(native.execution.effectiveTier ==
                    mparser::ModuleExecutionTier::Bytecode,
                "unavailable native backend did not fall back to bytecode");
        require(native.execution.fallbackOccurred,
                "unavailable native backend did not report fallback");
    }

    const auto automatic = module.execute(accumulateRequest(
        mparser::ModuleExecutionBackend::Automatic));
    requireOutputs(automatic, 5050, 100);
    require(automatic.execution.effectiveTier !=
                mparser::ModuleExecutionTier::Mixed,
            "single-region automatic request reported a mixed tier");
}

void runDiagnosticSmoke(const mparser::CompiledModule& module) {
    auto missingRequest = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    missingRequest.entryFunction = "missingEntry";
    const auto missing = module.execute(missingRequest);
    require(missing.status ==
                mparser::ModuleInvocationStatus::RequestRejected,
            "missing entry was not rejected");
    const auto* missingDiagnostic = findDiagnostic(
        missing, "MParser:EntryFunctionNotFound");
    require(missingDiagnostic != nullptr,
            "missing entry diagnostic identifier was not stable");
    require(missingDiagnostic->phase ==
                mparser::ModuleDiagnosticPhase::Validation,
            "missing entry diagnostic has the wrong phase");

    auto outputRequest = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    outputRequest.requestedOutputCount = 3;
    const auto outputMismatch = module.execute(outputRequest);
    require(findDiagnostic(
                outputMismatch,
                "MParser:OutputCountMismatch") != nullptr,
            "output count mismatch was not identified");

    auto malformedRequest = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    malformedRequest.arguments.front() =
        mparser::makeRuntimeMatrixValue(2, 2, {1, 2, 3, 4});
    malformedRequest.arguments.front().elements.pop_back();
    const auto malformed = module.execute(malformedRequest);
    require(findDiagnostic(
                malformed,
                "MParser:InvalidArgumentValue") != nullptr,
            "malformed RuntimeValue argument was not rejected");

    auto duplicateWorkspace = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    duplicateWorkspace.initialWorkspace = {
        {"seed", number(1)}, {"seed", number(2)}};
    const auto duplicate = module.execute(duplicateWorkspace);
    require(findDiagnostic(
                duplicate,
                "MParser:DuplicateWorkspaceVariable") != nullptr,
            "duplicate workspace variable was not rejected");

    auto transientWorkspace = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    transientWorkspace.initialWorkspace = {
        {"missing", mparser::makeRuntimeMissingValue()}};
    const auto transient = module.execute(transientWorkspace);
    require(findDiagnostic(
                transient,
                "MParser:TransientWorkspaceValue") != nullptr,
            "transient workspace value was not rejected");

    auto invalidBackend = accumulateRequest(
        mparser::ModuleExecutionBackend::Bytecode);
    invalidBackend.backend =
        static_cast<mparser::ModuleExecutionBackend>(1000);
    const auto invalid = module.execute(invalidBackend);
    require(findDiagnostic(
                invalid,
                "MParser:InvalidExecutionBackend") != nullptr,
            "invalid backend was not rejected");

    mparser::ModuleInvocationRequest warningRequest;
    warningRequest.entryFunction = "emitWarning";
    warningRequest.requestedOutputCount = 1;
    warningRequest.backend =
        mparser::ModuleExecutionBackend::Bytecode;
    const auto warning = module.execute(warningRequest);
    require(warning.succeeded(),
            "warning incorrectly failed module execution");
    require(warning.hasWarnings(),
            "warning result did not report warning presence");
    const auto* warningDiagnostic =
        findDiagnostic(warning, "Embed:Notice");
    require(warningDiagnostic != nullptr,
            "warning identifier was not preserved");
    require(warningDiagnostic->severity ==
                mparser::ModuleDiagnosticSeverity::Warning,
            "warning severity was not preserved");
    require(warningDiagnostic->phase ==
                mparser::ModuleDiagnosticPhase::Execution,
            "warning phase was not execution");

    mparser::ModuleInvocationRequest failureRequest;
    failureRequest.entryFunction = "failNow";
    failureRequest.requestedOutputCount = 1;
    failureRequest.backend =
        mparser::ModuleExecutionBackend::Bytecode;
    const auto failure = module.execute(failureRequest);
    require(failure.status ==
                mparser::ModuleInvocationStatus::RuntimeFailed,
            "uncaught runtime error did not fail execution");
    const auto* failureDiagnostic =
        findDiagnostic(failure, "Embed:Failure");
    require(failureDiagnostic != nullptr,
            "runtime error identifier was not preserved");
    require(failureDiagnostic->phase ==
                mparser::ModuleDiagnosticPhase::Execution,
            "runtime error phase was not execution");
}

void runCompilationAndScriptSmoke() {
    std::vector<mparser::SourceUnit> invalidSources{
        mparser::SourceUnit{
            "broken_module.m",
            "function out = broken(\nout = 1;\nend\n"}};
    const auto invalidModule =
        mparser::CompiledModule::compile(std::move(invalidSources));
    const auto compilation = invalidModule.execute();
    require(compilation.status ==
                mparser::ModuleInvocationStatus::CompilationFailed,
            "invalid module did not report compilation failure");
    require(!compilation.diagnostics.empty(),
            "compilation failure did not project diagnostics");
    require(compilation.diagnostics.front().phase ==
                mparser::ModuleDiagnosticPhase::Compilation,
            "compilation diagnostic has the wrong phase");
    require(compilation.diagnostics.front().source.available,
            "compilation diagnostic has no source range");
    require(compilation.diagnostics.front().source.sourceName ==
                "broken_module.m",
            "compilation diagnostic lost its source name");

    const auto script =
        mparser::CompiledModule::compile("summary = seed + 1;");
    require(script.valid(), "workspace script did not compile");
    mparser::ModuleInvocationRequest scriptRequest;
    scriptRequest.initialWorkspace = {{"seed", number(41)}};
    scriptRequest.backend =
        mparser::ModuleExecutionBackend::Bytecode;
    const auto scriptResult = script.execute(scriptRequest);
    require(scriptResult.succeeded(),
            "script workspace invocation failed");
    const auto* summary = findVariable(scriptResult, "summary");
    require(summary != nullptr,
            "script result did not expose workspace variables");
    requireScalar(*summary, 42, "script summary");

    const auto shadowScript = mparser::CompiledModule::compile(
        R"(selected = sin(end);
total = sum(sin(:));
loop_total = 0;
for shadow_index = 1:3
    loop_total = loop_total + sin(shadow_index);
end
)");
    require(shadowScript.valid(),
            "call/index shadowing script did not compile");
    mparser::ModuleInvocationRequest shadowRequest;
    shadowRequest.initialWorkspace = {
        {"sin", mparser::makeRuntimeVectorValue({10, 20, 30})}};
    shadowRequest.backend =
        mparser::ModuleExecutionBackend::Portable;
    const auto shadowResult = shadowScript.execute(shadowRequest);
    require(shadowResult.succeeded(),
            "initial workspace did not shadow a builtin call target");
    const auto* selected = findVariable(shadowResult, "selected");
    const auto* total = findVariable(shadowResult, "total");
    const auto* loopTotal = findVariable(shadowResult, "loop_total");
    require(selected != nullptr && total != nullptr && loopTotal != nullptr,
            "shadowing script did not expose its results");
    requireScalar(*selected, 30, "shadowed builtin end index");
    requireScalar(*total, 60, "shadowed builtin colon index");
    requireScalar(*loopTotal, 60, "shadowed typed builtin call");

    scriptRequest.arguments = {number(1)};
    const auto scriptArguments = script.execute(scriptRequest);
    require(findDiagnostic(
                scriptArguments,
                "MParser:ScriptArgumentsUnsupported") != nullptr,
            "script arguments were not rejected");
}

void runHostIntegrationSmoke() {
    const auto module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{{
            "host_output.m",
            R"(formatted = sprintf("value=%d", 42);
disp(formatted)
written = fprintf("pi=%.1f\n", 3.14);
40 + 2
41 + 2;
)"}});
    require(module.valid(), "host integration script did not compile");
    require(module.sourceInfo().size() == 1,
            "source metadata count mismatch");
    const auto* source = module.sourceInfo(0);
    require(source && source->name == "host_output.m" &&
                source->kind == mparser::CompiledSourceKind::Script &&
                source->hasTopLevelStatements &&
                !source->pureFunctionFile(),
            "script source metadata mismatch");

    const std::vector<mparser::ModuleExecutionBackend> backends{
        mparser::ModuleExecutionBackend::Bytecode,
        mparser::ModuleExecutionBackend::Portable,
        mparser::ModuleExecutionBackend::Native};
    for (const auto backend : backends) {
        std::vector<mparser::ModuleOutputEvent> observed;
        mparser::ModuleInvocationRequest request;
        request.backend = backend;
        request.outputSink = [&observed](
                                 const mparser::ModuleOutputEvent& event) {
            observed.push_back(event);
            return true;
        };
        const auto result = module.execute(request);
        require(result.succeeded(),
                "host integration execution failed");
        require(result.outputEvents.size() == 2 &&
                    observed.size() == result.outputEvents.size(),
                "host output event count mismatch");
        require(result.outputEvents[0].kind ==
                        mparser::ModuleOutputKind::Display &&
                    result.outputEvents[0].text == "value=42\n\n" &&
                    result.outputEvents[0].source.available &&
                    result.outputEvents[0].source.sourceName ==
                        "host_output.m" &&
                    result.outputEvents[0].sequence == 0,
                "display event projection mismatch");
        require(result.outputEvents[1].kind ==
                        mparser::ModuleOutputKind::StandardOutput &&
                    result.outputEvents[1].text == "pi=3.1\n",
                "formatted output projection mismatch");
        require(result.outputEvents[1].sequence == 1,
                "formatted output sequence mismatch");
        require(result.topLevelExpressions.size() == 2,
                "top-level expression count mismatch");
        requireScalar(result.topLevelExpressions[0].value, 42,
                      "visible top-level expression");
        require(!result.topLevelExpressions[0].outputSuppressed &&
                    result.topLevelExpressions[0].source.available &&
                    result.topLevelExpressions[0].source.sourceName ==
                        "host_output.m" &&
                    result.topLevelExpressions[0].sequence == 2 &&
                    result.topLevelExpressions[0].lineSpacing ==
                        mparser::RuntimeLineSpacing::Loose,
                "visible expression metadata mismatch");
        requireScalar(result.topLevelExpressions[1].value, 43,
                      "suppressed top-level expression");
        require(result.topLevelExpressions[1].outputSuppressed,
                "suppressed expression metadata mismatch");
        require(result.topLevelExpressions[1].sequence == 3,
                "suppressed expression sequence mismatch");
        const auto* answer = findVariable(result, "ans");
        require(answer != nullptr, "script did not publish ans");
        requireScalar(*answer, 43, "script ans");
    }

    const auto functionModule = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{{
            "primary.m",
            "function y = primary(x)\ny = x + 1;\nend\n"}});
    require(functionModule.valid(), "function metadata source did not compile");
    const auto* functionSource = functionModule.sourceInfo(0);
    require(functionSource &&
                functionSource->kind ==
                    mparser::CompiledSourceKind::Function &&
                functionSource->primaryFunction == "primary" &&
                functionSource->pureFunctionFile(),
            "function source metadata mismatch");

    mparser::ModuleInvocationRequest rejectedRequest;
    rejectedRequest.outputSink = [](const mparser::ModuleOutputEvent&) {
        return false;
    };
    const auto rejected = module.execute(rejectedRequest);
    require(!rejected.succeeded() &&
                findDiagnostic(rejected, "MParser:OutputSinkRejected"),
            "output sink rejection was not projected");
}

void runSessionSmoke(const mparser::CompiledModule& module) {
    auto session = module.createSession();
    mparser::ModuleInvocationRequest request;
    request.entryFunction = "nextCounter";
    request.requestedOutputCount = 1;
    request.backend = mparser::ModuleExecutionBackend::Bytecode;

    request.arguments = {number(2)};
    const auto first = session.execute(request);
    require(first.succeeded() && first.outputs.size() == 1,
            "first session invocation failed");
    requireScalar(first.outputs.front(), 2, "first session output");

    request.arguments = {number(3)};
    const auto second = session.execute(request);
    require(second.succeeded() && second.outputs.size() == 1,
            "second session invocation failed");
    requireScalar(second.outputs.front(), 5, "second session output");
    require(session.persistentVariables().size() == 1,
            "session did not retain persistent state");

    mparser::ModuleInvocationRequest writeWarning;
    writeWarning.entryFunction = "writeWarningState";
    writeWarning.requestedOutputCount = 1;
    writeWarning.backend = mparser::ModuleExecutionBackend::Bytecode;
    const auto warningWrite = session.execute(writeWarning);
    require(warningWrite.succeeded(),
            "session warning-state write failed");

    mparser::ModuleInvocationRequest readWarning;
    readWarning.entryFunction = "readWarningState";
    readWarning.requestedOutputCount = 3;
    readWarning.backend = mparser::ModuleExecutionBackend::Bytecode;
    const auto warningRead = session.execute(readWarning);
    require(warningRead.succeeded() && warningRead.outputs.size() == 3,
            "session warning-state read failed");
    const auto* state = mparser::runtimeStructField(
        warningRead.outputs[0], "state");
    require(state != nullptr, "session warning query has no state field");
    requireText(*state, "off", "session warning state");
    requireText(warningRead.outputs[1], "session message",
                "session last warning message");
    requireText(warningRead.outputs[2], "Embed:Persistent",
                "session last warning identifier");

    const auto statelessWrite = module.execute(writeWarning);
    const auto statelessRead = module.execute(readWarning);
    require(statelessWrite.succeeded() && statelessRead.succeeded() &&
                statelessRead.outputs.size() == 3,
            "stateless warning-state probes failed");
    const auto* statelessState = mparser::runtimeStructField(
        statelessRead.outputs[0], "state");
    require(statelessState != nullptr,
            "stateless warning query has no state field");
    requireText(*statelessState, "on", "stateless warning state");
    requireText(statelessRead.outputs[1], "",
                "stateless last warning message");
    requireText(statelessRead.outputs[2], "",
                "stateless last warning identifier");

    session.reset();
    request.arguments = {number(1)};
    const auto reset = session.execute(request);
    require(reset.succeeded() && reset.outputs.size() == 1,
            "reset session invocation failed");
    requireScalar(reset.outputs.front(), 1, "reset session output");

    const auto resetWarning = session.execute(readWarning);
    require(resetWarning.succeeded() && resetWarning.outputs.size() == 3,
            "reset session warning-state read failed");
    const auto* resetState = mparser::runtimeStructField(
        resetWarning.outputs[0], "state");
    require(resetState != nullptr,
            "reset warning query has no state field");
    requireText(*resetState, "on", "reset session warning state");
    requireText(resetWarning.outputs[1], "",
                "reset session last warning message");
    requireText(resetWarning.outputs[2], "",
                "reset session last warning identifier");
}

void runNameSmoke() {
    require(mparser::moduleExecutionBackendName(
                mparser::ModuleExecutionBackend::Automatic) ==
                "automatic",
            "backend name is unstable");
    require(mparser::moduleExecutionTierName(
                mparser::ModuleExecutionTier::Mixed) == "mixed",
            "tier name is unstable");
    require(mparser::moduleInvocationStatusName(
                mparser::ModuleInvocationStatus::RequestRejected) ==
                "request-rejected",
            "status name is unstable");
    require(mparser::moduleDiagnosticPhaseName(
                mparser::ModuleDiagnosticPhase::Validation) ==
                "validation",
            "diagnostic phase name is unstable");
    require(mparser::moduleDiagnosticSeverityName(
                mparser::ModuleDiagnosticSeverity::Warning) ==
                "warning",
            "diagnostic severity name is unstable");
    require(mparser::moduleOutputKindName(
                mparser::ModuleOutputKind::StandardOutput) ==
                "stdout",
            "output kind name is unstable");
}

} // namespace

int main() {
    try {
        const auto module =
            mparser::CompiledModule::compile(kModuleSource);
        require(module.valid(), "embedding module did not compile");
        runBackendSmoke(module);
        runDiagnosticSmoke(module);
        runCompilationAndScriptSmoke();
        runHostIntegrationSmoke();
        runSessionSmoke(module);
        runNameSmoke();
        std::cout << "module execution smoke tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "module execution smoke tests failed: "
                  << exception.what() << "\n";
        return 1;
    }
}
