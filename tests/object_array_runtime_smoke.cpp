#include "mparser/execution/interpreter.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    mparser::setRuntimeDimensions(result, {1, 1});
    return result;
}

mparser::RuntimeValue rowVector(std::vector<double> values) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Vector;
    result.elements = std::move(values);
    mparser::setRuntimeDimensions(result, {1, result.elements.size()});
    return result;
}

mparser::RuntimeValue logicalRow(std::vector<double> values) {
    auto result = rowVector(std::move(values));
    result.numericClass = mparser::RuntimeNumericClass::Logical;
    return result;
}

mparser::RuntimeValue object(
    std::string className, double id, bool handle = false) {
    std::map<std::string, mparser::RuntimeValue> fields;
    fields.emplace("Id", number(id));
    return mparser::makeRuntimeObjectScalar(
        std::move(className), std::move(fields), handle);
}

double idOf(const mparser::RuntimeValue& value) {
    const auto* fields = mparser::runtimeObjectFields(value);
    assert(fields != nullptr);
    const auto found = fields->find("Id");
    assert(found != fields->end());
    assert(found->second.kind == mparser::RuntimeValueKind::Number);
    return found->second.number;
}

mparser::RuntimeObjectArrayPolicy heterogeneousPolicy() {
    mparser::RuntimeObjectArrayPolicy policy;
    policy.resolveCommonClass =
        [](const std::vector<std::string>& classes,
           std::string_view preferred) {
            if (classes.empty()) {
                return mparser::RuntimeObjectClassResolutionResult{
                    !preferred.empty(), std::string(preferred),
                    preferred.empty() ? "missing object class" : ""};
            }
            const bool allSame = std::all_of(
                classes.begin(), classes.end(),
                [&](const std::string& name) {
                    return name == classes.front();
                });
            const auto inShapeHierarchy = [](std::string_view name) {
                return name == "Shape" || name == "Circle" ||
                       name == "Square";
            };
            if ((!preferred.empty() && preferred != "Shape" &&
                 classes.front() != preferred) ||
                !std::all_of(classes.begin(), classes.end(),
                             inShapeHierarchy)) {
                return mparser::RuntimeObjectClassResolutionResult{
                    false, {}, "incompatible object classes"};
            }
            return mparser::RuntimeObjectClassResolutionResult{
                true, allSame ? classes.front() : "Shape", {}};
        };
    return policy;
}

void testStorageAndIndexing() {
    auto array = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {object("ValueItem", 1), object("ValueItem", 2),
         object("ValueItem", 3), object("ValueItem", 4)},
        {2, 2}, "ValueItem", false);
    assert(array.succeeded);
    assert(array.value.objectElements.size() == 4);
    assert(idOf(array.value.objectElements[0]) == 1);
    assert(idOf(array.value.objectElements[1]) == 3);
    assert(idOf(array.value.objectElements[2]) == 2);
    assert(idOf(array.value.objectElements[3]) == 4);
    for (size_t index = 0; index < 4; ++index) {
        const auto* element =
            mparser::runtimeObjectLogicalElement(array.value, index);
        assert(element != nullptr);
        assert(idOf(*element) == static_cast<double>(index + 1));
    }

    const auto linear = mparser::runtimeIndexObject(
        array.value, {number(2)});
    assert(linear.succeeded);
    assert(mparser::isRuntimeScalarObject(linear.value));
    assert(idOf(linear.value) == 2);

    const auto subscripted = mparser::runtimeIndexObject(
        array.value, {rowVector({2, 1}), number(2)});
    assert(subscripted.succeeded);
    assert(mparser::runtimeDimensions(subscripted.value) ==
           std::vector<size_t>({2, 1}));
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               subscripted.value, 0)) == 4);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               subscripted.value, 1)) == 3);

    const auto empty = mparser::runtimeIndexObject(
        array.value, {rowVector({})});
    assert(empty.succeeded);
    assert(empty.value.className == "ValueItem");
    assert(mparser::runtimeDimensions(empty.value) ==
           std::vector<size_t>({1, 0}));
    assert(mparser::runtimeObjectElementCount(empty.value) == 0);
    assert(mparser::runtimeValueToString(empty.value) ==
           "<ValueItem 1x0>");
    assert(mparser::runtimeValueToString(array.value) ==
           "<ValueItem 2x2>");

    auto vector = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {object("ValueItem", 1), object("ValueItem", 2),
         object("ValueItem", 3)},
        {1, 3}, "ValueItem", false);
    assert(vector.succeeded);
    const auto masked = mparser::runtimeIndexObject(
        vector.value, {logicalRow({1, 0, 1})});
    assert(masked.succeeded);
    assert(mparser::runtimeDimensions(masked.value) ==
           std::vector<size_t>({1, 2}));
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               masked.value, 0)) == 1);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               masked.value, 1)) == 3);

    auto cube = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {object("ValueItem", 1), object("ValueItem", 2),
         object("ValueItem", 3), object("ValueItem", 4),
         object("ValueItem", 5), object("ValueItem", 6),
         object("ValueItem", 7), object("ValueItem", 8)},
        {2, 2, 2}, "ValueItem", false);
    assert(cube.succeeded);
    const auto cubeElement = mparser::runtimeIndexObject(
        cube.value, {number(2), number(1), number(2)});
    assert(cubeElement.succeeded);
    assert(idOf(cubeElement.value) == 6);
}

