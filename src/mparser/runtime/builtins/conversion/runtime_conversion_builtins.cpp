#include "mparser/runtime/builtins/conversion/runtime_conversion_builtins.h"

#include "mparser/embedding/compiled_module.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_execution_control.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mparser {
namespace {

constexpr size_t kMaximumStr2numSourceBytes = 1024U * 1024U;

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
                       "conversion builtin produced an unexpected output "
                       "count",
                       "MParser:ConversionContractViolation");
    }
    return BuiltinResult::success(std::move(outputs));
}

bool executionCheckpoint(const BuiltinCall& call, size_t index) {
    return !call.context || !call.context->executionControl ||
           (index & 255U) != 0U ||
           call.context->executionControl->checkpoint();
}

bool executionControlStopped(const BuiltinCall& call) {
    return call.context && call.context->executionControl &&
           call.context->executionControl->stopReason() !=
               RuntimeExecutionStopReason::None;
}

RuntimeValue emptyDouble() {
    return makeRuntimeMatrixValue(0, 0, {});
}

std::string asciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char value) {
        return value >= 'A' && value <= 'Z'
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    });
    return text;
}

template <typename Value>
std::optional<std::vector<Value>> logicalToStorage(
    const std::vector<size_t>& dimensions,
    std::vector<Value> logical) {
    std::vector<Value> storage(logical.size());
    for (size_t index = 0; index < logical.size(); ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            index, dimensions);
        const auto offset = coordinates
                                ? runtimeRowMajorStorageOffset(
                                      *coordinates, dimensions)
                                : std::nullopt;
        if (!offset || *offset >= storage.size()) {
            return std::nullopt;
        }
        storage[*offset] = std::move(logical[index]);
    }
    return storage;
}

RuntimeValue cellFromLogicalOrder(
    std::vector<size_t> dimensions,
    std::vector<RuntimeValue> logical) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    auto storage = logicalToStorage(dimensions, std::move(logical));
    return storage
               ? makeRuntimeCellValue(std::move(dimensions),
                                      std::move(*storage))
               : makeRuntimeCellValue({0, 0}, {});
}

double numericElementAsDouble(
    const RuntimeNumericElementValue& value, bool imaginary = false) {
    if (runtimeNumericClassIsInteger(value.numericClass) ||
        value.numericClass == RuntimeNumericClass::Logical) {
        const std::uint64_t bits = imaginary
                                       ? value.integerImaginaryBits
                                       : value.integerRealBits;
        if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
            return static_cast<double>(std::bit_cast<std::int64_t>(bits));
        }
        return static_cast<double>(bits);
    }
    return imaginary ? value.imaginary : value.real;
}

std::string exactIntegerText(
    const RuntimeNumericElementValue& value, bool imaginary = false) {
    const std::uint64_t bits = imaginary
                                   ? value.integerImaginaryBits
                                   : value.integerRealBits;
    if (runtimeNumericClassIsSignedInteger(value.numericClass)) {
        return std::to_string(std::bit_cast<std::int64_t>(bits));
    }
    return std::to_string(bits);
}

std::string classPreservingIntegerText(
    const RuntimeNumericElementValue& value) {
    constexpr std::uint64_t kFlintmax = 9007199254740992ULL;
    bool requiresExactLiteral = false;
    if (value.numericClass == RuntimeNumericClass::UInt64) {
        requiresExactLiteral = value.integerRealBits > kFlintmax;
    } else if (value.numericClass == RuntimeNumericClass::Int64) {
        const std::int64_t signedValue =
            std::bit_cast<std::int64_t>(value.integerRealBits);
        requiresExactLiteral =
            signedValue > static_cast<std::int64_t>(kFlintmax) ||
            signedValue < -static_cast<std::int64_t>(kFlintmax);
    }
    if (!requiresExactLiteral) {
        return exactIntegerText(value);
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "0x" << std::hex << std::nouppercase
           << std::setfill('0') << std::setw(16)
           << value.integerRealBits
           << (value.numericClass == RuntimeNumericClass::Int64
                   ? "s64"
                   : "u64");
    return output.str();
}

std::string floatingText(double value, size_t precision) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Inf" : "Inf";
    }
    if (value == 0.0) {
        value = 0.0;
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::defaultfloat
           << std::setprecision(static_cast<int>(std::min<size_t>(
                  precision,
                  static_cast<size_t>(std::numeric_limits<int>::max()))))
           << value;
    return output.str();
}

std::string roundedIntegerText(
    const RuntimeNumericElementValue& element) {
    if (runtimeNumericClassIsInteger(element.numericClass) ||
        element.numericClass == RuntimeNumericClass::Logical) {
        return exactIntegerText(element);
    }
    double value = element.real;
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return std::signbit(value) ? "-Inf" : "Inf";
    }
    value = std::round(value);
    if (value == 0.0) {
        value = 0.0;
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(0) << value;
    return output.str();
}

