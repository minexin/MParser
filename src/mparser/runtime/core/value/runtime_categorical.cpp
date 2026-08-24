#include "mparser/runtime/core/value/runtime_categorical.h"

#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace mparser {
namespace {

RuntimeCategoricalOperationResult failure(std::string error) {
    return RuntimeCategoricalOperationResult{false, {}, std::move(error)};
}

RuntimeCategoricalOperationResult success(RuntimeValue value) {
    return RuntimeCategoricalOperationResult{true, std::move(value), {}};
}

bool uniqueNonemptyNames(const std::vector<std::string>& names) {
    std::set<std::string> seen;
    return std::all_of(
        names.begin(), names.end(), [&](const std::string& name) {
            return !name.empty() && seen.insert(name).second;
        });
}

struct CategoryAtom {
    bool missing = false;
    bool numeric = false;
    RuntimeNumericElementValue numericValue;
    std::string key;
    std::string label;
};

std::string floatingText(double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

std::optional<CategoryAtom> numericAtom(
    const RuntimeValue& value, size_t logicalIndex) {
    const auto element = runtimeNumericElementValue(value, logicalIndex);
    if (!element || element->complex) {
        return std::nullopt;
    }
    CategoryAtom atom;
    atom.numeric = true;
    atom.numericValue = *element;
    if (runtimeNumericClassIsInteger(element->numericClass)) {
        if (runtimeNumericClassIsSignedInteger(element->numericClass)) {
            const auto signedValue = std::bit_cast<std::int64_t>(
                element->integerRealBits);
            atom.key = "n:" + std::to_string(signedValue);
            atom.label = std::to_string(signedValue);
        } else {
            atom.key = "n:" + std::to_string(element->integerRealBits);
            atom.label = std::to_string(element->integerRealBits);
        }
        return atom;
    }
    if (element->numericClass == RuntimeNumericClass::Logical) {
        atom.key = element->integerRealBits == 0 ? "n:0" : "n:1";
        atom.label = element->integerRealBits == 0 ? "false" : "true";
        return atom;
    }
    if (std::isnan(element->real)) {
        atom.missing = true;
        return atom;
    }
    const double normalized = element->real == 0.0 ? 0.0 : element->real;
    atom.key = "n:" + floatingText(normalized);
    atom.label = floatingText(normalized);
    return atom;
}

std::optional<CategoryAtom> textAtom(
    const RuntimeValue& value, size_t logicalIndex) {
    if (isRuntimeStringArray(value)) {
        const RuntimeStringElement* element =
            runtimeStringElement(value, logicalIndex);
        if (!element) {
            return std::nullopt;
        }
        CategoryAtom atom;
        atom.missing = element->missing || element->value.empty();
        if (!atom.missing) {
            atom.label = runtimeUtf16ToUtf8(element->value);
            atom.key = "t:" + atom.label;
        }
        return atom;
    }
    if (value.kind == RuntimeValueKind::Cell) {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            value, logicalIndex);
        if (!offset || *offset >= value.cells.size()) {
            return std::nullopt;
        }
        const RuntimeValue& element = value.cells[*offset];
        const auto text = runtimeTextScalarUtf8(element);
        if (!text) {
            return std::nullopt;
        }
        CategoryAtom atom;
        atom.missing = text->empty();
        if (!atom.missing) {
            atom.label = *text;
            atom.key = "t:" + atom.label;
        }
        return atom;
    }
    return std::nullopt;
}

struct CategoryAtomsResult {
    bool succeeded = false;
    std::vector<CategoryAtom> atoms;
    std::vector<size_t> dimensions;
    std::string error;
};

CategoryAtomsResult categoryAtoms(const RuntimeValue& value,
                                  bool allowCharacterScalar) {
    CategoryAtomsResult result;
    result.dimensions = runtimeDimensions(value);
    const size_t count = runtimeShapeElementCount(value);
    result.atoms.reserve(count);

    if (isRuntimeNumericValue(value)) {
        for (size_t index = 0; index < count; ++index) {
            const auto atom = numericAtom(value, index);
            if (!atom) {
                result.error =
                    "categorical numeric data must be real-valued";
                return result;
            }
            result.atoms.push_back(*atom);
        }
        result.succeeded = true;
        return result;
    }
    if (isRuntimeStringArray(value) ||
        value.kind == RuntimeValueKind::Cell) {
        for (size_t index = 0; index < count; ++index) {
            const auto atom = textAtom(value, index);
            if (!atom) {
                result.error =
                    "categorical text data must be a string array or Cell array of text scalars";
                return result;
            }
            result.atoms.push_back(*atom);
        }
        result.succeeded = true;
        return result;
    }
    if (allowCharacterScalar) {
        const auto text = runtimeTextScalarUtf8(value);
        if (text) {
            CategoryAtom atom;
            atom.missing = text->empty();
            if (!atom.missing) {
                atom.label = *text;
                atom.key = "t:" + atom.label;
            }
            result.dimensions = {1, 1};
            result.atoms.push_back(std::move(atom));
            result.succeeded = true;
            return result;
        }
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        CategoryAtom missing;
        missing.missing = true;
        result.atoms.assign(count, missing);
        result.succeeded = true;
        return result;
    }
    result.error =
        "categorical data must be numeric, string, or a Cell array of text";
    return result;
}

std::vector<std::uint32_t> logicalCodes(const RuntimeValue& value) {
    std::vector<std::uint32_t> result;
    result.reserve(runtimeShapeElementCount(value));
    for (size_t index = 0; index < runtimeShapeElementCount(value);
         ++index) {
        result.push_back(runtimeCategoricalCode(value, index));
    }
    return result;
}

RuntimeCategoricalOperationResult copyWith(
    const RuntimeValue& source, std::vector<std::string> categories,
    std::vector<std::uint32_t> codes) {
    const auto* storage = runtimeCategoricalStorage(source);
    if (!storage) {
        return failure("categorical operation requires a categorical value");
    }
    return runtimeMakeCategoricalValue(
        runtimeDimensions(source), std::move(categories),
        std::move(codes), storage->ordinal,
        storage->protectedCategories);
}

std::optional<size_t> categoryIndex(
    const std::vector<std::string>& categories,
    std::string_view name) {
    const auto found = std::find(categories.begin(), categories.end(), name);
    return found == categories.end()
               ? std::nullopt
               : std::optional<size_t>(static_cast<size_t>(
                     std::distance(categories.begin(), found)));
}

std::vector<size_t> grownDimensions(
    const RuntimeValue& target,
    const RuntimeIndexSelectionsResult& selections) {
    const auto oldDimensions = runtimeDimensions(target);
    if (selections.indices.size() == 1) {
        const auto extent = runtimeIndexSelectionRequiredExtent(
            selections.indices.front());
        if (!extent || *extent <= runtimeShapeElementCount(target)) {
            return oldDimensions;
        }
        if (oldDimensions.size() == 2 && oldDimensions[1] == 1 &&
            oldDimensions[0] != 1) {
            return {*extent, 1};
        }
        return {1, *extent};
    }

    auto dimensions = oldDimensions;
    const auto effective = runtimeEffectiveSubscriptDimensions(
        target, selections.indices.size());
    if (dimensions.size() < selections.indices.size()) {
        dimensions.resize(selections.indices.size(), 1);
    }
    for (size_t index = 0; index < selections.indices.size(); ++index) {
        const auto extent = runtimeIndexSelectionRequiredExtent(
            selections.indices[index]);
        if (extent) {
            dimensions[index] = std::max(dimensions[index], *extent);
        } else if (index < effective.size()) {
            dimensions[index] = std::max(dimensions[index], effective[index]);
        }
    }
    return normalizeRuntimeDimensions(std::move(dimensions));
}

std::optional<std::vector<std::uint32_t>> growCodes(
    const RuntimeValue& target,
    const std::vector<size_t>& newDimensions) {
    const auto oldDimensions = runtimeDimensions(target);
    const auto count = checkedRuntimeDimensionProduct(newDimensions);
    if (!count) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> result(
        *count, kRuntimeCategoricalUndefinedCode);
    for (size_t logicalIndex = 0;
         logicalIndex < runtimeShapeElementCount(target); ++logicalIndex) {
        auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, oldDimensions);
        if (!coordinates) {
            return std::nullopt;
        }
        coordinates->resize(newDimensions.size(), 0);
        const auto destination = runtimeColumnMajorLinearIndex(
            *coordinates, newDimensions);
        if (!destination || *destination >= result.size()) {
            return std::nullopt;
        }
        result[*destination] = runtimeCategoricalCode(target, logicalIndex);
    }
    return result;
}

