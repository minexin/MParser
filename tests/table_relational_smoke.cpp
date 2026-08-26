#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_table_relational.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        std::cerr << "table relational smoke failure: " << message
                  << '\n';
        std::exit(1);
    }
}

mparser::RuntimeValue numericColumn(std::vector<double> values) {
    const size_t count = values.size();
    auto result = mparser::runtimeNumericValueFromLogicalOrder(
        {count, 1}, std::move(values),
        mparser::RuntimeNumericClass::Double);
    require(result.has_value(), "numeric column construction failed");
    return std::move(*result);
}

mparser::RuntimeValue integerColumn(std::vector<double> values) {
    auto result = mparser::runtimeConvertNumericClass(
        numericColumn(std::move(values)),
        mparser::RuntimeNumericClass::Int32);
    require(result.has_value(), "integer column construction failed");
    return std::move(*result);
}

mparser::RuntimeValue stringColumn(
    std::vector<std::string> values,
    std::vector<bool> missing = {}) {
    if (missing.empty()) {
        missing.assign(values.size(), false);
    }
    require(values.size() == missing.size(),
            "string missing mask has the wrong size");
    std::vector<mparser::RuntimeStringElement> elements;
    elements.reserve(values.size());
    for (size_t index = 0; index < values.size(); ++index) {
        elements.push_back(mparser::RuntimeStringElement{
            mparser::runtimeUtf8ToUtf16(values[index]), missing[index]});
    }
    return mparser::makeRuntimeStringArray(
        {values.size(), 1}, std::move(elements));
}

mparser::RuntimeValue characterColumn(std::u16string values) {
    const size_t count = values.size();
    return mparser::makeRuntimeCharacterArray(
        {count, 1}, std::move(values));
}

mparser::RuntimeValue complexColumn(
    std::vector<std::pair<double, double>> values) {
    std::vector<mparser::RuntimeNumericElementValue> elements;
    elements.reserve(values.size());
    for (const auto& [real, imaginary] : values) {
        mparser::RuntimeNumericElementValue element;
        element.numericClass = mparser::RuntimeNumericClass::Double;
        element.real = real;
        element.imaginary = imaginary;
        element.complex = true;
        elements.push_back(element);
    }
    auto result = mparser::runtimeNumericValueFromElements(
        {values.size(), 1}, std::move(elements),
        mparser::RuntimeNumericClass::Double);
    require(result.has_value(), "complex column construction failed");
    return std::move(*result);
}

mparser::RuntimeValue dateColumn(std::vector<double> days) {
    auto result = mparser::runtimeConstructDateTime({
        mparser::makeRuntimeNumberValue(2024.0),
        mparser::makeRuntimeNumberValue(1.0),
        numericColumn(std::move(days)),
    });
    require(result.succeeded, "datetime column construction failed: " +
                               result.error);
    return std::move(result.value);
}

mparser::RuntimeValue text(std::string value) {
    return mparser::makeRuntimeCharacterVectorUtf8(value);
}

mparser::RuntimeValue textList(
    std::initializer_list<std::string> values) {
    std::vector<mparser::RuntimeValue> cells;
    cells.reserve(values.size());
    for (const auto& value : values) {
        cells.push_back(text(value));
    }
    return mparser::makeRuntimeCellValue(std::move(cells));
}

mparser::RuntimeValue cellColumn(
    std::vector<mparser::RuntimeValue> values) {
    const size_t count = values.size();
    return mparser::makeRuntimeCellValue(
        {count, 1}, std::move(values));
}

mparser::RuntimeValue table(
    std::vector<mparser::RuntimeValue> variables,
    std::vector<std::string> names) {
    auto result = mparser::runtimeMakeTable(
        std::move(variables), std::move(names));
    require(result.succeeded, "table construction failed: " + result.error);
    return std::move(result.value);
}

const mparser::RuntimeValue& member(
    const mparser::RuntimeValue& value, std::string_view name,
    mparser::RuntimeValue& storage) {
    auto result = mparser::runtimeTableMemberValue(value, name);
    require(result.succeeded,
            "table member is unavailable: " + std::string(name));
    storage = std::move(result.value);
    return storage;
}

double numberAt(const mparser::RuntimeValue& value, size_t index = 0) {
    auto result = mparser::runtimeNumericElement(value, index);
    require(result.has_value(), "numeric element is unavailable");
    return *result;
}

