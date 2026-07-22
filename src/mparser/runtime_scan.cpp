#include "mparser/runtime_scan.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mparser {
namespace {

enum class ScanKind {
    Sum,
    Product,
    Minimum,
    Maximum,
};

enum class MissingPolicy {
    Include,
    Omit,
};

struct ScanOptions {
    std::optional<size_t> dimension;
    bool reverse = false;
    MissingPolicy missingPolicy = MissingPolicy::Include;
    bool directionSpecified = false;
    bool missingPolicySpecified = false;
};

RuntimeScanResult failure(std::string error) {
    RuntimeScanResult result;
    result.error = std::move(error);
    return result;
}

RuntimeScanResult success(std::vector<RuntimeValue> outputs) {
    RuntimeScanResult result;
    result.succeeded = true;
    result.outputs = std::move(outputs);
    return result;
}

std::optional<ScanKind> scanKind(std::string_view name) {
    if (name == "cumsum") {
        return ScanKind::Sum;
    }
    if (name == "cumprod") {
        return ScanKind::Product;
    }
    if (name == "cummin") {
        return ScanKind::Minimum;
    }
    if (name == "cummax") {
        return ScanKind::Maximum;
    }
    return std::nullopt;
}

bool isExtrema(ScanKind kind) {
    return kind == ScanKind::Minimum || kind == ScanKind::Maximum;
}

std::optional<double> scalarNumeric(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericElement(value, 0);
}

std::optional<size_t> positiveDimension(const RuntimeValue& value) {
    const auto raw = scalarNumeric(value);
    const auto parsed =
        raw ? checkedRuntimeNonnegativeInteger(*raw) : std::nullopt;
    if (!parsed || *parsed == 0) {
        return std::nullopt;
    }
    return *parsed - 1;
}

size_t firstNonsingletonDimension(
    const std::vector<size_t>& dimensions) {
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (dimensions[index] != 1) {
            return index;
        }
    }
    return 0;
}

bool parseMissingPolicy(std::string_view text,
                        MissingPolicy& policy) {
    if (text == "omitnan" || text == "omitmissing") {
        policy = MissingPolicy::Omit;
        return true;
    }
    if (text == "includenan" || text == "includemissing") {
        policy = MissingPolicy::Include;
        return true;
    }
    return false;
}

bool parseScanOptions(ScanKind kind,
                      const std::vector<RuntimeValue>& arguments,
                      ScanOptions& options, std::string& error) {
    options.missingPolicy = isExtrema(kind) ? MissingPolicy::Omit
                                             : MissingPolicy::Include;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const RuntimeValue& argument = arguments[index];
        const auto text = runtimeTextScalarUtf8(argument);
        if (!text) {
            if (options.dimension) {
                error = "cumulative dimension was specified more than once";
                return false;
            }
            options.dimension = positiveDimension(argument);
            if (!options.dimension) {
                error = "cumulative dimension must be a positive integer scalar";
                return false;
            }
            continue;
        }

        if (*text == "forward" || *text == "reverse") {
            if (options.directionSpecified) {
                error = "cumulative direction was specified more than once";
                return false;
            }
            options.directionSpecified = true;
            options.reverse = *text == "reverse";
            continue;
        }

        MissingPolicy policy;
        if (parseMissingPolicy(*text, policy)) {
            if (options.missingPolicySpecified) {
                error =
                    "cumulative missing-value policy was specified more than once";
                return false;
            }
            options.missingPolicySpecified = true;
            options.missingPolicy = policy;
            continue;
        }

        error = "unsupported cumulative option: " + *text;
        return false;
    }
    return true;
}

std::optional<std::vector<double>> logicalNumericValues(
    const RuntimeValue& input) {
    const size_t count = runtimeShapeElementCount(input);
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto value = runtimeNumericElement(input, index);
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
    }
    return values;
}

