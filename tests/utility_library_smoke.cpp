#include "mparser/builtin_registry.h"
#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_session_state.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_system.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string readSource(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "utility-library sample is unavailable");
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

std::shared_ptr<mparser::RuntimeSessionState>
randomSession(std::uint64_t seed = 5489U) {
    mparser::RuntimeSystemContextOptions options;
    options.capabilities = mparser::RuntimeSystemCapability::Random;
    options.randomSeed = seed;
    return std::make_shared<mparser::RuntimeSessionState>(
        std::make_shared<mparser::RuntimeSystemContext>(
            std::move(options)));
}

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "utility-library source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "utility-library source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "utility-library source did not lower");

    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = randomSession();
    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic, interpreterOptions);

    mparser::BytecodeVmOptions vmOptions;
    vmOptions.sessionState = randomSession();
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic, vmOptions);
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
    throw std::runtime_error("missing utility-library variable: " +
                             std::string(name));
}

std::vector<double> numericValues(const mparser::RuntimeValue& value) {
    std::vector<double> values;
    const size_t count = mparser::runtimeShapeElementCount(value);
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto element = mparser::runtimeNumericElement(value, index);
        require(element.has_value(),
                "utility-library result contains a nonnumeric element");
        values.push_back(*element);
    }
    return values;
}

mparser::RuntimeValue exactIntegerValue(
    mparser::RuntimeNumericClass numericClass,
    std::vector<size_t> dimensions,
    const std::vector<std::uint64_t>& bits) {
    std::vector<mparser::RuntimeNumericElementValue> elements;
    elements.reserve(bits.size());
    for (const std::uint64_t value : bits) {
        mparser::RuntimeNumericElementValue element;
        element.numericClass = numericClass;
        element.real = static_cast<double>(value);
        element.integerRealBits = value;
        elements.push_back(element);
    }
    auto result = mparser::runtimeNumericValueFromElements(
        std::move(dimensions), std::move(elements), numericClass);
    require(result.has_value(),
            "exact utility-library integer could not be constructed");
    return std::move(*result);
}

mparser::BuiltinResult invoke(
    const std::shared_ptr<const mparser::BuiltinRegistry>& registry,
    std::string_view name, std::vector<mparser::RuntimeValue> arguments,
    size_t outputs, mparser::BuiltinCallContext* context = nullptr) {
    return registry->invoke(
        name, mparser::BuiltinCall{arguments, outputs, {}, context});
}

void requireDiagnostic(const mparser::BuiltinResult& result,
                       std::string_view identifier) {
    require(!result.succeeded && result.outputs.empty() &&
                result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier == identifier,
            "utility-library builtin reported the wrong diagnostic");
}

template <typename Result>
void requireDiagnostic(const Result& result,
                       std::string_view identifier) {
    require(!result.diagnostics.empty(),
            "invalid utility-library source unexpectedly succeeded");
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier == identifier) {
            return;
        }
    }
    throw std::runtime_error(
        "utility-library runtime reported the wrong diagnostic");
}

template <typename Result>
void verifySample(const Result& result) {
    require(result.diagnostics.empty(),
            "utility-library sample produced a runtime diagnostic");
    const auto& summary = variable(result, "summary");
    require(summary.kind == mparser::RuntimeValueKind::Number &&
                summary.number == 13.0,
            "utility-library summary mismatch");

    const auto& exact = variable(result, "exact_common");
    const auto exactElement =
        mparser::runtimeNumericElementValue(exact, 0);
    require(exact.numericClass == mparser::RuntimeNumericClass::UInt64 &&
                exactElement && exactElement->integerRealBits == 2U,
            "gcd did not preserve exact uint64 output");

    const auto& grid = variable(result, "grid_z");
    require(mparser::runtimeDimensions(grid) ==
                std::vector<size_t>({2, 2, 2}) &&
                numericValues(grid) ==
                    std::vector<double>({5, 5, 5, 5, 6, 6, 6, 6}),
            "meshgrid N-dimensional layout mismatch");

    const auto& selection = variable(result, "selection");
    const auto selected = numericValues(selection);
    require(mparser::runtimeDimensions(selection) ==
                std::vector<size_t>({1, 8}) &&
                std::set<double>(selected.begin(), selected.end()).size() ==
                    selected.size(),
            "randperm did not produce a unique row selection");
}

