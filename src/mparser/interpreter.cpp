#include "mparser/interpreter.h"
#include "mparser/function_signature.h"

#include <algorithm>
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

RuntimeValue numberValue(double value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Number;
    result.number = value;
    result.rows = 1;
    result.columns = 1;
    return result;
}

RuntimeValue stringValue(std::string value) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::String;
    result.text = std::move(value);
    result.rows = 1;
    result.columns = result.text.size();
    return result;
}

RuntimeValue vectorValue(std::vector<double> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Vector;
    result.elements = std::move(values);
    result.rows = 1;
    result.columns = result.elements.size();
    return result;
}

RuntimeValue matrixValue(size_t rows, size_t columns,
                         std::vector<double> values) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Matrix;
    result.elements = std::move(values);
    result.rows = rows;
    result.columns = columns;
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
        return left.number == right.number;
    }
    if (isString(left) && isString(right)) {
        return left.text == right.text;
    }
    if (isArray(left) && isArray(right)) {
        return left.rows == right.rows && left.columns == right.columns &&
               left.elements == right.elements;
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
    if (isNumber(value)) {
        return 1;
    }
    return value.rows;
}

size_t columnCount(const RuntimeValue& value) {
    if (isNumber(value)) {
        return 1;
    }
    return value.columns;
}

size_t elementCount(const RuntimeValue& value) {
    if (isNumber(value)) {
        return 1;
    }
    return value.elements.size();
}

double elementAt(const RuntimeValue& value, size_t index) {
    return isNumber(value) ? value.number : value.elements[index];
}

