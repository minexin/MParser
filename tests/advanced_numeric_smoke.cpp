#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_execution_control.h"
#include "mparser/execution/jit/runtime_native_numeric.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_struct.h"
#include "mparser/semantic/semantic.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct RuntimePair {
    mparser::InterpreterResult interpreter;
    mparser::BytecodeVmResult vm;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string readSource(const char* path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "advanced numeric sample is unavailable");
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

RuntimePair runBoth(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parse = parser.parse();
    require(parse.diagnostics.empty(),
            "advanced numeric source did not parse");
    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parse.root);
    require(semantic.diagnostics.empty(),
            "advanced numeric source failed semantic analysis");
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    require(bytecode.diagnostics.empty(),
            "advanced numeric source did not lower");
    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(semantic);
    mparser::BytecodeVm vm;
    auto executed = vm.run(bytecode, semantic);
    return {std::move(interpreted), std::move(executed)};
}

template <typename Result>
const mparser::RuntimeValue& variable(const Result& result,
                                      std::string_view name) {
    for (const auto& candidate : result.variables) {
        if (candidate.name == name) {
            return candidate.value;
        }
    }
    throw std::runtime_error("missing advanced numeric variable: " +
                             std::string(name));
}

template <typename Result>
void requireNoDiagnostics(const Result& result, std::string_view context) {
    if (result.diagnostics.empty()) {
        return;
    }
    const auto& diagnostic = result.diagnostics.front();
    throw std::runtime_error(std::string(context) + ": " +
                             diagnostic.identifier + ": " +
                             diagnostic.message);
}

std::vector<double> realValues(const mparser::RuntimeValue& value) {
    std::vector<double> values;
    values.reserve(mparser::runtimeShapeElementCount(value));
    for (size_t index = 0;
         index < mparser::runtimeShapeElementCount(value); ++index) {
        const auto element = mparser::runtimeNumericElementValue(value, index);
        require(element.has_value(),
                "advanced numeric value contains a nonnumeric element");
        values.push_back(element->real);
    }
    return values;
}

template <typename Result>
void requireDiagnostic(const Result& result,
                       std::string_view identifier) {
    require(!result.diagnostics.empty(),
            "invalid advanced numeric source unexpectedly succeeded");
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.identifier == identifier) {
            return;
        }
    }
    throw std::runtime_error(
        "advanced numeric source reported the wrong diagnostic");
}

void requireDiagnostic(const mparser::BuiltinResult& result,
                       std::string_view identifier) {
    require(!result.succeeded && result.outputs.empty() &&
                result.diagnostics.size() == 1 &&
                result.diagnostics.front().identifier == identifier,
            "advanced numeric builtin reported the wrong diagnostic");
}

template <typename Result>
void verifySample(const Result& result) {
    requireNoDiagnostics(result, "advanced numeric sample");
    require(variable(result, "advanced_numeric_summary").number == 174.0,
            "advanced numeric sample summary mismatch");
    const auto& cube = variable(result, "cube_median");
    require(mparser::runtimeDimensions(cube) ==
                std::vector<size_t>({1, 1, 4}) &&
                realValues(cube) ==
                    std::vector<double>({3.5, 9.5, 15.5, 21.5}),
            "N-dimensional median result mismatch");
    const auto& spectrum = variable(result, "spectrum");
    require(spectrum.numericClass ==
                mparser::RuntimeNumericClass::Single &&
                spectrum.numericComplex,
            "FFT did not preserve single/complex metadata");
    const auto& fit = variable(result, "fit_info");
    const auto* r = mparser::runtimeStructField(fit, "R");
    require(fit.kind == mparser::RuntimeValueKind::Struct && r &&
                mparser::runtimeStructField(fit, "df") &&
                mparser::runtimeStructField(fit, "normr"),
            "polyfit statistics structure mismatch");
    const auto rDimensions = mparser::runtimeDimensions(*r);
    const auto rValues = realValues(*r);
    require(rDimensions.size() == 2 &&
                rDimensions[0] == rDimensions[1],
            "polyfit R has the wrong shape");
    for (size_t column = 0; column < rDimensions[1]; ++column) {
        for (size_t row = column + 1; row < rDimensions[0]; ++row) {
            require(std::abs(rValues[row + column * rDimensions[0]]) <
                        1e-12,
                    "polyfit R is not upper triangular");
        }
    }
}

