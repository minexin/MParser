#include "mparser/runtime_advanced_numeric.h"

#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_native_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

using Complex = native_numeric::Complex;
using ComplexMatrix = native_numeric::Matrix;

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier) {
    return BuiltinResult::failure(call.span, std::move(message),
                                  std::move(identifier));
}

BuiltinResult outputsFor(const BuiltinCall& call,
                         std::vector<RuntimeValue> outputs) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "advanced numeric builtin produced an unexpected "
                       "output count",
                       "MParser:AdvancedNumericContractViolation");
    }
    return BuiltinResult::success(std::move(outputs));
}

RuntimeNumericOperationResult operationFailure(std::string message) {
    return RuntimeNumericOperationResult{false, {}, std::move(message)};
}

RuntimeNumericOperationResult operationSuccess(RuntimeValue value) {
    return RuntimeNumericOperationResult{true, std::move(value), {}};
}

bool checkpoint(const BuiltinCall& call) {
    return !call.context || !call.context->executionControl ||
           call.context->executionControl->checkpoint();
}

bool periodicCheckpoint(const BuiltinCall& call, size_t index,
                        size_t interval = 16384U) {
    return index % interval != 0 || checkpoint(call);
}

bool observeOutput(const BuiltinCall& call, size_t count,
                   RuntimeNumericClass numericClass,
                   bool complex = false) {
    size_t bytesPerElement = sizeof(double);
    if (runtimeNumericClassIsInteger(numericClass)) {
        bytesPerElement += sizeof(std::uint64_t);
    }
    if (complex) {
        if (bytesPerElement >
            std::numeric_limits<size_t>::max() / 2U) {
            return false;
        }
        bytesPerElement *= 2U;
    }
    if (count >
        std::numeric_limits<size_t>::max() / bytesPerElement) {
        return false;
    }
    return !call.context || !call.context->executionControl ||
           call.context->executionControl->observeArrayBytes(
               count * bytesPerElement);
}

BuiltinResult stopped(const BuiltinCall& call, std::string_view name) {
    return failure(call,
                   std::string(name) +
                       " execution was stopped by runtime control",
                   "MParser:ExecutionStopped");
}

bool isFloatingNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           runtimeNumericClassIsFloating(value.numericClass);
}

RuntimeNumericClass floatingResultClass(
    const std::vector<const RuntimeValue*>& values) {
    for (const RuntimeValue* value : values) {
        if (value && value->numericClass == RuntimeNumericClass::Single) {
            return RuntimeNumericClass::Single;
        }
    }
    return RuntimeNumericClass::Double;
}

std::optional<size_t> positiveDimension(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex || !std::isfinite(element->real) ||
        std::floor(element->real) != element->real ||
        element->real < 1.0 ||
        element->real > static_cast<double>(
                            std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<size_t>(element->real - 1.0);
}

std::optional<size_t> nonnegativeSize(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    return checkedRuntimeNonnegativeInteger(element->real);
}

std::optional<double> realScalar(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    return element->real;
}

std::optional<Complex> numericElement(const RuntimeValue& value,
                                      size_t logicalIndex) {
    const auto element = runtimeNumericElementValue(value, logicalIndex);
    if (!element) {
        return std::nullopt;
    }
    return Complex(element->real,
                   element->complex ? element->imaginary : 0.0);
}

RuntimeNumericElementValue numericElementValue(
    Complex value, RuntimeNumericClass numericClass,
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

std::optional<RuntimeValue> makeNumericValue(
    std::vector<size_t> dimensions, const std::vector<Complex>& values,
    RuntimeNumericClass numericClass, bool preserveComplex = false) {
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(values.size());
    for (const Complex value : values) {
        elements.push_back(numericElementValue(
            value, numericClass, preserveComplex));
    }
    return runtimeNumericValueFromElements(
        std::move(dimensions), std::move(elements), numericClass);
}

std::optional<RuntimeValue> makeNumericValue(
    std::vector<size_t> dimensions, const ComplexMatrix& matrix,
    RuntimeNumericClass numericClass, bool preserveComplex = false) {
    return makeNumericValue(std::move(dimensions), matrix.values(),
                            numericClass, preserveComplex);
}

struct MatrixInput {
    ComplexMatrix matrix;
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double;
    bool complex = false;
};

std::optional<MatrixInput> matrixInput(const RuntimeValue& value,
                                       std::string& error) {
    if (!isFloatingNumeric(value)) {
        error = "input must be a single or double numeric array";
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
        error = "input must be two-dimensional";
        return std::nullopt;
    }
    ComplexMatrix matrix(dimensions[0], dimensions[1]);
    for (size_t column = 0; column < dimensions[1]; ++column) {
        for (size_t row = 0; row < dimensions[0]; ++row) {
            const auto element = numericElement(
                value, row + column * dimensions[0]);
            if (!element) {
                error = "matrix input contains an unreadable element";
                return std::nullopt;
            }
            matrix(row, column) = *element;
        }
    }
    return MatrixInput{std::move(matrix), value.numericClass,
                       value.numericComplex};
}

RuntimeNumericOperationResult solveMatrices(
    const RuntimeValue& left, const RuntimeValue& right,
    bool rightDivision) {
    std::string error;
    const auto leftInput = matrixInput(left, error);
    if (!leftInput) {
        return operationFailure("left matrix " + error);
    }
    const auto rightInput = matrixInput(right, error);
    if (!rightInput) {
        return operationFailure("right matrix " + error);
    }

    const RuntimeNumericClass outputClass =
        floatingResultClass({&left, &right});
    native_numeric::SolveResult solved;
    std::vector<size_t> dimensions;
    if (!rightDivision) {
        if (leftInput->matrix.rows() != rightInput->matrix.rows()) {
            return operationFailure(
                "matrix left division requires equal row counts");
        }
        solved = native_numeric::solve(
            leftInput->matrix, rightInput->matrix,
            left.numericClass == RuntimeNumericClass::Single ||
                    right.numericClass == RuntimeNumericClass::Single
                ? static_cast<double>(std::numeric_limits<float>::epsilon())
                : std::numeric_limits<double>::epsilon());
        dimensions = {
            leftInput->matrix.columns(), rightInput->matrix.columns()};
    } else {
        if (leftInput->matrix.columns() != rightInput->matrix.columns()) {
            return operationFailure(
                "matrix right division requires equal column counts");
        }
        const ComplexMatrix transposedRight =
            native_numeric::adjoint(rightInput->matrix);
        const ComplexMatrix transposedLeft =
            native_numeric::adjoint(leftInput->matrix);
        solved = native_numeric::solve(
            transposedRight, transposedLeft,
            left.numericClass == RuntimeNumericClass::Single ||
                    right.numericClass == RuntimeNumericClass::Single
                ? static_cast<double>(std::numeric_limits<float>::epsilon())
                : std::numeric_limits<double>::epsilon());
        dimensions = {
            leftInput->matrix.rows(), rightInput->matrix.rows()};
    }
    if (!solved.succeeded) {
        return operationFailure("matrix division failed: " + solved.error);
    }
    ComplexMatrix solution = rightDivision
                                 ? native_numeric::adjoint(solved.solution)
                                 : std::move(solved.solution);
    const bool preserveComplex =
        leftInput->complex || rightInput->complex;
    auto value = makeNumericValue(dimensions, solution, outputClass,
                                  preserveComplex);
    return value ? operationSuccess(std::move(*value))
                 : operationFailure(
                       "matrix division could not construct its result");
}

BuiltinResult detBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto input = matrixInput(call.arguments.front(), error);
    if (!input || input->matrix.rows() != input->matrix.columns()) {
        return failure(call,
                       "det expects a square single or double matrix" +
                           (error.empty() ? std::string{} : ": " + error),
                       "MParser:InvalidDetInput");
    }
    if (!checkpoint(call) || !observeOutput(call, 1, input->numericClass,
                                             input->complex)) {
        return stopped(call, "det");
    }
    const auto determinant = native_numeric::determinant(
        input->matrix,
        input->numericClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon());
    if (!determinant) {
        return failure(call, "det could not factor its input",
                       "MParser:InvalidDetResult");
    }
    auto value = makeNumericValue({1, 1}, {*determinant},
                                  input->numericClass, input->complex);
    return value ? outputsFor(call, {std::move(*value)})
                 : failure(call, "det could not construct its result",
                           "MParser:InvalidDetResult");
}

BuiltinResult invBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto input = matrixInput(call.arguments.front(), error);
    if (!input || input->matrix.rows() != input->matrix.columns()) {
        return failure(call,
                       "inv expects a square single or double matrix" +
                           (error.empty() ? std::string{} : ": " + error),
                       "MParser:InvalidInvInput");
    }
    const size_t count = static_cast<size_t>(input->matrix.size());
    if (!checkpoint(call) ||
        !observeOutput(call, count, input->numericClass, input->complex)) {
        return stopped(call, "inv");
    }
    const auto inverse = native_numeric::inverse(
        input->matrix,
        input->numericClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon());
    if (!inverse) {
        return failure(call, "inv input matrix is singular",
                       "MParser:SingularMatrix");
    }
    auto value = makeNumericValue(runtimeDimensions(call.arguments.front()),
                                  *inverse, input->numericClass,
                                  input->complex);
    return value ? outputsFor(call, {std::move(*value)})
                 : failure(call, "inv could not construct its result",
                           "MParser:InvalidInvResult");
}

