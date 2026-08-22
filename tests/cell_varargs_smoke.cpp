#include "mparser/embedding/adaptive_module_runtime.h"
#include "mparser/embedding/compiled_module.h"

#include <cassert>
#include <cmath>
#include <iostream>

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

const std::string kVariadicSource = R"(function [count, first, second] = collect(seed, varargin)
count = nargin;
first = varargin{1};
second = varargin{2};
end

function [value, varargout] = spread(seed)
value = seed;
varargout{1} = nargin;
varargout{2} = nargout;
end

function [value, in_count, out_count] = local_spread()
[value, in_count, out_count] = spread(7);
end
)";

void runVariadicEntrySmoke() {
    const auto module = mparser::CompiledModule::compile(kVariadicSource);
    assert(module.valid());

    mparser::BytecodeVmOptions options;
    options.entryFunction = "collect";
    options.arguments = {number(10), number(20), number(30)};
    options.requestedOutputCount = 3;
    const auto result = module.invoke(options);
    assert(result.diagnostics.empty());
    assert(result.outputs.size() == 3);
    assert(result.outputNames.size() == 3);
    assert(result.outputNames[0] == "count");
    assertNumber(result.outputs[0], 3);
    assertNumber(result.outputs[1], 20);
    assertNumber(result.outputs[2], 30);
}

void runVarargoutAndLocalCallSmoke() {
    const auto module = mparser::CompiledModule::compile(kVariadicSource);
    assert(module.valid());

    mparser::BytecodeVmOptions options;
    options.entryFunction = "spread";
    options.arguments = {number(9)};
    options.requestedOutputCount = 3;
    const auto direct = module.invoke(options);
    assert(direct.diagnostics.empty());
    assert(direct.outputs.size() == 3);
    assert(direct.outputNames[0] == "value");
    assert(direct.outputNames[1] == "varargout1");
    assert(direct.outputNames[2] == "varargout2");
    assertNumber(direct.outputs[0], 9);
    assertNumber(direct.outputs[1], 1);
    assertNumber(direct.outputs[2], 3);

    options.entryFunction = "local_spread";
    options.arguments.clear();
    const auto local = module.invoke(options);
    assert(local.diagnostics.empty());
    assert(local.outputs.size() == 3);
    assertNumber(local.outputs[0], 7);
    assertNumber(local.outputs[1], 1);
    assertNumber(local.outputs[2], 3);
}

void runAdaptiveVariadicSmoke() {
    const auto module = mparser::CompiledModule::compile(kVariadicSource);
    assert(module.valid());

    mparser::AdaptiveModuleRuntime runtime(
        module, mparser::AdaptiveModuleRuntimeOptions{4, 2});
    const auto first = runtime.invoke(
        "spread", {number(5)}, 1);
    const auto expanded = runtime.invoke(
        "spread", {number(5)}, 3);
    assert(first.adaptive.runtime.diagnostics.empty());
    assert(expanded.adaptive.runtime.diagnostics.empty());
    assert(first.adaptive.runtime.outputs.size() == 1);
    assert(expanded.adaptive.runtime.outputs.size() == 3);
    assertNumber(expanded.adaptive.runtime.outputs[2], 3);
}

} // namespace

int main() {
    runVariadicEntrySmoke();
    runVarargoutAndLocalCallSmoke();
    runAdaptiveVariadicSmoke();
    std::cout << "cell and variadic function smoke tests passed\n";
    return 0;
}
