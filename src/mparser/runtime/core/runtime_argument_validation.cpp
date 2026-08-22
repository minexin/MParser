#include "mparser/runtime/core/runtime_argument_validation.h"

#include "mparser/runtime/builtins/runtime_array_ops.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>

namespace mparser {
namespace {

bool isNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value);
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
}

size_t valueCount(const RuntimeValue& value, std::string_view className) {
    if (value.kind == RuntimeValueKind::Missing) {
        return 0;
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        return runtimeShapeElementCount(value);
    }
    if (isRuntimeStringArray(value) && className == "string") {
        return runtimeShapeElementCount(value);
    }
    if (isRuntimeTextValue(value)) {
        return runtimeShapeElementCount(value);
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

struct NameValueField {
    std::string declaration;
    std::string field;
};

enum class NameMatchKind {
    None,
    Resolved,
    Ambiguous,
};

struct NameMatch {
    NameMatchKind kind = NameMatchKind::None;
    std::string declaration;
    std::vector<std::string> candidates;
};

std::vector<NameValueField> collectNameValueFields(
    const std::vector<std::string>& declarations) {
    std::vector<NameValueField> fields;
    fields.reserve(declarations.size());
    for (const auto& declaration : declarations) {
        const size_t dot = declaration.find('.');
        if (dot == std::string::npos || dot == 0 ||
            dot + 1 >= declaration.size()) {
            continue;
        }
        fields.push_back(NameValueField{
            declaration, declaration.substr(dot + 1)});
    }
    return fields;
}

NameMatch resolveNameValueName(const std::vector<NameValueField>& fields,
                               std::string_view supplied) {
    NameMatch match;
    for (const auto& field : fields) {
        if (field.field == supplied) {
            match.candidates.push_back(field.declaration);
        }
    }
    if (match.candidates.size() == 1) {
        match.kind = NameMatchKind::Resolved;
        match.declaration = match.candidates.front();
        return match;
    }
    if (match.candidates.size() > 1) {
        match.kind = NameMatchKind::Ambiguous;
        return match;
    }

    for (const auto& field : fields) {
        if (field.field.starts_with(supplied)) {
            match.candidates.push_back(field.declaration);
        }
    }
    if (match.candidates.size() == 1) {
        match.kind = NameMatchKind::Resolved;
        match.declaration = match.candidates.front();
    } else if (!match.candidates.empty()) {
        match.kind = NameMatchKind::Ambiguous;
    }
    return match;
}

std::string ambiguousNameMessage(std::string_view supplied,
                                 const NameMatch& match) {
    std::ostringstream message;
    message << "ambiguous name-value argument: " << supplied;
    if (!match.candidates.empty()) {
        message << " (matches ";
        for (size_t index = 0; index < match.candidates.size(); ++index) {
            if (index != 0) {
                message << ", ";
            }
            const auto& declaration = match.candidates[index];
            const size_t dot = declaration.find('.');
            message << (dot == std::string::npos
                            ? declaration
                            : declaration.substr(dot + 1));
        }
        message << ")";
    }
    return message.str();
}

bool isNameValueWrapper(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::NameValueArgument;
}

bool positionalPrefixIsValid(const FunctionSignature& signature,
                             size_t count) {
    return functionPositionalArgumentCountStatus(signature, count) ==
           FunctionArgumentCountStatus::Valid;
}

std::string positionalCountError(const FunctionSignature& signature,
                                 size_t count) {
    if (functionPositionalArgumentCountStatus(signature, count) ==
        FunctionArgumentCountStatus::IncompleteRepeatingGroup) {
        return "incomplete repeating argument group: expected a multiple of " +
               std::to_string(functionRepeatingParameterCount(signature)) +
               " values";
    }
    return "function argument count mismatch";
}

RuntimeInvocationNormalizationResult normalizationFailure(
    std::string error) {
    RuntimeInvocationNormalizationResult result;
    result.error = std::move(error);
    return result;
}

RuntimeInvocationNormalizationResult parseNameValueTail(
    const FunctionSignature& signature,
    const std::vector<NameValueField>& fields,
    const std::vector<RuntimeValue>& arguments, size_t tailBegin) {
    if (!positionalPrefixIsValid(signature, tailBegin)) {
        return normalizationFailure(positionalCountError(signature, tailBegin));
    }

    RuntimeInvocationNormalizationResult result;
    result.positionalArguments.assign(arguments.begin(),
                                      arguments.begin() + tailBegin);
    result.positionalArgumentCount = tailBegin;
    size_t index = tailBegin;
    while (index < arguments.size()) {
        std::string suppliedName;
        RuntimeValue value;
        if (isNameValueWrapper(arguments[index])) {
            if (arguments[index].text.empty() ||
                arguments[index].cells.size() != 1) {
                return normalizationFailure(
                    "malformed name=value argument");
            }
            suppliedName = arguments[index].text;
            value = arguments[index].cells.front();
            ++index;
        } else {
            const auto name = runtimeTextScalarUtf8(arguments[index]);
            if (!name) {
                return normalizationFailure(
                    "positional argument cannot follow a name-value argument");
            }
            suppliedName = *name;
            if (index + 1 >= arguments.size()) {
                return normalizationFailure(
                    "name-value argument is missing a value: " +
                    suppliedName);
            }
            value = arguments[index + 1];
            index += 2;
        }

        const NameMatch match = resolveNameValueName(fields, suppliedName);
        if (match.kind == NameMatchKind::None) {
            return normalizationFailure("unknown name-value argument: " +
                                        suppliedName);
        }
        if (match.kind == NameMatchKind::Ambiguous) {
            return normalizationFailure(
                ambiguousNameMessage(suppliedName, match));
        }
        result.nameValueArguments[match.declaration] = std::move(value);
    }
    result.succeeded = true;
    return result;
}

RuntimeArgumentValidationResult coerceClass(
    RuntimeValue value, const PropertySpec& spec,
    const RuntimeArgumentValidationOptions& options) {
    const std::string& type = spec.className;
    if (type.empty()) {
        return success(std::move(value));
    }
    if (const auto numericClass = runtimeNumericClassFromName(type)) {
        if (!isNumeric(value)) {
            return failure("value must be numeric for class " + type);
        }
        auto converted = runtimeConvertNumericClass(
            std::move(value), *numericClass);
        if (!converted) {
            return failure("value cannot be converted to class " + type);
        }
        return success(std::move(*converted));
    }
    if (type == "char") {
        if (isRuntimeCharacterArray(value)) {
            return success(std::move(value));
        }
        if (const auto text = runtimeTextScalarCodeUnits(value)) {
            return success(makeRuntimeCharacterVector(*text));
        }
        return failure("value must be text for class char");
    }
    if (type == "string") {
        if (isRuntimeStringArray(value)) {
            return success(std::move(value));
        }
        if (const auto text = runtimeTextScalarCodeUnits(value)) {
            return success(makeRuntimeStringScalar(*text));
        }
        return failure("value must be text for class string");
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
    if (type == "missing") {
        return value.kind == RuntimeValueKind::MissingArray
                   ? success(std::move(value))
                   : failure("value must be a missing array");
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
    if (isNumeric(value) && currentCount == 1 && *count > 1) {
        const auto element = runtimeNumericElementValue(value, 0);
        if (!element) {
            return failure("numeric value cannot be expanded to the argument size");
        }
        auto expanded = runtimeNumericValueFromElements(
            dimensions,
            std::vector<RuntimeNumericElementValue>(*count, *element),
            value.numericClass);
        return expanded
                   ? success(std::move(*expanded))
                   : failure("numeric value cannot be expanded to the argument size");
    }
    if (currentCount != *count) {
        return failure("value element count does not match the argument size");
    }
    if (value.kind == RuntimeValueKind::MissingArray ||
        isNumeric(value) || isCell(value) || isRuntimeTextValue(value)) {
        auto reshaped = runtimeReshapeValue(value, std::move(dimensions));
        return reshaped.succeeded
                   ? success(std::move(reshaped.value))
                   : failure(std::move(reshaped.error));
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
    if (value.kind != RuntimeValueKind::MissingArray &&
        !isNumeric(value) && !isCell(value) &&
        !isRuntimeTextValue(value)) {
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
    if (!isNumeric(value)) {
        return false;
    }
    const size_t count = runtimeShapeElementCount(value);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element || !predicate(*element)) {
            return false;
        }
    }
    return true;
}

long double numericRealValue(
    const RuntimeNumericElementValue& value) {
    if (!runtimeNumericClassIsInteger(value.numericClass)) {
        return static_cast<long double>(value.real);
    }
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        return static_cast<long double>(
            std::bit_cast<std::int64_t>(value.integerRealBits));
    }
    return static_cast<long double>(value.integerRealBits);
}

bool numericElementIsFinite(
    const RuntimeNumericElementValue& value) {
    return runtimeNumericClassIsInteger(value.numericClass) ||
           (std::isfinite(value.real) &&
            (!value.complex || std::isfinite(value.imaginary)));
}

bool numericElementIsNotNan(
    const RuntimeNumericElementValue& value) {
    return runtimeNumericClassIsInteger(value.numericClass) ||
           (!std::isnan(value.real) &&
            (!value.complex || !std::isnan(value.imaginary)));
}

bool numericElementIsNonzero(
    const RuntimeNumericElementValue& value) {
    if (runtimeNumericClassIsInteger(value.numericClass)) {
        return value.integerRealBits != 0 ||
               (value.complex && value.integerImaginaryBits != 0);
    }
    return value.real != 0.0 ||
           (value.complex && value.imaginary != 0.0);
}

bool numericElementIsInteger(
    const RuntimeNumericElementValue& value) {
    return !value.complex &&
           (runtimeNumericClassIsInteger(value.numericClass) ||
            (std::isfinite(value.real) &&
             std::floor(value.real) == value.real));
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
    const auto realNumeric = [&](auto predicate, std::string message)
        -> std::optional<std::string> {
        if (!isNumeric(value)) {
            return name + " requires numeric data";
        }
        if (value.numericComplex) {
            return "value must be real";
        }
        return allNumeric(value, predicate)
                   ? std::nullopt
                   : std::optional<std::string>(std::move(message));
    };
    if (name == "mustBePositive")
        return realNumeric(
            [](const RuntimeNumericElementValue& x) {
                return numericRealValue(x) > 0.0L;
            },
            "value must be positive");
    if (name == "mustBeNonpositive")
        return realNumeric(
            [](const RuntimeNumericElementValue& x) {
                return numericRealValue(x) <= 0.0L;
            },
            "value must be nonpositive");
    if (name == "mustBeNonnegative")
        return realNumeric(
            [](const RuntimeNumericElementValue& x) {
                return numericRealValue(x) >= 0.0L;
            },
            "value must be nonnegative");
    if (name == "mustBeNegative")
        return realNumeric(
            [](const RuntimeNumericElementValue& x) {
                return numericRealValue(x) < 0.0L;
            },
            "value must be negative");
    if (name == "mustBeFinite")
        return numeric(numericElementIsFinite, "value must be finite");
    if (name == "mustBeNonNan")
        return numeric(numericElementIsNotNan,
                       "value must not contain NaN");
    if (name == "mustBeNonzero")
        return numeric(numericElementIsNonzero, "value must be nonzero");
    if (name == "mustBeInteger")
        return realNumeric(numericElementIsInteger,
                           "value must contain integers");
    if (name == "mustBeReal")
        return isNumeric(value) && !value.numericComplex
                   ? std::nullopt
                   : std::optional<std::string>("value must be real");
    if (name == "mustBeFloat")
        return isNumeric(value) &&
                       runtimeNumericClassIsFloating(value.numericClass)
                   ? std::nullopt
                   : std::optional<std::string>(
                         "value must be a floating-point array");
    if (name == "mustBeNumeric")
        return isNumeric(value) &&
                       value.numericClass != RuntimeNumericClass::Logical
                   ? std::nullopt
                   : std::optional<std::string>("value must be numeric");
    if (name == "mustBeNumericOrLogical")
        return isNumeric(value)
                   ? std::nullopt
                   : std::optional<std::string>(
                         "value must be numeric or logical");
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
    if (name == "mustBeText") return isRuntimeTextValue(value) ? std::nullopt : std::optional<std::string>("value must be text");
    if (name == "mustBeTextScalar") return runtimeTextScalarCodeUnits(value) ? std::nullopt : std::optional<std::string>("value must be a text scalar");
    if (name == "mustBeNonzeroLengthText") {
        const auto text = runtimeTextScalarCodeUnits(value);
        return (text && !text->empty()) ? std::nullopt : std::optional<std::string>("text value must have nonzero length");
    }
    if (name == "mustBeNonmissing") {
        bool present = value.kind != RuntimeValueKind::Missing;
        if (value.kind == RuntimeValueKind::MissingArray) {
            present = count == 0;
        } else if (isNumeric(value)) {
            present = allNumeric(value, numericElementIsNotNan);
        } else if (isRuntimeStringArray(value)) {
            present = std::none_of(
                value.stringElements.begin(), value.stringElements.end(),
                [](const RuntimeStringElement& element) {
                    return element.missing;
                });
        }
        return present ? std::nullopt : std::optional<std::string>("value must not be missing");
    }
    if (name == "mustBeGreaterThan" || name == "mustBeLessThan" ||
        name == "mustBeGreaterThanOrEqual" ||
        name == "mustBeLessThanOrEqual") {
        if (!isNumeric(value)) return name + " requires numeric data";
        if (value.numericComplex) return "value must be real";
        const auto limit = comparisonValue(validator);
        if (!limit) return name + " requires one literal comparison value";
        const long double threshold = static_cast<long double>(*limit);
        bool valid = false;
        if (name == "mustBeGreaterThan")
            valid = allNumeric(value, [&](const auto& x) {
                return numericRealValue(x) > threshold;
            });
        else if (name == "mustBeLessThan")
            valid = allNumeric(value, [&](const auto& x) {
                return numericRealValue(x) < threshold;
            });
        else if (name == "mustBeGreaterThanOrEqual")
            valid = allNumeric(value, [&](const auto& x) {
                return numericRealValue(x) >= threshold;
            });
        else
            valid = allNumeric(value, [&](const auto& x) {
                return numericRealValue(x) <= threshold;
            });
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

RuntimeInvocationNormalizationResult normalizeRuntimeInvocationArguments(
    const FunctionSignature& signature,
    const std::vector<std::string>& nameValueDeclarations,
    const std::vector<RuntimeValue>& arguments) {
    const auto fields = collectNameValueFields(nameValueDeclarations);
    if (fields.empty() &&
        !functionHasNameValueParameters(signature)) {
        const auto status =
            functionPositionalArgumentCountStatus(signature, arguments.size());
        if (status != FunctionArgumentCountStatus::Valid) {
            return normalizationFailure(
                positionalCountError(signature, arguments.size()));
        }
        RuntimeInvocationNormalizationResult result;
        result.succeeded = true;
        result.positionalArguments = arguments;
        result.positionalArgumentCount = arguments.size();
        return result;
    }

    const size_t required =
        functionRequiredPositionalParameterCount(signature);
    std::optional<size_t> firstWrapper;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (isNameValueWrapper(arguments[index])) {
            firstWrapper = index;
            break;
        }
    }

    const size_t scanEnd = firstWrapper.value_or(arguments.size());
    for (size_t index = required; index < scanEnd; ++index) {
        const auto name = runtimeTextScalarUtf8(arguments[index]);
        if (!positionalPrefixIsValid(signature, index) || !name) {
            continue;
        }
        const NameMatch match =
            resolveNameValueName(fields, *name);
        if (match.kind != NameMatchKind::None) {
            return parseNameValueTail(signature, fields, arguments, index);
        }
    }

    if (firstWrapper) {
        return parseNameValueTail(signature, fields, arguments,
                                  *firstWrapper);
    }

    const auto positionalStatus =
        functionPositionalArgumentCountStatus(signature, arguments.size());
    if (positionalStatus == FunctionArgumentCountStatus::Valid) {
        RuntimeInvocationNormalizationResult result;
        result.succeeded = true;
        result.positionalArguments = arguments;
        result.positionalArgumentCount = arguments.size();
        return result;
    }

    for (size_t index = required; index < arguments.size(); ++index) {
        if (positionalPrefixIsValid(signature, index) &&
            runtimeTextScalarUtf8(arguments[index])) {
            return parseNameValueTail(signature, fields, arguments, index);
        }
    }
    return normalizationFailure(
        positionalCountError(signature, arguments.size()));
}

void initializeRuntimeFunctionOutputs(
    RuntimeWorkspace& frame,
    const FunctionSignature& signature) {
    const std::string_view repeatingName =
        functionRepeatingOutputName(signature);
    for (const auto& output : signature.outputs) {
        if (output != repeatingName) {
            frame.try_emplace(output, RuntimeValue{});
        }
    }
    if (!repeatingName.empty()) {
        RuntimeValue repeating;
        repeating.kind = RuntimeValueKind::Cell;
        setRuntimeDimensions(repeating, {1, 0});
        frame[std::string(repeatingName)] = std::move(repeating);
    }
}

RuntimeOutputValidationResult validateRuntimeFunctionOutputs(
    RuntimeWorkspace& frame,
    const std::vector<RuntimeOutputArgumentContract>& contracts,
    const RuntimeArgumentValidationOptions& options) {
    for (const auto& contract : contracts) {
        const auto found = frame.find(contract.name);
        if (found == frame.end() ||
            found->second.kind == RuntimeValueKind::Missing) {
            continue;
        }

        if (!contract.repeating) {
            auto validation = validateRuntimeArgument(
                std::move(found->second), contract.spec, options);
            if (!validation.succeeded) {
                return RuntimeOutputValidationResult{
                    false, contract.name, contract.span,
                    std::move(validation.error)};
            }
            found->second = std::move(validation.value);
            continue;
        }

        if (!isCell(found->second)) {
            return RuntimeOutputValidationResult{
                false, contract.name, contract.span,
                "repeating output must be a Cell"};
        }
        for (size_t index = 0; index < found->second.cells.size(); ++index) {
            auto validation = validateRuntimeArgument(
                std::move(found->second.cells[index]), contract.spec, options);
            if (!validation.succeeded) {
                return RuntimeOutputValidationResult{
                    false,
                    contract.name + "{" + std::to_string(index + 1) + "}",
                    contract.span, std::move(validation.error)};
            }
            found->second.cells[index] = std::move(validation.value);
        }
    }
    return {};
}

std::vector<RuntimeValue> collectRuntimeFunctionOutputs(
    const RuntimeWorkspace& frame,
    const FunctionSignature& signature, size_t requestedOutputCount) {
    std::vector<RuntimeValue> outputs;
    outputs.reserve(requestedOutputCount);
    const size_t fixedCount = functionFixedOutputCount(signature);
    const std::string_view repeatingName =
        functionRepeatingOutputName(signature);
    const auto repeating = repeatingName.empty()
                               ? frame.end()
                               : frame.find(std::string(repeatingName));

    for (size_t index = 0; index < requestedOutputCount; ++index) {
        if (index < fixedCount) {
            const auto value = frame.find(signature.outputs[index]);
            outputs.push_back(value == frame.end() ? RuntimeValue{}
                                                   : value->second);
            continue;
        }
        const size_t repeatingIndex = index - fixedCount;
        outputs.push_back(
            repeating != frame.end() && isCell(repeating->second) &&
                    repeatingIndex < repeating->second.cells.size()
                ? repeating->second.cells[repeatingIndex]
                : RuntimeValue{});
    }
    return outputs;
}

std::vector<std::string> runtimeFunctionOutputNames(
    const FunctionSignature& signature, size_t requestedOutputCount) {
    std::vector<std::string> names;
    names.reserve(requestedOutputCount);
    const size_t fixedCount = functionFixedOutputCount(signature);
    const std::string repeatingName(
        functionRepeatingOutputName(signature));
    for (size_t index = 0; index < requestedOutputCount; ++index) {
        if (index < fixedCount) {
            names.push_back(signature.outputs[index]);
            continue;
        }
        names.push_back(repeatingName +
                        std::to_string(index - fixedCount + 1));
    }
    return names;
}

} // namespace mparser
