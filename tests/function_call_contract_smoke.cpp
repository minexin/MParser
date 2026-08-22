#include "mparser/embedding/adaptive_module_runtime.h"
#include "mparser/embedding/compiled_module.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

void assertNumber(const mparser::RuntimeValue& value, double expected) {
    assert(value.kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value.number - expected) < 1e-9);
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

const std::string kEntrySource = R"(function [total, input_count, output_count] = contract(x, n)
total = 0;
for i = 1:n
    total = total + x * i;
end
input_count = nargin;
output_count = nargout;
end
)";

mparser::BytecodeVmResult invoke(
    const mparser::CompiledModule& module,
    std::optional<size_t> requestedOutputCount) {
    mparser::BytecodeVmOptions options;
    options.entryFunction = "contract";
    options.arguments = {number(2), number(4)};
    options.requestedOutputCount = requestedOutputCount;
    return module.invoke(options);
}

void runTopLevelContractSmoke() {
    auto module = mparser::CompiledModule::compile(kEntrySource);
    assert(module.valid());

    const auto all = invoke(module, std::nullopt);
    assert(all.diagnostics.empty());
    assert(all.requestedOutputCount == 3);
    assert(all.outputs.size() == 3);
    assert(all.outputNames.size() == 3);
    assertNumber(all.outputs[0], 20);
    assertNumber(all.outputs[1], 2);
    assertNumber(all.outputs[2], 3);

    const auto zero = invoke(module, 0);
    assert(zero.diagnostics.empty());
    assert(zero.requestedOutputCount == 0);
    assert(zero.outputs.empty());
    assert(zero.outputNames.empty());
    assertNumber(*findVariable(zero, "nargin"), 2);
    assertNumber(*findVariable(zero, "nargout"), 0);
    assertNumber(*findVariable(zero, "output_count"), 0);

    const auto one = invoke(module, 1);
    assert(one.diagnostics.empty());
    assert(one.requestedOutputCount == 1);
    assert(one.outputs.size() == 1);
    assert(one.outputNames.size() == 1);
    assert(one.outputNames[0] == "total");
    assertNumber(one.outputs[0], 20);
    assertNumber(*findVariable(one, "output_count"), 1);

    const auto two = invoke(module, 2);
    assert(two.diagnostics.empty());
    assert(two.outputs.size() == 2);
    assertNumber(two.outputs[0], 20);
    assertNumber(two.outputs[1], 2);
    assertNumber(*findVariable(two, "output_count"), 2);

    const auto tooMany = invoke(module, 4);
    assert(tooMany.outputs.empty());
    assert(tooMany.diagnostics.size() == 1);
    assert(tooMany.diagnostics[0].message ==
           "function output count mismatch for: contract");
}

void runLocalCallContractSmoke() {
    auto module = mparser::CompiledModule::compile(R"(
function [single_code, pair_code, pair_in, pair_out] = local_contract()
single_code = probe(7);
[pair_code, pair_in, pair_out] = probe(7);
end

function [code, in_count, out_count] = probe(x)
code = nargin * 10 + nargout;
in_count = nargin;
out_count = nargout;
end
)");
    assert(module.valid());

    mparser::BytecodeVmOptions options;
    options.entryFunction = "local_contract";
    options.requestedOutputCount = 4;
    const auto result = module.invoke(options);
    assert(result.diagnostics.empty());
    assert(result.outputs.size() == 4);
    assertNumber(result.outputs[0], 11);
    assertNumber(result.outputs[1], 13);
    assertNumber(result.outputs[2], 1);
    assertNumber(result.outputs[3], 3);
}

void runAdaptiveContractSmoke() {
    auto module = mparser::CompiledModule::compile(kEntrySource);
    mparser::AdaptiveBytecodeVmOptions options;
    options.hotLoopThreshold = 8;
    options.entryFunction = "contract";
    options.arguments = {number(2), number(4)};
    options.requestedOutputCount = 1;
    auto session = module.createAdaptiveSession(options);

    const auto first = session.run();
    assert(first.runtime.outputs.size() == 1);
    assertNumber(*findVariable(first.runtime, "nargout"), 1);

    session.setRequestedOutputCount(3);
    const auto promoted = session.run();
    assert(promoted.promotionOccurred);
    assert(promoted.runtime.outputs.size() == 3);
    assertNumber(promoted.runtime.outputs[2], 3);

    session.setRequestedOutputCount(2);
    const auto typed = session.run();
    assert(typed.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(typed.runtime.outputs.size() == 2);
    assertNumber(*findVariable(typed.runtime, "nargout"), 2);
    assert(session.requestedOutputCount() == 2);
}

void runModuleRuntimeContractSmoke() {
    auto module = mparser::CompiledModule::compile(kEntrySource);
    mparser::AdaptiveModuleRuntime runtime(
        module, mparser::AdaptiveModuleRuntimeOptions{8, 2});

    const auto one = runtime.invoke(
        "contract", {number(2), number(4)}, 1);
    const auto three = runtime.invoke(
        "contract", {number(2), number(4)}, 3);
    const auto two = runtime.invoke(
        "contract", {number(2), number(4)}, 2);
    assert(one.sessionCreated);
    assert(!three.sessionCreated);
    assert(three.adaptive.promotionOccurred);
    assert(two.adaptive.tier == mparser::AdaptiveBytecodeTier::Typed);
    assert(one.adaptive.runtime.outputs.size() == 1);
    assert(three.adaptive.runtime.outputs.size() == 3);
    assert(two.adaptive.runtime.outputs.size() == 2);
    assertNumber(*findVariable(two.adaptive.runtime, "nargout"), 2);
}

} // namespace

int main() {
    runTopLevelContractSmoke();
    runLocalCallContractSmoke();
    runAdaptiveContractSmoke();
    runModuleRuntimeContractSmoke();
    std::cout << "function call contract smoke tests passed\n";
    return 0;
}