std::vector<double> numericValues(const mparser::RuntimeValue& value) {
    std::vector<double> result;
    const size_t count = mparser::runtimeShapeElementCount(value);
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(numberAt(value, index));
    }
    return result;
}

void requireNumbers(const mparser::RuntimeValue& value,
                    std::initializer_list<double> expected,
                    std::string message) {
    const auto actual = numericValues(value);
    require(actual.size() == expected.size(),
            std::move(message) + " (length)");
    size_t index = 0;
    for (const double candidate : expected) {
        const double observed = actual[index++];
        require((std::isnan(candidate) && std::isnan(observed)) ||
                    observed == candidate,
                message + " (value)");
    }
}

std::vector<std::string> variableNames(
    const mparser::RuntimeValue& value) {
    const auto* storage = mparser::runtimeTableStorage(value);
    require(storage != nullptr, "result is not a table");
    std::vector<std::string> result;
    for (const auto& variable : storage->variables) {
        result.push_back(variable.name);
    }
    return result;
}

mparser::BuiltinResult invoke(
    std::string_view name,
    const std::vector<mparser::RuntimeValue>& arguments,
    size_t outputs = 1,
    mparser::BuiltinCallContext* context = nullptr) {
    const auto registry = mparser::defaultBuiltinRegistry();
    return registry->invoke(
        name, mparser::BuiltinCall{
                  arguments, outputs, mparser::SourceSpan{}, context});
}

mparser::RuntimeValue categoricalColumn(
    std::vector<std::string> values) {
    auto result = mparser::runtimeConstructCategorical(
        stringColumn(std::move(values)));
    require(result.succeeded,
            "categorical column construction failed: " + result.error);
    return std::move(result.value);
}