BuiltinResult int2strBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input)) {
        return failure(call, "int2str expects a numeric input",
                       "MParser:InvalidInt2strInput");
    }
    if (!executionCheckpoint(call, 0)) {
        return failure(call,
                       "int2str was stopped by runtime execution control",
                       "MParser:ExecutionStopped");
    }
    const auto dimensions = runtimeDimensions(input);
    const size_t count = runtimeShapeElementCount(input);
    const size_t rows = dimensions.empty() ? 0 : dimensions.front();
    const size_t columns = rows == 0 ? 0 : count / rows;
    if (rows == 0 || columns == 0) {
        return exactOutputs(
            call, {makeRuntimeCharacterArray({0, 0}, {})});
    }

    std::vector<std::vector<std::string>> text(
        rows, std::vector<std::string>(columns));
    size_t globalWidth = 0;
    size_t firstWidth = 0;
    for (size_t column = 0; column < columns; ++column) {
        for (size_t row = 0; row < rows; ++row) {
            if (!executionCheckpoint(call, row + rows * column)) {
                return failure(call,
                               "int2str was stopped by runtime execution "
                               "control",
                               "MParser:ExecutionStopped");
            }
            const auto element = runtimeNumericElementValue(
                input, row + rows * column);
            if (!element) {
                return failure(call,
                               "int2str could not read a numeric element",
                               "MParser:InvalidInt2strInput");
            }
            text[row][column] = roundedIntegerText(*element);
            const size_t magnitudeWidth =
                !text[row][column].empty() &&
                        text[row][column].front() == '-'
                    ? text[row][column].size() - 1
                    : text[row][column].size();
            globalWidth = std::max(globalWidth, magnitudeWidth);
            if (column == 0) {
                firstWidth = std::max(firstWidth,
                                      text[row][column].size());
            }
        }
    }
    const auto trailingWidth =
        columns > 1
            ? (columns - 1) * (globalWidth + 2)
            : 0;
    if (trailingWidth >
        std::numeric_limits<size_t>::max() - firstWidth) {
        return failure(call, "int2str output is too large",
                       "MParser:InvalidInt2strShape");
    }
    const size_t width = firstWidth + trailingWidth;
    std::u16string storage;
    if (rows > 0 && width >
                        std::numeric_limits<size_t>::max() / rows) {
        return failure(call, "int2str output is too large",
                       "MParser:InvalidInt2strShape");
    }
    storage.reserve(rows * width);
    for (size_t row = 0; row < rows; ++row) {
        const auto appendField = [&storage](const std::string& value,
                                            size_t fieldWidth) {
            storage.append(fieldWidth - value.size(), u' ');
            storage.append(runtimeUtf8ToUtf16(value));
        };
        appendField(text[row][0], firstWidth);
        for (size_t column = 1; column < columns; ++column) {
            appendField(text[row][column], globalWidth + 2);
        }
    }
    return exactOutputs(
        call, {makeRuntimeCharacterArray({rows, width},
                                         std::move(storage))});
}

std::optional<size_t> precisionArgument(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    const auto converted = runtimeNumericElementAsNonnegativeSize(*element);
    return converted && *converted > 0 && *converted <= 1000
               ? converted
               : std::nullopt;
}

std::string complexComponentText(double value, size_t precision) {
    if (std::isnan(value)) {
        return "1i*NaN";
    }
    if (std::isinf(value)) {
        return "1i*Inf";
    }
    return floatingText(value, precision) + "i";
}

std::string numericMat2strElement(
    const RuntimeNumericElementValue& element, size_t precision,
    bool includeClass) {
    if (element.numericClass == RuntimeNumericClass::Logical) {
        return element.integerRealBits == 0 ? "false" : "true";
    }
    if (runtimeNumericClassIsInteger(element.numericClass)) {
        return includeClass ? classPreservingIntegerText(element)
                            : exactIntegerText(element);
    }
    const double real = numericElementAsDouble(element);
    std::string result = floatingText(real, precision);
    if (!element.complex) {
        return result;
    }
    const double imaginary = numericElementAsDouble(element, true);
    const bool negative = std::signbit(imaginary) &&
                          !std::isnan(imaginary);
    result += negative ? '-' : '+';
    result += complexComponentText(
        negative ? -imaginary : imaginary, precision);
    return result;
}

std::string quoteCharacterRow(std::u16string_view row) {
    std::u16string quoted;
    quoted.push_back(u'\'');
    for (const char16_t value : row) {
        quoted.push_back(value);
        if (value == u'\'') {
            quoted.push_back(value);
        }
    }
    quoted.push_back(u'\'');
    return runtimeUtf16ToUtf8(quoted);
}

