#include "mparser/runtime/builtins/array/runtime_set_builtins.h"

#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

constexpr size_t kMaximumMaterializedSetElements = 10000000U;

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier) {
    return BuiltinResult::failure(call.span, std::move(message),
                                  std::move(identifier));
}

BuiltinResult exactOutputs(const BuiltinCall& call,
                           std::vector<RuntimeValue> outputs) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "set builtin produced an unexpected output count",
                       "MParser:SetContractViolation");
    }
    return BuiltinResult::success(std::move(outputs));
}

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

enum class SetDomain {
    Numeric,
    Character,
    String,
    CellText,
    Missing,
};

struct SetAtom {
    RuntimeNumericElementValue numeric;
    std::u16string text;
    bool missing = false;
};

struct SetKey {
    std::vector<SetAtom> atoms;
};

struct SetRecord {
    SetKey key;
    size_t source = 0;
};

struct SetInput {
    SetDomain domain = SetDomain::Numeric;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    std::vector<size_t> dimensions;
    size_t keyWidth = 1;
    std::vector<SetRecord> records;
};

struct SetOptions {
    bool rows = false;
    bool stable = false;
};

struct SelectedRecord {
    SetKey key;
    std::optional<size_t> sourceA;
    std::optional<size_t> sourceB;
};

bool executionCheckpoint(const BuiltinCall& call, size_t index) {
    return !call.context || !call.context->executionControl ||
           (index & 255U) != 0U ||
           call.context->executionControl->checkpoint();
}

bool numericMissing(const RuntimeNumericElementValue& value) {
    return runtimeNumericClassIsFloating(value.numericClass) &&
           (std::isnan(value.real) ||
            (value.complex && std::isnan(value.imaginary)));
}

bool numericEqual(const RuntimeNumericElementValue& left,
                  const RuntimeNumericElementValue& right) {
    const auto result = runtimeApplyNumericElementBinary(
        "==", left, right, RuntimeNumericClass::Logical);
    return result && result->real != 0.0;
}

std::optional<int> exactScalarCompare(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right) {
    const auto less = runtimeApplyNumericElementBinary(
        "<", left, right, RuntimeNumericClass::Logical);
    const auto greater = runtimeApplyNumericElementBinary(
        ">", left, right, RuntimeNumericClass::Logical);
    if (!less || !greater) {
        return std::nullopt;
    }
    return less->real != 0.0 ? -1 : greater->real != 0.0 ? 1 : 0;
}

RuntimeNumericElementValue numericComponent(
    const RuntimeNumericElementValue& value, bool imaginary) {
    RuntimeNumericElementValue component = value;
    component.real = imaginary
                         ? (value.complex ? value.imaginary : 0.0)
                         : value.real;
    component.integerRealBits = imaginary
                                    ? (value.complex
                                           ? value.integerImaginaryBits
                                           : 0)
                                    : value.integerRealBits;
    component.imaginary = 0.0;
    component.integerImaginaryBits = 0;
    component.complex = false;
    return component;
}

bool atomEqual(SetDomain domain, const SetAtom& left,
               const SetAtom& right) {
    if (domain == SetDomain::Numeric) {
        return numericEqual(left.numeric, right.numeric);
    }
    if (domain == SetDomain::Missing || left.missing || right.missing) {
        return false;
    }
    return left.text == right.text;
}

int atomCompare(SetDomain domain, const SetAtom& left,
                const SetAtom& right) {
    if (domain == SetDomain::Numeric) {
        const bool leftMissing = numericMissing(left.numeric);
        const bool rightMissing = numericMissing(right.numeric);
        if (leftMissing != rightMissing) {
            return leftMissing ? 1 : -1;
        }
        if (leftMissing) {
            return 0;
        }
        if (!left.numeric.complex && !right.numeric.complex) {
            const auto exact = exactScalarCompare(left.numeric,
                                                  right.numeric);
            if (exact) {
                return *exact;
            }
        }
        const int extrema = runtimeCompareNumericElementsForExtrema(
            left.numeric, right.numeric);
        if (extrema != 0 || numericEqual(left.numeric, right.numeric)) {
            return extrema;
        }
        const auto real = exactScalarCompare(
            numericComponent(left.numeric, false),
            numericComponent(right.numeric, false));
        if (real && *real != 0) {
            return *real;
        }
        const auto imaginary = exactScalarCompare(
            numericComponent(left.numeric, true),
            numericComponent(right.numeric, true));
        return imaginary.value_or(0);
    }
    if (domain == SetDomain::Missing) {
        return 0;
    }
    if (left.missing != right.missing) {
        return left.missing ? 1 : -1;
    }
    if (left.missing) {
        return 0;
    }
    return left.text < right.text ? -1
         : left.text > right.text ? 1
                                  : 0;
}

