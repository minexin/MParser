#include "mparser/cpp_api.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

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
        static_assert(sourceApiVersion.major == 2);
        static_assert(sourceApiVersion.minor == 0);
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

        std::cout << "cpp sdk = " << scalar(sumResult.output(0)) << ','
                  << scalar(counterResult.output(0)) << ",abi-"
                  << mparser::sdk::abiMajor() << '.'
                  << mparser::sdk::abiRevision() << ",cpp-"
                  << sourceApiVersion.major << '.'
                  << sourceApiVersion.minor << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
