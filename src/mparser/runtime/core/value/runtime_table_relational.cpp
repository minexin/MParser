#include "mparser/runtime/core/value/runtime_table_relational.h"

#include "mparser/runtime/core/session/runtime_execution_control.h"
#include "mparser/runtime/core/value/runtime_array.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mparser {
namespace {

enum class KeyAtomKind : std::uint8_t {
    Boundary,
    Missing,
    Numeric,
    Text,
    Categorical,
    Temporal,
};

struct KeyAtom {
    KeyAtomKind kind = KeyAtomKind::Boundary;
    std::uint64_t tag = 0;
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::u16string text;

    bool operator==(const KeyAtom&) const = default;
};

struct RowKey {
    std::vector<KeyAtom> atoms;
    bool matchable = true;

    bool operator==(const RowKey& other) const {
        return atoms == other.atoms;
    }
};

enum class NumericSpecial : std::uint8_t {
    Finite,
    PositiveInfinity,
    NegativeInfinity,
};

struct CanonicalNumericPart {
    NumericSpecial special = NumericSpecial::Finite;
    bool negative = false;
    std::uint64_t significand = 0;
    std::int64_t exponent = 0;
};

constexpr std::uint64_t kNumericImaginaryComponent = 1U;
constexpr std::uint64_t kNumericSpecialShift = 2U;
constexpr std::uint64_t kNumericSignBit = 1U << 8U;

void combineHash(size_t& seed, size_t value) {
    seed ^= value + static_cast<size_t>(0x9e3779b9U) +
            (seed << 6U) + (seed >> 2U);
}

struct RowKeyHash {
    size_t operator()(const RowKey& key) const noexcept {
        size_t result = 0;
        for (const KeyAtom& atom : key.atoms) {
            combineHash(result, static_cast<size_t>(atom.kind));
            combineHash(result, std::hash<std::uint64_t>{}(atom.tag));
            combineHash(result, std::hash<std::uint64_t>{}(atom.first));
            combineHash(result, std::hash<std::uint64_t>{}(atom.second));
            combineHash(result, std::hash<std::u16string>{}(atom.text));
        }
        return result;
    }
};

bool executionCheckpoint(RuntimeExecutionControl* control,
                         size_t& operation) {
    const bool shouldCheck = (operation++ & 255U) == 0U;
    return !control || !shouldCheck || control->checkpoint();
}

void appendMissing(RowKey& key, std::uint64_t tag) {
    key.atoms.push_back(
        KeyAtom{KeyAtomKind::Missing, tag, 0, 0, {}});
    key.matchable = false;
}

CanonicalNumericPart canonicalNumericPart(
    const RuntimeNumericElementValue& element, bool imaginary) {
    if (imaginary && !element.complex) {
        return {};
    }
    if (runtimeNumericClassIsInteger(element.numericClass)) {
        const std::uint64_t bits = imaginary
                                       ? element.integerImaginaryBits
                                       : element.integerRealBits;
        const bool negative =
            runtimeNumericClassIsSignedInteger(element.numericClass) &&
            std::bit_cast<std::int64_t>(bits) < 0;
        const std::uint64_t magnitude = negative ? 0U - bits : bits;
        CanonicalNumericPart result;
        result.negative = negative && magnitude != 0;
        result.significand = magnitude;
        while (result.significand != 0 &&
               (result.significand & 1U) == 0) {
            result.significand >>= 1U;
            ++result.exponent;
        }
        return result;
    }

    const double value = imaginary ? element.imaginary : element.real;
    if (std::isinf(value)) {
        return CanonicalNumericPart{
            value < 0.0 ? NumericSpecial::NegativeInfinity
                        : NumericSpecial::PositiveInfinity};
    }

    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t fraction = bits & ((std::uint64_t{1} << 52U) - 1U);
    const std::uint64_t exponentBits =
        (bits >> 52U) & ((std::uint64_t{1} << 11U) - 1U);
    CanonicalNumericPart result;
    result.negative = ((bits >> 63U) != 0U);
    if (exponentBits == 0U) {
        result.significand = fraction;
        result.exponent = -1074;
    } else {
        result.significand = (std::uint64_t{1} << 52U) | fraction;
        result.exponent = static_cast<std::int64_t>(exponentBits) -
                          1023 - 52;
    }
    if (result.significand == 0) {
        result.negative = false;
        result.exponent = 0;
    } else {
        while ((result.significand & 1U) == 0U) {
            result.significand >>= 1U;
            ++result.exponent;
        }
    }
    return result;
}

bool numericElementHasNaN(
    const RuntimeNumericElementValue& element) {
    return !runtimeNumericClassIsInteger(element.numericClass) &&
           (std::isnan(element.real) ||
            (element.complex && std::isnan(element.imaginary)));
}

void appendCanonicalNumericPart(RowKey& key,
                                const CanonicalNumericPart& part,
                                bool imaginary) {
    KeyAtom atom;
    atom.kind = KeyAtomKind::Numeric;
    atom.tag = imaginary ? kNumericImaginaryComponent : 0U;
    if (part.special != NumericSpecial::Finite) {
        atom.tag |= static_cast<std::uint64_t>(part.special)
                    << kNumericSpecialShift;
    }
    if (part.negative) {
        atom.tag |= kNumericSignBit;
    }
    atom.first = part.significand;
    atom.second = std::bit_cast<std::uint64_t>(part.exponent);
    key.atoms.push_back(std::move(atom));
}

bool appendNumericElementKey(const RuntimeNumericElementValue& element,
                             RowKey& key) {
    if (numericElementHasNaN(element)) {
        appendMissing(key, 0);
        return true;
    }
    appendCanonicalNumericPart(
        key, canonicalNumericPart(element, false), false);
    appendCanonicalNumericPart(
        key, canonicalNumericPart(element, true), true);
    return true;
}

bool appendScalarCellKey(const RuntimeValue& value, RowKey& key,
                         std::string& error) {
    if (value.kind == RuntimeValueKind::Missing) {
        appendMissing(key, 2);
        return true;
    }
    if (runtimeShapeElementCount(value) != 1) {
        error = "Cell join/group keys must contain scalar values";
        return false;
    }
    if (isRuntimeNumericValue(value)) {
        const auto element = runtimeNumericElementValue(value, 0);
        if (!element) {
            error = "numeric Cell key could not be read";
            return false;
        }
        return appendNumericElementKey(*element, key);
    }
    if (isRuntimeStringScalar(value)) {
        const RuntimeStringElement* element = runtimeStringElement(value, 0);
        if (!element) {
            error = "string Cell key could not be read";
            return false;
        }
        if (element->missing) {
            appendMissing(key, 1);
        } else {
            key.atoms.push_back(
                KeyAtom{KeyAtomKind::Text, 0, 0, 0, element->value});
        }
        return true;
    }
    if (isRuntimeCharacterVector(value)) {
        key.atoms.push_back(KeyAtom{
            KeyAtomKind::Text, 0, 0, 0, value.characterElements});
        return true;
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        appendMissing(key, 2);
        return true;
    }
    error =
        "Cell join/group keys support numeric, text, and missing scalars";
    return false;
}

bool appendVariableKey(const RuntimeValue& value, size_t row,
                       size_t variableOrdinal, RowKey& key,
                       std::string& error) {
    const size_t rows = runtimeDimension(value, 0);
    if (row >= rows) {
        error = "join/group key row is out of bounds";
        return false;
    }
    const size_t count = runtimeShapeElementCount(value);
    const size_t trailingCount = rows == 0 ? 0 : count / rows;
    key.atoms.push_back(KeyAtom{
        KeyAtomKind::Boundary, 0,
        static_cast<std::uint64_t>(variableOrdinal), 0, {}});
    for (size_t trailing = 0; trailing < trailingCount; ++trailing) {
        const size_t logicalIndex = row + rows * trailing;
        key.atoms.push_back(KeyAtom{
            KeyAtomKind::Boundary, 1,
            static_cast<std::uint64_t>(trailing), 0, {}});
        if (isRuntimeNumericValue(value)) {
            const auto element =
                runtimeNumericElementValue(value, logicalIndex);
            if (!element) {
                error = "numeric join/group key could not be read";
                return false;
            }
            appendNumericElementKey(*element, key);
            continue;
        }
        if (isRuntimeCategoricalValue(value)) {
            const std::uint32_t code =
                runtimeCategoricalCode(value, logicalIndex);
            if (code == kRuntimeCategoricalUndefinedCode) {
                appendMissing(key, 3);
            } else {
                key.atoms.push_back(KeyAtom{
                    KeyAtomKind::Categorical, 0, 0, 0,
                    runtimeUtf8ToUtf16(std::string(
                        runtimeCategoricalLabel(value, logicalIndex)))});
            }
            continue;
        }
        if (isRuntimeTemporalValue(value)) {
            const auto payload =
                runtimeTemporalPayload(value, logicalIndex);
            const auto kind = runtimeTemporalKind(value);
            if (!payload || !kind) {
                error = "temporal join/group key could not be read";
                return false;
            }
            const std::uint64_t tag =
                static_cast<std::uint64_t>(*kind);
            if (std::isnan(*payload)) {
                appendMissing(key, 4 + tag);
            } else {
                key.atoms.push_back(KeyAtom{
                    KeyAtomKind::Temporal, tag,
                    std::bit_cast<std::uint64_t>(*payload), 0, {}});
            }
            continue;
        }
        if (isRuntimeStringArray(value)) {
            const RuntimeStringElement* element =
                runtimeStringElement(value, logicalIndex);
            if (!element) {
                error = "string join/group key could not be read";
                return false;
            }
            if (element->missing) {
                appendMissing(key, 6);
            } else {
                key.atoms.push_back(KeyAtom{
                    KeyAtomKind::Text, 0, 0, 0, element->value});
            }
            continue;
        }
        if (isRuntimeCharacterArray(value)) {
            const auto element =
                runtimeCharacterElement(value, logicalIndex);
            if (!element) {
                error = "character join/group key could not be read";
                return false;
            }
            key.atoms.push_back(KeyAtom{
                KeyAtomKind::Text, 0, 0, 0,
                std::u16string(1, *element)});
            continue;
        }
        if (value.kind == RuntimeValueKind::Cell) {
            const auto offset =
                runtimeColumnMajorLinearToStorageOffset(value,
                                                        logicalIndex);
            if (!offset || *offset >= value.cells.size() ||
                !appendScalarCellKey(value.cells[*offset], key, error)) {
                if (error.empty()) {
                    error = "Cell join/group key could not be read";
                }
                return false;
            }
            continue;
        }
        if (value.kind == RuntimeValueKind::MissingArray) {
            appendMissing(key, 7);
            continue;
        }
        error =
            "join/group keys support numeric, text, Cell text/numeric, categorical, and temporal variables";
        return false;
    }
    return true;
}

std::optional<RowKey> makeRowKey(
    const RuntimeTableStorage& storage,
    const std::vector<size_t>& variableIndices, size_t row,
    std::string& error) {
    RowKey key;
    for (size_t ordinal = 0; ordinal < variableIndices.size(); ++ordinal) {
        const size_t index = variableIndices[ordinal];
        if (index >= storage.variables.size() ||
            !appendVariableKey(storage.variables[index].value, row,
                               ordinal, key, error)) {
            if (error.empty()) {
                error = "join/group key variable index is invalid";
            }
            return std::nullopt;
        }
    }
    return key;
}

struct IndexResult {
    bool succeeded = false;
    std::vector<size_t> indices;
    std::string error;
};

IndexResult resolveVariableIndices(
    const RuntimeTableStorage& storage,
    const std::vector<std::string>& names, std::string_view role,
    bool emptyMeansAll) {
    if (names.empty() && emptyMeansAll) {
        std::vector<size_t> indices(storage.variables.size());
        for (size_t index = 0; index < indices.size(); ++index) {
            indices[index] = index;
        }
        return IndexResult{true, std::move(indices), {}};
    }
    if (names.empty()) {
        return IndexResult{true, {}, {}};
    }
    std::set<size_t> used;
    std::vector<size_t> indices;
    indices.reserve(names.size());
    for (const std::string& name : names) {
        const auto found = std::find_if(
            storage.variables.begin(), storage.variables.end(),
            [&](const RuntimeTableVariable& variable) {
                return variable.name == name;
            });
        if (found == storage.variables.end()) {
            return IndexResult{
                false, {}, std::string(role) +
                               " is not available: " + name};
        }
        const size_t index = static_cast<size_t>(
            std::distance(storage.variables.begin(), found));
        if (!used.insert(index).second) {
            return IndexResult{
                false, {}, std::string(role) +
                               " contains a duplicate: " + name};
        }
        indices.push_back(index);
    }
    return IndexResult{true, std::move(indices), {}};
}

bool keyVariablesCompatible(const RuntimeValue& left,
                            const RuntimeValue& right) {
    auto leftDimensions = runtimeDimensions(left);
    auto rightDimensions = runtimeDimensions(right);
    if (leftDimensions.size() != rightDimensions.size()) {
        return false;
    }
    if (!leftDimensions.empty()) {
        leftDimensions.front() = 1;
        rightDimensions.front() = 1;
    }
    if (leftDimensions != rightDimensions) {
        return false;
    }
    if (isRuntimeNumericValue(left) && isRuntimeNumericValue(right)) {
        return true;
    }
    if (isRuntimeCategoricalValue(left) &&
        isRuntimeCategoricalValue(right)) {
        return true;
    }
    if (isRuntimeTemporalValue(left) && isRuntimeTemporalValue(right)) {
        return runtimeTemporalKind(left) == runtimeTemporalKind(right);
    }
    if ((isRuntimeStringArray(left) || isRuntimeCharacterArray(left)) &&
        (isRuntimeStringArray(right) ||
         isRuntimeCharacterArray(right))) {
        return true;
    }
    if (left.kind == RuntimeValueKind::Cell &&
        right.kind == RuntimeValueKind::Cell) {
        return true;
    }
    return left.kind == RuntimeValueKind::MissingArray &&
           right.kind == RuntimeValueKind::MissingArray;
}

RuntimeTableOperationResult makeMissingRowsLike(
    const RuntimeValue& exemplar, size_t count) {
    std::vector<size_t> dimensions = runtimeDimensions(exemplar);
    if (dimensions.empty()) {
        dimensions = {1, 1};
    }
    dimensions.front() = count;
    std::vector<size_t> trailingDimensions(dimensions.begin() + 1,
                                           dimensions.end());
    const auto trailingCount =
        checkedRuntimeDimensionProduct(trailingDimensions);
    if (!trailingCount ||
        (count != 0 && *trailingCount >
                           std::numeric_limits<size_t>::max() / count)) {
        return RuntimeTableOperationResult{
            false, {}, "outer join missing-row shape is too large"};
    }
    const size_t elementCount = count * *trailingCount;
    if (isRuntimeNumericValue(exemplar)) {
        std::vector<RuntimeNumericElementValue> elements(elementCount);
        for (auto& element : elements) {
            element.numericClass = exemplar.numericClass;
            element.complex = exemplar.numericComplex;
            if (runtimeNumericClassIsFloating(exemplar.numericClass)) {
                element.real = std::numeric_limits<double>::quiet_NaN();
            }
        }
        auto value = runtimeNumericValueFromElements(
            std::move(dimensions), std::move(elements),
            exemplar.numericClass);
        return value
                   ? RuntimeTableOperationResult{true, std::move(*value), {}}
                   : RuntimeTableOperationResult{
                         false, {},
                         "outer join numeric missing rows are invalid"};
    }
    if (isRuntimeStringArray(exemplar)) {
        std::vector<RuntimeStringElement> elements(
            elementCount, RuntimeStringElement{{}, true});
        return RuntimeTableOperationResult{
            true,
            makeRuntimeStringArray(std::move(dimensions),
                                   std::move(elements)),
            {}};
    }
    if (isRuntimeCharacterArray(exemplar)) {
        return RuntimeTableOperationResult{
            true,
            makeRuntimeCharacterArray(
                std::move(dimensions),
                std::u16string(elementCount, u' ')),
            {}};
    }
    if (exemplar.kind == RuntimeValueKind::Cell) {
        return RuntimeTableOperationResult{
            true,
            makeRuntimeCellValue(
                std::move(dimensions),
                std::vector<RuntimeValue>(elementCount,
                                          makeRuntimeMissingValue())),
            {}};
    }
    if (exemplar.kind == RuntimeValueKind::MissingArray) {
        return RuntimeTableOperationResult{
            true, makeRuntimeMissingArrayValue(std::move(dimensions)), {}};
    }
    if (isRuntimeCategoricalValue(exemplar)) {
        const RuntimeCategoricalStorage* storage =
            runtimeCategoricalStorage(exemplar);
        if (!storage) {
            return RuntimeTableOperationResult{
                false, {}, "outer join categorical storage is invalid"};
        }
        auto value = runtimeMakeCategoricalValue(
            std::move(dimensions), storage->categories,
            std::vector<std::uint32_t>(
                elementCount, kRuntimeCategoricalUndefinedCode),
            storage->ordinal, storage->protectedCategories);
        return RuntimeTableOperationResult{
            value.succeeded, std::move(value.value),
            std::move(value.error)};
    }
    if (isRuntimeTemporalValue(exemplar)) {
        const auto kind = runtimeTemporalKind(exemplar);
        if (!kind) {
            return RuntimeTableOperationResult{
                false, {}, "outer join temporal kind is invalid"};
        }
        auto value = runtimeMakeTemporalValue(
            *kind, std::move(dimensions),
            std::vector<double>(
                elementCount,
                std::numeric_limits<double>::quiet_NaN()));
        return RuntimeTableOperationResult{
            value.succeeded, std::move(value.value),
            std::move(value.error)};
    }
    return RuntimeTableOperationResult{
        false, {},
        "outer join cannot synthesize missing rows for variable class " +
            exemplar.className};
}

RuntimeTableOperationResult mapRows(
    const RuntimeValue& source,
    const std::vector<size_t>& rows,
    RuntimeExecutionControl* executionControl) {
    if (rows.empty()) {
        return runtimeSelectTableVariableRows(source, {});
    }
    std::vector<RuntimeValue> chunks;
    chunks.reserve(rows.size());
    size_t operation = 0;
    for (const size_t row : rows) {
        if (!executionCheckpoint(executionControl, operation)) {
            return RuntimeTableOperationResult{
                false, {},
                "join row mapping was stopped by runtime execution control"};
        }
        RuntimeTableOperationResult chunk =
            row == kRuntimeTableUnmatchedRow
                ? makeMissingRowsLike(source, 1)
                : runtimeSelectTableVariableRows(source, {row});
        if (!chunk.succeeded) {
            return chunk;
        }
        chunks.push_back(std::move(chunk.value));
    }
    auto combined = runtimeConcatenateValues(1, chunks);
    return RuntimeTableOperationResult{
        combined.succeeded, std::move(combined.value),
        std::move(combined.error)};
}

RuntimeTableOperationResult coerceMergedRightRow(
    const RuntimeValue& leftExemplar, RuntimeValue rightRow) {
    if (isRuntimeNumericValue(leftExemplar) &&
        isRuntimeNumericValue(rightRow)) {
        auto converted = runtimeConvertNumericClass(
            std::move(rightRow), leftExemplar.numericClass);
        return converted
                   ? RuntimeTableOperationResult{
                         true, std::move(*converted), {}}
                   : RuntimeTableOperationResult{
                         false, {},
                         "merged outer-join key cannot be represented by "
                         "the left numeric class"};
    }
    if (isRuntimeStringArray(leftExemplar) &&
        isRuntimeCharacterArray(rightRow)) {
        auto converted = runtimeConvertToString(rightRow);
        return RuntimeTableOperationResult{
            converted.succeeded, std::move(converted.value),
            std::move(converted.error)};
    }
    if (isRuntimeCharacterArray(leftExemplar) &&
        isRuntimeStringArray(rightRow)) {
        auto converted = runtimeConvertToCharacter(rightRow);
        return RuntimeTableOperationResult{
            converted.succeeded, std::move(converted.value),
            std::move(converted.error)};
    }
    return RuntimeTableOperationResult{
        true, std::move(rightRow), {}};
}

RuntimeTableOperationResult mapMergedRows(
    const RuntimeValue& left, const RuntimeValue& right,
    const std::vector<size_t>& leftRows,
    const std::vector<size_t>& rightRows,
    RuntimeExecutionControl* executionControl) {
    if (leftRows.size() != rightRows.size()) {
        return RuntimeTableOperationResult{
            false, {}, "merged join row maps have different lengths"};
    }
    if (leftRows.empty()) {
        return runtimeSelectTableVariableRows(left, {});
    }
    std::vector<RuntimeValue> chunks;
    chunks.reserve(leftRows.size());
    size_t operation = 0;
    for (size_t index = 0; index < leftRows.size(); ++index) {
        if (!executionCheckpoint(executionControl, operation)) {
            return RuntimeTableOperationResult{
                false, {},
                "merged join row mapping was stopped by runtime execution control"};
        }
        const bool useLeft =
            leftRows[index] != kRuntimeTableUnmatchedRow;
        auto selected = runtimeSelectTableVariableRows(
            useLeft ? left : right,
            {useLeft ? leftRows[index] : rightRows[index]});
        if (!selected.succeeded) {
            return selected;
        }
        if (!useLeft) {
            selected = coerceMergedRightRow(
                left, std::move(selected.value));
            if (!selected.succeeded) {
                return selected;
            }
        }
        chunks.push_back(std::move(selected.value));
    }
    auto combined = runtimeConcatenateValues(1, chunks);
    return RuntimeTableOperationResult{
        combined.succeeded, std::move(combined.value),
        std::move(combined.error)};
}

bool includesLeft(RuntimeTableJoinType type) {
    return type == RuntimeTableJoinType::Left ||
           type == RuntimeTableJoinType::Full;
}

bool includesRight(RuntimeTableJoinType type) {
    return type == RuntimeTableJoinType::Right ||
           type == RuntimeTableJoinType::Full;
}

std::string uniqueOutputName(std::string base,
                             std::set<std::string>& used) {
    if (used.insert(base).second) {
        return base;
    }
    const std::string stem = std::move(base);
    size_t suffix = 1;
    do {
        base = stem + "_" + std::to_string(suffix++);
    } while (!used.insert(base).second);
    return base;
}

enum class OutputSide {
    Merged,
    Left,
    Right,
};

struct OutputVariable {
    OutputSide side = OutputSide::Left;
    size_t leftIndex = 0;
    size_t rightIndex = 0;
    std::string name;
};

void resolveOutputNames(std::vector<OutputVariable>& variables) {
    std::unordered_map<std::string, size_t> counts;
    for (const auto& variable : variables) {
        ++counts[variable.name];
    }
    for (auto& variable : variables) {
        if (counts[variable.name] <= 1) {
            continue;
        }
        if (variable.side == OutputSide::Left) {
            variable.name += "_L";
        } else if (variable.side == OutputSide::Right) {
            variable.name += "_R";
        }
    }
    std::set<std::string> used;
    for (auto& variable : variables) {
        variable.name = uniqueOutputName(std::move(variable.name), used);
    }
}

RuntimeTableJoinResult joinFailure(std::string error) {
    return RuntimeTableJoinResult{
        false, {}, {}, {}, std::move(error)};
}

} // namespace