std::vector<size_t> uniqueIndices(std::vector<size_t> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

RuntimeCategoricalOperationResult operandAsCategorical(
    const RuntimeValue& value) {
    if (isRuntimeCategoricalValue(value)) {
        return success(value);
    }
    auto atoms = categoryAtoms(value, true);
    if (!atoms.succeeded) {
        return failure(std::move(atoms.error));
    }
    std::vector<std::string> categories;
    for (const auto& atom : atoms.atoms) {
        if (!atom.missing &&
            std::find(categories.begin(), categories.end(), atom.label) ==
                categories.end()) {
            categories.push_back(atom.label);
        }
    }
    std::vector<std::uint32_t> codes;
    codes.reserve(atoms.atoms.size());
    for (const auto& atom : atoms.atoms) {
        const auto found = categoryIndex(categories, atom.label);
        codes.push_back(atom.missing || !found
                            ? kRuntimeCategoricalUndefinedCode
                            : static_cast<std::uint32_t>(*found + 1));
    }
    return runtimeMakeCategoricalValue(
        std::move(atoms.dimensions), std::move(categories),
        std::move(codes));
}

} // namespace

bool isRuntimeCategoricalValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           value.className == kRuntimeCategoricalClassName &&
           value.categoricalStorage != nullptr;
}

const RuntimeCategoricalStorage* runtimeCategoricalStorage(
    const RuntimeValue& value) {
    return isRuntimeCategoricalValue(value)
               ? value.categoricalStorage.get()
               : nullptr;
}

RuntimeCategoricalStorage* runtimeMutableCategoricalStorage(
    RuntimeValue& value) {
    if (!isRuntimeCategoricalValue(value)) {
        return nullptr;
    }
    if (value.categoricalStorage.use_count() != 1) {
        value.categoricalStorage =
            std::make_shared<RuntimeCategoricalStorage>(
                *value.categoricalStorage);
    }
    return value.categoricalStorage.get();
}

