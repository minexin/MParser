#include "mparser/cpp_api.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

double scalar(const mparser::sdk::Value& value) {
    const auto data = value.numericData();
    if (data.size() != 1) {
        throw std::runtime_error("expected a scalar result");
    }
    return data.front();
}

} // namespace

int main() {
    try {
        constexpr auto sourceApiVersion =
            mparser::sdk::sourceApiVersion();
        static_assert(sourceApiVersion.major == 1);
        static_assert(sourceApiVersion.minor == 2);
        const auto module = mparser::sdk::Module::compile(R"(
function total = sumTo(limit)
total = 0;
for i = 1:limit
    total = total + i;
end
end

function out = nextCounter(step)
persistent count
if isempty(count)
    count = 0;
end
count = count + step;
out = count;
end
)", "cpp_embedding_demo.m");
        if (!module.isValid()) {
            for (const auto& diagnostic : module.diagnostics()) {
                std::cerr << diagnostic.message << '\n';
            }
            return 1;
        }

        mparser::sdk::Invocation sumRequest;
        sumRequest.entryFunction = "sumTo";
        sumRequest.arguments = {mparser::sdk::Value::scalar(100)};
        sumRequest.requestedOutputCount = 1;
        const auto sumResult = module.execute(sumRequest);
        if (!sumResult.succeeded()) {
            return 1;
        }

        auto session = module.createSession();
        mparser::sdk::Invocation counterRequest;
        counterRequest.entryFunction = "nextCounter";
        counterRequest.arguments = {mparser::sdk::Value::scalar(2)};
        counterRequest.requestedOutputCount = 1;
        static_cast<void>(session.execute(counterRequest));
        counterRequest.arguments = {mparser::sdk::Value::scalar(3)};
        const auto counterResult = session.execute(counterRequest);
        if (!counterResult.succeeded()) {
            return 1;
        }

        const auto hostModule = mparser::sdk::Module::compile(
            R"(formatted = sprintf("value=%d", 42);
disp(formatted)
written = fprintf("pi=%.1f\n", 3.14);
40 + 2
41 + 2;
)",
            "cpp_host_integration_demo.m",
            mparser::sdk::SourceLoadOptions{});
        const auto metadata = hostModule.sourceMetadata();
        if (!hostModule.isValid() || metadata.size() != 1 ||
            metadata.front().kind != mparser::sdk::SourceKind::Script ||
            !metadata.front().hasTopLevelStatements ||
            metadata.front().pureFunctionFile) {
            return 1;
        }

        std::vector<mparser::sdk::OutputEvent> observedOutput;
        mparser::sdk::Invocation hostRequest;
        hostRequest.outputSink = [&observedOutput](const auto& event) {
            observedOutput.push_back(event);
            std::cout << event.text;
            return true;
        };
        const auto hostResult = hostModule.execute(hostRequest);
        const auto outputEvents = hostResult.outputEvents();
        const auto expressions = hostResult.topLevelExpressions();
        if (!hostResult.succeeded() || observedOutput.size() != 2 ||
            outputEvents.size() != 2 || expressions.size() != 2 ||
            outputEvents[0].sequence != 0 ||
            outputEvents[1].sequence != 1 ||
            expressions[0].sequence != 2 ||
            expressions[0].outputSuppressed ||
            expressions[1].sequence != 3 ||
            !expressions[1].outputSuppressed ||
            std::abs(scalar(expressions[1].value) - 43.0) > 1e-9) {
            return 1;
        }

        std::cout << "cpp sdk = " << scalar(sumResult.output(0)) << ','
                  << scalar(counterResult.output(0))
                  << ",abi-generation-"
                  << mparser::sdk::abiGeneration() << "-revision-"
                  << mparser::sdk::abiRevision() << ",cpp-api-"
                  << sourceApiVersion.major << '.'
                  << sourceApiVersion.minor
                  << ",host-output-2-2\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
