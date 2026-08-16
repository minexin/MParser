#include "mparser/runtime_numeric_library_builtins.h"

#include "mparser/runtime_execution_control.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

BuiltinResult failure(const BuiltinCall& call, std::string message,
                      std::string identifier) {
    return BuiltinResult::failure(call.span, std::move(message),
                                  std::move(identifier));
}

BuiltinResult exactOutputs(const BuiltinCall& call,
                           std::vector<RuntimeValue> outputs) {
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }
    if (outputs.size() != call.requestedOutputCount) {
        return failure(call,
                       "numeric library builtin produced an unexpected "
                       "output count",
                       "MParser:NumericLibraryContractViolation");
    }
    return BuiltinResult::success(std::move(outputs));
}

bool executionCheckpoint(const BuiltinCall& call) {
    return !call.context || !call.context->executionControl ||
           call.context->executionControl->checkpoint();
}

bool observeNumericOutput(const BuiltinCall& call, size_t count,
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
    if (count > std::numeric_limits<size_t>::max() /
                    bytesPerElement) {
        return false;
    }
    if (!call.context || !call.context->executionControl) {
        return true;
    }
    return call.context->executionControl->observeArrayBytes(
        count * bytesPerElement);
}

bool periodicExecutionCheckpoint(const BuiltinCall& call, size_t index,
                                 size_t interval = 16384U) {
    return index % interval != 0 || executionCheckpoint(call);
}

BuiltinResult executionStopped(const BuiltinCall& call,
                               std::string_view name) {
    return failure(call, std::string(name) +
                             " execution was stopped by runtime control",
                   "MParser:ExecutionStopped");
}

std::optional<RuntimeNumericElementValue> numericScalar(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1 ||
        value.numericClass == RuntimeNumericClass::Logical) {
        return std::nullopt;
    }
    return runtimeNumericElementValue(value, 0);
}

std::uint64_t signedMagnitude(std::int64_t value) {
    return value < 0
               ? static_cast<std::uint64_t>(-(value + 1)) + 1U
               : static_cast<std::uint64_t>(value);
}

std::optional<std::uint64_t> exactMagnitude(
    const RuntimeNumericElementValue& value, bool allowNegative) {
    if (value.complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            const std::int64_t signedValue =
                std::bit_cast<std::int64_t>(value.integerRealBits);
            if (!allowNegative && signedValue < 0) {
                return std::nullopt;
            }
            return signedMagnitude(signedValue);
        }
        return value.integerRealBits;
    }
    if (!runtimeNumericClassIsFloating(value.numericClass) ||
        !std::isfinite(value.real) || std::trunc(value.real) != value.real ||
        (!allowNegative && value.real < 0.0) ||
        std::fabs(value.real) > 9007199254740992.0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::fabs(value.real));
}

