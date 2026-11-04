#include "mparser/runtime/core/value/runtime_range.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace mparser {

namespace {
template<class Number>
RuntimeColonRange planFloatingRange(Number start, Number step, Number stop) {
    RuntimeColonRange range;
    range.start = start;
    range.step = step;
    range.stop = stop;
    range.single = sizeof(Number) == sizeof(float);
    range.succeeded = true;
    if (step == 0 || (step > 0 && start > stop) || (step < 0 && start < stop)) {
        return range;
    }
    if (std::isnan(start) || std::isnan(step) || std::isnan(stop)) {
        range.start = range.stop = std::numeric_limits<Number>::quiet_NaN();
        range.count = 1;
        return range;
    }
    Number span = stop - start;
    Number quotient = span / step;
    if (std::isinf(span) && std::isfinite(start) && std::isfinite(stop)) {
        quotient = stop / step - start / step;
    }
    if (std::isnan(quotient)) {
        range.start = range.stop = std::numeric_limits<Number>::quiet_NaN();
        range.count = 1;
        return range;
    }
    const auto maximumCount = std::vector<double>{}.max_size();
    if (!std::isfinite(quotient) || quotient >= static_cast<double>(maximumCount)) {
        range.succeeded = false;
        range.error = "colon range exceeds maximum array size";
        return range;
    }
    if (quotient == 0) {
        range.stop = start;
        range.count = 1;
        return range;
    }

    Number intervals = std::floor(Number(quotient + Number(0.5)));
    const auto endpoint = [start, step](Number distance) {
        const Number offset = distance * step;
        const Number result = start + offset;
        return !std::isfinite(offset) || !std::isfinite(result)
            ? std::fma(distance, step, start) : result;
    };
    Number last = endpoint(intervals);
    const Number tolerance = Number(2) * std::numeric_limits<Number>::epsilon() *
        std::max(std::abs(start), std::abs(stop));
    const Number difference = step > 0 ? Number(last - stop) : Number(stop - last);
    if (std::abs(difference) <= tolerance) {
        last = stop;
    } else if (difference > 0) {
        intervals -= Number(1);
        last = endpoint(intervals);
    }
    range.count = static_cast<size_t>(intervals) + 1;
    range.stop = last;
    return range;
}

template<class Number>
double floatingRangeElement(const RuntimeColonSequence& range, size_t index) {
    const double start = range.start;
    const double stop = range.stop;
    const double step = range.step;
    if (index == 0) { return start; }
    const size_t remaining = range.count - 1 - index;
    if (remaining == 0) { return stop; }
    if (index < remaining) { return Number(start + static_cast<double>(index) * step); }
    if (index > remaining) { return Number(stop - static_cast<double>(remaining) * step); }
    // Fill from both ends, with a symmetric midpoint, rather than accumulating error.
    const double sum = start + stop;
    return std::isfinite(sum) ? Number(sum / 2)
        : Number(start / 2 + stop / 2);
}
} // namespace

RuntimeColonRange runtimePlanColonRange(double start, double step, double stop) {
    return planFloatingRange(start, step, stop);
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

double runtimeColonRangeElement(const RuntimeColonSequence& range, size_t index) {
    return range.single ? floatingRangeElement<float>(range, index)
                        : floatingRangeElement<double>(range, index);
}

std::vector<double> runtimeMaterializeColonRange(
    const RuntimeColonRange& range) {
    std::vector<double> values(range.count);
    for (size_t index = 0; index < range.count; ++index) {
        values[index] = runtimeColonRangeElement(range, index);
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
    const double step = realTerms.size() == 3 ? realTerms[1] : 1.0;
    const double stop = realTerms.back();
    const auto range = single
        ? planFloatingRange(static_cast<float>(realTerms.front()), static_cast<float>(step), static_cast<float>(stop))
        : runtimePlanColonRange(realTerms.front(), step, stop);
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
