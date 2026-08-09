#include "mparser/runtime_numeric.h"

#include "mparser/runtime_shape.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace mparser {
namespace {

std::int64_t signedIntegerFromBits(std::uint64_t bits) {
    return std::bit_cast<std::int64_t>(bits);
}

std::uint64_t signedIntegerBits(std::int64_t value) {
    return std::bit_cast<std::uint64_t>(value);
}

std::optional<std::uint64_t> integerBitsFromDouble(
    double value, RuntimeNumericClass numericClass) {
    if (std::isnan(value)) {
        value = 0.0;
    }

    switch (numericClass) {
    case RuntimeNumericClass::Int8: {
        const double converted = std::clamp(
            std::round(value),
            static_cast<double>(std::numeric_limits<std::int8_t>::min()),
            static_cast<double>(std::numeric_limits<std::int8_t>::max()));
        return signedIntegerBits(static_cast<std::int8_t>(converted));
    }
    case RuntimeNumericClass::UInt8: {
        const double converted = std::clamp(
            std::round(value), 0.0,
            static_cast<double>(std::numeric_limits<std::uint8_t>::max()));
        return static_cast<std::uint8_t>(converted);
    }
    case RuntimeNumericClass::Int16: {
        const double converted = std::clamp(
            std::round(value),
            static_cast<double>(std::numeric_limits<std::int16_t>::min()),
            static_cast<double>(std::numeric_limits<std::int16_t>::max()));
        return signedIntegerBits(static_cast<std::int16_t>(converted));
    }
    case RuntimeNumericClass::UInt16: {
        const double converted = std::clamp(
            std::round(value), 0.0,
            static_cast<double>(std::numeric_limits<std::uint16_t>::max()));
        return static_cast<std::uint16_t>(converted);
    }
    case RuntimeNumericClass::Int32: {
        const double converted = std::clamp(
            std::round(value),
            static_cast<double>(std::numeric_limits<std::int32_t>::min()),
            static_cast<double>(std::numeric_limits<std::int32_t>::max()));
        return signedIntegerBits(static_cast<std::int32_t>(converted));
    }
    case RuntimeNumericClass::UInt32: {
        const double converted = std::clamp(
            std::round(value), 0.0,
            static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
        return static_cast<std::uint32_t>(converted);
    }
    case RuntimeNumericClass::Int64: {
        constexpr double minimum = -9223372036854775808.0;
        constexpr double upperExclusive = 9223372036854775808.0;
        if (value <= minimum) {
            return signedIntegerBits(std::numeric_limits<std::int64_t>::min());
        }
        if (value >= upperExclusive) {
            return signedIntegerBits(std::numeric_limits<std::int64_t>::max());
        }
        return signedIntegerBits(
            static_cast<std::int64_t>(std::round(value)));
    }
    case RuntimeNumericClass::UInt64: {
        constexpr double upperExclusive = 18446744073709551616.0;
        if (value <= 0.0) {
            return std::uint64_t{0};
        }
        if (value >= upperExclusive) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(std::round(value));
    }
    case RuntimeNumericClass::Double:
    case RuntimeNumericClass::Logical:
    case RuntimeNumericClass::Single:
        return std::nullopt;
    }
    return std::nullopt;
}

double integerBitsToDouble(std::uint64_t bits,
                           RuntimeNumericClass numericClass) {
    if (runtimeNumericClassIsSignedInteger(numericClass)) {
        return static_cast<double>(signedIntegerFromBits(bits));
    }
    return static_cast<double>(bits);
}

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

std::uint64_t integerMaximum(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::int8_t>::max());
    case RuntimeNumericClass::UInt8:
        return std::numeric_limits<std::uint8_t>::max();
    case RuntimeNumericClass::Int16:
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::int16_t>::max());
    case RuntimeNumericClass::UInt16:
        return std::numeric_limits<std::uint16_t>::max();
    case RuntimeNumericClass::Int32:
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max());
    case RuntimeNumericClass::UInt32:
        return std::numeric_limits<std::uint32_t>::max();
    case RuntimeNumericClass::Int64:
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
    case RuntimeNumericClass::UInt64:
        return std::numeric_limits<std::uint64_t>::max();
    default:
        return 0;
    }
}

std::optional<std::uint64_t> convertIntegerBits(
    std::uint64_t bits, RuntimeNumericClass sourceClass,
    RuntimeNumericClass targetClass) {
    const bool sourceSigned =
        runtimeNumericClassIsSignedInteger(sourceClass);
    const bool targetSigned =
        runtimeNumericClassIsSignedInteger(targetClass);
    const std::uint64_t targetMaximum = integerMaximum(targetClass);

    if (sourceSigned) {
        const std::int64_t source = signedIntegerFromBits(bits);
        if (targetSigned) {
            const std::int64_t targetMinimum = signedMinimum(targetClass);
            const std::int64_t targetMaximumSigned =
                static_cast<std::int64_t>(targetMaximum);
            return signedIntegerBits(std::clamp(
                source, targetMinimum, targetMaximumSigned));
        }
        if (source <= 0) {
            return std::uint64_t{0};
        }
        return std::min(static_cast<std::uint64_t>(source),
                        targetMaximum);
    }

    if (targetSigned) {
        const std::uint64_t clamped = std::min(bits, targetMaximum);
        return signedIntegerBits(static_cast<std::int64_t>(clamped));
    }
    return std::min(bits, targetMaximum);
}

std::optional<RuntimeNumericElementValue> convertNumericElement(
    const RuntimeNumericElementValue& source,
    RuntimeNumericClass targetClass) {
    RuntimeNumericElementValue result;
    result.numericClass = targetClass;

    if (runtimeNumericClassIsInteger(targetClass)) {
        if (source.complex &&
            !(runtimeNumericClassIsInteger(source.numericClass) &&
              source.numericClass == targetClass)) {
            return std::nullopt;
        }
        if (runtimeNumericClassIsInteger(source.numericClass)) {
            const auto converted = convertIntegerBits(
                source.integerRealBits, source.numericClass, targetClass);
            if (!converted) {
                return std::nullopt;
            }
            result.integerRealBits = *converted;
            if (source.complex) {
                const auto imaginary = convertIntegerBits(
                    source.integerImaginaryBits,
                    source.numericClass, targetClass);
                if (!imaginary) {
                    return std::nullopt;
                }
                result.integerImaginaryBits = *imaginary;
            }
        } else {
            const auto converted =
                integerBitsFromDouble(source.real, targetClass);
            if (!converted) {
                return std::nullopt;
            }
            result.integerRealBits = *converted;
        }
        result.real = integerBitsToDouble(
            result.integerRealBits, targetClass);
        result.complex = source.complex;
        if (result.complex) {
            result.imaginary = integerBitsToDouble(
                result.integerImaginaryBits, targetClass);
        }
        return result;
    }

    double real = source.real;
    double imaginary = source.imaginary;
    if (runtimeNumericClassIsInteger(source.numericClass)) {
        real = integerBitsToDouble(
            source.integerRealBits, source.numericClass);
        if (source.complex) {
            imaginary = integerBitsToDouble(
                source.integerImaginaryBits, source.numericClass);
        }
    }

    if (targetClass == RuntimeNumericClass::Logical) {
        if (std::isnan(real) || std::isnan(imaginary)) {
            return std::nullopt;
        }
        result.real = real == 0.0 && imaginary == 0.0 ? 0.0 : 1.0;
        return result;
    }
    if (targetClass == RuntimeNumericClass::Single) {
        result.real = static_cast<double>(static_cast<float>(real));
        result.imaginary =
            static_cast<double>(static_cast<float>(imaginary));
    } else if (targetClass == RuntimeNumericClass::Double) {
        result.real = real;
        result.imaginary = imaginary;
    } else {
        return std::nullopt;
    }
    result.complex = source.complex;
    return result;
}

} // namespace

bool isRuntimeNumericValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number ||
           value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isRuntimeLogical(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           value.numericClass == RuntimeNumericClass::Logical;
}

