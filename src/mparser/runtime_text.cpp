#include "mparser/runtime_text.h"

#include "mparser/runtime_index.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_value_ops.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

namespace mparser {
namespace {

constexpr char32_t kReplacementCharacter = 0xfffd;

void appendUtf16(std::u16string &output, char32_t codePoint) {
  if (codePoint <= 0xffff) {
    if (codePoint >= 0xd800 && codePoint <= 0xdfff) {
      codePoint = kReplacementCharacter;
    }
    output.push_back(static_cast<char16_t>(codePoint));
    return;
  }
  if (codePoint > 0x10ffff) {
    codePoint = kReplacementCharacter;
  }
  codePoint -= 0x10000;
  output.push_back(static_cast<char16_t>(0xd800 + (codePoint >> 10)));
  output.push_back(static_cast<char16_t>(0xdc00 + (codePoint & 0x3ff)));
}

void appendUtf8(std::string &output, char32_t codePoint) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else if (codePoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  }
}

bool isContinuation(unsigned char value) { return (value & 0xc0) == 0x80; }

} // namespace

bool isRuntimeCharacterArray(const RuntimeValue &value) {
  return value.kind == RuntimeValueKind::CharacterArray;
}

bool isRuntimeStringArray(const RuntimeValue &value) {
  return value.kind == RuntimeValueKind::StringArray;
}

bool isRuntimeTextValue(const RuntimeValue &value) {
  return isRuntimeCharacterArray(value) || isRuntimeStringArray(value);
}

bool isRuntimeCharacterVector(const RuntimeValue &value) {
  if (!isRuntimeCharacterArray(value)) {
    return false;
  }
  const auto dimensions = runtimeDimensions(value);
  return dimensions.size() == 2 &&
         (dimensions[0] == 1 || (dimensions[0] == 0 && dimensions[1] == 0));
}

bool isRuntimeStringScalar(const RuntimeValue &value) {
  return isRuntimeStringArray(value) && runtimeShapeElementCount(value) == 1 &&
         value.stringElements.size() == 1;
}

std::u16string runtimeUtf8ToUtf16(std::string_view text) {
  std::u16string output;
  output.reserve(text.size());
  size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    char32_t codePoint = 0;
    size_t length = 0;
    char32_t minimum = 0;
    if (first <= 0x7f) {
      codePoint = first;
      length = 1;
    } else if ((first & 0xe0) == 0xc0) {
      codePoint = first & 0x1f;
      length = 2;
      minimum = 0x80;
    } else if ((first & 0xf0) == 0xe0) {
      codePoint = first & 0x0f;
      length = 3;
      minimum = 0x800;
    } else if ((first & 0xf8) == 0xf0) {
      codePoint = first & 0x07;
      length = 4;
      minimum = 0x10000;
    } else {
      appendUtf16(output, kReplacementCharacter);
      ++index;
      continue;
    }

    if (index + length > text.size()) {
      appendUtf16(output, kReplacementCharacter);
      ++index;
      continue;
    }
    bool valid = true;
    for (size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if (!isContinuation(next)) {
        valid = false;
        break;
      }
      codePoint = (codePoint << 6) | (next & 0x3f);
    }
    if (!valid || codePoint < minimum || codePoint > 0x10ffff ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
      appendUtf16(output, kReplacementCharacter);
      ++index;
      continue;
    }
    appendUtf16(output, codePoint);
    index += length;
  }
  return output;
}

std::string runtimeUtf16ToUtf8(std::u16string_view text) {
  std::string output;
  output.reserve(text.size());
  for (size_t index = 0; index < text.size(); ++index) {
    char32_t codePoint = text[index];
    if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
      if (index + 1 < text.size()) {
        const char32_t low = text[index + 1];
        if (low >= 0xdc00 && low <= 0xdfff) {
          codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
          ++index;
        } else {
          codePoint = kReplacementCharacter;
        }
      } else {
        codePoint = kReplacementCharacter;
      }
    } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
      codePoint = kReplacementCharacter;
    }
    appendUtf8(output, codePoint);
  }
  return output;
}

RuntimeValue makeRuntimeCharacterArray(std::vector<size_t> dimensions,
                                       std::u16string elements) {
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count || *count != elements.size()) {
    return RuntimeValue{};
  }
  RuntimeValue result;
  result.kind = RuntimeValueKind::CharacterArray;
  result.characterElements = std::move(elements);
  setRuntimeDimensions(result, std::move(dimensions));
  return result;
}

RuntimeValue makeRuntimeCharacterVector(std::u16string text) {
  const size_t size = text.size();
  return makeRuntimeCharacterArray(size == 0 ? std::vector<size_t>{0, 0}
                                             : std::vector<size_t>{1, size},
                                   std::move(text));
}

RuntimeValue makeRuntimeCharacterVectorUtf8(std::string_view text) {
  return makeRuntimeCharacterVector(runtimeUtf8ToUtf16(text));
}

RuntimeValue
makeRuntimeStringArray(std::vector<size_t> dimensions,
                       std::vector<RuntimeStringElement> elements) {
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count || *count != elements.size()) {
    return RuntimeValue{};
  }
  RuntimeValue result;
  result.kind = RuntimeValueKind::StringArray;
  result.stringElements = std::move(elements);
  setRuntimeDimensions(result, std::move(dimensions));
  return result;
}

RuntimeValue makeRuntimeStringScalar(std::u16string text) {
  std::vector<RuntimeStringElement> elements;
  elements.push_back(RuntimeStringElement{std::move(text), false});
  return makeRuntimeStringArray({1, 1}, std::move(elements));
}

RuntimeValue makeRuntimeStringScalarUtf8(std::string_view text) {
  return makeRuntimeStringScalar(runtimeUtf8ToUtf16(text));
}

std::optional<char16_t> runtimeCharacterElement(const RuntimeValue &value,
                                                size_t logicalIndex) {
  if (!isRuntimeCharacterArray(value)) {
    return std::nullopt;
  }
  const auto storageOffset =
      runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
  if (!storageOffset || *storageOffset >= value.characterElements.size()) {
    return std::nullopt;
  }
  return value.characterElements[*storageOffset];
}

const RuntimeStringElement *runtimeStringElement(const RuntimeValue &value,
                                                 size_t logicalIndex) {
  if (!isRuntimeStringArray(value)) {
    return nullptr;
  }
  const auto storageOffset =
      runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
  if (!storageOffset || *storageOffset >= value.stringElements.size()) {
    return nullptr;
  }
  return &value.stringElements[*storageOffset];
}

std::optional<std::u16string>
runtimeTextScalarCodeUnits(const RuntimeValue &value) {
  if (isRuntimeCharacterVector(value)) {
    return value.characterElements;
  }
  if (!isRuntimeStringScalar(value) || value.stringElements.front().missing) {
    return std::nullopt;
  }
  return value.stringElements.front().value;
}

std::optional<std::string> runtimeTextScalarUtf8(const RuntimeValue &value) {
  const auto text = runtimeTextScalarCodeUnits(value);
  return text ? std::optional<std::string>(runtimeUtf16ToUtf8(*text))
              : std::nullopt;
}

bool runtimeTextPayloadEqual(const RuntimeValue &left,
                             const RuntimeValue &right) {
  if (left.kind != right.kind ||
      runtimeDimensions(left) != runtimeDimensions(right)) {
    return false;
  }
  if (isRuntimeCharacterArray(left)) {
    return left.characterElements == right.characterElements;
  }
  if (isRuntimeStringArray(left)) {
    return left.stringElements == right.stringElements;
  }
  return false;
}

