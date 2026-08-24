#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "table runtime smoke failure: " << message << '\n';
        std::exit(1);
    }
}

mparser::RuntimeValue column(std::vector<double> values) {
    const size_t count = values.size();
    auto result = mparser::runtimeNumericValueFromLogicalOrder(
        {count, 1}, std::move(values),
        mparser::RuntimeNumericClass::Double);
    require(result.has_value(), "numeric column construction failed");
    return std::move(*result);
}

double numberAt(const mparser::RuntimeValue& value, size_t index = 0) {
    const auto result = mparser::runtimeNumericElement(value, index);
    require(result.has_value(), "numeric element is unavailable");
    return *result;
}

mparser::RuntimeValue text(std::string value) {
    return mparser::makeRuntimeCharacterVectorUtf8(value);
}

mparser::BuiltinResult invoke(
    std::string_view name,
    const std::vector<mparser::RuntimeValue>& arguments,
    size_t requestedOutputs = 1) {
    const auto registry = mparser::defaultBuiltinRegistry();
    mparser::BuiltinCall call{arguments, requestedOutputs,
                              mparser::SourceSpan{}, nullptr};
    return registry->invoke(name, call);
}

} // namespace

int main() {
    using namespace mparser;

    auto made = runtimeMakeTable(
        {column({1.0, 2.0}), column({3.0, 4.0})},
        {"A", "B"}, {"r1", "r2"});
    require(made.succeeded, made.error);
    const RuntimeValue table = made.value;
    require(isRuntimeTableValue(table), "constructed value is not a table");
    require(runtimeDimensions(table) == std::vector<size_t>({2, 2}),
            "table shape is not height-by-variable-count");
    require(runtimeValueToString(table) ==
                "<table 2x2 variables=A,B>",
            "table display summary changed");

    const auto contract = validateRuntimeValueContract(table);
    require(contract.valid, "table contract failed: " + contract.error);
    const auto bytes = runtimeValueArrayBytes(table);
    require(bytes && *bytes >= 4 * sizeof(double),
            "table recursive byte accounting is incomplete");
    require(runtimeValuesEqual(table, table),
            "table must equal an identical value");

    auto member = runtimeTableMemberValue(table, "B");
    require(member.succeeded && numberAt(member.value, 1) == 4.0,
            "table variable member access failed");
    auto properties = runtimeTableMemberValue(table, "Properties");
    require(properties.succeeded &&
                isRuntimeScalarStruct(properties.value),
            "table Properties snapshot is unavailable");
    const RuntimeValue* variableNames = runtimeStructField(
        properties.value, "VariableNames");
    require(variableNames && variableNames->kind == RuntimeValueKind::Cell &&
                variableNames->cells.size() == 2,
            "table Properties.VariableNames is invalid");

    require(runtimeSetStructField(
                properties.value, "Description", text("smoke")),
            "Properties.Description update setup failed");
    require(runtimeSetStructField(
                properties.value, "DimensionNames",
                makeRuntimeCellValue(
                    {1, 2}, {text("Rows"), text("Vars")})),
            "Properties.DimensionNames update setup failed");
    require(runtimeSetStructField(
                properties.value, "UserData", text("payload")),
            "Properties.UserData update setup failed");
    auto described = runtimeSetTableMember(
        table, "Properties", properties.value);
    require(described.succeeded, described.error);
    const auto* describedStorage = runtimeTableStorage(described.value);
    require(describedStorage && describedStorage->description == "smoke" &&
                describedStorage->dimensionNames ==
                    std::vector<std::string>({"Rows", "Vars"}) &&
                runtimeTextScalarUtf8(describedStorage->userData) ==
                    std::optional<std::string>("payload"),
            "table Properties copyback failed");
    require(runtimeTableStorage(table)->description.empty() &&
                runtimeTableStorage(table)->dimensionNames ==
                    std::vector<std::string>({"Row", "Variables"}),
            "table metadata mutation broke copy-on-write");

    auto rowByName = runtimeIndexTable(
        table, {text("r2"), text("B")});
    require(rowByName.succeeded, rowByName.error);
    require(runtimeDimensions(rowByName.value) ==
                std::vector<size_t>({1, 1}),
            "named table indexing has the wrong shape");
    member = runtimeTableMemberValue(rowByName.value, "B");
    require(member.succeeded && numberAt(member.value) == 4.0,
            "named table indexing selected the wrong value");

    auto reordered = runtimeIndexTable(
        table,
        {column({2.0}),
         makeRuntimeCellValue({text("B"), text("A")})});
    require(reordered.succeeded, reordered.error);
    require(runtimeTableStorage(reordered.value)->variables[0].name == "B" &&
                runtimeTableStorage(reordered.value)->variables[1].name == "A",
            "table variable-name selection did not preserve order");

    auto duplicated = runtimeIndexTable(
        table,
        {makeRuntimeCellValue({text("r1"), text("r1")}),
         makeRuntimeCellValue({text("A"), text("A")})});
    require(duplicated.succeeded, duplicated.error);
    const auto* duplicatedStorage = runtimeTableStorage(duplicated.value);
    require(duplicatedStorage &&
                duplicatedStorage->rowNames ==
                    std::vector<std::string>({"r1", "r1_1"}) &&
                duplicatedStorage->variables[0].name == "A" &&
                duplicatedStorage->variables[1].name == "A_1" &&
                validateRuntimeValueContract(duplicated.value).valid,
            "duplicate table selection did not produce legal unique names");

    auto contents = runtimeTableContents(
        table, {column({1.0, 2.0}), text("B")});
    require(contents.succeeded && contents.values.size() == 1 &&
                numberAt(contents.values.front(), 1) == 4.0,
            "table content extraction failed");

    auto assignedContents = runtimeAssignTableContents(
        table, {column({1.0, 2.0}), text("B")},
        column({11.0, 12.0}));
    require(assignedContents.succeeded, assignedContents.error);
    member = runtimeTableMemberValue(assignedContents.value, "B");
    require(member.succeeded && numberAt(member.value, 1) == 12.0,
            "table brace assignment failed");
    member = runtimeTableMemberValue(table, "B");
    require(member.succeeded && numberAt(member.value, 1) == 4.0,
            "table brace assignment mutated a shared source");

    auto added = runtimeSetTableMember(
        assignedContents.value, "Extra", column({5.0, 6.0}));
    require(added.succeeded && runtimeDimension(added.value, 1) == 3,
            "table variable insertion failed");
    auto deleted = runtimeDeleteTableIndexed(
        added.value,
        {column({1.0, 2.0}), text("Extra")}, {true, false});
    require(deleted.succeeded && runtimeDimension(deleted.value, 1) == 2,
            "complete table variable deletion failed");
    require(!runtimeDeleteTableIndexed(
                 added.value,
                 {column({1.0, 2.0}), text("Extra")}, {false, false})
                 .succeeded,
            "explicit full-row indices incorrectly acted as a colon deletion");

    auto rowDeleted = runtimeDeleteTableIndexed(
        table,
        {column({1.0}), column({1.0, 2.0})}, {false, true});
    require(rowDeleted.succeeded &&
                runtimeDimensions(rowDeleted.value) ==
                    std::vector<size_t>({1, 2}) &&
                runtimeTableStorage(rowDeleted.value)->rowNames ==
                    std::vector<std::string>({"r2"}),
            "table row deletion failed");
    member = runtimeTableMemberValue(rowDeleted.value, "B");
    require(member.succeeded && numberAt(member.value) == 4.0,
            "table row deletion selected the wrong remaining row");
    auto allRowsDeleted = runtimeDeleteTableIndexed(
        table,
        {column({1.0, 2.0}), column({1.0, 2.0})}, {true, true});
    require(allRowsDeleted.succeeded &&
                runtimeDimensions(allRowsDeleted.value) ==
                    std::vector<size_t>({0, 2}) &&
                runtimeTableStorage(allRowsDeleted.value)->variables.size() == 2,
            "T(:,:)=[] did not preserve variables while deleting rows");

    auto braceMatrix = runtimeNumericValueFromLogicalOrder(
        {2, 2}, {10.0, 20.0, 30.0, 40.0},
        RuntimeNumericClass::Double);
    require(braceMatrix.has_value(),
            "multi-variable brace assignment setup failed");
    auto multiBrace = runtimeAssignTableContents(
        table,
        {column({1.0, 2.0}), column({1.0, 2.0})}, *braceMatrix);
    require(multiBrace.succeeded, multiBrace.error);
    member = runtimeTableMemberValue(multiBrace.value, "A");
    require(member.succeeded && numberAt(member.value, 0) == 10.0 &&
                numberAt(member.value, 1) == 20.0,
            "multi-variable brace assignment did not split the first column");
    member = runtimeTableMemberValue(multiBrace.value, "B");
    require(member.succeeded && numberAt(member.value, 0) == 30.0 &&
                numberAt(member.value, 1) == 40.0,
            "multi-variable brace assignment did not split the second column");

    auto withEmptyWide = runtimeSetTableMember(
        table, "EmptyWide", makeRuntimeMatrixValue(2, 0, {}));
    require(withEmptyWide.succeeded &&
                runtimeDimension(withEmptyWide.value, 1) == 3,
            "zero-width table variable was treated as deletion");
    member = runtimeTableMemberValue(withEmptyWide.value, "EmptyWide");
    require(member.succeeded &&
                runtimeDimensions(member.value) ==
                    std::vector<size_t>({2, 0}),
            "zero-width table variable shape was not preserved");
    auto removedEmptyWide = runtimeSetTableMember(
        withEmptyWide.value, "EmptyWide",
        makeRuntimeMatrixValue(0, 0, {}), true);
    require(removedEmptyWide.succeeded &&
                runtimeDimension(removedEmptyWide.value, 1) == 2,
            "explicit member null assignment did not delete a variable");
    require(!runtimeSetTableMember(
                 table, "Absent", makeRuntimeMatrixValue(0, 0, {}), true)
                 .succeeded,
            "null assignment silently deleted an unavailable variable");

    auto replacement = runtimeMakeTable(
        {column({20.0})}, {"Ignored"});
    require(replacement.succeeded, replacement.error);
    auto indexedAssignment = runtimeAssignTableIndexed(
        table, {column({2.0}), text("A")}, replacement.value);
    require(indexedAssignment.succeeded, indexedAssignment.error);
    member = runtimeTableMemberValue(indexedAssignment.value, "A");
    require(member.succeeded && numberAt(member.value, 1) == 20.0,
            "table parenthesis assignment failed");

    auto wide = runtimeMakeTable(
        {makeRuntimeMatrixValue(2, 2, {1.0, 2.0, 3.0, 4.0})},
        {"Wide"});
    require(wide.succeeded && runtimeDimension(wide.value, 1) == 1,
            "wide table variables must count as one variable");
    contents = runtimeTableContents(
        wide.value, {column({1.0, 2.0}), text("Wide")});
    require(contents.succeeded && contents.values.size() == 1 &&
                runtimeDimensions(contents.values.front()) ==
                    std::vector<size_t>({2, 2}),
            "wide table variable contents lost shape");
    auto wideStructure = runtimeTableToStruct(wide.value);
    require(wideStructure.succeeded, wideStructure.error);
    auto wideRoundTrip = runtimeStructToTable(wideStructure.value);
    require(wideRoundTrip.succeeded &&
                runtimeValuesEqual(wideRoundTrip.value, wide.value),
            "wide table variable struct round trip lost its trailing shape");

    const RuntimeValue wideString = makeRuntimeStringArray(
        {2, 2},
        {{u"r1c1", false}, {u"r1c2", false},
         {u"r2c1", false}, {u"r2c2", false}});
    auto wideStringTable = runtimeMakeTable(
        {wideString}, {"WideString"});
    require(wideStringTable.succeeded, wideStringTable.error);
    auto wideStringStruct = runtimeTableToStruct(wideStringTable.value);
    auto wideStringRoundTrip = wideStringStruct.succeeded
                                   ? runtimeStructToTable(
                                         wideStringStruct.value)
                                   : RuntimeTableOperationResult{};
    require(wideStringStruct.succeeded &&
                wideStringRoundTrip.succeeded &&
                runtimeValuesEqual(
                    wideStringRoundTrip.value, wideStringTable.value),
            "wide string table variable changed representation in struct round trip");

    const RuntimeValue wideCell = makeRuntimeCellValue(
        {2, 2},
        {makeRuntimeNumberValue(1.0), makeRuntimeNumberValue(2.0),
         text("three"), text("four")});
    auto wideCellTable = runtimeMakeTable(
        {wideCell}, {"WideCell"});
    require(wideCellTable.succeeded, wideCellTable.error);
    auto wideCellStruct = runtimeTableToStruct(wideCellTable.value);
    auto wideCellRoundTrip = wideCellStruct.succeeded
                                 ? runtimeStructToTable(
                                       wideCellStruct.value)
                                 : RuntimeTableOperationResult{};
    require(wideCellStruct.succeeded &&
                wideCellRoundTrip.succeeded &&
                runtimeValuesEqual(
                    wideCellRoundTrip.value, wideCellTable.value),
            "wide Cell table variable became a nested Cell in struct round trip");

    const auto duration = runtimeTemporalUnit(
        "seconds", column({1.0, 2.0}));
    require(duration.succeeded, duration.error);
    auto temporalTable = runtimeMakeTable(
        {duration.value}, {"Elapsed"});
    require(temporalTable.succeeded, temporalTable.error);
    auto temporalRow = runtimeIndexTable(
        temporalTable.value, {column({2.0}), text("Elapsed")});
    require(temporalRow.succeeded, temporalRow.error);
    member = runtimeTableMemberValue(temporalRow.value, "Elapsed");
    require(member.succeeded && isRuntimeDurationValue(member.value) &&
                runtimeTemporalPayload(member.value).value_or(-1.0) == 2.0,
            "table row indexing does not preserve temporal values");
    const auto wideDuration = runtimeTemporalUnit(
        "seconds",
        makeRuntimeMatrixValue(2, 2, {1.0, 2.0, 3.0, 4.0}));
    require(wideDuration.succeeded, wideDuration.error);
    auto wideDurationTable = runtimeMakeTable(
        {wideDuration.value}, {"WideDuration"});
    require(wideDurationTable.succeeded, wideDurationTable.error);
    auto wideDurationStruct = runtimeTableToStruct(
        wideDurationTable.value);
    auto wideDurationRoundTrip =
        wideDurationStruct.succeeded
            ? runtimeStructToTable(wideDurationStruct.value)
            : RuntimeTableOperationResult{};
    require(wideDurationStruct.succeeded &&
                wideDurationRoundTrip.succeeded &&
                runtimeValuesEqual(
                    wideDurationRoundTrip.value,
                    wideDurationTable.value),
            "wide duration table variable changed representation in struct round trip");

    const RuntimeValue matrix =
        makeRuntimeMatrixValue(2, 2, {1.0, 2.0, 3.0, 4.0});
    auto fromArray = runtimeArrayToTable(
        matrix, {"Left", "Right"});
    require(fromArray.succeeded, fromArray.error);
    auto arrayResult = invoke("table2array", {fromArray.value});
    require(arrayResult.succeeded && arrayResult.outputs.size() == 1 &&
                runtimeValuesEqual(arrayResult.outputs.front(), matrix),
            "array2table/table2array round trip failed");

    auto horizontalLeft = runtimeMakeTable(
        {column({1.0, 2.0})}, {"Left"});
    auto horizontalRight = runtimeMakeTable(
        {column({3.0, 4.0})}, {"Right"});
    auto horizontal = runtimeConcatenateTables(
        2, {horizontalLeft.value, horizontalRight.value});
    require(horizontal.succeeded &&
                runtimeDimensions(horizontal.value) ==
                    std::vector<size_t>({2, 2}) &&
                runtimeTableStorage(horizontal.value)->variables[1].name ==
                    "Right",
            "horizontal table concatenation failed");
    require(!runtimeConcatenateTables(
                 2, {horizontalLeft.value, horizontalLeft.value})
                 .succeeded,
            "horizontal table concatenation accepted duplicate names");
    auto verticalBottom = runtimeMakeTable(
        {column({5.0, 6.0})}, {"Left"});
    auto vertical = runtimeConcatenateTables(
        1, {horizontalLeft.value, verticalBottom.value});
    require(vertical.succeeded &&
                runtimeDimensions(vertical.value) ==
                    std::vector<size_t>({4, 1}),
            "vertical table concatenation failed");
    member = runtimeTableMemberValue(vertical.value, "Left");
    require(member.succeeded && numberAt(member.value, 3) == 6.0,
            "vertical table concatenation lost the final row");

    const RuntimeValue categoryCells = makeRuntimeCellValue(
        {3, 1}, {text("b"), text("a"), text("b")});
    auto categories = runtimeConstructCategorical(categoryCells);
    require(categories.succeeded, categories.error);
    auto sortable = runtimeMakeTable(
        {column({3.0, 1.0, 2.0}), categories.value},
        {"X", "C"});
    require(sortable.succeeded, sortable.error);
    auto sorted = runtimeSortTable(
        sortable.value,
        {{RuntimeTableSortKeyKind::Variable, 1, false},
         {RuntimeTableSortKeyKind::Variable, 0, false}});
    require(sorted.succeeded &&
                sorted.order == std::vector<size_t>({1, 2, 0}),
            "sortrows categorical/numeric key order failed");
    member = runtimeTableMemberValue(sorted.value, "X");
    require(member.succeeded && numberAt(member.value, 0) == 1.0 &&
                numberAt(member.value, 2) == 3.0,
            "sortrows table row permutation failed");
    auto categoricalRowsDeleted = runtimeDeleteTableIndexed(
        sortable.value,
        {column({2.0}), column({1.0, 2.0})}, {false, true});
    require(categoricalRowsDeleted.succeeded,
            categoricalRowsDeleted.error);
    member = runtimeTableMemberValue(categoricalRowsDeleted.value, "C");
    require(member.succeeded && isRuntimeCategoricalValue(member.value) &&
                runtimeDimensions(member.value) ==
                    std::vector<size_t>({2, 1}) &&
                runtimeCategoricalLabel(member.value, 0) == "b",
            "table row deletion did not preserve categorical variables");

    const RuntimeValue zeroColumnMatrix =
        makeRuntimeMatrixValue(2, 0, {});
    require(!runtimeArrayToTable(
                 zeroColumnMatrix, {}, {"only-one-row"})
                 .succeeded,
            "zero-variable array2table accepted mismatched RowNames");
    auto zeroColumnTable = runtimeArrayToTable(
        zeroColumnMatrix, {}, {"r1", "r2"});
    require(zeroColumnTable.succeeded &&
                runtimeDimensions(zeroColumnTable.value) ==
                    std::vector<size_t>({2, 0}) &&
                validateRuntimeValueContract(zeroColumnTable.value).valid,
            "zero-variable array2table did not preserve a valid height");

    auto structure = runtimeTableToStruct(table);
    require(structure.succeeded &&
                runtimeStructElementCount(structure.value) == 2,
            "table2struct failed");
    auto fromStruct = runtimeStructToTable(
        structure.value, {}, {"r1", "r2"});
    require(fromStruct.succeeded &&
                runtimeValuesEqual(fromStruct.value, table),
            "struct2table round trip failed");

    const RuntimeValue emptyFieldStruct = makeRuntimeStructArrayValue(
        {}, {{}, {}}, {2, 1});
    require(!runtimeStructToTable(
                 emptyFieldStruct, {}, {"only-one-row"})
                 .succeeded,
            "zero-variable struct2table accepted mismatched RowNames");
    auto emptyFieldTable = runtimeStructToTable(
        emptyFieldStruct, {}, {"r1", "r2"});
    require(emptyFieldTable.succeeded &&
                runtimeDimensions(emptyFieldTable.value) ==
                    std::vector<size_t>({2, 0}) &&
                validateRuntimeValueContract(emptyFieldTable.value).valid,
            "zero-variable struct2table did not preserve a valid height");

    const RuntimeValue scalarStructure = makeRuntimeStructValue(
        {{"A", column({1.0, 2.0})},
         {"B", column({3.0, 4.0})}});
    auto scalarStructTable = runtimeStructToTable(scalarStructure);
    require(scalarStructTable.succeeded &&
                runtimeDimensions(scalarStructTable.value) ==
                    std::vector<size_t>({2, 2}),
            "scalar struct columns did not determine table height");
    member = runtimeTableMemberValue(scalarStructTable.value, "B");
    require(member.succeeded && numberAt(member.value, 1) == 4.0,
            "scalar struct column values were wrapped instead of expanded");

    auto empty = runtimeMakeTable({});
    require(empty.succeeded &&
                runtimeDimensions(empty.value) ==
                    std::vector<size_t>({0, 0}),
            "empty table construction failed");
    require(!runtimeIndexTable(table, {column({1.0})}).succeeded,
            "linear table indexing must be rejected");
    require(!runtimeMakeTable(
                 {column({1.0}), column({1.0, 2.0})})
                 .succeeded,
            "table accepted variables with mismatched row counts");
    require(!runtimeMakeTable(
                 {column({1.0}), column({2.0})}, {"A", "A"})
                 .succeeded,
            "table accepted duplicate variable names");
    require(!runtimeMakeTable(
                 {column({1.0})}, {"Properties"})
                 .succeeded,
            "table accepted the reserved Properties variable name");
    require(!runtimeMakeTable(
                 {column({1.0})}, {"Row"})
                 .succeeded,
            "table accepted a variable name that conflicts with a dimension");
    require(!runtimeMakeTable(
                 {column({1.0})}, {"A"}, {}, {"A", "Variables"})
                 .succeeded,
            "table accepted overlapping variable and dimension names");
    require(!runtimeSetTableMember(table, "Row", column({5.0, 6.0}))
                 .succeeded,
            "table member insertion accepted a dimension name");
    require(!runtimeAssignTableIndexed(
                 table,
                 {column({1.0, 2.0}),
                  makeRuntimeCellValue({text("A"), text("A")})},
                 runtimeMakeTable(
                     {column({5.0, 6.0}), column({7.0, 8.0})},
                     {"A", "A_1"})
                     .value)
                 .succeeded,
            "table assignment accepted duplicate variable selectors");
    require(!runtimeMakeTable(
                 {makeRuntimeFunctionHandleValue(RuntimeFunctionHandle{})},
                 {"Callback"})
                 .succeeded,
            "table accepted a variable without row-indexing semantics");

    auto compared = runtimeCompareTables("==", table, table);
    require(compared.succeeded && isRuntimeTableValue(compared.value),
            "table equality did not return a table");
    auto comparedArray = invoke("table2array", {compared.value});
    require(comparedArray.succeeded &&
                runtimeNumericTruthValue(comparedArray.outputs.front()) ==
                    std::optional<bool>(true),
            "table equality returned incorrect element values");

    RuntimeValue leftProperties = table;
    RuntimeValue rightProperties = table;
    auto* leftStorage = runtimeMutableTableStorage(leftProperties);
    auto* rightStorage = runtimeMutableTableStorage(rightProperties);
    require(leftStorage && rightStorage,
            "table property comparison setup failed");
    leftStorage->dimensionNames = {"LeftRows", "LeftVariables"};
    leftStorage->description = "left-description";
    leftStorage->userData = text("left-user-data");
    rightStorage->dimensionNames = {"RightRows", "RightVariables"};
    rightStorage->description = "right-description";
    rightStorage->userData = text("right-user-data");
    auto propertyComparison = runtimeCompareTables(
        "==", leftProperties, rightProperties);
    const auto* propertyStorage = propertyComparison.succeeded
                                      ? runtimeTableStorage(
                                            propertyComparison.value)
                                      : nullptr;
    require(propertyStorage &&
                propertyStorage->dimensionNames ==
                    leftStorage->dimensionNames &&
                propertyStorage->description ==
                    leftStorage->description &&
                runtimeValuesEqual(
                    propertyStorage->userData, leftStorage->userData),
            "table comparison did not inherit left-operand properties");

    auto equalityLeftLeaf = runtimeMakeTable(
        {column({1.0})}, {"Leaf"});
    auto equalityRightLeaf = runtimeMakeTable(
        {column({1.0})}, {"Leaf"});
    require(equalityLeftLeaf.succeeded && equalityRightLeaf.succeeded,
            "shared equality graph setup failed");
    RuntimeValue equalityLeft = equalityLeftLeaf.value;
    RuntimeValue equalityRight = equalityRightLeaf.value;
    for (size_t depth = 0; depth < 20; ++depth) {
        auto nextLeft = runtimeMakeTable(
            {makeRuntimeCellValue(
                {1, 2}, {equalityLeft, equalityLeft})},
            {"Nested"});
        auto nextRight = runtimeMakeTable(
            {makeRuntimeCellValue(
                {1, 2}, {equalityRight, equalityRight})},
            {"Nested"});
        require(nextLeft.succeeded && nextRight.succeeded,
                "shared equality graph construction failed");
        equalityLeft = std::move(nextLeft.value);
        equalityRight = std::move(nextRight.value);
    }
    require(runtimeValuesEqual(equalityLeft, equalityRight),
            "shared table equality graph did not terminate correctly");

    RuntimeValue comparisonLeft = equalityLeftLeaf.value;
    RuntimeValue comparisonRight = equalityRightLeaf.value;
    for (size_t depth = 0; depth < 20; ++depth) {
        auto nextLeft = runtimeMakeTable(
            {comparisonLeft, comparisonLeft}, {"Left", "Right"});
        auto nextRight = runtimeMakeTable(
            {comparisonRight, comparisonRight}, {"Left", "Right"});
        require(nextLeft.succeeded && nextRight.succeeded,
                "shared comparison graph construction failed");
        comparisonLeft = std::move(nextLeft.value);
        comparisonRight = std::move(nextRight.value);
    }
    auto sharedComparison = runtimeCompareTables(
        "==", comparisonLeft, comparisonRight);
    require(sharedComparison.succeeded,
            "shared table comparison graph did not terminate: " +
                sharedComparison.error);

    auto nanTable = runtimeMakeTable(
        {column({std::numeric_limits<double>::quiet_NaN()})}, {"A"});
    require(nanTable.succeeded, nanTable.error);
    const RuntimeValue sharedNanTable = nanTable.value;
    require(!runtimeValuesEqual(nanTable.value, sharedNanTable) &&
                runtimeValuesEqual(
                    nanTable.value, sharedNanTable,
                    RuntimeNaNEquality::Equal),
            "shared table storage bypassed NaN equality policy");

    const auto registry = defaultBuiltinRegistry();
    for (const std::string_view name : {
             "table", "height", "width", "istable", "array2table",
             "table2array", "struct2table", "table2struct", "sortrows"}) {
        const BuiltinDescriptor* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        BuiltinImplementationKind::Shared &&
                    descriptor->purity == BuiltinPurity::Pure &&
                    descriptor->errorIdentifier ==
                        "MParser:InvalidTableCall",
                "table builtin registry metadata is incomplete: " +
                    std::string(name));
    }
    auto height = invoke("height", {table});
    auto width = invoke("width", {table});
    auto isTable = invoke("istable", {table});
    require(height.succeeded && width.succeeded && isTable.succeeded &&
                numberAt(height.outputs.front()) == 2.0 &&
                numberAt(width.outputs.front()) == 2.0 &&
                numberAt(isTable.outputs.front()) == 1.0,
            "table query builtins returned incorrect values");
    auto sortedBuiltin = invoke(
        "sortrows", {sortable.value, text("C")}, 2);
    require(sortedBuiltin.succeeded &&
                sortedBuiltin.outputs.size() == 2 &&
                numberAt(sortedBuiltin.outputs[1], 0) == 2.0 &&
                numberAt(sortedBuiltin.outputs[1], 1) == 1.0 &&
                numberAt(sortedBuiltin.outputs[1], 2) == 3.0,
            "sortrows builtin outputs are incorrect: " +
                (sortedBuiltin.succeeded &&
                         sortedBuiltin.outputs.size() == 2
                     ? runtimeValueToString(sortedBuiltin.outputs[1])
                     : (!sortedBuiltin.diagnostics.empty()
                            ? sortedBuiltin.diagnostics.front().message
                            : std::string("call failed"))));

    const RuntimeValue emptyNames = makeRuntimeCellValue({0, 0}, {});
    require(!invoke(
                 "table",
                 {column({1.0}),
                  makeRuntimeNameValueArgument(
                      "VariableNames", emptyNames)})
                 .succeeded,
            "table accepted explicit empty VariableNames for a variable");
    require(!invoke(
                 "table",
                 {column({1.0}),
                  makeRuntimeNameValueArgument(
                      "DimensionNames", emptyNames)})
                 .succeeded,
            "table accepted explicit empty DimensionNames");
    require(!invoke(
                 "array2table",
                 {matrix,
                  makeRuntimeNameValueArgument(
                      "VariableNames", emptyNames)})
                 .succeeded,
            "array2table accepted explicit empty VariableNames");

    std::cout << "table runtime smoke passed\n";
    return 0;
}
