#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "categorical runtime smoke failure: "
                  << message << '\n';
        std::exit(1);
    }
}

mparser::RuntimeValue text(std::string value) {
    return mparser::makeRuntimeCharacterVectorUtf8(value);
}

mparser::RuntimeValue textCell(std::vector<std::string> values) {
    std::vector<mparser::RuntimeValue> cells;
    cells.reserve(values.size());
    for (auto& value : values) {
        cells.push_back(text(std::move(value)));
    }
    return mparser::makeRuntimeCellValue(std::move(cells));
}

mparser::RuntimeValue row(std::vector<double> values) {
    return mparser::makeRuntimeVectorValue(std::move(values));
}

double numberAt(const mparser::RuntimeValue& value,
                size_t logicalIndex = 0) {
    const auto element =
        mparser::runtimeNumericElement(value, logicalIndex);
    require(element.has_value(), "numeric output element is unavailable");
    return *element;
}

mparser::BuiltinResult invoke(
    std::string_view name,
    const std::vector<mparser::RuntimeValue>& arguments) {
    const auto registry = mparser::defaultBuiltinRegistry();
    return registry->invoke(
        name, mparser::BuiltinCall{
                  arguments, 1, mparser::SourceSpan{}, nullptr});
}

} // namespace

int main() {
    using namespace mparser;

    auto numeric = runtimeConstructCategorical(row({
        1.0, 2.0, 1.0,
        std::numeric_limits<double>::quiet_NaN()}));
    require(numeric.succeeded, numeric.error);
    const RuntimeValue numericValue = numeric.value;
    const auto* numericStorage = runtimeCategoricalStorage(numericValue);
    require(numericStorage &&
                numericStorage->categories ==
                    std::vector<std::string>({"1", "2"}) &&
                runtimeCategoricalCode(numericValue, 0) == 1 &&
                runtimeCategoricalCode(numericValue, 1) == 2 &&
                runtimeCategoricalCode(numericValue, 3) == 0,
            "numeric categorical construction changed");
    require(validateRuntimeValueContract(numericValue).valid,
            "categorical RuntimeValue contract failed");
    const auto bytes = runtimeValueArrayBytes(numericValue);
    require(bytes && *bytes >= 4 * sizeof(std::uint32_t),
            "categorical byte accounting omitted codes");

    auto numericOrder = runtimeConstructCategorical(
        row({10.0, 2.0, 1.0}));
    require(numericOrder.succeeded &&
                runtimeCategoricalStorage(numericOrder.value)->categories ==
                    std::vector<std::string>({"1", "2", "10"}) &&
                runtimeCategoricalCode(numericOrder.value, 0) == 3,
            "implicit numeric categories are not numerically ordered");

    auto words = runtimeConstructCategorical(
        textCell({"b", "a", "b", ""}));
    require(words.succeeded, words.error);
    const RuntimeValue source = words.value;
    const auto* wordStorage = runtimeCategoricalStorage(source);
    require(wordStorage &&
                wordStorage->categories ==
                    std::vector<std::string>({"a", "b"}) &&
                runtimeCategoricalCode(source, 0) == 2 &&
                runtimeCategoricalCode(source, 1) == 1 &&
                runtimeCategoricalCode(source, 3) == 0,
            "text categorical construction changed");

    const RuntimeValue valueSet = row({1.0, 2.0});
    const RuntimeValue categoryNames = textCell({"low", "high"});
    auto ordinal = runtimeConstructCategorical(
        row({1.0, 2.0, 1.0}), &valueSet, &categoryNames,
        true, false);
    require(ordinal.succeeded, ordinal.error);
    const auto* ordinalStorage = runtimeCategoricalStorage(ordinal.value);
    require(ordinalStorage && ordinalStorage->ordinal &&
                ordinalStorage->protectedCategories,
            "ordinal categorical is not protected");

    auto selected = runtimeIndexCategorical(
        source, {row({3.0, 1.0})});
    require(selected.succeeded &&
                runtimeDimensions(selected.value) ==
                    std::vector<size_t>({1, 2}) &&
                runtimeCategoricalCode(selected.value, 0) == 2 &&
                runtimeCategoricalCode(selected.value, 1) == 2,
            "categorical indexing failed");

    auto assigned = runtimeAssignCategoricalIndexed(
        source, {row({4.0})}, text("c"));
    require(assigned.succeeded, assigned.error);
    const auto* assignedStorage = runtimeCategoricalStorage(assigned.value);
    require(assignedStorage &&
                assignedStorage->categories ==
                    std::vector<std::string>({"a", "b", "c"}) &&
                runtimeCategoricalCode(assigned.value, 3) == 3 &&
                runtimeCategoricalStorage(source)->categories.size() == 2,
            "categorical assignment or copy-on-write failed");

    auto grown = runtimeAssignCategoricalIndexed(
        assigned.value, {row({6.0})}, text("a"));
    require(grown.succeeded &&
                runtimeDimensions(grown.value) ==
                    std::vector<size_t>({1, 6}) &&
                runtimeCategoricalCode(grown.value, 4) == 0 &&
                runtimeCategoricalCode(grown.value, 5) == 1,
            "categorical growth did not fill undefined elements");

    auto deleted = runtimeDeleteCategoricalIndexed(
        grown.value, {row({2.0})}, {false});
    require(deleted.succeeded &&
                runtimeDimensions(deleted.value) ==
                    std::vector<size_t>({1, 5}),
            "categorical deletion failed");

    auto nd = runtimeMakeCategoricalValue(
        {2, 2, 2}, {"a", "b", "c", "d", "e", "f", "g", "h"},
        {1, 2, 3, 4, 5, 6, 7, 8});
    require(nd.succeeded, nd.error);
    auto ndDeleted = runtimeDeleteCategoricalIndexed(
        nd.value,
        {row({1.0, 2.0}), row({1.0}), row({1.0, 2.0})},
        {true, false, true});
    require(ndDeleted.succeeded &&
                runtimeDimensions(ndDeleted.value) ==
                    std::vector<size_t>({2, 1, 2}) &&
                runtimeCategoricalCode(ndDeleted.value, 0) == 3 &&
                runtimeCategoricalCode(ndDeleted.value, 1) == 4 &&
                runtimeCategoricalCode(ndDeleted.value, 2) == 7 &&
                runtimeCategoricalCode(ndDeleted.value, 3) == 8,
            "N-dimensional categorical deletion remapped codes incorrectly");
    auto ndGrown = runtimeAssignCategoricalIndexed(
        nd.value,
        {makeRuntimeNumberValue(2.0), makeRuntimeNumberValue(2.0),
         makeRuntimeNumberValue(3.0)},
        text("a"));
    require(ndGrown.succeeded &&
                runtimeDimensions(ndGrown.value) ==
                    std::vector<size_t>({2, 2, 3}) &&
                runtimeCategoricalCode(ndGrown.value, 8) == 0 &&
                runtimeCategoricalCode(ndGrown.value, 10) == 0 &&
                runtimeCategoricalCode(ndGrown.value, 11) == 1,
            "N-dimensional categorical growth did not preserve layout and undefined fill");

    auto added = runtimeAddCategories(
        source, {"c", "d"}, "Before", "b");
    require(added.succeeded &&
                runtimeCategoricalStorage(added.value)->categories ==
                    std::vector<std::string>({"a", "c", "d", "b"}) &&
                runtimeCategoricalCode(added.value, 0) == 4,
            "addcats ordering or code remapping failed");
    auto removed = runtimeRemoveCategories(added.value, {"a", "c"});
    require(removed.succeeded &&
                runtimeCategoricalStorage(removed.value)->categories ==
                    std::vector<std::string>({"d", "b"}) &&
                runtimeCategoricalCode(removed.value, 1) == 0,
            "removecats did not create undefined values");
    auto renamed = runtimeRenameCategories(
        added.value, {"a", "d"}, {"A", "D"});
    require(renamed.succeeded &&
                runtimeCategoricalStorage(renamed.value)->categories ==
                    std::vector<std::string>({"A", "c", "D", "b"}),
            "renamecats failed");
    auto reordered = runtimeReorderCategories(
        added.value, {"d", "b", "a", "c"});
    require(reordered.succeeded &&
                runtimeCategoricalCode(reordered.value, 0) == 2 &&
                runtimeCategoricalCode(reordered.value, 1) == 3,
            "reordercats did not preserve labels");
    auto merged = runtimeMergeCategories(
        runtimeConstructCategorical(textCell({"a", "b", "c"})).value,
        {"a", "c"}, "ac");
    require(merged.succeeded &&
                runtimeCategoricalStorage(merged.value)->categories ==
                    std::vector<std::string>({"ac", "b"}) &&
                runtimeCategoricalCode(merged.value, 2) == 1,
            "mergecats failed");

    auto concatenated = runtimeConcatenateCategorical(
        2, {source, runtimeConstructCategorical(
                        textCell({"b", "c"})).value});
    require(concatenated.succeeded &&
                runtimeDimensions(concatenated.value) ==
                    std::vector<size_t>({1, 6}) &&
                runtimeCategoricalStorage(concatenated.value)->categories ==
                    std::vector<std::string>({"a", "b", "c"}),
            "categorical concatenation failed");

    auto equalB = runtimeCompareCategorical("==", source, text("b"));
    require(equalB.succeeded && numberAt(equalB.value, 0) == 1.0 &&
                numberAt(equalB.value, 1) == 0.0 &&
                numberAt(equalB.value, 3) == 0.0,
            "categorical label comparison failed");
    auto less = runtimeCompareCategorical(
        "<", runtimeIndexCategorical(ordinal.value, {row({1.0})}).value,
        runtimeIndexCategorical(ordinal.value, {row({2.0})}).value);
    require(less.succeeded && numberAt(less.value) == 1.0,
            "ordinal categorical comparison failed");

    auto withUnused = runtimeAddCategories(source, {"unused"});
    require(withUnused.succeeded &&
                runtimeValuesEqual(source, withUnused.value,
                                   RuntimeNaNEquality::Equal) &&
                !runtimeValuesEqual(source, withUnused.value),
            "categorical isequal/isequaln undefined policy changed");

    const auto registry = defaultBuiltinRegistry();
    for (const std::string_view name : {
             "categorical", "iscategorical", "categories",
             "isundefined", "isordinal", "isprotected", "addcats",
             "removecats", "renamecats", "reordercats", "mergecats",
             "countcats"}) {
        const BuiltinDescriptor* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        BuiltinImplementationKind::Shared &&
                    descriptor->errorIdentifier ==
                        "MParser:InvalidCategoricalCall",
                "categorical registry descriptor is incomplete: " +
                    std::string(name));
    }
    auto built = invoke("categorical", {textCell({"b", "a", ""})});
    auto counts = built.succeeded
                      ? invoke("countcats", {built.outputs.front()})
                      : BuiltinResult{};
    auto converted = built.succeeded
                         ? invoke("double", {built.outputs.front()})
                         : BuiltinResult{};
    require(built.succeeded && counts.succeeded && converted.succeeded &&
                numberAt(counts.outputs.front(), 0) == 1.0 &&
                numberAt(counts.outputs.front(), 1) == 1.0 &&
                std::isnan(numberAt(converted.outputs.front(), 2)),
            "categorical registry invocation failed");

    std::cout << "categorical runtime smoke passed\n";
    return 0;
}
