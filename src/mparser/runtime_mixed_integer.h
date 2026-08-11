#pragma once

#include "mparser/runtime_numeric.h"

#include <string_view>

namespace mparser {

struct RuntimeMixedIntegerOperationResult {
    bool handled = false;
    bool succeeded = false;
    RuntimeNumericElementValue value;
};

RuntimeMixedIntegerOperationResult runtimeApplyMixedIntegerDoubleOperation(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass resultClass);

} // namespace mparser
