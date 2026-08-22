#include "mparser/runtime_native_numeric.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace mparser::native_numeric {
namespace {

double matrixScale(const Matrix& matrix) {
    double scale = 0.0;
    for (const Complex value : matrix.values()) {
        scale = std::max(scale, std::abs(value));
    }
    return scale;
}

double toleranceFor(const Matrix& matrix, double epsilon) {
    return std::max(1.0, matrixScale(matrix)) *
           static_cast<double>(std::max(matrix.rows(), matrix.columns())) *
           epsilon;
}

struct LuFactorization {
    Matrix factors;
    std::vector<size_t> pivots;
    int parity = 1;
    double tolerance = 0.0;
    bool singular = false;
};

LuFactorization factorLu(const Matrix& matrix, double epsilon) {
    LuFactorization result;
    result.factors = matrix;
    result.pivots.resize(matrix.rows());
    result.tolerance = toleranceFor(matrix, epsilon);
    if (matrix.rows() != matrix.columns()) {
        result.singular = true;
        return result;
    }

    const size_t size = matrix.rows();
    for (size_t pivot = 0; pivot < size; ++pivot) {
        size_t selected = pivot;
        double selectedMagnitude = std::abs(result.factors(pivot, pivot));
        for (size_t row = pivot + 1; row < size; ++row) {
            const double magnitude = std::abs(result.factors(row, pivot));
            if (magnitude > selectedMagnitude) {
                selected = row;
                selectedMagnitude = magnitude;
            }
        }
        result.pivots[pivot] = selected;
        if (!(selectedMagnitude > result.tolerance)) {
            result.singular = true;
            return result;
        }
        if (selected != pivot) {
            for (size_t column = 0; column < size; ++column) {
                std::swap(result.factors(pivot, column),
                          result.factors(selected, column));
            }
            result.parity = -result.parity;
        }
        const Complex diagonal = result.factors(pivot, pivot);
        for (size_t row = pivot + 1; row < size; ++row) {
            result.factors(row, pivot) /= diagonal;
            const Complex multiplier = result.factors(row, pivot);
            for (size_t column = pivot + 1; column < size; ++column) {
                result.factors(row, column) -=
                    multiplier * result.factors(pivot, column);
            }
        }
    }
    return result;
}

std::optional<Matrix> solveLu(const LuFactorization& factorization,
                              const Matrix& right) {
    const size_t size = factorization.factors.rows();
    if (factorization.singular || right.rows() != size) {
        return std::nullopt;
    }
    Matrix solution = right;
    for (size_t pivot = 0; pivot < size; ++pivot) {
        if (factorization.pivots[pivot] != pivot) {
            for (size_t column = 0; column < solution.columns(); ++column) {
                std::swap(solution(pivot, column),
                          solution(factorization.pivots[pivot], column));
            }
        }
    }
    for (size_t column = 0; column < solution.columns(); ++column) {
        for (size_t row = 0; row < size; ++row) {
            for (size_t inner = 0; inner < row; ++inner) {
                solution(row, column) -=
                    factorization.factors(row, inner) *
                    solution(inner, column);
            }
        }
        for (size_t reverse = 0; reverse < size; ++reverse) {
            const size_t row = size - reverse - 1;
            for (size_t inner = row + 1; inner < size; ++inner) {
                solution(row, column) -=
                    factorization.factors(row, inner) *
                    solution(inner, column);
            }
            const Complex diagonal = factorization.factors(row, row);
            if (!(std::abs(diagonal) > factorization.tolerance)) {
                return std::nullopt;
            }
            solution(row, column) /= diagonal;
        }
    }
    return solution;
}

SolveResult solveLeastSquares(const Matrix& left, const Matrix& right,
                              double epsilon) {
    SolveResult result;
    const size_t rows = left.rows();
    const size_t columns = left.columns();
    if (right.rows() != rows) {
        result.error = "left and right row counts do not match";
        return result;
    }
    if (columns == 0) {
        result.succeeded = true;
        result.solution = Matrix(0, right.columns());
        result.upperTriangular = Matrix(0, 0);
        return result;
    }
    if (rows == 0) {
        result.error = "an empty system has no unique numeric solution";
        return result;
    }

    Matrix work = left;
    Matrix q(rows, std::min(rows, columns));
    Matrix r(std::min(rows, columns), columns);
    std::vector<size_t> permutation(columns);
    std::iota(permutation.begin(), permutation.end(), 0);
    std::vector<double> columnNorms(columns, 0.0);
    for (size_t column = 0; column < columns; ++column) {
        for (size_t row = 0; row < rows; ++row) {
            columnNorms[column] += std::norm(work(row, column));
        }
    }
    const double threshold = toleranceFor(left, epsilon);
    const size_t limit = std::min(rows, columns);
    for (size_t pivot = 0; pivot < limit; ++pivot) {
        size_t selected = pivot;
        for (size_t column = pivot + 1; column < columns; ++column) {
            if (columnNorms[column] > columnNorms[selected]) {
                selected = column;
            }
        }
        if (selected != pivot) {
            for (size_t row = 0; row < rows; ++row) {
                std::swap(work(row, pivot), work(row, selected));
            }
            for (size_t row = 0; row < pivot; ++row) {
                std::swap(r(row, pivot), r(row, selected));
            }
            std::swap(columnNorms[pivot], columnNorms[selected]);
            std::swap(permutation[pivot], permutation[selected]);
        }
        const double columnNorm = std::sqrt(std::max(0.0, columnNorms[pivot]));
        if (!(columnNorm > threshold)) {
            break;
        }
        r(pivot, pivot) = columnNorm;
        for (size_t row = 0; row < rows; ++row) {
            q(row, pivot) = work(row, pivot) / columnNorm;
        }
        ++result.rank;
        for (size_t column = pivot + 1; column < columns; ++column) {
            Complex projection{};
            for (size_t row = 0; row < rows; ++row) {
                projection += std::conj(q(row, pivot)) * work(row, column);
            }
            for (size_t row = 0; row < rows; ++row) {
                work(row, column) -= q(row, pivot) * projection;
            }
            Complex correction{};
            for (size_t row = 0; row < rows; ++row) {
                correction += std::conj(q(row, pivot)) * work(row, column);
            }
            projection += correction;
            r(pivot, column) = projection;
            columnNorms[column] = 0.0;
            for (size_t row = 0; row < rows; ++row) {
                work(row, column) -= q(row, pivot) * correction;
                columnNorms[column] += std::norm(work(row, column));
            }
        }
    }

    if (result.rank < std::min(rows, columns)) {
        result.error = "matrix is rank deficient";
        return result;
    }

    Matrix projected(result.rank, right.columns());
    for (size_t column = 0; column < right.columns(); ++column) {
        for (size_t row = 0; row < result.rank; ++row) {
            for (size_t inner = 0; inner < rows; ++inner) {
                projected(row, column) +=
                    std::conj(q(inner, row)) * right(inner, column);
            }
        }
    }
    Matrix pivoted(columns, right.columns());
    for (size_t column = 0; column < right.columns(); ++column) {
        for (size_t reverse = 0; reverse < result.rank; ++reverse) {
            const size_t row = result.rank - reverse - 1;
            Complex value = projected(row, column);
            for (size_t inner = row + 1; inner < result.rank; ++inner) {
                value -= r(row, inner) * pivoted(inner, column);
            }
            if (!(std::abs(r(row, row)) > threshold)) {
                result.error = "matrix is rank deficient";
                return result;
            }
            pivoted(row, column) = value / r(row, row);
        }
    }
    result.solution = Matrix(columns, right.columns());
    for (size_t pivot = 0; pivot < columns; ++pivot) {
        for (size_t column = 0; column < right.columns(); ++column) {
            result.solution(permutation[pivot], column) =
                pivoted(pivot, column);
        }
    }

    result.upperTriangular = Matrix(columns, columns);
    for (size_t row = 0; row < result.rank; ++row) {
        for (size_t column = row; column < columns; ++column) {
            result.upperTriangular(row, column) = r(row, column);
        }
    }
    result.succeeded = true;
    return result;
}

bool hermitianEigen(Matrix matrix, double epsilon,
                    std::vector<double>& values, Matrix& vectors) {
    const size_t size = matrix.rows();
    if (size != matrix.columns()) {
        return false;
    }
    vectors = Matrix::identity(size);
    if (size == 0) {
        values.clear();
        return true;
    }
    const double threshold = std::max(1.0, frobeniusNorm(matrix)) *
                             static_cast<double>(size) * epsilon;
    const size_t maxSweeps = std::max<size_t>(32, size * 8);
    bool converged = false;
    for (size_t sweep = 0; sweep < maxSweeps; ++sweep) {
        double maximum = 0.0;
        for (size_t column = 1; column < size; ++column) {
            for (size_t row = 0; row < column; ++row) {
                maximum = std::max(maximum, std::abs(matrix(row, column)));
            }
        }
        if (!(maximum > threshold)) {
            converged = true;
            break;
        }
        for (size_t p = 0; p + 1 < size; ++p) {
            for (size_t q = p + 1; q < size; ++q) {
                const Complex offDiagonal = matrix(p, q);
                const double magnitude = std::abs(offDiagonal);
                if (!(magnitude > threshold)) {
                    continue;
                }
                const double diagonalP = matrix(p, p).real();
                const double diagonalQ = matrix(q, q).real();
                const double tau =
                    (diagonalQ - diagonalP) / (2.0 * magnitude);
                const double tangent =
                    tau >= 0.0
                        ? 1.0 / (tau + std::hypot(1.0, tau))
                        : -1.0 / (-tau + std::hypot(1.0, tau));
                const double cosine = 1.0 / std::hypot(1.0, tangent);
                const double sine = tangent * cosine;
                const Complex phase = offDiagonal / magnitude;

                for (size_t row = 0; row < size; ++row) {
                    const Complex valueP = matrix(row, p);
                    const Complex valueQ = matrix(row, q);
                    matrix(row, p) =
                        cosine * valueP - sine * std::conj(phase) * valueQ;
                    matrix(row, q) =
                        sine * phase * valueP + cosine * valueQ;
                }
                for (size_t column = 0; column < size; ++column) {
                    const Complex valueP = matrix(p, column);
                    const Complex valueQ = matrix(q, column);
                    matrix(p, column) =
                        cosine * valueP - sine * phase * valueQ;
                    matrix(q, column) =
                        sine * std::conj(phase) * valueP + cosine * valueQ;
                }
                matrix(p, q) = {};
                matrix(q, p) = {};
                matrix(p, p) = Complex(matrix(p, p).real(), 0.0);
                matrix(q, q) = Complex(matrix(q, q).real(), 0.0);

                for (size_t row = 0; row < size; ++row) {
                    const Complex valueP = vectors(row, p);
                    const Complex valueQ = vectors(row, q);
                    vectors(row, p) =
                        cosine * valueP - sine * std::conj(phase) * valueQ;
                    vectors(row, q) =
                        sine * phase * valueP + cosine * valueQ;
                }
            }
        }
    }
    if (!converged) {
        double maximum = 0.0;
        for (size_t column = 1; column < size; ++column) {
            for (size_t row = 0; row < column; ++row) {
                maximum = std::max(maximum, std::abs(matrix(row, column)));
            }
        }
        converged = maximum <= threshold * 8.0;
    }
    if (!converged) {
        return false;
    }

    std::vector<size_t> order(size);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        return matrix(left, left).real() < matrix(right, right).real();
    });
    values.resize(size);
    Matrix sortedVectors(size, size);
    for (size_t output = 0; output < size; ++output) {
        values[output] = matrix(order[output], order[output]).real();
        for (size_t row = 0; row < size; ++row) {
            sortedVectors(row, output) = vectors(row, order[output]);
        }
    }
    vectors = std::move(sortedVectors);
    return true;
}

