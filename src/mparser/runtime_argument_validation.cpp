#include "mparser/runtime_argument_validation.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace mparser {
namespace {

bool isNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value);
}

bool isArray(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Vector ||
           value.kind == RuntimeValueKind::Matrix;
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
}

bool isString(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::String;
}

size_t valueCount(const RuntimeValue& value, std::string_view className) {
    if (value.kind == RuntimeValueKind::Missing) {
        return 0;
    }
    if (isString(value) && className == "string") {
        return value.rows * value.columns;
    }
    if (isString(value)) {
        return value.text.size();
    }
    if (isCell(value)) {
        return value.cells.size();
    }
    return value.kind == RuntimeValueKind::Number ? 1
                                                  : value.elements.size();
}

std::optional<double> parseNumber(std::string_view text) {
    std::string buffer(text);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::optional<size_t> dimensionValue(const PropertyDimensionSpec& dimension) {
    if (dimension.text == ":") {
        return std::nullopt;
    }
    const auto parsed = parseNumber(dimension.text);
    if (!parsed || !std::isfinite(*parsed) || *parsed <= 0.0 ||
        std::floor(*parsed) != *parsed) {
        return std::nullopt;
    }
    return static_cast<size_t>(*parsed);
}

RuntimeArgumentValidationResult failure(std::string message) {
    return RuntimeArgumentValidationResult{false, {}, std::move(message)};
}

RuntimeArgumentValidationResult success(RuntimeValue value) {
    return RuntimeArgumentValidationResult{true, std::move(value), {}};
}

RuntimeArgumentValidationResult coerceClass(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options) {
    const std::string& type = spec.className;
    if (type.empty()) {
        return success(std::move(value));
    }
    if (type == "double" || type == "logical") {
        if (!isNumeric(value)) {
            return failure("value must be numeric for class " + type);
        }
        auto converted = runtimeConvertNumericClass(
            std::move(value), type == "logical"
                                  ? RuntimeNumericClass::Logical
                                  : RuntimeNumericClass::Double);
        if (!converted) {
            return failure("value cannot be converted to class " + type);
        }
        return success(std::move(*converted));
    }
    if (type == "char" || type == "string") {
        if (!isString(value)) {
            return failure("value must be text for class " + type);
        }
        if (type == "string" && (value.rows != 0 || value.columns != 0)) {
            setRuntimeDimensions(value, {1, 1});
        }
        return success(std::move(value));
    }
    if (type == "cell") {
        return isCell(value) ? success(std::move(value))
                             : failure("value must be a cell array");
    }
    if (type == "handle") {
        return value.kind == RuntimeValueKind::Object && value.handleObject
                   ? success(std::move(value))
                   : failure("value must be a handle object");
    }
    if (value.kind == RuntimeValueKind::Missing) {
        return success(std::move(value));
    }
    if (value.kind != RuntimeValueKind::Object) {
        return failure("value must be an object of class " + type);
    }
    if (options.objectIsA
            ? options.objectIsA(value.className, type)
            : value.className == type) {
        return success(std::move(value));
    }
    if (options.classAvailable && !options.classAvailable(type)) {
        return failure("argument class is not available: " + type);
    }
    return failure("value must be an object of class " + type);
}

RuntimeArgumentValidationResult reshapeValue(
    RuntimeValue value, std::vector<size_t> dimensions,
    std::string_view className) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure("argument dimensions are too large");
    }
    const size_t currentCount = valueCount(value, className);
    if (value.kind == RuntimeValueKind::Number) {
        if (*count == 1) {
            return success(std::move(value));
        }
        value.kind = dimensions.size() == 2 && dimensions[0] == 1
                         ? RuntimeValueKind::Vector
                         : RuntimeValueKind::Matrix;
        value.elements.assign(*count, value.number);
        value.number = 0.0;
        setRuntimeDimensions(value, std::move(dimensions));
        return success(std::move(value));
    }
    if (currentCount != *count) {
        return failure("value element count does not match the argument size");
    }
    if (isArray(value)) {
        if (*count == 1) {
            value.kind = RuntimeValueKind::Number;
            value.number = value.elements.front();
            value.elements.clear();
            setRuntimeDimensions(value, {1, 1});
            return success(std::move(value));
        }
        value.kind = dimensions.size() == 2 && dimensions[0] == 1
                         ? RuntimeValueKind::Vector
                         : RuntimeValueKind::Matrix;
        setRuntimeDimensions(value, std::move(dimensions));
        return success(std::move(value));
    }
    if (isCell(value) || isString(value)) {
        setRuntimeDimensions(value, std::move(dimensions));
        return success(std::move(value));
    }
    return failure("value cannot be reshaped to the argument size");
}