BuiltinResult traceBuiltin(const BuiltinCall& call) {
    const RuntimeValue& value = call.arguments.front();
    if (!isRuntimeNumericValue(value) ||
        runtimeDimensions(value).size() != 2 ||
        (value.numericComplex &&
         runtimeNumericClassIsInteger(value.numericClass))) {
        return failure(call, "trace expects a two-dimensional numeric array",
                       "MParser:InvalidTraceInput");
    }
    const auto dimensions = runtimeDimensions(value);
    const size_t count = std::min(dimensions[0], dimensions[1]);
    RuntimeNumericElementValue accumulator;
    accumulator.numericClass = value.numericClass;
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(
            value, index + index * dimensions[0]);
        if (!element) {
            return failure(call, "trace could not read an input element",
                           "MParser:InvalidTraceInput");
        }
        const auto sum = runtimeApplyNumericElementBinary(
            "+", accumulator, *element, value.numericClass);
        if (!sum) {
            return failure(call, "trace result is not representable",
                           "MParser:InvalidTraceResult");
        }
        accumulator = *sum;
    }
    auto result = runtimeNumericValueFromElements(
        {1, 1}, {accumulator}, value.numericClass);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "trace could not construct its result",
                            "MParser:InvalidTraceResult");
}

BuiltinResult normBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto input = matrixInput(call.arguments.front(), error);
    if (!input) {
        return failure(call, "norm " + error,
                       "MParser:InvalidNormInput");
    }
    enum class Kind { One, Two, Infinity, NegativeInfinity, Frobenius, P };
    Kind kind = Kind::Two;
    double p = 2.0;
    if (call.arguments.size() == 2) {
        if (const auto text = runtimeTextScalarUtf8(call.arguments[1])) {
            if (*text != "fro") {
                return failure(call, "norm text option must be 'fro'",
                               "MParser:InvalidNormOption");
            }
            kind = Kind::Frobenius;
        } else {
            const auto requested = realScalar(call.arguments[1]);
            if (!requested || std::isnan(*requested)) {
                return failure(call,
                               "norm order must be a real numeric scalar",
                               "MParser:InvalidNormOption");
            }
            p = *requested;
            if (p == 1.0) {
                kind = Kind::One;
            } else if (p == 2.0) {
                kind = Kind::Two;
            } else if (p == std::numeric_limits<double>::infinity()) {
                kind = Kind::Infinity;
            } else if (p == -std::numeric_limits<double>::infinity()) {
                kind = Kind::NegativeInfinity;
            } else {
                kind = Kind::P;
            }
        }
    }
    const bool vector = input->matrix.rows() == 1 ||
                        input->matrix.columns() == 1;
    double result = 0.0;
    if (vector) {
        std::vector<double> magnitudes;
        magnitudes.reserve(input->matrix.size());
        for (const Complex value : input->matrix.values()) {
            magnitudes.push_back(std::abs(value));
        }
        if (kind == Kind::Infinity) {
            result = magnitudes.empty()
                         ? 0.0
                         : *std::max_element(magnitudes.begin(),
                                             magnitudes.end());
        } else if (kind == Kind::NegativeInfinity) {
            result = magnitudes.empty()
                         ? 0.0
                         : *std::min_element(magnitudes.begin(),
                                             magnitudes.end());
        } else if (kind == Kind::One) {
            result = std::accumulate(magnitudes.begin(), magnitudes.end(),
                                     0.0);
        } else if (kind == Kind::Two || kind == Kind::Frobenius) {
            result = native_numeric::frobeniusNorm(input->matrix);
        } else {
            if (!(p >= 1.0) || !std::isfinite(p)) {
                return failure(call,
                               "vector norm order must be at least one",
                               "MParser:InvalidNormOption");
            }
            long double sum = 0.0L;
            for (const double magnitude : magnitudes) {
                sum += std::pow(static_cast<long double>(magnitude), p);
            }
            result = static_cast<double>(std::pow(sum, 1.0L / p));
        }
    } else if (kind == Kind::Frobenius) {
        result = native_numeric::frobeniusNorm(input->matrix);
    } else if (kind == Kind::One) {
        result = native_numeric::oneNorm(input->matrix);
    } else if (kind == Kind::Infinity) {
        result = native_numeric::infinityNorm(input->matrix);
    } else if (kind == Kind::Two) {
        const auto singularValues = native_numeric::singularValues(
            input->matrix,
            input->numericClass == RuntimeNumericClass::Single
                ? static_cast<double>(std::numeric_limits<float>::epsilon())
                : std::numeric_limits<double>::epsilon());
        result = singularValues.empty() ? 0.0 : singularValues.front();
    } else {
        return failure(call,
                       "matrix norm supports orders 1, 2, Inf, or 'fro'",
                       "MParser:InvalidNormOption");
    }
    if (!checkpoint(call) ||
        !observeOutput(call, 1, input->numericClass)) {
        return stopped(call, "norm");
    }
    auto value = makeNumericValue({1, 1}, {Complex(result, 0.0)},
                                  input->numericClass);
    return value ? outputsFor(call, {std::move(*value)})
                 : failure(call, "norm could not construct its result",
                           "MParser:InvalidNormResult");
}

BuiltinResult rankBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto input = matrixInput(call.arguments.front(), error);
    if (!input) {
        return failure(call, "rank " + error,
                       "MParser:InvalidRankInput");
    }
    const auto singularValues = native_numeric::singularValues(
        input->matrix,
        input->numericClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon());
    double tolerance = 0.0;
    if (call.arguments.size() == 2) {
        const auto requested = realScalar(call.arguments[1]);
        if (!requested || !std::isfinite(*requested) || *requested < 0.0) {
            return failure(call,
                           "rank tolerance must be a nonnegative real scalar",
                           "MParser:InvalidRankTolerance");
        }
        tolerance = *requested;
    } else if (!singularValues.empty()) {
        const double epsilon =
            input->numericClass == RuntimeNumericClass::Single
                ? static_cast<double>(std::numeric_limits<float>::epsilon())
                : std::numeric_limits<double>::epsilon();
        tolerance = static_cast<double>(std::max(
                        input->matrix.rows(), input->matrix.columns())) *
                    epsilon * singularValues.front();
    }
    size_t rank = 0;
    for (const double singularValue : singularValues) {
        rank += singularValue > tolerance ? 1U : 0U;
    }
    if (!checkpoint(call) ||
        !observeOutput(call, 1, RuntimeNumericClass::Double)) {
        return stopped(call, "rank");
    }
    return outputsFor(call, {makeRuntimeNumberValue(
                                static_cast<double>(rank))});
}