std::uint64_t integerClassMaximum(RuntimeNumericClass numericClass) {
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

RuntimeNumericElementValue exactRealElement(
    std::uint64_t value, RuntimeNumericClass numericClass) {
    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    result.real = static_cast<double>(value);
    result.integerRealBits = value;
    return result;
}

RuntimeNumericElementValue floatingRealElement(
    double value, RuntimeNumericClass numericClass) {
    RuntimeNumericElementValue result;
    result.numericClass = numericClass;
    result.real = numericClass == RuntimeNumericClass::Single
                      ? static_cast<double>(static_cast<float>(value))
                      : value;
    return result;
}

BuiltinResult factorialBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input) ||
        input.numericClass == RuntimeNumericClass::Logical) {
        return failure(call, "factorial expects a non-logical numeric array",
                       "MParser:InvalidFactorialInput");
    }

    const size_t count = runtimeShapeElementCount(input);
    if (!executionCheckpoint(call) ||
        !observeNumericOutput(call, count, input.numericClass)) {
        return executionStopped(call, "factorial");
    }
    std::vector<RuntimeNumericElementValue> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (!periodicExecutionCheckpoint(call, index)) {
            return executionStopped(call, "factorial");
        }
        const auto element = runtimeNumericElementValue(input, index);
        if (!element || element->complex || std::isnan(element->real)) {
            return failure(call,
                           "factorial inputs must be nonnegative real "
                           "integers or positive infinity",
                           "MParser:InvalidFactorialInput");
        }
        if (runtimeNumericClassIsInteger(input.numericClass)) {
            const auto magnitude = exactMagnitude(*element, false);
            if (!magnitude) {
                return failure(call,
                               "factorial integer inputs must be "
                               "nonnegative",
                               "MParser:InvalidFactorialInput");
            }
            const std::uint64_t maximum =
                integerClassMaximum(input.numericClass);
            std::uint64_t product = 1;
            for (std::uint64_t factor = 2; factor <= *magnitude;
                 ++factor) {
                if (product > maximum / factor) {
                    product = maximum;
                    break;
                }
                product *= factor;
            }
            values.push_back(exactRealElement(product,
                                              input.numericClass));
            continue;
        }
        if (std::isinf(element->real) && element->real > 0.0) {
            values.push_back(floatingRealElement(
                std::numeric_limits<double>::infinity(),
                input.numericClass));
            continue;
        }
        if (!std::isfinite(element->real) || element->real < 0.0 ||
            std::trunc(element->real) != element->real) {
            return failure(call,
                           "factorial inputs must be nonnegative real "
                           "integers or positive infinity",
                           "MParser:InvalidFactorialInput");
        }
        const double result = element->real > 170.0
                                  ? std::numeric_limits<double>::infinity()
                                  : std::tgamma(element->real + 1.0);
        values.push_back(floatingRealElement(result,
                                             input.numericClass));
    }
    auto result = runtimeNumericValueFromElements(
        runtimeDimensions(input), std::move(values), input.numericClass);
    if (!result) {
        return failure(call, "factorial result has an invalid shape",
                       "MParser:InvalidFactorialResult");
    }
    return exactOutputs(call, {std::move(*result)});
}

struct BinaryIntegerSpec {
    RuntimeNumericClass outputClass = RuntimeNumericClass::Double;
    std::vector<size_t> outputDimensions;
};

std::optional<BinaryIntegerSpec> binaryIntegerSpec(
    const RuntimeValue& left, const RuntimeValue& right,
    std::string& error) {
    if (!isRuntimeNumericValue(left) || !isRuntimeNumericValue(right) ||
        left.numericClass == RuntimeNumericClass::Logical ||
        right.numericClass == RuntimeNumericClass::Logical) {
        error = "inputs must be non-logical numeric arrays";
        return std::nullopt;
    }
    const bool leftInteger = runtimeNumericClassIsInteger(left.numericClass);
    const bool rightInteger = runtimeNumericClassIsInteger(right.numericClass);
    RuntimeNumericClass outputClass = RuntimeNumericClass::Double;
    if (leftInteger || rightInteger) {
        const RuntimeValue& integerInput = leftInteger ? left : right;
        const RuntimeValue& otherInput = leftInteger ? right : left;
        const bool matchingInteger =
            runtimeNumericClassIsInteger(otherInput.numericClass) &&
            otherInput.numericClass == integerInput.numericClass;
        const bool doubleScalar =
            otherInput.numericClass == RuntimeNumericClass::Double &&
            runtimeShapeElementCount(otherInput) == 1;
        if (!matchingInteger && !doubleScalar) {
            error = "integer inputs require the same class or a double "
                    "scalar counterpart";
            return std::nullopt;
        }
        outputClass = integerInput.numericClass;
    } else {
        outputClass = left.numericClass == RuntimeNumericClass::Single ||
                              right.numericClass == RuntimeNumericClass::Single
                          ? RuntimeNumericClass::Single
                          : RuntimeNumericClass::Double;
    }

    const auto leftDimensions = runtimeDimensions(left);
    const auto rightDimensions = runtimeDimensions(right);
    const bool leftScalar = runtimeShapeElementCount(left) == 1;
    const bool rightScalar = runtimeShapeElementCount(right) == 1;
    std::vector<size_t> outputDimensions;
    if (leftDimensions == rightDimensions || rightScalar) {
        outputDimensions = leftDimensions;
    } else if (leftScalar) {
        outputDimensions = rightDimensions;
    } else {
        error = "inputs must have the same dimensions or one must be scalar";
        return std::nullopt;
    }
    return BinaryIntegerSpec{outputClass, std::move(outputDimensions)};
}

