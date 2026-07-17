#pragma once

#include "mparser/interpreter.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class RuntimeMetadataKind {
    MetaData,
    Class,
    Property,
    Method,
    Event,
    EnumerationMember,
    Namespace,
    Function,
    CallSignature,
    Argument,
    ArgumentIdentifier,
    ArgumentValidation,
    ArgumentValidator,
    DefaultArgumentValue,
    ArrayDimension,
    FixedDimension,
    UnrestrictedDimension,
};

std::string_view runtimeMetadataClassName(RuntimeMetadataKind kind);

std::string canonicalRuntimeMetadataClassName(std::string_view name);

std::optional<RuntimeMetadataKind>
runtimeMetadataKind(const RuntimeValue& value);

bool isRuntimeMetadataObject(const RuntimeValue& value);

bool isRuntimeMetadataScalar(const RuntimeValue& value);

bool isRuntimeMetadataArray(const RuntimeValue& value);

RuntimeValue makeRuntimeMetadataObject(RuntimeMetadataKind kind,
                                       std::string identity);

RuntimeValue makeRuntimeMetadataArray(
    RuntimeMetadataKind kind, std::vector<RuntimeValue> elements,
    std::vector<size_t> dimensions = {});

bool runtimeMetadataIsa(const RuntimeValue& value,
                        std::string_view className);

std::string runtimeValueClassName(const RuntimeValue& value);

} // namespace mparser
