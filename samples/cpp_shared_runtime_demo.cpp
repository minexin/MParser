#include "mparser/cpp_api.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::Invocation;
using mparser::sdk::Module;
using mparser::sdk::Result;
using mparser::sdk::Runtime;
using mparser::sdk::SourceUnit;
using mparser::sdk::Value;

Result invoke(Runtime& runtime, const Module& module, std::string entry,
              std::vector<Value> arguments = {}) {
    Invocation request;
    request.entryFunction = std::move(entry);
    request.arguments = std::move(arguments);
    request.requestedOutputCount = 1;
    return runtime.execute(module, request);
}

Value output(Result result, const char* operation) {
    if (!result.succeeded() || result.outputCount() != 1) {
        std::string message = std::string(operation) + " failed";
        for (const auto& diagnostic : result.diagnostics()) {
            message += ": " + diagnostic.message;
        }
        throw std::runtime_error(message);
    }
    return result.output(0);
}

double scalar(const Value& value) {
    const auto values = value.numericData<double>();
    if (values.size() != 1) {
        throw std::runtime_error("expected a numeric scalar");
    }
    return values.front();
}

void requireScalar(const Value& value, double expected,
                   const char* operation) {
    if (std::abs(scalar(value) - expected) > 1e-9) {
        throw std::runtime_error(
            std::string(operation) + " returned an unexpected value");
    }
}

} // namespace

int main() {
    try {
        const std::array<SourceUnit, 2> producerSources{
            SourceUnit{
                "shared_runtime_producer.m",
                R"MATLAB(function out = makeAffine(factor)
offset = 2;
out = @(x) x * factor + offset;
end

function out = makeCounter(value)
out = SharedCounter(value);
end

function out = writeShared(value)
global shared_value
shared_value = value;
out = shared_value;
end
)MATLAB"},
            SourceUnit{
                "SharedCounter.m",
                R"MATLAB(classdef SharedCounter < handle
    properties
        Value
    end
    methods
        function obj = SharedCounter(value)
            obj.Value = value;
        end
        function out = add(obj, delta)
            obj.Value = obj.Value + delta;
            out = obj.Value;
        end
    end
end
)MATLAB"}};
        Module producer = Module::compile(producerSources);
        const Module consumer = Module::compile(
            R"MATLAB(function out = apply(fn, value)
out = fn(value);
end

function out = bump(counter, delta)
out = counter.add(delta);
end

function out = readShared()
global shared_value
out = shared_value;
end
)MATLAB",
            "shared_runtime_consumer.m");
        if (!producer.isValid() || !consumer.isValid()) {
            throw std::runtime_error("shared runtime modules did not compile");
        }

        Runtime runtime = Runtime::create();
        Value closure = output(
            invoke(runtime, producer, "makeAffine", {Value::scalar(3)}),
            "create closure");
        Value counter = output(
            invoke(runtime, producer, "makeCounter", {Value::scalar(10)}),
            "create counter");
        requireScalar(
            output(invoke(runtime, producer, "writeShared",
                          {Value::scalar(9)}),
                   "write global"),
            9, "write global");

        producer = {};

        const Value closureResult = output(
            invoke(runtime, consumer, "apply",
                   {closure, Value::scalar(4)}),
            "cross-module closure");
        const Value firstCounter = output(
            invoke(runtime, consumer, "bump",
                   {counter, Value::scalar(5)}),
            "first object call");
        const Value secondCounter = output(
            invoke(runtime, consumer, "bump",
                   {counter, Value::scalar(2)}),
            "second object call");
        const Value shared = output(
            invoke(runtime, consumer, "readShared"), "read global");

        requireScalar(closureResult, 14, "cross-module closure");
        requireScalar(firstCounter, 15, "first object call");
        requireScalar(secondCounter, 17, "second object call");
        requireScalar(shared, 9, "read global");

        std::cout << "shared runtime sdk = closure-14,object-15-17,global-9,"
                  << "abi-generation-" << mparser::sdk::abiGeneration()
                  << "-revision-" << mparser::sdk::abiRevision() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
