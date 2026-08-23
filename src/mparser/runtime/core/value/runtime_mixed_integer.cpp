#include "mparser/runtime/core/value/runtime_mixed_integer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace mparser {
namespace {

class BigUnsigned {
  public:
    BigUnsigned() = default;

    explicit BigUnsigned(std::uint64_t value) {
        if (value != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(value));
            const auto high = static_cast<std::uint32_t>(value >> 32U);
            if (high != 0) {
                limbs_.push_back(high);
            }
        }
    }

    [[nodiscard]] bool empty() const { return limbs_.empty(); }

    [[nodiscard]] size_t bitLength() const {
        if (limbs_.empty()) {
            return 0;
        }
        return (limbs_.size() - 1) * 32U +
               (32U - static_cast<size_t>(
                          std::countl_zero(limbs_.back())));
    }

    [[nodiscard]] bool bit(size_t index) const {
        const size_t word = index / 32U;
        return word < limbs_.size() &&
               ((limbs_[word] >> (index % 32U)) & 1U) != 0;
    }

    [[nodiscard]] bool anyBitBelow(size_t exclusive) const {
        const size_t completeWords = exclusive / 32U;
        for (size_t index = 0;
             index < std::min(completeWords, limbs_.size()); ++index) {
            if (limbs_[index] != 0) {
                return true;
            }
        }
        const unsigned remainder = static_cast<unsigned>(exclusive % 32U);
        if (remainder == 0 || completeWords >= limbs_.size()) {
            return false;
        }
        const std::uint32_t mask =
            (std::uint32_t{1} << remainder) - 1U;
        return (limbs_[completeWords] & mask) != 0;
    }

    void setBit(size_t index) {
        const size_t word = index / 32U;
        if (limbs_.size() <= word) {
            limbs_.resize(word + 1, 0);
        }
        limbs_[word] |= std::uint32_t{1} << (index % 32U);
    }

    [[nodiscard]] int compare(const BigUnsigned& other) const {
        if (limbs_.size() != other.limbs_.size()) {
            return limbs_.size() < other.limbs_.size() ? -1 : 1;
        }
        for (size_t index = limbs_.size(); index != 0; --index) {
            if (limbs_[index - 1] != other.limbs_[index - 1]) {
                return limbs_[index - 1] < other.limbs_[index - 1]
                           ? -1
                           : 1;
            }
        }
        return 0;
    }

