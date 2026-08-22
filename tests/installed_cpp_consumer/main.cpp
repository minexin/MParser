#include "mparser/cpp_api.hpp"

#include "consumer_module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr double kTolerance = 1e-9;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double scalar(const mparser::sdk::Value& value) {
    const auto data = value.numericData();
    require(data.size() == 1, "expected scalar value");
    return data.front();
}

bool hasDiagnostic(
    const std::vector<mparser::sdk::Diagnostic>& diagnostics,
    const std::string& identifier) {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) {
            return diagnostic.identifier == identifier;
        });
}

} // namespace

int main() {
    try {
        const auto version = mparser::sdk::runtimeVersion();
        const auto sourceApiVersion =
            mparser::sdk::sourceApiVersion();
        require(version.major == MPARSER_EXPECTED_VERSION_MAJOR &&
                    version.minor == MPARSER_EXPECTED_VERSION_MINOR &&
                    version.patch == MPARSER_EXPECTED_VERSION_PATCH,
                "installed header and runtime versions differ");
        require(sourceApiVersion.major ==
                    MPARSER_EXPECTED_CPP_API_VERSION_MAJOR &&
                    sourceApiVersion.minor ==
                    MPARSER_EXPECTED_CPP_API_VERSION_MINOR &&
                    mparserInstalledCppApiVersionToken() ==
                        sourceApiVersion.major * 100u +
                            sourceApiVersion.minor,
                "installed C++ source API versions differ across translation units");

        const auto module = mparser::sdk::Module::compile(R"(
function [total, last] = accumulate(limit)
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

function out = failNow()
error("Installed:Failure", "expected failure");
out = 0;
end

function out = identity(value)
out = value;
end
)", "installed_cpp_consumer.m");
        require(module.isValid(), "module compilation failed");

        mparser::sdk::Invocation accumulate;
        accumulate.entryFunction = "accumulate";
        accumulate.arguments = {mparser::sdk::Value::scalar(100)};
        accumulate.requestedOutputCount = 2;
        accumulate.collectProfile = true;
        auto result = module.execute(accumulate);
        require(result.succeeded() && result.outputCount() == 2,
                "multi-output invocation failed");
        auto retainedTotal = result.output(0);
        require(std::abs(scalar(retainedTotal) - 5050.0) < kTolerance &&
                    std::abs(scalar(result.output(1)) - 100.0) < kTolerance,
                "multi-output values differ");
        require(result.executionSummary().executedInstructionCount > 0,
                "execution summary is empty");
        result = {};
        require(std::abs(scalar(retainedTotal) - 5050.0) < kTolerance,
                "retained value lost its owner");

        const std::array<mparser::sdk::NamedValue, 2> fields{
            mparser::sdk::NamedValue{"second", mparser::sdk::Value::scalar(2)},
            mparser::sdk::NamedValue{"first", mparser::sdk::Value::scalar(1)}};
        const auto structure = mparser::sdk::Value::structure(fields);
        require(structure.structFieldNames() ==
                    std::vector<std::string>({"second", "first"}),
                "structure field order differs");
        mparser::sdk::Invocation identity;
        identity.entryFunction = "identity";
        identity.arguments = {structure};
        identity.requestedOutputCount = 1;
        require(module.execute(identity).output(0).structFieldNames() ==
                    std::vector<std::string>({"second", "first"}),
                "composite value round trip differs");

        mparser::sdk::Invocation failure;
        failure.entryFunction = "failNow";
        failure.requestedOutputCount = 1;
        const auto failed = module.execute(failure);
        require(!failed.succeeded() &&
                    hasDiagnostic(failed.diagnostics(), "Installed:Failure"),
                "runtime diagnostic differs");

        auto session = module.createSession();
        mparser::sdk::Invocation counter;
        counter.entryFunction = "nextCounter";
        counter.arguments = {mparser::sdk::Value::scalar(2)};
        counter.requestedOutputCount = 1;
        require(std::abs(scalar(session.execute(counter).output(0)) - 2.0) <
                    kTolerance,
                "first session invocation differs");
        counter.arguments = {mparser::sdk::Value::scalar(3)};
        const auto counterValue = scalar(session.execute(counter).output(0));
        require(std::abs(counterValue - 5.0) < kTolerance,
                "persistent session invocation differs");

        const auto hostModule = mparser::sdk::Module::compile(
            R"(formatted = sprintf("value=%d", 42);
disp(formatted)
written = fprintf("pi=%.1f\n", 3.14);
40 + 2
41 + 2;
)",
            "installed_host_cpp.m",
            mparser::sdk::SourceLoadOptions{});
        require(hostModule.isValid(), "host script compilation failed");
        const auto metadata = hostModule.sourceMetadata();
        require(metadata.size() == 1 &&
                    metadata.front().name.ends_with(
                        "installed_host_cpp.m") &&
                    metadata.front().kind ==
                        mparser::sdk::SourceKind::Script &&
                    metadata.front().hasTopLevelStatements &&
                    !metadata.front().pureFunctionFile,
                "host script metadata differs");

        std::vector<mparser::sdk::OutputEvent> observedOutput;
        mparser::sdk::Invocation hostInvocation;
        hostInvocation.outputSink = [&observedOutput](const auto& event) {
            observedOutput.push_back(event);
            return true;
        };
        const auto hostResult = hostModule.execute(hostInvocation);
        const auto outputEvents = hostResult.outputEvents();
        const auto expressions = hostResult.topLevelExpressions();
        require(hostResult.succeeded() && outputEvents.size() == 2 &&
                    observedOutput.size() == 2 &&
                    outputEvents[0].kind ==
                        mparser::sdk::OutputKind::Display &&
                    outputEvents[0].text == "value=42\n\n" &&
                    outputEvents[0].sequence == 0 &&
                    outputEvents[1].kind ==
                        mparser::sdk::OutputKind::StandardOutput &&
                    outputEvents[1].text == "pi=3.1\n" &&
                    outputEvents[1].sequence == 1 &&
                    observedOutput[0].text == outputEvents[0].text &&
                    observedOutput[1].text == outputEvents[1].text,
                "host output events differ");
        require(expressions.size() == 2 &&
                    std::abs(scalar(expressions[0].value) - 42.0) <
                        kTolerance &&
                    !expressions[0].outputSuppressed &&
                    expressions[0].sequence == 2 &&
                    std::abs(scalar(expressions[1].value) - 43.0) <
                        kTolerance &&
                    expressions[1].outputSuppressed &&
                    expressions[1].sequence == 3 &&
                    expressions[1].source &&
                    expressions[1].source->sourceName.ends_with(
                        "installed_host_cpp.m"),
                "host top-level expressions differ");

        const auto randomModule = mparser::sdk::Module::compile(R"(
function value = draw()
value = rand();
end
)", "installed_system_context_cpp.m");
        mparser::sdk::SystemContextOptions systemOptions;
        systemOptions.capabilities =
            mparser::sdk::SystemCapability::Random;
        systemOptions.rootDirectory = ".";
        auto systemContext =
            mparser::sdk::SystemContext::rootedNative(systemOptions);
        require(mparser::sdk::hasSystemCapability(
                    systemContext.capabilities(),
                    mparser::sdk::SystemCapability::Random),
                "system context capability differs");
        mparser::sdk::Invocation draw;
        draw.entryFunction = "draw";
        draw.requestedOutputCount = 1;
        const double statelessRandom =
            scalar(randomModule.execute(draw, systemContext).output(0));
        auto randomSession = randomModule.createSession(systemContext);
        systemContext = {};
        const double sessionRandom =
            scalar(randomSession.execute(draw).output(0));
        require(statelessRandom >= 0.0 && statelessRandom < 1.0 &&
                    sessionRandom >= 0.0 && sessionRandom < 1.0 &&
                    statelessRandom != sessionRandom,
                "system context random/session lifetime differs");

        std::cout << "installed-cpp-consumer = "
                  << version.major << '.' << version.minor << '.'
                  << version.patch << ',' << scalar(retainedTotal) << ','
                  << counterValue << ",abi-generation-"
                  << mparser::sdk::abiGeneration() << "-revision-"
                  << mparser::sdk::abiRevision()
                  << ",cpp-api-" << sourceApiVersion.major << '.'
                  << sourceApiVersion.minor
                  << ",host-output-2-2"
                  << ",system-context-rooted-retained\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