BuiltinResult mat2strBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!executionCheckpoint(call, 0)) {
        return failure(call,
                       "mat2str was stopped by runtime execution control",
                       "MParser:ExecutionStopped");
    }
    size_t precision = 15;
    if (call.arguments.size() >= 2) {
        const auto parsed = precisionArgument(call.arguments[1]);
        if (!parsed) {
            return failure(call,
                           "mat2str precision must be a positive integer "
                           "scalar",
                           "MParser:InvalidMat2strPrecision");
        }
        precision = *parsed;
    }
    bool includeClass = false;
    if (call.arguments.size() == 3) {
        const auto option = runtimeTextScalarUtf8(call.arguments[2]);
        if (!option || asciiLower(*option) != "class") {
            return failure(call,
                           "mat2str third input must be 'class'",
                           "MParser:InvalidMat2strOption");
        }
        includeClass = true;
    }

    const auto dimensions = runtimeDimensions(input);
    if (dimensions.size() != 2) {
        return failure(call,
                       "mat2str input must be two-dimensional",
                       "MParser:InvalidMat2strShape");
    }
    const size_t rows = dimensions[0];
    const size_t columns = dimensions[1];
    std::string text;
    if (isRuntimeCharacterArray(input)) {
        if (rows == 0 || columns == 0) {
            text = "''";
        } else if (rows == 1) {
            text = quoteCharacterRow(input.characterElements);
        } else {
            text.push_back('[');
            for (size_t row = 0; row < rows; ++row) {
                if (row != 0) {
                    text.push_back(';');
                }
                text += quoteCharacterRow(std::u16string_view(
                    input.characterElements.data() + row * columns,
                    columns));
            }
            text.push_back(']');
        }
        if (includeClass) {
            text = "char(" + text + ')';
        }
        return exactOutputs(
            call, {makeRuntimeCharacterVectorUtf8(text)});
    }
    if (!isRuntimeNumericValue(input)) {
        return failure(call,
                       "mat2str expects a numeric, logical, or character "
                       "array",
                       "MParser:InvalidMat2strInput");
    }

    if (rows == 0 || columns == 0) {
        text = rows == 0 && columns == 0
                   ? "[]"
                   : "zeros(" + std::to_string(rows) + ',' +
                         std::to_string(columns) + ')';
    } else if (rows == 1 && columns == 1) {
        const auto element = runtimeNumericElementValue(input, 0);
        if (!element) {
            return failure(call,
                           "mat2str could not read a numeric element",
                           "MParser:InvalidMat2strInput");
        }
        text = numericMat2strElement(*element, precision,
                                     includeClass);
    } else {
        text.push_back('[');
        for (size_t row = 0; row < rows; ++row) {
            if (row != 0) {
                text.push_back(';');
            }
            for (size_t column = 0; column < columns; ++column) {
                if (!executionCheckpoint(call,
                                         row + rows * column)) {
                    return failure(
                        call,
                        "mat2str was stopped by runtime execution control",
                        "MParser:ExecutionStopped");
                }
                if (column != 0) {
                    text.push_back(' ');
                }
                const auto element = runtimeNumericElementValue(
                    input, row + rows * column);
                if (!element) {
                    return failure(call,
                                   "mat2str could not read a numeric "
                                   "element",
                                   "MParser:InvalidMat2strInput");
                }
                text += numericMat2strElement(*element, precision,
                                              includeClass);
            }
        }
        text.push_back(']');
    }
    if (includeClass) {
        text = std::string(runtimeNumericClassName(input.numericClass)) +
               '(' + text + ')';
    }
    return exactOutputs(
        call, {makeRuntimeCharacterVectorUtf8(text)});
}

std::optional<std::string> str2numSource(const RuntimeValue& value) {
    if (isRuntimeStringScalar(value)) {
        const auto* element = runtimeStringElement(value, 0);
        if (!element || element->missing) {
            return std::nullopt;
        }
        return runtimeUtf16ToUtf8(element->value);
    }
    if (!isRuntimeCharacterArray(value)) {
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
        return std::nullopt;
    }
    std::string source;
    for (size_t row = 0; row < dimensions[0]; ++row) {
        if (row != 0) {
            source.push_back(';');
        }
        source += runtimeUtf16ToUtf8(std::u16string_view(
            value.characterElements.data() + row * dimensions[1],
            dimensions[1]));
    }
    return source;
}

bool safePureDescriptor(const BuiltinDescriptor& descriptor) {
    return descriptor.implementation !=
               BuiltinImplementationKind::Unsupported &&
           descriptor.purity == BuiltinPurity::Pure &&
           descriptor.determinism ==
               BuiltinDeterminism::Deterministic &&
           descriptor.sideEffects == BuiltinSideEffect::None &&
           descriptor.requiredContext ==
               BuiltinContextPermission::None;
}

bool safeStr2numSyntax(const SyntaxNode& node,
                      const BuiltinRegistry& registry) {
    switch (node.kind) {
    case SyntaxKind::CompilationUnit:
    case SyntaxKind::ExpressionStatement:
    case SyntaxKind::ParenthesizedExpr:
    case SyntaxKind::MatrixExpr:
    case SyntaxKind::MatrixRow:
        return std::all_of(
            node.children.begin(), node.children.end(),
            [&registry](const auto& child) {
                return safeStr2numSyntax(*child, registry);
            });
    case SyntaxKind::NumberLiteralExpr:
        return true;
    case SyntaxKind::IdentifierExpr: {
        const std::string name = asciiLower(node.label);
        return name == "i" || name == "j" || name == "inf" ||
               name == "nan" || name == "pi";
    }
    case SyntaxKind::UnaryExpr:
        return (node.label == "+" || node.label == "-") &&
               node.children.size() == 1 &&
               safeStr2numSyntax(*node.children.front(), registry);
    case SyntaxKind::BinaryExpr: {
        static const std::set<std::string, std::less<>> operations{
            "+", "-", "*", "/", "\\", "^", ".*", "./",
            ".\\", ".^", ":", "<", "<=", ">", ">=", "==",
            "~=", "&", "|"};
        return operations.contains(node.label) &&
               node.children.size() == 2 &&
               safeStr2numSyntax(*node.children[0], registry) &&
               safeStr2numSyntax(*node.children[1], registry);
    }
    case SyntaxKind::PostfixExpr:
        return (node.label == "'" || node.label == ".'") &&
               node.children.size() == 1 &&
               safeStr2numSyntax(*node.children.front(), registry);
    case SyntaxKind::CallOrIndexExpr: {
        if (node.children.empty() ||
            node.children.front()->kind != SyntaxKind::IdentifierExpr) {
            return false;
        }
        const BuiltinDescriptor* descriptor = registry.find(
            node.children.front()->label);
        if (!descriptor || !safePureDescriptor(*descriptor)) {
            return false;
        }
        return std::all_of(
            std::next(node.children.begin()), node.children.end(),
            [&registry](const auto& child) {
                return safeStr2numSyntax(*child, registry);
            });
    }
    default:
        return false;
    }
}