void registryAndBoundarySmoke() {
    const auto registry = mparser::defaultBuiltinRegistry();
    for (const std::string_view name : {
             "factorial", "flip", "fliplr", "flipud", "gcd",
             "isprime", "lcm", "logspace", "meshgrid", "primes",
             "randperm", "strfind", "strrep"}) {
        require(registry->contains(name),
                "utility-library builtin is absent from the registry");
    }

    const auto* primes = registry->find("primes");
    require(primes &&
                primes->implementation ==
                    mparser::BuiltinImplementationKind::Context &&
                mparser::hasBuiltinContextPermission(
                    primes->contextPermissions,
                    mparser::BuiltinContextPermission::ExecutionControl),
            "primes execution-control metadata mismatch");
    const auto* randperm = registry->find("randperm");
    require(randperm &&
                randperm->determinism ==
                    mparser::BuiltinDeterminism::Nondeterministic &&
                mparser::hasBuiltinSideEffect(
                    randperm->sideEffects,
                    mparser::BuiltinSideEffect::RandomState) &&
                mparser::hasBuiltinContextPermission(
                    randperm->contextPermissions,
                    mparser::BuiltinContextPermission::ExecutionControl),
            "randperm random/resource metadata mismatch");

    constexpr std::uint64_t largestPrime = 18446744073709551557ULL;
    auto result = invoke(
        registry, "isprime",
        {exactIntegerValue(mparser::RuntimeNumericClass::UInt64,
                           {1, 2},
                           {largestPrime,
                            std::numeric_limits<std::uint64_t>::max()})},
        1);
    require(result.succeeded && result.outputs.size() == 1 &&
                numericValues(result.outputs.front()) ==
                    std::vector<double>({1, 0}),
            "isprime lost exact uint64 semantics");

    result = invoke(
        registry, "gcd",
        {exactIntegerValue(mparser::RuntimeNumericClass::UInt64,
                           {1, 1}, {largestPrime - 1U}),
         exactIntegerValue(mparser::RuntimeNumericClass::UInt64,
                           {1, 1}, {4U})},
        1);
    const auto gcdElement = result.succeeded && !result.outputs.empty()
                                ? mparser::runtimeNumericElementValue(
                                      result.outputs.front(), 0)
                                : std::nullopt;
    require(result.succeeded && gcdElement &&
                gcdElement->integerRealBits == 4U,
            "gcd truncated an exact uint64 input");

    result = invoke(registry, "lcm",
                    {mparser::makeRuntimeNumberValue(100000000),
                     mparser::makeRuntimeNumberValue(99999989)},
                    1);
    const auto largeLcm = result.succeeded && !result.outputs.empty()
                              ? mparser::runtimeNumericElement(
                                    result.outputs.front(), 0)
                              : std::nullopt;
    require(largeLcm && *largeLcm > 9.5e15,
            "floating lcm was truncated at flintmax");

    result = invoke(registry, "factorial",
                    {mparser::makeRuntimeLogicalValue(true)}, 1);
    requireDiagnostic(result, "MParser:InvalidFactorialInput");

    const std::string emoji = "\xF0\x9F\x98\x80";
    result = invoke(
        registry, "strfind",
        {mparser::makeRuntimeCharacterVectorUtf8(
             std::string("A") + emoji + "B" + emoji),
         mparser::makeRuntimeCharacterVectorUtf8(emoji)},
        1);
    require(result.succeeded &&
                numericValues(result.outputs.front()) ==
                    std::vector<double>({2, 5}),
            "strfind did not report UTF-16 code-unit positions");

    result = invoke(
        registry, "flip",
        {mparser::makeRuntimeMissingArrayValue({2, 1000000000}),
         mparser::makeRuntimeNumberValue(2)},
        1);
    require(result.succeeded && result.outputs.front().kind ==
                mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(result.outputs.front()) ==
                    std::vector<size_t>({2, 1000000000}),
            "flip materialized or reshaped a large missing array");
}