BuiltinResult eigBuiltin(const BuiltinCall& call) {
    std::string error;
    const auto input = matrixInput(call.arguments.front(), error);
    if (!input || input->matrix.rows() != input->matrix.columns()) {
        return failure(call,
                       "eig expects a square single or double matrix" +
                           (error.empty() ? std::string{} : ": " + error),
                       "MParser:InvalidEigInput");
    }
    const size_t size = input->matrix.rows();
    size_t outputElements = size;
    if (call.requestedOutputCount > 1) {
        if (size != 0 &&
            size > std::numeric_limits<size_t>::max() / size) {
            return failure(call, "eig output dimensions are too large",
                           "MParser:InvalidEigResult");
        }
        const size_t matrixElements = size * size;
        if (matrixElements > std::numeric_limits<size_t>::max() / 2U) {
            return failure(call, "eig output dimensions are too large",
                           "MParser:InvalidEigResult");
        }
        outputElements = matrixElements * 2U;
    }
    if (!checkpoint(call) ||
        !observeOutput(call, outputElements, input->numericClass, true)) {
        return stopped(call, "eig");
    }

    const auto decomposition = native_numeric::eigen(
        input->matrix,
        input->numericClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon(),
        call.requestedOutputCount > 1);
    if (!decomposition.converged) {
        return failure(call, "eig failed to converge",
                       "MParser:EigNoConvergence");
    }

    const std::vector<Complex>& diagonalValues = decomposition.values;
    bool complexResult = input->complex;
    for (const Complex value : diagonalValues) {
        complexResult = complexResult || value.imag() != 0.0;
    }
    for (const Complex value : decomposition.vectors.values()) {
        complexResult = complexResult || value.imag() != 0.0;
    }
    if (call.requestedOutputCount <= 1) {
        auto values = makeNumericValue({size, 1}, diagonalValues,
                                       input->numericClass, complexResult);
        return values ? outputsFor(call, {std::move(*values)})
                      : failure(call, "eig could not construct eigenvalues",
                                "MParser:InvalidEigResult");
    }
    ComplexMatrix diagonal(size, size);
    for (size_t index = 0; index < size; ++index) {
        diagonal(index, index) = diagonalValues[index];
    }
    auto vectors = makeNumericValue({size, size}, decomposition.vectors,
                                    input->numericClass, complexResult);
    auto values = makeNumericValue({size, size}, diagonal,
                                   input->numericClass, complexResult);
    if (!vectors || !values) {
        return failure(call, "eig could not construct its outputs",
                       "MParser:InvalidEigResult");
    }
    return outputsFor(call,
                      {std::move(*vectors), std::move(*values)});
}

enum class MissingPolicy { Include, Omit };

struct ReductionSelection {
    bool specified = false;
    bool all = false;
    std::vector<size_t> dimensions;
};

bool parseMissingPolicy(std::string_view text, MissingPolicy& policy) {
    if (text == "omitnan" || text == "omitmissing") {
        policy = MissingPolicy::Omit;
        return true;
    }
    if (text == "includenan" || text == "includemissing") {
        policy = MissingPolicy::Include;
        return true;
    }
    return false;
}

bool parseReductionSelection(const RuntimeValue& value,
                             ReductionSelection& selection,
                             std::string& error) {
    if (selection.specified) {
        error = "reduction dimensions were specified more than once";
        return false;
    }
    selection.specified = true;
    if (const auto text = runtimeTextScalarUtf8(value)) {
        if (*text != "all") {
            error = "reduction dimension text must be 'all'";
            return false;
        }
        selection.all = true;
        return true;
    }
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) == 0) {
        error = "reduction dimensions must be positive integers";
        return false;
    }
    for (size_t index = 0; index < runtimeShapeElementCount(value); ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element || element->complex || !std::isfinite(element->real) ||
            std::floor(element->real) != element->real ||
            element->real < 1.0 ||
            element->real > static_cast<double>(
                                std::numeric_limits<size_t>::max())) {
            error = "reduction dimensions must be positive integers";
            return false;
        }
        const size_t dimension = static_cast<size_t>(element->real - 1.0);
        if (std::find(selection.dimensions.begin(),
                      selection.dimensions.end(), dimension) !=
            selection.dimensions.end()) {
            error = "reduction dimensions must not repeat";
            return false;
        }
        selection.dimensions.push_back(dimension);
    }
    std::sort(selection.dimensions.begin(), selection.dimensions.end());
    return true;
}

std::vector<size_t> selectedDimensions(
    const std::vector<size_t>& inputDimensions,
    const ReductionSelection& selection) {
    if (selection.all) {
        std::vector<size_t> result(inputDimensions.size());
        std::iota(result.begin(), result.end(), size_t{0});
        return result;
    }
    if (selection.specified) {
        return selection.dimensions;
    }
    for (size_t index = 0; index < inputDimensions.size(); ++index) {
        if (inputDimensions[index] != 1) {
            return {index};
        }
    }
    return {0};
}

struct ReductionLayout {
    std::vector<size_t> inputDimensions;
    std::vector<size_t> outputDimensions;
    std::vector<bool> reduced;
    size_t outputCount = 0;
};

std::optional<ReductionLayout> reductionLayout(
    const RuntimeValue& input, const ReductionSelection& selection) {
    auto inputDimensions = runtimeDimensions(input);
    const auto chosen = selectedDimensions(inputDimensions, selection);
    size_t rank = inputDimensions.size();
    for (const size_t dimension : chosen) {
        if (dimension == std::numeric_limits<size_t>::max()) {
            return std::nullopt;
        }
        rank = std::max(rank, dimension + 1);
    }
    inputDimensions.resize(rank, 1);
    std::vector<size_t> outputDimensions = inputDimensions;
    std::vector<bool> reduced(rank, false);
    for (const size_t dimension : chosen) {
        reduced[dimension] = true;
        outputDimensions[dimension] = 1;
    }
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto count = checkedRuntimeDimensionProduct(outputDimensions);
    if (!count) {
        return std::nullopt;
    }
    return ReductionLayout{std::move(inputDimensions),
                           std::move(outputDimensions),
                           std::move(reduced), *count};
}

std::optional<size_t> reductionBucketIndex(
    size_t inputIndex, const ReductionLayout& layout) {
    auto coordinates = runtimeColumnMajorCoordinates(
        inputIndex, layout.inputDimensions);
    if (!coordinates) {
        return std::nullopt;
    }
    for (size_t index = 0; index < layout.reduced.size(); ++index) {
        if (layout.reduced[index]) {
            (*coordinates)[index] = 0;
        }
    }
    coordinates->resize(layout.outputDimensions.size(), 0);
    return runtimeColumnMajorLinearIndex(*coordinates,
                                         layout.outputDimensions);
}

bool numericMissing(const RuntimeNumericElementValue& value) {
    return std::isnan(value.real) ||
           (value.complex && std::isnan(value.imaginary));
}