void qrDecompose(const Matrix& input, Matrix& q, Matrix& r,
                 double epsilon) {
    const size_t rows = input.rows();
    const size_t columns = input.columns();
    r = input;
    q = Matrix::identity(rows);
    const size_t limit = std::min(rows, columns);
    const double threshold = toleranceFor(input, epsilon);
    for (size_t pivot = 0; pivot < limit; ++pivot) {
        long double normSquared = 0.0L;
        for (size_t row = pivot; row < rows; ++row) {
            normSquared += std::norm(r(row, pivot));
        }
        const double norm = std::sqrt(static_cast<double>(normSquared));
        if (!(norm > threshold)) {
            continue;
        }
        const Complex first = r(pivot, pivot);
        const Complex phase = std::abs(first) == 0.0
                                  ? Complex(1.0, 0.0)
                                  : first / std::abs(first);
        std::vector<Complex> reflector(rows - pivot);
        for (size_t row = pivot; row < rows; ++row) {
            reflector[row - pivot] = r(row, pivot);
        }
        reflector.front() += phase * norm;
        long double reflectorNormSquared = 0.0L;
        for (const Complex value : reflector) {
            reflectorNormSquared += std::norm(value);
        }
        if (!(reflectorNormSquared > 0.0L)) {
            continue;
        }
        const double beta = 2.0 / static_cast<double>(reflectorNormSquared);
        for (size_t column = pivot; column < columns; ++column) {
            Complex projection{};
            for (size_t index = 0; index < reflector.size(); ++index) {
                projection += std::conj(reflector[index]) *
                              r(pivot + index, column);
            }
            projection *= beta;
            for (size_t index = 0; index < reflector.size(); ++index) {
                r(pivot + index, column) -= reflector[index] * projection;
            }
        }
        for (size_t row = 0; row < rows; ++row) {
            Complex projection{};
            for (size_t index = 0; index < reflector.size(); ++index) {
                projection += q(row, pivot + index) * reflector[index];
            }
            projection *= beta;
            for (size_t index = 0; index < reflector.size(); ++index) {
                q(row, pivot + index) -=
                    projection * std::conj(reflector[index]);
            }
        }
    }
}

