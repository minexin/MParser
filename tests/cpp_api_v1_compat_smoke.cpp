#include "mparser/cpp_api.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        constexpr auto sourceApiVersion =
            mparser::sdk::sourceApiVersion();
        static_assert(sourceApiVersion.major == 1);
        static_assert(sourceApiVersion.minor == 0);

        const auto module = mparser::sdk::Module::compile(R"(
function out = frozenCppApi(value)
out = value + 2;
end
)", "cpp_api_1_0_snapshot.m");
        if (!module.isValid()) {
            throw std::runtime_error("snapshot module compilation failed");
        }
        mparser::sdk::Invocation invocation;
        invocation.entryFunction = "frozenCppApi";
        invocation.arguments = {mparser::sdk::Value::scalar(40)};
        invocation.requestedOutputCount = 1;
        const auto result = module.execute(invocation);
        if (!result.succeeded()) {
            throw std::runtime_error("snapshot invocation failed");
        }
        const auto outputValue = result.output(0);
        const auto output = outputValue.numericData();
        if (output.size() != 1 ||
            std::abs(output.front() - 42.0) > 1e-9) {
            throw std::runtime_error("snapshot invocation failed");
        }
        std::cout << "cpp api 1.0 snapshot compatibility = 42,abi-"
                  << mparser::sdk::abiMajor() << '.'
                  << mparser::sdk::abiRevision() << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