RuntimeTableJoinResult runtimeJoinTables(
    const RuntimeValue& left, const RuntimeValue& right,
    const RuntimeTableJoinOptions& options) {
    const RuntimeTableStorage* leftStorage = runtimeTableStorage(left);
    const RuntimeTableStorage* rightStorage = runtimeTableStorage(right);
    if (!leftStorage || !rightStorage) {
        return joinFailure("joins currently require two table inputs");
    }
    if (options.leftKeys.size() != options.rightKeys.size() ||
        options.leftKeys.empty()) {
        return joinFailure(
            "join key lists must be nonempty and have equal lengths");
    }
    const auto leftKeys = resolveVariableIndices(
        *leftStorage, options.leftKeys, "left join key", false);
    const auto rightKeys = resolveVariableIndices(
        *rightStorage, options.rightKeys, "right join key", false);
    const auto leftVariables = resolveVariableIndices(
        *leftStorage, options.leftVariables, "left join variable",
        !options.hasLeftVariables);
    const auto rightVariables = resolveVariableIndices(
        *rightStorage, options.rightVariables, "right join variable",
        !options.hasRightVariables);
    if (!leftKeys.succeeded) {
        return joinFailure(leftKeys.error);
    }
    if (!rightKeys.succeeded) {
        return joinFailure(rightKeys.error);
    }
    if (!leftVariables.succeeded) {
        return joinFailure(leftVariables.error);
    }
    if (!rightVariables.succeeded) {
        return joinFailure(rightVariables.error);
    }
    for (size_t index = 0; index < leftKeys.indices.size(); ++index) {
        if (!keyVariablesCompatible(
                leftStorage->variables[leftKeys.indices[index]].value,
                rightStorage->variables[rightKeys.indices[index]].value)) {
            return joinFailure(
                "join key variables must have compatible classes and trailing shapes: " +
                options.leftKeys[index] + " / " +
                options.rightKeys[index]);
        }
    }

    std::vector<size_t> leftRows;
    std::vector<size_t> rightRows;
    std::string keyError;
    size_t operation = 0;
    if (options.type == RuntimeTableJoinType::Right) {
        std::unordered_map<RowKey, std::vector<size_t>, RowKeyHash>
            leftMatches;
        for (size_t row = 0; row < leftStorage->rowCount; ++row) {
            if (!executionCheckpoint(options.executionControl, operation)) {
                return joinFailure(
                    "join was stopped by runtime execution control");
            }
            auto key = makeRowKey(*leftStorage, leftKeys.indices,
                                  row, keyError);
            if (!key) {
                return joinFailure(std::move(keyError));
            }
            if (key->matchable) {
                leftMatches[*key].push_back(row);
            }
        }
        for (size_t row = 0; row < rightStorage->rowCount; ++row) {
            if (!executionCheckpoint(options.executionControl, operation)) {
                return joinFailure(
                    "join was stopped by runtime execution control");
            }
            auto key = makeRowKey(*rightStorage, rightKeys.indices,
                                  row, keyError);
            if (!key) {
                return joinFailure(std::move(keyError));
            }
            const auto matches = key->matchable
                                     ? leftMatches.find(*key)
                                     : leftMatches.end();
            if (matches == leftMatches.end()) {
                leftRows.push_back(kRuntimeTableUnmatchedRow);
                rightRows.push_back(row);
                continue;
            }
            for (const size_t leftRow : matches->second) {
                if (!executionCheckpoint(options.executionControl,
                                         operation)) {
                    return joinFailure(
                        "join was stopped by runtime execution control");
                }
                leftRows.push_back(leftRow);
                rightRows.push_back(row);
            }
        }
    } else {
        std::unordered_map<RowKey, std::vector<size_t>, RowKeyHash>
            rightMatches;
        for (size_t row = 0; row < rightStorage->rowCount; ++row) {
            if (!executionCheckpoint(options.executionControl, operation)) {
                return joinFailure(
                    "join was stopped by runtime execution control");
            }
            auto key = makeRowKey(*rightStorage, rightKeys.indices,
                                  row, keyError);
            if (!key) {
                return joinFailure(std::move(keyError));
            }
            if (key->matchable) {
                rightMatches[*key].push_back(row);
            }
        }
        std::vector<bool> matchedRight(rightStorage->rowCount, false);
        for (size_t row = 0; row < leftStorage->rowCount; ++row) {
            if (!executionCheckpoint(options.executionControl, operation)) {
                return joinFailure(
                    "join was stopped by runtime execution control");
            }
            auto key = makeRowKey(*leftStorage, leftKeys.indices,
                                  row, keyError);
            if (!key) {
                return joinFailure(std::move(keyError));
            }
            const auto matches = key->matchable
                                     ? rightMatches.find(*key)
                                     : rightMatches.end();
            if (matches == rightMatches.end()) {
                if (includesLeft(options.type)) {
                    leftRows.push_back(row);
                    rightRows.push_back(kRuntimeTableUnmatchedRow);
                }
                continue;
            }
            for (const size_t rightRow : matches->second) {
                if (!executionCheckpoint(options.executionControl,
                                         operation)) {
                    return joinFailure(
                        "join was stopped by runtime execution control");
                }
                leftRows.push_back(row);
                rightRows.push_back(rightRow);
                matchedRight[rightRow] = true;
            }
        }
        if (includesRight(options.type)) {
            for (size_t row = 0; row < matchedRight.size(); ++row) {
                if (!executionCheckpoint(options.executionControl,
                                         operation)) {
                    return joinFailure(
                        "join was stopped by runtime execution control");
                }
                if (!matchedRight[row]) {
                    leftRows.push_back(kRuntimeTableUnmatchedRow);
                    rightRows.push_back(row);
                }
            }
        }
    }

    std::vector<OutputVariable> outputVariables;
    if (options.mergeKeys) {
        for (size_t index = 0; index < leftKeys.indices.size(); ++index) {
            outputVariables.push_back(OutputVariable{
                OutputSide::Merged, leftKeys.indices[index],
                rightKeys.indices[index], options.leftKeys[index]});
        }
    }
    for (const size_t index : leftVariables.indices) {
        if (options.mergeKeys &&
            std::find(leftKeys.indices.begin(), leftKeys.indices.end(),
                      index) != leftKeys.indices.end()) {
            continue;
        }
        outputVariables.push_back(OutputVariable{
            OutputSide::Left, index, 0,
            leftStorage->variables[index].name});
    }
    for (const size_t index : rightVariables.indices) {
        if (options.mergeKeys &&
            std::find(rightKeys.indices.begin(), rightKeys.indices.end(),
                      index) != rightKeys.indices.end()) {
            continue;
        }
        outputVariables.push_back(OutputVariable{
            OutputSide::Right, 0, index,
            rightStorage->variables[index].name});
    }
    resolveOutputNames(outputVariables);

    std::vector<RuntimeValue> values;
    std::vector<std::string> names;
    values.reserve(outputVariables.size());
    names.reserve(outputVariables.size());
    for (const OutputVariable& output : outputVariables) {
        RuntimeTableOperationResult mapped;
        if (output.side == OutputSide::Merged) {
            mapped = mapMergedRows(
                leftStorage->variables[output.leftIndex].value,
                rightStorage->variables[output.rightIndex].value,
                leftRows, rightRows, options.executionControl);
        } else if (output.side == OutputSide::Left) {
            mapped = mapRows(
                leftStorage->variables[output.leftIndex].value,
                leftRows, options.executionControl);
        } else {
            mapped = mapRows(
                rightStorage->variables[output.rightIndex].value,
                rightRows, options.executionControl);
        }
        if (!mapped.succeeded) {
            return joinFailure("join output variable " + output.name +
                               ": " + mapped.error);
        }
        values.push_back(std::move(mapped.value));
        names.push_back(output.name);
    }
    auto result = outputVariables.empty()
                      ? runtimeMakeEmptyTable(leftRows.size())
                      : runtimeMakeTable(std::move(values),
                                         std::move(names));
    if (!result.succeeded) {
        return joinFailure(std::move(result.error));
    }
    return RuntimeTableJoinResult{
        true, std::move(result.value), std::move(leftRows),
        std::move(rightRows), {}};
}

