#pragma once

#include "mparser/runtime/core/value/runtime_native_numeric.h"
#include "mparser/runtime/core/value/runtime_numeric.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace mparser {

struct RuntimeDenseMatrixInput {
    native_numeric::Matrix matrix;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    bool complex = false;
};

bool isRuntimeFloatingNumericValue(const RuntimeValue& value);

RuntimeNumericClass runtimeFloatingNumericResultClass(
    std::initializer_list<const RuntimeValue*> values);

std::optional<native_numeric::Complex> runtimeDenseNumericElement(
    const RuntimeValue& value, size_t logicalIndex);

std::optional<RuntimeValue> makeRuntimeDenseNumericValue(
    std::vector<size_t> dimensions,
    const std::vector<native_numeric::Complex>& values,
    RuntimeNumericClass numericClass, bool preserveComplex = false);

std::optional<RuntimeValue> makeRuntimeDenseNumericValue(
    std::vector<size_t> dimensions,
    const native_numeric::Matrix& matrix,
    RuntimeNumericClass numericClass, bool preserveComplex = false);

std::optional<RuntimeDenseMatrixInput> runtimeDenseMatrixInput(
    const RuntimeValue& value, std::string& error);

RuntimeNumericOperationResult runtimeApplyDenseMatrixDivision(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);

} // namespace mparser
