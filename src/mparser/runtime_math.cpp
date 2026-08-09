#include "mparser/runtime_math.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

namespace mparser {
namespace {

std::int64_t signedMinimum(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
        return std::numeric_limits<std::int8_t>::min();
    case RuntimeNumericClass::Int16:
        return std::numeric_limits<std::int16_t>::min();
    case RuntimeNumericClass::Int32:
        return std::numeric_limits<std::int32_t>::min();
    case RuntimeNumericClass::Int64:
        return std::numeric_limits<std::int64_t>::min();
    default:
        return 0;
    }
}

std::int64_t signedMaximum(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
        return std::numeric_limits<std::int8_t>::max();
    case RuntimeNumericClass::Int16:
        return std::numeric_limits<std::int16_t>::max();
    case RuntimeNumericClass::Int32:
        return std::numeric_limits<std::int32_t>::max();
    case RuntimeNumericClass::Int64:
        return std::numeric_limits<std::int64_t>::max();
    default:
        return 0;
    }
}

std::optional<std::complex<double>> applyComplexMath(
    std::string_view name, std::complex<double> value) {
    if (name == "acos") {
        return std::acos(value);
    }
    if (name == "acosh") {
        return std::acosh(value);
    }
    if (name == "asin") {
        return std::asin(value);
    }
    if (name == "asinh") {
        return std::asinh(value);
    }
    if (name == "atan") {
        return std::atan(value);
    }
    if (name == "atanh") {
        return std::atanh(value);
    }
    if (name == "cos") {
        return std::cos(value);
    }
    if (name == "cosh") {
        return std::cosh(value);
    }
    if (name == "exp") {
        return std::exp(value);
    }
    if (name == "log") {
        return std::log(value);
    }
    if (name == "log10") {
        return std::log10(value);
    }
    if (name == "log2") {
        return std::log(value) / std::log(2.0);
    }
    if (name == "sin") {
        return std::sin(value);
    }
    if (name == "sinh") {
        return std::sinh(value);
    }
    if (name == "sqrt") {
        return std::sqrt(value);
    }
    if (name == "tan") {
        return std::tan(value);
    }
    if (name == "tanh") {
        return std::tanh(value);
    }
    return std::nullopt;
}

bool isRoundingMath(std::string_view name) {
    return name == "ceil" || name == "fix" || name == "floor" ||
           name == "round";
}

bool isPredicateMath(std::string_view name) {
    return name == "isfinite" || name == "isinf" ||
           name == "isnan";
}

double applyRoundingMath(std::string_view name, double value) {
    if (name == "ceil") {
        return std::ceil(value);
    }
    if (name == "fix") {
        return std::trunc(value);
    }
    if (name == "floor") {
        return std::floor(value);
    }
    return std::round(value);
}

bool applyPredicateMath(std::string_view name,
                        const RuntimeNumericElementValue& value) {
    if (name == "isfinite") {
        return std::isfinite(value.real) &&
               (!value.complex || std::isfinite(value.imaginary));
    }
    if (name == "isinf") {
        return std::isinf(value.real) ||
               (value.complex && std::isinf(value.imaginary));
    }
    return std::isnan(value.real) ||
           (value.complex && std::isnan(value.imaginary));
}

std::optional<RuntimeNumericElementValue> integerAbsolute(
    const RuntimeNumericElementValue& source) {
    if (source.complex) {
        return std::nullopt;
    }
    RuntimeNumericElementValue result = source;
    if (!runtimeNumericClassIsSignedInteger(source.numericClass)) {
        return result;
    }
    const std::int64_t value =
        std::bit_cast<std::int64_t>(source.integerRealBits);
    const std::int64_t minimum = signedMinimum(source.numericClass);
    const std::int64_t absolute =
        value == minimum ? signedMaximum(source.numericClass)
                         : value < 0 ? -value : value;
    result.integerRealBits = std::bit_cast<std::uint64_t>(absolute);
    result.real = static_cast<double>(absolute);
    return result;
}

RuntimeNumericElementValue integerSign(
    const RuntimeNumericElementValue& source) {
    RuntimeNumericElementValue result = source;
    std::int64_t sign = 0;
    if (runtimeNumericClassIsSignedInteger(source.numericClass)) {
        const std::int64_t value =
            std::bit_cast<std::int64_t>(source.integerRealBits);
        sign = value < 0 ? -1 : value > 0 ? 1 : 0;
        result.integerRealBits = std::bit_cast<std::uint64_t>(sign);
    } else {
        result.integerRealBits = source.integerRealBits == 0 ? 0 : 1;
        sign = source.integerRealBits == 0 ? 0 : 1;
    }
    result.real = static_cast<double>(sign);
    return result;
}

