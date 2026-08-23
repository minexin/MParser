#include "mparser/runtime/builtins/array/runtime_collection_builtins.h"

#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier) {
    return BuiltinResult::failure(call.span, std::move(message),
                                  std::move(identifier));
}

BuiltinResult exactOutputs(const BuiltinCall& call,
                           std::vector<RuntimeValue> outputs,
                           std::vector<Diagnostic> diagnostics = {}) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success({}, std::move(diagnostics));
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "collection builtin produced an unexpected output "
                       "count",
                       "MParser:CollectionContractViolation");
    }
    return BuiltinResult::success(std::move(outputs),
                                  std::move(diagnostics));
}

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

std::optional<size_t> positiveDimension(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    const auto converted = runtimeNumericElementAsNonnegativeSize(*element);
    return converted && *converted != 0 ? converted : std::nullopt;
}

std::optional<bool> logicalScalar(const RuntimeValue& value) {
    if (runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericTruthValue(value);
}

size_t firstNonsingletonDimension(const RuntimeValue& value) {
    const auto dimensions = runtimeDimensions(value);
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] != 1) {
            return index + 1;
        }
    }
    return 1;
}

bool numericMissing(const RuntimeNumericElementValue& value) {
    return runtimeNumericClassIsFloating(value.numericClass) &&
           (std::isnan(value.real) ||
            (value.complex && std::isnan(value.imaginary)));
}

int compareRealComponents(const RuntimeNumericElementValue& left,
                          const RuntimeNumericElementValue& right) {
    if (!left.complex && !right.complex) {
        return runtimeCompareNumericElementsForExtrema(left, right);
    }
    if (left.real < right.real) {
        return -1;
    }
    if (left.real > right.real) {
        return 1;
    }
    const double leftImaginary = left.complex ? left.imaginary : 0.0;
    const double rightImaginary = right.complex ? right.imaginary : 0.0;
    return leftImaginary < rightImaginary
               ? -1
               : (leftImaginary > rightImaginary ? 1 : 0);
}

enum class SortDirection { Ascending, Descending };
enum class MissingPlacement { Automatic, First, Last };
enum class ComparisonMethod { Automatic, Real, Magnitude };

struct SortOptions {
    size_t dimension = 1;
    SortDirection direction = SortDirection::Ascending;
    MissingPlacement missingPlacement = MissingPlacement::Automatic;
    ComparisonMethod comparisonMethod = ComparisonMethod::Automatic;
};

std::optional<SortOptions> parseSortOptions(
    const BuiltinCall& call, std::string& error) {
    SortOptions options;
    options.dimension = firstNonsingletonDimension(call.arguments.front());
    size_t cursor = 1;
    if (cursor < call.arguments.size()) {
        const RuntimeValue& candidate = call.arguments[cursor];
        if (isRuntimeNumericValue(candidate)) {
            if (runtimeShapeElementCount(candidate) != 0) {
                const auto dimension = positiveDimension(candidate);
                if (!dimension) {
                    error = "sort dimension must be a positive integer scalar";
                    return std::nullopt;
                }
                options.dimension = *dimension;
            }
            ++cursor;
        }
    }
    if (cursor < call.arguments.size()) {
        const auto direction = runtimeTextScalarUtf8(call.arguments[cursor]);
        if (direction) {
            const std::string lower = asciiLower(*direction);
            if (lower == "ascend" || lower == "descend") {
                options.direction = lower == "ascend"
                                        ? SortDirection::Ascending
                                        : SortDirection::Descending;
                ++cursor;
            }
        }
    }

    while (cursor < call.arguments.size()) {
        std::string name;
        const RuntimeValue* value = nullptr;
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            name = call.arguments[cursor].text;
            if (call.arguments[cursor].cells.size() != 1) {
                error = "sort name-value argument is malformed";
                return std::nullopt;
            }
            value = &call.arguments[cursor].cells.front();
            ++cursor;
        } else {
            const auto rawName =
                runtimeTextScalarUtf8(call.arguments[cursor]);
            if (!rawName || cursor + 1 >= call.arguments.size()) {
                error = "sort options must be name-value pairs";
                return std::nullopt;
            }
            name = *rawName;
            value = &call.arguments[cursor + 1];
            cursor += 2;
        }
        name = asciiLower(std::move(name));
        const auto rawValue = runtimeTextScalarUtf8(*value);
        if (!rawValue) {
            error = "sort option values must be text scalars";
            return std::nullopt;
        }
        const std::string lowerValue = asciiLower(*rawValue);
        if (name == "missingplacement") {
            if (lowerValue == "auto") {
                options.missingPlacement = MissingPlacement::Automatic;
            } else if (lowerValue == "first") {
                options.missingPlacement = MissingPlacement::First;
            } else if (lowerValue == "last") {
                options.missingPlacement = MissingPlacement::Last;
            } else {
                error = "MissingPlacement must be auto, first, or last";
                return std::nullopt;
            }
        } else if (name == "comparisonmethod") {
            if (lowerValue == "auto") {
                options.comparisonMethod = ComparisonMethod::Automatic;
            } else if (lowerValue == "real") {
                options.comparisonMethod = ComparisonMethod::Real;
            } else if (lowerValue == "abs") {
                options.comparisonMethod = ComparisonMethod::Magnitude;
            } else {
                error = "ComparisonMethod must be auto, real, or abs";
                return std::nullopt;
            }
        } else {
            error = "unknown sort option: " + name;
            return std::nullopt;
        }
    }
    return options;
}

struct NumericSortItem {
    RuntimeNumericElementValue value;
    size_t axisIndex = 0;
};

bool numericSortLess(const NumericSortItem& left,
                     const NumericSortItem& right,
                     const SortOptions& options) {
    const bool leftMissing = numericMissing(left.value);
    const bool rightMissing = numericMissing(right.value);
    if (leftMissing != rightMissing) {
        const bool missingFirst =
            options.missingPlacement == MissingPlacement::First ||
            (options.missingPlacement == MissingPlacement::Automatic &&
             options.direction == SortDirection::Descending);
        return leftMissing == missingFirst;
    }
    if (leftMissing && rightMissing) {
        return left.axisIndex < right.axisIndex;
    }
    int comparison =
        options.comparisonMethod == ComparisonMethod::Real
            ? compareRealComponents(left.value, right.value)
            : runtimeCompareNumericElementsForExtrema(left.value,
                                                      right.value);
    if (options.direction == SortDirection::Descending) {
        comparison = -comparison;
    }
    return comparison != 0 ? comparison < 0
                           : left.axisIndex < right.axisIndex;
}

