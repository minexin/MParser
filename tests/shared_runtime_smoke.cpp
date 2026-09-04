#include "mparser/cpp_api.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::ApiError;
using mparser::sdk::Backend;
using mparser::sdk::Invocation;
using mparser::sdk::InvocationStatus;
using mparser::sdk::Module;
using mparser::sdk::NamedValue;
using mparser::sdk::Runtime;
using mparser::sdk::SourceUnit;
using mparser::sdk::StopReason;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

constexpr double kTolerance = 1e-9;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double scalar(const Value& value) {
    require(value.kind() == ValueKind::Numeric,
            "expected a numeric scalar");
    const auto data = value.numericData();
    require(data.size() == 1, "expected one numeric element");
    return data.front();
}

void requireScalar(const Value& value, double expected,
                   const std::string& message) {
    require(std::abs(scalar(value) - expected) < kTolerance, message);
}

bool hasDiagnostic(const mparser::sdk::Result& result,
                   const std::string& text) {
    for (const auto& diagnostic : result.diagnostics()) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

mparser::sdk::Result invoke(
    Runtime& runtime, const Module& module, std::string entry,
    std::vector<Value> arguments = {}, Backend backend = Backend::Bytecode,
    std::size_t outputCount = 1) {
    Invocation invocation;
    invocation.entryFunction = std::move(entry);
    invocation.arguments = std::move(arguments);
    invocation.requestedOutputCount = outputCount;
    invocation.backend = backend;
    return runtime.execute(module, invocation);
}

Value requireOutput(mparser::sdk::Result result,
                    const std::string& context) {
    if (!result.succeeded()) {
        std::string message = context + " did not succeed";
        for (const auto& diagnostic : result.diagnostics()) {
            message += " [" + diagnostic.identifier + ": " +
                       diagnostic.message + "]";
        }
        throw std::runtime_error(message);
    }
    require(result.outputCount() == 1,
            context + " did not produce one output");
    return result.output(0);
}

Module compileProducer() {
    const std::array<SourceUnit, 3> sources{
        SourceUnit{
            "shared_runtime_producer.m",
            R"(function out = makeAffine(factor)
offset = 2;
out = @(x) x * factor + offset;
end

function out = makeNamedHandle()
out = @increment;
end

function out = makeFailureHandle()
out = @failFromProducer;
end

function out = makeOutputHandle()
out = @emitFromProducer;
end

function out = increment(value)
out = value + 1;
end

function out = failFromProducer()
error("SharedRuntime:ProducerFailure", "producer failed");
out = 0;
end

function out = emitFromProducer()
disp("producer-output");
out = 7;
end

function out = writeGlobal(value)
global shared_value
shared_value = value;
out = shared_value;
end

function out = nextPersistent(step)
persistent count
if isempty(count)
    count = 0;
end
count = count + step;
out = count;
end

function out = makeMeter(value)
out = SharedMeter(value);
end

function out = makeValueMeter(value)
out = ValueMeter(value);
end
)"},
        SourceUnit{
            "SharedMeter.m",
            R"(classdef SharedMeter < handle
    properties
        Value
    end
    properties (Access = private)
        Secret
    end
    methods
        function obj = SharedMeter(value)
            obj.Value = value;
            obj.Secret = value + 100;
        end
        function out = add(obj, delta)
            obj.Value = obj.Value + delta;
            out = obj.Value;
        end
        function out = read(obj)
            out = obj.Value;
        end
    end
    methods (Access = private)
        function out = secret(obj)
            out = obj.Value + 1000;
        end
    end
end
)"},
        SourceUnit{
            "ValueMeter.m",
            R"(classdef ValueMeter
    properties
        Value
    end
    methods
        function obj = ValueMeter(value)
            obj.Value = value;
        end
    end
end
)"}};
    auto module = Module::compile(sources);
    require(module.isValid(), "producer module did not compile");
    return module;
}

Module compileConsumer() {
    auto module = Module::compile(
        R"(function out = applyHandle(fn, value)
out = fn(value);
end

function out = applyNoArg(fn)
out = fn();
end

function out = applyCell(box, value)
fn = box{1};
out = fn(value);
end

function out = applyStruct(box, value)
fn = box.Callback;
out = fn(value);
end

function out = readGlobal()
global shared_value
out = shared_value;
end

function out = mutateMeter(obj, delta)
out = obj.add(delta);
end

function out = readMeter(obj)
out = obj.read();
end

function out = readMeterProperty(obj)
out = obj.Value;
end

function out = readPrivateProperty(obj)
out = obj.Secret;
end

function out = assignMeterProperty(obj, value)
obj.Value = value;
out = obj;
end

function out = assignPrivateProperty(obj, value)
obj.Secret = value;
out = obj;
end

function out = callPrivate(obj)
out = obj.secret();
end

function out = roundTrip(proxy, callback, value)
out = proxy(callback, value);
end

function out = spinThrough(callback)
out = callback();
end
)",
        "shared_runtime_consumer.m");
    require(module.isValid(), "consumer module did not compile");
    return module;
}

Module compileRelay() {
    auto module = Module::compile(
        R"(function out = makeRelay()
out = @relay;
end

function out = relay(callback, value)
out = callback(value) + 10;
end
)",
        "shared_runtime_relay.m");
    require(module.isValid(), "relay module did not compile");
    return module;
}