void testValueAndHandleAssignment() {
    auto values = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {object("ValueItem", 1), object("ValueItem", 2)},
        {1, 2}, "ValueItem", false);
    assert(values.succeeded);
    auto assigned = mparser::runtimeAssignObjectIndexed(
        values.value, {number(2)}, object("ValueItem", 9));
    assert(assigned.succeeded);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               assigned.value, 0)) == 1);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               assigned.value, 1)) == 9);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               values.value, 1)) == 2);

    const auto repeated = mparser::runtimeAssignObjectIndexed(
        values.value, {rowVector({2, 2})},
        mparser::runtimeMakeObjectArrayFromLogicalOrder(
            {object("ValueItem", 8), object("ValueItem", 9)},
            {1, 2}, "ValueItem", false)
            .value);
    assert(repeated.succeeded);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               repeated.value, 1)) == 9);

    const auto handle = object("HandleItem", 7, true);
    auto handles = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {handle, handle}, {1, 2}, "HandleItem", true);
    assert(handles.succeeded);
    const auto* first =
        mparser::runtimeObjectLogicalElement(handles.value, 0);
    const auto* second =
        mparser::runtimeObjectLogicalElement(handles.value, 1);
    assert(first != nullptr && second != nullptr);
    assert(first->sharedFields == second->sharedFields);

    auto failedGrowth = mparser::runtimeAssignObjectIndexed(
        handle, {number(3)}, object("HandleItem", 10, true));
    assert(!failedGrowth.succeeded);
    assert(idOf(handle) == 7);

    size_t defaultCount = 0;
    mparser::RuntimeObjectArrayPolicy policy;
    policy.constructDefault =
        [&](std::string_view className) {
            ++defaultCount;
            return mparser::RuntimeObjectOperationResult{
                true,
                object(std::string(className),
                       100 + static_cast<double>(defaultCount), true),
                {}};
        };
    auto grown = mparser::runtimeAssignObjectIndexed(
        handle, {number(3)}, object("HandleItem", 10, true), policy);
    assert(grown.succeeded);
    assert(defaultCount == 1);
    assert(mparser::runtimeDimensions(grown.value) ==
           std::vector<size_t>({1, 3}));
    const auto* original =
        mparser::runtimeObjectLogicalElement(grown.value, 0);
    const auto* filler =
        mparser::runtimeObjectLogicalElement(grown.value, 1);
    const auto* inserted =
        mparser::runtimeObjectLogicalElement(grown.value, 2);
    assert(original && filler && inserted);
    assert(idOf(*original) == 7);
    assert(idOf(*filler) == 101);
    assert(idOf(*inserted) == 10);
    assert(original->sharedFields != filler->sharedFields);
    assert(filler->sharedFields != inserted->sharedFields);

    size_t attemptedDefaults = 0;
    mparser::RuntimeObjectArrayPolicy failingPolicy;
    failingPolicy.constructDefault =
        [&](std::string_view className) {
            ++attemptedDefaults;
            if (attemptedDefaults == 2) {
                return mparser::RuntimeObjectOperationResult{
                    false, {}, "intentional constructor failure"};
            }
            return mparser::RuntimeObjectOperationResult{
                true, object(std::string(className), 200, true), {}};
        };
    const auto failedTransaction =
        mparser::runtimeAssignObjectIndexed(
            handle, {number(4)}, object("HandleItem", 40, true),
            failingPolicy);
    assert(!failedTransaction.succeeded);
    assert(attemptedDefaults == 2);
    assert(idOf(handle) == 7);
}

void testHeterogeneousArraysAndDeletion() {
    const auto policy = heterogeneousPolicy();
    auto mixed = mparser::runtimeConcatenateObject(
        2, {object("Circle", 1), object("Square", 2)}, policy);
    assert(mixed.succeeded);
    assert(mixed.value.className == "Shape");
    assert(mparser::runtimeDimensions(mixed.value) ==
           std::vector<size_t>({1, 2}));

    const auto narrowed = mparser::runtimeIndexObject(
        mixed.value, {number(1)}, policy);
    assert(narrowed.succeeded);
    assert(narrowed.value.className == "Circle");

    const auto deletedMixed = mparser::runtimeDeleteObjectIndexed(
        mixed.value, {number(2)}, {false}, policy);
    assert(deletedMixed.succeeded);
    assert(deletedMixed.value.className == "Circle");
    assert(mparser::isRuntimeScalarObject(deletedMixed.value));

    const auto incompatible = mparser::runtimeConcatenateObject(
        2, {object("Circle", 1), object("Square", 2)});
    assert(!incompatible.succeeded);

    auto matrix = mparser::runtimeMakeObjectArrayFromLogicalOrder(
        {object("ValueItem", 1), object("ValueItem", 2),
         object("ValueItem", 3), object("ValueItem", 4)},
        {2, 2}, "ValueItem", false);
    assert(matrix.succeeded);
    const auto deleted = mparser::runtimeDeleteObjectIndexed(
        matrix.value, {number(2), rowVector({1, 2})}, {false, true});
    assert(deleted.succeeded);
    assert(mparser::runtimeDimensions(deleted.value) ==
           std::vector<size_t>({1, 2}));
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               deleted.value, 0)) == 1);
    assert(idOf(*mparser::runtimeObjectLogicalElement(
               deleted.value, 1)) == 3);

    const auto invalidLinearDelete =
        mparser::runtimeDeleteObjectIndexed(
            matrix.value, {number(2)}, {false});
    assert(!invalidLinearDelete.succeeded);
}

void testSpecialObjectsRemainIsolated() {
    const auto metadata = mparser::makeRuntimeMetadataObject(
        mparser::RuntimeMetadataKind::Class, "ValueItem");
    assert(!mparser::isRuntimeClassObject(metadata));
    assert(mparser::runtimeObjectElementCount(metadata) == 0);
}

} // namespace

int main() {
    testStorageAndIndexing();
    testValueAndHandleAssignment();
    testHeterogeneousArraysAndDeletion();
    testSpecialObjectsRemainIsolated();
    return 0;
}
