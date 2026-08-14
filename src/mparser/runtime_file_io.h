#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace mparser {

struct RuntimeFileScanSize {
    std::optional<size_t> maximumMatches;
    bool scalarRequested = false;
    bool matrixRequested = false;
    size_t rows = 0;
    std::optional<size_t> columns;
};

struct RuntimeFileScanResult {
    bool succeeded = false;
    RuntimeValue value;
    size_t matchedCount = 0;
    size_t consumedBytes = 0;
    std::string error;
};

RuntimeFileScanResult runtimeScanFormattedText(
    std::string_view input, std::string_view format,
    const RuntimeFileScanSize& size = {});

} // namespace mparser