bool generalEigenvalues(const Matrix& input, double epsilon,
                        std::vector<Complex>& values) {
    const size_t size = input.rows();
    Matrix iterate = input;
    const double threshold = std::max(1.0, frobeniusNorm(input)) *
                             static_cast<double>(size) * epsilon;
    const size_t maxIterations = std::max<size_t>(256, size * size * 128);
    for (size_t iteration = 0; iteration < maxIterations; ++iteration) {
        bool converged = true;
        for (size_t row = 1; row < size; ++row) {
            for (size_t column = 0; column < row; ++column) {
                const double local = threshold *
                    (1.0 + std::abs(iterate(row, row)) +
                     std::abs(iterate(column, column)));
                if (std::abs(iterate(row, column)) <= local) {
                    iterate(row, column) = {};
                } else {
                    converged = false;
                }
            }
        }
        if (converged) {
            values.resize(size);
            for (size_t index = 0; index < size; ++index) {
                values[index] = iterate(index, index);
            }
            return true;
        }

        Complex shift = iterate(size - 1, size - 1);
        if (size > 1) {
            const Complex a = iterate(size - 2, size - 2);
            const Complex b = iterate(size - 2, size - 1);
            const Complex c = iterate(size - 1, size - 2);
            const Complex d = iterate(size - 1, size - 1);
            const Complex trace = (a + d) * 0.5;
            const Complex discriminant =
                std::sqrt((a - d) * (a - d) * 0.25 + b * c);
            const Complex candidateA = trace + discriminant;
            const Complex candidateB = trace - discriminant;
            shift = std::abs(candidateA - d) < std::abs(candidateB - d)
                        ? candidateA
                        : candidateB;
        }
        Matrix shifted = iterate;
        for (size_t index = 0; index < size; ++index) {
            shifted(index, index) -= shift;
        }
        Matrix q;
        Matrix r;
        qrDecompose(shifted, q, r, epsilon);
        iterate = multiply(r, q);
        for (size_t index = 0; index < size; ++index) {
            iterate(index, index) += shift;
        }
    }
    return false;
}