void joinSmoke() {
    using namespace mparser;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const RuntimeValue left = table(
        {numericColumn({1, 2, 2, nan}), numericColumn({10, 20, 21, 99}),
         numericColumn({100, 200, 201, 999})},
        {"K", "X", "Z"});
    const RuntimeValue right = table(
        {numericColumn({2, 2, 3, nan}), numericColumn({30, 31, 40, 41}),
         numericColumn({300, 301, 400, 401})},
        {"K", "X", "W"});

    RuntimeTableJoinOptions options;
    options.leftKeys = {"K"};
    options.rightKeys = {"K"};
    auto inner = runtimeJoinTables(left, right, options);
    require(inner.succeeded, "inner join failed: " + inner.error);
    require(inner.leftRows == std::vector<size_t>({1, 1, 2, 2}) &&
                inner.rightRows == std::vector<size_t>({0, 1, 0, 1}),
            "inner join did not preserve duplicate-key Cartesian order");
    require(variableNames(inner.value) ==
                std::vector<std::string>({"K", "X_L", "Z", "X_R", "W"}),
            "inner join output names are not MATLAB-style");
    RuntimeValue value;
    requireNumbers(member(inner.value, "K", value), {2, 2, 2, 2},
                   "inner join key values are wrong");
    requireNumbers(member(inner.value, "X_L", value), {20, 20, 21, 21},
                   "inner join left values are wrong");
    requireNumbers(member(inner.value, "X_R", value), {30, 31, 30, 31},
                   "inner join right values are wrong");

    auto outer = options;
    outer.mergeKeys = false;
    outer.type = RuntimeTableJoinType::Full;
    auto full = runtimeJoinTables(left, right, outer);
    require(full.succeeded, "full outer join failed: " + full.error);
    require(full.leftRows ==
                std::vector<size_t>({0, 1, 1, 2, 2, 3,
                                     kRuntimeTableUnmatchedRow,
                                     kRuntimeTableUnmatchedRow}) &&
                full.rightRows ==
                    std::vector<size_t>({kRuntimeTableUnmatchedRow, 0, 1,
                                         0, 1, kRuntimeTableUnmatchedRow,
                                         2, 3}),
            "full outer join row maps are not stable");
    require(variableNames(full.value) ==
                std::vector<std::string>({"K_L", "X_L", "Z", "K_R",
                                          "X_R", "W"}),
            "full outer join conflict suffixes are wrong");
    requireNumbers(member(full.value, "K_L", value),
                   {1, 2, 2, 2, 2, nan, nan, nan},
                   "full outer left key fill is wrong");
    requireNumbers(member(full.value, "K_R", value),
                   {nan, 2, 2, 2, 2, nan, 3, nan},
                   "full outer right key fill is wrong");

    auto leftOuter = options;
    leftOuter.type = RuntimeTableJoinType::Left;
    leftOuter.mergeKeys = true;
    auto leftResult = runtimeJoinTables(left, right, leftOuter);
    require(leftResult.succeeded && leftResult.leftRows.size() == 6,
            "left outer join did not retain all left rows");
    require(variableNames(leftResult.value) ==
                std::vector<std::string>({"K", "X_L", "Z", "X_R", "W"}),
            "merged left outer join names are wrong");
    requireNumbers(member(leftResult.value, "K", value),
                   {1, 2, 2, 2, 2, nan},
                   "merged left outer key fill is wrong");
    require(std::isnan(numberAt(
                member(leftResult.value, "X_R", value), 0)),
            "left outer join did not synthesize numeric missing value");

    RuntimeTableJoinOptions emptyVariables = options;
    emptyVariables.mergeKeys = false;
    emptyVariables.hasLeftVariables = true;
    emptyVariables.hasRightVariables = true;
    auto emptyResult = runtimeJoinTables(left, right, emptyVariables);
    require(emptyResult.succeeded &&
                runtimeDimensions(emptyResult.value) ==
                    std::vector<size_t>({4, 0}),
            "explicit empty join variable selections lost row count");

    auto registryInner = invoke(
        "innerjoin", {left, right, text("Keys"), text("K")}, 3);
    require(registryInner.succeeded && registryInner.outputs.size() == 3,
            "registry innerjoin invocation failed");
    requireNumbers(registryInner.outputs[1], {2, 2, 3, 3},
                   "innerjoin left index output is wrong");
    requireNumbers(registryInner.outputs[2], {1, 2, 1, 2},
                   "innerjoin right index output is wrong");

    auto caseInsensitive = invoke(
        "outerjoin",
        {left, right, text("keys"), text("K"), text("mergekeys"),
         makeRuntimeNumberValue(1.0), text("type"), text("LEFT")});
    require(caseInsensitive.succeeded,
            "join name-value options were not case-insensitive");

    auto duplicateOption = invoke(
        "outerjoin", {left, right, text("Keys"), text("K"),
                       text("keys"), text("K")});
    require(!duplicateOption.succeeded,
            "outerjoin accepted a duplicate Keys option");

    RuntimeCancellationToken cancellation;
    cancellation.requestCancellation();
    RuntimeExecutionControl cancelled(
        {}, std::optional<RuntimeCancellationToken>{cancellation});
    BuiltinCallContext cancelledContext;
    cancelledContext.executionControl = &cancelled;
    auto stoppedJoin = invoke(
        "innerjoin", {left, right, text("Keys"), text("K")}, 1,
        &cancelledContext);
    require(!stoppedJoin.succeeded &&
                !stoppedJoin.diagnostics.empty() &&
                stoppedJoin.diagnostics.front().identifier ==
                    "MParser:ExecutionStopped",
            "innerjoin did not report execution cancellation");
}