RuntimeCategoricalOperationResult runtimeMakeCategoricalValue(
    std::vector<size_t> dimensions,
    std::vector<std::string> categories,
    std::vector<std::uint32_t> logicalCodes,
    bool ordinal, bool protectedCategories) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != logicalCodes.size()) {
        return failure(
            "categorical code count does not match its dimensions");
    }
    if (!uniqueNonemptyNames(categories)) {
        return failure(
            "categorical categories must be unique and nonempty");
    }
    if (std::any_of(
            logicalCodes.begin(), logicalCodes.end(),
            [&](std::uint32_t code) {
                return code > categories.size();
            })) {
        return failure("categorical code is outside its category dictionary");
    }

    auto storage = std::make_shared<RuntimeCategoricalStorage>();
    storage->categories = std::move(categories);
    storage->codes.resize(*count, kRuntimeCategoricalUndefinedCode);
    storage->ordinal = ordinal;
    storage->protectedCategories = protectedCategories || ordinal;
    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(
                                      *coordinates, dimensions)
                                : std::nullopt;
        if (!offset || *offset >= storage->codes.size()) {
            return failure("categorical logical layout could not be mapped");
        }
        storage->codes[*offset] = logicalCodes[logicalIndex];
    }

    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeCategoricalClassName);
    result.categoricalStorage = std::move(storage);
    setRuntimeDimensions(result, std::move(dimensions));
    std::string error;
    if (!validateRuntimeCategoricalStorage(result, error)) {
        return failure(std::move(error));
    }
    return success(std::move(result));
}

RuntimeCategoricalOperationResult runtimeConstructCategorical(
    const RuntimeValue& data, const RuntimeValue* valueSet,
    const RuntimeValue* categoryNames, bool ordinal,
    bool protectedCategories) {
    if (isRuntimeCategoricalValue(data) && !valueSet && !categoryNames) {
        RuntimeValue result = data;
        auto* storage = runtimeMutableCategoricalStorage(result);
        storage->ordinal = ordinal || storage->ordinal;
        storage->protectedCategories =
            protectedCategories || storage->protectedCategories ||
            storage->ordinal;
        return success(std::move(result));
    }

    auto dataAtoms = categoryAtoms(data, false);
    if (!dataAtoms.succeeded) {
        return failure(std::move(dataAtoms.error));
    }
    std::vector<CategoryAtom> dictionaryAtoms;
    if (valueSet) {
        auto parsed = categoryAtoms(*valueSet, true);
        if (!parsed.succeeded) {
            return failure("categorical value set: " + parsed.error);
        }
        dictionaryAtoms = std::move(parsed.atoms);
        if (std::any_of(dictionaryAtoms.begin(), dictionaryAtoms.end(),
                        [](const CategoryAtom& atom) {
                            return atom.missing;
                        })) {
            return failure(
                "categorical value set must not contain missing values");
        }
    } else {
        dictionaryAtoms = dataAtoms.atoms;
        dictionaryAtoms.erase(
            std::remove_if(dictionaryAtoms.begin(), dictionaryAtoms.end(),
                           [](const CategoryAtom& atom) {
                               return atom.missing;
                           }),
            dictionaryAtoms.end());
        std::sort(dictionaryAtoms.begin(), dictionaryAtoms.end(),
                  [](const CategoryAtom& left, const CategoryAtom& right) {
                      if (left.numeric && right.numeric) {
                          return runtimeCompareNumericElementsForExtrema(
                                     left.numericValue,
                                     right.numericValue) < 0;
                      }
                      return left.label < right.label;
                  });
        dictionaryAtoms.erase(
            std::unique(dictionaryAtoms.begin(), dictionaryAtoms.end(),
                        [](const CategoryAtom& left,
                           const CategoryAtom& right) {
                            return left.key == right.key;
                        }),
            dictionaryAtoms.end());
    }

    std::set<std::string> dictionaryKeys;
    for (const auto& atom : dictionaryAtoms) {
        if (!dictionaryKeys.insert(atom.key).second) {
            return failure("categorical value set must contain unique values");
        }
    }

    std::vector<std::string> categories;
    if (categoryNames) {
        auto parsed = runtimeCategoricalNames(
            *categoryNames, "category");
        if (!parsed.succeeded) {
            return failure(std::move(parsed.error));
        }
        categories = std::move(parsed.names);
        if (categories.size() != dictionaryAtoms.size()) {
            return failure(
                "categorical category names must match the value set size");
        }
    } else {
        categories.reserve(dictionaryAtoms.size());
        for (const auto& atom : dictionaryAtoms) {
            categories.push_back(atom.label);
        }
    }
    if (!uniqueNonemptyNames(categories)) {
        return failure(
            "categorical category names must be unique and nonempty");
    }

    std::unordered_map<std::string, std::uint32_t> codeByKey;
    for (size_t index = 0; index < dictionaryAtoms.size(); ++index) {
        codeByKey.emplace(dictionaryAtoms[index].key,
                          static_cast<std::uint32_t>(index + 1));
    }
    std::vector<std::uint32_t> codes;
    codes.reserve(dataAtoms.atoms.size());
    for (const auto& atom : dataAtoms.atoms) {
        const auto found = atom.missing ? codeByKey.end()
                                        : codeByKey.find(atom.key);
        codes.push_back(found == codeByKey.end()
                            ? kRuntimeCategoricalUndefinedCode
                            : found->second);
    }
    return runtimeMakeCategoricalValue(
        std::move(dataAtoms.dimensions), std::move(categories),
        std::move(codes), ordinal, protectedCategories);
}

