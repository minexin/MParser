#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_table.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_timetable.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "timetable runtime smoke failure: "
                  << message << '\n';
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

mparser::RuntimeValue text(std::string value) {
    return mparser::makeRuntimeCharacterVectorUtf8(value);
}

double numberAt(const mparser::RuntimeValue& value, size_t index = 0) {
    const auto result = mparser::runtimeNumericElement(value, index);
    require(result.has_value(), "numeric element is unavailable");
    return *result;
}

mparser::RuntimeValue dates(std::vector<double> days) {
    auto result = mparser::runtimeConstructDateTime({
        mparser::makeRuntimeNumberValue(2024.0),
        mparser::makeRuntimeNumberValue(1.0),
        column(std::move(days))});
    require(result.succeeded, result.error);
    return std::move(result.value);
}

mparser::BuiltinResult invoke(
    std::string_view name,
    const std::vector<mparser::RuntimeValue>& arguments,
    size_t requestedOutputs = 1) {
    const auto registry = mparser::defaultBuiltinRegistry();
    return registry->invoke(
        name, mparser::BuiltinCall{
                  arguments, requestedOutputs,
                  mparser::SourceSpan{}, nullptr});
}

} // namespace

int main() {
    using namespace mparser;

    const RuntimeValue rowTimes = dates({2.0, 1.0, 2.0});
    auto made = runtimeMakeTimetable(
        rowTimes, {column({3.0, 1.0, 2.0})}, {"X"});
    require(made.succeeded, made.error);
    const RuntimeValue timetable = made.value;
    require(isRuntimeTimetableValue(timetable),
            "constructed value is not a timetable");
    require(runtimeDimensions(timetable) ==
                std::vector<size_t>({3, 1}),
            "timetable shape is not height-by-variable-count");
    require(runtimeValueToString(timetable) ==
                "<timetable 3x1 variables=X>",
            "timetable display summary changed");
    const auto contract = validateRuntimeValueContract(timetable);
    require(contract.valid,
            "timetable contract failed: " + contract.error);
    const auto bytes = runtimeValueArrayBytes(timetable);
    require(bytes && *bytes >= 6 * sizeof(double),
            "timetable recursive byte accounting omitted RowTimes");
    require(runtimeValuesEqual(timetable, timetable),
            "timetable must equal an identical value");

    const RuntimeTabularStorage* storage =
        runtimeTimetableStorage(timetable);
    require(storage &&
                storage->rowAxisKind == RuntimeTabularRowAxisKind::Times &&
                storage->dimensionNames ==
                    std::vector<std::string>({"Time", "Variables"}) &&
                runtimeDimensions(storage->rowTimes) ==
                    std::vector<size_t>({3, 1}),
            "timetable RowTimes metadata is invalid");

    auto member = runtimeTableMemberValue(timetable, "Time");
    require(member.succeeded &&
                runtimeTemporalValuesEqual(member.value, rowTimes),
            "row-time dimension member access failed");
    auto properties = runtimeTableMemberValue(timetable, "Properties");
    require(properties.succeeded &&
                isRuntimeScalarStruct(properties.value) &&
                runtimeStructField(properties.value, "RowTimes") != nullptr &&
                runtimeStructField(properties.value, "RowNames") == nullptr,
            "timetable Properties did not expose RowTimes exclusively");

    const RuntimeValue requested = dates({2.0});
    auto selected = runtimeIndexTable(
        timetable, {requested, makeRuntimeNumberValue(1.0)});
    require(selected.succeeded, selected.error);
    require(runtimeDimensions(selected.value) ==
                std::vector<size_t>({2, 1}),
            "exact time indexing did not preserve duplicate matches");
    member = runtimeTableMemberValue(selected.value, "X");
    require(member.succeeded && numberAt(member.value, 0) == 3.0 &&
                numberAt(member.value, 1) == 2.0,
            "exact time indexing selected the wrong rows");

    const RuntimeValue replacementTimes = dates({3.0, 4.0, 5.0});
    auto retimed = runtimeSetTimetableRowTimes(
        timetable, replacementTimes);
    require(retimed.succeeded, retimed.error);
    require(runtimeTemporalValuesEqual(
                runtimeTimetableStorage(retimed.value)->rowTimes,
                replacementTimes) &&
                runtimeTemporalValuesEqual(
                    runtimeTimetableStorage(timetable)->rowTimes,
                    rowTimes),
            "RowTimes assignment broke copy-on-write");
    auto invalidRetiming = runtimeSetTimetableRowTimes(
        timetable, dates({1.0, 2.0}));
    require(!invalidRetiming.succeeded,
            "RowTimes accepted a mismatched height");

    auto assignmentSource = runtimeMakeTimetable(
        dates({9.0}), {column({99.0})}, {"X"});
    require(assignmentSource.succeeded, assignmentSource.error);
    auto assignedRow = runtimeAssignTableIndexed(
        timetable,
        {makeRuntimeNumberValue(2.0), makeRuntimeNumberValue(1.0)},
        assignmentSource.value);
    require(assignedRow.succeeded, assignedRow.error);
    require(numberAt(runtimeTableMemberValue(
                         assignedRow.value, "X").value, 1) == 99.0 &&
                runtimeTemporalValuesEqual(
                    runtimeTimetableStorage(assignedRow.value)->rowTimes,
                    rowTimes),
            "timetable row assignment must preserve target RowTimes");
    assignmentSource.value.tabularStorage->dimensionNames =
        {"OtherTime", "Variables"};
    auto mismatchedAssignment = runtimeAssignTableIndexed(
        timetable,
        {makeRuntimeNumberValue(2.0), makeRuntimeNumberValue(1.0)},
        assignmentSource.value);
    require(!mismatchedAssignment.succeeded,
            "timetable assignment accepted mismatched dimension names");

    auto rowDeleted = runtimeDeleteTableIndexed(
        timetable,
        {makeRuntimeNumberValue(2.0), makeRuntimeNumberValue(1.0)},
        {false, true});
    require(rowDeleted.succeeded, rowDeleted.error);
    require(runtimeDimensions(rowDeleted.value) ==
                std::vector<size_t>({2, 1}) &&
                runtimeDimensions(
                    runtimeTimetableStorage(rowDeleted.value)->rowTimes) ==
                    std::vector<size_t>({2, 1}),
            "timetable row deletion did not update RowTimes transactionally");

    auto vertical = runtimeConcatenateTimetables(
        1, {timetable, timetable});
    require(vertical.succeeded &&
                runtimeDimensions(vertical.value) ==
                    std::vector<size_t>({6, 1}),
            vertical.error.empty()
                ? "vertical timetable concatenation failed"
                : vertical.error);
    auto right = runtimeMakeTimetable(
        rowTimes, {column({30.0, 10.0, 20.0})}, {"Y"});
    require(right.succeeded, right.error);
    auto horizontal = runtimeConcatenateTimetables(
        2, {timetable, right.value});
    require(horizontal.succeeded &&
                runtimeDimensions(horizontal.value) ==
                    std::vector<size_t>({3, 2}),
            horizontal.error.empty()
                ? "horizontal timetable concatenation failed"
                : horizontal.error);

    auto sorted = runtimeSortTable(
        timetable,
        {{RuntimeTableSortKeyKind::Variable, 0, false}});
    require(sorted.succeeded &&
                sorted.order == std::vector<size_t>({1, 2, 0}),
            sorted.error.empty() ? "timetable sortrows order failed"
                                 : sorted.error);
    require(runtimeTemporalPayload(
                runtimeTimetableStorage(sorted.value)->rowTimes, 0) ==
                runtimeTemporalPayload(rowTimes, 1),
            "timetable sortrows did not permute RowTimes");
    auto sortedByTime = runtimeSortTable(
        timetable,
        {{RuntimeTableSortKeyKind::RowAxis, 0, false}});
    require(sortedByTime.succeeded &&
                sortedByTime.order ==
                    std::vector<size_t>({1, 0, 2}) &&
                runtimeTemporalPayload(
                    runtimeTimetableStorage(sortedByTime.value)->rowTimes,
                    0) == runtimeTemporalPayload(rowTimes, 1),
            sortedByTime.error.empty()
                ? "timetable RowTimes sort key failed"
                : sortedByTime.error);

    auto sourceTable = runtimeMakeTable(
        {rowTimes, column({3.0, 1.0, 2.0})}, {"When", "X"});
    require(sourceTable.succeeded, sourceTable.error);
    auto converted = runtimeTableToTimetable(sourceTable.value);
    require(converted.succeeded, converted.error);
    storage = runtimeTimetableStorage(converted.value);
    require(storage && storage->variables.size() == 1 &&
                storage->variables.front().name == "X" &&
                storage->dimensionNames.front() == "When",
            "table2timetable did not consume the first temporal variable");

    auto explicitConversion = runtimeTableToTimetable(
        runtimeMakeTable({column({3.0, 1.0, 2.0})}, {"X"}).value,
        &rowTimes);
    require(explicitConversion.succeeded &&
                runtimeTimetableStorage(explicitConversion.value)
                        ->variables.size() == 1,
            explicitConversion.error.empty()
                ? "explicit table2timetable conversion failed"
                : explicitConversion.error);
    auto asTable = runtimeTimetableToTable(converted.value);
    require(asTable.succeeded &&
                runtimeTableStorage(asTable.value)->variables.size() == 2 &&
                runtimeTableStorage(asTable.value)
                        ->variables.front().name == "When",
            asTable.error.empty()
                ? "timetable2table default omitted RowTimes"
                : asTable.error);
    auto withoutTimes = runtimeTimetableToTable(converted.value, false);
    require(withoutTimes.succeeded &&
                runtimeTableStorage(withoutTimes.value)
                        ->variables.size() == 1,
            withoutTimes.error.empty()
                ? "timetable2table ConvertRowTimes=false failed"
                : withoutTimes.error);

    auto arrayConverted = runtimeArrayToTimetable(
        makeRuntimeMatrixValue(3, 2,
                               {1.0, 2.0, 3.0,
                                4.0, 5.0, 6.0}),
        rowTimes, {"A", "B"});
    require(arrayConverted.succeeded &&
                runtimeDimensions(arrayConverted.value) ==
                    std::vector<size_t>({3, 2}),
            arrayConverted.error.empty()
                ? "array2timetable conversion failed"
                : arrayConverted.error);

    auto nat = runtimeConstructNaT(
        {makeRuntimeNumberValue(3.0), makeRuntimeNumberValue(1.0)});
    require(nat.succeeded, nat.error);
    auto natTimetable = runtimeMakeTimetable(
        nat.value, {column({1.0, 2.0, 3.0})}, {"X"});
    require(natTimetable.succeeded &&
                validateRuntimeValueContract(natTimetable.value).valid,
            "NaT RowTimes must remain legal");

    for (const std::string_view name : {
             "timetable", "istimetable", "array2timetable",
             "table2timetable", "timetable2table"}) {
        const auto* descriptor = defaultBuiltinRegistry()->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        BuiltinImplementationKind::Shared &&
                    descriptor->errorIdentifier ==
                        "MParser:InvalidTimetableCall",
                "timetable registry descriptor is incomplete: " +
                    std::string(name));
    }
    auto built = invoke(
        "timetable",
        {rowTimes, column({3.0, 1.0, 2.0}),
         makeRuntimeNameValueArgument(
             "VariableNames",
             makeRuntimeCellValue({1, 1}, {text("X")}))});
    require(built.succeeded && built.outputs.size() == 1 &&
                isRuntimeTimetableValue(built.outputs.front()),
            "timetable registry invocation failed");
    auto empty = invoke("timetable", {});
    require(empty.succeeded && empty.outputs.size() == 1 &&
                runtimeDimensions(empty.outputs.front()) ==
                    std::vector<size_t>({0, 0}),
            "empty timetable construction failed");
    auto predicate = invoke("istimetable", {timetable});
    require(predicate.succeeded && predicate.outputs.size() == 1 &&
                numberAt(predicate.outputs.front()) == 1.0,
            "istimetable predicate failed");

    std::cout << "timetable runtime smoke passed\n";
    return 0;
}