BuiltinResult sortNumeric(const BuiltinCall& call,
                          const SortOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    RuntimeValue sorted = input;
    const auto originalDimensions = runtimeDimensions(input);
    auto dimensions = originalDimensions;
    dimensions.resize(std::max(dimensions.size(), options.dimension), 1);
    const size_t axis = options.dimension - 1;
    const size_t extent = dimensions[axis];
    const size_t count = runtimeShapeElementCount(input);
    std::vector<double> indexValues(count, 1.0);

    auto sliceDimensions = dimensions;
    sliceDimensions[axis] = 1;
    const auto sliceCount = checkedRuntimeDimensionProduct(sliceDimensions);
    if (!sliceCount) {
        return failure(call, "sort dimensions are too large",
                       "MParser:InvalidSortShape");
    }
    for (size_t slice = 0; slice < *sliceCount; ++slice) {
        auto coordinates = runtimeColumnMajorCoordinates(
            slice, sliceDimensions);
        if (!coordinates) {
            return failure(call, "sort could not map an array slice",
                           "MParser:InvalidSortShape");
        }
        std::vector<NumericSortItem> items;
        items.reserve(extent);
        for (size_t position = 0; position < extent; ++position) {
            (*coordinates)[axis] = position;
            const auto logical = runtimeColumnMajorLinearIndex(
                *coordinates, dimensions);
            const auto element = logical
                                     ? runtimeNumericElementValue(input,
                                                                  *logical)
                                     : std::nullopt;
            if (!logical || !element) {
                return failure(call,
                               "sort could not read a numeric element",
                               "MParser:InvalidSortInput");
            }
            items.push_back(NumericSortItem{*element, position});
        }
        std::stable_sort(items.begin(), items.end(),
                         [&options](const auto& left, const auto& right) {
                             return numericSortLess(left, right, options);
                         });
        for (size_t position = 0; position < extent; ++position) {
            (*coordinates)[axis] = position;
            const auto logical = runtimeColumnMajorLinearIndex(
                *coordinates, dimensions);
            if (!logical ||
                !runtimeStoreNumericElementValue(sorted, *logical,
                                                 items[position].value)) {
                return failure(call,
                               "sort could not store a numeric element",
                               "MParser:InvalidSortInput");
            }
            indexValues[*logical] =
                static_cast<double>(items[position].axisIndex + 1);
        }
    }

    std::vector<RuntimeValue> outputs{std::move(sorted)};
    if (call.requestedOutputCount > 1) {
        auto indices = runtimeNumericValueFromLogicalOrder(
            originalDimensions, std::move(indexValues),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call, "sort index output has an invalid shape",
                           "MParser:InvalidSortShape");
        }
        outputs.push_back(std::move(*indices));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult sortMissing(const BuiltinCall& call,
                          const SortOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    std::vector<RuntimeValue> outputs{input};
    if (call.requestedOutputCount <= 1) {
        return exactOutputs(call, std::move(outputs));
    }

    const auto originalDimensions = runtimeDimensions(input);
    auto dimensions = originalDimensions;
    dimensions.resize(std::max(dimensions.size(), options.dimension), 1);
    const size_t axis = options.dimension - 1;
    std::vector<size_t> strideDimensions(
        dimensions.begin(), dimensions.begin() +
                                static_cast<std::ptrdiff_t>(axis));
    const auto stride = checkedRuntimeDimensionProduct(strideDimensions);
    if (!stride || *stride == 0) {
        return failure(call, "sort dimensions are too large",
                       "MParser:InvalidSortShape");
    }
    const size_t extent = dimensions[axis];
    const size_t count = runtimeShapeElementCount(input);
    std::vector<double> indices(count, 1.0);
    for (size_t logical = 0; logical < count; ++logical) {
        indices[logical] = static_cast<double>(
            (logical / *stride) % extent + 1);
    }
    auto indexValue = runtimeNumericValueFromLogicalOrder(
        originalDimensions, std::move(indices),
        RuntimeNumericClass::Double);
    if (!indexValue) {
        return failure(call, "sort index output has an invalid shape",
                       "MParser:InvalidSortShape");
    }
    outputs.push_back(std::move(*indexValue));
    return exactOutputs(call, std::move(outputs));
}

struct TextSortItem {
    std::u16string value;
    bool missing = false;
    size_t axisIndex = 0;
    RuntimeValue original;
};

bool textSortLess(const TextSortItem& left, const TextSortItem& right,
                  const SortOptions& options) {
    if (left.missing != right.missing) {
        const bool missingFirst =
            options.missingPlacement == MissingPlacement::First ||
            (options.missingPlacement == MissingPlacement::Automatic &&
             options.direction == SortDirection::Descending);
        return left.missing == missingFirst;
    }
    int comparison = left.value < right.value
                         ? -1
                         : (left.value > right.value ? 1 : 0);
    if (options.direction == SortDirection::Descending) {
        comparison = -comparison;
    }
    return comparison != 0 ? comparison < 0
                           : left.axisIndex < right.axisIndex;
}

std::optional<TextSortItem> textSortItem(const RuntimeValue& input,
                                         size_t logicalIndex,
                                         size_t axisIndex) {
    if (isRuntimeCharacterArray(input)) {
        const auto element = runtimeCharacterElement(input, logicalIndex);
        return element ? std::optional<TextSortItem>(TextSortItem{
                             std::u16string(1, *element), false, axisIndex,
                             makeRuntimeCharacterArray({1, 1},
                                                       std::u16string(1,
                                                                      *element))})
                       : std::nullopt;
    }
    if (isRuntimeStringArray(input)) {
        const auto* element = runtimeStringElement(input, logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        return TextSortItem{
            element->value, element->missing, axisIndex,
            makeRuntimeStringArray({1, 1}, {*element})};
    }
    if (input.kind == RuntimeValueKind::Cell) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            input, logicalIndex);
        if (!offset || *offset >= input.cells.size()) {
            return std::nullopt;
        }
        const auto text = runtimeTextScalarCodeUnits(input.cells[*offset]);
        if (!text) {
            return std::nullopt;
        }
        return TextSortItem{*text, false, axisIndex,
                            input.cells[*offset]};
    }
    return std::nullopt;
}