RuntimeCategoricalNamesResult runtimeCategoricalNames(
    const RuntimeValue& value, std::string_view role) {
    if (runtimeShapeElementCount(value) == 0) {
        return RuntimeCategoricalNamesResult{true, {}, {}};
    }
    if (const auto scalar = runtimeTextScalarUtf8(value)) {
        return scalar->empty()
                   ? RuntimeCategoricalNamesResult{
                         false, {}, std::string(role) +
                                        " names must not be empty"}
                   : RuntimeCategoricalNamesResult{true, {*scalar}, {}};
    }
    std::vector<std::string> names;
    const size_t count = runtimeShapeElementCount(value);
    names.reserve(count);
    if (isRuntimeStringArray(value)) {
        for (size_t index = 0; index < count; ++index) {
            const auto* element = runtimeStringElement(value, index);
            if (!element || element->missing || element->value.empty()) {
                return RuntimeCategoricalNamesResult{
                    false, {}, std::string(role) +
                                   " names must be nonmissing text"};
            }
            names.push_back(runtimeUtf16ToUtf8(element->value));
        }
        return RuntimeCategoricalNamesResult{true, std::move(names), {}};
    }
    if (value.kind == RuntimeValueKind::Cell) {
        for (size_t index = 0; index < count; ++index) {
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                value, index);
            const auto text = offset && *offset < value.cells.size()
                                  ? runtimeTextScalarUtf8(value.cells[*offset])
                                  : std::nullopt;
            if (!text || text->empty()) {
                return RuntimeCategoricalNamesResult{
                    false, {}, std::string(role) +
                                   " names must be nonempty text scalars"};
            }
            names.push_back(*text);
        }
        return RuntimeCategoricalNamesResult{true, std::move(names), {}};
    }
    return RuntimeCategoricalNamesResult{
        false, {}, std::string(role) +
                       " names must be text or a Cell/string text array"};
}

std::uint32_t runtimeCategoricalCode(const RuntimeValue& value,
                                     size_t logicalIndex) {
    const auto* storage = runtimeCategoricalStorage(value);
    const auto offset = runtimeColumnMajorLinearToStorageOffset(
        value, logicalIndex);
    return storage && offset && *offset < storage->codes.size()
               ? storage->codes[*offset]
               : kRuntimeCategoricalUndefinedCode;
}

std::string_view runtimeCategoricalLabel(const RuntimeValue& value,
                                         size_t logicalIndex) {
    const auto* storage = runtimeCategoricalStorage(value);
    const std::uint32_t code = runtimeCategoricalCode(value, logicalIndex);
    return storage && code > 0 && code <= storage->categories.size()
               ? std::string_view(storage->categories[code - 1])
               : std::string_view{};
}

RuntimeCategoricalOperationResult runtimeIndexCategorical(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts, bool linearColon) {
    const auto* storage = runtimeCategoricalStorage(target);
    if (!storage) {
        return failure("categorical indexing requires a categorical target");
    }
    const auto selections = runtimeResolveIndexSelections(
        target, subscripts, false, linearColon);
    if (!selections.succeeded) {
        return failure(std::move(selections.error));
    }
    const auto count = checkedRuntimeDimensionProduct(
        selections.resultDimensions);
    if (!count) {
        return failure("indexed categorical dimensions are too large");
    }
    std::vector<std::uint32_t> codes;
    codes.reserve(*count);
    for (size_t ordinal = 0; ordinal < *count; ++ordinal) {
        const auto source = runtimeIndexSelectionSourceLogicalIndex(
            selections, ordinal);
        if (!source) {
            return failure("categorical indexing could not map an element");
        }
        codes.push_back(runtimeCategoricalCode(target, *source));
    }
    return runtimeMakeCategoricalValue(
        selections.resultDimensions, storage->categories,
        std::move(codes), storage->ordinal,
        storage->protectedCategories);
}

RuntimeCategoricalOperationResult runtimeAssignCategoricalIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const RuntimeValue& value) {
    const auto* targetStorage = runtimeCategoricalStorage(target);
    if (!targetStorage) {
        return failure(
            "categorical assignment requires a categorical target");
    }
    auto sourceResult = operandAsCategorical(value);
    if (!sourceResult.succeeded) {
        return failure("categorical assignment value: " +
                       sourceResult.error);
    }
    const auto* sourceStorage =
        runtimeCategoricalStorage(sourceResult.value);
    if (targetStorage->ordinal &&
        (!sourceStorage->ordinal ||
         sourceStorage->categories != targetStorage->categories)) {
        return failure(
            "ordinal categorical assignment requires identical categories");
    }

    std::vector<std::string> categories = targetStorage->categories;
    std::vector<std::uint32_t> sourceCodes;
    sourceCodes.reserve(runtimeShapeElementCount(sourceResult.value));
    for (size_t index = 0;
         index < runtimeShapeElementCount(sourceResult.value); ++index) {
        const std::uint32_t code = runtimeCategoricalCode(
            sourceResult.value, index);
        if (code == kRuntimeCategoricalUndefinedCode) {
            sourceCodes.push_back(code);
            continue;
        }
        const std::string_view label = runtimeCategoricalLabel(
            sourceResult.value, index);
        auto found = categoryIndex(categories, label);
        if (!found) {
            if (targetStorage->protectedCategories) {
                return failure(
                    "protected categorical assignment contains an unknown category: " +
                    std::string(label));
            }
            categories.emplace_back(label);
            found = categories.size() - 1;
        }
        sourceCodes.push_back(static_cast<std::uint32_t>(*found + 1));
    }

    const auto selections = runtimeResolveIndexSelections(
        target, subscripts, true);
    if (!selections.succeeded) {
        return failure(std::move(selections.error));
    }
    std::vector<size_t> selectionDimensions;
    for (const auto& selection : selections.indices) {
        selectionDimensions.push_back(selection.size());
    }
    const auto selectionCount = checkedRuntimeDimensionProduct(
        selectionDimensions);
    if (!selectionCount) {
        return failure("categorical assignment dimensions are too large");
    }
    const bool scalarExpansion = sourceCodes.size() == 1;
    if (!scalarExpansion && sourceCodes.size() != *selectionCount) {
        return failure(
            "categorical assignment requires matching element counts");
    }

    const auto dimensions = grownDimensions(target, selections);
    auto codes = growCodes(target, dimensions);
    if (!codes) {
        return failure("categorical assignment dimensions are too large");
    }
    if (selections.indices.size() == 1) {
        for (size_t ordinal = 0;
             ordinal < selections.indices.front().size(); ++ordinal) {
            const size_t destination = selections.indices.front()[ordinal];
            if (destination >= codes->size()) {
                return failure(
                    "categorical assignment index is out of bounds");
            }
            (*codes)[destination] =
                sourceCodes[scalarExpansion ? 0 : ordinal];
        }
    } else {
        for (size_t ordinal = 0; ordinal < *selectionCount; ++ordinal) {
            const auto selectedCoordinates =
                runtimeColumnMajorCoordinates(ordinal,
                                              selectionDimensions);
            if (!selectedCoordinates) {
                return failure(
                    "categorical assignment could not map the value shape");
            }
            std::vector<size_t> destinationCoordinates(
                selections.indices.size(), 0);
            for (size_t axis = 0; axis < selections.indices.size(); ++axis) {
                destinationCoordinates[axis] = selections.indices[axis]
                    [(*selectedCoordinates)[axis]];
            }
            const auto destination = runtimeColumnMajorLinearIndex(
                destinationCoordinates, dimensions);
            if (!destination || *destination >= codes->size()) {
                return failure(
                    "categorical assignment could not map a destination");
            }
            (*codes)[*destination] =
                sourceCodes[scalarExpansion ? 0 : ordinal];
        }
    }
    return runtimeMakeCategoricalValue(
        dimensions, std::move(categories), std::move(*codes),
        targetStorage->ordinal, targetStorage->protectedCategories);
}