Matrix eigenvectorsFromValues(const Matrix& matrix,
                              const std::vector<Complex>& values,
                              double epsilon) {
    const size_t size = matrix.rows();
    Matrix vectors(size, size);
    for (size_t index = 0; index < size; ++index) {
        Matrix shifted = matrix;
        for (size_t diagonal = 0; diagonal < size; ++diagonal) {
            shifted(diagonal, diagonal) -= values[index];
        }
        const Matrix gram = multiply(adjoint(shifted), shifted);
        std::vector<double> gramValues;
        Matrix gramVectors;
        if (!hermitianEigen(gram, epsilon, gramValues, gramVectors) ||
            gramVectors.columns() == 0) {
            continue;
        }
        for (size_t row = 0; row < size; ++row) {
            vectors(row, index) = gramVectors(row, 0);
        }
    }
    return vectors;
}

bool powerOfTwo(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

void radixTwoTransform(std::vector<Complex>& values, bool inverse) {
    const size_t size = values.size();
    for (size_t index = 1, reversed = 0; index < size; ++index) {
        size_t bit = size >> 1U;
        for (; (reversed & bit) != 0; bit >>= 1U) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    const double pi = std::acos(-1.0);
    for (size_t length = 2; length <= size; length <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * pi /
                             static_cast<double>(length);
        const Complex step(std::cos(angle), std::sin(angle));
        for (size_t offset = 0; offset < size; offset += length) {
            Complex weight(1.0, 0.0);
            const size_t half = length / 2;
            for (size_t index = 0; index < half; ++index) {
                const Complex even = values[offset + index];
                const Complex odd = values[offset + index + half] * weight;
                values[offset + index] = even + odd;
                values[offset + index + half] = even - odd;
                weight *= step;
            }
        }
        if (length == size) {
            break;
        }
    }
    if (inverse) {
        const double scale = 1.0 / static_cast<double>(size);
        for (Complex& value : values) {
            value *= scale;
        }
    }
}

bool bluesteinTransform(std::vector<Complex>& values, bool inverse) {
    const size_t size = values.size();
    if (size > std::numeric_limits<size_t>::max() / 2U + 1U) {
        return false;
    }
    const size_t required = size * 2U - 1U;
    size_t convolutionSize = 1;
    while (convolutionSize < required) {
        if (convolutionSize > std::numeric_limits<size_t>::max() / 2U) {
            return false;
        }
        convolutionSize *= 2U;
    }
    std::vector<Complex> left(convolutionSize);
    std::vector<Complex> right(convolutionSize);
    const double sign = inverse ? 1.0 : -1.0;
    const double pi = std::acos(-1.0);
    for (size_t index = 0; index < size; ++index) {
        const long double squared =
            static_cast<long double>(index) *
            static_cast<long double>(index);
        const double angle = static_cast<double>(
            std::fmod(squared, static_cast<long double>(size) * 2.0L)) *
            pi / static_cast<double>(size);
        const Complex chirp(std::cos(sign * angle),
                            std::sin(sign * angle));
        const Complex inverseChirp(std::cos(-sign * angle),
                                   std::sin(-sign * angle));
        left[index] = values[index] * chirp;
        right[index] = inverseChirp;
        if (index != 0) {
            right[convolutionSize - index] = inverseChirp;
        }
    }
    radixTwoTransform(left, false);
    radixTwoTransform(right, false);
    for (size_t index = 0; index < convolutionSize; ++index) {
        left[index] *= right[index];
    }
    radixTwoTransform(left, true);
    for (size_t index = 0; index < size; ++index) {
        const long double squared =
            static_cast<long double>(index) *
            static_cast<long double>(index);
        const double angle = static_cast<double>(
            std::fmod(squared, static_cast<long double>(size) * 2.0L)) *
            pi / static_cast<double>(size);
        const Complex chirp(std::cos(sign * angle),
                            std::sin(sign * angle));
        values[index] = left[index] * chirp;
        if (inverse) {
            values[index] /= static_cast<double>(size);
        }
    }
    return true;
}

} // namespace

Matrix::Matrix(size_t rows, size_t columns, Complex value)
    : rows_(rows), columns_(columns) {
    if (rows != 0 &&
        columns > std::numeric_limits<size_t>::max() / rows) {
        throw std::length_error("native numeric matrix dimensions overflow");
    }
    values_.assign(rows * columns, value);
}

size_t Matrix::rows() const noexcept { return rows_; }
size_t Matrix::columns() const noexcept { return columns_; }
size_t Matrix::size() const noexcept { return values_.size(); }
bool Matrix::empty() const noexcept { return values_.empty(); }

Complex& Matrix::operator()(size_t row, size_t column) {
    return values_[row + column * rows_];
}

const Complex& Matrix::operator()(size_t row, size_t column) const {
    return values_[row + column * rows_];
}

const std::vector<Complex>& Matrix::values() const noexcept { return values_; }
std::vector<Complex>& Matrix::values() noexcept { return values_; }

Matrix Matrix::identity(size_t size) {
    Matrix result(size, size);
    for (size_t index = 0; index < size; ++index) {
        result(index, index) = 1.0;
    }
    return result;
}

Matrix adjoint(const Matrix& matrix) {
    Matrix result(matrix.columns(), matrix.rows());
    for (size_t column = 0; column < matrix.columns(); ++column) {
        for (size_t row = 0; row < matrix.rows(); ++row) {
            result(column, row) = std::conj(matrix(row, column));
        }
    }
    return result;
}

Matrix multiply(const Matrix& left, const Matrix& right) {
    if (left.columns() != right.rows()) {
        return {};
    }
    Matrix result(left.rows(), right.columns());
    for (size_t column = 0; column < right.columns(); ++column) {
        for (size_t inner = 0; inner < left.columns(); ++inner) {
            const Complex rightValue = right(inner, column);
            for (size_t row = 0; row < left.rows(); ++row) {
                result(row, column) += left(row, inner) * rightValue;
            }
        }
    }
    return result;
}

double frobeniusNorm(const Matrix& matrix) {
    long double sum = 0.0L;
    for (const Complex value : matrix.values()) {
        sum += static_cast<long double>(std::norm(value));
    }
    return std::sqrt(static_cast<double>(sum));
}

double oneNorm(const Matrix& matrix) {
    double result = 0.0;
    for (size_t column = 0; column < matrix.columns(); ++column) {
        double sum = 0.0;
        for (size_t row = 0; row < matrix.rows(); ++row) {
            sum += std::abs(matrix(row, column));
        }
        result = std::max(result, sum);
    }
    return result;
}

double infinityNorm(const Matrix& matrix) {
    double result = 0.0;
    for (size_t row = 0; row < matrix.rows(); ++row) {
        double sum = 0.0;
        for (size_t column = 0; column < matrix.columns(); ++column) {
            sum += std::abs(matrix(row, column));
        }
        result = std::max(result, sum);
    }
    return result;
}

std::optional<Complex> determinant(const Matrix& matrix, double epsilon) {
    if (matrix.rows() != matrix.columns()) {
        return std::nullopt;
    }
    if (matrix.rows() == 0) {
        return Complex(1.0, 0.0);
    }
    const auto factorization = factorLu(matrix, epsilon);
    if (factorization.singular) {
        return Complex{};
    }
    Complex result = static_cast<double>(factorization.parity);
    for (size_t index = 0; index < matrix.rows(); ++index) {
        result *= factorization.factors(index, index);
    }
    return result;
}

SolveResult solve(const Matrix& left, const Matrix& right, double epsilon) {
    SolveResult result;
    if (left.rows() != right.rows()) {
        result.error = "left and right row counts do not match";
        return result;
    }
    if (left.rows() == left.columns()) {
        if (left.rows() == 0) {
            result.succeeded = true;
            result.solution = Matrix(0, right.columns());
            result.rank = 0;
            return result;
        }
        const auto factorization = factorLu(left, epsilon);
        const auto solution = solveLu(factorization, right);
        if (!solution) {
            result.error = "matrix is singular";
            return result;
        }
        result.succeeded = true;
        result.solution = *solution;
        result.rank = left.rows();
        result.upperTriangular = Matrix(left.rows(), left.columns());
        for (size_t row = 0; row < left.rows(); ++row) {
            for (size_t column = row; column < left.columns(); ++column) {
                result.upperTriangular(row, column) =
                    factorization.factors(row, column);
            }
        }
        return result;
    }
    if (left.rows() > left.columns()) {
        return solveLeastSquares(left, right, epsilon);
    }

    const Matrix leftAdjoint = adjoint(left);
    const Matrix gram = multiply(left, leftAdjoint);
    const auto gramSolution = solve(gram, right, epsilon);
    if (!gramSolution.succeeded) {
        result.error = "underdetermined matrix is rank deficient";
        return result;
    }
    result.succeeded = true;
    result.solution = multiply(leftAdjoint, gramSolution.solution);
    result.rank = left.rows();
    return result;
}

SolveResult solveLeastSquaresUnpivoted(const Matrix& left,
                                       const Matrix& right,
                                       double epsilon) {
    SolveResult result;
    if (left.rows() != right.rows()) {
        result.error = "left and right row counts do not match";
        return result;
    }
    if (left.rows() < left.columns()) {
        result.error = "unpivoted QR requires at least as many rows as columns";
        return result;
    }
    if (left.columns() == 0) {
        result.succeeded = true;
        result.solution = Matrix(0, right.columns());
        result.upperTriangular = Matrix(0, 0);
        return result;
    }

    Matrix q;
    Matrix packedR;
    qrDecompose(left, q, packedR, epsilon);
    const size_t columns = left.columns();
    const double threshold = toleranceFor(left, epsilon);
    result.upperTriangular = Matrix(columns, columns);
    for (size_t row = 0; row < columns; ++row) {
        if (!(std::abs(packedR(row, row)) > threshold)) {
            result.error = "matrix is rank deficient";
            return result;
        }
        for (size_t column = row; column < columns; ++column) {
            result.upperTriangular(row, column) = packedR(row, column);
        }
    }

    Matrix projected(columns, right.columns());
    for (size_t column = 0; column < right.columns(); ++column) {
        for (size_t row = 0; row < columns; ++row) {
            for (size_t inner = 0; inner < left.rows(); ++inner) {
                projected(row, column) +=
                    std::conj(q(inner, row)) * right(inner, column);
            }
        }
    }
    result.solution = Matrix(columns, right.columns());
    for (size_t column = 0; column < right.columns(); ++column) {
        for (size_t reverse = 0; reverse < columns; ++reverse) {
            const size_t row = columns - reverse - 1;
            Complex value = projected(row, column);
            for (size_t inner = row + 1; inner < columns; ++inner) {
                value -= result.upperTriangular(row, inner) *
                         result.solution(inner, column);
            }
            result.solution(row, column) =
                value / result.upperTriangular(row, row);
        }
    }
    result.rank = columns;
    result.succeeded = true;
    return result;
}

std::optional<Matrix> inverse(const Matrix& matrix, double epsilon) {
    if (matrix.rows() != matrix.columns()) {
        return std::nullopt;
    }
    const auto result = solve(matrix, Matrix::identity(matrix.rows()), epsilon);
    return result.succeeded ? std::optional<Matrix>(result.solution)
                            : std::nullopt;
}

std::vector<double> singularValues(const Matrix& matrix, double epsilon) {
    if (matrix.rows() == 0 || matrix.columns() == 0) {
        return {};
    }
    const Matrix gram = matrix.rows() >= matrix.columns()
                            ? multiply(adjoint(matrix), matrix)
                            : multiply(matrix, adjoint(matrix));
    std::vector<double> eigenvalues;
    Matrix vectors;
    if (!hermitianEigen(gram, epsilon, eigenvalues, vectors)) {
        return {};
    }
    std::vector<double> result;
    result.reserve(eigenvalues.size());
    const double negativeTolerance =
        std::max(1.0, frobeniusNorm(gram)) * epsilon *
        static_cast<double>(gram.rows());
    for (double value : eigenvalues) {
        if (value < 0.0 && std::abs(value) <= negativeTolerance) {
            value = 0.0;
        }
        result.push_back(std::sqrt(std::max(0.0, value)));
    }
    std::sort(result.begin(), result.end(), std::greater<double>());
    return result;
}

EigenResult eigen(const Matrix& matrix, double epsilon,
                  bool computeVectors) {
    EigenResult result;
    if (matrix.rows() != matrix.columns()) {
        return result;
    }
    const size_t size = matrix.rows();
    if (size == 0) {
        result.converged = true;
        result.vectors = Matrix(0, 0);
        return result;
    }
    bool isHermitian = true;
    const double threshold =
        std::max(1.0, frobeniusNorm(matrix)) * epsilon *
        static_cast<double>(size) * 8.0;
    for (size_t column = 0; column < size && isHermitian; ++column) {
        for (size_t row = 0; row <= column; ++row) {
            if (std::abs(matrix(row, column) -
                         std::conj(matrix(column, row))) > threshold) {
                isHermitian = false;
                break;
            }
        }
    }
    if (isHermitian) {
        std::vector<double> realValues;
        Matrix vectors;
        result.converged =
            hermitianEigen(matrix, epsilon, realValues, vectors);
        result.values.reserve(realValues.size());
        for (const double value : realValues) {
            result.values.emplace_back(value, 0.0);
        }
        if (computeVectors) {
            result.vectors = std::move(vectors);
        }
        return result;
    }

    result.converged = generalEigenvalues(matrix, epsilon, result.values);
    if (result.converged && computeVectors) {
        result.vectors = eigenvectorsFromValues(matrix, result.values,
                                                epsilon);
    }
    return result;
}

bool transform(std::vector<Complex>& values, bool inverse) {
    if (values.empty()) {
        return true;
    }
    if (powerOfTwo(values.size())) {
        radixTwoTransform(values, inverse);
        return true;
    }
    return bluesteinTransform(values, inverse);
}

} // namespace mparser::native_numeric
