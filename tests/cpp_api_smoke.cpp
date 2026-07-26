#include "mparser/cpp_api.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using mparser::sdk::ApiError;
using mparser::sdk::Backend;
using mparser::sdk::CancellationToken;
using mparser::sdk::DiagnosticPhase;
using mparser::sdk::Invocation;
using mparser::sdk::InvocationStatus;
using mparser::sdk::Module;
using mparser::sdk::NamedValue;
using mparser::sdk::NumericClass;
using mparser::sdk::Result;
using mparser::sdk::SourceLoadOptions;
using mparser::sdk::SourceUnit;
using mparser::sdk::StopReason;
using mparser::sdk::Value;
using mparser::sdk::ValueKind;

constexpr double kTolerance = 1e-9;

const std::string kModuleSource = R"(function [total, last] = accumulate(limit)
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

function out = emitWarning()
warning("Embed:Notice", "notice %d", 3);
out = 7;
end

function out = failNow()
error("Embed:Failure", "failed %d", 9);
out = 0;
end

function out = spin()
out = 0;
while 1
    out = out + 1;
end
end

function out = identity(value)
out = value;
end
)";

double scalar(const Value& value) {
    assert(value.kind() == ValueKind::Numeric);
    const auto data = value.numericData();
    assert(data.size() == 1);
    return data[0];
}