std::string_view runtimeNumericClassName(
    RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Double:
        return "double";
    case RuntimeNumericClass::Logical:
        return "logical";
    case RuntimeNumericClass::Single:
        return "single";
    case RuntimeNumericClass::Int8:
        return "int8";
    case RuntimeNumericClass::UInt8:
        return "uint8";
    case RuntimeNumericClass::Int16:
        return "int16";
    case RuntimeNumericClass::UInt16:
        return "uint16";
    case RuntimeNumericClass::Int32:
        return "int32";
    case RuntimeNumericClass::UInt32:
        return "uint32";
    case RuntimeNumericClass::Int64:
        return "int64";
    case RuntimeNumericClass::UInt64:
        return "uint64";
    }
    return "double";
}

std::optional<RuntimeNumericClass>
runtimeNumericClassFromName(std::string_view name) {
    if (name == "double") {
        return RuntimeNumericClass::Double;
    }
    if (name == "logical") {
        return RuntimeNumericClass::Logical;
    }
    if (name == "single") {
        return RuntimeNumericClass::Single;
    }
    if (name == "int8") {
        return RuntimeNumericClass::Int8;
    }
    if (name == "uint8") {
        return RuntimeNumericClass::UInt8;
    }
    if (name == "int16") {
        return RuntimeNumericClass::Int16;
    }
    if (name == "uint16") {
        return RuntimeNumericClass::UInt16;
    }
    if (name == "int32") {
        return RuntimeNumericClass::Int32;
    }
    if (name == "uint32") {
        return RuntimeNumericClass::UInt32;
    }
    if (name == "int64") {
        return RuntimeNumericClass::Int64;
    }
    if (name == "uint64") {
        return RuntimeNumericClass::UInt64;
    }
    return std::nullopt;
}

bool runtimeNumericClassIsFloating(RuntimeNumericClass numericClass) {
    return numericClass == RuntimeNumericClass::Double ||
           numericClass == RuntimeNumericClass::Single;
}

bool runtimeNumericClassIsInteger(RuntimeNumericClass numericClass) {
    switch (numericClass) {
    case RuntimeNumericClass::Int8:
    case RuntimeNumericClass::UInt8:
    case RuntimeNumericClass::Int16:
    case RuntimeNumericClass::UInt16:
    case RuntimeNumericClass::Int32:
    case RuntimeNumericClass::UInt32:
    case RuntimeNumericClass::Int64:
    case RuntimeNumericClass::UInt64:
        return true;
    case RuntimeNumericClass::Double:
    case RuntimeNumericClass::Logical:
    case RuntimeNumericClass::Single:
        return false;
    }
    return false;
}

bool runtimeNumericClassIsSignedInteger(
    RuntimeNumericClass numericClass) {
    return numericClass == RuntimeNumericClass::Int8 ||
           numericClass == RuntimeNumericClass::Int16 ||
           numericClass == RuntimeNumericClass::Int32 ||
           numericClass == RuntimeNumericClass::Int64;
}

bool runtimeNumericClassHasLegacyDoubleStorage(
    RuntimeNumericClass numericClass) {
    return numericClass != RuntimeNumericClass::Int64 &&
           numericClass != RuntimeNumericClass::UInt64;
}

std::optional<RuntimeValue> runtimeParseNumericLiteral(
    std::string_view text) {
    bool imaginary = false;
    if (!text.empty() &&
        (text.back() == 'i' || text.back() == 'j')) {
        imaginary = true;
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const std::string buffer(text);
    char* end = nullptr;
    const double parsed = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() ||
        end != buffer.c_str() + buffer.size()) {
        return std::nullopt;
    }

    RuntimeNumericElementValue element;
    if (imaginary) {
        element.imaginary = parsed;
        element.complex = true;
    } else {
        element.real = parsed;
    }
    return runtimeNumericValueFromElements(
        {1, 1}, {element}, RuntimeNumericClass::Double);
}

std::optional<double> runtimeCoerceNumericElement(
    double value, RuntimeNumericClass numericClass) {
    RuntimeNumericElementValue source;
    source.real = value;
    const auto converted = convertNumericElement(source, numericClass);
    return converted ? std::optional<double>(converted->real)
                     : std::nullopt;
}

std::optional<double> runtimeNumericElement(
    const RuntimeValue& value, size_t logicalIndex) {
    const auto element = runtimeNumericElementValue(value, logicalIndex);
    return element ? std::optional<double>(element->real)
                   : std::nullopt;
}

std::optional<RuntimeNumericElementValue> runtimeNumericElementValue(
    const RuntimeValue& value, size_t logicalIndex) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }
    if (value.kind == RuntimeValueKind::Number) {
        return logicalIndex == 0
                   ? runtimeNumericStorageElementValue(value, 0)
                   : std::nullopt;
    }
    const auto storageOffset =
        runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
    return storageOffset
               ? runtimeNumericStorageElementValue(value, *storageOffset)
               : std::nullopt;
}

std::optional<size_t> runtimeNumericElementAsNonnegativeSize(
    const RuntimeNumericElementValue& value) {
    if (value.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        std::uint64_t magnitude = value.integerRealBits;
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            const std::int64_t signedValue =
                signedIntegerFromBits(value.integerRealBits);
            if (signedValue < 0) {
                return std::nullopt;
            }
            magnitude = static_cast<std::uint64_t>(signedValue);
        }
        if (magnitude >
            static_cast<std::uint64_t>(
                std::numeric_limits<size_t>::max())) {
            return std::nullopt;
        }
        return static_cast<size_t>(magnitude);
    }
    if (!std::isfinite(value.real) ||
        std::floor(value.real) != value.real || value.real < 0.0 ||
        static_cast<long double>(value.real) >
            static_cast<long double>(
                std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<size_t>(value.real);
}

std::optional<RuntimeNumericElementValue> runtimeNumericStorageElementValue(
    const RuntimeValue& value, size_t storageOffset) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    double real = 0.0;
    if (value.kind == RuntimeValueKind::Number) {
        if (storageOffset != 0) {
            return std::nullopt;
        }
        real = value.number;
    } else if (storageOffset < value.elements.size()) {
        real = value.elements[storageOffset];
    } else {
        return std::nullopt;
    }

    RuntimeNumericElementValue result;
    result.numericClass = value.numericClass;
    result.real = real;
    result.complex = value.numericComplex;
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        if (storageOffset < value.exactIntegerElements.size()) {
            result.integerRealBits =
                value.exactIntegerElements[storageOffset];
            result.real = integerBitsToDouble(
                result.integerRealBits, value.numericClass);
        } else {
            const auto bits = integerBitsFromDouble(
                real, value.numericClass);
            if (!bits) {
                return std::nullopt;
            }
            result.integerRealBits = *bits;
        }
        if (result.complex) {
            if (storageOffset >=
                value.exactIntegerImaginaryElements.size()) {
                return std::nullopt;
            }
            result.integerImaginaryBits =
                value.exactIntegerImaginaryElements[storageOffset];
            result.imaginary = integerBitsToDouble(
                result.integerImaginaryBits, value.numericClass);
        }
    } else if (result.complex) {
        if (storageOffset >= value.imaginaryElements.size()) {
            return std::nullopt;
        }
        result.imaginary = value.imaginaryElements[storageOffset];
    }
    return result;
}

std::optional<RuntimeNumericElementValue> runtimeConvertNumericElementValue(
    const RuntimeNumericElementValue& value,
    RuntimeNumericClass numericClass) {
    return convertNumericElement(value, numericClass);
}

