#include "mparser/runtime/core/value/runtime_dense_numeric.h"

#include "mparser/runtime/core/value/runtime_shape.h"

#include <limits>
#include <utility>

namespace mparser {
namespace {

RuntimeNumericOperationResult failure(std::string message) {
    return RuntimeNumericOperationResult{false, {}, std::move(message)};
}

RuntimeNumericOperationResult success(RuntimeValue value) {
    return RuntimeNumericOperationResult{true, std::move(value), {}};
}

RuntimeNumericElementValue denseNumericElementValue(
    native_numeric::Complex value, RuntimeNumericClass numericClass,
    bool preserveComplex = false) {
    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    if (numericClass == RuntimeNumericClass::Single) {
        result.real = static_cast<double>(
            static_cast<float>(value.real()));
        result.imaginary = static_cast<double>(
            static_cast<float>(value.imag()));
    } else {
        result.real = value.real();
        result.imaginary = value.imag();
    }
    result.complex = preserveComplex || result.imaginary != 0.0;
    return result;
}

} // namespace

bool isRuntimeFloatingNumericValue(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           runtimeNumericClassIsFloating(value.numericClass);
}

RuntimeNumericClass runtimeFloatingNumericResultClass(
    std::initializer_list<const RuntimeValue*> values) {
    for (const RuntimeValue* value : values) {
        if (value && value->numericClass == RuntimeNumericClass::Single) {
            return RuntimeNumericClass::Single;
        }
    }
    return RuntimeNumericClass::Double;
}

std::optional<native_numeric::Complex> runtimeDenseNumericElement(
    const RuntimeValue& value, size_t logicalIndex) {
    const auto element = runtimeNumericElementValue(value, logicalIndex);
    if (!element) {
        return std::nullopt;
    }
    return native_numeric::Complex(
        element->real, element->complex ? element->imaginary : 0.0);
}

std::optional<RuntimeValue> makeRuntimeDenseNumericValue(
    std::vector<size_t> dimensions,
    const std::vector<native_numeric::Complex>& values,
    RuntimeNumericClass numericClass, bool preserveComplex) {
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(values.size());
    for (const auto value : values) {
        elements.push_back(denseNumericElementValue(
            value, numericClass, preserveComplex));
    }
    return runtimeNumericValueFromElements(
        std::move(dimensions), std::move(elements), numericClass);
}

std::optional<RuntimeValue> makeRuntimeDenseNumericValue(
    std::vector<size_t> dimensions,
    const native_numeric::Matrix& matrix,
    RuntimeNumericClass numericClass, bool preserveComplex) {
    return makeRuntimeDenseNumericValue(
        std::move(dimensions), matrix.values(), numericClass,
        preserveComplex);
}

std::optional<RuntimeDenseMatrixInput> runtimeDenseMatrixInput(
    const RuntimeValue& value, std::string& error) {
    if (!isRuntimeFloatingNumericValue(value)) {
        error = "input must be a single or double numeric array";
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
        error = "input must be two-dimensional";
        return std::nullopt;
    }
    native_numeric::Matrix matrix(dimensions[0], dimensions[1]);
    for (size_t column = 0; column < dimensions[1]; ++column) {
        for (size_t row = 0; row < dimensions[0]; ++row) {
            const auto element = runtimeDenseNumericElement(
                value, row + column * dimensions[0]);
            if (!element) {
                error = "matrix input contains an unreadable element";
                return std::nullopt;
            }
            matrix(row, column) = *element;
        }
    }
    return RuntimeDenseMatrixInput{
        std::move(matrix), value.numericClass, value.numericComplex};
}

RuntimeNumericOperationResult runtimeApplyDenseMatrixDivision(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    const bool rightDivision = operation == "/";
    if (!rightDivision && operation != "\\") {
        return failure("unsupported dense matrix division operator: " +
                       std::string(operation));
    }

    std::string error;
    const auto leftInput = runtimeDenseMatrixInput(left, error);
    if (!leftInput) {
        return failure("left matrix " + error);
    }
    const auto rightInput = runtimeDenseMatrixInput(right, error);
    if (!rightInput) {
        return failure("right matrix " + error);
    }

    const RuntimeNumericClass outputClass =
        runtimeFloatingNumericResultClass({&left, &right});
    native_numeric::SolveResult solved;
    std::vector<size_t> dimensions;
    const double epsilon =
        left.numericClass == RuntimeNumericClass::Single ||
                right.numericClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon();
    if (!rightDivision) {
        if (leftInput->matrix.rows() != rightInput->matrix.rows()) {
            return failure(
                "matrix left division requires equal row counts");
        }
        solved = native_numeric::solve(
            leftInput->matrix, rightInput->matrix, epsilon);
        dimensions = {
            leftInput->matrix.columns(), rightInput->matrix.columns()};
    } else {
        if (leftInput->matrix.columns() != rightInput->matrix.columns()) {
            return failure(
                "matrix right division requires equal column counts");
        }
        const auto transposedRight =
            native_numeric::adjoint(rightInput->matrix);
        const auto transposedLeft =
            native_numeric::adjoint(leftInput->matrix);
        solved = native_numeric::solve(
            transposedRight, transposedLeft, epsilon);
        dimensions = {
            leftInput->matrix.rows(), rightInput->matrix.rows()};
    }
    if (!solved.succeeded) {
        return failure("matrix division failed: " + solved.error);
    }
    native_numeric::Matrix solution =
        rightDivision ? native_numeric::adjoint(solved.solution)
                      : std::move(solved.solution);
    const bool preserveComplex = leftInput->complex || rightInput->complex;
    auto value = makeRuntimeDenseNumericValue(
        dimensions, solution, outputClass, preserveComplex);
    return value ? success(std::move(*value))
                 : failure(
                       "matrix division could not construct its result");
}

} // namespace mparser