void executionControlAndRandomSmoke() {
    const auto registry = mparser::defaultBuiltinRegistry();

    mparser::RuntimeExecutionLimits primeLimits;
    primeLimits.maxArrayBytes = 8;
    mparser::RuntimeExecutionControl primeControl(primeLimits);
    mparser::BuiltinCallContext primeContext;
    primeContext.executionControl = &primeControl;
    auto result = invoke(registry, "primes",
                         {mparser::makeRuntimeNumberValue(1000)}, 1,
                         &primeContext);
    requireDiagnostic(result, "MParser:ExecutionStopped");
    require(primeControl.stopReason() ==
                mparser::RuntimeExecutionStopReason::ArrayByteLimit,
            "primes did not stop at its workspace/output byte boundary");

    mparser::RuntimeCancellationToken cancellation;
    cancellation.requestCancellation();
    mparser::RuntimeExecutionControl cancelled(
        {}, std::optional<mparser::RuntimeCancellationToken>{cancellation});
    mparser::BuiltinCallContext cancelledContext;
    cancelledContext.executionControl = &cancelled;
    result = invoke(registry, "factorial",
                    {mparser::makeRuntimeVectorValue({1, 2, 3})}, 1,
                    &cancelledContext);
    requireDiagnostic(result, "MParser:ExecutionStopped");
    require(cancelled.stopReason() ==
                mparser::RuntimeExecutionStopReason::Cancelled,
            "numeric utility cancellation was not observed before work");

    mparser::RuntimeSystemContextOptions systemOptions;
    systemOptions.capabilities =
        mparser::RuntimeSystemCapability::Random;
    systemOptions.randomSeed = 42U;
    mparser::RuntimeSystemContext system(std::move(systemOptions));
    mparser::BuiltinCallContext randomContext;
    randomContext.systemContext = &system;
    result = invoke(registry, "randperm",
                    {mparser::makeRuntimeNumberValue(1000000000),
                     mparser::makeRuntimeNumberValue(8)},
                    1, &randomContext);
    require(result.succeeded && result.outputs.size() == 1,
            "sparse randperm selection failed");
    const auto first = numericValues(result.outputs.front());
    require(first.size() == 8 &&
                std::set<double>(first.begin(), first.end()).size() == 8,
            "sparse randperm selection contains duplicates");
    require(system.reseedRandom(42U).succeeded,
            "randperm test could not restore the random seed");
    result = invoke(registry, "randperm",
                    {mparser::makeRuntimeNumberValue(1000000000),
                     mparser::makeRuntimeNumberValue(8)},
                    1, &randomContext);
    require(result.succeeded &&
                numericValues(result.outputs.front()) == first,
            "randperm was not reproducible after rng reseeding");

    require(system.reseedRandom(17U).succeeded,
            "randperm resource test could not set the random seed");
    const auto before = system.randomState();
    require(before.succeeded,
            "randperm resource test could not capture random state");
    mparser::RuntimeExecutionLimits randomLimits;
    randomLimits.maxArrayBytes = 7;
    mparser::RuntimeExecutionControl randomControl(randomLimits);
    randomContext.executionControl = &randomControl;
    result = invoke(registry, "randperm",
                    {mparser::makeRuntimeNumberValue(10),
                     mparser::makeRuntimeNumberValue(2)},
                    1, &randomContext);
    requireDiagnostic(result, "MParser:ExecutionStopped");
    const auto after = system.randomState();
    require(after.succeeded &&
                before.value.engineState == after.value.engineState,
            "rejected randperm call advanced the random state");
}