RuntimeCategoricalOperationResult runtimeDeleteCategoricalIndexed(
    const RuntimeValue& target,
    const std::vector<RuntimeValue>& subscripts,
    const std::vector<bool>& colonSubscripts) {
    const auto* storage = runtimeCategoricalStorage(target);
    if (!storage) {
        return failure(
            "categorical null assignment requires a categorical target");
    }
    const auto selections = runtimeResolveIndexSelections(
        target, subscripts, false);
    if (!selections.succeeded) {
        return failure(std::move(selections.error));
    }
    const auto oldDimensions = runtimeDimensions(target);
    if (selections.indices.size() == 1) {
        auto removed = uniqueIndices(selections.indices.front());
        const bool vectorShape = oldDimensions.size() == 2 &&
            (oldDimensions[0] == 1 || oldDimensions[1] == 1);
        if (!vectorShape && removed.size() !=
                                runtimeShapeElementCount(target)) {
            return failure(
                "linear categorical deletion requires a vector or all elements");
        }
        std::vector<bool> mask(runtimeShapeElementCount(target), false);
        for (const size_t index : removed) {
            if (index >= mask.size()) {
                return failure("categorical deletion index is out of bounds");
            }
            mask[index] = true;
        }
        std::vector<std::uint32_t> codes;
        for (size_t index = 0; index < mask.size(); ++index) {
            if (!mask[index]) {
                codes.push_back(runtimeCategoricalCode(target, index));
            }
        }
        std::vector<size_t> dimensions;
        if (codes.empty() && !vectorShape) {
            dimensions = {0, 0};
        } else if (oldDimensions[0] == 1) {
            dimensions = {1, codes.size()};
        } else {
            dimensions = {codes.size(), 1};
        }
        return runtimeMakeCategoricalValue(
            std::move(dimensions), storage->categories,
            std::move(codes), storage->ordinal,
            storage->protectedCategories);
    }

    if (colonSubscripts.size() != selections.indices.size()) {
        return failure("categorical deletion subscript metadata is invalid");
    }
    size_t deletionAxis = selections.indices.size();
    for (size_t axis = 0; axis < colonSubscripts.size(); ++axis) {
        if (!colonSubscripts[axis]) {
            if (deletionAxis != selections.indices.size()) {
                return failure(
                    "categorical deletion allows one non-colon subscript");
            }
            deletionAxis = axis;
        }
    }
    if (deletionAxis == selections.indices.size()) {
        return failure(
            "categorical deletion requires one non-colon subscript");
    }
    auto dimensions = oldDimensions;
    dimensions.resize(selections.indices.size(), 1);
    auto removed = uniqueIndices(selections.indices[deletionAxis]);
    std::vector<bool> removedMask(dimensions[deletionAxis], false);
    for (const size_t index : removed) {
        if (index >= removedMask.size()) {
            return failure("categorical deletion index is out of bounds");
        }
        removedMask[index] = true;
    }
    auto newDimensions = dimensions;
    newDimensions[deletionAxis] -= removed.size();
    const auto newCount = checkedRuntimeDimensionProduct(newDimensions);
    if (!newCount) {
        return failure("categorical deletion dimensions are too large");
    }
    std::vector<std::uint32_t> codes;
    codes.reserve(*newCount);
    for (size_t oldIndex = 0;
         oldIndex < runtimeShapeElementCount(target); ++oldIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            oldIndex, dimensions);
        if (!coordinates ||
            removedMask[(*coordinates)[deletionAxis]]) {
            continue;
        }
        codes.push_back(runtimeCategoricalCode(target, oldIndex));
    }
    return runtimeMakeCategoricalValue(
        std::move(newDimensions), storage->categories,
        std::move(codes), storage->ordinal,
        storage->protectedCategories);
}

