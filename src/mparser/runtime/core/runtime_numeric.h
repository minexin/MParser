#pragma once

#include "mparser/runtime/core/runtime_value.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

bool isRuntimeNumericValue(const RuntimeValue& value);

bool isRuntimeLogical(const RuntimeValue& value);

std::string_view runtimeNumericClassName(RuntimeNumericClass numericClass);

std::optional<RuntimeNumericClass>
runtimeNumericClassFromName(std::string_view name);

bool runtimeNumericClassIsFloating(RuntimeNumericClass numericClass);

bool runtimeNumericClassIsInteger(RuntimeNumericClass numericClass);

bool runtimeNumericClassIsSignedInteger(RuntimeNumericClass numericClass);

bool runtimeNumericClassHasLegacyDoubleStorage(
    RuntimeNumericClass numericClass);

struct RuntimeNumericElementValue {
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    double real = 0.0;
    double imaginary = 0.0;
    std::uint64_t integerRealBits = 0;
    std::uint64_t integerImaginaryBits = 0;
    bool complex = false;
};

std::optional<RuntimeValue> runtimeParseNumericLiteral(
    std::string_view text);

std::optional<double> runtimeCoerceNumericElement(
    double value, RuntimeNumericClass numericClass);

std::optional<double> runtimeNumericElement(
    const RuntimeValue& value, size_t logicalIndex);

std::optional<RuntimeNumericElementValue> runtimeNumericElementValue(
    const RuntimeValue& value, size_t logicalIndex);

std::optional<size_t> runtimeNumericElementAsNonnegativeSize(
    const RuntimeNumericElementValue& value);

std::optional<RuntimeNumericElementValue> runtimeNumericStorageElementValue(
    const RuntimeValue& value, size_t storageOffset);

std::optional<RuntimeNumericElementValue> runtimeConvertNumericElementValue(
    const RuntimeNumericElementValue& value,
    RuntimeNumericClass numericClass);

std::optional<RuntimeNumericElementValue> runtimeApplyNumericElementBinary(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass resultClass);

bool runtimeStoreNumericElementValue(
    RuntimeValue& target, size_t logicalIndex,
    const RuntimeNumericElementValue& value);

std::optional<RuntimeValue> runtimeNumericValueFromLogicalOrder(
    std::vector<size_t> dimensions, std::vector<double> values,
    RuntimeNumericClass numericClass);

std::optional<RuntimeValue> runtimeNumericValueFromElements(
    std::vector<size_t> dimensions,
    std::vector<RuntimeNumericElementValue> values,
    RuntimeNumericClass numericClass);

std::optional<std::vector<RuntimeValue>>
runtimeNumericForLoopColumns(const RuntimeValue& value);

std::optional<RuntimeValue> runtimeConvertNumericClass(
    RuntimeValue value, RuntimeNumericClass numericClass);

bool runtimeNumericPredicate(std::string_view name,
                             const RuntimeValue& value);

std::optional<bool> runtimeNumericTruthValue(
    const RuntimeValue& value);

bool runtimeNumericValuesIdentical(
    const RuntimeValue& left, const RuntimeValue& right);

bool runtimeNumericValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    bool equalNaNs = false);

int runtimeCompareNumericElementsForExtrema(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right);

struct RuntimeNumericOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

RuntimeNumericOperationResult runtimeApplyNumericUnary(
    std::string_view operation, const RuntimeValue& value);

RuntimeNumericOperationResult runtimeApplyNumericBinary(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);

RuntimeNumericOperationResult runtimeTransposeNumeric(
    const RuntimeValue& value, bool conjugate);

bool isRuntimeComplexNumericBuiltin(std::string_view name);

RuntimeNumericOperationResult runtimeApplyComplexNumericBuiltin(
    std::string_view name,
    const std::vector<RuntimeValue>& arguments);

} // namespace mparser