RuntimeNumericElementValue floatingSign(
    const RuntimeNumericElementValue& source) {
    RuntimeNumericElementValue result;
    if (!source.complex) {
        result.real = source.real < 0.0 ? -1.0
                    : source.real > 0.0 ? 1.0
                    : source.real == 0.0 ? 0.0
                                         : source.real;
        return result;
    }
    const std::complex<double> input(source.real, source.imaginary);
    const double magnitude = std::abs(input);
    const std::complex<double> sign =
        magnitude == 0.0 ? std::complex<double>{0.0, 0.0}
                         : input / magnitude;
    result.real = sign.real();
    result.imaginary = sign.imag();
    result.complex = true;
    return result;
}

double floatingSpacing(double magnitude,
                       RuntimeNumericClass numericClass) {
    if (!std::isfinite(magnitude)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    magnitude = std::fabs(magnitude);
    if (numericClass == RuntimeNumericClass::Single) {
        const float value = static_cast<float>(magnitude);
        if (value == 0.0F || std::fpclassify(value) == FP_SUBNORMAL) {
            return std::numeric_limits<float>::denorm_min();
        }
        return std::ldexp(std::numeric_limits<float>::epsilon(),
                          std::ilogb(value));
    }
    if (magnitude == 0.0 ||
        std::fpclassify(magnitude) == FP_SUBNORMAL) {
        return std::numeric_limits<double>::denorm_min();
    }
    return std::ldexp(std::numeric_limits<double>::epsilon(),
                      std::ilogb(magnitude));
}

double floatingNextPowerExponent(double magnitude) {
    if (magnitude == 0.0 || !std::isfinite(magnitude)) {
        return magnitude;
    }
    int exponent = 0;
    const double fraction = std::frexp(std::fabs(magnitude), &exponent);
    return static_cast<double>(
        fraction == 0.5 ? exponent - 1 : exponent);
}

RuntimeNumericElementValue integerNextPowerExponent(
    const RuntimeNumericElementValue& source) {
    const std::uint64_t magnitude =
        runtimeNumericClassIsSignedInteger(source.numericClass)
            ? [&] {
                  const std::int64_t value =
                      std::bit_cast<std::int64_t>(
                          source.integerRealBits);
                  return value >= 0
                             ? static_cast<std::uint64_t>(value)
                             : static_cast<std::uint64_t>(
                                   -(value + 1)) + 1;
              }()
            : source.integerRealBits;
    const std::uint64_t exponent =
        magnitude == 0
            ? 0
            : static_cast<std::uint64_t>(
                  std::bit_width(magnitude) -
                  ((magnitude & (magnitude - 1)) == 0 ? 1 : 0));

    RuntimeNumericElementValue result;
    result.numericClass = source.numericClass;
    result.real = static_cast<double>(exponent);
    result.integerRealBits = exponent;
    return result;
}

} // namespace

bool isRuntimePureUnaryMathBuiltin(std::string_view name) {
    return name == "abs" || name == "acos" || name == "acosh" ||
           name == "asin" || name == "asinh" || name == "atan" ||
           name == "atanh" || name == "ceil" || name == "cos" ||
           name == "cosh" || name == "exp" || name == "fix" ||
           name == "floor" || name == "isfinite" ||
           name == "isinf" || name == "isnan" || name == "log" ||
           name == "log10" || name == "log2" ||
           name == "nextpow2" || name == "round" ||
           name == "sign" || name == "sin" || name == "sinh" ||
           name == "sqrt" || name == "tan" || name == "tanh";
}

