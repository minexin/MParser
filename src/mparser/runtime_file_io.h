#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

enum class RuntimeFileByteOrder {
    LittleEndian,
    BigEndian,
};

struct RuntimeFileLineResult {
    std::string text;
    std::string terminator;
    size_t consumedBytes = 0;
    bool hasValue = false;
};

struct RuntimeBinaryPrecision {
    RuntimeNumericClass sourceClass = RuntimeNumericClass::UInt8;
    RuntimeNumericClass outputClass = RuntimeNumericClass::Double;
    size_t valuesPerBlock = 1;
};

struct RuntimeBinaryPrecisionResult {
    bool succeeded = false;
    RuntimeBinaryPrecision precision;
    std::string error;
};

struct RuntimeBinaryReadSize {
    std::optional<size_t> maximumValues;
    bool scalarRequested = false;
    bool matrixRequested = false;
    size_t rows = 0;
    std::optional<size_t> columns;
};

struct RuntimeBinaryReadResult {
    bool succeeded = false;
    RuntimeValue value;
    size_t valueCount = 0;
    size_t consumedBytes = 0;
    std::string error;
};

struct RuntimeBinaryWriteResult {
    bool succeeded = false;
    std::string bytes;
    size_t valueCount = 0;
    size_t blockBytes = 0;
    std::string error;
};

RuntimeFileScanResult runtimeScanFormattedText(
    std::string_view input, std::string_view format,
    const RuntimeFileScanSize& size = {});

RuntimeFileByteOrder runtimeNativeFileByteOrder();
std::optional<RuntimeFileByteOrder>
runtimeFileByteOrderFromName(std::string_view name);
std::string_view runtimeFileByteOrderName(RuntimeFileByteOrder order);

RuntimeFileLineResult runtimeReadFileLine(
    std::string_view input, bool keepTerminator,
    std::optional<size_t> maximumCharacters = std::nullopt);

RuntimeBinaryPrecisionResult runtimeParseBinaryPrecision(
    std::string_view text, bool allowOutputClass);
RuntimeBinaryReadResult runtimeDecodeBinaryData(
    std::string_view input,
    const RuntimeBinaryPrecision& precision,
    const RuntimeBinaryReadSize& size, size_t skipBytes,
    RuntimeFileByteOrder byteOrder);
RuntimeBinaryWriteResult runtimeEncodeBinaryData(
    const RuntimeValue& value,
    const RuntimeBinaryPrecision& precision,
    RuntimeFileByteOrder byteOrder);

} // namespace mparser