std::optional<RuntimeNumericElementValue> expandedElement(
    const RuntimeValue& value,
    const std::vector<size_t>& outputCoordinates) {
    const auto offset = runtimeImplicitExpansionStorageOffset(
        outputCoordinates, runtimeDimensions(value));
    return offset ? runtimeNumericStorageElementValue(value, *offset)
                  : std::nullopt;
}

BuiltinResult gcdLcmBuiltin(std::string_view name,
                            const BuiltinCall& call) {
    const RuntimeValue& left = call.arguments[0];
    const RuntimeValue& right = call.arguments[1];
    std::string error;
    const auto spec = binaryIntegerSpec(left, right, error);
    if (!spec) {
        return failure(call, std::string(name) + " " + error,
                       "MParser:InvalidIntegerPair");
    }
    const auto count = checkedRuntimeDimensionProduct(
        spec->outputDimensions);
    if (!count) {
        return failure(call, std::string(name) + " dimensions are too large",
                       "MParser:InvalidIntegerPairShape");
    }
    if (!executionCheckpoint(call) ||
        !observeNumericOutput(call, *count, spec->outputClass)) {
        return executionStopped(call, name);
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(*count);
    for (size_t logical = 0; logical < *count; ++logical) {
        if (!periodicExecutionCheckpoint(call, logical)) {
            return executionStopped(call, name);
        }
        const auto coordinates = runtimeColumnMajorCoordinates(
            logical, spec->outputDimensions);
        const auto leftElement = coordinates
                                     ? expandedElement(left, *coordinates)
                                     : std::nullopt;
        const auto rightElement = coordinates
                                      ? expandedElement(right, *coordinates)
                                      : std::nullopt;
        const auto leftMagnitude = leftElement
                                       ? exactMagnitude(*leftElement,
                                                        name == "gcd")
                                       : std::nullopt;
        const auto rightMagnitude = rightElement
                                        ? exactMagnitude(*rightElement,
                                                         name == "gcd")
                                        : std::nullopt;
        if (!leftMagnitude || !rightMagnitude) {
            return failure(call,
                           std::string(name) +
                               " inputs must be integer-valued" +
                               (name == "lcm" ? " and positive" : ""),
                           "MParser:InvalidIntegerPair");
        }
        const std::uint64_t leftValue = leftMagnitude.value();
        const std::uint64_t rightValue = rightMagnitude.value();
        if (name == "lcm" && (leftValue == 0 || rightValue == 0)) {
            return failure(call,
                           "lcm inputs must be integer-valued and positive",
                           "MParser:InvalidIntegerPair");
        }
        const std::uint64_t divisor =
            std::gcd(leftValue, rightValue);
        RuntimeNumericElementValue output;
        if (name == "lcm") {
            const std::uint64_t reduced = leftValue / divisor;
            if (runtimeNumericClassIsInteger(spec->outputClass)) {
                const std::uint64_t maximum =
                    integerClassMaximum(spec->outputClass);
                const std::uint64_t result =
                    reduced > maximum / rightValue
                        ? maximum
                        : reduced * rightValue;
                output = exactRealElement(result, spec->outputClass);
            } else {
                const long double result =
                    static_cast<long double>(reduced) *
                    static_cast<long double>(rightValue);
                output = floatingRealElement(
                    static_cast<double>(result), spec->outputClass);
            }
        } else if (runtimeNumericClassIsInteger(spec->outputClass)) {
            output = exactRealElement(
                std::min(divisor,
                         integerClassMaximum(spec->outputClass)),
                spec->outputClass);
        } else {
            output = floatingRealElement(static_cast<double>(divisor),
                                         spec->outputClass);
        }
        values.push_back(output);
    }
    auto result = runtimeNumericValueFromElements(
        spec->outputDimensions, std::move(values), spec->outputClass);
    if (!result) {
        return failure(call, std::string(name) +
                                 " result has an invalid shape",
                       "MParser:InvalidIntegerPairShape");
    }
    return exactOutputs(call, {std::move(*result)});
}

std::uint64_t addModulo(std::uint64_t left, std::uint64_t right,
                        std::uint64_t modulus) {
    return left >= modulus - right ? left - (modulus - right)
                                   : left + right;
}

std::uint64_t multiplyModulo(std::uint64_t left, std::uint64_t right,
                             std::uint64_t modulus) {
    std::uint64_t result = 0;
    left %= modulus;
    while (right != 0) {
        if ((right & 1U) != 0U) {
            result = addModulo(result, left, modulus);
        }
        right >>= 1U;
        if (right != 0) {
            left = addModulo(left, left, modulus);
        }
    }
    return result;
}

std::uint64_t powerModulo(std::uint64_t base, std::uint64_t exponent,
                          std::uint64_t modulus) {
    std::uint64_t result = 1;
    while (exponent != 0) {
        if ((exponent & 1U) != 0U) {
            result = multiplyModulo(result, base, modulus);
        }
        exponent >>= 1U;
        if (exponent != 0) {
            base = multiplyModulo(base, base, modulus);
        }
    }
    return result;
}

bool isPrime(std::uint64_t value) {
    if (value < 2) {
        return false;
    }
    for (const std::uint64_t prime :
         {2ULL, 3ULL, 5ULL, 7ULL, 11ULL, 13ULL, 17ULL, 19ULL,
          23ULL, 29ULL, 31ULL, 37ULL}) {
        if (value % prime == 0) {
            return value == prime;
        }
    }

    std::uint64_t odd = value - 1;
    unsigned shifts = 0;
    while ((odd & 1U) == 0U) {
        odd >>= 1U;
        ++shifts;
    }
    for (const std::uint64_t witness :
         {2ULL, 325ULL, 9375ULL, 28178ULL, 450775ULL,
          9780504ULL, 1795265022ULL}) {
        if (witness % value == 0) {
            continue;
        }
        std::uint64_t candidate = powerModulo(witness, odd, value);
        if (candidate == 1 || candidate == value - 1) {
            continue;
        }
        bool composite = true;
        for (unsigned round = 1; round < shifts; ++round) {
            candidate = multiplyModulo(candidate, candidate, value);
            if (candidate == value - 1) {
                composite = false;
                break;
            }
        }
        if (composite) {
            return false;
        }
    }
    return true;
}

BuiltinResult isprimeBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input) ||
        input.numericClass == RuntimeNumericClass::Logical) {
        return failure(call, "isprime expects a non-logical numeric array",
                       "MParser:InvalidPrimeInput");
    }
    const size_t count = runtimeShapeElementCount(input);
    if (!executionCheckpoint(call) ||
        !observeNumericOutput(call, count,
                              RuntimeNumericClass::Logical)) {
        return executionStopped(call, "isprime");
    }
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (!periodicExecutionCheckpoint(call, index, 1024U)) {
            return executionStopped(call, "isprime");
        }
        const auto element = runtimeNumericElementValue(input, index);
        const auto magnitude = element
                                   ? exactMagnitude(*element, false)
                                   : std::nullopt;
        if (!magnitude) {
            return failure(call,
                           "isprime inputs must be nonnegative integers",
                           "MParser:InvalidPrimeInput");
        }
        values.push_back(isPrime(*magnitude) ? 1.0 : 0.0);
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        runtimeDimensions(input), std::move(values),
        RuntimeNumericClass::Logical);
    if (!result) {
        return failure(call, "isprime result has an invalid shape",
                       "MParser:InvalidPrimeResult");
    }
    return exactOutputs(call, {std::move(*result)});
}