template <typename Result>
void verifyExtendedFamily(const Result& result) {
    require(result.diagnostics.empty(),
            "extended utility family emitted a diagnostic");
    require(numericValues(variable(result, "row_default")) ==
                std::vector<double>({3, 2, 1}),
            "default row flip selected the wrong dimension");
    require(numericValues(variable(result, "column_default")) ==
                std::vector<double>({3, 2, 1}) &&
                mparser::runtimeDimensions(
                    variable(result, "column_default")) ==
                    std::vector<size_t>({3, 1}),
            "default column flip selected the wrong dimension");
    require(mparser::runtimeDimensions(
                variable(result, "empty_factorial")) ==
                std::vector<size_t>({0, 3}),
            "factorial did not preserve an empty matrix shape");
    require(numericValues(variable(result, "one_grid")) ==
                std::vector<double>({1, 1, 2, 2}),
            "single-output one-input meshgrid layout mismatch");
    require(numericValues(variable(result, "one_x")) ==
                std::vector<double>({1, 1, 2, 2, 1, 1, 2, 2}) &&
                numericValues(variable(result, "one_y")) ==
                std::vector<double>({1, 2, 1, 2, 1, 2, 1, 2}) &&
                mparser::runtimeDimensions(variable(result, "one_z")) ==
                    std::vector<size_t>({2, 2, 2}),
            "three-output one-input meshgrid layout mismatch");
    require(mparser::runtimeShapeElementCount(
                variable(result, "empty_positions")) == 0 &&
                mparser::runtimeTextScalarUtf8(
                    variable(result, "unchanged")) ==
                    std::optional<std::string>{"abc"},
            "empty text pattern behavior mismatch");
    require(numericValues(variable(result, "floored_space")) ==
                std::vector<double>({1, 10, 100}) &&
                mparser::runtimeDimensions(
                    variable(result, "negative_space")) ==
                    std::vector<size_t>({1, 0}) &&
                mparser::runtimeDimensions(
                    variable(result, "negative_primes")) ==
                    std::vector<size_t>({1, 0}),
            "numeric utility scalar-boundary behavior mismatch");
    const auto& cells = variable(result, "flipped_cells");
    require(cells.kind == mparser::RuntimeValueKind::Cell &&
                cells.cells.size() == 3 &&
                mparser::runtimeNumericElement(cells.cells[0], 0) == 3.0 &&
                mparser::runtimeNumericElement(cells.cells[2], 0) == 1.0,
            "cell row flip payload mismatch");
    require(variable(result, "summary").number == 9.0,
            "extended utility summary mismatch");
}

void runtimeFamilySmoke() {
    const auto extended = runBoth(R"(
row_default = flip([1 2 3]);
column_default = flip([1; 2; 3]);
empty_factorial = factorial(zeros(0, 3));
one_grid = meshgrid([1 2]);
[one_x, one_y, one_z] = meshgrid([1 2]);
empty_positions = strfind('abc', '');
unchanged = strrep('abc', '', 'X');
floored_space = logspace(0, 2, 3.9);
negative_space = logspace(0, 2, -2);
negative_primes = primes(-3);
flipped_cells = fliplr({1, 2, 3});
summary = 9;
)");
    verifyExtendedFamily(extended.interpreter);
    verifyExtendedFamily(extended.vm);
}

void runtimeErrorSmoke() {
    struct Case {
        std::string_view source;
        std::string_view identifier;
    };
    for (const Case& test : {
             Case{"bad = factorial(-1);",
                  "MParser:InvalidFactorialInput"},
             Case{"bad = primes(5.5);",
                  "MParser:InvalidPrimesInput"},
             Case{"bad = logspace(int8(0), 2, 3);",
                  "MParser:InvalidLogspaceInput"},
             Case{"bad = gcd(int8(2), uint8(2));",
                  "MParser:InvalidIntegerPair"},
             Case{"bad = gcd([12 15], [18; 25]);",
                  "MParser:InvalidIntegerPair"},
             Case{"bad = flip([1 2], 0);",
                  "MParser:InvalidArrayOperation"},
             Case{"bad = strfind({'ok', 3}, 'o');",
                  "MParser:InvalidTextInput"},
             Case{"bad = randperm(2, 3);",
                  "MParser:InvalidSystemBuiltinCall"},
             Case{"[x,y,z] = meshgrid([1 2], [3 4]);",
                  "MParser:InvalidMeshgridArity"},
             Case{"bad = meshgrid(true);",
                  "MParser:InvalidMeshgridInput"},
             Case{"bad = meshgrid(1 + 2i);",
                  "MParser:InvalidMeshgridInput"}}) {
        const auto result = runBoth(test.source);
        requireDiagnostic(result.interpreter, test.identifier);
        requireDiagnostic(result.vm, test.identifier);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2,
                "utility-library smoke expects the sample path");
        const auto sample = runBoth(readSource(argv[1]));
        verifySample(sample.interpreter);
        verifySample(sample.vm);
        registryAndBoundarySmoke();
        executionControlAndRandomSmoke();
        runtimeFamilySmoke();
        runtimeErrorSmoke();
        std::cout << "utility library smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Utility library smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