bool keyEqual(SetDomain domain, const SetKey& left,
              const SetKey& right) {
    return left.atoms.size() == right.atoms.size() &&
           std::equal(left.atoms.begin(), left.atoms.end(),
                      right.atoms.begin(),
                      [domain](const SetAtom& a, const SetAtom& b) {
                          return atomEqual(domain, a, b);
                      });
}

int keyCompare(SetDomain domain, const SetKey& left,
               const SetKey& right) {
    const size_t count = std::min(left.atoms.size(), right.atoms.size());
    for (size_t index = 0; index < count; ++index) {
        const int comparison = atomCompare(
            domain, left.atoms[index], right.atoms[index]);
        if (comparison != 0) {
            return comparison;
        }
    }
    return left.atoms.size() < right.atoms.size() ? -1
         : left.atoms.size() > right.atoms.size() ? 1
                                                  : 0;
}

bool isCellTextArray(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Cell) {
        return false;
    }
    for (size_t index = 0; index < runtimeShapeElementCount(value);
         ++index) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            value, index);
        if (!offset || *offset >= value.cells.size() ||
            !isRuntimeCharacterVector(value.cells[*offset])) {
            return false;
        }
    }
    return true;
}

std::optional<SetDomain> setDomain(const RuntimeValue& value) {
    if (isRuntimeNumericValue(value)) {
        return SetDomain::Numeric;
    }
    if (isRuntimeCharacterArray(value)) {
        return SetDomain::Character;
    }
    if (isRuntimeStringArray(value)) {
        return SetDomain::String;
    }
    if (isCellTextArray(value)) {
        return SetDomain::CellText;
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        return SetDomain::Missing;
    }
    return std::nullopt;
}