namespace {

RuntimeTextOperationResult textFailure(std::string error) {
  return RuntimeTextOperationResult{false, {}, std::move(error)};
}

RuntimeTextOperationResult textSuccess(RuntimeValue value) {
  return RuntimeTextOperationResult{true, std::move(value), {}};
}

RuntimeTextMutationResult mutationFailure(std::string error) {
  return RuntimeTextMutationResult{false, std::move(error)};
}

RuntimeTextMutationResult mutationSuccess() {
  return RuntimeTextMutationResult{true, {}};
}

RuntimeValue logicalArray(std::vector<size_t> dimensions,
                          std::vector<double> elements) {
  auto result = runtimeNumericValueFromLogicalOrder(
      std::move(dimensions), std::move(elements), RuntimeNumericClass::Logical);
  return result.value_or(RuntimeValue{});
}

RuntimeValue numericArray(std::vector<size_t> dimensions,
                          std::vector<double> elements) {
  auto result = runtimeNumericValueFromLogicalOrder(
      std::move(dimensions), std::move(elements), RuntimeNumericClass::Double);
  return result.value_or(RuntimeValue{});
}

std::optional<RuntimeValue>
cellFromLogicalOrder(std::vector<size_t> dimensions,
                     std::vector<RuntimeValue> logicalCells) {
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count || *count != logicalCells.size()) {
    return std::nullopt;
  }
  RuntimeValue result;
  result.kind = RuntimeValueKind::Cell;
  result.cells.resize(*count);
  for (size_t index = 0; index < *count; ++index) {
    const auto coordinates = runtimeColumnMajorCoordinates(index, dimensions);
    const auto offset =
        coordinates ? runtimeRowMajorStorageOffset(*coordinates, dimensions)
                    : std::nullopt;
    if (!offset) {
      return std::nullopt;
    }
    result.cells[*offset] = std::move(logicalCells[index]);
  }
  setRuntimeDimensions(result, dimensions);
  return result;
}

std::optional<RuntimeValue>
characterFromLogicalOrder(std::vector<size_t> dimensions,
                          const std::vector<char16_t> &logical) {
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count || *count != logical.size()) {
    return std::nullopt;
  }
  std::u16string storage(*count, u'\0');
  for (size_t index = 0; index < *count; ++index) {
    const auto coordinates = runtimeColumnMajorCoordinates(index, dimensions);
    const auto offset =
        coordinates ? runtimeRowMajorStorageOffset(*coordinates, dimensions)
                    : std::nullopt;
    if (!offset) {
      return std::nullopt;
    }
    storage[*offset] = logical[index];
  }
  return makeRuntimeCharacterArray(std::move(dimensions), std::move(storage));
}

std::optional<RuntimeValue>
stringFromLogicalOrder(std::vector<size_t> dimensions,
                       const std::vector<RuntimeStringElement> &logical) {
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count || *count != logical.size()) {
    return std::nullopt;
  }
  std::vector<RuntimeStringElement> storage(*count);
  for (size_t index = 0; index < *count; ++index) {
    const auto coordinates = runtimeColumnMajorCoordinates(index, dimensions);
    const auto offset =
        coordinates ? runtimeRowMajorStorageOffset(*coordinates, dimensions)
                    : std::nullopt;
    if (!offset) {
      return std::nullopt;
    }
    storage[*offset] = logical[index];
  }
  return makeRuntimeStringArray(std::move(dimensions), std::move(storage));
}

std::optional<RuntimeValue> normalizedStringValue(const RuntimeValue &value) {
  if (isRuntimeStringArray(value)) {
    return value;
  }
  if (!isRuntimeCharacterArray(value)) {
    return std::nullopt;
  }
  const auto dimensions = runtimeDimensions(value);
  if (dimensions.size() != 2) {
    return std::nullopt;
  }
  if (dimensions[0] <= 1) {
    return makeRuntimeStringScalar(value.characterElements);
  }
  std::vector<RuntimeStringElement> elements;
  elements.reserve(dimensions[0]);
  for (size_t row = 0; row < dimensions[0]; ++row) {
    const size_t offset = row * dimensions[1];
    elements.push_back(RuntimeStringElement{
        std::u16string(value.characterElements.data() + offset, dimensions[1]),
        false});
  }
  return makeRuntimeStringArray({dimensions[0], 1}, std::move(elements));
}

std::optional<RuntimeValue>
normalizedComparisonStringValue(const RuntimeValue &value) {
  if (isRuntimeTextValue(value)) {
    return normalizedStringValue(value);
  }
  if (value.kind != RuntimeValueKind::Cell ||
      runtimeShapeElementCount(value) != value.cells.size()) {
    return std::nullopt;
  }

  std::vector<RuntimeStringElement> elements;
  elements.reserve(value.cells.size());
  for (const auto &cell : value.cells) {
    const auto text = runtimeTextScalarCodeUnits(cell);
    if (!text) {
      return std::nullopt;
    }
    const bool missing =
        isRuntimeStringArray(cell) && cell.stringElements.size() == 1 &&
        cell.stringElements.front().missing;
    elements.push_back(RuntimeStringElement{*text, missing});
  }
  return makeRuntimeStringArray(runtimeDimensions(value),
                                std::move(elements));
}

std::vector<size_t>
nonSingletonDimensions(const std::vector<size_t> &dimensions) {
  std::vector<size_t> result;
  for (const size_t dimension : dimensions) {
    if (dimension != 1) {
      result.push_back(dimension);
    }
  }
  return result;
}

bool growTextTarget(RuntimeValue &target,
                    const std::vector<size_t> &oldViewDimensions,
                    std::vector<size_t> newDimensions) {
  newDimensions = normalizeRuntimeDimensions(std::move(newDimensions));
  const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
  const auto oldViewCount = checkedRuntimeDimensionProduct(oldViewDimensions);
  const size_t oldCount = runtimeShapeElementCount(target);
  if (!newCount || !oldViewCount || *oldViewCount != oldCount) {
    return false;
  }

  if (isRuntimeCharacterArray(target)) {
    std::u16string grown(*newCount, u' ');
    for (size_t oldOffset = 0; oldOffset < oldCount; ++oldOffset) {
      auto coordinates =
          runtimeRowMajorCoordinates(oldOffset, oldViewDimensions);
      coordinates.resize(newDimensions.size(), 0);
      const auto newOffset =
          runtimeRowMajorStorageOffset(coordinates, newDimensions);
      if (!newOffset || oldOffset >= target.characterElements.size()) {
        return false;
      }
      grown[*newOffset] = target.characterElements[oldOffset];
    }
    target.characterElements = std::move(grown);
  } else if (isRuntimeStringArray(target)) {
    std::vector<RuntimeStringElement> grown(*newCount,
                                            RuntimeStringElement{{}, true});
    for (size_t oldOffset = 0; oldOffset < oldCount; ++oldOffset) {
      auto coordinates =
          runtimeRowMajorCoordinates(oldOffset, oldViewDimensions);
      coordinates.resize(newDimensions.size(), 0);
      const auto newOffset =
          runtimeRowMajorStorageOffset(coordinates, newDimensions);
      if (!newOffset || oldOffset >= target.stringElements.size()) {
        return false;
      }
      grown[*newOffset] = target.stringElements[oldOffset];
    }
    target.stringElements = std::move(grown);
  } else {
    return false;
  }
  setRuntimeDimensions(target, std::move(newDimensions));
  return true;
}

