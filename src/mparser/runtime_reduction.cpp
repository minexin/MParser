#include "mparser/runtime_reduction.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mparser {
namespace {

enum class ReductionKind {
    Sum,
    Product,
    Mean,
    Minimum,
    Maximum,
    Any,
    All,
};

enum class MissingPolicy {
    Include,
    Omit,
};

struct DimensionSelection {
    bool specified = false;
    bool all = false;
    std::vector<size_t> dimensions;
};

struct ReductionOptions {
    DimensionSelection selection;
    MissingPolicy missingPolicy = MissingPolicy::Include;
    RuntimeNumericClass outputClass = RuntimeNumericClass::Double;
    bool linearIndices = false;
};

struct ReductionBucket {
    double value = 0.0;
    size_t validCount = 0;
    size_t index = 1;
    bool initialized = false;
};

RuntimeReductionResult failure(std::string error) {
    return RuntimeReductionResult{false, {}, std::move(error)};
}

RuntimeReductionResult success(std::vector<RuntimeValue> outputs) {
    return RuntimeReductionResult{true, std::move(outputs), {}};
}

std::optional<ReductionKind> reductionKind(std::string_view name) {
    if (name == "sum") {
        return ReductionKind::Sum;
    }
    if (name == "prod") {
        return ReductionKind::Product;
    }
    if (name == "mean") {
        return ReductionKind::Mean;
    }
    if (name == "min") {
        return ReductionKind::Minimum;
    }
    if (name == "max") {
        return ReductionKind::Maximum;
    }
    if (name == "any") {
        return ReductionKind::Any;
    }
    if (name == "all") {
        return ReductionKind::All;
    }
    return std::nullopt;
}

bool isExtrema(ReductionKind kind) {
    return kind == ReductionKind::Minimum ||
           kind == ReductionKind::Maximum;
}

bool isEmptyNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value) &&
           runtimeShapeElementCount(value) == 0;
}

std::optional<double> scalarNumeric(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    return runtimeNumericElement(value, 0);
}

std::optional<RuntimeValue> numericValueFromLogicalOrder(
    std::vector<size_t> dimensions, std::vector<double> values,
    RuntimeNumericClass numericClass) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != values.size()) {
        return std::nullopt;
    }

    for (double& value : values) {
        const auto converted =
            runtimeCoerceNumericElement(value, numericClass);
        if (!converted) {
            return std::nullopt;
        }
        value = *converted;
    }

    RuntimeValue result;
    result.numericClass = numericClass;
    if (*count == 1) {
        result.kind = RuntimeValueKind::Number;
        result.number = values.front();
        setRuntimeDimensions(result, {1, 1});
        return result;
    }

    result.kind = dimensions.size() == 2 && dimensions[0] == 1
                      ? RuntimeValueKind::Vector
                      : RuntimeValueKind::Matrix;
    result.elements.resize(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, dimensions);
        const auto storageOffset = coordinates
                                       ? runtimeRowMajorStorageOffset(
                                             *coordinates, dimensions)
                                       : std::nullopt;
        if (!storageOffset || *storageOffset >= result.elements.size()) {
            return std::nullopt;
        }
        result.elements[*storageOffset] = values[logicalIndex];
    }
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

bool parsePositiveDimensions(const RuntimeValue& value,
                             std::vector<size_t>& dimensions,
                             std::string& error) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) == 0) {
        error = "reduction dimensions must be positive integers";
        return false;
    }

    const size_t count = runtimeShapeElementCount(value);
    dimensions.clear();
    dimensions.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto raw = runtimeNumericElement(value, index);
        const auto parsed =
            raw ? checkedRuntimeNonnegativeInteger(*raw) : std::nullopt;
        if (!parsed || *parsed == 0) {
            error = "reduction dimensions must be positive integers";
            return false;
        }
        const size_t zeroBased = *parsed - 1;
        if (std::find(dimensions.begin(), dimensions.end(), zeroBased) !=
            dimensions.end()) {
            error = "reduction dimensions must not repeat";
            return false;
        }
        dimensions.push_back(zeroBased);
    }
    std::sort(dimensions.begin(), dimensions.end());
    return true;
}

