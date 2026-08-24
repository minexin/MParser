#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/runtime/core/indexing/runtime_assignment.h"
#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/runtime/core/value/runtime_value.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

mparser::RuntimeValue number(double value) {
    return mparser::makeRuntimeNumberValue(value);
}

mparser::RuntimeValue vector(std::vector<double> values) {
    return mparser::makeRuntimeVectorValue(std::move(values));
}

mparser::RuntimeSparseOperationResult constructTriplet() {
    return mparser::runtimeConstructSparse({
        vector({1, 3, 3}), vector({1, 2, 2}), vector({4, 5, -2}),
        number(3), number(3), number(8),
    });
}

void assertElement(const mparser::RuntimeValue& value, size_t index,
                  double expected) {
    const auto element =
        mparser::runtimeNumericElementValue(value, index);
    assert(element);
    assert(!element->complex);
    assert(element->real == expected);
}

} // namespace

int main() {
    const auto dense = mparser::makeRuntimeMatrixValue(
        3, 3, {1, 0, 0, 0, 2, 0, 0, 0, 3});
    const auto sparse = mparser::runtimeSparseFromNumeric(dense);
    assert(sparse.succeeded);
    assert(mparser::isRuntimeSparseValue(sparse.value));
    assert(sparse.value.sparseStorage);
    assert(sparse.value.sparseStorage->values.size() == 3);
    assert(sparse.value.sparseStorage->columnPointers ==
           std::vector<size_t>({0, 1, 2, 3}));
    std::string contractError;
    assert(mparser::validateRuntimeSparseStorage(
        sparse.value, contractError));
    assertElement(sparse.value, 0, 1);
    assertElement(sparse.value, 4, 2);
    assertElement(sparse.value, 8, 3);
    assertElement(sparse.value, 1, 0);

    const auto triplet = constructTriplet();
    assert(triplet.succeeded);
    assert(triplet.value.sparseStorage->values.size() == 2);
    assertElement(triplet.value, 0, 4);
    assertElement(triplet.value, 5, 3);
    const auto full = mparser::runtimeSparseToFull(triplet.value);
    assert(full.succeeded);
    assertElement(full.value, 6, 0);
    assertElement(full.value, 5, 3);
    const auto nonzeros = mparser::runtimeSparseNonzeros(triplet.value);
    assert(nonzeros.succeeded);
    assert(mparser::runtimeShapeElementCount(nonzeros.value) == 2);
    assertElement(nonzeros.value, 0, 4);
    assertElement(nonzeros.value, 1, 3);

    const auto eye = mparser::runtimeSpeye({number(3)});
    assert(eye.succeeded);
    assert(eye.value.sparseStorage->values.size() == 3);
    const auto ones = mparser::runtimeSparseSpones(triplet.value);
    assert(ones.succeeded);
    assert(ones.value.sparseStorage->values.size() == 2);

    auto assigned = triplet.value;
    const auto assignment = mparser::runtimeAssignNumericIndexed(
        assigned, {number(2), number(3)}, number(7));
    assert(assignment.succeeded);
    assert(mparser::isRuntimeSparseValue(assigned));
    assertElement(assigned, 7, 7);
    assert(triplet.value.sparseStorage->values.size() == 2);

    const auto indexed = mparser::runtimeIndexNumeric(
        assigned, {number(2), number(3)});
    assert(indexed.succeeded);
    assert(mparser::isRuntimeSparseValue(indexed.value));
    assertElement(indexed.value, 0, 7);

    auto removed = assigned;
    const auto removal = mparser::runtimeAssignNumericIndexed(
        removed, {number(2), number(3)}, number(0));
    assert(removal.succeeded);
    assert(mparser::isRuntimeSparseValue(removed));
    assert(removed.sparseStorage->values.size() == 2);

    const auto transposed = mparser::runtimeTransposeNumeric(
        assigned, false);
    assert(transposed.succeeded);
    assert(mparser::isRuntimeSparseValue(transposed.value));
    assertElement(transposed.value, 5, 7);

    const auto complexDense = mparser::makeRuntimeMatrixValue(
        2, 2, {0, 0, 0, 4});
    auto complex = complexDense;
    complex.numericComplex = true;
    complex.imaginaryElements = {0, 0, 0, 2};
    const auto complexSparse = mparser::runtimeSparseFromNumeric(complex);
    assert(complexSparse.succeeded);
    assert(complexSparse.value.sparseStorage->complex);
    const auto complexFull = mparser::runtimeSparseToFull(
        complexSparse.value);
    assert(complexFull.succeeded);
    const auto complexElement =
        mparser::runtimeNumericElementValue(complexFull.value, 3);
    assert(complexElement && complexElement->complex);
    assert(complexElement->real == 4 && complexElement->imaginary == 2);
    const auto conjugated = mparser::runtimeTransposeNumeric(
        complexSparse.value, true);
    assert(conjugated.succeeded);
    const auto conjugatedElement =
        mparser::runtimeNumericElementValue(conjugated.value, 3);
    assert(conjugatedElement && conjugatedElement->complex);
    assert(conjugatedElement->real == 4 &&
           conjugatedElement->imaginary == -2);

    const auto singleDense = mparser::runtimeConvertNumericClass(
        dense, mparser::RuntimeNumericClass::Single);
    assert(singleDense);
    const auto singleSparse =
        mparser::runtimeSparseFromNumeric(*singleDense);
    assert(!singleSparse.succeeded);
    const auto singlePattern =
        mparser::runtimeSparseSpones(*singleDense);
    assert(singlePattern.succeeded);
    assert(mparser::isRuntimeSparseValue(singlePattern.value));
    assert(singlePattern.value.numericClass ==
           mparser::RuntimeNumericClass::Double);

    const auto logicalDense = mparser::runtimeConvertNumericClass(
        dense, mparser::RuntimeNumericClass::Logical);
    assert(logicalDense);
    const auto logicalSparse =
        mparser::runtimeSparseFromNumeric(*logicalDense);
    assert(logicalSparse.succeeded);
    assert(logicalSparse.value.numericClass ==
           mparser::RuntimeNumericClass::Logical);

    const auto emptyTriplet = mparser::runtimeConstructSparse({
        vector({}), vector({}), number(1),
    });
    assert(emptyTriplet.succeeded);
    assert(mparser::runtimeDimensions(emptyTriplet.value) ==
           std::vector<size_t>({0, 0}));

    const auto registry = mparser::defaultBuiltinRegistry();
    const auto* descriptor = registry->find("sparse");
    assert(descriptor);
    assert(descriptor->implementation ==
           mparser::BuiltinImplementationKind::Shared);
    assert(!descriptor->argumentConstraints.empty());
    assert(descriptor->argumentConstraints.front().value ==
           mparser::BuiltinValueConstraint::Numeric);
    std::vector<mparser::RuntimeValue> builtinArguments{dense};
    mparser::BuiltinCall call{builtinArguments, 1, {}, nullptr};
    const auto builtinResult = descriptor->handler(call);
    assert(builtinResult.succeeded);
    assert(builtinResult.outputs.size() == 1);
    assert(mparser::isRuntimeSparseValue(builtinResult.outputs.front()));

    const auto* fullDescriptor = registry->find("full");
    assert(fullDescriptor);
    std::vector<mparser::RuntimeValue> fullArguments{dense};
    mparser::BuiltinCall fullCall{fullArguments, 1, {}, nullptr};
    const auto fullBuiltinResult = fullDescriptor->handler(fullCall);
    assert(fullBuiltinResult.succeeded);
    assert(fullBuiltinResult.outputs.size() == 1);
    assert(mparser::runtimeValuesEqual(
        fullBuiltinResult.outputs.front(), dense));

    const auto* predicateDescriptor = registry->find("issparse");
    assert(predicateDescriptor);
    assert(predicateDescriptor->argumentConstraints.front().value ==
           mparser::BuiltinValueConstraint::Any);

    const auto valid =
        mparser::validateRuntimeValueContract(assigned);
    assert(valid.valid && valid.error.empty());

    auto invalidDense = dense;
    invalidDense.sparseStorage = assigned.sparseStorage;
    const auto invalidDenseContract =
        mparser::validateRuntimeValueContract(invalidDense);
    assert(!invalidDenseContract.valid);

    auto oversizedStorage =
        std::make_shared<mparser::RuntimeSparseStorage>();
    oversizedStorage->rows = 1;
    oversizedStorage->columns = std::numeric_limits<size_t>::max();
    auto oversizedSparse =
        mparser::makeRuntimeSparseValue(std::move(oversizedStorage));
    const auto oversizedContract =
        mparser::validateRuntimeValueContract(oversizedSparse);
    assert(!oversizedContract.valid);
    std::cout << "sparse runtime smoke passed: nnz="
              << assigned.sparseStorage->values.size() << '\n';
    return 0;
}