bool runtimeStoreNumericElementValue(
    RuntimeValue& target, size_t logicalIndex,
    const RuntimeNumericElementValue& value) {
    if (!isRuntimeNumericValue(target)) {
        return false;
    }
    const auto converted =
        convertNumericElement(value, target.numericClass);
    if (!converted) {
        return false;
    }

    const auto dimensions = runtimeDimensions(target);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || logicalIndex >= *count) {
        return false;
    }
    size_t storageOffset = 0;
    if (target.kind == RuntimeValueKind::Number) {
        if (logicalIndex != 0) {
            return false;
        }
    } else {
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            target, logicalIndex);
        if (!offset || *offset >= target.elements.size()) {
            return false;
        }
        storageOffset = *offset;
    }

    const bool integer =
        runtimeNumericClassIsInteger(target.numericClass);
    if (integer && target.exactIntegerElements.size() != *count) {
        target.exactIntegerElements.resize(*count);
        for (size_t offset = 0; offset < *count; ++offset) {
            const double real = target.kind == RuntimeValueKind::Number
                                    ? target.number
                                    : target.elements[offset];
            const auto bits = integerBitsFromDouble(
                real, target.numericClass);
            if (!bits) {
                return false;
            }
            target.exactIntegerElements[offset] = *bits;
        }
    }

    if (converted->complex && !target.numericComplex) {
        target.numericComplex = true;
        if (integer) {
            target.exactIntegerImaginaryElements.assign(*count, 0);
        } else {
            target.imaginaryElements.assign(*count, 0.0);
        }
    }

    if (target.kind == RuntimeValueKind::Number) {
        target.number = converted->real;
    } else {
        target.elements[storageOffset] = converted->real;
    }
    if (integer) {
        target.exactIntegerElements[storageOffset] =
            converted->integerRealBits;
        if (target.numericComplex) {
            if (target.exactIntegerImaginaryElements.size() != *count) {
                target.exactIntegerImaginaryElements.assign(*count, 0);
            }
            target.exactIntegerImaginaryElements[storageOffset] =
                converted->complex
                    ? converted->integerImaginaryBits
                    : 0;
        }
    } else if (target.numericComplex) {
        if (target.imaginaryElements.size() != *count) {
            target.imaginaryElements.assign(*count, 0.0);
        }
        target.imaginaryElements[storageOffset] =
            converted->complex ? converted->imaginary : 0.0;
    }
    return true;
}

std::optional<RuntimeValue> runtimeNumericValueFromLogicalOrder(
    std::vector<size_t> dimensions, std::vector<double> values,
    RuntimeNumericClass numericClass) {
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(values.size());
    for (double value : values) {
        RuntimeNumericElementValue element;
        element.real = value;
        elements.push_back(element);
    }
    return runtimeNumericValueFromElements(
        std::move(dimensions), std::move(elements), numericClass);
}

std::optional<RuntimeValue> runtimeNumericValueFromElements(
    std::vector<size_t> dimensions,
    std::vector<RuntimeNumericElementValue> values,
    RuntimeNumericClass numericClass) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != values.size()) {
        return std::nullopt;
    }

    bool complex = false;
    for (RuntimeNumericElementValue& value : values) {
        const auto converted = convertNumericElement(value, numericClass);
        if (!converted) {
            return std::nullopt;
        }
        value = *converted;
        complex = complex || value.complex;
    }

    RuntimeValue result;
    result.numericClass = numericClass;
    result.numericComplex = complex;
    if (*count == 1) {
        result.kind = RuntimeValueKind::Number;
    } else {
        result.kind = dimensions.size() == 2 && dimensions[0] == 1
                          ? RuntimeValueKind::Vector
                          : RuntimeValueKind::Matrix;
        result.elements.resize(*count);
    }
    if (runtimeNumericClassIsInteger(numericClass)) {
        result.exactIntegerElements.resize(*count);
        if (complex) {
            result.exactIntegerImaginaryElements.resize(*count);
        }
    } else if (complex) {
        result.imaginaryElements.resize(*count);
    }

    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= *count) {
            return std::nullopt;
        }
        const RuntimeNumericElementValue& value = values[logicalIndex];
        if (result.kind == RuntimeValueKind::Number) {
            result.number = value.real;
        } else {
            result.elements[*storageOffset] = value.real;
        }
        if (runtimeNumericClassIsInteger(numericClass)) {
            result.exactIntegerElements[*storageOffset] =
                value.integerRealBits;
            if (complex) {
                result.exactIntegerImaginaryElements[*storageOffset] =
                    value.integerImaginaryBits;
            }
        } else if (complex) {
            result.imaginaryElements[*storageOffset] = value.imaginary;
        }
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

std::optional<std::vector<RuntimeValue>>
runtimeNumericForLoopColumns(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    const auto dimensions = runtimeDimensions(value);
    const size_t rowCount = dimensions.front();
    const std::vector<size_t> trailingDimensions(
        dimensions.begin() + 1, dimensions.end());
    const auto columnCount =
        checkedRuntimeDimensionProduct(trailingDimensions);
    if (!columnCount) {
        return std::nullopt;
    }

    std::vector<RuntimeValue> columns;
    columns.reserve(*columnCount);
    for (size_t column = 0; column < *columnCount; ++column) {
        std::vector<RuntimeNumericElementValue> elements;
        elements.reserve(rowCount);
        for (size_t row = 0; row < rowCount; ++row) {
            const auto element = runtimeNumericElementValue(
                value, column * rowCount + row);
            if (!element) {
                return std::nullopt;
            }
            elements.push_back(*element);
        }
        auto columnValue = runtimeNumericValueFromElements(
            {rowCount, 1}, std::move(elements), value.numericClass);
        if (!columnValue) {
            return std::nullopt;
        }
        columns.push_back(std::move(*columnValue));
    }
    return columns;
}

std::optional<RuntimeValue> runtimeConvertNumericClass(
    RuntimeValue value, RuntimeNumericClass numericClass) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }

    if (value.numericClass == numericClass) {
        return value;
    }

    const auto dimensions = runtimeDimensions(value);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return std::nullopt;
    }
    if (value.numericComplex &&
        runtimeNumericClassIsInteger(numericClass)) {
        return std::nullopt;
    }

    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
            return std::nullopt;
        }
        elements.push_back(*element);
    }
    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(elements), numericClass);
    if (result && *count == 0 && value.numericComplex &&
        runtimeNumericClassIsFloating(numericClass)) {
        result->numericComplex = true;
    }
    return result;
}

bool runtimeNumericPredicate(std::string_view name,
                             const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return false;
    }
    if (name == "isnumeric") {
        return value.numericClass != RuntimeNumericClass::Logical;
    }
    if (name == "isfloat") {
        return runtimeNumericClassIsFloating(value.numericClass);
    }
    if (name == "isinteger") {
        return runtimeNumericClassIsInteger(value.numericClass);
    }
    if (name == "islogical") {
        return value.numericClass == RuntimeNumericClass::Logical;
    }
    return false;
}

std::optional<bool> runtimeNumericTruthValue(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) || value.numericComplex) {
        return std::nullopt;
    }
    const auto count = checkedRuntimeDimensionProduct(
        runtimeDimensions(value));
    if (!count) {
        return std::nullopt;
    }
    if (*count == 0) {
        return false;
    }
    for (size_t index = 0; index < *count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
            return std::nullopt;
        }
        if (runtimeNumericClassIsInteger(element->numericClass)) {
            if (element->integerRealBits == 0) {
                return false;
            }
        } else {
            if (std::isnan(element->real)) {
                return std::nullopt;
            }
            if (element->real == 0.0) {
                return false;
            }
        }
    }
    return true;
}

int runtimeCompareNumericElementsForExtrema(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right) {
    if (!left.complex && !right.complex) {
        if (runtimeNumericClassIsInteger(left.numericClass) &&
            left.numericClass == right.numericClass) {
            if (runtimeNumericClassIsSignedInteger(left.numericClass)) {
                const std::int64_t leftValue =
                    signedIntegerFromBits(left.integerRealBits);
                const std::int64_t rightValue =
                    signedIntegerFromBits(right.integerRealBits);
                return leftValue < rightValue
                           ? -1
                           : (leftValue > rightValue ? 1 : 0);
            }
            return left.integerRealBits < right.integerRealBits
                       ? -1
                       : (left.integerRealBits > right.integerRealBits ? 1
                                                                        : 0);
        }
        return left.real < right.real
                   ? -1
                   : (left.real > right.real ? 1 : 0);
    }

    const double leftMagnitude = std::hypot(left.real, left.imaginary);
    const double rightMagnitude = std::hypot(right.real, right.imaginary);
    if (leftMagnitude < rightMagnitude) {
        return -1;
    }
    if (leftMagnitude > rightMagnitude) {
        return 1;
    }
    const double leftPhase = std::atan2(left.imaginary, left.real);
    const double rightPhase = std::atan2(right.imaginary, right.real);
    return leftPhase < rightPhase
               ? -1
               : (leftPhase > rightPhase ? 1 : 0);
}