    void add(const BigUnsigned& other) {
        const size_t count = std::max(limbs_.size(), other.limbs_.size());
        limbs_.resize(count, 0);
        std::uint64_t carry = 0;
        for (size_t index = 0; index < count; ++index) {
            const std::uint64_t right =
                index < other.limbs_.size() ? other.limbs_[index] : 0;
            const std::uint64_t sum =
                static_cast<std::uint64_t>(limbs_[index]) + right + carry;
            limbs_[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32U;
        }
        if (carry != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void increment() { add(BigUnsigned(1)); }

    void subtract(const BigUnsigned& other) {
        std::uint64_t borrow = 0;
        for (size_t index = 0; index < limbs_.size(); ++index) {
            const std::uint64_t right =
                (index < other.limbs_.size() ? other.limbs_[index] : 0) +
                borrow;
            const std::uint64_t left = limbs_[index];
            limbs_[index] = static_cast<std::uint32_t>(left - right);
            borrow = left < right ? 1 : 0;
        }
        normalize();
    }

    void shiftLeft(size_t bits) {
        if (limbs_.empty() || bits == 0) {
            return;
        }
        const size_t words = bits / 32U;
        const unsigned remainder = static_cast<unsigned>(bits % 32U);
        if (words != 0) {
            limbs_.insert(limbs_.begin(), words, 0);
        }
        if (remainder == 0) {
            return;
        }
        std::uint64_t carry = 0;
        for (auto& limb : limbs_) {
            const std::uint64_t shifted =
                (static_cast<std::uint64_t>(limb) << remainder) | carry;
            limb = static_cast<std::uint32_t>(shifted);
            carry = shifted >> 32U;
        }
        if (carry != 0) {
            limbs_.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void shiftRight(size_t bits) {
        if (limbs_.empty() || bits == 0) {
            return;
        }
        const size_t words = bits / 32U;
        const unsigned remainder = static_cast<unsigned>(bits % 32U);
        if (words >= limbs_.size()) {
            limbs_.clear();
            return;
        }
        if (words != 0) {
            limbs_.erase(limbs_.begin(), limbs_.begin() +
                                             static_cast<std::ptrdiff_t>(words));
        }
        if (remainder != 0) {
            std::uint32_t carry = 0;
            for (size_t index = limbs_.size(); index != 0; --index) {
                const std::uint32_t next = limbs_[index - 1];
                limbs_[index - 1] =
                    (next >> remainder) |
                    (carry << (32U - remainder));
                carry = next & ((std::uint32_t{1} << remainder) - 1U);
            }
        }
        normalize();
    }

    [[nodiscard]] std::uint64_t toUint64() const {
        std::uint64_t value = limbs_.empty() ? 0 : limbs_[0];
        if (limbs_.size() > 1) {
            value |= static_cast<std::uint64_t>(limbs_[1]) << 32U;
        }
        return value;
    }

    [[nodiscard]] static BigUnsigned multiply(const BigUnsigned& left,
                                              const BigUnsigned& right) {
        BigUnsigned result;
        if (left.empty() || right.empty()) {
            return result;
        }
        result.limbs_.assign(left.limbs_.size() + right.limbs_.size(), 0);
        for (size_t leftIndex = 0; leftIndex < left.limbs_.size();
             ++leftIndex) {
            std::uint64_t carry = 0;
            for (size_t rightIndex = 0; rightIndex < right.limbs_.size();
                 ++rightIndex) {
                const size_t output = leftIndex + rightIndex;
                const std::uint64_t product =
                    static_cast<std::uint64_t>(left.limbs_[leftIndex]) *
                        right.limbs_[rightIndex] +
                    result.limbs_[output] + carry;
                result.limbs_[output] =
                    static_cast<std::uint32_t>(product);
                carry = product >> 32U;
            }
            size_t output = leftIndex + right.limbs_.size();
            while (carry != 0) {
                const std::uint64_t sum =
                    static_cast<std::uint64_t>(result.limbs_[output]) + carry;
                result.limbs_[output] = static_cast<std::uint32_t>(sum);
                carry = sum >> 32U;
                ++output;
                if (carry != 0 && output == result.limbs_.size()) {
                    result.limbs_.push_back(0);
                }
            }
        }
        result.normalize();
        return result;
    }

    static std::pair<BigUnsigned, BigUnsigned>
    divide(const BigUnsigned& numerator, const BigUnsigned& denominator) {
        BigUnsigned quotient;
        BigUnsigned remainder;
        if (denominator.empty()) {
            return {std::move(quotient), std::move(remainder)};
        }
        const size_t bits = numerator.bitLength();
        for (size_t index = bits; index != 0; --index) {
            remainder.shiftLeft(1);
            if (numerator.bit(index - 1)) {
                remainder.increment();
            }
            if (remainder.compare(denominator) >= 0) {
                remainder.subtract(denominator);
                quotient.setBit(index - 1);
            }
        }
        return {std::move(quotient), std::move(remainder)};
    }

  private:
    void normalize() {
        while (!limbs_.empty() && limbs_.back() == 0) {
            limbs_.pop_back();
        }
    }

    std::vector<std::uint32_t> limbs_;
};

struct SignedDyadic {
    bool negative = false;
    BigUnsigned significand;
    int exponent = 0;
};

std::int64_t signedIntegerValue(std::uint64_t bits,
                                RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
        return std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(bits));
    case RuntimeNumericClass::Int16:
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits));
    case RuntimeNumericClass::Int32:
        return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
    case RuntimeNumericClass::Int64:
        return std::bit_cast<std::int64_t>(bits);
    default:
        return 0;
    }
}

std::uint64_t signedMagnitude(std::int64_t value) {
    return value >= 0
               ? static_cast<std::uint64_t>(value)
               : static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

SignedDyadic integerDyadic(const RuntimeNumericElementValue& value) {
    SignedDyadic result;
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        const std::int64_t signedValue =
            signedIntegerValue(value.integerRealBits, value.numericClass);
        result.negative = signedValue < 0;
        result.significand = BigUnsigned(signedMagnitude(signedValue));
    } else {
        result.significand = BigUnsigned(value.integerRealBits);
    }
    return result;
}

SignedDyadic doubleDyadic(double value) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t fraction = bits & ((std::uint64_t{1} << 52U) - 1U);
    const unsigned biasedExponent =
        static_cast<unsigned>((bits >> 52U) & 0x7ffU);