RuntimeTextMutationResult ensureTextCapacity(RuntimeValue &target,
                                             const RuntimeIndexSelectionsResult
                                                 &selections) {
  const auto oldDimensions = runtimeDimensions(target);
  if (selections.indices.size() == 1) {
    const auto extent =
        runtimeIndexSelectionRequiredExtent(selections.indices.front());
    if (!extent || *extent <= runtimeShapeElementCount(target)) {
      return mutationSuccess();
    }
    std::vector<size_t> newDimensions;
    if (oldDimensions.size() == 2 && oldDimensions[0] == 1) {
      newDimensions = {1, *extent};
    } else if (oldDimensions.size() == 2 && oldDimensions[1] == 1) {
      newDimensions = {*extent, 1};
    } else {
      newDimensions = oldDimensions;
      std::vector<size_t> leading(newDimensions.begin(),
                                  newDimensions.end() - 1);
      const auto leadingCount = checkedRuntimeDimensionProduct(leading);
      if (!leadingCount || *leadingCount == 0) {
        newDimensions = {1, *extent};
      } else {
        newDimensions.back() =
            *extent / *leadingCount + (*extent % *leadingCount == 0 ? 0 : 1);
      }
    }
    return growTextTarget(target, oldDimensions, std::move(newDimensions))
               ? mutationSuccess()
               : mutationFailure(
                     "text indexed assignment dimensions are too large");
  }

  std::vector<std::optional<size_t>> extents;
  bool growthRequired = false;
  for (size_t index = 0; index < selections.indices.size(); ++index) {
    extents.push_back(
        runtimeIndexSelectionRequiredExtent(selections.indices[index]));
    growthRequired = growthRequired ||
                     (extents.back() &&
                      *extents.back() > selections.effectiveDimensions[index]);
  }
  if (!growthRequired) {
    return mutationSuccess();
  }

  std::vector<size_t> oldViewDimensions = oldDimensions;
  std::vector<size_t> newDimensions = oldDimensions;
  const bool foldsTrailingDimensions =
      selections.indices.size() < oldDimensions.size();
  const size_t finalSubscript = selections.indices.size() - 1;
  const bool growsFoldedDimension =
      foldsTrailingDimensions && extents[finalSubscript] &&
      *extents[finalSubscript] > selections.effectiveDimensions[finalSubscript];
  if (growsFoldedDimension) {
    oldViewDimensions = selections.effectiveDimensions;
    newDimensions = selections.effectiveDimensions;
  } else if (selections.indices.size() > newDimensions.size()) {
    oldViewDimensions.resize(selections.indices.size(), 1);
    newDimensions.resize(selections.indices.size(), 1);
  }
  for (size_t index = 0; index < extents.size(); ++index) {
    if (foldsTrailingDimensions && !growsFoldedDimension &&
        index == finalSubscript) {
      continue;
    }
    if (extents[index]) {
      newDimensions[index] = std::max(newDimensions[index], *extents[index]);
    }
  }
  return growTextTarget(target, oldViewDimensions, std::move(newDimensions))
             ? mutationSuccess()
             : mutationFailure(
                   "text indexed assignment dimensions are too large");
}

std::optional<RuntimeValue> assignmentTextValue(const RuntimeValue &target,
                                                const RuntimeValue &value) {
  if (isRuntimeCharacterArray(target)) {
    if (isRuntimeCharacterArray(value)) {
      return value;
    }
    const auto text = runtimeTextScalarCodeUnits(value);
    return text ? std::optional<RuntimeValue>(makeRuntimeCharacterVector(*text))
                : std::nullopt;
  }
  if (isRuntimeStringArray(target)) {
    if (value.kind == RuntimeValueKind::MissingArray) {
      const size_t count = runtimeShapeElementCount(value);
      std::vector<RuntimeStringElement> elements(count);
      for (auto &element : elements) {
        element.missing = true;
      }
      return makeRuntimeStringArray(runtimeDimensions(value),
                                    std::move(elements));
    }
    if (isRuntimeStringArray(value)) {
      return value;
    }
    const auto text = runtimeTextScalarCodeUnits(value);
    return text ? std::optional<RuntimeValue>(makeRuntimeStringScalar(*text))
                : std::nullopt;
  }
  return std::nullopt;
}

bool assignTextElement(RuntimeValue &target, size_t targetLogicalIndex,
                       const RuntimeValue &value, size_t valueLogicalIndex) {
  const auto offset =
      runtimeColumnMajorLinearToStorageOffset(target, targetLogicalIndex);
  if (!offset) {
    return false;
  }
  if (isRuntimeCharacterArray(target)) {
    const auto element = runtimeCharacterElement(value, valueLogicalIndex);
    if (!element || *offset >= target.characterElements.size()) {
      return false;
    }
    target.characterElements[*offset] = *element;
    return true;
  }
  const auto *element = runtimeStringElement(value, valueLogicalIndex);
  if (!element || *offset >= target.stringElements.size()) {
    return false;
  }
  target.stringElements[*offset] = *element;
  return true;
}

std::vector<size_t> uniqueIndices(std::vector<size_t> indices) {
  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
  return indices;
}

std::optional<size_t>
expandedTextStorageOffset(const RuntimeValue &source,
                          const std::vector<size_t> &outputDimensions,
                          size_t outputLogicalIndex) {
  const auto coordinates =
      runtimeColumnMajorCoordinates(outputLogicalIndex, outputDimensions);
  return coordinates ? runtimeImplicitExpansionStorageOffset(
                           *coordinates, runtimeDimensions(source))
                     : std::nullopt;
}

char16_t foldAscii(char16_t value) {
  return value >= u'A' && value <= u'Z'
             ? static_cast<char16_t>(value - u'A' + u'a')
             : value;
}

bool stringElementEqual(const RuntimeStringElement &left,
                        const RuntimeStringElement &right, bool ignoreCase) {
  if (left.missing || right.missing) {
    return left.missing && right.missing;
  }
  if (!ignoreCase) {
    return left.value == right.value;
  }
  return left.value.size() == right.value.size() &&
         std::equal(left.value.begin(), left.value.end(), right.value.begin(),
                    [](char16_t a, char16_t b) {
                      return foldAscii(a) == foldAscii(b);
                    });
}