RuntimeArgumentValidationResult coerceSize(RuntimeValue value,
                                            const PropertySpec& spec) {
    if (spec.dimensions.empty()) {
        return success(std::move(value));
    }
    std::vector<std::optional<size_t>> expected;
    expected.reserve(std::max<size_t>(2, spec.dimensions.size()));
    for (const auto& dimension : spec.dimensions) {
        if (dimension.text != ":" && !dimensionValue(dimension)) {
            return failure("argument dimension cannot be represented at runtime");
        }
        expected.push_back(dimensionValue(dimension));
    }
    if (expected.size() == 1) {
        expected.push_back(1);
    }

    const auto actual = runtimeDimensions(value);
    bool exact = true;
    for (size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] &&
            runtimeDimension(value, index) != *expected[index]) {
            exact = false;
        }
    }
    for (size_t index = expected.size(); index < actual.size(); ++index) {
        exact = exact && actual[index] == 1;
    }
    if (exact) {
        return success(std::move(value));
    }
    if (!isNumeric(value) && !isCell(value) && !isString(value)) {
        return failure("value shape does not match the argument size");
    }

    size_t wildcardCount = 0;
    size_t wildcardIndex = 0;
    std::vector<size_t> target(expected.size(), 1);
    for (size_t index = 0; index < expected.size(); ++index) {
        if (expected[index]) {
            target[index] = *expected[index];
        } else {
            ++wildcardCount;
            wildcardIndex = index;
        }
    }
    if (wildcardCount > 1) {
        return failure("value shape cannot be inferred for multiple wildcard dimensions");
    }
    const size_t count = valueCount(value, spec.className);
    if (wildcardCount == 1) {
        const auto fixed = checkedRuntimeDimensionProduct(target);
        if (!fixed || *fixed == 0 || count % *fixed != 0) {
            return failure("value element count does not match the argument size");
        }
        target[wildcardIndex] = count / *fixed;
    }
    return reshapeValue(std::move(value), std::move(target), spec.className);
}

template <typename Predicate>
bool allNumeric(const RuntimeValue& value, Predicate predicate) {
    if (value.kind == RuntimeValueKind::Number) {
        return predicate(value.number);
    }
    return isArray(value) &&
           std::all_of(value.elements.begin(), value.elements.end(), predicate);
}

std::optional<double> comparisonValue(
    const PropertyValidatorSpec& validator) {
    if (validator.arguments.empty() || validator.arguments.size() > 2) {
        return std::nullopt;
    }
    return parseNumber(validator.arguments.back());
}