std::optional<long double> scalarReal(const RuntimeValue& value) {
    const auto element = numericScalar(value);
    if (!element || element->complex) {
        return std::nullopt;
    }
    if (runtimeNumericClassIsInteger(element->numericClass)) {
        if (runtimeNumericClassIsSignedInteger(element->numericClass)) {
            return static_cast<long double>(
                std::bit_cast<std::int64_t>(element->integerRealBits));
        }
        return static_cast<long double>(element->integerRealBits);
    }
    return static_cast<long double>(element->real);
}

BuiltinResult primesBuiltin(const BuiltinCall& call) {
    const auto rawLimit = scalarReal(call.arguments.front());
    if (!rawLimit || !std::isfinite(*rawLimit)) {
        return failure(call, "primes expects a finite real numeric scalar",
                       "MParser:InvalidPrimesInput");
    }
    const long double floored = std::floor(*rawLimit);
    if (floored != *rawLimit) {
        return failure(call, "primes expects a real integer scalar",
                       "MParser:InvalidPrimesInput");
    }
    const RuntimeNumericClass inputClass =
        call.arguments.front().numericClass;
    if (floored < 2.0L) {
        auto empty = runtimeNumericValueFromElements(
            {1, 0}, {}, inputClass);
        return empty
                   ? exactOutputs(call, {std::move(*empty)})
                   : failure(call, "primes empty result is invalid",
                             "MParser:InvalidPrimesResult");
    }
    if (floored > static_cast<long double>(
                      std::numeric_limits<size_t>::max() - 1U)) {
        return failure(call, "primes input is too large",
                       "MParser:PrimesResourceLimit");
    }
    const size_t limit = static_cast<size_t>(floored);
    const size_t sieveBytes = limit / 8U + 1U;
    if (!executionCheckpoint(call) ||
        (call.context && call.context->executionControl &&
         !call.context->executionControl->observeArrayBytes(sieveBytes))) {
        return executionStopped(call, "primes");
    }

    std::vector<bool> composite(limit + 1U, false);
    for (size_t candidate = 2;
         candidate <= limit / candidate; ++candidate) {
        if (!periodicExecutionCheckpoint(call, candidate, 1024U)) {
            return executionStopped(call, "primes");
        }
        if (composite[candidate]) {
            continue;
        }
        size_t work = 0;
        for (size_t multiple = candidate * candidate;
             multiple <= limit; multiple += candidate) {
            composite[multiple] = true;
            ++work;
            if (!periodicExecutionCheckpoint(call, work, 65536U)) {
                return executionStopped(call, "primes");
            }
        }
    }
    std::vector<RuntimeNumericElementValue> values;
    for (size_t candidate = 2; candidate <= limit; ++candidate) {
        if (!composite[candidate]) {
            if (!observeNumericOutput(call, values.size() + 1U,
                                      inputClass)) {
                return executionStopped(call, "primes");
            }
            values.push_back(runtimeNumericClassIsInteger(inputClass)
                                 ? exactRealElement(candidate, inputClass)
                                 : floatingRealElement(
                                       static_cast<double>(candidate),
                                       inputClass));
        }
        if ((candidate & 65535U) == 0U &&
            !executionCheckpoint(call)) {
            return executionStopped(call, "primes");
        }
    }
    const size_t resultCount = values.size();
    auto result = runtimeNumericValueFromElements(
        {1, resultCount}, std::move(values), inputClass);
    if (!result) {
        return failure(call, "primes result has an invalid shape",
                       "MParser:InvalidPrimesResult");
    }
    return exactOutputs(call, {std::move(*result)});
}