mparser::BuiltinResult invoke(
    const std::shared_ptr<const mparser::BuiltinRegistry>& registry,
    std::string_view name, std::vector<mparser::RuntimeValue> arguments,
    size_t outputs, mparser::BuiltinCallContext* context = nullptr) {
    return registry->invoke(
        name, mparser::BuiltinCall{arguments, outputs, {}, context});
}

void registryAndResourceSmoke() {
    const auto registry = mparser::defaultBuiltinRegistry();
    for (const std::string_view name : {
             "conv", "cross", "det", "dot", "eig", "fft", "ifft",
             "inv", "median", "norm", "polyfit", "polyval", "rank",
             "std", "trace", "trapz", "var"}) {
        const auto* descriptor = registry->find(name);
        require(descriptor &&
                    descriptor->implementation ==
                        mparser::BuiltinImplementationKind::Context &&
                    descriptor->purity == mparser::BuiltinPurity::Pure &&
                    descriptor->determinism ==
                        mparser::BuiltinDeterminism::Deterministic &&
                    mparser::hasBuiltinContextPermission(
                        descriptor->contextPermissions,
                        mparser::BuiltinContextPermission::ExecutionControl),
                "advanced numeric descriptor metadata mismatch");
    }

    mparser::RuntimeCancellationToken token;
    token.requestCancellation();
    mparser::RuntimeExecutionControl cancelled(
        {}, std::optional<mparser::RuntimeCancellationToken>{token});
    mparser::BuiltinCallContext context;
    context.executionControl = &cancelled;
    auto result = invoke(
        registry, "fft",
        {mparser::makeRuntimeVectorValue({1, 2, 3, 4})}, 1, &context);
    requireDiagnostic(result, "MParser:ExecutionStopped");
    require(cancelled.stopReason() ==
                mparser::RuntimeExecutionStopReason::Cancelled,
            "FFT did not observe cancellation before allocation");

    mparser::RuntimeExecutionLimits limits;
    limits.maxArrayBytes = 8;
    mparser::RuntimeExecutionControl bounded(limits);
    context.executionControl = &bounded;
    result = invoke(
        registry, "conv",
        {mparser::makeRuntimeVectorValue({1, 2}),
         mparser::makeRuntimeVectorValue({3, 4})},
        1, &context);
    requireDiagnostic(result, "MParser:ExecutionStopped");
    require(bounded.stopReason() ==
                mparser::RuntimeExecutionStopReason::ArrayByteLimit,
            "convolution ignored its output byte boundary");
}

template <typename Result>
void verifyBroadFamily(const Result& result) {
    requireNoDiagnostics(result, "broad advanced numeric family");
    require(std::abs(variable(result, "median_nan").number - 2.0) <
                1e-12,
            "median omitnan mismatch");
    require(realValues(variable(result, "matrix_median")) ==
                std::vector<double>({2.5, 6.5, 10.5}),
            "vector-dimension median mismatch");
    require(realValues(variable(result, "same_conv")) ==
                std::vector<double>({3, 5, 3}) &&
                mparser::runtimeDimensions(
                    variable(result, "column_conv")) ==
                    std::vector<size_t>({3, 1}),
            "convolution mode/orientation mismatch");
    require(variable(result, "single_inverse").numericClass ==
                mparser::RuntimeNumericClass::Single,
            "inverse did not preserve single precision");
    const auto evaluated = realValues(variable(result, "evaluated"));
    require(evaluated.size() == 2 &&
                std::abs(evaluated[0] - 7.0) < 1e-12 &&
                std::abs(evaluated[1] - 9.0) < 1e-12,
            "polyfit/polyval result mismatch");
    require(variable(result, "summary").number == 28.0,
            "broad advanced numeric summary mismatch");
}