std::optional<SetAtom> inputAtom(const RuntimeValue& value,
                                 SetDomain domain,
                                 size_t logicalIndex) {
    SetAtom atom;
    if (domain == SetDomain::Numeric) {
        const auto element = runtimeNumericElementValue(value,
                                                        logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        atom.numeric = *element;
        return atom;
    }
    if (domain == SetDomain::Character) {
        const auto element = runtimeCharacterElement(value, logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        atom.text.assign(1, *element);
        return atom;
    }
    if (domain == SetDomain::String) {
        const auto* element = runtimeStringElement(value, logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        atom.text = element->value;
        atom.missing = element->missing;
        return atom;
    }
    if (domain == SetDomain::CellText) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            value, logicalIndex);
        if (!offset || *offset >= value.cells.size()) {
            return std::nullopt;
        }
        const auto text = runtimeTextScalarCodeUnits(value.cells[*offset]);
        if (!text) {
            return std::nullopt;
        }
        atom.text = *text;
        return atom;
    }
    atom.missing = true;
    return atom;
}

std::optional<SetInput> buildSetInput(const BuiltinCall& call,
                                      const RuntimeValue& value,
                                      bool rows,
                                      std::optional<RuntimeNumericClass>
                                          missingNumericClass,
                                      std::string& error,
                                      std::string& identifier) {
    const auto sourceDomain = setDomain(value);
    if (!sourceDomain) {
        error = "set inputs must be numeric, character, string, Cell text, "
                "or missing arrays";
        identifier = "MParser:InvalidSetInput";
        return std::nullopt;
    }
    const SetDomain domain =
        *sourceDomain == SetDomain::Missing && missingNumericClass
            ? SetDomain::Numeric
            : *sourceDomain;
    const auto dimensions = runtimeDimensions(value);
    if (rows && dimensions.size() != 2) {
        error = "set rows mode requires two-dimensional inputs";
        identifier = "MParser:InvalidSetShape";
        return std::nullopt;
    }
    if (rows && domain != SetDomain::Numeric &&
        domain != SetDomain::Character) {
        error = "set rows mode supports numeric and character arrays";
        identifier = "MParser:InvalidSetInput";
        return std::nullopt;
    }

    const size_t sourceCount = rows ? dimensions[0]
                                    : runtimeShapeElementCount(value);
    const size_t keyWidth = rows ? dimensions[1] : 1;
    if (*sourceDomain == SetDomain::Missing &&
        sourceCount > kMaximumMaterializedSetElements) {
        error = "shape-only missing set input is too large to materialize";
        identifier = "MParser:SetInputTooLarge";
        return std::nullopt;
    }
    SetInput input;
    input.domain = domain;
    input.numericClass = missingNumericClass.value_or(value.numericClass);
    input.dimensions = dimensions;
    input.keyWidth = keyWidth;
    input.records.reserve(sourceCount);
    for (size_t source = 0; source < sourceCount; ++source) {
        if (!executionCheckpoint(call, source)) {
            error = "set operation was stopped by runtime execution control";
            identifier = "MParser:ExecutionStopped";
            return std::nullopt;
        }
        SetKey key;
        key.atoms.reserve(keyWidth);
        for (size_t column = 0; column < keyWidth; ++column) {
            const size_t logical = rows
                                       ? source + dimensions[0] * column
                                       : source;
            std::optional<SetAtom> atom;
            if (*sourceDomain == SetDomain::Missing &&
                missingNumericClass) {
                SetAtom converted;
                converted.numeric.numericClass = *missingNumericClass;
                converted.numeric.real =
                    std::numeric_limits<double>::quiet_NaN();
                atom = std::move(converted);
            } else {
                atom = inputAtom(value, domain, logical);
            }
            if (!atom) {
                error = "set operation could not read an input element";
                identifier = "MParser:InvalidSetInput";
                return std::nullopt;
            }
            key.atoms.push_back(std::move(*atom));
        }
        input.records.push_back(SetRecord{std::move(key), source});
    }
    return input;
}

std::optional<RuntimeNumericClass> missingNumericClass(
    const RuntimeValue& value, const RuntimeValue& other) {
    return value.kind == RuntimeValueKind::MissingArray &&
                   isRuntimeNumericValue(other) &&
                   runtimeNumericClassIsFloating(other.numericClass)
               ? std::optional<RuntimeNumericClass>(other.numericClass)
               : std::nullopt;
}

bool compatibleInputs(const SetInput& left, const SetInput& right,
                      bool rows, std::string& error) {
    if (left.domain != right.domain) {
        if (left.domain != SetDomain::Numeric ||
            right.domain != SetDomain::Numeric) {
            error = "set inputs must use compatible data types";
            return false;
        }
    }
    if (rows && left.keyWidth != right.keyWidth) {
        error = "set rows inputs must have the same number of columns";
        return false;
    }
    return true;
}

std::optional<SetOptions> parseSetOptions(const BuiltinCall& call,
                                          bool membership,
                                          std::string& error) {
    SetOptions options;
    for (size_t cursor = 2; cursor < call.arguments.size(); ++cursor) {
        const auto raw = runtimeTextScalarUtf8(call.arguments[cursor]);
        if (!raw) {
            error = "set options must be text scalars";
            return std::nullopt;
        }
        const std::string option = asciiLower(*raw);
        if (option == "rows") {
            options.rows = true;
        } else if (!membership && option == "stable") {
            options.stable = true;
        } else if (!membership && option == "sorted") {
            options.stable = false;
        } else {
            error = "unsupported set option: " + *raw;
            return std::nullopt;
        }
    }
    return options;
}

std::vector<SetRecord> uniqueRecords(const SetInput& input) {
    std::vector<size_t> order(input.records.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&input](size_t left, size_t right) {
                         const int comparison = keyCompare(
                             input.domain, input.records[left].key,
                             input.records[right].key);
                         return comparison != 0
                                    ? comparison < 0
                                    : input.records[left].source <
                                          input.records[right].source;
                     });
    std::vector<SetRecord> unique;
    unique.reserve(order.size());
    for (const size_t index : order) {
        const SetRecord& candidate = input.records[index];
        if (unique.empty() ||
            !keyEqual(input.domain, unique.back().key, candidate.key)) {
            unique.push_back(candidate);
        }
    }
    std::stable_sort(unique.begin(), unique.end(),
                     [](const SetRecord& left, const SetRecord& right) {
                         return left.source < right.source;
                     });
    return unique;
}