bool executionStopped(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.identifier == "MParser:ExecutionStopped" ||
                   diagnostic.identifier ==
                       "MParser:ExecutionInstructionLimit" ||
                   diagnostic.identifier ==
                       "MParser:ExecutionArrayLimit";
        });
}

BuiltinResult str2numBuiltin(const BuiltinCall& call) {
    const auto raw = str2numSource(call.arguments.front());
    if (!raw) {
        return failure(call,
                       "str2num expects a character vector/matrix or string "
                       "scalar",
                       "MParser:InvalidStr2numInput");
    }
    if (raw->size() > kMaximumStr2numSourceBytes) {
        return failure(call, "str2num source exceeds the 1 MiB limit",
                       "MParser:Str2numSourceLimitExceeded");
    }
    if (raw->find_first_not_of(" \t\r\n") == std::string::npos) {
        return exactOutputs(call, {emptyDouble()});
    }
    const std::string expression = '[' + *raw + ']';
    Lexer lexer(expression);
    Parser parser(lexer.lex());
    auto parsed = parser.parse();
    if (!parsed.root || !parsed.diagnostics.empty()) {
        return exactOutputs(call, {emptyDouble()});
    }
    const BuiltinRegistry& registry =
        call.context && call.context->registry
            ? *call.context->registry
            : *defaultBuiltinRegistry();
    if (parsed.root->children.size() != 1 ||
        parsed.root->children.front()->kind !=
            SyntaxKind::ExpressionStatement ||
        !safeStr2numSyntax(*parsed.root, registry)) {
        return exactOutputs(call, {emptyDouble()});
    }

    std::shared_ptr<const BuiltinRegistry> registryOwner;
    if (call.context && call.context->registry) {
        registryOwner = std::shared_ptr<const BuiltinRegistry>(
            call.context->registry,
            [](const BuiltinRegistry*) {});
    } else {
        registryOwner = defaultBuiltinRegistry();
    }
    CompiledModuleCompileOptions compileOptions;
    compileOptions.builtinRegistry = std::move(registryOwner);
    CompiledModule module = CompiledModule::compile(
        std::vector<SourceUnit>{SourceUnit{
            "<str2num>",
            "__mparser_str2num_value = " + expression + ";"}},
        compileOptions);
    if (!module.valid()) {
        return exactOutputs(call, {emptyDouble()});
    }
    BytecodeVmOptions options;
    options.profiling = BytecodeVmProfilingMode::Disabled;
    if (call.context && call.context->executionControl) {
        options.executionControl =
            std::shared_ptr<RuntimeExecutionControl>(
                call.context->executionControl,
                [](RuntimeExecutionControl*) {});
    }
    const auto runtime = module.invoke(options);
    if (hasErrorDiagnostics(runtime.diagnostics)) {
        if (executionStopped(runtime.diagnostics)) {
            return BuiltinResult{false, {}, runtime.diagnostics};
        }
        return exactOutputs(call, {emptyDouble()});
    }
    const auto value = std::find_if(
        runtime.variables.begin(), runtime.variables.end(),
        [](const RuntimeVariable& variable) {
            return variable.name == "__mparser_str2num_value";
        });
    if (value == runtime.variables.end() ||
        !isRuntimeNumericValue(value->value)) {
        return exactOutputs(call, {emptyDouble()});
    }
    return exactOutputs(call, {value->value});
}

std::optional<std::vector<size_t>> num2cellDimensions(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }
    std::vector<size_t> dimensions;
    dimensions.reserve(runtimeShapeElementCount(value));
    for (size_t index = 0; index < runtimeShapeElementCount(value);
         ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element || element->complex) {
            return std::nullopt;
        }
        const auto dimension = runtimeNumericElementAsNonnegativeSize(
            *element);
        if (!dimension || *dimension == 0 ||
            std::find(dimensions.begin(), dimensions.end(),
                      *dimension) != dimensions.end()) {
            return std::nullopt;
        }
        dimensions.push_back(*dimension);
    }
    return dimensions;
}

std::optional<RuntimeValue> scalarElement(
    const RuntimeValue& input, size_t logicalIndex) {
    if (isRuntimeNumericValue(input)) {
        const auto element = runtimeNumericElementValue(input, logicalIndex);
        return element
                   ? runtimeNumericValueFromElements(
                         {1, 1}, {*element}, input.numericClass)
                   : std::nullopt;
    }
    if (isRuntimeCharacterArray(input)) {
        const auto element = runtimeCharacterElement(input, logicalIndex);
        return element
                   ? std::optional<RuntimeValue>(
                         makeRuntimeCharacterArray({1, 1},
                                                   {*element}))
                   : std::nullopt;
    }
    if (isRuntimeStringArray(input)) {
        const auto* element = runtimeStringElement(input, logicalIndex);
        return element
                   ? std::optional<RuntimeValue>(
                         makeRuntimeStringArray({1, 1}, {*element}))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::MissingArray) {
        return logicalIndex < runtimeShapeElementCount(input)
                   ? std::optional<RuntimeValue>(
                         makeRuntimeMissingArrayValue({1, 1}))
                   : std::nullopt;
    }
    return std::nullopt;
}