std::string_view trimAsciiWhitespace(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

std::optional<std::string>
normalizeThousandsSeparators(std::string_view text) {
  const size_t exponent = text.find_first_of("eE");
  const size_t decimal = text.find('.');
  const size_t integerEnd = std::min(
      decimal == std::string_view::npos ? text.size() : decimal,
      exponent == std::string_view::npos ? text.size() : exponent);
  const size_t integerBegin =
      !text.empty() && (text.front() == '+' || text.front() == '-') ? 1 : 0;
  if (integerBegin > integerEnd) {
    return std::nullopt;
  }

  const std::string_view integer =
      text.substr(integerBegin, integerEnd - integerBegin);
  if (text.find(',') == std::string_view::npos) {
    return std::string(text);
  }
  if (integer.find(',') == std::string_view::npos ||
      text.substr(integerEnd).find(',') != std::string_view::npos) {
    return std::nullopt;
  }

  size_t groupStart = 0;
  size_t groupIndex = 0;
  while (groupStart <= integer.size()) {
    const size_t comma = integer.find(',', groupStart);
    const size_t groupEnd = comma == std::string_view::npos
                                ? integer.size()
                                : comma;
    const size_t groupLength = groupEnd - groupStart;
    if (groupLength == 0 ||
        (groupIndex == 0 ? groupLength > 3 : groupLength != 3) ||
        !std::all_of(integer.begin() +
                         static_cast<std::ptrdiff_t>(groupStart),
                     integer.begin() +
                         static_cast<std::ptrdiff_t>(groupEnd),
                     [](char value) {
                       return value >= '0' && value <= '9';
                     })) {
      return std::nullopt;
    }
    ++groupIndex;
    if (comma == std::string_view::npos) {
      break;
    }
    groupStart = comma + 1;
  }

  std::string normalized;
  normalized.reserve(text.size());
  for (const char value : text) {
    if (value != ',') {
      normalized.push_back(value);
    }
  }
  return normalized;
}

std::optional<double> parseTextReal(std::string_view text,
                                    bool allowHexadecimal) {
  text = trimAsciiWhitespace(text);
  const auto normalized = normalizeThousandsSeparators(text);
  if (!normalized || normalized->empty()) {
    return std::nullopt;
  }
  size_t prefix = ((*normalized)[0] == '+' || (*normalized)[0] == '-')
                      ? 1
                      : 0;
  if (prefix + 2 <= normalized->size() && (*normalized)[prefix] == '0') {
    const char marker = (*normalized)[prefix + 1];
    if (marker == 'b' || marker == 'B' ||
        (!allowHexadecimal && (marker == 'x' || marker == 'X'))) {
      return std::nullopt;
    }
  }
  char *end = nullptr;
  const double value = std::strtod(normalized->c_str(), &end);
  if (end == normalized->c_str() ||
      end != normalized->c_str() + normalized->size()) {
    return std::nullopt;
  }
  return value;
}

struct ParsedTextTerm {
  double value = 0.0;
  bool imaginary = false;
};

bool isImaginaryUnit(char value) {
  return value == 'i' || value == 'j';
}

std::optional<double> parseImaginaryCoefficient(std::string_view term,
                                                size_t unit,
                                                bool allowHexadecimal) {
  std::string_view prefix = trimAsciiWhitespace(term.substr(0, unit));
  std::string_view suffix = trimAsciiWhitespace(term.substr(unit + 1));
  if (suffix.empty()) {
    if (!prefix.empty() && prefix.back() == '*') {
      prefix = trimAsciiWhitespace(prefix.substr(0, prefix.size() - 1));
    }
    if (prefix.empty() || prefix == "+") {
      return 1.0;
    }
    if (prefix == "-") {
      return -1.0;
    }
    return parseTextReal(prefix, allowHexadecimal);
  }

  double sign = 1.0;
  if (prefix == "-") {
    sign = -1.0;
  } else if (!prefix.empty() && prefix != "+") {
    return std::nullopt;
  }
  if (suffix.front() != '*') {
    return std::nullopt;
  }
  suffix = trimAsciiWhitespace(suffix.substr(1));
  const auto coefficient = parseTextReal(suffix, allowHexadecimal);
  return coefficient ? std::optional<double>(sign * *coefficient)
                     : std::nullopt;
}

std::optional<ParsedTextTerm> parseTextTerm(std::string_view term,
                                           bool allowHexadecimal) {
  term = trimAsciiWhitespace(term);
  if (term.empty()) {
    return std::nullopt;
  }

  if (term.find('*') == std::string_view::npos) {
    const auto real = parseTextReal(term, allowHexadecimal);
    if (real) {
      return ParsedTextTerm{*real, false};
    }
  }

  std::optional<double> coefficient;
  for (size_t index = 0; index < term.size(); ++index) {
    if (!isImaginaryUnit(term[index])) {
      continue;
    }
    const auto candidate =
        parseImaginaryCoefficient(term, index, allowHexadecimal);
    if (!candidate) {
      continue;
    }
    if (coefficient) {
      return std::nullopt;
    }
    coefficient = *candidate;
  }
  return coefficient
             ? std::optional<ParsedTextTerm>(
                   ParsedTextTerm{*coefficient, true})
             : std::nullopt;
}

std::vector<size_t> textTermSeparators(std::string_view text) {
  std::vector<size_t> separators;
  for (size_t index = 1; index < text.size(); ++index) {
    if (text[index] != '+' && text[index] != '-') {
      continue;
    }
    size_t previous = index;
    while (previous > 0 &&
           std::isspace(static_cast<unsigned char>(text[previous - 1])) != 0) {
      --previous;
    }
    if (previous == 0) {
      continue;
    }
    const char previousValue = text[previous - 1];
    if (previousValue != 'e' && previousValue != 'E' &&
        previousValue != 'p' && previousValue != 'P' &&
        previousValue != '*') {
      separators.push_back(index);
    }
  }
  return separators;
}

std::optional<RuntimeNumericElementValue>
parseTextNumericElement(std::string_view input, bool allowBaseLiteral) {
  input = trimAsciiWhitespace(input);
  if (input.empty()) {
    return std::nullopt;
  }

  const auto separators = textTermSeparators(input);
  if (separators.size() > 1) {
    return std::nullopt;
  }
  const auto first = parseTextTerm(
      separators.empty() ? input : input.substr(0, separators.front()),
      allowBaseLiteral);
  if (!first) {
    return std::nullopt;
  }

  RuntimeNumericElementValue result;
  if (separators.empty()) {
    if (first->imaginary) {
      result.imaginary = first->value;
      result.complex = true;
    } else {
      result.real = first->value;
    }
    return result;
  }

  auto second = parseTextTerm(input.substr(separators.front() + 1),
                              allowBaseLiteral);
  if (!second || first->imaginary == second->imaginary) {
    return std::nullopt;
  }
  if (input[separators.front()] == '-') {
    second->value = -second->value;
  }
  const ParsedTextTerm &real = first->imaginary ? *second : *first;
  const ParsedTextTerm &imaginary = first->imaginary ? *first : *second;
  result.real = real.value;
  result.imaginary = imaginary.value;
  result.complex = true;
  return result;
}

RuntimeNumericElementValue invalidTextNumber() {
  RuntimeNumericElementValue result;
  result.real = std::numeric_limits<double>::quiet_NaN();
  return result;
}

RuntimeTextOperationResult textNumbersFromStrings(const RuntimeValue &value,
                                                  bool allowBaseLiteral) {
  std::vector<RuntimeNumericElementValue> elements;
  const size_t count = runtimeShapeElementCount(value);
  elements.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *element = runtimeStringElement(value, index);
    if (!element) {
      return textFailure("numeric text conversion could not map a string");
    }
    if (element->missing) {
      elements.push_back(invalidTextNumber());
      continue;
    }
    const auto parsed = parseTextNumericElement(
        runtimeUtf16ToUtf8(element->value), allowBaseLiteral);
    elements.push_back(parsed.value_or(invalidTextNumber()));
  }
  const auto result = runtimeNumericValueFromElements(
      runtimeDimensions(value), std::move(elements),
      RuntimeNumericClass::Double);
  return result ? textSuccess(*result)
                : textFailure("numeric text conversion result is invalid");
}

} // namespace

RuntimeTextOperationResult
runtimeIndexText(const RuntimeValue &target,
                 const std::vector<RuntimeValue> &subscripts) {
  return runtimeIndexText(target, subscripts, false);
}

RuntimeTextOperationResult
runtimeIndexText(const RuntimeValue &target,
                 const std::vector<RuntimeValue> &subscripts,
                 bool linearColon) {
  if (!isRuntimeTextValue(target)) {
    return textFailure("text indexing requires a text target");
  }
  if (subscripts.empty()) {
    return textFailure("text indexing requires subscripts");
  }
  const auto selections =
      runtimeResolveIndexSelections(target, subscripts, false,
                                    linearColon);
  if (!selections.succeeded) {
    return textFailure(selections.error);
  }
  const auto count =
      checkedRuntimeDimensionProduct(selections.resultDimensions);
  if (!count) {
    return textFailure("indexed text dimensions are too large");
  }
  if (isRuntimeCharacterArray(target)) {
    std::vector<char16_t> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
      const auto source =
          runtimeIndexSelectionSourceLogicalIndex(selections, index);
      const auto element =
          source ? runtimeCharacterElement(target, *source) : std::nullopt;
      if (!element) {
        return textFailure("text indexing could not map an element");
      }
      elements.push_back(*element);
    }
    const auto result =
        characterFromLogicalOrder(selections.resultDimensions, elements);
    return result ? textSuccess(*result)
                  : textFailure("indexed character shape is invalid");
  }

  std::vector<RuntimeStringElement> elements;
  elements.reserve(*count);
  for (size_t index = 0; index < *count; ++index) {
    const auto source =
        runtimeIndexSelectionSourceLogicalIndex(selections, index);
    const auto *element =
        source ? runtimeStringElement(target, *source) : nullptr;
    if (!element) {
      return textFailure("text indexing could not map an element");
    }
    elements.push_back(*element);
  }
  const auto result =
      stringFromLogicalOrder(selections.resultDimensions, elements);
  return result ? textSuccess(*result)
                : textFailure("indexed string shape is invalid");
}