std::vector<size_t> sortedRecordOrder(const SetInput& input) {
    std::vector<size_t> order(input.records.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&input](size_t left, size_t right) {
                         const int comparison = keyCompare(
                             input.domain, input.records[left].key,
                             input.records[right].key);
                         return comparison != 0
                                    ? comparison < 0
                                    : input.records[left].source <
                                          input.records[right].source;
                     });
    return order;
}

std::optional<size_t> findMatch(const SetInput& input,
                                const std::vector<size_t>& sorted,
                                const SetKey& key) {
    const auto begin = std::lower_bound(
        sorted.begin(), sorted.end(), key,
        [&input](size_t index, const SetKey& candidate) {
            return keyCompare(input.domain, input.records[index].key,
                              candidate) < 0;
        });
    std::optional<size_t> first;
    for (auto cursor = begin; cursor != sorted.end(); ++cursor) {
        const int comparison = keyCompare(
            input.domain, input.records[*cursor].key, key);
        if (comparison != 0) {
            break;
        }
        if (keyEqual(input.domain, input.records[*cursor].key, key) &&
            (!first || input.records[*cursor].source < *first)) {
            first = input.records[*cursor].source;
        }
    }
    return first;
}

std::optional<SetKey> convertNumericKey(
    const SetKey& key, RuntimeNumericClass numericClass) {
    SetKey converted;
    converted.atoms.reserve(key.atoms.size());
    for (const SetAtom& atom : key.atoms) {
        const auto value = runtimeConvertNumericElementValue(
            atom.numeric, numericClass);
        if (!value) {
            return std::nullopt;
        }
        SetAtom output;
        output.numeric = *value;
        converted.atoms.push_back(std::move(output));
    }
    return converted;
}

bool normalizeSelectedNumeric(std::vector<SelectedRecord>& selected,
                              RuntimeNumericClass numericClass,
                              std::string& error) {
    for (auto& record : selected) {
        if (!record.sourceB || record.sourceA) {
            continue;
        }
        auto converted = convertNumericKey(record.key, numericClass);
        if (!converted) {
            error = "set input cannot be converted to the first input's "
                    "numeric class";
            return false;
        }
        record.key = std::move(*converted);
    }
    return true;
}

void deduplicateSelected(SetDomain domain,
                         std::vector<SelectedRecord>& selected) {
    std::vector<size_t> order(selected.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [domain, &selected](size_t left, size_t right) {
                         const int comparison = keyCompare(
                             domain, selected[left].key,
                             selected[right].key);
                         return comparison != 0 ? comparison < 0
                                                : left < right;
                     });
    std::vector<bool> keep(selected.size(), true);
    std::optional<size_t> representative;
    for (const size_t index : order) {
        if (!representative ||
            !keyEqual(domain, selected[*representative].key,
                      selected[index].key)) {
            representative = index;
        } else {
            keep[index] = false;
        }
    }
    std::vector<SelectedRecord> unique;
    unique.reserve(selected.size());
    for (size_t index = 0; index < selected.size(); ++index) {
        if (keep[index]) {
            unique.push_back(std::move(selected[index]));
        }
    }
    selected = std::move(unique);
}