RuntimeValue arrayValueForShape(size_t rows, size_t columns,
                                std::vector<double> values) {
    if (rows == 1) {
        return vectorValue(std::move(values));
    }
    return matrixValue(rows, columns, std::move(values));
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

RuntimeValue mapUnary(const RuntimeValue& value, double (*operation)(double)) {
    if (isNumber(value)) {
        return numberValue(operation(value.number));
    }

    std::vector<double> mapped;
    mapped.reserve(value.elements.size());
    for (double element : value.elements) {
        mapped.push_back(operation(element));
    }
    return arrayValueForShape(rowCount(value), columnCount(value),
                              std::move(mapped));
}

bool isWholeNumber(double value) {
    return std::isfinite(value) && std::floor(value) == value;
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
        if (arguments.size() != signature.parameters.size()) {
            addDiagnostic(node, "function argument count mismatch for: " +
                                    node.label);
            return FunctionCallResult{{missingValue()}};
        }
        const size_t outputCount =
            requestedOutputCount.value_or(signature.outputs.size());
        if (outputCount > signature.outputs.size()) {
            addDiagnostic(node, "function output count mismatch for: " +
                                    node.label);
            return FunctionCallResult{
                std::vector<RuntimeValue>(outputCount, missingValue())};
        }

        FunctionControlContext controlContext(loopDepth_, controlSignal_);
        frames_.push_back({});
        for (size_t i = 0; i < signature.parameters.size(); ++i) {
            currentFrame()[signature.parameters[i]] = arguments[i];
        }
        for (const auto& output : signature.outputs) {
            currentFrame()[output] = missingValue();
        }
        currentFrame()["nargin"] =
            numberValue(static_cast<double>(arguments.size()));
        currentFrame()["nargout"] =
            numberValue(static_cast<double>(outputCount));

        executeChildren(node);

        auto completedFrame = std::move(currentFrame());
        frames_.pop_back();

        FunctionCallResult result;
        result.outputs.reserve(outputCount);
        for (size_t index = 0; index < outputCount; ++index) {
            const auto& outputName = signature.outputs[index];
            const auto output = completedFrame.find(outputName);
            if (output != completedFrame.end()) {
                result.outputs.push_back(output->second);
            } else {
                result.outputs.push_back(missingValue());
            }
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
        case HirKind::Property:
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
        case HirKind::CallOrIndex:
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

        assignTarget(target, evaluate(value));
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

    void assignTarget(const HirNode& target, const RuntimeValue& value) {
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
            addDiagnostic(target,
                          "member assignment is not executable in the reference "
                          "interpreter yet");
            break;
        case HirKind::CallOrIndex:
            assignIndexedTarget(target, value);
            break;
        case HirKind::BraceIndex:
            addDiagnostic(target,
                          "indexed assignment is not executable in the reference "
                          "interpreter yet");
            break;
        default:
            addDiagnostic(target, "unsupported assignment target");
            break;
        }
    }

    void assignIndexedTarget(const HirNode& target, const RuntimeValue& value) {
        if (target.children.empty()) {
            addDiagnostic(target, "indexed assignment is missing a target");
            return;
        }
        if (!isNumber(value)) {
            addDiagnostic(target,
                          "indexed assignment currently requires a scalar "
                          "numeric value");
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
        if (arguments.size() != 1 && arguments.size() != 2) {
            addDiagnostic(target,
                          "indexed assignment currently supports one or two "
                          "subscripts");
            return;
        }
        for (const auto& argument : arguments) {
            if (!isNumeric(argument)) {
                addDiagnostic(target,
                              "indexed assignment requires numeric subscripts");
                return;
            }
        }

        if (arguments.size() == 2) {
            assignTwoSubscriptTarget(target, targetValue, arguments,
                                     value.number);
            return;
        }

        assignLinearTarget(target, targetValue, arguments.front(),
                           value.number);
    }

    void assignTwoSubscriptTarget(const HirNode& node, RuntimeValue& target,
                                  const std::vector<RuntimeValue>& arguments,
                                  double value) {
        const auto rows = checkedIndices(node, arguments[0], rowCount(target));
        const auto columns =
            checkedIndices(node, arguments[1], columnCount(target));
        if (!rows || !columns) {
            return;
        }

        for (size_t row : *rows) {
            for (size_t column : *columns) {
                assignMatrixElement(target, row, column, value);
            }
        }
    }

    void assignLinearTarget(const HirNode& node, RuntimeValue& target,
                            const RuntimeValue& subscript, double value) {
        if (isNumber(subscript)) {
            const auto index = checkedIndex(node, subscript.number,
                                            elementCount(target));
            if (!index) {
                return;
            }
            assignLinearElement(target, *index, value);
            return;
        }

        for (double rawIndex : subscript.elements) {
            const auto index = checkedIndex(node, rawIndex,
                                            elementCount(target));
            if (!index) {
                return;
            }
            assignLinearElement(target, *index, value);
        }
    }

    void assignLinearElement(RuntimeValue& target, size_t zeroBasedIndex,
                             double value) {
        if (isNumber(target)) {
            target.number = value;
            return;
        }
        if (!isMatrix(target)) {
            target.elements[zeroBasedIndex] = value;
            return;
        }

        const size_t row = zeroBasedIndex % target.rows;
        const size_t column = zeroBasedIndex / target.rows;
        target.elements[row * target.columns + column] = value;
    }

    void assignMatrixElement(RuntimeValue& target, size_t row, size_t column,
                             double value) {
        if (isNumber(target)) {
            target.number = value;
            return;
        }
        target.elements[row * columnCount(target) + column] = value;
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
            currentFrame()[loopVariable] = numberValue(value);
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
        case HirKind::CallOrIndex:
            return evaluateCallOrIndex(node);
        case HirKind::Assignment:
            executeAssignment(node);
            return missingValue();
        case HirKind::Postfix:
            return evaluatePostfix(node);
        case HirKind::Module:
        case HirKind::Class:
        case HirKind::Function:
        case HirKind::Property:
        case HirKind::MethodPrototype:
        case HirKind::Control:
        case HirKind::ControlArm:
        case HirKind::OutputList:
        case HirKind::ParameterList:
        case HirKind::Cell:
        case HirKind::MemberAccess:
        case HirKind::BraceIndex:
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
            return matrixValue(value.elements.size(), 1, value.elements);
        }
        if (isMatrix(value)) {
            std::vector<double> transposed;
            transposed.reserve(value.elements.size());
            for (size_t column = 0; column < value.columns; ++column) {
                for (size_t row = 0; row < value.rows; ++row) {
                    transposed.push_back(value.elements[row * value.columns +
                                                        column]);
                }
            }
            return matrixValue(value.columns, value.rows,
                               std::move(transposed));
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
                return numberValue(1.0);
            }
            if (node.label == "false") {
                return numberValue(0.0);
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

    RuntimeValue evaluateMatrix(const HirNode& node) {
        if (node.children.empty()) {
            return vectorValue({});
        }

        if (node.children.front()->kind != HirKind::MatrixRow) {
            std::vector<double> elements;
            for (const auto& child : node.children) {
                appendNumericElements(*child, elements);
            }
            return vectorValue(std::move(elements));
        }

        std::vector<double> elements;
        size_t columns = 0;
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

    RuntimeValue evaluateMatrixRow(const HirNode& node) {
        std::vector<double> elements;
        for (const auto& child : node.children) {
            appendNumericElements(*child, elements);
        }

        return vectorValue(std::move(elements));
    }

    void appendNumericElements(const HirNode& node, std::vector<double>& elements) {
        const RuntimeValue value = evaluate(node);
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
                return numberValue(0.0);
            }
            if (node.label == "||" && leftValue) {
                return numberValue(1.0);
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
            return numberValue(truthy(right) ? 1.0 : 0.0);
        }

        const RuntimeValue right = evaluate(*node.children[1]);
        if (isString(left) || isString(right)) {
            if (isString(left) && isString(right) &&
                (node.label == "==" || node.label == "~=")) {
                const bool equal = runtimeEqual(left, right);
                return numberValue((node.label == "==") == equal ? 1.0
                                                                 : 0.0);
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
            return applyMatrixMultiply(node, left, right);
        }

        if ((node.label == "/" || node.label == "\\" || node.label == "^") &&
            isArray(left) && isArray(right)) {
            addDiagnostic(node,
                          "matrix division and matrix power are not "
                          "implemented for array operands yet");
            return missingValue();
        }

        if (isArray(left) && isArray(right) &&
            (rowCount(left) != rowCount(right) ||
             columnCount(left) != columnCount(right))) {
            addDiagnostic(node,
                          "elementwise operands must have the same shape");
            return missingValue();
        }

        const size_t rows = isArray(left) ? rowCount(left) : rowCount(right);
        const size_t columns =
            isArray(left) ? columnCount(left) : columnCount(right);
        const size_t count = rows * columns;
        std::vector<double> elements;
        elements.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const double leftValue =
                isArray(left) ? left.elements[index] : left.number;
            const double rightValue =
                isArray(right) ? right.elements[index] : right.number;
            const RuntimeValue value =
                applyScalarBinary(node, leftValue, rightValue);
            if (!isNumber(value)) {
                return missingValue();
            }
            elements.push_back(value.number);
        }

        return arrayValueForShape(rows, columns, std::move(elements));
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
            return numberValue(left > right ? 1.0 : 0.0);
        }
        if (node.label == "<") {
            return numberValue(left < right ? 1.0 : 0.0);
        }
        if (node.label == ">=") {
            return numberValue(left >= right ? 1.0 : 0.0);
        }
        if (node.label == "<=") {
            return numberValue(left <= right ? 1.0 : 0.0);
        }
        if (node.label == "==") {
            return numberValue(left == right ? 1.0 : 0.0);
        }
        if (node.label == "~=") {
            return numberValue(left != right ? 1.0 : 0.0);
        }
        if (node.label == "&" || node.label == "&&") {
            return numberValue((truthy(numberValue(left)) &&
                                truthy(numberValue(right)))
                                   ? 1.0
                                   : 0.0);
        }
        if (node.label == "|" || node.label == "||") {
            return numberValue((truthy(numberValue(left)) ||
                                truthy(numberValue(right)))
                                   ? 1.0
                                   : 0.0);
        }

        addDiagnostic(node, "unsupported binary operator: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateColon(const HirNode& node) {
        std::vector<double> terms;
        collectColonTerms(node, terms);
        if (terms.size() != 2 && terms.size() != 3) {
            addDiagnostic(node, "colon range must have two or three operands");
            return missingValue();
        }

        const double start = terms[0];
        const double step = terms.size() == 3 ? terms[1] : 1.0;
        const double stop = terms.size() == 3 ? terms[2] : terms[1];
        std::vector<double> values;

        if (step == 0.0) {
            addDiagnostic(node, "colon range step cannot be zero");
            return vectorValue(values);
        }

        if (step > 0.0) {
            for (double value = start; value <= stop; value += step) {
                values.push_back(value);
            }
        } else {
            for (double value = start; value >= stop; value += step) {
                values.push_back(value);
            }
        }

        return vectorValue(std::move(values));
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
        if (total <= 1) {
            return static_cast<double>(elementCount(target));
        }
        if (position == 0) {
            return static_cast<double>(rowCount(target));
        }
        if (position == 1) {
            return static_cast<double>(columnCount(target));
        }
        return 1.0;
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
        if (terms.size() != 2 && terms.size() != 3) {
            addDiagnostic(node, "colon range must have two or three operands");
            return missingValue();
        }

        const double start = terms[0];
        const double step = terms.size() == 3 ? terms[1] : 1.0;
        const double stop = terms.size() == 3 ? terms[2] : terms[1];
        std::vector<double> values;

        if (step == 0.0) {
            addDiagnostic(node, "colon range step cannot be zero");
            return vectorValue(values);
        }

        if (step > 0.0) {
            for (double value = start; value <= stop; value += step) {
                values.push_back(value);
            }
        } else {
            for (double value = start; value >= stop; value += step) {
                values.push_back(value);
            }
        }

        return vectorValue(std::move(values));
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
            return vectorValue({});
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

    RuntimeValue evaluateIndex(const HirNode& node, const RuntimeValue& target,
                               const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 && arguments.size() != 2) {
            addDiagnostic(node,
                          "indexing currently supports one or two subscripts");
            return missingValue();
        }

        if (!isNumeric(target)) {
            addDiagnostic(node, "indexing requires a numeric target");
            return missingValue();
        }

        for (const auto& argument : arguments) {
            if (!isNumeric(argument)) {
                addDiagnostic(node, "indexing requires numeric subscripts");
                return missingValue();
            }
        }

        if (arguments.size() == 2) {
            const auto rows =
                checkedIndices(node, arguments[0], rowCount(target));
            const auto columns =
                checkedIndices(node, arguments[1], columnCount(target));
            if (!rows || !columns) {
                return missingValue();
            }

            std::vector<double> values;
            values.reserve(rows->size() * columns->size());
            for (size_t row : *rows) {
                for (size_t column : *columns) {
                    values.push_back(matrixElement(target, row, column));
                }
            }

            if (values.size() == 1) {
                return numberValue(values.front());
            }
            return arrayValueForShape(rows->size(), columns->size(),
                                      std::move(values));
        }

        if (isNumber(arguments.front())) {
            const auto index =
                checkedIndex(node, arguments.front().number,
                             elementCount(target));
            if (!index) {
                return missingValue();
            }

            if (isNumber(target)) {
                return target;
            }
            return numberValue(linearElement(target, *index));
        }

        std::vector<double> values;
        for (double rawIndex : arguments.front().elements) {
            const auto index =
                checkedIndex(node, rawIndex, elementCount(target));
            if (!index) {
                return missingValue();
            }
            values.push_back(isNumber(target) ? target.number
                                              : linearElement(target, *index));
        }

        return vectorValue(std::move(values));
    }

    std::optional<std::vector<size_t>>
    checkedIndices(const HirNode& node, const RuntimeValue& subscript,
                   size_t length) {
        std::vector<size_t> indices;
        if (isNumber(subscript)) {
            const auto index = checkedIndex(node, subscript.number, length);
            if (!index) {
                return std::nullopt;
            }
            indices.push_back(*index);
            return indices;
        }

        if (!isArray(subscript)) {
            addDiagnostic(node, "indexing requires numeric subscripts");
            return std::nullopt;
        }

        indices.reserve(subscript.elements.size());
        for (double rawIndex : subscript.elements) {
            const auto index = checkedIndex(node, rawIndex, length);
            if (!index) {
                return std::nullopt;
            }
            indices.push_back(*index);
        }
        return indices;
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
        if (!isMatrix(value)) {
            return elementAt(value, zeroBasedIndex);
        }

        const size_t row = zeroBasedIndex % value.rows;
        const size_t column = zeroBasedIndex / value.rows;
        return matrixElement(value, row, column);
    }

    FunctionCallResult
    callBuiltin(const HirNode& node, const std::string& name,
                const std::vector<RuntimeValue>& arguments,
                size_t requestedOutputCount) {
        if (name == "zeros" || name == "ones" || name == "eye") {
            return callArrayConstructorBuiltin(node, name, arguments);
        }
        if (name == "linspace") {
            return callLinspaceBuiltin(node, arguments);
        }
        if (name == "strcmp") {
            return callStrcmpBuiltin(node, arguments);
        }

        if (arguments.size() != 1 || !isNumeric(arguments.front())) {
            addDiagnostic(node, "builtin call currently requires one numeric "
                                "argument: " +
                                    name);
            return FunctionCallResult{{missingValue()}};
        }

        if (name == "abs") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::fabs(value); })}};
        }
        if (name == "acos") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::acos(value); })}};
        }
        if (name == "asin") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::asin(value); })}};
        }
        if (name == "atan") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::atan(value); })}};
        }
        if (name == "cos") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::cos(value); })}};
        }
        if (name == "exp") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::exp(value); })}};
        }
        if (name == "log") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::log(value); })}};
        }
        if (name == "sin") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::sin(value); })}};
        }
        if (name == "sqrt") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::sqrt(value); })}};
        }
        if (name == "tan") {
            return FunctionCallResult{{mapUnary(
                arguments.front(), [](double value) { return std::tan(value); })}};
        }
        if (name == "length") {
            return FunctionCallResult{{numberValue(static_cast<double>(
                rowCount(arguments.front()) > columnCount(arguments.front())
                    ? rowCount(arguments.front())
                    : columnCount(arguments.front())))}};
        }
        if (name == "numel") {
            return FunctionCallResult{{numberValue(static_cast<double>(
                elementCount(arguments.front())))}};
        }
        if (name == "size") {
            return callSizeBuiltin(arguments.front(), requestedOutputCount);
        }
        if (name == "sum") {
            return FunctionCallResult{{reduceBuiltin(
                arguments.front(), 0.0, [](double total, double element) {
                    return total + element;
                })}};
        }
        if (name == "max") {
            return FunctionCallResult{{reduceExtrema(node, arguments.front(), true)}};
        }
        if (name == "min") {
            return FunctionCallResult{{reduceExtrema(node, arguments.front(), false)}};
        }
        if (name == "mean") {
            const RuntimeValue total =
                reduceBuiltin(arguments.front(), 0.0,
                              [](double sum, double element) {
                                  return sum + element;
                              });
            if (!isNumber(total)) {
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{numberValue(
                total.number /
                static_cast<double>(elementCount(arguments.front())))}};
        }
        if (name == "any") {
            return FunctionCallResult{{numberValue(
                truthyAny(arguments.front()) ? 1.0 : 0.0)}};
        }
        if (name == "all") {
            return FunctionCallResult{{numberValue(
                truthy(arguments.front()) ? 1.0 : 0.0)}};
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

        return FunctionCallResult{{numberValue(
            runtimeEqual(arguments[0], arguments[1]) ? 1.0 : 0.0)}};
    }

    FunctionCallResult callSizeBuiltin(const RuntimeValue& value,
                                       size_t requestedOutputCount) const {
        const double rows = static_cast<double>(rowCount(value));
        const double columns = static_cast<double>(columnCount(value));
        if (requestedOutputCount <= 1) {
            return FunctionCallResult{{vectorValue({rows, columns})}};
        }

        FunctionCallResult result;
        result.outputs.reserve(requestedOutputCount);
        result.outputs.push_back(numberValue(rows));
        result.outputs.push_back(numberValue(columns));
        for (size_t index = 2; index < requestedOutputCount; ++index) {
            result.outputs.push_back(numberValue(1.0));
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

        const auto [rows, columns] = *shape;
        std::vector<double> elements(rows * columns, name == "ones" ? 1.0 : 0.0);
        if (name == "eye") {
            const size_t diagonal = rows < columns ? rows : columns;
            for (size_t index = 0; index < diagonal; ++index) {
                elements[index * columns + index] = 1.0;
            }
        }

        return FunctionCallResult{{arrayValueForShape(rows, columns,
                                                      std::move(elements))}};
    }

    std::optional<std::pair<size_t, size_t>>
    constructorShape(const HirNode& node, const std::string& name,
                     const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty() || arguments.size() > 2) {
            addDiagnostic(node, "array constructor expects one or two dimensions: " +
                                    name);
            return std::nullopt;
        }

        if (arguments.size() == 2) {
            if (!isNumber(arguments[0]) || !isNumber(arguments[1])) {
                addDiagnostic(node,
                              "array constructor dimensions must be scalar numbers: " +
                                  name);
                return std::nullopt;
            }

            const auto rows = dimensionFromNumber(node, arguments[0].number);
            const auto columns = dimensionFromNumber(node, arguments[1].number);
            if (!rows || !columns) {
                return std::nullopt;
            }
            return std::pair<size_t, size_t>{*rows, *columns};
        }

        const RuntimeValue& shape = arguments.front();
        if (isNumber(shape)) {
            const auto dimension = dimensionFromNumber(node, shape.number);
            if (!dimension) {
                return std::nullopt;
            }
            return std::pair<size_t, size_t>{*dimension, *dimension};
        }

        if (!isArray(shape) || shape.elements.size() < 2) {
            addDiagnostic(node,
                          "array constructor shape vector must contain at "
                          "least two dimensions: " +
                              name);
            return std::nullopt;
        }

        const auto rows = dimensionFromNumber(node, shape.elements[0]);
        const auto columns = dimensionFromNumber(node, shape.elements[1]);
        if (!rows || !columns) {
            return std::nullopt;
        }
        return std::pair<size_t, size_t>{*rows, *columns};
    }

    std::optional<size_t> dimensionFromNumber(const HirNode& node,
                                              double value) {
        if (!isWholeNumber(value) || value < 0.0) {
            addDiagnostic(node,
                          "dimension or count must be a "
                          "nonnegative integer");
            return std::nullopt;
        }
        return static_cast<size_t>(value);
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

    RuntimeValue reduceBuiltin(const RuntimeValue& value, double initial,
                               double (*operation)(double, double)) {
        if (isNumber(value)) {
            return numberValue(value.number);
        }

        double result = initial;
        for (double element : value.elements) {
            result = operation(result, element);
        }
        return numberValue(result);
    }

    RuntimeValue reduceExtrema(const HirNode& node, const RuntimeValue& value,
                               bool maximum) {
        if (isNumber(value)) {
            return value;
        }
        if (value.elements.empty()) {
            addDiagnostic(node, "cannot reduce an empty vector");
            return missingValue();
        }

        double result = value.elements.front();
        for (double element : value.elements) {
            result = maximum ? std::fmax(result, element)
                             : std::fmin(result, element);
        }
        return numberValue(result);
    }

    bool truthyAny(const RuntimeValue& value) const {
        if (isNumber(value)) {
            return truthy(value);
        }
        for (double element : value.elements) {
            if (element != 0.0 && !std::isnan(element)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::map<std::string, RuntimeValue>> frames_;
    std::map<std::string, const HirNode*> functionsByName_;
    std::map<std::string, RuntimeValue> resultFrame_;
    std::vector<Diagnostic> diagnostics_;
    std::optional<size_t> diagnosticTrapBase_;
    size_t loopDepth_ = 0;
    ControlSignal controlSignal_ = ControlSignal::None;
    static constexpr size_t kMaxWhileIterations = 1'000'000;
};

} // namespace

InterpreterResult Interpreter::run(const SemanticResult& semantic) {
    InterpreterContext context;
    return context.run(semantic);
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
    }
    return "<missing>";
}

} // namespace mparser