RuntimeTextOperationResult
runtimeIndexStringContents(const RuntimeValue &target,
                           const std::vector<RuntimeValue> &subscripts) {
  if (!isRuntimeStringArray(target)) {
    return textFailure("brace text indexing requires a string array");
  }
  auto indexed = runtimeIndexText(target, subscripts);
  if (!indexed.succeeded) {
    return indexed;
  }
  std::vector<RuntimeValue> values;
  const size_t count = runtimeShapeElementCount(indexed.value);
  values.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *element = runtimeStringElement(indexed.value, index);
    if (!element) {
      return textFailure("string contents indexing could not map an element");
    }
    values.push_back(makeRuntimeCharacterVector(
        element->missing ? std::u16string{} : element->value));
  }
  return values.size() == 1
             ? textSuccess(std::move(values.front()))
             : textSuccess(makeRuntimeCommaSeparatedList(std::move(values)));
}

RuntimeTextMutationResult
runtimeAssignTextIndexed(RuntimeValue &target,
                         const std::vector<RuntimeValue> &subscripts,
                         const RuntimeValue &value) {
  if (!isRuntimeTextValue(target)) {
    return mutationFailure("text indexed assignment requires a text target");
  }
  const auto assigned = assignmentTextValue(target, value);
  if (!assigned) {
    return mutationFailure(
        "text indexed assignment requires a compatible text value");
  }
  if (subscripts.empty()) {
    return mutationFailure("text indexing requires subscripts");
  }
  const auto selections =
      runtimeResolveIndexSelections(target, subscripts, true);
  if (!selections.succeeded) {
    return mutationFailure(selections.error);
  }
  const auto selectionCount =
      checkedRuntimeDimensionProduct(selections.resultDimensions);
  if (!selectionCount) {
    return mutationFailure("text indexed assignment dimensions are too large");
  }
  const size_t valueCount = runtimeShapeElementCount(*assigned);
  const bool scalarExpansion = valueCount == 1;
  if (!scalarExpansion && valueCount != *selectionCount) {
    return mutationFailure(
        "text indexed assignment requires matching element counts");
  }
  if (!scalarExpansion && selections.indices.size() > 1 &&
      nonSingletonDimensions(selections.resultDimensions) !=
          nonSingletonDimensions(runtimeDimensions(*assigned))) {
    return mutationFailure("text indexed assignment dimensions do not match");
  }
  auto capacity = ensureTextCapacity(target, selections);
  if (!capacity.succeeded) {
    return capacity;
  }

  RuntimeIndexSelectionsResult grown = selections;
  grown.effectiveDimensions =
      runtimeEffectiveSubscriptDimensions(target, subscripts.size());
  for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
    const auto targetIndex =
        runtimeIndexSelectionSourceLogicalIndex(grown, ordinal);
    if (!targetIndex || !assignTextElement(target, *targetIndex, *assigned,
                                           scalarExpansion ? 0 : ordinal)) {
      return mutationFailure(
          "text indexed assignment could not map an element");
    }
  }
  return mutationSuccess();
}

RuntimeTextMutationResult
runtimeDeleteTextIndexed(RuntimeValue &target,
                         const std::vector<RuntimeValue> &subscripts,
                         const std::vector<bool> &colonSubscripts) {
  if (!isRuntimeTextValue(target)) {
    return mutationFailure("text deletion requires a text target");
  }
  if (colonSubscripts.size() != subscripts.size()) {
    return mutationFailure("text deletion subscript metadata is inconsistent");
  }
  if (subscripts.empty()) {
    return mutationFailure("text indexing requires subscripts");
  }
  const auto selections =
      runtimeResolveIndexSelections(target, subscripts, false);
  if (!selections.succeeded) {
    return mutationFailure(selections.error);
  }

  if (selections.indices.size() == 1) {
    const auto indices = uniqueIndices(selections.indices.front());
    if (indices.empty()) {
      return mutationSuccess();
    }
    const size_t oldCount = runtimeShapeElementCount(target);
    const auto oldDimensions = runtimeDimensions(target);
    const bool vectorShape = oldDimensions.size() == 2 &&
                             (oldDimensions[0] == 1 || oldDimensions[1] == 1);
    if (!vectorShape && indices.size() != oldCount) {
      return mutationFailure(
          "linear text deletion requires a vector or all elements");
    }
    std::vector<bool> removed(oldCount, false);
    for (const size_t index : indices) {
      if (index >= oldCount) {
        return mutationFailure("text index is out of bounds");
      }
      removed[index] = true;
    }
    std::vector<size_t> newDimensions;
    const size_t keptCount = oldCount - indices.size();
    if (keptCount == 0 && !vectorShape) {
      newDimensions = {0, 0};
    } else if (oldDimensions[0] == 1) {
      newDimensions = {1, keptCount};
    } else {
      newDimensions = {keptCount, 1};
    }
    if (isRuntimeCharacterArray(target)) {
      std::vector<char16_t> kept;
      kept.reserve(keptCount);
      for (size_t index = 0; index < oldCount; ++index) {
        if (!removed[index]) {
          const auto element = runtimeCharacterElement(target, index);
          if (!element) {
            return mutationFailure("text deletion could not read an element");
          }
          kept.push_back(*element);
        }
      }
      const auto result = characterFromLogicalOrder(newDimensions, kept);
      if (!result) {
        return mutationFailure("text deletion shape is invalid");
      }
      target = *result;
    } else {
      std::vector<RuntimeStringElement> kept;
      kept.reserve(keptCount);
      for (size_t index = 0; index < oldCount; ++index) {
        if (!removed[index]) {
          const auto *element = runtimeStringElement(target, index);
          if (!element) {
            return mutationFailure("text deletion could not read an element");
          }
          kept.push_back(*element);
        }
      }
      const auto result = stringFromLogicalOrder(newDimensions, kept);
      if (!result) {
        return mutationFailure("text deletion shape is invalid");
      }
      target = *result;
    }
    return mutationSuccess();
  }

  size_t deletionDimension = selections.indices.size();
  for (size_t index = 0; index < colonSubscripts.size(); ++index) {
    if (colonSubscripts[index]) {
      continue;
    }
    if (deletionDimension != selections.indices.size()) {
      return mutationFailure(
          "text deletion can have only one non-colon subscript");
    }
    deletionDimension = index;
  }
  if (deletionDimension == selections.indices.size()) {
    return mutationFailure("text deletion requires one non-colon subscript");
  }
  auto oldDimensions = runtimeDimensions(target);
  if (selections.indices.size() < oldDimensions.size()) {
    return mutationFailure(
        "N-dimensional text deletion requires one subscript per dimension");
  }
  oldDimensions.resize(selections.indices.size(), 1);
  const auto removedIndices =
      uniqueIndices(selections.indices[deletionDimension]);
  if (removedIndices.empty()) {
    return mutationSuccess();
  }
  std::vector<bool> removed(oldDimensions[deletionDimension], false);
  for (const size_t index : removedIndices) {
    if (index >= removed.size()) {
      return mutationFailure("text index is out of bounds");
    }
    removed[index] = true;
  }
  std::vector<size_t> removedBefore(removed.size() + 1, 0);
  for (size_t index = 0; index < removed.size(); ++index) {
    removedBefore[index + 1] = removedBefore[index] + (removed[index] ? 1 : 0);
  }
  auto newDimensions = oldDimensions;
  newDimensions[deletionDimension] -= removedIndices.size();
  const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
  if (!newCount) {
    return mutationFailure("text deletion dimensions are too large");
  }
  if (isRuntimeCharacterArray(target)) {
    std::u16string kept(*newCount, u'\0');
    for (size_t oldOffset = 0; oldOffset < target.characterElements.size();
         ++oldOffset) {
      auto coordinates = runtimeRowMajorCoordinates(oldOffset, oldDimensions);
      const size_t selected = coordinates[deletionDimension];
      if (removed[selected]) {
        continue;
      }
      coordinates[deletionDimension] -= removedBefore[selected];
      const auto newOffset =
          runtimeRowMajorStorageOffset(coordinates, newDimensions);
      if (!newOffset) {
        return mutationFailure("text deletion could not map an element");
      }
      kept[*newOffset] = target.characterElements[oldOffset];
    }
    target = makeRuntimeCharacterArray(newDimensions, std::move(kept));
  } else {
    std::vector<RuntimeStringElement> kept(*newCount);
    for (size_t oldOffset = 0; oldOffset < target.stringElements.size();
         ++oldOffset) {
      auto coordinates = runtimeRowMajorCoordinates(oldOffset, oldDimensions);
      const size_t selected = coordinates[deletionDimension];
      if (removed[selected]) {
        continue;
      }
      coordinates[deletionDimension] -= removedBefore[selected];
      const auto newOffset =
          runtimeRowMajorStorageOffset(coordinates, newDimensions);
      if (!newOffset) {
        return mutationFailure("text deletion could not map an element");
      }
      kept[*newOffset] = target.stringElements[oldOffset];
    }
    target = makeRuntimeStringArray(newDimensions, std::move(kept));
  }
  return mutationSuccess();
}

