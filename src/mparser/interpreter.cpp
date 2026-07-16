#include "mparser/interpreter.h"
#include "mparser/function_signature.h"
#include "mparser/runtime_array_ops.h"
#include "mparser/runtime_argument_validation.h"
#include "mparser/runtime_assignment.h"
#include "mparser/runtime_index.h"
#include "mparser/runtime_math.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_range.h"
#include "mparser/runtime_reduction.h"
#include "mparser/runtime_scan.h"
#include "mparser/runtime_shape.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace mparser {
namespace {

RuntimeValue missingValue() {
    return RuntimeValue{};
}

RuntimeValue numberValue(
    double value,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value;
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue logicalValue(bool value) {
    return numberValue(value ? 1.0 : 0.0,
                       RuntimeNumericClass::Logical);
}

RuntimeValue stringValue(std::string value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::String;
    result.text = std::move(value);
    setRuntimeDimensions(result, {1, result.text.size()});
    return result;
}

RuntimeValue vectorValue(
    std::vector<double> values,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Vector;
    result.elements = std::move(values);
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {1, result.elements.size()});
    return result;
}

RuntimeValue matrixValue(size_t rows, size_t columns,
                         std::vector<double> values,
                         RuntimeNumericClass numericClass =
                             RuntimeNumericClass::Double) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Matrix;
    result.elements = std::move(values);
    result.numericClass = numericClass;
    setRuntimeDimensions(result, {rows, columns});
    return result;
}

RuntimeValue cellValue(std::vector<RuntimeValue> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells = std::move(values);
    setRuntimeDimensions(result, {1, result.cells.size()});
    return result;
}

RuntimeValue cellValueForDimensions(std::vector<size_t> dimensions,
                                    std::vector<RuntimeValue> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Cell;
    result.cells = std::move(values);
    setRuntimeDimensions(result, std::move(dimensions));
    return result;
}

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isString(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::String;
}

bool isVector(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Vector;
}

bool isMatrix(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Matrix;
}

bool isCell(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Cell;
}

bool isStruct(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Struct;
}

const std::map<std::string, RuntimeValue>& objectFields(
    const RuntimeValue& value) {
    if (value.handleObject && value.sharedFields) {
        return *value.sharedFields;
    }
    return value.fields;
}

bool isNumeric(const RuntimeValue& value) {
    return isNumber(value) || isVector(value) || isMatrix(value);
}

