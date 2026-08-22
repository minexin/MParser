#include "mparser/runtime/core/runtime_range.h"

#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {

RuntimeColonRange runtimePlanColonRange(double start, double step,
                                        double stop) {
    RuntimeColonRange range;
    range.start = start;
    range.step = step;
    range.stop = stop;
    if (step == 0.0) {
        range.error = "colon range step cannot be zero";
        return range;
    }
    range.succeeded = true;
    return range;
}

RuntimeColonRange runtimePlanColonRange(
    const std::vector<double>& terms) {
    if (terms.size() != 2 && terms.size() != 3) {
        RuntimeColonRange range;
        range.error = "colon range must have two or three operands";
        return range;
    }

    const double step = terms.size() == 3 ? terms[1] : 1.0;
    const double stop = terms.size() == 3 ? terms[2] : terms[1];
    return runtimePlanColonRange(terms[0], step, stop);
}

bool runtimeColonRangeContains(const RuntimeColonRange& range,
                               double value) {
    if (!range.succeeded || std::isnan(value) ||
        std::isnan(range.stop)) {
        return false;
    }
    return range.step > 0.0 ? value <= range.stop
                            : value >= range.stop;
}

std::vector<double> runtimeMaterializeColonRange(
    const RuntimeColonRange& range) {
    std::vector<double> values;
    for (double value = range.start;
         runtimeColonRangeContains(range, value);) {
        values.push_back(value);
        const double next = value + range.step;
        if (next == value) {
            break;
        }
        value = next;
    }
    return values;
}

namespace {

RuntimeColonValueResult valueFailure(std::string message) {
    return RuntimeColonValueResult{false, RuntimeValue{},
                                   std::move(message)};
}

RuntimeColonValueResult valueSuccess(RuntimeValue value) {
    return RuntimeColonValueResult{true, std::move(value), {}};
}

std::optional<RuntimeNumericElementValue> scalarElement(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericElementValue(value, 0);
}

RuntimeColonValueResult materializeSignedIntegerRange(
    const std::vector<RuntimeNumericElementValue>& terms,
    RuntimeNumericClass numericClass) {
    const auto signedValue = [](std::uint64_t bits) {
        return std::bit_cast<std::int64_t>(bits);
    };
    const std::int64_t start = signedValue(terms[0].integerRealBits);
    const std::int64_t step = terms.size() == 3
                                  ? signedValue(terms[1].integerRealBits)
                                  : 1;
    const std::int64_t stop = signedValue(
        terms.size() == 3 ? terms[2].integerRealBits
                          : terms[1].integerRealBits);
    if (step == 0) {
        return valueFailure("colon range step cannot be zero");
    }

    std::vector<RuntimeNumericElementValue> values;
    for (std::int64_t current = start;
         step > 0 ? current <= stop : current >= stop;) {
        RuntimeNumericElementValue value;
        value.numericClass = numericClass;
        value.real = static_cast<double>(current);
        value.integerRealBits = std::bit_cast<std::uint64_t>(current);
        values.push_back(value);
        if ((step > 0 &&
             current > std::numeric_limits<std::int64_t>::max() - step) ||
            (step < 0 &&
             current < std::numeric_limits<std::int64_t>::min() - step)) {
            break;
        }
        current += step;
    }

    const size_t count = values.size();
    auto result = runtimeNumericValueFromElements(
        {1, count}, std::move(values), numericClass);
    return result ? valueSuccess(std::move(*result))
                  : valueFailure("colon could not construct its integer result");
}

RuntimeColonValueResult materializeUnsignedIntegerRange(
    const std::vector<RuntimeNumericElementValue>& terms,
    RuntimeNumericClass numericClass) {
    const std::uint64_t start = terms[0].integerRealBits;
    const std::uint64_t step =
        terms.size() == 3 ? terms[1].integerRealBits : 1;
    const std::uint64_t stop =
        terms.size() == 3 ? terms[2].integerRealBits
                          : terms[1].integerRealBits;
    if (step == 0) {
        return valueFailure("colon range step cannot be zero");
    }

    std::vector<RuntimeNumericElementValue> values;
    for (std::uint64_t current = start; current <= stop;) {
        RuntimeNumericElementValue value;
        value.numericClass = numericClass;
        value.real = static_cast<double>(current);
        value.integerRealBits = current;
        values.push_back(value);
        if (current > std::numeric_limits<std::uint64_t>::max() - step) {
            break;
        }
        current += step;
    }

    const size_t count = values.size();
    auto result = runtimeNumericValueFromElements(
        {1, count}, std::move(values), numericClass);
    return result ? valueSuccess(std::move(*result))
                  : valueFailure("colon could not construct its integer result");
}

} // namespace

RuntimeColonValueResult runtimeMaterializeColonValue(
    const std::vector<RuntimeValue>& operands) {
    if (operands.size() != 2 && operands.size() != 3) {
        return valueFailure("colon range must have two or three operands");
    }

    std::vector<RuntimeNumericElementValue> terms;
    terms.reserve(operands.size());
    for (const RuntimeValue& operand : operands) {
        const auto element = scalarElement(operand);
        if (!element) {
            return valueFailure("colon operand must be a scalar number");
        }
        if (element->complex) {
            return valueFailure("colon operands must be real scalar numbers");
        }
        terms.push_back(*element);
    }

    const bool anyInteger = std::any_of(
        terms.begin(), terms.end(), [](const auto& term) {
            return runtimeNumericClassIsInteger(term.numericClass);
        });
    if (anyInteger) {
        const RuntimeNumericClass numericClass = terms.front().numericClass;
        if (!runtimeNumericClassIsInteger(numericClass) ||
            !std::all_of(terms.begin(), terms.end(),
                         [numericClass](const auto& term) {
                             return term.numericClass == numericClass;
                         })) {
            return valueFailure(
                "integer colon operands must use the same integer class");
        }
        return runtimeNumericClassIsSignedInteger(numericClass)
                   ? materializeSignedIntegerRange(terms, numericClass)
                   : materializeUnsignedIntegerRange(terms, numericClass);
    }

    std::vector<double> realTerms;
    realTerms.reserve(terms.size());
    bool single = false;
    for (const auto& term : terms) {
        realTerms.push_back(term.real);
        single = single || term.numericClass == RuntimeNumericClass::Single;
    }
    const auto range = runtimePlanColonRange(realTerms);
    if (!range.succeeded) {
        return valueFailure(range.error);
    }
    std::vector<double> materialized = runtimeMaterializeColonRange(range);
    const size_t count = materialized.size();
    auto result = runtimeNumericValueFromLogicalOrder(
        {1, count}, std::move(materialized),
        single ? RuntimeNumericClass::Single
               : RuntimeNumericClass::Double);
    return result ? valueSuccess(std::move(*result))
                  : valueFailure("colon could not construct its numeric result");
}

} // namespace mparser