RuntimeValue textResultFromLogical(
    const RuntimeValue& input, const std::vector<size_t>& dimensions,
    const std::vector<RuntimeValue>& logical) {
    if (isRuntimeCharacterArray(input)) {
        std::u16string storage(logical.size(), u'\0');
        for (size_t index = 0; index < logical.size(); ++index) {
            const auto coordinates = runtimeColumnMajorCoordinates(
                index, dimensions);
            const auto offset = coordinates
                                    ? runtimeRowMajorStorageOffset(
                                          *coordinates, dimensions)
                                    : std::nullopt;
            if (offset && !logical[index].characterElements.empty()) {
                storage[*offset] = logical[index].characterElements.front();
            }
        }
        return makeRuntimeCharacterArray(dimensions, std::move(storage));
    }
    if (isRuntimeStringArray(input)) {
        std::vector<RuntimeStringElement> storage(logical.size());
        for (size_t index = 0; index < logical.size(); ++index) {
            const auto coordinates = runtimeColumnMajorCoordinates(
                index, dimensions);
            const auto offset = coordinates
                                    ? runtimeRowMajorStorageOffset(
                                          *coordinates, dimensions)
                                    : std::nullopt;
            if (offset && !logical[index].stringElements.empty()) {
                storage[*offset] = logical[index].stringElements.front();
            }
        }
        return makeRuntimeStringArray(dimensions, std::move(storage));
    }
    std::vector<RuntimeValue> storage(logical.size());
    for (size_t index = 0; index < logical.size(); ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(*coordinates,
                                                              dimensions)
                                : std::nullopt;
        if (offset) {
            storage[*offset] = logical[index];
        }
    }
    return makeRuntimeCellValue(dimensions, std::move(storage));
}

BuiltinResult sortText(const BuiltinCall& call,
                       const SortOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    const auto originalDimensions = runtimeDimensions(input);
    auto dimensions = originalDimensions;
    dimensions.resize(std::max(dimensions.size(), options.dimension), 1);
    const size_t axis = options.dimension - 1;
    const size_t extent = dimensions[axis];
    std::vector<RuntimeValue> logical(runtimeShapeElementCount(input));
    std::vector<double> indexValues(logical.size(), 1.0);
    auto sliceDimensions = dimensions;
    sliceDimensions[axis] = 1;
    const auto sliceCount = checkedRuntimeDimensionProduct(sliceDimensions);
    if (!sliceCount) {
        return failure(call, "sort dimensions are too large",
                       "MParser:InvalidSortShape");
    }
    for (size_t slice = 0; slice < *sliceCount; ++slice) {
        auto coordinates = runtimeColumnMajorCoordinates(
            slice, sliceDimensions);
        if (!coordinates) {
            return failure(call, "sort could not map a text slice",
                           "MParser:InvalidSortShape");
        }
        std::vector<TextSortItem> items;
        items.reserve(extent);
        for (size_t position = 0; position < extent; ++position) {
            (*coordinates)[axis] = position;
            const auto source = runtimeColumnMajorLinearIndex(
                *coordinates, dimensions);
            const auto item = source
                                  ? textSortItem(input, *source, position)
                                  : std::nullopt;
            if (!source || !item) {
                return failure(call,
                               "sort text input contains an unsupported "
                               "element",
                               "MParser:InvalidSortInput");
            }
            items.push_back(*item);
        }
        std::stable_sort(items.begin(), items.end(),
                         [&options](const auto& left, const auto& right) {
                             return textSortLess(left, right, options);
                         });
        for (size_t position = 0; position < extent; ++position) {
            (*coordinates)[axis] = position;
            const auto destination = runtimeColumnMajorLinearIndex(
                *coordinates, dimensions);
            if (!destination || *destination >= logical.size()) {
                return failure(call,
                               "sort could not store a text element",
                               "MParser:InvalidSortShape");
            }
            logical[*destination] = items[position].original;
            indexValues[*destination] =
                static_cast<double>(items[position].axisIndex + 1);
        }
    }
    std::vector<RuntimeValue> outputs{
        textResultFromLogical(input, originalDimensions, logical)};
    if (call.requestedOutputCount > 1) {
        auto indices = runtimeNumericValueFromLogicalOrder(
            originalDimensions, std::move(indexValues),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call, "sort index output has an invalid shape",
                           "MParser:InvalidSortShape");
        }
        outputs.push_back(std::move(*indices));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult sortBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto options = parseSortOptions(call, error);
    if (!options) {
        return failure(call, std::move(error),
                       "MParser:InvalidSortOption");
    }
    if (isRuntimeNumericValue(call.arguments.front())) {
        return sortNumeric(call, *options);
    }
    if (call.arguments.front().kind == RuntimeValueKind::MissingArray) {
        return sortMissing(call, *options);
    }
    if (isRuntimeTextValue(call.arguments.front()) ||
        call.arguments.front().kind == RuntimeValueKind::Cell) {
        return sortText(call, *options);
    }
    return failure(call,
                   "sort expects a numeric, text, or Cell text array",
                   "MParser:InvalidSortInput");
}

bool numericElementsEqual(const RuntimeNumericElementValue& left,
                          const RuntimeNumericElementValue& right,
                          bool missingDistinct) {
    const bool leftMissing = numericMissing(left);
    const bool rightMissing = numericMissing(right);
    if (leftMissing || rightMissing) {
        return !missingDistinct && leftMissing && rightMissing;
    }
    if (left.numericClass != right.numericClass ||
        left.complex != right.complex) {
        return false;
    }
    if (runtimeNumericClassIsInteger(left.numericClass) ||
        left.numericClass == RuntimeNumericClass::Logical) {
        return left.integerRealBits == right.integerRealBits &&
               (!left.complex ||
                left.integerImaginaryBits ==
                    right.integerImaginaryBits);
    }
    return left.real == right.real &&
           (!left.complex || left.imaginary == right.imaginary);
}

struct UniqueOptions {
    bool stable = false;
    bool last = false;
    bool rows = false;
    bool missingDistinct = false;
};