void broadFamilySmoke() {
    const auto result = runBoth(R"(
median_nan = median([1 NaN 3], 'omitnan');
population = std([1 2 3], 1);
matrix_median = median(reshape(1:12, [2 2 3]), [1 2]);
complex_dot = dot([1+i 2-i], [1+i 2-i]);
expanded_cross = cross([1 0 0; 0 1 0], [0 1 0]);
round_trip = ifft(fft([1+2i 3-4i 5]));
same_conv = conv([1 2 3], [1 1], 'same');
column_conv = conv([1 2], [3; 4]);
integral = trapz([0 1 2], [0 1 4]);
left_solution = [1 2; 3 5] \ [5; 13];
right_solution = [1 2] / [1 0; 0 2];
[eigenvectors, eigenvalues] = eig([2 1; 1 2]);
single_inverse = inv(single([2 0; 0 4]));
fit = polyfit([0 1 2], [1 3 5], 1);
evaluated = polyval(fit, [3 4]);
summary = 28;
)");
    verifyBroadFamily(result.interpreter);
    verifyBroadFamily(result.vm);
}

void errorSmoke() {
    struct Case {
        std::string_view source;
        std::string_view identifier;
    };
    for (const Case& test : {
             Case{"bad = det([1 2 3]);", "MParser:InvalidDetInput"},
             Case{"bad = inv([1 2; 2 4]);", "MParser:SingularMatrix"},
             Case{"bad = norm([1 2; 3 4], 3);",
                  "MParser:InvalidNormOption"},
             Case{"bad = rank(eye(2), -1);",
                  "MParser:InvalidRankTolerance"},
             Case{"bad = cross([1 2], [3 4]);",
                  "MParser:InvalidCrossDimension"},
             Case{"bad = fft([1 2], -1);", "MParser:InvalidFftLength"},
             Case{"bad = conv(eye(2), [1 2]);",
                  "MParser:InvalidConvInput"},
             Case{"bad = std([1 2], 2);",
                  "MParser:InvalidVarianceOption"}}) {
        const auto result = runBoth(test.source);
        requireDiagnostic(result.interpreter, test.identifier);
        requireDiagnostic(result.vm, test.identifier);
    }
}

double decompositionResidual(
    const mparser::native_numeric::Matrix& matrix,
    const mparser::native_numeric::EigenResult& decomposition) {
    using mparser::native_numeric::Matrix;
    const Matrix left = mparser::native_numeric::multiply(
        matrix, decomposition.vectors);
    Matrix diagonal(decomposition.values.size(),
                    decomposition.values.size());
    for (size_t index = 0; index < decomposition.values.size(); ++index) {
        diagonal(index, index) = decomposition.values[index];
    }
    const Matrix right = mparser::native_numeric::multiply(
        decomposition.vectors, diagonal);
    double residual = 0.0;
    for (size_t index = 0; index < left.size(); ++index) {
        residual = std::max(
            residual,
            std::abs(left.values()[index] - right.values()[index]));
    }
    return residual;
}

