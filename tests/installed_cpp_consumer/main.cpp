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

        std::cout << "installed-cpp-consumer = "
                  << version.major << '.' << version.minor << '.'
                  << version.patch << ',' << scalar(retainedTotal) << ','
                  << counterValue << ",abi-" << mparser::sdk::abiMajor()
                  << '.' << mparser::sdk::abiRevision()
                  << ",cpp-" << sourceApiVersion.major << '.'
                  << sourceApiVersion.minor << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