std::optional<size_t> logspacePointCount(const RuntimeValue& value) {
    const auto raw = scalarReal(value);
    if (!raw || !std::isfinite(*raw)) {
        return std::nullopt;
    }
    if (*raw <= 0.0L) {
        return 0;
    }
    const long double floored = std::floor(*raw);
    if (floored > static_cast<long double>(
                      std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<size_t>(floored);
}

BuiltinResult logspaceBuiltin(const BuiltinCall& call) {
    const auto start = numericScalar(call.arguments[0]);
    const auto finish = numericScalar(call.arguments[1]);
    const auto count = call.arguments.size() == 3
                           ? logspacePointCount(call.arguments[2])
                           : std::optional<size_t>{50};
    if (!start || !finish || !count ||
        !runtimeNumericClassIsFloating(start->numericClass) ||
        !runtimeNumericClassIsFloating(finish->numericClass)) {
        return failure(call,
                       "logspace endpoints must be floating-point scalars "
                       "and the point count must be a finite real scalar",
                       "MParser:InvalidLogspaceInput");
    }
    const RuntimeNumericClass outputClass =
        start->numericClass == RuntimeNumericClass::Single ||
                finish->numericClass == RuntimeNumericClass::Single
            ? RuntimeNumericClass::Single
            : RuntimeNumericClass::Double;
    const std::complex<double> begin(start->real,
                                     start->complex
                                         ? start->imaginary
                                         : 0.0);
    std::complex<double> end(finish->real,
                             finish->complex
                                 ? finish->imaginary
                                 : 0.0);
    const bool piEndpoint = !finish->complex &&
                            finish->real == std::acos(-1.0);
    if (piEndpoint) {
        end = std::complex<double>(std::log10(std::acos(-1.0)), 0.0);
    }
    if (!executionCheckpoint(call) ||
        !observeNumericOutput(call, *count, outputClass,
                              start->complex || finish->complex)) {
        return executionStopped(call, "logspace");
    }

    std::vector<RuntimeNumericElementValue> values;
    values.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        if (!periodicExecutionCheckpoint(call, index)) {
            return executionStopped(call, "logspace");
        }
        const double ratio = *count <= 1
                                 ? 1.0
                                 : static_cast<double>(index) /
                                       static_cast<double>(*count - 1);
        const std::complex<double> exponent =
            begin + (end - begin) * ratio;
        std::complex<double> value = std::pow(10.0, exponent);
        if (piEndpoint && index + 1 == *count) {
            value = {std::acos(-1.0), 0.0};
        }
        RuntimeNumericElementValue element;
        element.numericClass = outputClass;
        element.real = value.real();
        element.imaginary = value.imag();
        element.complex = start->complex || finish->complex ||
                          value.imag() != 0.0;
        values.push_back(element);
    }
    auto result = runtimeNumericValueFromElements(
        {1, *count}, std::move(values), outputClass);
    if (!result) {
        return failure(call, "logspace result has an invalid shape",
                       "MParser:InvalidLogspaceResult");
    }
    return exactOutputs(call, {std::move(*result)});
}

bool numericVector(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        value.numericClass == RuntimeNumericClass::Logical ||
        value.numericComplex) {
        return false;
    }
    const size_t count = runtimeShapeElementCount(value);
    if (count == 0 || count == 1) {
        return true;
    }
    size_t nonsingleton = 0;
    for (const size_t dimension : runtimeDimensions(value)) {
        nonsingleton += dimension > 1 ? 1U : 0U;
    }
    return nonsingleton <= 1;
}

