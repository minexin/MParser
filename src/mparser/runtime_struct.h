#pragma once

#include "mparser/interpreter.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeStructOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

struct RuntimeStructFieldNameResult {
    bool succeeded = false;
    std::string name;
    std::string error;
};

bool isRuntimeStructFieldName(std::string_view name);

RuntimeStructFieldNameResult
runtimeStructFieldName(const RuntimeValue& value);

std::vector<std::string>
runtimeStructFieldOrder(const RuntimeValue& value);

bool runtimeSetStructField(RuntimeValue& structure, std::string name,
                           RuntimeValue value);

RuntimeStructOperationResult runtimeConstructScalarStruct(
    const std::vector<RuntimeValue>& arguments);

RuntimeStructOperationResult
runtimeStructFieldNames(const RuntimeValue& structure);

RuntimeStructOperationResult runtimeStructIsField(
    const RuntimeValue& value, const RuntimeValue& names);

RuntimeStructOperationResult runtimeRemoveStructFields(
    const RuntimeValue& structure, const RuntimeValue& names);

} // namespace mparser