void sortSelected(SetDomain domain,
                  std::vector<SelectedRecord>& selected) {
    std::stable_sort(selected.begin(), selected.end(),
                     [domain](const SelectedRecord& left,
                              const SelectedRecord& right) {
                         return keyCompare(domain, left.key,
                                           right.key) < 0;
                     });
}

bool rowVector(const RuntimeValue& value) {
    const auto dimensions = runtimeDimensions(value);
    return dimensions.size() == 2 && dimensions[0] == 1;
}

std::vector<size_t> setOutputDimensions(
    std::string_view name, const RuntimeValue& left,
    const RuntimeValue& right, const SetOptions& options,
    size_t count, size_t width) {
    if (options.rows) {
        return {count, width};
    }
    const bool row = name == "setdiff" ? rowVector(left)
                                        : rowVector(left) &&
                                              rowVector(right);
    return row ? std::vector<size_t>{1, count}
               : std::vector<size_t>{count, 1};
}

template <typename Element>
std::optional<std::vector<Element>> logicalToStorage(
    const std::vector<size_t>& dimensions,
    std::vector<Element> logical) {
    std::vector<Element> storage(logical.size());
    for (size_t index = 0; index < logical.size(); ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(
                                      *coordinates, dimensions)
                                : std::nullopt;
        if (!offset || *offset >= storage.size()) {
            return std::nullopt;
        }
        storage[*offset] = std::move(logical[index]);
    }
    return storage;
}

std::optional<RuntimeValue> setOutputValue(
    const SetInput& left, const std::vector<size_t>& dimensions,
    const std::vector<SelectedRecord>& selected) {
    const size_t width = left.keyWidth;
    const size_t count = selected.size();
    if (left.domain == SetDomain::Missing) {
        return makeRuntimeMissingArrayValue(dimensions);
    }
    if (left.domain == SetDomain::Numeric) {
        std::vector<RuntimeNumericElementValue> elements;
        elements.reserve(count * width);
        if (width == 1) {
            for (const auto& record : selected) {
                elements.push_back(record.key.atoms.front().numeric);
            }
        } else {
            for (size_t column = 0; column < width; ++column) {
                for (const auto& record : selected) {
                    elements.push_back(record.key.atoms[column].numeric);
                }
            }
        }
        return runtimeNumericValueFromElements(
            dimensions, std::move(elements), left.numericClass);
    }
    if (left.domain == SetDomain::Character) {
        std::u16string logical;
        logical.reserve(count * width);
        if (width == 1) {
            for (const auto& record : selected) {
                logical.push_back(record.key.atoms.front().text.front());
            }
        } else {
            for (size_t column = 0; column < width; ++column) {
                for (const auto& record : selected) {
                    logical.push_back(record.key.atoms[column].text.front());
                }
            }
        }
        std::vector<char16_t> logicalVector(logical.begin(), logical.end());
        auto storage = logicalToStorage(dimensions,
                                        std::move(logicalVector));
        if (!storage) {
            return std::nullopt;
        }
        return makeRuntimeCharacterArray(
            dimensions, std::u16string(storage->begin(), storage->end()));
    }
    if (left.domain == SetDomain::String) {
        std::vector<RuntimeStringElement> logical;
        logical.reserve(count);
        for (const auto& record : selected) {
            const SetAtom& atom = record.key.atoms.front();
            logical.push_back(RuntimeStringElement{atom.text,
                                                   atom.missing});
        }
        auto storage = logicalToStorage(dimensions, std::move(logical));
        return storage ? std::optional<RuntimeValue>(
                             makeRuntimeStringArray(dimensions,
                                                    std::move(*storage)))
                       : std::nullopt;
    }

    std::vector<RuntimeValue> logical;
    logical.reserve(count);
    for (const auto& record : selected) {
        logical.push_back(makeRuntimeCharacterVector(
            record.key.atoms.front().text));
    }
    auto storage = logicalToStorage(dimensions, std::move(logical));
    return storage ? std::optional<RuntimeValue>(
                         makeRuntimeCellValue(dimensions,
                                              std::move(*storage)))
                   : std::nullopt;
}

