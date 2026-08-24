#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::string_view kRuntimeTableClassName = "table";
inline constexpr std::string_view kRuntimeTimetableClassName = "timetable";

enum class RuntimeTabularKind {
    Table,
    Timetable,
};

enum class RuntimeTabularRowAxisKind {
    None,
    Names,
    Times,
};

struct RuntimeTabularVariable {
    std::string name;
    RuntimeValue value;
};

struct RuntimeTabularStorage {
    RuntimeTabularKind kind = RuntimeTabularKind::Table;
    size_t rowCount = 0;
    std::vector<RuntimeTabularVariable> variables;
    RuntimeTabularRowAxisKind rowAxisKind =
        RuntimeTabularRowAxisKind::None;
    std::vector<std::string> rowNames;
    RuntimeValue rowTimes;
    std::vector<std::string> dimensionNames{"Row", "Variables"};
    std::string description;
    RuntimeValue userData;
};

bool isRuntimeTabularValue(const RuntimeValue& value);

const RuntimeTabularStorage* runtimeTabularStorage(
    const RuntimeValue& value);
RuntimeTabularStorage* runtimeMutableTabularStorage(RuntimeValue& value);

bool runtimeTabularVariableSupportsRows(const RuntimeValue& value);

} // namespace mparser