void updateAccumulator(ScanKind kind, MissingPolicy policy,
                       double value, bool& initialized,
                       double& accumulator) {
    const bool missing = std::isnan(value);
    if (kind == ScanKind::Sum || kind == ScanKind::Product) {
        if (missing && policy == MissingPolicy::Omit) {
            return;
        }
        accumulator = kind == ScanKind::Sum ? accumulator + value
                                             : accumulator * value;
        return;
    }

    if (missing) {
        if (policy == MissingPolicy::Include) {
            initialized = true;
            accumulator = std::numeric_limits<double>::quiet_NaN();
        }
        return;
    }
    if (!initialized) {
        initialized = true;
        accumulator = value;
        return;
    }
    if (std::isnan(accumulator)) {
        return;
    }
    const bool better = kind == ScanKind::Minimum
                            ? value < accumulator
                            : value > accumulator;
    if (better) {
        accumulator = value;
    }
}

RuntimeScanResult cumulativeBuiltin(
    ScanKind kind, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (arguments.empty() || arguments.size() > 4) {
        return failure("cumulative operation received an unsupported argument count");
    }
    if (!isRuntimeNumericValue(arguments.front())) {
        return failure("cumulative operation requires a numeric input");
    }
    if (requestedOutputCount > 1) {
        return failure("cumulative operation supports at most one output");
    }

    ScanOptions options;
    std::string error;
    if (!parseScanOptions(kind, arguments, options, error)) {
        return failure(std::move(error));
    }

    const RuntimeValue& input = arguments.front();
    const auto dimensions = runtimeDimensions(input);
    const size_t dimension = options.dimension.value_or(
        firstNonsingletonDimension(dimensions));
    const RuntimeNumericClass outputClass =
        isExtrema(kind) || input.numericClass != RuntimeNumericClass::Logical
            ? input.numericClass
            : RuntimeNumericClass::Double;

    if (dimension >= dimensions.size()) {
        auto unchanged = runtimeConvertNumericClass(input, outputClass);
        if (!unchanged) {
            return failure("cumulative result could not be represented");
        }
        std::vector<RuntimeValue> outputs;
        if (requestedOutputCount != 0) {
            outputs.push_back(std::move(*unchanged));
        }
        return success(std::move(outputs));
    }

    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure("cumulative input dimensions are too large");
    }
    std::vector<double> outputValues(
        *count, std::numeric_limits<double>::quiet_NaN());
    std::vector<size_t> lineDimensions = dimensions;
    const size_t lineLength = lineDimensions[dimension];
    lineDimensions[dimension] = 1;
    const auto lineCount = checkedRuntimeDimensionProduct(lineDimensions);
    if (!lineCount) {
        return failure("cumulative line dimensions are too large");
    }

    for (size_t lineIndex = 0; lineIndex < *lineCount; ++lineIndex) {
        const auto baseCoordinates = runtimeColumnMajorCoordinates(
            lineIndex, lineDimensions);
        if (!baseCoordinates) {
            return failure("cumulative operation could not map an input line");
        }
        auto coordinates = *baseCoordinates;
        bool initialized = !isExtrema(kind);
        double accumulator = kind == ScanKind::Product ? 1.0 : 0.0;
        for (size_t step = 0; step < lineLength; ++step) {
            const size_t position = options.reverse
                                        ? lineLength - step - 1
                                        : step;
            coordinates[dimension] = position;
            const auto logicalIndex = runtimeColumnMajorLinearIndex(
                coordinates, dimensions);
            const auto value = logicalIndex
                                   ? runtimeNumericElement(input,
                                                           *logicalIndex)
                                   : std::nullopt;
            if (!logicalIndex || !value || *logicalIndex >= outputValues.size()) {
                return failure("cumulative operation could not map an element");
            }
            updateAccumulator(kind, options.missingPolicy, *value,
                              initialized, accumulator);
            outputValues[*logicalIndex] =
                initialized ? accumulator
                            : std::numeric_limits<double>::quiet_NaN();
        }
    }

    auto output = runtimeNumericValueFromLogicalOrder(
        dimensions, std::move(outputValues), outputClass);
    if (!output) {
        return failure("cumulative result could not be represented");
    }
    std::vector<RuntimeValue> outputs;
    if (requestedOutputCount != 0) {
        outputs.push_back(std::move(*output));
    }
    return success(std::move(outputs));
}

