#pragma once

#include <complex>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mparser::native_numeric {

using Complex = std::complex<double>;

class Matrix {
public:
    Matrix() = default;
    Matrix(size_t rows, size_t columns, Complex value = {});

    [[nodiscard]] size_t rows() const noexcept;
    [[nodiscard]] size_t columns() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    Complex& operator()(size_t row, size_t column);
    const Complex& operator()(size_t row, size_t column) const;

    [[nodiscard]] const std::vector<Complex>& values() const noexcept;
    [[nodiscard]] std::vector<Complex>& values() noexcept;

    static Matrix identity(size_t size);

private:
    size_t rows_ = 0;
    size_t columns_ = 0;
    std::vector<Complex> values_;
};

struct SolveResult {
    bool succeeded = false;
    Matrix solution;
    Matrix upperTriangular;
    size_t rank = 0;
    std::string error;
};

struct EigenResult {
    bool converged = false;
    std::vector<Complex> values;
    Matrix vectors;
};

[[nodiscard]] Matrix adjoint(const Matrix& matrix);
[[nodiscard]] Matrix multiply(const Matrix& left, const Matrix& right);
[[nodiscard]] double frobeniusNorm(const Matrix& matrix);
[[nodiscard]] double oneNorm(const Matrix& matrix);
[[nodiscard]] double infinityNorm(const Matrix& matrix);

[[nodiscard]] std::optional<Complex> determinant(
    const Matrix& matrix, double epsilon);
[[nodiscard]] SolveResult solve(const Matrix& left, const Matrix& right,
                                double epsilon);
[[nodiscard]] SolveResult solveLeastSquaresUnpivoted(
    const Matrix& left, const Matrix& right, double epsilon);
[[nodiscard]] std::optional<Matrix> inverse(const Matrix& matrix,
                                            double epsilon);
[[nodiscard]] std::vector<double> singularValues(const Matrix& matrix,
                                                 double epsilon);
[[nodiscard]] EigenResult eigen(const Matrix& matrix, double epsilon,
                                bool computeVectors);

[[nodiscard]] bool transform(std::vector<Complex>& values, bool inverse);

} // namespace mparser::native_numeric
