#include "mparser/execution/bytecode/vm/bytecode_vm.h"
#include "mparser/semantic/argument_contract.h"
#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/semantic/function_signature.h"
#include "mparser/runtime/builtins/array/runtime_array_ops.h"
#include "mparser/runtime/core/object_model/runtime_argument_validation.h"
#include "mparser/runtime/core/indexing/runtime_assignment.h"
#include "mparser/runtime/core/session/runtime_call_frame.h"
#include "mparser/runtime/core/value/runtime_cell.h"
#include "mparser/runtime/core/session/runtime_exception.h"
#include "mparser/runtime/core/indexing/runtime_index.h"
#include "mparser/runtime/core/indexing/runtime_lvalue.h"
#include "mparser/runtime/core/object_model/runtime_metadata.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_datetime.h"
#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_range.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_sparse.h"
#include "mparser/execution/runtime_source_evaluation.h"
#include "mparser/execution/runtime_source_storage.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"
#include "mparser/runtime/core/value/runtime_value_ops.h"
#include "mparser/runtime/core/session/runtime_warning.h"
#include "mparser/execution/jit/dense_array_region_executor.h"
#include "mparser/execution/jit/typed_ir.h"
#include "mparser/execution/jit/typed_region_executor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace mparser {

class BytecodeVmTrustedAccess {
public:
    static BytecodeRegionContract analyzeRegion(
        const BytecodeRegionAnalyzer& analyzer,
        const BytecodeProgram& program,
        std::string_view candidateKind, size_t sourcePc,
        std::string_view target) {
        return analyzer.analyzeValidated(
            program, candidateKind, sourcePc, target);
    }

    static TypedRegionExecutionResult executeRegion(
        const ScalarTypedRegionExecutor& executor,
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeValue& loopRange,
        const RuntimeWorkspace& variables,
        TypedRegionBackend backend) {
        return executor.executeValidated(
            program, region, loopRange, variables, backend);
    }

    static DenseArrayRegionExecutionResult executeDenseRegion(
        const DenseArrayTypedRegionExecutor& executor,
        const BytecodeProgram& program,
        const BytecodeRegionContract& region,
        const RuntimeWorkspace& variables,
        TypedRegionBackend backend) {
        return executor.executeValidated(
            program, region, variables, backend);
    }
};

namespace {

constexpr size_t kHotLoopThreshold = 10;
constexpr std::string_view kScriptProfileName = "<script>";

std::string_view executionStopIdentifier(
    RuntimeExecutionStopReason reason) {
    switch (reason) {
    case RuntimeExecutionStopReason::Cancelled:
        return "MParser:ExecutionCancelled";
    case RuntimeExecutionStopReason::InstructionLimit:
        return "MParser:InstructionLimitExceeded";
    case RuntimeExecutionStopReason::WallTimeLimit:
        return "MParser:WallTimeLimitExceeded";
    case RuntimeExecutionStopReason::CallDepthLimit:
        return "MParser:CallDepthLimitExceeded";
    case RuntimeExecutionStopReason::ArrayByteLimit:
        return "MParser:ArrayByteLimitExceeded";
    case RuntimeExecutionStopReason::DiagnosticLimit:
        return "MParser:DiagnosticLimitExceeded";
    case RuntimeExecutionStopReason::None:
        break;
    }
    return "MParser:RuntimeStopped";
}

std::string executionStopMessage(
    RuntimeExecutionStopReason reason,
    const RuntimeExecutionLimits& limits) {
    switch (reason) {
    case RuntimeExecutionStopReason::Cancelled:
        return "module execution was cancelled";
    case RuntimeExecutionStopReason::InstructionLimit:
        return "module execution reached the instruction limit of " +
               std::to_string(limits.maxInstructionCount);
    case RuntimeExecutionStopReason::WallTimeLimit:
        return "module execution reached the wall-time limit of " +
               std::to_string(limits.maxWallTime.count()) +
               " nanoseconds";
    case RuntimeExecutionStopReason::CallDepthLimit:
        return "module execution reached the call-depth limit of " +
               std::to_string(limits.maxCallDepth);
    case RuntimeExecutionStopReason::ArrayByteLimit:
        return "module execution exceeded the per-value array-byte "
               "limit of " +
               std::to_string(limits.maxArrayBytes);
    case RuntimeExecutionStopReason::DiagnosticLimit:
        return "module execution exceeded the diagnostic limit of " +
               std::to_string(limits.maxDiagnosticCount);
    case RuntimeExecutionStopReason::None:
        break;
    }
    return "module execution stopped";
}

constexpr std::string_view kEventDataClassName = "event.EventData";
constexpr std::string_view kPropertyEventClassName = "event.PropertyEvent";
constexpr std::string_view kEventListenerClassName = "event.listener";

bool isRuntimeExceptionMethodName(std::string_view name) {
    return name == "addCause" || name == "getReport" ||
           name == "throw" || name == "rethrow" ||
           name == "throwAsCaller" || name == "addCorrection";
}
constexpr std::string_view kPropertyListenerClassName =
    "event.proplistener";
constexpr std::string_view kDynamicPropsClassName = "dynamicprops";
constexpr std::string_view kHeterogeneousClassName =
    "matlab.mixin.Heterogeneous";
constexpr std::string_view kHandleValidityField =
    "__mparser_handle_valid";
constexpr std::string_view kListenerValidityField = "__mparser_valid";
constexpr std::string_view kObjectBeingDestroyedEventName =
    "ObjectBeingDestroyed";
constexpr std::string_view kDynamicPropertyDescriptorPrefix =
    "__mparser_dynamic_property_descriptor::";
constexpr std::string_view kDynamicPropertyValuePrefix =
    "__mparser_dynamic_property_value::";

bool isBuiltinHandleSuperclass(std::string_view name) {
    return name == "handle" || name == kEventDataClassName ||
           name == kDynamicPropsClassName;
}

bool isBuiltinNonExecutableSuperclass(std::string_view name) {
    return isBuiltinHandleSuperclass(name) ||
           name == kHeterogeneousClassName;
}

bool isBuiltinHandleRuntimeClass(std::string_view name) {
    const std::string canonical =
        canonicalRuntimeMetadataClassName(name);
    return canonical == "handle" ||
           canonical == kDynamicPropsClassName ||
           canonical == kEventDataClassName ||
           canonical == kPropertyEventClassName ||
           canonical == kEventListenerClassName ||
           canonical == kPropertyListenerClassName ||
           runtimeMetadataClassIsa(canonical, "handle");
}

bool isBuiltinReflectableClass(std::string_view name) {
    const std::string canonical =
        canonicalRuntimeMetadataClassName(name);
    return canonical == "double" || canonical == "logical" ||
           canonical == kRuntimeDateTimeClassName ||
           canonical == kRuntimeDurationClassName ||
           canonical == "char" || canonical == "string" ||
           canonical == "cell" ||
           canonical == "struct" || canonical == "function_handle" ||
           canonical == "numeric" || canonical == "handle" ||
           canonical == kDynamicPropsClassName ||
           canonical == kEventDataClassName ||
           canonical == kPropertyEventClassName ||
           canonical == kEventListenerClassName ||
           canonical == kPropertyListenerClassName ||
           findRuntimeMetadataTypeDescriptor(canonical) != nullptr;
}

std::pair<std::string, std::string>
splitMetadataIdentity(std::string_view identity) {
    const size_t separator = identity.find('/');
    if (separator == std::string_view::npos) {
        return {std::string(identity), {}};
    }
    return {std::string(identity.substr(0, separator)),
            std::string(identity.substr(separator + 1))};
}

std::string trimAscii(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string lowerAscii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

bool isRuntimeIdentifier(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto identifierStart = [](char character) {
        const unsigned char value =
            static_cast<unsigned char>(character);
        return std::isalpha(value) != 0 || character == '_';
    };
    const auto identifierPart = [](char character) {
        const unsigned char value =
            static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_';
    };
    return identifierStart(name.front()) &&
           std::all_of(name.begin() + 1, name.end(), identifierPart);
}

RuntimeValue missingValue() {
    return makeRuntimeMissingValue();
}

RuntimeValue numberValue(
    double value,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double) {
    return makeRuntimeNumberValue(value, numericClass);
}

RuntimeValue logicalValue(bool value) {
    return makeRuntimeLogicalValue(value);
}

RuntimeValue characterValue(std::string value) {
    return makeRuntimeCharacterVectorUtf8(value);
}

RuntimeValue vectorValue(
    std::vector<double> values,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double) {
    return makeRuntimeVectorValue(std::move(values), numericClass);
}

RuntimeValue matrixValue(size_t rows, size_t columns,
                         std::vector<double> values,
                         RuntimeNumericClass numericClass =
                             RuntimeNumericClass::Double) {
    return makeRuntimeMatrixValue(rows, columns, std::move(values),
                                  numericClass);
}

RuntimeValue cellValue(std::vector<RuntimeValue> values) {
    return makeRuntimeCellValue(std::move(values));
}

RuntimeValue cellValueForShape(size_t rows, size_t columns,
                               std::vector<RuntimeValue> values) {
    RuntimeValue result = cellValue(std::move(values));
    setRuntimeDimensions(result, {rows, columns});
    return result;
}

RuntimeValue cellValueForDimensions(std::vector<size_t> dimensions,
                                    std::vector<RuntimeValue> values) {
    return makeRuntimeCellValue(std::move(dimensions),
                                std::move(values));
}

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isString(const RuntimeValue& value) {
    return runtimeTextScalarCodeUnits(value).has_value();
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
}

bool isFunctionHandle(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::FunctionHandle &&
           value.functionHandle != nullptr;
}

bool isObject(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object;
}

RuntimeValue objectValue(std::string className,
                         std::map<std::string, RuntimeValue> fields,
                         bool handleObject) {
    return makeRuntimeObjectScalar(
        std::move(className), std::move(fields), handleObject);
}

const std::map<std::string, RuntimeValue>& objectFields(
    const RuntimeValue& value) {
    const auto* fields = runtimeObjectFields(value);
    return fields ? *fields : value.fields;
}

bool isNumeric(const RuntimeValue& value) {
    return isRuntimeNumericValue(value);
}

size_t rowCount(const RuntimeValue& value) {
    return runtimeDimension(value, 0);
}

size_t columnCount(const RuntimeValue& value) {
    return runtimeDimension(value, 1);
}

std::string runtimeKindName(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Missing:
    case RuntimeValueKind::MissingArray:
        return "missing";
    case RuntimeValueKind::Number:
        return "number";
    case RuntimeValueKind::CharacterArray:
        return "character_array";
    case RuntimeValueKind::StringArray:
        return "string_array";
    case RuntimeValueKind::Vector:
        return "vector";
    case RuntimeValueKind::Matrix:
        return "matrix";
    case RuntimeValueKind::Cell:
        return "cell";
    case RuntimeValueKind::FunctionHandle:
        return "function_handle";
    case RuntimeValueKind::Struct:
        return "struct";
    case RuntimeValueKind::CommaSeparatedList:
        return "comma_separated_list";
    case RuntimeValueKind::NameValueArgument:
        return "name_value_argument";
    case RuntimeValueKind::Object:
        return "object";
    }
    return "unknown";
}

void observeValue(BytecodeValueObservation& observation,
                  const RuntimeValue& value) {
    const std::string kind = runtimeKindName(value);
    const std::string numericClass =
        isRuntimeNumericValue(value)
            ? std::string(runtimeNumericClassName(value.numericClass))
            : std::string{};
    const bool numericComplex =
        isRuntimeNumericValue(value) && value.numericComplex;
    const size_t rows = rowCount(value);
    const size_t columns = columnCount(value);
    const auto dimensions = runtimeDimensions(value);

    if (observation.observationCount == 0) {
        observation.kind = kind;
        observation.numericClass = numericClass;
        observation.rows = rows;
        observation.columns = columns;
        observation.dimensions = dimensions;
        observation.observationCount = 1;
        observation.stable = true;
        observation.numericComplex = numericComplex;
        return;
    }

    ++observation.observationCount;
    if (!observation.stable) {
        return;
    }

    if (observation.kind != kind ||
        observation.numericClass != numericClass ||
        observation.numericComplex != numericComplex ||
        observation.rows != rows ||
        observation.columns != columns ||
        observation.dimensions != dimensions) {
        observation.kind = "mixed";
        observation.numericClass.clear();
        observation.rows = 0;
        observation.columns = 0;
        observation.dimensions.clear();
        observation.stable = false;
        observation.numericComplex = false;
    }
}

void observeValues(std::vector<BytecodeValueObservation>& observations,
                   const std::vector<RuntimeValue>& values) {
    if (observations.size() < values.size()) {
        observations.resize(values.size());
    }

    for (size_t index = 0; index < values.size(); ++index) {
        observeValue(observations[index], values[index]);
    }

    if (observations.size() > values.size()) {
        for (size_t index = values.size(); index < observations.size();
             ++index) {
            observations[index].stable = false;
            observations[index].kind = "mixed";
            observations[index].numericClass.clear();
            observations[index].dimensions.clear();
            observations[index].numericComplex = false;
        }
    }
}

size_t elementCount(const RuntimeValue& value) {
    return runtimeShapeElementCount(value);
}

RuntimeValue arrayValueForDimensions(std::vector<size_t> dimensions,
                                     std::vector<double> values,
                                     RuntimeNumericClass numericClass =
                                         RuntimeNumericClass::Double) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    RuntimeValue result;
    result.kind = dimensions.size() == 2 && dimensions[0] == 1
                      ? RuntimeValueKind::Vector
                      : RuntimeValueKind::Matrix;
    result.elements = std::move(values);
    result.numericClass = numericClass;
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

RuntimeValue oneBasedIndexRange(size_t length) {
    std::vector<double> values;
    values.reserve(length);
    for (size_t index = 1; index <= length; ++index) {
        values.push_back(static_cast<double>(index));
    }
    return vectorValue(std::move(values));
}

bool truthy(const RuntimeValue& value) {
    return runtimeNumericTruthValue(value).value_or(false);
}

std::optional<double> parseRealNumber(std::string_view text) {
    std::string buffer(text);
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::string decodeStringLiteral(std::string_view text) {
    if (text.size() < 2) {
        return std::string(text);
    }

    const char quote = text.front();
    if ((quote != '\'' && quote != '"') || text.back() != quote) {
        return std::string(text);
    }

    std::string decoded;
    for (size_t index = 1; index + 1 < text.size(); ++index) {
        const char c = text[index];
        if (c == quote && index + 1 < text.size() - 1 &&
            text[index + 1] == quote) {
            decoded.push_back(quote);
            ++index;
            continue;
        }
        decoded.push_back(c);
    }
    return decoded;
}

bool runtimeEqual(const RuntimeValue& left, const RuntimeValue& right) {
    if (isRuntimeTemporalValue(left) || isRuntimeTemporalValue(right)) {
        return runtimeTemporalValuesEqual(left, right);
    }
    if (isNumeric(left) && isNumeric(right)) {
        return runtimeNumericValuesIdentical(left, right);
    }
    if (isString(left) && isString(right)) {
        return runtimeTextPayloadEqual(left, right);
    }
    if (isCell(left) && isCell(right)) {
        if (runtimeDimensions(left) != runtimeDimensions(right) ||
            left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeEqual(left.cells[index], right.cells[index])) {
                return false;
            }
        }
        return true;
    }
    if (isFunctionHandle(left) && isFunctionHandle(right)) {
        return left.functionHandle->identity ==
               right.functionHandle->identity;
    }
    if (left.kind == RuntimeValueKind::NameValueArgument &&
        right.kind == RuntimeValueKind::NameValueArgument) {
        return left.text == right.text && left.cells.size() == 1 &&
               right.cells.size() == 1 &&
               runtimeEqual(left.cells.front(), right.cells.front());
    }
    if (left.kind == RuntimeValueKind::Struct &&
        right.kind == RuntimeValueKind::Struct) {
        if (runtimeDimensions(left) != runtimeDimensions(right) ||
            runtimeStructFieldOrder(left) !=
                runtimeStructFieldOrder(right) ||
            runtimeStructElementCount(left) !=
                runtimeStructElementCount(right)) {
            return false;
        }
        for (size_t offset = 0;
             offset < runtimeStructElementCount(left); ++offset) {
            const auto* leftElement = runtimeStructElement(left, offset);
            const auto* rightElement = runtimeStructElement(right, offset);
            if (!leftElement || !rightElement ||
                leftElement->size() != rightElement->size()) {
                return false;
            }
            for (const auto& [name, value] : *leftElement) {
                const auto other = rightElement->find(name);
                if (other == rightElement->end() ||
                    !runtimeEqual(value, other->second)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (isRuntimeCommaSeparatedList(left) &&
        isRuntimeCommaSeparatedList(right)) {
        if (left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeEqual(left.cells[index], right.cells[index])) {
                return false;
            }
        }
        return true;
    }
    if (isRuntimeMetadataObject(left) ||
        isRuntimeMetadataObject(right)) {
        if (!isRuntimeMetadataObject(left) ||
            !isRuntimeMetadataObject(right) ||
            canonicalRuntimeMetadataClassName(left.className) !=
                canonicalRuntimeMetadataClassName(right.className)) {
            return false;
        }
        if (isRuntimeMetadataScalar(left) ||
            isRuntimeMetadataScalar(right)) {
            return isRuntimeMetadataScalar(left) &&
                   isRuntimeMetadataScalar(right) &&
                   left.text == right.text;
        }
        if (runtimeDimensions(left) != runtimeDimensions(right) ||
            left.cells.size() != right.cells.size()) {
            return false;
        }
        for (size_t index = 0; index < left.cells.size(); ++index) {
            if (!runtimeEqual(left.cells[index], right.cells[index])) {
                return false;
            }
        }
        return true;
    }
    if (isObject(left) && isObject(right)) {
        if (isRuntimeClassObject(left) &&
            isRuntimeClassObject(right) &&
            (!isRuntimeScalarObject(left) ||
             !isRuntimeScalarObject(right))) {
            return runtimeObjectArraysEqual(
                left, right,
                [](const RuntimeValue& leftElement,
                   const RuntimeValue& rightElement) {
                    return runtimeEqual(leftElement, rightElement);
                });
        }
        if (!left.enumerationMemberName.empty() ||
            !right.enumerationMemberName.empty()) {
            return left.className == right.className &&
                   left.enumerationMemberName ==
                       right.enumerationMemberName;
        }
        if (left.handleObject || right.handleObject) {
            return left.handleObject && right.handleObject &&
                   left.sharedFields && right.sharedFields &&
                   left.sharedFields.get() == right.sharedFields.get();
        }
        const auto& leftFields = objectFields(left);
        const auto& rightFields = objectFields(right);
        if (left.className != right.className ||
            left.handleObject != right.handleObject ||
            leftFields.size() != rightFields.size()) {
            return false;
        }
        for (const auto& [name, value] : leftFields) {
            const auto other = rightFields.find(name);
            if (other == rightFields.end() ||
                !runtimeEqual(value, other->second)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

struct StackValue {
    RuntimeValue value;
    bool isBuiltinReference = false;
    std::string builtinName;
    bool isFunctionReference = false;
    std::string functionName;
    bool isClassReference = false;
    std::string className;
    bool isMethodReference = false;
    std::string methodClassName;
    std::string methodName;
    std::string methodDeclaringClass;
    std::optional<RuntimeValue> receiver;
};

struct ForLoopState {
    std::string variable;
    std::vector<RuntimeValue> values;
    size_t nextIndex = 0;
    size_t headerPc = 0;
    BindingRef binding;
};

struct IndexContext {
    RuntimeValue target;
    size_t total = 0;
    size_t position = 0;
    bool hasTarget = false;
};

struct ActiveLvalue {
    std::string rootName;
    BindingRef binding;
    RuntimeLvalueTransaction transaction;
    bool failed = false;

    ActiveLvalue(std::string name, BindingRef rootBinding,
                 RuntimeValue root)
        : rootName(std::move(name)), binding(rootBinding),
          transaction(std::move(root)) {}
};

struct SwitchContext {
    RuntimeValue selector;
    bool matched = false;
    size_t beginPc = 0;
    size_t endPc = 0;
};

struct TryContext {
    size_t diagnosticBase = 0;
    size_t catchTarget = 0;
    std::string catchVariable;
    size_t stackDepth = 0;
    size_t forLoopDepth = 0;
    size_t indexContextDepth = 0;
    size_t lvalueDepth = 0;
    size_t switchContextDepth = 0;
    size_t beginPc = 0;
    size_t endPc = 0;
};

enum class MemberAccessLevel {
    Public,
    Protected,
    Private,
    Immutable,
    ClassList,
};

struct MemberAccessPolicy {
    MemberAccessLevel level = MemberAccessLevel::Public;
    std::vector<std::string> classNames;
    bool selectiveClassList = false;
    bool privateMemberIdentity = false;

    bool operator==(const MemberAccessPolicy& other) const {
        return level == other.level && classNames == other.classNames;
    }
};

struct ArgumentContract {
    std::string name;
    PropertySpec spec;
    SourceSpan span;
    ArgumentBlockKind blockKind = ArgumentBlockKind::Input;
    std::string sourceClass;
    std::string defaultExpression;
    std::vector<std::string> defaultReferencedArguments;
    size_t defaultEntry = 0;
    size_t defaultEnd = 0;
    bool hasDefaultRange = false;
};

struct ValidatedFunctionArguments {
    std::vector<RuntimeValue> values;
    size_t positionalArgumentCount = 0;
};

void collectNameReferences(const HirNode& node,
                           std::vector<std::string>& names) {
    if (node.kind == HirKind::NameRef && !node.label.empty() &&
        std::find(names.begin(), names.end(), node.label) == names.end()) {
        names.push_back(node.label);
    }
    for (const auto& child : node.children) {
        collectNameReferences(*child, names);
    }
}

void collectArgumentContracts(
    const HirNode& function, const ArgumentContractCatalog& catalog,
    std::vector<ArgumentContract>& contracts) {
    const auto resolution = resolveArgumentContracts(function, catalog);
    for (const auto& contract : resolution.contracts) {
        ArgumentContract result;
        result.name = contract.name;
        result.spec = contract.property;
        result.span = contract.span;
        result.blockKind = contract.blockKind;
        if (contract.declaration) {
            result.sourceClass =
                contract.declaration->nameValueSourceClass;
            if (result.spec.hasExplicitDefault &&
                !contract.declaration->children.empty()) {
                const HirNode& expression =
                    *contract.declaration->children.front();
                result.defaultExpression = expression.raw;
                collectNameReferences(
                    expression, result.defaultReferencedArguments);
            }
        }
        contracts.push_back(std::move(result));
    }
}

bool hasArgumentBlock(const HirNode& function, bool input) {
    return std::any_of(
        function.children.begin(), function.children.end(),
        [input](const auto& child) {
            if (child->kind != HirKind::ArgumentBlock) {
                return false;
            }
            const auto kind = child->argumentBlock.kind;
            return input
                       ? kind == ArgumentBlockKind::Input ||
                             kind == ArgumentBlockKind::RepeatingInput
                       : kind == ArgumentBlockKind::Output ||
                             kind == ArgumentBlockKind::RepeatingOutput;
        });
}

struct FunctionInfo {
    std::string name;
    std::string key;
    std::string lexicalParent;
    std::string metadataIdentifier;
    std::string displayName;
    std::string namespaceName;
    std::string fullPath;
    std::string declaringClass;
    FunctionSignature signature;
    std::vector<AttributeSyntax> attributes;
    std::vector<std::string> explicitSuperclassConstructors;
    MemberAccessPolicy access;
    bool staticMethod = false;
    bool abstractMethod = false;
    bool sealedMethod = false;
    bool hidden = false;
    bool hasBody = true;
    bool propertyAccessor = false;
    std::string accessorProperty;
    bool hasInputArgumentBlock = false;
    bool hasOutputArgumentBlock = false;
    bool classDestructor = false;
    bool hasNestedFunctions = false;
    size_t entry = 0;
    size_t end = 0;
    SourceSpan span;
    std::vector<ArgumentContract> argumentContracts;
    std::vector<std::string> captureNames;
};

bool hasDirectNestedFunction(const HirNode& function) {
    return std::any_of(
        function.children.begin(), function.children.end(),
        [](const auto& child) {
            return child->kind == HirKind::Function;
        });
}

struct ReflectedArgument {
    std::string name;
    std::string identifierName;
    std::string groupName;
    const ArgumentContract* contract = nullptr;
    bool required = false;
    bool repeating = false;
    bool nameValue = false;
};

enum class MetafunctionSelectorKind {
    None,
    Arguments,
    ArgumentTypes,
};

struct MetafunctionSelector {
    MetafunctionSelectorKind kind = MetafunctionSelectorKind::None;
    std::vector<RuntimeValue> arguments;
    std::vector<std::string> argumentTypes;
};

using MethodCandidates = std::vector<FunctionInfo>;
using PrivateMethodTable = std::map<std::string, MethodCandidates>;

struct PropertyInfo {
    std::string name;
    std::string declaringClass;
    std::string storageKey;
    PropertySpec spec;
    std::vector<AttributeSyntax> attributes;
    MemberAccessPolicy getAccess;
    MemberAccessPolicy setAccess;
    bool constant = false;
    bool dependent = false;
    bool abortSet = false;
    bool abstractProperty = false;
    bool transient = false;
    bool hidden = false;
    bool getObservable = false;
    bool setObservable = false;
    bool nonCopyable = false;
    bool weakHandle = false;
    double partialMatchPriority = 1.0;
    std::string getterName;
    std::string setterName;
    SourceSpan span;
    size_t initializerEntry = 0;
    size_t initializerEnd = 0;
    bool hasInitializerRange = false;
    bool defaultEvaluationActive = false;
    bool defaultEvaluated = false;
    RuntimeValue defaultValue;
};

using PropertyInfoPtr = std::shared_ptr<PropertyInfo>;
using PropertyCandidates = std::vector<PropertyInfoPtr>;
using PropertyTable = std::map<std::string, PropertyCandidates>;

struct EnumerationMemberInfo {
    std::string name;
    std::string declaringClass;
    std::vector<AttributeSyntax> attributes;
    SourceSpan span;
    int argumentCount = 0;
    size_t initializerEntry = 0;
    size_t initializerEnd = 0;
    bool hasInitializerRange = false;
    bool hidden = false;
    bool evaluationActive = false;
    bool evaluated = false;
    RuntimeValue value;
};

struct EventInfo {
    std::string name;
    std::string declaringClass;
    std::vector<AttributeSyntax> attributes;
    MemberAccessPolicy listenAccess;
    MemberAccessPolicy notifyAccess;
    SourceSpan span;
    bool hidden = false;
};

struct ClassInfo {
    std::string name;
    SourceSpan span;
    std::vector<AttributeSyntax> attributes;
    std::vector<std::string> superclasses;
    bool directHandleClass = false;
    bool handleClass = false;
    bool directHeterogeneousClass = false;
    std::string heterogeneousRoot;
    bool explicitlyAbstract = false;
    bool abstractClass = false;
    bool sealedClass = false;
    bool enumerationClass = false;
    bool hidden = false;
    bool constructOnLoad = false;
    bool handleCompatible = false;
    bool restrictsSubclassing = false;
    std::vector<std::string> allowedSubclasses;
    bool hierarchyValid = true;
    std::map<std::string, PropertyInfoPtr> declaredProperties;
    std::vector<PropertyInfoPtr> declaredPropertyOrder;
    std::map<std::string, EnumerationMemberInfo>
        declaredEnumerationMembers;
    std::vector<std::string> declaredEnumerationOrder;
    std::map<std::string, EventInfo> declaredEvents;
    std::vector<std::string> declaredEventOrder;
    std::map<std::string, FunctionInfo> declaredMethods;
    std::map<std::string, bool> declaredStaticMethods;
    PropertyTable properties;
    std::vector<PropertyInfoPtr> propertyOrder;
    std::map<std::string, FunctionInfo> methods;
    PrivateMethodTable privateMethods;
    std::map<std::string, bool> staticMethods;
    std::map<std::string, FunctionInfo> abstractMethods;
    std::map<std::string, PropertyInfoPtr> abstractProperties;
    std::map<std::string, EventInfo> events;
    std::vector<std::string> eventOrder;
};

enum class ClassResolutionState {
    Unresolved,
    Resolving,
    Resolved,
};

struct ConstructionContext {
    RuntimeValue object;
    std::set<std::string> initializedClasses;
    std::set<std::string> activeClasses;
};

struct ActiveClassFunction {
    std::string className;
    std::string methodName;
    std::string constructorOutput;
    ConstructionContext* construction = nullptr;
};

struct ActiveFunctionFrame {
    std::string key;
    size_t frameIndex = 0;
};

struct SourceCallerOverride {
    size_t parentFrameCount = 0;
    RuntimeWorkspace* workspace = nullptr;
};

struct EventListenerRecord {
    size_t id = 0;
    std::weak_ptr<std::map<std::string, RuntimeValue>> sourceFields;
    std::weak_ptr<std::map<std::string, RuntimeValue>> listenerFields;
    std::shared_ptr<std::map<std::string, RuntimeValue>>
        retainedListenerFields;
    std::string sourceClass;
    std::string eventName;
    std::string propertyStorageKey;
    size_t dynamicPropertyId = 0;
    bool propertyListener = false;
    bool coupled = false;
    bool callbackActive = false;
};

struct PropertyListenerTarget {
    RuntimeValue descriptor;
    std::string name;
    std::string storageKey;
    size_t dynamicPropertyId = 0;
    bool getObservable = false;
    bool setObservable = false;
};

struct DynamicPropertyRecord {
    size_t id = 0;
    std::weak_ptr<std::map<std::string, RuntimeValue>> ownerFields;
    std::weak_ptr<std::map<std::string, RuntimeValue>> descriptorFields;
    std::string ownerClass;
    std::string name;
};

struct ActiveTypedLoopRegion {
    size_t regionId = 0;
    std::string kind;
    std::string target;
    BytecodeRegionContract contract;
};

struct ActiveTypedDenseRegion {
    size_t regionId = 0;
    BytecodeRegionContract contract;
    std::vector<BytecodeTypedIrGuard> guards;
};

StackValue runtimeStackValue(RuntimeValue value) {
    StackValue result;
    result.value = std::move(value);
    return result;
}

StackValue builtinStackValue(
    std::string name,
    std::optional<RuntimeValue> receiver = std::nullopt) {
    StackValue result;
    result.isBuiltinReference = true;
    result.builtinName = std::move(name);
    result.receiver = std::move(receiver);
    return result;
}

StackValue functionStackValue(std::string name) {
    StackValue result;
    result.isFunctionReference = true;
    result.functionName = std::move(name);
    return result;
}

StackValue classStackValue(std::string name) {
    StackValue result;
    result.isClassReference = true;
    result.className = std::move(name);
    return result;
}

StackValue methodStackValue(std::string className, std::string methodName,
                            std::string declaringClass,
                            std::optional<RuntimeValue> receiver = std::nullopt) {
    StackValue result;
    result.isMethodReference = true;
    result.methodClassName = std::move(className);
    result.methodName = std::move(methodName);
    result.methodDeclaringClass = std::move(declaringClass);
    result.receiver = std::move(receiver);
    return result;
}

bool isBoundaryEnter(BytecodeOp op) {
    return op == BytecodeOp::EnterClass ||
           op == BytecodeOp::EnterPropertyInitializer ||
           op == BytecodeOp::EnterEnumerationMemberInitializer ||
           op == BytecodeOp::EnterArgumentDefault ||
           op == BytecodeOp::EnterFunction || op == BytecodeOp::EnterControl;
}

bool isBoundaryLeave(BytecodeOp op) {
    return op == BytecodeOp::LeaveClass ||
           op == BytecodeOp::LeavePropertyInitializer ||
           op == BytecodeOp::LeaveEnumerationMemberInitializer ||
           op == BytecodeOp::LeaveArgumentDefault ||
           op == BytecodeOp::LeaveFunction || op == BytecodeOp::LeaveControl;
}

bool isTopLevelRuntimeOp(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::LoadName:
    case BytecodeOp::LoadLiteral:
    case BytecodeOp::StoreName:
    case BytecodeOp::StoreMember:
    case BytecodeOp::StoreIndex:
    case BytecodeOp::StoreBraceIndex:
    case BytecodeOp::UnaryOp:
    case BytecodeOp::BinaryOp:
    case BytecodeOp::PostfixOp:
    case BytecodeOp::MemberAccess:
    case BytecodeOp::CallOrIndex:
    case BytecodeOp::CallSuperclass:
    case BytecodeOp::BraceIndex:
    case BytecodeOp::MakeMatrix:
    case BytecodeOp::MakeMatrixRow:
    case BytecodeOp::MakeCell:
    case BytecodeOp::MakeCellRow:
    case BytecodeOp::MakeFunctionHandle:
    case BytecodeOp::LoadMetaClass:
    case BytecodeOp::EnterControl:
    case BytecodeOp::SwitchBegin:
    case BytecodeOp::SwitchCase:
    case BytecodeOp::SwitchOtherwise:
    case BytecodeOp::SwitchEnd:
    case BytecodeOp::TryBegin:
    case BytecodeOp::TryEnd:
    case BytecodeOp::Jump:
    case BytecodeOp::JumpIfFalse:
    case BytecodeOp::Break:
    case BytecodeOp::Continue:
    case BytecodeOp::Return:
    case BytecodeOp::ForBegin:
    case BytecodeOp::ForNext:
    case BytecodeOp::CaptureExpression:
    case BytecodeOp::Pop:
    case BytecodeOp::DeclareGlobal:
    case BytecodeOp::DeclarePersistent:
    case BytecodeOp::BeginIndexContext:
    case BytecodeOp::BeginIndexArgument:
    case BytecodeOp::BeginLvalue:
    case BytecodeOp::BeginLvalueIndexContext:
    case BytecodeOp::LvalueDescendMember:
    case BytecodeOp::LvalueDescendIndex:
    case BytecodeOp::LvalueDescendBrace:
    case BytecodeOp::StorePathMember:
    case BytecodeOp::StorePathIndex:
    case BytecodeOp::StorePathBrace:
        return true;
    default:
        return false;
    }
}

class BytecodeVmContext {
public:
    BytecodeVmResult run(const BytecodeProgram& program,
                         const SemanticResult& semantic,
                         const BytecodeTypedIrModule* typedIr,
                         const BytecodeVmOptions& options,
                         bool verifyBytecode) {
        program_ = &program;
        semantic_ = &semantic;
        callableContext_ = options.callableContext
                               ? options.callableContext
                               : makeRuntimeCallableContext();
        sessionState_ = options.sessionState
                            ? options.sessionState
                            : std::make_shared<RuntimeSessionState>();
        executionControl_ = options.executionControl
                                ? options.executionControl
                                : std::make_shared<
                                      RuntimeExecutionControl>();
        executionControlActive_ = executionControl_->active();
        outputEvents_.clear();
        expressionResults_.clear();
        nextConsoleSequence_ = 0;
        runtimeOutputSink_ = [this, external = options.outputSink](
                                 const RuntimeOutputEvent& event) {
            auto recorded = event;
            recorded.sequence = nextConsoleSequence_++;
            outputEvents_.push_back(recorded);
            return !external || external(recorded);
        };
        profilingEnabled_ =
            options.profiling == BytecodeVmProfilingMode::Full;
        typedRegionBackend_ = options.typedRegionBackend;
        typedRegionsEnabled_ = typedIr != nullptr;
        inheritedWorkspaceFrames_.clear();
        for (RuntimeWorkspace* workspace :
             options.inheritedWorkspaceFrames) {
            if (workspace) {
                inheritedWorkspaceFrames_.push_back(workspace);
            }
        }
        inheritedSourceCallables_ = options.inheritedCallables;
        inheritedSourceCallableScopes_ =
            options.inheritedCallableScopes;
        inheritedSourceCallableInvoker_ =
            options.inheritedCallableInvoker;
        inheritedSourceCallableWorkspace_ =
            options.inheritedCallableWorkspace;
        inheritedSourceStorageResolver_ =
            options.inheritedStorageResolver;
        inheritedSourceStorageDeclarer_ =
            options.inheritedStorageDeclarer;
        inheritedSourceStorageClearer_ =
            options.inheritedStorageClearer;
        inheritedSourceStorageWorkspace_ =
            options.inheritedStorageWorkspace;
        requestedEntryFunction_ = options.entryFunction;
        entryArguments_ = options.arguments;
        requestedEntryOutputCount_ = options.requestedOutputCount;
        executedEntryFunction_.clear();
        executedRequestedOutputCount_ = 0;
        entrySignature_.reset();
        entryOutputs_.clear();
        entryOutputNames_.clear();
        diagnostics_ = program.diagnostics;
        BytecodeValidationResult validation;
        if (verifyBytecode) {
            validation = validateBytecodeProgram(program, &semantic);
        }
        diagnostics_.insert(
            diagnostics_.end(),
            std::make_move_iterator(validation.diagnostics.begin()),
            std::make_move_iterator(validation.diagnostics.end()));
        warnings_.clear();
        pendingException_.reset();
        executionStopDiagnosticAdded_ = false;
        exceptionCallerFrames_.clear();
        activeExceptionFunctionNames_.clear();
        scriptModeActive_ = false;
        frames_.clear();
        frames_.push_back(makeRuntimeScriptFrame());
        baseGlobalNames_.clear();
        activePersistentFunctionKeys_.clear();
        activeFunctionFrames_.clear();
        sourceCallerOverrides_.clear();
        activeClassFunctions_.clear();
        activeAnonymousBodyOutputCounts_.clear();
        eventListeners_.clear();
        dynamicProperties_.clear();
        activeDynamicPropertyGetters_.clear();
        activeDynamicPropertySetters_.clear();
        destroyingHandleFields_.clear();
        nextEventListenerId_ = 1;
        nextDynamicPropertyId_ = 1;
        resetProfiling(program.instructions.size());
        initializeWorkspace(options.initialWorkspace);
        if (validation.succeeded) {
            if (semantic.root) {
                argumentContractCatalog_ =
                    buildArgumentContractCatalog(*semantic.root);
            }
            collectFunctionNodes(semantic.root.get());
            collectStructuredControlRanges(program);
            collectFunctionRanges(program);
            configureDeclaredClassMembers();
            resolveClassHierarchies();
            finalizeEnumerationSemantics();
            validateResolvedClassMembers();
            registerWorkspaceDynamicProperties();
            if (typedIr && !typedIr->regions.empty() &&
                executionControl_->
                    requiresInstructionCheckpoints()) {
                executionControl_->
                    markOptimizedExecutionSuppressed();
            } else {
                collectTypedRegions(typedIr);
            }
        }

        const bool scriptMode = validation.succeeded &&
                                requestedEntryFunction_.empty() &&
                                hasTopLevelExecutable(program);
        if (scriptMode && !entryArguments_.empty()) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "script entry does not accept arguments"});
        } else if (scriptMode && requestedEntryOutputCount_.value_or(0) > 0) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "script entry does not declare outputs"});
        } else if (diagnostics_.empty()) {
            const SourceSpan entrySpan =
                program.instructions.empty()
                    ? SourceSpan{}
                    : program.instructions.front().span;
            if (prepareExecutionControl(entrySpan) &&
                enterExecutionCall(entrySpan)) {
                execute(scriptMode);
                leaveExecutionCall();
            }
        }
        finalizeEntryOutputs();

        BytecodeVmResult result;
        auto variables = frames_.front().workspace;
        for (const auto& name : baseGlobalNames_) {
            if (const auto value = sessionState_->findGlobal(name)) {
                variables[name] = *value;
            }
        }
        for (const auto& [name, value] : variables) {
            result.variables.push_back(RuntimeVariable{name, value});
        }
        result.entryFunction = executedEntryFunction_;
        result.outputNames = entryOutputNames_;
        result.outputs = entryOutputs_;
        result.outputEvents = std::move(outputEvents_);
        result.expressionResults = std::move(expressionResults_);
        result.requestedOutputCount = executedRequestedOutputCount_;
        result.diagnostics = std::move(warnings_);
        result.diagnostics.insert(
            result.diagnostics.end(),
            std::make_move_iterator(diagnostics_.begin()),
            std::make_move_iterator(diagnostics_.end()));
        result.executedInstructionCount = executedInstructionCount_;
        result.execution = executionControl_->snapshot();
        if (profilingEnabled_) {
            result.profile = buildProfile();
        }
        for (const auto& [regionId, execution] : typedRegionExecutions_) {
            (void)regionId;
            result.typedRegionExecutions.push_back(execution);
        }
        return result;
    }

private:
    void resetProfiling(size_t instructionCount) {
        if (profilingEnabled_) {
            instructionExecutionCounts_.assign(instructionCount, 0);
        } else {
            instructionExecutionCounts_.clear();
        }
        functionProfiles_.clear();
        loopProfiles_.clear();
        callSiteProfiles_.clear();
        assignmentProfiles_.clear();
        loadProfiles_.clear();
        workspaceInputProfiles_.clear();
        functionEntryProfiles_.clear();
        typedLoopRegions_.clear();
        typedDenseRegions_.clear();
        typedRegionExecutions_.clear();
        functionProfileStack_.clear();
        executedInstructionCount_ = 0;
        currentPc_ = 0;
    }

    BytecodeVmProfile buildProfile() const {
        BytecodeVmProfile profile;
        profile.collected = true;
        profile.hotLoopThreshold = kHotLoopThreshold;

        if (program_) {
            for (size_t pc = 0;
                 pc < instructionExecutionCounts_.size() &&
                 pc < program_->instructions.size();
                 ++pc) {
                const size_t count = instructionExecutionCounts_[pc];
                if (count == 0) {
                    continue;
                }

                const auto& instruction = program_->instructions[pc];
                profile.instructions.push_back(BytecodeInstructionProfile{
                    pc,
                    bytecodeOpName(instruction.op),
                    instruction.operand,
                    instruction.span,
                    count});
            }
        }

        for (const auto& [name, function] : functionProfiles_) {
            profile.functions.push_back(function);
        }

        for (const auto& [pc, loop] : loopProfiles_) {
            BytecodeLoopProfile copy = loop;
            copy.hot = copy.iterationCount >= kHotLoopThreshold ||
                       copy.backedgeCount >= kHotLoopThreshold;
            profile.loops.push_back(std::move(copy));
        }

        for (const auto& [pc, site] : callSiteProfiles_) {
            profile.callSites.push_back(site);
        }

        for (const auto& [pc, assignment] : assignmentProfiles_) {
            profile.assignments.push_back(assignment);
        }

        for (const auto& [pc, load] : loadProfiles_) {
            profile.loads.push_back(load);
        }

        for (const auto& [name, input] : workspaceInputProfiles_) {
            profile.workspaceInputs.push_back(input);
        }

        for (const auto& [name, entry] : functionEntryProfiles_) {
            profile.functionEntries.push_back(entry);
        }

        return profile;
    }

    void trimDiagnosticsToLimit() {
        const size_t limit =
            executionControl_->limits().maxDiagnosticCount;
        if (limit == 0) {
            return;
        }
        if (warnings_.size() > limit) {
            warnings_.resize(limit);
        }
        const size_t remaining =
            limit - std::min(limit, warnings_.size());
        if (diagnostics_.size() > remaining) {
            diagnostics_.resize(remaining);
        }
    }

    void addExecutionStopDiagnostic(const SourceSpan& span) {
        if (executionStopDiagnosticAdded_ ||
            !executionControl_ ||
            executionControl_->stopReason() ==
                RuntimeExecutionStopReason::None) {
            return;
        }
        if (executionControl_->stopReason() ==
            RuntimeExecutionStopReason::DiagnosticLimit) {
            trimDiagnosticsToLimit();
        }
        pendingException_.reset();
        Diagnostic diagnostic{
            span,
            executionStopMessage(
                executionControl_->stopReason(),
                executionControl_->limits()),
            std::string(executionStopIdentifier(
                executionControl_->stopReason()))};
        diagnostic.stack = exceptionFrames(span);
        diagnostics_.push_back(std::move(diagnostic));
        executionStopDiagnosticAdded_ = true;
    }

    bool observeRuntimeState(const SourceSpan& span) {
        if (!executionControlActive_ ||
            !executionControl_->hasArrayByteLimit()) {
            return true;
        }
        const auto observe = [this](const RuntimeValue& value) {
            return executionControl_->observeValue(value);
        };

        for (const auto& frame : frames_) {
            for (const auto& [name, value] : frame.workspace) {
                (void)name;
                if (!observe(value)) {
                    addExecutionStopDiagnostic(span);
                    return false;
                }
            }
        }
        for (const auto& value : stack_) {
            if (!observe(value.value) ||
                (value.receiver && !observe(*value.receiver))) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        for (const auto& loop : forLoopStack_) {
            const size_t bytes =
                loop.values.size() >
                        std::numeric_limits<size_t>::max() /
                            sizeof(double)
                    ? std::numeric_limits<size_t>::max()
                    : loop.values.size() * sizeof(double);
            if (!executionControl_->observeArrayBytes(bytes)) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        for (const auto& context : indexContextStack_) {
            if (!observe(context.target)) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        for (const auto& lvalue : lvalueStack_) {
            if (lvalue &&
                (!observe(lvalue->transaction.root()) ||
                 !observe(lvalue->transaction.current()))) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        for (const auto& context : switchContextStack_) {
            if (!observe(context.selector)) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        if (pendingException_ && !observe(*pendingException_)) {
            addExecutionStopDiagnostic(span);
            return false;
        }
        for (const auto& value : entryArguments_) {
            if (!observe(value)) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        for (const auto& value : entryOutputs_) {
            if (!observe(value)) {
                addExecutionStopDiagnostic(span);
                return false;
            }
        }
        return true;
    }

    bool observeDiagnosticBudget(const SourceSpan& span) {
        if (!executionControlActive_ ||
            !executionControl_->hasDiagnosticLimit()) {
            return true;
        }
        if (executionControl_->observeDiagnosticCount(
                warnings_.size() + diagnostics_.size())) {
            return true;
        }
        addExecutionStopDiagnostic(span);
        return false;
    }

    bool prepareExecutionControl(const SourceSpan& span) {
        if (!executionControlActive_) {
            return true;
        }
        if (!executionControl_->checkpoint()) {
            addExecutionStopDiagnostic(span);
            return false;
        }
        return observeRuntimeState(span) &&
               observeDiagnosticBudget(span);
    }

    bool beforeControlledInstruction(const SourceSpan& span) {
        if (!executionControlActive_) {
            return true;
        }
        if (executionControl_->beforeInstruction()) {
            return true;
        }
        addExecutionStopDiagnostic(span);
        return false;
    }

    bool afterControlledInstruction(const SourceSpan& span) {
        if (!executionControlActive_) {
            return true;
        }
        if (!executionControl_->completeInstruction()) {
            addExecutionStopDiagnostic(span);
            return false;
        }
        return observeRuntimeState(span) &&
               observeDiagnosticBudget(span);
    }

    bool enterExecutionCall(const SourceSpan& span) {
        if (!executionControlActive_) {
            return true;
        }
        if (executionControl_->enterCall()) {
            return true;
        }
        addExecutionStopDiagnostic(span);
        return false;
    }

    void leaveExecutionCall() noexcept {
        if (executionControlActive_) {
            executionControl_->leaveCall();
        }
    }

    void initializeWorkspace(
        const std::vector<RuntimeVariable>& variables) {
        for (const auto& variable : variables) {
            currentFrame()[variable.name] = variable.value;
            if (!profilingEnabled_) {
                continue;
            }
            auto& profile = workspaceInputProfiles_[variable.name];
            profile.name = variable.name;
            observeValue(profile.valueObservation, variable.value);
        }
    }

    void recordInstruction(size_t pc,
                           const BytecodeInstruction& instruction) {
        currentPc_ = pc;
        if (!profilingEnabled_) {
            return;
        }
        if (pc < instructionExecutionCounts_.size()) {
            ++instructionExecutionCounts_[pc];
        }
        if (!functionProfileStack_.empty()) {
            auto& function =
                functionProfiles_[functionProfileStack_.back()];
            ++function.executedInstructionCount;
        }
        (void)instruction;
    }

    void enterFunctionProfile(std::string name, SourceSpan span) {
        if (profilingEnabled_) {
            auto& profile = functionProfiles_[name];
            if (profile.name.empty()) {
                profile.name = name;
                profile.span = span;
            }
            ++profile.callCount;
        }
        functionProfileStack_.push_back(std::move(name));
    }

    void leaveFunctionProfile() {
        if (!functionProfileStack_.empty()) {
            functionProfileStack_.pop_back();
        }
    }

    BytecodeLoopProfile& loopProfile(size_t headerPc,
                                     const BytecodeInstruction& instruction) {
        auto [it, inserted] = loopProfiles_.try_emplace(headerPc);
        auto& profile = it->second;
        if (inserted) {
            profile.headerPc = headerPc;
            profile.span = instruction.span;
        }
        if ((profile.variable.empty() || profile.variable == "<backedge>") &&
            !instruction.operand.empty()) {
            profile.variable = instruction.operand;
        }
        return profile;
    }

    void recordForEntry(const BytecodeInstruction& instruction,
                        size_t valueCount,
                        const RuntimeValue* variableValue) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = loopProfile(currentPc_, instruction);
        ++profile.entryCount;
        if (valueCount > 0) {
            ++profile.iterationCount;
        }
        if (variableValue) {
            observeValue(profile.variableObservation, *variableValue);
        }
    }

    void recordForBackedge(const ForLoopState& state,
                           const BytecodeInstruction& instruction,
                           const RuntimeValue& variableValue) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = loopProfile(state.headerPc, instruction);
        ++profile.backedgeCount;
        ++profile.iterationCount;
        observeValue(profile.variableObservation, variableValue);
    }

    void recordForCompletion(const ForLoopState& state,
                             const BytecodeInstruction& instruction) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = loopProfile(state.headerPc, instruction);
        ++profile.completionCount;
    }

    void recordForBreak(const ForLoopState& state,
                        const BytecodeInstruction& instruction) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = loopProfile(state.headerPc, instruction);
        ++profile.breakCount;
    }

    void recordContinue(const BytecodeInstruction& instruction) {
        if (!profilingEnabled_) {
            return;
        }
        if (forLoopStack_.empty()) {
            return;
        }
        auto& profile =
            loopProfile(forLoopStack_.back().headerPc, instruction);
        ++profile.continueCount;
    }

    void recordGenericBackedge(const BytecodeInstruction& instruction,
                               size_t target) {
        if (!profilingEnabled_) {
            return;
        }
        if (target > currentPc_) {
            return;
        }
        auto& profile = loopProfile(target, instruction);
        if (profile.variable.empty()) {
            profile.variable = "<backedge>";
        }
        ++profile.backedgeCount;
        ++profile.iterationCount;
    }

    BytecodeCallSiteProfile&
    recordCallSite(const BytecodeInstruction& instruction, std::string kind,
                   std::string target) {
        auto& profile = callSiteProfiles_[currentPc_];
        if (profile.kind.empty()) {
            profile.pc = currentPc_;
            profile.kind = std::move(kind);
            profile.target = std::move(target);
            profile.span = instruction.span;
            profile.resultCount = instruction.resultCount;
        }
        ++profile.executionCount;
        return profile;
    }

    void recordAssignment(const BytecodeInstruction& instruction,
                          std::string kind,
                          const RuntimeValue& value) {
        recordAssignmentAt(currentPc_, instruction, std::move(kind), value);
    }

    void recordAssignmentAt(size_t pc,
                            const BytecodeInstruction& instruction,
                            std::string kind,
                            const RuntimeValue& value) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = assignmentProfiles_[pc];
        if (profile.kind.empty()) {
            profile.pc = pc;
            profile.kind = std::move(kind);
            profile.target = instruction.operand;
            profile.span = instruction.span;
            if (!forLoopStack_.empty()) {
                profile.inLoop = true;
                profile.loopHeaderPc = forLoopStack_.back().headerPc;
            }
        } else if (!forLoopStack_.empty()) {
            profile.inLoop = true;
            if (profile.loopHeaderPc == 0) {
                profile.loopHeaderPc = forLoopStack_.back().headerPc;
            }
        }
        ++profile.executionCount;
        observeValue(profile.valueObservation, value);
    }

    void recordLoad(const BytecodeInstruction& instruction,
                    const RuntimeValue& value) {
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = loadProfiles_[currentPc_];
        if (profile.name.empty()) {
            profile.pc = currentPc_;
            profile.name = instruction.operand;
            profile.span = instruction.span;
        }
        observeValue(profile.valueObservation, value);
    }

    const SemanticSourceInfo*
    sourceInfo(const SourceSpan& span) const {
        if (!semantic_ || span.begin.sourceId == kInvalidSourceId ||
            span.begin.sourceId >= semantic_->sources.size()) {
            return nullptr;
        }
        return &semantic_->sources[span.begin.sourceId];
    }

    class ExecutionCallGuard {
    public:
        ExecutionCallGuard(BytecodeVmContext& context,
                           const SourceSpan& callSite)
            : context_(context),
              entered_(
                  context_.enterExecutionCall(callSite)) {}

        ~ExecutionCallGuard() {
            if (entered_) {
                context_.leaveExecutionCall();
            }
        }

        explicit operator bool() const noexcept {
            return entered_;
        }

    private:
        BytecodeVmContext& context_;
        bool entered_ = false;
    };

    class ExceptionFunctionGuard {
    public:
        ExceptionFunctionGuard(BytecodeVmContext& context,
                               std::string name,
                               const SourceSpan& callSite)
            : context_(context), callerPushed_(
                                     context_.enterExceptionFunction(
                                         std::move(name), callSite)) {}

        ~ExceptionFunctionGuard() {
            context_.leaveExceptionFunction(callerPushed_);
        }

    private:
        BytecodeVmContext& context_;
        bool callerPushed_ = false;
    };

    RuntimeExceptionFrame exceptionFrame(
        const SourceSpan& span, std::string name) const {
        RuntimeExceptionFrame frame;
        frame.line = span.begin.line;
        frame.name = std::move(name);
        if (const auto* source = sourceInfo(span)) {
            frame.file = source->name;
        }
        return frame;
    }

    bool enterExceptionFunction(std::string name,
                                const SourceSpan& callSite) {
        const bool hasCaller = scriptModeActive_ ||
                               !activeExceptionFunctionNames_.empty();
        if (hasCaller) {
            exceptionCallerFrames_.push_back(exceptionFrame(
                callSite,
                activeExceptionFunctionNames_.empty()
                    ? std::string("<script>")
                    : activeExceptionFunctionNames_.back()));
        }
        activeExceptionFunctionNames_.push_back(std::move(name));
        return hasCaller;
    }

    void leaveExceptionFunction(bool callerPushed) {
        if (!activeExceptionFunctionNames_.empty()) {
            activeExceptionFunctionNames_.pop_back();
        }
        if (callerPushed && !exceptionCallerFrames_.empty()) {
            exceptionCallerFrames_.pop_back();
        }
    }

    std::vector<RuntimeExceptionFrame>
    exceptionFrames(const SourceSpan& span) const {
        std::vector<RuntimeExceptionFrame> frames;
        frames.push_back(exceptionFrame(
            span, activeExceptionFunctionNames_.empty()
                      ? std::string("<script>")
                      : activeExceptionFunctionNames_.back()));
        for (auto caller = exceptionCallerFrames_.rbegin();
             caller != exceptionCallerFrames_.rend(); ++caller) {
            frames.push_back(*caller);
        }
        return frames;
    }

    SourceSpan exceptionDiagnosticSpan(
        const RuntimeValue& exception, SourceSpan fallback) const {
        const auto frames = runtimeExceptionFrames(exception);
        if (frames.empty()) {
            return fallback;
        }
        const auto& top = frames.front();
        fallback.begin.line = top.line;
        fallback.end.line = top.line;
        fallback.begin.column = 1;
        fallback.end.column = 1;
        if (semantic_) {
            for (size_t sourceId = 0;
                 sourceId < semantic_->sources.size(); ++sourceId) {
                if (semantic_->sources[sourceId].name == top.file) {
                    fallback.begin.sourceId = sourceId;
                    fallback.end.sourceId = sourceId;
                    break;
                }
            }
        }
        return fallback;
    }

    std::string publicFunctionIdentifier(std::string_view name) const {
        if (name.starts_with("$path") || name.starts_with("$private")) {
            const size_t separator = name.find('>');
            if (separator != std::string_view::npos &&
                separator + 1 < name.size()) {
                return std::string(name.substr(separator + 1));
            }
        }
        return std::string(name);
    }

    std::string functionDisplayName(std::string_view identifier) const {
        const size_t local = identifier.find_last_of('>');
        const size_t qualified = identifier.find_last_of('.');
        const size_t separator =
            local != std::string_view::npos ? local : qualified;
        return std::string(separator == std::string_view::npos
                               ? identifier
                               : identifier.substr(separator + 1));
    }

    std::string functionNamespaceName(
        std::string_view identifier) const {
        const size_t local = identifier.find('>');
        const std::string_view owner =
            local == std::string_view::npos ? identifier
                                            : identifier.substr(0, local);
        const size_t dot = owner.find_last_of('.');
        return dot == std::string_view::npos
                   ? std::string{}
                   : std::string(owner.substr(0, dot));
    }

    void populateFunctionMetadata(FunctionInfo& info,
                                  const HirNode* hir) const {
        info.metadataIdentifier = publicFunctionIdentifier(info.name);
        info.displayName = functionDisplayName(info.metadataIdentifier);
        if (const auto* source = sourceInfo(info.span)) {
            info.namespaceName = source->namespaceName;
            info.fullPath = source->name;
        }
        if (info.namespaceName.empty()) {
            info.namespaceName =
                functionNamespaceName(info.metadataIdentifier);
        }
        if (hir) {
            info.hasInputArgumentBlock = hasArgumentBlock(*hir, true);
            info.hasOutputArgumentBlock = hasArgumentBlock(*hir, false);
        }
    }

    void registerFunctionMetadata(const FunctionInfo& info) {
        if (info.metadataIdentifier.empty() ||
            ambiguousFunctionMetadataIdentifiers_.contains(
                info.metadataIdentifier)) {
            return;
        }
        const auto [existing, inserted] =
            functionsByMetadataIdentifier_.try_emplace(
                info.metadataIdentifier, info.name);
        if (!inserted && existing->second != info.name) {
            functionsByMetadataIdentifier_.erase(existing);
            ambiguousFunctionMetadataIdentifiers_.insert(
                info.metadataIdentifier);
        }
    }

    void collectFunctionNodes(const HirNode* node, std::string className = {},
                              bool staticMethodBlock = false,
                              std::string lexicalParent = {}) {
        if (!node) {
            return;
        }

        if (node->kind == HirKind::Class) {
            className = node->label;
            auto& info = classesByName_[className];
            info.name = className;
            info.span = node->span;
            info.attributes = node->attributes;
            info.superclasses = node->superclasses;
            info.directHandleClass = std::any_of(
                node->superclasses.begin(), node->superclasses.end(),
                [](const std::string& superclass) {
                    return isBuiltinHandleSuperclass(superclass);
                });
            info.directHeterogeneousClass = std::find(
                node->superclasses.begin(), node->superclasses.end(),
                kHeterogeneousClassName) != node->superclasses.end();
        }
        if (node->kind == HirKind::Property && !className.empty()) {
            auto& klass = classesByName_[className];
            if (klass.declaredProperties.contains(node->label)) {
                diagnostics_.push_back(Diagnostic{
                    node->span,
                    "duplicate property declaration: " + className + "." +
                        node->label});
            } else {
                auto property = std::make_shared<PropertyInfo>();
                property->name = node->label;
                property->declaringClass = className;
                property->storageKey = className + "::" + node->label;
                property->spec = node->property;
                property->attributes = node->attributes;
                property->span = node->span;
                klass.declaredProperties[node->label] = property;
                klass.declaredPropertyOrder.push_back(std::move(property));
            }
        }
        if (node->kind == HirKind::Event && !className.empty()) {
            auto& klass = classesByName_[className];
            EventInfo event;
            event.name = node->label;
            event.declaringClass = className;
            event.attributes = node->attributes;
            event.span = node->span;
            if (!klass.declaredEvents
                     .try_emplace(node->label, std::move(event))
                     .second) {
                diagnostics_.push_back(Diagnostic{
                    node->span,
                    "duplicate event declaration: " + className + "." +
                        node->label});
            } else {
                klass.declaredEventOrder.push_back(node->label);
            }
        }
        if (node->kind == HirKind::EnumerationMember &&
            !className.empty()) {
            auto& klass = classesByName_[className];
            klass.enumerationClass = true;
            EnumerationMemberInfo member;
            member.name = node->label;
            member.declaringClass = className;
            member.attributes = node->attributes;
            member.span = node->span;
            member.argumentCount = static_cast<int>(node->children.size());
            if (!klass.declaredEnumerationMembers
                     .try_emplace(node->label, std::move(member))
                     .second) {
                diagnostics_.push_back(Diagnostic{
                    node->span,
                    "duplicate enumeration member declaration: " +
                        className + "." + node->label});
            } else {
                klass.declaredEnumerationOrder.push_back(node->label);
            }
        }
        for (const auto& attribute : node->attributes) {
            if (attribute.name == "Static") {
                staticMethodBlock = !attribute.negated;
            }
        }
        std::string childLexicalParent = lexicalParent;
        if (node->kind == HirKind::Function) {
            const std::string key =
                !lexicalParent.empty()
                    ? lexicalParent + ">" + node->label
                    : !className.empty()
                          ? className + "." + node->label
                          : node->label;
            functionNodes_[key] = node;
            childLexicalParent = key;
            if (!className.empty() && lexicalParent.empty()) {
                const std::string methodKey =
                    className + "." + node->label;
                if (!classFunctionNodes_.try_emplace(methodKey, node).second) {
                    diagnostics_.push_back(Diagnostic{
                        node->span,
                        "duplicate method declaration: " + methodKey});
                }
                classesByName_[className].declaredStaticMethods[node->label] =
                    staticMethodBlock;
            }
        }
        if (node->kind == HirKind::MethodPrototype && !className.empty()) {
            FunctionInfo info;
            info.name = node->label;
            info.declaringClass = className;
            info.signature = parseFunctionSignature(*node);
            info.attributes = node->attributes;
            info.staticMethod = staticMethodBlock;
            info.hasBody = false;
            info.span = node->span;
            populateFunctionMetadata(info, node);

            auto& klass = classesByName_[className];
            if (info.name.empty()) {
                diagnostics_.push_back(Diagnostic{
                    node->span,
                    "method prototype requires a method name in class: " +
                        className});
            } else if (!klass.declaredMethods
                            .try_emplace(info.name, std::move(info))
                            .second) {
                diagnostics_.push_back(Diagnostic{
                    node->span,
                    "duplicate method declaration: " + className + "." +
                        node->label});
            } else {
                klass.declaredStaticMethods[node->label] = staticMethodBlock;
            }
        }

        for (const auto& child : node->children) {
            collectFunctionNodes(child.get(), className, staticMethodBlock,
                                 childLexicalParent);
        }
    }

    void collectExplicitSuperclassConstructors(
        const HirNode& node,
        std::vector<std::string>& constructors) const {
        for (const auto& child : node.children) {
            if (child->kind == HirKind::Function) {
                continue;
            }
            if (child->kind == HirKind::SuperclassCall &&
                child->binding.kind == BindingKind::Class) {
                constructors.push_back(child->label);
            }
            collectExplicitSuperclassConstructors(*child, constructors);
        }
    }

    void collectArgumentDefaultRanges(const BytecodeProgram& program,
                                      size_t begin, size_t end,
                                      FunctionInfo& info) {
        for (size_t pc = begin; pc < end; ++pc) {
            const auto& instruction = program.instructions[pc];
            if (instruction.op == BytecodeOp::EnterFunction) {
                size_t depth = 1;
                while (++pc < end && depth != 0) {
                    const auto op = program.instructions[pc].op;
                    if (op == BytecodeOp::EnterFunction) {
                        ++depth;
                    } else if (op == BytecodeOp::LeaveFunction) {
                        --depth;
                    }
                }
                continue;
            }
            if (instruction.op != BytecodeOp::EnterArgumentDefault) {
                continue;
            }
            if (instruction.target <= static_cast<int>(pc + 1) ||
                instruction.target > static_cast<int>(end)) {
                addDiagnostic(instruction,
                              "argument default has an invalid bytecode range");
                return;
            }
            const auto contract = std::find_if(
                info.argumentContracts.begin(), info.argumentContracts.end(),
                [&](const ArgumentContract& candidate) {
                    return candidate.blockKind == ArgumentBlockKind::Input &&
                           candidate.name == instruction.operand;
                });
            if (contract == info.argumentContracts.end()) {
                addDiagnostic(instruction,
                              "argument default has no declaration: " +
                                  instruction.operand);
                return;
            }
            contract->defaultEntry = pc + 1;
            contract->defaultEnd =
                static_cast<size_t>(instruction.target - 1);
            contract->hasDefaultRange = true;
            pc = static_cast<size_t>(instruction.target - 1);
        }
    }

    void collectFunctionRanges(const BytecodeProgram& program) {
        struct ActiveFunctionRange {
            size_t end = 0;
            std::string key;
        };

        functionEndAt_.assign(program.instructions.size(), 0);
        std::vector<std::string> classStack;
        std::vector<ActiveFunctionRange> activeFunctions;
        for (size_t pc = 0; pc < program.instructions.size(); ++pc) {
            while (!activeFunctions.empty() &&
                   pc > activeFunctions.back().end) {
                activeFunctions.pop_back();
            }
            const auto& instruction = program.instructions[pc];
            if (instruction.op == BytecodeOp::EnterClass) {
                classStack.push_back(instruction.operand);
                continue;
            }
            if (instruction.op == BytecodeOp::LeaveClass) {
                if (!classStack.empty()) {
                    classStack.pop_back();
                }
                continue;
            }
            if (instruction.op == BytecodeOp::EnterPropertyInitializer) {
                size_t depth = 1;
                size_t end = pc + 1;
                while (end < program.instructions.size() && depth > 0) {
                    const auto op = program.instructions[end].op;
                    if (op == BytecodeOp::EnterPropertyInitializer) {
                        ++depth;
                    } else if (op ==
                               BytecodeOp::LeavePropertyInitializer) {
                        --depth;
                        if (depth == 0) {
                            break;
                        }
                    }
                    ++end;
                }
                if (end >= program.instructions.size()) {
                    addDiagnostic(
                        instruction,
                        "property initializer has no matching leave");
                    return;
                }
                if (classStack.empty()) {
                    addDiagnostic(instruction,
                                  "property initializer is outside a class");
                    return;
                }
                auto& klass = classesByName_[classStack.back()];
                const auto property =
                    klass.declaredProperties.find(instruction.operand);
                if (property == klass.declaredProperties.end()) {
                    addDiagnostic(instruction,
                                  "property initializer has no declaration: " +
                                      classStack.back() + "." +
                                      instruction.operand);
                    return;
                }
                property->second->initializerEntry = pc + 1;
                property->second->initializerEnd = end;
                property->second->hasInitializerRange = true;
                pc = end;
                continue;
            }
            if (instruction.op ==
                BytecodeOp::EnterEnumerationMemberInitializer) {
                size_t depth = 1;
                size_t end = pc + 1;
                while (end < program.instructions.size() && depth > 0) {
                    const auto op = program.instructions[end].op;
                    if (op == BytecodeOp::EnterEnumerationMemberInitializer) {
                        ++depth;
                    } else if (
                        op ==
                        BytecodeOp::LeaveEnumerationMemberInitializer) {
                        --depth;
                        if (depth == 0) {
                            break;
                        }
                    }
                    ++end;
                }
                if (end >= program.instructions.size()) {
                    addDiagnostic(
                        instruction,
                        "enumeration member initializer has no matching "
                        "leave");
                    return;
                }
                if (classStack.empty()) {
                    addDiagnostic(
                        instruction,
                        "enumeration member initializer is outside a class");
                    return;
                }
                auto& klass = classesByName_[classStack.back()];
                const auto member = klass.declaredEnumerationMembers.find(
                    instruction.operand);
                if (member == klass.declaredEnumerationMembers.end()) {
                    addDiagnostic(
                        instruction,
                        "enumeration member initializer has no declaration: " +
                            classStack.back() + "." + instruction.operand);
                    return;
                }
                member->second.initializerEntry = pc + 1;
                member->second.initializerEnd = end;
                member->second.hasInitializerRange = true;
                member->second.argumentCount = instruction.operandCount;
                pc = end;
                continue;
            }
            if (instruction.op != BytecodeOp::EnterFunction) {
                continue;
            }

            size_t depth = 1;
            size_t end = pc + 1;
            while (end < program.instructions.size() && depth > 0) {
                const auto op = program.instructions[end].op;
                if (op == BytecodeOp::EnterFunction) {
                    ++depth;
                } else if (op == BytecodeOp::LeaveFunction) {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
                ++end;
            }
            if (end >= program.instructions.size()) {
                addDiagnostic(instruction,
                              "bytecode function has no matching leave");
                return;
            }

            FunctionInfo info;
            info.name = instruction.operand;
            info.entry = pc + 1;
            info.end = end;
            info.span = instruction.span;
            info.lexicalParent = activeFunctions.empty()
                                     ? std::string{}
                                     : activeFunctions.back().key;
            info.key = !info.lexicalParent.empty()
                           ? info.lexicalParent + ">" + info.name
                           : !classStack.empty()
                                 ? classStack.back() + "." + info.name
                                 : info.name;
            const std::string functionKey = info.key;
            functionEndAt_[pc] = end;

            const HirNode* hirNode = nullptr;
            if (const auto hir = functionNodes_.find(info.key);
                hir != functionNodes_.end()) {
                hirNode = hir->second;
                info.signature = parseFunctionSignature(*hirNode);
                info.attributes = hirNode->attributes;
                info.declaringClass = hirNode->lexicalClassName;
                collectArgumentContracts(*hirNode, argumentContractCatalog_,
                                         info.argumentContracts);
                collectArgumentDefaultRanges(program, info.entry, info.end,
                                             info);
                info.captureNames =
                    semantic_ ? nestedFunctionCaptureNames(*hirNode, *semantic_)
                              : std::vector<std::string>{};
                info.hasNestedFunctions =
                    hasDirectNestedFunction(*hirNode);
                populateFunctionMetadata(info, hirNode);
            } else {
                populateFunctionMetadata(info, nullptr);
            }

            if (!info.lexicalParent.empty()) {
                functionsByName_[functionKey] = std::move(info);
            } else if (classStack.empty()) {
                functionsByName_[functionKey] = std::move(info);
                registerFunctionMetadata(functionsByName_.at(functionKey));
            } else {
                info.declaringClass = classStack.back();
                if (hirNode) {
                    collectExplicitSuperclassConstructors(
                        *hirNode, info.explicitSuperclassConstructors);
                }
                const auto& declaredStatic =
                    classesByName_[classStack.back()].declaredStaticMethods;
                info.staticMethod = declaredStatic.contains(info.name) &&
                                    declaredStatic.at(info.name);
                auto& declaredMethods =
                    classesByName_[classStack.back()].declaredMethods;
                if (!declaredMethods.try_emplace(info.name, std::move(info))
                         .second) {
                    diagnostics_.push_back(Diagnostic{
                        instruction.span,
                        "duplicate method declaration: " +
                            classStack.back() + "." + instruction.operand});
                }
            }
            activeFunctions.push_back(ActiveFunctionRange{end, functionKey});
        }
    }

    void collectStructuredControlRanges(
        const BytecodeProgram& program) {
        switchEndAt_.assign(program.instructions.size(), 0);
        tryEndAt_.assign(program.instructions.size(), 0);
        std::vector<size_t> switches;
        std::vector<size_t> tries;
        for (size_t pc = 0; pc < program.instructions.size(); ++pc) {
            switch (program.instructions[pc].op) {
            case BytecodeOp::SwitchBegin:
                switches.push_back(pc);
                break;
            case BytecodeOp::SwitchEnd:
                if (!switches.empty()) {
                    switchEndAt_[switches.back()] = pc;
                    switches.pop_back();
                }
                break;
            case BytecodeOp::TryBegin:
                tries.push_back(pc);
                break;
            case BytecodeOp::TryEnd:
                if (!tries.empty()) {
                    tryEndAt_[tries.back()] = pc;
                    tries.pop_back();
                }
                break;
            default:
                break;
            }
        }
    }

    std::optional<bool> logicalAttributeValue(
        const AttributeSyntax& attribute, const std::string& owner) {
        if (attribute.value.empty()) {
            return !attribute.negated;
        }
        if (attribute.negated) {
            diagnostics_.push_back(Diagnostic{
                attribute.span,
                "negated logical attribute cannot also have a value: " +
                    owner + "." + attribute.name});
            return std::nullopt;
        }
        const std::string value = lowerAscii(trimAscii(attribute.value));
        if (value == "true") {
            return true;
        }
        if (value == "false") {
            return false;
        }
        diagnostics_.push_back(Diagnostic{
            attribute.span,
            "logical attribute requires true or false: " + owner + "." +
                attribute.name});
        return std::nullopt;
    }

    void configureClassAttributes(ClassInfo& klass) {
        for (const auto& attribute : klass.attributes) {
            const std::string name = lowerAscii(attribute.name);
            if (name == "abstract") {
                if (const auto value =
                        logicalAttributeValue(attribute, klass.name)) {
                    klass.explicitlyAbstract = *value;
                }
            } else if (name == "sealed") {
                if (const auto value =
                        logicalAttributeValue(attribute, klass.name)) {
                    klass.sealedClass = *value;
                }
            } else if (name == "hidden") {
                if (const auto value =
                        logicalAttributeValue(attribute, klass.name)) {
                    klass.hidden = *value;
                }
            } else if (name == "constructonload") {
                if (const auto value =
                        logicalAttributeValue(attribute, klass.name)) {
                    klass.constructOnLoad = *value;
                }
            } else if (name == "handlecompatible") {
                if (const auto value =
                        logicalAttributeValue(attribute, klass.name)) {
                    klass.handleCompatible = *value;
                }
            } else if (name == "allowedsubclasses") {
                if (attribute.negated || attribute.value.empty() ||
                    !attribute.hasMetaClassList) {
                    klass.hierarchyValid = false;
                    diagnostics_.push_back(Diagnostic{
                        attribute.span,
                        "AllowedSubclasses requires ?Class or a cell array "
                        "of meta-class references: " +
                            klass.name});
                    continue;
                }
                klass.restrictsSubclassing = true;
                klass.allowedSubclasses.clear();
                for (const auto& className : attribute.metaClassNames) {
                    if (classesByName_.contains(className)) {
                        klass.allowedSubclasses.push_back(className);
                    }
                }
                std::sort(klass.allowedSubclasses.begin(),
                          klass.allowedSubclasses.end());
                klass.allowedSubclasses.erase(
                    std::unique(klass.allowedSubclasses.begin(),
                                klass.allowedSubclasses.end()),
                    klass.allowedSubclasses.end());
            }
        }
    }

    std::optional<MemberAccessPolicy> accessAttributeValue(
        const AttributeSyntax& attribute, const std::string& owner,
        bool allowImmutable) {
        if (attribute.negated || attribute.value.empty()) {
            diagnostics_.push_back(Diagnostic{
                attribute.span,
                "access attribute requires an explicit value: " + owner +
                    "." + attribute.name});
            return std::nullopt;
        }
        if (attribute.hasMetaClassList) {
            std::vector<std::string> classNames;
            for (const auto& className : attribute.metaClassNames) {
                if (classesByName_.contains(className)) {
                    classNames.push_back(className);
                }
            }
            std::sort(classNames.begin(), classNames.end());
            classNames.erase(
                std::unique(classNames.begin(), classNames.end()),
                classNames.end());
            if (classNames.empty()) {
                return MemberAccessPolicy{MemberAccessLevel::Private, {},
                                          true,
                                          attribute.metaClassNames.empty()};
            }
            return MemberAccessPolicy{MemberAccessLevel::ClassList,
                                      std::move(classNames), true};
        }
        const std::string value = lowerAscii(trimAscii(attribute.value));
        if (value == "public") {
            return MemberAccessPolicy{MemberAccessLevel::Public, {}};
        }
        if (value == "protected") {
            return MemberAccessPolicy{MemberAccessLevel::Protected, {}};
        }
        if (value == "private") {
            return MemberAccessPolicy{MemberAccessLevel::Private, {}, false,
                                      true};
        }
        if (allowImmutable && value == "immutable") {
            return MemberAccessPolicy{MemberAccessLevel::Immutable, {}};
        }
        diagnostics_.push_back(Diagnostic{
            attribute.span,
            "unsupported access attribute value for " + owner + "." +
                attribute.name + ": " + attribute.value});
        return std::nullopt;
    }

    void configurePropertyAttributes(PropertyInfo& property) {
        const std::string owner = propertyDisplayName(property);
        for (const auto& attribute : property.attributes) {
            const std::string name = lowerAscii(attribute.name);
            if (name == "access") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, false)) {
                    property.getAccess = *access;
                    property.setAccess = *access;
                }
            } else if (name == "getaccess") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, false)) {
                    property.getAccess = *access;
                }
            } else if (name == "setaccess") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, true)) {
                    property.setAccess = *access;
                }
            } else if (name == "constant") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.constant = *value;
                }
            } else if (name == "dependent") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.dependent = *value;
                }
            } else if (name == "abortset") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.abortSet = *value;
                }
            } else if (name == "abstract") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.abstractProperty = *value;
                }
            } else if (name == "transient") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.transient = *value;
                }
            } else if (name == "hidden") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.hidden = *value;
                }
            } else if (name == "getobservable") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.getObservable = *value;
                }
            } else if (name == "setobservable") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.setObservable = *value;
                }
            } else if (name == "noncopyable") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.nonCopyable = *value;
                }
            } else if (name == "weakhandle" ||
                       name == "weakreference") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    property.weakHandle = *value;
                }
            } else if (name == "partialmatchpriority") {
                const auto value = parseRealNumber(
                    trimAscii(attribute.value));
                if (attribute.negated || !value || !std::isfinite(*value) ||
                    *value < 1.0 || std::floor(*value) != *value) {
                    diagnostics_.push_back(Diagnostic{
                        attribute.span,
                        "PartialMatchPriority requires a positive integer: " +
                            owner});
                } else {
                    property.partialMatchPriority = *value;
                }
            }
        }

        if (property.abstractProperty && property.constant) {
            diagnostics_.push_back(Diagnostic{
                property.span,
                "property cannot be both Abstract and Constant: " + owner});
        } else if (property.constant && !property.spec.hasExplicitDefault) {
            diagnostics_.push_back(Diagnostic{
                property.span,
                "constant property requires a default value: " + owner});
        }
        if (property.constant && property.dependent) {
            diagnostics_.push_back(Diagnostic{
                property.span,
                "property cannot be both Constant and Dependent: " + owner});
        }
        if (property.dependent && property.spec.hasExplicitDefault) {
            diagnostics_.push_back(Diagnostic{
                property.span,
                "dependent property cannot define a default value: " + owner});
        }
        if (property.abstractProperty && property.spec.hasExplicitDefault) {
            diagnostics_.push_back(Diagnostic{
                property.span,
                "abstract property cannot define a default value: " + owner});
        }
    }

    void configureMethodAttributes(ClassInfo& klass, FunctionInfo& method) {
        const std::string owner = klass.name + "." + method.name;
        for (const auto& attribute : method.attributes) {
            const std::string name = lowerAscii(attribute.name);
            if (name == "access") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, false)) {
                    method.access = *access;
                }
            } else if (name == "static") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    method.staticMethod = *value;
                }
            } else if (name == "abstract") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    method.abstractMethod = *value;
                }
            } else if (name == "sealed") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    method.sealedMethod = *value;
                }
            } else if (name == "hidden") {
                if (const auto value = logicalAttributeValue(attribute, owner)) {
                    method.hidden = *value;
                }
            }
        }

        if (method.abstractMethod && method.sealedMethod) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "method cannot be both Abstract and Sealed: " + owner});
        }
        if (method.abstractMethod && method.hasBody) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "abstract method must be declared as a prototype: " + owner});
        }
        if (!method.abstractMethod && !method.hasBody) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "method prototype must be declared Abstract: " + owner});
        }
        if (method.abstractMethod && method.name == klass.name) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "class constructor cannot be Abstract: " + owner});
        }

        method.classDestructor =
            method.name == "delete" && method.hasBody &&
            !method.staticMethod && !method.abstractMethod &&
            !method.sealedMethod &&
            method.signature.parameters.size() == 1 &&
            !method.signature.hasVarargin &&
            method.signature.outputs.empty() &&
            !method.signature.hasVarargout &&
            !method.hasInputArgumentBlock &&
            !method.hasOutputArgumentBlock;

        const bool getter = method.name.rfind("get.", 0) == 0;
        const bool setter = method.name.rfind("set.", 0) == 0;
        if (!getter && !setter) {
            return;
        }
        method.propertyAccessor = true;
        method.accessorProperty = method.name.substr(4);
        if (method.accessorProperty.empty() ||
            method.accessorProperty.find('.') != std::string::npos) {
            diagnostics_.push_back(Diagnostic{
                method.span, "invalid property access method name: " + owner});
            return;
        }
        if (!method.attributes.empty()) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "property access methods must be declared in a methods block "
                "without attributes: " + owner});
        }
        const auto property =
            klass.declaredProperties.find(method.accessorProperty);
        if (property == klass.declaredProperties.end()) {
            diagnostics_.push_back(Diagnostic{
                method.span,
                "property access method has no property declaration: " +
                    owner});
            return;
        }
        if (getter) {
            property->second->getterName = method.name;
        } else {
            property->second->setterName = method.name;
        }
    }

    void configureEnumerationMemberAttributes(
        EnumerationMemberInfo& member) {
        const std::string owner =
            member.declaringClass + "." + member.name;
        for (const auto& attribute : member.attributes) {
            if (lowerAscii(attribute.name) != "hidden") {
                continue;
            }
            if (const auto value = logicalAttributeValue(attribute, owner)) {
                member.hidden = *value;
            }
        }
    }

    void configureEventAttributes(EventInfo& event) {
        const std::string owner =
            event.declaringClass + "." + event.name;
        for (const auto& attribute : event.attributes) {
            const std::string name = lowerAscii(attribute.name);
            if (name == "listenaccess") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, false)) {
                    event.listenAccess = *access;
                }
            } else if (name == "notifyaccess") {
                if (const auto access =
                        accessAttributeValue(attribute, owner, false)) {
                    event.notifyAccess = *access;
                }
            } else if (name == "hidden") {
                if (const auto value =
                        logicalAttributeValue(attribute, owner)) {
                    event.hidden = *value;
                }
            }
        }
    }

    void configureDeclaredClassMembers() {
        for (auto& [className, klass] : classesByName_) {
            (void)className;
            configureClassAttributes(klass);
            if (klass.enumerationClass) {
                klass.sealedClass = true;
            }
            for (const auto& property : klass.declaredPropertyOrder) {
                configurePropertyAttributes(*property);
            }
            for (auto& [memberName, member] :
                 klass.declaredEnumerationMembers) {
                (void)memberName;
                configureEnumerationMemberAttributes(member);
            }
            for (auto& [eventName, event] : klass.declaredEvents) {
                (void)eventName;
                configureEventAttributes(event);
            }
            for (auto& [methodName, method] : klass.declaredMethods) {
                configureMethodAttributes(klass, method);
                klass.declaredStaticMethods[methodName] = method.staticMethod;
            }
        }
    }

    void finalizeEnumerationSemantics() {
        for (auto& [className, klass] : classesByName_) {
            if (!klass.enumerationClass || klass.handleClass) {
                continue;
            }
            for (const auto& property : klass.declaredPropertyOrder) {
                if (property->constant) {
                    continue;
                }
                for (const auto& attribute : property->attributes) {
                    const std::string name = lowerAscii(attribute.name);
                    if (name != "access" && name != "setaccess") {
                        continue;
                    }
                    if (lowerAscii(trimAscii(attribute.value)) !=
                        "immutable") {
                        diagnostics_.push_back(Diagnostic{
                            attribute.span,
                            "value enumeration property SetAccess must be "
                            "immutable: " +
                                className + "." + property->name});
                    }
                }
                property->setAccess = MemberAccessPolicy{
                    MemberAccessLevel::Immutable, {}};
            }
        }
    }

    void validateResolvedClassMembers() {
        for (const auto& [className, klass] : classesByName_) {
            if (!klass.declaredEvents.empty() && !klass.handleClass) {
                diagnostics_.push_back(Diagnostic{
                    klass.span,
                    "events can be declared only by handle classes: " +
                        className});
            }
            for (const auto& property : klass.declaredPropertyOrder) {
                const std::string owner = propertyDisplayName(*property);
                if (property->abortSet && !klass.handleClass) {
                    diagnostics_.push_back(Diagnostic{
                        property->span,
                        "AbortSet is supported only for handle-class "
                        "properties: " + owner});
                }
                if (property->abstractProperty &&
                    (!property->getterName.empty() ||
                     !property->setterName.empty())) {
                    diagnostics_.push_back(Diagnostic{
                        property->span,
                        "abstract property cannot define access methods: " +
                            owner});
                }
                if (!property->abstractProperty && property->dependent &&
                    property->getterName.empty()) {
                    diagnostics_.push_back(Diagnostic{
                        property->span,
                        "dependent property requires a get method: " + owner});
                }
                if (property->constant &&
                    (!property->getterName.empty() ||
                     !property->setterName.empty())) {
                    diagnostics_.push_back(Diagnostic{
                        property->span,
                        "constant property cannot define access methods: " +
                            owner});
                }

                if (!property->getterName.empty()) {
                    const auto& getter =
                        klass.declaredMethods.at(property->getterName);
                    if (getter.staticMethod || getter.signature.hasVarargin ||
                        functionHasRepeatingOutput(getter.signature) ||
                        getter.signature.parameters.size() != 1 ||
                        getter.signature.outputs.size() != 1) {
                        diagnostics_.push_back(Diagnostic{
                            getter.span,
                            "property get method must accept one object and "
                            "return one value: " + className + "." +
                                getter.name});
                    }
                }
                if (!property->setterName.empty()) {
                    const auto& setter =
                        klass.declaredMethods.at(property->setterName);
                    const size_t expectedOutputs = klass.handleClass ? 0 : 1;
                    const bool outputCountValid =
                        klass.handleClass
                            ? setter.signature.outputs.size() <= 1
                            : setter.signature.outputs.size() == expectedOutputs;
                    if (setter.staticMethod || setter.signature.hasVarargin ||
                        functionHasRepeatingOutput(setter.signature) ||
                        setter.signature.parameters.size() != 2 ||
                        !outputCountValid) {
                        diagnostics_.push_back(Diagnostic{
                            setter.span,
                            klass.handleClass
                                ? "handle-class property set method must "
                                  "accept object and value and return at most "
                                  "one object: " + className + "." + setter.name
                                : "value-class property set method must accept "
                                  "object and value and return one object: " +
                                      className + "." + setter.name});
                    }
                }
            }
        }
    }

    bool hasPropertyValidation(const PropertyInfo& property) const {
        return !property.spec.dimensions.empty() ||
               !property.spec.className.empty() ||
               !property.spec.validators.empty();
    }

    bool isFullyPrivateProperty(const PropertyInfo& property) const {
        return property.getAccess.level == MemberAccessLevel::Private &&
               property.setAccess.level == MemberAccessLevel::Private;
    }

    bool hasNonPrivateProperty(
        const PropertyCandidates& candidates) const {
        return std::any_of(
            candidates.begin(), candidates.end(),
            [this](const PropertyInfoPtr& property) {
                return !isFullyPrivateProperty(*property);
            });
    }

    PropertyInfoPtr selectAbstractPropertyImplementation(
        const PropertyCandidates& candidates) const {
        const auto visible = std::find_if(
            candidates.begin(), candidates.end(),
            [this](const PropertyInfoPtr& property) {
                return !isFullyPrivateProperty(*property);
            });
        if (visible != candidates.end()) {
            return *visible;
        }
        return candidates.empty() ? nullptr : candidates.back();
    }

    void mergeInheritedProperty(
        ClassInfo& info, PropertyTable& properties,
        std::vector<PropertyInfoPtr>& propertyOrder,
        const PropertyInfoPtr& candidate, bool& valid) {
        auto& candidates = properties[candidate->name];
        if (std::any_of(candidates.begin(), candidates.end(),
                        [&candidate](const PropertyInfoPtr& existing) {
                            return existing->storageKey ==
                                   candidate->storageKey;
                        })) {
            return;
        }

        if (!isFullyPrivateProperty(*candidate)) {
            const auto conflict = std::find_if(
                candidates.begin(), candidates.end(),
                [this](const PropertyInfoPtr& existing) {
                    return !isFullyPrivateProperty(*existing);
                });
            if (conflict != candidates.end()) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info,
                    "property:" + info.name + ":" + candidate->name,
                    "ambiguous inherited property: " + info.name + "." +
                        candidate->name + " from " +
                        (*conflict)->declaringClass + " and " +
                        candidate->declaringClass);
                return;
            }
        }

        candidates.push_back(candidate);
        propertyOrder.push_back(candidate);
    }

    bool isPrivateMethod(const FunctionInfo& method) const {
        return method.access.level == MemberAccessLevel::Private &&
               method.access.privateMemberIdentity;
    }

    void mergePrivateMethod(PrivateMethodTable& methods,
                            const FunctionInfo& candidate) const {
        auto& candidates = methods[candidate.name];
        const bool alreadyPresent = std::any_of(
            candidates.begin(), candidates.end(),
            [&candidate](const FunctionInfo& existing) {
                return existing.declaringClass == candidate.declaringClass;
            });
        if (!alreadyPresent) {
            candidates.push_back(candidate);
        }
    }

    void mergePrivateMethods(PrivateMethodTable& methods,
                             const PrivateMethodTable& inherited) const {
        for (const auto& [name, candidates] : inherited) {
            (void)name;
            for (const auto& candidate : candidates) {
                if (candidate.name == candidate.declaringClass) {
                    continue;
                }
                mergePrivateMethod(methods, candidate);
            }
        }
    }

    void mergeAbstractPropertyRequirement(
        ClassInfo& info,
        std::map<std::string, PropertyInfoPtr>& requirements,
        const PropertyInfoPtr& candidate, bool& valid) {
        const auto existing = requirements.find(candidate->name);
        if (existing == requirements.end()) {
            requirements[candidate->name] = candidate;
            return;
        }
        if (existing->second == candidate) {
            return;
        }

        if (existing->second->getAccess != candidate->getAccess ||
            existing->second->setAccess != candidate->setAccess) {
            valid = false;
            reportClassHierarchyDiagnostic(
                info, "abstract-property-access:" + info.name + ":" +
                          candidate->name,
                "inherited abstract property access contracts conflict: " +
                    info.name + "." + candidate->name);
        }

        const bool existingValidation =
            hasPropertyValidation(*existing->second);
        const bool candidateValidation = hasPropertyValidation(*candidate);
        if (existingValidation && candidateValidation) {
            valid = false;
            reportClassHierarchyDiagnostic(
                info, "abstract-property-validation:" + info.name + ":" +
                          candidate->name,
                "multiple inherited abstract properties define validation: " +
                    info.name + "." + candidate->name);
        } else if (candidateValidation) {
            existing->second = candidate;
        }
    }

    bool applyAbstractPropertyRequirement(
        ClassInfo& info, PropertyInfo& implementation,
        const PropertyInfo& requirement) {
        bool valid = true;
        if (implementation.getAccess != requirement.getAccess ||
            implementation.setAccess != requirement.setAccess) {
            valid = false;
            reportClassHierarchyDiagnostic(
                info, "abstract-property-implementation-access:" +
                          info.name + ":" + implementation.name,
                "abstract property implementation must preserve GetAccess "
                "and SetAccess: " +
                    info.name + "." + implementation.name);
        }

        if (!hasPropertyValidation(requirement)) {
            return valid;
        }
        if (hasPropertyValidation(implementation)) {
            valid = false;
            reportClassHierarchyDiagnostic(
                info, "abstract-property-implementation-validation:" +
                          info.name + ":" + implementation.name,
                "abstract property implementation cannot redefine inherited "
                "validation: " +
                    info.name + "." + implementation.name);
            return valid;
        }

        implementation.spec.dimensions = requirement.spec.dimensions;
        implementation.spec.className = requirement.spec.className;
        implementation.spec.classSpan = requirement.spec.classSpan;
        implementation.spec.validators = requirement.spec.validators;
        return valid;
    }

    void resolveClassHierarchies() {
        std::map<std::string, ClassResolutionState> states;
        for (const auto& [name, info] : classesByName_) {
            (void)info;
            resolveClassHierarchy(name, states);
        }
    }

    bool resolveClassHierarchy(
        const std::string& className,
        std::map<std::string, ClassResolutionState>& states) {
        auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            return false;
        }

        auto& state = states[className];
        if (state == ClassResolutionState::Resolved) {
            return klass->second.hierarchyValid;
        }
        if (state == ClassResolutionState::Resolving) {
            klass->second.hierarchyValid = false;
            reportClassHierarchyDiagnostic(
                klass->second, "cycle:" + className,
                "cyclic class inheritance involving: " + className);
            return false;
        }

        state = ClassResolutionState::Resolving;
        auto& info = klass->second;
        bool valid = info.hierarchyValid;
        bool handleClass = info.directHandleClass;
        std::string heterogeneousRoot =
            info.directHeterogeneousClass ? className : std::string{};
        PropertyTable properties;
        std::vector<PropertyInfoPtr> propertyOrder;
        std::map<std::string, FunctionInfo> methods;
        PrivateMethodTable privateMethods;
        std::map<std::string, bool> staticMethods;
        std::map<std::string, FunctionInfo> abstractMethods;
        std::map<std::string, PropertyInfoPtr> abstractProperties;
        std::map<std::string, EventInfo> events;
        std::vector<std::string> eventOrder;

        for (const auto& superclassName : info.superclasses) {
            if (isBuiltinHandleSuperclass(superclassName)) {
                handleClass = true;
                continue;
            }
            if (superclassName == kHeterogeneousClassName) {
                continue;
            }

            const auto superclass = classesByName_.find(superclassName);
            if (superclass == classesByName_.end()) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "missing:" + className + ":" + superclassName,
                    "superclass is not available: " + superclassName +
                        " (required by " + className + ")");
                continue;
            }

            if (!resolveClassHierarchy(superclassName, states)) {
                valid = false;
            }
            const auto& base = superclass->second;
            handleClass = handleClass || base.handleClass;
            if (!base.heterogeneousRoot.empty()) {
                if (heterogeneousRoot.empty()) {
                    heterogeneousRoot = base.heterogeneousRoot;
                } else if (heterogeneousRoot !=
                           base.heterogeneousRoot) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info,
                        "heterogeneous-root:" + className + ":" +
                            superclassName,
                        "class cannot combine distinct heterogeneous "
                        "hierarchies: " + className);
                }
            }
            const bool effectivelySealed =
                base.sealedClass ||
                (base.restrictsSubclassing &&
                 base.allowedSubclasses.empty());
            if (effectivelySealed) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "sealed-class:" + className + ":" + superclassName,
                    "sealed class cannot be subclassed: " + superclassName +
                        " (required by " + className + ")");
            } else if (base.restrictsSubclassing &&
                       !std::binary_search(base.allowedSubclasses.begin(),
                                           base.allowedSubclasses.end(),
                                           className)) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info,
                    "disallowed-subclass:" + className + ":" +
                        superclassName,
                    "class is not listed in AllowedSubclasses: " +
                        className + " cannot derive from " + superclassName);
            }

            for (const auto& property : base.propertyOrder) {
                mergeInheritedProperty(info, properties, propertyOrder,
                                       property, valid);
            }

            for (const auto& [propertyName, property] :
                 base.abstractProperties) {
                (void)propertyName;
                mergeAbstractPropertyRequirement(
                    info, abstractProperties, property, valid);
            }

            for (const auto& eventName : base.eventOrder) {
                const auto candidate = base.events.find(eventName);
                if (candidate == base.events.end()) {
                    continue;
                }
                const auto existing = events.find(eventName);
                if (existing == events.end()) {
                    events[eventName] = candidate->second;
                    eventOrder.push_back(eventName);
                    continue;
                }
                if (existing->second.declaringClass ==
                    candidate->second.declaringClass) {
                    continue;
                }
                if (classDerivesFrom(candidate->second.declaringClass,
                                     existing->second.declaringClass)) {
                    events[eventName] = candidate->second;
                    continue;
                }
                if (classDerivesFrom(existing->second.declaringClass,
                                     candidate->second.declaringClass)) {
                    continue;
                }
                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "event:" + className + ":" + eventName,
                    "ambiguous inherited event: " + className + "." +
                        eventName + " from " +
                        existing->second.declaringClass + " and " +
                        candidate->second.declaringClass);
            }

            mergePrivateMethods(privateMethods, base.privateMethods);

            for (const auto& [methodName, method] : base.methods) {
                if (method.name == method.declaringClass) {
                    continue;
                }
                if (method.classDestructor) {
                    continue;
                }

                const bool candidateStatic =
                    base.staticMethods.contains(methodName) &&
                    base.staticMethods.at(methodName);
                const auto existing = methods.find(methodName);
                if (existing == methods.end()) {
                    methods[methodName] = method;
                    staticMethods[methodName] = candidateStatic;
                    continue;
                }
                if (existing->second.declaringClass == method.declaringClass) {
                    continue;
                }
                if (info.declaredMethods.contains(methodName)) {
                    if (method.sealedMethod) {
                        valid = false;
                        reportClassHierarchyDiagnostic(
                            info,
                            "sealed-method:" + className + ":" + methodName,
                            "sealed method cannot be redefined: " +
                                className + "." + methodName +
                                " (declared by " + method.declaringClass +
                                ")");
                    }
                    continue;
                }
                if (classDerivesFrom(method.declaringClass,
                                     existing->second.declaringClass)) {
                    methods[methodName] = method;
                    staticMethods[methodName] = candidateStatic;
                    continue;
                }
                if (classDerivesFrom(existing->second.declaringClass,
                                     method.declaringClass)) {
                    continue;
                }

                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "method:" + className + ":" + methodName,
                    "ambiguous inherited method: " + className + "." +
                        methodName + " from " +
                        existing->second.declaringClass + " and " +
                        method.declaringClass);
            }

            for (const auto& [methodName, method] : base.abstractMethods) {
                abstractMethods.try_emplace(methodName, method);
            }
        }

        for (auto requirement = abstractMethods.begin();
             requirement != abstractMethods.end();) {
            if (methods.contains(requirement->first)) {
                requirement = abstractMethods.erase(requirement);
            } else {
                ++requirement;
            }
        }

        for (auto requirement = abstractProperties.begin();
             requirement != abstractProperties.end();) {
            const auto implementation = properties.find(requirement->first);
            if (implementation == properties.end()) {
                ++requirement;
                continue;
            }

            const auto original = selectAbstractPropertyImplementation(
                implementation->second);
            if (!original) {
                ++requirement;
                continue;
            }
            auto effective = std::make_shared<PropertyInfo>(*original);
            if (!applyAbstractPropertyRequirement(
                    info, *effective, *requirement->second)) {
                valid = false;
            }
            for (auto& ordered : propertyOrder) {
                if (ordered == original) {
                    ordered = effective;
                }
            }
            for (auto& candidate : implementation->second) {
                if (candidate == original) {
                    candidate = effective;
                }
            }
            requirement = abstractProperties.erase(requirement);
        }

        for (const auto& property : info.declaredPropertyOrder) {
            const std::string& propertyName = property->name;
            if (property->abstractProperty) {
                const auto inherited = properties.find(propertyName);
                if (inherited != properties.end() &&
                    hasNonPrivateProperty(inherited->second)) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info,
                        "abstract-property-over-concrete:" + className + ":" +
                            propertyName,
                        "abstract property cannot redeclare an inherited "
                        "concrete property: " +
                            className + "." + propertyName);
                }
                mergeAbstractPropertyRequirement(
                    info, abstractProperties, property, valid);
                continue;
            }

            if (const auto inherited = properties.find(propertyName);
                inherited != properties.end() &&
                hasNonPrivateProperty(inherited->second)) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "property:" + className + ":" + propertyName,
                    "inherited property cannot be redeclared: " + className +
                        "." + propertyName);
                continue;
            }
            if (const auto requirement =
                    abstractProperties.find(propertyName);
                requirement != abstractProperties.end()) {
                if (!applyAbstractPropertyRequirement(
                        info, *property, *requirement->second)) {
                    valid = false;
                }
                abstractProperties.erase(requirement);
            }
            properties[propertyName].push_back(property);
            propertyOrder.push_back(property);
        }

        if (handleClass &&
            !events.contains(
                std::string(kObjectBeingDestroyedEventName))) {
            const auto& event = handleDestructionEvent();
            events[event.name] = event;
            eventOrder.push_back(event.name);
        }

        for (const auto& eventName : info.declaredEventOrder) {
            const auto& event = info.declaredEvents.at(eventName);
            if (events.contains(eventName)) {
                valid = false;
                reportClassHierarchyDiagnostic(
                    info, "event-redeclare:" + className + ":" + eventName,
                    "inherited event cannot be redeclared: " + className +
                        "." + eventName);
                continue;
            }
            events[eventName] = event;
            eventOrder.push_back(eventName);
        }

        for (const auto& [methodName, method] : info.declaredMethods) {
            if (method.propertyAccessor) {
                continue;
            }

            if (method.classDestructor) {
                methods[methodName] = method;
                staticMethods[methodName] = false;
                abstractMethods.erase(methodName);
                continue;
            }

            const auto inherited = methods.find(methodName);
            const auto inheritedPrivate = privateMethods.find(methodName);
            if (methodName != className &&
                inheritedPrivate != privateMethods.end()) {
                const auto sealed = std::find_if(
                    inheritedPrivate->second.begin(),
                    inheritedPrivate->second.end(),
                    [](const FunctionInfo& candidate) {
                        return candidate.sealedMethod;
                    });
                if (sealed != inheritedPrivate->second.end()) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info, "sealed-method:" + className + ":" + methodName,
                        "sealed method cannot be redefined: " + className +
                            "." + methodName + " (declared by " +
                            sealed->declaringClass + ")");
                }
            }

            if (isPrivateMethod(method)) {
                if (inherited != methods.end() && methodName != className) {
                    if (inherited->second.sealedMethod) {
                        valid = false;
                        reportClassHierarchyDiagnostic(
                            info,
                            "sealed-method:" + className + ":" + methodName,
                            "sealed method cannot be redefined: " +
                                className + "." + methodName +
                                " (declared by " +
                                inherited->second.declaringClass + ")");
                    }
                    if (inherited->second.access.selectiveClassList &&
                        !policyAllowsClass(inherited->second.access,
                                           inherited->second.declaringClass,
                                           className)) {
                        valid = false;
                        reportClassHierarchyDiagnostic(
                            info,
                            "method-override-access:" + className + ":" +
                                methodName,
                            "subclass cannot override inaccessible method: " +
                                className + "." + methodName +
                                " (declared by " +
                                inherited->second.declaringClass + ")");
                    }
                    if (inherited->second.access != method.access) {
                        valid = false;
                        reportClassHierarchyDiagnostic(
                            info,
                            "method-access:" + className + ":" + methodName,
                            "overriding method must preserve Access: " +
                                className + "." + methodName);
                    }
                }
                mergePrivateMethod(privateMethods, method);
                if (method.abstractMethod) {
                    abstractMethods[methodName] = method;
                }
                continue;
            }

            if (method.abstractMethod) {
                if (inherited != methods.end() &&
                    inherited->second.sealedMethod) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info, "sealed-method:" + className + ":" + methodName,
                        "sealed method cannot be redefined: " + className +
                            "." + methodName + " (declared by " +
                            inherited->second.declaringClass + ")");
                }
                if (inherited != methods.end() &&
                    inherited->second.access.selectiveClassList &&
                    !policyAllowsClass(inherited->second.access,
                                       inherited->second.declaringClass,
                                       className)) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info,
                        "method-override-access:" + className + ":" +
                            methodName,
                        "subclass cannot override inaccessible method: " +
                            className + "." + methodName + " (declared by " +
                            inherited->second.declaringClass + ")");
                }
                methods.erase(methodName);
                staticMethods.erase(methodName);
                abstractMethods[methodName] = method;
                continue;
            }

            abstractMethods.erase(methodName);
            if (inherited != methods.end() && methodName != className) {
                if (inherited->second.sealedMethod) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info, "sealed-method:" + className + ":" + methodName,
                        "sealed method cannot be redefined: " + className +
                            "." + methodName + " (declared by " +
                            inherited->second.declaringClass + ")");
                }
                if (inherited->second.access.selectiveClassList &&
                    !policyAllowsClass(inherited->second.access,
                                       inherited->second.declaringClass,
                                       className)) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info,
                        "method-override-access:" + className + ":" +
                            methodName,
                        "subclass cannot override inaccessible method: " +
                            className + "." + methodName + " (declared by " +
                            inherited->second.declaringClass + ")");
                }
                if (inherited->second.access != method.access) {
                    valid = false;
                    reportClassHierarchyDiagnostic(
                        info,
                        "method-access:" + className + ":" + methodName,
                        "overriding method must preserve Access: " +
                            className + "." + methodName);
                }
            }
            methods[methodName] = method;
            staticMethods[methodName] =
                info.declaredStaticMethods.contains(methodName) &&
                info.declaredStaticMethods.at(methodName);
        }

        info.handleClass = handleClass;
        info.heterogeneousRoot = std::move(heterogeneousRoot);
        info.abstractClass = info.explicitlyAbstract ||
                             !abstractMethods.empty() ||
                             !abstractProperties.empty();
        const bool effectivelySealed =
            info.sealedClass ||
            (info.restrictsSubclassing && info.allowedSubclasses.empty());
        if (effectivelySealed && info.abstractClass) {
            valid = false;
            reportClassHierarchyDiagnostic(
                info, "sealed-abstract-class:" + className,
                "sealed class cannot define or inherit abstract members: " +
                    className);
        }
        info.hierarchyValid = valid;
        info.properties = std::move(properties);
        info.propertyOrder = std::move(propertyOrder);
        info.methods = std::move(methods);
        info.privateMethods = std::move(privateMethods);
        info.staticMethods = std::move(staticMethods);
        info.abstractMethods = std::move(abstractMethods);
        info.abstractProperties = std::move(abstractProperties);
        info.events = std::move(events);
        info.eventOrder = std::move(eventOrder);
        state = ClassResolutionState::Resolved;
        return valid;
    }

    bool classDerivesFrom(const std::string& className,
                          const std::string& possibleSuperclass) const {
        std::set<std::string> visiting;
        return classDerivesFrom(className, possibleSuperclass, visiting);
    }

    bool classDerivesFrom(const std::string& className,
                          const std::string& possibleSuperclass,
                          std::set<std::string>& visiting) const {
        if (className == possibleSuperclass) {
            return true;
        }
        if (possibleSuperclass == "handle" &&
            (className == kDynamicPropsClassName ||
             className == kEventDataClassName ||
             className == kPropertyEventClassName ||
             className == kEventListenerClassName ||
             className == kPropertyListenerClassName)) {
            return true;
        }
        if (className == kPropertyEventClassName &&
            possibleSuperclass == kEventDataClassName) {
            return true;
        }
        if (className == kPropertyListenerClassName &&
            possibleSuperclass == kEventListenerClassName) {
            return true;
        }
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end() ||
            !visiting.insert(className).second) {
            return false;
        }
        for (const auto& superclass : klass->second.superclasses) {
            if (superclass == possibleSuperclass ||
                classDerivesFrom(superclass, possibleSuperclass, visiting)) {
                visiting.erase(className);
                return true;
            }
        }
        visiting.erase(className);
        return false;
    }

    RuntimeObjectClassResolutionResult resolveObjectArrayClass(
        const std::vector<std::string>& classNames,
        std::string_view preferredClassName) const {
        const auto failure = [](std::string error) {
            return RuntimeObjectClassResolutionResult{
                false, {}, std::move(error)};
        };
        if (classNames.empty()) {
            return preferredClassName.empty()
                       ? failure(
                             "empty object array requires a class identity")
                       : RuntimeObjectClassResolutionResult{
                             true, std::string(preferredClassName), {}};
        }

        const bool allSame = std::all_of(
            classNames.begin(), classNames.end(),
            [&](const std::string& name) {
                return name == classNames.front();
            });
        if (preferredClassName.empty() && allSame) {
            return RuntimeObjectClassResolutionResult{
                true, classNames.front(), {}};
        }
        if (!preferredClassName.empty() && allSame &&
            classNames.front() == preferredClassName) {
            return RuntimeObjectClassResolutionResult{
                true, classNames.front(), {}};
        }

        std::string heterogeneousRoot;
        if (!preferredClassName.empty()) {
            const auto preferred =
                classesByName_.find(std::string(preferredClassName));
            if (preferred == classesByName_.end() ||
                preferred->second.heterogeneousRoot.empty()) {
                return failure(
                    "object assignment requires class " +
                    std::string(preferredClassName));
            }
            heterogeneousRoot = preferred->second.heterogeneousRoot;
        }
        for (const auto& className : classNames) {
            const auto klass = classesByName_.find(className);
            if (klass == classesByName_.end() ||
                klass->second.heterogeneousRoot.empty()) {
                return failure(
                    "ordinary object arrays cannot mix class " +
                    className);
            }
            if (heterogeneousRoot.empty()) {
                heterogeneousRoot = klass->second.heterogeneousRoot;
            } else if (heterogeneousRoot !=
                       klass->second.heterogeneousRoot) {
                return failure(
                    "object array classes belong to distinct heterogeneous "
                    "hierarchies");
            }
        }

        std::vector<std::string> commonClasses;
        for (const auto& [candidateName, candidate] : classesByName_) {
            if (candidate.heterogeneousRoot != heterogeneousRoot) {
                continue;
            }
            if (std::all_of(
                    classNames.begin(), classNames.end(),
                    [&](const std::string& className) {
                        return classDerivesFrom(className, candidateName);
                    })) {
                commonClasses.push_back(candidateName);
            }
        }

        std::vector<std::string> mostSpecific;
        for (const auto& candidate : commonClasses) {
            const bool shadowed = std::any_of(
                commonClasses.begin(), commonClasses.end(),
                [&](const std::string& other) {
                    return other != candidate &&
                           classDerivesFrom(other, candidate);
                });
            if (!shadowed) {
                mostSpecific.push_back(candidate);
            }
        }
        if (mostSpecific.size() != 1) {
            return failure(
                mostSpecific.empty()
                    ? "object array classes have no common heterogeneous superclass"
                    : "object array classes have an ambiguous common superclass");
        }
        return RuntimeObjectClassResolutionResult{
            true, std::move(mostSpecific.front()), {}};
    }

    RuntimeObjectOperationResult constructDefaultObjectForArray(
        const BytecodeInstruction& instruction,
        std::string_view className) {
        const size_t diagnosticCount = diagnostics_.size();
        auto outputs = callClassConstructor(
            instruction, std::string(className), {}, 1);
        if (diagnostics_.size() != diagnosticCount ||
            outputs.size() != 1 ||
            !isRuntimeScalarObject(outputs.front())) {
            return RuntimeObjectOperationResult{};
        }
        return RuntimeObjectOperationResult{
            true, std::move(outputs.front()), {}};
    }

    RuntimeObjectArrayPolicy objectArrayPolicy(
        const BytecodeInstruction& instruction) {
        RuntimeObjectArrayPolicy policy;
        policy.resolveCommonClass =
            [this](const std::vector<std::string>& classNames,
                   std::string_view preferredClassName) {
                return resolveObjectArrayClass(
                    classNames, preferredClassName);
            };
        policy.constructDefault =
            [this, &instruction](std::string_view className) {
                return constructDefaultObjectForArray(
                    instruction, className);
            };
        return policy;
    }

    bool reflectableClassExists(std::string_view className) const {
        const std::string canonical =
            canonicalRuntimeMetadataClassName(className);
        return classesByName_.contains(canonical) ||
               isBuiltinReflectableClass(canonical);
    }

    bool reflectableClassDerivesFrom(
        std::string_view className,
        std::string_view possibleSuperclass) const {
        const std::string actual =
            canonicalRuntimeMetadataClassName(className);
        const std::string expected =
            canonicalRuntimeMetadataClassName(possibleSuperclass);
        if (actual == expected) {
            return true;
        }

        const RuntimeValue metadataProbe =
            makeRuntimeMetadataObject(RuntimeMetadataKind::MetaData,
                                      "<probe>");
        RuntimeValue typedProbe = metadataProbe;
        typedProbe.className = actual;
        if (runtimeMetadataIsa(typedProbe, expected)) {
            return true;
        }

        if (expected == "numeric" && actual == "double") {
            return true;
        }
        return classDerivesFrom(actual, expected);
    }

    RuntimeValue metadataClassValue(std::string className) const {
        return makeRuntimeMetadataObject(
            RuntimeMetadataKind::Class,
            canonicalRuntimeMetadataClassName(className));
    }

    RuntimeValue metadataClassArray(
        const std::vector<std::string>& classNames) const {
        std::vector<RuntimeValue> values;
        values.reserve(classNames.size());
        for (const auto& className : classNames) {
            values.push_back(metadataClassValue(className));
        }
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::Class, std::move(values),
            {classNames.size(), 1});
    }

    RuntimeValue emptyMetadataArray(RuntimeMetadataKind kind) const {
        return makeRuntimeMetadataArray(kind, {}, {0, 0});
    }

    const FunctionInfo*
    functionForMetadata(std::string_view identity) const {
        const auto function =
            functionsByName_.find(std::string(identity));
        return function == functionsByName_.end() ? nullptr
                                                  : &function->second;
    }

    const FunctionInfo*
    functionForPublicMetadataIdentifier(
        std::string_view identifier) const {
        if (ambiguousFunctionMetadataIdentifiers_.contains(
                std::string(identifier))) {
            return nullptr;
        }
        const auto identity = functionsByMetadataIdentifier_.find(
            std::string(identifier));
        return identity == functionsByMetadataIdentifier_.end()
                   ? nullptr
                   : functionForMetadata(identity->second);
    }

    std::string functionCallSignatureIdentity(
        const FunctionInfo& function) const {
        return "function/" + function.name;
    }

    std::string methodCallSignatureIdentity(
        std::string_view methodIdentity) const {
        return "method/" + std::string(methodIdentity);
    }

    const FunctionInfo* callableForCallSignature(
        std::string_view identity) const {
        constexpr std::string_view functionPrefix = "function/";
        constexpr std::string_view methodPrefix = "method/";
        if (identity.starts_with(functionPrefix)) {
            return functionForMetadata(
                identity.substr(functionPrefix.size()));
        }
        if (identity.starts_with(methodPrefix)) {
            return methodForMetadata(
                identity.substr(methodPrefix.size()));
        }
        return nullptr;
    }

    const ArgumentContract*
    reflectedArgumentContract(const FunctionInfo& info,
                              const std::string& name,
                              ArgumentBlockKind primary,
                              ArgumentBlockKind alternate) const {
        if (const auto* contract =
                argumentContract(info, name, primary)) {
            return contract;
        }
        return argumentContract(info, name, alternate);
    }

    std::vector<ReflectedArgument> reflectedInputArguments(
        const FunctionInfo& info) const {
        std::vector<ReflectedArgument> arguments;
        const size_t required =
            functionRequiredPositionalParameterCount(info.signature);
        for (size_t index = 0; index < info.signature.parameters.size();
             ++index) {
            const auto kind =
                functionParameterKind(info.signature, index);
            if (kind == FunctionParameterKind::NameValue) {
                continue;
            }
            const std::string& name = info.signature.parameters[index];
            const bool repeating =
                kind == FunctionParameterKind::Repeating;
            arguments.push_back(ReflectedArgument{
                name,
                name,
                {},
                reflectedArgumentContract(
                    info, name,
                    repeating ? ArgumentBlockKind::RepeatingInput
                              : ArgumentBlockKind::Input,
                    repeating ? ArgumentBlockKind::Input
                              : ArgumentBlockKind::RepeatingInput),
                !repeating && index < required,
                repeating,
                false});
        }
        if (info.signature.hasVarargin) {
            arguments.push_back(ReflectedArgument{
                "varargin",
                "varargin",
                {},
                reflectedArgumentContract(
                    info, "varargin", ArgumentBlockKind::RepeatingInput,
                    ArgumentBlockKind::Input),
                false,
                true,
                false});
        }
        for (const auto& contract : info.argumentContracts) {
            if (contract.blockKind != ArgumentBlockKind::Input) {
                continue;
            }
            const size_t dot = contract.name.find('.');
            if (dot == std::string::npos || dot == 0 ||
                dot + 1 >= contract.name.size()) {
                continue;
            }
            const std::string group = contract.name.substr(0, dot);
            const auto parameter = std::find(
                info.signature.parameters.begin(),
                info.signature.parameters.end(), group);
            if (parameter == info.signature.parameters.end()) {
                continue;
            }
            const size_t parameterIndex = static_cast<size_t>(
                std::distance(info.signature.parameters.begin(), parameter));
            if (functionParameterKind(info.signature, parameterIndex) !=
                FunctionParameterKind::NameValue) {
                continue;
            }
            arguments.push_back(ReflectedArgument{
                contract.name,
                contract.name.substr(dot + 1),
                group,
                &contract,
                false,
                false,
                true});
        }
        return arguments;
    }

    std::vector<ReflectedArgument> reflectedOutputArguments(
        const FunctionInfo& info) const {
        std::vector<ReflectedArgument> arguments;
        const std::string_view repeatingName =
            functionRepeatingOutputName(info.signature);
        for (const auto& name : info.signature.outputs) {
            const bool repeating = name == repeatingName;
            arguments.push_back(ReflectedArgument{
                name,
                name,
                {},
                reflectedArgumentContract(
                    info, name,
                    repeating ? ArgumentBlockKind::RepeatingOutput
                              : ArgumentBlockKind::Output,
                    repeating ? ArgumentBlockKind::Output
                              : ArgumentBlockKind::RepeatingOutput),
                !repeating,
                repeating,
                false});
        }
        if (info.signature.hasVarargout &&
            std::find(info.signature.outputs.begin(),
                      info.signature.outputs.end(), "varargout") ==
                info.signature.outputs.end()) {
            arguments.push_back(ReflectedArgument{
                "varargout",
                "varargout",
                {},
                reflectedArgumentContract(
                    info, "varargout", ArgumentBlockKind::RepeatingOutput,
                    ArgumentBlockKind::Output),
                false,
                true,
                false});
        }
        return arguments;
    }

    std::optional<ReflectedArgument> reflectedArgumentForMetadata(
        std::string_view identity,
        std::string* signatureIdentity = nullptr) const {
        const auto parse = [&](std::string_view marker, bool input)
            -> std::optional<ReflectedArgument> {
            const size_t separator = identity.rfind(marker);
            if (separator == std::string_view::npos ||
                separator + marker.size() >= identity.size()) {
                return std::nullopt;
            }
            const std::string indexText(
                identity.substr(separator + marker.size()));
            char* end = nullptr;
            const unsigned long long parsed =
                std::strtoull(indexText.c_str(), &end, 10);
            if (!end || *end != '\0') {
                return std::nullopt;
            }
            const std::string signature(identity.substr(0, separator));
            const FunctionInfo* callable =
                callableForCallSignature(signature);
            if (!callable) {
                return std::nullopt;
            }
            const auto arguments =
                input ? reflectedInputArguments(*callable)
                      : reflectedOutputArguments(*callable);
            if (parsed >= arguments.size()) {
                return std::nullopt;
            }
            if (signatureIdentity) {
                *signatureIdentity = signature;
            }
            return arguments[static_cast<size_t>(parsed)];
        };
        if (const auto input = parse("/input/", true)) {
            return input;
        }
        return parse("/output/", false);
    }

    RuntimeValue argumentIdentifierValue(
        std::string_view name, std::string_view groupName = {}) const {
        return makeRuntimeMetadataObject(
            RuntimeMetadataKind::ArgumentIdentifier,
            std::string(groupName) + "/" + std::string(name));
    }

    RuntimeValue referencedArgumentIdentifiers(
        const FunctionInfo& info,
        const std::vector<std::string>& references) const {
        const auto inputs = reflectedInputArguments(info);
        const auto outputs = reflectedOutputArguments(info);
        std::vector<RuntimeValue> values;
        std::set<std::string> seen;
        const auto append = [&](const ReflectedArgument& argument,
                                std::vector<RuntimeValue>& destination) {
            const std::string identity =
                argument.groupName + "/" + argument.identifierName;
            if (seen.insert(identity).second) {
                destination.push_back(argumentIdentifierValue(
                    argument.identifierName, argument.groupName));
            }
        };
        for (const auto& rawReference : references) {
            const std::string reference = trimAscii(rawReference);
            const auto input = std::find_if(
                inputs.begin(), inputs.end(),
                [&](const ReflectedArgument& argument) {
                    return argument.name == reference ||
                           argument.identifierName == reference;
                });
            if (input != inputs.end()) {
                append(*input, values);
                continue;
            }
            const auto output = std::find_if(
                outputs.begin(), outputs.end(),
                [&](const ReflectedArgument& argument) {
                    return argument.name == reference ||
                           argument.identifierName == reference;
                });
            if (output != outputs.end()) {
                append(*output, values);
            }
        }
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::ArgumentIdentifier,
            std::move(values), {1, valueCount});
    }

    std::string propertyMetadataIdentity(
        const ClassInfo& viewClass,
        const PropertyInfo& property) const {
        return viewClass.name + "/" + property.storageKey;
    }

    std::string methodMetadataIdentity(
        const ClassInfo& viewClass,
        const FunctionInfo& method) const {
        return viewClass.name + "/" + method.declaringClass + "/" +
               method.name;
    }

    std::string eventMetadataIdentity(
        const ClassInfo& viewClass, const EventInfo& event) const {
        return viewClass.name + "/" + event.declaringClass + "/" +
               event.name;
    }

    PropertyInfoPtr propertyForMetadata(
        std::string_view identity) const {
        const auto [viewClassName, storageKey] =
            splitMetadataIdentity(identity);
        const auto view = classesByName_.find(viewClassName);
        if (view == classesByName_.end() || storageKey.empty()) {
            return nullptr;
        }
        const auto property = std::find_if(
            view->second.propertyOrder.begin(),
            view->second.propertyOrder.end(),
            [&](const PropertyInfoPtr& candidate) {
                return candidate->storageKey == storageKey;
            });
        if (property != view->second.propertyOrder.end()) {
            return *property;
        }
        for (const auto& [name, candidate] :
             view->second.abstractProperties) {
            (void)name;
            if (candidate->storageKey == storageKey) {
                return candidate;
            }
        }
        return nullptr;
    }

    const FunctionInfo* methodForMetadata(
        std::string_view identity) const {
        const auto [viewClassName, memberIdentity] =
            splitMetadataIdentity(identity);
        (void)viewClassName;
        const auto [declaringClass, methodName] =
            splitMetadataIdentity(memberIdentity);
        const auto owner = classesByName_.find(declaringClass);
        if (owner == classesByName_.end() || methodName.empty()) {
            return nullptr;
        }
        const auto method =
            owner->second.declaredMethods.find(methodName);
        return method == owner->second.declaredMethods.end()
                   ? nullptr
                   : &method->second;
    }

    const EventInfo& handleDestructionEvent() const {
        static const EventInfo event = [] {
            EventInfo value;
            value.name = std::string(kObjectBeingDestroyedEventName);
            value.declaringClass = "handle";
            value.notifyAccess = MemberAccessPolicy{
                MemberAccessLevel::Private, {}, false, true};
            return value;
        }();
        return event;
    }

    const EventInfo* eventForMetadata(
        std::string_view identity) const {
        const auto [viewClassName, memberIdentity] =
            splitMetadataIdentity(identity);
        (void)viewClassName;
        const auto [declaringClass, eventName] =
            splitMetadataIdentity(memberIdentity);
        if (declaringClass == "handle" &&
            eventName == kObjectBeingDestroyedEventName) {
            return &handleDestructionEvent();
        }
        const auto owner = classesByName_.find(declaringClass);
        if (owner == classesByName_.end() || eventName.empty()) {
            return nullptr;
        }
        const auto event =
            owner->second.declaredEvents.find(eventName);
        return event == owner->second.declaredEvents.end()
                   ? nullptr
                   : &event->second;
    }

    const EnumerationMemberInfo* enumerationMemberForMetadata(
        std::string_view identity) const {
        const auto [className, memberName] =
            splitMetadataIdentity(identity);
        const auto owner = classesByName_.find(className);
        if (owner == classesByName_.end() || memberName.empty()) {
            return nullptr;
        }
        const auto member =
            owner->second.declaredEnumerationMembers.find(memberName);
        return member ==
                       owner->second.declaredEnumerationMembers.end()
                   ? nullptr
                   : &member->second;
    }

    bool policyAllowsClass(const MemberAccessPolicy& access,
                           const std::string& declaringClass,
                           const std::string& requestingClass) const {
        if (access.level == MemberAccessLevel::Public) {
            return true;
        }
        if (requestingClass.empty()) {
            return false;
        }
        if (access.level == MemberAccessLevel::Private) {
            return requestingClass == declaringClass;
        }
        if (access.level == MemberAccessLevel::Protected) {
            return classDerivesFrom(requestingClass, declaringClass);
        }
        if (access.level == MemberAccessLevel::ClassList) {
            if (requestingClass == declaringClass) {
                return true;
            }
            for (const auto& allowedClass : access.classNames) {
                if (classDerivesFrom(requestingClass, allowedClass)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool hasMemberAccess(const MemberAccessPolicy& access,
                         const std::string& declaringClass) const {
        if (access.level == MemberAccessLevel::Public) {
            return true;
        }
        if (activeClassFunctions_.empty()) {
            return false;
        }
        const auto& active = activeClassFunctions_.back();
        if (access.level == MemberAccessLevel::Immutable) {
            return active.className == declaringClass &&
                   active.methodName == declaringClass &&
                   active.construction != nullptr;
        }
        if (access.level == MemberAccessLevel::Protected &&
            classDerivesFrom(declaringClass, active.className)) {
            return true;
        }
        return policyAllowsClass(access, declaringClass, active.className);
    }

    bool hasConstructorAccess(const MemberAccessPolicy& access,
                              const std::string& declaringClass,
                              const std::string& requestingClass) const {
        return policyAllowsClass(access, declaringClass, requestingClass);
    }

    bool activePropertyGetter(const PropertyInfo& property) const {
        return !activeClassFunctions_.empty() &&
               activeClassFunctions_.back().className ==
                   property.declaringClass &&
               activeClassFunctions_.back().methodName == property.getterName;
    }

    bool activePropertyWriter(const PropertyInfo& property) const {
        if (activeClassFunctions_.empty() ||
            activeClassFunctions_.back().className !=
                property.declaringClass) {
            return false;
        }
        const auto& method = activeClassFunctions_.back().methodName;
        return method == property.getterName || method == property.setterName;
    }

    void reportClassHierarchyDiagnostic(const ClassInfo& info,
                                        std::string key,
                                        std::string message) {
        if (!classHierarchyDiagnosticKeys_.insert(std::move(key)).second) {
            return;
        }
        diagnostics_.push_back(Diagnostic{info.span, std::move(message)});
    }

    void collectTypedRegions(const BytecodeTypedIrModule* typedIr) {
        if (!typedIr) {
            return;
        }

        std::map<size_t, size_t> idCounts;
        std::map<size_t, size_t> loopSourceCounts;
        std::map<size_t, size_t> denseSourceCounts;
        for (const auto& region : typedIr->regions) {
            ++idCounts[region.id];
            if (region.kind == "scalar-loop") {
                ++loopSourceCounts[region.sourcePc];
            } else if (region.kind == "dense-elementwise" ||
                       region.kind == "dense-reduction") {
                ++denseSourceCounts[region.sourcePc];
            }
        }
        BytecodeRegionAnalyzer regionAnalyzer(
            semantic_ ? semantic_->builtinRegistry : nullptr);

        for (const auto& region : typedIr->regions) {
            BytecodeTypedRegionExecutionProfile execution;
            execution.regionId = region.id;
            execution.sourcePc = region.sourcePc;
            execution.kind = region.kind;
            execution.target = region.target;
            execution.eligible =
                region.region.eligibleForTypedExecution;
            execution.backend = "none";
            execution.lastFallbackKind =
                region.region.fallbackKind;
            execution.lastReason = region.region.reason;

            if (!region.region.eligibleForTypedExecution) {
                typedRegionExecutions_[region.id] =
                    std::move(execution);
                continue;
            }

            if (region.kind == "scalar-loop") {
                const auto expected =
                    BytecodeVmTrustedAccess::analyzeRegion(
                        regionAnalyzer, *program_, "hot-loop",
                        region.sourcePc, region.target);
                const bool sourceMatches =
                    region.sourcePc < program_->instructions.size() &&
                    program_->instructions[region.sourcePc].op ==
                        BytecodeOp::ForBegin &&
                    region.target ==
                        program_->instructions[region.sourcePc].operand;
                const bool unique =
                    idCounts[region.id] == 1 &&
                    loopSourceCounts[region.sourcePc] == 1;
                if (sourceMatches && unique &&
                    expected.eligibleForTypedExecution &&
                    bytecodeRegionContractsEquivalent(
                        region.region, expected)) {
                    typedRegionExecutions_[region.id] = execution;
                    typedLoopRegions_[region.sourcePc] =
                        ActiveTypedLoopRegion{
                            region.id, region.kind, region.target,
                            expected};
                    continue;
                }
            } else if (region.kind == "dense-elementwise" ||
                       region.kind == "dense-reduction") {
                const auto expected =
                    BytecodeVmTrustedAccess::analyzeRegion(
                        regionAnalyzer, *program_,
                        "dense-array-assignment", region.sourcePc,
                        region.target);
                const bool sourceMatches =
                    region.sourcePc < program_->instructions.size() &&
                    program_->instructions[region.sourcePc].op ==
                        BytecodeOp::StoreName &&
                    program_->instructions[region.sourcePc].operand ==
                        region.target &&
                    expected.bodyEndPc == region.sourcePc &&
                    expected.endPc == region.sourcePc + 1;
                const bool kindMatches =
                    (region.kind == "dense-reduction") ==
                    (expected.reductionOperationCount != 0);
                std::set<std::string, std::less<>> guardedInputs;
                bool guardsMatch =
                    region.guards.size() == expected.inputs.size();
                for (const auto& guard : region.guards) {
                    const bool knownInput =
                        std::find(expected.inputs.begin(),
                                  expected.inputs.end(), guard.role) !=
                        expected.inputs.end();
                    const bool numericKind =
                        guard.value.kind == "numeric" ||
                        guard.value.kind == "number" ||
                        guard.value.kind == "vector" ||
                        guard.value.kind == "matrix";
                    const bool numericClassContract =
                        guard.value.numericClass == "double" ||
                        guard.value.numericClass == "single" ||
                        (guard.value.numericClass == "floating" &&
                         !guard.value.numericClassKnown);
                    guardsMatch = guardsMatch &&
                                  guard.source == "region-input" &&
                                  knownInput && numericKind &&
                                  numericClassContract &&
                                  guardedInputs.insert(guard.role).second;
                }
                const bool unique =
                    idCounts[region.id] == 1 &&
                    denseSourceCounts[region.sourcePc] == 1 &&
                    !typedDenseRegions_.contains(expected.beginPc);
                if (sourceMatches && kindMatches && guardsMatch && unique &&
                    expected.eligibleForTypedExecution &&
                    bytecodeRegionContractsEquivalent(
                        region.region, expected)) {
                    typedRegionExecutions_[region.id] = execution;
                    typedDenseRegions_[expected.beginPc] =
                        ActiveTypedDenseRegion{
                            region.id, expected, region.guards};
                    continue;
                }
            } else {
                typedRegionExecutions_[region.id] =
                    std::move(execution);
                continue;
            }

            execution.eligible = false;
            execution.lastFallbackKind =
                RuntimeFallbackKind::InvalidContract;
            execution.lastReason =
                "typed region contract does not match its bytecode program";
            typedRegionExecutions_[region.id] = std::move(execution);
        }
    }

    bool hasTopLevelExecutable(const BytecodeProgram& program) const {
        size_t nestedDepth = 0;
        for (const auto& instruction : program.instructions) {
            if (isBoundaryLeave(instruction.op) && nestedDepth > 0) {
                --nestedDepth;
                continue;
            }
            if (nestedDepth == 0 && isTopLevelRuntimeOp(instruction.op)) {
                return true;
            }
            if (isBoundaryEnter(instruction.op)) {
                ++nestedDepth;
            }
        }
        return false;
    }

    void execute(bool scriptMode) {
        scriptModeActive_ = scriptMode;
        bool activeFunction = scriptMode;
        bool ranFirstFunction = false;
        bool enteredProfile = false;
        bool enteredExceptionFunction = false;
        bool enteredPersistentFunction = false;
        bool enteredActiveFunctionFrame = false;
        size_t skipDepth = 0;

        if (scriptMode) {
            enterFunctionProfile(std::string(kScriptProfileName),
                                 SourceSpan{});
            enteredProfile = true;
        }

        size_t pc = 0;
        while (pc < program_->instructions.size()) {
            const auto& instruction = program_->instructions[pc];

            if (skipDepth > 0) {
                if (isBoundaryEnter(instruction.op)) {
                    ++skipDepth;
                } else if (isBoundaryLeave(instruction.op)) {
                    --skipDepth;
                }
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::EnterModule ||
                instruction.op == BytecodeOp::LeaveModule) {
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::EnterClass) {
                skipDepth = 1;
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::EnterFunction) {
                const bool selected = requestedEntryFunction_.empty() ||
                                      instruction.operand ==
                                          requestedEntryFunction_;
                if (scriptMode || ranFirstFunction || !selected) {
                    skipDepth = 1;
                    ++pc;
                    continue;
                }
                if (!prepareEntryFunction(instruction)) {
                    break;
                }
                enterFunctionProfile(instruction.operand, instruction.span);
                activeExceptionFunctionNames_.push_back(
                    publicFunctionIdentifier(instruction.operand));
                activePersistentFunctionKeys_.push_back(
                    persistentFunctionKey(instruction.operand));
                enteredProfile = true;
                enteredExceptionFunction = true;
                enteredPersistentFunction = true;
                if (const auto function =
                        functionsByName_.find(instruction.operand);
                    function != functionsByName_.end()) {
                    activeFunctionFrames_.push_back(ActiveFunctionFrame{
                        function->first, frames_.size() - 1});
                    enteredActiveFunctionFrame = true;
                }
                activeFunction = true;
                ranFirstFunction = true;
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::LeaveFunction) {
                if (activeFunction && !scriptMode) {
                    leaveFunctionProfile();
                    if (enteredExceptionFunction) {
                        activeExceptionFunctionNames_.pop_back();
                    }
                    if (enteredPersistentFunction) {
                        activePersistentFunctionKeys_.pop_back();
                    }
                    if (enteredActiveFunctionFrame) {
                        activeFunctionFrames_.pop_back();
                    }
                    enteredProfile = false;
                    enteredExceptionFunction = false;
                    enteredPersistentFunction = false;
                    enteredActiveFunctionFrame = false;
                    activeFunction = false;
                    break;
                }
                ++pc;
                continue;
            }

            if (!activeFunction) {
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::EnterControl) {
                addDiagnostic(instruction,
                              "bytecode VM does not execute control blocks "
                              "yet");
                break;
            }

            if (!beforeControlledInstruction(instruction.span)) {
                break;
            }
            recordInstruction(pc, instruction);
            const auto nextPc = executeInstruction(instruction);
            ++executedInstructionCount_;
            if (!afterControlledInstruction(instruction.span)) {
                break;
            }
            if (!diagnostics_.empty()) {
                if (const auto recovery = recoverTryDiagnostic()) {
                    pc = *recovery;
                    continue;
                }
                break;
            }
            if (returnRequested_) {
                break;
            }
            pc = nextPc.value_or(pc + 1);
        }

        if (enteredProfile) {
            leaveFunctionProfile();
        }
        if (enteredExceptionFunction &&
            !activeExceptionFunctionNames_.empty()) {
            activeExceptionFunctionNames_.pop_back();
        }
        if (enteredPersistentFunction &&
            !activePersistentFunctionKeys_.empty()) {
            activePersistentFunctionKeys_.pop_back();
        }
        if (enteredActiveFunctionFrame &&
            !activeFunctionFrames_.empty()) {
            activeFunctionFrames_.pop_back();
        }
        scriptModeActive_ = false;
        if (!scriptMode && !requestedEntryFunction_.empty() &&
            !ranFirstFunction && diagnostics_.empty()) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "entry function is not available: " +
                                  requestedEntryFunction_});
        }
    }

    const ArgumentContract* argumentContract(
        const FunctionInfo& info, std::string_view name,
        ArgumentBlockKind blockKind) const {
        const auto found = std::find_if(
            info.argumentContracts.begin(), info.argumentContracts.end(),
            [&](const ArgumentContract& candidate) {
                return candidate.blockKind == blockKind &&
                       candidate.name == name;
            });
        return found == info.argumentContracts.end() ? nullptr : &*found;
    }

    bool validateFunctionOutputs(
        const std::string& name, const FunctionInfo& info,
        RuntimeWorkspace& frame) {
        std::vector<RuntimeOutputArgumentContract> contracts;
        for (const auto& contract : info.argumentContracts) {
            if (contract.blockKind != ArgumentBlockKind::Output &&
                contract.blockKind != ArgumentBlockKind::RepeatingOutput) {
                continue;
            }
            contracts.push_back(RuntimeOutputArgumentContract{
                contract.name, contract.spec, contract.span,
                contract.blockKind == ArgumentBlockKind::RepeatingOutput});
        }

        RuntimeArgumentValidationOptions options;
        options.objectIsA =
            [this](const std::string& actual, const std::string& expected) {
                return classDerivesFrom(actual, expected);
            };
        options.classAvailable = [this](const std::string& className) {
            return classesByName_.contains(className);
        };
        const auto validation =
            validateRuntimeFunctionOutputs(frame, contracts, options);
        if (validation.succeeded) {
            return true;
        }
        diagnostics_.push_back(Diagnostic{
            validation.span,
            "output argument validation failed for " + name + "." +
                validation.argumentName + ": " + validation.error});
        return false;
    }

    std::optional<RuntimeValue> evaluateArgumentDefault(
        const BytecodeInstruction& instruction, const std::string& functionName,
        const ArgumentContract& contract) {
        if (!contract.hasDefaultRange) {
            addDiagnostic(instruction,
                          "argument default expression has no bytecode range: " +
                              functionName + "." + contract.name);
            return std::nullopt;
        }

        auto savedStack = std::move(stack_);
        auto savedForLoops = std::move(forLoopStack_);
        auto savedIndexContexts = std::move(indexContextStack_);
        auto savedSwitchContexts = std::move(switchContextStack_);
        auto savedTryContexts = std::move(tryContextStack_);
        const bool savedReturnRequested = returnRequested_;
        stack_.clear();
        forLoopStack_.clear();
        indexContextStack_.clear();
        switchContextStack_.clear();
        tryContextStack_.clear();
        returnRequested_ = false;

        const size_t diagnosticCount = diagnostics_.size();
        executeFunctionBody(contract.defaultEntry, contract.defaultEnd);
        std::optional<RuntimeValue> value;
        if (diagnostics_.size() == diagnosticCount) {
            if (stack_.size() != 1) {
                addDiagnostic(instruction,
                              "argument default expression must produce one "
                              "value: " + functionName + "." + contract.name);
            } else {
                value = popRuntime(instruction, "argument default expression");
            }
        }

        stack_ = std::move(savedStack);
        forLoopStack_ = std::move(savedForLoops);
        indexContextStack_ = std::move(savedIndexContexts);
        switchContextStack_ = std::move(savedSwitchContexts);
        tryContextStack_ = std::move(savedTryContexts);
        returnRequested_ = savedReturnRequested;
        return value;
    }

    std::optional<ValidatedFunctionArguments> validateFunctionArguments(
        const BytecodeInstruction& instruction, const std::string& name,
        const FunctionInfo& info,
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount) {
        std::vector<std::string> nameValueDeclarations;
        for (const auto& contract : info.argumentContracts) {
            if (contract.blockKind == ArgumentBlockKind::Input &&
                contract.name.find('.') != std::string::npos) {
                nameValueDeclarations.push_back(contract.name);
            }
        }
        auto normalized = normalizeRuntimeInvocationArguments(
            info.signature, nameValueDeclarations, arguments);
        if (!normalized.succeeded) {
            addDiagnostic(instruction, "function invocation failed for " + name +
                                           ": " + normalized.error);
            return std::nullopt;
        }
        setRuntimeCallFrameArity(
            frames_.back(), normalized.positionalArgumentCount,
            requestedOutputCount);
        const auto& positionalArguments = normalized.positionalArguments;

        const size_t fixedParameterCount =
            functionPositionalParameterCount(info.signature);
        const size_t repeatingGroupWidth =
            functionRepeatingParameterCount(info.signature);
        const size_t repeatingValueCount =
            positionalArguments.size() > fixedParameterCount
                ? positionalArguments.size() - fixedParameterCount
                : 0;

        RuntimeArgumentValidationOptions validationOptions;
        validationOptions.objectIsA =
            [this](const std::string& actual, const std::string& expected) {
                return classDerivesFrom(actual, expected);
            };
        validationOptions.classAvailable = [this](const std::string& className) {
            return classesByName_.contains(className);
        };
        auto validateValue = [&](RuntimeValue value,
                                 const ArgumentContract* contract,
                                 std::optional<size_t> occurrence =
                                     std::nullopt)
            -> std::optional<RuntimeValue> {
            if (contract == nullptr) {
                return value;
            }
            auto result = validateRuntimeArgument(
                std::move(value), contract->spec, validationOptions);
            if (!result.succeeded) {
                std::string argumentName = contract->name;
                if (occurrence) {
                    argumentName += "{" + std::to_string(*occurrence + 1) +
                                    "}";
                }
                addDiagnostic(instruction,
                              "argument validation failed for " + name + "." +
                                  argumentName + ": " +
                                  std::move(result.error));
                return std::nullopt;
            }
            return std::move(result.value);
        };

        std::vector<RuntimeValue> validated;
        validated.reserve(std::max(positionalArguments.size(),
                                   info.signature.parameters.size()));
        for (size_t index = 0; index < fixedParameterCount; ++index) {
            const std::string& parameterName = info.signature.parameters[index];
            const ArgumentContract* contract = argumentContract(
                info, parameterName, ArgumentBlockKind::Input);
            RuntimeValue value;
            if (index < positionalArguments.size()) {
                value = positionalArguments[index];
            } else if (contract != nullptr &&
                       contract->spec.hasExplicitDefault) {
                auto defaultValue =
                    evaluateArgumentDefault(instruction, name, *contract);
                if (!defaultValue) {
                    return std::nullopt;
                }
                value = std::move(*defaultValue);
            } else {
                addDiagnostic(instruction,
                              "required argument is missing for " + name +
                                  ": " + parameterName);
                return std::nullopt;
            }

            auto result = validateValue(std::move(value), contract);
            if (!result) {
                return std::nullopt;
            }
            currentFrame()[parameterName] = *result;
            validated.push_back(std::move(*result));
        }

        if (repeatingGroupWidth != 0) {
            const size_t occurrenceCount =
                positionalArguments.size() < fixedParameterCount
                    ? 0
                    : repeatingValueCount / repeatingGroupWidth;
            size_t groupIndex = 0;
            for (size_t index = fixedParameterCount;
                 index < info.signature.parameters.size(); ++index) {
                if (functionParameterKind(info.signature, index) !=
                    FunctionParameterKind::Repeating) {
                    continue;
                }
                const std::string& parameterName =
                    info.signature.parameters[index];
                const ArgumentContract* contract = argumentContract(
                    info, parameterName, ArgumentBlockKind::RepeatingInput);
                std::vector<RuntimeValue> values;
                values.reserve(occurrenceCount);
                for (size_t occurrence = 0; occurrence < occurrenceCount;
                     ++occurrence) {
                    const size_t argumentIndex =
                        fixedParameterCount +
                        occurrence * repeatingGroupWidth + groupIndex;
                    auto result = validateValue(positionalArguments[argumentIndex],
                                                contract, occurrence);
                    if (!result) {
                        return std::nullopt;
                    }
                    values.push_back(std::move(*result));
                }
                RuntimeValue cell = cellValue(std::move(values));
                currentFrame()[parameterName] = cell;
                validated.push_back(std::move(cell));
                ++groupIndex;
            }
        }

        RuntimeWorkspace nameValueStructures;
        for (size_t index = 0; index < info.signature.parameters.size();
             ++index) {
            if (functionParameterKind(info.signature, index) ==
                FunctionParameterKind::NameValue) {
                RuntimeValue structure = makeRuntimeStructValue();
                nameValueStructures.emplace(info.signature.parameters[index],
                                            structure);
                currentFrame()[info.signature.parameters[index]] =
                    std::move(structure);
            }
        }
        for (const auto& contract : info.argumentContracts) {
            const size_t dot = contract.name.find('.');
            if (contract.blockKind != ArgumentBlockKind::Input ||
                dot == std::string::npos) {
                continue;
            }
            const std::string root = contract.name.substr(0, dot);
            const std::string field = contract.name.substr(dot + 1);
            auto structure = nameValueStructures.find(root);
            if (structure == nameValueStructures.end()) {
                continue;
            }

            std::optional<RuntimeValue> value;
            if (const auto supplied =
                    normalized.nameValueArguments.find(contract.name);
                supplied != normalized.nameValueArguments.end()) {
                value = supplied->second;
            } else if (contract.spec.hasExplicitDefault) {
                value = evaluateArgumentDefault(instruction, name, contract);
                if (!value) {
                    return std::nullopt;
                }
            }
            if (!value) {
                continue;
            }
            auto result = validateValue(std::move(*value), &contract);
            if (!result) {
                return std::nullopt;
            }
            runtimeSetStructField(structure->second, field,
                                  std::move(*result));
            currentFrame()[root] = structure->second;
        }
        for (size_t index = 0; index < info.signature.parameters.size();
             ++index) {
            if (functionParameterKind(info.signature, index) !=
                FunctionParameterKind::NameValue) {
                continue;
            }
            const std::string& root = info.signature.parameters[index];
            currentFrame()[root] = nameValueStructures[root];
            validated.push_back(nameValueStructures[root]);
        }

        if (info.signature.hasVarargin) {
            const ArgumentContract* contract = argumentContract(
                info, "varargin", ArgumentBlockKind::RepeatingInput);
            const size_t begin =
                repeatingGroupWidth == 0
                    ? std::min(positionalArguments.size(), fixedParameterCount)
                    : positionalArguments.size();
            for (size_t index = begin; index < positionalArguments.size();
                 ++index) {
                auto result = validateValue(positionalArguments[index], contract,
                                            index - begin);
                if (!result) {
                    return std::nullopt;
                }
                validated.push_back(std::move(*result));
            }
        }
        return ValidatedFunctionArguments{
            std::move(validated), normalized.positionalArgumentCount};
    }

    bool prepareEntryFunction(const BytecodeInstruction& instruction) {
        const auto function = functionsByName_.find(instruction.operand);
        if (function == functionsByName_.end()) {
            return true;
        }

        const auto& signature = function->second.signature;
        const size_t requestedOutputCount =
            requestedEntryOutputCount_.value_or(signature.outputs.size());
        if (!functionOutputCountIsValid(signature, requestedOutputCount)) {
            addDiagnostic(instruction,
                          "function output count mismatch for: " +
                              instruction.operand);
            return false;
        }

        RuntimeWorkspace entryWorkspace =
            std::move(frames_.front().workspace);
        frames_.front() = makeRuntimeFunctionFrame(
            RuntimeCallFrameKind::Function, instruction.operand,
            instruction.span, entryArguments_.size(),
            requestedOutputCount,
            std::move(entryWorkspace));
        configurePersistentScope(frames_.front(), function->second);
        auto validated = validateFunctionArguments(
            instruction, instruction.operand, function->second,
            entryArguments_, requestedOutputCount);
        if (!validated) {
            return false;
        }
        const size_t providedArgumentCount =
            validated->positionalArgumentCount;
        entryArguments_ = std::move(validated->values);

        initializeFunctionFrame(signature, entryArguments_, requestedOutputCount,
                                providedArgumentCount);
        executedEntryFunction_ = instruction.operand;
        executedRequestedOutputCount_ = requestedOutputCount;
        entrySignature_ = signature;
        if (profilingEnabled_) {
            auto& profile = functionEntryProfiles_[instruction.operand];
            profile.name = instruction.operand;
            profile.parameters = signature.parameters;
            profile.outputs = signature.outputs;
            if (signature.hasVarargin) {
                profile.parameters.push_back("varargin");
            }
            if (signature.hasVarargout) {
                profile.outputs.push_back("varargout");
            }
            ++profile.invocationCount;
            observeValues(profile.argumentObservations, entryArguments_);
        }
        return true;
    }

    void finalizeEntryOutputs() {
        if (!entrySignature_) {
            return;
        }
        auto& frame = frames_.front().workspace;
        if (diagnostics_.empty()) {
            const auto function = functionsByName_.find(executedEntryFunction_);
            if (function != functionsByName_.end() &&
                !validateFunctionOutputs(executedEntryFunction_,
                                         function->second, frame)) {
                entryOutputs_.assign(executedRequestedOutputCount_,
                                     missingValue());
                entryOutputNames_ = runtimeFunctionOutputNames(
                    *entrySignature_, executedRequestedOutputCount_);
                return;
            }
        }
        entryOutputs_ = collectFunctionOutputs(
            frame, *entrySignature_, executedRequestedOutputCount_);
        entryOutputNames_ = collectFunctionOutputNames(
            *entrySignature_, executedRequestedOutputCount_);
        if (profilingEnabled_) {
            auto& profile = functionEntryProfiles_[executedEntryFunction_];
            observeValues(profile.resultObservations, entryOutputs_);
        }
    }

    void initializeFunctionFrame(const FunctionSignature& signature,
                                 const std::vector<RuntimeValue>& arguments,
                                 size_t requestedOutputCount,
                                 size_t providedArgumentCount) {
        for (size_t index = 0; index < signature.parameters.size(); ++index) {
            currentFrame()[signature.parameters[index]] = arguments[index];
        }
        if (signature.hasVarargin) {
            std::vector<RuntimeValue> values(
                arguments.begin() + signature.parameters.size(), arguments.end());
            currentFrame()["varargin"] = cellValue(std::move(values));
        }
        initializeRuntimeFunctionOutputs(currentFrame(), signature);
        setRuntimeCallFrameArity(frames_.back(), providedArgumentCount,
                                 requestedOutputCount);
    }

    std::vector<RuntimeValue> collectFunctionOutputs(
        const RuntimeWorkspace& frame,
        const FunctionSignature& signature, size_t requestedOutputCount) const {
        return collectRuntimeFunctionOutputs(frame, signature,
                                             requestedOutputCount);
    }

    std::vector<std::string> collectFunctionOutputNames(
        const FunctionSignature& signature, size_t requestedOutputCount) const {
        return runtimeFunctionOutputNames(signature, requestedOutputCount);
    }

    void executeFunctionBody(size_t entry, size_t end) {
        size_t pc = entry;
        while (pc < end && pc < program_->instructions.size()) {
            const auto& instruction = program_->instructions[pc];
            if (instruction.op == BytecodeOp::EnterFunction &&
                pc < functionEndAt_.size() && functionEndAt_[pc] > pc) {
                pc = functionEndAt_[pc] + 1;
                continue;
            }
            if (instruction.op == BytecodeOp::EnterControl) {
                addDiagnostic(instruction,
                              "bytecode VM does not execute control blocks "
                              "yet");
                break;
            }

            if (!beforeControlledInstruction(instruction.span)) {
                break;
            }
            recordInstruction(pc, instruction);
            const auto nextPc = executeInstruction(instruction);
            ++executedInstructionCount_;
            if (!afterControlledInstruction(instruction.span)) {
                break;
            }
            if (!diagnostics_.empty()) {
                if (const auto recovery = recoverTryDiagnostic()) {
                    pc = *recovery;
                    continue;
                }
                break;
            }
            if (returnRequested_) {
                break;
            }
            pc = nextPc.value_or(pc + 1);
        }
    }

    std::optional<size_t>
    executeInstruction(const BytecodeInstruction& instruction) {
        if (const auto typedTarget = executeTypedDenseRegion()) {
            return typedTarget;
        }
        switch (instruction.op) {
        case BytecodeOp::LoadName:
            loadName(instruction);
            break;
        case BytecodeOp::LoadLiteral:
            loadLiteral(instruction);
            break;
        case BytecodeOp::LoadMetaClass:
            loadMetaClass(instruction);
            break;
        case BytecodeOp::StoreName:
            storeName(instruction);
            break;
        case BytecodeOp::DeclareGlobal:
        case BytecodeOp::DeclarePersistent:
            declareWorkspaceVariable(instruction);
            break;
        case BytecodeOp::StoreMember:
            storeMember(instruction);
            break;
        case BytecodeOp::StoreIndex:
            storeIndex(instruction);
            break;
        case BytecodeOp::StoreBraceIndex:
            storeBraceIndex(instruction);
            break;
        case BytecodeOp::StorePathMember:
            storePathMember(instruction);
            break;
        case BytecodeOp::StorePathIndex:
            storePathIndex(instruction);
            break;
        case BytecodeOp::StorePathBrace:
            storePathBrace(instruction);
            break;
        case BytecodeOp::UnaryOp:
            applyUnary(instruction);
            break;
        case BytecodeOp::BinaryOp:
            applyBinary(instruction);
            break;
        case BytecodeOp::PostfixOp:
            applyPostfix(instruction);
            break;
        case BytecodeOp::MakeMatrixRow:
            makeMatrixRow(instruction);
            break;
        case BytecodeOp::MakeMatrix:
            makeMatrix(instruction);
            break;
        case BytecodeOp::MakeCell:
            makeCell(instruction);
            break;
        case BytecodeOp::MakeCellRow:
            makeCellRow(instruction);
            break;
        case BytecodeOp::MemberAccess:
            memberAccess(instruction);
            break;
        case BytecodeOp::MakeNameValueArgument: {
            const auto value =
                popRuntime(instruction, "name=value argument value");
            if (value) {
                const auto single = runtimeRequireSingleValue(
                    *value, "name=value argument");
                if (!single.succeeded) {
                    addDiagnostic(instruction, single.error);
                    break;
                }
                pushRuntime(makeRuntimeNameValueArgument(
                    instruction.operand, single.value));
            }
            break;
        }
        case BytecodeOp::CallOrIndex:
            callOrIndex(instruction);
            break;
        case BytecodeOp::CallSuperclass:
            callSuperclass(instruction);
            break;
        case BytecodeOp::BraceIndex:
            braceIndex(instruction);
            break;
        case BytecodeOp::Jump:
            return jump(instruction);
        case BytecodeOp::JumpIfFalse:
            return jumpIfFalse(instruction);
        case BytecodeOp::Break:
            return breakLoop(instruction);
        case BytecodeOp::Continue:
            return continueLoop(instruction);
        case BytecodeOp::Return:
            returnRequested_ = true;
            break;
        case BytecodeOp::ForBegin:
            if (const auto typedTarget = executeTypedLoop(instruction)) {
                return typedTarget;
            }
            return beginFor(instruction);
        case BytecodeOp::ForNext:
            return nextFor(instruction);
        case BytecodeOp::CaptureExpression:
            captureExpression(instruction);
            break;
        case BytecodeOp::Pop:
            (void)popRuntime(instruction, "discard");
            break;
        case BytecodeOp::BeginIndexContext:
            beginIndexContext(instruction);
            break;
        case BytecodeOp::BeginIndexArgument:
            beginIndexArgument(instruction);
            break;
        case BytecodeOp::BeginLvalue:
            beginLvalue(instruction);
            break;
        case BytecodeOp::BeginLvalueIndexContext:
            beginLvalueIndexContext(instruction);
            break;
        case BytecodeOp::LvalueDescendMember:
            descendLvalueMember(instruction);
            break;
        case BytecodeOp::LvalueDescendIndex:
            descendLvalueIndex(instruction);
            break;
        case BytecodeOp::LvalueDescendBrace:
            descendLvalueBrace(instruction);
            break;
        case BytecodeOp::SwitchBegin:
            switchBegin(instruction);
            break;
        case BytecodeOp::SwitchCase:
            return switchCase(instruction);
        case BytecodeOp::SwitchOtherwise:
            return switchOtherwise(instruction);
        case BytecodeOp::SwitchEnd:
            switchEnd(instruction);
            break;
        case BytecodeOp::TryBegin:
            return tryBegin(instruction);
        case BytecodeOp::TryEnd:
            return tryEnd(instruction);
        case BytecodeOp::ControlHeader:
        case BytecodeOp::ControlArm:
        case BytecodeOp::LeaveControl:
            break;
        case BytecodeOp::MakeFunctionHandle:
            return makeFunctionHandle(instruction);
        case BytecodeOp::EnterArgumentDefault:
            if (instruction.target < 0) {
                addDiagnostic(instruction,
                              "argument default has no continuation target");
                break;
            }
            return static_cast<size_t>(instruction.target);
        case BytecodeOp::EnterModule:
        case BytecodeOp::LeaveModule:
        case BytecodeOp::EnterClass:
        case BytecodeOp::LeaveClass:
        case BytecodeOp::EnterPropertyInitializer:
        case BytecodeOp::LeavePropertyInitializer:
        case BytecodeOp::EnterEnumerationMemberInitializer:
        case BytecodeOp::LeaveEnumerationMemberInitializer:
        case BytecodeOp::LeaveArgumentDefault:
        case BytecodeOp::EnterFunction:
        case BytecodeOp::LeaveFunction:
        case BytecodeOp::EnterControl:
        case BytecodeOp::Unknown:
            addDiagnostic(instruction,
                          "bytecode VM does not execute instruction yet: " +
                              std::string(bytecodeOpName(instruction.op)));
            break;
        }
        return std::nullopt;
    }

    bool configureNamedFunctionHandle(
        const BytecodeInstruction& instruction,
        RuntimeFunctionHandle& info) {
        const std::string& target = instruction.operand;
        if (instruction.binding.kind == BindingKind::Builtin) {
            info.kind = RuntimeFunctionHandleKind::Builtin;
            info.backend = RuntimeFunctionHandleBackend::Independent;
            info.targetName = symbolName(instruction.binding).value_or(target);
            return true;
        }

        std::string functionName = target;
        if (instruction.binding.kind == BindingKind::Function) {
            functionName = symbolName(instruction.binding).value_or(target);
            const auto function = resolveLocalFunction(functionName);
            if (function) {
                info.kind = RuntimeFunctionHandleKind::Function;
                info.backend = RuntimeFunctionHandleBackend::Bytecode;
                info.context = callableContext_;
                info.targetName = function->key;
                info.span = function->info->span;
                info.sourceFile = function->info->fullPath;
                return true;
            }
        }

        const size_t firstDot = target.find('.');
        if (firstDot != std::string::npos) {
            const std::string receiverName = target.substr(0, firstDot);
            const std::string methodName = target.substr(firstDot + 1);
            const auto receiver = currentFrame().find(receiverName);
            if (receiver != currentFrame().end() &&
                isObject(receiver->second) &&
                methodName.find('.') == std::string::npos) {
                const auto klass =
                    classesByName_.find(receiver->second.className);
                const FunctionInfo* method =
                    klass == classesByName_.end()
                        ? nullptr
                        : selectMethod(klass->second, methodName);
                if (!method || method->staticMethod) {
                    addDiagnostic(instruction,
                                  "bound method is not available: " + target);
                    return false;
                }
                if (!hasMemberAccess(method->access,
                                     method->declaringClass)) {
                    addDiagnostic(instruction,
                                  "method access is denied: " +
                                      method->declaringClass + "." +
                                      method->name);
                    return false;
                }
                info.kind = RuntimeFunctionHandleKind::Method;
                info.backend = RuntimeFunctionHandleBackend::Bytecode;
                info.context = callableContext_;
                info.className = receiver->second.className;
                info.methodName = methodName;
                info.declaringClass = method->declaringClass;
                info.receiver = receiver->second;
                info.span = method->span;
                info.sourceFile = method->fullPath;
                return true;
            }

            const size_t lastDot = target.find_last_of('.');
            const std::string className = target.substr(0, lastDot);
            const std::string staticMethodName = target.substr(lastDot + 1);
            const auto klass = classesByName_.find(className);
            const FunctionInfo* method =
                klass == classesByName_.end()
                    ? nullptr
                    : selectMethod(klass->second, staticMethodName, false);
            if (method && method->staticMethod) {
                if (!hasMemberAccess(method->access,
                                     method->declaringClass)) {
                    addDiagnostic(instruction,
                                  "method access is denied: " +
                                      method->declaringClass + "." +
                                      method->name);
                    return false;
                }
                info.kind = RuntimeFunctionHandleKind::Method;
                info.backend = RuntimeFunctionHandleBackend::Bytecode;
                info.context = callableContext_;
                info.className = className;
                info.methodName = staticMethodName;
                info.declaringClass = method->declaringClass;
                info.span = method->span;
                info.sourceFile = method->fullPath;
                return true;
            }
        }

        auto function = resolveLocalFunction(functionName);
        if (!function && functionName != target) {
            function = resolveLocalFunction(target);
        }
        if (function) {
            info.kind = RuntimeFunctionHandleKind::Function;
            info.backend = RuntimeFunctionHandleBackend::Bytecode;
            info.context = callableContext_;
            info.targetName = function->key;
            info.span = function->info->span;
            info.sourceFile = function->info->fullPath;
            return true;
        }

        if (builtinRegistry().contains(target)) {
            info.kind = RuntimeFunctionHandleKind::Builtin;
            info.backend = RuntimeFunctionHandleBackend::Independent;
            info.targetName = target;
            return true;
        }

        addDiagnostic(instruction,
                      "function handle target is not available: " + target);
        return false;
    }

    bool configureTextFunctionHandle(
        const BytecodeInstruction& instruction, std::string_view target,
        RuntimeFunctionHandle& info) {
        if (target.empty()) {
            addDiagnostic(instruction,
                          "function name string cannot be empty");
            return false;
        }
        if (target.front() == '@') {
            addDiagnostic(
                instruction,
                "str2func does not parse anonymous function text; create "
                "anonymous handles with @(...) syntax");
            return false;
        }

        const std::string name(target);
        if (ambiguousFunctionMetadataIdentifiers_.contains(name)) {
            addDiagnostic(instruction,
                          "function name string is ambiguous: " + name);
            return false;
        }
        if (const FunctionInfo* function =
                functionForPublicMetadataIdentifier(name)) {
            const bool privateFunction =
                function->name.starts_with("$private");
            const bool localFunction =
                function->metadataIdentifier.find('>') != std::string::npos;
            if (privateFunction || localFunction) {
                addDiagnostic(
                    instruction,
                    "function name string cannot resolve a private or local "
                    "function: " +
                        name);
                return false;
            }
            info.kind = RuntimeFunctionHandleKind::Function;
            info.backend = RuntimeFunctionHandleBackend::Bytecode;
            info.context = callableContext_;
            info.targetName = function->name;
            info.span = function->span;
            info.sourceFile = function->fullPath;
            return true;
        }

        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot != 0 && dot + 1 < name.size()) {
            const std::string className = name.substr(0, dot);
            const std::string methodName = name.substr(dot + 1);
            const auto klass = classesByName_.find(className);
            const FunctionInfo* method =
                klass == classesByName_.end()
                    ? nullptr
                    : selectMethod(klass->second, methodName, false);
            if (method && method->staticMethod) {
                if (method->access.level != MemberAccessLevel::Public) {
                    addDiagnostic(instruction,
                                  "function name string cannot resolve a "
                                  "non-public method: " +
                                      name);
                    return false;
                }
                info.kind = RuntimeFunctionHandleKind::Method;
                info.backend = RuntimeFunctionHandleBackend::Bytecode;
                info.context = callableContext_;
                info.className = className;
                info.methodName = methodName;
                info.declaringClass = method->declaringClass;
                info.span = method->span;
                info.sourceFile = method->fullPath;
                return true;
            }
        }

        if (builtinRegistry().contains(name)) {
            info.kind = RuntimeFunctionHandleKind::Builtin;
            info.backend = RuntimeFunctionHandleBackend::Independent;
            info.targetName = name;
            return true;
        }

        addDiagnostic(instruction,
                      "function name string is not available: " + name);
        return false;
    }

    std::optional<RuntimeValue> functionHandleFromText(
        const BytecodeInstruction& instruction, std::string_view target) {
        if (const auto* inherited = inheritedSourceCallable(target);
            inherited && inherited->textResolutionAllowed) {
            return inherited->callable;
        }
        RuntimeFunctionHandle info;
        info.display = "@" + std::string(target);
        info.span = instruction.span;
        if (!configureTextFunctionHandle(instruction, target, info)) {
            return std::nullopt;
        }
        return makeRuntimeFunctionHandleValue(std::move(info));
    }

    std::optional<size_t> makeFunctionHandle(
        const BytecodeInstruction& instruction) {
        RuntimeFunctionHandle info;
        info.span = instruction.span;
        info.lexicalClassName = instruction.receiverName;

        std::optional<size_t> continuation;
        if (instruction.operand == "@()") {
            continuation = checkedTarget(instruction);
            if (!continuation) {
                return std::nullopt;
            }
            info.kind = RuntimeFunctionHandleKind::Anonymous;
            info.backend = RuntimeFunctionHandleBackend::Bytecode;
            info.context = callableContext_;
            info.parameters = instruction.parameters;
            info.capturedVariables = captureRuntimeWorkspace(
                currentFrame(), instruction.captureNames);
            info.entry = currentPc_ + 1;
            info.end = *continuation;
            info.display = instruction.calleeName;
            if (info.display.empty()) {
                info.display = "@(";
                for (size_t index = 0; index < info.parameters.size();
                     ++index) {
                    if (index > 0) {
                        info.display += ",";
                    }
                    info.display += info.parameters[index];
                }
                info.display += ")";
            }
            if (const auto* source = sourceInfo(info.span)) {
                info.sourceFile = source->name;
            }
            if (info.entry >= info.end) {
                addDiagnostic(instruction,
                              "anonymous function handle requires a body");
                return continuation;
            }
        } else {
            if (const auto* inherited =
                    inheritedSourceCallable(instruction.operand)) {
                pushRuntime(inherited->callable);
                return continuation;
            }
            info.display = instruction.calleeName.empty()
                               ? "@" + instruction.operand
                               : instruction.calleeName;
            if (!configureNamedFunctionHandle(instruction, info)) {
                return std::nullopt;
            }
        }

        pushRuntime(makeRuntimeFunctionHandleValue(std::move(info)));
        return continuation;
    }

    std::optional<size_t> checkedTarget(
        const BytecodeInstruction& instruction) {
        if (instruction.target < 0) {
            addDiagnostic(instruction,
                          "bytecode control instruction has no jump target");
            return std::nullopt;
        }

        const auto target = static_cast<size_t>(instruction.target);
        if (!program_ || target > program_->instructions.size()) {
            addDiagnostic(instruction,
                          "bytecode control instruction target is out of "
                          "bounds");
            return std::nullopt;
        }
        return target;
    }

    std::optional<size_t> jump(const BytecodeInstruction& instruction) {
        const auto target = checkedTarget(instruction);
        if (target) {
            recordGenericBackedge(instruction, *target);
            unwindStructuredContexts(*target);
        }
        return target;
    }

    std::optional<size_t> jumpIfFalse(
        const BytecodeInstruction& instruction) {
        const auto condition = popRuntime(instruction, "conditional jump");
        if (!condition) {
            return std::nullopt;
        }
        const auto conditionValue = runtimeNumericTruthValue(*condition);
        if (!conditionValue) {
            addDiagnostic(
                instruction,
                "bytecode condition must be a real numeric value without NaN");
            return std::nullopt;
        }
        if (!*conditionValue) {
            const auto target = checkedTarget(instruction);
            if (target) {
                unwindStructuredContexts(*target);
            }
            return target;
        }
        return std::nullopt;
    }

    std::optional<size_t> beginFor(
        const BytecodeInstruction& instruction) {
        const auto range = popRuntime(instruction, "for loop range");
        if (!range) {
            return std::nullopt;
        }

        auto values = runtimeNumericForLoopColumns(*range);
        if (!values) {
            addDiagnostic(
                instruction,
                "bytecode for loop range must be a valid numeric array");
            return std::nullopt;
        }
        const RuntimeValue* observedValue =
            values->empty() ? nullptr : &values->front();
        recordForEntry(instruction, values->size(), observedValue);
        if (values->empty()) {
            return checkedTarget(instruction);
        }

        storeVariable(instruction, values->front());
        forLoopStack_.push_back(
            ForLoopState{instruction.operand, std::move(*values), 1,
                         currentPc_, instruction.binding});
        return std::nullopt;
    }

    bool typedDenseGuardsMatch(
        const ActiveTypedDenseRegion& active,
        std::string& failureReason) const {
        for (const auto& guard : active.guards) {
            if (guard.source != "region-input" || guard.role.empty()) {
                failureReason =
                    "typed dense region contains an unsupported guard";
                return false;
            }
            const auto found = currentFrame().find(guard.role);
            if (found == currentFrame().end()) {
                failureReason =
                    "typed dense guard input is unavailable: " +
                    guard.role;
                return false;
            }
            const RuntimeValue& value = found->second;
            const std::string actualKind = runtimeKindName(value);
            const bool kindMatches =
                guard.value.kind == "numeric"
                    ? isRuntimeNumericValue(value)
                    : actualKind == guard.value.kind;
            const std::string actualNumericClass =
                isRuntimeNumericValue(value)
                    ? std::string(runtimeNumericClassName(
                          value.numericClass))
                    : std::string{};
            const auto expectedDimensions =
                guard.value.dimensions.empty()
                    ? normalizeRuntimeDimensions(
                          {guard.value.rows, guard.value.columns})
                    : normalizeRuntimeDimensions(
                          guard.value.dimensions);
            const bool numericClassMatches =
                !guard.value.numericClassKnown ||
                actualNumericClass == guard.value.numericClass;
            const bool complexMatches =
                !guard.value.complexKnown ||
                !isRuntimeNumericValue(value) ||
                value.numericComplex == guard.value.numericComplex;
            const bool shapeMatches =
                !guard.value.shapeKnown ||
                runtimeDimensions(value) == expectedDimensions;
            if (!kindMatches ||
                !numericClassMatches || !complexMatches ||
                !shapeMatches) {
                failureReason =
                    "typed dense guard failed for input: " + guard.role;
                return false;
            }
        }
        return true;
    }

    std::optional<size_t> executeTypedDenseRegion() {
        const auto active = typedDenseRegions_.find(currentPc_);
        if (active == typedDenseRegions_.end()) {
            return std::nullopt;
        }

        auto& execution =
            typedRegionExecutions_[active->second.regionId];
        ++execution.attemptCount;
        std::string guardFailure;
        if (!typedDenseGuardsMatch(active->second, guardFailure)) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::UnsupportedInput;
            execution.lastReason = std::move(guardFailure);
            return std::nullopt;
        }

        DenseArrayTypedRegionExecutor executor(
            semantic_ ? semantic_->builtinRegistry : nullptr);
        auto result = BytecodeVmTrustedAccess::executeDenseRegion(
            executor, *program_, active->second.contract,
            currentFrame(), typedRegionBackend_);
        execution.backend = typedRegionBackendName(result.backend);
        execution.nativeCompilationCount +=
            result.nativeCompilationCount;
        execution.nativeCacheHitCount += result.nativeCacheHitCount;
        execution.nativeCacheInsertionCount +=
            result.nativeCacheInsertionCount;
        execution.nativeCacheBypassCount +=
            result.nativeCacheBypassCount;
        execution.nativeCacheEvictionCount +=
            result.nativeCacheEvictionCount;
        execution.nativeCacheEvictedCodeBytes +=
            result.nativeCacheEvictedCodeBytes;
        execution.nativeCodeSize =
            std::max(execution.nativeCodeSize,
                     result.nativeCodeSize);
        execution.nativePlatform = result.nativePlatform;
        execution.lastFallbackKind = result.fallbackKind;
        execution.nativeFallbackKind =
            result.nativeFallbackKind;
        execution.nativeFallbackReason =
            result.nativeFallbackReason;
        execution.lastReason = result.reason;
        if (result.status != TypedRegionExecutionStatus::Executed) {
            ++execution.fallbackCount;
            return std::nullopt;
        }

        const size_t storePc = active->second.contract.bodyEndPc;
        if (storePc >= program_->instructions.size()) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::InvalidContract;
            execution.lastReason =
                "typed dense store instruction is unavailable";
            return std::nullopt;
        }
        const auto& store = program_->instructions[storePc];
        recordAssignmentAt(storePc, store, "name", result.value);
        storeVariable(store, std::move(result.value));

        ++execution.executionCount;
        execution.iterationCount += result.elementCount;
        execution.executedInstructionCount +=
            result.executedInstructionCount;
        execution.executedKernelInstructionCount +=
            result.executedKernelInstructionCount;
        return active->second.contract.endPc;
    }

    std::optional<size_t> executeTypedLoop(
        const BytecodeInstruction& instruction) {
        const auto active = typedLoopRegions_.find(currentPc_);
        if (active == typedLoopRegions_.end()) {
            return std::nullopt;
        }

        auto& execution =
            typedRegionExecutions_[active->second.regionId];
        ++execution.attemptCount;
        if (stack_.empty()) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::MissingStackValue;
            execution.lastReason = "typed loop range stack value is missing";
            return std::nullopt;
        }
        const StackValue& rangeValue = stack_.back();
        if (rangeValue.isBuiltinReference ||
            rangeValue.isFunctionReference) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::UnsupportedRuntimeValue;
            execution.lastReason =
                "typed loop range is not a runtime value";
            return std::nullopt;
        }

        const auto loopValues =
            runtimeNumericForLoopColumns(rangeValue.value);
        if (!loopValues ||
            std::any_of(loopValues->begin(), loopValues->end(),
                        [](const RuntimeValue& value) {
                            return value.kind != RuntimeValueKind::Number;
                        })) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::UnsupportedRuntimeValue;
            execution.lastReason =
                "typed scalar loop requires scalar column values";
            return std::nullopt;
        }

        const auto shadowedCall = std::find_if(
            active->second.contract.callTargets.begin(),
            active->second.contract.callTargets.end(),
            [this](const std::string& name) {
                return currentFrame().contains(name);
            });
        if (shadowedCall !=
            active->second.contract.callTargets.end()) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::UnsupportedRuntimeValue;
            execution.lastReason =
                "typed callable target is shadowed by workspace variable: " +
                *shadowedCall;
            return std::nullopt;
        }

        if (active->second.contract.scalarFunctionCallCount != 0 &&
            executionControl_->limits().maxCallDepth != 0) {
            ++execution.fallbackCount;
            execution.lastFallbackKind =
                RuntimeFallbackKind::UnsupportedOperation;
            execution.lastReason =
                "typed scalar function specialization is suppressed while a call-depth limit is active";
            executionControl_->markOptimizedExecutionSuppressed();
            return std::nullopt;
        }

        ScalarTypedRegionExecutor executor(
            semantic_ ? semantic_->builtinRegistry : nullptr);
        auto result = BytecodeVmTrustedAccess::executeRegion(
            executor, *program_, active->second.contract,
            rangeValue.value, currentFrame(), typedRegionBackend_);
        if (result.status != TypedRegionExecutionStatus::Executed) {
            ++execution.fallbackCount;
            execution.backend =
                typedRegionBackendName(typedRegionBackend_);
            execution.nativePlatform =
                std::move(result.nativePlatform);
            execution.lastFallbackKind = result.fallbackKind;
            execution.nativeFallbackKind =
                result.nativeFallbackKind;
            execution.nativeFallbackReason =
                std::move(result.nativeFallbackReason);
            execution.lastReason = result.reason;
            return std::nullopt;
        }

        if (profilingEnabled_) {
            const RuntimeValue* observedValue = nullptr;
            if (!loopValues->empty()) {
                observedValue = &loopValues->front();
            }
            recordForEntry(instruction, loopValues->size(), observedValue);
            if (!loopValues->empty()) {
                const auto& latch = program_->instructions[
                    active->second.contract.bodyEndPc];
                ForLoopState state{instruction.operand, *loopValues, 1,
                                   currentPc_,
                                   instruction.binding};
                for (size_t index = 1; index < loopValues->size(); ++index) {
                    recordForBackedge(state, latch, (*loopValues)[index]);
                }
                recordForCompletion(state, latch);
            }
        }

        stack_.pop_back();
        currentFrame() = std::move(result.variables);
        ++execution.executionCount;
        execution.iterationCount += result.iterationCount;
        execution.nestedIterationCount += result.nestedIterationCount;
        execution.executedInstructionCount +=
            result.executedInstructionCount;
        execution.executedKernelInstructionCount +=
            result.executedKernelInstructionCount;
        execution.backend = typedRegionBackendName(result.backend);
        execution.nativeCompilationCount +=
            result.nativeCompiled ? 1 : 0;
        execution.nativeCacheHitCount +=
            result.nativeCacheHit ? 1 : 0;
        execution.nativeCacheInsertionCount +=
            result.nativeCacheStored ? 1 : 0;
        execution.nativeCacheBypassCount +=
            result.nativeCacheBypassed ? 1 : 0;
        execution.nativeCacheEvictionCount +=
            result.nativeCacheEvictionCount;
        execution.nativeCacheEvictedCodeBytes +=
            result.nativeCacheEvictedCodeBytes;
        execution.nativeCodeSize = result.nativeCodeSize;
        execution.nativePlatform = std::move(result.nativePlatform);
        execution.lastFallbackKind = result.fallbackKind;
        execution.nativeFallbackKind =
            result.nativeFallbackKind;
        execution.nativeFallbackReason =
            std::move(result.nativeFallbackReason);
        execution.lastReason = result.reason;

        return active->second.contract.endPc;
    }

    std::optional<size_t> nextFor(
        const BytecodeInstruction& instruction) {
        if (forLoopStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode for-next encountered without active for "
                          "loop");
            return std::nullopt;
        }

        auto& state = forLoopStack_.back();
        if (state.nextIndex >= state.values.size()) {
            recordForCompletion(state, instruction);
            forLoopStack_.pop_back();
            return std::nullopt;
        }

        RuntimeValue nextValue = state.values[state.nextIndex];
        storeVariable(state.variable, state.binding, nextValue,
                      instruction);
        ++state.nextIndex;
        recordForBackedge(state, instruction, nextValue);
        return checkedTarget(instruction);
    }

    std::optional<size_t> breakLoop(
        const BytecodeInstruction& instruction) {
        if (forLoopStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode break encountered without active for loop");
            return std::nullopt;
        }
        const auto target = checkedTarget(instruction);
        if (!target) {
            return std::nullopt;
        }
        recordForBreak(forLoopStack_.back(), instruction);
        forLoopStack_.pop_back();
        unwindStructuredContexts(*target);
        return target;
    }

    std::optional<size_t> continueLoop(
        const BytecodeInstruction& instruction) {
        recordContinue(instruction);
        const auto target = checkedTarget(instruction);
        if (target) {
            unwindStructuredContexts(*target);
        }
        return target;
    }

    void unwindStructuredContexts(size_t target) {
        while (!switchContextStack_.empty()) {
            const auto& context = switchContextStack_.back();
            if (target > context.beginPc &&
                target <= context.endPc) {
                break;
            }
            switchContextStack_.pop_back();
        }
        while (!tryContextStack_.empty()) {
            const auto& context = tryContextStack_.back();
            if (target > context.beginPc &&
                target <= context.endPc) {
                break;
            }
            tryContextStack_.pop_back();
        }
    }

    void switchBegin(const BytecodeInstruction& instruction) {
        const auto selector = popRuntime(instruction, "switch selector");
        if (!selector) {
            return;
        }
        const size_t end =
            currentPc_ < switchEndAt_.size()
                ? switchEndAt_[currentPc_]
                : currentPc_;
        switchContextStack_.push_back(
            SwitchContext{*selector, false, currentPc_, end});
    }

    std::optional<size_t> switchCase(
        const BytecodeInstruction& instruction) {
        const auto candidate = popRuntime(instruction, "switch case value");
        if (!candidate) {
            return std::nullopt;
        }
        if (switchContextStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode switch case has no active switch");
            return std::nullopt;
        }

        auto& context = switchContextStack_.back();
        const bool matches =
            isCell(*candidate)
                ? std::any_of(
                      candidate->cells.begin(), candidate->cells.end(),
                      [&](const RuntimeValue& element) {
                          return runtimeEqual(context.selector, element);
                      })
                : runtimeEqual(context.selector, *candidate);
        if (context.matched || !matches) {
            return checkedTarget(instruction);
        }
        context.matched = true;
        return std::nullopt;
    }

    std::optional<size_t> switchOtherwise(
        const BytecodeInstruction& instruction) {
        if (switchContextStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode otherwise has no active switch");
            return std::nullopt;
        }
        auto& context = switchContextStack_.back();
        if (context.matched) {
            return checkedTarget(instruction);
        }
        context.matched = true;
        return std::nullopt;
    }

    void switchEnd(const BytecodeInstruction& instruction) {
        if (switchContextStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode switch end has no active switch");
            return;
        }
        switchContextStack_.pop_back();
    }

    std::optional<size_t> tryBegin(
        const BytecodeInstruction& instruction) {
        const auto catchTarget = checkedTarget(instruction);
        if (!catchTarget) {
            return std::nullopt;
        }

        tryContextStack_.push_back(TryContext{
            diagnostics_.size(),
            *catchTarget,
            instruction.operand,
            stack_.size(),
            forLoopStack_.size(),
            indexContextStack_.size(),
            lvalueStack_.size(),
            switchContextStack_.size(),
            currentPc_,
            currentPc_ < tryEndAt_.size()
                ? tryEndAt_[currentPc_]
                : currentPc_});
        return std::nullopt;
    }

    std::optional<size_t> tryEnd(
        const BytecodeInstruction& instruction) {
        if (tryContextStack_.empty()) {
            addDiagnostic(instruction, "bytecode try end has no active try");
            return std::nullopt;
        }
        tryContextStack_.pop_back();
        return checkedTarget(instruction);
    }

    std::optional<size_t> recoverTryDiagnostic() {
        if (tryContextStack_.empty() || diagnostics_.empty()) {
            return std::nullopt;
        }

        TryContext context = std::move(tryContextStack_.back());
        tryContextStack_.pop_back();

        const Diagnostic diagnostic = diagnostics_.back();
        RuntimeValue exception = pendingException_.value_or(
            runtimeExceptionFromDiagnostic(
                diagnostic, exceptionFrames(diagnostic.span)));
        pendingException_.reset();
        diagnostics_.resize(context.diagnosticBase);
        stack_.resize(context.stackDepth);
        forLoopStack_.resize(context.forLoopDepth);
        indexContextStack_.resize(context.indexContextDepth);
        lvalueStack_.resize(context.lvalueDepth);
        switchContextStack_.resize(context.switchContextDepth);

        if (!context.catchVariable.empty()) {
            currentFrame()[context.catchVariable] = std::move(exception);
        }
        return context.catchTarget;
    }

    void beginIndexContext(const BytecodeInstruction& instruction) {
        if (instruction.operandCount < 0) {
            addDiagnostic(instruction,
                          "bytecode index context has negative arity");
            return;
        }
        if (stack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode index context requires a target");
            return;
        }
        const StackValue& target = stack_.back();
        if (target.isBuiltinReference || target.isFunctionReference ||
            target.isClassReference || target.isMethodReference ||
            isFunctionHandle(target.value)) {
            indexContextStack_.push_back(IndexContext{
                missingValue(), static_cast<size_t>(instruction.operandCount),
                0, false});
            return;
        }
        if (target.value.kind != RuntimeValueKind::MissingArray &&
            !isNumeric(target.value) && !isCell(target.value) &&
            !isRuntimeTextValue(target.value) &&
            target.value.kind != RuntimeValueKind::Struct &&
            !isRuntimeClassObject(target.value) &&
            !isRuntimeMetadataObject(target.value)) {
            addDiagnostic(instruction,
                          "bytecode index context requires a missing, numeric, "
                          "text, cell, structure, object, or metadata target");
            return;
        }

        indexContextStack_.push_back(IndexContext{
            target.value, static_cast<size_t>(instruction.operandCount), 0,
            true});
    }

    void beginIndexArgument(const BytecodeInstruction& instruction) {
        if (indexContextStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode index argument has no active context");
            return;
        }
        if (instruction.operandCount < 0) {
            addDiagnostic(instruction,
                          "bytecode index argument has negative position");
            return;
        }
        indexContextStack_.back().position =
            static_cast<size_t>(instruction.operandCount);
    }

    std::optional<RuntimeValue> literalInIndexContext(
        const BytecodeInstruction& instruction) const {
        if (indexContextStack_.empty()) {
            return std::nullopt;
        }

        for (auto context = indexContextStack_.rbegin();
             context != indexContextStack_.rend(); ++context) {
            if (!context->hasTarget) {
                continue;
            }
            if (instruction.operand == "end") {
                return numberValue(
                    endValueForIndex(context->target, context->position,
                                     context->total));
            }
            if (instruction.operand == ":") {
                return oneBasedIndexRange(static_cast<size_t>(
                    endValueForIndex(context->target, context->position,
                                     context->total)));
            }
            break;
        }
        return std::nullopt;
    }

    double endValueForIndex(const RuntimeValue& target, size_t position,
                            size_t total) const {
        const auto dimensions =
            runtimeEffectiveSubscriptDimensions(target, total);
        return position < dimensions.size()
                   ? static_cast<double>(dimensions[position])
                   : 1.0;
    }

    void finishIndexContext() {
        if (!indexContextStack_.empty()) {
            indexContextStack_.pop_back();
        }
    }

    void finishCallOrIndexContext(
        const BytecodeInstruction& instruction) {
        if (instruction.hasIndexContext) {
            finishIndexContext();
        }
    }

    ActiveLvalue* activeLvalue(
        const BytecodeInstruction& instruction) {
        if (lvalueStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode lvalue operation has no active path");
            return nullptr;
        }
        return lvalueStack_.back().get();
    }

    void beginLvalue(const BytecodeInstruction& instruction) {
        const auto variable = loadStoredVariable(instruction);
        RuntimeValue root =
            variable ? *variable : missingValue();
        lvalueStack_.push_back(std::make_unique<ActiveLvalue>(
            instruction.operand, instruction.binding,
            std::move(root)));
    }

    void beginLvalueIndexContext(
        const BytecodeInstruction& instruction) {
        ActiveLvalue* active = activeLvalue(instruction);
        if (!active) {
            return;
        }
        if (instruction.operandCount < 0) {
            addDiagnostic(instruction,
                          "bytecode lvalue index context has negative arity");
            active->failed = true;
            return;
        }
        indexContextStack_.push_back(IndexContext{
            active->failed ? missingValue()
                           : active->transaction.current(),
            static_cast<size_t>(instruction.operandCount), 0,
            !active->failed});
    }

    std::optional<std::string> lvalueMemberName(
        const BytecodeInstruction& instruction) {
        if (instruction.operand != ".()") {
            return instruction.operand;
        }
        const auto dynamicValue =
            popRuntime(instruction, "dynamic lvalue member name");
        if (!dynamicValue) {
            return std::nullopt;
        }
        const auto dynamicName = runtimeStructFieldName(*dynamicValue);
        if (!dynamicName.succeeded) {
            addDiagnostic(instruction, dynamicName.error);
            return std::nullopt;
        }
        return dynamicName.name;
    }

    std::optional<std::vector<RuntimeValue>> lvalueSubscripts(
        const BytecodeInstruction& instruction) {
        auto values = popRuntimeValues(
            instruction, instruction.operandCount,
            "lvalue path subscripts");
        finishIndexContext();
        if (!values) {
            return std::nullopt;
        }
        return runtimeExpandedValues(*values);
    }

    RuntimeValue lvalueMissingSeed(
        const BytecodeInstruction& instruction) const {
        if (instruction.receiverName == "cell") {
            return cellValueForDimensions({0, 0}, {});
        }
        if (instruction.receiverName == "numeric") {
            return matrixValue(0, 0, {});
        }
        return makeRuntimeStructValue();
    }

    RuntimeLvalueOperationResult readObjectMemberForLvalue(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target, std::string_view name) {
        const size_t stackDepth = stack_.size();
        const size_t diagnosticCount = diagnostics_.size();
        BytecodeInstruction resolved = instruction;
        resolved.operand = std::string(name);
        resolved.receiverName.clear();
        resolved.resultCount = 1;
        stack_.push_back(runtimeStackValue(target));
        memberAccessResolved(resolved);
        if (diagnostics_.size() != diagnosticCount ||
            stack_.size() != stackDepth + 1) {
            stack_.resize(stackDepth);
            return RuntimeLvalueOperationResult{};
        }
        const auto value =
            popRuntime(resolved, "nested object member access");
        stack_.resize(stackDepth);
        if (!value) {
            return RuntimeLvalueOperationResult{};
        }
        return RuntimeLvalueOperationResult{true, *value, {}};
    }

    RuntimeLvalueOperationResult writeObjectMemberForLvalue(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target, std::string_view name,
        const RuntimeValue& value) {
        static const std::string temporaryName =
            "\x1f" "mparser_lvalue_target";
        const auto previous = currentFrame().find(temporaryName);
        const std::optional<RuntimeValue> previousValue =
            previous == currentFrame().end()
                ? std::nullopt
                : std::optional<RuntimeValue>{previous->second};
        const size_t stackDepth = stack_.size();
        const size_t diagnosticCount = diagnostics_.size();
        currentFrame()[temporaryName] = target;
        pushRuntime(value);

        BytecodeInstruction resolved = instruction;
        resolved.operand = std::string(name);
        resolved.receiverName = temporaryName;
        storeMemberResolved(resolved);

        std::optional<RuntimeValue> updated;
        if (diagnostics_.size() == diagnosticCount) {
            const auto stored = currentFrame().find(temporaryName);
            if (stored != currentFrame().end()) {
                updated = stored->second;
            }
        }
        stack_.resize(stackDepth);
        if (previousValue) {
            currentFrame()[temporaryName] = *previousValue;
        } else {
            currentFrame().erase(temporaryName);
        }
        if (!updated) {
            return RuntimeLvalueOperationResult{};
        }
        return RuntimeLvalueOperationResult{true, *updated, {}};
    }

    RuntimeLvalueHooks lvalueHooks(
        const BytecodeInstruction& instruction) {
        RuntimeLvalueHooks hooks;
        hooks.readObjectMember =
            [this, &instruction](const RuntimeValue& target,
                                 std::string_view name) {
                return readObjectMemberForLvalue(instruction, target, name);
            };
        hooks.writeObjectMember =
            [this, &instruction](const RuntimeValue& target,
                                 std::string_view name,
                                 const RuntimeValue& value) {
                return writeObjectMemberForLvalue(
                    instruction, target, name, value);
            };
        hooks.objectArrays = objectArrayPolicy(instruction);
        return hooks;
    }

    void failActiveLvalue(const BytecodeInstruction& instruction,
                          ActiveLvalue& active,
                          std::string error) {
        active.failed = true;
        if (!error.empty()) {
            addDiagnostic(instruction, std::move(error));
        }
    }

    void descendLvalueMember(
        const BytecodeInstruction& instruction) {
        ActiveLvalue* active = activeLvalue(instruction);
        const auto name = lvalueMemberName(instruction);
        if (!active || !name) {
            if (active) {
                active->failed = true;
            }
            return;
        }
        if (active->failed) {
            return;
        }
        RuntimeLvalueSegment segment;
        segment.kind = RuntimeLvalueSegmentKind::Member;
        segment.memberName = *name;
        auto result = active->transaction.descend(
            std::move(segment), lvalueHooks(instruction),
            lvalueMissingSeed(instruction));
        if (!result.succeeded) {
            failActiveLvalue(instruction, *active,
                             std::move(result.error));
        }
    }

    void descendLvalueIndexed(
        const BytecodeInstruction& instruction,
        RuntimeLvalueSegmentKind kind) {
        ActiveLvalue* active = activeLvalue(instruction);
        const auto subscripts = lvalueSubscripts(instruction);
        if (!active || !subscripts) {
            if (active) {
                active->failed = true;
            }
            return;
        }
        if (active->failed) {
            return;
        }
        RuntimeLvalueSegment segment;
        segment.kind = kind;
        segment.subscripts = *subscripts;
        segment.colonSubscripts = instruction.colonSubscripts;
        auto result = active->transaction.descend(
            std::move(segment), lvalueHooks(instruction),
            lvalueMissingSeed(instruction));
        if (!result.succeeded) {
            failActiveLvalue(instruction, *active,
                             std::move(result.error));
        }
    }

    void descendLvalueIndex(
        const BytecodeInstruction& instruction) {
        descendLvalueIndexed(instruction,
                            RuntimeLvalueSegmentKind::Parenthesis);
    }

    void descendLvalueBrace(
        const BytecodeInstruction& instruction) {
        descendLvalueIndexed(instruction,
                            RuntimeLvalueSegmentKind::Brace);
    }

    static bool isSessionBinding(BindingRef binding) {
        return binding.kind == BindingKind::GlobalVariable ||
               binding.kind == BindingKind::PersistentVariable;
    }

    static std::string_view unqualifiedFunctionKey(
        std::string_view key) {
        const size_t nested = key.find_last_of('>');
        if (nested != std::string_view::npos) {
            return key.substr(nested + 1);
        }
        const size_t member = key.find_last_of('.');
        return member == std::string_view::npos
                   ? key
                   : key.substr(member + 1);
    }

    struct ResolvedLocalFunction {
        std::string key;
        const FunctionInfo* info = nullptr;
    };

    std::optional<ResolvedLocalFunction> resolveLocalFunctionInScope(
        std::string_view name, std::string scope) const {
        if (!scope.empty()) {
            if (unqualifiedFunctionKey(scope) == name) {
                const auto recursive = functionsByName_.find(scope);
                if (recursive != functionsByName_.end()) {
                    return ResolvedLocalFunction{
                        recursive->first, &recursive->second};
                }
            }
            while (!scope.empty()) {
                const std::string candidate =
                    scope + ">" + std::string(name);
                const auto nested = functionsByName_.find(candidate);
                if (nested != functionsByName_.end()) {
                    return ResolvedLocalFunction{
                        nested->first, &nested->second};
                }
                const size_t separator = scope.find_last_of('>');
                if (separator == std::string::npos) {
                    break;
                }
                scope.resize(separator);
            }
        }

        const auto function = functionsByName_.find(std::string(name));
        if (function == functionsByName_.end()) {
            return std::nullopt;
        }
        return ResolvedLocalFunction{function->first, &function->second};
    }

    std::optional<ResolvedLocalFunction> resolveLocalFunction(
        std::string_view name) const {
        return resolveLocalFunctionInScope(
            name, activeFunctionFrames_.empty()
                      ? std::string{}
                      : activeFunctionFrames_.back().key);
    }

    std::string sourceCallableScopeKey(size_t frameIndex) const {
        for (auto active = activeFunctionFrames_.rbegin();
             active != activeFunctionFrames_.rend(); ++active) {
            if (active->frameIndex == frameIndex) {
                return active->key;
            }
        }
        return {};
    }

    std::vector<RuntimeSourceCallable> sourceCallablesForFrame(
        size_t frameIndex) const {
        struct Candidate {
            std::string key;
            const FunctionInfo* info = nullptr;
            bool textResolutionAllowed = false;
        };

        std::map<std::string, Candidate> candidates;
        std::set<std::string> ambiguous;
        const auto addCandidate =
            [&candidates, &ambiguous](std::string name,
                                      Candidate candidate) {
                if (name.empty() || ambiguous.contains(name)) {
                    return;
                }
                const auto [existing, inserted] =
                    candidates.emplace(std::move(name), candidate);
                if (!inserted && existing->second.key != candidate.key) {
                    ambiguous.insert(existing->first);
                    candidates.erase(existing);
                } else if (!inserted) {
                    existing->second.textResolutionAllowed =
                        existing->second.textResolutionAllowed ||
                        candidate.textResolutionAllowed;
                }
            };

        const std::string scope = sourceCallableScopeKey(frameIndex);
        std::set<std::string> lexicalNames;
        for (const auto& [key, function] : functionsByName_) {
            (void)function;
            lexicalNames.emplace(unqualifiedFunctionKey(key));
        }
        for (const auto& name : lexicalNames) {
            const auto function =
                resolveLocalFunctionInScope(name, scope);
            if (!function) {
                continue;
            }
            const bool textAllowed =
                !function->info->name.starts_with("$private") &&
                function->info->metadataIdentifier.find('>') ==
                    std::string::npos;
            addCandidate(
                name, Candidate{function->key, function->info,
                                textAllowed});
        }
        for (const auto& [identifier, key] :
             functionsByMetadataIdentifier_) {
            if (identifier.find('>') != std::string::npos) {
                continue;
            }
            const auto function = functionsByName_.find(key);
            if (function == functionsByName_.end() ||
                function->second.name.starts_with("$path") ||
                function->second.name.starts_with("$private")) {
                continue;
            }
            addCandidate(
                identifier,
                Candidate{
                    function->first, &function->second,
                    !function->second.name.starts_with("$private")});
        }
        size_t sourceId = kInvalidSourceId;
        if (frameIndex < frames_.size()) {
            sourceId = frames_[frameIndex].span.begin.sourceId;
        }
        if (sourceId == kInvalidSourceId && semantic_ &&
            !semantic_->sources.empty()) {
            sourceId = 0;
        }
        if (semantic_ && sourceId < semantic_->sources.size()) {
            for (const auto& binding :
                 semantic_->sources[sourceId].functionBindings) {
                const auto function =
                    functionsByName_.find(binding.target);
                if (function == functionsByName_.end()) {
                    continue;
                }
                addCandidate(
                    binding.alias,
                    Candidate{
                        function->first, &function->second,
                        !function->second.name.starts_with(
                            "$private")});
            }
        }

        std::vector<RuntimeSourceCallable> callables;
        callables.reserve(candidates.size());
        for (const auto& [name, candidate] : candidates) {
            RuntimeFunctionHandle handle;
            handle.kind = RuntimeFunctionHandleKind::Function;
            handle.backend = RuntimeFunctionHandleBackend::Bytecode;
            handle.context = callableContext_;
            handle.display = "@" + name;
            handle.targetName = candidate.key;
            handle.span = candidate.info->span;
            handle.sourceFile = candidate.info->fullPath;
            callables.push_back(RuntimeSourceCallable{
                name, makeRuntimeFunctionHandleValue(std::move(handle)),
                static_cast<size_t>(preferredImplicitOutputCount(
                    candidate.info->signature)),
                candidate.textResolutionAllowed});
        }
        return callables;
    }

    std::vector<RuntimeSourceCallableScope>
    sourceCallableScopes() {
        std::vector<RuntimeSourceCallableScope> scopes;
        scopes.reserve(frames_.size());
        for (size_t index = 0; index < frames_.size(); ++index) {
            scopes.push_back(RuntimeSourceCallableScope{
                &frames_[index].workspace,
                sourceCallablesForFrame(index)});
        }
        return scopes;
    }

    RuntimeCallFrame* sourceStorageFrame(
        RuntimeWorkspace* workspace) {
        if (!workspace) {
            return nullptr;
        }
        const auto frame = std::find_if(
            frames_.begin(), frames_.end(),
            [workspace](RuntimeCallFrame& candidate) {
                return &candidate.workspace == workspace;
            });
        return frame == frames_.end() ? nullptr : &*frame;
    }

    const RuntimeCallFrame* sourceStorageFrame(
        const RuntimeWorkspace* workspace) const {
        if (!workspace) {
            return nullptr;
        }
        const auto frame = std::find_if(
            frames_.begin(), frames_.end(),
            [workspace](const RuntimeCallFrame& candidate) {
                return &candidate.workspace == workspace;
            });
        return frame == frames_.end() ? nullptr : &*frame;
    }

    std::optional<RuntimeSourceStorageBinding>
    sourceStorageBinding(const RuntimeWorkspace* workspace,
                         std::string_view name) const {
        const RuntimeCallFrame* frame = sourceStorageFrame(workspace);
        return frame ? runtimeSourceStorageBinding(*frame, name)
                     : std::nullopt;
    }

    RuntimeSourceStorageDeclarationResult declareSourceStorage(
        RuntimeWorkspace* workspace, RuntimeSourceStorageKind kind,
        std::string_view name, const RuntimeValue* localValue,
        SourceSpan span) {
        RuntimeCallFrame* frame = sourceStorageFrame(workspace);
        if (!frame) {
            RuntimeSourceStorageDeclarationResult result;
            result.diagnostics.push_back(Diagnostic{
                span, "dynamic storage owner workspace is unavailable",
                "MParser:MissingDynamicStorageContext"});
            return result;
        }
        auto result = runtimeDeclareSourceStorage(
            *frame, *sessionState_, kind, name, localValue, span);
        if (result.succeeded &&
            kind == RuntimeSourceStorageKind::Global &&
            frame == &frames_.front() &&
            frame->kind == RuntimeCallFrameKind::Script) {
            baseGlobalNames_.insert(std::string(name));
        }
        return result;
    }

    void clearSourceStorage(RuntimeWorkspace* workspace,
                            std::string_view name) {
        RuntimeCallFrame* frame = sourceStorageFrame(workspace);
        if (!frame) {
            return;
        }
        runtimeClearSourceStorage(*frame, name);
        if (name.empty()) {
            if (frame == &frames_.front() &&
                frame->kind == RuntimeCallFrameKind::Script) {
                baseGlobalNames_.clear();
            }
            return;
        }
        const std::string variable(name);
        if (frame == &frames_.front() &&
            frame->kind == RuntimeCallFrameKind::Script) {
            baseGlobalNames_.erase(variable);
        }
    }

    const RuntimeSourceCallable* inheritedSourceCallable(
        std::string_view name) const {
        const auto callable = std::find_if(
            inheritedSourceCallables_.begin(),
            inheritedSourceCallables_.end(),
            [name](const RuntimeSourceCallable& candidate) {
                return candidate.name == name;
            });
        return callable == inheritedSourceCallables_.end()
                   ? nullptr
                   : &*callable;
    }

    const RuntimeSourceCallable* inheritedSourceCallable(
        const RuntimeValue& handle) const {
        if (!isFunctionHandle(handle)) {
            return nullptr;
        }
        const size_t identity = handle.functionHandle->identity;
        const auto callable = std::find_if(
            inheritedSourceCallables_.begin(),
            inheritedSourceCallables_.end(),
            [identity](const RuntimeSourceCallable& candidate) {
                return isFunctionHandle(candidate.callable) &&
                       candidate.callable.functionHandle->identity ==
                           identity;
            });
        return callable == inheritedSourceCallables_.end()
                   ? nullptr
                   : &*callable;
    }

    const RuntimeSourceCallableScope* inheritedSourceCallableScope(
        const RuntimeWorkspace* workspace) const {
        const auto scope = std::find_if(
            inheritedSourceCallableScopes_.begin(),
            inheritedSourceCallableScopes_.end(),
            [workspace](const RuntimeSourceCallableScope& candidate) {
                return candidate.workspace == workspace;
            });
        return scope == inheritedSourceCallableScopes_.end()
                   ? nullptr
                   : &*scope;
    }

    std::string persistentFunctionKey(
        const FunctionInfo& info) const {
        if (!info.key.empty()) {
            return info.key;
        }
        if (!info.declaringClass.empty()) {
            return info.declaringClass + "." + info.name;
        }
        return info.name;
    }

    std::string persistentFunctionKey(
        std::string_view functionName) const {
        const auto function =
            functionsByName_.find(std::string(functionName));
        return function == functionsByName_.end()
                   ? std::string(functionName)
                   : persistentFunctionKey(function->second);
    }

    void configurePersistentScope(RuntimeCallFrame& frame,
                                  const FunctionInfo& info) const {
        frame.persistentScope = RuntimePersistentScope{
            callableContext_->identity, persistentFunctionKey(info)};
        frame.dynamicPersistentDeclarationsAllowed =
            info.lexicalParent.empty() && !info.hasNestedFunctions;
    }

    std::optional<RuntimeSourceStorageBinding>
    dynamicStorageBinding(std::string_view name) const {
        if (!frames_.empty()) {
            if (const auto local = sourceStorageBinding(
                    &frames_.back().workspace, name)) {
                return local;
            }
        }
        if (inheritedSourceStorageResolver_ &&
            inheritedSourceStorageWorkspace_) {
            return inheritedSourceStorageResolver_(
                inheritedSourceStorageWorkspace_, name);
        }
        return std::nullopt;
    }

    std::optional<RuntimeSourceStorageBinding> storageBinding(
        const BytecodeInstruction& instruction) const {
        if (instruction.binding.kind == BindingKind::GlobalVariable) {
            return RuntimeSourceStorageBinding{
                RuntimeSourceStorageKind::Global, std::nullopt};
        }
        if (instruction.binding.kind ==
            BindingKind::PersistentVariable) {
            if (const auto dynamic =
                    dynamicStorageBinding(instruction.operand);
                dynamic && dynamic->kind ==
                               RuntimeSourceStorageKind::Persistent) {
                return dynamic;
            }
            if (!frames_.empty() && frames_.back().persistentScope) {
                return RuntimeSourceStorageBinding{
                    RuntimeSourceStorageKind::Persistent,
                    frames_.back().persistentScope};
            }
            if (!activePersistentFunctionKeys_.empty()) {
                return RuntimeSourceStorageBinding{
                    RuntimeSourceStorageKind::Persistent,
                    RuntimePersistentScope{
                        callableContext_->identity,
                        activePersistentFunctionKeys_.back()}};
            }
            return RuntimeSourceStorageBinding{
                RuntimeSourceStorageKind::Persistent, std::nullopt};
        }
        return dynamicStorageBinding(instruction.operand);
    }

    void recordStorageBinding(
        std::string_view name,
        const RuntimeSourceStorageBinding& binding) {
        const std::string variable(name);
        if (binding.kind == RuntimeSourceStorageKind::Global) {
            frames_.back().globalBindings.insert(variable);
            return;
        }
        frames_.back().persistentBindings.insert(variable);
        if (!frames_.back().persistentScope && binding.persistentScope) {
            frames_.back().persistentScope = binding.persistentScope;
        }
    }

    std::optional<RuntimeValue> loadStoredVariable(
        const BytecodeInstruction& instruction,
        const RuntimeSourceStorageBinding& binding) {
        recordStorageBinding(instruction.operand, binding);
        if (binding.kind == RuntimeSourceStorageKind::Global) {
            RuntimeValue value =
                sessionState_->declareGlobal(instruction.operand);
            currentFrame()[instruction.operand] = value;
            return value;
        }
        if (!binding.persistentScope) {
            addDiagnostic(
                instruction,
                "persistent variable has no active function: " +
                    instruction.operand,
                "MParser:PersistentNotInFunction");
            return std::nullopt;
        }
        RuntimeValue value = sessionState_->declarePersistent(
            binding.persistentScope->contextIdentity,
            binding.persistentScope->function,
            instruction.operand);
        currentFrame()[instruction.operand] = value;
        return value;
    }

    std::optional<RuntimeValue> loadStoredVariable(
        const BytecodeInstruction& instruction) {
        if (const auto binding = storageBinding(instruction)) {
            return loadStoredVariable(instruction, *binding);
        }
        const auto variable = currentFrame().find(instruction.operand);
        return variable == currentFrame().end()
                   ? std::nullopt
                   : std::optional<RuntimeValue>(variable->second);
    }

    void storeVariable(const BytecodeInstruction& instruction,
                       RuntimeValue value) {
        const auto binding = storageBinding(instruction);
        if (!binding) {
            currentFrame()[instruction.operand] = std::move(value);
            return;
        }
        recordStorageBinding(instruction.operand, *binding);
        if (binding->kind == RuntimeSourceStorageKind::Global) {
            sessionState_->storeGlobal(instruction.operand, value);
            currentFrame()[instruction.operand] = std::move(value);
            return;
        }
        if (!binding->persistentScope) {
            addDiagnostic(
                instruction,
                "persistent variable has no active function: " +
                    instruction.operand,
                "MParser:PersistentNotInFunction");
            return;
        }
        sessionState_->storePersistent(
            binding->persistentScope->contextIdentity,
            binding->persistentScope->function,
            instruction.operand, value);
        currentFrame()[instruction.operand] = std::move(value);
    }

    void storeVariable(std::string name, BindingRef binding,
                       RuntimeValue value,
                       const BytecodeInstruction& source) {
        BytecodeInstruction instruction = source;
        instruction.operand = std::move(name);
        instruction.binding = binding;
        storeVariable(instruction, std::move(value));
    }

    void captureExpression(
        const BytecodeInstruction& instruction) {
        auto value = popRuntime(instruction, "top-level expression");
        if (!value || frames_.empty() ||
            frames_.back().kind != RuntimeCallFrameKind::Script ||
            value->kind == RuntimeValueKind::Missing) {
            return;
        }
        const RuntimeDisplayFormat displayFormat =
            sessionState_->displayFormat();
        const std::string displayText = runtimeFormatConsoleValue(
            *value, displayFormat);
        currentFrame()["ans"] = *value;
        expressionResults_.push_back(RuntimeExpressionResult{
            std::move(*value), instruction.span,
            instruction.outputSuppressed,
            nextConsoleSequence_++, displayText,
            displayFormat.spacing});
    }

    void declareWorkspaceVariable(
        const BytecodeInstruction& instruction) {
        if (inheritedSourceStorageDeclarer_ &&
            inheritedSourceStorageWorkspace_) {
            const auto local =
                currentFrame().find(instruction.operand);
            const RuntimeValue* localValue =
                local == currentFrame().end() ? nullptr : &local->second;
            auto result = inheritedSourceStorageDeclarer_(
                inheritedSourceStorageWorkspace_,
                instruction.op == BytecodeOp::DeclareGlobal
                    ? RuntimeSourceStorageKind::Global
                    : RuntimeSourceStorageKind::Persistent,
                instruction.operand, localValue, instruction.span);
            appendBuiltinDiagnostics(
                instruction, std::move(result.diagnostics));
            if (!result.succeeded) {
                return;
            }
            currentFrame()[instruction.operand] = result.value;
            if (result.binding) {
                recordStorageBinding(instruction.operand,
                                     *result.binding);
            }
            return;
        }
        if (instruction.op == BytecodeOp::DeclareGlobal) {
            RuntimeValue value =
                sessionState_->declareGlobal(instruction.operand);
            currentFrame()[instruction.operand] = std::move(value);
            frames_.back().globalBindings.insert(instruction.operand);
            if (frames_.size() == 1) {
                baseGlobalNames_.insert(instruction.operand);
            }
            return;
        }
        const auto binding = storageBinding(instruction);
        if (!binding || !binding->persistentScope) {
            addDiagnostic(
                instruction,
                "persistent declaration has no active function: " +
                    instruction.operand,
                "MParser:PersistentNotInFunction");
            return;
        }
        recordStorageBinding(instruction.operand, *binding);
        currentFrame()[instruction.operand] =
            sessionState_->declarePersistent(
                binding->persistentScope->contextIdentity,
                binding->persistentScope->function,
                instruction.operand);
    }

    void loadName(const BytecodeInstruction& instruction) {
        if (const auto binding = storageBinding(instruction)) {
            const auto variable =
                loadStoredVariable(instruction, *binding);
            if (variable) {
                recordLoad(instruction, *variable);
                stack_.push_back(runtimeStackValue(*variable));
            }
            return;
        }
        if (const auto variable =
                currentFrame().find(instruction.operand);
            variable != currentFrame().end()) {
            recordLoad(instruction, variable->second);
            stack_.push_back(
                runtimeStackValue(variable->second));
            return;
        }

        if (instruction.binding.kind == BindingKind::Builtin) {
            if (instruction.calleeReference) {
                stack_.push_back(
                    builtinStackValue(instruction.operand));
                return;
            }
            const BuiltinDescriptor* descriptor =
                builtinRegistry().find(instruction.operand);
            const std::string_view builtinName =
                descriptor ? std::string_view(descriptor->name)
                           : std::string_view(instruction.operand);
            if (descriptor && descriptor->handler &&
                descriptor->inputs.accepts(0)) {
                BytecodeInstruction callInstruction = instruction;
                if (const auto outputCount =
                        anonymousBodyOutputCount(callInstruction)) {
                    callInstruction.resultCount = *outputCount;
                } else if (anonymousBodyUsesImplicitOutput(
                               callInstruction) ||
                           callInstruction.implicitExpressionOutput) {
                    callInstruction.implicitExpressionOutput = true;
                    callInstruction.resultCount = static_cast<int>(
                        descriptor->implicitOutputCount(0));
                }
                auto outputs = callBuiltinOutputs(
                    callInstruction, instruction.operand, {},
                    callInstruction.resultCount);
                pushOutputValues(callInstruction, outputs);
                return;
            }
            if (builtinName == "clc" ||
                builtinName == "tic" || builtinName == "toc") {
                pushRuntime(callBuiltin(instruction, instruction.operand, {}));
                return;
            }
            if (builtinName == "pi") {
                stack_.push_back(
                    runtimeStackValue(numberValue(3.14159265358979323846)));
                return;
            }
            if (builtinName == "i" || builtinName == "j") {
                stack_.push_back(runtimeStackValue(
                    *runtimeParseNumericLiteral("1i")));
                return;
            }
            if (builtinName == "eps") {
                stack_.push_back(runtimeStackValue(
                    numberValue(std::numeric_limits<double>::epsilon())));
                return;
            }
            if (builtinName == "inf") {
                stack_.push_back(runtimeStackValue(numberValue(
                    std::numeric_limits<double>::infinity())));
                return;
            }
            if (builtinName == "nan") {
                stack_.push_back(runtimeStackValue(numberValue(
                    std::numeric_limits<double>::quiet_NaN())));
                return;
            }
            if (builtinName == "true") {
                stack_.push_back(runtimeStackValue(logicalValue(true)));
                return;
            }
            if (builtinName == "false") {
                stack_.push_back(runtimeStackValue(logicalValue(false)));
                return;
            }

            stack_.push_back(builtinStackValue(instruction.operand));
            return;
        }

        if (instruction.binding.kind == BindingKind::Function) {
            stack_.push_back(functionStackValue(instruction.operand));
            return;
        }

        if (instruction.binding.kind == BindingKind::EnumerationMember &&
            semantic_ && instruction.binding.symbolId >= 0) {
            const size_t symbolIndex =
                static_cast<size_t>(instruction.binding.symbolId);
            if (symbolIndex >= semantic_->symbols.size()) {
                addDiagnostic(instruction,
                              "enumeration member binding has an invalid "
                              "symbol");
                return;
            }
            const auto& symbol = semantic_->symbols[symbolIndex];
            std::string className = symbol.typeName;
            if (className.empty() && symbol.scopeId >= 0 &&
                static_cast<size_t>(symbol.scopeId) <
                    semantic_->scopes.size()) {
                className = semantic_->scopes[
                    static_cast<size_t>(symbol.scopeId)].label;
            }
            if (const auto value = enumerationMemberValue(
                    instruction, className, symbol.name)) {
                stack_.push_back(runtimeStackValue(*value));
            }
            return;
        }

        if (classesByName_.contains(instruction.operand)) {
            stack_.push_back(classStackValue(instruction.operand));
            return;
        }

        if (instruction.binding.kind == BindingKind::Method && semantic_ &&
            instruction.binding.symbolId >= 0) {
            const size_t symbolIndex =
                static_cast<size_t>(instruction.binding.symbolId);
            if (symbolIndex >= semantic_->symbols.size()) {
                addDiagnostic(instruction,
                              "method binding has an invalid symbol");
                return;
            }
            const auto& symbol = semantic_->symbols[symbolIndex];
            if (symbol.scopeId < 0 ||
                static_cast<size_t>(symbol.scopeId) >=
                    semantic_->scopes.size()) {
                addDiagnostic(instruction,
                              "method binding has no declaring class");
                return;
            }

            std::string className =
                semantic_->scopes[static_cast<size_t>(symbol.scopeId)].label;
            const std::string suffix = "." + symbol.name;
            const bool qualifiedReference =
                instruction.operand.size() > suffix.size() &&
                instruction.operand.ends_with(suffix);
            if (qualifiedReference) {
                className = instruction.operand.substr(
                    0, instruction.operand.size() - suffix.size());
            }

            const auto klass = classesByName_.find(className);
            const FunctionInfo* method =
                klass == classesByName_.end()
                    ? nullptr
                    : selectMethod(klass->second, symbol.name, false);
            if (!method || (qualifiedReference && !method->staticMethod)) {
                addDiagnostic(instruction,
                              "imported static method is not available: " +
                                  instruction.operand);
                return;
            }
            stack_.push_back(methodStackValue(
                className, symbol.name, method->declaringClass));
            return;
        }

        addDiagnostic(instruction,
                      "unknown bytecode runtime variable: " +
                          instruction.operand);
    }

    void loadLiteral(const BytecodeInstruction& instruction) {
        if (auto contextual = literalInIndexContext(instruction)) {
            stack_.push_back(runtimeStackValue(std::move(*contextual)));
            return;
        }

        if (instruction.operand.size() >= 2 &&
            (instruction.operand.front() == '\'' ||
             instruction.operand.front() == '"')) {
            const std::string decoded =
                decodeStringLiteral(instruction.operand);
            stack_.push_back(runtimeStackValue(
                instruction.operand.front() == '\''
                    ? makeRuntimeCharacterVectorUtf8(decoded)
                    : makeRuntimeStringScalarUtf8(decoded)));
            return;
        }

        if (auto number = runtimeParseNumericLiteral(
                instruction.operand)) {
            stack_.push_back(runtimeStackValue(std::move(*number)));
            return;
        }

        addDiagnostic(instruction,
                      "bytecode VM cannot load literal: " +
                          instruction.operand);
    }

    void loadMetaClass(const BytecodeInstruction& instruction) {
        const std::string className =
            canonicalRuntimeMetadataClassName(instruction.operand);
        if (!reflectableClassExists(className)) {
            addDiagnostic(instruction,
                          "class metadata is not available: " + className);
            return;
        }
        pushRuntime(metadataClassValue(className));
    }

    void storeName(const BytecodeInstruction& instruction) {
        const auto value = popRuntime(instruction, "store");
        if (!value) {
            return;
        }
        const auto single = runtimeRequireSingleValue(
            *value, "assignment right-hand side");
        if (!single.succeeded) {
            addDiagnostic(instruction, single.error);
            return;
        }
        storeVariable(instruction, single.value);
        recordAssignment(instruction, "name", single.value);
    }

    PropertyInfoPtr selectProperty(const ClassInfo& klass,
                                   const std::string& name,
                                   bool lexicalContext = true) const {
        const auto found = klass.properties.find(name);
        if (found == klass.properties.end() || found->second.empty()) {
            return nullptr;
        }

        if (lexicalContext && !activeClassFunctions_.empty()) {
            const auto& requestingClass =
                activeClassFunctions_.back().className;
            const auto local = std::find_if(
                found->second.rbegin(), found->second.rend(),
                [&requestingClass](const PropertyInfoPtr& property) {
                    return property->declaringClass == requestingClass;
                });
            if (local != found->second.rend()) {
                return *local;
            }
        }

        const auto visible = std::find_if(
            found->second.rbegin(), found->second.rend(),
            [this](const PropertyInfoPtr& property) {
                return !isFullyPrivateProperty(*property);
            });
        return visible != found->second.rend() ? *visible
                                               : found->second.back();
    }

    const FunctionInfo* selectMethod(const ClassInfo& klass,
                                     const std::string& name,
                                     bool lexicalContext = true) const {
        const auto privateMethods = klass.privateMethods.find(name);
        if (lexicalContext && privateMethods != klass.privateMethods.end() &&
            !activeClassFunctions_.empty()) {
            const auto& requestingClass =
                activeClassFunctions_.back().className;
            const auto local = std::find_if(
                privateMethods->second.rbegin(),
                privateMethods->second.rend(),
                [&requestingClass](const FunctionInfo& method) {
                    return method.declaringClass == requestingClass;
                });
            if (local != privateMethods->second.rend()) {
                return &*local;
            }
        }

        const auto visible = klass.methods.find(name);
        if (visible != klass.methods.end()) {
            return &visible->second;
        }
        if (privateMethods == klass.privateMethods.end() ||
            privateMethods->second.empty()) {
            return nullptr;
        }

        const auto local = std::find_if(
            privateMethods->second.rbegin(), privateMethods->second.rend(),
            [&klass](const FunctionInfo& method) {
                return method.declaringClass == klass.name;
            });
        if (local != privateMethods->second.rend()) {
            return &*local;
        }
        return lexicalContext ? &privateMethods->second.back() : nullptr;
    }

    std::optional<RuntimeValue> invokePropertyGetter(
        const BytecodeInstruction& instruction, const RuntimeValue& target,
        PropertyInfo& property) {
        if (property.constant) {
            return propertyDefault(property);
        }
        if (!property.getterName.empty() && !activePropertyGetter(property)) {
            const auto klass = classesByName_.find(property.declaringClass);
            if (klass == classesByName_.end() ||
                !klass->second.declaredMethods.contains(property.getterName)) {
                addDiagnostic(instruction,
                              "property get method is not available: " +
                                  propertyDisplayName(property));
                return std::nullopt;
            }
            const size_t diagnosticCount = diagnostics_.size();
            auto outputs = callFunctionInfo(
                instruction,
                property.declaringClass + "." + property.getterName,
                klass->second.declaredMethods.at(property.getterName),
                {target}, 1, std::nullopt, nullptr, false);
            if (diagnostics_.size() != diagnosticCount || outputs.empty()) {
                return std::nullopt;
            }
            return outputs.front();
        }
        if (property.dependent) {
            addDiagnostic(instruction,
                          "dependent property has no stored value: " +
                              propertyDisplayName(property));
            return std::nullopt;
        }
        const auto& fields = objectFields(target);
        const auto field = fields.find(property.storageKey);
        if (field == fields.end()) {
            addDiagnostic(instruction,
                          "property storage is not available: " +
                              propertyDisplayName(property));
            return std::nullopt;
        }
        return field->second;
    }

    void writeStoredProperty(RuntimeValue& target, const PropertyInfo& property,
                             const RuntimeValue& value) {
        if (target.handleObject && target.sharedFields) {
            (*target.sharedFields)[property.storageKey] = value;
        } else {
            target.fields[property.storageKey] = value;
        }
    }

    std::string dynamicPropertyDescriptorKey(
        std::string_view name) const {
        return std::string(kDynamicPropertyDescriptorPrefix) +
               std::string(name);
    }

    std::string dynamicPropertyValueKey(size_t id) const {
        return std::string(kDynamicPropertyValuePrefix) +
               std::to_string(id);
    }

    bool handleObjectIsValid(const RuntimeValue& value) const {
        if (!isObject(value) || !value.handleObject ||
            !value.sharedFields) {
            return false;
        }
        const auto valid = value.sharedFields->find(
            std::string(kHandleValidityField));
        return valid == value.sharedFields->end() ||
               truthy(valid->second);
    }

    bool handleObjectIsDestroying(const RuntimeValue& value) const {
        return value.sharedFields &&
               destroyingHandleFields_.contains(
                   value.sharedFields.get());
    }

    bool handleObjectIsUsable(const RuntimeValue& value) const {
        return handleObjectIsValid(value) ||
               handleObjectIsDestroying(value);
    }

    bool requireUsableHandleObject(
        const BytecodeInstruction& instruction,
        const RuntimeValue& value) {
        if (handleObjectIsUsable(value)) {
            return true;
        }
        addDiagnostic(instruction,
                      "invalid or deleted object: " + value.className);
        return false;
    }

    bool requireUsableHandleValue(
        const BytecodeInstruction& instruction,
        const RuntimeValue& value) {
        if (!isRuntimeClassObject(value) ||
            isRuntimeScalarObject(value)) {
            return requireUsableHandleObject(instruction, value);
        }
        for (size_t logicalIndex = 0;
             logicalIndex < runtimeObjectElementCount(value);
             ++logicalIndex) {
            const auto* element =
                runtimeObjectLogicalElement(value, logicalIndex);
            if (!element ||
                !requireUsableHandleObject(instruction, *element)) {
                return false;
            }
        }
        return true;
    }

    bool isDynamicPropertyDescriptor(
        const RuntimeValue& value) const {
        return runtimeMetadataKind(value) ==
                   RuntimeMetadataKind::DynamicProperty &&
               value.opaqueId != 0 && value.sharedFields;
    }

    bool dynamicPropertyIsValid(const RuntimeValue& descriptor) const {
        if (!isDynamicPropertyDescriptor(descriptor)) {
            return false;
        }
        const auto valid = descriptor.sharedFields->find(
            std::string(kListenerValidityField));
        if (valid == descriptor.sharedFields->end() ||
            !truthy(valid->second)) {
            return false;
        }
        const auto record = dynamicProperties_.find(descriptor.opaqueId);
        if (record == dynamicProperties_.end() ||
            record->second.ownerFields.expired()) {
            return false;
        }
        const auto descriptorFields =
            record->second.descriptorFields.lock();
        return descriptorFields &&
               descriptorFields.get() == descriptor.sharedFields.get();
    }

    std::string dynamicPropertyName(
        const RuntimeValue& descriptor) const {
        if (!descriptor.sharedFields) {
            return {};
        }
        const auto name = descriptor.sharedFields->find("Name");
        return name != descriptor.sharedFields->end()
                   ? runtimeTextScalarUtf8(name->second)
                         .value_or(std::string{})
                   : std::string{};
    }

    void registerDynamicProperty(
        const RuntimeValue& owner,
        const RuntimeValue& descriptor) {
        if (!owner.sharedFields ||
            !isDynamicPropertyDescriptor(descriptor)) {
            return;
        }
        const std::string name = dynamicPropertyName(descriptor);
        if (name.empty()) {
            return;
        }
        dynamicProperties_[descriptor.opaqueId] = DynamicPropertyRecord{
            descriptor.opaqueId, owner.sharedFields,
            descriptor.sharedFields, owner.className, name};
        nextDynamicPropertyId_ =
            std::max(nextDynamicPropertyId_, descriptor.opaqueId + 1);
    }

    void registerOwnerDynamicProperties(const RuntimeValue& owner) {
        if (!owner.sharedFields) {
            return;
        }
        for (const auto& [key, value] : *owner.sharedFields) {
            if (key.rfind(kDynamicPropertyDescriptorPrefix, 0) != 0) {
                continue;
            }
            registerDynamicProperty(owner, value);
        }
    }

    void registerWorkspaceDynamicProperties() {
        for (const auto& [name, value] : currentFrame()) {
            (void)name;
            if (isObject(value) && value.handleObject &&
                value.sharedFields &&
                !isRuntimeMetadataObject(value)) {
                registerOwnerDynamicProperties(value);
            }
        }
    }

    std::optional<RuntimeValue> dynamicPropertyDescriptor(
        const RuntimeValue& owner, std::string_view name) {
        if (!owner.sharedFields) {
            return std::nullopt;
        }
        const auto found = owner.sharedFields->find(
            dynamicPropertyDescriptorKey(name));
        if (found == owner.sharedFields->end() ||
            !isDynamicPropertyDescriptor(found->second)) {
            return std::nullopt;
        }
        registerDynamicProperty(owner, found->second);
        if (!dynamicPropertyIsValid(found->second)) {
            return std::nullopt;
        }
        return found->second;
    }

    RuntimeValue makeDynamicPropertyDescriptor(
        size_t id, const std::string& name) const {
        RuntimeValue descriptor = makeRuntimeMetadataObject(
            RuntimeMetadataKind::DynamicProperty,
            "dynamic-property/" + std::to_string(id));
        descriptor.opaqueId = id;
        descriptor.sharedFields =
            std::make_shared<std::map<std::string, RuntimeValue>>();
        auto& fields = *descriptor.sharedFields;
        fields[std::string(kListenerValidityField)] = logicalValue(true);
        fields["Name"] = characterValue(name);
        fields["Description"] = characterValue("");
        fields["DetailedDescription"] = characterValue("");
        fields["GetAccess"] = characterValue("public");
        fields["SetAccess"] = characterValue("public");
        fields["Dependent"] = logicalValue(false);
        fields["Constant"] = logicalValue(false);
        fields["Abstract"] = logicalValue(false);
        fields["Transient"] = logicalValue(false);
        fields["Hidden"] = logicalValue(false);
        fields["GetObservable"] = logicalValue(false);
        fields["SetObservable"] = logicalValue(false);
        fields["AbortSet"] = logicalValue(false);
        fields["NonCopyable"] = logicalValue(true);
        fields["WeakHandle"] = logicalValue(false);
        fields["PartialMatchPriority"] = numberValue(1.0);
        fields["HasDefault"] = logicalValue(false);
        fields["DefaultValue"] = vectorValue({});
        fields["Validation"] = makeRuntimeMetadataArray(
            RuntimeMetadataKind::PropertyValidation, {}, {0, 0});
        fields["GetMethod"] = vectorValue({});
        fields["SetMethod"] = vectorValue({});
        return descriptor;
    }

    std::optional<MemberAccessPolicy> dynamicPropertyAccessPolicy(
        const RuntimeValue& value) const {
        if (const auto text = runtimeTextScalarUtf8(value)) {
            const std::string access = lowerAscii(*text);
            if (access == "public") {
                return MemberAccessPolicy{MemberAccessLevel::Public, {}};
            }
            if (access == "protected") {
                return MemberAccessPolicy{MemberAccessLevel::Protected, {}};
            }
            if (access == "private") {
                return MemberAccessPolicy{MemberAccessLevel::Private, {},
                                          false, true};
            }
            return std::nullopt;
        }
        if (runtimeMetadataKind(value) != RuntimeMetadataKind::Class) {
            return std::nullopt;
        }
        std::vector<std::string> classes;
        if (isRuntimeMetadataScalar(value)) {
            classes.push_back(
                canonicalRuntimeMetadataClassName(value.text));
        } else {
            for (const auto& element : value.cells) {
                if (runtimeMetadataKind(element) !=
                        RuntimeMetadataKind::Class ||
                    !isRuntimeMetadataScalar(element)) {
                    return std::nullopt;
                }
                classes.push_back(
                    canonicalRuntimeMetadataClassName(element.text));
            }
        }
        return classes.empty()
                   ? MemberAccessPolicy{MemberAccessLevel::Private, {},
                                        true, true}
                   : MemberAccessPolicy{MemberAccessLevel::ClassList,
                                        std::move(classes), true};
    }

    MemberAccessPolicy dynamicPropertyAccessPolicy(
        const RuntimeValue& descriptor, std::string_view field) const {
        if (!descriptor.sharedFields) {
            return {};
        }
        const auto value = descriptor.sharedFields->find(
            std::string(field));
        if (value == descriptor.sharedFields->end()) {
            return {};
        }
        return dynamicPropertyAccessPolicy(value->second)
            .value_or(MemberAccessPolicy{});
    }

    bool dynamicPropertyLogicalField(
        const RuntimeValue& descriptor, std::string_view field) const {
        if (!descriptor.sharedFields) {
            return false;
        }
        const auto value = descriptor.sharedFields->find(
            std::string(field));
        return value != descriptor.sharedFields->end() &&
               truthy(value->second);
    }

    RuntimeValue addDynamicPropertyBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isObject(arguments[0]) ||
            !arguments[0].handleObject || !arguments[0].sharedFields ||
            !isString(arguments[1])) {
            addDiagnostic(
                instruction,
                "addprop expects a dynamicprops handle object and "
                "property-name string");
            return missingValue();
        }
        const RuntimeValue& owner = arguments[0];
        if (!requireUsableHandleObject(instruction, owner)) {
            return missingValue();
        }
        const std::string name = *runtimeTextScalarUtf8(arguments[1]);
        if (!classDerivesFrom(owner.className,
                              std::string(kDynamicPropsClassName))) {
            addDiagnostic(instruction,
                          "addprop object must derive from dynamicprops: " +
                              owner.className);
            return missingValue();
        }
        if (!isRuntimeIdentifier(name)) {
            addDiagnostic(instruction,
                          "dynamic property name is not a valid identifier: " +
                              name);
            return missingValue();
        }
        registerOwnerDynamicProperties(owner);
        const auto klass = classesByName_.find(owner.className);
        if ((klass != classesByName_.end() &&
             klass->second.properties.contains(name)) ||
            dynamicPropertyDescriptor(owner, name)) {
            addDiagnostic(instruction,
                          "property already exists: " + owner.className +
                              "." + name);
            return missingValue();
        }

        const size_t id = nextDynamicPropertyId_++;
        RuntimeValue descriptor =
            makeDynamicPropertyDescriptor(id, name);
        (*owner.sharedFields)[dynamicPropertyDescriptorKey(name)] =
            descriptor;
        (*owner.sharedFields)[dynamicPropertyValueKey(id)] =
            vectorValue({});
        registerDynamicProperty(owner, descriptor);
        return descriptor;
    }

    RuntimeValue findPropertyBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isObject(arguments[0]) ||
            !arguments[0].handleObject || !isString(arguments[1])) {
            addDiagnostic(instruction,
                          "findprop expects a handle object and "
                          "property-name string");
            return missingValue();
        }
        const RuntimeValue& owner = arguments[0];
        if (!requireUsableHandleObject(instruction, owner)) {
            return missingValue();
        }
        const std::string name = *runtimeTextScalarUtf8(arguments[1]);
        if (const auto descriptor =
                dynamicPropertyDescriptor(owner, name)) {
            return *descriptor;
        }
        const auto klass = classesByName_.find(owner.className);
        if (klass != classesByName_.end()) {
            if (const auto property =
                    selectProperty(klass->second, name)) {
                return makeRuntimeMetadataObject(
                    RuntimeMetadataKind::Property,
                    propertyMetadataIdentity(klass->second, *property));
            }
        }
        return emptyMetadataArray(RuntimeMetadataKind::Property);
    }

    RuntimeValue findMetadataObjectsBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() < 3 ||
            arguments.size() % 2 == 0 ||
            !isRuntimeMetadataObject(arguments.front())) {
            addDiagnostic(
                instruction,
                "findobj expects a metadata object array followed by "
                "property-name/value pairs");
            return missingValue();
        }
        for (size_t index = 1; index < arguments.size(); index += 2) {
            if (!isString(arguments[index])) {
                addDiagnostic(
                    instruction,
                    "findobj metadata property names must be text scalars");
                return missingValue();
            }
        }

        std::vector<RuntimeValue> candidates;
        if (isRuntimeMetadataScalar(arguments.front())) {
            candidates.push_back(arguments.front());
        } else {
            candidates = arguments.front().cells;
        }

        std::vector<RuntimeValue> matches;
        for (const auto& candidate : candidates) {
            if (!isRuntimeMetadataScalar(candidate)) {
                addDiagnostic(
                    instruction,
                    "findobj metadata array contains a non-scalar element");
                return missingValue();
            }
            bool matched = true;
            for (size_t index = 1; index < arguments.size();
                 index += 2) {
                const std::string propertyName =
                    *runtimeTextScalarUtf8(arguments[index]);
                const auto names =
                    runtimeMetadataPropertyNames(candidate.className);
                if (std::find(names.begin(), names.end(),
                              propertyName) == names.end()) {
                    addDiagnostic(
                        instruction,
                        "findobj metadata property is not available: " +
                            canonicalRuntimeMetadataClassName(
                                candidate.className) +
                            "." + propertyName);
                    return missingValue();
                }
                const size_t diagnosticCount = diagnostics_.size();
                const RuntimeValue actual =
                    metadataScalarMemberValue(
                        instruction, candidate, propertyName);
                if (diagnostics_.size() != diagnosticCount) {
                    return missingValue();
                }
                if (!runtimeEqual(actual, arguments[index + 1])) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                matches.push_back(candidate);
            }
        }

        if (matches.size() == 1) {
            return std::move(matches.front());
        }
        const RuntimeMetadataKind kind =
            *runtimeMetadataKind(arguments.front());
        const size_t matchCount = matches.size();
        return makeRuntimeMetadataArray(
            kind, std::move(matches), {matchCount, 1});
    }

    RuntimeValue findClassObjectsBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        const RuntimeValue& source = arguments.front();
        if (!isRuntimeClassObject(source) ||
            !source.handleObject) {
            addDiagnostic(
                instruction,
                "findobj expects a handle object or metadata array");
            return missingValue();
        }

        std::vector<RuntimeValue> candidates;
        candidates.reserve(runtimeObjectElementCount(source));
        for (size_t index = 0;
             index < runtimeObjectElementCount(source); ++index) {
            const auto* candidate =
                runtimeObjectLogicalElement(source, index);
            if (!candidate) {
                addDiagnostic(
                    instruction,
                    "findobj could not map a handle-array element");
                return missingValue();
            }
            candidates.push_back(*candidate);
        }

        std::vector<RuntimeValue> matches;
        for (const auto& candidate : candidates) {
            bool matched = true;
            for (size_t index = 1; index < arguments.size();
                 index += 2) {
                const std::string propertyName =
                    *runtimeTextScalarUtf8(arguments[index]);
                const size_t diagnosticCount = diagnostics_.size();
                BytecodeInstruction memberInstruction = instruction;
                memberInstruction.operand = propertyName;
                memberInstruction.resultCount = 1;
                const auto actual = resolveScalarObjectMember(
                    memberInstruction, candidate);
                if (diagnostics_.size() != diagnosticCount) {
                    return missingValue();
                }
                if (!actual || actual->isBuiltinReference ||
                    actual->isFunctionReference ||
                    actual->isClassReference ||
                    actual->isMethodReference) {
                    addDiagnostic(
                        instruction,
                        "findobj class member is not a readable property: " +
                            candidate.className + "." + propertyName);
                    return missingValue();
                }
                if (!runtimeEqual(
                        actual->value, arguments[index + 1])) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                matches.push_back(candidate);
            }
        }

        if (matches.size() == 1) {
            return std::move(matches.front());
        }
        const size_t matchCount = matches.size();
        auto result = runtimeMakeObjectArrayFromLogicalOrder(
            std::move(matches), {matchCount, 1}, source.className,
            source.handleObject, objectArrayPolicy(instruction),
            source.className);
        if (!result.succeeded) {
            addDiagnostic(
                instruction, "findobj " + std::move(result.error));
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue findObjectsBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty() ||
            arguments.size() % 2 == 0) {
            addDiagnostic(
                instruction,
                "findobj expects a source followed by "
                "property-name/value pairs");
            return missingValue();
        }
        if (arguments.size() == 1) {
            if (isRuntimeMetadataObject(arguments.front()) ||
                (isRuntimeClassObject(arguments.front()) &&
                 arguments.front().handleObject)) {
                return arguments.front();
            }
            addDiagnostic(
                instruction,
                "findobj expects a handle object or metadata array");
            return missingValue();
        }
        for (size_t index = 1; index < arguments.size(); index += 2) {
            if (!isString(arguments[index])) {
                addDiagnostic(
                    instruction,
                    "findobj property names must be text scalars");
                return missingValue();
            }
        }
        return isRuntimeMetadataObject(arguments.front())
                   ? findMetadataObjectsBuiltin(
                         instruction, arguments)
                   : findClassObjectsBuiltin(
                         instruction, arguments);
    }

    RuntimeValue dynamicPropertyMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& descriptor, std::string_view memberName) {
        if (!dynamicPropertyIsValid(descriptor)) {
            addDiagnostic(instruction,
                          "dynamic property descriptor is not valid");
            return missingValue();
        }
        if (memberName == "DefiningClass") {
            return emptyMetadataArray(RuntimeMetadataKind::Class);
        }
        if (descriptor.sharedFields) {
            const auto value = descriptor.sharedFields->find(
                std::string(memberName));
            if (value != descriptor.sharedFields->end()) {
                return value->second;
            }
        }
        addDiagnostic(instruction,
                      "dynamic property metadata field is not available: " +
                          dynamicPropertyName(descriptor) + "." +
                          std::string(memberName));
        return missingValue();
    }

    void storeDynamicPropertyMetadata(
        const BytecodeInstruction& instruction,
        RuntimeValue& descriptor, const RuntimeValue& value) {
        if (!dynamicPropertyIsValid(descriptor)) {
            addDiagnostic(instruction,
                          "dynamic property descriptor is not valid");
            return;
        }
        const std::string& member = instruction.operand;
        static const std::set<std::string> logicalMembers = {
            "AbortSet", "GetObservable", "Hidden", "NonCopyable",
            "SetObservable", "Transient", "WeakHandle"};
        if (logicalMembers.contains(member)) {
            const auto logical = isNumber(value)
                                     ? runtimeNumericTruthValue(value)
                                     : std::nullopt;
            if (!logical) {
                addDiagnostic(instruction,
                              "dynamic property logical metadata requires "
                              "a real scalar numeric value without NaN: " +
                                  member);
                return;
            }
            (*descriptor.sharedFields)[member] =
                logicalValue(*logical);
            return;
        }
        if (member == "GetAccess" || member == "SetAccess") {
            if (!dynamicPropertyAccessPolicy(value)) {
                addDiagnostic(instruction,
                              "unsupported dynamic property access value: " +
                                  member);
                return;
            }
            (*descriptor.sharedFields)[member] = value;
            return;
        }
        if (member == "GetMethod" || member == "SetMethod") {
            const bool empty = value.kind == RuntimeValueKind::Missing ||
                               elementCount(value) == 0;
            if (!empty && !isFunctionHandle(value)) {
                addDiagnostic(instruction,
                              "dynamic property " + member +
                                  " requires a function handle or empty "
                                  "value");
                return;
            }
            (*descriptor.sharedFields)[member] =
                empty ? vectorValue({}) : value;
            return;
        }
        if (member == "PartialMatchPriority") {
            const auto element = isNumber(value)
                                     ? runtimeNumericElementValue(value, 0)
                                     : std::nullopt;
            if (!element || element->complex ||
                !std::isfinite(element->real) ||
                element->real <= 0.0) {
                addDiagnostic(
                    instruction,
                    "dynamic property PartialMatchPriority requires a "
                    "positive real scalar");
                return;
            }
            (*descriptor.sharedFields)[member] = value;
            return;
        }
        addDiagnostic(instruction,
                      "dynamic property metadata is read-only: " +
                          descriptor.className + "." + member);
    }

    std::optional<RuntimeValue> readDynamicProperty(
        const BytecodeInstruction& instruction,
        const RuntimeValue& owner,
        const RuntimeValue& descriptor) {
        if (!dynamicPropertyIsValid(descriptor)) {
            addDiagnostic(instruction,
                          "dynamic property descriptor is not valid");
            return std::nullopt;
        }
        if (!hasMemberAccess(
                dynamicPropertyAccessPolicy(descriptor, "GetAccess"),
                owner.className)) {
            addDiagnostic(instruction,
                          "dynamic property get access is denied: " +
                              owner.className + "." +
                              dynamicPropertyName(descriptor));
            return std::nullopt;
        }
        const size_t id = descriptor.opaqueId;
        const auto stored = owner.sharedFields->find(
            dynamicPropertyValueKey(id));
        if (stored == owner.sharedFields->end()) {
            addDiagnostic(instruction,
                          "dynamic property storage is not available: " +
                              dynamicPropertyName(descriptor));
            return std::nullopt;
        }
        const auto getter = descriptor.sharedFields->find("GetMethod");
        if (activeDynamicPropertyGetters_.contains(id) ||
            getter == descriptor.sharedFields->end() ||
            !isFunctionHandle(getter->second)) {
            return stored->second;
        }

        activeDynamicPropertyGetters_.insert(id);
        const size_t diagnosticCount = diagnostics_.size();
        auto outputs = callFunctionHandle(instruction, getter->second,
                                          {owner}, 1);
        activeDynamicPropertyGetters_.erase(id);
        if (diagnostics_.size() != diagnosticCount || outputs.empty()) {
            return std::nullopt;
        }
        return outputs.front();
    }

    bool writeDynamicProperty(
        const BytecodeInstruction& instruction,
        RuntimeValue& owner, const RuntimeValue& descriptor,
        const RuntimeValue& value) {
        if (!dynamicPropertyIsValid(descriptor)) {
            addDiagnostic(instruction,
                          "dynamic property descriptor is not valid");
            return false;
        }
        if (!hasMemberAccess(
                dynamicPropertyAccessPolicy(descriptor, "SetAccess"),
                owner.className)) {
            addDiagnostic(instruction,
                          "dynamic property set access is denied: " +
                              owner.className + "." +
                              dynamicPropertyName(descriptor));
            return false;
        }
        const size_t id = descriptor.opaqueId;
        const std::string storageKey = dynamicPropertyValueKey(id);
        if (activeDynamicPropertySetters_.contains(id)) {
            (*owner.sharedFields)[storageKey] = value;
            return true;
        }
        if (dynamicPropertyLogicalField(descriptor, "AbortSet")) {
            const auto current =
                readDynamicProperty(instruction, owner, descriptor);
            if (!current) {
                return false;
            }
            if (runtimeEqual(*current, value)) {
                return true;
            }
        }
        const bool observable =
            dynamicPropertyLogicalField(descriptor, "SetObservable");
        if (observable &&
            !dispatchPropertyEvent(instruction, owner, descriptor, {}, id,
                                   "PreSet")) {
            return false;
        }
        const auto setter = descriptor.sharedFields->find("SetMethod");
        if (setter != descriptor.sharedFields->end() &&
            isFunctionHandle(setter->second)) {
            activeDynamicPropertySetters_.insert(id);
            const size_t diagnosticCount = diagnostics_.size();
            (void)callFunctionHandle(instruction, setter->second,
                                     {owner, value}, 0);
            activeDynamicPropertySetters_.erase(id);
            if (diagnostics_.size() != diagnosticCount) {
                return false;
            }
        } else {
            (*owner.sharedFields)[storageKey] = value;
        }
        if (observable) {
            (void)dispatchPropertyEvent(instruction, owner, descriptor, {},
                                        id, "PostSet");
        }
        return true;
    }

    void deleteDynamicProperty(
        const BytecodeInstruction& instruction,
        const RuntimeValue& descriptor) {
        if (!isDynamicPropertyDescriptor(descriptor)) {
            addDiagnostic(instruction,
                          "delete expects a handle object or dynamic "
                          "property descriptor");
            return;
        }
        if (!dynamicPropertyIsValid(descriptor)) {
            return;
        }
        const auto record = dynamicProperties_.find(descriptor.opaqueId);
        if (record == dynamicProperties_.end()) {
            addDiagnostic(instruction,
                          "dynamic property owner is not available");
            return;
        }
        const auto owner = record->second.ownerFields.lock();
        if (owner) {
            owner->erase(dynamicPropertyDescriptorKey(
                record->second.name));
            owner->erase(dynamicPropertyValueKey(descriptor.opaqueId));
        }
        for (auto& [id, listener] : eventListeners_) {
            (void)id;
            const auto listenerSource = listener.sourceFields.lock();
            if (!listener.propertyListener ||
                listener.dynamicPropertyId != descriptor.opaqueId ||
                !owner || !listenerSource ||
                listenerSource.get() != owner.get()) {
                continue;
            }
            if (const auto fields = listener.listenerFields.lock()) {
                (*fields)[std::string(kListenerValidityField)] =
                    logicalValue(false);
            }
        }
        (*descriptor.sharedFields)[std::string(kListenerValidityField)] =
            logicalValue(false);
    }

    RuntimeValue stringColumnCell(
        const std::vector<std::string>& values) const {
        std::vector<RuntimeValue> cells;
        cells.reserve(values.size());
        for (const auto& value : values) {
            cells.push_back(characterValue(value));
        }
        return cellValueForShape(values.size(), 1, std::move(cells));
    }

    RuntimeValue accessMetadataValue(
        const MemberAccessPolicy& access) const {
        if (access.selectiveClassList) {
            if (access.classNames.empty()) {
                return metadataClassArray({});
            }
            if (access.classNames.size() == 1) {
                return metadataClassValue(access.classNames.front());
            }
            return metadataClassArray(access.classNames);
        }
        switch (access.level) {
        case MemberAccessLevel::Public:
            return characterValue("public");
        case MemberAccessLevel::Protected:
            return characterValue("protected");
        case MemberAccessLevel::Private:
            return characterValue("private");
        case MemberAccessLevel::Immutable:
            return characterValue("immutable");
        case MemberAccessLevel::ClassList:
            return metadataClassArray(access.classNames);
        }
        return characterValue("public");
    }

    RuntimeValue propertyMetadataList(const ClassInfo& klass) const {
        std::vector<RuntimeValue> values;
        std::set<std::string> seen;
        auto append = [&](const PropertyInfoPtr& property) {
            if (!property ||
                (property->declaringClass != klass.name &&
                 isFullyPrivateProperty(*property)) ||
                !seen.insert(property->storageKey).second) {
                return;
            }
            values.push_back(makeRuntimeMetadataObject(
                RuntimeMetadataKind::Property,
                propertyMetadataIdentity(klass, *property)));
        };
        for (const auto& property : klass.propertyOrder) {
            append(property);
        }
        for (const auto& [name, property] : klass.abstractProperties) {
            (void)name;
            append(property);
        }
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::Property, std::move(values),
            {valueCount, 1});
    }

    RuntimeValue methodMetadataList(const ClassInfo& klass) const {
        std::vector<RuntimeValue> values;
        std::set<std::string> seen;
        auto append = [&](const FunctionInfo& method) {
            if (method.declaringClass != klass.name &&
                method.access.level == MemberAccessLevel::Private &&
                method.access.privateMemberIdentity) {
                return;
            }
            const std::string identity =
                methodMetadataIdentity(klass, method);
            if (!seen.insert(identity).second) {
                return;
            }
            values.push_back(makeRuntimeMetadataObject(
                RuntimeMetadataKind::Method, identity));
        };
        for (const auto& [name, method] : klass.methods) {
            (void)name;
            append(method);
        }
        for (const auto& [name, method] : klass.abstractMethods) {
            (void)name;
            append(method);
        }
        for (const auto& [name, method] : klass.declaredMethods) {
            (void)name;
            append(method);
        }
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::Method, std::move(values),
            {valueCount, 1});
    }

    RuntimeValue eventMetadataList(const ClassInfo& klass) const {
        std::vector<RuntimeValue> values;
        values.reserve(klass.eventOrder.size());
        for (const auto& eventName : klass.eventOrder) {
            const auto event = klass.events.find(eventName);
            if (event == klass.events.end()) {
                continue;
            }
            values.push_back(makeRuntimeMetadataObject(
                RuntimeMetadataKind::Event,
                eventMetadataIdentity(klass, event->second)));
        }
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::Event, std::move(values),
            {valueCount, 1});
    }

    RuntimeValue enumerationMetadataList(const ClassInfo& klass) const {
        std::vector<RuntimeValue> values;
        values.reserve(klass.declaredEnumerationOrder.size());
        for (const auto& memberName : klass.declaredEnumerationOrder) {
            values.push_back(makeRuntimeMetadataObject(
                RuntimeMetadataKind::EnumerationMember,
                klass.name + "/" + memberName));
        }
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            RuntimeMetadataKind::EnumerationMember, std::move(values),
            {valueCount, 1});
    }

    std::vector<std::string> builtinMetadataPropertyNames(
        std::string_view className) const {
        const std::string canonical =
            canonicalRuntimeMetadataClassName(className);
        if (findRuntimeMetadataTypeDescriptor(canonical)) {
            return runtimeMetadataPropertyNames(canonical);
        }
        if (canonical == kEventDataClassName) {
            return {"Source", "EventName"};
        }
        if (canonical == kPropertyEventClassName) {
            return {"AffectedObject", "Source", "EventName"};
        }
        if (canonical == kEventListenerClassName) {
            return {"Source", "EventName", "Callback", "Enabled",
                    "Recursive"};
        }
        if (canonical == kPropertyListenerClassName) {
            return {"Source", "EventName", "Callback", "Enabled",
                    "Recursive", "Object"};
        }
        return {};
    }

    std::vector<std::string> builtinMetadataMethodNames(
        std::string_view className) const {
        const std::string canonical =
            canonicalRuntimeMetadataClassName(className);
        if (findRuntimeMetadataTypeDescriptor(canonical)) {
            return runtimeMetadataMethodNames(canonical);
        }
        if (canonical == "handle") {
            return {"addlistener", "delete", "findobj", "findprop",
                    "isvalid", "listener", "notify"};
        }
        if (canonical == kDynamicPropsClassName) {
            return {"addlistener", "addprop", "delete", "findobj",
                    "findprop", "isvalid", "listener", "notify"};
        }
        if (canonical == kEventListenerClassName ||
            canonical == kPropertyListenerClassName) {
            return {"delete", "isvalid"};
        }
        return {};
    }

    RuntimeValue namespaceMetadataValue(
        std::string_view className) const {
        const size_t dot = className.find_last_of('.');
        if (dot == std::string_view::npos) {
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::Namespace, {}, {0, 1});
        }
        return makeRuntimeMetadataObject(
            RuntimeMetadataKind::Namespace,
            std::string(className.substr(0, dot)));
    }

    RuntimeValue metadataClassMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const std::string className =
            canonicalRuntimeMetadataClassName(target.text);
        const auto found = classesByName_.find(className);
        const ClassInfo* klass =
            found == classesByName_.end() ? nullptr : &found->second;
        const auto* metadataType =
            findRuntimeMetadataTypeDescriptor(className);
        if (!klass && !isBuiltinReflectableClass(className)) {
            addDiagnostic(instruction,
                          "metadata class is not available: " + className);
            return missingValue();
        }

        if (memberName == "Name") {
            return characterValue(className);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "Hidden") {
            return logicalValue(
                klass ? klass->hidden
                      : metadataType && metadataType->hiddenClass);
        }
        if (memberName == "Sealed") {
            return logicalValue(
                klass ? klass->sealedClass
                      : (metadataType && metadataType->sealedClass) ||
                            className == kPropertyEventClassName);
        }
        if (memberName == "Abstract") {
            return logicalValue(
                klass ? klass->abstractClass
                      : (metadataType &&
                         metadataType->abstractClass) ||
                            className == "handle" ||
                            className == "numeric" ||
                            className == kDynamicPropsClassName);
        }
        if (memberName == "Enumeration") {
            return logicalValue(klass && klass->enumerationClass);
        }
        if (memberName == "ConstructOnLoad") {
            return logicalValue(
                klass ? klass->constructOnLoad
                      : (metadataType &&
                         metadataType->constructOnLoad) ||
                            className == kPropertyEventClassName ||
                            className == kPropertyListenerClassName);
        }
        if (memberName == "HandleCompatible") {
            const bool builtInHandleCompatible =
                className == "handle" ||
                className == kDynamicPropsClassName ||
                className == kEventDataClassName ||
                className == kPropertyEventClassName ||
                className == kEventListenerClassName ||
                className == kPropertyListenerClassName ||
                (metadataType &&
                 metadataType->handleCompatible);
            return logicalValue(
                klass ? klass->handleClass ||
                            klass->handleCompatible
                      : builtInHandleCompatible);
        }
        if (memberName == "RestrictsSubclassing") {
            return logicalValue(
                klass ? klass->sealedClass ||
                            klass->restrictsSubclassing
                      : (metadataType &&
                         metadataType->restrictsSubclassing) ||
                            className == kPropertyEventClassName);
        }
        if (memberName == "InferiorClasses") {
            return metadataClassArray({});
        }
        if (memberName == "Namespace" ||
            memberName == "ContainingPackage") {
            return namespaceMetadataValue(className);
        }
        if (memberName == "Aliases") {
            return cellValueForShape(0, 1, {});
        }
        if (memberName == "PropertyList") {
            return klass
                       ? propertyMetadataList(*klass)
                       : makeRuntimeMetadataArray(
                             RuntimeMetadataKind::Property, {}, {0, 1});
        }
        if (memberName == "MethodList") {
            return klass
                       ? methodMetadataList(*klass)
                       : makeRuntimeMetadataArray(
                             RuntimeMetadataKind::Method, {}, {0, 1});
        }
        if (memberName == "EventList") {
            if (klass) {
                return eventMetadataList(*klass);
            }
            if (isBuiltinHandleRuntimeClass(className)) {
                return makeRuntimeMetadataArray(
                    RuntimeMetadataKind::Event,
                    {makeRuntimeMetadataObject(
                        RuntimeMetadataKind::Event,
                        className + "/handle/" +
                            std::string(
                                kObjectBeingDestroyedEventName))},
                    {1, 1});
            }
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::Event, {}, {0, 1});
        }
        if (memberName == "EnumerationMemberList") {
            return klass
                       ? enumerationMetadataList(*klass)
                       : makeRuntimeMetadataArray(
                             RuntimeMetadataKind::EnumerationMember, {},
                             {0, 1});
        }
        if (memberName == "SuperclassList") {
            if (klass) {
                return metadataClassArray(klass->superclasses);
            }
            if (metadataType && metadataType->superclass) {
                return metadataClassArray(
                    {std::string(runtimeMetadataClassName(
                        *metadataType->superclass))});
            }
            if (className == kPropertyEventClassName) {
                return metadataClassArray(
                    {std::string(kEventDataClassName)});
            }
            if (className == kPropertyListenerClassName) {
                return metadataClassArray(
                    {std::string(kEventListenerClassName)});
            }
            if (className == kDynamicPropsClassName ||
                className == kEventDataClassName ||
                className == kEventListenerClassName) {
                return metadataClassArray({"handle"});
            }
            if (className == "double") {
                return metadataClassArray({"numeric"});
            }
            return metadataClassArray({});
        }

        addDiagnostic(instruction,
                      "metadata class property is not available: " +
                          className + "." + std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataPropertyMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        PropertyInfoPtr property = propertyForMetadata(target.text);
        if (!property) {
            addDiagnostic(instruction,
                          "metadata property descriptor is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(property->name);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "GetAccess") {
            return accessMetadataValue(property->getAccess);
        }
        if (memberName == "SetAccess") {
            return accessMetadataValue(property->setAccess);
        }
        if (memberName == "Dependent") {
            return logicalValue(property->dependent);
        }
        if (memberName == "Constant") {
            return logicalValue(property->constant);
        }
        if (memberName == "Abstract") {
            return logicalValue(property->abstractProperty);
        }
        if (memberName == "Transient") {
            return logicalValue(property->transient);
        }
        if (memberName == "Hidden") {
            return logicalValue(property->hidden);
        }
        if (memberName == "GetObservable") {
            return logicalValue(property->getObservable);
        }
        if (memberName == "SetObservable") {
            return logicalValue(property->setObservable);
        }
        if (memberName == "AbortSet") {
            return logicalValue(property->abortSet);
        }
        if (memberName == "NonCopyable") {
            return logicalValue(property->nonCopyable);
        }
        if (memberName == "WeakHandle") {
            return logicalValue(property->weakHandle);
        }
        if (memberName == "PartialMatchPriority") {
            return numberValue(property->partialMatchPriority);
        }
        if (memberName == "HasDefault") {
            return logicalValue(property->spec.hasExplicitDefault);
        }
        if (memberName == "DefaultValue") {
            if (!property->spec.hasExplicitDefault) {
                addDiagnostic(instruction,
                              "metadata property has no default value: " +
                                  propertyDisplayName(*property));
                return missingValue();
            }
            const auto value = propertyDefault(*property);
            return value.value_or(missingValue());
        }
        if (memberName == "Validation") {
            const bool validated =
                !property->spec.className.empty() ||
                !property->spec.dimensions.empty() ||
                !property->spec.validators.empty();
            return validated
                       ? makeRuntimeMetadataObject(
                             RuntimeMetadataKind::PropertyValidation,
                             target.text)
                       : emptyMetadataArray(
                             RuntimeMetadataKind::PropertyValidation);
        }
        if (memberName == "GetMethod" ||
            memberName == "SetMethod") {
            return missingValue();
        }
        if (memberName == "DefiningClass") {
            return metadataClassValue(property->declaringClass);
        }

        addDiagnostic(instruction,
                      "metadata property field is not available: " +
                          propertyDisplayName(*property) + "." +
                          std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataMethodMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const FunctionInfo* method = methodForMetadata(target.text);
        if (!method) {
            addDiagnostic(instruction,
                          "metadata method descriptor is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(method->displayName.empty()
                                      ? method->name
                                      : method->displayName);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "Access") {
            return accessMetadataValue(method->access);
        }
        if (memberName == "Static") {
            return logicalValue(method->staticMethod);
        }
        if (memberName == "Abstract") {
            return logicalValue(method->abstractMethod);
        }
        if (memberName == "Sealed") {
            return logicalValue(method->sealedMethod);
        }
        if (memberName == "Hidden") {
            return logicalValue(method->hidden);
        }
        if (memberName == "InputNames") {
            std::vector<std::string> names = method->signature.parameters;
            if (method->signature.hasVarargin) {
                names.push_back("varargin");
            }
            return stringColumnCell(names);
        }
        if (memberName == "OutputNames") {
            std::vector<std::string> names = method->signature.outputs;
            if (method->signature.hasVarargout) {
                names.push_back("varargout");
            }
            return stringColumnCell(names);
        }
        if (memberName == "Signature") {
            return makeRuntimeMetadataObject(
                RuntimeMetadataKind::CallSignature,
                methodCallSignatureIdentity(target.text));
        }
        if (memberName == "FullPath") {
            return characterValue(method->fullPath);
        }
        if (memberName == "DefiningClass") {
            return metadataClassValue(method->declaringClass);
        }

        addDiagnostic(instruction,
                      "metadata method field is not available: " +
                          method->declaringClass + "." + method->name +
                          "." + std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataFunctionMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const FunctionInfo* function =
            functionForMetadata(target.text);
        if (!function) {
            addDiagnostic(instruction,
                          "metadata function descriptor is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(function->displayName);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "FullPath") {
            return characterValue(function->fullPath);
        }
        if (memberName == "NamespaceName") {
            return characterValue(function->namespaceName);
        }
        if (memberName == "Signature") {
            return makeRuntimeMetadataObject(
                RuntimeMetadataKind::CallSignature,
                functionCallSignatureIdentity(*function));
        }
        addDiagnostic(instruction,
                      "metadata function field is not available: " +
                          function->metadataIdentifier + "." +
                          std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataCallSignatureMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const FunctionInfo* callable =
            callableForCallSignature(target.text);
        if (!callable) {
            addDiagnostic(
                instruction,
                "metadata call signature descriptor is not available");
            return missingValue();
        }
        const auto argumentArray =
            [&](bool input) {
                const auto arguments =
                    input ? reflectedInputArguments(*callable)
                          : reflectedOutputArguments(*callable);
                std::vector<RuntimeValue> values;
                values.reserve(arguments.size());
                for (size_t index = 0; index < arguments.size(); ++index) {
                    values.push_back(makeRuntimeMetadataObject(
                        RuntimeMetadataKind::Argument,
                        target.text +
                            (input ? "/input/" : "/output/") +
                            std::to_string(index)));
                }
                const size_t valueCount = values.size();
                return makeRuntimeMetadataArray(
                    RuntimeMetadataKind::Argument, std::move(values),
                    {1, valueCount});
            };
        if (memberName == "Inputs") {
            return argumentArray(true);
        }
        if (memberName == "Outputs") {
            return argumentArray(false);
        }
        if (memberName == "HasInputValidation") {
            return logicalValue(callable->hasInputArgumentBlock);
        }
        if (memberName == "HasOutputValidation") {
            return logicalValue(callable->hasOutputArgumentBlock);
        }
        addDiagnostic(
            instruction,
            "metadata call signature field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataArgumentMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        std::string signatureIdentity;
        const auto argument = reflectedArgumentForMetadata(
            target.text, &signatureIdentity);
        if (!argument) {
            addDiagnostic(instruction,
                          "metadata argument descriptor is not available");
            return missingValue();
        }
        if (memberName == "Identifier") {
            return argumentIdentifierValue(
                argument->identifierName, argument->groupName);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "Required") {
            return logicalValue(argument->required);
        }
        if (memberName == "Repeating") {
            return logicalValue(argument->repeating);
        }
        if (memberName == "NameValue") {
            return logicalValue(argument->nameValue);
        }
        if (memberName == "Validation") {
            const bool validated =
                argument->contract &&
                (!argument->contract->spec.className.empty() ||
                 !argument->contract->spec.dimensions.empty() ||
                 !argument->contract->spec.validators.empty());
            return validated
                       ? makeRuntimeMetadataObject(
                             RuntimeMetadataKind::ArgumentValidation,
                             target.text)
                       : emptyMetadataArray(
                             RuntimeMetadataKind::ArgumentValidation);
        }
        if (memberName == "DefaultValue") {
            return argument->contract &&
                           argument->contract->spec.hasExplicitDefault
                       ? makeRuntimeMetadataObject(
                             RuntimeMetadataKind::DefaultArgumentValue,
                             target.text)
                       : emptyMetadataArray(
                             RuntimeMetadataKind::DefaultArgumentValue);
        }
        if (memberName == "SourceClass") {
            if (!argument->contract ||
                argument->contract->sourceClass.empty()) {
                return emptyMetadataArray(RuntimeMetadataKind::Class);
            }
            return metadataClassValue(argument->contract->sourceClass);
        }
        addDiagnostic(instruction,
                      "metadata argument field is not available: " +
                          argument->name + "." +
                          std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataArgumentIdentifierMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const auto [groupName, name] =
            splitMetadataIdentity(target.text);
        if (name.empty()) {
            addDiagnostic(
                instruction,
                "metadata argument identifier is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(name);
        }
        if (memberName == "GroupName") {
            return characterValue(groupName);
        }
        addDiagnostic(
            instruction,
            "metadata argument identifier field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataValidationSize(
        std::string_view identity, const PropertySpec& spec) const {
        std::vector<RuntimeValue> values;
        values.reserve(spec.dimensions.size());
        bool allFixed = !spec.dimensions.empty();
        bool allUnrestricted = !spec.dimensions.empty();
        for (size_t index = 0; index < spec.dimensions.size(); ++index) {
            const bool unrestricted =
                spec.dimensions[index].text == ":";
            allFixed = allFixed && !unrestricted;
            allUnrestricted = allUnrestricted && unrestricted;
            values.push_back(makeRuntimeMetadataObject(
                unrestricted
                    ? RuntimeMetadataKind::UnrestrictedDimension
                    : RuntimeMetadataKind::FixedDimension,
                std::string(identity) + "/dimension/" +
                    std::to_string(index)));
        }
        const RuntimeMetadataKind kind =
            allFixed
                ? RuntimeMetadataKind::FixedDimension
                : allUnrestricted
                ? RuntimeMetadataKind::UnrestrictedDimension
                : RuntimeMetadataKind::ArrayDimension;
        const size_t valueCount = values.size();
        return makeRuntimeMetadataArray(
            kind, std::move(values), {1, valueCount});
    }

    RuntimeValue metadataArgumentValidationMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const auto argument =
            reflectedArgumentForMetadata(target.text);
        if (!argument || !argument->contract) {
            addDiagnostic(
                instruction,
                "metadata argument validation descriptor is not available");
            return missingValue();
        }
        const PropertySpec& spec = argument->contract->spec;
        if (memberName == "Class") {
            return spec.className.empty()
                       ? emptyMetadataArray(RuntimeMetadataKind::Class)
                       : metadataClassValue(spec.className);
        }
        if (memberName == "Size") {
            return metadataValidationSize(target.text, spec);
        }
        if (memberName == "Functions") {
            std::vector<RuntimeValue> values;
            values.reserve(spec.validators.size());
            for (size_t index = 0; index < spec.validators.size(); ++index) {
                values.push_back(makeRuntimeMetadataObject(
                    RuntimeMetadataKind::ArgumentValidator,
                    target.text + "/validator/" +
                        std::to_string(index)));
            }
            const size_t valueCount = values.size();
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::ArgumentValidator,
                std::move(values), {1, valueCount});
        }
        addDiagnostic(
            instruction,
            "metadata argument validation field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataPropertyValidationMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const PropertyInfoPtr property =
            propertyForMetadata(target.text);
        if (!property) {
            addDiagnostic(
                instruction,
                "metadata property validation descriptor is not available");
            return missingValue();
        }
        const PropertySpec& spec = property->spec;
        if (memberName == "Class") {
            return spec.className.empty()
                       ? emptyMetadataArray(RuntimeMetadataKind::Class)
                       : metadataClassValue(spec.className);
        }
        if (memberName == "Size") {
            return metadataValidationSize(target.text, spec);
        }
        if (memberName == "ValidationFunctions") {
            std::vector<RuntimeValue> functions;
            functions.reserve(spec.validators.size());
            for (size_t index = 0; index < spec.validators.size(); ++index) {
                RuntimeFunctionHandle handle;
                handle.kind = RuntimeFunctionHandleKind::Function;
                handle.backend =
                    RuntimeFunctionHandleBackend::Bytecode;
                handle.context = callableContext_;
                handle.display =
                    "@" + spec.validators[index].name;
                handle.targetName =
                    "__mparser_property_validator/" + target.text +
                    "/validator/" + std::to_string(index);
                functions.push_back(
                    makeRuntimeFunctionHandleValue(std::move(handle)));
            }
            const size_t functionCount = functions.size();
            return cellValueForShape(
                1, functionCount, std::move(functions));
        }
        addDiagnostic(
            instruction,
            "metadata property validation field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataArgumentValidatorMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        constexpr std::string_view marker = "/validator/";
        const size_t separator = target.text.rfind(marker);
        if (separator == std::string::npos) {
            addDiagnostic(
                instruction,
                "metadata argument validator descriptor is not available");
            return missingValue();
        }
        const std::string indexText =
            target.text.substr(separator + marker.size());
        char* end = nullptr;
        const unsigned long long parsed =
            std::strtoull(indexText.c_str(), &end, 10);
        const std::string argumentIdentity =
            target.text.substr(0, separator);
        std::string signatureIdentity;
        const auto argument = reflectedArgumentForMetadata(
            argumentIdentity, &signatureIdentity);
        if (!end || *end != '\0' || !argument || !argument->contract ||
            parsed >= argument->contract->spec.validators.size()) {
            addDiagnostic(
                instruction,
                "metadata argument validator descriptor is not available");
            return missingValue();
        }
        const auto& validator =
            argument->contract->spec.validators[
                static_cast<size_t>(parsed)];
        if (memberName == "Name") {
            return characterValue(validator.name);
        }
        if (memberName == "Function") {
            return missingValue();
        }
        if (memberName == "ReferencedArguments") {
            const FunctionInfo* callable =
                callableForCallSignature(signatureIdentity);
            return callable
                       ? referencedArgumentIdentifiers(
                             *callable, validator.arguments)
                       : makeRuntimeMetadataArray(
                             RuntimeMetadataKind::ArgumentIdentifier,
                             {}, {1, 0});
        }
        addDiagnostic(
            instruction,
            "metadata argument validator field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataDefaultArgumentValueMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        std::string signatureIdentity;
        const auto argument = reflectedArgumentForMetadata(
            target.text, &signatureIdentity);
        if (!argument || !argument->contract ||
            !argument->contract->spec.hasExplicitDefault) {
            addDiagnostic(
                instruction,
                "metadata default argument value is not available");
            return missingValue();
        }
        if (memberName == "Expression") {
            return characterValue(
                argument->contract->defaultExpression);
        }
        if (memberName == "ReferencedArguments") {
            const FunctionInfo* callable =
                callableForCallSignature(signatureIdentity);
            return callable
                       ? referencedArgumentIdentifiers(
                             *callable,
                             argument->contract
                                 ->defaultReferencedArguments)
                       : makeRuntimeMetadataArray(
                             RuntimeMetadataKind::ArgumentIdentifier,
                             {}, {1, 0});
        }
        addDiagnostic(
            instruction,
            "metadata default argument value field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataFixedDimensionMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        constexpr std::string_view marker = "/dimension/";
        const size_t separator = target.text.rfind(marker);
        if (separator == std::string::npos) {
            addDiagnostic(
                instruction,
                "metadata fixed dimension descriptor is not available");
            return missingValue();
        }
        const std::string indexText =
            target.text.substr(separator + marker.size());
        char* end = nullptr;
        const unsigned long long parsed =
            std::strtoull(indexText.c_str(), &end, 10);
        const std::string validationIdentity =
            target.text.substr(0, separator);
        const PropertySpec* spec = nullptr;
        if (const auto argument =
                reflectedArgumentForMetadata(validationIdentity);
            argument && argument->contract) {
            spec = &argument->contract->spec;
        } else if (const PropertyInfoPtr property =
                       propertyForMetadata(validationIdentity)) {
            spec = &property->spec;
        }
        if (!end || *end != '\0' || !spec ||
            parsed >= spec->dimensions.size() ||
            spec->dimensions[static_cast<size_t>(parsed)].text == ":") {
            addDiagnostic(
                instruction,
                "metadata fixed dimension descriptor is not available");
            return missingValue();
        }
        if (memberName == "Length") {
            const std::string& text =
                spec->dimensions[static_cast<size_t>(parsed)].text;
            char* lengthEnd = nullptr;
            const unsigned long long length =
                std::strtoull(text.c_str(), &lengthEnd, 10);
            if (lengthEnd && *lengthEnd == '\0') {
                return numberValue(static_cast<double>(length));
            }
        }
        addDiagnostic(
            instruction,
            "metadata fixed dimension field is not available: " +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataEventMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const EventInfo* event = eventForMetadata(target.text);
        if (!event) {
            addDiagnostic(instruction,
                          "metadata event descriptor is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(event->name);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "Hidden") {
            return logicalValue(event->hidden);
        }
        if (memberName == "ListenAccess") {
            return accessMetadataValue(event->listenAccess);
        }
        if (memberName == "NotifyAccess") {
            return accessMetadataValue(event->notifyAccess);
        }
        if (memberName == "DefiningClass") {
            return metadataClassValue(event->declaringClass);
        }

        addDiagnostic(instruction,
                      "metadata event field is not available: " +
                          event->declaringClass + "." + event->name +
                          "." + std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataEnumerationMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const EnumerationMemberInfo* member =
            enumerationMemberForMetadata(target.text);
        if (!member) {
            addDiagnostic(
                instruction,
                "metadata enumeration descriptor is not available");
            return missingValue();
        }
        if (memberName == "Name") {
            return characterValue(member->name);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "Hidden") {
            return logicalValue(member->hidden);
        }
        if (memberName == "DefiningClass") {
            return metadataClassValue(member->declaringClass);
        }
        addDiagnostic(
            instruction,
            "metadata enumeration field is not available: " +
                member->declaringClass + "." + member->name + "." +
                std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataNamespaceMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        const std::string& namespaceName = target.text;
        if (memberName == "Name") {
            return characterValue(namespaceName);
        }
        if (memberName == "Description" ||
            memberName == "DetailedDescription") {
            return characterValue("");
        }
        if (memberName == "ClassList") {
            std::vector<std::string> classes;
            const std::string prefix = namespaceName + ".";
            for (const auto& [className, klass] : classesByName_) {
                (void)klass;
                if (className.rfind(prefix, 0) != 0 ||
                    className.find('.', prefix.size()) !=
                        std::string::npos) {
                    continue;
                }
                classes.push_back(className);
            }
            return metadataClassArray(classes);
        }
        if (memberName == "FunctionList") {
            std::vector<const FunctionInfo*> functions;
            for (const auto& [name, function] : functionsByName_) {
                (void)name;
                if (function.namespaceName == namespaceName &&
                    function.metadataIdentifier.find('>') ==
                        std::string::npos) {
                    functions.push_back(&function);
                }
            }
            std::sort(
                functions.begin(), functions.end(),
                [](const FunctionInfo* left,
                   const FunctionInfo* right) {
                    return left->metadataIdentifier <
                           right->metadataIdentifier;
                });
            std::vector<RuntimeValue> values;
            values.reserve(functions.size());
            for (const FunctionInfo* function : functions) {
                values.push_back(makeRuntimeMetadataObject(
                    RuntimeMetadataKind::Function, function->name));
            }
            const size_t valueCount = values.size();
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::Function, std::move(values),
                {valueCount, 1});
        }
        if (memberName == "InnerNamespaces") {
            std::set<std::string> namespaces;
            const std::string prefix = namespaceName + ".";
            for (const auto& [className, klass] : classesByName_) {
                (void)klass;
                if (className.rfind(prefix, 0) != 0) {
                    continue;
                }
                const size_t nextDot =
                    className.find('.', prefix.size());
                if (nextDot != std::string::npos) {
                    namespaces.insert(
                        className.substr(0, nextDot));
                }
            }
            for (const auto& [name, function] : functionsByName_) {
                (void)name;
                if (function.namespaceName.rfind(prefix, 0) != 0) {
                    continue;
                }
                const size_t nextDot =
                    function.namespaceName.find('.', prefix.size());
                if (nextDot != std::string::npos) {
                    namespaces.insert(
                        function.namespaceName.substr(0, nextDot));
                } else {
                    namespaces.insert(function.namespaceName);
                }
            }
            std::vector<RuntimeValue> values;
            values.reserve(namespaces.size());
            for (const auto& name : namespaces) {
                values.push_back(makeRuntimeMetadataObject(
                    RuntimeMetadataKind::Namespace, name));
            }
            const size_t valueCount = values.size();
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::Namespace, std::move(values),
                {valueCount, 1});
        }
        if (memberName == "OuterNamespace" ||
            memberName == "ContainingPackage") {
            const size_t dot = namespaceName.find_last_of('.');
            if (dot == std::string::npos) {
                return makeRuntimeMetadataArray(
                    RuntimeMetadataKind::Namespace, {}, {0, 1});
            }
            return makeRuntimeMetadataObject(
                RuntimeMetadataKind::Namespace,
                namespaceName.substr(0, dot));
        }
        addDiagnostic(instruction,
                      "metadata namespace field is not available: " +
                          namespaceName + "." +
                          std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataScalarMemberValue(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        if (!isRuntimeMetadataScalar(target)) {
            addDiagnostic(
                instruction,
                "metadata member access requires a scalar metadata object");
            return missingValue();
        }
        switch (*runtimeMetadataKind(target)) {
        case RuntimeMetadataKind::Class:
            return metadataClassMember(instruction, target, memberName);
        case RuntimeMetadataKind::Property:
            return metadataPropertyMember(instruction, target, memberName);
        case RuntimeMetadataKind::DynamicProperty:
            return dynamicPropertyMember(instruction, target, memberName);
        case RuntimeMetadataKind::Method:
            return metadataMethodMember(instruction, target, memberName);
        case RuntimeMetadataKind::Event:
            return metadataEventMember(instruction, target, memberName);
        case RuntimeMetadataKind::EnumerationMember:
            return metadataEnumerationMember(instruction, target,
                                             memberName);
        case RuntimeMetadataKind::Namespace:
            return metadataNamespaceMember(instruction, target, memberName);
        case RuntimeMetadataKind::Function:
            return metadataFunctionMember(instruction, target, memberName);
        case RuntimeMetadataKind::CallSignature:
            return metadataCallSignatureMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::Argument:
            return metadataArgumentMember(instruction, target, memberName);
        case RuntimeMetadataKind::ArgumentIdentifier:
            return metadataArgumentIdentifierMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::ArgumentValidation:
            return metadataArgumentValidationMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::ArgumentValidator:
            return metadataArgumentValidatorMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::DefaultArgumentValue:
            return metadataDefaultArgumentValueMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::PropertyValidation:
            return metadataPropertyValidationMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::FixedDimension:
            return metadataFixedDimensionMember(
                instruction, target, memberName);
        case RuntimeMetadataKind::ArrayDimension:
        case RuntimeMetadataKind::UnrestrictedDimension:
        case RuntimeMetadataKind::MetaData:
            break;
        }
        addDiagnostic(instruction,
                      "metadata member is not available: " +
                          std::string(memberName));
        return missingValue();
    }

    RuntimeValue metadataMemberValue(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target,
        std::string_view memberName) {
        if (isRuntimeMetadataScalar(target)) {
            return metadataScalarMemberValue(
                instruction, target, memberName);
        }
        if (!isRuntimeMetadataArray(target)) {
            addDiagnostic(
                instruction,
                "metadata member access requires a metadata object");
            return missingValue();
        }

        const auto properties =
            runtimeMetadataPropertyNames(target.className);
        if (std::find(properties.begin(), properties.end(),
                      memberName) == properties.end()) {
            addDiagnostic(
                instruction,
                "metadata array property is not available: " +
                    canonicalRuntimeMetadataClassName(target.className) +
                    "." + std::string(memberName));
            return missingValue();
        }

        std::vector<RuntimeValue> values;
        values.reserve(target.cells.size());
        for (const auto& element : target.cells) {
            if (!isRuntimeMetadataScalar(element) ||
                !runtimeMetadataClassIsa(
                    element.className, target.className)) {
                addDiagnostic(
                    instruction,
                    "metadata array contains an incompatible element");
                return missingValue();
            }
            const size_t diagnosticCount = diagnostics_.size();
            RuntimeValue value = metadataScalarMemberValue(
                instruction, element, memberName);
            if (diagnostics_.size() != diagnosticCount) {
                return missingValue();
            }
            values.push_back(std::move(value));
        }
        return makeRuntimeCommaSeparatedList(std::move(values));
    }

    void memberAccess(const BytecodeInstruction& instruction) {
        if (instruction.operand != ".()") {
            memberAccessResolved(instruction);
            return;
        }

        const auto dynamicValue =
            popRuntime(instruction, "dynamic member name");
        if (!dynamicValue) {
            return;
        }
        const auto dynamicName = runtimeStructFieldName(*dynamicValue);
        if (!dynamicName.succeeded) {
            addDiagnostic(instruction, dynamicName.error);
            return;
        }

        BytecodeInstruction resolved = instruction;
        resolved.operand = dynamicName.name;
        memberAccessResolved(resolved);
    }

    std::optional<StackValue> resolveScalarObjectMember(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target) {
        const size_t stackDepth = stack_.size();
        const size_t diagnosticCount = diagnostics_.size();
        BytecodeInstruction scalarInstruction = instruction;
        scalarInstruction.resultCount = 1;
        stack_.push_back(runtimeStackValue(target));
        memberAccessResolved(scalarInstruction);
        if (diagnostics_.size() != diagnosticCount ||
            stack_.size() != stackDepth + 1) {
            stack_.resize(stackDepth);
            return std::nullopt;
        }
        StackValue result = std::move(stack_.back());
        stack_.resize(stackDepth);
        return result;
    }

    void memberAccessObjectArray(
        const BytecodeInstruction& instruction,
        const RuntimeValue& target) {
        const size_t count = runtimeObjectElementCount(target);
        if (count == 0) {
            const auto klass = classesByName_.find(target.className);
            if (klass != classesByName_.end()) {
                if (const auto property = selectProperty(
                        klass->second, instruction.operand)) {
                    if (!hasMemberAccess(property->getAccess,
                                         property->declaringClass)) {
                        addDiagnostic(
                            instruction,
                            "property get access is denied: " +
                                propertyDisplayName(*property));
                        return;
                    }
                    if (instruction.resultCount != 0) {
                        pushRuntime(makeRuntimeCommaSeparatedList({}));
                    }
                    return;
                }
                if (const auto* method = selectMethod(
                        klass->second, instruction.operand)) {
                    if (!hasMemberAccess(method->access,
                                         method->declaringClass)) {
                        addDiagnostic(
                            instruction,
                            "method access is denied: " +
                                method->declaringClass + "." +
                                method->name);
                        return;
                    }
                    stack_.push_back(methodStackValue(
                        target.className, instruction.operand,
                        method->declaringClass, target));
                    return;
                }
            }
            const bool handleMethod =
                target.handleObject &&
                (instruction.operand == "addlistener" ||
                 instruction.operand == "delete" ||
                 instruction.operand == "findobj" ||
                 instruction.operand == "findprop" ||
                 instruction.operand == "isvalid" ||
                 instruction.operand == "listener" ||
                 instruction.operand == "notify");
            if (handleMethod) {
                stack_.push_back(builtinStackValue(
                    instruction.operand, target));
                return;
            }
            addDiagnostic(instruction,
                          "class member is not available: " +
                              target.className + "." +
                              instruction.operand);
            return;
        }

        std::vector<RuntimeValue> values;
        values.reserve(count);
        for (size_t logicalIndex = 0; logicalIndex < count;
             ++logicalIndex) {
            const auto* element =
                runtimeObjectLogicalElement(target, logicalIndex);
            if (!element) {
                addDiagnostic(
                    instruction,
                    "object array member access could not map an element");
                return;
            }
            auto resolved = resolveScalarObjectMember(
                instruction, *element);
            if (!resolved) {
                return;
            }
            if (logicalIndex == 0 && resolved->isMethodReference) {
                resolved->receiver = target;
                stack_.push_back(std::move(*resolved));
                return;
            }
            if (logicalIndex == 0 && resolved->isBuiltinReference) {
                stack_.push_back(builtinStackValue(
                    resolved->builtinName, target));
                return;
            }
            if (resolved->isMethodReference ||
                resolved->isBuiltinReference ||
                resolved->isFunctionReference ||
                resolved->isClassReference) {
                addDiagnostic(
                    instruction,
                    "object array member kind differs between elements");
                return;
            }
            values.push_back(std::move(resolved->value));
        }

        if (instruction.resultCount == 0) {
            return;
        }
        RuntimeValue list =
            makeRuntimeCommaSeparatedList(std::move(values));
        if (instruction.resultCount == 1) {
            pushRuntime(std::move(list));
            return;
        }
        std::vector<RuntimeValue> expanded;
        appendRuntimeExpandedValues(expanded, list);
        if (expanded.size() !=
            static_cast<size_t>(instruction.resultCount)) {
            addDiagnostic(
                instruction,
                "object property produced " +
                    std::to_string(expanded.size()) +
                    " comma-separated values for " +
                    std::to_string(instruction.resultCount) +
                    " outputs");
            return;
        }
        for (auto& value : expanded) {
            pushRuntime(std::move(value));
        }
    }

    void memberAccessResolved(const BytecodeInstruction& instruction) {
        const auto target = popStackValue(instruction, "member access target");
        if (!target) {
            return;
        }
        if (target->isClassReference) {
            const auto klass = classesByName_.find(target->className);
            if (klass == classesByName_.end()) {
                addDiagnostic(instruction, "class is not available: " +
                                              target->className);
                return;
            }
            if (klass->second.declaredEnumerationMembers.contains(
                    instruction.operand)) {
                if (const auto value = enumerationMemberValue(
                        instruction, target->className,
                        instruction.operand)) {
                    stack_.push_back(runtimeStackValue(*value));
                }
                return;
            }
            if (const auto property =
                    selectProperty(klass->second, instruction.operand,
                                   false)) {
                if (!property->constant) {
                    addDiagnostic(
                        instruction,
                        "class-qualified property access requires Constant: " +
                            target->className + "." + instruction.operand);
                    return;
                }
                if (!hasMemberAccess(property->getAccess,
                                     property->declaringClass)) {
                    addDiagnostic(instruction,
                                  "property get access is denied: " +
                                      propertyDisplayName(*property));
                    return;
                }
                if (const auto value = propertyDefault(*property)) {
                    stack_.push_back(runtimeStackValue(*value));
                }
                return;
            }
            const auto* method = selectMethod(
                klass->second, instruction.operand, false);
            if (!method || !method->staticMethod) {
                addDiagnostic(instruction, "class method is not available: " +
                                              target->className + "." +
                                              instruction.operand);
                return;
            }
            if (!hasMemberAccess(method->access, method->declaringClass)) {
                addDiagnostic(instruction, "method access is denied: " +
                                              method->declaringClass + "." +
                                              method->name);
                return;
            }
            stack_.push_back(methodStackValue(target->className,
                                               instruction.operand,
                                               method->declaringClass));
            return;
        }
        if (target->value.kind == RuntimeValueKind::Struct) {
            auto field = runtimeStructFieldValues(
                target->value, instruction.operand);
            if (!field.succeeded) {
                addDiagnostic(instruction, field.error);
                return;
            }
            if (instruction.resultCount == 0) {
                return;
            }
            if (instruction.resultCount == 1) {
                pushRuntime(std::move(field.value));
                return;
            }
            std::vector<RuntimeValue> values;
            appendRuntimeExpandedValues(values, field.value);
            if (values.size() !=
                static_cast<size_t>(instruction.resultCount)) {
                addDiagnostic(
                    instruction,
                    "structure field produced " +
                        std::to_string(values.size()) +
                        " comma-separated values for " +
                        std::to_string(instruction.resultCount) +
                        " outputs");
                return;
            }
            for (auto& value : values) {
                pushRuntime(std::move(value));
            }
            return;
        }
        if (isRuntimeException(target->value)) {
            if (isRuntimeExceptionMethodName(instruction.operand)) {
                stack_.push_back(builtinStackValue(
                    instruction.operand, target->value));
                return;
            }
            const RuntimeValue* property = runtimeExceptionProperty(
                target->value, instruction.operand);
            if (!property) {
                addDiagnostic(instruction,
                              "MException property is not available: " +
                                  instruction.operand);
                return;
            }
            stack_.push_back(runtimeStackValue(*property));
            return;
        }
        if (isRuntimeTemporalValue(target->value)) {
            auto property = runtimeTemporalMemberValue(
                target->value, instruction.operand);
            if (!property.succeeded) {
                addDiagnostic(instruction, std::move(property.error));
                return;
            }
            if (instruction.resultCount == 0) {
                return;
            }
            if (instruction.resultCount == 1) {
                pushRuntime(std::move(property.value));
                return;
            }
            std::vector<RuntimeValue> values;
            appendRuntimeExpandedValues(values, property.value);
            if (values.size() !=
                static_cast<size_t>(instruction.resultCount)) {
                addDiagnostic(
                    instruction,
                    "temporal property produced " +
                        std::to_string(values.size()) +
                        " values for " +
                        std::to_string(instruction.resultCount) +
                        " outputs");
                return;
            }
            for (auto& value : values) {
                pushRuntime(std::move(value));
            }
            return;
        }
        if (!isObject(target->value)) {
            addDiagnostic(instruction,
                          "member access requires a class object target");
            return;
        }
        if (isRuntimeMetadataObject(target->value)) {
            const auto metadataKind =
                runtimeMetadataKind(target->value);
            const bool dynamicPropertyMethod =
                metadataKind ==
                    RuntimeMetadataKind::DynamicProperty &&
                (instruction.operand == "delete" ||
                 instruction.operand == "isvalid");
            const bool propertyValidationMethod =
                metadataKind ==
                    RuntimeMetadataKind::PropertyValidation &&
                (instruction.operand == "isValidValue" ||
                 instruction.operand == "validateValue");
            if (dynamicPropertyMethod ||
                propertyValidationMethod) {
                if (!isRuntimeMetadataScalar(target->value)) {
                    addDiagnostic(
                        instruction,
                        "metadata method access requires a scalar "
                        "metadata object");
                    return;
                }
                stack_.push_back(builtinStackValue(
                    instruction.operand, target->value));
                return;
            }
            RuntimeValue value = metadataMemberValue(
                instruction, target->value, instruction.operand);
            if (instruction.resultCount == 0) {
                return;
            }
            if (instruction.resultCount == 1) {
                pushRuntime(std::move(value));
                return;
            }
            std::vector<RuntimeValue> values;
            appendRuntimeExpandedValues(values, value);
            if (values.size() !=
                static_cast<size_t>(instruction.resultCount)) {
                addDiagnostic(
                    instruction,
                    "metadata property produced " +
                        std::to_string(values.size()) +
                        " comma-separated values for " +
                        std::to_string(instruction.resultCount) +
                        " outputs");
                return;
            }
            for (auto& item : values) {
                pushRuntime(std::move(item));
            }
            return;
        }
        if (isRuntimeClassObject(target->value) &&
            !isRuntimeScalarObject(target->value)) {
            memberAccessObjectArray(instruction, target->value);
            return;
        }
        const bool builtInListener =
            target->value.className == kEventListenerClassName ||
            target->value.className == kPropertyListenerClassName;
        if (builtInListener &&
            (instruction.operand == "delete" ||
             instruction.operand == "isvalid")) {
            stack_.push_back(builtinStackValue(
                instruction.operand, target->value));
            return;
        }
        const bool builtInEventData =
            target->value.className == kEventDataClassName ||
            target->value.className == kPropertyEventClassName;
        const bool inheritedEventDataProperty =
            (instruction.operand == "Source" ||
             instruction.operand == "EventName") &&
            classDerivesFrom(target->value.className,
                             std::string(kEventDataClassName));
        if (builtInEventData || inheritedEventDataProperty ||
            builtInListener) {
            const auto& fields = objectFields(target->value);
            const auto field = fields.find(instruction.operand);
            if (field == fields.end() ||
                instruction.operand.rfind("__mparser_", 0) == 0) {
                addDiagnostic(instruction,
                              "built-in event object property is not "
                              "available: " +
                                  target->value.className + "." +
                                  instruction.operand);
                return;
            }
            stack_.push_back(runtimeStackValue(field->second));
            return;
        }
        const auto klass = classesByName_.find(target->value.className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction, "class is not available: " +
                                          target->value.className);
            return;
        }
        if (target->value.handleObject &&
            instruction.operand != "delete" &&
            instruction.operand != "isvalid" &&
            !requireUsableHandleObject(instruction, target->value)) {
            return;
        }
        if (const auto descriptor = dynamicPropertyDescriptor(
                target->value, instruction.operand)) {
            const bool observable =
                dynamicPropertyLogicalField(*descriptor,
                                            "GetObservable");
            if (observable &&
                !dispatchPropertyEvent(
                    instruction, target->value, *descriptor, {},
                    descriptor->opaqueId, "PreGet")) {
                return;
            }
            if (const auto value = readDynamicProperty(
                    instruction, target->value, *descriptor)) {
                if (observable &&
                    !dispatchPropertyEvent(
                        instruction, target->value, *descriptor, {},
                        descriptor->opaqueId, "PostGet")) {
                    return;
                }
                stack_.push_back(runtimeStackValue(*value));
            }
            return;
        }
        if (const auto property =
                selectProperty(klass->second, instruction.operand)) {
            if (!hasMemberAccess(property->getAccess,
                                 property->declaringClass)) {
                addDiagnostic(instruction,
                              "property get access is denied: " +
                                  propertyDisplayName(*property));
                return;
            }
            const RuntimeValue descriptor =
                propertyDescriptorForSource(target->value, *property);
            if (property->getObservable && target->value.handleObject &&
                !dispatchPropertyEvent(
                    instruction, target->value, descriptor,
                    property->storageKey, 0, "PreGet")) {
                return;
            }
            if (const auto value = invokePropertyGetter(
                    instruction, target->value, *property)) {
                if (property->getObservable &&
                    target->value.handleObject &&
                    !dispatchPropertyEvent(
                        instruction, target->value, descriptor,
                        property->storageKey, 0, "PostGet")) {
                    return;
                }
                stack_.push_back(runtimeStackValue(*value));
            }
            return;
        }
        if (const auto* method =
                selectMethod(klass->second, instruction.operand)) {
            if (!hasMemberAccess(method->access, method->declaringClass)) {
                addDiagnostic(instruction, "method access is denied: " +
                                              method->declaringClass + "." +
                                              method->name);
                return;
            }
            stack_.push_back(methodStackValue(target->value.className,
                                               instruction.operand,
                                               method->declaringClass,
                                               target->value));
            return;
        }
        const bool dynamicPropsMethod =
            instruction.operand == "addprop" &&
            classDerivesFrom(target->value.className,
                             std::string(kDynamicPropsClassName));
        const bool handleMethod =
            target->value.handleObject &&
            (instruction.operand == "addlistener" ||
             instruction.operand == "delete" ||
             instruction.operand == "findobj" ||
             instruction.operand == "findprop" ||
             instruction.operand == "isvalid");
        const bool eventMethod =
            target->value.handleObject &&
            (instruction.operand == "listener" ||
             instruction.operand == "notify");
        if (dynamicPropsMethod || handleMethod || eventMethod) {
            stack_.push_back(builtinStackValue(
                instruction.operand, target->value));
            return;
        }
        addDiagnostic(instruction, "class member is not available: " +
                                      target->value.className + "." +
                                      instruction.operand);
    }

    void storeMember(const BytecodeInstruction& instruction) {
        if (instruction.operand != ".()") {
            storeMemberResolved(instruction);
            return;
        }

        const auto dynamicValue =
            popRuntime(instruction, "dynamic member assignment name");
        if (!dynamicValue) {
            return;
        }
        const auto dynamicName = runtimeStructFieldName(*dynamicValue);
        if (!dynamicName.succeeded) {
            addDiagnostic(instruction, dynamicName.error);
            return;
        }

        BytecodeInstruction resolved = instruction;
        resolved.operand = dynamicName.name;
        storeMemberResolved(resolved);
    }

    void storeMemberResolved(const BytecodeInstruction& instruction) {
        std::optional<StackValue> target;
        if (!instruction.receiverName.empty()) {
            BytecodeInstruction receiver = instruction;
            receiver.operand = instruction.receiverName;
            receiver.binding = instruction.receiverBinding;
            const auto variable = loadStoredVariable(receiver);
            target = variable
                         ? runtimeStackValue(*variable)
                         : runtimeStackValue(makeRuntimeStructValue());
        } else {
            target = popStackValue(instruction,
                                   "member assignment target");
        }
        auto value = popRuntime(instruction, "member assignment value");
        if (!target || !value) {
            return;
        }
        const auto single = runtimeRequireSingleValue(
            *value, "member assignment right-hand side");
        if (!single.succeeded) {
            addDiagnostic(instruction, single.error);
            return;
        }
        value = single.value;
        if (target->isClassReference) {
            const auto klass = classesByName_.find(target->className);
            if (klass != classesByName_.end()) {
                const auto property =
                    selectProperty(klass->second, instruction.operand,
                                   false);
                if (property && property->constant) {
                    addDiagnostic(instruction,
                                  "constant property cannot be assigned: " +
                                      propertyDisplayName(*property));
                    return;
                }
            }
            addDiagnostic(
                instruction,
                "class-qualified property assignment requires Constant, "
                "which is read-only: " + target->className + "." +
                    instruction.operand);
            return;
        }
        if (target->value.kind == RuntimeValueKind::Struct) {
            if (instruction.receiverName.empty()) {
                addDiagnostic(
                    instruction,
                    "structure member assignment currently requires a "
                    "variable target");
                return;
            }
            RuntimeValue updated = target->value;
            if (!runtimeSetStructField(updated, instruction.operand,
                                       *value)) {
                addDiagnostic(
                    instruction,
                    "direct field assignment requires a scalar structure");
                return;
            }
            storeVariable(instruction.receiverName,
                          instruction.receiverBinding,
                          updated, instruction);
            recordAssignment(instruction, "struct-member", updated);
            return;
        }
        if (isRuntimeException(target->value)) {
            addDiagnostic(instruction,
                          "MException properties are read-only: " +
                              instruction.operand);
            return;
        }
        if (!isObject(target->value)) {
            addDiagnostic(instruction,
                          "member assignment requires a class object target");
            return;
        }

        RuntimeValue updated = target->value;
        if (isRuntimeMetadataObject(updated)) {
            if (runtimeMetadataKind(updated) ==
                RuntimeMetadataKind::DynamicProperty) {
                storeDynamicPropertyMetadata(instruction, updated, *value);
                if (!instruction.receiverName.empty()) {
                    storeVariable(instruction.receiverName,
                                  instruction.receiverBinding, updated,
                                  instruction);
                }
                return;
            }
            addDiagnostic(instruction,
                          "metadata properties are read-only: " +
                              updated.className + "." +
                              instruction.operand);
            return;
        }
        if (isRuntimeClassObject(updated) &&
            !isRuntimeScalarObject(updated)) {
            addDiagnostic(
                instruction,
                "property assignment requires a scalar object target; "
                "index object array elements explicitly");
            return;
        }
        if (updated.className == kEventDataClassName ||
            updated.className == kPropertyEventClassName) {
            addDiagnostic(instruction,
                          "built-in event data property is read-only: " +
                              updated.className + "." +
                              instruction.operand);
            return;
        }
        if (updated.className == kEventListenerClassName ||
            updated.className == kPropertyListenerClassName) {
            if (!updated.sharedFields) {
                addDiagnostic(instruction,
                              "event listener has no runtime state");
                return;
            }
            if (instruction.operand == "Enabled" ||
                instruction.operand == "Recursive") {
                if (!isNumber(*value)) {
                    addDiagnostic(instruction,
                                  "event listener logical property requires "
                                  "a scalar numeric value: " +
                                      instruction.operand);
                    return;
                }
            } else if (instruction.operand == "Callback") {
                if (!isFunctionHandle(*value)) {
                    addDiagnostic(instruction,
                                  "event listener Callback requires a "
                                  "function handle");
                    return;
                }
            } else {
                addDiagnostic(instruction,
                              "event listener property is read-only: " +
                                  instruction.operand);
                return;
            }
            (*updated.sharedFields)[instruction.operand] = *value;
            if (!instruction.receiverName.empty()) {
                storeVariable(instruction.receiverName,
                              instruction.receiverBinding, updated,
                              instruction);
            }
            recordAssignment(instruction, "listener-member", updated);
            return;
        }
        const auto klass = classesByName_.find(updated.className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction,
                          "class is not available for member assignment: " +
                              updated.className);
            return;
        }
        if (updated.handleObject &&
            !requireUsableHandleObject(instruction, updated)) {
            return;
        }
        if (const auto descriptor = dynamicPropertyDescriptor(
                updated, instruction.operand)) {
            if (!writeDynamicProperty(instruction, updated, *descriptor,
                                      *value)) {
                return;
            }
            if (!instruction.receiverName.empty()) {
                storeVariable(instruction.receiverName,
                              instruction.receiverBinding, updated,
                              instruction);
            }
            recordAssignment(instruction, "dynamic-member", updated);
            return;
        }
        const auto property =
            selectProperty(klass->second, instruction.operand);
        if (!property) {
            addDiagnostic(instruction, "property is not available: " +
                                           updated.className + "." +
                                           instruction.operand);
            return;
        }

        PropertyInfo& info = *property;
        if (info.constant) {
            addDiagnostic(instruction,
                          "constant property cannot be assigned: " +
                              propertyDisplayName(info));
            return;
        }
        if (!updated.enumerationMemberName.empty() &&
            !updated.handleObject) {
            addDiagnostic(instruction,
                          "value enumeration properties are immutable: " +
                              updated.className + "." + instruction.operand);
            return;
        }
        if (!hasMemberAccess(info.setAccess, info.declaringClass)) {
            addDiagnostic(instruction,
                          "property set access is denied: " +
                              propertyDisplayName(info));
            return;
        }
        if (instruction.receiverName.empty()) {
            addDiagnostic(instruction,
                          "member assignment requires a direct variable target");
            return;
        }

        if (activePropertyWriter(info)) {
            if (info.dependent) {
                addDiagnostic(instruction,
                              "dependent property cannot store a value: " +
                                  propertyDisplayName(info));
                return;
            }
            writeStoredProperty(updated, info, *value);
            storeVariable(instruction.receiverName,
                          instruction.receiverBinding,
                          updated, instruction);
            recordAssignment(instruction, "member", updated);
            return;
        }

        const auto validated = validatePropertyValue(
            instruction, info, *value);
        if (!validated) {
            return;
        }

        if (info.abortSet) {
            const auto current = invokePropertyGetter(instruction, updated, info);
            if (!current) {
                return;
            }
            if (runtimeEqual(*current, *validated)) {
                storeVariable(instruction.receiverName,
                              instruction.receiverBinding, updated,
                              instruction);
                return;
            }
        }

        const RuntimeValue propertyDescriptor =
            propertyDescriptorForSource(updated, info);
        if (info.setObservable && updated.handleObject &&
            !dispatchPropertyEvent(
                instruction, updated, propertyDescriptor,
                info.storageKey, 0, "PreSet")) {
            return;
        }

        if (!info.setterName.empty()) {
            const auto owner = classesByName_.find(info.declaringClass);
            if (owner == classesByName_.end() ||
                !owner->second.declaredMethods.contains(info.setterName)) {
                addDiagnostic(instruction,
                              "property set method is not available: " +
                                  propertyDisplayName(info));
                return;
            }
            const auto& setter =
                owner->second.declaredMethods.at(info.setterName);
            const int requestedCount =
                setter.signature.outputs.empty() ? 0 : 1;
            const size_t diagnosticCount = diagnostics_.size();
            auto outputs = callFunctionInfo(
                instruction, info.declaringClass + "." + info.setterName,
                setter, {updated, *validated}, requestedCount, std::nullopt,
                nullptr, false);
            if (diagnostics_.size() != diagnosticCount) {
                return;
            }
            if (requestedCount == 1) {
                if (outputs.empty() || !isObject(outputs.front()) ||
                    !classDerivesFrom(outputs.front().className,
                                      info.declaringClass)) {
                    addDiagnostic(instruction,
                                  "property set method did not return a "
                                  "compatible object: " +
                                      propertyDisplayName(info));
                    return;
                }
                updated = outputs.front();
            }
        } else {
            if (info.dependent) {
                addDiagnostic(instruction,
                              "dependent property has no set method: " +
                                  propertyDisplayName(info));
                return;
            }
            writeStoredProperty(updated, info, *validated);
        }
        storeVariable(instruction.receiverName,
                      instruction.receiverBinding,
                      updated, instruction);
        recordAssignment(instruction, "member", updated);
        if (info.setObservable && updated.handleObject) {
            (void)dispatchPropertyEvent(
                instruction, updated, propertyDescriptor,
                info.storageKey, 0, "PostSet");
        }
    }

    void finishPathStore(const BytecodeInstruction& instruction,
                         RuntimeLvalueSegment segment,
                         const std::optional<RuntimeValue>& value) {
        if (lvalueStack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode path store has no active lvalue");
            return;
        }
        auto active = std::move(lvalueStack_.back());
        lvalueStack_.pop_back();
        if (!value || active->failed) {
            return;
        }

        auto result = active->transaction.assign(
            std::move(segment), *value, instruction.nullAssignment,
            lvalueHooks(instruction));
        if (!result.succeeded) {
            if (!result.error.empty()) {
                addDiagnostic(instruction, std::move(result.error));
            }
            return;
        }
        storeVariable(active->rootName, active->binding,
                      active->transaction.root(), instruction);
        BytecodeInstruction profileInstruction = instruction;
        profileInstruction.operand = active->rootName;
        recordAssignment(profileInstruction, "path",
                         active->transaction.root());
    }

    void storePathMember(const BytecodeInstruction& instruction) {
        const auto name = lvalueMemberName(instruction);
        const auto value =
            popRuntime(instruction, "path member assignment value");
        if (!name) {
            if (!lvalueStack_.empty()) {
                lvalueStack_.pop_back();
            }
            return;
        }
        RuntimeLvalueSegment segment;
        segment.kind = RuntimeLvalueSegmentKind::Member;
        segment.memberName = *name;
        finishPathStore(instruction, std::move(segment), value);
    }

    void storePathIndexed(const BytecodeInstruction& instruction,
                          RuntimeLvalueSegmentKind kind) {
        const auto subscripts = lvalueSubscripts(instruction);
        const auto value =
            popRuntime(instruction, "path indexed assignment value");
        RuntimeLvalueSegment segment;
        segment.kind = kind;
        if (subscripts) {
            segment.subscripts = *subscripts;
        }
        segment.colonSubscripts = instruction.colonSubscripts;
        finishPathStore(instruction, std::move(segment),
                        subscripts ? value : std::nullopt);
    }

    void storePathIndex(const BytecodeInstruction& instruction) {
        storePathIndexed(instruction,
                         RuntimeLvalueSegmentKind::Parenthesis);
    }

    void storePathBrace(const BytecodeInstruction& instruction) {
        storePathIndexed(instruction,
                         RuntimeLvalueSegmentKind::Brace);
    }

    void storeIndex(const BytecodeInstruction& instruction) {
        const auto rawArguments = popRuntimeValues(
            instruction, instruction.operandCount,
            "indexed assignment arguments");
        const auto target = popRuntime(instruction,
                                       "indexed assignment target");
        const auto value = popRuntime(instruction,
                                      "indexed assignment value");
        finishIndexContext();
        if (!rawArguments || !target || !value) {
            return;
        }
        const auto single = runtimeRequireSingleValue(
            *value, "indexed assignment right-hand side");
        if (!single.succeeded) {
            addDiagnostic(instruction, single.error);
            return;
        }
        const auto arguments = runtimeExpandedValues(*rawArguments);
        if (instruction.operand.empty()) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment requires a variable "
                          "target");
            return;
        }
        if (arguments.empty()) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment requires subscripts");
            return;
        }

        if (target->kind == RuntimeValueKind::Struct) {
            auto result = instruction.nullAssignment
                              ? runtimeDeleteStructIndexed(*target, arguments)
                              : runtimeAssignStructIndexed(
                                    *target, arguments, single.value);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return;
            }
            recordAssignment(instruction, "index", result.value);
            storeVariable(instruction, std::move(result.value));
            return;
        }
        if (target->kind == RuntimeValueKind::Cell) {
            auto result = instruction.nullAssignment
                              ? runtimeDeleteCellIndexed(
                                    *target, arguments,
                                    instruction.colonSubscripts)
                              : runtimeAssignCellIndexed(
                                    *target, arguments, single.value);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return;
            }
            recordAssignment(instruction, "index", result.value);
            storeVariable(instruction, std::move(result.value));
            return;
        }
        if (isRuntimeTextValue(*target)) {
            RuntimeValue updated = *target;
            const auto result = instruction.nullAssignment
                                    ? runtimeDeleteTextIndexed(
                                          updated, arguments,
                                          instruction.colonSubscripts)
                                    : runtimeAssignTextIndexed(
                                          updated, arguments,
                                          single.value);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return;
            }
            recordAssignment(instruction, "index", updated);
            storeVariable(instruction, std::move(updated));
            return;
        }
        if (target->kind == RuntimeValueKind::MissingArray) {
            RuntimeValue updated = *target;
            const auto result =
                instruction.nullAssignment
                    ? runtimeDeleteMissingIndexed(
                          updated, arguments,
                          instruction.colonSubscripts)
                    : runtimeAssignMissingIndexed(
                          updated, arguments, single.value);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return;
            }
            recordAssignment(instruction, "index", updated);
            storeVariable(instruction, std::move(updated));
            return;
        }
        if (isRuntimeClassObject(*target)) {
            auto result = instruction.nullAssignment
                              ? runtimeDeleteObjectIndexed(
                                    *target, arguments,
                                    instruction.colonSubscripts,
                                    objectArrayPolicy(instruction))
                              : runtimeAssignObjectIndexed(
                                    *target, arguments, single.value,
                                    objectArrayPolicy(instruction));
            if (!result.succeeded) {
                if (!result.error.empty()) {
                    addDiagnostic(instruction,
                                  "bytecode " + result.error);
                }
                return;
            }
            recordAssignment(instruction, "index", result.value);
            storeVariable(instruction, std::move(result.value));
            return;
        }
        if (!isNumeric(*target) ||
            (!isNumeric(single.value) &&
             single.value.kind != RuntimeValueKind::MissingArray)) {
            addDiagnostic(
                instruction,
                "bytecode indexed assignment requires compatible numeric, "
                "missing, Cell, structure, text, or object values");
            return;
        }

        RuntimeValue updated = *target;
        const auto result =
            instruction.nullAssignment
                ? runtimeDeleteNumericIndexed(
                      updated, arguments, instruction.colonSubscripts)
                : runtimeAssignNumericIndexed(updated, arguments,
                                              single.value);
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return;
        }
        recordAssignment(instruction, "index", updated);
        storeVariable(instruction, std::move(updated));
    }

    void applyUnary(const BytecodeInstruction& instruction) {
        const auto value = popRuntime(instruction, "unary operator");
        if (!value) {
            return;
        }
        if (isRuntimeTemporalValue(*value)) {
            auto temporal = runtimeApplyTemporalUnary(
                instruction.operand, *value);
            if (!temporal.succeeded) {
                addDiagnostic(instruction, std::move(temporal.error));
                return;
            }
            pushRuntime(std::move(temporal.value));
            return;
        }
        if (!isNumeric(*value)) {
            addDiagnostic(instruction,
                          "bytecode unary operator requires numeric input");
            return;
        }
        auto result = runtimeApplyNumericUnary(instruction.operand, *value);
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return;
        }
        pushRuntime(std::move(result.value));
    }

    void applyBinary(const BytecodeInstruction& instruction) {
        if (instruction.operand == ":") {
            const auto operands = popRuntimeValues(
                instruction, instruction.operandCount, "colon operator");
            if (operands) {
                applyColon(instruction, *operands);
            }
            return;
        }

        const auto right = popRuntime(instruction, "binary operator");
        const auto left = popRuntime(instruction, "binary operator");
        if (!left || !right) {
            return;
        }

        if (isRuntimeTemporalValue(*left) ||
            isRuntimeTemporalValue(*right)) {
            auto temporal = runtimeApplyTemporalBinary(
                instruction.operand, *left, *right);
            if (!temporal.succeeded) {
                addDiagnostic(instruction, std::move(temporal.error));
                return;
            }
            pushRuntime(std::move(temporal.value));
            return;
        }

        if (isRuntimeTextValue(*left) || isRuntimeTextValue(*right)) {
            if (isRuntimeTextValue(*left) && isRuntimeTextValue(*right)) {
                RuntimeTextOperationResult result;
                if (instruction.operand == "+" &&
                    (isRuntimeStringArray(*left) ||
                     isRuntimeStringArray(*right))) {
                    result = runtimeAppendText(*left, *right);
                } else {
                    result = runtimeCompareText(
                        instruction.operand, *left, *right);
                }
                if (result.succeeded) {
                    pushRuntime(std::move(result.value));
                    return;
                }
                addDiagnostic(instruction, "bytecode " + result.error);
                return;
            }
            addDiagnostic(instruction,
                          "bytecode text operators require compatible text values");
            return;
        }

        // Sparse values use the object kind to keep their CSC storage opaque,
        // but their operators follow the numeric path and may densify.
        if (isRuntimeSparseValue(*left) || isRuntimeSparseValue(*right)) {
            if (!isNumeric(*left) || !isNumeric(*right)) {
                addDiagnostic(instruction,
                              "bytecode sparse operators require numeric values");
                return;
            }
            pushRuntime(applyNumericBinary(instruction, *left, *right));
            return;
        }

        if (isObject(*left) || isObject(*right)) {
            const bool classMetadataOperands =
                isRuntimeMetadataScalar(*left) &&
                isRuntimeMetadataScalar(*right) &&
                runtimeMetadataKind(*left) ==
                    RuntimeMetadataKind::Class &&
                runtimeMetadataKind(*right) ==
                    RuntimeMetadataKind::Class;
            if (classMetadataOperands &&
                (instruction.operand == "<" ||
                 instruction.operand == "<=" ||
                 instruction.operand == ">" ||
                 instruction.operand == ">=")) {
                const bool equal =
                    canonicalRuntimeMetadataClassName(left->text) ==
                    canonicalRuntimeMetadataClassName(right->text);
                const bool leftSubclass =
                    reflectableClassDerivesFrom(left->text, right->text);
                const bool rightSubclass =
                    reflectableClassDerivesFrom(right->text, left->text);
                bool result = false;
                if (instruction.operand == "<") {
                    result = leftSubclass && !equal;
                } else if (instruction.operand == "<=") {
                    result = leftSubclass;
                } else if (instruction.operand == ">") {
                    result = rightSubclass && !equal;
                } else {
                    result = rightSubclass;
                }
                pushRuntime(logicalValue(result));
                return;
            }
            if (isObject(*left) && isObject(*right) &&
                (instruction.operand == "==" ||
                 instruction.operand == "~=")) {
                if (isRuntimeClassObject(*left) &&
                    isRuntimeClassObject(*right)) {
                    auto compared = runtimeCompareObjectArrays(
                        *left, *right, instruction.operand == "~=",
                        [](const RuntimeValue& leftElement,
                           const RuntimeValue& rightElement) {
                            return runtimeEqual(leftElement, rightElement);
                        });
                    if (!compared.succeeded) {
                        addDiagnostic(instruction,
                                      "bytecode " + compared.error);
                        return;
                    }
                    pushRuntime(std::move(compared.value));
                    return;
                }
                const bool equal = runtimeEqual(*left, *right);
                pushRuntime(
                    logicalValue((instruction.operand == "==") == equal));
                return;
            }
            addDiagnostic(
                instruction,
                "bytecode object operators support equality; metadata class "
                "objects also support <, <=, >, and >=");
            return;
        }

        if (!isNumeric(*left) || !isNumeric(*right)) {
            addDiagnostic(instruction,
                          "bytecode binary operator requires numeric values");
            return;
        }

        pushRuntime(applyNumericBinary(instruction, *left, *right));
    }

    void applyPostfix(const BytecodeInstruction& instruction) {
        const auto value = popRuntime(instruction, "postfix operator");
        if (!value) {
            return;
        }

        if (instruction.operand != "'" &&
            instruction.operand != ".'") {
            addDiagnostic(instruction,
                          "unsupported bytecode postfix operator: " +
                              instruction.operand);
            return;
        }

        if (isNumeric(*value)) {
            auto result = runtimeTransposeNumeric(
                *value, instruction.operand == "'");
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return;
            }
            pushRuntime(std::move(result.value));
            return;
        }

        if (value->kind == RuntimeValueKind::MissingArray ||
            isRuntimeTextValue(*value) || isCell(*value) ||
            isRuntimeClassObject(*value)) {
            if (runtimeDimensionCount(*value) > 2) {
                addDiagnostic(instruction,
                              "bytecode transpose requires a two-dimensional array");
                return;
            }
            auto result = runtimeArrayOperationBuiltin(
                "permute", {*value, vectorValue({2.0, 1.0})},
                objectArrayPolicy(instruction));
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return;
            }
            pushRuntime(std::move(result.value));
            return;
        }

        addDiagnostic(
            instruction,
            "bytecode transpose requires missing, numeric, text, cell, or "
            "object input");
    }

    void makeMatrixRow(const BytecodeInstruction& instruction) {
        const auto rawValues = popRuntimeValues(
            instruction, instruction.operandCount, "matrix row");
        if (!rawValues) {
            return;
        }
        const auto values = runtimeExpandedValues(*rawValues);
        auto result = runtimeArrayOperationBuiltin(
            "horzcat", values, objectArrayPolicy(instruction));
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return;
        }
        pushRuntime(std::move(result.value));
    }

    void makeMatrix(const BytecodeInstruction& instruction) {
        const auto rawRows = popRuntimeValues(
            instruction, instruction.operandCount, "matrix");
        if (!rawRows) {
            return;
        }
        const auto rows = runtimeExpandedValues(*rawRows);
        if (rows.empty()) {
            pushRuntime(matrixValue(0, 0, {}));
            return;
        }

        auto result = runtimeArrayOperationBuiltin(
            "vertcat", rows, objectArrayPolicy(instruction));
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return;
        }
        pushRuntime(std::move(result.value));
    }

    void makeCell(const BytecodeInstruction& instruction) {
        const auto rawRows = popRuntimeValues(
            instruction, instruction.operandCount, "cell literal");
        if (!rawRows) {
            return;
        }
        const auto rows = runtimeExpandedValues(*rawRows);
        if (rows.empty()) {
            pushRuntime(cellValueForDimensions({0, 0}, {}));
            return;
        }
        auto result = runtimeArrayOperationBuiltin(
            "vertcat", rows, objectArrayPolicy(instruction));
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return;
        }
        pushRuntime(std::move(result.value));
    }

    void makeCellRow(const BytecodeInstruction& instruction) {
        const auto rawValues = popRuntimeValues(
            instruction, instruction.operandCount, "cell literal row");
        if (!rawValues) {
            return;
        }
        pushRuntime(cellValue(runtimeExpandedValues(*rawValues)));
    }

    void braceIndex(const BytecodeInstruction& instruction) {
        const auto arguments = popRuntimeValues(instruction,
                                                 instruction.operandCount,
                                                 "cell index arguments");
        const auto target = popRuntime(instruction, "cell index target");
        finishIndexContext();
        if (!arguments || !target) {
            return;
        }
        if (isRuntimeStringArray(*target)) {
            auto result = runtimeIndexStringContents(
                *target, runtimeExpandedValues(*arguments));
            if (!result.succeeded) {
                addDiagnostic(instruction, std::move(result.error));
                return;
            }
            if (instruction.resultCount == 1) {
                pushRuntime(std::move(result.value));
            } else if (instruction.resultCount > 1) {
                std::vector<RuntimeValue> values;
                appendRuntimeExpandedValues(values, result.value);
                if (values.size() !=
                    static_cast<size_t>(instruction.resultCount)) {
                    addDiagnostic(
                        instruction,
                        "string contents output count mismatch: requested " +
                            std::to_string(instruction.resultCount) +
                            ", produced " + std::to_string(values.size()));
                    return;
                }
                for (auto& value : values) {
                    pushRuntime(std::move(value));
                }
            }
            return;
        }
        auto result = runtimeIndexCellContents(
            *target, runtimeExpandedValues(*arguments));
        if (!result.succeeded) {
            addDiagnostic(instruction, std::move(result.error));
            return;
        }
        if (instruction.resultCount == 1) {
            pushRuntime(std::move(result.value));
        } else if (instruction.resultCount > 1) {
            std::vector<RuntimeValue> values;
            appendRuntimeExpandedValues(values, result.value);
            if (values.size() !=
                static_cast<size_t>(instruction.resultCount)) {
                addDiagnostic(
                    instruction,
                    "cell contents output count mismatch: requested " +
                        std::to_string(instruction.resultCount) +
                        ", produced " + std::to_string(values.size()));
                return;
            }
            for (auto& value : values) {
                pushRuntime(std::move(value));
            }
        }
    }

    void storeBraceIndex(const BytecodeInstruction& instruction) {
        const auto arguments = popRuntimeValues(instruction,
                                                 instruction.operandCount,
                                                 "cell assignment arguments");
        const auto target = popRuntime(instruction, "cell assignment target");
        const auto value = popRuntime(instruction, "cell assignment value");
        finishIndexContext();
        if (!arguments || !target || !value) {
            return;
        }
        const auto single = runtimeRequireSingleValue(
            *value, "cell assignment right-hand side");
        if (!single.succeeded) {
            addDiagnostic(instruction, single.error);
            return;
        }
        if (instruction.operand.empty()) {
            addDiagnostic(instruction,
                          "cell assignment requires a variable target");
            return;
        }
        if (isRuntimeStringArray(*target)) {
            RuntimeValue updated = *target;
            const auto result = runtimeAssignStringContents(
                updated, runtimeExpandedValues(*arguments), single.value);
            if (!result.succeeded) {
                addDiagnostic(instruction, result.error);
                return;
            }
            storeVariable(instruction, updated);
            recordAssignment(instruction, "string-content", updated);
            return;
        }
        auto result = runtimeAssignCellContents(
            *target, runtimeExpandedValues(*arguments), single.value);
        if (!result.succeeded) {
            addDiagnostic(instruction, std::move(result.error));
            return;
        }
        storeVariable(instruction, result.value);
        recordAssignment(instruction, "cell", result.value);
    }

    int preferredImplicitOutputCount(
        const BuiltinDescriptor* descriptor,
        size_t suppliedInputCount = 0) const {
        return descriptor
                   ? static_cast<int>(descriptor->implicitOutputCount(
                         suppliedInputCount))
                   : 1;
    }

    int preferredImplicitBuiltinOutputCount(
        std::string_view name,
        size_t suppliedInputCount = 0) const {
        if (name == "validateValue") {
            return 0;
        }
        return preferredImplicitOutputCount(
            builtinRegistry().find(name), suppliedInputCount);
    }

    int preferredImplicitOutputCount(
        const FunctionSignature& signature) const {
        if (functionOutputCountIsValid(signature, 1)) {
            return 1;
        }
        return functionOutputCountIsValid(signature, 0) ? 0 : 1;
    }

    const FunctionInfo* implicitMethodInfo(
        const std::string& className, const std::string& methodName,
        const std::string& methodDeclaringClass) const {
        if (!methodDeclaringClass.empty()) {
            const auto owner = classesByName_.find(methodDeclaringClass);
            if (owner == classesByName_.end()) {
                return nullptr;
            }
            const auto declared =
                owner->second.declaredMethods.find(methodName);
            return declared == owner->second.declaredMethods.end()
                       ? nullptr
                       : &declared->second;
        }
        const auto klass = classesByName_.find(className);
        return klass == classesByName_.end()
                   ? nullptr
                   : selectMethod(klass->second, methodName);
    }

    int preferredImplicitMethodOutputCount(
        const std::string& className, const std::string& methodName,
        const std::string& methodDeclaringClass) const {
        const FunctionInfo* method = implicitMethodInfo(
            className, methodName, methodDeclaringClass);
        if (!method) {
            return 1;
        }
        return method->classDestructor
                   ? 0
                   : preferredImplicitOutputCount(method->signature);
    }

    int preferredImplicitHandleOutputCount(
        const RuntimeValue& handle,
        size_t suppliedInputCount = 0) const {
        if (!isFunctionHandle(handle)) {
            return 1;
        }
        const RuntimeFunctionHandle& info = *handle.functionHandle;
        if (info.targetName.rfind(
                "__mparser_property_validator/", 0) == 0) {
            return 0;
        }
        if (info.kind == RuntimeFunctionHandleKind::Anonymous) {
            return 1;
        }
        if (info.kind == RuntimeFunctionHandleKind::Builtin) {
            return preferredImplicitBuiltinOutputCount(
                info.targetName, suppliedInputCount);
        }
        if (info.kind == RuntimeFunctionHandleKind::Method) {
            const auto declaring =
                classesByName_.find(info.declaringClass);
            if (declaring == classesByName_.end()) {
                return 1;
            }
            const auto method =
                declaring->second.declaredMethods.find(info.methodName);
            if (method == declaring->second.declaredMethods.end()) {
                return 1;
            }
            return method->second.classDestructor
                       ? 0
                       : preferredImplicitOutputCount(
                             method->second.signature);
        }
        const auto function = functionsByName_.find(info.targetName);
        if (function != functionsByName_.end()) {
            return preferredImplicitOutputCount(
                function->second.signature);
        }
        if (const auto* inherited = inheritedSourceCallable(handle)) {
            return static_cast<int>(inherited->implicitOutputCount);
        }
        return 1;
    }

    std::optional<int> anonymousBodyOutputCount(
        const BytecodeInstruction& instruction) const {
        if (!instruction.anonymousBodyOutput ||
            activeAnonymousBodyOutputCounts_.empty() ||
            activeAnonymousBodyOutputCounts_.back() < 0) {
            return std::nullopt;
        }
        return activeAnonymousBodyOutputCounts_.back();
    }

    bool anonymousBodyUsesImplicitOutput(
        const BytecodeInstruction& instruction) const {
        return instruction.anonymousBodyOutput &&
               !activeAnonymousBodyOutputCounts_.empty() &&
               activeAnonymousBodyOutputCounts_.back() < 0;
    }

    void callOrIndex(const BytecodeInstruction& instruction) {
        const auto rawArguments = popRuntimeValues(
            instruction, instruction.operandCount,
            "call/index arguments");
        const auto callee = popStackValue(instruction, "call/index callee");
        if (!rawArguments || !callee) {
            return;
        }
        const auto arguments = runtimeExpandedValues(*rawArguments);

        if (instruction.operand == "system-command") {
            BytecodeInstruction callInstruction = instruction;
            callInstruction.resultCount = 0;
            auto outputs = callBuiltinOutputs(
                callInstruction, "system", arguments, 0);
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (callee->isClassReference) {
            BytecodeInstruction callInstruction = instruction;
            if (const auto outputCount =
                    anonymousBodyOutputCount(callInstruction)) {
                callInstruction.resultCount = *outputCount;
            } else if (anonymousBodyUsesImplicitOutput(
                           callInstruction) ||
                       callInstruction.implicitExpressionOutput) {
                callInstruction.implicitExpressionOutput = true;
                callInstruction.resultCount = 1;
            }
            auto outputs = callClassConstructor(
                callInstruction, callee->className, arguments,
                callInstruction.resultCount);
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (callee->isMethodReference) {
            BytecodeInstruction callInstruction = instruction;
            if (const auto outputCount =
                    anonymousBodyOutputCount(callInstruction)) {
                callInstruction.resultCount = *outputCount;
            } else if (anonymousBodyUsesImplicitOutput(
                           callInstruction) ||
                       callInstruction.implicitExpressionOutput) {
                callInstruction.implicitExpressionOutput = true;
                callInstruction.resultCount =
                    preferredImplicitMethodOutputCount(
                        callee->methodClassName, callee->methodName,
                        callee->methodDeclaringClass);
            }
            auto outputs = callClassMethod(
                callInstruction, callee->methodClassName,
                callee->methodName, callee->methodDeclaringClass,
                callee->receiver, arguments,
                callInstruction.resultCount);
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (isFunctionHandle(callee->value)) {
            BytecodeInstruction callInstruction = instruction;
            if (const auto outputCount =
                    anonymousBodyOutputCount(callInstruction)) {
                callInstruction.resultCount = *outputCount;
            } else if (anonymousBodyUsesImplicitOutput(
                           callInstruction) ||
                       callInstruction.implicitExpressionOutput) {
                callInstruction.implicitExpressionOutput = true;
                callInstruction.resultCount =
                    preferredImplicitHandleOutputCount(
                        callee->value, arguments.size());
            }
            BytecodeCallSiteProfile* profile = nullptr;
            if (profilingEnabled_) {
                profile = &recordCallSite(instruction, "function-handle",
                                          callee->value.text);
                observeValues(profile->argumentObservations, arguments);
            }
            auto outputs = callFunctionHandle(
                callInstruction, callee->value, arguments,
                callInstruction.resultCount);
            if (callInstruction.implicitExpressionOutput &&
                callee->value.functionHandle &&
                callee->value.functionHandle->kind ==
                    RuntimeFunctionHandleKind::Anonymous &&
                outputs.empty()) {
                callInstruction.resultCount = 0;
            }
            if (profile) {
                observeValues(profile->resultObservations, outputs);
            }
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (callee->isFunctionReference) {
            const std::string& name = callee->functionName;
            BytecodeInstruction callInstruction = instruction;
            if (const auto outputCount =
                    anonymousBodyOutputCount(callInstruction)) {
                callInstruction.resultCount = *outputCount;
            } else if (anonymousBodyUsesImplicitOutput(
                           callInstruction) ||
                       callInstruction.implicitExpressionOutput) {
                callInstruction.implicitExpressionOutput = true;
                const auto function = resolveLocalFunction(name);
                if (function) {
                    callInstruction.resultCount =
                        preferredImplicitOutputCount(
                            function->info->signature);
                } else if (const auto* inherited =
                               inheritedSourceCallable(name)) {
                    callInstruction.resultCount = static_cast<int>(
                        inherited->implicitOutputCount);
                } else {
                    callInstruction.resultCount = 1;
                }
            }
            BytecodeCallSiteProfile* profile = nullptr;
            if (profilingEnabled_) {
                profile = &recordCallSite(instruction, "function", name);
                observeValues(profile->argumentObservations, arguments);
            }
            auto outputs = callLocalFunction(
                callInstruction, name, arguments,
                callInstruction.resultCount);
            if (profile) {
                observeValues(profile->resultObservations, outputs);
            }
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (callee->isBuiltinReference) {
            const std::string& name = callee->builtinName;
            std::vector<RuntimeValue> callArguments = arguments;
            if (callee->receiver) {
                callArguments.insert(callArguments.begin(),
                                     *callee->receiver);
            }
            BytecodeInstruction callInstruction = instruction;
            if (const auto outputCount =
                    anonymousBodyOutputCount(callInstruction)) {
                callInstruction.resultCount = *outputCount;
            } else if (anonymousBodyUsesImplicitOutput(
                           callInstruction) ||
                       callInstruction.implicitExpressionOutput) {
                callInstruction.implicitExpressionOutput = true;
                callInstruction.resultCount =
                    preferredImplicitBuiltinOutputCount(
                        name, callArguments.size());
            }
            BytecodeCallSiteProfile* profile = nullptr;
            if (profilingEnabled_) {
                profile = &recordCallSite(instruction, "builtin", name);
                observeValues(profile->argumentObservations,
                              callArguments);
            }
            auto outputs = callBuiltinOutputs(
                callInstruction, name, callArguments,
                callInstruction.resultCount);
            if (profile) {
                observeValues(profile->resultObservations, outputs);
            }
            finishCallOrIndexContext(instruction);
            pushOutputValues(callInstruction, outputs);
            return;
        }

        if (instruction.resultCount != 0 && instruction.resultCount != 1) {
            finishCallOrIndexContext(instruction);
            addDiagnostic(instruction,
                          "bytecode indexing cannot produce multiple outputs");
            return;
        }

        if (callee->value.kind != RuntimeValueKind::MissingArray &&
            !isNumeric(callee->value) &&
            !isRuntimeTextValue(callee->value) &&
            callee->value.kind != RuntimeValueKind::Struct &&
            callee->value.kind != RuntimeValueKind::Cell &&
            !isRuntimeClassObject(callee->value) &&
            !isRuntimeMetadataObject(callee->value)) {
            finishCallOrIndexContext(instruction);
            addDiagnostic(instruction,
                          "bytecode indexing requires a numeric, text, cell, "
                          "structure, object, or metadata target");
            return;
        }
        BytecodeCallSiteProfile* profile = nullptr;
        if (profilingEnabled_) {
            profile = &recordCallSite(instruction, "index",
                                      instruction.operand);
            profile->hasReceiverObservation = true;
            observeValue(profile->receiverObservation, callee->value);
            observeValues(profile->argumentObservations, arguments);
        }
        RuntimeValue result = indexValue(instruction, callee->value,
                                         arguments);
        if (instruction.resultCount == 0) {
            finishCallOrIndexContext(instruction);
            return;
        }
        const std::vector<RuntimeValue> outputs{result};
        if (profile) {
            observeValues(profile->resultObservations, outputs);
        }
        finishCallOrIndexContext(instruction);
        pushRuntime(std::move(result));
    }

    std::vector<RuntimeValue> callAnonymousFunctionHandle(
        const BytecodeInstruction& instruction,
        const RuntimeFunctionHandle& info,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        if (arguments.size() != info.parameters.size()) {
            addDiagnostic(instruction,
                          "anonymous function argument count mismatch: " +
                              info.display);
            return missingOutputs(requestedCount);
        }
        if (requestedCount < 0 || requestedCount > 1) {
            addDiagnostic(instruction,
                          "anonymous function supports at most one output: " +
                              info.display);
            return missingOutputs(requestedCount);
        }

        ExecutionCallGuard executionCall(*this, instruction.span);
        if (!executionCall) {
            return missingOutputs(requestedCount);
        }
        ExceptionFunctionGuard exceptionTrace(
            *this, info.display.empty() ? std::string("<anonymous>")
                                        : info.display,
            instruction.span);

        const bool savedReturnRequested = returnRequested_;
        const size_t savedPc = currentPc_;
        const size_t stackBase = stack_.size();
        auto savedForLoopStack = std::move(forLoopStack_);
        auto savedIndexContextStack = std::move(indexContextStack_);
        auto savedSwitchContextStack = std::move(switchContextStack_);
        auto savedTryContextStack = std::move(tryContextStack_);
        returnRequested_ = false;
        forLoopStack_.clear();
        indexContextStack_.clear();
        switchContextStack_.clear();
        tryContextStack_.clear();

        frames_.push_back(makeRuntimeFunctionFrame(
            RuntimeCallFrameKind::AnonymousFunction,
            info.display.empty() ? std::string("<anonymous>")
                                 : info.display,
            info.span, arguments.size(),
            instruction.implicitExpressionOutput
                ? 0
                : static_cast<size_t>(requestedCount),
            info.capturedVariables));
        for (size_t index = 0; index < info.parameters.size(); ++index) {
            if (info.parameters[index] != "~") {
                currentFrame()[info.parameters[index]] = arguments[index];
            }
        }

        if (!info.lexicalClassName.empty()) {
            activeClassFunctions_.push_back(ActiveClassFunction{
                info.lexicalClassName, "<anonymous>", {}, nullptr});
        }
        enterFunctionProfile(info.display, info.span);
        activeAnonymousBodyOutputCounts_.push_back(
            instruction.implicitExpressionOutput ? -1 : requestedCount);
        executeFunctionBody(info.entry, info.end);
        activeAnonymousBodyOutputCounts_.pop_back();
        leaveFunctionProfile();
        if (!info.lexicalClassName.empty()) {
            activeClassFunctions_.pop_back();
        }

        RuntimeValue output = missingValue();
        const bool producedOutput = stack_.size() > stackBase;
        if (producedOutput) {
            output = stack_.back().value;
        } else if (requestedCount != 0 &&
                   !instruction.implicitExpressionOutput &&
                   diagnostics_.empty()) {
            addDiagnostic(instruction,
                          "anonymous function body produced no value: " +
                              info.display);
        }
        stack_.resize(stackBase);
        frames_.pop_back();
        returnRequested_ = savedReturnRequested;
        currentPc_ = savedPc;
        forLoopStack_ = std::move(savedForLoopStack);
        indexContextStack_ = std::move(savedIndexContextStack);
        switchContextStack_ = std::move(savedSwitchContextStack);
        tryContextStack_ = std::move(savedTryContextStack);

        if (requestedCount == 0 ||
            (instruction.implicitExpressionOutput && !producedOutput)) {
            return {};
        }
        return {output};
    }

    std::vector<RuntimeValue> callInheritedSourceCallable(
        const BytecodeInstruction& instruction,
        const RuntimeValue& callable,
        const std::vector<RuntimeValue>& arguments,
        int requestedCount) {
        if (requestedCount < 0) {
            addDiagnostic(
                instruction,
                "inherited callable result count cannot be negative");
            return {};
        }
        if (!inheritedSourceCallableInvoker_) {
            addDiagnostic(
                instruction,
                "inherited callable invocation context is unavailable",
                "MParser:MissingInheritedCallableContext");
            return missingOutputs(requestedCount);
        }

        if (inheritedSourceCallableWorkspace_) {
            *inheritedSourceCallableWorkspace_ = currentFrame();
        }
        auto result = inheritedSourceCallableInvoker_(
            callable, arguments, static_cast<size_t>(requestedCount),
            instruction.span, inheritedSourceCallableWorkspace_);
        if (inheritedSourceCallableWorkspace_) {
            currentFrame() = *inheritedSourceCallableWorkspace_;
        }

        for (const auto& event : result.outputEvents) {
            if (!runtimeOutputSink_ || !runtimeOutputSink_(event)) {
                addDiagnostic(
                    instruction,
                    "dynamic parent callable output was rejected",
                    "MParser:OutputSinkRejected");
                break;
            }
        }
        appendBuiltinDiagnostics(
            instruction, std::move(result.diagnostics));
        if (!result.succeeded) {
            return missingOutputs(requestedCount);
        }
        if (result.outputs.size() !=
            static_cast<size_t>(requestedCount)) {
            addDiagnostic(
                instruction,
                "inherited callable produced an unexpected number of "
                "outputs",
                "MParser:InvalidInheritedCallableResult");
            return missingOutputs(requestedCount);
        }
        return std::move(result.outputs);
    }

    std::vector<RuntimeValue> callFunctionHandle(
        const BytecodeInstruction& instruction, const RuntimeValue& handle,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        if (!isFunctionHandle(handle)) {
            addDiagnostic(instruction,
                          "function handle descriptor is unavailable");
            return missingOutputs(requestedCount);
        }

        const RuntimeFunctionHandle& info = *handle.functionHandle;
        if (info.kind != RuntimeFunctionHandleKind::Builtin &&
            (info.backend != RuntimeFunctionHandleBackend::Bytecode ||
             !info.context || info.context != callableContext_)) {
            if (inheritedSourceCallableInvoker_) {
                return callInheritedSourceCallable(
                    instruction, handle, arguments, requestedCount);
            }
            addDiagnostic(
                instruction,
                "function handle belongs to a different compiled module");
            return missingOutputs(requestedCount);
        }

        auto savedActiveClassFunctions = std::move(activeClassFunctions_);
        activeClassFunctions_.clear();
        std::vector<RuntimeValue> outputs;
        if (info.kind == RuntimeFunctionHandleKind::Anonymous) {
            outputs = callAnonymousFunctionHandle(
                instruction, info, arguments, requestedCount);
        } else if (
            info.targetName.rfind(
                "__mparser_property_validator/", 0) == 0) {
            outputs = callPropertyValidatorHandle(
                instruction, info.targetName, arguments,
                requestedCount);
        } else if (info.kind == RuntimeFunctionHandleKind::Builtin) {
            outputs = callBuiltinOutputs(instruction, info.targetName,
                                         arguments, requestedCount);
        } else {
            const FunctionInfo* callable = nullptr;
            if (info.kind == RuntimeFunctionHandleKind::Function) {
                const auto function = functionsByName_.find(info.targetName);
                if (function != functionsByName_.end()) {
                    callable = &function->second;
                }
            } else if (info.kind == RuntimeFunctionHandleKind::Method) {
                const auto declaring =
                    classesByName_.find(info.declaringClass);
                if (declaring != classesByName_.end()) {
                    const auto method = declaring->second.declaredMethods.find(
                        info.methodName);
                    if (method != declaring->second.declaredMethods.end()) {
                        callable = &method->second;
                    }
                }
            }
            if (!callable) {
                addDiagnostic(instruction,
                              "function handle target is unavailable: " +
                                  info.display);
                activeClassFunctions_ =
                    std::move(savedActiveClassFunctions);
                return missingOutputs(requestedCount);
            }

            std::vector<RuntimeValue> callArguments = arguments;
            if (info.receiver) {
                callArguments.insert(callArguments.begin(), *info.receiver);
            }
            const std::string name =
                info.kind == RuntimeFunctionHandleKind::Method
                    ? info.declaringClass + "." + info.methodName
                    : info.targetName;
            outputs = callFunctionInfo(
                instruction, name, *callable, callArguments,
                requestedCount, std::nullopt, nullptr, false);
        }
        activeClassFunctions_ = std::move(savedActiveClassFunctions);
        return outputs;
    }

    void callSuperclass(const BytecodeInstruction& instruction) {
        const auto rawArguments = popRuntimeValues(
            instruction, instruction.operandCount,
            "superclass call arguments");
        if (!rawArguments) {
            return;
        }
        const auto arguments = runtimeExpandedValues(*rawArguments);

        const bool activeConstructor =
            !activeClassFunctions_.empty() &&
            activeClassFunctions_.back().construction != nullptr &&
            activeClassFunctions_.back().constructorOutput ==
                instruction.receiverName;
        if (instruction.binding.kind == BindingKind::Class ||
            activeConstructor) {
            callSuperclassConstructor(instruction, arguments);
            return;
        }
        callQualifiedSuperclassMethod(instruction, arguments);
    }

    void callSuperclassConstructor(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (activeClassFunctions_.empty() ||
            activeClassFunctions_.back().construction == nullptr) {
            addDiagnostic(
                instruction,
                "superclass constructor call is not inside a constructor");
            return;
        }

        const ActiveClassFunction active = activeClassFunctions_.back();
        auto klass = classesByName_.find(active.className);
        if (klass == classesByName_.end() ||
            std::find(klass->second.superclasses.begin(),
                      klass->second.superclasses.end(),
                      instruction.operand) ==
                klass->second.superclasses.end() ||
            isBuiltinNonExecutableSuperclass(instruction.operand)) {
            addDiagnostic(
                instruction,
                "superclass constructor is not a direct executable "
                "superclass: " +
                    instruction.operand);
            return;
        }
        if (instruction.receiverName != active.constructorOutput) {
            addDiagnostic(
                instruction,
                "superclass constructor must use the constructor output "
                "object");
            return;
        }

        auto receiver = currentFrame().find(instruction.receiverName);
        if (receiver == currentFrame().end() ||
            !isObject(receiver->second)) {
            addDiagnostic(instruction,
                          "superclass constructor receiver is not an object: " +
                              instruction.receiverName);
            return;
        }
        active.construction->object = receiver->second;

        BytecodeCallSiteProfile* profile = nullptr;
        if (profilingEnabled_) {
            profile = &recordCallSite(
                instruction, "super-constructor", instruction.operand);
            observeValues(profile->argumentObservations, arguments);
        }
        const size_t diagnosticCount = diagnostics_.size();
        auto outputs = constructClass(instruction, instruction.operand,
                                      arguments, *active.construction, 1,
                                      true, active.className);
        if (diagnostics_.size() != diagnosticCount || outputs.empty()) {
            return;
        }

        currentFrame()[instruction.receiverName] =
            active.construction->object;
        std::vector<RuntimeValue> visibleOutputs;
        if (instruction.resultCount == 1) {
            visibleOutputs.push_back(active.construction->object);
        } else if (instruction.resultCount != 0) {
            addDiagnostic(
                instruction,
                "superclass constructor call supports at most one output");
            return;
        }
        if (profile) {
            observeValues(profile->resultObservations, visibleOutputs);
        }
        pushOutputValues(instruction, visibleOutputs);
    }

    void callQualifiedSuperclassMethod(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (activeClassFunctions_.empty()) {
            addDiagnostic(
                instruction,
                "qualified superclass method call is not inside a class "
                "method");
            return;
        }
        const ActiveClassFunction active = activeClassFunctions_.back();
        if (active.className.empty() ||
            active.methodName != instruction.receiverName ||
            active.className == instruction.operand ||
            !classDerivesFrom(active.className, instruction.operand)) {
            addDiagnostic(
                instruction,
                "invalid qualified superclass method call: " +
                    instruction.receiverName + "@" + instruction.operand);
            return;
        }
        const auto superclass = classesByName_.find(instruction.operand);
        if (superclass == classesByName_.end() ||
            !superclass->second.declaredMethods.contains(
                instruction.receiverName)) {
            addDiagnostic(instruction,
                          "superclass method is not available: " +
                              instruction.operand + "." +
                              instruction.receiverName);
            return;
        }
        if (arguments.empty() || !isObject(arguments.front()) ||
            !classDerivesFrom(arguments.front().className,
                              instruction.operand)) {
            addDiagnostic(
                instruction,
                "qualified superclass method requires a compatible object "
                "argument");
            return;
        }

        BytecodeCallSiteProfile* profile = nullptr;
        if (profilingEnabled_) {
            profile = &recordCallSite(
                instruction, "super-method",
                instruction.operand + "." + instruction.receiverName);
            profile->hasReceiverObservation = true;
            observeValue(profile->receiverObservation, arguments.front());
            observeValues(profile->argumentObservations, arguments);
        }
        const auto& method = superclass->second.declaredMethods.at(
            instruction.receiverName);
        if (!hasMemberAccess(method.access, method.declaringClass)) {
            addDiagnostic(instruction,
                          "superclass method access is denied: " +
                              instruction.operand + "." +
                              instruction.receiverName);
            return;
        }
        auto outputs = callFunctionInfo(
            instruction,
            instruction.operand + "." + instruction.receiverName, method,
            arguments, instruction.resultCount, std::nullopt, nullptr,
            false);
        if (profile) {
            observeValues(profile->resultObservations, outputs);
        }
        pushOutputValues(instruction, outputs);
    }

    const EventInfo* selectEvent(const RuntimeValue& source,
                                 const std::string& eventName) const {
        if (!isObject(source)) {
            return nullptr;
        }
        if (source.handleObject &&
            eventName == kObjectBeingDestroyedEventName) {
            return &handleDestructionEvent();
        }
        const auto klass = classesByName_.find(source.className);
        if (klass == classesByName_.end()) {
            return nullptr;
        }
        const auto event = klass->second.events.find(eventName);
        return event == klass->second.events.end() ? nullptr
                                                   : &event->second;
    }

    bool isPropertyEventName(std::string_view name) const {
        return name == "PreGet" || name == "PostGet" ||
               name == "PreSet" || name == "PostSet";
    }

    bool isPropertyGetEvent(std::string_view name) const {
        return name == "PreGet" || name == "PostGet";
    }

    RuntimeValue propertyDescriptorForSource(
        const RuntimeValue& source, const PropertyInfo& property) const {
        const auto view = classesByName_.find(source.className);
        if (view == classesByName_.end()) {
            return emptyMetadataArray(RuntimeMetadataKind::Property);
        }
        return makeRuntimeMetadataObject(
            RuntimeMetadataKind::Property,
            propertyMetadataIdentity(view->second, property));
    }

    std::optional<PropertyListenerTarget> propertyListenerTarget(
        const BytecodeInstruction& instruction,
        const RuntimeValue& source, const RuntimeValue& selector,
        bool requireMetadataSelector) {
        const auto klass = classesByName_.find(source.className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction,
                          "property listener class is not available: " +
                              source.className);
            return std::nullopt;
        }

        if (isString(selector)) {
            if (requireMetadataSelector) {
                addDiagnostic(
                    instruction,
                    "event.proplistener requires a scalar property "
                    "metadata descriptor");
                return std::nullopt;
            }
            const std::string selectorName =
                *runtimeTextScalarUtf8(selector);
            if (const auto descriptor = dynamicPropertyDescriptor(
                    source, selectorName)) {
                return PropertyListenerTarget{
                    *descriptor, dynamicPropertyName(*descriptor), {},
                    descriptor->opaqueId,
                    dynamicPropertyLogicalField(*descriptor,
                                                "GetObservable"),
                    dynamicPropertyLogicalField(*descriptor,
                                                "SetObservable")};
            }
            const auto property =
                selectProperty(klass->second, selectorName);
            if (!property) {
                addDiagnostic(instruction,
                              "property is not available for listener: " +
                                  source.className + "." + selectorName);
                return std::nullopt;
            }
            return PropertyListenerTarget{
                propertyDescriptorForSource(source, *property),
                property->name, property->storageKey, 0,
                property->getObservable,
                property->setObservable};
        }

        const auto kind = runtimeMetadataKind(selector);
        if (kind == RuntimeMetadataKind::DynamicProperty) {
            registerOwnerDynamicProperties(source);
            if (!dynamicPropertyIsValid(selector)) {
                addDiagnostic(instruction,
                              "dynamic property descriptor is not valid");
                return std::nullopt;
            }
            const auto record = dynamicProperties_.find(selector.opaqueId);
            const auto owner =
                record == dynamicProperties_.end()
                    ? nullptr
                    : record->second.ownerFields.lock();
            const auto descriptorFields =
                record == dynamicProperties_.end()
                    ? nullptr
                    : record->second.descriptorFields.lock();
            if (!owner || owner.get() != source.sharedFields.get() ||
                !descriptorFields || !selector.sharedFields ||
                descriptorFields.get() != selector.sharedFields.get()) {
                addDiagnostic(
                    instruction,
                    "dynamic property descriptor does not belong to the "
                    "listener source");
                return std::nullopt;
            }
            return PropertyListenerTarget{
                selector, dynamicPropertyName(selector), {},
                selector.opaqueId,
                dynamicPropertyLogicalField(selector, "GetObservable"),
                dynamicPropertyLogicalField(selector, "SetObservable")};
        }

        if (kind == RuntimeMetadataKind::Property &&
            isRuntimeMetadataScalar(selector)) {
            const PropertyInfoPtr property =
                propertyForMetadata(selector.text);
            const bool belongsToSource =
                property && std::any_of(
                                klass->second.propertyOrder.begin(),
                                klass->second.propertyOrder.end(),
                                [&property](const PropertyInfoPtr& candidate) {
                                    return candidate &&
                                           candidate->storageKey ==
                                               property->storageKey;
                                });
            if (!belongsToSource) {
                addDiagnostic(
                    instruction,
                    "property metadata descriptor does not belong to the "
                    "listener source");
                return std::nullopt;
            }
            return PropertyListenerTarget{
                propertyDescriptorForSource(source, *property),
                property->name, property->storageKey, 0,
                property->getObservable, property->setObservable};
        }

        addDiagnostic(
            instruction,
            "property listener selector must be a property-name string or "
            "scalar property metadata descriptor");
        return std::nullopt;
    }

    RuntimeValue createEventListener(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments, bool coupled) {
        if (arguments.size() != 3 || !isObject(arguments[0]) ||
            !arguments[0].handleObject || !arguments[0].sharedFields ||
            !isString(arguments[1]) ||
            !isFunctionHandle(arguments[2])) {
            addDiagnostic(
                instruction,
                std::string(coupled ? "addlistener" : "listener") +
                    " expects a handle object, event-name string, and "
                    "function handle");
            return missingValue();
        }

        const RuntimeValue& source = arguments[0];
        if (!requireUsableHandleObject(instruction, source)) {
            return missingValue();
        }
        const std::string eventName =
            *runtimeTextScalarUtf8(arguments[1]);
        const EventInfo* event = selectEvent(source, eventName);
        if (!event) {
            addDiagnostic(instruction,
                          "event is not available: " + source.className +
                              "." + eventName);
            return missingValue();
        }
        if (!hasMemberAccess(event->listenAccess,
                             event->declaringClass)) {
            addDiagnostic(instruction,
                          "event listen access is denied: " +
                              event->declaringClass + "." + eventName);
            return missingValue();
        }

        const size_t id = nextEventListenerId_++;
        std::map<std::string, RuntimeValue> fields;
        fields["Source"] = source;
        fields["EventName"] = characterValue(eventName);
        fields["Callback"] = arguments[2];
        fields["Enabled"] = numberValue(1.0);
        fields["Recursive"] = numberValue(0.0);
        fields[std::string(kListenerValidityField)] = numberValue(1.0);
        RuntimeValue listener = objectValue(
            std::string(kEventListenerClassName), std::move(fields), true);
        listener.opaqueId = id;

        EventListenerRecord record;
        record.id = id;
        record.sourceFields = source.sharedFields;
        record.listenerFields = listener.sharedFields;
        if (coupled) {
            record.retainedListenerFields = listener.sharedFields;
        }
        record.sourceClass = source.className;
        record.eventName = eventName;
        record.coupled = coupled;
        eventListeners_[id] = std::move(record);
        return listener;
    }

    RuntimeValue createPropertyListener(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments, bool coupled,
        bool requireMetadataSelector = false) {
        if (arguments.size() != 4 || !isObject(arguments[0]) ||
            !arguments[0].handleObject || !arguments[0].sharedFields ||
            !isString(arguments[2]) ||
            !isFunctionHandle(arguments[3])) {
            addDiagnostic(
                instruction,
                std::string(requireMetadataSelector
                                ? "event.proplistener"
                                : coupled ? "addlistener" : "listener") +
                    " expects a handle object, property selector, property "
                    "event name, and function handle");
            return missingValue();
        }

        const RuntimeValue& source = arguments[0];
        if (!requireUsableHandleObject(instruction, source)) {
            return missingValue();
        }
        const std::string eventName =
            *runtimeTextScalarUtf8(arguments[2]);
        if (!isPropertyEventName(eventName)) {
            addDiagnostic(instruction,
                          "unknown property event name: " + eventName);
            return missingValue();
        }
        const auto target = propertyListenerTarget(
            instruction, source, arguments[1], requireMetadataSelector);
        if (!target) {
            return missingValue();
        }
        const bool observable = isPropertyGetEvent(eventName)
                                    ? target->getObservable
                                    : target->setObservable;
        if (!observable) {
            addDiagnostic(
                instruction,
                std::string(isPropertyGetEvent(eventName)
                                ? "GetObservable"
                                : "SetObservable") +
                    " is false for property: " +
                    target->name);
            return missingValue();
        }

        const size_t id = nextEventListenerId_++;
        std::map<std::string, RuntimeValue> fields;
        fields["Source"] = target->descriptor;
        fields["Object"] = cellValue({source});
        fields["EventName"] = characterValue(eventName);
        fields["Callback"] = arguments[3];
        fields["Enabled"] = logicalValue(true);
        fields["Recursive"] = logicalValue(false);
        fields[std::string(kListenerValidityField)] = logicalValue(true);
        RuntimeValue listener = objectValue(
            std::string(kPropertyListenerClassName), std::move(fields), true);
        listener.opaqueId = id;

        EventListenerRecord record;
        record.id = id;
        record.sourceFields = source.sharedFields;
        record.listenerFields = listener.sharedFields;
        if (coupled) {
            record.retainedListenerFields = listener.sharedFields;
        }
        record.sourceClass = source.className;
        record.eventName = eventName;
        record.propertyStorageKey = target->storageKey;
        record.dynamicPropertyId = target->dynamicPropertyId;
        record.propertyListener = true;
        record.coupled = coupled;
        eventListeners_[id] = std::move(record);
        return listener;
    }

    bool invokeEventListenerCallback(
        const BytecodeInstruction& instruction, size_t id,
        const std::vector<RuntimeValue>& callbackArguments) {
        auto record = eventListeners_.find(id);
        if (record == eventListeners_.end()) {
            return true;
        }
        const auto listenerFields =
            record->second.listenerFields.lock();
        if (!listenerFields) {
            return true;
        }
        const auto valid = listenerFields->find(
            std::string(kListenerValidityField));
        const auto enabled = listenerFields->find("Enabled");
        const auto recursive = listenerFields->find("Recursive");
        const auto callback = listenerFields->find("Callback");
        if (valid == listenerFields->end() || !truthy(valid->second) ||
            enabled == listenerFields->end() ||
            !truthy(enabled->second) || callback == listenerFields->end() ||
            !isFunctionHandle(callback->second)) {
            return true;
        }
        const bool recursionEnabled =
            recursive != listenerFields->end() &&
            truthy(recursive->second);
        if (record->second.callbackActive && !recursionEnabled) {
            return true;
        }

        record->second.callbackActive = true;
        const size_t diagnosticCount = diagnostics_.size();
        (void)callFunctionHandle(instruction, callback->second,
                                 callbackArguments, 0);
        record->second.callbackActive = false;
        return diagnostics_.size() == diagnosticCount;
    }

    bool dispatchPropertyEvent(
        const BytecodeInstruction& instruction,
        const RuntimeValue& source, const RuntimeValue& descriptor,
        std::string_view storageKey, size_t dynamicPropertyId,
        std::string_view eventName) {
        if (!source.sharedFields) {
            return true;
        }

        std::vector<size_t> listeners;
        for (const auto& [id, record] : eventListeners_) {
            const auto listenerSource = record.sourceFields.lock();
            if (!record.propertyListener || !listenerSource ||
                listenerSource.get() != source.sharedFields.get() ||
                record.eventName != eventName) {
                continue;
            }
            const bool propertyMatches =
                dynamicPropertyId != 0
                    ? record.dynamicPropertyId == dynamicPropertyId
                    : record.dynamicPropertyId == 0 &&
                          record.propertyStorageKey == storageKey;
            if (propertyMatches) {
                listeners.push_back(id);
            }
        }
        if (listeners.empty()) {
            return true;
        }

        RuntimeValue eventData = objectValue(
            std::string(kPropertyEventClassName),
            {{"AffectedObject", source},
             {"EventName", characterValue(std::string(eventName))},
             {"Source", descriptor}},
            true);
        for (const size_t id : listeners) {
            if (!invokeEventListenerCallback(
                    instruction, id, {descriptor, eventData})) {
                return false;
            }
        }
        return true;
    }

    RuntimeValue eventNamesBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 ||
            (!isString(arguments[0]) && !isObject(arguments[0]))) {
            addDiagnostic(instruction,
                          "events expects a class-name string or object");
            return missingValue();
        }
        const std::string className = canonicalRuntimeMetadataClassName(
            isString(arguments[0])
                ? *runtimeTextScalarUtf8(arguments[0])
                                   : arguments[0].className);
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            if (isBuiltinReflectableClass(className)) {
                return stringColumnCell(
                    isBuiltinHandleRuntimeClass(className)
                        ? std::vector<std::string>{std::string(
                              kObjectBeingDestroyedEventName)}
                        : std::vector<std::string>{});
            }
            addDiagnostic(instruction,
                          "event class is not available: " + className);
            return missingValue();
        }

        std::vector<std::string> names;
        for (const auto& eventName : klass->second.eventOrder) {
            const auto event = klass->second.events.find(eventName);
            if (event == klass->second.events.end() || event->second.hidden ||
                event->second.listenAccess.level !=
                    MemberAccessLevel::Public) {
                continue;
            }
            names.push_back(eventName);
        }
        return stringColumnCell(names);
    }

    RuntimeValue metaclassBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1) {
            addDiagnostic(instruction, "metaclass expects one object");
            return missingValue();
        }
        const std::string className =
            runtimeValueClassName(arguments.front());
        if (!reflectableClassExists(className)) {
            addDiagnostic(instruction,
                          "class metadata is not available: " + className);
            return missingValue();
        }
        return metadataClassValue(className);
    }

    RuntimeValue metadataClassFromNameBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 || !isString(arguments.front())) {
            addDiagnostic(
                instruction,
                "matlab.metadata.Class.fromName expects one class-name string");
            return missingValue();
        }
        const std::string className =
            canonicalRuntimeMetadataClassName(
                *runtimeTextScalarUtf8(arguments.front()));
        if (!reflectableClassExists(className)) {
            return makeRuntimeMetadataArray(
                RuntimeMetadataKind::Class, {}, {0, 1});
        }
        return metadataClassValue(className);
    }

    std::optional<MetafunctionSelector> parseMetafunctionSelector(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        MetafunctionSelector selector;
        if (arguments.size() == 1) {
            return selector;
        }
        if (arguments.size() != 2 ||
            arguments[1].kind != RuntimeValueKind::NameValueArgument ||
            arguments[1].cells.size() != 1) {
            addDiagnostic(
                instruction,
                "metafunction accepts one identifier plus either "
                "Arguments= or ArgumentTypes=");
            return std::nullopt;
        }
        const std::string option = lowerAscii(arguments[1].text);
        const RuntimeValue& value = arguments[1].cells.front();
        if (option == "arguments") {
            if (value.kind != RuntimeValueKind::Cell) {
                addDiagnostic(
                    instruction,
                    "metafunction Arguments must be a Cell array");
                return std::nullopt;
            }
            selector.kind = MetafunctionSelectorKind::Arguments;
            selector.arguments = value.cells;
            return selector;
        }
        if (option == "argumenttypes") {
            selector.kind = MetafunctionSelectorKind::ArgumentTypes;
            if (isString(value)) {
                selector.argumentTypes.push_back(
                    canonicalRuntimeMetadataClassName(
                        *runtimeTextScalarUtf8(value)));
                return selector;
            }
            if (value.kind != RuntimeValueKind::Cell) {
                addDiagnostic(
                    instruction,
                    "metafunction ArgumentTypes must be a class-name "
                    "string or Cell array of class-name strings");
                return std::nullopt;
            }
            for (const auto& type : value.cells) {
                if (!isString(type)) {
                    addDiagnostic(
                        instruction,
                        "metafunction ArgumentTypes Cell elements must be "
                        "class-name strings");
                    return std::nullopt;
                }
                selector.argumentTypes.push_back(
                    canonicalRuntimeMetadataClassName(
                        *runtimeTextScalarUtf8(type)));
            }
            return selector;
        }
        addDiagnostic(instruction,
                      "unknown metafunction selector: " +
                          arguments[1].text);
        return std::nullopt;
    }

    std::vector<std::string> nameValueArgumentDeclarations(
        const FunctionInfo& info) const {
        std::vector<std::string> names;
        for (const auto& contract : info.argumentContracts) {
            if (contract.blockKind == ArgumentBlockKind::Input &&
                contract.name.find('.') != std::string::npos) {
                names.push_back(contract.name);
            }
        }
        return names;
    }

    bool metadataArgumentTypeMatches(
        std::string_view actual,
        std::string_view expected) const {
        if (expected.empty()) {
            return true;
        }
        const std::string canonicalActual =
            canonicalRuntimeMetadataClassName(actual);
        const std::string canonicalExpected =
            canonicalRuntimeMetadataClassName(expected);
        return canonicalActual == canonicalExpected ||
               reflectableClassDerivesFrom(canonicalActual,
                                           canonicalExpected);
    }

    const ArgumentContract* positionalArgumentContract(
        const FunctionInfo& info, size_t argumentIndex) const {
        const size_t fixed =
            functionPositionalParameterCount(info.signature);
        const size_t repeating =
            functionRepeatingParameterCount(info.signature);
        if (argumentIndex < fixed) {
            return reflectedArgumentContract(
                info, info.signature.parameters[argumentIndex],
                ArgumentBlockKind::Input,
                ArgumentBlockKind::RepeatingInput);
        }
        if (repeating != 0) {
            const size_t parameterIndex =
                fixed + (argumentIndex - fixed) % repeating;
            if (parameterIndex < info.signature.parameters.size()) {
                return reflectedArgumentContract(
                    info, info.signature.parameters[parameterIndex],
                    ArgumentBlockKind::RepeatingInput,
                    ArgumentBlockKind::Input);
            }
        }
        if (info.signature.hasVarargin) {
            return reflectedArgumentContract(
                info, "varargin", ArgumentBlockKind::RepeatingInput,
                ArgumentBlockKind::Input);
        }
        return nullptr;
    }

    bool metafunctionSelectorMatches(
        const FunctionInfo& info,
        const MetafunctionSelector& selector,
        std::string_view receiverClass = {},
        bool requireReceiver = false) const {
        if (selector.kind == MetafunctionSelectorKind::None) {
            return true;
        }

        if (selector.kind ==
            MetafunctionSelectorKind::ArgumentTypes) {
            if (functionPositionalArgumentCountStatus(
                    info.signature,
                    selector.argumentTypes.size()) !=
                FunctionArgumentCountStatus::Valid) {
                return false;
            }
            if (requireReceiver &&
                (selector.argumentTypes.empty() ||
                 !metadataArgumentTypeMatches(
                     selector.argumentTypes.front(),
                     receiverClass))) {
                return false;
            }
            for (size_t index = 0;
                 index < selector.argumentTypes.size(); ++index) {
                const ArgumentContract* contract =
                    positionalArgumentContract(info, index);
                if (contract &&
                    !contract->spec.className.empty() &&
                    !metadataArgumentTypeMatches(
                        selector.argumentTypes[index],
                        contract->spec.className)) {
                    return false;
                }
            }
            return true;
        }

        auto normalized = normalizeRuntimeInvocationArguments(
            info.signature, nameValueArgumentDeclarations(info),
            selector.arguments);
        if (!normalized.succeeded) {
            return false;
        }
        if (requireReceiver &&
            (normalized.positionalArguments.empty() ||
             !metadataArgumentTypeMatches(
                 runtimeValueClassName(
                     normalized.positionalArguments.front()),
                 receiverClass))) {
            return false;
        }

        RuntimeArgumentValidationOptions validationOptions;
        validationOptions.objectIsA =
            [this](const std::string& actual,
                   const std::string& expected) {
                return classDerivesFrom(actual, expected);
            };
        validationOptions.classAvailable =
            [this](const std::string& className) {
                return reflectableClassExists(className);
            };
        for (size_t index = 0;
             index < normalized.positionalArguments.size(); ++index) {
            const ArgumentContract* contract =
                positionalArgumentContract(info, index);
            if (!contract) {
                continue;
            }
            const auto validation = validateRuntimeArgument(
                normalized.positionalArguments[index],
                contract->spec, validationOptions);
            if (!validation.succeeded) {
                return false;
            }
        }
        for (const auto& [name, value] :
             normalized.nameValueArguments) {
            const ArgumentContract* contract = argumentContract(
                info, name, ArgumentBlockKind::Input);
            if (!contract) {
                return false;
            }
            const auto validation = validateRuntimeArgument(
                value, contract->spec, validationOptions);
            if (!validation.succeeded) {
                return false;
            }
        }
        return true;
    }

    RuntimeValue methodMetadataValue(
        const ClassInfo& viewClass,
        const FunctionInfo& method) const {
        return makeRuntimeMetadataObject(
            RuntimeMetadataKind::Method,
            methodMetadataIdentity(viewClass, method));
    }

    RuntimeValue metafunctionBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty() || !isString(arguments.front())) {
            addDiagnostic(
                instruction,
                "metafunction expects a function or method identifier string");
            return missingValue();
        }
        const auto selector =
            parseMetafunctionSelector(instruction, arguments);
        if (!selector) {
            return missingValue();
        }
        const std::string identifier =
            *runtimeTextScalarUtf8(arguments.front());

        if (ambiguousFunctionMetadataIdentifiers_.contains(identifier)) {
            addDiagnostic(
                instruction,
                "function metadata identifier is ambiguous: " +
                    identifier);
            return missingValue();
        }

        if (const FunctionInfo* function =
                functionForPublicMetadataIdentifier(identifier)) {
            if (!metafunctionSelectorMatches(*function, *selector)) {
                addDiagnostic(
                    instruction,
                    "function signature metadata does not match selector: " +
                        identifier);
                return missingValue();
            }
            return makeRuntimeMetadataObject(
                RuntimeMetadataKind::Function, function->name);
        }

        const size_t slash = identifier.find_last_of('/');
        if (slash != std::string::npos && slash != 0 &&
            slash + 1 < identifier.size()) {
            const std::string className =
                identifier.substr(0, slash);
            const std::string methodName =
                identifier.substr(slash + 1);
            const auto klass = classesByName_.find(className);
            const FunctionInfo* method =
                klass == classesByName_.end()
                    ? nullptr
                    : selectMethod(klass->second, methodName, false);
            const bool constructor =
                method && method->name == className;
            if (!method ||
                !metafunctionSelectorMatches(
                    *method, *selector, className,
                    !method->staticMethod && !constructor)) {
                addDiagnostic(instruction,
                              "method metadata is not available: " +
                                  identifier);
                return missingValue();
            }
            return methodMetadataValue(klass->second, *method);
        }

        if (const auto klass = classesByName_.find(identifier);
            klass != classesByName_.end()) {
            const auto constructor =
                klass->second.declaredMethods.find(identifier);
            if (constructor !=
                    klass->second.declaredMethods.end() &&
                metafunctionSelectorMatches(
                    constructor->second, *selector)) {
                return methodMetadataValue(
                    klass->second, constructor->second);
            }
            addDiagnostic(instruction,
                          "constructor metadata is not available: " +
                              identifier);
            return missingValue();
        }

        const ClassInfo* dottedClass = nullptr;
        const FunctionInfo* dottedMethod = nullptr;
        for (const auto& [className, klass] : classesByName_) {
            const std::string prefix = className + ".";
            if (identifier.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string methodName =
                identifier.substr(prefix.size());
            if (methodName.empty() ||
                methodName.find('.') != std::string::npos) {
                continue;
            }
            const FunctionInfo* method =
                selectMethod(klass, methodName, false);
            if (!method || !method->staticMethod ||
                !metafunctionSelectorMatches(
                    *method, *selector, className, false)) {
                continue;
            }
            if (!dottedClass ||
                className.size() > dottedClass->name.size()) {
                dottedClass = &klass;
                dottedMethod = method;
            }
        }
        if (dottedClass && dottedMethod) {
            return methodMetadataValue(*dottedClass, *dottedMethod);
        }

        std::string receiverClass;
        if (selector->kind ==
                MetafunctionSelectorKind::Arguments &&
            !selector->arguments.empty()) {
            receiverClass =
                runtimeValueClassName(selector->arguments.front());
        } else if (
            selector->kind ==
                MetafunctionSelectorKind::ArgumentTypes &&
            !selector->argumentTypes.empty()) {
            receiverClass = selector->argumentTypes.front();
        }
        if (!receiverClass.empty()) {
            const auto klass = classesByName_.find(receiverClass);
            const FunctionInfo* method =
                klass == classesByName_.end()
                    ? nullptr
                    : selectMethod(klass->second, identifier, false);
            if (method && !method->staticMethod &&
                metafunctionSelectorMatches(
                    *method, *selector, receiverClass, true)) {
                return methodMetadataValue(klass->second, *method);
            }
        }

        addDiagnostic(instruction,
                      "function or method metadata is not available: " +
                          identifier);
        return missingValue();
    }

    RuntimeValue propertyNamesBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 ||
            (!isString(arguments.front()) &&
             !isObject(arguments.front()))) {
            addDiagnostic(
                instruction,
                "properties expects a class-name string or object");
            return missingValue();
        }
        const std::string className = canonicalRuntimeMetadataClassName(
            isString(arguments.front())
                ? *runtimeTextScalarUtf8(arguments.front())
                                        : arguments.front().className);
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            if (isBuiltinReflectableClass(className)) {
                return stringColumnCell(
                    builtinMetadataPropertyNames(className));
            }
            addDiagnostic(instruction,
                          "property class is not available: " + className);
            return missingValue();
        }

        std::vector<std::string> names;
        std::set<std::string> seen;
        for (const auto& property : klass->second.propertyOrder) {
            if (!property || property->hidden ||
                property->getAccess.level != MemberAccessLevel::Public ||
                property->getAccess.selectiveClassList ||
                !seen.insert(property->name).second) {
                continue;
            }
            names.push_back(property->name);
        }
        if (isObject(arguments.front()) &&
            arguments.front().sharedFields) {
            registerOwnerDynamicProperties(arguments.front());
            for (const auto& [id, record] : dynamicProperties_) {
                (void)id;
                const auto owner = record.ownerFields.lock();
                const auto descriptorFields =
                    record.descriptorFields.lock();
                if (!owner || !descriptorFields ||
                    owner.get() !=
                        arguments.front().sharedFields.get()) {
                    continue;
                }
                RuntimeValue descriptor = makeRuntimeMetadataObject(
                    RuntimeMetadataKind::DynamicProperty,
                    "dynamic-property/" + std::to_string(record.id));
                descriptor.opaqueId = record.id;
                descriptor.sharedFields = descriptorFields;
                const MemberAccessPolicy getAccess =
                    dynamicPropertyAccessPolicy(descriptor, "GetAccess");
                if (!dynamicPropertyIsValid(descriptor) ||
                    dynamicPropertyLogicalField(descriptor, "Hidden") ||
                    getAccess.level != MemberAccessLevel::Public ||
                    getAccess.selectiveClassList ||
                    !seen.insert(record.name).second) {
                    continue;
                }
                names.push_back(record.name);
            }
        }
        return stringColumnCell(names);
    }

    RuntimeValue methodNamesBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty() || arguments.size() > 2 ||
            (!isString(arguments.front()) &&
             !isObject(arguments.front())) ||
            (arguments.size() == 2 &&
             (!isString(arguments[1]) ||
              *runtimeTextScalarUtf8(arguments[1]) != "-full"))) {
            addDiagnostic(
                instruction,
                "methods expects a class-name string or object and optional "
                "'-full'");
            return missingValue();
        }
        if (arguments.size() == 2) {
            addDiagnostic(
                instruction,
                "methods '-full' descriptions are not implemented yet");
            return missingValue();
        }
        const std::string className = canonicalRuntimeMetadataClassName(
            isString(arguments.front())
                ? *runtimeTextScalarUtf8(arguments.front())
                                        : arguments.front().className);
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            if (isBuiltinReflectableClass(className)) {
                return stringColumnCell(
                    builtinMetadataMethodNames(className));
            }
            addDiagnostic(instruction,
                          "method class is not available: " + className);
            return missingValue();
        }

        std::vector<std::string> names;
        for (const auto& [methodName, method] : klass->second.methods) {
            if (method.hidden || method.propertyAccessor ||
                method.access.level != MemberAccessLevel::Public ||
                method.access.selectiveClassList) {
                continue;
            }
            names.push_back(methodName);
        }
        const auto appendBuiltin = [&](std::string_view method) {
            if (std::find(names.begin(), names.end(), method) ==
                names.end()) {
                names.emplace_back(method);
            }
        };
        if (klass->second.handleClass) {
            for (const std::string_view method :
                 {"addlistener", "findobj", "findprop", "isvalid",
                  "listener", "notify"}) {
                appendBuiltin(method);
            }
            const auto ownDestructor =
                klass->second.declaredMethods.find("delete");
            if (ownDestructor == klass->second.declaredMethods.end() ||
                (ownDestructor->second.classDestructor &&
                 !ownDestructor->second.hidden &&
                 ownDestructor->second.access.level ==
                     MemberAccessLevel::Public &&
                 !ownDestructor->second.access.selectiveClassList)) {
                appendBuiltin("delete");
            }
        }
        if (classDerivesFrom(className,
                             std::string(kDynamicPropsClassName))) {
            appendBuiltin("addprop");
        }
        return stringColumnCell(names);
    }

    RuntimeValue isPropertyBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isObject(arguments[0]) ||
            !isString(arguments[1])) {
            addDiagnostic(instruction,
                          "isprop expects an object and property-name string");
            return missingValue();
        }
        const std::string className =
            canonicalRuntimeMetadataClassName(arguments[0].className);
        const std::string propertyName =
            *runtimeTextScalarUtf8(arguments[1]);
        if (dynamicPropertyDescriptor(arguments[0],
                                      propertyName)) {
            return logicalValue(true);
        }
        const auto klass = classesByName_.find(className);
        if (klass != classesByName_.end()) {
            return logicalValue(
                klass->second.properties.contains(propertyName));
        }
        if (isBuiltinReflectableClass(className)) {
            const auto names = builtinMetadataPropertyNames(className);
            return logicalValue(std::find(names.begin(), names.end(),
                                          propertyName) != names.end());
        }
        return logicalValue(false);
    }

    RuntimeValue isMethodBuiltin(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isObject(arguments[0]) ||
            !isString(arguments[1])) {
            addDiagnostic(instruction,
                          "ismethod expects an object and method-name string");
            return missingValue();
        }
        const std::string className =
            canonicalRuntimeMetadataClassName(arguments[0].className);
        const std::string methodName =
            *runtimeTextScalarUtf8(arguments[1]);
        const bool semanticHandle =
            isRuntimeMetadataObject(arguments[0])
                ? runtimeMetadataIsa(arguments[0], "handle")
                : arguments[0].handleObject;
        if (runtimeMetadataKind(arguments[0]) ==
                RuntimeMetadataKind::DynamicProperty &&
            (methodName == "delete" || methodName == "isvalid")) {
            return logicalValue(true);
        }
        if ((methodName == "addprop" &&
             classDerivesFrom(className,
                              std::string(kDynamicPropsClassName))) ||
            ((methodName == "addlistener" ||
              methodName == "delete" ||
              methodName == "findobj" ||
              methodName == "findprop" ||
              methodName == "isvalid" ||
              methodName == "listener" ||
              methodName == "notify") &&
             semanticHandle)) {
            if (methodName == "delete") {
                const auto klass = classesByName_.find(className);
                if (klass != classesByName_.end()) {
                    const auto declaredDelete =
                        klass->second.declaredMethods.find("delete");
                    if (declaredDelete !=
                        klass->second.declaredMethods.end()) {
                        const FunctionInfo& method =
                            declaredDelete->second;
                        return logicalValue(
                            !method.hidden &&
                            method.access.level ==
                                MemberAccessLevel::Public &&
                            !method.access.selectiveClassList);
                    }
                }
            }
            return logicalValue(true);
        }
        const auto klass = classesByName_.find(className);
        if (klass != classesByName_.end()) {
            const auto method =
                klass->second.methods.find(methodName);
            return logicalValue(
                method != klass->second.methods.end() &&
                !method->second.hidden &&
                !method->second.propertyAccessor &&
                method->second.access.level ==
                    MemberAccessLevel::Public &&
                !method->second.access.selectiveClassList);
        }
        if (isBuiltinReflectableClass(className)) {
            const auto names = builtinMetadataMethodNames(className);
            return logicalValue(std::find(names.begin(), names.end(),
                                          methodName) != names.end());
        }
        return logicalValue(false);
    }

    RuntimeValue eventListenerIsValid(const RuntimeValue& value) const {
        if (isRuntimeClassObject(value) &&
            !isRuntimeScalarObject(value)) {
            std::vector<double> valid;
            valid.reserve(runtimeObjectElementCount(value));
            for (size_t logicalIndex = 0;
                 logicalIndex < runtimeObjectElementCount(value);
                 ++logicalIndex) {
                const auto* element =
                    runtimeObjectLogicalElement(value, logicalIndex);
                if (!element) {
                    return logicalValue(false);
                }
                const RuntimeValue scalar = eventListenerIsValid(*element);
                valid.push_back(scalar.number);
            }
            const auto result = runtimeNumericValueFromLogicalOrder(
                runtimeDimensions(value), std::move(valid),
                RuntimeNumericClass::Logical);
            return result.value_or(logicalValue(false));
        }
        if (runtimeMetadataKind(value) ==
            RuntimeMetadataKind::DynamicProperty) {
            return logicalValue(dynamicPropertyIsValid(value));
        }
        if (!isObject(value) ||
            (value.className != kEventListenerClassName &&
             value.className != kPropertyListenerClassName) ||
            !value.sharedFields) {
            return logicalValue(handleObjectIsValid(value));
        }
        const auto valid = value.sharedFields->find(
            std::string(kListenerValidityField));
        return logicalValue(valid != value.sharedFields->end() &&
                            truthy(valid->second));
    }

    void collectClassDestructors(
        const std::string& className, std::set<std::string>& visited,
        std::vector<const FunctionInfo*>& destructors) const {
        if (!visited.insert(className).second) {
            return;
        }
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            return;
        }
        const auto destructor =
            klass->second.declaredMethods.find("delete");
        if (klass->second.handleClass &&
            destructor != klass->second.declaredMethods.end() &&
            destructor->second.classDestructor) {
            destructors.push_back(&destructor->second);
        }
        for (const auto& superclass : klass->second.superclasses) {
            if (!isBuiltinNonExecutableSuperclass(superclass)) {
                collectClassDestructors(superclass, visited,
                                        destructors);
            }
        }
    }

    std::vector<size_t> destructionEventListeners(
        const RuntimeValue& source) const {
        std::vector<size_t> listeners;
        if (!source.sharedFields) {
            return listeners;
        }
        for (const auto& [id, record] : eventListeners_) {
            const auto listenerSource = record.sourceFields.lock();
            if (!record.propertyListener && listenerSource &&
                listenerSource.get() == source.sharedFields.get() &&
                record.eventName == kObjectBeingDestroyedEventName) {
                listeners.push_back(id);
            }
        }
        return listeners;
    }

    void cleanupDestroyedHandleState(const RuntimeValue& source) {
        if (!source.sharedFields) {
            return;
        }
        registerOwnerDynamicProperties(source);
        std::set<size_t> dynamicPropertyIds;
        for (auto property = dynamicProperties_.begin();
             property != dynamicProperties_.end();) {
            const auto owner = property->second.ownerFields.lock();
            if (!owner || owner.get() != source.sharedFields.get()) {
                ++property;
                continue;
            }
            dynamicPropertyIds.insert(property->first);
            if (const auto descriptor =
                    property->second.descriptorFields.lock()) {
                (*descriptor)[std::string(kListenerValidityField)] =
                    logicalValue(false);
            }
            source.sharedFields->erase(dynamicPropertyDescriptorKey(
                property->second.name));
            source.sharedFields->erase(
                dynamicPropertyValueKey(property->first));
            property = dynamicProperties_.erase(property);
        }

        for (auto& [id, listener] : eventListeners_) {
            (void)id;
            const auto listenerSource = listener.sourceFields.lock();
            if (!listenerSource ||
                listenerSource.get() != source.sharedFields.get()) {
                continue;
            }
            const bool invalidatedDynamicProperty =
                listener.propertyListener &&
                dynamicPropertyIds.contains(
                    listener.dynamicPropertyId);
            if (!listener.coupled && !invalidatedDynamicProperty) {
                continue;
            }
            if (const auto fields = listener.listenerFields.lock()) {
                (*fields)[std::string(kListenerValidityField)] =
                    logicalValue(false);
            }
            listener.retainedListenerFields.reset();
        }
    }

    void destroyHandleObject(const BytecodeInstruction& instruction,
                             const RuntimeValue& source) {
        if (!isObject(source) || !source.handleObject ||
            !source.sharedFields) {
            addDiagnostic(instruction,
                          "delete expects a scalar handle object");
            return;
        }
        if (!handleObjectIsValid(source)) {
            return;
        }

        (*source.sharedFields)[std::string(kHandleValidityField)] =
            logicalValue(false);
        destroyingHandleFields_.insert(source.sharedFields.get());

        std::vector<Diagnostic> lifecycleDiagnostics;
        const auto runLifecycleStage = [&](const auto& action) {
            const size_t diagnosticBase = diagnostics_.size();
            action();
            lifecycleDiagnostics.insert(
                lifecycleDiagnostics.end(),
                diagnostics_.begin() +
                    static_cast<std::ptrdiff_t>(diagnosticBase),
                diagnostics_.end());
            diagnostics_.resize(diagnosticBase);
        };

        RuntimeValue eventData = objectValue(
            std::string(kEventDataClassName),
            {{"EventName", characterValue(std::string(
                               kObjectBeingDestroyedEventName))},
             {"Source", source}},
            true);
        for (const size_t id : destructionEventListeners(source)) {
            runLifecycleStage([&] {
                (void)invokeEventListenerCallback(
                    instruction, id, {source, eventData});
            });
        }

        std::set<std::string> visited;
        std::vector<const FunctionInfo*> destructors;
        collectClassDestructors(source.className, visited,
                                destructors);
        for (const FunctionInfo* destructor : destructors) {
            runLifecycleStage([&] {
                (void)callFunctionInfo(
                    instruction,
                    destructor->declaringClass + ".delete",
                    *destructor, {source}, 0, std::nullopt, nullptr,
                    false);
            });
        }

        cleanupDestroyedHandleState(source);
        destroyingHandleFields_.erase(source.sharedFields.get());
        diagnostics_.insert(diagnostics_.end(),
                            lifecycleDiagnostics.begin(),
                            lifecycleDiagnostics.end());
    }

    void deleteRuntimeObject(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 || !isObject(arguments[0])) {
            addDiagnostic(instruction,
                          "delete expects one handle object");
            return;
        }
        const RuntimeValue& source = arguments.front();
        const auto klass = classesByName_.find(source.className);
        const FunctionInfo* method =
            klass == classesByName_.end()
                ? nullptr
                : selectMethod(klass->second, "delete");
        const bool classDestructor =
            method && method->classDestructor &&
            klass != classesByName_.end() &&
            klass->second.handleClass;
        if (method && !classDestructor) {
            if (!hasMemberAccess(method->access,
                                 method->declaringClass)) {
                addDiagnostic(instruction,
                              "method access is denied: " +
                                  method->declaringClass + ".delete");
                return;
            }
            (void)callFunctionInfo(
                instruction, method->declaringClass + ".delete",
                *method, {source}, 0, std::nullopt, nullptr, false);
            return;
        }
        if (isRuntimeClassObject(source) &&
            !isRuntimeScalarObject(source)) {
            if (!source.handleObject) {
                addDiagnostic(instruction,
                              "delete expects handle objects");
                return;
            }
            if (classDestructor &&
                !hasMemberAccess(method->access,
                                 method->declaringClass)) {
                addDiagnostic(instruction,
                              "method access is denied: " +
                                  method->declaringClass + ".delete");
                return;
            }
            for (size_t logicalIndex = 0;
                 logicalIndex < runtimeObjectElementCount(source);
                 ++logicalIndex) {
                const auto* element =
                    runtimeObjectLogicalElement(source, logicalIndex);
                if (!element) {
                    addDiagnostic(
                        instruction,
                        "delete could not map an object array element");
                    return;
                }
                if (element->className == kEventListenerClassName ||
                    element->className == kPropertyListenerClassName) {
                    deleteEventListener(instruction, {*element});
                } else {
                    destroyHandleObject(instruction, *element);
                }
            }
            return;
        }
        if (!source.handleObject || !source.sharedFields) {
            addDiagnostic(instruction,
                          "delete expects a scalar handle object");
            return;
        }
        if (classDestructor &&
            !hasMemberAccess(method->access,
                             method->declaringClass)) {
            addDiagnostic(instruction,
                          "method access is denied: " +
                              method->declaringClass + ".delete");
            return;
        }
        destroyHandleObject(instruction, source);
    }

    void deleteEventListener(const BytecodeInstruction& instruction,
                             const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 || !isObject(arguments[0]) ||
            (arguments[0].className != kEventListenerClassName &&
             arguments[0].className != kPropertyListenerClassName) ||
            !arguments[0].sharedFields) {
            addDiagnostic(instruction,
                          "delete currently expects an event listener");
            return;
        }
        (*arguments[0].sharedFields)[std::string(kListenerValidityField)] =
            numberValue(0.0);
        if (const auto listener =
                eventListeners_.find(arguments[0].opaqueId);
            listener != eventListeners_.end()) {
            listener->second.retainedListenerFields.reset();
        }
    }

    void notifyEvent(const BytecodeInstruction& instruction,
                     const std::vector<RuntimeValue>& arguments) {
        if ((arguments.size() != 2 && arguments.size() != 3) ||
            !isObject(arguments[0]) || !arguments[0].handleObject ||
            !arguments[0].sharedFields || !isString(arguments[1])) {
            addDiagnostic(
                instruction,
                "notify expects a handle object, event-name string, and "
                "optional event data object");
            return;
        }

        const RuntimeValue& source = arguments[0];
        if (!requireUsableHandleObject(instruction, source)) {
            return;
        }
        const std::string eventName =
            *runtimeTextScalarUtf8(arguments[1]);
        const EventInfo* event = selectEvent(source, eventName);
        if (!event) {
            addDiagnostic(instruction,
                          "event is not available: " + source.className +
                              "." + eventName);
            return;
        }
        if (!hasMemberAccess(event->notifyAccess,
                             event->declaringClass)) {
            addDiagnostic(instruction,
                          "event notify access is denied: " +
                              event->declaringClass + "." + eventName);
            return;
        }

        RuntimeValue eventData;
        if (arguments.size() == 3) {
            if (!isObject(arguments[2]) ||
                (arguments[2].className != kEventDataClassName &&
                 !classDerivesFrom(arguments[2].className,
                                   std::string(kEventDataClassName)))) {
                addDiagnostic(instruction,
                              "notify event data must derive from "
                              "event.EventData");
                return;
            }
            eventData = arguments[2];
            if (!eventData.handleObject || !eventData.sharedFields) {
                addDiagnostic(instruction,
                              "notify event data must be a handle object");
                return;
            }
            (*eventData.sharedFields)["Source"] = source;
            (*eventData.sharedFields)["EventName"] =
                characterValue(eventName);
        } else {
            eventData = objectValue(
                std::string(kEventDataClassName),
                {{"EventName", characterValue(eventName)},
                 {"Source", source}},
                true);
        }

        std::vector<size_t> listeners;
        for (const auto& [id, record] : eventListeners_) {
            const auto listenerSource = record.sourceFields.lock();
            if (!record.propertyListener && listenerSource &&
                listenerSource.get() == source.sharedFields.get() &&
                record.eventName == eventName) {
                listeners.push_back(id);
            }
        }

        for (const size_t id : listeners) {
            if (!invokeEventListenerCallback(
                    instruction, id, {source, eventData})) {
                return;
            }
        }
    }

    RuntimeSourceCallableInvocationResult invokeSourceCallable(
        const BytecodeInstruction& instruction,
        const RuntimeValue& callable,
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount, SourceSpan callbackSpan,
        RuntimeWorkspace* ownerWorkspace) {
        RuntimeSourceCallableInvocationResult result;
        if (requestedOutputCount >
            static_cast<size_t>(std::numeric_limits<int>::max())) {
            result.diagnostics.push_back(Diagnostic{
                callbackSpan,
                "dynamic parent callable requested too many outputs",
                "MParser:InvalidDynamicCall"});
            return result;
        }

        const size_t diagnosticStart = diagnostics_.size();
        const size_t warningStart = warnings_.size();
        auto savedPendingException = std::move(pendingException_);
        auto savedOutputSink = std::move(runtimeOutputSink_);
        pendingException_.reset();
        runtimeOutputSink_ = [&result](const RuntimeOutputEvent& event) {
            result.outputEvents.push_back(event);
            return true;
        };

        if (ownerWorkspace) {
            sourceCallerOverrides_.push_back(SourceCallerOverride{
                frames_.size(), ownerWorkspace});
        }
        result.outputs = callFunctionHandle(
            instruction, callable, arguments,
            static_cast<int>(requestedOutputCount));
        if (ownerWorkspace) {
            sourceCallerOverrides_.pop_back();
        }

        result.diagnostics.reserve(
            diagnostics_.size() - diagnosticStart +
            warnings_.size() - warningStart);
        std::move(
            warnings_.begin() +
                static_cast<std::ptrdiff_t>(warningStart),
            warnings_.end(),
            std::back_inserter(result.diagnostics));
        std::move(
            diagnostics_.begin() +
                static_cast<std::ptrdiff_t>(diagnosticStart),
            diagnostics_.end(),
            std::back_inserter(result.diagnostics));
        warnings_.resize(warningStart);
        diagnostics_.resize(diagnosticStart);
        pendingException_ = std::move(savedPendingException);
        runtimeOutputSink_ = std::move(savedOutputSink);

        for (auto& diagnostic : result.diagnostics) {
            diagnostic.span = callbackSpan;
        }
        result.succeeded = !std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            isErrorDiagnostic);
        if (!result.succeeded) {
            result.outputs.clear();
        }
        return result;
    }

    std::vector<RuntimeValue> callBuiltinOutputs(
        const BytecodeInstruction& instruction, const std::string& name,
        const std::vector<RuntimeValue>& arguments, int requestedCount,
        std::optional<size_t> callerOutputCount = std::nullopt) {
        if (requestedCount < 0) {
            addDiagnostic(instruction,
                          "bytecode call result count cannot be negative");
            return {};
        }

        const bool fileDeleteDispatch =
            name == "delete" &&
            (arguments.empty() ||
             std::all_of(arguments.begin(), arguments.end(),
                         [](const RuntimeValue& argument) {
                             return isRuntimeCharacterVector(argument) ||
                                    isRuntimeStringArray(argument);
                         }));
        if (const BuiltinDescriptor* descriptor =
                builtinRegistry().find(name);
            descriptor && (name != "delete" || fileDeleteDispatch) &&
            descriptor->implementation !=
                BuiltinImplementationKind::Intrinsic) {
            RuntimeObjectArrayPolicy objectPolicy =
                objectArrayPolicy(instruction);
            BuiltinWorkspaceAccess workspace;
            workspace.variables = &currentFrame();
            workspace.resolveVariables = [this](BuiltinWorkspaceScope scope) {
                return workspaceFor(scope);
            };
            workspace.clearVariables = [this] {
                clearSourceStorage(&currentFrame(), {});
                if (inheritedSourceStorageClearer_ &&
                    inheritedSourceStorageWorkspace_) {
                    inheritedSourceStorageClearer_(
                        inheritedSourceStorageWorkspace_, {});
                }
                currentFrame().clear();
            };
            workspace.eraseVariable = [this](std::string_view variable) {
                clearSourceStorage(&currentFrame(), variable);
                if (inheritedSourceStorageClearer_ &&
                    inheritedSourceStorageWorkspace_) {
                    inheritedSourceStorageClearer_(
                        inheritedSourceStorageWorkspace_, variable);
                }
                return currentFrame().erase(std::string(variable)) != 0;
            };
            workspace.functionExists = [this](std::string_view name) {
                return functionsByName_.contains(std::string(name)) ||
                       functionForPublicMetadataIdentifier(name) != nullptr;
            };
            workspace.classExists = [this](std::string_view name) {
                return classesByName_.contains(std::string(name));
            };
            BuiltinDisplayFormatAccess displayFormat;
            displayFormat.current = [this] {
                return sessionState_->displayFormat();
            };
            displayFormat.replace = [this](RuntimeDisplayFormat value) {
                return sessionState_->replaceDisplayFormat(value);
            };
            BuiltinCallContext context;
            context.workspace = &workspace;
            context.warningContext =
                sessionState_->warningContext().get();
            context.objectArrayPolicy = &objectPolicy;
            context.executionControl =
                executionControl_.get();
            context.outputSink = &runtimeOutputSink_;
            context.systemContext =
                sessionState_->systemContext().get();
            context.displayFormat = &displayFormat;
            context.registry = &builtinRegistry();
            if (hasBuiltinContextPermission(
                    descriptor->contextPermissions,
                    BuiltinContextPermission::DynamicCall)) {
                context.dynamicInvoker =
                    [this, &instruction](
                    const RuntimeValue& callable,
                    const std::vector<RuntimeValue>& callbackArguments,
                    size_t callbackOutputCount, SourceSpan callbackSpan) {
                    if (callbackOutputCount >
                        static_cast<size_t>(
                            std::numeric_limits<int>::max())) {
                        return BuiltinResult::failure(
                            callbackSpan,
                            "dynamic builtin callback requested too many "
                            "outputs",
                            "MParser:InvalidDynamicCall");
                    }

                    const size_t diagnosticStart = diagnostics_.size();
                    const size_t warningStart = warnings_.size();
                    auto savedPendingException =
                        std::move(pendingException_);
                    pendingException_.reset();

                    std::vector<RuntimeValue> outputs;
                    std::optional<RuntimeValue> handle;
                    if (isFunctionHandle(callable)) {
                        handle = callable;
                    } else if (const auto text =
                                   runtimeTextScalarUtf8(callable)) {
                        handle = functionHandleFromText(instruction,
                                                        *text);
                    } else {
                        addDiagnostic(
                            instruction,
                            "dynamic builtin callback expects a function "
                            "handle or function name string");
                    }
                    if (handle) {
                        outputs = callFunctionHandle(
                            instruction, *handle, callbackArguments,
                            static_cast<int>(callbackOutputCount));
                    }

                    std::vector<Diagnostic> nestedDiagnostics;
                    nestedDiagnostics.reserve(
                        diagnostics_.size() - diagnosticStart +
                        warnings_.size() - warningStart);
                    std::move(warnings_.begin() +
                                  static_cast<std::ptrdiff_t>(warningStart),
                              warnings_.end(),
                              std::back_inserter(nestedDiagnostics));
                    std::move(
                        diagnostics_.begin() +
                            static_cast<std::ptrdiff_t>(diagnosticStart),
                        diagnostics_.end(),
                        std::back_inserter(nestedDiagnostics));
                    warnings_.resize(warningStart);
                    diagnostics_.resize(diagnosticStart);
                    pendingException_ =
                        std::move(savedPendingException);

                    const bool failed = std::any_of(
                        nestedDiagnostics.begin(),
                        nestedDiagnostics.end(), isErrorDiagnostic);
                    return failed
                               ? BuiltinResult{
                                     false, {},
                                     std::move(nestedDiagnostics)}
                               : BuiltinResult::success(
                                     std::move(outputs),
                                     std::move(nestedDiagnostics));
                };
            }
            if (hasBuiltinContextPermission(
                    descriptor->contextPermissions,
                    BuiltinContextPermission::SourceEvaluation)) {
                context.sourceEvaluator =
                    [this, &instruction](
                        const BuiltinSourceEvaluationRequest& request) {
                    RuntimeWorkspace* target = workspaceFor(request.workspace);
                    if (!target) {
                        BuiltinSourceEvaluationResult result;
                        result.diagnostics.push_back(Diagnostic{
                            request.span,
                            "dynamic source workspace is unavailable",
                            "MParser:MissingBuiltinContext"});
                        return result;
                    }
                    RuntimeSourceEvaluationOptions options;
                    options.builtinRegistry =
                        semantic_ && semantic_->builtinRegistry
                            ? semantic_->builtinRegistry
                            : defaultBuiltinRegistry();
                    options.sessionState = sessionState_;
                    options.executionControl = executionControl_;
                    options.outputSink = runtimeOutputSink_;
                    options.typedRegionBackend = typedRegionBackend_;
                    options.enableTypedRegions = typedRegionsEnabled_;
                    options.inheritedWorkspaceFrames =
                        workspaceAncestorsFor(request.workspace);
                    if (!inheritedSourceCallableInvoker_) {
                        options.inheritedCallableScopes =
                            sourceCallableScopes();
                        for (const auto& scope :
                             options.inheritedCallableScopes) {
                            if (scope.workspace == target) {
                                options.inheritedCallables =
                                    scope.callables;
                                break;
                            }
                        }
                        options.inheritedCallableWorkspace = target;
                        options.inheritedCallableInvoker =
                            [this, &instruction](
                                const RuntimeValue& callable,
                                const std::vector<RuntimeValue>& arguments,
                                size_t requestedOutputCount,
                                SourceSpan callbackSpan,
                                RuntimeWorkspace* ownerWorkspace) {
                                return invokeSourceCallable(
                                    instruction, callable, arguments,
                                    requestedOutputCount, callbackSpan,
                                    ownerWorkspace);
                            };
                    } else {
                        const auto* callableScope =
                            inheritedSourceCallableScope(target);
                        if (target == &currentFrame()) {
                            options.inheritedCallables =
                                inheritedSourceCallables_;
                            options.inheritedCallableWorkspace =
                                inheritedSourceCallableWorkspace_;
                        } else if (callableScope) {
                            options.inheritedCallables =
                                callableScope->callables;
                            options.inheritedCallableWorkspace =
                                callableScope->workspace;
                        } else {
                            options.inheritedCallables =
                                inheritedSourceCallables_;
                            options.inheritedCallableWorkspace =
                                inheritedSourceCallableWorkspace_;
                        }
                        options.inheritedCallableScopes =
                            inheritedSourceCallableScopes_;
                        options.inheritedCallableInvoker =
                            inheritedSourceCallableInvoker_;
                    }
                    if (!inheritedSourceStorageResolver_ ||
                        !inheritedSourceStorageDeclarer_) {
                        options.inheritedStorageResolver =
                            [this](RuntimeWorkspace* ownerWorkspace,
                                   std::string_view name) {
                                return sourceStorageBinding(
                                    ownerWorkspace, name);
                            };
                        options.inheritedStorageDeclarer =
                            [this](RuntimeWorkspace* ownerWorkspace,
                                   RuntimeSourceStorageKind kind,
                                   std::string_view name,
                                   const RuntimeValue* localValue,
                                   SourceSpan span) {
                                return declareSourceStorage(
                                    ownerWorkspace, kind, name,
                                    localValue, span);
                            };
                        options.inheritedStorageClearer =
                            [this](RuntimeWorkspace* ownerWorkspace,
                                   std::string_view name) {
                                clearSourceStorage(ownerWorkspace, name);
                            };
                        options.inheritedStorageWorkspace = target;
                    } else {
                        options.inheritedStorageResolver =
                            inheritedSourceStorageResolver_;
                        options.inheritedStorageDeclarer =
                            inheritedSourceStorageDeclarer_;
                        options.inheritedStorageClearer =
                            inheritedSourceStorageClearer_;
                        options.inheritedStorageWorkspace =
                            target == &currentFrame()
                                ? inheritedSourceStorageWorkspace_
                                : target;
                    }
                    return evaluateRuntimeSource(request, *target, options);
                };
            }
            BuiltinResult result = builtinRegistry().invoke(
                name,
                BuiltinCall{
                    arguments,
                    static_cast<size_t>(requestedCount),
                    instruction.span,
                    &context,
                    callerOutputCount
                        ? callerOutputCount
                        : instruction.implicitExpressionOutput
                        ? std::optional<size_t>{0}
                        : std::nullopt});
            appendBuiltinDiagnostics(
                instruction, std::move(result.diagnostics));
            if (!result.succeeded) {
                return missingOutputs(requestedCount);
            }
            return std::move(result.outputs);
        }

        if (name == "feval") {
            if (arguments.empty()) {
                addDiagnostic(
                    instruction,
                    "feval expects a function handle or function name string");
                return missingOutputs(requestedCount);
            }

            RuntimeValue handle;
            if (isFunctionHandle(arguments.front())) {
                handle = arguments.front();
            } else if (isString(arguments.front())) {
                const auto resolved = functionHandleFromText(
                    instruction,
                    *runtimeTextScalarUtf8(arguments.front()));
                if (!resolved) {
                    return missingOutputs(requestedCount);
                }
                handle = *resolved;
            } else {
                addDiagnostic(
                    instruction,
                    "feval expects a function handle or function name string");
                return missingOutputs(requestedCount);
            }

            return callFunctionHandle(
                instruction, handle,
                std::vector<RuntimeValue>(arguments.begin() + 1,
                                          arguments.end()),
                requestedCount);
        }

        if (name == "str2func" || name == "func2str" ||
            name == "functions") {
            if (requestedCount > 1) {
                addDiagnostic(instruction,
                              name + " supports at most one output");
                return missingOutputs(requestedCount);
            }
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              name + " expects exactly one argument");
                return missingOutputs(requestedCount);
            }

            RuntimeValue result;
            if (name == "str2func") {
                if (!isString(arguments.front())) {
                    addDiagnostic(instruction,
                                  "str2func expects a function name string");
                    return missingOutputs(requestedCount);
                }
                const auto resolved = functionHandleFromText(
                    instruction,
                    *runtimeTextScalarUtf8(arguments.front()));
                if (!resolved) {
                    return missingOutputs(requestedCount);
                }
                result = *resolved;
            } else {
                if (!isFunctionHandle(arguments.front())) {
                    addDiagnostic(instruction,
                                  name + " expects a function handle");
                    return missingOutputs(requestedCount);
                }
                result = name == "func2str"
                             ? characterValue(runtimeFunctionHandleText(
                                   arguments.front()))
                             : runtimeFunctionHandleMetadata(
                                   arguments.front());
            }
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{std::move(result)};
        }

        if (name == "MException" && requestedCount > 1) {
            addDiagnostic(instruction,
                          "MException supports at most one output",
                          "MParser:InvalidException");
            return missingOutputs(requestedCount);
        }

        if (name == "addCause" || name == "getReport") {
            if (requestedCount > 1) {
                addDiagnostic(instruction,
                              name + " supports at most one output",
                              "MParser:InvalidException");
                return missingOutputs(requestedCount);
            }
            auto result = name == "addCause"
                              ? runtimeAddExceptionCause(arguments)
                              : runtimeGetExceptionReport(arguments);
            if (!result.succeeded) {
                addDiagnostic(instruction, std::move(result.error),
                              "MParser:InvalidException");
                return missingOutputs(requestedCount);
            }
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{
                             std::move(result.value)};
        }

        if (name == "addCorrection") {
            addDiagnostic(
                instruction,
                "MException correction objects are outside the supported "
                "exception subset",
                "MParser:UnsupportedExceptionCorrection");
            return missingOutputs(requestedCount);
        }

        if (name == "assert" && requestedCount != 0) {
            addDiagnostic(instruction, "assert does not produce outputs",
                          "MParser:InvalidAssertion");
            return missingOutputs(requestedCount);
        }

        if (name == "isValidValue" ||
            name == "validateValue") {
            return callPropertyValidationMethod(
                instruction, name, arguments, requestedCount);
        }

        if (name == "findobj") {
            if (requestedCount > 1) {
                addDiagnostic(
                    instruction,
                    "findobj supports at most one output");
                return missingOutputs(requestedCount);
            }
            RuntimeValue result =
                findObjectsBuiltin(instruction, arguments);
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{
                             std::move(result)};
        }

        if (name == "addprop" || name == "findprop") {
            if (requestedCount > 1) {
                addDiagnostic(instruction,
                              name + " supports at most one output");
                return missingOutputs(requestedCount);
            }
            RuntimeValue result =
                name == "addprop"
                    ? addDynamicPropertyBuiltin(instruction, arguments)
                    : findPropertyBuiltin(instruction, arguments);
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{std::move(result)};
        }

        if (name == "addlistener" || name == "listener" ||
            name == "event.proplistener") {
            if (requestedCount > 1) {
                addDiagnostic(instruction,
                              name + " supports at most one output");
                return missingOutputs(requestedCount);
            }
            RuntimeValue listener;
            if (name == "event.proplistener") {
                listener = createPropertyListener(
                    instruction, arguments, false, true);
            } else if (arguments.size() == 4) {
                listener = createPropertyListener(
                    instruction, arguments, name == "addlistener");
            } else {
                listener = createEventListener(
                    instruction, arguments, name == "addlistener");
            }
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{std::move(listener)};
        }
        if (name == "notify") {
            if (requestedCount != 0) {
                addDiagnostic(instruction,
                              "notify does not produce outputs");
                return missingOutputs(requestedCount);
            }
            notifyEvent(instruction, arguments);
            return {};
        }
        if (name == "delete") {
            if (requestedCount != 0) {
                addDiagnostic(instruction,
                              "delete does not produce outputs");
                return missingOutputs(requestedCount);
            }
            if (arguments.size() == 1 &&
                isDynamicPropertyDescriptor(arguments.front())) {
                deleteDynamicProperty(instruction, arguments.front());
            } else if (
                arguments.size() == 1 &&
                isObject(arguments.front()) &&
                isRuntimeScalarObject(arguments.front()) &&
                (arguments.front().className ==
                     kEventListenerClassName ||
                 arguments.front().className ==
                     kPropertyListenerClassName)) {
                deleteEventListener(instruction, arguments);
            } else {
                deleteRuntimeObject(instruction, arguments);
            }
            return {};
        }

        if (name == "enumeration") {
            return enumerationBuiltinOutputs(instruction, arguments,
                                             requestedCount);
        }

        if (requestedCount == 0) {
            (void)callBuiltin(instruction, name, arguments);
            return {};
        }

        if (requestedCount != 1) {
            addDiagnostic(instruction,
                          "bytecode builtin does not support multiple "
                          "outputs yet: " +
                              name);
            return missingOutputs(requestedCount);
        }

        return {callBuiltin(instruction, name, arguments)};
    }

    std::vector<RuntimeValue> callLocalFunction(
        const BytecodeInstruction& instruction, const std::string& name,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        if (requestedCount < 0) {
            addDiagnostic(instruction,
                          "bytecode call result count cannot be negative");
            return {};
        }

        const auto function = resolveLocalFunction(name);
        if (!function) {
            if (const auto* inherited =
                    inheritedSourceCallable(name)) {
                return callInheritedSourceCallable(
                    instruction, inherited->callable, arguments,
                    requestedCount);
            }
            addDiagnostic(instruction,
                          "local function is not available: " + name);
            return missingOutputs(requestedCount);
        }

        return callFunctionInfo(instruction, function->key, *function->info,
                                arguments, requestedCount, std::nullopt,
                                nullptr, false);
    }

    BytecodeInstruction propertyInstruction(
        const PropertyInfo& property) const {
        BytecodeInstruction instruction;
        instruction.operand = property.declaringClass + "." + property.name;
        instruction.span = property.span;
        return instruction;
    }

    BytecodeInstruction enumerationInstruction(
        const EnumerationMemberInfo& member) const {
        BytecodeInstruction instruction;
        instruction.operand = member.declaringClass + "." + member.name;
        instruction.span = member.span;
        instruction.operandCount = member.argumentCount;
        return instruction;
    }

    std::optional<std::vector<RuntimeValue>>
    evaluateEnumerationArguments(EnumerationMemberInfo& member) {
        const BytecodeInstruction instruction =
            enumerationInstruction(member);
        if (!member.hasInitializerRange) {
            addDiagnostic(
                instruction,
                "enumeration member has no initializer bytecode range");
            return std::nullopt;
        }

        auto savedStack = std::move(stack_);
        auto savedForLoops = std::move(forLoopStack_);
        auto savedIndexContexts = std::move(indexContextStack_);
        auto savedSwitchContexts = std::move(switchContextStack_);
        auto savedTryContexts = std::move(tryContextStack_);
        const bool savedReturnRequested = returnRequested_;
        stack_.clear();
        forLoopStack_.clear();
        indexContextStack_.clear();
        switchContextStack_.clear();
        tryContextStack_.clear();
        returnRequested_ = false;
        frames_.push_back(makeRuntimeInitializerFrame(
            member.declaringClass + "." + member.name, member.span));
        activeClassFunctions_.push_back(ActiveClassFunction{
            member.declaringClass,
            "<enumeration-member:" + member.name + ">", {}, nullptr});

        const size_t diagnosticCount = diagnostics_.size();
        executeFunctionBody(member.initializerEntry, member.initializerEnd);
        std::optional<std::vector<RuntimeValue>> arguments;
        if (diagnostics_.size() == diagnosticCount) {
            if (stack_.size() != static_cast<size_t>(member.argumentCount)) {
                addDiagnostic(
                    instruction,
                    "enumeration member initializer produced an unexpected "
                    "number of values");
            } else {
                arguments = popRuntimeValues(
                    instruction, member.argumentCount,
                    "enumeration member constructor argument");
            }
        }

        activeClassFunctions_.pop_back();
        frames_.pop_back();
        stack_ = std::move(savedStack);
        forLoopStack_ = std::move(savedForLoops);
        indexContextStack_ = std::move(savedIndexContexts);
        switchContextStack_ = std::move(savedSwitchContexts);
        tryContextStack_ = std::move(savedTryContexts);
        returnRequested_ = savedReturnRequested;
        return arguments;
    }

    std::optional<RuntimeValue> enumerationMemberValue(
        const BytecodeInstruction& instruction,
        const std::string& className, const std::string& memberName) {
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end() ||
            !klass->second.enumerationClass) {
            addDiagnostic(instruction,
                          "enumeration class is not available: " +
                              className);
            return std::nullopt;
        }
        const auto found =
            klass->second.declaredEnumerationMembers.find(memberName);
        if (found == klass->second.declaredEnumerationMembers.end()) {
            addDiagnostic(instruction,
                          "enumeration member is not available: " +
                              className + "." + memberName);
            return std::nullopt;
        }

        auto& member = found->second;
        if (member.evaluated) {
            return member.value;
        }
        if (member.evaluationActive) {
            addDiagnostic(instruction,
                          "recursive enumeration member evaluation: " +
                              className + "." + memberName);
            return std::nullopt;
        }

        member.evaluationActive = true;
        const auto arguments = evaluateEnumerationArguments(member);
        if (!arguments) {
            member.evaluationActive = false;
            return std::nullopt;
        }
        const size_t diagnosticCount = diagnostics_.size();
        auto outputs = callClassConstructor(
            enumerationInstruction(member), className, *arguments, 1, true);
        if (outputs.empty() || diagnostics_.size() != diagnosticCount ||
            !isObject(outputs.front())) {
            member.evaluationActive = false;
            return std::nullopt;
        }
        outputs.front().enumerationMemberName = memberName;
        member.value = outputs.front();
        member.evaluated = true;
        member.evaluationActive = false;
        return member.value;
    }

    std::optional<std::string> enumerationClassName(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1) {
            addDiagnostic(
                instruction,
                "enumeration expects one class-name or enumeration value");
            return std::nullopt;
        }

        std::string className;
        if (isString(arguments.front())) {
            className = *runtimeTextScalarUtf8(arguments.front());
        } else if (isObject(arguments.front()) &&
                   !arguments.front().enumerationMemberName.empty()) {
            className = arguments.front().className;
        } else {
            addDiagnostic(
                instruction,
                "enumeration expects a class-name string or enumeration "
                "value");
            return std::nullopt;
        }
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end() ||
            !klass->second.enumerationClass) {
            addDiagnostic(instruction,
                          "enumeration class is not available: " +
                              className);
            return std::nullopt;
        }
        return className;
    }

    std::vector<RuntimeValue> enumerationBuiltinOutputs(
        const BytecodeInstruction& instruction,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        if (requestedCount < 0 || requestedCount > 2) {
            addDiagnostic(
                instruction,
                "enumeration supports at most two outputs");
            return missingOutputs(requestedCount);
        }
        const auto className = enumerationClassName(instruction, arguments);
        if (!className) {
            return missingOutputs(requestedCount);
        }

        auto& klass = classesByName_.at(*className);
        std::vector<RuntimeValue> values;
        std::vector<RuntimeValue> names;
        for (const auto& memberName : klass.declaredEnumerationOrder) {
            const auto& member =
                klass.declaredEnumerationMembers.at(memberName);
            if (member.hidden) {
                continue;
            }
            const auto value = enumerationMemberValue(
                instruction, *className, memberName);
            if (!value) {
                return missingOutputs(requestedCount);
            }
            values.push_back(*value);
            names.push_back(characterValue(memberName));
        }

        if (requestedCount == 0) {
            return {};
        }
        const size_t valueCount = values.size();
        const size_t nameCount = names.size();
        std::vector<RuntimeValue> outputs;
        auto objectArray = runtimeMakeObjectArrayFromLogicalOrder(
            std::move(values), {valueCount, 1}, *className,
            klass.handleClass, objectArrayPolicy(instruction), *className);
        if (!objectArray.succeeded) {
            addDiagnostic(instruction,
                          "enumeration " + std::move(objectArray.error));
            return missingOutputs(requestedCount);
        }
        outputs.push_back(std::move(objectArray.value));
        if (requestedCount == 2) {
            outputs.push_back(cellValueForShape(nameCount, 1,
                                                std::move(names)));
        }
        return outputs;
    }

    std::string propertyDisplayName(const PropertyInfo& property) const {
        return property.declaringClass.empty()
                   ? property.name
                   : property.declaringClass + "." + property.name;
    }

    std::optional<size_t> propertyDimension(
        const PropertyDimensionSpec& dimension) const {
        if (dimension.text == ":") {
            return std::nullopt;
        }
        const auto value = parseRealNumber(dimension.text);
        if (!value) {
            return std::nullopt;
        }
        return checkedRuntimeNonnegativeInteger(*value);
    }

    bool propertyValidationError(const BytecodeInstruction& instruction,
                                 const PropertyInfo& property,
                                 std::string message) {
        addDiagnostic(instruction, "property validation failed for " +
                                       propertyDisplayName(property) + ": " +
                                       std::move(message));
        return false;
    }

    std::optional<RuntimeValue> validatePropertyValue(
        const BytecodeInstruction& instruction, const PropertyInfo& property,
        RuntimeValue value) {
        RuntimeArgumentValidationOptions options;
        options.classAvailable = [this](const std::string& name) {
            return classesByName_.contains(name);
        };
        options.objectIsA = [this](const std::string& actual,
                                  const std::string& expected) {
            return classesByName_.contains(expected) &&
                   classDerivesFrom(actual, expected);
        };
        auto validation = validateRuntimeArgument(
            std::move(value), property.spec, options);
        if (!validation.succeeded) {
            propertyValidationError(
                instruction, property, std::move(validation.error));
            return std::nullopt;
        }
        return std::move(validation.value);
    }

    std::vector<RuntimeValue> callPropertyValidationMethod(
        const BytecodeInstruction& instruction, std::string_view name,
        const std::vector<RuntimeValue>& arguments,
        int requestedCount) {
        if (arguments.size() != 2 ||
            runtimeMetadataKind(arguments.front()) !=
                RuntimeMetadataKind::PropertyValidation ||
            !isRuntimeMetadataScalar(arguments.front())) {
            addDiagnostic(
                instruction,
                std::string(name) +
                    " expects a scalar PropertyValidation object and "
                    "one candidate value");
            return missingOutputs(requestedCount);
        }
        const PropertyInfoPtr property =
            propertyForMetadata(arguments.front().text);
        if (!property) {
            addDiagnostic(
                instruction,
                "metadata property validation descriptor is not available");
            return missingOutputs(requestedCount);
        }

        if (name == "isValidValue") {
            if (requestedCount > 1) {
                addDiagnostic(
                    instruction,
                    "isValidValue supports at most one output");
                return missingOutputs(requestedCount);
            }
            const size_t diagnosticCount = diagnostics_.size();
            const auto pendingException = pendingException_;
            const bool valid =
                validatePropertyValue(
                    instruction, *property, arguments[1])
                    .has_value();
            diagnostics_.resize(diagnosticCount);
            pendingException_ = pendingException;
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{
                             logicalValue(valid)};
        }

        if (requestedCount != 0) {
            addDiagnostic(
                instruction,
                "validateValue does not produce outputs");
            return missingOutputs(requestedCount);
        }
        (void)validatePropertyValue(
            instruction, *property, arguments[1]);
        return {};
    }

    std::vector<RuntimeValue> callPropertyValidatorHandle(
        const BytecodeInstruction& instruction,
        std::string_view encodedName,
        const std::vector<RuntimeValue>& arguments,
        int requestedCount) {
        constexpr std::string_view prefix =
            "__mparser_property_validator/";
        constexpr std::string_view marker = "/validator/";
        if (requestedCount != 0) {
            addDiagnostic(
                instruction,
                "property validation functions do not produce outputs");
            return missingOutputs(requestedCount);
        }
        if (arguments.size() != 1 ||
            encodedName.rfind(prefix, 0) != 0) {
            addDiagnostic(
                instruction,
                "property validation function expects one candidate value");
            return {};
        }

        const std::string_view identityAndIndex =
            encodedName.substr(prefix.size());
        const size_t separator =
            identityAndIndex.rfind(marker);
        if (separator == std::string_view::npos) {
            addDiagnostic(
                instruction,
                "property validation function descriptor is malformed");
            return {};
        }
        const std::string identity(
            identityAndIndex.substr(0, separator));
        const std::string indexText(
            identityAndIndex.substr(separator + marker.size()));
        char* end = nullptr;
        const unsigned long long parsed =
            std::strtoull(indexText.c_str(), &end, 10);
        const PropertyInfoPtr property =
            propertyForMetadata(identity);
        if (!end || *end != '\0' || !property ||
            parsed >= property->spec.validators.size()) {
            addDiagnostic(
                instruction,
                "property validation function descriptor is not available");
            return {};
        }
        PropertySpec validatorSpec;
        validatorSpec.validators.push_back(
            property->spec.validators[static_cast<size_t>(parsed)]);
        const auto validation = validateRuntimeArgument(
            arguments.front(), validatorSpec);
        if (!validation.succeeded) {
            propertyValidationError(
                instruction, *property, validation.error);
        }
        return {};
    }

    std::optional<RuntimeValue> implicitPropertyDefault(
        const PropertyInfo& property) {
        const BytecodeInstruction instruction = propertyInstruction(property);
        const std::string& type = property.spec.className;
        if (property.spec.dimensions.empty() && type.empty()) {
            return missingValue();
        }

        std::vector<size_t> dimensions = {0, 0};
        if (!property.spec.dimensions.empty()) {
            dimensions.clear();
            for (const auto& dimension : property.spec.dimensions) {
                if (dimension.text != ":" && !propertyDimension(dimension)) {
                    propertyValidationError(
                        instruction, property,
                        "property dimension cannot be represented at runtime");
                    return std::nullopt;
                }
                dimensions.push_back(propertyDimension(dimension).value_or(0));
            }
            if (dimensions.size() == 1) {
                dimensions.push_back(1);
            }
            dimensions = normalizeRuntimeDimensions(std::move(dimensions));
        }
        const auto count = checkedRuntimeDimensionProduct(dimensions);
        if (!count) {
            propertyValidationError(instruction, property,
                                    "property dimensions are too large");
            return std::nullopt;
        }

        if (type.empty()) {
            return arrayValueForDimensions(
                dimensions, std::vector<double>(*count, 0.0));
        }
        if (const auto numericClass =
                runtimeNumericClassFromName(type)) {
            return arrayValueForDimensions(
                dimensions, std::vector<double>(*count, 0.0),
                *numericClass);
        }
        if (type == "char") {
            return makeRuntimeCharacterArray(
                dimensions, std::u16string(*count, u' '));
        }
        if (type == "string") {
            return makeRuntimeStringArray(
                dimensions, std::vector<RuntimeStringElement>(*count));
        }
        if (type == "cell") {
            return cellValueForDimensions(
                dimensions,
                std::vector<RuntimeValue>(*count, missingValue()));
        }
        if (!classesByName_.contains(type)) {
            propertyValidationError(instruction, property,
                                    "property class is not available: " + type);
            return std::nullopt;
        }
        if (property.spec.dimensions.empty() || *count == 0) {
            return missingValue();
        }
        if (*count != 1) {
            propertyValidationError(
                instruction, property,
                "implicit object-array defaults currently require scalar size");
            return std::nullopt;
        }
        const size_t diagnosticCount = diagnostics_.size();
        auto outputs = callClassConstructor(instruction, type, {}, 1);
        if (outputs.empty() || diagnostics_.size() != diagnosticCount) {
            return std::nullopt;
        }
        return outputs.front();
    }

    std::optional<RuntimeValue> evaluateExplicitPropertyDefault(
        const PropertyInfo& property) {
        const BytecodeInstruction instruction = propertyInstruction(property);
        if (!property.hasInitializerRange) {
            addDiagnostic(instruction,
                          "property default expression has no bytecode range");
            return std::nullopt;
        }

        auto savedStack = std::move(stack_);
        auto savedForLoops = std::move(forLoopStack_);
        auto savedIndexContexts = std::move(indexContextStack_);
        auto savedSwitchContexts = std::move(switchContextStack_);
        auto savedTryContexts = std::move(tryContextStack_);
        const bool savedReturnRequested = returnRequested_;
        stack_.clear();
        forLoopStack_.clear();
        indexContextStack_.clear();
        switchContextStack_.clear();
        tryContextStack_.clear();
        returnRequested_ = false;
        frames_.push_back(makeRuntimeInitializerFrame(
            property.declaringClass + "." + property.name,
            property.span));
        activeClassFunctions_.push_back(ActiveClassFunction{
            property.declaringClass,
            "<property-default:" + property.name + ">", {}, nullptr});

        const size_t diagnosticCount = diagnostics_.size();
        executeFunctionBody(property.initializerEntry,
                            property.initializerEnd);
        std::optional<RuntimeValue> value;
        if (diagnostics_.size() == diagnosticCount) {
            if (stack_.size() != 1) {
                addDiagnostic(
                    instruction,
                    "property default expression must produce one value");
            } else {
                value = popRuntime(instruction, "property default expression");
            }
        }

        activeClassFunctions_.pop_back();
        frames_.pop_back();
        stack_ = std::move(savedStack);
        forLoopStack_ = std::move(savedForLoops);
        indexContextStack_ = std::move(savedIndexContexts);
        switchContextStack_ = std::move(savedSwitchContexts);
        tryContextStack_ = std::move(savedTryContexts);
        returnRequested_ = savedReturnRequested;
        return value;
    }

    std::optional<RuntimeValue> propertyDefault(PropertyInfo& property) {
        if (property.defaultEvaluated) {
            return property.defaultValue;
        }
        const BytecodeInstruction instruction = propertyInstruction(property);
        if (property.defaultEvaluationActive) {
            addDiagnostic(instruction,
                          "recursive property default evaluation: " +
                              propertyDisplayName(property));
            return std::nullopt;
        }

        property.defaultEvaluationActive = true;
        auto rawValue = property.spec.hasExplicitDefault
                            ? evaluateExplicitPropertyDefault(property)
                            : implicitPropertyDefault(property);
        if (!rawValue) {
            property.defaultEvaluationActive = false;
            return std::nullopt;
        }
        auto value = validatePropertyValue(instruction, property,
                                           std::move(*rawValue));
        property.defaultEvaluationActive = false;
        if (!value) {
            return std::nullopt;
        }
        property.defaultValue = *value;
        property.defaultEvaluated = true;
        return property.defaultValue;
    }

    std::optional<std::map<std::string, RuntimeValue>>
    initializePropertyValues(const ClassInfo& klass) {
        std::map<std::string, RuntimeValue> values;
        for (const auto& property : klass.propertyOrder) {
            if (property->abstractProperty || property->dependent) {
                continue;
            }
            const auto value = propertyDefault(*property);
            if (!value) {
                return std::nullopt;
            }
            if (!property->constant) {
                values[property->storageKey] = *value;
            }
        }
        return values;
    }

    std::vector<RuntimeValue> callClassConstructor(
        const BytecodeInstruction& instruction, const std::string& className,
        const std::vector<RuntimeValue>& arguments, int requestedCount,
        bool internalEnumerationConstruction = false) {
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction, "class is not available: " + className);
            return missingOutputs(requestedCount);
        }
        if (klass->second.enumerationClass &&
            !internalEnumerationConstruction) {
            if (requestedCount < 0 || requestedCount > 1) {
                addDiagnostic(
                    instruction,
                    "enumeration constructor supports at most one output: " +
                        className);
                return missingOutputs(requestedCount);
            }
            std::string memberName;
            if (arguments.empty()) {
                if (klass->second.declaredEnumerationOrder.empty()) {
                    addDiagnostic(instruction,
                                  "enumeration class has no members: " +
                                      className);
                    return missingOutputs(requestedCount);
                }
                memberName =
                    klass->second.declaredEnumerationOrder.front();
            } else if (arguments.size() == 1 &&
                       isString(arguments.front())) {
                memberName = *runtimeTextScalarUtf8(arguments.front());
            } else {
                addDiagnostic(
                    instruction,
                    "enumeration constructor expects zero arguments or one "
                    "member-name string: " +
                        className);
                return missingOutputs(requestedCount);
            }
            const auto value = enumerationMemberValue(
                instruction, className, memberName);
            if (!value) {
                return missingOutputs(requestedCount);
            }
            if (requestedCount == 0) {
                return {};
            }
            return {*value};
        }
        if (!klass->second.hierarchyValid) {
            addDiagnostic(instruction,
                          "class hierarchy is invalid: " + className);
            return missingOutputs(requestedCount);
        }
        if (klass->second.abstractClass) {
            std::string message =
                "abstract class cannot be instantiated: " + className;
            std::string separator = " (unimplemented: ";
            for (const auto& [name, method] :
                 klass->second.abstractMethods) {
                (void)method;
                message += separator + "method " + name;
                separator = ", ";
            }
            for (const auto& [name, property] :
                 klass->second.abstractProperties) {
                (void)property;
                message += separator + "property " + name;
                separator = ", ";
            }
            if (separator == ", ") {
                message += ")";
            }
            addDiagnostic(instruction, std::move(message));
            return missingOutputs(requestedCount);
        }
        if (requestedCount < 0) {
            addDiagnostic(instruction,
                          "bytecode call result count cannot be negative");
            return {};
        }
        const std::string requestingClass =
            internalEnumerationConstruction
                ? className
                : activeClassFunctions_.empty()
                ? std::string{}
                : activeClassFunctions_.back().className;
        if (const auto constructor =
                klass->second.declaredMethods.find(className);
            constructor != klass->second.declaredMethods.end() &&
            !hasConstructorAccess(constructor->second.access, className,
                                  requestingClass)) {
            addDiagnostic(instruction,
                          "constructor access is denied: " + className);
            return missingOutputs(requestedCount);
        }
        const auto properties = initializePropertyValues(klass->second);
        if (!properties) {
            return missingOutputs(requestedCount);
        }
        ConstructionContext construction;
        construction.object = objectValue(
            className, *properties, klass->second.handleClass);
        if (construction.object.handleObject &&
            construction.object.sharedFields) {
            (*construction.object.sharedFields)[
                std::string(kHandleValidityField)] = logicalValue(true);
        }
        return constructClass(instruction, className, arguments, construction,
                              requestedCount, false, requestingClass);
    }

    std::vector<RuntimeValue> constructClass(
        const BytecodeInstruction& instruction, const std::string& className,
        const std::vector<RuntimeValue>& arguments,
        ConstructionContext& construction, int requestedCount,
        bool explicitCall, const std::string& requestingClass) {
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction, "class is not available: " + className);
            return missingOutputs(requestedCount);
        }
        if (!klass->second.hierarchyValid) {
            addDiagnostic(instruction,
                          "class hierarchy is invalid: " + className);
            return missingOutputs(requestedCount);
        }
        if (construction.initializedClasses.contains(className)) {
            if (explicitCall) {
                addDiagnostic(
                    instruction,
                    "superclass constructor called more than once: " +
                        className);
            }
            return requestedCount == 0
                       ? std::vector<RuntimeValue>{}
                       : std::vector<RuntimeValue>{construction.object};
        }
        if (!construction.activeClasses.insert(className).second) {
            addDiagnostic(instruction,
                          "recursive superclass construction: " + className);
            return missingOutputs(requestedCount);
        }

        const auto constructor = klass->second.declaredMethods.find(className);
        if (constructor != klass->second.declaredMethods.end() &&
            !hasConstructorAccess(constructor->second.access, className,
                                  requestingClass)) {
            construction.activeClasses.erase(className);
            addDiagnostic(instruction,
                          "constructor access is denied: " + className);
            return missingOutputs(requestedCount);
        }
        if (constructor == klass->second.declaredMethods.end()) {
            bool passedArguments = false;
            for (const auto& superclassName : klass->second.superclasses) {
                if (isBuiltinNonExecutableSuperclass(superclassName)) {
                    continue;
                }
                const std::vector<RuntimeValue> superclassArguments =
                    passedArguments ? std::vector<RuntimeValue>{} : arguments;
                passedArguments = true;
                (void)constructClass(instruction, superclassName,
                                     superclassArguments, construction, 1,
                                     false, className);
                if (!diagnostics_.empty()) {
                    construction.activeClasses.erase(className);
                    return missingOutputs(requestedCount);
                }
            }
            if (!arguments.empty() && !passedArguments) {
                construction.activeClasses.erase(className);
                addDiagnostic(instruction,
                              "class has no constructor arguments: " +
                                  className);
                return missingOutputs(requestedCount);
            }
            construction.activeClasses.erase(className);
            construction.initializedClasses.insert(className);
            if (requestedCount == 0) {
                return {};
            }
            if (requestedCount != 1) {
                addDiagnostic(
                    instruction,
                    "default class constructor supports one output: " +
                        className);
                return missingOutputs(requestedCount);
            }
            return {construction.object};
        }

        std::set<std::string> explicitSuperclasses(
            constructor->second.explicitSuperclassConstructors.begin(),
            constructor->second.explicitSuperclassConstructors.end());
        const size_t diagnosticCount = diagnostics_.size();
        for (const auto& superclassName : klass->second.superclasses) {
            if (isBuiltinNonExecutableSuperclass(superclassName) ||
                explicitSuperclasses.contains(superclassName)) {
                continue;
            }
            (void)constructClass(instruction, superclassName, {}, construction,
                                 1, false, className);
            if (diagnostics_.size() != diagnosticCount) {
                construction.activeClasses.erase(className);
                return missingOutputs(requestedCount);
            }
        }

        const int internalRequestedCount = std::max(1, requestedCount);
        auto outputs = callFunctionInfo(
            instruction, className, constructor->second, arguments,
            internalRequestedCount, construction.object, &construction,
            true);
        if (diagnostics_.size() != diagnosticCount || outputs.empty()) {
            construction.activeClasses.erase(className);
            return missingOutputs(requestedCount);
        }
        construction.object = outputs.front();
        if (!isObject(construction.object)) {
            construction.activeClasses.erase(className);
            addDiagnostic(instruction,
                          "class constructor did not return an object: " +
                              className);
            return missingOutputs(requestedCount);
        }

        for (const auto& superclassName : klass->second.superclasses) {
            if (isBuiltinNonExecutableSuperclass(superclassName)) {
                continue;
            }
            if (!construction.initializedClasses.contains(superclassName)) {
                addDiagnostic(
                    instruction,
                    "superclass constructor was not called: " + className +
                        " -> " + superclassName);
            }
        }
        construction.activeClasses.erase(className);
        if (diagnostics_.size() != diagnosticCount) {
            return missingOutputs(requestedCount);
        }
        construction.initializedClasses.insert(className);

        if (requestedCount == 0) {
            return {};
        }
        outputs.resize(static_cast<size_t>(requestedCount), missingValue());
        outputs.front() = construction.object;
        return outputs;
    }

    std::vector<RuntimeValue> callClassMethod(
        const BytecodeInstruction& instruction, const std::string& className,
        const std::string& methodName,
        const std::string& methodDeclaringClass,
        const std::optional<RuntimeValue>& receiver,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        const auto klass = classesByName_.find(className);
        if (klass == classesByName_.end()) {
            addDiagnostic(instruction, "class method is not available: " +
                                          className + "." + methodName);
            return missingOutputs(requestedCount);
        }
        const FunctionInfo* method = nullptr;
        if (!methodDeclaringClass.empty()) {
            const auto owner = classesByName_.find(methodDeclaringClass);
            if (owner != classesByName_.end()) {
                const auto declared =
                    owner->second.declaredMethods.find(methodName);
                if (declared != owner->second.declaredMethods.end()) {
                    method = &declared->second;
                }
            }
        } else {
            method = selectMethod(klass->second, methodName);
        }
        if (!method) {
            addDiagnostic(instruction, "class method is not available: " +
                                          className + "." + methodName);
            return missingOutputs(requestedCount);
        }
        const RuntimeValue* invocationReceiver = nullptr;
        if (!method->staticMethod) {
            if (receiver) {
                invocationReceiver = &*receiver;
            } else if (!arguments.empty()) {
                invocationReceiver = &arguments.front();
            }
        }
        if (invocationReceiver && invocationReceiver->handleObject &&
            methodName != "delete" &&
            !requireUsableHandleValue(instruction,
                                      *invocationReceiver)) {
            return missingOutputs(requestedCount);
        }
        std::vector<RuntimeValue> callArguments = arguments;
        if (receiver) {
            callArguments.insert(callArguments.begin(), *receiver);
        }
        const std::string declaringClass = method->declaringClass.empty()
                                               ? className
                                               : method->declaringClass;
        if (!hasMemberAccess(method->access, declaringClass)) {
            addDiagnostic(instruction, "method access is denied: " +
                                          declaringClass + "." + methodName);
            return missingOutputs(requestedCount);
        }
        if (method->classDestructor && klass->second.handleClass) {
            std::optional<RuntimeValue> destructionTarget;
            if (receiver && arguments.empty()) {
                destructionTarget = *receiver;
            } else if (!receiver && arguments.size() == 1) {
                destructionTarget = arguments.front();
            } else {
                addDiagnostic(
                    instruction,
                    "class destructor accepts only its object: " +
                        declaringClass + ".delete");
                return missingOutputs(requestedCount);
            }
            if (requestedCount != 0) {
                addDiagnostic(instruction,
                              "class destructor does not produce outputs: " +
                                  declaringClass + ".delete");
                return missingOutputs(requestedCount);
            }
            if (isRuntimeClassObject(*destructionTarget) &&
                !isRuntimeScalarObject(*destructionTarget)) {
                deleteRuntimeObject(instruction, {*destructionTarget});
            } else {
                destroyHandleObject(instruction, *destructionTarget);
            }
            return {};
        }
        return callFunctionInfo(instruction,
                                declaringClass + "." + methodName, *method,
                                callArguments, requestedCount, std::nullopt,
                                nullptr, false);
    }

    std::vector<RuntimeValue> callFunctionInfo(
        const BytecodeInstruction& instruction, const std::string& name,
        const FunctionInfo& info, const std::vector<RuntimeValue>& arguments,
        int requestedCount, std::optional<RuntimeValue> constructorObject,
        ConstructionContext* construction, bool constructorInvocation) {
        if (requestedCount < 0) {
            addDiagnostic(instruction,
                          "bytecode call result count cannot be negative");
            return {};
        }
        if (!info.hasBody || info.abstractMethod) {
            addDiagnostic(instruction,
                          "abstract method has no implementation: " + name);
            return missingOutputs(requestedCount);
        }

        if (!functionOutputCountIsValid(
                info.signature, static_cast<size_t>(requestedCount))) {
            addDiagnostic(instruction,
                          "function output count mismatch for: " + name);
            return missingOutputs(requestedCount);
        }

        const std::string callableKey =
            info.key.empty() ? name : info.key;
        RuntimeWorkspace capturedWorkspace;
        std::map<std::string, size_t> captureOwners;
        std::optional<size_t> lexicalParentFrame;
        if (!info.lexicalParent.empty()) {
            for (auto active = activeFunctionFrames_.rbegin();
                 active != activeFunctionFrames_.rend(); ++active) {
                if (active->key == info.lexicalParent) {
                    lexicalParentFrame = active->frameIndex;
                    break;
                }
            }
            if (!lexicalParentFrame) {
                addDiagnostic(
                    instruction,
                    "nested function lexical parent is not active: " +
                        callableKey);
                return missingOutputs(requestedCount);
            }
        }
        for (const std::string& captureName : info.captureNames) {
            std::optional<size_t> owner;
            for (auto active = activeFunctionFrames_.rbegin();
                 active != activeFunctionFrames_.rend(); ++active) {
                if (!callableKey.starts_with(active->key + ">") ||
                    active->frameIndex >= frames_.size()) {
                    continue;
                }
                const auto value = frames_[active->frameIndex]
                                       .workspace.find(captureName);
                if (value == frames_[active->frameIndex].workspace.end()) {
                    continue;
                }
                capturedWorkspace[captureName] = value->second;
                owner = active->frameIndex;
                break;
            }
            if (!owner && lexicalParentFrame) {
                owner = *lexicalParentFrame;
            }
            if (owner) {
                captureOwners[captureName] = *owner;
            }
        }

        ExecutionCallGuard executionCall(*this, instruction.span);
        if (!executionCall) {
            return missingOutputs(requestedCount);
        }
        const std::string traceName =
            info.metadataIdentifier.empty()
                ? publicFunctionIdentifier(name)
                : info.metadataIdentifier;
        ExceptionFunctionGuard exceptionTrace(*this, traceName,
                                              instruction.span);
        const size_t callerOutputCount =
            instruction.implicitExpressionOutput
                ? 0
                : static_cast<size_t>(requestedCount);

        frames_.push_back(makeRuntimeFunctionFrame(
            RuntimeCallFrameKind::Function, traceName, info.span,
            arguments.size(), callerOutputCount));
        configurePersistentScope(frames_.back(), info);
        auto validatedArguments =
            validateFunctionArguments(instruction, name, info, arguments,
                                      callerOutputCount);
        if (!validatedArguments) {
            frames_.pop_back();
            return missingOutputs(requestedCount);
        }

        // Discard validation-only locals while retaining parameter map nodes
        // and the frame's persistent-scope metadata. Validated values are
        // rebound below, preserving the former two-frame isolation without
        // rebuilding a RuntimeWorkspace for every function call.
        for (auto binding = frames_.back().workspace.begin();
             binding != frames_.back().workspace.end();) {
            const bool declaredParameter =
                std::find(info.signature.parameters.begin(),
                          info.signature.parameters.end(),
                          binding->first) !=
                info.signature.parameters.end();
            const bool varargin = info.signature.hasVarargin &&
                                  binding->first == "varargin";
            if (!declaredParameter && !varargin) {
                binding = frames_.back().workspace.erase(binding);
            } else {
                ++binding;
            }
        }
        frames_.back().globalBindings.clear();
        frames_.back().persistentBindings.clear();
        for (auto& [captureName, captureValue] : capturedWorkspace) {
            currentFrame().try_emplace(captureName,
                                       std::move(captureValue));
        }

        const bool savedReturnRequested = returnRequested_;
        auto savedForLoopStack = std::move(forLoopStack_);
        auto savedIndexContextStack = std::move(indexContextStack_);
        auto savedSwitchContextStack = std::move(switchContextStack_);
        auto savedTryContextStack = std::move(tryContextStack_);
        returnRequested_ = false;
        forLoopStack_.clear();
        indexContextStack_.clear();
        switchContextStack_.clear();
        tryContextStack_.clear();

        activeFunctionFrames_.push_back(ActiveFunctionFrame{
            callableKey, frames_.size() - 1});
        initializeFunctionFrame(info.signature, validatedArguments->values,
                                callerOutputCount,
                                validatedArguments->positionalArgumentCount);
        if (constructorObject && !info.signature.outputs.empty()) {
            currentFrame()[info.signature.outputs.front()] = *constructorObject;
        }

        activeClassFunctions_.push_back(ActiveClassFunction{
            info.declaringClass, info.name,
            constructorInvocation && !info.signature.outputs.empty()
                ? info.signature.outputs.front()
                : std::string{},
            construction});
        activePersistentFunctionKeys_.push_back(
            persistentFunctionKey(info));

        enterFunctionProfile(name, info.span);
        const size_t bodyDiagnosticCount = diagnostics_.size();
        executeFunctionBody(info.entry, info.end);
        leaveFunctionProfile();

        activePersistentFunctionKeys_.pop_back();
        activeClassFunctions_.pop_back();

        auto completedFrame = std::move(currentFrame());
        for (const auto& [captureName, owner] : captureOwners) {
            const auto value = completedFrame.find(captureName);
            if (value != completedFrame.end() &&
                owner < frames_.size() - 1) {
                frames_[owner].workspace[captureName] = value->second;
            }
        }
        activeFunctionFrames_.pop_back();
        frames_.pop_back();
        returnRequested_ = savedReturnRequested;
        forLoopStack_ = std::move(savedForLoopStack);
        indexContextStack_ = std::move(savedIndexContextStack);
        switchContextStack_ = std::move(savedSwitchContextStack);
        tryContextStack_ = std::move(savedTryContextStack);

        if (diagnostics_.size() == bodyDiagnosticCount &&
            !validateFunctionOutputs(name, info, completedFrame)) {
            return missingOutputs(requestedCount);
        }
        return collectFunctionOutputs(completedFrame, info.signature,
                                      static_cast<size_t>(requestedCount));
    }

    std::vector<RuntimeValue> missingOutputs(int count) const {
        if (count < 0) {
            count = 0;
        }
        return std::vector<RuntimeValue>(static_cast<size_t>(count),
                                         missingValue());
    }

    void pushOutputValues(const BytecodeInstruction& instruction,
                          const std::vector<RuntimeValue>& outputs) {
        if (instruction.resultCount == 0) {
            if (instruction.implicitExpressionOutput &&
                !instruction.anonymousBodyOutput) {
                pushRuntime(missingValue());
            }
            return;
        }
        if (outputs.empty()) {
            addDiagnostic(instruction,
                          "bytecode call produced no outputs");
            return;
        }
        for (const auto& output : outputs) {
            pushRuntime(output);
        }
    }

    RuntimeValue callBuiltin(const BytecodeInstruction& instruction,
                             const std::string& name,
                             const std::vector<RuntimeValue>& arguments) {
        if (name == "MException") {
            auto result = runtimeConstructMException(arguments);
            if (!result.succeeded) {
                addDiagnostic(instruction, std::move(result.error),
                              "MParser:InvalidException");
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "error") {
            auto result = runtimeCreateErrorException(arguments);
            if (!result.succeeded) {
                addDiagnostic(instruction, std::move(result.error),
                              "MParser:InvalidException");
                return missingValue();
            }
            const RuntimeValue* identifier =
                runtimeExceptionProperty(result.value, "identifier");
            const RuntimeValue* message =
                runtimeExceptionProperty(result.value, "message");
            const auto identifierText = identifier
                ? runtimeTextScalarUtf8(*identifier) : std::nullopt;
            const auto messageText = message
                ? runtimeTextScalarUtf8(*message) : std::nullopt;
            if (identifierText && messageText && identifierText->empty() &&
                messageText->empty()) {
                return missingValue();
            }
            raiseException(
                instruction, result.value,
                runtimeExceptionFrameCount(result.value) == 0
                    ? RuntimeExceptionStackPolicy::Replace
                    : RuntimeExceptionStackPolicy::Preserve);
            return missingValue();
        }
        if (name == "throw" || name == "rethrow" ||
            name == "throwAsCaller") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " expects one MException object",
                              "MParser:InvalidException");
                return missingValue();
            }
            const auto policy =
                name == "rethrow"
                    ? RuntimeExceptionStackPolicy::Preserve
                    : name == "throwAsCaller"
                          ? RuntimeExceptionStackPolicy::AsCaller
                          : RuntimeExceptionStackPolicy::Replace;
            raiseException(instruction, arguments.front(), policy);
            return missingValue();
        }
        if (name == "assert") {
            if (arguments.empty() || !isNumeric(arguments.front())) {
                addDiagnostic(instruction,
                              "assert condition must be numeric or logical",
                              "MParser:InvalidAssertion");
                return missingValue();
            }
            const auto condition = runtimeNumericTruthValue(
                arguments.front());
            if (!condition) {
                addDiagnostic(
                    instruction,
                    "assert condition must be a real numeric value without NaN",
                    "MParser:InvalidAssertion");
                return missingValue();
            }
            if (*condition) {
                return missingValue();
            }

            std::vector<RuntimeValue> errorArguments;
            if (arguments.size() == 1) {
                errorArguments = {
                    characterValue("MParser:AssertionFailed"),
                    characterValue("Assertion failed.")};
            } else {
                errorArguments.assign(arguments.begin() + 1,
                                      arguments.end());
            }
            auto result = runtimeCreateErrorException(errorArguments);
            if (!result.succeeded) {
                addDiagnostic(instruction, std::move(result.error),
                              "MParser:InvalidAssertion");
                return missingValue();
            }
            raiseException(instruction, result.value,
                           RuntimeExceptionStackPolicy::Replace);
            return missingValue();
        }
        if (name == "clc" || name == "tic" ||
            name == "toc") {
            if (!arguments.empty()) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " currently expects no arguments");
                return missingValue();
            }
            if (name == "clc") {
                return missingValue();
            }
            if (name == "tic") {
                ticStart_ = std::chrono::steady_clock::now();
                return numberValue(0.0);
            }
            if (!ticStart_) {
                addDiagnostic(instruction,
                              "bytecode toc requires a preceding tic");
                return missingValue();
            }
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - *ticStart_;
            return numberValue(elapsed.count());
        }
        if (name == "metaclass") {
            return metaclassBuiltin(instruction, arguments);
        }
        if (name == "matlab.metadata.Class.fromName" ||
            name == "meta.class.fromName") {
            return metadataClassFromNameBuiltin(instruction, arguments);
        }
        if (name == "metafunction") {
            return metafunctionBuiltin(instruction, arguments);
        }
        if (name == "properties") {
            return propertyNamesBuiltin(instruction, arguments);
        }
        if (name == "methods") {
            return methodNamesBuiltin(instruction, arguments);
        }
        if (name == "events") {
            return eventNamesBuiltin(instruction, arguments);
        }
        if (name == "struct") {
            auto result = runtimeConstructScalarStruct(arguments);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "isfield") {
            if (arguments.size() != 2) {
                addDiagnostic(instruction,
                              "bytecode isfield expects two arguments");
                return missingValue();
            }
            auto result = runtimeStructIsField(arguments[0], arguments[1]);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "fieldnames") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode fieldnames expects one argument");
                return missingValue();
            }
            auto result = runtimeStructFieldNames(arguments.front());
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "rmfield") {
            if (arguments.size() != 2) {
                addDiagnostic(instruction,
                              "bytecode rmfield expects two arguments");
                return missingValue();
            }
            auto result = runtimeRemoveStructFields(arguments[0],
                                                    arguments[1]);
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "isstruct") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode isstruct expects one argument");
                return missingValue();
            }
            return logicalValue(arguments.front().kind ==
                                RuntimeValueKind::Struct);
        }
        if (name == "isprop") {
            return isPropertyBuiltin(instruction, arguments);
        }
        if (name == "ismethod") {
            return isMethodBuiltin(instruction, arguments);
        }
        if (name == "isvalid") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode isvalid expects one argument");
                return missingValue();
            }
            return eventListenerIsValid(arguments.front());
        }
        if (name == "isenum") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode isenum expects one argument");
                return missingValue();
            }
            return logicalValue(
                isObject(arguments.front()) &&
                !arguments.front().enumerationMemberName.empty());
        }
        if (name == "class") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode class expects one argument");
                return missingValue();
            }
            return characterValue(runtimeValueClassName(arguments.front()));
        }
        if (name == "isa") {
            if (arguments.size() != 2 || !isString(arguments[1])) {
                addDiagnostic(
                    instruction,
                    "bytecode isa expects a value and class-name string");
                return missingValue();
            }
            const RuntimeValue& value = arguments.front();
            const std::string target =
                canonicalRuntimeMetadataClassName(
                    *runtimeTextScalarUtf8(arguments[1]));
            bool matches = false;
            if (isNumeric(value)) {
                matches = target ==
                              runtimeNumericClassName(value.numericClass) ||
                          (target == "numeric" &&
                           value.numericClass !=
                               RuntimeNumericClass::Logical);
            } else if (isRuntimeMetadataObject(value)) {
                matches = runtimeMetadataIsa(value, target);
            } else if (isObject(value)) {
                matches =
                    reflectableClassDerivesFrom(value.className, target);
            } else if (isRuntimeCharacterArray(value)) {
                matches = target == "char";
            } else if (isRuntimeStringArray(value)) {
                matches = target == "string";
            } else if (isCell(value)) {
                matches = target == "cell";
            } else if (value.kind == RuntimeValueKind::Struct) {
                matches = target == "struct";
            } else if (isFunctionHandle(value)) {
                matches = target == "function_handle";
            }
            return logicalValue(matches);
        }
        if (name == "double" && arguments.size() == 1 &&
            isRuntimeCharacterArray(arguments.front())) {
            auto result = runtimeCharacterCodes(arguments.front());
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "logical" || name == "double") {
            if (arguments.size() != 1 || !isNumeric(arguments.front())) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " expects one numeric argument");
                return missingValue();
            }
            const auto converted = runtimeConvertNumericClass(
                arguments.front(),
                name == "logical" ? RuntimeNumericClass::Logical
                                  : RuntimeNumericClass::Double);
            if (!converted) {
                addDiagnostic(instruction,
                              "bytecode logical cannot convert NaN values");
                return missingValue();
            }
            return *converted;
        }
        if (name == "islogical") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode islogical expects one argument");
                return missingValue();
            }
            return logicalValue(isRuntimeLogical(arguments.front()));
        }
        if (name == "ischar" || name == "isstring" ||
            name == "isStringScalar") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode " + name + " expects one argument");
                return missingValue();
            }
            return logicalValue(
                name == "ischar"
                    ? isRuntimeCharacterArray(arguments.front())
                : name == "isstring"
                    ? isRuntimeStringArray(arguments.front())
                    : isRuntimeStringScalar(arguments.front()));
        }
        if (name == "char" || name == "string") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " expects one argument");
                return missingValue();
            }
            RuntimeTextOperationResult result;
            if (isRuntimeTemporalValue(arguments.front())) {
                auto temporal = runtimeTemporalFormat(
                    arguments.front(), name == "string");
                result.succeeded = temporal.succeeded;
                result.value = std::move(temporal.value);
                result.error = std::move(temporal.error);
            } else {
                result = name == "char"
                             ? runtimeConvertToCharacter(arguments.front())
                             : runtimeConvertToString(arguments.front());
            }
            if (result.succeeded) {
                return std::move(result.value);
            }
            if (isObject(arguments.front()) &&
                !arguments.front().enumerationMemberName.empty()) {
                return name == "char"
                           ? makeRuntimeCharacterVectorUtf8(
                                 arguments.front().enumerationMemberName)
                           : makeRuntimeStringScalarUtf8(
                                 arguments.front().enumerationMemberName);
            }
            addDiagnostic(instruction,
                          "bytecode " + result.error);
            return missingValue();
        }
        if (name == "cellstr" || name == "strlength" ||
            name == "ismissing") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode " + name + " expects one argument");
                return missingValue();
            }
            RuntimeTextOperationResult result;
            if (name == "cellstr") {
                result = runtimeCellstr(arguments.front());
            } else if (name == "strlength") {
                result = runtimeStringLengths(arguments.front());
            } else if (isRuntimeTemporalValue(arguments.front())) {
                auto temporal = runtimeTemporalPredicate(
                    "isnat", arguments.front());
                result.succeeded = temporal.succeeded;
                result.value = std::move(temporal.value);
                result.error = std::move(temporal.error);
            } else {
                result = runtimeTextMissingMask(arguments.front());
            }
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (name == "length" || name == "numel" ||
            name == "ndims" || name == "isempty") {
            if (arguments.size() != 1) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " expects one argument");
                return missingValue();
            }
            if (name == "length") {
                const auto dimensions = runtimeDimensions(arguments.front());
                return numberValue(static_cast<double>(*std::max_element(
                    dimensions.begin(), dimensions.end())));
            }
            if (name == "numel") {
                return numberValue(static_cast<double>(
                    elementCount(arguments.front())));
            }
            if (name == "isempty") {
                return logicalValue(elementCount(arguments.front()) == 0);
            }
            if (name == "ndims") {
                return numberValue(static_cast<double>(
                    runtimeDimensionCount(arguments.front())));
            }
        }
        if (name == "strcmp" || name == "strcmpi") {
            if (arguments.size() != 2) {
                addDiagnostic(instruction,
                              "bytecode " + name +
                                  " expects two arguments");
                return missingValue();
            }
            auto result = runtimeCompareText(
                name, arguments[0], arguments[1], name == "strcmpi");
            if (!result.succeeded) {
                addDiagnostic(instruction,
                              "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }

        addDiagnostic(instruction,
                      "bytecode builtin is not executable yet: " + name);
        return missingValue();
    }

    RuntimeValue indexValue(const BytecodeInstruction& instruction,
                            const RuntimeValue& target,
                            const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty()) {
            addDiagnostic(instruction,
                          "bytecode indexing requires subscripts");
            return missingValue();
        }

        const bool linearColon =
            arguments.size() == 1 &&
            instruction.colonSubscripts.size() == 1 &&
            instruction.colonSubscripts.front();

        if (isRuntimeMetadataObject(target)) {
            return indexMetadataValue(instruction, target, arguments);
        }
        if (target.kind == RuntimeValueKind::Struct) {
            auto result = runtimeIndexStruct(target, arguments,
                                             linearColon);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (target.kind == RuntimeValueKind::Cell) {
            auto result = runtimeIndexCell(target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (isRuntimeTextValue(target)) {
            auto result = runtimeIndexText(target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (isRuntimeClassObject(target)) {
            auto result = runtimeIndexObject(
                target, arguments, objectArrayPolicy(instruction),
                linearColon);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (target.kind == RuntimeValueKind::MissingArray) {
            auto result = runtimeIndexMissingArray(
                target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(instruction, "bytecode " + result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (!isNumeric(target)) {
            addDiagnostic(instruction,
                          "bytecode indexing requires a missing, numeric, "
                          "cell, structure, text, or object target");
            return missingValue();
        }
        auto result = runtimeIndexNumeric(target, arguments, linearColon);
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue indexMetadataValue(
        const BytecodeInstruction& instruction, const RuntimeValue& target,
        const std::vector<RuntimeValue>& arguments) {
        const auto metadataKind = runtimeMetadataKind(target);
        if (!metadataKind || arguments.empty()) {
            addDiagnostic(instruction,
                          "bytecode metadata indexing requires subscripts");
            return missingValue();
        }
        for (const auto& argument : arguments) {
            if (!isNumeric(argument)) {
                addDiagnostic(
                    instruction,
                    "bytecode metadata indexing requires numeric or logical "
                    "subscripts");
                return missingValue();
            }
        }

        auto elementAtLinearIndex =
            [&](size_t linearIndex) -> std::optional<RuntimeValue> {
            if (isRuntimeMetadataScalar(target)) {
                return linearIndex == 0
                           ? std::optional<RuntimeValue>{target}
                           : std::nullopt;
            }
            const auto storageOffset =
                runtimeColumnMajorLinearToStorageOffset(target, linearIndex);
            if (!storageOffset || *storageOffset >= target.cells.size()) {
                return std::nullopt;
            }
            return target.cells[*storageOffset];
        };

        if (arguments.size() > 1) {
            const auto effectiveDimensions =
                runtimeEffectiveSubscriptDimensions(target, arguments.size());
            std::vector<std::vector<size_t>> selections;
            std::vector<size_t> selectionDimensions;
            selections.reserve(arguments.size());
            selectionDimensions.reserve(arguments.size());
            for (size_t index = 0; index < arguments.size(); ++index) {
                auto selection = checkedIndices(
                    instruction, arguments[index], effectiveDimensions[index]);
                if (!selection) {
                    return missingValue();
                }
                selectionDimensions.push_back(selection->size());
                selections.push_back(std::move(*selection));
            }

            const auto count =
                checkedRuntimeDimensionProduct(selectionDimensions);
            if (!count) {
                addDiagnostic(
                    instruction,
                    "bytecode metadata indexed result dimensions are too large");
                return missingValue();
            }
            std::vector<RuntimeValue> values;
            values.reserve(*count);
            for (size_t outputOffset = 0; outputOffset < *count;
                 ++outputOffset) {
                const auto outputCoordinates = runtimeRowMajorCoordinates(
                    outputOffset, selectionDimensions);
                std::vector<size_t> sourceCoordinates(arguments.size(), 0);
                for (size_t index = 0; index < arguments.size(); ++index) {
                    sourceCoordinates[index] =
                        selections[index][outputCoordinates[index]];
                }
                const auto storageOffset = runtimeSubscriptsToStorageOffset(
                    target, sourceCoordinates, effectiveDimensions);
                if (!storageOffset || *storageOffset >= target.cells.size()) {
                    addDiagnostic(
                        instruction,
                        "bytecode metadata indexing could not map subscripts");
                    return missingValue();
                }
                values.push_back(target.cells[*storageOffset]);
            }
            if (values.size() == 1) {
                return values.front();
            }
            return makeRuntimeMetadataArray(
                *metadataKind, std::move(values), selectionDimensions);
        }

        const RuntimeValue& subscript = arguments.front();
        auto selection = runtimeResolveIndexSelection(
            subscript, elementCount(target), false);
        if (!selection.succeeded) {
            addDiagnostic(instruction,
                          "bytecode metadata " +
                              std::move(selection.error));
            return missingValue();
        }

        std::vector<RuntimeValue> values;
        values.reserve(selection.indices.size());
        for (const size_t linearIndex : selection.indices) {
            const auto value = elementAtLinearIndex(linearIndex);
            if (!value) {
                addDiagnostic(
                    instruction,
                    "bytecode metadata index could not map storage");
                return missingValue();
            }
            values.push_back(*value);
        }
        if (values.size() == 1) {
            return values.front();
        }
        const auto dimensions = runtimeLinearIndexResultDimensions(
            target, subscript, values.size(), selection.logicalMask,
            instruction.colonSubscripts.size() == 1 &&
                instruction.colonSubscripts.front());
        return makeRuntimeMetadataArray(
            *metadataKind, std::move(values), dimensions);
    }

    std::optional<std::vector<size_t>>
    checkedIndices(const BytecodeInstruction& instruction,
                   const RuntimeValue& subscript, size_t length) {
        auto selection =
            runtimeResolveIndexSelection(subscript, length, false);
        if (!selection.succeeded) {
            addDiagnostic(instruction,
                          "bytecode " + std::move(selection.error));
            return std::nullopt;
        }
        return std::move(selection.indices);
    }

    void applyColon(const BytecodeInstruction& instruction,
                    const std::vector<RuntimeValue>& operands) {
        auto range = runtimeMaterializeColonValue(operands);
        if (!range.succeeded) {
            addDiagnostic(instruction, "bytecode " + range.error);
            return;
        }
        pushRuntime(std::move(range.value));
    }

    RuntimeValue applyNumericBinary(const BytecodeInstruction& instruction,
                                    const RuntimeValue& left,
                                    const RuntimeValue& right) {
        auto result = runtimeApplyNumericBinary(
            instruction.operand, left, right);
        if (!result.succeeded) {
            addDiagnostic(instruction, "bytecode " + result.error);
            return missingValue();
        }
        return std::move(result.value);
    }

    std::optional<StackValue>
    popStackValue(const BytecodeInstruction& instruction,
                  std::string_view context) {
        if (stack_.empty()) {
            addDiagnostic(instruction,
                          "bytecode stack underflow during " +
                              std::string(context));
            return std::nullopt;
        }
        StackValue value = stack_.back();
        stack_.pop_back();
        return value;
    }

    std::optional<RuntimeValue>
    popRuntime(const BytecodeInstruction& instruction,
               std::string_view context) {
        const auto value = popStackValue(instruction, context);
        if (!value) {
            return std::nullopt;
        }
        if (value->isBuiltinReference) {
            addDiagnostic(instruction,
                          "bytecode builtin reference is not a runtime value: " +
                              value->builtinName);
            return std::nullopt;
        }
        if (value->isFunctionReference) {
            addDiagnostic(
                instruction,
                "bytecode function reference is not a runtime value: " +
                    value->functionName);
            return std::nullopt;
        }
        return value->value;
    }

    std::optional<std::vector<RuntimeValue>>
    popRuntimeValues(const BytecodeInstruction& instruction, int count,
                     std::string_view context) {
        if (count < 0) {
            addDiagnostic(instruction,
                          "bytecode instruction has negative operand count");
            return std::nullopt;
        }

        std::vector<RuntimeValue> values;
        values.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            const auto value = popRuntime(instruction, context);
            if (!value) {
                return std::nullopt;
            }
            values.push_back(*value);
        }
        std::reverse(values.begin(), values.end());
        return values;
    }

    void pushRuntime(RuntimeValue value) {
        stack_.push_back(runtimeStackValue(std::move(value)));
    }

    void addDiagnostic(const BytecodeInstruction& instruction,
                       std::string message,
                       std::string identifier =
                           std::string(kRuntimeErrorIdentifier)) {
        Diagnostic diagnostic{instruction.span, std::move(message),
                              std::move(identifier)};
        diagnostic.stack = exceptionFrames(instruction.span);
        pendingException_ = runtimeExceptionFromDiagnostic(
            diagnostic, diagnostic.stack);
        diagnostics_.push_back(std::move(diagnostic));
    }

    const BuiltinRegistry& builtinRegistry() const {
        if (semantic_ && semantic_->builtinRegistry) {
            return *semantic_->builtinRegistry;
        }
        return *defaultBuiltinRegistry();
    }

    void appendBuiltinDiagnostics(
        const BytecodeInstruction& instruction,
        std::vector<Diagnostic> diagnostics) {
        for (auto& diagnostic : diagnostics) {
            if (diagnostic.span.begin.sourceId ==
                kInvalidSourceId) {
                diagnostic.span = instruction.span;
            }
            if (diagnostic.stack.empty()) {
                diagnostic.stack =
                    exceptionFrames(instruction.span);
            }
            if (diagnostic.severity ==
                DiagnosticSeverity::Warning) {
                warnings_.push_back(std::move(diagnostic));
                continue;
            }
            pendingException_ = runtimeExceptionFromDiagnostic(
                diagnostic, diagnostic.stack);
            diagnostics_.push_back(std::move(diagnostic));
        }
    }

    void raiseException(const BytecodeInstruction& instruction,
                        const RuntimeValue& exception,
                        RuntimeExceptionStackPolicy policy) {
        auto prepared = runtimePrepareExceptionForThrow(
            exception, exceptionFrames(instruction.span), policy);
        if (!prepared.succeeded) {
            addDiagnostic(instruction, std::move(prepared.error),
                          "MParser:InvalidException");
            return;
        }
        pendingException_ = std::move(prepared.value);
        auto diagnostic = runtimeDiagnosticFromException(
            *pendingException_, instruction.span);
        diagnostic.span =
            exceptionDiagnosticSpan(*pendingException_, diagnostic.span);
        diagnostics_.push_back(std::move(diagnostic));
    }

    RuntimeWorkspace& currentFrame() {
        return frames_.back().workspace;
    }

    const RuntimeWorkspace& currentFrame() const {
        return frames_.back().workspace;
    }

    const SourceCallerOverride* activeSourceCallerOverride() const {
        if (sourceCallerOverrides_.empty()) {
            return nullptr;
        }
        const auto& override = sourceCallerOverrides_.back();
        return override.workspace &&
                       frames_.size() == override.parentFrameCount + 1
                   ? &override
                   : nullptr;
    }

    RuntimeWorkspace* workspaceFor(BuiltinWorkspaceScope scope) {
        const size_t count =
            inheritedWorkspaceFrames_.size() + frames_.size();
        if (count == 0) {
            return nullptr;
        }
        size_t index = count - 1;
        if (scope == BuiltinWorkspaceScope::Base) {
            index = 0;
        } else if (scope == BuiltinWorkspaceScope::Caller) {
            if (const auto* override =
                    activeSourceCallerOverride()) {
                return override->workspace;
            }
            index = count > 1 ? count - 2 : 0;
        }
        if (index < inheritedWorkspaceFrames_.size()) {
            return inheritedWorkspaceFrames_[index];
        }
        return &frames_[index - inheritedWorkspaceFrames_.size()].workspace;
    }

    std::vector<RuntimeWorkspace*> workspaceAncestorsFor(
        BuiltinWorkspaceScope scope) {
        std::vector<RuntimeWorkspace*> ancestors;
        const size_t count =
            inheritedWorkspaceFrames_.size() + frames_.size();
        if (count == 0) {
            return ancestors;
        }
        if (scope == BuiltinWorkspaceScope::Caller) {
            if (const auto* override =
                    activeSourceCallerOverride()) {
                for (RuntimeWorkspace* workspace :
                     inheritedWorkspaceFrames_) {
                    if (workspace == override->workspace) {
                        return ancestors;
                    }
                    ancestors.push_back(workspace);
                }
                for (auto& frame : frames_) {
                    if (&frame.workspace == override->workspace) {
                        return ancestors;
                    }
                    ancestors.push_back(&frame.workspace);
                }
                ancestors.clear();
            }
        }
        size_t index = count - 1;
        if (scope == BuiltinWorkspaceScope::Base) {
            index = 0;
        } else if (scope == BuiltinWorkspaceScope::Caller) {
            index = count > 1 ? count - 2 : 0;
        }
        ancestors.reserve(index);
        for (size_t frame = 0; frame < index; ++frame) {
            if (frame < inheritedWorkspaceFrames_.size()) {
                ancestors.push_back(inheritedWorkspaceFrames_[frame]);
            } else {
                ancestors.push_back(
                    &frames_[frame - inheritedWorkspaceFrames_.size()]
                         .workspace);
            }
        }
        return ancestors;
    }

    std::optional<std::string> symbolName(BindingRef binding) const {
        if (!semantic_ || binding.symbolId < 0) {
            return std::nullopt;
        }
        const auto index = static_cast<size_t>(binding.symbolId);
        if (index >= semantic_->symbols.size()) {
            return std::nullopt;
        }
        return semantic_->symbols[index].name;
    }

    const BytecodeProgram* program_ = nullptr;
    const SemanticResult* semantic_ = nullptr;
    ArgumentContractCatalog argumentContractCatalog_;
    std::vector<StackValue> stack_;
    std::vector<ForLoopState> forLoopStack_;
    std::vector<IndexContext> indexContextStack_;
    std::vector<std::unique_ptr<ActiveLvalue>> lvalueStack_;
    std::vector<SwitchContext> switchContextStack_;
    std::vector<TryContext> tryContextStack_;
    std::vector<size_t> switchEndAt_;
    std::vector<size_t> tryEndAt_;
    std::vector<size_t> functionEndAt_;
    std::deque<RuntimeCallFrame> frames_;
    std::vector<int> activeAnonymousBodyOutputCounts_;
    std::shared_ptr<RuntimeSessionState> sessionState_;
    std::shared_ptr<RuntimeExecutionControl> executionControl_;
    std::set<std::string> baseGlobalNames_;
    std::vector<std::string> activePersistentFunctionKeys_;
    std::vector<ActiveFunctionFrame> activeFunctionFrames_;
    std::vector<SourceCallerOverride> sourceCallerOverrides_;
    std::vector<ActiveClassFunction> activeClassFunctions_;
    std::shared_ptr<RuntimeCallableContext> callableContext_;
    std::map<size_t, EventListenerRecord> eventListeners_;
    std::map<size_t, DynamicPropertyRecord> dynamicProperties_;
    std::set<size_t> activeDynamicPropertyGetters_;
    std::set<size_t> activeDynamicPropertySetters_;
    std::set<const void*> destroyingHandleFields_;
    size_t nextEventListenerId_ = 1;
    size_t nextDynamicPropertyId_ = 1;
    std::map<std::string, const HirNode*> functionNodes_;
    std::map<std::string, const HirNode*> classFunctionNodes_;
    std::map<std::string, FunctionInfo> functionsByName_;
    std::map<std::string, std::string> functionsByMetadataIdentifier_;
    std::set<std::string> ambiguousFunctionMetadataIdentifiers_;
    std::map<std::string, ClassInfo> classesByName_;
    std::set<std::string> classHierarchyDiagnosticKeys_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Diagnostic> warnings_;
    std::optional<RuntimeValue> pendingException_;
    RuntimeOutputSink runtimeOutputSink_;
    std::vector<RuntimeOutputEvent> outputEvents_;
    std::vector<RuntimeExpressionResult> expressionResults_;
    std::uint64_t nextConsoleSequence_ = 0;
    std::optional<std::chrono::steady_clock::time_point> ticStart_;
    std::vector<size_t> instructionExecutionCounts_;
    std::map<std::string, BytecodeFunctionProfile> functionProfiles_;
    std::map<size_t, BytecodeLoopProfile> loopProfiles_;
    std::map<size_t, BytecodeCallSiteProfile> callSiteProfiles_;
    std::map<size_t, BytecodeAssignmentProfile> assignmentProfiles_;
    std::map<size_t, BytecodeLoadProfile> loadProfiles_;
    std::map<std::string, BytecodeWorkspaceInputProfile>
        workspaceInputProfiles_;
    std::map<std::string, BytecodeFunctionEntryProfile>
        functionEntryProfiles_;
    std::map<size_t, ActiveTypedLoopRegion> typedLoopRegions_;
    std::map<size_t, ActiveTypedDenseRegion> typedDenseRegions_;
    std::map<size_t, BytecodeTypedRegionExecutionProfile>
        typedRegionExecutions_;
    std::vector<std::string> functionProfileStack_;
    std::vector<std::string> activeExceptionFunctionNames_;
    std::vector<RuntimeExceptionFrame> exceptionCallerFrames_;
    size_t executedInstructionCount_ = 0;
    size_t currentPc_ = 0;
    bool returnRequested_ = false;
    bool profilingEnabled_ = true;
    bool scriptModeActive_ = false;
    bool executionControlActive_ = false;
    bool executionStopDiagnosticAdded_ = false;
    TypedRegionBackend typedRegionBackend_ = TypedRegionBackend::Auto;
    bool typedRegionsEnabled_ = false;
    std::vector<RuntimeWorkspace*> inheritedWorkspaceFrames_;
    std::vector<RuntimeSourceCallable> inheritedSourceCallables_;
    std::vector<RuntimeSourceCallableScope>
        inheritedSourceCallableScopes_;
    RuntimeSourceCallableInvoker inheritedSourceCallableInvoker_;
    RuntimeWorkspace* inheritedSourceCallableWorkspace_ = nullptr;
    RuntimeSourceStorageResolver inheritedSourceStorageResolver_;
    RuntimeSourceStorageDeclarer inheritedSourceStorageDeclarer_;
    RuntimeSourceStorageClearer inheritedSourceStorageClearer_;
    RuntimeWorkspace* inheritedSourceStorageWorkspace_ = nullptr;
    std::string requestedEntryFunction_;
    std::vector<RuntimeValue> entryArguments_;
    std::optional<size_t> requestedEntryOutputCount_;
    std::string executedEntryFunction_;
    size_t executedRequestedOutputCount_ = 0;
    std::optional<FunctionSignature> entrySignature_;
    std::vector<RuntimeValue> entryOutputs_;
    std::vector<std::string> entryOutputNames_;
};

} // namespace

BytecodeVmResult BytecodeVm::run(const BytecodeProgram& program,
                                 const SemanticResult& semantic) {
    BytecodeVmContext context;
    return context.run(program, semantic, nullptr, BytecodeVmOptions{},
                       true);
}

BytecodeVmResult BytecodeVm::run(const BytecodeProgram& program,
                                 const SemanticResult& semantic,
                                 const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, nullptr, options, true);
}

BytecodeVmResult BytecodeVm::run(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeTypedIrModule& typedIr) {
    BytecodeVmContext context;
    return context.run(program, semantic, &typedIr, BytecodeVmOptions{},
                       true);
}

BytecodeVmResult BytecodeVm::run(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeTypedIrModule& typedIr,
    const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, &typedIr, options, true);
}

BytecodeVmResult BytecodeVm::runValidated(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, nullptr, options, false);
}

BytecodeVmResult BytecodeVm::runValidated(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeTypedIrModule& typedIr,
    const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, &typedIr, options, false);
}

} // namespace mparser