bool runtimeNumericValuesIdentical(
    const RuntimeValue& left, const RuntimeValue& right) {
    if (!isRuntimeNumericValue(left) ||
        !isRuntimeNumericValue(right) ||
        left.numericClass != right.numericClass ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }
    const auto count = checkedRuntimeDimensionProduct(
        runtimeDimensions(left));
    if (!count) {
        return false;
    }
    for (size_t index = 0; index < *count; ++index) {
        const auto leftElement = runtimeNumericElementValue(left, index);
        const auto rightElement = runtimeNumericElementValue(right, index);
        if (!leftElement || !rightElement) {
            return false;
        }
        if (runtimeNumericClassIsInteger(left.numericClass)) {
            if (leftElement->integerRealBits !=
                rightElement->integerRealBits) {
                return false;
            }
            const std::uint64_t leftImaginary =
                leftElement->complex
                    ? leftElement->integerImaginaryBits
                    : 0;
            const std::uint64_t rightImaginary =
                rightElement->complex
                    ? rightElement->integerImaginaryBits
                    : 0;
            if (leftImaginary != rightImaginary) {
                return false;
            }
            continue;
        }
        if (leftElement->real != rightElement->real) {
            return false;
        }
        const double leftImaginary =
            leftElement->complex ? leftElement->imaginary : 0.0;
        const double rightImaginary =
            rightElement->complex ? rightElement->imaginary : 0.0;
        if (leftImaginary != rightImaginary) {
            return false;
        }
    }
    return true;
}

namespace {

bool isRuntimeNumericArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isRelationalOperation(std::string_view operation) {
    return operation == ">" || operation == "<" ||
           operation == ">=" || operation == "<=" ||
           operation == "==" || operation == "~=";
}

bool isLogicalOperation(std::string_view operation) {
    return operation == "&" || operation == "|" ||
           operation == "&&" || operation == "||";
}

bool isArithmeticOperation(std::string_view operation) {
    return operation == "+" || operation == "-" ||
           operation == "*" || operation == ".*" ||
           operation == "/" || operation == "./" ||
           operation == "\\" || operation == ".\\" ||
           operation == "^" || operation == ".^" ||
           operation == "mod" || operation == "rem";
}

RuntimeNumericOperationResult numericOperationFailure(std::string error) {
    RuntimeNumericOperationResult result;
    result.error = std::move(error);
    return result;
}

RuntimeNumericOperationResult numericOperationSuccess(RuntimeValue value) {
    RuntimeNumericOperationResult result;
    result.succeeded = true;
    result.value = std::move(value);
    return result;
}

std::optional<RuntimeNumericClass> arithmeticResultClass(
    const RuntimeValue& left, const RuntimeValue& right,
    std::string& error) {
    const bool leftInteger =
        runtimeNumericClassIsInteger(left.numericClass);
    const bool rightInteger =
        runtimeNumericClassIsInteger(right.numericClass);
    if (leftInteger || rightInteger) {
        if (leftInteger && rightInteger &&
            left.numericClass == right.numericClass) {
            return left.numericClass;
        }

        if (leftInteger &&
            right.numericClass == RuntimeNumericClass::Double &&
            right.kind == RuntimeValueKind::Number) {
            if (left.numericClass == RuntimeNumericClass::Int64 ||
                left.numericClass == RuntimeNumericClass::UInt64) {
                error = "64-bit integer arithmetic with scalar double is not "
                        "implemented without precision loss";
                return std::nullopt;
            }
            return left.numericClass;
        }
        if (rightInteger &&
            left.numericClass == RuntimeNumericClass::Double &&
            left.kind == RuntimeValueKind::Number) {
            if (right.numericClass == RuntimeNumericClass::Int64 ||
                right.numericClass == RuntimeNumericClass::UInt64) {
                error = "64-bit integer arithmetic with scalar double is not "
                        "implemented without precision loss";
                return std::nullopt;
            }
            return right.numericClass;
        }

        error = "integer arithmetic requires the same integer class or a "
                "scalar double operand";
        return std::nullopt;
    }

    if (left.numericClass == RuntimeNumericClass::Single ||
        right.numericClass == RuntimeNumericClass::Single) {
        return RuntimeNumericClass::Single;
    }
    return RuntimeNumericClass::Double;
}

std::uint64_t signedMagnitude(std::int64_t value) {
    return value >= 0
               ? static_cast<std::uint64_t>(value)
               : static_cast<std::uint64_t>(-(value + 1)) + 1;
}

std::int64_t saturatingSignedAdd(std::int64_t left,
                                 std::int64_t right,
                                 RuntimeNumericClass numericClass) {
    const std::int64_t minimum = signedMinimum(numericClass);
    const std::int64_t maximum =
        static_cast<std::int64_t>(integerMaximum(numericClass));
    if (right > 0 && left > maximum - right) {
        return maximum;
    }
    if (right < 0 && left < minimum - right) {
        return minimum;
    }
    return left + right;
}

std::int64_t saturatingSignedSubtract(
    std::int64_t left, std::int64_t right,
    RuntimeNumericClass numericClass) {
    const std::int64_t minimum = signedMinimum(numericClass);
    const std::int64_t maximum =
        static_cast<std::int64_t>(integerMaximum(numericClass));
    if (right < 0 && left > maximum + right) {
        return maximum;
    }
    if (right > 0 && left < minimum + right) {
        return minimum;
    }
    return left - right;
}

std::int64_t saturatingSignedMultiply(
    std::int64_t left, std::int64_t right,
    RuntimeNumericClass numericClass) {
    const bool negative = (left < 0) != (right < 0);
    const std::uint64_t leftMagnitude = signedMagnitude(left);
    const std::uint64_t rightMagnitude = signedMagnitude(right);
    const std::int64_t minimum = signedMinimum(numericClass);
    const std::int64_t maximum =
        static_cast<std::int64_t>(integerMaximum(numericClass));
    const std::uint64_t limit =
        negative ? signedMagnitude(minimum)
                 : static_cast<std::uint64_t>(maximum);
    if (leftMagnitude != 0 && rightMagnitude > limit / leftMagnitude) {
        return negative ? minimum : maximum;
    }
    const std::uint64_t magnitude = leftMagnitude * rightMagnitude;
    if (!negative) {
        return static_cast<std::int64_t>(magnitude);
    }
    if (magnitude == signedMagnitude(minimum)) {
        return minimum;
    }
    return -static_cast<std::int64_t>(magnitude);
}

std::uint64_t saturatingUnsignedAdd(
    std::uint64_t left, std::uint64_t right,
    RuntimeNumericClass numericClass) {
    const std::uint64_t maximum = integerMaximum(numericClass);
    return right > maximum - left ? maximum : left + right;
}

std::uint64_t saturatingUnsignedMultiply(
    std::uint64_t left, std::uint64_t right,
    RuntimeNumericClass numericClass) {
    const std::uint64_t maximum = integerMaximum(numericClass);
    return left != 0 && right > maximum / left
               ? maximum
               : left * right;
}

bool roundIntegerDivisionAwayFromZero(
    std::uint64_t remainderMagnitude,
    std::uint64_t divisorMagnitude) {
    return remainderMagnitude != 0 &&
           remainderMagnitude >=
               divisorMagnitude - remainderMagnitude;
}

std::int64_t roundedSignedDivide(
    std::int64_t numerator, std::int64_t denominator,
    RuntimeNumericClass numericClass) {
    const std::int64_t minimum = signedMinimum(numericClass);
    const std::int64_t maximum =
        static_cast<std::int64_t>(integerMaximum(numericClass));
    if (denominator == 0) {
        if (numerator == 0) {
            return 0;
        }
        return numerator < 0 ? minimum : maximum;
    }
    if (numerator == minimum && denominator == -1) {
        return maximum;
    }

    std::int64_t quotient = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;
    if (roundIntegerDivisionAwayFromZero(
            signedMagnitude(remainder),
            signedMagnitude(denominator))) {
        const std::int64_t adjustment =
            (numerator < 0) != (denominator < 0) ? -1 : 1;
        quotient = saturatingSignedAdd(
            quotient, adjustment, numericClass);
    }
    return std::clamp(quotient, minimum, maximum);
}

std::uint64_t roundedUnsignedDivide(
    std::uint64_t numerator, std::uint64_t denominator,
    RuntimeNumericClass numericClass) {
    if (denominator == 0) {
        return numerator == 0 ? 0 : integerMaximum(numericClass);
    }
    std::uint64_t quotient = numerator / denominator;
    const std::uint64_t remainder = numerator % denominator;
    if (roundIntegerDivisionAwayFromZero(remainder, denominator)) {
        quotient = saturatingUnsignedAdd(
            quotient, 1, numericClass);
    }
    return quotient;
}

std::int64_t saturatingSignedPower(
    std::int64_t base, std::int64_t exponent,
    RuntimeNumericClass numericClass) {
    if (exponent < 0) {
        if (base == 0) {
            return integerMaximum(numericClass);
        }
        if (base == 1) {
            return 1;
        }
        if (base == -1) {
            return (signedMagnitude(exponent) & 1U) != 0U ? -1 : 1;
        }
        return 0;
    }

    std::uint64_t remaining = static_cast<std::uint64_t>(exponent);
    std::int64_t result = 1;
    std::int64_t factor = base;
    while (remaining != 0) {
        if ((remaining & 1U) != 0U) {
            result = saturatingSignedMultiply(
                result, factor, numericClass);
        }
        remaining >>= 1U;
        if (remaining != 0) {
            factor = saturatingSignedMultiply(
                factor, factor, numericClass);
        }
    }
    return result;
}

std::uint64_t saturatingUnsignedPower(
    std::uint64_t base, std::uint64_t exponent,
    RuntimeNumericClass numericClass) {
    std::uint64_t result = 1;
    std::uint64_t factor = base;
    while (exponent != 0) {
        if ((exponent & 1U) != 0U) {
            result = saturatingUnsignedMultiply(
                result, factor, numericClass);
        }
        exponent >>= 1U;
        if (exponent != 0) {
            factor = saturatingUnsignedMultiply(
                factor, factor, numericClass);
        }
    }
    return result;
}

std::optional<RuntimeNumericElementValue> applyExactIntegerOperation(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass numericClass) {
    if (!runtimeNumericClassIsInteger(left.numericClass) ||
        !runtimeNumericClassIsInteger(right.numericClass)) {
        return std::nullopt;
    }

    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    if (runtimeNumericClassIsSignedInteger(numericClass)) {
        std::int64_t leftValue = signedIntegerFromBits(
            left.integerRealBits);
        std::int64_t rightValue = signedIntegerFromBits(
            right.integerRealBits);
        std::int64_t value = 0;
        if (operation == "+") {
            value = saturatingSignedAdd(
                leftValue, rightValue, numericClass);
        } else if (operation == "-") {
            value = saturatingSignedSubtract(
                leftValue, rightValue, numericClass);
        } else if (operation == "*" || operation == ".*") {
            value = saturatingSignedMultiply(
                leftValue, rightValue, numericClass);
        } else if (operation == "/" || operation == "./") {
            value = roundedSignedDivide(
                leftValue, rightValue, numericClass);
        } else if (operation == "\\" || operation == ".\\") {
            value = roundedSignedDivide(
                rightValue, leftValue, numericClass);
        } else if (operation == "^" || operation == ".^") {
            value = saturatingSignedPower(
                leftValue, rightValue, numericClass);
        } else if (operation == "mod" || operation == "rem") {
            if (rightValue == 0) {
                value = operation == "mod" ? leftValue : 0;
            } else if (leftValue == signedMinimum(numericClass) &&
                       rightValue == -1) {
                value = 0;
            } else {
                value = leftValue % rightValue;
                if (operation == "mod" && value != 0 &&
                    ((value < 0) != (rightValue < 0))) {
                    value += rightValue;
                }
            }
        } else {
            return std::nullopt;
        }
        result.integerRealBits = signedIntegerBits(value);
    } else {
        const std::uint64_t leftValue = left.integerRealBits;
        const std::uint64_t rightValue = right.integerRealBits;
        std::uint64_t value = 0;
        if (operation == "+") {
            value = saturatingUnsignedAdd(
                leftValue, rightValue, numericClass);
        } else if (operation == "-") {
            value = leftValue < rightValue ? 0 : leftValue - rightValue;
        } else if (operation == "*" || operation == ".*") {
            value = saturatingUnsignedMultiply(
                leftValue, rightValue, numericClass);
        } else if (operation == "/" || operation == "./") {
            value = roundedUnsignedDivide(
                leftValue, rightValue, numericClass);
        } else if (operation == "\\" || operation == ".\\") {
            value = roundedUnsignedDivide(
                rightValue, leftValue, numericClass);
        } else if (operation == "^" || operation == ".^") {
            value = saturatingUnsignedPower(
                leftValue, rightValue, numericClass);
        } else if (operation == "mod" || operation == "rem") {
            value = rightValue == 0
                        ? (operation == "mod" ? leftValue : 0)
                        : leftValue % rightValue;
        } else {
            return std::nullopt;
        }
        result.integerRealBits = value;
    }
    result.real = integerBitsToDouble(
        result.integerRealBits, numericClass);
    return result;
}

std::optional<int> compareIntegerWithDouble(
    const RuntimeNumericElementValue& integer, double floating) {
    if (std::isnan(floating)) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsSignedInteger(integer.numericClass)) {
        const std::int64_t value = signedIntegerFromBits(
            integer.integerRealBits);
        constexpr double lower = -9223372036854775808.0;
        constexpr double upperExclusive = 9223372036854775808.0;
        if (floating < lower) {
            return 1;
        }
        if (floating >= upperExclusive) {
            return -1;
        }
        const std::int64_t integral =
            static_cast<std::int64_t>(floating);
        if (value < integral) {
            return -1;
        }
        if (value > integral) {
            return 1;
        }
        if (floating > static_cast<double>(integral)) {
            return -1;
        }
        if (floating < static_cast<double>(integral)) {
            return 1;
        }
        return 0;
    }