RuntimeTextMutationResult
runtimeAssignStringContents(RuntimeValue &target,
                            const std::vector<RuntimeValue> &subscripts,
                            const RuntimeValue &value) {
  if (!isRuntimeStringArray(target)) {
    return mutationFailure("brace text assignment requires a string array");
  }
  const auto text = runtimeTextScalarCodeUnits(value);
  if (!text) {
    return mutationFailure("string contents assignment requires a text scalar");
  }
  RuntimeValue assigned = makeRuntimeStringScalar(*text);
  return runtimeAssignTextIndexed(target, subscripts, assigned);
}

RuntimeTextOperationResult
runtimeConcatenateText(size_t dimension,
                       const std::vector<RuntimeValue> &values) {
  if (dimension == 0 || values.empty()) {
    return textFailure(
        "text concatenation requires a positive dimension and values");
  }
  const bool stringResult =
      std::any_of(values.begin(), values.end(), isRuntimeStringArray);
  std::vector<RuntimeValue> inputs;
  inputs.reserve(values.size());
  for (const auto &value : values) {
    if (!isRuntimeTextValue(value)) {
      return textFailure("text concatenation inputs must all be text arrays");
    }
    if (stringResult) {
      const auto converted = normalizedStringValue(value);
      if (!converted) {
        return textFailure(
            "character input cannot be converted for string concatenation");
      }
      inputs.push_back(*converted);
    } else {
      inputs.push_back(value);
    }
  }

  size_t dimensionCount = std::max<size_t>(dimension, 2);
  for (const auto &value : inputs) {
    dimensionCount = std::max(dimensionCount, runtimeDimensionCount(value));
  }
  const size_t axis = dimension - 1;
  auto outputDimensions = runtimeDimensions(inputs.front());
  outputDimensions.resize(dimensionCount, 1);
  outputDimensions[axis] = 0;
  std::vector<std::vector<size_t>> inputDimensions;
  for (const auto &value : inputs) {
    auto dimensions = runtimeDimensions(value);
    dimensions.resize(dimensionCount, 1);
    if (!inputDimensions.empty()) {
      for (size_t index = 0; index < dimensionCount; ++index) {
        if (index != axis &&
            dimensions[index] != inputDimensions.front()[index]) {
          return textFailure("text concatenation dimensions must agree outside "
                             "the selected dimension");
        }
      }
    }
    if (outputDimensions[axis] >
        std::numeric_limits<size_t>::max() - dimensions[axis]) {
      return textFailure("text concatenation dimensions are too large");
    }
    outputDimensions[axis] += dimensions[axis];
    inputDimensions.push_back(std::move(dimensions));
  }
  const auto outputCount = checkedRuntimeDimensionProduct(outputDimensions);
  if (!outputCount) {
    return textFailure("text concatenation dimensions are too large");
  }

  size_t axisOffset = 0;
  if (!stringResult) {
    std::u16string elements(*outputCount, u'\0');
    for (size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
      const auto &input = inputs[inputIndex];
      for (size_t sourceOffset = 0;
           sourceOffset < input.characterElements.size(); ++sourceOffset) {
        auto coordinates = runtimeRowMajorCoordinates(
            sourceOffset, inputDimensions[inputIndex]);
        coordinates[axis] += axisOffset;
        const auto outputOffset =
            runtimeRowMajorStorageOffset(coordinates, outputDimensions);
        if (!outputOffset) {
          return textFailure("text concatenation could not map a character");
        }
        elements[*outputOffset] = input.characterElements[sourceOffset];
      }
      axisOffset += inputDimensions[inputIndex][axis];
    }
    return textSuccess(
        makeRuntimeCharacterArray(outputDimensions, std::move(elements)));
  }

  std::vector<RuntimeStringElement> elements(*outputCount);
  for (size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
    const auto &input = inputs[inputIndex];
    for (size_t sourceOffset = 0; sourceOffset < input.stringElements.size();
         ++sourceOffset) {
      auto coordinates =
          runtimeRowMajorCoordinates(sourceOffset, inputDimensions[inputIndex]);
      coordinates[axis] += axisOffset;
      const auto outputOffset =
          runtimeRowMajorStorageOffset(coordinates, outputDimensions);
      if (!outputOffset) {
        return textFailure("text concatenation could not map a string element");
      }
      elements[*outputOffset] = input.stringElements[sourceOffset];
    }
    axisOffset += inputDimensions[inputIndex][axis];
  }
  return textSuccess(
      makeRuntimeStringArray(outputDimensions, std::move(elements)));
}