Module compileSpinner() {
    auto module = Module::compile(
        R"(function out = makeSpinner()
out = @spin;
end

function out = spin()
out = 0;
while 1
    out = out + 1;
end
end
)",
        "shared_runtime_spinner.m");
    require(module.isValid(), "spinner module did not compile");
    return module;
}

void requireOwnerMismatch(const auto& operation,
                          const std::string& context) {
    try {
        operation();
    } catch (const ApiError& error) {
        require(error.status() == MPARSER_API_STATUS_OWNER_MISMATCH,
                context + " returned the wrong API status");
        return;
    }
    throw std::runtime_error(context + " did not reject mixed ownership");
}

void runSharedRuntimeSmoke() {
    const Module producer = compileProducer();
    const Module consumer = compileConsumer();
    const Module relay = compileRelay();
    const Module spinner = compileSpinner();
    Runtime runtime = Runtime::create();

    Value closure = requireOutput(
        invoke(runtime, producer, "makeAffine", {Value::scalar(3)}),
        "closure factory");
    require(closure.isModuleBound(),
            "closure was not retained by the shared runtime");
    for (const Backend backend :
         {Backend::Bytecode, Backend::Portable, Backend::Automatic}) {
        requireScalar(
            requireOutput(
                invoke(runtime, consumer, "applyHandle",
                       {closure, Value::scalar(4)}, backend),
                "cross-module closure"),
            14, "cross-module closure returned the wrong value");
    }
#if MPARSER_SHARED_RUNTIME_NATIVE_AVAILABLE
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "applyHandle",
                   {closure, Value::scalar(4)}, Backend::Native),
            "native cross-module closure"),
        14, "native cross-module closure returned the wrong value");
