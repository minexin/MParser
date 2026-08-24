#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"
#include "mparser/semantic/semantic.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult bytecode;
};

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.diagnostics.empty(), "datetime source did not parse");
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    require(semantic.diagnostics.empty(),
            "datetime source failed semantic analysis");
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "datetime source did not lower");
    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic);
    return {std::move(interpreted), std::move(executed)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing datetime variable: " +
                             std::string(name));
}

template <typename Result>
void requireClean(const Result& result, std::string_view engine) {
    require(result.diagnostics.empty(),
            std::string(engine) + " reported a datetime diagnostic");
}

void compare(const mparser::RuntimeValue& left,
             const mparser::RuntimeValue& right, std::string_view name) {
    require(mparser::runtimeValuesEqual(left, right,
                                        mparser::RuntimeNaNEquality::Equal),
            std::string(name) + " differs between interpreter and VM");
}

void registrySmoke() {
    const auto registry = mparser::defaultBuiltinRegistry();
    for (const std::string_view name : {
             "datetime", "duration", "NaT", "year", "month", "day",
             "hour", "minute", "second", "days", "hours", "minutes",
             "seconds", "isdatetime", "isduration", "isnat"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Shared &&
                    descriptor->purity == mparser::BuiltinPurity::Pure &&
                    descriptor->determinism ==
                        mparser::BuiltinDeterminism::Deterministic,
                "temporal descriptor metadata mismatch for " +
                    std::string(name));
    }
}

void runtimeSmoke() {
    auto date = mparser::runtimeConstructDateTime({
        mparser::makeRuntimeNumberValue(2024),
        mparser::makeRuntimeNumberValue(2),
        mparser::makeRuntimeNumberValue(29),
        mparser::makeRuntimeNumberValue(23),
        mparser::makeRuntimeNumberValue(59),
        mparser::makeRuntimeNumberValue(58.5),
    });
    require(date.succeeded, "valid leap-day datetime was rejected");
    auto invalid = mparser::runtimeConstructDateTime({
        mparser::makeRuntimeNumberValue(2023),
        mparser::makeRuntimeNumberValue(2),
        mparser::makeRuntimeNumberValue(29),
    });
    require(!invalid.succeeded, "invalid civil date was accepted");
    auto textDate = mparser::runtimeConstructDateTime(
        {mparser::makeRuntimeCharacterVectorUtf8("2020-01-02 03:04:05")});
    require(textDate.succeeded &&
                std::fabs(*mparser::runtimeTemporalPayload(textDate.value) -
                          *mparser::runtimeTemporalPayload(
                              mparser::runtimeConstructDateTime({
                                  mparser::makeRuntimeNumberValue(2020),
                                  mparser::makeRuntimeNumberValue(1),
                                  mparser::makeRuntimeNumberValue(2),
                                  mparser::makeRuntimeNumberValue(3),
                                  mparser::makeRuntimeNumberValue(4),
                                  mparser::makeRuntimeNumberValue(5),
                              }).value)) <
                    1e-12,
            "ISO datetime parsing mismatch");
    auto contract = mparser::validateRuntimeValueContract(date.value);
    require(contract.valid, "datetime violates RuntimeValue contract");
}

void scriptSmoke() {
    const auto pair = runBoth(R"(
stamp = datetime(2020, 1, 2, 3, 4, 5);
offset = duration(1, 30, 0);
shifted = stamp + offset;
elapsed = shifted - stamp;
dates = datetime([2020; 2021], [1; 2], [2; 3]);
years = year(dates);
nat = NaT(2, 2);
mask = ismissing(nat);
text = char(shifted);
same = isequal(stamp, datetime(2020, 1, 2, 3, 4, 5));
property_year = stamp.Year;
comparison = stamp < shifted;
scaled = 2 * offset;
)");
    requireClean(pair.interpreter, "interpreter");
    requireClean(pair.bytecode, "bytecode");

    compare(variable(pair.interpreter, "stamp"),
            variable(pair.bytecode, "stamp"), "stamp");
    compare(variable(pair.interpreter, "shifted"),
            variable(pair.bytecode, "shifted"), "shifted");
    compare(variable(pair.interpreter, "elapsed"),
            variable(pair.bytecode, "elapsed"), "elapsed");
    compare(variable(pair.interpreter, "years"),
            variable(pair.bytecode, "years"), "years");
    compare(variable(pair.interpreter, "mask"),
            variable(pair.bytecode, "mask"), "NaT mask");
    require(mparser::runtimeTextScalarUtf8(
                variable(pair.interpreter, "text")) ==
                std::optional<std::string>("2020-01-02 04:34:05"),
            "datetime character formatting mismatch");
    require(variable(pair.interpreter, "same").number == 1.0 &&
                variable(pair.interpreter, "property_year").number == 2020.0 &&
                variable(pair.interpreter, "comparison").number == 1.0,
            "datetime scalar assertions mismatch");
    require(mparser::runtimeDimensions(variable(pair.interpreter, "nat")) ==
                std::vector<size_t>({2, 2}),
            "NaT array shape mismatch");
}

} // namespace

int main() {
    try {
        registrySmoke();
        runtimeSmoke();
        scriptSmoke();
        std::cout << "datetime/duration smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "datetime/duration smoke failure: " << error.what()
                  << '\n';
        return 1;
    }
}