BuiltinResult medianBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input) ||
        (input.numericComplex &&
         runtimeNumericClassIsInteger(input.numericClass))) {
        return failure(call, "median expects a numeric array",
                       "MParser:InvalidMedianInput");
    }
    ReductionSelection selection;
    MissingPolicy missingPolicy = MissingPolicy::Include;
    bool outputTypeSpecified = false;
    RuntimeNumericClass outputClass = input.numericClass;
    std::string error;
    for (size_t index = 1; index < call.arguments.size(); ++index) {
        const RuntimeValue& argument = call.arguments[index];
        const auto text = runtimeTextScalarUtf8(argument);
        if (!text) {
            if (!parseReductionSelection(argument, selection, error)) {
                return failure(call, "median " + error,
                               "MParser:InvalidMedianOption");
            }
            continue;
        }
        if (*text == "all") {
            if (!parseReductionSelection(argument, selection, error)) {
                return failure(call, "median " + error,
                               "MParser:InvalidMedianOption");
            }
        } else if (parseMissingPolicy(*text, missingPolicy)) {
        } else if (*text == "native" || *text == "double") {
            if (outputTypeSpecified) {
                return failure(call,
                               "median output type was specified twice",
                               "MParser:InvalidMedianOption");
            }
            outputTypeSpecified = true;
            outputClass = *text == "double"
                              ? RuntimeNumericClass::Double
                              : input.numericClass;
        } else {
            return failure(call, "unsupported median option: " + *text,
                           "MParser:InvalidMedianOption");
        }
    }
    const auto layout = reductionLayout(input, selection);
    if (!layout || !checkpoint(call) ||
        !observeOutput(call, layout ? layout->outputCount : 0,
                       outputClass, input.numericComplex)) {
        return layout ? stopped(call, "median")
                      : failure(call, "median dimensions are too large",
                                "MParser:InvalidMedianShape");
    }
    std::vector<std::vector<RuntimeNumericElementValue>> buckets(
        layout->outputCount);
    const size_t inputCount = runtimeShapeElementCount(input);
    for (size_t index = 0; index < inputCount; ++index) {
        if (!periodicCheckpoint(call, index)) {
            return stopped(call, "median");
        }
        const auto element = runtimeNumericElementValue(input, index);
        const auto bucket = reductionBucketIndex(index, *layout);
        if (!element || !bucket || *bucket >= buckets.size()) {
            return failure(call, "median could not map an input element",
                           "MParser:InvalidMedianShape");
        }
        if (missingPolicy == MissingPolicy::Omit &&
            numericMissing(*element)) {
            continue;
        }
        buckets[*bucket].push_back(*element);
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(buckets.size());
    for (auto& bucket : buckets) {
        if (bucket.empty()) {
            RuntimeNumericElementValue missing;
            missing.numericClass = RuntimeNumericClass::Double;
            missing.real = std::numeric_limits<double>::quiet_NaN();
            values.push_back(missing);
            if (runtimeNumericClassIsInteger(outputClass) ||
                outputClass == RuntimeNumericClass::Logical) {
                outputClass = RuntimeNumericClass::Double;
            }
            continue;
        }
        const auto missing = std::find_if(
            bucket.begin(), bucket.end(), numericMissing);
        if (missing != bucket.end()) {
            values.push_back(*missing);
            continue;
        }
        std::stable_sort(
            bucket.begin(), bucket.end(),
            [](const RuntimeNumericElementValue& left,
               const RuntimeNumericElementValue& right) {
                return runtimeCompareNumericElementsForExtrema(left,
                                                                right) < 0;
            });
        const size_t upper = bucket.size() / 2;
        RuntimeNumericElementValue selected = bucket[upper];
        if (bucket.size() % 2 == 0) {
            selected.numericClass = RuntimeNumericClass::Double;
            selected.real = (bucket[upper - 1].real +
                             bucket[upper].real) /
                            2.0;
            selected.imaginary = (bucket[upper - 1].imaginary +
                                  bucket[upper].imaginary) /
                                 2.0;
            selected.complex = bucket[upper - 1].complex ||
                               bucket[upper].complex;
            const auto converted = runtimeConvertNumericElementValue(
                selected, outputClass);
            if (!converted) {
                return failure(call,
                               "median result is not representable in its "
                               "output class",
                               "MParser:InvalidMedianResult");
            }
            selected = *converted;
        }
        values.push_back(selected);
    }
    auto result = runtimeNumericValueFromElements(
        layout->outputDimensions, std::move(values), outputClass);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "median could not construct its result",
                            "MParser:InvalidMedianResult");
}

struct VarianceBucket {
    Complex sum{};
    size_t count = 0;
    bool missing = false;
};

BuiltinResult varianceBuiltin(std::string_view name,
                              const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isFloatingNumeric(input)) {
        return failure(call,
                       std::string(name) +
                           " expects a single or double numeric array",
                       "MParser:InvalidVarianceInput");
    }
    bool population = false;
    bool weightParsed = false;
    ReductionSelection selection;
    MissingPolicy missingPolicy = MissingPolicy::Include;
    std::string error;
    for (size_t index = 1; index < call.arguments.size(); ++index) {
        const RuntimeValue& argument = call.arguments[index];
        if (const auto text = runtimeTextScalarUtf8(argument)) {
            if (*text == "all") {
                if (!parseReductionSelection(argument, selection, error)) {
                    return failure(call, std::string(name) + " " + error,
                                   "MParser:InvalidVarianceOption");
                }
            } else if (!parseMissingPolicy(*text, missingPolicy)) {
                return failure(call,
                               "unsupported " + std::string(name) +
                                   " option: " + *text,
                               "MParser:InvalidVarianceOption");
            }
            continue;
        }
        if (!weightParsed) {
            weightParsed = true;
            if (runtimeShapeElementCount(argument) == 0) {
                continue;
            }
            const auto weight = realScalar(argument);
            if (!weight || (*weight != 0.0 && *weight != 1.0)) {
                return failure(call,
                               std::string(name) +
                                   " normalization must be 0 or 1",
                               "MParser:InvalidVarianceOption");
            }
            population = *weight == 1.0;
            continue;
        }
        if (!parseReductionSelection(argument, selection, error)) {
            return failure(call, std::string(name) + " " + error,
                           "MParser:InvalidVarianceOption");
        }
    }
    const auto layout = reductionLayout(input, selection);
    if (!layout || !checkpoint(call) ||
        !observeOutput(call, layout ? layout->outputCount : 0,
                       input.numericClass)) {
        return layout ? stopped(call, name)
                      : failure(call,
                                std::string(name) +
                                    " dimensions are too large",
                                "MParser:InvalidVarianceShape");
    }
    std::vector<VarianceBucket> buckets(layout->outputCount);
    const size_t inputCount = runtimeShapeElementCount(input);
    for (size_t index = 0; index < inputCount; ++index) {
        if (!periodicCheckpoint(call, index)) {
            return stopped(call, name);
        }
        const auto element = runtimeNumericElementValue(input, index);
        const auto bucketIndex = reductionBucketIndex(index, *layout);
        if (!element || !bucketIndex || *bucketIndex >= buckets.size()) {
            return failure(call,
                           std::string(name) +
                               " could not map an input element",
                           "MParser:InvalidVarianceShape");
        }
        if (numericMissing(*element)) {
            if (missingPolicy == MissingPolicy::Include) {
                buckets[*bucketIndex].missing = true;
            }
            continue;
        }
        buckets[*bucketIndex].sum +=
            Complex(element->real,
                    element->complex ? element->imaginary : 0.0);
        ++buckets[*bucketIndex].count;
    }
    std::vector<double> squaredDeviation(buckets.size(), 0.0);
    for (size_t index = 0; index < inputCount; ++index) {
        const auto element = runtimeNumericElementValue(input, index);
        const auto bucketIndex = reductionBucketIndex(index, *layout);
        if (!element || !bucketIndex ||
            numericMissing(*element) || buckets[*bucketIndex].count == 0) {
            continue;
        }
        const Complex mean = buckets[*bucketIndex].sum /
                             static_cast<double>(
                                 buckets[*bucketIndex].count);
        const Complex value(element->real,
                            element->complex ? element->imaginary : 0.0);
        squaredDeviation[*bucketIndex] += std::norm(value - mean);
    }
    std::vector<Complex> values;
    values.reserve(buckets.size());
    for (size_t index = 0; index < buckets.size(); ++index) {
        const VarianceBucket& bucket = buckets[index];
        double value = std::numeric_limits<double>::quiet_NaN();
        if (!bucket.missing && bucket.count != 0) {
            const size_t denominator = population
                                           ? bucket.count
                                           : std::max<size_t>(
                                                 bucket.count - 1, 1);
            value = squaredDeviation[index] /
                    static_cast<double>(denominator);
            if (name == "std") {
                value = std::sqrt(value);
            }
        }
        values.emplace_back(value, 0.0);
    }
    auto result = makeNumericValue(layout->outputDimensions, values,
                                   input.numericClass);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call,
                            std::string(name) +
                                " could not construct its result",
                            "MParser:InvalidVarianceResult");
}

size_t firstNonsingletonDimension(
    const std::vector<size_t>& dimensions) {
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] != 1) {
            return index;
        }
    }
    return 0;
}