RuntimeCategoricalOperationResult runtimeConcatenateCategorical(
    size_t dimension, const std::vector<RuntimeValue>& values) {
    if (dimension == 0 || values.empty()) {
        return failure(
            "categorical concatenation requires a positive dimension and inputs");
    }
    if (!std::all_of(values.begin(), values.end(),
                     isRuntimeCategoricalValue)) {
        return failure(
            "categorical concatenation requires categorical inputs");
    }
    const auto* first = runtimeCategoricalStorage(values.front());
    std::vector<std::string> categories = first->categories;
    for (size_t index = 1; index < values.size(); ++index) {
        const auto* storage = runtimeCategoricalStorage(values[index]);
        if (storage->ordinal != first->ordinal) {
            return failure(
                "categorical concatenation requires matching ordinal state");
        }
        if (first->ordinal && storage->categories != first->categories) {
            return failure(
                "ordinal categorical concatenation requires identical category order");
        }
        if ((first->protectedCategories || storage->protectedCategories) &&
            storage->categories != first->categories) {
            return failure(
                "protected categorical concatenation requires identical categories");
        }
        if (!first->ordinal) {
            for (const std::string& category : storage->categories) {
                if (!categoryIndex(categories, category)) {
                    categories.push_back(category);
                }
            }
        }
    }

    size_t dimensionCount = std::max<size_t>(dimension, 2);
    for (const auto& value : values) {
        dimensionCount = std::max(
            dimensionCount, runtimeDimensionCount(value));
    }
    const size_t axis = dimension - 1;
    auto outputDimensions = runtimeDimensions(values.front());
    outputDimensions.resize(dimensionCount, 1);
    outputDimensions[axis] = 0;
    std::vector<std::vector<size_t>> inputDimensions;
    for (const auto& value : values) {
        auto dimensions = runtimeDimensions(value);
        dimensions.resize(dimensionCount, 1);
        if (!inputDimensions.empty()) {
            for (size_t current = 0; current < dimensionCount; ++current) {
                if (current != axis &&
                    dimensions[current] != inputDimensions.front()[current]) {
                    return failure(
                        "categorical concatenation dimensions must agree outside the selected dimension");
                }
            }
        }
        if (dimensions[axis] >
            std::numeric_limits<size_t>::max() - outputDimensions[axis]) {
            return failure(
                "categorical concatenation dimensions are too large");
        }
        outputDimensions[axis] += dimensions[axis];
        inputDimensions.push_back(std::move(dimensions));
    }
    const auto outputCount = checkedRuntimeDimensionProduct(
        outputDimensions);
    if (!outputCount) {
        return failure("categorical concatenation dimensions are too large");
    }
    std::vector<std::uint32_t> outputCodes(
        *outputCount, kRuntimeCategoricalUndefinedCode);
    size_t axisOffset = 0;
    for (size_t input = 0; input < values.size(); ++input) {
        for (size_t logicalIndex = 0;
             logicalIndex < runtimeShapeElementCount(values[input]);
             ++logicalIndex) {
            auto coordinates = runtimeColumnMajorCoordinates(
                logicalIndex, inputDimensions[input]);
            if (!coordinates) {
                return failure(
                    "categorical concatenation could not map an input");
            }
            (*coordinates)[axis] += axisOffset;
            const auto destination = runtimeColumnMajorLinearIndex(
                *coordinates, outputDimensions);
            if (!destination || *destination >= outputCodes.size()) {
                return failure(
                    "categorical concatenation could not map an output");
            }
            const std::uint32_t code = runtimeCategoricalCode(
                values[input], logicalIndex);
            if (code == kRuntimeCategoricalUndefinedCode) {
                continue;
            }
            const auto mapped = categoryIndex(
                categories,
                runtimeCategoricalLabel(values[input], logicalIndex));
            if (!mapped) {
                return failure(
                    "categorical concatenation lost a category mapping");
            }
            outputCodes[*destination] =
                static_cast<std::uint32_t>(*mapped + 1);
        }
        axisOffset += inputDimensions[input][axis];
    }
    return runtimeMakeCategoricalValue(
        std::move(outputDimensions), std::move(categories),
        std::move(outputCodes), first->ordinal,
        first->protectedCategories);
}

RuntimeCategoricalOperationResult runtimeCompareCategorical(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    auto lhsResult = operandAsCategorical(left);
    auto rhsResult = operandAsCategorical(right);
    if (!lhsResult.succeeded || !rhsResult.succeeded) {
        return failure("categorical comparison requires categorical or text operands");
    }
    const RuntimeValue& lhsValue = lhsResult.value;
    const RuntimeValue& rhsValue = rhsResult.value;
    const auto* lhs = runtimeCategoricalStorage(lhsValue);
    const auto* rhs = runtimeCategoricalStorage(rhsValue);
    const bool relational = operation == "<" || operation == "<=" ||
                            operation == ">" || operation == ">=";
    if (operation != "==" && operation != "~=" && !relational) {
        return failure("unsupported categorical comparison operator");
    }
    if (relational &&
        (!lhs->ordinal || !rhs->ordinal ||
         lhs->categories != rhs->categories)) {
        return failure(
            "categorical relational comparison requires identical ordinal categories");
    }
    if (lhs->ordinal != rhs->ordinal) {
        return failure(
            "categorical comparison requires matching ordinal state");
    }
    const auto dimensions = runtimeImplicitExpansionDimensions(
        runtimeDimensions(lhsValue), runtimeDimensions(rhsValue));
    if (!dimensions) {
        return failure(
            "categorical operands have incompatible dimensions");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure("categorical comparison dimensions are too large");
    }
    std::vector<double> output;
    output.reserve(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, *dimensions);
        const auto leftOffset = coordinates
                                    ? runtimeImplicitExpansionStorageOffset(
                                          *coordinates,
                                          runtimeDimensions(lhsValue))
                                    : std::nullopt;
        const auto rightOffset = coordinates
                                     ? runtimeImplicitExpansionStorageOffset(
                                           *coordinates,
                                           runtimeDimensions(rhsValue))
                                     : std::nullopt;
        if (!leftOffset || !rightOffset ||
            *leftOffset >= lhs->codes.size() ||
            *rightOffset >= rhs->codes.size()) {
            return failure(
                "categorical comparison could not map an element");
        }
        const std::uint32_t leftCode = lhs->codes[*leftOffset];
        const std::uint32_t rightCode = rhs->codes[*rightOffset];
        bool result = false;
        if (leftCode == kRuntimeCategoricalUndefinedCode ||
            rightCode == kRuntimeCategoricalUndefinedCode) {
            result = operation == "~=";
        } else if (relational) {
            if (operation == "<") {
                result = leftCode < rightCode;
            } else if (operation == "<=") {
                result = leftCode <= rightCode;
            } else if (operation == ">") {
                result = leftCode > rightCode;
            } else {
                result = leftCode >= rightCode;
            }
        } else {
            const bool equal = lhs->categories[leftCode - 1] ==
                               rhs->categories[rightCode - 1];
            result = operation == "==" ? equal : !equal;
        }
        output.push_back(result ? 1.0 : 0.0);
    }
    auto value = runtimeNumericValueFromLogicalOrder(
        *dimensions, std::move(output), RuntimeNumericClass::Logical);
    return value ? success(std::move(*value))
                 : failure("categorical comparison result is invalid");
}