std::optional<RuntimeValue> indexOutput(
    const std::vector<double>& indices) {
    return runtimeNumericValueFromLogicalOrder(
        {indices.size(), 1}, indices, RuntimeNumericClass::Double);
}

std::optional<std::vector<double>> sequentialIndices(
    const BuiltinCall& call, size_t count) {
    if (count > kMaximumMaterializedSetElements) {
        return std::nullopt;
    }
    std::vector<double> indices(count);
    for (size_t index = 0; index < count; ++index) {
        if (!executionCheckpoint(call, index)) {
            return std::nullopt;
        }
        indices[index] = static_cast<double>(index + 1);
    }
    return indices;
}

BuiltinResult missingSetBuiltin(std::string_view name,
                                const BuiltinCall& call,
                                const SetOptions& options) {
    if (options.rows) {
        return failure(call,
                       "set rows mode does not support missing arrays",
                       "MParser:InvalidSetInput");
    }
    if (!executionCheckpoint(call, 0)) {
        return failure(call,
                       "set operation was stopped by runtime execution "
                       "control",
                       "MParser:ExecutionStopped");
    }
    const size_t leftCount = runtimeShapeElementCount(call.arguments[0]);
    const size_t rightCount = runtimeShapeElementCount(call.arguments[1]);
    if (name == "ismember") {
        if (leftCount > kMaximumMaterializedSetElements) {
            return failure(call,
                           "ismember missing output is too large to "
                           "materialize",
                           "MParser:SetInputTooLarge");
        }
        const auto dimensions = runtimeDimensions(call.arguments[0]);
        auto flags = runtimeNumericValueFromLogicalOrder(
            dimensions, std::vector<double>(leftCount, 0.0),
            RuntimeNumericClass::Logical);
        if (!flags) {
            return failure(call, "ismember output shape is invalid",
                           "MParser:InvalidSetShape");
        }
        std::vector<RuntimeValue> outputs{std::move(*flags)};
        if (call.requestedOutputCount > 1) {
            auto locations = runtimeNumericValueFromLogicalOrder(
                dimensions, std::vector<double>(leftCount, 0.0),
                RuntimeNumericClass::Double);
            if (!locations) {
                return failure(call,
                               "ismember location shape is invalid",
                               "MParser:InvalidSetShape");
            }
            outputs.push_back(std::move(*locations));
        }
        return exactOutputs(call, std::move(outputs));
    }

    size_t outputCount = 0;
    if (name == "setdiff") {
        outputCount = leftCount;
    } else if (name != "intersect") {
        if (leftCount >
            std::numeric_limits<size_t>::max() - rightCount) {
            return failure(call, "set output dimensions are too large",
                           "MParser:InvalidSetShape");
        }
        outputCount = leftCount + rightCount;
    }
    const auto dimensions = setOutputDimensions(
        name, call.arguments[0], call.arguments[1], options,
        outputCount, 1);
    std::vector<RuntimeValue> outputs{
        makeRuntimeMissingArrayValue(dimensions)};
    if (call.requestedOutputCount > 1) {
        const size_t count = name == "intersect" ? 0 : leftCount;
        auto indices = sequentialIndices(call, count);
        if (!indices) {
            return failure(call,
                           "set index output is too large to materialize or "
                           "execution was stopped",
                           call.context && call.context->executionControl &&
                                   call.context->executionControl
                                           ->stopReason() !=
                                       RuntimeExecutionStopReason::None
                               ? "MParser:ExecutionStopped"
                               : "MParser:SetInputTooLarge");
        }
        auto value = indexOutput(*indices);
        if (!value) {
            return failure(call, "set first index output is invalid",
                           "MParser:InvalidSetShape");
        }
        outputs.push_back(std::move(*value));
    }
    if (call.requestedOutputCount > 2) {
        const size_t count = name == "intersect" ? 0 : rightCount;
        auto indices = sequentialIndices(call, count);
        if (!indices) {
            return failure(call,
                           "set index output is too large to materialize or "
                           "execution was stopped",
                           call.context && call.context->executionControl &&
                                   call.context->executionControl
                                           ->stopReason() !=
                                       RuntimeExecutionStopReason::None
                               ? "MParser:ExecutionStopped"
                               : "MParser:SetInputTooLarge");
        }
        auto value = indexOutput(*indices);
        if (!value) {
            return failure(call, "set second index output is invalid",
                           "MParser:InvalidSetShape");
        }
        outputs.push_back(std::move(*value));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult ismemberBuiltin(const BuiltinCall& call,
                              const SetOptions& options) {
    std::string error;
    std::string identifier;
    const auto leftMissingClass = missingNumericClass(
        call.arguments[0], call.arguments[1]);
    const auto rightMissingClass = missingNumericClass(
        call.arguments[1], call.arguments[0]);
    auto left = buildSetInput(call, call.arguments[0], options.rows,
                              leftMissingClass, error, identifier);
    if (!left) {
        return failure(call, std::move(error), std::move(identifier));
    }
    auto right = buildSetInput(call, call.arguments[1], options.rows,
                               rightMissingClass, error, identifier);
    if (!right) {
        return failure(call, std::move(error), std::move(identifier));
    }
    if (!compatibleInputs(*left, *right, options.rows, error)) {
        return failure(call, std::move(error),
                       "MParser:IncompatibleSetInputs");
    }
    const auto sortedRight = sortedRecordOrder(*right);
    std::vector<double> membership(left->records.size(), 0.0);
    std::vector<double> locations(left->records.size(), 0.0);
    for (size_t index = 0; index < left->records.size(); ++index) {
        if (!executionCheckpoint(call, index)) {
            return failure(call,
                           "ismember was stopped by runtime execution "
                           "control",
                           "MParser:ExecutionStopped");
        }
        const auto match = findMatch(
            *right, sortedRight, left->records[index].key);
        if (match) {
            membership[index] = 1.0;
            locations[index] = static_cast<double>(*match + 1);
        }
    }
    const auto outputDimensions = options.rows
                                      ? std::vector<size_t>{
                                            left->records.size(), 1}
                                      : left->dimensions;
    auto flags = runtimeNumericValueFromLogicalOrder(
        outputDimensions, std::move(membership),
        RuntimeNumericClass::Logical);
    if (!flags) {
        return failure(call, "ismember output shape is invalid",
                       "MParser:InvalidSetShape");
    }
    std::vector<RuntimeValue> outputs{std::move(*flags)};
    if (call.requestedOutputCount > 1) {
        auto locationValue = runtimeNumericValueFromLogicalOrder(
            outputDimensions, std::move(locations),
            RuntimeNumericClass::Double);
        if (!locationValue) {
            return failure(call, "ismember location shape is invalid",
                           "MParser:InvalidSetShape");
        }
        outputs.push_back(std::move(*locationValue));
    }
    return exactOutputs(call, std::move(outputs));
}

BuiltinResult setOperationBuiltin(std::string_view name,
                                  const BuiltinCall& call,
                                  const SetOptions& options) {
    std::string error;
    std::string identifier;
    const auto leftMissingClass = missingNumericClass(
        call.arguments[0], call.arguments[1]);
    const auto rightMissingClass = missingNumericClass(
        call.arguments[1], call.arguments[0]);
    auto left = buildSetInput(call, call.arguments[0], options.rows,
                              leftMissingClass, error, identifier);
    if (!left) {
        return failure(call, std::move(error), std::move(identifier));
    }
    auto right = buildSetInput(call, call.arguments[1], options.rows,
                               rightMissingClass, error, identifier);
    if (!right) {
        return failure(call, std::move(error), std::move(identifier));
    }
    if (!compatibleInputs(*left, *right, options.rows, error)) {
        return failure(call, std::move(error),
                       "MParser:IncompatibleSetInputs");
    }

    const auto uniqueLeft = uniqueRecords(*left);
    const auto uniqueRight = uniqueRecords(*right);
    const auto sortedLeft = sortedRecordOrder(*left);
    const auto sortedRight = sortedRecordOrder(*right);
    std::vector<SelectedRecord> selected;
    selected.reserve(uniqueLeft.size() + uniqueRight.size());

    if (name == "union") {
        for (const auto& record : uniqueLeft) {
            selected.push_back(
                SelectedRecord{record.key, record.source, std::nullopt});
        }
        for (const auto& record : uniqueRight) {
            if (!findMatch(*left, sortedLeft, record.key)) {
                selected.push_back(SelectedRecord{
                    record.key, std::nullopt, record.source});
            }
        }
    } else if (name == "intersect") {
        for (const auto& record : uniqueLeft) {
            const auto match = findMatch(*right, sortedRight,
                                         record.key);
            if (match) {
                selected.push_back(SelectedRecord{
                    record.key, record.source, *match});
            }
        }
    } else if (name == "setdiff") {
        for (const auto& record : uniqueLeft) {
            if (!findMatch(*right, sortedRight, record.key)) {
                selected.push_back(SelectedRecord{
                    record.key, record.source, std::nullopt});
            }
        }
    } else {
        for (const auto& record : uniqueLeft) {
            if (!findMatch(*right, sortedRight, record.key)) {
                selected.push_back(SelectedRecord{
                    record.key, record.source, std::nullopt});
            }
        }
        for (const auto& record : uniqueRight) {
            if (!findMatch(*left, sortedLeft, record.key)) {
                selected.push_back(SelectedRecord{
                    record.key, std::nullopt, record.source});
            }
        }
    }

    if (left->domain == SetDomain::Numeric &&
        !normalizeSelectedNumeric(selected, left->numericClass, error)) {
        return failure(call, std::move(error),
                       "MParser:IncompatibleSetInputs");
    }
    deduplicateSelected(left->domain, selected);
    if (!options.stable) {
        sortSelected(left->domain, selected);
    }

    const auto dimensions = setOutputDimensions(
        name, call.arguments[0], call.arguments[1], options,
        selected.size(), left->keyWidth);
    auto values = setOutputValue(*left, dimensions, selected);
    if (!values) {
        return failure(call, "set output could not be constructed",
                       "MParser:InvalidSetShape");
    }
    std::vector<RuntimeValue> outputs{std::move(*values)};
    if (call.requestedOutputCount > 1) {
        std::vector<double> indices;
        indices.reserve(selected.size());
        for (const auto& record : selected) {
            if (record.sourceA) {
                indices.push_back(
                    static_cast<double>(*record.sourceA + 1));
            }
        }
        auto value = indexOutput(indices);
        if (!value) {
            return failure(call, "set first index output is invalid",
                           "MParser:InvalidSetShape");
        }
        outputs.push_back(std::move(*value));
    }
    if (call.requestedOutputCount > 2) {
        std::vector<double> indices;
        indices.reserve(selected.size());
        for (const auto& record : selected) {
            if (record.sourceB) {
                indices.push_back(
                    static_cast<double>(*record.sourceB + 1));
            }
        }
        auto value = indexOutput(indices);
        if (!value) {
            return failure(call, "set second index output is invalid",
                           "MParser:InvalidSetShape");
        }
        outputs.push_back(std::move(*value));
    }
    return exactOutputs(call, std::move(outputs));
}

} // namespace

bool isRuntimeSetLibraryBuiltin(std::string_view name) {
    return name == "intersect" || name == "ismember" ||
           name == "setdiff" || name == "setxor" ||
           name == "union";
}

BuiltinResult invokeRuntimeSetLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    std::string error;
    const auto options = parseSetOptions(call, name == "ismember", error);
    if (!options) {
        return failure(call, std::move(error),
                       "MParser:InvalidSetOption");
    }
    if (call.arguments[0].kind == RuntimeValueKind::MissingArray &&
        call.arguments[1].kind == RuntimeValueKind::MissingArray) {
        return missingSetBuiltin(name, call, *options);
    }
    if (name == "ismember") {
        return ismemberBuiltin(call, *options);
    }
    if (name == "union" || name == "intersect" ||
        name == "setdiff" || name == "setxor") {
        return setOperationBuiltin(name, call, *options);
    }
    return failure(call, "unsupported set builtin",
                   "MParser:UnsupportedSetBuiltin");
}

} // namespace mparser