BuiltinResult dotBuiltin(const BuiltinCall& call) {
    const RuntimeValue& left = call.arguments[0];
    const RuntimeValue& right = call.arguments[1];
    if (!isFloatingNumeric(left) || !isFloatingNumeric(right)) {
        return failure(call,
                       "dot inputs must be single or double numeric arrays",
                       "MParser:InvalidDotInput");
    }
    const auto expanded = runtimeImplicitExpansionDimensions(
        runtimeDimensions(left), runtimeDimensions(right));
    if (!expanded) {
        return failure(call, "dot inputs have incompatible dimensions",
                       "MParser:InvalidDotShape");
    }
    size_t dimension = firstNonsingletonDimension(*expanded);
    if (call.arguments.size() == 3) {
        const auto requested = positiveDimension(call.arguments[2]);
        if (!requested) {
            return failure(call, "dot dimension must be a positive integer",
                           "MParser:InvalidDotDimension");
        }
        dimension = *requested;
    }
    auto inputDimensions = *expanded;
    inputDimensions.resize(std::max(inputDimensions.size(), dimension + 1),
                           1);
    auto outputDimensions = inputDimensions;
    outputDimensions[dimension] = 1;
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount =
        checkedRuntimeDimensionProduct(outputDimensions);
    const RuntimeNumericClass outputClass =
        floatingResultClass({&left, &right});
    if (!outputCount || !checkpoint(call) ||
        !observeOutput(call, outputCount.value_or(0), outputClass,
                       left.numericComplex || right.numericComplex)) {
        return outputCount ? stopped(call, "dot")
                           : failure(call, "dot dimensions are too large",
                                     "MParser:InvalidDotShape");
    }
    std::vector<Complex> values(*outputCount, Complex{});
    const auto inputCount = checkedRuntimeDimensionProduct(inputDimensions);
    if (!inputCount) {
        return failure(call, "dot dimensions are too large",
                       "MParser:InvalidDotShape");
    }
    for (size_t index = 0; index < *inputCount; ++index) {
        if (!periodicCheckpoint(call, index)) {
            return stopped(call, "dot");
        }
        auto coordinates = runtimeColumnMajorCoordinates(
            index, inputDimensions);
        if (!coordinates) {
            return failure(call, "dot could not map input coordinates",
                           "MParser:InvalidDotShape");
        }
        const auto leftOffset = runtimeImplicitExpansionStorageOffset(
            *coordinates, runtimeDimensions(left));
        const auto rightOffset = runtimeImplicitExpansionStorageOffset(
            *coordinates, runtimeDimensions(right));
        if (!leftOffset || !rightOffset) {
            return failure(call, "dot could not map an input element",
                           "MParser:InvalidDotShape");
        }
        const auto leftValue =
            runtimeNumericStorageElementValue(left, *leftOffset);
        const auto rightValue =
            runtimeNumericStorageElementValue(right, *rightOffset);
        (*coordinates)[dimension] = 0;
        coordinates->resize(outputDimensions.size(), 0);
        const auto outputIndex = runtimeColumnMajorLinearIndex(
            *coordinates, outputDimensions);
        if (!leftValue || !rightValue || !outputIndex) {
            return failure(call, "dot could not map an input element",
                           "MParser:InvalidDotShape");
        }
        const Complex leftComplex(
            leftValue->real,
            leftValue->complex ? leftValue->imaginary : 0.0);
        const Complex rightComplex(
            rightValue->real,
            rightValue->complex ? rightValue->imaginary : 0.0);
        values[*outputIndex] += std::conj(leftComplex) * rightComplex;
    }
    auto result = makeNumericValue(
        outputDimensions, values, outputClass,
        left.numericComplex || right.numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "dot could not construct its result",
                            "MParser:InvalidDotResult");
}