bool parseDimensionSelection(const RuntimeValue& value,
                             DimensionSelection& selection,
                             std::string& error) {
    if (selection.specified) {
        error = "reduction dimension was specified more than once";
        return false;
    }
    selection.specified = true;
    if (value.kind == RuntimeValueKind::String) {
        if (value.text != "all") {
            error = "reduction dimension string must be \"all\"";
            return false;
        }
        selection.all = true;
        return true;
    }
    return parsePositiveDimensions(value, selection.dimensions, error);
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

bool parseOrdinaryReductionOptions(
    ReductionKind kind, const RuntimeValue& input,
    const std::vector<RuntimeValue>& arguments, ReductionOptions& options,
    std::string& error) {
    bool outputTypeSpecified = false;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const RuntimeValue& argument = arguments[index];
        if (argument.kind != RuntimeValueKind::String) {
            if (!parseDimensionSelection(argument, options.selection,
                                         error)) {
                return false;
            }
            continue;
        }

        if (argument.text == "all") {
            if (!parseDimensionSelection(argument, options.selection,
                                         error)) {
                return false;
            }
            continue;
        }
        if (parseMissingPolicy(argument.text, options.missingPolicy)) {
            if (kind == ReductionKind::Any || kind == ReductionKind::All) {
                error = "any and all do not accept a missing-value flag";
                return false;
            }
            continue;
        }
        if (argument.text == "default" || argument.text == "double" ||
            argument.text == "native") {
            if (kind == ReductionKind::Any || kind == ReductionKind::All) {
                error = "any and all do not accept an output type";
                return false;
            }
            if (outputTypeSpecified) {
                error = "reduction output type was specified more than once";
                return false;
            }
            outputTypeSpecified = true;
            if (argument.text == "native" &&
                kind != ReductionKind::Mean) {
                options.outputClass = input.numericClass;
            }
            continue;
        }
        error = "unsupported reduction option: " + argument.text;
        return false;
    }
    return true;
}

std::vector<size_t> selectedDimensions(
    const std::vector<size_t>& inputDimensions,
    const DimensionSelection& selection) {
    if (selection.all) {
        std::vector<size_t> result(inputDimensions.size());
        for (size_t index = 0; index < result.size(); ++index) {
            result[index] = index;
        }
        return result;
    }
    if (selection.specified) {
        return selection.dimensions;
    }
    for (size_t index = 0; index < inputDimensions.size(); ++index) {
        if (inputDimensions[index] != 1) {
            return {index};
        }
    }
    return {0};
}

double initialValue(ReductionKind kind) {
    if (kind == ReductionKind::Product || kind == ReductionKind::All) {
        return 1.0;
    }
    return 0.0;
}

bool reductionTruth(double value) {
    return value != 0.0;
}

void updateBucket(ReductionBucket& bucket, ReductionKind kind,
                  double value, size_t index, MissingPolicy policy) {
    const bool missing = std::isnan(value);
    if (missing && policy == MissingPolicy::Omit &&
        kind != ReductionKind::Any && kind != ReductionKind::All) {
        return;
    }

    switch (kind) {
    case ReductionKind::Sum:
    case ReductionKind::Mean:
        bucket.value += value;
        ++bucket.validCount;
        return;
    case ReductionKind::Product:
        bucket.value *= value;
        ++bucket.validCount;
        return;
    case ReductionKind::Any:
        bucket.value = bucket.value != 0.0 || reductionTruth(value)
                           ? 1.0
                           : 0.0;
        ++bucket.validCount;
        return;
    case ReductionKind::All:
        bucket.value = bucket.value != 0.0 && reductionTruth(value)
                           ? 1.0
                           : 0.0;
        ++bucket.validCount;
        return;
    case ReductionKind::Minimum:
    case ReductionKind::Maximum:
        break;
    }

    if (!bucket.initialized) {
        bucket.value = value;
        bucket.index = index;
        bucket.initialized = true;
        ++bucket.validCount;
        return;
    }
    if (std::isnan(bucket.value)) {
        return;
    }
    if (missing) {
        bucket.value = value;
        bucket.index = index;
        return;
    }
    const bool better = kind == ReductionKind::Minimum
                            ? value < bucket.value
                            : value > bucket.value;
    if (better) {
        bucket.value = value;
        bucket.index = index;
    }
    ++bucket.validCount;
}