BuiltinResult meshgridBuiltin(const BuiltinCall& call) {
    if (std::any_of(call.arguments.begin(), call.arguments.end(),
                    [](const RuntimeValue& value) {
                        return !numericVector(value);
                    })) {
        return failure(call, "meshgrid inputs must be numeric vectors",
                       "MParser:InvalidMeshgridInput");
    }
    size_t rank = call.arguments.size();
    if (rank == 1) {
        rank = std::max<size_t>(2, call.requestedOutputCount);
    }
    if (rank > 3 || call.requestedOutputCount > rank) {
        return failure(call,
                       "meshgrid supports one to three grid dimensions",
                       "MParser:InvalidMeshgridArity");
    }
    if (call.requestedOutputCount == 0) {
        return BuiltinResult::success();
    }

    std::vector<const RuntimeValue*> sources(rank,
                                              &call.arguments.front());
    for (size_t index = 0; index < call.arguments.size(); ++index) {
        sources[index] = &call.arguments[index];
    }
    std::vector<size_t> lengths(rank, 0);
    for (size_t index = 0; index < rank; ++index) {
        lengths[index] = runtimeShapeElementCount(*sources[index]);
    }
    std::vector<size_t> dimensions;
    dimensions.reserve(rank);
    dimensions.push_back(lengths[1]);
    dimensions.push_back(lengths[0]);
    if (rank == 3) {
        dimensions.push_back(lengths[2]);
    }
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure(call, "meshgrid dimensions are too large",
                       "MParser:InvalidMeshgridShape");
    }

    std::vector<RuntimeValue> outputs;
    outputs.reserve(call.requestedOutputCount);
    for (size_t output = 0; output < call.requestedOutputCount; ++output) {
        const RuntimeValue& source = *sources[output];
        if (!executionCheckpoint(call) ||
            !observeNumericOutput(call, *count, source.numericClass,
                                  source.numericComplex)) {
            return executionStopped(call, "meshgrid");
        }
        std::vector<RuntimeNumericElementValue> values;
        values.reserve(*count);
        for (size_t logical = 0; logical < *count; ++logical) {
            if (!periodicExecutionCheckpoint(call, logical)) {
                return executionStopped(call, "meshgrid");
            }
            auto coordinates = runtimeColumnMajorCoordinates(
                logical, dimensions);
            if (!coordinates) {
                return failure(call,
                               "meshgrid could not map output coordinates",
                               "MParser:InvalidMeshgridShape");
            }
            coordinates->resize(rank, 0);
            const size_t sourceIndex = output == 0
                                           ? (*coordinates)[1]
                                           : output == 1
                                               ? (*coordinates)[0]
                                               : (*coordinates)[2];
            const auto element = runtimeNumericElementValue(source,
                                                             sourceIndex);
            if (!element) {
                return failure(call,
                               "meshgrid could not read an input element",
                               "MParser:InvalidMeshgridInput");
            }
            values.push_back(*element);
        }
        auto value = runtimeNumericValueFromElements(
            dimensions, std::move(values), source.numericClass);
        if (!value) {
            return failure(call, "meshgrid output has an invalid shape",
                           "MParser:InvalidMeshgridShape");
        }
        outputs.push_back(std::move(*value));
    }
    return exactOutputs(call, std::move(outputs));
}

} // namespace

bool isRuntimeNumericLibraryBuiltin(std::string_view name) {
    return name == "factorial" || name == "gcd" ||
           name == "isprime" || name == "lcm" ||
           name == "logspace" || name == "meshgrid" ||
           name == "primes";
}

BuiltinResult invokeRuntimeNumericLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "factorial") {
        return factorialBuiltin(call);
    }
    if (name == "gcd" || name == "lcm") {
        return gcdLcmBuiltin(name, call);
    }
    if (name == "isprime") {
        return isprimeBuiltin(call);
    }
    if (name == "primes") {
        return primesBuiltin(call);
    }
    if (name == "logspace") {
        return logspaceBuiltin(call);
    }
    if (name == "meshgrid") {
        return meshgridBuiltin(call);
    }
    return failure(call, "unknown numeric library builtin",
                   "MParser:UnknownBuiltin");
}

} // namespace mparser
