#pragma once

#include "mparser/runtime_value.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mparser {

bool isRuntimeCharacterArray(const RuntimeValue &value);
bool isRuntimeStringArray(const RuntimeValue &value);
bool isRuntimeTextValue(const RuntimeValue &value);
bool isRuntimeCharacterVector(const RuntimeValue &value);
bool isRuntimeStringScalar(const RuntimeValue &value);

std::u16string runtimeUtf8ToUtf16(std::string_view text);
std::string runtimeUtf16ToUtf8(std::u16string_view text);

RuntimeValue makeRuntimeCharacterArray(std::vector<size_t> dimensions,
                                       std::u16string elements);
RuntimeValue makeRuntimeCharacterVector(std::u16string text);
RuntimeValue makeRuntimeCharacterVectorUtf8(std::string_view text);
RuntimeValue makeRuntimeStringArray(std::vector<size_t> dimensions,
                                    std::vector<RuntimeStringElement> elements);
RuntimeValue makeRuntimeStringScalar(std::u16string text);
RuntimeValue makeRuntimeStringScalarUtf8(std::string_view text);

std::optional<char16_t> runtimeCharacterElement(const RuntimeValue &value,
                                                size_t logicalIndex);
const RuntimeStringElement *runtimeStringElement(const RuntimeValue &value,
                                                 size_t logicalIndex);
std::optional<std::u16string>
runtimeTextScalarCodeUnits(const RuntimeValue &value);
std::optional<std::string> runtimeTextScalarUtf8(const RuntimeValue &value);
bool runtimeTextPayloadEqual(const RuntimeValue &left,
                             const RuntimeValue &right);

struct RuntimeTextOperationResult {
  bool succeeded = false;
  RuntimeValue value;
  std::string error;
};

struct RuntimeTextMutationResult {
  bool succeeded = false;
  std::string error;
};

RuntimeTextOperationResult
runtimeIndexText(const RuntimeValue &target,
                 const std::vector<RuntimeValue> &subscripts);
RuntimeTextOperationResult
runtimeIndexStringContents(const RuntimeValue &target,
                           const std::vector<RuntimeValue> &subscripts);
RuntimeTextMutationResult
runtimeAssignTextIndexed(RuntimeValue &target,
                         const std::vector<RuntimeValue> &subscripts,
                         const RuntimeValue &value);
RuntimeTextMutationResult
runtimeDeleteTextIndexed(RuntimeValue &target,
                         const std::vector<RuntimeValue> &subscripts,
                         const std::vector<bool> &colonSubscripts);
RuntimeTextMutationResult
runtimeAssignStringContents(RuntimeValue &target,
                            const std::vector<RuntimeValue> &subscripts,
                            const RuntimeValue &value);

RuntimeTextOperationResult
runtimeConcatenateText(size_t dimension,
                       const std::vector<RuntimeValue> &values);
RuntimeTextOperationResult runtimeCompareText(std::string_view operation,
                                              const RuntimeValue &left,
                                              const RuntimeValue &right,
                                              bool ignoreCase = false);
RuntimeTextOperationResult runtimeAppendText(const RuntimeValue &left,
                                             const RuntimeValue &right);
RuntimeTextOperationResult runtimeConvertToCharacter(const RuntimeValue &value);
RuntimeTextOperationResult runtimeConvertToString(const RuntimeValue &value);
RuntimeTextOperationResult runtimeStringLengths(const RuntimeValue &value);
RuntimeTextOperationResult runtimeCharacterCodes(const RuntimeValue &value);
RuntimeTextOperationResult runtimeTextMissingMask(const RuntimeValue &value);
RuntimeTextOperationResult runtimeCellstr(const RuntimeValue &value);

} // namespace mparser