void groupingSmoke() {
    using namespace mparser;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const RuntimeValue source = table(
        {numericColumn({2, 1, 2, nan}),
         stringColumn({"b", "a", "b", ""}, {false, false, false, true}),
         numericColumn({10, 20, 30, 40})},
        {"K", "S", "V"});

    auto grouping = runtimeGroupTableRows(source, {"K", "S"});
    require(grouping.succeeded, "multi-key grouping failed: " +
                                   grouping.error);
    require(grouping.groups ==
                std::vector<std::vector<size_t>>({{1}, {0, 2}, {3}}),
            "grouping did not sort and coalesce rows deterministically");

    const RuntimeValue groupVariables = textList({"K", "S"});
    auto counts = invoke("groupcounts", {source, groupVariables});
    require(counts.succeeded,
            "groupcounts failed: " +
                (counts.diagnostics.empty()
                     ? std::string("no diagnostic")
                     : counts.diagnostics.front().message));
    require(variableNames(counts.outputs.front()) ==
                std::vector<std::string>({"K", "S", "GroupCount", "Percent"}),
            "groupcounts output names are wrong");
    RuntimeValue value;
    requireNumbers(member(counts.outputs.front(), "GroupCount", value),
                   {1, 2, 1}, "groupcounts values are wrong");
    requireNumbers(member(counts.outputs.front(), "Percent", value),
                   {25, 50, 25}, "groupcounts percentages are wrong");

    auto summaries = invoke(
        "groupsummary",
        {source, groupVariables, textList({"mean", "sum"}), text("V")});
    require(summaries.succeeded, "groupsummary failed");
    require(variableNames(summaries.outputs.front()) ==
                std::vector<std::string>({"K", "S", "GroupCount",
                                          "mean_V", "sum_V"}),
            "groupsummary output names are wrong");
    requireNumbers(member(summaries.outputs.front(), "mean_V", value),
                   {20, 20, 40}, "groupsummary means are wrong");
    requireNumbers(member(summaries.outputs.front(), "sum_V", value),
                   {20, 40, 40}, "groupsummary sums are wrong");

    auto defaultSummary = invoke("groupsummary", {source, text("K")});
    require(defaultSummary.succeeded,
            "groupsummary default count summary failed");
    require(variableNames(defaultSummary.outputs.front()) ==
                std::vector<std::string>({"K", "GroupCount"}),
            "groupsummary default output did not stay count-only");

    auto badMethod = invoke(
        "groupsummary", {source, text("K"), text("median"), text("V")});
    require(badMethod.succeeded,
            "median groupsummary should use the shared reduction builtin: " +
                (badMethod.diagnostics.empty()
                     ? std::string("no diagnostic")
                     : badMethod.diagnostics.front().message));
    auto unsupported = invoke(
        "groupsummary", {source, text("K"), text("doesNotExist")});
    require(!unsupported.succeeded,
            "groupsummary accepted an unsupported summary method");

    const RuntimeValue numericCells = table(
        {cellColumn({makeRuntimeNumberValue(2.0),
                     makeRuntimeMissingValue(),
                     makeRuntimeNumberValue(1.0),
                     makeRuntimeNumberValue(2.0)}),
         numericColumn({10, 20, 30, 40})},
        {"K", "V"});
    auto cellGrouping = runtimeGroupTableRows(numericCells, {"K"});
    require(cellGrouping.succeeded,
            "numeric/missing Cell grouping failed: " +
                cellGrouping.error);
    require(cellGrouping.groups ==
                std::vector<std::vector<size_t>>({{2}, {0, 3}, {1}}),
            "numeric/missing Cell grouping order is wrong");

    auto cellCounts = invoke("groupcounts", {numericCells, text("K")});
    require(cellCounts.succeeded,
            "numeric/missing Cell groupcounts failed");
    requireNumbers(member(cellCounts.outputs.front(), "GroupCount", value),
                   {1, 2, 1},
                   "numeric/missing Cell groupcounts values are wrong");

    auto uppercaseMethod = invoke(
        "groupsummary",
        {source, text("K"), text("MEAN"), text("V")});
    require(uppercaseMethod.succeeded,
            "groupsummary method names were not case-insensitive");

    const RuntimeValue empty = table(
        {numericColumn({}), numericColumn({})}, {"K", "V"});
    auto emptyCounts = invoke("groupcounts", {empty, text("K")});
    require(emptyCounts.succeeded &&
                runtimeDimensions(emptyCounts.outputs.front()) ==
                    std::vector<size_t>({0, 3}),
            "empty groupcounts did not preserve a zero-row table");
    auto emptySummary = invoke(
        "groupsummary", {empty, text("K"), text("mean"), text("V")});
    require(emptySummary.succeeded &&
                runtimeDimensions(emptySummary.outputs.front()) ==
                    std::vector<size_t>({0, 3}),
            "empty groupsummary did not preserve a zero-row table");
    (void)nan;
}