bool isEmptyNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           runtimeShapeElementCount(value) == 0;
}

RuntimeScanResult differenceBuiltin(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (arguments.empty() || arguments.size() > 3) {
        return failure("diff expects one to three arguments");
    }
    if (!isRuntimeNumericValue(arguments.front())) {
        return failure("diff requires a numeric input");
    }
    if (requestedOutputCount > 1) {
        return failure("diff supports at most one output");
    }

    size_t order = 1;
    if (arguments.size() >= 2 && !isEmptyNumeric(arguments[1])) {
        const auto raw = scalarNumeric(arguments[1]);
        const auto parsed =
            raw ? checkedRuntimeNonnegativeInteger(*raw) : std::nullopt;
        if (!parsed || *parsed == 0) {
            return failure("diff order must be a positive integer scalar or []");
        }
        order = *parsed;
    }

    auto dimensions = runtimeDimensions(arguments.front());
    const size_t dimension =
        arguments.size() == 3
            ? positiveDimension(arguments[2]).value_or(
                  std::numeric_limits<size_t>::max())
            : firstNonsingletonDimension(dimensions);
    if (dimension == std::numeric_limits<size_t>::max()) {
        return failure("diff dimension must be a positive integer scalar");
    }
    dimensions.resize(std::max(dimensions.size(), dimension + 1), 1);

    auto values = logicalNumericValues(arguments.front());
    if (!values) {
        return failure("diff could not read the input array");
    }
    const size_t passCount = std::min(order, dimensions[dimension]);
    for (size_t pass = 0; pass < passCount; ++pass) {
        std::vector<size_t> outputDimensions = dimensions;
        outputDimensions[dimension] =
            dimensions[dimension] == 0 ? 0
                                        : dimensions[dimension] - 1;
        const auto outputCount =
            checkedRuntimeDimensionProduct(outputDimensions);
        if (!outputCount) {
            return failure("diff output dimensions are too large");
        }
        std::vector<double> outputValues(*outputCount);
        for (size_t outputIndex = 0; outputIndex < *outputCount;
             ++outputIndex) {
            const auto lowerCoordinates = runtimeColumnMajorCoordinates(
                outputIndex, outputDimensions);
            if (!lowerCoordinates) {
                return failure("diff could not map an output element");
            }
            auto upperCoordinates = *lowerCoordinates;
            ++upperCoordinates[dimension];
            const auto lowerIndex = runtimeColumnMajorLinearIndex(
                *lowerCoordinates, dimensions);
            const auto upperIndex = runtimeColumnMajorLinearIndex(
                upperCoordinates, dimensions);
            if (!lowerIndex || !upperIndex ||
                *lowerIndex >= values->size() ||
                *upperIndex >= values->size()) {
                return failure("diff could not map an input pair");
            }
            outputValues[outputIndex] =
                (*values)[*upperIndex] - (*values)[*lowerIndex];
        }
        dimensions = std::move(outputDimensions);
        values = std::move(outputValues);
    }

    const RuntimeNumericClass outputClass =
        arguments.front().numericClass == RuntimeNumericClass::Logical
            ? RuntimeNumericClass::Double
            : arguments.front().numericClass;
    auto output = runtimeNumericValueFromLogicalOrder(
        dimensions, std::move(*values), outputClass);
    if (!output) {
        return failure("diff result could not be represented");
    }
    std::vector<RuntimeValue> outputs;
    if (requestedOutputCount != 0) {
        outputs.push_back(std::move(*output));
    }
    return success(std::move(outputs));
}

} // namespace

bool isRuntimeScanBuiltin(std::string_view name) {
    return scanKind(name).has_value() || name == "diff";
}

RuntimeScanResult runtimeScanBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (name == "diff") {
        return differenceBuiltin(arguments, requestedOutputCount);
    }
    const auto kind = scanKind(name);
    if (!kind) {
        return failure("unsupported scan builtin: " + std::string(name));
    }
    return cumulativeBuiltin(*kind, arguments, requestedOutputCount);
}

} // namespace mparser