    const std::uint64_t value = integer.integerRealBits;
    constexpr double upperExclusive = 18446744073709551616.0;
    if (floating < 0.0) {
        return 1;
    }
    if (floating >= upperExclusive) {
        return -1;
    }
    const std::uint64_t integral =
        static_cast<std::uint64_t>(floating);
    if (value < integral) {
        return -1;
    }
    if (value > integral) {
        return 1;
    }
    if (floating > static_cast<double>(integral)) {
        return -1;
    }
    return 0;
}

std::optional<int> compareNumericElements(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right) {
    const bool leftInteger =
        runtimeNumericClassIsInteger(left.numericClass);
    const bool rightInteger =
        runtimeNumericClassIsInteger(right.numericClass);
    if (leftInteger && rightInteger) {
        const bool leftSigned =
            runtimeNumericClassIsSignedInteger(left.numericClass);
        const bool rightSigned =
            runtimeNumericClassIsSignedInteger(right.numericClass);
        if (leftSigned && rightSigned) {
            const std::int64_t leftValue = signedIntegerFromBits(
                left.integerRealBits);
            const std::int64_t rightValue = signedIntegerFromBits(
                right.integerRealBits);
            return leftValue < rightValue ? -1
                 : leftValue > rightValue ? 1
                                          : 0;
        }
        if (!leftSigned && !rightSigned) {
            return left.integerRealBits < right.integerRealBits ? -1
                 : left.integerRealBits > right.integerRealBits ? 1
                                                                : 0;
        }
        if (leftSigned) {
            const std::int64_t leftValue = signedIntegerFromBits(
                left.integerRealBits);
            if (leftValue < 0) {
                return -1;
            }
            const std::uint64_t converted =
                static_cast<std::uint64_t>(leftValue);
            return converted < right.integerRealBits ? -1
                 : converted > right.integerRealBits ? 1
                                                     : 0;
        }
        const std::int64_t rightValue = signedIntegerFromBits(
            right.integerRealBits);
        if (rightValue < 0) {
            return 1;
        }
        const std::uint64_t converted =
            static_cast<std::uint64_t>(rightValue);
        return left.integerRealBits < converted ? -1
             : left.integerRealBits > converted ? 1
                                                : 0;
    }
    if (leftInteger) {
        return compareIntegerWithDouble(left, right.real);
    }
    if (rightInteger) {
        const auto compared = compareIntegerWithDouble(right, left.real);
        return compared ? std::optional<int>(-*compared) : std::nullopt;
    }
    if (std::isnan(left.real) || std::isnan(right.real)) {
        return std::nullopt;
    }
    return left.real < right.real ? -1
         : left.real > right.real ? 1
                                  : 0;
}