RuntimeReductionResult reduceNumeric(
    ReductionKind kind, const RuntimeValue& input,
    ReductionOptions options, size_t requestedOutputCount) {
    auto inputDimensions = runtimeDimensions(input);
    auto dimensions = selectedDimensions(inputDimensions,
                                         options.selection);
    const size_t requestedRank =
        dimensions.empty() ? inputDimensions.size()
                           : dimensions.back() + 1;
    inputDimensions.resize(
        std::max(inputDimensions.size(), requestedRank), 1);

    if (options.selection.all) {
        dimensions = selectedDimensions(inputDimensions,
                                        options.selection);
    }
    if (isExtrema(kind) && requestedOutputCount > 1 &&
        dimensions.size() > 1 && !options.linearIndices &&
        !options.selection.all) {
        return failure(
            "multi-dimension min/max index output requires \"linear\"");
    }

    std::vector<size_t> outputDimensions = inputDimensions;
    for (const size_t dimension : dimensions) {
        outputDimensions[dimension] =
            isExtrema(kind) && inputDimensions[dimension] == 0 ? 0 : 1;
    }

    const bool defaultZeroByZero =
        !options.selection.specified && inputDimensions.size() == 2 &&
        inputDimensions[0] == 0 && inputDimensions[1] == 0;
    if (defaultZeroByZero && !isExtrema(kind)) {
        outputDimensions = {1, 1};
    }
    if (options.selection.all && isExtrema(kind) &&
        runtimeShapeElementCount(input) == 0) {
        outputDimensions = {0, 0};
    }

    const auto outputCount =
        checkedRuntimeDimensionProduct(outputDimensions);
    if (!outputCount) {
        return failure("reduction output dimensions are too large");
    }

    std::vector<ReductionBucket> buckets(*outputCount);
    for (auto& bucket : buckets) {
        bucket.value = initialValue(kind);
    }

    const size_t inputCount = runtimeShapeElementCount(input);
    for (size_t logicalIndex = 0; logicalIndex < inputCount;
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, inputDimensions);
        const auto value = runtimeNumericElement(input, logicalIndex);
        if (!coordinates || !value) {
            return failure("reduction could not map the input array");
        }

        auto outputCoordinates = *coordinates;
        for (const size_t dimension : dimensions) {
            outputCoordinates[dimension] = 0;
        }
        const auto outputIndex = runtimeColumnMajorLinearIndex(
            outputCoordinates, outputDimensions);
        if (!outputIndex || *outputIndex >= buckets.size()) {
            return failure("reduction could not map the output array");
        }

        size_t reportedIndex = logicalIndex + 1;
        if (!options.linearIndices && !options.selection.all &&
            dimensions.size() == 1) {
            reportedIndex = (*coordinates)[dimensions.front()] + 1;
        }
        updateBucket(buckets[*outputIndex], kind, *value, reportedIndex,
                     options.missingPolicy);
    }

    std::vector<double> values;
    std::vector<double> indices;
    values.reserve(buckets.size());
    indices.reserve(buckets.size());
    for (auto& bucket : buckets) {
        if (kind == ReductionKind::Mean) {
            bucket.value = bucket.validCount == 0
                               ? std::numeric_limits<double>::quiet_NaN()
                               : bucket.value /
                                     static_cast<double>(bucket.validCount);
        } else if (isExtrema(kind) && !bucket.initialized) {
            bucket.value = std::numeric_limits<double>::quiet_NaN();
            bucket.index = 1;
        }
        values.push_back(bucket.value);
        indices.push_back(static_cast<double>(bucket.index));
    }

    RuntimeNumericClass outputClass = options.outputClass;
    if (kind == ReductionKind::Any || kind == ReductionKind::All) {
        outputClass = RuntimeNumericClass::Logical;
    } else if (isExtrema(kind)) {
        outputClass = input.numericClass;
    }
    const auto valueResult = numericValueFromLogicalOrder(
        outputDimensions, std::move(values), outputClass);
    if (!valueResult) {
        return failure("reduction result could not be represented");
    }

    std::vector<RuntimeValue> outputs{*valueResult};
    if (isExtrema(kind) && requestedOutputCount > 1) {
        const auto indexResult = numericValueFromLogicalOrder(
            outputDimensions, std::move(indices),
            RuntimeNumericClass::Double);
        if (!indexResult) {
            return failure("reduction indices could not be represented");
        }
        outputs.push_back(*indexResult);
    }
    outputs.resize(requestedOutputCount);
    return success(std::move(outputs));
}