    SignedDyadic result;
    result.negative = (bits >> 63U) != 0;
    if (biasedExponent == 0) {
        result.significand = BigUnsigned(fraction);
        result.exponent = -1074;
    } else {
        result.significand =
            BigUnsigned((std::uint64_t{1} << 52U) | fraction);
        result.exponent = static_cast<int>(biasedExponent) - 1023 - 52;
    }
    return result;
}

SignedDyadic addDyadic(SignedDyadic left, SignedDyadic right) {
    const int exponent = std::min(left.exponent, right.exponent);
    left.significand.shiftLeft(
        static_cast<size_t>(left.exponent - exponent));
    right.significand.shiftLeft(
        static_cast<size_t>(right.exponent - exponent));

    SignedDyadic result;
    result.exponent = exponent;
    if (left.negative == right.negative) {
        result.negative = left.negative;
        result.significand = std::move(left.significand);
        result.significand.add(right.significand);
    } else {
        const int comparison = left.significand.compare(right.significand);
        if (comparison >= 0) {
            result.negative = left.negative;
            result.significand = std::move(left.significand);
            result.significand.subtract(right.significand);
        } else {
            result.negative = right.negative;
            result.significand = std::move(right.significand);
            result.significand.subtract(left.significand);
        }
    }
    if (result.significand.empty()) {
        result.negative = false;
    }
    return result;
}

SignedDyadic multiplyDyadic(const SignedDyadic& left,
                            const SignedDyadic& right) {
    SignedDyadic result;
    result.negative = left.negative != right.negative;
    result.significand = BigUnsigned::multiply(
        left.significand, right.significand);
    result.exponent = left.exponent + right.exponent;
    if (result.significand.empty()) {
        result.negative = false;
    }
    return result;
}

SignedDyadic roundDyadicToExtended(SignedDyadic value) {
    constexpr size_t precision = 64;
    const size_t bits = value.significand.bitLength();
    if (bits <= precision) {
        return value;
    }

    const size_t discarded = bits - precision;
    const bool guard = value.significand.bit(discarded - 1U);
    const bool sticky = value.significand.anyBitBelow(discarded - 1U);
    const bool retainedOdd = value.significand.bit(discarded);
    value.significand.shiftRight(discarded);
    value.exponent += static_cast<int>(discarded);
    if (guard && (sticky || retainedOdd)) {
        value.significand.increment();
        if (value.significand.bitLength() > precision) {
            value.significand.shiftRight(1);
            ++value.exponent;
        }
    }
    return value;
}

SignedDyadic divideDyadicToExtended(const SignedDyadic& numerator,
                                    const SignedDyadic& denominator) {
    SignedDyadic result;
    result.negative = numerator.negative != denominator.negative;
    if (numerator.significand.empty()) {
        result.negative = false;
        return result;
    }

    const int numeratorBits =
        static_cast<int>(numerator.significand.bitLength());
    const int denominatorBits =
        static_cast<int>(denominator.significand.bitLength());
    const int bitDifference = numeratorBits - denominatorBits;

    bool belowCandidatePower = false;
    if (bitDifference >= 0) {
        BigUnsigned scaledDenominator = denominator.significand;
        scaledDenominator.shiftLeft(static_cast<size_t>(bitDifference));
        belowCandidatePower =
            numerator.significand.compare(scaledDenominator) < 0;
    } else {
        BigUnsigned scaledNumerator = numerator.significand;
        scaledNumerator.shiftLeft(static_cast<size_t>(-bitDifference));
        belowCandidatePower =
            scaledNumerator.compare(denominator.significand) < 0;
    }

    const int valueExponent =
        numerator.exponent - denominator.exponent + bitDifference -
        (belowCandidatePower ? 1 : 0);
    result.exponent = valueExponent - 63;

    BigUnsigned scaledNumerator = numerator.significand;
    BigUnsigned scaledDenominator = denominator.significand;
    const int scale = numerator.exponent - denominator.exponent -
                      result.exponent;
    if (scale >= 0) {
        scaledNumerator.shiftLeft(static_cast<size_t>(scale));
    } else {
        scaledDenominator.shiftLeft(static_cast<size_t>(-scale));
    }

    auto [quotient, remainder] = BigUnsigned::divide(
        scaledNumerator, scaledDenominator);
    remainder.shiftLeft(1);
    const int halfComparison = remainder.compare(scaledDenominator);
    if (halfComparison > 0 ||
        (halfComparison == 0 && quotient.bit(0))) {
        quotient.increment();
        if (quotient.bitLength() > 64U) {
            quotient.shiftRight(1);
            ++result.exponent;
        }
    }
    result.significand = std::move(quotient);
    return result;
}