RuntimeNumericElementValue imaginaryComponent(
    const RuntimeNumericElementValue& value) {
    RuntimeNumericElementValue result = value;
    result.real = value.complex ? value.imaginary : 0.0;
    result.integerRealBits =
        value.complex ? value.integerImaginaryBits : 0;
    result.imaginary = 0.0;
    result.integerImaginaryBits = 0;
    result.complex = false;
    return result;
}

bool numericElementsEqual(
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right) {
    const auto realComparison = compareNumericElements(left, right);
    if (!realComparison || *realComparison != 0) {
        return false;
    }
    const auto imaginaryComparison = compareNumericElements(
        imaginaryComponent(left), imaginaryComponent(right));
    return imaginaryComparison && *imaginaryComparison == 0;
}

std::optional<double> applyScalarOperation(std::string_view operation,
                                           double left, double right) {
    if (operation == "+") {
        return left + right;
    }
    if (operation == "-") {
        return left - right;
    }
    if (operation == "*" || operation == ".*") {
        return left * right;
    }
    if (operation == "/" || operation == "./") {
        return left / right;
    }
    if (operation == "\\" || operation == ".\\") {
        return right / left;
    }
    if (operation == "^" || operation == ".^") {
        return std::pow(left, right);
    }
    if (operation == "rem") {
        return std::fmod(left, right);
    }
    if (operation == "mod") {
        if (right == 0.0 && !std::isnan(left)) {
            return left;
        }
        double remainder = std::fmod(left, right);
        if (remainder != 0.0 && !std::isnan(remainder) &&
            std::signbit(remainder) != std::signbit(right)) {
            remainder += right;
        }
        if (remainder == 0.0) {
            remainder = std::copysign(0.0, right);
        }
        return remainder;
    }
    if (operation == ">") {
        return left > right ? 1.0 : 0.0;
    }
    if (operation == "<") {
        return left < right ? 1.0 : 0.0;
    }
    if (operation == ">=") {
        return left >= right ? 1.0 : 0.0;
    }
    if (operation == "<=") {
        return left <= right ? 1.0 : 0.0;
    }
    if (operation == "==") {
        return left == right ? 1.0 : 0.0;
    }
    if (operation == "~=") {
        return left != right ? 1.0 : 0.0;
    }
    if (operation == "&" || operation == "&&") {
        return left != 0.0 && right != 0.0 ? 1.0 : 0.0;
    }
    if (operation == "|" || operation == "||") {
        return left != 0.0 || right != 0.0 ? 1.0 : 0.0;
    }
    return std::nullopt;
}

std::optional<bool> numericElementTruthy(
    const RuntimeNumericElementValue& value) {
    if (value.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        return value.integerRealBits != 0;
    }
    if (std::isnan(value.real)) {
        return std::nullopt;
    }
    return value.real != 0.0;
}

std::optional<std::complex<double>> applyComplexScalarOperation(
    std::string_view operation, std::complex<double> left,
    std::complex<double> right) {
    if (operation == "+") {
        return left + right;
    }
    if (operation == "-") {
        return left - right;
    }
    if (operation == "*" || operation == ".*") {
        return left * right;
    }
    if (operation == "/" || operation == "./") {
        return left / right;
    }
    if (operation == "\\" || operation == ".\\") {
        return right / left;
    }
    if (operation == "^" || operation == ".^") {
        return std::pow(left, right);
    }
    return std::nullopt;
}

std::optional<RuntimeNumericElementValue> applyScalarElementOperation(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass resultClass) {
    if (isRelationalOperation(operation)) {
        bool value = false;
        if (operation == "==") {
            value = numericElementsEqual(left, right);
        } else if (operation == "~=") {
            value = !numericElementsEqual(left, right);
        } else {
            const auto comparison = compareNumericElements(left, right);
            if (comparison) {
                value = operation == ">"   ? *comparison > 0
                      : operation == "<"   ? *comparison < 0
                      : operation == ">="  ? *comparison >= 0
                                            : *comparison <= 0;
            }
        }
        RuntimeNumericElementValue result;
        result.numericClass = RuntimeNumericClass::Logical;
        result.real = value ? 1.0 : 0.0;
        return result;
    }
    if (isLogicalOperation(operation)) {
        const auto leftValue = numericElementTruthy(left);
        const auto rightValue = numericElementTruthy(right);
        if (!leftValue || !rightValue) {
            return std::nullopt;
        }
        RuntimeNumericElementValue result;
        result.numericClass = RuntimeNumericClass::Logical;
        result.real = operation == "&" || operation == "&&"
                          ? (*leftValue && *rightValue ? 1.0 : 0.0)
                          : (*leftValue || *rightValue ? 1.0 : 0.0);
        return result;
    }
    if (runtimeNumericClassIsInteger(resultClass) &&
        runtimeNumericClassIsInteger(left.numericClass) &&
        runtimeNumericClassIsInteger(right.numericClass)) {
        return applyExactIntegerOperation(
            operation, left, right, resultClass);
    }

    if (left.complex || right.complex) {
        if (runtimeNumericClassIsInteger(left.numericClass) ||
            runtimeNumericClassIsInteger(right.numericClass)) {
            return std::nullopt;
        }
        const auto raw = applyComplexScalarOperation(
            operation,
            {left.real, left.complex ? left.imaginary : 0.0},
            {right.real, right.complex ? right.imaginary : 0.0});
        if (!raw) {
            return std::nullopt;
        }
        RuntimeNumericElementValue result;
        result.real = raw->real();
        result.imaginary = raw->imag();
        result.complex = result.imaginary != 0.0;
        return convertNumericElement(result, resultClass);
    }

    if ((operation == "^" || operation == ".^") &&
        left.real < 0.0 && std::isfinite(right.real) &&
        std::floor(right.real) != right.real) {
        const auto raw = applyComplexScalarOperation(
            operation, {left.real, 0.0}, {right.real, 0.0});
        if (!raw) {
            return std::nullopt;
        }
        RuntimeNumericElementValue result;
        result.real = raw->real();
        result.imaginary = raw->imag();
        result.complex = result.imaginary != 0.0;
        return convertNumericElement(result, resultClass);
    }

    const auto raw = applyScalarOperation(
        operation, left.real, right.real);
    if (!raw) {
        return std::nullopt;
    }
    RuntimeNumericElementValue result;
    result.real = *raw;
    return convertNumericElement(result, resultClass);
}

std::optional<RuntimeNumericElementValue> numericElementAtCoordinates(
    const RuntimeValue& value, const std::vector<size_t>& coordinates) {
    if (value.kind == RuntimeValueKind::Number) {
        return runtimeNumericStorageElementValue(value, 0);
    }
    const auto offset = runtimeImplicitExpansionStorageOffset(
        coordinates, runtimeDimensions(value));
    return offset
               ? runtimeNumericStorageElementValue(value, *offset)
               : std::nullopt;
}

RuntimeNumericOperationResult applyMatrixMultiply(
    const RuntimeValue& left, const RuntimeValue& right,
    RuntimeNumericClass resultClass) {
    if (runtimeDimensionCount(left) > 2 ||
        runtimeDimensionCount(right) > 2) {
        return numericOperationFailure(
            "matrix multiplication requires two-dimensional arrays");
    }

    const auto leftDimensions = runtimeDimensions(left);
    const auto rightDimensions = runtimeDimensions(right);
    const size_t leftRows = leftDimensions[0];
    const size_t leftColumns = leftDimensions[1];
    const size_t rightRows = rightDimensions[0];
    const size_t rightColumns = rightDimensions[1];
    if (leftColumns != rightRows) {
        return numericOperationFailure(
            "matrix dimensions do not agree for *");
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(leftRows * rightColumns);
    for (size_t column = 0; column < rightColumns; ++column) {
        for (size_t row = 0; row < leftRows; ++row) {
            RuntimeNumericElementValue total;
            total.numericClass = resultClass;
            for (size_t inner = 0; inner < leftColumns; ++inner) {
                const auto leftValue = runtimeNumericElementValue(
                    left, row + inner * leftRows);
                const auto rightValue = runtimeNumericElementValue(
                    right, inner + column * rightRows);
                if (!leftValue || !rightValue) {
                    return numericOperationFailure(
                        "matrix multiplication could not read an operand");
                }

                const auto product = applyScalarElementOperation(
                    "*", *leftValue, *rightValue, resultClass);
                if (!product) {
                    return numericOperationFailure(
                        "matrix multiplication result is not representable");
                }
                const auto accumulated = applyScalarElementOperation(
                    "+", total, *product, resultClass);
                if (!accumulated) {
                    return numericOperationFailure(
                        "matrix multiplication result is not representable");
                }
                total = *accumulated;
            }
            values.push_back(total);
        }
    }

    auto result = runtimeNumericValueFromElements(
        {leftRows, rightColumns}, std::move(values), resultClass);
    if (!result) {
        return numericOperationFailure(
            "matrix multiplication could not construct its result");
    }
    return numericOperationSuccess(std::move(*result));
}

} // namespace