std::optional<RuntimeValue> sliceValue(
    const BuiltinCall& call, const RuntimeValue& input,
    const std::vector<size_t>& inputDimensions,
    const std::vector<bool>& grouped,
    const std::vector<size_t>& outputCoordinates) {
    std::vector<size_t> sliceDimensions(inputDimensions.size(), 1);
    for (size_t axis = 0; axis < inputDimensions.size(); ++axis) {
        if (grouped[axis]) {
            sliceDimensions[axis] = inputDimensions[axis];
        }
    }
    const auto count = checkedRuntimeDimensionProduct(sliceDimensions);
    if (!count) {
        return std::nullopt;
    }
    std::vector<size_t> inputCoordinates(inputDimensions.size(), 0);
    const auto sourceLogicalIndex = [&](size_t sliceLogicalIndex)
        -> std::optional<size_t> {
        const auto sliceCoordinates = runtimeColumnMajorCoordinates(
            sliceLogicalIndex, sliceDimensions);
        if (!sliceCoordinates) {
            return std::nullopt;
        }
        for (size_t axis = 0; axis < inputDimensions.size(); ++axis) {
            inputCoordinates[axis] = grouped[axis]
                                         ? (*sliceCoordinates)[axis]
                                         : outputCoordinates[axis];
        }
        return runtimeColumnMajorLinearIndex(inputCoordinates,
                                             inputDimensions);
    };

    if (isRuntimeNumericValue(input)) {
        std::vector<RuntimeNumericElementValue> values;
        values.reserve(*count);
        for (size_t index = 0; index < *count; ++index) {
            if (!executionCheckpoint(call, index)) {
                return std::nullopt;
            }
            const auto source = sourceLogicalIndex(index);
            const auto element = source
                                     ? runtimeNumericElementValue(
                                           input, *source)
                                     : std::nullopt;
            if (!element) {
                return std::nullopt;
            }
            values.push_back(*element);
        }
        return runtimeNumericValueFromElements(
            sliceDimensions, std::move(values), input.numericClass);
    }
    if (isRuntimeCharacterArray(input)) {
        std::u16string logical;
        logical.reserve(*count);
        for (size_t index = 0; index < *count; ++index) {
            if (!executionCheckpoint(call, index)) {
                return std::nullopt;
            }
            const auto source = sourceLogicalIndex(index);
            const auto element = source
                                     ? runtimeCharacterElement(input,
                                                               *source)
                                     : std::nullopt;
            if (!element) {
                return std::nullopt;
            }
            logical.push_back(*element);
        }
        std::vector<char16_t> logicalValues(logical.begin(), logical.end());
        auto storage = logicalToStorage(sliceDimensions,
                                        std::move(logicalValues));
        if (!storage) {
            return std::nullopt;
        }
        return makeRuntimeCharacterArray(
            sliceDimensions,
            std::u16string(storage->begin(), storage->end()));
    }
    if (isRuntimeStringArray(input)) {
        std::vector<RuntimeStringElement> logical;
        logical.reserve(*count);
        for (size_t index = 0; index < *count; ++index) {
            if (!executionCheckpoint(call, index)) {
                return std::nullopt;
            }
            const auto source = sourceLogicalIndex(index);
            const auto* element = source
                                      ? runtimeStringElement(input,
                                                             *source)
                                      : nullptr;
            if (!element) {
                return std::nullopt;
            }
            logical.push_back(*element);
        }
        auto storage = logicalToStorage(sliceDimensions,
                                        std::move(logical));
        return storage
                   ? std::optional<RuntimeValue>(makeRuntimeStringArray(
                         sliceDimensions, std::move(*storage)))
                   : std::nullopt;
    }
    if (input.kind == RuntimeValueKind::MissingArray) {
        return makeRuntimeMissingArrayValue(sliceDimensions);
    }
    return std::nullopt;
}