bool isArray(const RuntimeValue& value) {
    return isVector(value) || isMatrix(value);
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
    if (isNumber(left) && isNumber(right)) {
        return left.numericClass == right.numericClass &&
               left.number == right.number;
    }
    if (isString(left) && isString(right)) {
        return left.text == right.text;
    }
    if (isArray(left) && isArray(right)) {
        return left.numericClass == right.numericClass &&
               runtimeDimensions(left) == runtimeDimensions(right) &&
               left.elements == right.elements;
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
    if (isStruct(left) && isStruct(right)) {
        if (left.fields.size() != right.fields.size()) {
            return false;
        }
        for (const auto& [name, value] : left.fields) {
            const auto other = right.fields.find(name);
            if (other == right.fields.end() ||
                !runtimeEqual(value, other->second)) {
                return false;
            }
        }
        return true;
    }
    if (left.kind == RuntimeValueKind::NameValueArgument &&
        right.kind == RuntimeValueKind::NameValueArgument) {
        return left.text == right.text && left.cells.size() == 1 &&
               right.cells.size() == 1 &&
               runtimeEqual(left.cells.front(), right.cells.front());
    }
    if (left.kind == RuntimeValueKind::Object &&
        right.kind == RuntimeValueKind::Object) {
        if (!left.enumerationMemberName.empty() ||
            !right.enumerationMemberName.empty()) {
            return left.className == right.className &&
                   left.enumerationMemberName ==
                       right.enumerationMemberName;
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

RuntimeValue oneBasedIndexRange(size_t length) {
    std::vector<double> values;
    values.reserve(length);
    for (size_t index = 1; index <= length; ++index) {
        values.push_back(static_cast<double>(index));
    }
    return vectorValue(std::move(values));
}

size_t rowCount(const RuntimeValue& value) {
    return runtimeDimension(value, 0);
}

size_t columnCount(const RuntimeValue& value) {
    return runtimeDimension(value, 1);
}

size_t elementCount(const RuntimeValue& value) {
    return runtimeShapeElementCount(value);
}

double elementAt(const RuntimeValue& value, size_t index) {
    return isNumber(value) ? value.number : value.elements[index];
}

RuntimeValue arrayValueForShape(size_t rows, size_t columns,
                                std::vector<double> values,
                                RuntimeNumericClass numericClass =
                                    RuntimeNumericClass::Double) {
    if (rows == 1) {
        return vectorValue(std::move(values), numericClass);
    }
    return matrixValue(rows, columns, std::move(values), numericClass);
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

bool truthy(const RuntimeValue& value) {
    if (value.kind == RuntimeValueKind::Number) {
        return value.number != 0.0 && !std::isnan(value.number);
    }
    if (isArray(value)) {
        for (double element : value.elements) {
            if (element == 0.0 || std::isnan(element)) {
                return false;
            }
        }
        return !value.elements.empty();
    }
    return false;
}

template <typename Operation>
RuntimeValue mapUnary(
    const RuntimeValue& value, Operation operation,
    RuntimeNumericClass numericClass = RuntimeNumericClass::Double) {
    if (isNumber(value)) {
        return numberValue(operation(value.number), numericClass);
    }

    std::vector<double> mapped;
    mapped.reserve(value.elements.size());
    for (double element : value.elements) {
        mapped.push_back(operation(element));
    }
    return arrayValueForDimensions(runtimeDimensions(value),
                                   std::move(mapped), numericClass);
}

bool isWholeNumber(double value) {
    return std::isfinite(value) && std::floor(value) == value;
}

bool isLogicalBinaryOperator(std::string_view operation) {
    return operation == ">" || operation == "<" || operation == ">=" ||
           operation == "<=" || operation == "==" || operation == "~=" ||
           operation == "&" || operation == "|" || operation == "&&" ||
           operation == "||";
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

struct FunctionCallResult {
    std::vector<RuntimeValue> outputs;
};

struct ArgumentContractRef {
    const HirNode* declaration = nullptr;
    ArgumentBlockKind blockKind = ArgumentBlockKind::Input;
};

void collectArgumentContracts(const HirNode& function,
                              std::vector<ArgumentContractRef>& contracts) {
    for (const auto& block : function.children) {
        if (block->kind != HirKind::ArgumentBlock) {
            continue;
        }
        for (const auto& declaration : block->children) {
            if (declaration->kind == HirKind::Argument) {
                contracts.push_back(
                    ArgumentContractRef{declaration.get(),
                                        block->argumentBlock.kind});
            }
        }
    }
}

class InterpreterContext {
public:
    InterpreterResult run(const SemanticResult& semantic) {
        frames_.push_back({});
        if (semantic.root) {
            collectFunctions(*semantic.root);
            executeModule(*semantic.root);
        }

        InterpreterResult result;
        const auto& variables = resultFrame_.empty() ? currentFrame()
                                                     : resultFrame_;
        for (const auto& [name, value] : variables) {
            result.variables.push_back(RuntimeVariable{name, value});
        }
        result.diagnostics = std::move(diagnostics_);
        return result;
    }

private:
    enum class ControlSignal {
        None,
        Break,
        Continue,
        Return,
    };

    class LoopDepthGuard {
    public:
        explicit LoopDepthGuard(size_t& depth) : depth_(depth) {
            ++depth_;
        }

        ~LoopDepthGuard() {
            --depth_;
        }

    private:
        size_t& depth_;
    };

    class FunctionControlContext {
    public:
        FunctionControlContext(size_t& depth, ControlSignal& signal)
            : depth_(depth), signal_(signal), savedDepth_(depth),
              savedSignal_(signal) {
            depth_ = 0;
            signal_ = ControlSignal::None;
        }

        ~FunctionControlContext() {
            depth_ = savedDepth_;
            signal_ = savedSignal_;
        }

    private:
        size_t& depth_;
        ControlSignal& signal_;
        size_t savedDepth_;
        ControlSignal savedSignal_;
    };

    class DiagnosticTrapGuard {
    public:
        DiagnosticTrapGuard(std::optional<size_t>& trapBase,
                            size_t diagnosticBase)
            : trapBase_(trapBase), savedTrapBase_(trapBase) {
            trapBase_ = diagnosticBase;
        }

        ~DiagnosticTrapGuard() {
            trapBase_ = savedTrapBase_;
        }

    private:
        std::optional<size_t>& trapBase_;
        std::optional<size_t> savedTrapBase_;
    };

    void addDiagnostic(const HirNode& node, std::string message) {
        diagnostics_.push_back(Diagnostic{node.span, std::move(message)});
    }

    bool diagnosticTrapTriggered() const {
        return diagnosticTrapBase_ &&
               diagnostics_.size() > *diagnosticTrapBase_;
    }

    std::map<std::string, RuntimeValue>& currentFrame() {
        return frames_.back();
    }

    const std::map<std::string, RuntimeValue>& currentFrame() const {
        return frames_.back();
    }

    void collectFunctions(const HirNode& node) {
        if (node.kind == HirKind::Module) {
            for (const auto& child : node.children) {
                if (child->kind == HirKind::Function) {
                    functionsByName_[child->label] = child.get();
                }
            }
        }

        for (const auto& child : node.children) {
            if (child->kind == HirKind::Class) {
                continue;
            }
            collectFunctions(*child);
        }
    }

    void executeModule(const HirNode& node) {
        const HirNode* firstFunction = nullptr;
        bool hasTopLevelExecutable = false;

        for (const auto& child : node.children) {
            if (child->kind == HirKind::Function && firstFunction == nullptr) {
                firstFunction = child.get();
                continue;
            }
            if (child->kind != HirKind::Class &&
                child->kind != HirKind::Function) {
                hasTopLevelExecutable = true;
            }
        }

        if (!hasTopLevelExecutable && firstFunction != nullptr) {
            (void)callFunction(*firstFunction, {}, true);
            return;
        }

        for (const auto& child : node.children) {
            if (child->kind == HirKind::Function) {
                continue;
            }
            executeNode(*child);
            if (controlSignal_ == ControlSignal::Return) {
                controlSignal_ = ControlSignal::None;
                return;
            }
        }
    }

    void executeFunction(const HirNode& node) {
        (void)callFunction(node, {}, false);
    }

    FunctionCallResult callFunction(const HirNode& node,
                                    const std::vector<RuntimeValue>& arguments,
                                    bool captureResultFrame,
                                    std::optional<size_t> requestedOutputCount =
                                        std::nullopt) {
        const FunctionSignature signature = parseFunctionSignature(node);
        std::vector<ArgumentContractRef> contracts;
        collectArgumentContracts(node, contracts);
        auto contractFor = [&](std::string_view parameter,
                               ArgumentBlockKind blockKind) -> const HirNode* {
            const auto found = std::find_if(
                contracts.begin(), contracts.end(),
                [&](const ArgumentContractRef& contract) {
                    return contract.blockKind == blockKind &&
                           contract.declaration->label == parameter;
                });
            return found == contracts.end() ? nullptr : found->declaration;
        };
        const size_t outputCount =
            requestedOutputCount.value_or(signature.outputs.size());
        const auto missingOutputs = [&] {
            return FunctionCallResult{
                std::vector<RuntimeValue>(outputCount, missingValue())};
        };
        if (std::any_of(contracts.begin(), contracts.end(),
                        [](const ArgumentContractRef& contract) {
                            return contract.blockKind ==
                                       ArgumentBlockKind::Output ||
                                   contract.blockKind ==
                                       ArgumentBlockKind::RepeatingOutput;
                        })) {
            addDiagnostic(node,
                          "output arguments blocks are not executable yet for: " +
                              node.label);
            return missingOutputs();
        }

        std::vector<std::string> nameValueDeclarations;
        for (const auto& contract : contracts) {
            if (contract.blockKind == ArgumentBlockKind::Input &&
                contract.declaration->label.find('.') != std::string::npos) {
                nameValueDeclarations.push_back(contract.declaration->label);
            }
        }
        auto normalized = normalizeRuntimeInvocationArguments(
            signature, nameValueDeclarations, arguments);
        if (!normalized.succeeded) {
            addDiagnostic(node, "function invocation failed for " + node.label +
                                    ": " + normalized.error);
            return missingOutputs();
        }
        const auto& positionalArguments = normalized.positionalArguments;

        const size_t fixedParameterCount =
            functionPositionalParameterCount(signature);
        const size_t repeatingGroupWidth =
            functionRepeatingParameterCount(signature);
        const size_t repeatingValueCount =
            positionalArguments.size() > fixedParameterCount
                ? positionalArguments.size() - fixedParameterCount
                : 0;
        if (!signature.hasVarargout && outputCount > signature.outputs.size()) {
            addDiagnostic(node, "function output count mismatch for: " +
                                    node.label);
            return missingOutputs();
        }

        frames_.push_back({});
        currentFrame()["nargin"] =
            numberValue(static_cast<double>(
                normalized.positionalArgumentCount));
        currentFrame()["nargout"] =
            numberValue(static_cast<double>(outputCount));
        auto validateValue = [&](RuntimeValue value, const HirNode* contract,
                                 std::optional<size_t> occurrence =
                                     std::nullopt)
            -> std::optional<RuntimeValue> {
            if (contract == nullptr) {
                return value;
            }
            auto validation =
                validateRuntimeArgument(std::move(value), contract->property);
            if (!validation.succeeded) {
                std::string argumentName = contract->label;
                if (occurrence) {
                    argumentName += "{" + std::to_string(*occurrence + 1) +
                                    "}";
                }
                addDiagnostic(*contract,
                              "argument validation failed for " + node.label +
                                  "." + argumentName + ": " +
                                  std::move(validation.error));
                return std::nullopt;
            }
            return std::move(validation.value);
        };

        for (size_t index = 0; index < fixedParameterCount; ++index) {
            const std::string& parameterName = signature.parameters[index];
            const HirNode* contract =
                contractFor(parameterName, ArgumentBlockKind::Input);
            RuntimeValue value;
            if (index < positionalArguments.size()) {
                value = positionalArguments[index];
            } else if (contract != nullptr &&
                       contract->property.hasExplicitDefault &&
                       !contract->children.empty()) {
                const size_t diagnosticCount = diagnostics_.size();
                value = evaluate(*contract->children.front());
                if (diagnostics_.size() != diagnosticCount) {
                    frames_.pop_back();
                    return missingOutputs();
                }
            } else {
                addDiagnostic(node, "required argument is missing for " +
                                        node.label + ": " + parameterName);
                frames_.pop_back();
                return missingOutputs();
            }

            auto validated = validateValue(std::move(value), contract);
            if (!validated) {
                frames_.pop_back();
                return missingOutputs();
            }
            currentFrame()[parameterName] = std::move(*validated);
        }

        if (repeatingGroupWidth != 0) {
            const size_t occurrenceCount =
                positionalArguments.size() < fixedParameterCount
                    ? 0
                    : repeatingValueCount / repeatingGroupWidth;
            size_t groupIndex = 0;
            for (size_t index = fixedParameterCount;
                 index < signature.parameters.size(); ++index) {
                if (functionParameterKind(signature, index) !=
                    FunctionParameterKind::Repeating) {
                    continue;
                }
                const std::string& parameterName = signature.parameters[index];
                const HirNode* contract = contractFor(
                    parameterName, ArgumentBlockKind::RepeatingInput);
                std::vector<RuntimeValue> values;
                values.reserve(occurrenceCount);
                for (size_t occurrence = 0; occurrence < occurrenceCount;
                     ++occurrence) {
                    const size_t argumentIndex =
                        fixedParameterCount +
                        occurrence * repeatingGroupWidth + groupIndex;
                    auto validated = validateValue(
                        positionalArguments[argumentIndex], contract,
                        occurrence);
                    if (!validated) {
                        frames_.pop_back();
                        return missingOutputs();
                    }
                    values.push_back(std::move(*validated));
                }
                currentFrame()[parameterName] = cellValue(std::move(values));
                ++groupIndex;
            }
        }

        FunctionControlContext controlContext(loopDepth_, controlSignal_);
        if (signature.hasVarargin) {
            const HirNode* contract = contractFor(
                "varargin", ArgumentBlockKind::RepeatingInput);
            std::vector<RuntimeValue> values;
            const size_t begin =
                repeatingGroupWidth == 0
                    ? std::min(positionalArguments.size(), fixedParameterCount)
                    : positionalArguments.size();
            values.reserve(positionalArguments.size() - begin);
            for (size_t index = begin; index < positionalArguments.size();
                 ++index) {
                auto validated =
                    validateValue(positionalArguments[index], contract,
                                  index - begin);
                if (!validated) {
                    frames_.pop_back();
                    return missingOutputs();
                }
                values.push_back(std::move(*validated));
            }
            currentFrame()["varargin"] = cellValue(std::move(values));
        }

        std::map<std::string, RuntimeValue> nameValueStructures;
        for (size_t index = 0; index < signature.parameters.size(); ++index) {
            if (functionParameterKind(signature, index) ==
                FunctionParameterKind::NameValue) {
                nameValueStructures.emplace(
                    signature.parameters[index], makeRuntimeStructValue());
            }
        }
        for (const auto& contractRef : contracts) {
            const HirNode& contract = *contractRef.declaration;
            const size_t dot = contract.label.find('.');
            if (contractRef.blockKind != ArgumentBlockKind::Input ||
                dot == std::string::npos) {
                continue;
            }
            const std::string root = contract.label.substr(0, dot);
            const std::string field = contract.label.substr(dot + 1);
            auto structure = nameValueStructures.find(root);
            if (structure == nameValueStructures.end()) {
                continue;
            }

            std::optional<RuntimeValue> value;
            if (const auto supplied =
                    normalized.nameValueArguments.find(contract.label);
                supplied != normalized.nameValueArguments.end()) {
                value = supplied->second;
            } else if (contract.property.hasExplicitDefault &&
                       !contract.children.empty()) {
                const size_t diagnosticCount = diagnostics_.size();
                value = evaluate(*contract.children.front());
                if (diagnostics_.size() != diagnosticCount) {
                    frames_.pop_back();
                    return missingOutputs();
                }
            }
            if (!value) {
                continue;
            }
            auto validated = validateValue(std::move(*value), &contract);
            if (!validated) {
                frames_.pop_back();
                return missingOutputs();
            }
            structure->second.fields[field] = std::move(*validated);
        }
        for (auto& [name, structure] : nameValueStructures) {
            currentFrame()[name] = std::move(structure);
        }
        for (const auto& output : signature.outputs) {
            currentFrame()[output] = missingValue();
        }
        if (signature.hasVarargout) {
            currentFrame()["varargout"] = cellValue({});
        }
        currentFrame()["nargin"] =
            numberValue(static_cast<double>(
                normalized.positionalArgumentCount));
        currentFrame()["nargout"] =
            numberValue(static_cast<double>(outputCount));

        executeChildren(node);

        auto completedFrame = std::move(currentFrame());
        frames_.pop_back();

        FunctionCallResult result;
        result.outputs.reserve(outputCount);
        for (size_t index = 0; index < outputCount; ++index) {
            if (index < signature.outputs.size()) {
                const auto output = completedFrame.find(signature.outputs[index]);
                result.outputs.push_back(output == completedFrame.end()
                                             ? missingValue()
                                             : output->second);
                continue;
            }
            const auto varargout = completedFrame.find("varargout");
            const size_t variableIndex = index - signature.outputs.size();
            result.outputs.push_back(
                varargout != completedFrame.end() && isCell(varargout->second) &&
                        variableIndex < varargout->second.cells.size()
                    ? varargout->second.cells[variableIndex]
                    : missingValue());
        }

        if (captureResultFrame) {
            resultFrame_ = std::move(completedFrame);
        }
        return result;
    }

    void executeNode(const HirNode& node) {
        switch (node.kind) {
        case HirKind::Module:
            executeModule(node);
            break;
        case HirKind::Function:
            executeFunction(node);
            break;
        case HirKind::Statement:
            if (executeControlStatement(node)) {
                break;
            }
            executeChildren(node);
            break;
        case HirKind::ControlHeader:
            executeChildren(node);
            break;
        case HirKind::Assignment:
            executeAssignment(node);
            break;
        case HirKind::Control:
            executeControl(node);
            break;
        case HirKind::Class:
        case HirKind::ArgumentBlock:
        case HirKind::Argument:
        case HirKind::Import:
        case HirKind::Property:
        case HirKind::Event:
        case HirKind::EnumerationMember:
        case HirKind::MethodPrototype:
        case HirKind::ControlArm:
        case HirKind::OutputList:
        case HirKind::ParameterList:
            break;
        case HirKind::NameRef:
        case HirKind::Literal:
        case HirKind::Unary:
        case HirKind::Binary:
        case HirKind::Postfix:
        case HirKind::Matrix:
        case HirKind::MatrixRow:
        case HirKind::Cell:
        case HirKind::MemberAccess:
        case HirKind::NameValueArgument:
        case HirKind::CallOrIndex:
        case HirKind::SuperclassCall:
        case HirKind::BraceIndex:
        case HirKind::FunctionHandle:
        case HirKind::MetaClass:
        case HirKind::Unknown:
            (void)evaluate(node);
            break;
        }
    }

    void executeChildren(const HirNode& node) {
        for (const auto& child : node.children) {
            executeNode(*child);
            if (controlSignal_ != ControlSignal::None ||
                diagnosticTrapTriggered()) {
                return;
            }
        }
    }

    void executeRange(const HirNode& node, size_t begin, size_t end) {
        for (size_t index = begin; index < end && index < node.children.size();
             ++index) {
            executeNode(*node.children[index]);
            if (controlSignal_ != ControlSignal::None ||
                diagnosticTrapTriggered()) {
                return;
            }
        }
    }

    bool executeControlStatement(const HirNode& node) {
        if (node.label != "break" && node.label != "continue" &&
            node.label != "return") {
            return false;
        }

        if (node.label == "return") {
            controlSignal_ = ControlSignal::Return;
            return true;
        }

        if (loopDepth_ == 0) {
            addDiagnostic(node,
                          node.label +
                              " statement is only valid inside a loop");
            return true;
        }

        controlSignal_ = node.label == "break" ? ControlSignal::Break
                                                : ControlSignal::Continue;
        return true;
    }

    void executeAssignment(const HirNode& node) {
        if (node.children.size() < 2) {
            addDiagnostic(node, "assignment is missing a right-hand value");
            return;
        }

        const HirNode& target = *node.children.front();
        const HirNode& value = *node.children[1];
        if (target.kind == HirKind::OutputList) {
            assignTargetList(target,
                             evaluateValues(value, target.children.size()));
            return;
        }

        const bool nullAssignment =
            target.kind == HirKind::CallOrIndex &&
            value.kind == HirKind::Matrix && value.children.empty();
        assignTarget(target, evaluate(value), nullAssignment);
    }

    std::vector<RuntimeValue> evaluateValues(const HirNode& node,
                                             size_t requestedOutputCount) {
        if (node.kind == HirKind::CallOrIndex &&
            (node.binding.kind == BindingKind::Function ||
             node.binding.kind == BindingKind::Builtin)) {
            if (node.children.empty()) {
                return {missingValue()};
            }

            const HirNode& callee = *node.children.front();
            const std::vector<RuntimeValue> arguments =
                evaluateArguments(node);
            if (node.binding.kind == BindingKind::Function) {
                return callLocalFunction(node, callee.label, arguments,
                                         requestedOutputCount)
                    .outputs;
            }
            return callBuiltin(node, callee.label, arguments,
                               requestedOutputCount)
                .outputs;
        }

        return {evaluate(node)};
    }

    void assignTargetList(const HirNode& target,
                          const std::vector<RuntimeValue>& values) {
        if (values.size() < target.children.size()) {
            addDiagnostic(target,
                          "not enough values to assign output list");
        }

        for (size_t index = 0; index < target.children.size(); ++index) {
            const RuntimeValue value =
                index < values.size() ? values[index] : missingValue();
            assignTarget(*target.children[index], value);
        }
    }

    void assignTarget(const HirNode& target, const RuntimeValue& value,
                      bool nullAssignment = false) {
        switch (target.kind) {
        case HirKind::NameRef:
            currentFrame()[target.label] = value;
            break;
        case HirKind::OutputList:
            for (const auto& child : target.children) {
                assignTarget(*child, value);
            }
            break;
        case HirKind::Literal:
            if (target.label == "~") {
                break;
            }
            addDiagnostic(target, "unsupported assignment target");
            break;
        case HirKind::MemberAccess:
            assignMemberTarget(target, value);
            break;
        case HirKind::CallOrIndex:
            assignIndexedTarget(target, value, nullAssignment);
            break;
        case HirKind::BraceIndex:
            assignBraceIndexedTarget(target, value);
            break;
        default:
            addDiagnostic(target, "unsupported assignment target");
            break;
        }
    }

    void assignMemberTarget(const HirNode& target,
                            const RuntimeValue& value) {
        if (target.children.empty() ||
            target.children.front()->kind != HirKind::NameRef) {
            addDiagnostic(target,
                          "structure member assignment currently requires a "
                          "variable target");
            return;
        }
        const std::string& name = target.children.front()->label;
        const auto variable = currentFrame().find(name);
        if (variable == currentFrame().end() || !isStruct(variable->second)) {
            addDiagnostic(target,
                          "member assignment requires a structure target: " +
                              name);
            return;
        }
        variable->second.fields[target.label] = value;
    }

    void assignIndexedTarget(const HirNode& target, const RuntimeValue& value,
                             bool nullAssignment) {
        if (target.children.empty()) {
            addDiagnostic(target, "indexed assignment is missing a target");
            return;
        }
        if (!isNumeric(value)) {
            addDiagnostic(target,
                          "indexed assignment requires a numeric value");
            return;
        }

        const HirNode& callee = *target.children.front();
        if (callee.kind != HirKind::NameRef) {
            addDiagnostic(target,
                          "indexed assignment currently requires a variable "
                          "target");
            return;
        }

        auto variable = currentFrame().find(callee.label);
        if (variable == currentFrame().end()) {
            addDiagnostic(target,
                          "indexed assignment target is not defined: " +
                              callee.label);
            return;
        }
        RuntimeValue& targetValue = variable->second;
        if (!isNumeric(targetValue)) {
            addDiagnostic(target,
                          "indexed assignment requires a numeric target");
            return;
        }

        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(target, targetValue);
        if (arguments.empty()) {
            addDiagnostic(target,
                          "indexed assignment requires subscripts");
            return;
        }
        for (const auto& argument : arguments) {
            if (!isNumeric(argument)) {
                addDiagnostic(target,
                              "indexed assignment requires numeric subscripts");
                return;
            }
        }

        RuntimeNumericAssignmentResult result;
        if (nullAssignment) {
            std::vector<bool> colonSubscripts;
            colonSubscripts.reserve(target.children.size() - 1);
            for (size_t index = 1; index < target.children.size(); ++index) {
                const HirNode& subscript = *target.children[index];
                colonSubscripts.push_back(
                    subscript.kind == HirKind::Literal &&
                    subscript.label == ":");
            }
            result = runtimeDeleteNumericIndexed(
                targetValue, arguments, colonSubscripts);
        } else {
            result = runtimeAssignNumericIndexed(
                targetValue, arguments, value);
        }
        if (!result.succeeded) {
            addDiagnostic(target, result.error);
        }
    }

    void assignBraceIndexedTarget(const HirNode& target,
                                  const RuntimeValue& value) {
        if (target.children.empty() ||
            target.children.front()->kind != HirKind::NameRef) {
            addDiagnostic(target,
                          "brace assignment currently requires a variable target");
            return;
        }

        auto variable = currentFrame().find(target.children.front()->label);
        if (variable == currentFrame().end()) {
            addDiagnostic(target, "brace assignment target is not defined: " +
                                      target.children.front()->label);
            return;
        }
        RuntimeValue& cell = variable->second;
        if (!isCell(cell)) {
            addDiagnostic(target, "brace assignment requires a cell target");
            return;
        }

        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(target, cell);
        std::optional<size_t> storageOffset;
        if (arguments.size() == 1 && isNumber(arguments.front())) {
            const double rawIndex = arguments.front().number;
            const auto oneBasedIndex =
                checkedRuntimeNonnegativeInteger(rawIndex);
            if (!oneBasedIndex || *oneBasedIndex == 0) {
                addDiagnostic(target,
                              "cell index must be a positive integer");
                return;
            }
            const size_t index = *oneBasedIndex - 1;
            if (index >= cell.cells.size()) {
                if (runtimeDimensionCount(cell) > 2 || rowCount(cell) != 1) {
                    addDiagnostic(
                        target,
                        "cell linear growth currently requires a row cell array");
                    return;
                }
                cell.cells.resize(index + 1, missingValue());
                setRuntimeDimensions(cell, {1, cell.cells.size()});
                storageOffset = index;
            } else {
                storageOffset = runtimeColumnMajorLinearToStorageOffset(
                    cell, index);
            }
        } else {
            storageOffset = checkedCellStorageOffset(target, cell, arguments);
        }
        if (!storageOffset) {
            return;
        }
        cell.cells[*storageOffset] = value;
    }

    void executeControl(const HirNode& node) {
        if (node.label == "for" || node.label == "parfor") {
            executeFor(node);
            return;
        }
        if (node.label == "if") {
            executeIf(node);
            return;
        }
        if (node.label == "while") {
            executeWhile(node);
            return;
        }
        if (node.label == "switch") {
            executeSwitch(node);
            return;
        }
        if (node.label == "try") {
            executeTry(node);
            return;
        }

        addDiagnostic(node, "control block is not executable yet: " + node.label);
    }

    void executeFor(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            addDiagnostic(node, "for loop is missing a header");
            return;
        }

        const HirNode* header = node.children.front().get();
        if (header->children.empty() ||
            header->children.front()->kind != HirKind::Assignment) {
            addDiagnostic(*header, "for loop header must assign a range");
            return;
        }

        const HirNode& assignment = *header->children.front();
        if (assignment.children.size() < 2 ||
            assignment.children.front()->kind != HirKind::NameRef) {
            addDiagnostic(assignment, "for loop variable is not supported");
            return;
        }

        const std::string loopVariable = assignment.children.front()->label;
        const RuntimeValue range = evaluate(*assignment.children[1]);
        const std::vector<double> values = valuesForLoopRange(range);

        LoopDepthGuard loop(loopDepth_);
        for (double value : values) {
            currentFrame()[loopVariable] =
                numberValue(value, range.numericClass);
            executeRange(node, 1, node.children.size());
            if (diagnosticTrapTriggered()) {
                return;
            }
            if (controlSignal_ == ControlSignal::Break) {
                controlSignal_ = ControlSignal::None;
                break;
            }
            if (controlSignal_ == ControlSignal::Continue) {
                controlSignal_ = ControlSignal::None;
                continue;
            }
            if (controlSignal_ != ControlSignal::None) {
                return;
            }
        }
    }

    std::vector<double> valuesForLoopRange(const RuntimeValue& range) const {
        if (isArray(range)) {
            return range.elements;
        }
        if (isNumber(range)) {
            return {range.number};
        }
        return {};
    }

    void executeIf(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            addDiagnostic(node, "if block is missing a condition");
            return;
        }

        std::vector<size_t> arms;
        for (size_t index = 1; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm) {
                arms.push_back(index);
            }
        }

        const size_t firstArm =
            arms.empty() ? node.children.size() : arms.front();
        if (truthy(evaluateHeader(*node.children.front()))) {
            executeRange(node, 1, firstArm);
            return;
        }

        for (size_t armIndex = 0; armIndex < arms.size(); ++armIndex) {
            const size_t current = arms[armIndex];
            const size_t next = armIndex + 1 < arms.size()
                                    ? arms[armIndex + 1]
                                    : node.children.size();
            const HirNode& arm = *node.children[current];
            if (arm.label == "else") {
                executeRange(node, current + 1, next);
                return;
            }
            if (arm.label == "elseif" && !arm.children.empty() &&
                truthy(evaluateHeader(*arm.children.front()))) {
                executeRange(node, current + 1, next);
                return;
            }
        }
    }

    void executeWhile(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            addDiagnostic(node, "while loop is missing a condition");
            return;
        }

        const HirNode& header = *node.children.front();
        size_t iterations = 0;
        LoopDepthGuard loop(loopDepth_);
        while (truthy(evaluateHeader(header))) {
            if (++iterations > kMaxWhileIterations) {
                addDiagnostic(node,
                              "while loop exceeded the interpreter iteration "
                              "limit");
                return;
            }
            executeRange(node, 1, node.children.size());
            if (diagnosticTrapTriggered()) {
                return;
            }
            if (controlSignal_ == ControlSignal::Break) {
                controlSignal_ = ControlSignal::None;
                break;
            }
            if (controlSignal_ == ControlSignal::Continue) {
                controlSignal_ = ControlSignal::None;
                continue;
            }
            if (controlSignal_ != ControlSignal::None) {
                return;
            }
        }
    }

    void executeSwitch(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            addDiagnostic(node, "switch block is missing a selector");
            return;
        }

        const RuntimeValue selector = evaluateHeader(*node.children.front());
        std::optional<size_t> otherwiseArm;
        for (size_t index = 1; index < node.children.size(); ++index) {
            if (node.children[index]->kind != HirKind::ControlArm) {
                continue;
            }

            const HirNode& arm = *node.children[index];
            if (arm.label == "otherwise") {
                otherwiseArm = index;
                continue;
            }
            if (arm.label != "case") {
                continue;
            }

            if (arm.children.empty() ||
                arm.children.front()->kind != HirKind::ControlHeader) {
                addDiagnostic(arm, "case arm is missing a value");
                return;
            }

            if (caseMatches(selector, evaluateHeader(*arm.children.front()))) {
                executeRange(node, index + 1, nextControlArm(node, index + 1));
                return;
            }
        }

        if (otherwiseArm) {
            executeRange(node, *otherwiseArm + 1,
                         nextControlArm(node, *otherwiseArm + 1));
        }
    }

    void executeTry(const HirNode& node) {
        std::optional<size_t> catchArm;
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm &&
                node.children[index]->label == "catch") {
                catchArm = index;
                break;
            }
        }

        const size_t bodyBegin =
            (!node.children.empty() &&
             node.children.front()->kind == HirKind::ControlHeader)
                ? 1
                : 0;
        const size_t bodyEnd = catchArm.value_or(node.children.size());
        const size_t diagnosticBase = diagnostics_.size();
        {
            DiagnosticTrapGuard trap(diagnosticTrapBase_, diagnosticBase);
            executeRange(node, bodyBegin, bodyEnd);
        }

        if (controlSignal_ != ControlSignal::None) {
            return;
        }
        if (diagnostics_.size() == diagnosticBase) {
            return;
        }
        if (!catchArm) {
            return;
        }

        const std::string message = diagnostics_.back().message;
        diagnostics_.resize(diagnosticBase);

        const HirNode& arm = *node.children[*catchArm];
        if (const auto name = catchVariableName(arm)) {
            currentFrame()[*name] = stringValue(message);
        }
        executeRange(node, *catchArm + 1, nextControlArm(node, *catchArm + 1));
    }

    size_t nextControlArm(const HirNode& node, size_t begin) const {
        for (size_t index = begin; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm) {
                return index;
            }
        }
        return node.children.size();
    }

    bool caseMatches(const RuntimeValue& selector,
                     const RuntimeValue& candidate) const {
        return runtimeEqual(selector, candidate);
    }

    std::optional<std::string> catchVariableName(const HirNode& arm) const {
        if (arm.children.empty() ||
            arm.children.front()->kind != HirKind::ControlHeader) {
            return std::nullopt;
        }
        return firstNameRefLabel(*arm.children.front());
    }

    std::optional<std::string> firstNameRefLabel(const HirNode& node) const {
        if (node.kind == HirKind::NameRef) {
            return node.label;
        }

        for (const auto& child : node.children) {
            if (const auto result = firstNameRefLabel(*child)) {
                return result;
            }
        }
        return std::nullopt;
    }

    RuntimeValue evaluateHeader(const HirNode& header) {
        if (header.children.empty()) {
            return missingValue();
        }
        if (header.children.front()->kind == HirKind::Assignment) {
            executeAssignment(*header.children.front());
            return currentFrame()[header.children.front()
                                      ->children.front()
                                      ->label];
        }
        return evaluate(*header.children.front());
    }

    RuntimeValue evaluate(const HirNode& node) {
        switch (node.kind) {
        case HirKind::Statement:
        case HirKind::ControlHeader:
            return evaluateTransparent(node);
        case HirKind::NameRef:
            return evaluateName(node);
        case HirKind::Literal:
            return evaluateLiteral(node);
        case HirKind::Unary:
            return evaluateUnary(node);
        case HirKind::Binary:
            return evaluateBinary(node);
        case HirKind::Matrix:
            return evaluateMatrix(node);
        case HirKind::MatrixRow:
            return evaluateMatrixRow(node);
        case HirKind::Cell:
            return evaluateCell(node);
        case HirKind::MemberAccess:
            return evaluateMemberAccess(node);
        case HirKind::NameValueArgument:
            return evaluateNameValueArgument(node);
        case HirKind::CallOrIndex:
            return evaluateCallOrIndex(node);
        case HirKind::BraceIndex:
            return evaluateBraceIndex(node);
        case HirKind::Assignment:
            executeAssignment(node);
            return missingValue();
        case HirKind::Postfix:
            return evaluatePostfix(node);
        case HirKind::Module:
        case HirKind::Class:
        case HirKind::Function:
        case HirKind::ArgumentBlock:
        case HirKind::Argument:
        case HirKind::Import:
        case HirKind::Property:
        case HirKind::Event:
        case HirKind::EnumerationMember:
        case HirKind::MethodPrototype:
        case HirKind::Control:
        case HirKind::ControlArm:
        case HirKind::OutputList:
        case HirKind::ParameterList:
        case HirKind::SuperclassCall:
        case HirKind::FunctionHandle:
        case HirKind::MetaClass:
        case HirKind::Unknown:
            addDiagnostic(node, "expression is not executable yet: " +
                                    std::string(hirKindName(node.kind)));
            return missingValue();
        }
        return missingValue();
    }

    RuntimeValue evaluateTransparent(const HirNode& node) {
        RuntimeValue result = missingValue();
        for (const auto& child : node.children) {
            result = evaluate(*child);
        }
        return result;
    }

    RuntimeValue evaluateCell(const HirNode& node) {
        std::vector<RuntimeValue> values;
        values.reserve(node.children.size());
        for (const auto& child : node.children) {
            values.push_back(evaluate(*child));
        }
        return cellValue(std::move(values));
    }

    RuntimeValue evaluateNameValueArgument(const HirNode& node) {
        if (node.children.size() != 1) {
            addDiagnostic(node, "name=value argument requires one value");
            return missingValue();
        }
        return makeRuntimeNameValueArgument(node.label,
                                            evaluate(*node.children.front()));
    }

    RuntimeValue evaluateMemberAccess(const HirNode& node) {
        if (node.children.empty()) {
            addDiagnostic(node, "member access is missing a target");
            return missingValue();
        }
        const RuntimeValue target = evaluate(*node.children.front());
        if (!isStruct(target)) {
            addDiagnostic(node,
                          "member access requires a structure in the reference "
                          "interpreter");
            return missingValue();
        }
        const auto field = target.fields.find(node.label);
        if (field == target.fields.end()) {
            addDiagnostic(node, "structure field is not available: " +
                                    node.label);
            return missingValue();
        }
        return field->second;
    }

    RuntimeValue evaluatePostfix(const HirNode& node) {
        if (node.children.empty()) {
            return missingValue();
        }

        const RuntimeValue value = evaluate(*node.children.front());
        if (node.label != "'") {
            addDiagnostic(node, "unsupported postfix operator: " + node.label);
            return missingValue();
        }

        if (isNumber(value)) {
            return value;
        }
        if (isVector(value)) {
            return matrixValue(value.elements.size(), 1, value.elements,
                               value.numericClass);
        }
        if (isMatrix(value)) {
            if (runtimeDimensionCount(value) > 2) {
                addDiagnostic(node,
                              "transpose requires a two-dimensional array");
                return missingValue();
            }
            std::vector<double> transposed;
            transposed.reserve(value.elements.size());
            for (size_t column = 0; column < value.columns; ++column) {
                for (size_t row = 0; row < value.rows; ++row) {
                    transposed.push_back(value.elements[row * value.columns +
                                                        column]);
                }
            }
            return matrixValue(value.columns, value.rows,
                               std::move(transposed), value.numericClass);
        }

        addDiagnostic(node, "transpose requires numeric input");
        return missingValue();
    }

    RuntimeValue evaluateName(const HirNode& node) {
        const auto variable = currentFrame().find(node.label);
        if (variable != currentFrame().end()) {
            return variable->second;
        }

        if (node.binding.kind == BindingKind::Builtin) {
            if (node.label == "clear" || node.label == "clc" ||
                node.label == "tic" || node.label == "toc") {
                return firstOutput(callBuiltin(node, node.label, {}, 1));
            }
            if (node.label == "pi") {
                return numberValue(3.14159265358979323846);
            }
            if (node.label == "eps") {
                return numberValue(std::numeric_limits<double>::epsilon());
            }
            if (node.label == "inf") {
                return numberValue(std::numeric_limits<double>::infinity());
            }
            if (node.label == "nan") {
                return numberValue(std::numeric_limits<double>::quiet_NaN());
            }
            if (node.label == "true") {
                return logicalValue(true);
            }
            if (node.label == "false") {
                return logicalValue(false);
            }
        }

        addDiagnostic(node, "unknown runtime variable: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateLiteral(const HirNode& node) {
        if (node.label == "end" || node.label == ":") {
            addDiagnostic(node, "literal is not executable in this context: " +
                                    node.label);
            return missingValue();
        }

        if (node.raw.size() >= 2 &&
            (node.raw.front() == '\'' || node.raw.front() == '"')) {
            return stringValue(decodeStringLiteral(node.raw));
        }

        if (const auto number = parseNumber(node.label)) {
            return numberValue(*number);
        }

        addDiagnostic(node, "unsupported literal: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateUnary(const HirNode& node) {
        if (node.children.empty()) {
            return missingValue();
        }

        const RuntimeValue value = evaluate(*node.children.front());
        if (!isNumeric(value)) {
            addDiagnostic(node, "unary operator requires numeric input");
            return missingValue();
        }

        if (node.label == "+") {
            return mapUnary(value, [](double element) { return element; });
        }
        if (node.label == "-") {
            return mapUnary(value, [](double element) { return -element; });
        }
        if (node.label == "~") {
            return mapUnary(value, [](double element) {
                return element == 0.0 ? 1.0 : 0.0;
            }, RuntimeNumericClass::Logical);
        }

        addDiagnostic(node, "unsupported unary operator: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateMatrix(const HirNode& node) {
        if (node.children.empty()) {
            return matrixValue(0, 0, {});
        }

        if (node.children.front()->kind != HirKind::MatrixRow) {
            std::vector<double> elements;
            std::optional<RuntimeNumericClass> numericClass;
            for (const auto& child : node.children) {
                if (!appendNumericElements(*child, elements,
                                           numericClass)) {
                    return missingValue();
                }
            }
            return vectorValue(
                std::move(elements),
                numericClass.value_or(RuntimeNumericClass::Double));
        }

        std::vector<double> elements;
        size_t columns = 0;
        std::optional<RuntimeNumericClass> numericClass;
        for (const auto& child : node.children) {
            const RuntimeValue value = evaluate(*child);
            if (isVector(value)) {
                if (columns == 0) {
                    columns = value.elements.size();
                } else if (columns != value.elements.size()) {
                    addDiagnostic(*child,
                                  "matrix rows must have the same length");
                    return missingValue();
                }
                if (!numericClass) {
                    numericClass = value.numericClass;
                }
                for (const double element : value.elements) {
                    const auto converted = runtimeCoerceNumericElement(
                        element, *numericClass);
                    if (!converted) {
                        addDiagnostic(
                            *child,
                            "matrix literal cannot convert NaN to logical");
                        return missingValue();
                    }
                    elements.push_back(*converted);
                }
                continue;
            }

            addDiagnostic(*child, "matrix row did not produce a row vector");
            return missingValue();
        }

        if (node.children.size() == 1) {
            return vectorValue(
                std::move(elements),
                numericClass.value_or(RuntimeNumericClass::Double));
        }
        return matrixValue(
            node.children.size(), columns, std::move(elements),
            numericClass.value_or(RuntimeNumericClass::Double));
    }

    RuntimeValue evaluateMatrixRow(const HirNode& node) {
        std::vector<double> elements;
        std::optional<RuntimeNumericClass> numericClass;
        for (const auto& child : node.children) {
            if (!appendNumericElements(*child, elements, numericClass)) {
                return missingValue();
            }
        }

        return vectorValue(
            std::move(elements),
            numericClass.value_or(RuntimeNumericClass::Double));
    }

    bool appendNumericElements(
        const HirNode& node, std::vector<double>& elements,
        std::optional<RuntimeNumericClass>& numericClass) {
        const RuntimeValue value = evaluate(node);
        if (!isNumeric(value)) {
            addDiagnostic(node,
                          "matrix literal currently supports numeric values");
            return false;
        }

        if (!numericClass) {
            numericClass = value.numericClass;
        }
        const size_t count = isNumber(value) ? 1 : value.elements.size();
        for (size_t index = 0; index < count; ++index) {
            const double element =
                isNumber(value) ? value.number : value.elements[index];
            const auto converted =
                runtimeCoerceNumericElement(element, *numericClass);
            if (!converted) {
                addDiagnostic(node,
                              "matrix literal cannot convert NaN to logical");
                return false;
            }
            elements.push_back(*converted);
        }
        return true;
    }

    RuntimeValue evaluateBinary(const HirNode& node) {
        if (node.label == ":") {
            return evaluateColon(node);
        }
        if (node.children.size() < 2) {
            return missingValue();
        }

        const size_t leftDiagnosticBase = diagnostics_.size();
        const RuntimeValue left = evaluate(*node.children[0]);
        if (node.label == "&&" || node.label == "||") {
            if (!isNumeric(left)) {
                if (diagnostics_.size() == leftDiagnosticBase) {
                    addDiagnostic(node,
                                  "short-circuit operator requires numeric or "
                                  "logical operands");
                }
                return missingValue();
            }

            const bool leftValue = truthy(left);
            if (node.label == "&&" && !leftValue) {
                return logicalValue(false);
            }
            if (node.label == "||" && leftValue) {
                return logicalValue(true);
            }

            const size_t rightDiagnosticBase = diagnostics_.size();
            const RuntimeValue right = evaluate(*node.children[1]);
            if (!isNumeric(right)) {
                if (diagnostics_.size() == rightDiagnosticBase) {
                    addDiagnostic(node,
                                  "short-circuit operator requires numeric or "
                                  "logical operands");
                }
                return missingValue();
            }
            return logicalValue(truthy(right));
        }

        const RuntimeValue right = evaluate(*node.children[1]);
        if (isString(left) || isString(right)) {
            if (isString(left) && isString(right) &&
                (node.label == "==" || node.label == "~=")) {
                const bool equal = runtimeEqual(left, right);
                return logicalValue((node.label == "==") == equal);
            }

            addDiagnostic(node,
                          "string binary operators currently support only "
                          "== and ~= between strings");
            return missingValue();
        }
        if (!isNumeric(left) || !isNumeric(right)) {
            addDiagnostic(node, "binary operator requires numeric values");
            return missingValue();
        }

        return applyBinary(node, left, right);
    }

    RuntimeValue applyBinary(const HirNode& node, const RuntimeValue& left,
                             const RuntimeValue& right) {
        if (isNumber(left) && isNumber(right)) {
            return applyScalarBinary(node, left.number, right.number);
        }

        if (node.label == "*" && isArray(left) && isArray(right)) {
            if (runtimeDimensionCount(left) > 2 ||
                runtimeDimensionCount(right) > 2) {
                addDiagnostic(node,
                              "matrix multiplication requires two-dimensional arrays");
                return missingValue();
            }
            return applyMatrixMultiply(node, left, right);
        }

        if ((node.label == "/" || node.label == "\\" || node.label == "^") &&
            isArray(left) && isArray(right)) {
            addDiagnostic(node,
                          "matrix division and matrix power are not "
                          "implemented for array operands yet");
            return missingValue();
        }

        std::vector<size_t> dimensions;
        if (isArray(left) && isArray(right)) {
            const auto expanded = runtimeImplicitExpansionDimensions(
                runtimeDimensions(left), runtimeDimensions(right));
            if (!expanded) {
                addDiagnostic(
                    node,
                    "elementwise operands have incompatible dimensions");
                return missingValue();
            }
            dimensions = *expanded;
        } else {
            dimensions = isArray(left) ? runtimeDimensions(left)
                                       : runtimeDimensions(right);
        }
        const size_t count =
            checkedRuntimeDimensionProduct(dimensions).value_or(0);
        std::vector<double> elements;
        elements.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto coordinates =
                runtimeRowMajorCoordinates(index, dimensions);
            const auto leftOffset =
                isArray(left)
                    ? runtimeImplicitExpansionStorageOffset(
                          coordinates, runtimeDimensions(left))
                    : std::optional<size_t>(0);
            const auto rightOffset =
                isArray(right)
                    ? runtimeImplicitExpansionStorageOffset(
                          coordinates, runtimeDimensions(right))
                    : std::optional<size_t>(0);
            if (!leftOffset || !rightOffset) {
                addDiagnostic(node,
                              "elementwise expansion could not map an operand");
                return missingValue();
            }
            const double leftValue =
                isArray(left) ? left.elements[*leftOffset] : left.number;
            const double rightValue =
                isArray(right) ? right.elements[*rightOffset] : right.number;
            const RuntimeValue value =
                applyScalarBinary(node, leftValue, rightValue);
            if (!isNumber(value)) {
                return missingValue();
            }
            elements.push_back(value.number);
        }

        const RuntimeNumericClass resultClass =
            isLogicalBinaryOperator(node.label)
                ? RuntimeNumericClass::Logical
                : RuntimeNumericClass::Double;
        return arrayValueForDimensions(dimensions, std::move(elements),
                                       resultClass);
    }

    RuntimeValue applyMatrixMultiply(const HirNode& node,
                                     const RuntimeValue& left,
                                     const RuntimeValue& right) {
        const size_t leftRows = rowCount(left);
        const size_t leftColumns = columnCount(left);
        const size_t rightRows = rowCount(right);
        const size_t rightColumns = columnCount(right);

        if (leftColumns != rightRows) {
            addDiagnostic(node, "matrix dimensions do not agree for *");
            return missingValue();
        }

        std::vector<double> result(leftRows * rightColumns, 0.0);
        for (size_t row = 0; row < leftRows; ++row) {
            for (size_t column = 0; column < rightColumns; ++column) {
                double total = 0.0;
                for (size_t inner = 0; inner < leftColumns; ++inner) {
                    total += matrixElement(left, row, inner) *
                             matrixElement(right, inner, column);
                }
                result[row * rightColumns + column] = total;
            }
        }

        if (leftRows == 1 && rightColumns == 1) {
            return numberValue(result.front());
        }
        return arrayValueForShape(leftRows, rightColumns, std::move(result));
    }

    double matrixElement(const RuntimeValue& value, size_t row,
                         size_t column) const {
        if (isNumber(value)) {
            return value.number;
        }
        return value.elements[row * columnCount(value) + column];
    }

    RuntimeValue applyScalarBinary(const HirNode& node, double left,
                                   double right) {
        if (node.label == "+") {
            return numberValue(left + right);
        }
        if (node.label == "-") {
            return numberValue(left - right);
        }
        if (node.label == "*" || node.label == ".*") {
            return numberValue(left * right);
        }
        if (node.label == "/" || node.label == "./") {
            return numberValue(left / right);
        }
        if (node.label == "^" || node.label == ".^") {
            return numberValue(std::pow(left, right));
        }
        if (node.label == ">") {
            return logicalValue(left > right);
        }
        if (node.label == "<") {
            return logicalValue(left < right);
        }
        if (node.label == ">=") {
            return logicalValue(left >= right);
        }
        if (node.label == "<=") {
            return logicalValue(left <= right);
        }
        if (node.label == "==") {
            return logicalValue(left == right);
        }
        if (node.label == "~=") {
            return logicalValue(left != right);
        }
        if (node.label == "&" || node.label == "&&") {
            return logicalValue(truthy(numberValue(left)) &&
                                truthy(numberValue(right)));
        }
        if (node.label == "|" || node.label == "||") {
            return logicalValue(truthy(numberValue(left)) ||
                                truthy(numberValue(right)));
        }

        addDiagnostic(node, "unsupported binary operator: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateColon(const HirNode& node) {
        std::vector<double> terms;
        collectColonTerms(node, terms);
        const auto range = runtimePlanColonRange(terms);
        if (!range.succeeded) {
            addDiagnostic(node, range.error);
            return range.error == "colon range step cannot be zero"
                       ? vectorValue({})
                       : missingValue();
        }
        return vectorValue(runtimeMaterializeColonRange(range));
    }

    void collectColonTerms(const HirNode& node, std::vector<double>& terms) {
        if (node.kind == HirKind::Binary && node.label == ":") {
            for (const auto& child : node.children) {
                collectColonTerms(*child, terms);
            }
            return;
        }

        const RuntimeValue value = evaluate(node);
        if (!isNumber(value)) {
            addDiagnostic(node, "colon operand must be a scalar number");
            return;
        }
        terms.push_back(value.number);
    }

    std::vector<RuntimeValue> evaluateArguments(const HirNode& node) {
        std::vector<RuntimeValue> arguments;
        for (size_t index = 1; index < node.children.size(); ++index) {
            arguments.push_back(evaluate(*node.children[index]));
        }
        return arguments;
    }

    std::vector<RuntimeValue> evaluateIndexArguments(
        const HirNode& node, const RuntimeValue& target) {
        std::vector<RuntimeValue> arguments;
        const size_t total = node.children.size() - 1;
        for (size_t index = 1; index < node.children.size(); ++index) {
            arguments.push_back(evaluateWithIndexContext(
                *node.children[index], target, index - 1, total));
        }
        return arguments;
    }

    RuntimeValue evaluateWithIndexContext(const HirNode& node,
                                          const RuntimeValue& target,
                                          size_t position, size_t total) {
        switch (node.kind) {
        case HirKind::Statement:
        case HirKind::ControlHeader: {
            RuntimeValue result = missingValue();
            for (const auto& child : node.children) {
                result = evaluateWithIndexContext(*child, target, position,
                                                  total);
            }
            return result;
        }
        case HirKind::Literal:
            if (node.label == "end") {
                return numberValue(endValueForIndex(target, position, total));
            }
            if (node.label == ":") {
                return oneBasedIndexRange(static_cast<size_t>(
                    endValueForIndex(target, position, total)));
            }
            return evaluateLiteral(node);
        case HirKind::Unary:
            return evaluateUnaryWithIndexContext(node, target, position, total);
        case HirKind::Binary:
            return evaluateBinaryWithIndexContext(node, target, position, total);
        case HirKind::Matrix:
            return evaluateMatrixWithIndexContext(node, target, position, total);
        case HirKind::MatrixRow:
            return evaluateMatrixRowWithIndexContext(node, target, position,
                                                     total);
        default:
            return evaluate(node);
        }
    }

    double endValueForIndex(const RuntimeValue& target, size_t position,
                            size_t total) const {
        const auto dimensions =
            runtimeEffectiveSubscriptDimensions(target, total);
        return position < dimensions.size()
                   ? static_cast<double>(dimensions[position])
                   : 1.0;
    }

    RuntimeValue evaluateUnaryWithIndexContext(const HirNode& node,
                                               const RuntimeValue& target,
                                               size_t position, size_t total) {
        if (node.children.empty()) {
            return missingValue();
        }

        const RuntimeValue value =
            evaluateWithIndexContext(*node.children.front(), target, position,
                                     total);
        if (!isNumeric(value)) {
            addDiagnostic(node, "unary operator requires numeric input");
            return missingValue();
        }

        if (node.label == "+") {
            return value;
        }
        if (node.label == "-") {
            return mapUnary(value, [](double element) { return -element; });
        }
        if (node.label == "~") {
            return mapUnary(value, [](double element) {
                return (element != 0.0 && !std::isnan(element)) ? 0.0 : 1.0;
            });
        }

        addDiagnostic(node, "unsupported unary operator: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateBinaryWithIndexContext(const HirNode& node,
                                                const RuntimeValue& target,
                                                size_t position, size_t total) {
        if (node.label == ":") {
            return evaluateColonWithIndexContext(node, target, position, total);
        }
        if (node.children.size() < 2) {
            return missingValue();
        }

        const RuntimeValue left =
            evaluateWithIndexContext(*node.children[0], target, position,
                                     total);
        const RuntimeValue right =
            evaluateWithIndexContext(*node.children[1], target, position,
                                     total);
        if (!isNumeric(left) || !isNumeric(right)) {
            addDiagnostic(node, "binary operator requires numeric values");
            return missingValue();
        }

        return applyBinary(node, left, right);
    }

    RuntimeValue evaluateColonWithIndexContext(const HirNode& node,
                                               const RuntimeValue& target,
                                               size_t position, size_t total) {
        std::vector<double> terms;
        collectColonTermsWithIndexContext(node, target, position, total, terms);
        const auto range = runtimePlanColonRange(terms);
        if (!range.succeeded) {
            addDiagnostic(node, range.error);
            return range.error == "colon range step cannot be zero"
                       ? vectorValue({})
                       : missingValue();
        }
        return vectorValue(runtimeMaterializeColonRange(range));
    }

    void collectColonTermsWithIndexContext(
        const HirNode& node, const RuntimeValue& target, size_t position,
        size_t total, std::vector<double>& terms) {
        if (node.kind == HirKind::Binary && node.label == ":") {
            for (const auto& child : node.children) {
                collectColonTermsWithIndexContext(*child, target, position,
                                                  total, terms);
            }
            return;
        }

        const RuntimeValue value =
            evaluateWithIndexContext(node, target, position, total);
        if (!isNumber(value)) {
            addDiagnostic(node, "colon operand must be a scalar number");
            return;
        }
        terms.push_back(value.number);
    }

    RuntimeValue evaluateMatrixWithIndexContext(const HirNode& node,
                                                const RuntimeValue& target,
                                                size_t position, size_t total) {
        if (node.children.empty()) {
            return matrixValue(0, 0, {});
        }

        if (node.children.front()->kind != HirKind::MatrixRow) {
            std::vector<double> elements;
            for (const auto& child : node.children) {
                appendNumericElementsWithIndexContext(*child, target, position,
                                                      total, elements);
            }
            return vectorValue(std::move(elements));
        }

        std::vector<double> elements;
        size_t columns = 0;
        for (const auto& child : node.children) {
            const RuntimeValue value =
                evaluateWithIndexContext(*child, target, position, total);
            if (isVector(value)) {
                if (columns == 0) {
                    columns = value.elements.size();
                } else if (columns != value.elements.size()) {
                    addDiagnostic(*child,
                                  "matrix rows must have the same length");
                    return missingValue();
                }
                elements.insert(elements.end(), value.elements.begin(),
                                value.elements.end());
                continue;
            }

            addDiagnostic(*child, "matrix row did not produce a row vector");
            return missingValue();
        }

        if (node.children.size() == 1) {
            return vectorValue(std::move(elements));
        }
        return matrixValue(node.children.size(), columns, std::move(elements));
    }

    RuntimeValue evaluateMatrixRowWithIndexContext(
        const HirNode& node, const RuntimeValue& target, size_t position,
        size_t total) {
        std::vector<double> elements;
        for (const auto& child : node.children) {
            appendNumericElementsWithIndexContext(*child, target, position,
                                                  total, elements);
        }

        return vectorValue(std::move(elements));
    }

    void appendNumericElementsWithIndexContext(
        const HirNode& node, const RuntimeValue& target, size_t position,
        size_t total, std::vector<double>& elements) {
        const RuntimeValue value =
            evaluateWithIndexContext(node, target, position, total);
        if (isNumber(value)) {
            elements.push_back(value.number);
            return;
        }
        if (isArray(value)) {
            elements.insert(elements.end(), value.elements.begin(),
                            value.elements.end());
            return;
        }

        addDiagnostic(node, "matrix literal currently supports numeric values");
    }

    RuntimeValue firstOutput(const FunctionCallResult& result) const {
        if (result.outputs.empty()) {
            return missingValue();
        }
        return result.outputs.front();
    }

    RuntimeValue evaluateCallOrIndex(const HirNode& node) {
        if (node.children.empty()) {
            return missingValue();
        }

        const HirNode& callee = *node.children.front();
        if (node.binding.kind == BindingKind::Builtin) {
            const std::vector<RuntimeValue> arguments = evaluateArguments(node);
            return firstOutput(callBuiltin(node, callee.label, arguments, 1));
        }

        if (node.binding.kind == BindingKind::Function) {
            const std::vector<RuntimeValue> arguments = evaluateArguments(node);
            return firstOutput(callLocalFunction(node, callee.label,
                                                 arguments, 1));
        }

        const RuntimeValue target = evaluate(callee);
        return evaluateIndex(node, target, evaluateIndexArguments(node, target));
    }

    FunctionCallResult
    callLocalFunction(const HirNode& node, const std::string& name,
                      const std::vector<RuntimeValue>& arguments,
                      size_t requestedOutputCount = 1) {
        const auto function = functionsByName_.find(name);
        if (function == functionsByName_.end()) {
            addDiagnostic(node, "local function is not available: " + name);
            return FunctionCallResult{{missingValue()}};
        }

        return callFunction(*function->second, arguments, false,
                            requestedOutputCount);
    }

    RuntimeValue evaluateBraceIndex(const HirNode& node) {
        if (node.children.empty()) {
            addDiagnostic(node, "brace indexing is missing a target");
            return missingValue();
        }

        const RuntimeValue target = evaluate(*node.children.front());
        if (!isCell(target)) {
            addDiagnostic(node, "brace indexing requires a cell target");
            return missingValue();
        }
        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(node, target);
        const auto storageOffset =
            checkedCellStorageOffset(node, target, arguments);
        if (!storageOffset) {
            return missingValue();
        }
        return target.cells[*storageOffset];
    }

    std::optional<size_t> checkedCellStorageOffset(
        const HirNode& node, const RuntimeValue& target,
        const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty()) {
            addDiagnostic(node, "brace indexing requires subscripts");
            return std::nullopt;
        }
        for (const auto& argument : arguments) {
            if (!isNumber(argument)) {
                addDiagnostic(node,
                              "brace indexing requires scalar numeric subscripts");
                return std::nullopt;
            }
        }

        if (arguments.size() == 1) {
            const auto index = checkedIndex(node, arguments.front().number,
                                            target.cells.size());
            if (!index) {
                return std::nullopt;
            }
            const auto storageOffset =
                runtimeColumnMajorLinearToStorageOffset(target, *index);
            if (!storageOffset) {
                addDiagnostic(node,
                              "brace indexing could not map the linear subscript");
            }
            return storageOffset;
        }

        const auto effectiveDimensions =
            runtimeEffectiveSubscriptDimensions(target, arguments.size());
        std::vector<size_t> coordinates;
        coordinates.reserve(arguments.size());
        for (size_t index = 0; index < arguments.size(); ++index) {
            const auto coordinate = checkedIndex(
                node, arguments[index].number, effectiveDimensions[index]);
            if (!coordinate) {
                return std::nullopt;
            }
            coordinates.push_back(*coordinate);
        }
        const auto storageOffset = runtimeSubscriptsToStorageOffset(
            target, coordinates, effectiveDimensions);
        if (!storageOffset) {
            addDiagnostic(node, "brace indexing could not map the subscripts");
        }
        return storageOffset;
    }

    RuntimeValue evaluateIndex(const HirNode& node, const RuntimeValue& target,
                               const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty()) {
            addDiagnostic(node, "indexing requires subscripts");
            return missingValue();
        }

        if (!isNumeric(target)) {
            addDiagnostic(node, "indexing requires a numeric target");
            return missingValue();
        }

        for (const auto& argument : arguments) {
            if (!isNumeric(argument)) {
                addDiagnostic(
                    node,
                    "indexing requires numeric or logical subscripts");
                return missingValue();
            }
        }

        if (arguments.size() > 1) {
            const auto effectiveDimensions =
                runtimeEffectiveSubscriptDimensions(target, arguments.size());
            std::vector<std::vector<size_t>> selections;
            std::vector<size_t> selectionDimensions;
            selections.reserve(arguments.size());
            selectionDimensions.reserve(arguments.size());
            for (size_t index = 0; index < arguments.size(); ++index) {
                auto selection = checkedIndices(node, arguments[index],
                                                effectiveDimensions[index]);
                if (!selection) {
                    return missingValue();
                }
                selectionDimensions.push_back(selection->size());
                selections.push_back(std::move(*selection));
            }

            const auto count =
                checkedRuntimeDimensionProduct(selectionDimensions);
            if (!count) {
                addDiagnostic(node, "indexed result dimensions are too large");
                return missingValue();
            }
            std::vector<double> values;
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
                if (!storageOffset) {
                    addDiagnostic(node, "indexing could not map subscripts");
                    return missingValue();
                }
                values.push_back(isNumber(target)
                                     ? target.number
                                     : target.elements[*storageOffset]);
            }

            if (values.size() == 1) {
                return numberValue(values.front(), target.numericClass);
            }
            return arrayValueForDimensions(selectionDimensions,
                                           std::move(values),
                                           target.numericClass);
        }

        const RuntimeValue& subscript = arguments.front();
        auto selection = runtimeResolveIndexSelection(
            subscript, elementCount(target), false);
        if (!selection.succeeded) {
            addDiagnostic(node, std::move(selection.error));
            return missingValue();
        }

        std::vector<double> values;
        values.reserve(selection.indices.size());
        for (const size_t index : selection.indices) {
            values.push_back(isNumber(target) ? target.number
                                              : linearElement(target, index));
        }
        if (values.size() == 1) {
            return numberValue(values.front(), target.numericClass);
        }
        const auto dimensions = runtimeLinearIndexResultDimensions(
            target, subscript, values.size(), selection.logicalMask);
        return arrayValueForDimensions(dimensions, std::move(values),
                                       target.numericClass);
    }

    std::optional<std::vector<size_t>>
    checkedIndices(const HirNode& node, const RuntimeValue& subscript,
                   size_t length) {
        auto selection =
            runtimeResolveIndexSelection(subscript, length, false);
        if (!selection.succeeded) {
            addDiagnostic(node, std::move(selection.error));
            return std::nullopt;
        }
        return std::move(selection.indices);
    }

    std::optional<size_t> checkedIndex(const HirNode& node, double rawIndex,
                                       size_t length) {
        if (!isWholeNumber(rawIndex)) {
            addDiagnostic(node, "index must be a positive integer");
            return std::nullopt;
        }
        if (rawIndex < 1.0 || rawIndex > static_cast<double>(length)) {
            addDiagnostic(node, "index is out of bounds");
            return std::nullopt;
        }
        return static_cast<size_t>(rawIndex) - 1;
    }

    double linearElement(const RuntimeValue& value, size_t zeroBasedIndex) const {
        if (isNumber(value)) {
            return value.number;
        }
        const auto storageOffset =
            runtimeColumnMajorLinearToStorageOffset(value, zeroBasedIndex);
        return value.elements[*storageOffset];
    }

    FunctionCallResult
    callBuiltin(const HirNode& node, const std::string& name,
                const std::vector<RuntimeValue>& arguments,
                size_t requestedOutputCount) {
        if (name == "clear" || name == "clc" || name == "tic" ||
            name == "toc") {
            if (!arguments.empty()) {
                addDiagnostic(node, name + " currently expects no arguments");
                return FunctionCallResult{{missingValue()}};
            }
            if (name == "clear") {
                currentFrame().clear();
                return FunctionCallResult{{missingValue()}};
            }
            if (name == "clc") {
                return FunctionCallResult{{missingValue()}};
            }
            if (name == "tic") {
                ticStart_ = std::chrono::steady_clock::now();
                return FunctionCallResult{{numberValue(0.0)}};
            }
            if (!ticStart_) {
                addDiagnostic(node, "toc requires a preceding tic");
                return FunctionCallResult{{missingValue()}};
            }
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - *ticStart_;
            return FunctionCallResult{{numberValue(elapsed.count())}};
        }
        if (isRuntimeReductionBuiltin(name)) {
            auto result = runtimeReductionBuiltin(
                name, arguments, requestedOutputCount);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            return FunctionCallResult{std::move(result.outputs)};
        }
        if (isRuntimeScanBuiltin(name)) {
            auto result = runtimeScanBuiltin(
                name, arguments, requestedOutputCount);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            return FunctionCallResult{std::move(result.outputs)};
        }
        if (isRuntimeArrayOperationBuiltin(name)) {
            auto result = runtimeArrayOperationBuiltin(name, arguments);
            if (!result.succeeded) {
                addDiagnostic(node, result.error);
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "zeros" || name == "ones" || name == "eye" ||
            name == "true" || name == "false") {
            if ((name == "true" || name == "false") &&
                arguments.empty()) {
                return FunctionCallResult{{logicalValue(name == "true")}};
            }
            return callArrayConstructorBuiltin(node, name, arguments);
        }
        if (name == "cell") {
            const auto shape = constructorShape(node, name, arguments);
            if (!shape) {
                return FunctionCallResult{{missingValue()}};
            }
            const auto count = checkedRuntimeDimensionProduct(*shape);
            if (!count) {
                addDiagnostic(node, "cell dimensions are too large");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{cellValueForDimensions(
                *shape, std::vector<RuntimeValue>(*count, missingValue()))}};
        }
        if (name == "linspace") {
            return callLinspaceBuiltin(node, arguments);
        }
        if (name == "strcmp") {
            return callStrcmpBuiltin(node, arguments);
        }
        if (name == "struct") {
            if (!arguments.empty()) {
                addDiagnostic(node,
                              "struct currently supports only the empty form");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{makeRuntimeStructValue()}};
        }
        if (name == "isfield") {
            if (arguments.size() != 2 || !isStruct(arguments[0]) ||
                !isString(arguments[1])) {
                addDiagnostic(node,
                              "isfield expects a structure and field-name string");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{logicalValue(
                arguments[0].fields.contains(arguments[1].text))}};
        }
        if (name == "logical" || name == "double") {
            if (arguments.size() != 1 || !isNumeric(arguments.front())) {
                addDiagnostic(node,
                              name + " expects one numeric argument");
                return FunctionCallResult{{missingValue()}};
            }
            const auto converted = runtimeConvertNumericClass(
                arguments.front(),
                name == "logical" ? RuntimeNumericClass::Logical
                                  : RuntimeNumericClass::Double);
            if (!converted) {
                addDiagnostic(node, "logical cannot convert NaN values");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(*converted)}};
        }
        if (name == "islogical") {
            if (arguments.size() != 1) {
                addDiagnostic(node, "islogical expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{
                logicalValue(isRuntimeLogical(arguments.front()))}};
        }
        if (name == "class") {
            if (arguments.size() != 1) {
                addDiagnostic(node, "class expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            const RuntimeValue& value = arguments.front();
            if (isNumeric(value)) {
                return FunctionCallResult{{stringValue(std::string(
                    runtimeNumericClassName(value.numericClass)))}};
            }
            if (isString(value)) {
                return FunctionCallResult{{stringValue("char")}};
            }
            if (isCell(value)) {
                return FunctionCallResult{{stringValue("cell")}};
            }
            if (isStruct(value)) {
                return FunctionCallResult{{stringValue("struct")}};
            }
            return FunctionCallResult{{stringValue("missing")}};
        }
        if (name == "isa") {
            if (arguments.size() != 2 || !isString(arguments[1])) {
                addDiagnostic(node,
                              "isa expects a value and class-name string");
                return FunctionCallResult{{missingValue()}};
            }
            const RuntimeValue& value = arguments.front();
            const std::string& target = arguments[1].text;
            bool matches = false;
            if (isNumeric(value)) {
                matches = isRuntimeLogical(value)
                              ? target == "logical"
                              : target == "double" || target == "numeric";
            } else if (isString(value)) {
                matches = target == "char" || target == "string";
            } else if (isCell(value)) {
                matches = target == "cell";
            } else if (isStruct(value)) {
                matches = target == "struct";
            }
            return FunctionCallResult{{logicalValue(matches)}};
        }

        if (name == "size") {
            return callSizeBuiltin(node, arguments, requestedOutputCount);
        }
        if (name == "length" || name == "numel" || name == "ndims" ||
            name == "isempty") {
            if (arguments.size() != 1) {
                addDiagnostic(node, name + " expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            if (name == "length") {
                const auto dimensions = runtimeDimensions(arguments.front());
                return FunctionCallResult{{numberValue(static_cast<double>(
                    *std::max_element(dimensions.begin(), dimensions.end())))}};
            }
            if (name == "numel") {
                return FunctionCallResult{{numberValue(static_cast<double>(
                    elementCount(arguments.front())))}};
            }
            if (name == "ndims") {
                return FunctionCallResult{{numberValue(static_cast<double>(
                    runtimeDimensionCount(arguments.front())))}};
            }
            return FunctionCallResult{{logicalValue(
                elementCount(arguments.front()) == 0)}};
        }

        if (arguments.size() != 1 || !isNumeric(arguments.front())) {
            addDiagnostic(node, "builtin call currently requires one numeric "
                                "argument: " +
                                    name);
            return FunctionCallResult{{missingValue()}};
        }

        if (isRuntimePureUnaryMathBuiltin(name)) {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [&](double value) {
                    return *runtimeApplyPureUnaryMathBuiltin(name, value);
                })}};
        }
        addDiagnostic(node, "builtin is not executable yet: " + name);
        return FunctionCallResult{{missingValue()}};
    }

    FunctionCallResult
    callStrcmpBuiltin(const HirNode& node,
                      const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isString(arguments[0]) ||
            !isString(arguments[1])) {
            addDiagnostic(node, "strcmp expects two string arguments");
            return FunctionCallResult{{missingValue()}};
        }

        return FunctionCallResult{{
            logicalValue(runtimeEqual(arguments[0], arguments[1]))}};
    }

    FunctionCallResult callSizeBuiltin(
        const HirNode& node, const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount) {
        if (arguments.empty() || arguments.size() > 2) {
            addDiagnostic(node, "size expects an array and optional dimension");
            return FunctionCallResult{{missingValue()}};
        }
        if (requestedOutputCount == 0) {
            return FunctionCallResult{};
        }

        const RuntimeValue& value = arguments.front();
        const auto dimensions = runtimeDimensions(value);
        if (arguments.size() == 2) {
            if (requestedOutputCount != 1) {
                addDiagnostic(node,
                              "size with a dimension produces one output");
                return FunctionCallResult{{missingValue()}};
            }

            auto dimensionValue = [&](double raw) -> std::optional<double> {
                const auto dimension =
                    checkedRuntimeNonnegativeInteger(raw);
                if (!dimension || *dimension == 0) {
                    return std::nullopt;
                }
                return static_cast<double>(runtimeDimension(
                    value, *dimension - 1));
            };

            const RuntimeValue& requested = arguments[1];
            if (isNumber(requested)) {
                const auto result = dimensionValue(requested.number);
                if (!result) {
                    addDiagnostic(node,
                                  "size dimension must be a positive integer");
                    return FunctionCallResult{{missingValue()}};
                }
                return FunctionCallResult{{numberValue(*result)}};
            }
            if (!isArray(requested)) {
                addDiagnostic(node, "size dimensions must be numeric");
                return FunctionCallResult{{missingValue()}};
            }
            std::vector<double> results;
            results.reserve(requested.elements.size());
            for (const double raw : requested.elements) {
                const auto result = dimensionValue(raw);
                if (!result) {
                    addDiagnostic(node,
                                  "size dimension must be a positive integer");
                    return FunctionCallResult{{missingValue()}};
                }
                results.push_back(*result);
            }
            return FunctionCallResult{{vectorValue(std::move(results))}};
        }

        if (requestedOutputCount == 1) {
            std::vector<double> results;
            results.reserve(dimensions.size());
            for (const size_t dimension : dimensions) {
                results.push_back(static_cast<double>(dimension));
            }
            return FunctionCallResult{{vectorValue(std::move(results))}};
        }

        FunctionCallResult result;
        result.outputs.reserve(requestedOutputCount);
        for (size_t index = 0; index < requestedOutputCount; ++index) {
            size_t dimension = 1;
            if (index + 1 == requestedOutputCount &&
                requestedOutputCount < dimensions.size()) {
                std::vector<size_t> folded(dimensions.begin() + index,
                                           dimensions.end());
                dimension =
                    checkedRuntimeDimensionProduct(folded).value_or(0);
            } else if (index < dimensions.size()) {
                dimension = dimensions[index];
            }
            result.outputs.push_back(
                numberValue(static_cast<double>(dimension)));
        }
        return result;
    }

    FunctionCallResult
    callArrayConstructorBuiltin(const HirNode& node, const std::string& name,
                                const std::vector<RuntimeValue>& arguments) {
        const auto shape = constructorShape(node, name, arguments);
        if (!shape) {
            return FunctionCallResult{{missingValue()}};
        }

        if (name == "eye" && shape->size() > 2) {
            addDiagnostic(node, "eye creates only two-dimensional arrays");
            return FunctionCallResult{{missingValue()}};
        }
        const auto count = checkedRuntimeDimensionProduct(*shape);
        if (!count) {
            addDiagnostic(node, "array constructor dimensions are too large");
            return FunctionCallResult{{missingValue()}};
        }
        const bool logical = name == "true" || name == "false";
        std::vector<double> elements(
            *count, name == "ones" || name == "true" ? 1.0 : 0.0);
        if (name == "eye") {
            const size_t rows = (*shape)[0];
            const size_t columns = (*shape)[1];
            const size_t diagonal = rows < columns ? rows : columns;
            for (size_t index = 0; index < diagonal; ++index) {
                elements[index * columns + index] = 1.0;
            }
        }

        return FunctionCallResult{{
            arrayValueForDimensions(
                *shape, std::move(elements),
                logical ? RuntimeNumericClass::Logical
                        : RuntimeNumericClass::Double)}};
    }

    std::optional<std::vector<size_t>>
    constructorShape(const HirNode& node, const std::string& name,
                     const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty()) {
            addDiagnostic(node,
                          "array constructor expects dimensions: " + name);
            return std::nullopt;
        }

        if (arguments.size() > 1) {
            std::vector<size_t> dimensions;
            dimensions.reserve(arguments.size());
            for (const auto& argument : arguments) {
                if (!isNumber(argument)) {
                    addDiagnostic(
                        node,
                        "array constructor dimensions must be scalar numbers: " +
                            name);
                    return std::nullopt;
                }
                const auto dimension =
                    dimensionFromNumber(node, argument.number);
                if (!dimension) {
                    return std::nullopt;
                }
                dimensions.push_back(*dimension);
            }
            return normalizeRuntimeDimensions(std::move(dimensions));
        }

        const RuntimeValue& shape = arguments.front();
        if (isNumber(shape)) {
            const auto dimension = dimensionFromNumber(node, shape.number);
            if (!dimension) {
                return std::nullopt;
            }
            return std::vector<size_t>{*dimension, *dimension};
        }

        if (!isArray(shape) || shape.elements.empty()) {
            addDiagnostic(node,
                          "array constructor shape vector must contain "
                          "dimensions: " +
                              name);
            return std::nullopt;
        }

        std::vector<size_t> dimensions;
        dimensions.reserve(shape.elements.size());
        for (const double raw : shape.elements) {
            const auto dimension = dimensionFromNumber(node, raw);
            if (!dimension) {
                return std::nullopt;
            }
            dimensions.push_back(*dimension);
        }
        if (dimensions.size() == 1) {
            dimensions.push_back(dimensions.front());
        }
        return normalizeRuntimeDimensions(std::move(dimensions));
    }

    std::optional<size_t> dimensionFromNumber(const HirNode& node,
                                              double value) {
        const auto dimension = checkedRuntimeNonnegativeInteger(value);
        if (!dimension) {
            addDiagnostic(node,
                          "dimension or count must be a representable "
                          "nonnegative integer");
            return std::nullopt;
        }
        return dimension;
    }

    FunctionCallResult
    callLinspaceBuiltin(const HirNode& node,
                        const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 && arguments.size() != 3) {
            addDiagnostic(node,
                          "linspace expects two or three scalar arguments");
            return FunctionCallResult{{missingValue()}};
        }
        if (!isNumber(arguments[0]) || !isNumber(arguments[1]) ||
            (arguments.size() == 3 && !isNumber(arguments[2]))) {
            addDiagnostic(node, "linspace arguments must be scalar numbers");
            return FunctionCallResult{{missingValue()}};
        }

        size_t count = 100;
        if (arguments.size() == 3) {
            const auto requested = dimensionFromNumber(node, arguments[2].number);
            if (!requested) {
                return FunctionCallResult{{missingValue()}};
            }
            count = *requested;
        }

        std::vector<double> values;
        values.reserve(count);
        if (count == 0) {
            return FunctionCallResult{{vectorValue(std::move(values))}};
        }

        const double start = arguments[0].number;
        const double stop = arguments[1].number;
        if (count == 1) {
            values.push_back(stop);
            return FunctionCallResult{{vectorValue(std::move(values))}};
        }

        const double denominator = static_cast<double>(count - 1);
        for (size_t index = 0; index < count; ++index) {
            const double t = static_cast<double>(index) / denominator;
            values.push_back(start + (stop - start) * t);
        }

        return FunctionCallResult{{vectorValue(std::move(values))}};
    }

    std::vector<std::map<std::string, RuntimeValue>> frames_;
    std::map<std::string, const HirNode*> functionsByName_;
    std::map<std::string, RuntimeValue> resultFrame_;
    std::vector<Diagnostic> diagnostics_;
    std::optional<size_t> diagnosticTrapBase_;
    std::optional<std::chrono::steady_clock::time_point> ticStart_;
    size_t loopDepth_ = 0;
    ControlSignal controlSignal_ = ControlSignal::None;
    static constexpr size_t kMaxWhileIterations = 1'000'000;
};

} // namespace

