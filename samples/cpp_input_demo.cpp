#include "mparser/cpp_api.hpp"

#include <array>
#include <iostream>
#include <string>

int main() {
    using namespace mparser::sdk;
    try {
        const auto module = Module::compile(
            "function out=read_value(); x=4; n=input('Value: '); "
            "keyboard; out=n*x; end", "input_demo.m");
        SystemContextOptions contextOptions;
        contextOptions.rootDirectory = ".";
        contextOptions.capabilities = SystemCapability::DynamicEvaluation;
        auto session = module.createSession(SystemContext::rootedNative(contextOptions));
        const std::array<std::string, 3> replies{"x+3", "x=6;", "dbcont"};
        size_t next = 0;
        bool pending = true;
        Invocation invocation;
        invocation.entryFunction = "read_value";
        invocation.requestedOutputCount = 1;
        invocation.inputSource = [&](const InputRequest& request) {
            // A UI host can return Pending until its user submits a complete line.
            if (pending) {
                pending = false;
                return InputResult{InputStatus::Pending, {}, {}};
            }
            if (next == replies.size()) {
                return InputResult{};
            }
            std::cout << request.prompt << replies[next] << '\n';
            return InputResult{InputStatus::Ready, replies[next++], {}};
        };
        const auto result = session.execute(invocation);
        if (!result.succeeded() || next != replies.size()) {
            std::cerr << "input SDK demo failed\n";
            return 1;
        }
        const auto output = result.output(0);
        const auto values = output.numericData<double>();
        if (values.size() != 1 || values.front() != 42) {
            return 1;
        }
        std::cout << "input sdk demo = pending,expression,keyboard,42\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