RuntimeTextOperationResult runtimeCompareText(std::string_view operation,
                                              const RuntimeValue &left,
                                              const RuntimeValue &right,
                                              bool ignoreCase) {
  const bool strcmpOperation =
      operation == "strcmp" || operation == "strcmpi";
  if (!strcmpOperation &&
      (!isRuntimeTextValue(left) || !isRuntimeTextValue(right))) {
    return textFailure("text comparison requires text operands");
  }
  const bool stringComparison =
      strcmpOperation ||
      isRuntimeStringArray(left) || isRuntimeStringArray(right);
  if (stringComparison) {
    const auto leftStrings = strcmpOperation
                                 ? normalizedComparisonStringValue(left)
                                 : normalizedStringValue(left);
    const auto rightStrings = strcmpOperation
                                  ? normalizedComparisonStringValue(right)
                                  : normalizedStringValue(right);
    if (!leftStrings || !rightStrings) {
      return textFailure(
          "text comparison requires text arrays or Cell arrays of text "
          "scalars");
    }
    const auto dimensions = runtimeImplicitExpansionDimensions(
        runtimeDimensions(*leftStrings), runtimeDimensions(*rightStrings));
    if (!dimensions) {
      return textFailure("string comparison dimensions are incompatible");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
      return textFailure("string comparison dimensions are too large");
    }
    std::vector<double> logical;
    logical.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
      const auto leftOffset =
          expandedTextStorageOffset(*leftStrings, *dimensions, index);
      const auto rightOffset =
          expandedTextStorageOffset(*rightStrings, *dimensions, index);
      const auto *a =
          leftOffset && *leftOffset < leftStrings->stringElements.size()
              ? &leftStrings->stringElements[*leftOffset]
              : nullptr;
      const auto *b =
          rightOffset && *rightOffset < rightStrings->stringElements.size()
              ? &rightStrings->stringElements[*rightOffset]
              : nullptr;
      if (!a || !b) {
        return textFailure("string comparison could not map an element");
      }
      bool equal =
          stringElementEqual(*a, *b, ignoreCase || operation == "strcmpi");
      if (operation == "~=") {
        equal = !equal;
      } else if (operation != "==" && operation != "strcmp" &&
                 operation != "strcmpi") {
        return textFailure("string arrays support ==, ~=, strcmp, and strcmpi");
      }
      logical.push_back(equal ? 1.0 : 0.0);
    }
    return textSuccess(logicalArray(*dimensions, std::move(logical)));
  }

  const auto dimensions = runtimeImplicitExpansionDimensions(
      runtimeDimensions(left), runtimeDimensions(right));
  if (!dimensions) {
    return textFailure("character comparison dimensions are incompatible");
  }
  const auto count = checkedRuntimeDimensionProduct(*dimensions);
  if (!count) {
    return textFailure("character comparison dimensions are too large");
  }
  std::vector<double> logical;
  logical.reserve(*count);
  for (size_t index = 0; index < *count; ++index) {
    const auto leftOffset = expandedTextStorageOffset(left, *dimensions, index);
    const auto rightOffset =
        expandedTextStorageOffset(right, *dimensions, index);
    if (!leftOffset || !rightOffset ||
        *leftOffset >= left.characterElements.size() ||
        *rightOffset >= right.characterElements.size()) {
      return textFailure("character comparison could not map an element");
    }
    const char16_t a = left.characterElements[*leftOffset];
    const char16_t b = right.characterElements[*rightOffset];
    bool result = false;
    if (operation == "==")
      result = a == b;
    else if (operation == "~=")
      result = a != b;
    else if (operation == "<")
      result = a < b;
    else if (operation == "<=")
      result = a <= b;
    else if (operation == ">")
      result = a > b;
    else if (operation == ">=")
      result = a >= b;
    else
      return textFailure("unsupported character comparison operator");
    logical.push_back(result ? 1.0 : 0.0);
  }
  return textSuccess(logicalArray(*dimensions, std::move(logical)));
}

RuntimeTextOperationResult runtimeAppendText(const RuntimeValue &left,
                                             const RuntimeValue &right) {
  const auto leftStrings = normalizedStringValue(left);
  const auto rightStrings = normalizedStringValue(right);
  if (!leftStrings || !rightStrings) {
    return textFailure("string append requires text operands");
  }
  const auto dimensions = runtimeImplicitExpansionDimensions(
      runtimeDimensions(*leftStrings), runtimeDimensions(*rightStrings));
  if (!dimensions) {
    return textFailure("string append dimensions are incompatible");
  }
  const auto count = checkedRuntimeDimensionProduct(*dimensions);
  if (!count) {
    return textFailure("string append dimensions are too large");
  }
  std::vector<RuntimeStringElement> result;
  result.reserve(*count);
  for (size_t index = 0; index < *count; ++index) {
    const auto leftOffset =
        expandedTextStorageOffset(*leftStrings, *dimensions, index);
    const auto rightOffset =
        expandedTextStorageOffset(*rightStrings, *dimensions, index);
    const auto *a =
        leftOffset && *leftOffset < leftStrings->stringElements.size()
            ? &leftStrings->stringElements[*leftOffset]
            : nullptr;
    const auto *b =
        rightOffset && *rightOffset < rightStrings->stringElements.size()
            ? &rightStrings->stringElements[*rightOffset]
            : nullptr;
    if (!a || !b) {
      return textFailure("string append could not map an element");
    }
    RuntimeStringElement element;
    element.missing = a->missing || b->missing;
    if (!element.missing) {
      element.value = a->value + b->value;
    }
    result.push_back(std::move(element));
  }
  const auto value = stringFromLogicalOrder(*dimensions, result);
  return value ? textSuccess(*value)
               : textFailure("string append result shape is invalid");
}

RuntimeTextOperationResult
runtimeConvertToCharacter(const RuntimeValue &value) {
  if (isRuntimeCharacterArray(value)) {
    return textSuccess(value);
  }
  if (isRuntimeStringArray(value)) {
    if (isRuntimeStringScalar(value)) {
      const auto &element = value.stringElements.front();
      return textSuccess(makeRuntimeCharacterVector(
          element.missing ? std::u16string{} : element.value));
    }
    const size_t count = runtimeShapeElementCount(value);
    size_t width = 0;
    for (size_t index = 0; index < count; ++index) {
      const auto *element = runtimeStringElement(value, index);
      if (!element) {
        return textFailure("char conversion could not map a string");
      }
      if (!element->missing) {
        width = std::max(width, element->value.size());
      }
    }
    const auto outputCount = checkedRuntimeDimensionProduct({count, width});
    if (!outputCount) {
      return textFailure("char conversion dimensions are too large");
    }
    std::u16string rows(*outputCount, u' ');
    for (size_t index = 0; index < count; ++index) {
      const auto *element = runtimeStringElement(value, index);
      if (!element->missing) {
        std::copy(element->value.begin(), element->value.end(),
                  rows.begin() + static_cast<std::ptrdiff_t>(index * width));
      }
    }
    return textSuccess(
        makeRuntimeCharacterArray({count, width}, std::move(rows)));
  }
  if (isRuntimeNumericValue(value)) {
    const size_t count = runtimeShapeElementCount(value);
    std::vector<char16_t> elements;
    elements.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const auto raw = runtimeNumericElementValue(value, index);
      if (!raw) {
        return textFailure("char conversion could not map a numeric element");
      }
      if (raw->complex) {
        return textFailure("complex values cannot be converted to char");
      }
      double codeUnit = raw->real;
      if (std::isnan(codeUnit)) {
        codeUnit = 32.0;
      } else {
        codeUnit = std::clamp(std::trunc(codeUnit), 0.0, 65535.0);
      }
      elements.push_back(static_cast<char16_t>(codeUnit));
    }
    const auto result =
        characterFromLogicalOrder(runtimeDimensions(value), elements);
    return result ? textSuccess(*result)
                  : textFailure("char conversion shape is invalid");
  }
  return textFailure("char conversion is not available for this value");
}

RuntimeTextOperationResult runtimeConvertToString(const RuntimeValue &value) {
  if (value.kind == RuntimeValueKind::MissingArray) {
    return textSuccess(makeRuntimeStringArray(
        runtimeDimensions(value),
        std::vector<RuntimeStringElement>(
            runtimeShapeElementCount(value),
            RuntimeStringElement{u"", true})));
  }
  if (isRuntimeStringArray(value)) {
    return textSuccess(value);
  }
  if (isRuntimeCharacterArray(value)) {
    const auto converted = normalizedStringValue(value);
    return converted ? textSuccess(*converted)
                     : textFailure("string conversion requires a "
                                   "two-dimensional character array");
  }
  if (isRuntimeNumericValue(value)) {
    const size_t count = runtimeShapeElementCount(value);
    std::vector<RuntimeStringElement> elements;
    elements.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const auto raw = runtimeNumericElementValue(value, index);
      if (!raw) {
        return textFailure("string conversion could not map a numeric element");
      }
      auto scalar = runtimeNumericValueFromElements(
          {1, 1}, {*raw}, raw->numericClass);
      if (!scalar) {
        return textFailure(
            "string conversion could not construct a numeric scalar");
      }
      elements.push_back(RuntimeStringElement{
          runtimeUtf8ToUtf16(runtimeValueToString(*scalar)), false});
    }
    const auto result =
        stringFromLogicalOrder(runtimeDimensions(value), elements);
    return result ? textSuccess(*result)
                  : textFailure("string conversion shape is invalid");
  }
  return textFailure("string conversion is not available for this value");
}