BuiltinResult num2cellBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    if (!isRuntimeNumericValue(input) &&
        !isRuntimeCharacterArray(input) &&
        !isRuntimeStringArray(input) &&
        input.kind != RuntimeValueKind::MissingArray) {
        return failure(call,
                       "num2cell expects a dense numeric, logical, text, or "
                       "missing array",
                       "MParser:InvalidNum2cellInput");
    }
    if (call.arguments.size() == 1) {
        std::vector<RuntimeValue> logical;
        const size_t count = runtimeShapeElementCount(input);
        logical.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            if (!executionCheckpoint(call, index)) {
                return failure(call,
                               "num2cell was stopped by runtime execution "
                               "control",
                               "MParser:ExecutionStopped");
            }
            const auto element = scalarElement(input, index);
            if (!element) {
                return failure(call,
                               "num2cell could not map an input element",
                               "MParser:InvalidNum2cellInput");
            }
            logical.push_back(*element);
        }
        return exactOutputs(
            call, {cellFromLogicalOrder(runtimeDimensions(input),
                                        std::move(logical))});
    }

    const auto groupedDimensions = num2cellDimensions(call.arguments[1]);
    if (!groupedDimensions) {
        return failure(call,
                       "num2cell dimensions must be unique positive integer "
                       "values",
                       "MParser:InvalidNum2cellDimension");
    }
    auto inputDimensions = runtimeDimensions(input);
    size_t rank = inputDimensions.size();
    for (const size_t dimension : *groupedDimensions) {
        rank = std::max(rank, dimension);
    }
    inputDimensions.resize(rank, 1);
    std::vector<bool> grouped(rank, false);
    for (const size_t dimension : *groupedDimensions) {
        grouped[dimension - 1] = true;
    }
    std::vector<size_t> outputDimensions = inputDimensions;
    for (size_t axis = 0; axis < rank; ++axis) {
        if (grouped[axis]) {
            outputDimensions[axis] = 1;
        }
    }
    outputDimensions = normalizeRuntimeDimensions(
        std::move(outputDimensions));
    const auto outputCount = checkedRuntimeDimensionProduct(
        outputDimensions);
    if (!outputCount) {
        return failure(call, "num2cell output shape is too large",
                       "MParser:InvalidNum2cellShape");
    }
    std::vector<RuntimeValue> storage;
    storage.reserve(*outputCount);
    for (size_t offset = 0; offset < *outputCount; ++offset) {
        if (call.context && call.context->executionControl &&
            (offset & 255U) == 0U &&
            !call.context->executionControl->checkpoint()) {
            return failure(call,
                           "num2cell was stopped by runtime execution "
                           "control",
                           "MParser:ExecutionStopped");
        }
        auto coordinates = runtimeRowMajorCoordinates(
            offset, outputDimensions);
        coordinates.resize(rank, 0);
        const auto slice = sliceValue(call, input, inputDimensions,
                                      grouped, coordinates);
        if (!slice) {
            if (executionControlStopped(call)) {
                return failure(call,
                               "num2cell was stopped by runtime execution "
                               "control",
                               "MParser:ExecutionStopped");
            }
            return failure(call,
                           "num2cell could not construct an output slice",
                           "MParser:InvalidNum2cellShape");
        }
        storage.push_back(*slice);
    }
    return exactOutputs(
        call, {makeRuntimeCellValue(outputDimensions,
                                    std::move(storage))});
}

enum class CellBlockKind { Numeric, Character, String, Missing };

struct CellBlockLayout {
    size_t rank = 2;
    std::vector<size_t> cellDimensions;
    std::vector<std::vector<size_t>> axisOffsets;
    std::vector<size_t> logicalOutputDimensions;
    std::vector<size_t> outputDimensions;
    size_t outputCount = 0;
};

std::optional<CellBlockKind> cellBlockKind(const RuntimeValue& value) {
    if (isRuntimeNumericValue(value)) {
        return CellBlockKind::Numeric;
    }
    if (isRuntimeCharacterArray(value)) {
        return CellBlockKind::Character;
    }
    if (isRuntimeStringArray(value)) {
        return CellBlockKind::String;
    }
    if (value.kind == RuntimeValueKind::MissingArray) {
        return CellBlockKind::Missing;
    }
    return std::nullopt;
}

std::optional<CellBlockLayout> buildCellBlockLayout(
    const BuiltinCall& call, const RuntimeValue& cells, size_t rank,
    std::string& error, std::string& identifier) {
    CellBlockLayout layout;
    layout.rank = rank;
    layout.cellDimensions = runtimeDimensions(cells);
    layout.cellDimensions.resize(rank, 1);

    std::vector<std::vector<std::optional<size_t>>> segmentSizes(rank);
    for (size_t axis = 0; axis < rank; ++axis) {
        segmentSizes[axis].resize(layout.cellDimensions[axis]);
    }

    const size_t cellCount = runtimeShapeElementCount(cells);
    for (size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        if (!executionCheckpoint(call, cellIndex)) {
            error = "cell2mat was stopped by runtime execution control";
            identifier = "MParser:ExecutionStopped";
            return std::nullopt;
        }
        const auto coordinates = runtimeColumnMajorCoordinates(
            cellIndex, layout.cellDimensions);
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            cells, cellIndex);
        if (!coordinates || !offset || *offset >= cells.cells.size()) {
            error = "cell2mat could not map a Cell block";
            identifier = "MParser:InvalidCell2matShape";
            return std::nullopt;
        }
        const RuntimeValue& block = cells.cells[*offset];
        auto dimensions = runtimeDimensions(block);
        dimensions.resize(rank, 1);
        const size_t blockCount = runtimeShapeElementCount(block);
        for (size_t axis = 0; axis < rank; ++axis) {
            if (blockCount == 0 && dimensions[axis] == 0) {
                continue;
            }
            auto& segment = segmentSizes[axis][(*coordinates)[axis]];
            if (segment && *segment != dimensions[axis]) {
                error = "cell2mat block dimensions do not form a "
                        "rectangular grid";
                identifier = "MParser:InconsistentCell2matBlock";
                return std::nullopt;
            }
            segment = dimensions[axis];
        }
    }

    layout.axisOffsets.resize(rank);
    layout.logicalOutputDimensions.resize(rank, 0);
    for (size_t axis = 0; axis < rank; ++axis) {
        auto& offsets = layout.axisOffsets[axis];
        offsets.resize(layout.cellDimensions[axis] + 1, 0);
        for (size_t coordinate = 0;
             coordinate < layout.cellDimensions[axis]; ++coordinate) {
            const size_t segment =
                segmentSizes[axis][coordinate].value_or(0);
            if (segment > std::numeric_limits<size_t>::max() -
                              offsets[coordinate]) {
                error = "cell2mat output shape is too large";
                identifier = "MParser:InvalidCell2matShape";
                return std::nullopt;
            }
            offsets[coordinate + 1] = offsets[coordinate] + segment;
        }
        layout.logicalOutputDimensions[axis] = offsets.back();
    }

    for (size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        const auto coordinates = runtimeColumnMajorCoordinates(
            cellIndex, layout.cellDimensions);
        const auto offset = runtimeColumnMajorLinearToStorageOffset(
            cells, cellIndex);
        if (!coordinates || !offset || *offset >= cells.cells.size()) {
            error = "cell2mat could not validate a Cell block";
            identifier = "MParser:InvalidCell2matShape";
            return std::nullopt;
        }
        const RuntimeValue& block = cells.cells[*offset];
        auto dimensions = runtimeDimensions(block);
        dimensions.resize(rank, 1);
        const size_t blockCount = runtimeShapeElementCount(block);
        size_t expectedCount = 1;
        for (size_t axis = 0; axis < rank; ++axis) {
            const auto& offsets = layout.axisOffsets[axis];
            const size_t coordinate = (*coordinates)[axis];
            const size_t segment = offsets[coordinate + 1] -
                                   offsets[coordinate];
            if (expectedCount != 0 &&
                segment > std::numeric_limits<size_t>::max() /
                              expectedCount) {
                error = "cell2mat block shape is too large";
                identifier = "MParser:InvalidCell2matShape";
                return std::nullopt;
            }
            expectedCount *= segment;
            if (blockCount != 0 && dimensions[axis] != segment) {
                error = "cell2mat block dimensions do not form a "
                        "rectangular grid";
                identifier = "MParser:InconsistentCell2matBlock";
                return std::nullopt;
            }
        }
        if (blockCount == 0 ? expectedCount != 0
                            : blockCount != expectedCount) {
            error = "cell2mat empty blocks leave an invalid output grid";
            identifier = "MParser:InconsistentCell2matBlock";
            return std::nullopt;
        }
    }

    const auto outputCount = checkedRuntimeDimensionProduct(
        layout.logicalOutputDimensions);
    if (!outputCount) {
        error = "cell2mat output shape is too large";
        identifier = "MParser:InvalidCell2matShape";
        return std::nullopt;
    }
    layout.outputCount = *outputCount;
    layout.outputDimensions = normalizeRuntimeDimensions(
        layout.logicalOutputDimensions);
    return layout;
}