unsigned integerWidth(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
    case RuntimeNumericClass::UInt8:
        return 8;
    case RuntimeNumericClass::Int16:
    case RuntimeNumericClass::UInt16:
        return 16;
    case RuntimeNumericClass::Int32:
    case RuntimeNumericClass::UInt32:
        return 32;
    case RuntimeNumericClass::Int64:
    case RuntimeNumericClass::UInt64:
        return 64;
    default:
        return 0;
    }
}

std::uint64_t positiveMaximum(RuntimeNumericClass numericClass) {
    const unsigned width = integerWidth(numericClass);
    if (!runtimeNumericClassIsSignedInteger(numericClass)) {
        return width == 64 ? std::numeric_limits<std::uint64_t>::max()
                           : (std::uint64_t{1} << width) - 1U;
    }
    return width == 64
               ? static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())
               : (std::uint64_t{1} << (width - 1U)) - 1U;
}

std::uint64_t negativeMaximumMagnitude(RuntimeNumericClass numericClass) {
    const unsigned width = integerWidth(numericClass);
    return std::uint64_t{1} << (width - 1U);
}

std::uint64_t encodeSignedMagnitude(std::uint64_t magnitude,
                                    RuntimeNumericClass numericClass) {
    if (!runtimeNumericClassIsSignedInteger(numericClass)) {
        return 0;
    }
    if (magnitude == (std::uint64_t{1} << 63U)) {
        return std::uint64_t{1} << 63U;
    }
    return std::bit_cast<std::uint64_t>(
        -static_cast<std::int64_t>(magnitude));
}

RuntimeNumericElementValue integerResult(std::uint64_t bits,
                                         RuntimeNumericClass numericClass) {
    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    result.integerRealBits = bits;
    result.real = runtimeNumericClassIsSignedInteger(numericClass)
                      ? static_cast<double>(
                            signedIntegerValue(bits, numericClass))
                      : static_cast<double>(bits);
    return result;
}

RuntimeNumericElementValue clampMagnitude(BigUnsigned magnitude, bool negative,
                                          RuntimeNumericClass numericClass) {
    if (!runtimeNumericClassIsSignedInteger(numericClass)) {
        if (negative) {
            return integerResult(0, numericClass);
        }
        const std::uint64_t maximum = positiveMaximum(numericClass);
        if (magnitude.compare(BigUnsigned(maximum)) >= 0) {
            return integerResult(maximum, numericClass);
        }
        return integerResult(magnitude.toUint64(), numericClass);
    }

    const std::uint64_t limit =
        negative ? negativeMaximumMagnitude(numericClass)
                 : positiveMaximum(numericClass);
    if (magnitude.compare(BigUnsigned(limit)) >= 0) {
        return integerResult(
            negative ? encodeSignedMagnitude(limit, numericClass) : limit,
            numericClass);
    }
    const std::uint64_t value = magnitude.toUint64();
    return integerResult(
        negative && value != 0
            ? encodeSignedMagnitude(value, numericClass)
            : value,
        numericClass);
}

RuntimeNumericElementValue roundDyadic(SignedDyadic value,
                                       RuntimeNumericClass numericClass) {
    if (value.exponent >= 0) {
        value.significand.shiftLeft(static_cast<size_t>(value.exponent));
    } else {
        const size_t shift = static_cast<size_t>(-value.exponent);
        const bool roundAway =
            shift != 0 && value.significand.bit(shift - 1U);
        value.significand.shiftRight(shift);
        if (roundAway) {
            value.significand.increment();
        }
    }
    return clampMagnitude(
        std::move(value.significand), value.negative, numericClass);
}

RuntimeNumericElementValue roundRatio(
    const SignedDyadic& numerator, const SignedDyadic& denominator,
    RuntimeNumericClass numericClass) {
    if (denominator.significand.empty()) {
        if (numerator.significand.empty()) {
            return integerResult(0, numericClass);
        }
        const bool negative = numerator.negative != denominator.negative;
        BigUnsigned beyondLimit(positiveMaximum(numericClass));
        beyondLimit.increment();
        return clampMagnitude(
            std::move(beyondLimit), negative, numericClass);
    }
    return roundDyadic(
        divideDyadicToExtended(numerator, denominator), numericClass);
}