std::optional<double>
runtimeApplyPureUnaryMathBuiltin(std::string_view name, double value) {
    if (name == "abs") {
        return std::fabs(value);
    }
    if (name == "acos") {
        return std::acos(value);
    }
    if (name == "acosh") {
        return std::acosh(value);
    }
    if (name == "asin") {
        return std::asin(value);
    }
    if (name == "asinh") {
        return std::asinh(value);
    }
    if (name == "atan") {
        return std::atan(value);
    }
    if (name == "atanh") {
        return std::atanh(value);
    }
    if (isRoundingMath(name)) {
        return applyRoundingMath(name, value);
    }
    if (name == "cos") {
        return std::cos(value);
    }
    if (name == "cosh") {
        return std::cosh(value);
    }
    if (name == "exp") {
        return std::exp(value);
    }
    if (name == "log") {
        return std::log(value);
    }
    if (name == "log10") {
        return std::log10(value);
    }
    if (name == "log2") {
        return std::log2(value);
    }
    if (name == "nextpow2") {
        return floatingNextPowerExponent(std::fabs(value));
    }
    if (name == "sign") {
        return value < 0.0 ? -1.0 : value > 0.0 ? 1.0
                                              : value == 0.0 ? 0.0 : value;
    }
    if (name == "sin") {
        return std::sin(value);
    }
    if (name == "sinh") {
        return std::sinh(value);
    }
    if (name == "sqrt") {
        return std::sqrt(value);
    }
    if (name == "tan") {
        return std::tan(value);
    }
    if (name == "tanh") {
        return std::tanh(value);
    }
    if (name == "isfinite") {
        return std::isfinite(value) ? 1.0 : 0.0;
    }
    if (name == "isinf") {
        return std::isinf(value) ? 1.0 : 0.0;
    }
    if (name == "isnan") {
        return std::isnan(value) ? 1.0 : 0.0;
    }
    return std::nullopt;
}

std::optional<RuntimeValue>
runtimeApplyPureUnaryMathBuiltin(std::string_view name,
                                 const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    RuntimeNumericClass resultClass = value.numericClass;
    if (isPredicateMath(name)) {
        resultClass = RuntimeNumericClass::Logical;
    } else if (runtimeNumericClassIsInteger(value.numericClass)) {
        if ((name != "abs" && name != "sign" &&
             name != "nextpow2" && !isRoundingMath(name)) ||
            value.numericComplex) {
            return std::nullopt;
        }
    } else if (value.numericClass == RuntimeNumericClass::Logical) {
        resultClass = RuntimeNumericClass::Double;
    }

    const auto dimensions = runtimeDimensions(value);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return std::nullopt;
    }
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto source = runtimeNumericElementValue(value, index);
        if (!source) {
            return std::nullopt;
        }
        if (isPredicateMath(name)) {
            RuntimeNumericElementValue mapped;
            mapped.real = applyPredicateMath(name, *source) ? 1.0 : 0.0;
            elements.push_back(mapped);
            continue;
        }
        if (name == "nextpow2") {
            if (runtimeNumericClassIsInteger(value.numericClass)) {
                elements.push_back(
                    integerNextPowerExponent(*source));
            } else {
                RuntimeNumericElementValue mapped;
                double magnitude = source->complex
                                       ? std::hypot(source->real,
                                                    source->imaginary)
                                       : std::fabs(source->real);
                if (value.numericClass == RuntimeNumericClass::Single) {
                    magnitude = static_cast<double>(
                        static_cast<float>(magnitude));
                }
                mapped.real = floatingNextPowerExponent(magnitude);
                elements.push_back(mapped);
            }
            continue;
        }
        if (runtimeNumericClassIsInteger(value.numericClass)) {
            if (name == "abs") {
                const auto mapped = integerAbsolute(*source);
                if (!mapped) {
                    return std::nullopt;
                }
                elements.push_back(*mapped);
            } else if (name == "sign") {
                elements.push_back(integerSign(*source));
            } else {
                elements.push_back(*source);
            }
            continue;
        }

        const std::complex<double> input(
            source->real,
            source->complex ? source->imaginary : 0.0);
        RuntimeNumericElementValue mapped;
        if (name == "abs") {
            mapped.real = std::abs(input);
        } else if (isRoundingMath(name)) {
            mapped.real = applyRoundingMath(name, input.real());
            mapped.imaginary = applyRoundingMath(name, input.imag());
            mapped.complex = source->complex;
        } else if (name == "sign") {
            mapped = floatingSign(*source);
        } else {
            const auto result = applyComplexMath(name, input);
            if (!result) {
                return std::nullopt;
            }
            mapped.real = result->real();
            mapped.imaginary = result->imag();
            if (!source->complex &&
                std::fabs(source->real) > 1.0 &&
                name == "asin") {
                mapped.imaginary = std::copysign(
                    std::fabs(mapped.imaginary), -source->real);
            } else if (!source->complex &&
                       std::fabs(source->real) > 1.0 &&
                       name == "acos") {
                mapped.imaginary = std::copysign(
                    std::fabs(mapped.imaginary), source->real);
            }
            mapped.complex =
                source->complex || mapped.imaginary != 0.0;
        }
        elements.push_back(mapped);
    }

    return runtimeNumericValueFromElements(
        dimensions, std::move(elements), resultClass);
}

