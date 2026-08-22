#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
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
    require(input.good(), "standard-library sample is unavailable");
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "standard-library source did not parse");

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "standard-library source failed semantic analysis");

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "standard-library source did not lower");

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
    throw std::runtime_error("missing standard-library variable: " +
                             std::string(name));
}

template <typename Result>
void verifySample(const Result& result) {
    require(result.diagnostics.empty(),
            "standard-library sample produced a runtime diagnostic");
    const auto& summary = variable(result, "summary");
    require(summary.kind == mparser::RuntimeValueKind::Number &&
                summary.number == 12.0,
            "standard-library summary mismatch");
    require(mparser::runtimeTextScalarUtf8(variable(result, "joined")) ==
                std::optional<std::string>("abcd"),
            "space-separated character concatenation mismatch");
    require(mparser::runtimeDimensions(
                variable(result, "missing_ordered")) ==
                std::vector<size_t>({1, 3}),
            "missing sort lost its shape");
    require(variable(result, "unique_missing").kind ==
                mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(
                    variable(result, "unique_missing")) ==
                    std::vector<size_t>({1, 1}),
            "missing unique did not produce a shaped scalar");
    require(mparser::runtimeDimensions(
                variable(result, "distinct_missing")) ==
                std::vector<size_t>({1, 3}),
            "distinct missing values lost their shape");
    require(mparser::runtimeTextScalarUtf8(
                variable(result, "unique_characters")) ==
                std::optional<std::string>("abn"),
            "character unique result mismatch");
    require(mparser::runtimeDimensions(variable(result, "record_cells")) ==
                std::vector<size_t>({2, 1, 2}),
            "struct2cell N-dimensional mapping mismatch");
}

template <typename Result>
void requireDiagnostic(const Result& result, std::string_view identifier) {
    require(!result.diagnostics.empty(),
            "invalid standard-library call unexpectedly succeeded");
    require(std::any_of(
                result.diagnostics.begin(), result.diagnostics.end(),
                [identifier](const auto& diagnostic) {
                    return diagnostic.identifier == identifier;
                }),
            "invalid standard-library call reported the wrong identifier");
}