BuiltinResult crossBuiltin(const BuiltinCall& call) {
    const RuntimeValue& left = call.arguments[0];
    const RuntimeValue& right = call.arguments[1];
    if (!isFloatingNumeric(left) || !isFloatingNumeric(right)) {
        return failure(call,
                       "cross inputs must be single or double numeric arrays",
                       "MParser:InvalidCrossInput");
    }
    auto leftDimensions = runtimeDimensions(left);
    auto rightDimensions = runtimeDimensions(right);
    size_t dimension = std::numeric_limits<size_t>::max();
    if (call.arguments.size() == 3) {
        const auto requested = positiveDimension(call.arguments[2]);
        if (!requested) {
            return failure(call,
                           "cross dimension must be a positive integer",
                           "MParser:InvalidCrossDimension");
        }
        dimension = *requested;
    } else {
        const size_t rank =
            std::max(leftDimensions.size(), rightDimensions.size());
        leftDimensions.resize(rank, 1);
        rightDimensions.resize(rank, 1);
        for (size_t index = 0; index < rank; ++index) {
            if (leftDimensions[index] == 3 &&
                rightDimensions[index] == 3) {
                dimension = index;
                break;
            }
        }
    }
    if (dimension == std::numeric_limits<size_t>::max()) {
        return failure(call,
                       "cross inputs need a dimension of length three",
                       "MParser:InvalidCrossDimension");
    }
    const size_t rank =
        std::max({leftDimensions.size(), rightDimensions.size(),
                  dimension + 1});
    leftDimensions.resize(rank, 1);
    rightDimensions.resize(rank, 1);
    if (leftDimensions[dimension] != 3 ||
        rightDimensions[dimension] != 3) {
        return failure(call,
                       "cross selected dimension must have length three",
                       "MParser:InvalidCrossDimension");
    }
    std::vector<size_t> outputDimensions(rank, 1);
    for (size_t index = 0; index < rank; ++index) {
        if (index == dimension) {
            outputDimensions[index] = 3;
        } else if (leftDimensions[index] == rightDimensions[index]) {
            outputDimensions[index] = leftDimensions[index];
        } else if (leftDimensions[index] == 1) {
            outputDimensions[index] = rightDimensions[index];
        } else if (rightDimensions[index] == 1) {
            outputDimensions[index] = leftDimensions[index];
        } else {
            return failure(call,
                           "cross inputs have incompatible dimensions",
                           "MParser:InvalidCrossShape");
        }
    }
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto count = checkedRuntimeDimensionProduct(outputDimensions);
    const RuntimeNumericClass outputClass =
        floatingResultClass({&left, &right});
    if (!count || !checkpoint(call) ||
        !observeOutput(call, count.value_or(0), outputClass,
                       left.numericComplex || right.numericComplex)) {
        return count ? stopped(call, "cross")
                     : failure(call, "cross dimensions are too large",
                               "MParser:InvalidCrossShape");
    }
    std::vector<Complex> values(*count);
    std::vector<size_t> baseDimensions = outputDimensions;
    baseDimensions.resize(rank, 1);
    baseDimensions[dimension] = 1;
    const auto baseCount = checkedRuntimeDimensionProduct(baseDimensions);
    if (!baseCount) {
        return failure(call, "cross dimensions are too large",
                       "MParser:InvalidCrossShape");
    }
    for (size_t base = 0; base < *baseCount; ++base) {
        auto coordinates = runtimeColumnMajorCoordinates(
            base, baseDimensions);
        if (!coordinates) {
            return failure(call, "cross could not map coordinates",
                           "MParser:InvalidCrossShape");
        }
        Complex a[3];
        Complex b[3];
        for (size_t component = 0; component < 3; ++component) {
            (*coordinates)[dimension] = component;
            std::vector<size_t> leftCoordinates = *coordinates;
            std::vector<size_t> rightCoordinates = *coordinates;
            for (size_t index = 0; index < rank; ++index) {
                if (leftDimensions[index] == 1) {
                    leftCoordinates[index] = 0;
                }
                if (rightDimensions[index] == 1) {
                    rightCoordinates[index] = 0;
                }
            }
            const auto leftIndex = runtimeColumnMajorLinearIndex(
                leftCoordinates, leftDimensions);
            const auto rightIndex = runtimeColumnMajorLinearIndex(
                rightCoordinates, rightDimensions);
            if (!leftIndex || !rightIndex) {
                return failure(call,
                               "cross could not read an input element",
                               "MParser:InvalidCrossShape");
            }
            const auto leftValue = numericElement(left, *leftIndex);
            const auto rightValue = numericElement(right, *rightIndex);
            if (!leftValue || !rightValue) {
                return failure(call,
                               "cross could not read an input element",
                               "MParser:InvalidCrossShape");
            }
            a[component] = *leftValue;
            b[component] = *rightValue;
        }
        const Complex result[3] = {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
        for (size_t component = 0; component < 3; ++component) {
            (*coordinates)[dimension] = component;
            coordinates->resize(outputDimensions.size(), 0);
            const auto outputIndex = runtimeColumnMajorLinearIndex(
                *coordinates, outputDimensions);
            if (!outputIndex) {
                return failure(call,
                               "cross could not map an output element",
                               "MParser:InvalidCrossShape");
            }
            values[*outputIndex] = result[component];
        }
    }
    auto result = makeNumericValue(
        outputDimensions, values, outputClass,
        left.numericComplex || right.numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "cross could not construct its result",
                            "MParser:InvalidCrossResult");
}

BuiltinResult fftBuiltin(std::string_view name, const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isFloatingNumeric(input)) {
        return failure(call,
                       std::string(name) +
                           " expects a single or double numeric array",
                       "MParser:InvalidFftInput");
    }
    auto inputDimensions = runtimeDimensions(input);
    size_t dimension = firstNonsingletonDimension(inputDimensions);
    std::optional<size_t> requestedLength;
    if (call.arguments.size() >= 2 &&
        runtimeShapeElementCount(call.arguments[1]) != 0) {
        requestedLength = nonnegativeSize(call.arguments[1]);
        if (!requestedLength) {
            return failure(call,
                           std::string(name) +
                               " length must be a nonnegative integer",
                           "MParser:InvalidFftLength");
        }
    }
    if (call.arguments.size() == 3) {
        const auto requestedDimension = positiveDimension(call.arguments[2]);
        if (!requestedDimension) {
            return failure(call,
                           std::string(name) +
                               " dimension must be a positive integer",
                           "MParser:InvalidFftDimension");
        }
        dimension = *requestedDimension;
    }
    inputDimensions.resize(std::max(inputDimensions.size(), dimension + 1),
                           1);
    const size_t transformLength =
        requestedLength.value_or(inputDimensions[dimension]);
    std::vector<size_t> storageOutputDimensions = inputDimensions;
    storageOutputDimensions[dimension] = transformLength;
    std::vector<size_t> outputDimensions = storageOutputDimensions;
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount =
        checkedRuntimeDimensionProduct(storageOutputDimensions);
    if (!outputCount || !checkpoint(call) ||
        !observeOutput(call, outputCount.value_or(0), input.numericClass,
                       true)) {
        return outputCount ? stopped(call, name)
                           : failure(call,
                                     std::string(name) +
                                         " dimensions are too large",
                                     "MParser:InvalidFftShape");
    }
    if (transformLength == 0) {
        auto result = makeNumericValue(outputDimensions,
                                       std::vector<Complex>{},
                                       input.numericClass,
                                       input.numericComplex);
        return result ? outputsFor(call, {std::move(*result)})
                      : failure(call,
                                std::string(name) +
                                    " could not construct an empty result",
                                "MParser:InvalidFftResult");
    }

    std::vector<size_t> baseDimensions = storageOutputDimensions;
    baseDimensions[dimension] = 1;
    const auto baseCount = checkedRuntimeDimensionProduct(baseDimensions);
    if (!baseCount) {
        return failure(call,
                       std::string(name) + " dimensions are too large",
                       "MParser:InvalidFftShape");
    }
    std::vector<Complex> values(*outputCount);
    for (size_t base = 0; base < *baseCount; ++base) {
        if (!periodicCheckpoint(call, base, 256U)) {
            return stopped(call, name);
        }
        auto coordinates = runtimeColumnMajorCoordinates(
            base, baseDimensions);
        if (!coordinates) {
            return failure(call,
                           std::string(name) +
                               " could not map transform coordinates",
                           "MParser:InvalidFftShape");
        }
        std::vector<Complex> source(transformLength, Complex{});
        const size_t copied =
            std::min(transformLength, inputDimensions[dimension]);
        for (size_t index = 0; index < copied; ++index) {
            (*coordinates)[dimension] = index;
            const auto inputIndex = runtimeColumnMajorLinearIndex(
                *coordinates, inputDimensions);
            if (!inputIndex) {
                return failure(call,
                               std::string(name) +
                                   " could not read an input element",
                               "MParser:InvalidFftInput");
            }
            const auto element = numericElement(input, *inputIndex);
            if (!element) {
                return failure(call,
                               std::string(name) +
                                   " could not read an input element",
                               "MParser:InvalidFftInput");
            }
            source[index] = *element;
        }
        std::vector<Complex> transformed = std::move(source);
        if (!native_numeric::transform(transformed, name == "ifft")) {
            return failure(call,
                           std::string(name) +
                               " transform length exceeds the native backend "
                               "limit",
                           "MParser:InvalidFftResult");
        }
        for (size_t index = 0; index < transformLength; ++index) {
            std::vector<size_t> outputCoordinates = *coordinates;
            outputCoordinates[dimension] = index;
            const auto outputIndex = runtimeColumnMajorLinearIndex(
                outputCoordinates, storageOutputDimensions);
            if (!outputIndex) {
                return failure(call,
                               std::string(name) +
                                   " could not map an output element",
                               "MParser:InvalidFftShape");
            }
            values[*outputIndex] = transformed[index];
        }
    }
    auto result = makeNumericValue(outputDimensions, values,
                                   input.numericClass,
                                   input.numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call,
                            std::string(name) +
                                " could not construct its result",
                            "MParser:InvalidFftResult");
}

bool numericVector(const RuntimeValue& value) {
    if (!isFloatingNumeric(value)) {
        return false;
    }
    const auto dimensions = runtimeDimensions(value);
    return dimensions.size() == 2 &&
           (dimensions[0] == 1 || dimensions[1] == 1);
}

BuiltinResult convBuiltin(const BuiltinCall& call) {
    const RuntimeValue& left = call.arguments[0];
    const RuntimeValue& right = call.arguments[1];
    if (!numericVector(left) || !numericVector(right)) {
        return failure(call,
                       "conv inputs must be single or double numeric vectors",
                       "MParser:InvalidConvInput");
    }
    std::string shape = "full";
    if (call.arguments.size() == 3) {
        const auto requested = runtimeTextScalarUtf8(call.arguments[2]);
        if (!requested ||
            (*requested != "full" && *requested != "same" &&
             *requested != "valid")) {
            return failure(call,
                           "conv shape must be 'full', 'same', or 'valid'",
                           "MParser:InvalidConvShape");
        }
        shape = *requested;
    }
    const size_t leftCount = runtimeShapeElementCount(left);
    const size_t rightCount = runtimeShapeElementCount(right);
    if (leftCount != 0 && rightCount != 0 &&
        leftCount > std::numeric_limits<size_t>::max() - rightCount + 1U) {
        return failure(call, "conv result length overflowed",
                       "MParser:InvalidConvShape");
    }
    const size_t fullCount = leftCount == 0 || rightCount == 0
                                 ? 0
                                 : leftCount + rightCount - 1;
    size_t begin = 0;
    size_t count = fullCount;
    if (shape == "same") {
        begin = rightCount / 2;
        count = leftCount;
    } else if (shape == "valid") {
        begin = rightCount == 0 ? 0 : rightCount - 1;
        count = leftCount >= rightCount && rightCount != 0
                    ? leftCount - rightCount + 1
                    : 0;
    }
    const RuntimeNumericClass outputClass =
        floatingResultClass({&left, &right});
    if (!checkpoint(call) ||
        !observeOutput(call, count, outputClass,
                       left.numericComplex || right.numericComplex)) {
        return stopped(call, "conv");
    }
    std::vector<Complex> full(fullCount, Complex{});
    for (size_t leftIndex = 0; leftIndex < leftCount; ++leftIndex) {
        if (!periodicCheckpoint(call, leftIndex, 1024U)) {
            return stopped(call, "conv");
        }
        const auto leftValue = numericElement(left, leftIndex);
        if (!leftValue) {
            return failure(call, "conv could not read its first input",
                           "MParser:InvalidConvInput");
        }
        for (size_t rightIndex = 0; rightIndex < rightCount; ++rightIndex) {
            const auto rightValue = numericElement(right, rightIndex);
            if (!rightValue) {
                return failure(call, "conv could not read its second input",
                               "MParser:InvalidConvInput");
            }
            full[leftIndex + rightIndex] += *leftValue * *rightValue;
        }
    }
    std::vector<Complex> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const size_t source = begin + index;
        values.push_back(source < full.size() ? full[source] : Complex{});
    }
    const auto leftDimensions = runtimeDimensions(left);
    const auto rightDimensions = runtimeDimensions(right);
    const bool row = leftDimensions[0] == 1 && rightDimensions[0] == 1;
    auto result = makeNumericValue(row ? std::vector<size_t>{1, count}
                                       : std::vector<size_t>{count, 1},
                                   values, outputClass,
                                   left.numericComplex || right.numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "conv could not construct its result",
                            "MParser:InvalidConvResult");
}