void nativeBackendSmoke() {
    namespace native = mparser::native_numeric;
    const double epsilon = std::numeric_limits<double>::epsilon();

    native::Matrix square(2, 2);
    square(0, 0) = 4.0;
    square(1, 0) = 2.0;
    square(0, 1) = 1.0;
    square(1, 1) = 3.0;
    native::Matrix right(2, 1);
    right(0, 0) = 1.0;
    right(1, 0) = 2.0;
    const auto squareSolution = native::solve(square, right, epsilon);
    require(squareSolution.succeeded,
            "native LU solve rejected a nonsingular matrix");
    const auto reconstructed = native::multiply(
        square, squareSolution.solution);
    require(std::abs(reconstructed(0, 0) - right(0, 0)) < 1e-12 &&
                std::abs(reconstructed(1, 0) - right(1, 0)) < 1e-12,
            "native LU solve residual is too large");

    native::Matrix tall(3, 2);
    for (size_t row = 0; row < 3; ++row) {
        tall(row, 0) = 1.0;
        tall(row, 1) = static_cast<double>(row + 1);
    }
    native::Matrix observations(3, 1);
    observations(0, 0) = 1.0;
    observations(1, 0) = 2.0;
    observations(2, 0) = 2.0;
    const auto leastSquares = native::solve(tall, observations, epsilon);
    require(leastSquares.succeeded &&
                std::abs(leastSquares.solution(0, 0) - 2.0 / 3.0) <
                    1e-12 &&
                std::abs(leastSquares.solution(1, 0) - 0.5) < 1e-12,
            "native pivoted QR least-squares solution mismatch");

    native::Matrix wide(2, 3);
    wide(0, 0) = 1.0;
    wide(1, 1) = 1.0;
    wide(0, 2) = 1.0;
    wide(1, 2) = 1.0;
    const auto minimumNorm = native::solve(wide, right, epsilon);
    require(minimumNorm.succeeded &&
                std::abs(minimumNorm.solution(0, 0)) < 1e-12 &&
                std::abs(minimumNorm.solution(1, 0) - 1.0) < 1e-12 &&
                std::abs(minimumNorm.solution(2, 0) - 1.0) < 1e-12,
            "native underdetermined minimum-norm solution mismatch");

    native::Matrix diagonal(2, 2);
    diagonal(0, 0) = 3.0;
    diagonal(1, 1) = 4.0;
    const auto singularValues = native::singularValues(diagonal, epsilon);
    require(singularValues.size() == 2 &&
                std::abs(singularValues[0] - 4.0) < 1e-12 &&
                std::abs(singularValues[1] - 3.0) < 1e-12,
            "native singular-value calculation mismatch");

    native::Matrix hermitian(2, 2);
    hermitian(0, 0) = 2.0;
    hermitian(1, 1) = 3.0;
    hermitian(0, 1) = {1.0, 1.0};
    hermitian(1, 0) = {1.0, -1.0};
    const auto hermitianEigen = native::eigen(
        hermitian, epsilon, true);
    require(hermitianEigen.converged &&
                decompositionResidual(hermitian, hermitianEigen) < 1e-11,
            "native Hermitian eig residual is too large");

    native::Matrix rotation(2, 2);
    rotation(1, 0) = 1.0;
    rotation(0, 1) = -1.0;
    const auto generalEigen = native::eigen(rotation, epsilon, true);
    require(generalEigen.converged &&
                decompositionResidual(rotation, generalEigen) < 1e-10,
            "native general eig residual is too large");

    for (const size_t length : {1U, 2U, 3U, 5U, 8U, 17U}) {
        std::vector<native::Complex> values(length);
        for (size_t index = 0; index < length; ++index) {
            values[index] = native::Complex(
                static_cast<double>(index + 1),
                static_cast<double>(index % 3U) - 1.0);
        }
        const auto original = values;
        require(native::transform(values, false) &&
                    native::transform(values, true),
                "native FFT rejected a supported length");
        for (size_t index = 0; index < length; ++index) {
            require(std::abs(values[index] - original[index]) < 1e-10,
                    "native FFT round-trip residual is too large");
        }
    }

    std::vector<native::Complex> knownTransform = {1.0, 2.0, 3.0};
    require(native::transform(knownTransform, false) &&
                std::abs(knownTransform[0] - native::Complex(6.0, 0.0)) <
                    1e-12 &&
                std::abs(knownTransform[1] -
                         native::Complex(-1.5, std::sqrt(3.0) / 2.0)) <
                    1e-12 &&
                std::abs(knownTransform[2] -
                         native::Complex(-1.5, -std::sqrt(3.0) / 2.0)) <
                    1e-12,
            "native Bluestein FFT sign or scaling mismatch");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2,
                "advanced numeric smoke expects the sample path");
        const auto sample = runBoth(readSource(argv[1]));
        verifySample(sample.interpreter);
        verifySample(sample.vm);
        registryAndResourceSmoke();
        broadFamilySmoke();
        errorSmoke();
        nativeBackendSmoke();
        std::cout << "advanced numeric smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Advanced numeric smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