std::optional<double> expandedNumericElement(
    const RuntimeValue& value,
    const std::vector<size_t>& outputCoordinates) {
    if (value.kind == RuntimeValueKind::Number) {
        return value.number;
    }
    const auto offset = runtimeImplicitExpansionStorageOffset(
        outputCoordinates, runtimeDimensions(value));
    if (!offset || *offset >= value.elements.size()) {
        return std::nullopt;
    }
    return value.elements[*offset];
}

RuntimeReductionResult elementwiseExtrema(
    ReductionKind kind, const RuntimeValue& left,
    const RuntimeValue& right, size_t requestedOutputCount) {
    if (requestedOutputCount > 1) {
        return failure(
            "elementwise min/max supports only one output");
    }
    if (!isRuntimeNumericValue(left) || !isRuntimeNumericValue(right)) {
        return failure("elementwise min/max requires numeric inputs");
    }
    const auto dimensions = runtimeImplicitExpansionDimensions(
        runtimeDimensions(left), runtimeDimensions(right));
    if (!dimensions) {
        return failure(
            "elementwise min/max dimensions are incompatible");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure("elementwise min/max dimensions are too large");
    }

    std::vector<double> values;
    values.reserve(*count);
    for (size_t logicalIndex = 0; logicalIndex < *count;
         ++logicalIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            logicalIndex, *dimensions);
        const auto leftValue = coordinates
                                   ? expandedNumericElement(left, *coordinates)
                                   : std::nullopt;
        const auto rightValue = coordinates
                                    ? expandedNumericElement(right, *coordinates)
                                    : std::nullopt;
        if (!leftValue || !rightValue) {
            return failure("elementwise min/max could not map its inputs");
        }
        values.push_back(kind == ReductionKind::Minimum
                             ? std::fmin(*leftValue, *rightValue)
                             : std::fmax(*leftValue, *rightValue));
    }
    const RuntimeNumericClass outputClass =
        left.numericClass == RuntimeNumericClass::Logical &&
                right.numericClass == RuntimeNumericClass::Logical
            ? RuntimeNumericClass::Logical
            : RuntimeNumericClass::Double;
    const auto result = numericValueFromLogicalOrder(
        *dimensions, std::move(values), outputClass);
    if (!result) {
        return failure("elementwise min/max result could not be represented");
    }
    std::vector<RuntimeValue> outputs;
    if (requestedOutputCount != 0) {
        outputs.push_back(*result);
    }
    return success(std::move(outputs));
}