bool isRuntimePureBinaryMathBuiltin(std::string_view name) {
    return name == "atan2" || name == "hypot" ||
           name == "mod" || name == "rem";
}

std::optional<RuntimeValue>
runtimeApplyPureBinaryMathBuiltin(std::string_view name,
                                  const RuntimeValue& left,
                                  const RuntimeValue& right) {
    if (name == "mod" || name == "rem") {
        auto result = runtimeApplyNumericBinary(name, left, right);
        return result.succeeded
                   ? std::optional<RuntimeValue>(
                         std::move(result.value))
                   : std::nullopt;
    }
    if (!isRuntimePureBinaryMathBuiltin(name) ||
        !isRuntimeNumericValue(left) || !isRuntimeNumericValue(right) ||
        left.numericComplex || right.numericComplex ||
        runtimeNumericClassIsInteger(left.numericClass) ||
        runtimeNumericClassIsInteger(right.numericClass)) {
        return std::nullopt;
    }

    const auto leftDimensions = runtimeDimensions(left);
    const auto rightDimensions = runtimeDimensions(right);
    const auto dimensions = runtimeImplicitExpansionDimensions(
        leftDimensions, rightDimensions);
    if (!dimensions) {
        return std::nullopt;
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return std::nullopt;
    }

    const RuntimeNumericClass resultClass =
        left.numericClass == RuntimeNumericClass::Single ||
                right.numericClass == RuntimeNumericClass::Single
            ? RuntimeNumericClass::Single
            : RuntimeNumericClass::Double;
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, *dimensions);
        const auto leftOffset = coordinates
                                    ? runtimeImplicitExpansionStorageOffset(
                                          *coordinates, leftDimensions)
                                    : std::nullopt;
        const auto rightOffset = coordinates
                                     ? runtimeImplicitExpansionStorageOffset(
                                           *coordinates, rightDimensions)
                                     : std::nullopt;
        const auto leftValue = leftOffset
                                   ? runtimeNumericStorageElementValue(
                                         left, *leftOffset)
                                   : std::nullopt;
        const auto rightValue = rightOffset
                                    ? runtimeNumericStorageElementValue(
                                          right, *rightOffset)
                                    : std::nullopt;
        if (!leftValue || !rightValue) {
            return std::nullopt;
        }
        RuntimeNumericElementValue element;
        element.real = name == "atan2"
                           ? std::atan2(leftValue->real, rightValue->real)
                           : std::hypot(leftValue->real, rightValue->real);
        elements.push_back(element);
    }
    return runtimeNumericValueFromElements(
        *dimensions, std::move(elements), resultClass);
}

std::optional<RuntimeValue>
runtimeEpsilonBuiltin(const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() > 1) {
        return std::nullopt;
    }
    if (arguments.empty()) {
        return runtimeNumericValueFromLogicalOrder(
            {1, 1}, {std::numeric_limits<double>::epsilon()},
            RuntimeNumericClass::Double);
    }

    if (const auto className = runtimeTextScalarUtf8(arguments.front())) {
        if (*className != "double" && *className != "single") {
            return std::nullopt;
        }
        const RuntimeNumericClass numericClass =
            *className == "single" ? RuntimeNumericClass::Single
                                     : RuntimeNumericClass::Double;
        return runtimeNumericValueFromLogicalOrder(
            {1, 1},
            {numericClass == RuntimeNumericClass::Single
                 ? static_cast<double>(std::numeric_limits<float>::epsilon())
                 : std::numeric_limits<double>::epsilon()},
            numericClass);
    }

    const RuntimeValue& input = arguments.front();
    if (!isRuntimeNumericValue(input) ||
        !runtimeNumericClassIsFloating(input.numericClass)) {
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(input);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return std::nullopt;
    }
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto source = runtimeNumericElementValue(input, index);
        if (!source) {
            return std::nullopt;
        }
        RuntimeNumericElementValue element;
        element.real = floatingSpacing(
            source->complex
                ? std::hypot(source->real, source->imaginary)
                : source->real,
            input.numericClass);
        elements.push_back(element);
    }
    return runtimeNumericValueFromElements(
        dimensions, std::move(elements), input.numericClass);
}

} // namespace mparser