#endif

    requireOwnerMismatch(
        [&] {
            Invocation invocation;
            invocation.entryFunction = "applyHandle";
            invocation.arguments = {closure, Value::scalar(1)};
            invocation.requestedOutputCount = 1;
            (void)consumer.execute(invocation);
        },
        "standalone module");
    Runtime otherRuntime = Runtime::create();
    requireOwnerMismatch(
        [&] {
            (void)invoke(otherRuntime, consumer, "applyHandle",
                         {closure, Value::scalar(1)});
        },
        "different shared runtime");

    const std::array<std::size_t, 2> oneCell{1, 1};
    const std::array<Value, 1> cellElements{closure};
    const Value cell = Value::cell(oneCell, cellElements);
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "applyCell",
                   {cell, Value::scalar(5)}),
            "cell-contained closure"),
        17, "cell-contained closure returned the wrong value");
    const std::array<NamedValue, 1> fields{
        NamedValue{"Callback", closure}};
    const Value structure = Value::structure(fields);
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "applyStruct",
                   {structure, Value::scalar(6)}),
            "struct-contained closure"),
        20, "struct-contained closure returned the wrong value");

    Value otherClosure = requireOutput(
        invoke(otherRuntime, producer, "makeAffine", {Value::scalar(2)}),
        "other-runtime closure factory");
    requireOwnerMismatch(
        [&] {
            const std::array<Value, 2> mixed{closure, otherClosure};
            const std::array<std::size_t, 2> shape{1, 2};
            (void)Value::cell(shape, mixed);
        },
        "mixed-runtime cell");

    requireScalar(
        requireOutput(
            invoke(runtime, producer, "writeGlobal", {Value::scalar(9)}),
            "global writer"),
        9, "global writer returned the wrong value");
    requireScalar(
        requireOutput(invoke(runtime, consumer, "readGlobal"),
        "global reader"),
        9, "global state was not shared across modules");
    runtime.clearGlobal("shared_value");
    const Value clearedGlobal = requireOutput(
        invoke(runtime, consumer, "readGlobal"), "cleared global reader");
    require(clearedGlobal.kind() == ValueKind::Numeric &&
                clearedGlobal.elementCount() == 0,
            "clearGlobal did not clear shared global state");

    requireScalar(
        requireOutput(
            invoke(runtime, producer, "nextPersistent", {Value::scalar(2)}),
            "persistent first call"),
        2, "persistent first call returned the wrong value");
    requireScalar(
        requireOutput(
            invoke(runtime, producer, "nextPersistent", {Value::scalar(3)}),
            "persistent second call"),
        5, "persistent state was not retained");
    runtime.reset();
    requireScalar(
        requireOutput(
            invoke(runtime, producer, "nextPersistent", {Value::scalar(4)}),
            "persistent call after reset"),
        4, "runtime reset did not clear persistent state");

    Value meter = requireOutput(
        invoke(runtime, producer, "makeMeter", {Value::scalar(10)}),
        "meter factory");
    require(meter.isModuleBound(),
            "user object was not retained by the shared runtime");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "mutateMeter",
                   {meter, Value::scalar(5)}),
            "cross-module object mutation"),
        15, "cross-module object mutation returned the wrong value");
    requireScalar(
        requireOutput(invoke(runtime, consumer, "readMeter", {meter}),
                      "cross-module object read"),
        15, "handle-object mutation did not persist");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "readMeterProperty", {meter}),
            "cross-module public property read"),
        15, "cross-module public property returned the wrong value");
    Value updatedMeter = requireOutput(
        invoke(runtime, consumer, "assignMeterProperty",
               {meter, Value::scalar(23)}),
        "cross-module handle property assignment");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "readMeterProperty", {updatedMeter}),
            "updated handle property read"),
        23, "cross-module handle property assignment returned the wrong value");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "readMeterProperty", {meter}),
            "aliased handle property read"),
        23, "cross-module handle property assignment did not update aliases");
    const auto privateProperty =
        invoke(runtime, consumer, "readPrivateProperty", {meter});
    require(privateProperty.status() == InvocationStatus::RuntimeFailed,
            "cross-module private property read unexpectedly succeeded");
    require(hasDiagnostic(privateProperty, "property get access is denied"),
            "private property rejection diagnostic is missing");
    const auto privateAssignment = invoke(
        runtime, consumer, "assignPrivateProperty",
        {meter, Value::scalar(99)});
    require(privateAssignment.status() == InvocationStatus::RuntimeFailed,
            "cross-module private property assignment unexpectedly succeeded");
    require(hasDiagnostic(privateAssignment,
                          "property set access is denied"),
            "private property assignment rejection diagnostic is missing");
    const auto privateCall =
        invoke(runtime, consumer, "callPrivate", {meter});
    require(privateCall.status() == InvocationStatus::RuntimeFailed,
            "cross-module private method call unexpectedly succeeded");
    require(hasDiagnostic(privateCall, "method access is denied"),
            "private method rejection diagnostic is missing");

    Value valueMeter = requireOutput(
        invoke(runtime, producer, "makeValueMeter", {Value::scalar(31)}),
        "value meter factory");
    Value updatedValueMeter = requireOutput(
        invoke(runtime, consumer, "assignMeterProperty",
               {valueMeter, Value::scalar(42)}),
        "cross-module value property assignment");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "readMeterProperty",
                   {updatedValueMeter}),
            "updated value property read"),
        42, "cross-module value property assignment returned the wrong value");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "readMeterProperty", {valueMeter}),
            "original value property read"),
        31, "cross-module value property assignment mutated the original value");

    Value named = requireOutput(
        invoke(runtime, producer, "makeNamedHandle"),
        "named handle factory");
    Value relayHandle = requireOutput(
        invoke(runtime, relay, "makeRelay"), "relay handle factory");
    requireScalar(
        requireOutput(
            invoke(runtime, consumer, "roundTrip",
                   {relayHandle, named, Value::scalar(5)}),
            "reentrant callback chain"),
        16, "reentrant callback chain returned the wrong value");

    Value failureHandle = requireOutput(
        invoke(runtime, producer, "makeFailureHandle"),
        "failure handle factory");
    const auto failed =
        invoke(runtime, consumer, "applyNoArg", {failureHandle});
    require(failed.status() == InvocationStatus::RuntimeFailed,
            "cross-module failure unexpectedly succeeded");
    bool foundProducerDiagnostic = false;
    for (const auto& diagnostic : failed.diagnostics()) {
        if (diagnostic.identifier == "SharedRuntime:ProducerFailure" &&
            diagnostic.source &&
            diagnostic.source->sourceName ==
                "shared_runtime_producer.m") {
            foundProducerDiagnostic = true;
        }
    }
    require(foundProducerDiagnostic,
            "cross-module diagnostic lost producer source provenance");

    Value outputHandle = requireOutput(
        invoke(runtime, producer, "makeOutputHandle"),
        "output handle factory");
    const auto emitted =
        invoke(runtime, consumer, "applyNoArg", {outputHandle});
    require(emitted.succeeded(),
            "cross-module output callback did not succeed");
    bool foundProducerOutput = false;
    for (const auto& event : emitted.outputEvents()) {
        if (event.text.find("producer-output") != std::string::npos &&
            event.source &&
            event.source->sourceName ==
                "shared_runtime_producer.m") {
            foundProducerOutput = true;
        }
    }
    require(foundProducerOutput,
            "cross-module output lost producer source provenance");

    Value spinnerHandle = requireOutput(
        invoke(runtime, spinner, "makeSpinner"),
        "spinner handle factory");
    Invocation limited;
    limited.entryFunction = "spinThrough";
    limited.arguments = {spinnerHandle};
    limited.requestedOutputCount = 1;
    limited.backend = Backend::Bytecode;
    limited.limits.maximumInstructionCount = 64;
    const auto stopped = runtime.execute(consumer, limited);
    require(!stopped.succeeded(),
            "cross-module instruction limit did not stop execution");
    require(stopped.executionSummary().stopReason ==
                StopReason::InstructionLimit,
            "cross-module instruction limit reported the wrong stop reason");
}

} // namespace

int main() {
    try {
        runSharedRuntimeSmoke();
        std::cout << "shared runtime smoke = closure,ownership,composition,"
                     "state,object,reentrant,limits\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shared runtime smoke failed: " << error.what()
                  << '\n';
        return 1;
    }
}
