#pragma once

#include "mparser/runtime/core/value/runtime_value.h"

#include <cstddef>
#include <map>
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

using RuntimeStructElement = std::map<std::string, RuntimeValue>;

bool isRuntimeStructFieldName(std::string_view name);

RuntimeStructFieldNameResult
runtimeStructFieldName(const RuntimeValue& value);

std::vector<std::string>
runtimeStructFieldOrder(const RuntimeValue& value);

RuntimeValue makeRuntimeStructArrayValue(
    std::vector<std::string> fieldOrder,
    std::vector<RuntimeStructElement> elements,
    std::vector<size_t> dimensions);

bool isRuntimeScalarStruct(const RuntimeValue& value);

size_t runtimeStructElementCount(const RuntimeValue& value);

const RuntimeStructElement*
runtimeStructElement(const RuntimeValue& value, size_t storageOffset);

const RuntimeValue* runtimeStructField(
    const RuntimeValue& value, std::string_view name,
    size_t storageOffset = 0);

bool runtimeSetStructField(RuntimeValue& structure, std::string name,
                           RuntimeValue value);

RuntimeStructOperationResult runtimeConstructScalarStruct(
    const std::vector<RuntimeValue>& arguments);

RuntimeStructOperationResult runtimeStructFieldValues(
    const RuntimeValue& structure, std::string_view name);

RuntimeStructOperationResult runtimeIndexStruct(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts);
RuntimeStructOperationResult runtimeIndexStruct(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts,
    bool linearColon);

RuntimeStructOperationResult runtimeEnsureStructIndexedCapacity(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts);

RuntimeStructOperationResult runtimeAlignStructSchemaForCopyback(
    const RuntimeValue& structure,
    const RuntimeValue& nestedValue);

RuntimeStructOperationResult runtimeAssignStructIndexed(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value);

RuntimeStructOperationResult runtimeDeleteStructIndexed(
    const RuntimeValue& structure,
    const std::vector<RuntimeValue>& subscripts);

RuntimeStructOperationResult
runtimeStructFieldNames(const RuntimeValue& structure);

RuntimeStructOperationResult runtimeStructIsField(
    const RuntimeValue& value, const RuntimeValue& names);

RuntimeStructOperationResult runtimeRemoveStructFields(
    const RuntimeValue& structure, const RuntimeValue& names);

} // namespace mparser