RuntimeReductionResult extremaBuiltin(
    ReductionKind kind, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (arguments.empty() || arguments.size() > 5) {
        return failure("min/max received an unsupported argument count");
    }
    if (!isRuntimeNumericValue(arguments.front())) {
        return failure("min/max requires a numeric input");
    }
    if (requestedOutputCount > 2) {
        return failure("min/max supports at most two outputs");
    }

    if (arguments.size() == 2 && !isEmptyNumeric(arguments[1])) {
        return elementwiseExtrema(kind, arguments[0], arguments[1],
                                  requestedOutputCount);
    }

    ReductionOptions options;
    options.missingPolicy = MissingPolicy::Omit;
    options.outputClass = arguments.front().numericClass;
    size_t optionBegin = 1;
    if (arguments.size() >= 2) {
        if (!isEmptyNumeric(arguments[1])) {
            return failure(
                "dimension-aware min/max requires [] as its second argument");
        }
        optionBegin = 2;
    }

    std::string error;
    for (size_t index = optionBegin; index < arguments.size(); ++index) {
        const RuntimeValue& argument = arguments[index];
        if (argument.kind != RuntimeValueKind::String) {
            if (!parseDimensionSelection(argument, options.selection,
                                         error)) {
                return failure(std::move(error));
            }
            continue;
        }
        if (argument.text == "all") {
            if (!parseDimensionSelection(argument, options.selection,
                                         error)) {
                return failure(std::move(error));
            }
            continue;
        }
        if (argument.text == "linear") {
            if (options.linearIndices) {
                return failure(
                    "min/max linear index option was specified twice");
            }
            options.linearIndices = true;
            continue;
        }
        if (parseMissingPolicy(argument.text, options.missingPolicy)) {
            continue;
        }
        return failure("unsupported min/max option: " + argument.text);
    }
    return reduceNumeric(kind, arguments.front(), std::move(options),
                         requestedOutputCount);
}

std::vector<size_t> findVectorDimensions(const RuntimeValue& input,
                                         size_t resultCount,
                                         bool linearOutput) {
    if (!linearOutput) {
        return {resultCount, 1};
    }
    const auto dimensions = runtimeDimensions(input);
    if ((input.kind == RuntimeValueKind::Number && resultCount == 0) ||
        (dimensions.size() == 2 && dimensions[0] == 0 &&
         dimensions[1] == 0)) {
        return {0, 0};
    }
    if (dimensions.size() == 2 && dimensions[0] == 1) {
        return {1, resultCount};
    }
    return {resultCount, 1};
}