std::optional<std::string> applyValidator(
    const RuntimeValue& value, const PropertyValidatorSpec& validator,
    std::string_view className) {
    const std::string& name = validator.name;
    const auto numeric = [&](auto predicate, std::string message)
        -> std::optional<std::string> {
        if (!isNumeric(value)) {
            return name + " requires numeric data";
        }
        return allNumeric(value, predicate)
                   ? std::nullopt
                   : std::optional<std::string>(std::move(message));
    };
    if (name == "mustBePositive")
        return numeric([](double x) { return x > 0; }, "value must be positive");
    if (name == "mustBeNonpositive")
        return numeric([](double x) { return x <= 0; }, "value must be nonpositive");
    if (name == "mustBeNonnegative")
        return numeric([](double x) { return x >= 0; }, "value must be nonnegative");
    if (name == "mustBeNegative")
        return numeric([](double x) { return x < 0; }, "value must be negative");
    if (name == "mustBeFinite")
        return numeric([](double x) { return std::isfinite(x); }, "value must be finite");
    if (name == "mustBeNonNan")
        return numeric([](double x) { return !std::isnan(x); }, "value must not contain NaN");
    if (name == "mustBeNonzero")
        return numeric([](double x) { return x != 0; }, "value must be nonzero");
    if (name == "mustBeInteger")
        return numeric([](double x) { return std::isfinite(x) && std::floor(x) == x; },
                       "value must contain integers");
    if (name == "mustBeReal" || name == "mustBeFloat" ||
        name == "mustBeNumeric" || name == "mustBeNumericOrLogical") {
        return isNumeric(value) ? std::nullopt
                                : std::optional<std::string>(name + " requires numeric data");
    }
    if (name == "mustBeNonsparse") return std::nullopt;
    if (name == "mustBeSparse") return "sparse values are not represented by the current runtime";

    const size_t count = valueCount(value, className);
    const auto dimensions = runtimeDimensions(value);
    const size_t rows = runtimeDimension(value, 0);
    const size_t columns = runtimeDimension(value, 1);
    const bool empty = value.kind == RuntimeValueKind::Missing || count == 0;
    if (name == "mustBeNonempty") return !empty ? std::nullopt : std::optional<std::string>("value must be nonempty");
    if (name == "mustBeScalarOrEmpty") return count <= 1 ? std::nullopt : std::optional<std::string>("value must be scalar or empty");
    if (name == "mustBeVector") return (!empty && dimensions.size() == 2 && (rows == 1 || columns == 1)) ? std::nullopt : std::optional<std::string>("value must be a vector");
    if (name == "mustBeRow") return (dimensions.size() == 2 && rows == 1) ? std::nullopt : std::optional<std::string>("value must be a row");
    if (name == "mustBeColumn") return (dimensions.size() == 2 && columns == 1) ? std::nullopt : std::optional<std::string>("value must be a column");
    if (name == "mustBeMatrix") return dimensions.size() == 2 ? std::nullopt : std::optional<std::string>("value must be a matrix");
    if (name == "mustBeText" || name == "mustBeTextScalar") return isString(value) ? std::nullopt : std::optional<std::string>("value must be text");
    if (name == "mustBeNonzeroLengthText") return (isString(value) && !value.text.empty()) ? std::nullopt : std::optional<std::string>("text value must have nonzero length");
    if (name == "mustBeNonmissing") {
        const bool present = value.kind != RuntimeValueKind::Missing &&
            (!isNumeric(value) || allNumeric(value, [](double x) { return !std::isnan(x); }));
        return present ? std::nullopt : std::optional<std::string>("value must not be missing");
    }
    if (name == "mustBeGreaterThan" || name == "mustBeLessThan" ||
        name == "mustBeGreaterThanOrEqual" ||
        name == "mustBeLessThanOrEqual") {
        if (!isNumeric(value)) return name + " requires numeric data";
        const auto limit = comparisonValue(validator);
        if (!limit) return name + " requires one literal comparison value";
        bool valid = false;
        if (name == "mustBeGreaterThan") valid = allNumeric(value, [&](double x) { return x > *limit; });
        else if (name == "mustBeLessThan") valid = allNumeric(value, [&](double x) { return x < *limit; });
        else if (name == "mustBeGreaterThanOrEqual") valid = allNumeric(value, [&](double x) { return x >= *limit; });
        else valid = allNumeric(value, [&](double x) { return x <= *limit; });
        return valid ? std::nullopt : std::optional<std::string>("value does not satisfy " + name);
    }
    return "validator is not executable yet: " + name;
}

} // namespace

RuntimeArgumentValidationResult validateRuntimeArgument(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options) {
    auto classResult = coerceClass(std::move(value), spec, options);
    if (!classResult.succeeded) return classResult;
    auto sizeResult = coerceSize(std::move(classResult.value), spec);
    if (!sizeResult.succeeded) return sizeResult;
    for (const auto& validator : spec.validators) {
        if (const auto error = applyValidator(
                sizeResult.value, validator, spec.className)) {
            return failure(*error);
        }
    }
    return sizeResult;
}

} // namespace mparser