std::optional<RuntimeNumericElementValue> runtimeApplyNumericElementBinary(
    std::string_view operation,
    const RuntimeNumericElementValue& left,
    const RuntimeNumericElementValue& right,
    RuntimeNumericClass resultClass) {
    return applyScalarElementOperation(
        operation, left, right, resultClass);
}

bool runtimeNumericValuesEqual(
    const RuntimeValue& left, const RuntimeValue& right,
    bool equalNaNs) {
    if (!isRuntimeNumericValue(left) ||
        !isRuntimeNumericValue(right) ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }

    const auto count = checkedRuntimeDimensionProduct(
        runtimeDimensions(left));
    if (!count) {
        return false;
    }
    const auto componentsEqual = [equalNaNs](
                                     const RuntimeNumericElementValue& lhs,
                                     const RuntimeNumericElementValue& rhs) {
        const auto comparison = compareNumericElements(lhs, rhs);
        if (comparison && *comparison == 0) {
            return true;
        }
        return equalNaNs && !comparison &&
               !runtimeNumericClassIsInteger(lhs.numericClass) &&
               !runtimeNumericClassIsInteger(rhs.numericClass) &&
               std::isnan(lhs.real) && std::isnan(rhs.real);
    };

    for (size_t index = 0; index < *count; ++index) {
        const auto leftElement = runtimeNumericElementValue(left, index);
        const auto rightElement = runtimeNumericElementValue(right, index);
        if (!leftElement || !rightElement ||
            !componentsEqual(*leftElement, *rightElement) ||
            !componentsEqual(imaginaryComponent(*leftElement),
                             imaginaryComponent(*rightElement))) {
            return false;
        }
    }
    return true;
}

RuntimeNumericOperationResult runtimeApplyNumericUnary(
    std::string_view operation, const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return numericOperationFailure(
            "unary operator requires numeric input");
    }
    if (operation != "+" && operation != "-" && operation != "~") {
        return numericOperationFailure(
            "unsupported unary operator: " + std::string(operation));
    }
    if (value.numericComplex && operation == "~") {
        return numericOperationFailure(
            "logical operators require real numeric operands");
    }
    if (value.numericComplex &&
        runtimeNumericClassIsInteger(value.numericClass) &&
        (operation == "+" || operation == "-")) {
        return numericOperationFailure(
            "arithmetic is not supported for complex integer values");
    }

    RuntimeNumericClass resultClass = value.numericClass;
    if (operation == "~") {
        resultClass = RuntimeNumericClass::Logical;
    } else if (value.numericClass == RuntimeNumericClass::Logical) {
        resultClass = RuntimeNumericClass::Double;
    }

    const auto dimensions = runtimeDimensions(value);
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return numericOperationFailure(
            "unary operand dimensions overflow the runtime limit");
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
            return numericOperationFailure(
                "unary operator could not read its operand");
        }
        std::optional<RuntimeNumericElementValue> converted;
        if (operation == "~") {
            RuntimeNumericElementValue logical;
            logical.numericClass = RuntimeNumericClass::Logical;
            const auto elementValue = numericElementTruthy(*element);
            if (!elementValue) {
                return numericOperationFailure(
                    "logical conversion requires real, non-NaN values");
            }
            logical.real = *elementValue ? 0.0 : 1.0;
            converted = logical;
        } else if (operation == "-" &&
                   runtimeNumericClassIsInteger(resultClass)) {
            RuntimeNumericElementValue zero;
            zero.numericClass = resultClass;
            converted = applyExactIntegerOperation(
                "-", zero, *element, resultClass);
        } else {
            RuntimeNumericElementValue mapped = *element;
            if (operation == "-") {
                mapped.real = -mapped.real;
                mapped.imaginary = -mapped.imaginary;
            }
            converted = convertNumericElement(mapped, resultClass);
        }
        if (!converted) {
            return numericOperationFailure(
                "unary result is not representable in class " +
                std::string(runtimeNumericClassName(resultClass)));
        }
        values.push_back(*converted);
    }

    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(values), resultClass);
    if (!result) {
        return numericOperationFailure(
            "unary operator could not construct its result");
    }
    return numericOperationSuccess(std::move(*result));
}

RuntimeNumericOperationResult runtimeApplyNumericBinary(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    if (!isRuntimeNumericValue(left) || !isRuntimeNumericValue(right)) {
        return numericOperationFailure(
            "binary operator requires numeric values");
    }
    if (!isArithmeticOperation(operation) &&
        !isRelationalOperation(operation) &&
        !isLogicalOperation(operation)) {
        return numericOperationFailure(
            "unsupported binary operator: " + std::string(operation));
    }
    if (isLogicalOperation(operation) &&
        (left.numericComplex || right.numericComplex)) {
        return numericOperationFailure(
            "logical operators require real numeric operands");
    }
    if ((operation == "mod" || operation == "rem") &&
        (left.numericComplex || right.numericComplex)) {
        return numericOperationFailure(
            std::string(operation) +
            " requires real numeric operands");
    }
    if (isArithmeticOperation(operation) &&
        ((left.numericComplex &&
          runtimeNumericClassIsInteger(left.numericClass)) ||
         (right.numericComplex &&
          runtimeNumericClassIsInteger(right.numericClass)))) {
        return numericOperationFailure(
            "arithmetic is not supported for complex integer values");
    }

    RuntimeNumericClass resultClass = RuntimeNumericClass::Logical;
    if (isArithmeticOperation(operation)) {
        std::string classError;
        const auto selected = arithmeticResultClass(left, right, classError);
        if (!selected) {
            return numericOperationFailure(std::move(classError));
        }
        resultClass = *selected;
    }

    const bool leftArray = isRuntimeNumericArray(left);
    const bool rightArray = isRuntimeNumericArray(right);
    if (operation == "*" && leftArray && rightArray) {
        return applyMatrixMultiply(left, right, resultClass);
    }
    if ((operation == "/" || operation == "\\") &&
        leftArray && rightArray) {
        return numericOperationFailure(
            "matrix division is not implemented for array operands yet");
    }
    if (operation == "^" && (leftArray || rightArray)) {
        return numericOperationFailure(
            "matrix power is not implemented for array operands yet");
    }

    std::vector<size_t> dimensions{1, 1};
    if (leftArray && rightArray) {
        const auto expanded = runtimeImplicitExpansionDimensions(
            runtimeDimensions(left), runtimeDimensions(right));
        if (!expanded) {
            return numericOperationFailure(
                "elementwise operands have incompatible dimensions");
        }
        dimensions = *expanded;
    } else if (leftArray) {
        dimensions = runtimeDimensions(left);
    } else if (rightArray) {
        dimensions = runtimeDimensions(right);
    }

    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return numericOperationFailure(
            "binary result dimensions overflow the runtime limit");
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto coordinates =
            runtimeColumnMajorCoordinates(index, dimensions);
        if (!coordinates) {
            return numericOperationFailure(
                "binary result coordinates overflow the runtime limit");
        }
        const auto leftValue =
            numericElementAtCoordinates(left, *coordinates);
        const auto rightValue =
            numericElementAtCoordinates(right, *coordinates);
        if (!leftValue || !rightValue) {
            return numericOperationFailure(
                "elementwise expansion could not map an operand");
        }

        const auto converted = applyScalarElementOperation(
            operation, *leftValue, *rightValue, resultClass);
        if (!converted) {
            return numericOperationFailure(
                "binary result is not representable for operator " +
                std::string(operation) + " in class " +
                std::string(runtimeNumericClassName(resultClass)));
        }
        values.push_back(*converted);
    }

    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(values), resultClass);
    if (!result) {
        return numericOperationFailure(
            "binary operator could not construct its result");
    }
    return numericOperationSuccess(std::move(*result));
}