std::optional<UniqueOptions> parseUniqueOptions(
    const BuiltinCall& call, std::string& error) {
    UniqueOptions options;
    size_t cursor = 1;
    while (cursor < call.arguments.size()) {
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            const std::string name =
                asciiLower(call.arguments[cursor].text);
            if (name != "treatmissingasdistinct" ||
                call.arguments[cursor].cells.size() != 1) {
                error = "unsupported unique name-value option";
                return std::nullopt;
            }
            const auto value =
                logicalScalar(call.arguments[cursor].cells.front());
            if (!value) {
                error = "TreatMissingAsDistinct must be logical scalar";
                return std::nullopt;
            }
            options.missingDistinct = *value;
            ++cursor;
            continue;
        }
        const auto raw = runtimeTextScalarUtf8(call.arguments[cursor]);
        if (!raw) {
            error = "unique options must be text scalars";
            return std::nullopt;
        }
        const std::string option = asciiLower(*raw);
        if (option == "treatmissingasdistinct") {
            if (cursor + 1 >= call.arguments.size()) {
                error = "TreatMissingAsDistinct requires a value";
                return std::nullopt;
            }
            const auto value = logicalScalar(call.arguments[cursor + 1]);
            if (!value) {
                error = "TreatMissingAsDistinct must be logical scalar";
                return std::nullopt;
            }
            options.missingDistinct = *value;
            cursor += 2;
            continue;
        }
        if (option == "sorted") {
            options.stable = false;
        } else if (option == "stable") {
            options.stable = true;
        } else if (option == "first") {
            options.last = false;
        } else if (option == "last") {
            options.last = true;
        } else if (option == "rows") {
            options.rows = true;
        } else {
            error = "unsupported unique option: " + *raw;
            return std::nullopt;
        }
        ++cursor;
    }
    return options;
}

struct NumericUniqueGroup {
    std::vector<RuntimeNumericElementValue> key;
    std::vector<size_t> sources;
};

struct NumericUniqueEntry {
    std::vector<RuntimeNumericElementValue> key;
    size_t source = 0;
};

int compareNumericKeys(
    const std::vector<RuntimeNumericElementValue>& left,
    const std::vector<RuntimeNumericElementValue>& right) {
    const size_t count = std::min(left.size(), right.size());
    for (size_t index = 0; index < count; ++index) {
        const bool leftMissing = numericMissing(left[index]);
        const bool rightMissing = numericMissing(right[index]);
        if (leftMissing != rightMissing) {
            return leftMissing ? 1 : -1;
        }
        if (leftMissing) {
            continue;
        }
        const int comparison = runtimeCompareNumericElementsForExtrema(
            left[index], right[index]);
        if (comparison != 0) {
            return comparison;
        }
    }
    return left.size() < right.size()
               ? -1
               : (left.size() > right.size() ? 1 : 0);
}

bool numericKeysEqual(
    const std::vector<RuntimeNumericElementValue>& left,
    const std::vector<RuntimeNumericElementValue>& right,
    bool missingDistinct) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [missingDistinct](const auto& a, const auto& b) {
                          return numericElementsEqual(a, b,
                                                      missingDistinct);
                      });
}

BuiltinResult uniqueNumeric(const BuiltinCall& call,
                            const UniqueOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    const auto dimensions = runtimeDimensions(input);
    if (options.rows && dimensions.size() != 2) {
        return failure(call,
                       "unique rows requires a two-dimensional input",
                       "MParser:InvalidUniqueShape");
    }
    const size_t sourceCount = options.rows ? dimensions[0]
                                            : runtimeShapeElementCount(input);
    const size_t keyWidth = options.rows ? dimensions[1] : 1;
    std::vector<NumericUniqueEntry> entries;
    entries.reserve(sourceCount);
    for (size_t source = 0; source < sourceCount; ++source) {
        std::vector<RuntimeNumericElementValue> key;
        key.reserve(keyWidth);
        for (size_t column = 0; column < keyWidth; ++column) {
            const size_t logical = options.rows
                                       ? source + dimensions[0] * column
                                       : source;
            const auto element = runtimeNumericElementValue(input, logical);
            if (!element) {
                return failure(call,
                               "unique could not read a numeric element",
                               "MParser:InvalidUniqueInput");
            }
            key.push_back(*element);
        }
        entries.push_back(NumericUniqueEntry{std::move(key), source});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& left, const auto& right) {
                         const int comparison =
                             compareNumericKeys(left.key, right.key);
                         return comparison != 0
                                    ? comparison < 0
                                    : left.source < right.source;
                     });

    std::vector<NumericUniqueGroup> groups;
    groups.reserve(entries.size());
    for (auto& entry : entries) {
        if (groups.empty() ||
            !numericKeysEqual(groups.back().key, entry.key,
                              options.missingDistinct)) {
            groups.push_back(NumericUniqueGroup{
                std::move(entry.key), {entry.source}});
        } else {
            groups.back().sources.push_back(entry.source);
        }
    }
    if (options.stable) {
        std::stable_sort(groups.begin(), groups.end(),
                         [](const auto& left, const auto& right) {
                             return left.sources.front() <
                                    right.sources.front();
                         });
    }

    std::vector<size_t> representative(groups.size(), 0);
    std::vector<double> firstIndices;
    firstIndices.reserve(groups.size());
    std::vector<double> inverse(sourceCount, 0.0);
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        representative[groupIndex] = options.last
            ? groups[groupIndex].sources.back()
            : groups[groupIndex].sources.front();
        firstIndices.push_back(
            static_cast<double>(representative[groupIndex] + 1));
        for (const size_t source : groups[groupIndex].sources) {
            inverse[source] = static_cast<double>(groupIndex + 1);
        }
    }

    std::vector<RuntimeNumericElementValue> values;
    std::vector<size_t> outputDimensions;
    if (options.rows) {
        outputDimensions = {groups.size(), dimensions[1]};
        values.reserve(groups.size() * dimensions[1]);
        for (size_t column = 0; column < dimensions[1]; ++column) {
            for (size_t group = 0; group < groups.size(); ++group) {
                const size_t logical = representative[group] +
                                       dimensions[0] * column;
                const auto value = runtimeNumericElementValue(input,
                                                               logical);
                if (!value) {
                    return failure(call,
                                   "unique could not map an output row",
                                   "MParser:InvalidUniqueInput");
                }
                values.push_back(*value);
            }
        }
    } else {
        const bool rowVector = dimensions.size() == 2 &&
                               dimensions[0] == 1;
        const bool columnVector = dimensions.size() == 2 &&
                                  dimensions[1] == 1;
        outputDimensions = rowVector ? std::vector<size_t>{1, groups.size()}
                           : columnVector
                               ? std::vector<size_t>{groups.size(), 1}
                               : std::vector<size_t>{groups.size(), 1};
        values.reserve(groups.size());
        for (const size_t source : representative) {
            const auto value = runtimeNumericElementValue(input, source);
            if (!value) {
                return failure(call,
                               "unique could not map an output element",
                               "MParser:InvalidUniqueInput");
            }
            values.push_back(*value);
        }
    }
    auto uniqueValues = runtimeNumericValueFromElements(
        outputDimensions, std::move(values), input.numericClass);
    if (!uniqueValues) {
        return failure(call, "unique output has an invalid shape",
                       "MParser:InvalidUniqueShape");
    }
    std::vector<RuntimeValue> outputs{std::move(*uniqueValues)};
    if (call.requestedOutputCount > 1) {
        const size_t indexCount = firstIndices.size();
        auto indices = runtimeNumericValueFromLogicalOrder(
            {indexCount, 1}, std::move(firstIndices),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call, "unique index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*indices));
    }
    if (call.requestedOutputCount > 2) {
        const size_t inverseCount = inverse.size();
        auto indices = runtimeNumericValueFromLogicalOrder(
            {inverseCount, 1}, std::move(inverse),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call,
                           "unique inverse index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*indices));
    }
    return exactOutputs(call, std::move(outputs));
}

bool textElementsEqual(const TextSortItem& left,
                       const TextSortItem& right,
                       bool missingDistinct) {
    if (left.missing || right.missing) {
        return !missingDistinct && left.missing && right.missing;
    }
    return left.value == right.value;
}

int compareTextKeys(const std::vector<TextSortItem>& left,
                    const std::vector<TextSortItem>& right) {
    const size_t count = std::min(left.size(), right.size());
    for (size_t index = 0; index < count; ++index) {
        if (left[index].missing != right[index].missing) {
            return left[index].missing ? 1 : -1;
        }
        if (left[index].missing) {
            continue;
        }
        if (left[index].value < right[index].value) {
            return -1;
        }
        if (left[index].value > right[index].value) {
            return 1;
        }
    }
    return left.size() < right.size()
               ? -1
               : (left.size() > right.size() ? 1 : 0);
}

bool textKeysEqual(const std::vector<TextSortItem>& left,
                   const std::vector<TextSortItem>& right,
                   bool missingDistinct) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [missingDistinct](const auto& a, const auto& b) {
                          return textElementsEqual(a, b,
                                                   missingDistinct);
                      });
}