void verifyRegistryAndLargeInputs() {
    const auto registry = mparser::defaultBuiltinRegistry();
    for (const std::string_view name : {
             "cell2struct", "cellfun", "iscell", "lower", "num2str",
             "regexp", "sort", "strsplit", "strtrim", "struct2cell",
             "unique", "upper"}) {
        require(registry->contains(name),
                "standard-library builtin is absent from the registry");
    }
    const auto* cellfun = registry->find("cellfun");
    require(cellfun &&
                mparser::hasBuiltinContextPermission(
                    cellfun->requiredContext,
                    mparser::BuiltinContextPermission::DynamicCall),
            "cellfun dynamic-call contract is not declared");

    constexpr size_t kElementCount = 20000;
    std::vector<double> values;
    values.reserve(kElementCount);
    for (size_t index = 0; index < kElementCount; ++index) {
        values.push_back(static_cast<double>((index * 37U) % 97U));
    }
    auto input = mparser::runtimeNumericValueFromLogicalOrder(
        {1, kElementCount}, std::move(values),
        mparser::RuntimeNumericClass::Double);
    require(input.has_value(), "large unique input construction failed");
    std::vector<mparser::RuntimeValue> arguments{std::move(*input)};
    auto result = registry->invoke(
        "unique", mparser::BuiltinCall{arguments, 3, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 3 &&
                mparser::runtimeDimensions(result.outputs[0]) ==
                    std::vector<size_t>({1, 97}) &&
                mparser::runtimeDimensions(result.outputs[2]) ==
                    std::vector<size_t>({kElementCount, 1}),
            "large unique grouping or output shape mismatch");

    std::vector<mparser::RuntimeValue> missingArguments{
        mparser::makeRuntimeMissingArrayValue({1, 1000000000})};
    result = registry->invoke(
        "unique",
        mparser::BuiltinCall{missingArguments, 1, {}, nullptr});
    require(result.succeeded && result.outputs.size() == 1 &&
                result.outputs.front().kind ==
                    mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(result.outputs.front()) ==
                    std::vector<size_t>({1, 1}),
            "shape-only missing unique materialized or lost its shape");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2,
                "standard-library smoke expects the sample path");
        const auto sample = runBoth(readSource(argv[1]));
        verifySample(sample.interpreter);
        verifySample(sample.vm);
        const auto extended = runBoth(R"(
cube = reshape([4 1 3 2 8 5 7 6], 2, 2, 2);
[cube_sorted, cube_index] = sort(cube, 3, 'descend');
assert(isequal(cube_sorted, reshape([8 5 7 6 4 1 3 2], 2, 2, 2)));
assert(isequal(cube_index, reshape([2 2 2 2 1 1 1 1], 2, 2, 2)));
integer_sorted = sort(int64([3 -2 1]));
assert(isa(integer_sorted, 'int64') && isequal(integer_sorted, int64([-2 1 3])));
character_rows = unique(['ba'; 'ab'; 'ba'], 'rows');
cell_words = unique({'beta', 'alpha', 'beta'});
cell_sorted = sort({'alpha', 'beta'}, 'descend');
string_missing = unique(["beta", string(missing), "beta", string(missing)]);
assert(isequal(character_rows, ['ab'; 'ba']));
assert(strcmp(cell_words{1}, 'alpha') && strcmp(cell_words{2}, 'beta'));
assert(strcmp(cell_sorted{1}, 'beta') && strcmp(cell_sorted{2}, 'alpha'));
assert(numel(string_missing) == 2 && ismissing(string_missing(2)));
regex_parts = strsplit('a1b22c', '\d+', 'DelimiterType', 'RegularExpression');
tokens = regexp('a1', '([a-z])(\d)', 'tokens');
split_match = regexp('a1b2', '\d', 'split');
formatted = num2str(3.25, '%.1f');
assert(numel(regex_parts) == 3 && strcmp(regex_parts{2}, 'b'));
assert(strcmp(tokens{1}{1}, 'a') && strcmp(tokens{1}{2}, '1'));
assert(numel(split_match) == 3 && strcmp(split_match{2}, 'b'));
assert(strcmp(formatted, '3.2'));
summary = 7;
)");
        if (!extended.interpreter.diagnostics.empty()) {
            throw std::runtime_error(
                "extended interpreter coverage failed: " +
                extended.interpreter.diagnostics.front().message);
        }
        require(variable(extended.interpreter, "summary").number == 7.0,
                "extended interpreter summary mismatch");
        if (!extended.vm.diagnostics.empty()) {
            throw std::runtime_error(
                "extended bytecode coverage failed: " +
                extended.vm.diagnostics.front().message);
        }
        require(variable(extended.vm, "summary").number == 7.0,
                "extended bytecode summary mismatch");

        const auto badRegex = runBoth("bad = regexp('x', '[', 'match');\n");
        requireDiagnostic(badRegex.interpreter,
                          "MParser:InvalidRegularExpression");
        requireDiagnostic(badRegex.vm,
                          "MParser:InvalidRegularExpression");

        const auto badShape = runBoth(
            "bad = cellfun(@(x,y)x+y, {1,2}, {3});\n");
        requireDiagnostic(badShape.interpreter,
                          "MParser:InvalidCellfunShape");
        requireDiagnostic(badShape.vm,
                          "MParser:InvalidCellfunShape");

        const auto badCallback = runBoth(
            "bad = cellfun(@(x)x(2), {1});\n");
        require(!badCallback.interpreter.diagnostics.empty() &&
                    !badCallback.vm.diagnostics.empty(),
                "cellfun callback failure was not propagated");

        const auto duplicateFields = runBoth(
            "bad = cell2struct({1,2}, {'x','x'}, 2);\n");
        requireDiagnostic(duplicateFields.interpreter,
                          "MParser:InvalidStructFieldName");
        requireDiagnostic(duplicateFields.vm,
                          "MParser:InvalidStructFieldName");

        verifyRegistryAndLargeInputs();
        std::cout << "standard library smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Standard library smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