RuntimeNumericElementValue dyadicRemainder(
    const SignedDyadic& left, const SignedDyadic& right,
    bool modulus, RuntimeNumericClass numericClass) {
    if (right.significand.empty()) {
        if (modulus) {
            return roundDyadic(left, numericClass);
        }
        return integerResult(0, numericClass);
    }

    const int exponent = std::min(left.exponent, right.exponent);
    BigUnsigned leftInteger = left.significand;
    BigUnsigned rightInteger = right.significand;
    leftInteger.shiftLeft(static_cast<size_t>(left.exponent - exponent));
    rightInteger.shiftLeft(static_cast<size_t>(right.exponent - exponent));
    auto [quotient, remainder] = BigUnsigned::divide(
        leftInteger, rightInteger);
    (void)quotient;

    bool negative = left.negative;
    if (modulus && !remainder.empty()) {
        negative = right.negative;
        if (left.negative != right.negative) {
            rightInteger.subtract(remainder);
            remainder = std::move(rightInteger);
        }
    }
    return roundDyadic(
        SignedDyadic{negative, std::move(remainder), exponent},
        numericClass);
}

RuntimeNumericElementValue roundedDouble(double value,
                                         RuntimeNumericClass numericClass) {
    if (std::isnan(value)) {
        return integerResult(0, numericClass);
    }
    if (std::isinf(value)) {
        BigUnsigned beyondLimit(positiveMaximum(numericClass));
        beyondLimit.increment();
        return clampMagnitude(
            std::move(beyondLimit), std::signbit(value), numericClass);
    }
    return roundDyadic(doubleDyadic(value), numericClass);
}

RuntimeNumericElementValue applyNonFinite(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass numericClass) {
    const double a = left.real;
    const double b = right.real;

    if ((operation == "mod" || operation == "rem") &&
        runtimeNumericClassIsInteger(left.numericClass) &&
        std::isfinite(a) && std::isinf(b)) {
        if (operation == "rem" || a == 0.0 ||
            std::signbit(a) == std::signbit(b)) {
            return integerResult(left.integerRealBits, numericClass);
        }
        return roundedDouble(
            std::copysign(std::numeric_limits<double>::infinity(), b),
            numericClass);
    }

    double value = std::numeric_limits<double>::quiet_NaN();
    if (operation == "+") {
        value = a + b;
    } else if (operation == "-") {
        value = a - b;
    } else if (operation == "*" || operation == ".*") {
        value = a * b;
    } else if (operation == "/" || operation == "./") {
        value = a / b;
    } else if (operation == "\\" || operation == ".\\") {
        value = b / a;
    } else if (operation == "^" || operation == ".^") {
        value = std::pow(a, b);
    } else if (operation == "mod") {
        value = b == 0.0 ? a : std::fmod(a, b);
        if (value != 0.0 && !std::isnan(value) &&
            std::signbit(value) != std::signbit(b)) {
            value += b;
        }
        if (value == 0.0) {
            value = std::copysign(0.0, b);
        }
    } else if (operation == "rem") {
        value = std::fmod(a, b);
    }
    return roundedDouble(value, numericClass);
}

std::uint64_t integerExponent(
    const RuntimeNumericElementValue& value, bool& negative) {
    negative = false;
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        const std::int64_t signedValue =
            signedIntegerValue(value.integerRealBits, value.numericClass);
        negative = signedValue < 0;
        return signedMagnitude(signedValue);
    }
    return value.integerRealBits;
}