struct TextUniqueEntry {
    std::vector<TextSortItem> key;
    size_t source = 0;
};

struct TextUniqueGroup {
    std::vector<TextSortItem> key;
    std::vector<size_t> sources;
};

BuiltinResult uniqueText(const BuiltinCall& call,
                         const UniqueOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    const auto dimensions = runtimeDimensions(input);
    if (options.rows && dimensions.size() != 2) {
        return failure(call,
                       "unique rows requires a two-dimensional input",
                       "MParser:InvalidUniqueShape");
    }
    const size_t sourceCount = options.rows
                                   ? dimensions[0]
                                   : runtimeShapeElementCount(input);
    const size_t keyWidth = options.rows ? dimensions[1] : 1;
    std::vector<TextUniqueEntry> entries;
    entries.reserve(sourceCount);
    for (size_t source = 0; source < sourceCount; ++source) {
        std::vector<TextSortItem> key;
        key.reserve(keyWidth);
        for (size_t column = 0; column < keyWidth; ++column) {
            const size_t logical = options.rows
                                       ? source + dimensions[0] * column
                                       : source;
            auto element = textSortItem(input, logical, source);
            if (!element) {
                return failure(call,
                               "unique text input contains an unsupported "
                               "element",
                               "MParser:InvalidUniqueInput");
            }
            key.push_back(std::move(*element));
        }
        entries.push_back(TextUniqueEntry{std::move(key), source});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const auto& left, const auto& right) {
                         const int comparison =
                             compareTextKeys(left.key, right.key);
                         return comparison != 0
                                    ? comparison < 0
                                    : left.source < right.source;
                     });

    std::vector<TextUniqueGroup> groups;
    groups.reserve(entries.size());
    for (auto& entry : entries) {
        if (groups.empty() ||
            !textKeysEqual(groups.back().key, entry.key,
                           options.missingDistinct)) {
            groups.push_back(TextUniqueGroup{
                std::move(entry.key), {entry.source}});
        } else {
            groups.back().sources.push_back(entry.source);
        }
    }
    if (options.stable) {
        std::stable_sort(groups.begin(), groups.end(),
                         [](const auto& left, const auto& right) {
                             return left.sources.front() <
                                    right.sources.front();
                         });
    }

    std::vector<size_t> representative(groups.size(), 0);
    std::vector<double> firstIndices;
    firstIndices.reserve(groups.size());
    std::vector<double> inverse(sourceCount, 0.0);
    for (size_t group = 0; group < groups.size(); ++group) {
        representative[group] = options.last
                                    ? groups[group].sources.back()
                                    : groups[group].sources.front();
        firstIndices.push_back(
            static_cast<double>(representative[group] + 1));
        for (const size_t source : groups[group].sources) {
            inverse[source] = static_cast<double>(group + 1);
        }
    }

    std::vector<size_t> outputDimensions;
    std::vector<RuntimeValue> logical;
    if (options.rows) {
        outputDimensions = {groups.size(), dimensions[1]};
        logical.reserve(groups.size() * dimensions[1]);
        for (size_t column = 0; column < dimensions[1]; ++column) {
            for (size_t group = 0; group < groups.size(); ++group) {
                const size_t source = representative[group] +
                                      dimensions[0] * column;
                auto element = textSortItem(input, source, group);
                if (!element) {
                    return failure(call,
                                   "unique could not map a text output row",
                                   "MParser:InvalidUniqueInput");
                }
                logical.push_back(std::move(element->original));
            }
        }
    } else {
        const bool rowVector = dimensions.size() == 2 &&
                               dimensions[0] == 1;
        outputDimensions = rowVector
                               ? std::vector<size_t>{1, groups.size()}
                               : std::vector<size_t>{groups.size(), 1};
        logical.reserve(groups.size());
        for (size_t group = 0; group < groups.size(); ++group) {
            auto element = textSortItem(input, representative[group],
                                        group);
            if (!element) {
                return failure(call,
                               "unique could not map a text output element",
                               "MParser:InvalidUniqueInput");
            }
            logical.push_back(std::move(element->original));
        }
    }

    std::vector<RuntimeValue> outputs{
        textResultFromLogical(input, outputDimensions, logical)};
    if (call.requestedOutputCount > 1) {
        const size_t indexCount = firstIndices.size();
        auto indices = runtimeNumericValueFromLogicalOrder(
            {indexCount, 1}, std::move(firstIndices),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call, "unique index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*indices));
    }
    if (call.requestedOutputCount > 2) {
        const size_t inverseCount = inverse.size();
        auto indices = runtimeNumericValueFromLogicalOrder(
            {inverseCount, 1}, std::move(inverse),
            RuntimeNumericClass::Double);
        if (!indices) {
            return failure(call,
                           "unique inverse index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*indices));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult uniqueMissing(const BuiltinCall& call,
                            const UniqueOptions& options) {
    const RuntimeValue& input = call.arguments.front();
    const auto dimensions = runtimeDimensions(input);
    if (options.rows && dimensions.size() != 2) {
        return failure(call,
                       "unique rows requires a two-dimensional input",
                       "MParser:InvalidUniqueShape");
    }
    const size_t sourceCount = options.rows
                                   ? dimensions[0]
                                   : runtimeShapeElementCount(input);
    const size_t groupCount = options.missingDistinct
                                  ? sourceCount
                                  : (sourceCount == 0 ? 0 : 1);
    std::vector<size_t> outputDimensions;
    if (options.rows) {
        outputDimensions = {groupCount, dimensions[1]};
    } else {
        const bool rowVector = dimensions.size() == 2 &&
                               dimensions[0] == 1;
        outputDimensions = rowVector
                               ? std::vector<size_t>{1, groupCount}
                               : std::vector<size_t>{groupCount, 1};
    }
    std::vector<RuntimeValue> outputs{
        makeRuntimeMissingArrayValue(outputDimensions)};
    if (call.requestedOutputCount > 1) {
        std::vector<double> representatives(groupCount, 1.0);
        if (options.missingDistinct) {
            std::iota(representatives.begin(), representatives.end(), 1.0);
        } else if (groupCount != 0 && options.last) {
            representatives.front() = static_cast<double>(sourceCount);
        }
        auto indexValue = runtimeNumericValueFromLogicalOrder(
            {groupCount, 1}, std::move(representatives),
            RuntimeNumericClass::Double);
        if (!indexValue) {
            return failure(call,
                           "unique index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*indexValue));
    }
    if (call.requestedOutputCount > 2) {
        std::vector<double> inverse(sourceCount, 1.0);
        if (options.missingDistinct) {
            std::iota(inverse.begin(), inverse.end(), 1.0);
        }
        auto inverseValue = runtimeNumericValueFromLogicalOrder(
            {sourceCount, 1}, std::move(inverse),
            RuntimeNumericClass::Double);
        if (!inverseValue) {
            return failure(call,
                           "unique inverse index output has an invalid shape",
                           "MParser:InvalidUniqueShape");
        }
        outputs.push_back(std::move(*inverseValue));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult uniqueBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto options = parseUniqueOptions(call, error);
    if (!options) {
        return failure(call, std::move(error),
                       "MParser:InvalidUniqueOption");
    }
    if (!isRuntimeNumericValue(call.arguments.front())) {
        if (call.arguments.front().kind ==
            RuntimeValueKind::MissingArray) {
            return uniqueMissing(call, *options);
        }
        if (isRuntimeTextValue(call.arguments.front()) ||
            call.arguments.front().kind == RuntimeValueKind::Cell) {
            return uniqueText(call, *options);
        }
        return failure(call,
                       "unique expects a numeric, text, Cell text, or "
                       "missing array",
                       "MParser:InvalidUniqueInput");
    }
    return uniqueNumeric(call, *options);
}

std::optional<std::vector<std::string>> fieldNames(
    const RuntimeValue& value) {
    if (isRuntimeCharacterVector(value)) {
        const auto scalar = runtimeTextScalarUtf8(value);
        return scalar ? std::optional<std::vector<std::string>>(
                            std::vector<std::string>{*scalar})
                      : std::nullopt;
    }
    std::vector<std::string> names;
    if (isRuntimeStringArray(value)) {
        names.reserve(runtimeShapeElementCount(value));
        for (size_t index = 0; index < runtimeShapeElementCount(value);
             ++index) {
            const auto* element = runtimeStringElement(value, index);
            if (!element || element->missing) {
                return std::nullopt;
            }
            names.push_back(runtimeUtf16ToUtf8(element->value));
        }
    } else if (value.kind == RuntimeValueKind::Cell) {
        names.reserve(runtimeShapeElementCount(value));
        for (size_t index = 0; index < runtimeShapeElementCount(value);
             ++index) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                value, index);
            if (!offset || *offset >= value.cells.size()) {
                return std::nullopt;
            }
            const auto name = runtimeTextScalarUtf8(value.cells[*offset]);
            if (!name) {
                return std::nullopt;
            }
            names.push_back(*name);
        }
    } else {
        return std::nullopt;
    }
    if (std::any_of(names.begin(), names.end(), [](const std::string& name) {
            return !isRuntimeStructFieldName(name);
        })) {
        return std::nullopt;
    }
    if (std::set<std::string>(names.begin(), names.end()).size() !=
        names.size()) {
        return std::nullopt;
    }
    return names;
}

BuiltinResult struct2cellBuiltin(const BuiltinCall& call) {
    const RuntimeValue& structure = call.arguments.front();
    if (structure.kind != RuntimeValueKind::Struct) {
        return failure(call, "struct2cell expects a structure array",
                       "MParser:InvalidStructConversion");
    }
    const auto fields = runtimeStructFieldOrder(structure);
    const auto structureDimensions = runtimeDimensions(structure);
    std::vector<size_t> outputDimensions{fields.size()};
    outputDimensions.insert(outputDimensions.end(),
                            structureDimensions.begin(),
                            structureDimensions.end());
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount = checkedRuntimeDimensionProduct(
        outputDimensions);
    if (!outputCount) {
        return failure(call,
                       "struct2cell output dimensions are too large",
                       "MParser:InvalidStructConversion");
    }
    std::vector<RuntimeValue> cells(*outputCount);
    for (size_t outputOffset = 0; outputOffset < *outputCount;
         ++outputOffset) {
        const auto coordinates = runtimeRowMajorCoordinates(
            outputOffset, outputDimensions);
        if (coordinates.empty() || coordinates.front() >= fields.size()) {
            continue;
        }
        std::vector<size_t> structureCoordinates(
            coordinates.begin() + 1, coordinates.end());
        structureCoordinates.resize(structureDimensions.size(), 0);
        const auto structureOffset = runtimeRowMajorStorageOffset(
            structureCoordinates, structureDimensions);
        const RuntimeValue* field = structureOffset
            ? runtimeStructField(structure, fields[coordinates.front()],
                                 *structureOffset)
            : nullptr;
        if (!field) {
            return failure(call,
                           "struct2cell could not map a structure field",
                           "MParser:InvalidStructConversion");
        }
        cells[outputOffset] = *field;
    }
    return exactOutputs(
        call, {makeRuntimeCellValue(outputDimensions, std::move(cells))});
}

BuiltinResult cell2structBuiltin(const BuiltinCall& call) {
    const RuntimeValue& cells = call.arguments[0];
    if (cells.kind != RuntimeValueKind::Cell) {
        return failure(call, "cell2struct expects a Cell array",
                       "MParser:InvalidStructConversion");
    }
    const auto names = fieldNames(call.arguments[1]);
    if (!names) {
        return failure(call,
                       "cell2struct field names must be valid text names",
                       "MParser:InvalidStructFieldName");
    }
    const size_t dimension = call.arguments.size() == 3
                                 ? positiveDimension(call.arguments[2])
                                       .value_or(0)
                                 : 1;
    if (dimension == 0) {
        return failure(call,
                       "cell2struct dimension must be a positive integer",
                       "MParser:InvalidStructConversion");
    }
    auto inputDimensions = runtimeDimensions(cells);
    inputDimensions.resize(std::max(inputDimensions.size(), dimension), 1);
    const size_t axis = dimension - 1;
    if (inputDimensions[axis] != names->size()) {
        return failure(call,
                       "cell2struct field count must match the selected "
                       "Cell dimension",
                       "MParser:InvalidStructConversion");
    }
    auto outputDimensions = inputDimensions;
    outputDimensions[axis] = 1;
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount = checkedRuntimeDimensionProduct(
        outputDimensions);
    if (!outputCount) {
        return failure(call,
                       "cell2struct output dimensions are too large",
                       "MParser:InvalidStructConversion");
    }
    std::vector<RuntimeStructElement> elements(*outputCount);
    for (size_t outputOffset = 0; outputOffset < *outputCount;
         ++outputOffset) {
        auto coordinates = runtimeRowMajorCoordinates(
            outputOffset, outputDimensions);
        coordinates.resize(inputDimensions.size(), 0);
        for (size_t field = 0; field < names->size(); ++field) {
            coordinates[axis] = field;
            const auto cellOffset = runtimeRowMajorStorageOffset(
                coordinates, inputDimensions);
            if (!cellOffset || *cellOffset >= cells.cells.size()) {
                return failure(call,
                               "cell2struct could not map a Cell element",
                               "MParser:InvalidStructConversion");
            }
            elements[outputOffset].emplace((*names)[field],
                                           cells.cells[*cellOffset]);
        }
    }
    return exactOutputs(
        call, {makeRuntimeStructArrayValue(*names, std::move(elements),
                                           outputDimensions)});
}

RuntimeValue cellFromLogicalOrder(std::vector<size_t> dimensions,
                                  std::vector<RuntimeValue> logical) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    std::vector<RuntimeValue> storage(logical.size());
    for (size_t index = 0; index < logical.size(); ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(*coordinates,
                                                              dimensions)
                                : std::nullopt;
        if (offset) {
            storage[*offset] = std::move(logical[index]);
        }
    }
    return makeRuntimeCellValue(std::move(dimensions),
                                std::move(storage));
}

std::optional<RuntimeValue> cellfunInputElement(
    const RuntimeValue& input, size_t logicalIndex) {
    if (input.kind == RuntimeValueKind::Cell) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            input, logicalIndex);
        return offset && *offset < input.cells.size()
                   ? std::optional<RuntimeValue>(input.cells[*offset])
                   : std::nullopt;
    }
    if (isRuntimeStringArray(input)) {
        const auto* element = runtimeStringElement(input, logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        return element->missing
                   ? std::optional<RuntimeValue>(
                         makeRuntimeMissingArrayValue())
                   : std::optional<RuntimeValue>(
                         makeRuntimeCharacterVector(element->value));
    }
    return std::nullopt;
}

BuiltinResult cellfunBuiltin(const BuiltinCall& call) {
    const RuntimeValue& callable = call.arguments.front();
    size_t cursor = 1;
    std::vector<const RuntimeValue*> inputs;
    while (cursor < call.arguments.size()) {
        const RuntimeValue& candidate = call.arguments[cursor];
        if (candidate.kind == RuntimeValueKind::Cell ||
            isRuntimeStringArray(candidate)) {
            inputs.push_back(&candidate);
            ++cursor;
            continue;
        }
        break;
    }
    if (inputs.empty()) {
        return failure(call,
                       "cellfun requires at least one Cell or string input",
                       "MParser:InvalidCellfunInput");
    }
    const auto dimensions = runtimeDimensions(*inputs.front());
    if (std::any_of(inputs.begin(), inputs.end(),
                    [&dimensions](const RuntimeValue* value) {
                        return runtimeDimensions(*value) != dimensions;
                    })) {
        return failure(call,
                       "cellfun input arrays must have identical dimensions",
                       "MParser:InvalidCellfunShape");
    }

    bool uniformOutput = true;
    std::optional<RuntimeValue> errorHandler;
    while (cursor < call.arguments.size()) {
        std::string name;
        const RuntimeValue* value = nullptr;
        if (call.arguments[cursor].kind ==
            RuntimeValueKind::NameValueArgument) {
            name = call.arguments[cursor].text;
            if (call.arguments[cursor].cells.size() != 1) {
                return failure(call,
                               "cellfun name-value argument is malformed",
                               "MParser:InvalidCellfunOption");
            }
            value = &call.arguments[cursor].cells.front();
            ++cursor;
        } else {
            const auto rawName =
                runtimeTextScalarUtf8(call.arguments[cursor]);
            if (!rawName || cursor + 1 >= call.arguments.size()) {
                return failure(call,
                               "cellfun options must be name-value pairs",
                               "MParser:InvalidCellfunOption");
            }
            name = *rawName;
            value = &call.arguments[cursor + 1];
            cursor += 2;
        }
        name = asciiLower(std::move(name));
        if (name == "uniformoutput") {
            const auto raw = logicalScalar(*value);
            if (!raw) {
                return failure(call,
                               "UniformOutput must be a logical scalar",
                               "MParser:InvalidCellfunOption");
            }
            uniformOutput = *raw;
        } else if (name == "errorhandler") {
            errorHandler = *value;
        } else {
            return failure(call, "unknown cellfun option: " + name,
                           "MParser:InvalidCellfunOption");
        }
    }
    if (!call.context || !call.context->dynamicInvoker) {
        return failure(call,
                       "cellfun requires the runtime dynamic-call context",
                       "MParser:MissingBuiltinContext");
    }

    const size_t count = runtimeShapeElementCount(*inputs.front());
    std::vector<std::vector<RuntimeValue>> logicalOutputs(
        call.requestedOutputCount);
    for (auto& output : logicalOutputs) {
        output.reserve(count);
    }
    std::vector<Diagnostic> diagnostics;
    for (size_t index = 0; index < count; ++index) {
        if (call.context->executionControl &&
            (index & 255U) == 0U &&
            !call.context->executionControl->checkpoint()) {
            return failure(call,
                           "cellfun was stopped by runtime execution control",
                           "MParser:ExecutionStopped");
        }
        std::vector<RuntimeValue> arguments;
        arguments.reserve(inputs.size());
        for (const RuntimeValue* input : inputs) {
            const auto element = cellfunInputElement(*input, index);
            if (!element) {
                return failure(call,
                               "cellfun could not map an input element",
                               "MParser:InvalidCellfunShape");
            }
            arguments.push_back(*element);
        }
        BuiltinResult invoked = call.context->dynamicInvoker(
            callable, arguments, call.requestedOutputCount, call.span);
        if (!invoked.succeeded && errorHandler) {
            std::string identifier = "MParser:CellfunCallbackFailed";
            std::string message = "cellfun callback failed";
            if (!invoked.diagnostics.empty()) {
                identifier = invoked.diagnostics.front().identifier;
                message = invoked.diagnostics.front().message;
            }
            RuntimeValue errorInfo = makeRuntimeStructValue({
                {"identifier", makeRuntimeCharacterVectorUtf8(identifier)},
                {"index", makeRuntimeNumberValue(
                              static_cast<double>(index + 1))},
                {"message", makeRuntimeCharacterVectorUtf8(message)},
            });
            errorInfo.fieldOrder = {"identifier", "message", "index"};
            arguments.insert(arguments.begin(), std::move(errorInfo));
            invoked = call.context->dynamicInvoker(
                *errorHandler, arguments, call.requestedOutputCount,
                call.span);
        }
        if (!invoked.succeeded) {
            if (invoked.diagnostics.empty()) {
                return failure(call, "cellfun callback failed",
                               "MParser:CellfunCallbackFailed");
            }
            return BuiltinResult{false, {},
                                 std::move(invoked.diagnostics)};
        }
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(
                               invoked.diagnostics.begin()),
                           std::make_move_iterator(
                               invoked.diagnostics.end()));
        if (invoked.outputs.size() != call.requestedOutputCount) {
            return failure(call,
                           "cellfun callback returned an unexpected output "
                           "count",
                           "MParser:CellfunCallbackContract");
        }
        for (size_t output = 0; output < invoked.outputs.size(); ++output) {
            logicalOutputs[output].push_back(
                std::move(invoked.outputs[output]));
        }
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success({}, std::move(diagnostics));
    }

    std::vector<RuntimeValue> outputs;
    outputs.reserve(call.requestedOutputCount);
    for (auto& logical : logicalOutputs) {
        if (!uniformOutput) {
            outputs.push_back(cellFromLogicalOrder(dimensions,
                                                   std::move(logical)));
            continue;
        }
        if (logical.empty()) {
            auto empty = runtimeNumericValueFromLogicalOrder(
                dimensions, {}, RuntimeNumericClass::Double);
            if (!empty) {
                return failure(call,
                               "cellfun empty output has an invalid shape",
                               "MParser:InvalidCellfunOutput");
            }
            outputs.push_back(std::move(*empty));
            continue;
        }
        if (std::any_of(logical.begin(), logical.end(),
                        [](const RuntimeValue& value) {
                            return runtimeShapeElementCount(value) != 1;
                        })) {
            return failure(call,
                           "cellfun UniformOutput requires scalar callback "
                           "outputs",
                           "MParser:NonScalarCellfunOutput");
        }
        const RuntimeObjectArrayPolicy policy =
            call.context->objectArrayPolicy
                ? *call.context->objectArrayPolicy
                : RuntimeObjectArrayPolicy{};
        auto concatenated = runtimeArrayOperationBuiltin(
            "horzcat", logical, policy);
        if (!concatenated.succeeded) {
            return failure(call,
                           "cellfun could not concatenate uniform outputs: " +
                               concatenated.error,
                           "MParser:NonUniformCellfunOutput");
        }
        auto reshaped = runtimeReshapeValue(
            concatenated.value, dimensions, policy);
        if (!reshaped.succeeded) {
            return failure(call,
                           "cellfun could not reshape uniform outputs: " +
                               reshaped.error,
                           "MParser:InvalidCellfunOutput");
        }
        outputs.push_back(std::move(reshaped.value));
    }
    return exactOutputs(call, std::move(outputs),
                        std::move(diagnostics));
}

} // namespace

bool isRuntimeCollectionLibraryBuiltin(std::string_view name) {
    return name == "cell2struct" || name == "cellfun" ||
           name == "iscell" || name == "sort" ||
           name == "struct2cell" || name == "unique";
}

BuiltinResult invokeRuntimeCollectionLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "iscell") {
        return exactOutputs(
            call, {makeRuntimeLogicalValue(
                      call.arguments.front().kind ==
                      RuntimeValueKind::Cell)});
    }
    if (name == "sort") {
        return sortBuiltin(call);
    }
    if (name == "unique") {
        return uniqueBuiltin(call);
    }
    if (name == "struct2cell") {
        return struct2cellBuiltin(call);
    }
    if (name == "cell2struct") {
        return cell2structBuiltin(call);
    }
    if (name == "cellfun") {
        return cellfunBuiltin(call);
    }
    return failure(call, "unknown collection library builtin",
                   "MParser:UnknownBuiltin");
}

} // namespace mparser