BuiltinResult trapzBuiltin(const BuiltinCall& call) {
    const RuntimeValue* x = nullptr;
    const RuntimeValue* y = &call.arguments.front();
    size_t dimension = firstNonsingletonDimension(runtimeDimensions(*y));
    if (call.arguments.size() >= 2) {
        if (isRuntimeNumericValue(call.arguments[1]) &&
            runtimeShapeElementCount(call.arguments[1]) == 1 &&
            call.arguments.size() == 2) {
            const auto requested = positiveDimension(call.arguments[1]);
            if (requested) {
                dimension = *requested;
            } else {
                x = &call.arguments[0];
                y = &call.arguments[1];
                dimension = firstNonsingletonDimension(
                    runtimeDimensions(*y));
            }
        } else {
            x = &call.arguments[0];
            y = &call.arguments[1];
            dimension = firstNonsingletonDimension(runtimeDimensions(*y));
        }
    }
    if (call.arguments.size() == 3) {
        const auto requested = positiveDimension(call.arguments[2]);
        if (!requested) {
            return failure(call,
                           "trapz dimension must be a positive integer",
                           "MParser:InvalidTrapzDimension");
        }
        dimension = *requested;
    }
    if (!isFloatingNumeric(*y) || (x && !isFloatingNumeric(*x))) {
        return failure(call,
                       "trapz inputs must be single or double numeric arrays",
                       "MParser:InvalidTrapzInput");
    }
    auto inputDimensions = runtimeDimensions(*y);
    inputDimensions.resize(std::max(inputDimensions.size(), dimension + 1),
                           1);
    const size_t length = inputDimensions[dimension];
    if (x && runtimeShapeElementCount(*x) != length) {
        return failure(call,
                       "trapz coordinate vector must match the integration "
                       "dimension",
                       "MParser:InvalidTrapzShape");
    }
    auto outputDimensions = inputDimensions;
    outputDimensions[dimension] = 1;
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount =
        checkedRuntimeDimensionProduct(outputDimensions);
    const RuntimeNumericClass outputClass =
        x ? floatingResultClass({x, y}) : y->numericClass;
    if (!outputCount || !checkpoint(call) ||
        !observeOutput(call, outputCount.value_or(0), outputClass,
                       y->numericComplex)) {
        return outputCount ? stopped(call, "trapz")
                           : failure(call, "trapz dimensions are too large",
                                     "MParser:InvalidTrapzShape");
    }
    std::vector<Complex> values(*outputCount, Complex{});
    for (size_t output = 0; output < *outputCount; ++output) {
        auto coordinates = runtimeColumnMajorCoordinates(
            output, outputDimensions);
        if (!coordinates) {
            return failure(call, "trapz could not map output coordinates",
                           "MParser:InvalidTrapzShape");
        }
        coordinates->resize(inputDimensions.size(), 0);
        for (size_t index = 0; index + 1 < length; ++index) {
            (*coordinates)[dimension] = index;
            const auto leftIndex = runtimeColumnMajorLinearIndex(
                *coordinates, inputDimensions);
            (*coordinates)[dimension] = index + 1;
            const auto rightIndex = runtimeColumnMajorLinearIndex(
                *coordinates, inputDimensions);
            if (!leftIndex || !rightIndex) {
                return failure(call, "trapz could not read an input element",
                               "MParser:InvalidTrapzInput");
            }
            const auto leftValue = numericElement(*y, *leftIndex);
            const auto rightValue = numericElement(*y, *rightIndex);
            if (!leftValue || !rightValue) {
                return failure(call, "trapz could not read an input element",
                               "MParser:InvalidTrapzInput");
            }
            double spacing = 1.0;
            if (x) {
                const auto x0 = numericElement(*x, index);
                const auto x1 = numericElement(*x, index + 1);
                if (!x0 || !x1 || x0->imag() != 0.0 ||
                    x1->imag() != 0.0) {
                    return failure(call,
                                   "trapz coordinates must be real",
                                   "MParser:InvalidTrapzInput");
                }
                spacing = x1->real() - x0->real();
            }
            values[output] +=
                (*leftValue + *rightValue) * (0.5 * spacing);
        }
    }
    auto result = makeNumericValue(outputDimensions, values, outputClass,
                                   y->numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "trapz could not construct its result",
                            "MParser:InvalidTrapzResult");
}

BuiltinResult polyvalBuiltin(const BuiltinCall& call) {
    const RuntimeValue& coefficients = call.arguments[0];
    const RuntimeValue& input = call.arguments[1];
    if (!numericVector(coefficients) || !isFloatingNumeric(input)) {
        return failure(call,
                       "polyval expects a floating-point coefficient vector "
                       "and numeric input array",
                       "MParser:InvalidPolyvalInput");
    }
    const RuntimeNumericClass outputClass =
        floatingResultClass({&coefficients, &input});
    const size_t count = runtimeShapeElementCount(input);
    if (!checkpoint(call) ||
        !observeOutput(call, count, outputClass,
                       coefficients.numericComplex || input.numericComplex)) {
        return stopped(call, "polyval");
    }
    std::vector<Complex> coefficientValues;
    coefficientValues.reserve(runtimeShapeElementCount(coefficients));
    for (size_t index = 0;
         index < runtimeShapeElementCount(coefficients); ++index) {
        const auto coefficient = numericElement(coefficients, index);
        if (!coefficient) {
            return failure(call, "polyval could not read a coefficient",
                           "MParser:InvalidPolyvalInput");
        }
        coefficientValues.push_back(*coefficient);
    }
    std::vector<Complex> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (!periodicCheckpoint(call, index)) {
            return stopped(call, "polyval");
        }
        const auto x = numericElement(input, index);
        if (!x) {
            return failure(call, "polyval could not read an input element",
                           "MParser:InvalidPolyvalInput");
        }
        Complex value{};
        for (const Complex coefficient : coefficientValues) {
            value = value * *x + coefficient;
        }
        values.push_back(value);
    }
    auto result = makeNumericValue(
        runtimeDimensions(input), values, outputClass,
        coefficients.numericComplex || input.numericComplex);
    return result ? outputsFor(call, {std::move(*result)})
                  : failure(call, "polyval could not construct its result",
                            "MParser:InvalidPolyvalResult");
}

