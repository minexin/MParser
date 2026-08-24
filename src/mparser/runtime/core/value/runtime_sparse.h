#pragma once

#include "mparser/runtime/core/value/runtime_numeric.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::string_view kRuntimeSparseClassName = "sparse";

// Canonical compressed-column storage.  Row indices are strictly increasing
// within each column and explicit zero entries are omitted.
struct RuntimeSparseStorage {
    size_t rows = 0;
    size_t columns = 0;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    bool complex = false;
    std::vector<size_t> columnPointers;
    std::vector<size_t> rowIndices;
    std::vector<double> values;
    std::vector<double> imaginaryValues;
};

struct RuntimeSparseOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

bool isRuntimeSparseValue(const RuntimeValue& value);

const RuntimeSparseStorage* runtimeSparseStorage(
    const RuntimeValue& value);
RuntimeSparseStorage* runtimeMutableSparseStorage(RuntimeValue& value);

RuntimeValue makeRuntimeSparseValue(
    std::shared_ptr<RuntimeSparseStorage> storage);

bool validateRuntimeSparseStorage(const RuntimeValue& value,
                                  std::string& error);

std::optional<RuntimeNumericElementValue> runtimeSparseElementValue(
    const RuntimeValue& value, size_t logicalIndex);
std::optional<RuntimeNumericElementValue>
runtimeSparseStorageElementValue(const RuntimeValue& value,
                                 size_t storageOffset);

bool runtimeStoreSparseElementValue(
    RuntimeValue& target, size_t logicalIndex,
    const RuntimeNumericElementValue& value);

RuntimeSparseOperationResult runtimeSparseFromNumeric(
    const RuntimeValue& value);
RuntimeSparseOperationResult runtimeSparseToFull(
    const RuntimeValue& value);
RuntimeSparseOperationResult runtimeSparseNonzeros(
    const RuntimeValue& value);
RuntimeSparseOperationResult runtimeSparseSpones(
    const RuntimeValue& value);
RuntimeSparseOperationResult runtimeSparseConvertClass(
    const RuntimeValue& value, RuntimeNumericClass numericClass);

RuntimeSparseOperationResult runtimeConstructSparse(
    const std::vector<RuntimeValue>& arguments);
RuntimeSparseOperationResult runtimeSpalloc(
    const std::vector<RuntimeValue>& arguments);
RuntimeSparseOperationResult runtimeSpeye(
    const std::vector<RuntimeValue>& arguments);

RuntimeSparseOperationResult runtimeSparseUnary(
    std::string_view operation, const RuntimeValue& value);
RuntimeSparseOperationResult runtimeSparseTranspose(
    const RuntimeValue& value, bool conjugate);

} // namespace mparser