RuntimeMixedIntegerOperationResult powerOperation(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass numericClass) {
    RuntimeMixedIntegerOperationResult result;
    result.handled = true;

    if (runtimeNumericClassIsInteger(left.numericClass)) {
        const double exponentValue = right.real;
        if (std::isnan(exponentValue) || exponentValue < 0.0 ||
            (std::isfinite(exponentValue) &&
             std::floor(exponentValue) != exponentValue)) {
            return result;
        }

        if (std::isinf(exponentValue)) {
            result.value = roundedDouble(
                std::pow(left.real, exponentValue), numericClass);
            result.succeeded = true;
            return result;
        }

        const SignedDyadic base = integerDyadic(left);
        if (exponentValue == 0.0) {
            result.succeeded = true;
            result.value = integerResult(1, numericClass);
            return result;
        }
        if (base.significand.empty()) {
            result.succeeded = true;
            result.value = integerResult(0, numericClass);
            return result;
        }

        constexpr double uint64UpperExclusive = 18446744073709551616.0;
        if (exponentValue >= uint64UpperExclusive) {
            if (base.significand.compare(BigUnsigned(1)) == 0) {
                result.value = integerResult(1, numericClass);
            } else {
                BigUnsigned beyondLimit(positiveMaximum(numericClass));
                beyondLimit.increment();
                result.value = clampMagnitude(
                    std::move(beyondLimit), false, numericClass);
            }
            result.succeeded = true;
            return result;
        }

        std::uint64_t exponent =
            static_cast<std::uint64_t>(exponentValue);
        BigUnsigned value(1);
        BigUnsigned factor = base.significand;
        while (exponent != 0) {
            if ((exponent & 1U) != 0U) {
                value = BigUnsigned::multiply(value, factor);
                if (value.bitLength() > 65U) {
                    break;
                }
            }
            exponent >>= 1U;
            if (exponent != 0) {
                factor = BigUnsigned::multiply(factor, factor);
                if (factor.bitLength() > 65U) {
                    factor = BigUnsigned(
                        std::numeric_limits<std::uint64_t>::max());
                    factor.increment();
                }
            }
        }
        const bool negative = base.negative &&
                              (static_cast<std::uint64_t>(exponentValue) &
                               1U) != 0U;
        result.value = clampMagnitude(
            std::move(value), negative, numericClass);
        result.succeeded = true;
        return result;
    }

    bool negativeExponent = false;
    const std::uint64_t exponent = integerExponent(right, negativeExponent);
    if (negativeExponent) {
        return result;
    }
    double value = 1.0;
    if (exponent > (std::uint64_t{1} << 53U)) {
        const double magnitude = std::fabs(left.real);
        if (magnitude == 0.0 || magnitude < 1.0) {
            value = 0.0;
        } else if (magnitude == 1.0) {
            value = left.real < 0.0 && (exponent & 1U) != 0U ? -1.0 : 1.0;
        } else {
            value = left.real < 0.0 && (exponent & 1U) != 0U
                        ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::infinity();
        }
    } else {
        value = std::pow(left.real, static_cast<double>(exponent));
    }
    result.value = roundedDouble(value, numericClass);
    result.succeeded = true;
    return result;
}

} // namespace

RuntimeMixedIntegerOperationResult runtimeApplyMixedIntegerDoubleOperation(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass resultClass) {
    RuntimeMixedIntegerOperationResult result;
    const bool leftInteger =
        runtimeNumericClassIsInteger(left.numericClass);
    const bool rightInteger =
        runtimeNumericClassIsInteger(right.numericClass);
    const bool leftDouble = left.numericClass == RuntimeNumericClass::Double;
    const bool rightDouble = right.numericClass == RuntimeNumericClass::Double;
    const bool exact64Result = resultClass == RuntimeNumericClass::Int64 ||
                               resultClass == RuntimeNumericClass::UInt64;
    if (!((leftInteger && rightDouble) ||
          (leftDouble && rightInteger)) ||
        !exact64Result) {
        return result;
    }

    result.handled = true;
    if (left.complex || right.complex) {
        return result;
    }
    if (operation == "^" || operation == ".^") {
        return powerOperation(left, right, resultClass);
    }
    if (!std::isfinite(left.real) || !std::isfinite(right.real)) {
        result.value = applyNonFinite(
            operation, left, right, resultClass);
        result.succeeded = true;
        return result;
    }

    SignedDyadic leftValue = leftInteger
                                 ? integerDyadic(left)
                                 : doubleDyadic(left.real);
    SignedDyadic rightValue = rightInteger
                                  ? integerDyadic(right)
                                  : doubleDyadic(right.real);
    if (operation == "+") {
        result.value = roundDyadic(
            roundDyadicToExtended(
                addDyadic(std::move(leftValue), std::move(rightValue))),
            resultClass);
    } else if (operation == "-") {
        rightValue.negative = !rightValue.negative;
        result.value = roundDyadic(
            roundDyadicToExtended(
                addDyadic(std::move(leftValue), std::move(rightValue))),
            resultClass);
    } else if (operation == "*" || operation == ".*") {
        result.value = roundDyadic(
            roundDyadicToExtended(
                multiplyDyadic(leftValue, rightValue)), resultClass);
    } else if (operation == "/" || operation == "./") {
        result.value = roundRatio(leftValue, rightValue, resultClass);
    } else if (operation == "\\" || operation == ".\\") {
        result.value = roundRatio(rightValue, leftValue, resultClass);
    } else if (operation == "mod" || operation == "rem") {
        result.value = dyadicRemainder(
            leftValue, rightValue, operation == "mod", resultClass);
    } else {
        return result;
    }
    result.succeeded = true;
    return result;
}

} // namespace mparser