BuiltinResult polyfitBuiltin(const BuiltinCall& call) {
    const RuntimeValue& x = call.arguments[0];
    const RuntimeValue& y = call.arguments[1];
    const auto degree = nonnegativeSize(call.arguments[2]);
    if (!numericVector(x) || !numericVector(y) || !degree ||
        x.numericComplex || runtimeShapeElementCount(x) !=
                                runtimeShapeElementCount(y)) {
        return failure(call,
                       "polyfit expects equal-length floating-point vectors, "
                       "real x values, and a nonnegative degree",
                       "MParser:InvalidPolyfitInput");
    }
    const size_t sampleCount = runtimeShapeElementCount(x);
    if (*degree == std::numeric_limits<size_t>::max()) {
        return failure(call, "polyfit dimensions exceed the backend limit",
                       "MParser:InvalidPolyfitShape");
    }
    const size_t coefficientCount = *degree + 1;
    const RuntimeNumericClass outputClass =
        floatingResultClass({&x, &y});
    size_t outputElements = coefficientCount;
    if (call.requestedOutputCount > 1) {
        if (coefficientCount != 0 &&
            coefficientCount > std::numeric_limits<size_t>::max() /
                                   coefficientCount) {
            return failure(call, "polyfit output dimensions are too large",
                           "MParser:InvalidPolyfitShape");
        }
        const size_t extra = coefficientCount * coefficientCount + 2U;
        if (extra < coefficientCount * coefficientCount ||
            outputElements > std::numeric_limits<size_t>::max() - extra) {
            return failure(call, "polyfit output dimensions are too large",
                           "MParser:InvalidPolyfitShape");
        }
        outputElements += extra;
    }
    if (call.requestedOutputCount > 2) {
        if (outputElements > std::numeric_limits<size_t>::max() - 2U) {
            return failure(call, "polyfit output dimensions are too large",
                           "MParser:InvalidPolyfitShape");
        }
        outputElements += 2U;
    }
    if (!checkpoint(call) ||
        !observeOutput(call, outputElements, outputClass,
                       y.numericComplex)) {
        return stopped(call, "polyfit");
    }

    std::vector<double> xValues(sampleCount);
    ComplexMatrix yValues(sampleCount, 1);
    double mean = 0.0;
    for (size_t index = 0; index < sampleCount; ++index) {
        const auto xValue = numericElement(x, index);
        const auto yValue = numericElement(y, index);
        if (!xValue || !yValue || xValue->imag() != 0.0) {
            return failure(call, "polyfit could not read an input element",
                           "MParser:InvalidPolyfitInput");
        }
        xValues[index] = xValue->real();
        yValues(index, 0) = *yValue;
        mean += xValues[index];
    }
    if (sampleCount != 0) {
        mean /= static_cast<double>(sampleCount);
    }
    double scale = 1.0;
    if (call.requestedOutputCount > 2 && sampleCount > 1) {
        long double sum = 0.0L;
        for (const double value : xValues) {
            const long double delta = value - mean;
            sum += delta * delta;
        }
        scale = std::sqrt(static_cast<double>(
            sum / static_cast<long double>(sampleCount - 1)));
        if (scale == 0.0 || !std::isfinite(scale)) {
            scale = 1.0;
        }
    }

    ComplexMatrix vandermonde(sampleCount, coefficientCount);
    for (size_t row = 0; row < sampleCount; ++row) {
        const double normalized = call.requestedOutputCount > 2
                                      ? (xValues[row] - mean) / scale
                                      : xValues[row];
        Complex power{1.0, 0.0};
        for (size_t reverse = 0; reverse < coefficientCount; ++reverse) {
            const size_t column = coefficientCount - reverse - 1;
            vandermonde(row, column) = power;
            power *= normalized;
        }
    }
    const double solveEpsilon =
        outputClass == RuntimeNumericClass::Single
            ? static_cast<double>(std::numeric_limits<float>::epsilon())
            : std::numeric_limits<double>::epsilon();
    const auto decomposition =
        sampleCount >= coefficientCount
            ? native_numeric::solveLeastSquaresUnpivoted(
                  vandermonde, yValues, solveEpsilon)
            : native_numeric::solve(vandermonde, yValues, solveEpsilon);
    if (!decomposition.succeeded) {
        return failure(call, "polyfit could not solve its Vandermonde system: " +
                                 decomposition.error,
                       "MParser:PolyfitRankDeficient");
    }
    const ComplexMatrix fitted =
        native_numeric::multiply(vandermonde, decomposition.solution);
    long double residualSquared = 0.0L;
    for (size_t row = 0; row < sampleCount; ++row) {
        residualSquared += std::norm(fitted(row, 0) - yValues(row, 0));
    }
    const double residualNorm =
        std::sqrt(static_cast<double>(residualSquared));

    std::vector<Complex> coefficientValues;
    coefficientValues.reserve(coefficientCount);
    for (size_t index = 0; index < coefficientCount; ++index) {
        coefficientValues.push_back(decomposition.solution(index, 0));
    }
    auto coefficientValue = makeNumericValue(
        {1, coefficientCount}, coefficientValues, outputClass,
        y.numericComplex);
    if (!coefficientValue) {
        return failure(call, "polyfit could not construct coefficients",
                       "MParser:InvalidPolyfitResult");
    }
    if (call.requestedOutputCount <= 1) {
        return outputsFor(call, {std::move(*coefficientValue)});
    }

    ComplexMatrix r(coefficientCount, coefficientCount);
    const size_t copiedRows = std::min(
        decomposition.upperTriangular.rows(), coefficientCount);
    const size_t copiedColumns = std::min(
        decomposition.upperTriangular.columns(), coefficientCount);
    for (size_t row = 0; row < copiedRows; ++row) {
        for (size_t column = 0; column < copiedColumns; ++column) {
            r(row, column) = decomposition.upperTriangular(row, column);
        }
    }
    auto rValue = makeNumericValue(
        {coefficientCount, coefficientCount}, r, outputClass,
        y.numericComplex);
    if (!rValue) {
        return failure(call, "polyfit could not construct R",
                       "MParser:InvalidPolyfitResult");
    }
    RuntimeWorkspace fields;
    fields.emplace("R", std::move(*rValue));
    fields.emplace("df", makeRuntimeNumberValue(static_cast<double>(
                             sampleCount > coefficientCount
                                 ? sampleCount - coefficientCount
                                 : 0)));
    auto normValue = makeNumericValue(
        {1, 1}, {Complex(residualNorm, 0.0)}, outputClass);
    if (!normValue) {
        return failure(call, "polyfit could not construct normr",
                       "MParser:InvalidPolyfitResult");
    }
    fields.emplace("normr", std::move(*normValue));
    RuntimeValue stats = makeRuntimeStructValue(std::move(fields));
    if (call.requestedOutputCount == 2) {
        return outputsFor(call,
                          {std::move(*coefficientValue),
                           std::move(stats)});
    }
    auto mu = makeNumericValue(
        {1, 2},
        std::vector<Complex>{Complex(mean, 0.0),
                             Complex(scale, 0.0)},
        outputClass);
    if (!mu) {
        return failure(call, "polyfit could not construct mu",
                       "MParser:InvalidPolyfitResult");
    }
    return outputsFor(call,
                      {std::move(*coefficientValue), std::move(stats),
                       std::move(*mu)});
}

} // namespace

bool isRuntimeAdvancedNumericBuiltin(std::string_view name) {
    return name == "conv" || name == "cross" || name == "det" ||
           name == "dot" || name == "eig" || name == "fft" ||
           name == "ifft" || name == "inv" || name == "median" ||
           name == "norm" || name == "polyfit" || name == "polyval" ||
           name == "rank" || name == "std" || name == "trace" ||
           name == "trapz" || name == "var";
}

BuiltinResult invokeRuntimeAdvancedNumericBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "det") {
        return detBuiltin(call);
    }
    if (name == "inv") {
        return invBuiltin(call);
    }
    if (name == "trace") {
        return traceBuiltin(call);
    }
    if (name == "norm") {
        return normBuiltin(call);
    }
    if (name == "rank") {
        return rankBuiltin(call);
    }
    if (name == "eig") {
        return eigBuiltin(call);
    }
    if (name == "median") {
        return medianBuiltin(call);
    }
    if (name == "std" || name == "var") {
        return varianceBuiltin(name, call);
    }
    if (name == "dot") {
        return dotBuiltin(call);
    }
    if (name == "cross") {
        return crossBuiltin(call);
    }
    if (name == "fft" || name == "ifft") {
        return fftBuiltin(name, call);
    }
    if (name == "conv") {
        return convBuiltin(call);
    }
    if (name == "trapz") {
        return trapzBuiltin(call);
    }
    if (name == "polyval") {
        return polyvalBuiltin(call);
    }
    if (name == "polyfit") {
        return polyfitBuiltin(call);
    }
    return failure(call, "unknown advanced numeric builtin",
                   "MParser:UnknownBuiltin");
}

RuntimeNumericOperationResult runtimeApplyDenseMatrixDivision(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    if (operation == "\\") {
        return solveMatrices(left, right, false);
    }
    if (operation == "/") {
        return solveMatrices(left, right, true);
    }
    return operationFailure("unsupported dense matrix division operator: " +
                            std::string(operation));
}

} // namespace mparser