InterpreterResult Interpreter::run(const SemanticResult& semantic) {
    InterpreterContext context;
    return context.run(semantic);
}

RuntimeValue makeRuntimeStructValue(
    std::map<std::string, RuntimeValue> fields) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Struct;
    result.fields = std::move(fields);
    setRuntimeDimensions(result, {1, 1});
    return result;
}

RuntimeValue makeRuntimeNameValueArgument(std::string name,
                                          RuntimeValue value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::NameValueArgument;
    result.text = std::move(name);
    result.cells.push_back(std::move(value));
    setRuntimeDimensions(result, {1, 1});
    return result;
}

std::string runtimeValueToString(const RuntimeValue& value) {
    std::ostringstream output;
    output << std::setprecision(15);

    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return "<missing>";
    case RuntimeValueKind::Number:
        output << value.number;
        return output.str();
    case RuntimeValueKind::String:
        output << '"' << value.text << '"';
        return output.str();
    case RuntimeValueKind::Vector:
        output << "[";
        for (size_t index = 0; index < value.elements.size(); ++index) {
            if (index > 0) {
                output << " ";
            }
            output << value.elements[index];
        }
        output << "]";
        return output.str();
    case RuntimeValueKind::Matrix:
        if (runtimeDimensionCount(value) > 2) {
            const auto dimensions = runtimeDimensions(value);
            output << "array(";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
            output << ")[";
            for (size_t index = 0; index < value.elements.size(); ++index) {
                if (index != 0) {
                    output << " ";
                }
                const auto storageOffset =
                    runtimeColumnMajorLinearToStorageOffset(value, index);
                output << value.elements[*storageOffset];
            }
            output << "]";
            return output.str();
        }
        output << "[";
        for (size_t row = 0; row < value.rows; ++row) {
            if (row > 0) {
                output << "; ";
            }
            for (size_t column = 0; column < value.columns; ++column) {
                if (column > 0) {
                    output << " ";
                }
                output << value.elements[row * value.columns + column];
            }
        }
        output << "]";
        return output.str();
    case RuntimeValueKind::Cell:
        if (runtimeDimensionCount(value) > 2) {
            const auto dimensions = runtimeDimensions(value);
            output << "cell(";
            for (size_t index = 0; index < dimensions.size(); ++index) {
                if (index != 0) {
                    output << "x";
                }
                output << dimensions[index];
            }
            output << "){";
            for (size_t index = 0; index < value.cells.size(); ++index) {
                if (index != 0) {
                    output << ", ";
                }
                const auto storageOffset =
                    runtimeColumnMajorLinearToStorageOffset(value, index);
                output << runtimeValueToString(value.cells[*storageOffset]);
            }
            output << "}";
            return output.str();
        }
        output << "{";
        for (size_t index = 0; index < value.cells.size(); ++index) {
            if (index > 0) {
                output << ", ";
            }
            output << runtimeValueToString(value.cells[index]);
        }
        output << "}";
        return output.str();
    case RuntimeValueKind::FunctionHandle:
        return value.text.empty() ? "<function_handle>" : value.text;
    case RuntimeValueKind::Struct:
        output << "struct(";
        for (auto field = value.fields.begin(); field != value.fields.end();
             ++field) {
            if (field != value.fields.begin()) {
                output << ", ";
            }
            output << field->first << "="
                   << runtimeValueToString(field->second);
        }
        output << ")";
        return output.str();
    case RuntimeValueKind::NameValueArgument:
        return value.text + "=" +
               (value.cells.empty() ? std::string("<missing>")
                                    : runtimeValueToString(value.cells.front()));
    case RuntimeValueKind::Object:
        output << "<" << value.className;
        if (!value.enumerationMemberName.empty()) {
            output << "." << value.enumerationMemberName;
        }
        output << ">";
        return output.str();
    }
    return "<missing>";
}

} // namespace mparser