RuntimeCategoricalOperationResult runtimeCategoricalMissingMask(
    const RuntimeValue& value) {
    if (!isRuntimeCategoricalValue(value)) {
        return failure("isundefined expects a categorical input");
    }
    std::vector<double> mask;
    mask.reserve(runtimeShapeElementCount(value));
    for (size_t index = 0; index < runtimeShapeElementCount(value); ++index) {
        mask.push_back(runtimeCategoricalCode(value, index) ==
                               kRuntimeCategoricalUndefinedCode
                           ? 1.0
                           : 0.0);
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        runtimeDimensions(value), std::move(mask),
        RuntimeNumericClass::Logical);
    return result ? success(std::move(*result))
                  : failure("categorical missing mask is invalid");
}

RuntimeCategoricalOperationResult runtimeCategoricalToDouble(
    const RuntimeValue& value) {
    if (!isRuntimeCategoricalValue(value)) {
        return failure("double conversion expects a categorical input");
    }
    std::vector<double> codes;
    codes.reserve(runtimeShapeElementCount(value));
    for (size_t index = 0; index < runtimeShapeElementCount(value); ++index) {
        const std::uint32_t code = runtimeCategoricalCode(value, index);
        codes.push_back(code == kRuntimeCategoricalUndefinedCode
                            ? std::numeric_limits<double>::quiet_NaN()
                            : static_cast<double>(code));
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        runtimeDimensions(value), std::move(codes),
        RuntimeNumericClass::Double);
    return result ? success(std::move(*result))
                  : failure("categorical code conversion is invalid");
}

RuntimeCategoricalOperationResult runtimeCategoricalToString(
    const RuntimeValue& value) {
    if (!isRuntimeCategoricalValue(value)) {
        return failure("string conversion expects a categorical input");
    }
    std::vector<RuntimeStringElement> elements;
    elements.reserve(runtimeShapeElementCount(value));
    for (size_t index = 0; index < runtimeShapeElementCount(value); ++index) {
        const std::uint32_t code = runtimeCategoricalCode(value, index);
        if (code == kRuntimeCategoricalUndefinedCode) {
            elements.push_back(RuntimeStringElement{{}, true});
        } else {
            elements.push_back(RuntimeStringElement{
                runtimeUtf8ToUtf16(runtimeCategoricalLabel(value, index)),
                false});
        }
    }
    return success(makeRuntimeStringArray(
        runtimeDimensions(value), std::move(elements)));
}

RuntimeCategoricalOperationResult runtimeAddCategories(
    const RuntimeValue& value, std::vector<std::string> names,
    std::string_view placement, std::string_view anchor) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage || !uniqueNonemptyNames(names)) {
        return failure(
            "addcats requires a categorical value and unique nonempty names");
    }
    std::vector<std::string> categories = storage->categories;
    for (const std::string& name : names) {
        if (categoryIndex(categories, name)) {
            return failure("categorical category already exists: " + name);
        }
    }
    size_t insertion = categories.size();
    if (!placement.empty()) {
        const auto anchorIndex = categoryIndex(categories, anchor);
        if (!anchorIndex ||
            (placement != "Before" && placement != "After")) {
            return failure("addcats placement or anchor is invalid");
        }
        insertion = *anchorIndex + (placement == "After" ? 1 : 0);
    }
    categories.insert(categories.begin() +
                          static_cast<std::ptrdiff_t>(insertion),
                      names.begin(), names.end());
    auto codes = logicalCodes(value);
    for (auto& code : codes) {
        if (code > insertion) {
            code += static_cast<std::uint32_t>(names.size());
        }
    }
    return copyWith(value, std::move(categories), std::move(codes));
}

RuntimeCategoricalOperationResult runtimeRemoveCategories(
    const RuntimeValue& value, const std::vector<std::string>& names) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage) {
        return failure("removecats expects a categorical input");
    }
    std::set<size_t> removed;
    for (const std::string& name : names) {
        const auto index = categoryIndex(storage->categories, name);
        if (!index) {
            return failure("categorical category is not available: " + name);
        }
        removed.insert(*index);
    }
    std::vector<std::string> categories;
    std::vector<std::uint32_t> remap(storage->categories.size() + 1, 0);
    for (size_t index = 0; index < storage->categories.size(); ++index) {
        if (!removed.contains(index)) {
            categories.push_back(storage->categories[index]);
            remap[index + 1] = static_cast<std::uint32_t>(categories.size());
        }
    }
    auto codes = logicalCodes(value);
    for (auto& code : codes) {
        code = code < remap.size() ? remap[code] : 0;
    }
    return copyWith(value, std::move(categories), std::move(codes));
}