RuntimeTextOperationResult runtimeStringLengths(const RuntimeValue &value) {
  if (isRuntimeStringArray(value)) {
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> lengths;
    lengths.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const auto *element = runtimeStringElement(value, index);
      if (!element) {
        return textFailure("strlength could not map a string element");
      }
      lengths.push_back(element->missing
                            ? std::numeric_limits<double>::quiet_NaN()
                            : static_cast<double>(element->value.size()));
    }
    return textSuccess(
        numericArray(runtimeDimensions(value), std::move(lengths)));
  }
  if (isRuntimeCharacterArray(value)) {
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
      return textFailure(
          "strlength requires a two-dimensional character array");
    }
    if (dimensions[0] <= 1) {
      return textSuccess(
          numericArray({1, 1}, {static_cast<double>(dimensions[1])}));
    }
    return textSuccess(
        numericArray({dimensions[0], 1},
                     std::vector<double>(dimensions[0],
                                         static_cast<double>(dimensions[1]))));
  }
  return textFailure("strlength requires text input");
}

RuntimeTextOperationResult runtimeCharacterCodes(const RuntimeValue &value) {
  if (!isRuntimeCharacterArray(value)) {
    return textFailure("double text conversion requires a character array");
  }
  const size_t count = runtimeShapeElementCount(value);
  std::vector<double> elements;
  elements.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto element = runtimeCharacterElement(value, index);
    if (!element) {
      return textFailure("double text conversion could not map a character");
    }
    elements.push_back(static_cast<double>(*element));
  }
  return textSuccess(
      numericArray(runtimeDimensions(value), std::move(elements)));
}

RuntimeTextOperationResult
runtimeConvertStringToDouble(const RuntimeValue &value) {
  if (!isRuntimeStringArray(value)) {
    return textFailure("double string conversion requires a string array");
  }
  return textNumbersFromStrings(value, false);
}

RuntimeTextOperationResult runtimeStr2Double(const RuntimeValue &value) {
  if (isRuntimeStringArray(value)) {
    return textNumbersFromStrings(value, true);
  }
  if (isRuntimeCharacterArray(value)) {
    if (!isRuntimeCharacterVector(value)) {
      return textSuccess(makeRuntimeNumberValue(
          std::numeric_limits<double>::quiet_NaN()));
    }
    const auto text = runtimeTextScalarUtf8(value);
    const auto parsed =
        text ? parseTextNumericElement(*text, true) : std::nullopt;
    const auto result = runtimeNumericValueFromElements(
        {1, 1}, {parsed.value_or(invalidTextNumber())},
        RuntimeNumericClass::Double);
    return result ? textSuccess(*result)
                  : textFailure("str2double result is invalid");
  }
  if (value.kind == RuntimeValueKind::Cell) {
    const size_t count = runtimeShapeElementCount(value);
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const auto offset =
          runtimeColumnMajorLinearToStorageOffset(value, index);
      if (!offset || *offset >= value.cells.size()) {
        return textFailure("str2double could not map a cell element");
      }
      const RuntimeValue &cell = value.cells[*offset];
      const auto text = isRuntimeCharacterArray(cell) &&
                                isRuntimeCharacterVector(cell)
                            ? runtimeTextScalarUtf8(cell)
                            : std::nullopt;
      const auto parsed =
          text ? parseTextNumericElement(*text, true) : std::nullopt;
      elements.push_back(parsed.value_or(invalidTextNumber()));
    }
    const auto result = runtimeNumericValueFromElements(
        runtimeDimensions(value), std::move(elements),
        RuntimeNumericClass::Double);
    return result ? textSuccess(*result)
                  : textFailure("str2double cell result is invalid");
  }
  return textSuccess(makeRuntimeNumberValue(
      std::numeric_limits<double>::quiet_NaN()));
}

RuntimeTextOperationResult runtimeTextMissingMask(const RuntimeValue &value) {
  if (value.kind == RuntimeValueKind::MissingArray) {
    return textSuccess(logicalArray(
        runtimeDimensions(value),
        std::vector<double>(runtimeShapeElementCount(value), 1.0)));
  }
  if (isRuntimeNumericValue(value)) {
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> elements(count, 0.0);
    if (runtimeNumericClassIsFloating(value.numericClass)) {
      for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
          return textFailure("ismissing could not map a numeric element");
        }
        elements[index] =
            std::isnan(element->real) ||
                    (element->complex && std::isnan(element->imaginary))
                ? 1.0
                : 0.0;
      }
    }
    return textSuccess(
        logicalArray(runtimeDimensions(value), std::move(elements)));
  }
  if (isRuntimeCharacterArray(value)) {
    return textSuccess(logicalArray(
        runtimeDimensions(value),
        std::vector<double>(runtimeShapeElementCount(value), 0.0)));
  }
  if (!isRuntimeStringArray(value)) {
    return textFailure(
        "ismissing requires missing, numeric, character, or string input");
  }
  const size_t count = runtimeShapeElementCount(value);
  std::vector<double> elements;
  elements.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    const auto *element = runtimeStringElement(value, index);
    if (!element) {
      return textFailure("ismissing could not map a string element");
    }
    elements.push_back(element->missing ? 1.0 : 0.0);
  }
  return textSuccess(
      logicalArray(runtimeDimensions(value), std::move(elements)));
}

RuntimeTextOperationResult runtimeCellstr(const RuntimeValue &value) {
  std::vector<RuntimeValue> cells;
  std::vector<size_t> dimensions;
  if (isRuntimeStringArray(value)) {
    dimensions = runtimeDimensions(value);
    const size_t count = runtimeShapeElementCount(value);
    cells.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      const auto *element = runtimeStringElement(value, index);
      if (!element) {
        return textFailure("cellstr could not map a string element");
      }
      cells.push_back(makeRuntimeCharacterVector(
          element->missing ? std::u16string{} : element->value));
    }
  } else if (isRuntimeCharacterArray(value)) {
    const auto sourceDimensions = runtimeDimensions(value);
    if (sourceDimensions.size() != 2) {
      return textFailure("cellstr requires a two-dimensional character array");
    }
    const size_t rows = std::max<size_t>(sourceDimensions[0], 1);
    dimensions = sourceDimensions[0] <= 1
                     ? std::vector<size_t>{1, 1}
                     : std::vector<size_t>{sourceDimensions[0], 1};
    cells.reserve(rows);
    for (size_t row = 0; row < rows; ++row) {
      std::u16string text;
      if (sourceDimensions[0] != 0) {
        const size_t offset = row * sourceDimensions[1];
        text.assign(value.characterElements.data() + offset,
                    sourceDimensions[1]);
        while (!text.empty() && text.back() == u' ') {
          text.pop_back();
        }
      }
      cells.push_back(makeRuntimeCharacterVector(std::move(text)));
    }
  } else {
    return textFailure("cellstr requires character or string input");
  }
  auto result = cellFromLogicalOrder(std::move(dimensions), std::move(cells));
  return result ? textSuccess(std::move(*result))
                : textFailure("cellstr result shape is invalid");
}

} // namespace mparser