void typedKeySmoke() {
    using namespace mparser;
    RuntimeTableJoinOptions options;
    options.leftKeys = {"K"};
    options.rightKeys = {"K"};

    const RuntimeValue leftCategorical = table(
        {categoricalColumn({"red", "blue"}), numericColumn({1, 2})},
        {"K", "V"});
    const RuntimeValue rightCategorical = table(
        {categoricalColumn({"blue", "red"}), numericColumn({20, 10})},
        {"K", "W"});
    auto categorical = runtimeJoinTables(
        leftCategorical, rightCategorical, options);
    require(categorical.succeeded && categorical.leftRows.size() == 2,
            "categorical join did not match labels across dictionaries");

    const RuntimeValue leftDates = table(
        {dateColumn({1, 2}), numericColumn({1, 2})}, {"K", "V"});
    const RuntimeValue rightDates = table(
        {dateColumn({2, 1}), numericColumn({20, 10})}, {"K", "W"});
    auto temporal = runtimeJoinTables(leftDates, rightDates, options);
    require(temporal.succeeded && temporal.leftRows.size() == 2,
            "datetime join failed");

    const RuntimeValue leftComplex = table(
        {complexColumn({{1, 2}, {3, 4}}), numericColumn({1, 2})},
        {"K", "V"});
    const RuntimeValue rightComplex = table(
        {complexColumn({{3, 4}, {1, 2}}), numericColumn({20, 10})},
        {"K", "W"});
    auto complex = runtimeJoinTables(leftComplex, rightComplex, options);
    require(complex.succeeded && complex.leftRows.size() == 2,
            "complex numeric join failed");

    const RuntimeValue leftReal = table(
        {numericColumn({1, 2}), numericColumn({1, 2})}, {"K", "V"});
    const RuntimeValue rightComplexZero = table(
        {complexColumn({{2, 0}, {1, 0}}), numericColumn({20, 10})},
        {"K", "W"});
    auto realComplex = runtimeJoinTables(
        leftReal, rightComplexZero, options);
    require(realComplex.succeeded && realComplex.leftRows.size() == 2,
            "real keys did not match complex keys with zero imaginary part");

    const RuntimeValue leftCharacters = table(
        {characterColumn(u"ab"), numericColumn({1, 2})}, {"K", "V"});
    const RuntimeValue rightStrings = table(
        {stringColumn({"a", "b"}), numericColumn({20, 10})},
        {"K", "W"});
    auto textJoin = runtimeJoinTables(
        leftCharacters, rightStrings, options);
    require(textJoin.succeeded && textJoin.leftRows.size() == 2,
            "character and string keys did not compare row text");

    RuntimeTableJoinOptions full = options;
    full.type = RuntimeTableJoinType::Full;
    const RuntimeValue rightStringsWithExtra = table(
        {stringColumn({"a", "b", "c"}), numericColumn({20, 10, 30})},
        {"K", "W"});
    auto mergedText = runtimeJoinTables(
        leftCharacters, rightStringsWithExtra, full);
    RuntimeValue mergedKey;
    require(mergedText.succeeded &&
                isRuntimeCharacterArray(
                    member(mergedText.value, "K", mergedKey)),
            "merged character/string key did not retain the left key type");

    const RuntimeValue leftInteger = table(
        {integerColumn({1, 2}), numericColumn({1, 2})}, {"K", "V"});
    const RuntimeValue rightNumeric = table(
        {numericColumn({2, 1}), numericColumn({20, 10})}, {"K", "W"});
    auto compatible = runtimeJoinTables(leftInteger, rightNumeric, options);
    require(compatible.succeeded && compatible.leftRows.size() == 2,
            "integer and double keys did not compare by numeric value");

    const RuntimeValue emptyDouble = table(
        {numericColumn({}), numericColumn({})}, {"K", "V"});
    const RuntimeValue rightInteger = table(
        {integerColumn({2, 1}), numericColumn({20, 10})}, {"K", "W"});
    auto mergedNumeric = runtimeJoinTables(
        emptyDouble, rightInteger, full);
    require(mergedNumeric.succeeded,
            "merged cross-class numeric outer join failed: " +
                mergedNumeric.error);
    require(member(mergedNumeric.value, "K", mergedKey).numericClass ==
                RuntimeNumericClass::Double,
            "merged cross-class numeric key did not retain the left class");
    requireNumbers(mergedKey, {2, 1},
                   "merged cross-class numeric key values are wrong");
    auto incompatible = runtimeJoinTables(leftInteger, rightDates, options);
    require(!incompatible.succeeded,
            "join accepted numeric and temporal key classes");

    RuntimeTableJoinOptions differentNames;
    differentNames.leftKeys = {"K"};
    differentNames.rightKeys = {"Key"};
    const RuntimeValue rightRenamed = table(
        {dateColumn({2, 1}), numericColumn({20, 10})}, {"Key", "W"});
    auto renamed = runtimeJoinTables(leftDates, rightRenamed,
                                      differentNames);
    require(renamed.succeeded && renamed.leftRows.size() == 2,
            "LeftKeys/RightKeys style key mapping failed");
}

} // namespace

int main() {
    joinSmoke();
    groupingSmoke();
    typedKeySmoke();
    std::cout << "table relational smoke tests passed\n";
    return 0;
}