RuntimeNumericOperationResult runtimeTransposeNumeric(
    const RuntimeValue& value, bool conjugate) {
    if (!isRuntimeNumericValue(value)) {
        return numericOperationFailure(
            "transpose requires numeric input");
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
        return numericOperationFailure(
            "transpose requires a two-dimensional numeric array");
    }
    if (conjugate && value.numericComplex &&
        runtimeNumericClassIsInteger(value.numericClass)) {
        return numericOperationFailure(
            "conjugate transpose is not supported for complex integer values");
    }

    const size_t rows = dimensions[0];
    const size_t columns = dimensions[1];
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(rows * columns);
    for (size_t outputColumn = 0; outputColumn < rows;
         ++outputColumn) {
        for (size_t outputRow = 0; outputRow < columns;
             ++outputRow) {
            const auto element = runtimeNumericElementValue(
                value, outputColumn + outputRow * rows);
            if (!element) {
                return numericOperationFailure(
                    "transpose could not read a numeric element");
            }
            RuntimeNumericElementValue mapped = *element;
            if (conjugate && mapped.complex) {
                mapped.imaginary = -mapped.imaginary;
            }
            elements.push_back(mapped);
        }
    }

    auto result = runtimeNumericValueFromElements(
        {columns, rows}, std::move(elements), value.numericClass);
    if (!result) {
        return numericOperationFailure(
            "transpose could not construct its result");
    }
    return numericOperationSuccess(std::move(*result));
}

bool isRuntimeComplexNumericBuiltin(std::string_view name) {
    return name == "complex" || name == "conj" ||
           name == "imag" || name == "isreal" ||
           name == "real";
}

RuntimeNumericOperationResult runtimeApplyComplexNumericBuiltin(
    std::string_view name,
    const std::vector<RuntimeValue>& arguments) {
    if (!isRuntimeComplexNumericBuiltin(name)) {
        return numericOperationFailure(
            "unsupported complex numeric builtin: " +
            std::string(name));
    }
    const size_t maximumArguments = name == "complex" ? 2 : 1;
    if (arguments.empty() || arguments.size() > maximumArguments) {
        return numericOperationFailure(
            std::string(name) + " expects " +
            (name == "complex" ? "one or two" : "one") +
            " numeric arguments");
    }
    if (std::any_of(arguments.begin(), arguments.end(),
                    [](const RuntimeValue& argument) {
                        return !isRuntimeNumericValue(argument);
                    })) {
        return numericOperationFailure(
            std::string(name) + " requires numeric arguments");
    }

    const RuntimeValue& input = arguments.front();
    if (name == "isreal") {
        return numericOperationSuccess(
            makeRuntimeLogicalValue(!input.numericComplex));
    }

    if (name == "real" || name == "imag" || name == "conj") {
        if (name == "conj" && input.numericComplex &&
            runtimeNumericClassIsInteger(input.numericClass)) {
            return numericOperationFailure(
                "conj is not supported for complex integer values");
        }
        const auto dimensions = runtimeDimensions(input);
        const auto count = checkedRuntimeDimensionProduct(dimensions);
        if (!count) {
            return numericOperationFailure(
                std::string(name) + " input dimensions overflow");
        }
        std::vector<RuntimeNumericElementValue> elements;
        elements.reserve(*count);
        for (size_t index = 0; index < *count; ++index) {
            const auto source = runtimeNumericElementValue(input, index);
            if (!source) {
                return numericOperationFailure(
                    std::string(name) +
                    " could not read a numeric element");
            }
            RuntimeNumericElementValue result = *source;
            if (name == "real") {
                result.imaginary = 0.0;
                result.integerImaginaryBits = 0;
                result.complex = false;
            } else if (name == "imag") {
                result.real = source->complex
                                  ? source->imaginary
                                  : 0.0;
                result.integerRealBits =
                    source->complex
                        ? source->integerImaginaryBits
                        : 0;
                result.imaginary = 0.0;
                result.integerImaginaryBits = 0;
                result.complex = false;
            } else if (result.complex) {
                result.imaginary = -result.imaginary;
            }
            elements.push_back(result);
        }
        auto result = runtimeNumericValueFromElements(
            dimensions, std::move(elements), input.numericClass);
        return result
                   ? numericOperationSuccess(std::move(*result))
                   : numericOperationFailure(
                         std::string(name) +
                         " could not construct its result");
    }

    RuntimeNumericClass resultClass = input.numericClass;
    if (resultClass == RuntimeNumericClass::Logical) {
        resultClass = RuntimeNumericClass::Double;
    }
    std::vector<size_t> dimensions = runtimeDimensions(input);
    if (arguments.size() == 2) {
        const RuntimeValue& imaginary = arguments[1];
        if (input.numericComplex || imaginary.numericComplex) {
            return numericOperationFailure(
                "complex real and imaginary inputs must be real numeric values");
        }

        if (input.numericClass == imaginary.numericClass) {
            resultClass = input.numericClass == RuntimeNumericClass::Logical
                              ? RuntimeNumericClass::Double
                              : input.numericClass;
        } else if (input.numericClass == RuntimeNumericClass::Double) {
            resultClass = imaginary.numericClass ==
                                  RuntimeNumericClass::Logical
                              ? RuntimeNumericClass::Double
                              : imaginary.numericClass;
        } else if (imaginary.numericClass ==
                   RuntimeNumericClass::Double) {
            resultClass = input.numericClass ==
                                  RuntimeNumericClass::Logical
                              ? RuntimeNumericClass::Double
                              : input.numericClass;
        } else {
            return numericOperationFailure(
                "complex inputs must have the same class or one double input");
        }

        const bool inputArray = isRuntimeNumericArray(input);
        const bool imaginaryArray = isRuntimeNumericArray(imaginary);
        if (inputArray && imaginaryArray) {
            const auto expanded = runtimeImplicitExpansionDimensions(
                runtimeDimensions(input), runtimeDimensions(imaginary));
            if (!expanded) {
                return numericOperationFailure(
                    "complex inputs have incompatible dimensions");
            }
            dimensions = *expanded;
        } else if (imaginaryArray) {
            dimensions = runtimeDimensions(imaginary);
        }
    }

    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return numericOperationFailure(
            "complex result dimensions overflow");
    }
    std::vector<RuntimeNumericElementValue> elements;
    elements.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        if (!coordinates) {
            return numericOperationFailure(
                "complex result coordinates overflow");
        }
        const auto realSource = numericElementAtCoordinates(
            input, *coordinates);
        if (!realSource) {
            return numericOperationFailure(
                "complex could not map its real input");
        }
        const auto real = convertNumericElement(
            *realSource, resultClass);
        if (!real) {
            return numericOperationFailure(
                "complex real input is not representable in the result class");
        }

        RuntimeNumericElementValue result = *real;
        if (arguments.size() == 1) {
            result.complex = true;
            elements.push_back(result);
            continue;
        }

        const auto imaginarySource = numericElementAtCoordinates(
            arguments[1], *coordinates);
        const auto imaginary = imaginarySource
                                   ? convertNumericElement(
                                         *imaginarySource, resultClass)
                                   : std::nullopt;
        if (!imaginary) {
            return numericOperationFailure(
                "complex imaginary input is not representable in the result class");
        }
        result.imaginary = imaginary->real;
        result.integerImaginaryBits =
            imaginary->integerRealBits;
        result.complex = true;
        elements.push_back(result);
    }

    auto result = runtimeNumericValueFromElements(
        dimensions, std::move(elements), resultClass);
    return result
               ? numericOperationSuccess(std::move(*result))
               : numericOperationFailure(
                     "complex could not construct its result");
}

} // namespace mparser
