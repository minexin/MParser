#pragma once

#include "mparser/runtime/core/value/runtime_table.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mparser {

using RuntimeTimetableVariable = RuntimeTabularVariable;
using RuntimeTimetableStorage = RuntimeTabularStorage;

struct RuntimeTimetableRowSelectionResult {
    bool succeeded = false;
    std::vector<size_t> indices;
    std::string error;
};

bool isRuntimeTimetableValue(const RuntimeValue& value);

const RuntimeTabularStorage* runtimeTimetableStorage(
    const RuntimeValue& value);
RuntimeTabularStorage* runtimeMutableTimetableStorage(RuntimeValue& value);

RuntimeValue makeRuntimeTimetableValue(
    std::shared_ptr<RuntimeTabularStorage> storage);

RuntimeTableOperationResult runtimeMakeTimetable(
    RuntimeValue rowTimes,
    std::vector<RuntimeValue> variables = {},
    std::vector<std::string> variableNames = {},
    std::vector<std::string> dimensionNames = {});

RuntimeTableOperationResult runtimeArrayToTimetable(
    const RuntimeValue& value, const RuntimeValue& rowTimes,
    std::vector<std::string> variableNames = {});
RuntimeTableOperationResult runtimeTableToTimetable(
    const RuntimeValue& table,
    const RuntimeValue* explicitRowTimes = nullptr);
RuntimeTableOperationResult runtimeTimetableToTable(
    const RuntimeValue& timetable, bool convertRowTimes = true);
RuntimeTableOperationResult runtimeSetTimetableRowTimes(
    const RuntimeValue& timetable, const RuntimeValue& rowTimes);

RuntimeTableOperationResult runtimeConcatenateTimetables(
    size_t dimension, const std::vector<RuntimeValue>& values);

RuntimeTimetableRowSelectionResult runtimeResolveTimetableRowSelector(
    const RuntimeValue& timetable, const RuntimeValue& selector);

bool validateRuntimeTimetableStorage(const RuntimeValue& value,
                                     std::string& error);
bool runtimeTimetableValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableValueEquality& valueEquality);

} // namespace mparser