template <typename Store>
bool mapCellBlocks(
    const BuiltinCall& call, const RuntimeValue& cells,
    const CellBlockLayout& layout, Store store) {
    const size_t cellCount = runtimeShapeElementCount(cells);
    size_t visited = 0;
    for (size_t cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        const auto cellCoordinates = runtimeColumnMajorCoordinates(
            cellIndex, layout.cellDimensions);
        const auto cellOffset = runtimeColumnMajorLinearToStorageOffset(
            cells, cellIndex);
        if (!cellCoordinates || !cellOffset ||
            *cellOffset >= cells.cells.size()) {
            return false;
        }
        const RuntimeValue& block = cells.cells[*cellOffset];
        auto blockDimensions = runtimeDimensions(block);
        blockDimensions.resize(layout.rank, 1);
        const size_t blockCount = runtimeShapeElementCount(block);
        for (size_t blockIndex = 0; blockIndex < blockCount;
             ++blockIndex) {
            if (!executionCheckpoint(call, visited++)) {
                return false;
            }
            const auto blockCoordinates = runtimeColumnMajorCoordinates(
                blockIndex, blockDimensions);
            if (!blockCoordinates) {
                return false;
            }
            std::vector<size_t> outputCoordinates(
                layout.rank, 0);
            for (size_t axis = 0; axis < layout.rank; ++axis) {
                outputCoordinates[axis] =
                    layout.axisOffsets[axis][
                        (*cellCoordinates)[axis]] +
                    (*blockCoordinates)[axis];
            }
            const auto outputIndex = runtimeColumnMajorLinearIndex(
                outputCoordinates, layout.logicalOutputDimensions);
            if (!outputIndex ||
                !store(block, blockIndex, *outputIndex)) {
                return false;
            }
        }
    }
    return true;
}