RuntimeReductionResult findBuiltin(
    const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (arguments.empty() || arguments.size() > 3) {
        return failure("find expects one to three arguments");
    }
    if (!isRuntimeNumericValue(arguments.front())) {
        return failure("find requires a numeric input");
    }
    if (requestedOutputCount > 3) {
        return failure("find supports at most three outputs");
    }

    std::optional<size_t> limit;
    if (arguments.size() >= 2) {
        const auto raw = scalarNumeric(arguments[1]);
        const auto parsed =
            raw ? checkedRuntimeNonnegativeInteger(*raw) : std::nullopt;
        if (!parsed) {
            return failure(
                "find result limit must be a nonnegative integer scalar");
        }
        limit = *parsed;
    }
    bool last = false;
    if (arguments.size() == 3) {
        if (arguments[2].kind != RuntimeValueKind::String ||
            (arguments[2].text != "first" &&
             arguments[2].text != "last")) {
            return failure(
                "find direction must be \"first\" or \"last\"");
        }
        last = arguments[2].text == "last";
    }

    std::vector<size_t> matches;
    const size_t inputCount = runtimeShapeElementCount(arguments.front());
    for (size_t logicalIndex = 0; logicalIndex < inputCount;
         ++logicalIndex) {
        const auto value =
            runtimeNumericElement(arguments.front(), logicalIndex);
        if (!value) {
            return failure("find could not map the input array");
        }
        if (*value != 0.0) {
            matches.push_back(logicalIndex);
        }
    }
    if (limit && *limit < matches.size()) {
        if (last) {
            matches.erase(matches.begin(),
                          matches.end() - static_cast<std::ptrdiff_t>(*limit));
        } else {
            matches.resize(*limit);
        }
    }

    const auto inputDimensions = runtimeDimensions(arguments.front());
    std::vector<double> linearIndices;
    std::vector<double> rowIndices;
    std::vector<double> columnIndices;
    std::vector<double> foundValues;
    linearIndices.reserve(matches.size());
    rowIndices.reserve(matches.size());
    columnIndices.reserve(matches.size());
    foundValues.reserve(matches.size());
    const size_t rowCount = inputDimensions[0];
    for (const size_t logicalIndex : matches) {
        const auto value =
            runtimeNumericElement(arguments.front(), logicalIndex);
        if (!value || rowCount == 0) {
            return failure("find could not map a selected element");
        }
        linearIndices.push_back(static_cast<double>(logicalIndex + 1));
        rowIndices.push_back(
            static_cast<double>(logicalIndex % rowCount + 1));
        columnIndices.push_back(
            static_cast<double>(logicalIndex / rowCount + 1));
        foundValues.push_back(*value);
    }

    const size_t effectiveRequested =
        requestedOutputCount == 0 ? 1 : requestedOutputCount;
    std::vector<RuntimeValue> outputs;
    outputs.reserve(effectiveRequested);
    if (effectiveRequested == 1) {
        const auto result = numericValueFromLogicalOrder(
            findVectorDimensions(arguments.front(), matches.size(), true),
            std::move(linearIndices), RuntimeNumericClass::Double);
        if (!result) {
            return failure("find indices could not be represented");
        }
        outputs.push_back(*result);
    } else {
        const std::vector<size_t> dimensions{matches.size(), 1};
        const auto rows = numericValueFromLogicalOrder(
            dimensions, std::move(rowIndices),
            RuntimeNumericClass::Double);
        const auto columns = numericValueFromLogicalOrder(
            dimensions, std::move(columnIndices),
            RuntimeNumericClass::Double);
        if (!rows || !columns) {
            return failure("find subscripts could not be represented");
        }
        outputs.push_back(*rows);
        outputs.push_back(*columns);
        if (effectiveRequested == 3) {
            const auto values = numericValueFromLogicalOrder(
                dimensions, std::move(foundValues),
                arguments.front().numericClass);
            if (!values) {
                return failure("find values could not be represented");
            }
            outputs.push_back(*values);
        }
    }
    outputs.resize(requestedOutputCount);
    return success(std::move(outputs));
}

} // namespace

bool isRuntimeReductionBuiltin(std::string_view name) {
    return reductionKind(name).has_value() || name == "find";
}

RuntimeReductionResult runtimeReductionBuiltin(
    std::string_view name, const std::vector<RuntimeValue>& arguments,
    size_t requestedOutputCount) {
    if (name == "find") {
        return findBuiltin(arguments, requestedOutputCount);
    }
    const auto kind = reductionKind(name);
    if (!kind) {
        return failure("unsupported reduction builtin: " +
                       std::string(name));
    }
    if (arguments.empty()) {
        return failure(std::string(name) + " expects an input array");
    }
    if (!isRuntimeNumericValue(arguments.front())) {
        return failure(std::string(name) + " requires a numeric input");
    }
    if (isExtrema(*kind)) {
        return extremaBuiltin(*kind, arguments, requestedOutputCount);
    }
    if (requestedOutputCount > 1) {
        return failure(std::string(name) +
                       " supports at most one output");
    }

    ReductionOptions options;
    options.missingPolicy = MissingPolicy::Include;
    options.outputClass = RuntimeNumericClass::Double;
    std::string error;
    if (!parseOrdinaryReductionOptions(*kind, arguments.front(), arguments,
                                       options, error)) {
        return failure(std::move(error));
    }
    return reduceNumeric(*kind, arguments.front(), std::move(options),
                         requestedOutputCount);
}

} // namespace mparser
