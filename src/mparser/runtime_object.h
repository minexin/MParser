#pragma once

#include "mparser/runtime_value.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

struct RuntimeObjectOperationResult {
    bool succeeded = false;
    RuntimeValue value;
    std::string error;
};

struct RuntimeObjectClassResolutionResult {
    bool succeeded = false;
    std::string className;
    std::string error;
};

using RuntimeObjectCommonClassResolver = std::function<
    RuntimeObjectClassResolutionResult(
        const std::vector<std::string>&, std::string_view)>;
using RuntimeObjectDefaultFactory = std::function<
    RuntimeObjectOperationResult(std::string_view)>;
using RuntimeObjectElementEquality = std::function<
    bool(const RuntimeValue&, const RuntimeValue&)>;

struct RuntimeObjectArrayPolicy {
    RuntimeObjectCommonClassResolver resolveCommonClass;
    RuntimeObjectDefaultFactory constructDefault;
};

bool isRuntimeClassObject(const RuntimeValue& value);

bool isRuntimeScalarObject(const RuntimeValue& value);

size_t runtimeObjectElementCount(const RuntimeValue& value);

const RuntimeValue* runtimeObjectElement(
    const RuntimeValue& value, size_t storageOffset);

RuntimeValue* runtimeObjectElement(
    RuntimeValue& value, size_t storageOffset);

const RuntimeValue* runtimeObjectLogicalElement(
    const RuntimeValue& value, size_t logicalIndex);

RuntimeValue* runtimeObjectLogicalElement(
    RuntimeValue& value, size_t logicalIndex);

const std::map<std::string, RuntimeValue>* runtimeObjectFields(
    const RuntimeValue& value);

std::map<std::string, RuntimeValue>* runtimeObjectFields(
    RuntimeValue& value);

RuntimeValue makeRuntimeObjectScalar(
    std::string className,
    std::map<std::string, RuntimeValue> fields = {},
    bool handleObject = false);

RuntimeObjectOperationResult runtimeMakeObjectArrayFromLogicalOrder(
    std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions,
    std::string fallbackClassName,
    bool fallbackHandleObject,
    const RuntimeObjectArrayPolicy& policy = {},
    std::string preferredClassName = {});

RuntimeObjectOperationResult runtimeMakeObjectArrayFromStorageOrder(
    std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions,
    std::string fallbackClassName,
    bool fallbackHandleObject,
    const RuntimeObjectArrayPolicy& policy = {},
    std::string preferredClassName = {});

RuntimeObjectOperationResult runtimeIndexObject(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeObjectArrayPolicy& policy = {});

RuntimeObjectOperationResult runtimeAssignObjectIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value,
    const RuntimeObjectArrayPolicy& policy = {});

RuntimeObjectOperationResult runtimeEnsureObjectIndexedCapacity(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeObjectArrayPolicy& policy = {});

RuntimeObjectOperationResult runtimeDeleteObjectIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts,
    const RuntimeObjectArrayPolicy& policy = {});

RuntimeObjectOperationResult runtimeConcatenateObject(
    size_t dimension,
    const std::vector<RuntimeValue>& values,
    const RuntimeObjectArrayPolicy& policy = {});

bool runtimeObjectArraysEqual(
    const RuntimeValue& left,
    const RuntimeValue& right,
    const RuntimeObjectElementEquality& elementEquality);

RuntimeObjectOperationResult runtimeCompareObjectArrays(
    const RuntimeValue& left,
    const RuntimeValue& right,
    bool negate,
    const RuntimeObjectElementEquality& elementEquality);

} // namespace mparser
