#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

inline constexpr std::string_view kRuntimeCategoricalClassName =
    "categorical";
inline constexpr std::uint32_t kRuntimeCategoricalUndefinedCode = 0;

struct RuntimeCategoricalStorage {
    std::vector<std::string> categories;
    std::vector<std::uint32_t> codes;
    bool ordinal = false;
    bool protectedCategories = false;
};

struct RuntimeCategoricalOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

struct RuntimeCategoricalNamesResult {
    bool succeeded = false;
    std::vector<std::string> names;
    std::string error;
};

bool isRuntimeCategoricalValue(const RuntimeValue& value);

const RuntimeCategoricalStorage* runtimeCategoricalStorage(
    const RuntimeValue& value);
RuntimeCategoricalStorage* runtimeMutableCategoricalStorage(
    RuntimeValue& value);

RuntimeCategoricalOperationResult runtimeMakeCategoricalValue(
    std::vector<size_t> dimensions,
    std::vector<std::string> categories,
    std::vector<std::uint32_t> logicalCodes,
    bool ordinal = false, bool protectedCategories = false);

RuntimeCategoricalOperationResult runtimeConstructCategorical(
    const RuntimeValue& data,
    const RuntimeValue* valueSet = nullptr,
    const RuntimeValue* categoryNames = nullptr,
    bool ordinal = false, bool protectedCategories = false);

RuntimeCategoricalNamesResult runtimeCategoricalNames(
    const RuntimeValue& value, std::string_view role);

std::uint32_t runtimeCategoricalCode(
    const RuntimeValue& value, size_t logicalIndex);
std::string_view runtimeCategoricalLabel(
    const RuntimeValue& value, size_t logicalIndex);

RuntimeCategoricalOperationResult runtimeIndexCategorical(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon = false);
RuntimeCategoricalOperationResult runtimeAssignCategoricalIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);
RuntimeCategoricalOperationResult runtimeDeleteCategoricalIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts);
RuntimeCategoricalOperationResult runtimeConcatenateCategorical(
    size_t dimension, const std::vector<RuntimeValue>& values);

RuntimeCategoricalOperationResult runtimeCompareCategorical(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);
RuntimeCategoricalOperationResult runtimeCategoricalMissingMask(
    const RuntimeValue& value);
RuntimeCategoricalOperationResult runtimeCategoricalToDouble(
    const RuntimeValue& value);
RuntimeCategoricalOperationResult runtimeCategoricalToString(
    const RuntimeValue& value);

RuntimeCategoricalOperationResult runtimeAddCategories(
    const RuntimeValue& value, std::vector<std::string> names,
    std::string_view placement = {},
    std::string_view anchor = {});
RuntimeCategoricalOperationResult runtimeRemoveCategories(
    const RuntimeValue& value, const std::vector<std::string>& names);
RuntimeCategoricalOperationResult runtimeRenameCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& oldNames,
    const std::vector<std::string>& newNames);
RuntimeCategoricalOperationResult runtimeReorderCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& names);
RuntimeCategoricalOperationResult runtimeMergeCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& names,
    std::string mergedName);
RuntimeCategoricalOperationResult runtimeCategoricalCounts(
    const RuntimeValue& value);

bool validateRuntimeCategoricalStorage(
    const RuntimeValue& value, std::string& error);
bool runtimeCategoricalValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    bool equalUndefined);

} // namespace mparser