BuiltinResult cell2matBuiltin(const BuiltinCall& call) {
    const RuntimeValue& cells = call.arguments.front();
    if (cells.kind != RuntimeValueKind::Cell) {
        return failure(call, "cell2mat expects a Cell array",
                       "MParser:InvalidCell2matInput");
    }
    if (!executionCheckpoint(call, 0)) {
        return failure(call,
                       "cell2mat was stopped by runtime execution control",
                       "MParser:ExecutionStopped");
    }
    if (cells.cells.empty()) {
        return exactOutputs(call, {emptyDouble()});
    }
    const auto kind = cellBlockKind(cells.cells.front());
    if (!kind) {
        return failure(call,
                       "cell2mat supports dense numeric, logical, text, and "
                       "missing blocks",
                       "MParser:UnsupportedCell2matBlock");
    }
    size_t rank = runtimeDimensions(cells).size();
    for (const auto& block : cells.cells) {
        if (cellBlockKind(block) != kind) {
            return failure(call,
                           "cell2mat blocks must use one value family",
                           "MParser:InconsistentCell2matBlock");
        }
        rank = std::max(rank, runtimeDimensions(block).size());
    }
    RuntimeNumericClass numericClass =
        cells.cells.front().numericClass;
    for (const auto& block : cells.cells) {
        if (*kind == CellBlockKind::Numeric &&
            block.numericClass != numericClass) {
            return failure(call,
                           "cell2mat numeric blocks must have identical "
                           "numeric classes",
                           "MParser:InconsistentCell2matBlock");
        }
    }
    std::string layoutError;
    std::string layoutIdentifier;
    const auto layout = buildCellBlockLayout(
        call, cells, rank, layoutError, layoutIdentifier);
    if (!layout) {
        return failure(call, std::move(layoutError),
                       std::move(layoutIdentifier));
    }

    if (*kind == CellBlockKind::Missing) {
        return exactOutputs(
            call, {makeRuntimeMissingArrayValue(
                       layout->outputDimensions)});
    }
    if (*kind == CellBlockKind::Numeric) {
        std::vector<RuntimeNumericElementValue> values(
            layout->outputCount);
        const bool mapped = mapCellBlocks(
            call, cells, *layout,
            [&values](const RuntimeValue& block, size_t source,
                      size_t target) {
                const auto element = runtimeNumericElementValue(
                    block, source);
                if (!element || target >= values.size()) {
                    return false;
                }
                values[target] = *element;
                return true;
            });
        const auto output = mapped
                                ? runtimeNumericValueFromElements(
                                      layout->outputDimensions,
                                      std::move(values), numericClass)
                                : std::nullopt;
        if (!output) {
            if (executionControlStopped(call)) {
                return failure(call,
                               "cell2mat was stopped by runtime execution "
                               "control",
                               "MParser:ExecutionStopped");
            }
            return failure(call,
                           "cell2mat could not map numeric blocks",
                           "MParser:InvalidCell2matShape");
        }
        return exactOutputs(call, {*output});
    }
    if (*kind == CellBlockKind::Character) {
        std::vector<char16_t> logical(layout->outputCount);
        const bool mapped = mapCellBlocks(
            call, cells, *layout,
            [&logical](const RuntimeValue& block, size_t source,
                       size_t target) {
                const auto element = runtimeCharacterElement(block,
                                                             source);
                if (!element || target >= logical.size()) {
                    return false;
                }
                logical[target] = *element;
                return true;
            });
        auto storage = mapped
                           ? logicalToStorage(layout->outputDimensions,
                                              std::move(logical))
                           : std::nullopt;
        if (!storage) {
            if (executionControlStopped(call)) {
                return failure(call,
                               "cell2mat was stopped by runtime execution "
                               "control",
                               "MParser:ExecutionStopped");
            }
            return failure(call,
                           "cell2mat could not map character blocks",
                           "MParser:InvalidCell2matShape");
        }
        return exactOutputs(
            call, {makeRuntimeCharacterArray(
                       layout->outputDimensions,
                       std::u16string(storage->begin(), storage->end()))});
    }

    std::vector<RuntimeStringElement> logical(layout->outputCount);
    const bool mapped = mapCellBlocks(
        call, cells, *layout,
        [&logical](const RuntimeValue& block, size_t source,
                   size_t target) {
            const auto* element = runtimeStringElement(block, source);
            if (!element || target >= logical.size()) {
                return false;
            }
            logical[target] = *element;
            return true;
        });
    auto storage = mapped
                       ? logicalToStorage(layout->outputDimensions,
                                          std::move(logical))
                       : std::nullopt;
    if (!storage) {
        if (executionControlStopped(call)) {
            return failure(call,
                           "cell2mat was stopped by runtime execution "
                           "control",
                           "MParser:ExecutionStopped");
        }
        return failure(call, "cell2mat could not map string blocks",
                       "MParser:InvalidCell2matShape");
    }
    return exactOutputs(
        call, {makeRuntimeStringArray(layout->outputDimensions,
                                      std::move(*storage))});
}

BuiltinResult iscellstrBuiltin(const BuiltinCall& call) {
    const RuntimeValue& input = call.arguments.front();
    bool result = input.kind == RuntimeValueKind::Cell;
    if (result) {
        for (size_t index = 0;
             index < runtimeShapeElementCount(input); ++index) {
            if (!executionCheckpoint(call, index)) {
                return failure(
                    call,
                    "iscellstr was stopped by runtime execution control",
                    "MParser:ExecutionStopped");
            }
            const auto offset = runtimeColumnMajorLinearToStorageOffset(
                input, index);
            if (!offset || *offset >= input.cells.size() ||
                !isRuntimeCharacterArray(input.cells[*offset])) {
                result = false;
                break;
            }
        }
    }
    return exactOutputs(call, {makeRuntimeLogicalValue(result)});
}

} // namespace

bool isRuntimeConversionLibraryBuiltin(std::string_view name) {
    return name == "cell2mat" || name == "int2str" ||
           name == "iscellstr" || name == "mat2str" ||
           name == "num2cell" || name == "str2num";
}

BuiltinResult invokeRuntimeConversionLibraryBuiltin(
    std::string_view name, const BuiltinCall& call) {
    if (name == "int2str") {
        return int2strBuiltin(call);
    }
    if (name == "mat2str") {
        return mat2strBuiltin(call);
    }
    if (name == "str2num") {
        return str2numBuiltin(call);
    }
    if (name == "num2cell") {
        return num2cellBuiltin(call);
    }
    if (name == "cell2mat") {
        return cell2matBuiltin(call);
    }
    if (name == "iscellstr") {
        return iscellstrBuiltin(call);
    }
    return failure(call, "unsupported conversion builtin",
                   "MParser:UnsupportedConversionBuiltin");
}

} // namespace mparser
