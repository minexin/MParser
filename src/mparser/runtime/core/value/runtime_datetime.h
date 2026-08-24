#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <optional>
#include <string>
#include <string_view>

namespace mparser {

inline constexpr std::string_view kRuntimeDateTimeClassName = "datetime";
inline constexpr std::string_view kRuntimeDurationClassName = "duration";

enum class RuntimeTemporalKind {
    DateTime,
    Duration,
};

struct RuntimeTemporalOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

bool isRuntimeDateTimeValue(const RuntimeValue& value);
bool isRuntimeDurationValue(const RuntimeValue& value);
bool isRuntimeTemporalValue(const RuntimeValue& value);
std::optional<RuntimeTemporalKind>
runtimeTemporalKind(const RuntimeValue& value);

std::optional<double> runtimeTemporalPayload(
    const RuntimeValue& value, size_t logicalIndex = 0);

RuntimeTemporalOperationResult runtimeConstructDateTime(
    const std::vector<RuntimeValue>& arguments);
RuntimeTemporalOperationResult runtimeConstructDuration(
    const std::vector<RuntimeValue>& arguments);
RuntimeTemporalOperationResult runtimeConstructNaT(
    const std::vector<RuntimeValue>& arguments);

RuntimeTemporalOperationResult runtimeTemporalComponent(
    std::string_view name, const RuntimeValue& value);
RuntimeTemporalOperationResult runtimeTemporalUnit(
    std::string_view name, const RuntimeValue& value);
RuntimeTemporalOperationResult runtimeTemporalPredicate(
    std::string_view name, const RuntimeValue& value);
RuntimeTemporalOperationResult runtimeTemporalFormat(
    const RuntimeValue& value, bool asString);

RuntimeTemporalOperationResult runtimeApplyTemporalUnary(
    std::string_view operation, const RuntimeValue& value);
RuntimeTemporalOperationResult runtimeApplyTemporalBinary(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right);

RuntimeTemporalOperationResult runtimeTemporalMemberValue(
    const RuntimeValue& value, std::string_view member);

bool runtimeTemporalValuesEqual(const RuntimeValue& left,
                                const RuntimeValue& right,
                                bool equalNaNs = false);

} // namespace mparser