RuntimeTableGroupingResult runtimeGroupTableRows(
    const RuntimeValue& table,
    const std::vector<std::string>& keyVariables,
    RuntimeExecutionControl* executionControl) {
    const RuntimeTableStorage* storage = runtimeTabularStorage(table);
    if (!storage) {
        return RuntimeTableGroupingResult{
            false, {}, {},
            "grouping requires a table or timetable input"};
    }
    if (keyVariables.empty()) {
        return RuntimeTableGroupingResult{
            false, {}, {}, "group variable list must not be empty"};
    }
    const auto keys = resolveVariableIndices(
        *storage, keyVariables, "group variable", false);
    if (!keys.succeeded) {
        return RuntimeTableGroupingResult{
            false, {}, {}, keys.error};
    }
    std::vector<RuntimeTableSortKey> sortKeys;
    sortKeys.reserve(keys.indices.size());
    for (const size_t index : keys.indices) {
        sortKeys.push_back(RuntimeTableSortKey{
            RuntimeTableSortKeyKind::Variable, index, false});
    }
    auto sorted = runtimeSortTable(table, sortKeys);
    if (!sorted.succeeded) {
        return RuntimeTableGroupingResult{
            false, {}, {}, "group variables cannot be sorted: " +
                                   sorted.error};
    }

    std::vector<std::vector<size_t>> groups;
    std::unordered_map<RowKey, size_t, RowKeyHash> groupByKey;
    std::string error;
    size_t operation = 0;
    for (const size_t row : sorted.order) {
        if (!executionCheckpoint(executionControl, operation)) {
            return RuntimeTableGroupingResult{
                false, {}, {},
                "grouping was stopped by runtime execution control"};
        }
        auto key = makeRowKey(*storage, keys.indices, row, error);
        if (!key) {
            return RuntimeTableGroupingResult{
                false, {}, {}, std::move(error)};
        }
        const auto [found, inserted] =
            groupByKey.emplace(*key, groups.size());
        if (inserted) {
            groups.push_back({});
        }
        groups[found->second].push_back(row);
    }
    return RuntimeTableGroupingResult{
        true, keys.indices, std::move(groups), {}};
}

} // namespace mparser