RuntimeCategoricalOperationResult runtimeRenameCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& oldNames,
    const std::vector<std::string>& newNames) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage || oldNames.size() != newNames.size() ||
        !uniqueNonemptyNames(newNames)) {
        return failure("renamecats category lists are invalid");
    }
    auto categories = storage->categories;
    for (size_t index = 0; index < oldNames.size(); ++index) {
        const auto found = categoryIndex(categories, oldNames[index]);
        if (!found) {
            return failure("categorical category is not available: " +
                           oldNames[index]);
        }
        categories[*found] = newNames[index];
    }
    if (!uniqueNonemptyNames(categories)) {
        return failure("renamecats would create duplicate categories");
    }
    return copyWith(value, std::move(categories), logicalCodes(value));
}

RuntimeCategoricalOperationResult runtimeReorderCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& names) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage || names.size() != storage->categories.size() ||
        !uniqueNonemptyNames(names)) {
        return failure(
            "reordercats requires a permutation of all categories");
    }
    std::vector<std::uint32_t> remap(storage->categories.size() + 1, 0);
    for (size_t index = 0; index < names.size(); ++index) {
        const auto old = categoryIndex(storage->categories, names[index]);
        if (!old) {
            return failure(
                "reordercats requires a permutation of all categories");
        }
        remap[*old + 1] = static_cast<std::uint32_t>(index + 1);
    }
    auto codes = logicalCodes(value);
    for (auto& code : codes) {
        code = code < remap.size() ? remap[code] : 0;
    }
    return copyWith(value, names, std::move(codes));
}

RuntimeCategoricalOperationResult runtimeMergeCategories(
    const RuntimeValue& value,
    const std::vector<std::string>& names,
    std::string mergedName) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage || names.empty() || mergedName.empty()) {
        return failure("mergecats arguments are invalid");
    }
    std::set<size_t> merged;
    for (const std::string& name : names) {
        const auto index = categoryIndex(storage->categories, name);
        if (!index) {
            return failure("categorical category is not available: " + name);
        }
        merged.insert(*index);
    }
    const size_t first = *merged.begin();
    if (const auto existing = categoryIndex(storage->categories, mergedName);
        existing && !merged.contains(*existing)) {
        return failure("mergecats result category already exists");
    }
    std::vector<std::string> categories;
    std::vector<std::uint32_t> remap(storage->categories.size() + 1, 0);
    for (size_t index = 0; index < storage->categories.size(); ++index) {
        if (index == first) {
            categories.push_back(mergedName);
            for (const size_t old : merged) {
                remap[old + 1] =
                    static_cast<std::uint32_t>(categories.size());
            }
        } else if (!merged.contains(index)) {
            categories.push_back(storage->categories[index]);
            remap[index + 1] =
                static_cast<std::uint32_t>(categories.size());
        }
    }
    auto codes = logicalCodes(value);
    for (auto& code : codes) {
        code = code < remap.size() ? remap[code] : 0;
    }
    return copyWith(value, std::move(categories), std::move(codes));
}

RuntimeCategoricalOperationResult runtimeCategoricalCounts(
    const RuntimeValue& value) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage) {
        return failure("countcats expects a categorical input");
    }
    std::vector<double> counts(storage->categories.size(), 0.0);
    for (const std::uint32_t code : storage->codes) {
        if (code > 0 && code <= counts.size()) {
            counts[code - 1] += 1.0;
        }
    }
    return success(makeRuntimeVectorValue(std::move(counts)));
}

bool validateRuntimeCategoricalStorage(const RuntimeValue& value,
                                       std::string& error) {
    const auto* storage = runtimeCategoricalStorage(value);
    if (!storage) {
        error = "categorical value has no categorical storage";
        return false;
    }
    if (!uniqueNonemptyNames(storage->categories)) {
        error = "categorical category dictionary is invalid";
        return false;
    }
    if (storage->codes.size() != runtimeShapeElementCount(value)) {
        error = "categorical code count does not match its shape";
        return false;
    }
    if (std::any_of(storage->codes.begin(), storage->codes.end(),
                    [&](std::uint32_t code) {
                        return code > storage->categories.size();
                    })) {
        error = "categorical storage contains an invalid code";
        return false;
    }
    if (storage->ordinal && !storage->protectedCategories) {
        error = "ordinal categorical storage must be protected";
        return false;
    }
    return true;
}

bool runtimeCategoricalValuesEqual(const RuntimeValue& left,
                                   const RuntimeValue& right,
                                   bool equalUndefined) {
    const auto* lhs = runtimeCategoricalStorage(left);
    const auto* rhs = runtimeCategoricalStorage(right);
    if (!lhs || !rhs || runtimeDimensions(left) != runtimeDimensions(right) ||
        lhs->ordinal != rhs->ordinal ||
        (lhs->ordinal && lhs->categories != rhs->categories)) {
        return false;
    }
    for (size_t index = 0; index < runtimeShapeElementCount(left); ++index) {
        const std::uint32_t leftCode = runtimeCategoricalCode(left, index);
        const std::uint32_t rightCode = runtimeCategoricalCode(right, index);
        if (leftCode == 0 || rightCode == 0) {
            if (!equalUndefined || leftCode != rightCode) {
                return false;
            }
            continue;
        }
        if (lhs->categories[leftCode - 1] !=
            rhs->categories[rightCode - 1]) {
            return false;
        }
    }
    return true;
}

} // namespace mparser