const NamedValue* findVariable(
    const std::vector<NamedValue>& variables,
    const std::string& name) {
    const auto found = std::find_if(
        variables.begin(), variables.end(),
        [&](const NamedValue& variable) {
            return variable.name == name;
        });
    return found == variables.end() ? nullptr : &*found;
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

void runValueSmoke() {
    const std::array<std::size_t, 2> matrixShape{2, 2};
    const std::array<double, 4> matrixData{1, 3, 2, 4};
    const auto matrix = Value::numericArray(
        NumericClass::Double, matrixShape, matrixData);
    auto matrixCopy = matrix;
    auto matrixMoved = std::move(matrixCopy);
    assert(!matrixCopy.hasValue());
    assert(matrix.kind() == ValueKind::Numeric);
    assert(matrix.numericClass() == NumericClass::Double);
    assert(matrix.dimensions() ==
           std::vector<std::size_t>({2, 2}));
    assert(matrix.elementCount() == 4);
    assert(std::equal(
        matrixMoved.numericData().begin(), matrixMoved.numericData().end(),
        matrixData.begin()));

    const std::array<double, 4> logicalData{0, 1, 1, 0};
    const auto logical = Value::numericArray(
        NumericClass::Logical, matrixShape, logicalData);
    assert(logical.numericClass() == NumericClass::Logical);

    const std::array<std::size_t, 2> rowShape{1, 2};
    const std::array<std::uint16_t, 2> characters{'A', 'B'};
    const auto character = Value::characterArray(
        rowShape, characters);
    assert(character.kind() == ValueKind::Character);
    assert(std::equal(
        character.characterData().begin(),
        character.characterData().end(), characters.begin()));

    const std::array<std::optional<std::u16string>, 2> strings{
        std::u16string(u"alpha"), std::nullopt};
    const auto text = Value::stringArray(rowShape, strings);
    assert(text.kind() == ValueKind::String);
    assert(text.stringElement(0) == std::u16string(u"alpha"));
    assert(!text.stringElement(1));

    const std::array<Value, 2> cells{
        Value::scalar(7), text};
    const auto cell = Value::cell(rowShape, cells);
    assert(cell.kind() == ValueKind::Cell);
    assert(std::abs(scalar(cell.cellElement(0)) - 7.0) <
           kTolerance);
    assert(cell.cellElement(1).stringElement(0) ==
           std::u16string(u"alpha"));

    const std::array<std::size_t, 2> scalarShape{1, 1};
    const std::array<std::optional<std::u16string>, 1> label{
        std::u16string(u"record")};
    const std::array<NamedValue, 2> fields{
        NamedValue{"value", Value::scalar(9)},
        NamedValue{
            "label", Value::stringArray(scalarShape, label)}};
    const auto structure = Value::structure(fields);
    assert(structure.kind() == ValueKind::Structure);
    assert(structure.structFieldNames() ==
           std::vector<std::string>({"value", "label"}));
    assert(std::abs(scalar(structure.structField(0, 0)) - 9.0) <
           kTolerance);
    assert(structure.structField(0, 1).stringElement(0) ==
           std::u16string(u"record"));

    assert(Value::missing().kind() == ValueKind::Missing);

    bool rejectedEmptyHandle = false;
    try {
        static_cast<void>(Value{}.numericData());
    } catch (const ApiError& error) {
        rejectedEmptyHandle =
            error.status() == MPARSER_API_STATUS_INVALID_ARGUMENT;
    }
    assert(rejectedEmptyHandle);
}

Result invoke(
    const Module& module, std::string entry,
    std::vector<Value> arguments = {},
    std::optional<std::size_t> outputCount = 1) {
    Invocation request;
    request.entryFunction = std::move(entry);
    request.arguments = std::move(arguments);
    request.requestedOutputCount = outputCount;
    return module.execute(request);
}

void runModuleSmoke(
    const std::string& entryPath,
    const std::string& libraryPath) {
    const auto module = Module::compile(
        kModuleSource, "cpp_api_smoke.m");
    assert(module.isValid());
    assert(module.sourceNames() ==
           std::vector<std::string>({"cpp_api_smoke.m"}));
    const auto functions = module.functionNames();
    assert(std::find(
               functions.begin(), functions.end(), "accumulate") !=
           functions.end());

    Invocation accumulate;
    accumulate.entryFunction = "accumulate";
    accumulate.arguments = {Value::scalar(100)};
    accumulate.requestedOutputCount = 2;
    accumulate.backend = Backend::Automatic;
    accumulate.collectProfile = true;
    auto accumulated = module.execute(accumulate);
    assert(accumulated.succeeded());
    assert(accumulated.status() == InvocationStatus::Succeeded);
    assert(accumulated.entryFunction() == "accumulate");
    assert(accumulated.requestedOutputCount() == 2);
    assert(accumulated.outputCount() == 2);
    assert(accumulated.outputName(0) == "total");
    assert(accumulated.outputName(1) == "last");
    assert(std::abs(scalar(accumulated.output(0)) - 5050.0) <
           kTolerance);
    assert(std::abs(scalar(accumulated.output(1)) - 100.0) <
           kTolerance);
    const auto summary = accumulated.executionSummary();
    assert(summary.profilingCollected);
    assert(summary.executedInstructionCount > 0);

    auto retainedOutput = accumulated.output(0);
    accumulated = Result{};
    assert(std::abs(scalar(retainedOutput) - 5050.0) <
           kTolerance);

    const auto warned = invoke(module, "emitWarning");
    assert(warned.succeeded());
    assert(hasDiagnostic(warned.diagnostics(), "Embed:Notice"));

    const auto failed = invoke(module, "failNow");
    assert(!failed.succeeded());
    assert(failed.status() == InvocationStatus::RuntimeFailed);
    assert(hasDiagnostic(failed.diagnostics(), "Embed:Failure"));

    Invocation limited;
    limited.entryFunction = "spin";
    limited.requestedOutputCount = 1;
    limited.limits.maximumInstructionCount = 64;
    const auto stopped = module.execute(limited);
    assert(!stopped.succeeded());
    assert(stopped.executionSummary().stopReason ==
           StopReason::InstructionLimit);

    CancellationToken token;
    const auto sharedToken = token;
    token.request();
    assert(token.requested() && sharedToken.requested());
    Invocation cancelled;
    cancelled.entryFunction = "spin";
    cancelled.requestedOutputCount = 1;
    cancelled.cancellationToken = token;
    const auto cancelledResult = module.execute(cancelled);
    assert(!cancelledResult.succeeded());
    assert(cancelledResult.executionSummary().stopReason ==
           StopReason::Cancelled);

    auto session = module.createSession();
    Invocation counter;
    counter.entryFunction = "nextCounter";
    counter.arguments = {Value::scalar(2)};
    counter.requestedOutputCount = 1;
    assert(std::abs(scalar(session.execute(counter).output(0)) - 2.0) <
           kTolerance);
    counter.arguments = {Value::scalar(3)};
    assert(std::abs(scalar(session.execute(counter).output(0)) - 5.0) <
           kTolerance);
    session.reset();
    counter.arguments = {Value::scalar(4)};
    assert(std::abs(scalar(session.execute(counter).output(0)) - 4.0) <
           kTolerance);

    const std::array<SourceUnit, 2> sources{
        SourceUnit{
            "entry.m",
            "function out = entry(value)\n"
            "out = helper(value);\nend\n"},
        SourceUnit{
            "helper.m",
            "function out = helper(value)\n"
            "out = value + 2;\nend\n"}};
    const auto sourceGraph = Module::compile(sources);
    assert(sourceGraph.isValid());
    assert(std::abs(scalar(invoke(
               sourceGraph, "entry", {Value::scalar(40)}).output(0)) -
           42.0) < kTolerance);

    const auto invalid = Module::compile(
        "function out = broken(\n", "invalid_cpp_api.m");
    assert(!invalid.isValid());
    const auto compilationDiagnostics = invalid.diagnostics();
    assert(!compilationDiagnostics.empty());
    assert(compilationDiagnostics.front().phase ==
           DiagnosticPhase::Compilation);
    assert(compilationDiagnostics.front().source);
    assert(compilationDiagnostics.front().source->sourceName ==
           "invalid_cpp_api.m");

    const auto loaded = Module::loadFile(
        entryPath, SourceLoadOptions{{libraryPath}});
    assert(loaded.isValid());
    assert(loaded.sourceNames().size() > 1);
    const auto loadedResult = loaded.execute();
    assert(loadedResult.succeeded());
    const auto variables = loadedResult.variables();
    const auto* scaled = findVariable(variables, "scaled");
    const auto* revealed = findVariable(variables, "revealed");
    const auto* offset = findVariable(variables, "offset_value");
    const auto* staticValue = findVariable(variables, "static_value");
    assert(scaled && revealed && offset && staticValue);
    assert(std::abs(scalar(scaled->value) - 21.0) < kTolerance);
    assert(std::abs(scalar(revealed->value) - 107.0) < kTolerance);
    assert(std::abs(scalar(offset->value) - 12.0) < kTolerance);
    assert(std::abs(scalar(staticValue->value) - 12.0) <
           kTolerance);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    assert(mparser::sdk::abiMajor() == 1);
    assert(mparser::sdk::abiRevision() >= 1);
    runValueSmoke();
    runModuleSmoke(argv[1], argv[2]);
    std::cout << "cpp api smoke = 5050,42,21,abi-"
              << mparser::sdk::abiMajor() << '.'
              << mparser::sdk::abiRevision() << '\n';
    return 0;
}
