#include "mparser/interpreter.h"
#include "mparser/argument_contract.h"
#include "mparser/builtin_registry.h"
#include "mparser/function_signature.h"
#include "mparser/runtime_call_frame.h"
#include "mparser/runtime_array_ops.h"
#include "mparser/runtime_argument_validation.h"
#include "mparser/runtime_assignment.h"
#include "mparser/runtime_cell.h"
#include "mparser/runtime_exception.h"
#include "mparser/runtime_index.h"
#include "mparser/runtime_lvalue.h"
#include "mparser/runtime_metadata.h"
#include "mparser/runtime_numeric.h"
#include "mparser/runtime_object.h"
#include "mparser/runtime_range.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_source_evaluation.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value_ops.h"
#include "mparser/runtime_warning.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace mparser {
namespace {

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

RuntimeValue cellValueForDimensions(std::vector<size_t> dimensions,
                                    std::vector<RuntimeValue> values) {
    return makeRuntimeCellValue(std::move(dimensions),
                                std::move(values));
}

bool isNumber(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Number;
}

bool isText(const RuntimeValue& value) {
    return isRuntimeTextValue(value);
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

bool isFunctionHandle(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::FunctionHandle &&
           value.functionHandle != nullptr;
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

std::string trimAscii(std::string_view text) {
    const size_t begin = text.find_first_not_of(" \t\r\n\v\f");
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n\v\f") + 1;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> anonymousParameterNames(std::string_view text) {
    std::vector<std::string> names;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const size_t end = comma == std::string_view::npos ? text.size()
                                                           : comma;
        std::string name = trimAscii(text.substr(begin, end - begin));
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return names;
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
    if (isNumeric(left) && isNumeric(right)) {
        return runtimeNumericValuesIdentical(left, right);
    }
    if (isText(left) && isText(right)) {
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
    if (isStruct(left) && isStruct(right)) {
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
    if (left.kind == RuntimeValueKind::NameValueArgument &&
        right.kind == RuntimeValueKind::NameValueArgument) {
        return left.text == right.text && left.cells.size() == 1 &&
               right.cells.size() == 1 &&
               runtimeEqual(left.cells.front(), right.cells.front());
    }
    if (left.kind == RuntimeValueKind::Object &&
        right.kind == RuntimeValueKind::Object) {
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

size_t elementCount(const RuntimeValue& value) {
    return runtimeShapeElementCount(value);
}

struct FunctionCallResult {
    std::vector<RuntimeValue> outputs;
};

class InterpreterContext {
public:
    InterpreterResult run(const SemanticResult& semantic,
                          const InterpreterOptions& options) {
        semantic_ = &semantic;
        callableContext_ = options.callableContext
                               ? options.callableContext
                               : makeRuntimeCallableContext();
        sessionState_ = options.sessionState
                            ? options.sessionState
                            : std::make_shared<RuntimeSessionState>();
        outputEvents_.clear();
        expressionResults_.clear();
        activeIndexContexts_.clear();
        nextConsoleSequence_ = 0;
        runtimeOutputSink_ = [this, external = options.outputSink](
                                 const RuntimeOutputEvent& event) {
            auto recorded = event;
            recorded.sequence = nextConsoleSequence_++;
            outputEvents_.push_back(recorded);
            return !external || external(recorded);
        };
        pendingException_.reset();
        frames_.push_back(makeRuntimeScriptFrame());
        if (semantic.root) {
            argumentCatalog_ =
                buildArgumentContractCatalog(*semantic.root);
            collectFunctions(*semantic.root);
            executeModule(*semantic.root);
        }

        InterpreterResult result;
        auto variables = resultFrame_.empty() ? currentFrame()
                                              : resultFrame_;
        if (resultFrame_.empty()) {
            for (const auto& name : baseGlobalNames_) {
                if (const auto value =
                        sessionState_->findGlobal(name)) {
                    variables[name] = *value;
                }
            }
        }
        for (const auto& [name, value] : variables) {
            result.variables.push_back(RuntimeVariable{name, value});
        }
        result.outputEvents = std::move(outputEvents_);
        result.expressionResults = std::move(expressionResults_);
        result.diagnostics = std::move(warnings_);
        result.diagnostics.insert(
            result.diagnostics.end(),
            std::make_move_iterator(diagnostics_.begin()),
            std::make_move_iterator(diagnostics_.end()));
        return result;
    }

private:
    enum class ControlSignal {
        None,
        Break,
        Continue,
        Return,
    };

    struct ActiveIndexContext {
        const RuntimeValue* target = nullptr;
        size_t position = 0;
        size_t total = 0;
    };

    class IndexContextGuard {
    public:
        IndexContextGuard(std::vector<ActiveIndexContext>& contexts,
                          const RuntimeValue& target, size_t position,
                          size_t total)
            : contexts_(contexts) {
            contexts_.push_back(ActiveIndexContext{
                &target, position, total});
        }

        ~IndexContextGuard() {
            contexts_.pop_back();
        }

    private:
        std::vector<ActiveIndexContext>& contexts_;
    };

    class IndexContextSuspension {
    public:
        explicit IndexContextSuspension(
            std::vector<ActiveIndexContext>& contexts)
            : contexts_(contexts), saved_(std::move(contexts)) {}

        ~IndexContextSuspension() {
            contexts_ = std::move(saved_);
        }

    private:
        std::vector<ActiveIndexContext>& contexts_;
        std::vector<ActiveIndexContext> saved_;
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

    class FunctionNameGuard {
    public:
        FunctionNameGuard(std::vector<std::string>& names,
                          std::string name)
            : names_(names) {
            names_.push_back(std::move(name));
        }

        ~FunctionNameGuard() {
            names_.pop_back();
        }

    private:
        std::vector<std::string>& names_;
    };

    class ExceptionCallSiteGuard {
    public:
        ExceptionCallSiteGuard(
            std::vector<RuntimeExceptionFrame>& frames,
            std::optional<RuntimeExceptionFrame> frame)
            : frames_(frames), active_(frame.has_value()) {
            if (frame) {
                frames_.push_back(std::move(*frame));
            }
        }

        ~ExceptionCallSiteGuard() {
            if (active_) {
                frames_.pop_back();
            }
        }

    private:
        std::vector<RuntimeExceptionFrame>& frames_;
        bool active_ = false;
    };

    std::string publicFunctionName(std::string_view name) const {
        if (name.starts_with("$path") || name.starts_with("$private")) {
            const size_t separator = name.find('>');
            if (separator != std::string_view::npos &&
                separator + 1 < name.size()) {
                return std::string(name.substr(separator + 1));
            }
        }
        return std::string(name);
    }

    std::string sourceFileName(const SourceSpan& span) const {
        if (!semantic_ || span.begin.sourceId == kInvalidSourceId ||
            span.begin.sourceId >= semantic_->sources.size()) {
            return {};
        }
        return semantic_->sources[span.begin.sourceId].name;
    }

    RuntimeExceptionFrame exceptionFrame(
        const SourceSpan& span, std::string name) const {
        RuntimeExceptionFrame frame;
        frame.line = span.begin.line;
        frame.name = std::move(name);
        if (semantic_ && span.begin.sourceId != kInvalidSourceId &&
            span.begin.sourceId < semantic_->sources.size()) {
            frame.file = semantic_->sources[span.begin.sourceId].name;
        }
        return frame;
    }

    std::vector<RuntimeExceptionFrame>
    exceptionFrames(const SourceSpan& span) const {
        std::vector<RuntimeExceptionFrame> frames;
        frames.push_back(exceptionFrame(
            span, activeFunctionNames_.empty()
                      ? std::string("<script>")
                      : activeFunctionNames_.back()));
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

    void addDiagnostic(const HirNode& node, std::string message,
                       std::string identifier =
                           std::string(kRuntimeErrorIdentifier)) {
        Diagnostic diagnostic{node.span, std::move(message),
                              std::move(identifier)};
        diagnostic.stack = exceptionFrames(node.span);
        pendingException_ = runtimeExceptionFromDiagnostic(
            diagnostic, diagnostic.stack);
        diagnostics_.push_back(std::move(diagnostic));
    }

    void raiseException(const HirNode& node,
                        const RuntimeValue& exception,
                        RuntimeExceptionStackPolicy policy) {
        auto prepared = runtimePrepareExceptionForThrow(
            exception, exceptionFrames(node.span), policy);
        if (!prepared.succeeded) {
            addDiagnostic(node, std::move(prepared.error),
                          "MParser:InvalidException");
            return;
        }
        pendingException_ = std::move(prepared.value);
        auto diagnostic =
            runtimeDiagnosticFromException(*pendingException_, node.span);
        diagnostic.span =
            exceptionDiagnosticSpan(*pendingException_, diagnostic.span);
        diagnostics_.push_back(std::move(diagnostic));
    }

    bool diagnosticTrapTriggered() const {
        return diagnosticTrapBase_ &&
               diagnostics_.size() > *diagnosticTrapBase_;
    }

    RuntimeWorkspace& currentFrame() {
        return frames_.back().workspace;
    }

    const RuntimeWorkspace& currentFrame() const {
        return frames_.back().workspace;
    }

    RuntimeWorkspace* workspaceFor(BuiltinWorkspaceScope scope) {
        if (frames_.empty()) {
            return nullptr;
        }
        size_t index = frames_.size() - 1;
        if (scope == BuiltinWorkspaceScope::Base) {
            index = 0;
        } else if (scope == BuiltinWorkspaceScope::Caller) {
            index = frames_.size() > 1 ? frames_.size() - 2 : 0;
        }
        return &frames_[index].workspace;
    }

    std::vector<RuntimeWorkspace*> workspaceAncestorsFor(
        BuiltinWorkspaceScope scope) {
        std::vector<RuntimeWorkspace*> ancestors;
        if (frames_.empty()) {
            return ancestors;
        }
        size_t index = frames_.size() - 1;
        if (scope == BuiltinWorkspaceScope::Base) {
            index = 0;
        } else if (scope == BuiltinWorkspaceScope::Caller) {
            index = frames_.size() > 1 ? frames_.size() - 2 : 0;
        }
        ancestors.reserve(index);
        for (size_t frame = 0; frame < index; ++frame) {
            ancestors.push_back(&frames_[frame].workspace);
        }
        return ancestors;
    }

    const BuiltinRegistry& builtinRegistry() const {
        if (semantic_ && semantic_->builtinRegistry) {
            return *semantic_->builtinRegistry;
        }
        return *defaultBuiltinRegistry();
    }

    void appendBuiltinDiagnostics(
        const HirNode& node,
        std::vector<Diagnostic> diagnostics) {
        for (auto& diagnostic : diagnostics) {
            if (diagnostic.span.begin.sourceId ==
                kInvalidSourceId) {
                diagnostic.span = node.span;
            }
            if (diagnostic.stack.empty()) {
                diagnostic.stack = exceptionFrames(node.span);
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

    std::string persistentFunctionKey(
        const HirNode& function) const {
        if (!function.lexicalClassName.empty()) {
            return function.lexicalClassName + "." + function.label;
        }
        return function.label;
    }

    std::optional<RuntimeValue> loadStoredVariable(
        const HirNode& node) {
        if (node.binding.kind == BindingKind::GlobalVariable) {
            RuntimeValue value =
                sessionState_->declareGlobal(node.label);
            currentFrame()[node.label] = value;
            return value;
        }
        if (node.binding.kind == BindingKind::PersistentVariable) {
            if (activePersistentFunctionKeys_.empty()) {
                addDiagnostic(
                    node,
                    "persistent variable has no active function: " +
                        node.label);
                return std::nullopt;
            }
            RuntimeValue value = sessionState_->declarePersistent(
                callableContext_->identity,
                activePersistentFunctionKeys_.back(), node.label);
            currentFrame()[node.label] = value;
            return value;
        }
        const auto variable = currentFrame().find(node.label);
        return variable == currentFrame().end()
                   ? std::nullopt
                   : std::optional<RuntimeValue>(variable->second);
    }

    void storeVariable(const HirNode& node, RuntimeValue value) {
        if (node.binding.kind == BindingKind::GlobalVariable) {
            sessionState_->storeGlobal(node.label, value);
            currentFrame()[node.label] = std::move(value);
            return;
        }
        if (node.binding.kind == BindingKind::PersistentVariable) {
            if (activePersistentFunctionKeys_.empty()) {
                addDiagnostic(
                    node,
                    "persistent variable has no active function: " +
                        node.label);
                return;
            }
            sessionState_->storePersistent(
                callableContext_->identity,
                activePersistentFunctionKeys_.back(), node.label, value);
            currentFrame()[node.label] = std::move(value);
            return;
        }
        currentFrame()[node.label] = std::move(value);
    }

    void executeWorkspaceDeclaration(const HirNode& node) {
        for (const auto& name : node.children) {
            if (node.kind == HirKind::GlobalDeclaration) {
                currentFrame()[name->label] =
                    sessionState_->declareGlobal(name->label);
                if (frames_.size() == 1) {
                    baseGlobalNames_.insert(name->label);
                }
                continue;
            }
            if (activePersistentFunctionKeys_.empty()) {
                addDiagnostic(
                    *name,
                    "persistent declaration has no active function: " +
                        name->label);
                return;
            }
            currentFrame()[name->label] =
                sessionState_->declarePersistent(
                    callableContext_->identity,
                    activePersistentFunctionKeys_.back(),
                    name->label);
        }
    }

    void collectFunctions(const HirNode& node) {
        if (node.kind == HirKind::Module) {
            for (const auto& child : node.children) {
                if (child->kind == HirKind::Function) {
                    functionsByName_[child->label] = child.get();
                } else if (child->kind == HirKind::Class) {
                    classNames_.insert(child->label);
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
                                        std::nullopt,
                                    std::optional<SourceSpan> callSite =
                                        std::nullopt,
                                    std::optional<size_t> callerOutputCount =
                                        std::nullopt) {
        IndexContextSuspension indexContext(activeIndexContexts_);
        std::optional<RuntimeExceptionFrame> callerFrame;
        if (callSite) {
            callerFrame = exceptionFrame(
                *callSite,
                activeFunctionNames_.empty()
                    ? std::string("<script>")
                    : activeFunctionNames_.back());
        }
        ExceptionCallSiteGuard callSiteGuard(exceptionCallerFrames_,
                                             std::move(callerFrame));
        FunctionNameGuard functionName(
            activeFunctionNames_, publicFunctionName(node.label));
        FunctionNameGuard persistentFunction(
            activePersistentFunctionKeys_,
            persistentFunctionKey(node));

        const FunctionSignature signature = parseFunctionSignature(node);
        const size_t outputCount =
            requestedOutputCount.value_or(signature.outputs.size());
        const size_t reportedOutputCount =
            callerOutputCount.value_or(outputCount);
        const auto missingOutputs = [&] {
            return FunctionCallResult{
                std::vector<RuntimeValue>(outputCount, missingValue())};
        };
        const auto resolution =
            resolveArgumentContracts(node, argumentCatalog_);
        if (!resolution.diagnostics.empty()) {
            diagnostics_.insert(diagnostics_.end(),
                                resolution.diagnostics.begin(),
                                resolution.diagnostics.end());
            return missingOutputs();
        }
        const auto& contracts = resolution.contracts;
        auto contractFor = [&](std::string_view parameter,
                               ArgumentBlockKind blockKind)
            -> const ResolvedArgumentContract* {
            const auto found = std::find_if(
                contracts.begin(), contracts.end(),
                [&](const ResolvedArgumentContract& contract) {
                    return contract.blockKind == blockKind &&
                           contract.name == parameter;
                });
            return found == contracts.end() ? nullptr : &*found;
        };
        std::vector<RuntimeOutputArgumentContract> outputContracts;
        for (const auto& contract : contracts) {
            if (contract.blockKind != ArgumentBlockKind::Output &&
                contract.blockKind != ArgumentBlockKind::RepeatingOutput) {
                continue;
            }
            outputContracts.push_back(RuntimeOutputArgumentContract{
                contract.name, contract.property, contract.span,
                contract.blockKind == ArgumentBlockKind::RepeatingOutput});
        }

        std::vector<std::string> nameValueDeclarations;
        for (const auto& contract : contracts) {
            if (contract.blockKind == ArgumentBlockKind::Input &&
                contract.name.find('.') != std::string::npos) {
                nameValueDeclarations.push_back(contract.name);
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
        if (!functionOutputCountIsValid(signature, outputCount)) {
            addDiagnostic(node, "function output count mismatch for: " +
                                    node.label);
            return missingOutputs();
        }

        frames_.push_back(makeRuntimeFunctionFrame(
            RuntimeCallFrameKind::Function,
            publicFunctionName(node.label), node.span,
            normalized.positionalArgumentCount, reportedOutputCount));
        auto validateValue = [&](RuntimeValue value,
                                 const ResolvedArgumentContract* contract,
                                 std::optional<size_t> occurrence =
                                     std::nullopt)
            -> std::optional<RuntimeValue> {
            if (contract == nullptr) {
                return value;
            }
            auto validation =
                validateRuntimeArgument(std::move(value), contract->property);
            if (!validation.succeeded) {
                std::string argumentName = contract->name;
                if (occurrence) {
                    argumentName += "{" + std::to_string(*occurrence + 1) +
                                    "}";
                }
                diagnostics_.push_back(Diagnostic{
                    contract->span,
                    "argument validation failed for " + node.label + "." +
                        argumentName + ": " +
                        std::move(validation.error)});
                return std::nullopt;
            }
            return std::move(validation.value);
        };

        for (size_t index = 0; index < fixedParameterCount; ++index) {
            const std::string& parameterName = signature.parameters[index];
            const ResolvedArgumentContract* contract =
                contractFor(parameterName, ArgumentBlockKind::Input);
            RuntimeValue value;
            if (index < positionalArguments.size()) {
                value = positionalArguments[index];
            } else if (contract != nullptr &&
                       contract->property.hasExplicitDefault &&
                       contract->declaration != nullptr &&
                       !contract->declaration->children.empty()) {
                const size_t diagnosticCount = diagnostics_.size();
                value =
                    evaluate(*contract->declaration->children.front());
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
                const ResolvedArgumentContract* contract = contractFor(
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
            const ResolvedArgumentContract* contract = contractFor(
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

        RuntimeWorkspace nameValueStructures;
        for (size_t index = 0; index < signature.parameters.size(); ++index) {
            if (functionParameterKind(signature, index) ==
                FunctionParameterKind::NameValue) {
                nameValueStructures.emplace(
                    signature.parameters[index], makeRuntimeStructValue());
            }
        }
        for (const auto& contract : contracts) {
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
            } else if (contract.property.hasExplicitDefault &&
                       contract.declaration != nullptr &&
                       !contract.declaration->children.empty()) {
                const size_t diagnosticCount = diagnostics_.size();
                value =
                    evaluate(*contract.declaration->children.front());
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
            runtimeSetStructField(structure->second, field,
                                  std::move(*validated));
        }
        for (auto& [name, structure] : nameValueStructures) {
            currentFrame()[name] = std::move(structure);
        }
        initializeRuntimeFunctionOutputs(currentFrame(), signature);

        executeChildren(node);

        auto completedFrame = std::move(currentFrame());
        frames_.pop_back();
        const auto outputValidation =
            validateRuntimeFunctionOutputs(completedFrame, outputContracts);
        if (!outputValidation.succeeded) {
            diagnostics_.push_back(Diagnostic{
                outputValidation.span,
                "output argument validation failed for " + node.label + "." +
                    outputValidation.argumentName + ": " +
                    outputValidation.error});
            return missingOutputs();
        }

        FunctionCallResult result;
        result.outputs = collectRuntimeFunctionOutputs(
            completedFrame, signature, outputCount);

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
            if (node.capturesExpressionResult &&
                node.children.size() == 1 && !frames_.empty() &&
                frames_.back().kind == RuntimeCallFrameKind::Script) {
                const HirNode& expression = *node.children.front();
                if (!expressionProducesResult(expression)) {
                    (void)evaluateValues(expression, 0);
                    break;
                }
                const size_t diagnosticBase = diagnostics_.size();
                auto values = evaluateValues(expression, 1, true);
                if (diagnostics_.size() == diagnosticBase &&
                    values.size() == 1 &&
                    values.front().kind != RuntimeValueKind::Missing &&
                    !frames_.empty()) {
                    const RuntimeDisplayFormat displayFormat =
                        sessionState_->displayFormat();
                    const std::string displayText =
                        runtimeFormatConsoleValue(values.front(),
                                                  displayFormat);
                    currentFrame()["ans"] = values.front();
                    expressionResults_.push_back(
                        RuntimeExpressionResult{
                            std::move(values.front()), node.span,
                            node.outputSuppressed,
                            nextConsoleSequence_++, displayText,
                            displayFormat.spacing});
                }
                break;
            }
            if (node.children.size() == 1 &&
                node.children.front()->kind == HirKind::CallOrIndex) {
                (void)evaluateValues(*node.children.front(), 0);
                break;
            }
            if (node.children.size() == 1 &&
                node.children.front()->kind == HirKind::NameRef &&
                node.children.front()->binding.kind ==
                    BindingKind::Builtin) {
                (void)evaluateValues(*node.children.front(), 1, true);
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
        case HirKind::GlobalDeclaration:
        case HirKind::PersistentDeclaration:
            executeWorkspaceDeclaration(node);
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
        case HirKind::CellRow:
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
        const auto single = runtimeRequireSingleValue(
            evaluate(value), "assignment right-hand side");
        if (!single.succeeded) {
            addDiagnostic(value, single.error);
            return;
        }
        assignTarget(target, single.value, nullAssignment);
    }

    bool isRuntimeExceptionMethod(std::string_view name) const {
        return name == "addCause" || name == "getReport" ||
               name == "throw" || name == "rethrow" ||
               name == "throwAsCaller" || name == "addCorrection";
    }

    std::optional<FunctionCallResult> callRuntimeExceptionMethod(
        const HirNode& node, size_t requestedOutputCount,
        std::optional<size_t> callerOutputCount = std::nullopt) {
        if (node.kind != HirKind::CallOrIndex || node.children.empty()) {
            return std::nullopt;
        }
        const HirNode& callee = *node.children.front();
        if (callee.kind != HirKind::MemberAccess ||
            !isRuntimeExceptionMethod(callee.label) ||
            callee.children.empty()) {
            return std::nullopt;
        }

        RuntimeValue receiver = evaluate(*callee.children.front());
        if (!isRuntimeException(receiver)) {
            addDiagnostic(node, "MException method requires an MException "
                                "receiver: " + callee.label,
                          "MParser:InvalidException");
            return FunctionCallResult{
                std::vector<RuntimeValue>(requestedOutputCount,
                                          missingValue())};
        }
        std::vector<RuntimeValue> arguments = evaluateArguments(node);
        arguments.insert(arguments.begin(), std::move(receiver));
        return callBuiltin(node, callee.label, arguments,
                           requestedOutputCount, callerOutputCount);
    }

    std::vector<RuntimeValue> evaluateValues(
        const HirNode& node, size_t requestedOutputCount,
        bool implicitExpressionOutput = false) {
        if (node.kind == HirKind::NameRef &&
            node.binding.kind == BindingKind::Builtin) {
            const BuiltinDescriptor* descriptor =
                builtinRegistry().find(node.label);
            if (descriptor && descriptor->handler &&
                descriptor->inputs.accepts(0)) {
                const size_t outputCount =
                    implicitExpressionOutput
                        ? preferredImplicitOutputCount(descriptor)
                        : requestedOutputCount;
                return callBuiltin(
                           node, node.label, {}, outputCount,
                           implicitExpressionOutput
                               ? std::optional<size_t>{0}
                               : std::nullopt)
                    .outputs;
            }
        }
        if (node.kind == HirKind::CallOrIndex) {
            return evaluateCallOrIndexValues(
                node, requestedOutputCount,
                implicitExpressionOutput);
        }
        if (const auto method =
                callRuntimeExceptionMethod(
                    node, requestedOutputCount,
                    implicitExpressionOutput
                        ? std::optional<size_t>{0}
                        : std::nullopt)) {
            return method->outputs;
        }

        std::vector<RuntimeValue> result;
        appendRuntimeExpandedValues(result, evaluate(node));
        return result;
    }

    bool expressionProducesResult(const HirNode& expression) const {
        if (expression.kind == HirKind::NameRef &&
            expression.binding.kind == BindingKind::Builtin) {
            const auto* descriptor =
                builtinRegistry().find(expression.label);
            return !descriptor ||
                   descriptor->implicitOutputCount(0) != 0;
        }
        if (expression.kind != HirKind::CallOrIndex ||
            expression.children.empty()) {
            return true;
        }
        const HirNode& callee = *expression.children.front();
        if (expression.binding.kind == BindingKind::Builtin) {
            const auto* descriptor = builtinRegistry().find(callee.label);
            return !descriptor ||
                   descriptor->implicitOutputCount(
                       expression.children.size() - 1) != 0;
        }
        if (expression.binding.kind == BindingKind::Function) {
            const auto function = functionsByName_.find(callee.label);
            return function == functionsByName_.end() ||
                   functionOutputCountIsValid(
                       parseFunctionSignature(*function->second), 1);
        }
        return true;
    }

    size_t preferredImplicitOutputCount(
        const BuiltinDescriptor* descriptor,
        size_t suppliedInputCount = 0) const {
        return descriptor
                   ? descriptor->implicitOutputCount(suppliedInputCount)
                   : 1;
    }

    size_t preferredImplicitOutputCount(
        const FunctionSignature& signature) const {
        if (functionOutputCountIsValid(signature, 1)) {
            return 1;
        }
        return functionOutputCountIsValid(signature, 0) ? 0 : 1;
    }

    size_t preferredImplicitHandleOutputCount(
        const RuntimeValue& handle,
        size_t suppliedInputCount = 0) const {
        if (!isFunctionHandle(handle)) {
            return 1;
        }
        const RuntimeFunctionHandle& info = *handle.functionHandle;
        if (info.kind == RuntimeFunctionHandleKind::Anonymous) {
            return 1;
        }
        if (info.kind == RuntimeFunctionHandleKind::Builtin) {
            return preferredImplicitOutputCount(
                builtinRegistry().find(info.targetName),
                suppliedInputCount);
        }
        if (info.kind != RuntimeFunctionHandleKind::Function) {
            return 1;
        }
        const auto function = functionsByName_.find(info.targetName);
        return function == functionsByName_.end()
                   ? 1
                   : preferredImplicitOutputCount(
                         parseFunctionSignature(*function->second));
    }

    void assignTargetList(const HirNode& target,
                          const std::vector<RuntimeValue>& values) {
        if (values.size() != target.children.size()) {
            addDiagnostic(target,
                          values.size() < target.children.size()
                              ? "not enough values to assign output list"
                              : "too many values to assign output list");
        }

        for (size_t index = 0; index < target.children.size(); ++index) {
            const RuntimeValue value =
                index < values.size() ? values[index] : missingValue();
            assignTarget(*target.children[index], value);
        }
    }

    bool collectLvaluePath(
        const HirNode& target, const HirNode*& root,
        std::vector<const HirNode*>& segments) const {
        const HirNode* current = &target;
        while ((current->kind == HirKind::MemberAccess ||
                current->kind == HirKind::CallOrIndex ||
                current->kind == HirKind::BraceIndex) &&
               !current->children.empty()) {
            segments.push_back(current);
            current = current->children.front().get();
        }
        std::reverse(segments.begin(), segments.end());
        root = current;
        return root->kind == HirKind::NameRef && segments.size() > 1;
    }

    std::optional<RuntimeLvalueSegment> evaluateLvalueSegment(
        const HirNode& node, const RuntimeValue& parent) {
        RuntimeLvalueSegment segment;
        if (node.kind == HirKind::MemberAccess) {
            segment.kind = RuntimeLvalueSegmentKind::Member;
            segment.memberName = node.label;
            if (node.label != ".()") {
                return segment;
            }
            if (node.children.size() != 2) {
                addDiagnostic(
                    node,
                    "dynamic member assignment requires one field name "
                    "expression");
                return std::nullopt;
            }
            const auto dynamicName = runtimeStructFieldName(
                evaluate(*node.children[1]));
            if (!dynamicName.succeeded) {
                addDiagnostic(node, dynamicName.error);
                return std::nullopt;
            }
            segment.memberName = dynamicName.name;
            return segment;
        }

        segment.kind = node.kind == HirKind::BraceIndex
                           ? RuntimeLvalueSegmentKind::Brace
                           : RuntimeLvalueSegmentKind::Parenthesis;
        segment.subscripts = evaluateIndexArguments(node, parent);
        segment.colonSubscripts.reserve(
            node.children.empty() ? 0 : node.children.size() - 1);
        for (size_t index = 1; index < node.children.size(); ++index) {
            const HirNode& subscript = *node.children[index];
            segment.colonSubscripts.push_back(
                subscript.kind == HirKind::Literal &&
                subscript.label == ":");
        }
        return segment;
    }

    RuntimeValue missingPathSeed(
        const std::vector<const HirNode*>& segments,
        size_t nextIndex) const {
        const HirNode& nextSegment = *segments[nextIndex];
        if (nextSegment.kind == HirKind::BraceIndex) {
            return cellValueForDimensions({0, 0}, {});
        }
        if (nextSegment.kind == HirKind::CallOrIndex) {
            if (nextIndex + 1 < segments.size()) {
                const HirNode& selected = *segments[nextIndex + 1];
                if (selected.kind == HirKind::MemberAccess) {
                    return makeRuntimeStructValue();
                }
                if (selected.kind == HirKind::BraceIndex) {
                    return cellValueForDimensions({0, 0}, {});
                }
            }
            return matrixValue(0, 0, {});
        }
        return makeRuntimeStructValue();
    }

    bool assignNestedTarget(const HirNode& target,
                            const RuntimeValue& value,
                            bool nullAssignment) {
        const HirNode* root = nullptr;
        std::vector<const HirNode*> segments;
        if (!collectLvaluePath(target, root, segments)) {
            return false;
        }

        const auto variable = loadStoredVariable(*root);
        RuntimeLvalueTransaction transaction(
            variable ? *variable : missingValue());
        for (size_t index = 0; index + 1 < segments.size(); ++index) {
            const auto segment = evaluateLvalueSegment(
                *segments[index], transaction.current());
            if (!segment) {
                return true;
            }
            auto descended = transaction.descend(
                *segment, {}, missingPathSeed(segments, index + 1));
            if (!descended.succeeded) {
                addDiagnostic(target, std::move(descended.error));
                return true;
            }
        }

        const auto finalSegment = evaluateLvalueSegment(
            *segments.back(), transaction.current());
        if (!finalSegment) {
            return true;
        }
        auto assigned = transaction.assign(
            *finalSegment, value, nullAssignment);
        if (!assigned.succeeded) {
            addDiagnostic(target, std::move(assigned.error));
            return true;
        }
        storeVariable(*root, transaction.root());
        return true;
    }

    void assignTarget(const HirNode& target, const RuntimeValue& value,
                      bool nullAssignment = false) {
        if (assignNestedTarget(target, value, nullAssignment)) {
            return;
        }
        switch (target.kind) {
        case HirKind::NameRef:
            storeVariable(target, value);
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
        const HirNode& root = *target.children.front();
        const std::string& name = root.label;
        std::string fieldName = target.label;
        if (target.label == ".()") {
            if (target.children.size() != 2) {
                addDiagnostic(target,
                              "dynamic member assignment requires one field "
                              "name expression");
                return;
            }
            const auto dynamicName = runtimeStructFieldName(
                evaluate(*target.children[1]));
            if (!dynamicName.succeeded) {
                addDiagnostic(target, dynamicName.error);
                return;
            }
            fieldName = dynamicName.name;
        }

        const auto variable = loadStoredVariable(root);
        RuntimeValue updated =
            variable ? *variable : makeRuntimeStructValue();
        if (!isStruct(updated)) {
            addDiagnostic(target,
                          "member assignment requires a structure target: " +
                              name);
            return;
        }
        if (!runtimeSetStructField(updated, std::move(fieldName),
                                   value)) {
            addDiagnostic(
                target,
                "direct field assignment requires a scalar structure");
            return;
        }
        storeVariable(root, std::move(updated));
    }

    void assignIndexedTarget(const HirNode& target, const RuntimeValue& value,
                             bool nullAssignment) {
        if (target.children.empty()) {
            addDiagnostic(target, "indexed assignment is missing a target");
            return;
        }
        const HirNode& callee = *target.children.front();
        if (callee.kind != HirKind::NameRef) {
            addDiagnostic(target,
                          "indexed assignment currently requires a variable "
                          "target");
            return;
        }

        const auto variable = loadStoredVariable(callee);
        if (!variable) {
            addDiagnostic(target,
                          "indexed assignment target is not defined: " +
                              callee.label);
            return;
        }
        RuntimeValue targetValue = *variable;
        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(target, targetValue);
        if (arguments.empty()) {
            addDiagnostic(target,
                          "indexed assignment requires subscripts");
            return;
        }
        if (isStruct(targetValue)) {
            RuntimeStructOperationResult result;
            if (nullAssignment) {
                result = runtimeDeleteStructIndexed(targetValue, arguments);
            } else {
                result = runtimeAssignStructIndexed(targetValue, arguments,
                                                    value);
            }
            if (!result.succeeded) {
                addDiagnostic(target, result.error);
                return;
            }
            storeVariable(callee, std::move(result.value));
            return;
        }

        std::vector<bool> colonSubscripts;
        colonSubscripts.reserve(target.children.size() - 1);
        for (size_t index = 1; index < target.children.size(); ++index) {
            const HirNode& subscript = *target.children[index];
            colonSubscripts.push_back(
                subscript.kind == HirKind::Literal &&
                subscript.label == ":");
        }
        if (isCell(targetValue)) {
            auto result = nullAssignment
                              ? runtimeDeleteCellIndexed(
                                    targetValue, arguments,
                                    colonSubscripts)
                              : runtimeAssignCellIndexed(
                                    targetValue, arguments, value);
            if (!result.succeeded) {
                addDiagnostic(target, std::move(result.error));
                return;
            }
            storeVariable(callee, std::move(result.value));
            return;
        }
        if (isRuntimeTextValue(targetValue)) {
            const auto result = nullAssignment
                                    ? runtimeDeleteTextIndexed(
                                          targetValue, arguments,
                                          colonSubscripts)
                                    : runtimeAssignTextIndexed(
                                          targetValue, arguments, value);
            if (!result.succeeded) {
                addDiagnostic(target, result.error);
                return;
            }
            storeVariable(callee, std::move(targetValue));
            return;
        }

        if (targetValue.kind == RuntimeValueKind::MissingArray) {
            const auto result =
                nullAssignment
                    ? runtimeDeleteMissingIndexed(
                          targetValue, arguments, colonSubscripts)
                    : runtimeAssignMissingIndexed(
                          targetValue, arguments, value);
            if (!result.succeeded) {
                addDiagnostic(target, result.error);
                return;
            }
            storeVariable(callee, std::move(targetValue));
            return;
        }

        if (!isNumeric(targetValue) ||
            (!isNumeric(value) &&
             value.kind != RuntimeValueKind::MissingArray)) {
            addDiagnostic(target,
                          "indexed assignment requires compatible numeric, "
                          "missing, text, Cell, or structure values");
            return;
        }

        RuntimeNumericAssignmentResult result;
        if (nullAssignment) {
            result = runtimeDeleteNumericIndexed(
                targetValue, arguments, colonSubscripts);
        } else {
            result = runtimeAssignNumericIndexed(
                targetValue, arguments, value);
        }
        if (!result.succeeded) {
            addDiagnostic(target, result.error);
            return;
        }
        storeVariable(callee, std::move(targetValue));
    }

    void assignBraceIndexedTarget(const HirNode& target,
                                  const RuntimeValue& value) {
        if (target.children.empty() ||
            target.children.front()->kind != HirKind::NameRef) {
            addDiagnostic(target,
                          "brace assignment currently requires a variable target");
            return;
        }

        const HirNode& root = *target.children.front();
        const auto variable = loadStoredVariable(root);
        if (!variable) {
            addDiagnostic(target, "brace assignment target is not defined: " +
                                      root.label);
            return;
        }
        RuntimeValue indexed = *variable;

        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(target, indexed);
        if (isRuntimeStringArray(indexed)) {
            const auto result = runtimeAssignStringContents(
                indexed, arguments, value);
            if (!result.succeeded) {
                addDiagnostic(target, result.error);
                return;
            }
            storeVariable(root, std::move(indexed));
            return;
        }
        if (!isCell(indexed)) {
            addDiagnostic(target,
                          "brace assignment requires a cell or string target");
            return;
        }
        auto result = runtimeAssignCellContents(indexed, arguments, value);
        if (!result.succeeded) {
            addDiagnostic(target, std::move(result.error));
            return;
        }
        storeVariable(root, std::move(result.value));
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

        const HirNode& loopTarget = *assignment.children.front();
        const RuntimeValue range = evaluate(*assignment.children[1]);
        const auto values = runtimeNumericForLoopColumns(range);
        if (!values) {
            addDiagnostic(assignment,
                          "for loop range must be a valid numeric array");
            return;
        }

        LoopDepthGuard loop(loopDepth_);
        for (const RuntimeValue& value : *values) {
            storeVariable(loopTarget, value);
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
        const RuntimeValue conditionValue =
            evaluateHeader(*node.children.front());
        if (diagnosticTrapTriggered()) {
            return;
        }
        const auto condition = runtimeNumericTruthValue(conditionValue);
        if (!condition) {
            addDiagnostic(
                node,
                "if condition must be a real numeric value without NaN");
            return;
        }
        if (*condition) {
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
            if (arm.label == "elseif" && !arm.children.empty()) {
                const RuntimeValue armValue =
                    evaluateHeader(*arm.children.front());
                if (diagnosticTrapTriggered()) {
                    return;
                }
                const auto armCondition = runtimeNumericTruthValue(armValue);
                if (!armCondition) {
                    addDiagnostic(
                        arm,
                        "elseif condition must be a real numeric value without NaN");
                    return;
                }
                if (*armCondition) {
                    executeRange(node, current + 1, next);
                    return;
                }
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
        while (true) {
            const RuntimeValue conditionValue = evaluateHeader(header);
            if (diagnosticTrapTriggered()) {
                return;
            }
            const auto condition = runtimeNumericTruthValue(conditionValue);
            if (!condition) {
                addDiagnostic(
                    node,
                    "while condition must be a real numeric value without NaN");
                return;
            }
            if (!*condition) {
                break;
            }
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

        const Diagnostic diagnostic = diagnostics_.back();
        RuntimeValue exception = pendingException_.value_or(
            runtimeExceptionFromDiagnostic(
                diagnostic, exceptionFrames(diagnostic.span)));
        pendingException_.reset();
        diagnostics_.resize(diagnosticBase);

        const HirNode& arm = *node.children[*catchArm];
        if (const auto name = catchVariableName(arm)) {
            currentFrame()[*name] = std::move(exception);
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
            const HirNode& assignment =
                *header.children.front();
            executeAssignment(assignment);
            if (!assignment.children.empty() &&
                assignment.children.front()->kind ==
                    HirKind::NameRef) {
                const auto value = loadStoredVariable(
                    *assignment.children.front());
                return value ? *value : missingValue();
            }
            return missingValue();
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
        case HirKind::CellRow:
            return evaluateCellRow(node);
        case HirKind::MemberAccess:
            return evaluateMemberAccess(node);
        case HirKind::NameValueArgument:
            return evaluateNameValueArgument(node);
        case HirKind::CallOrIndex:
            return evaluateCallOrIndex(node);
        case HirKind::BraceIndex:
            return evaluateBraceIndex(node);
        case HirKind::FunctionHandle:
            return evaluateFunctionHandle(node);
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
        case HirKind::GlobalDeclaration:
        case HirKind::PersistentDeclaration:
        case HirKind::Property:
        case HirKind::Event:
        case HirKind::EnumerationMember:
        case HirKind::MethodPrototype:
        case HirKind::Control:
        case HirKind::ControlArm:
        case HirKind::OutputList:
        case HirKind::ParameterList:
        case HirKind::SuperclassCall:
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
        if (node.children.empty()) {
            return cellValueForDimensions({0, 0}, {});
        }

        std::vector<RuntimeValue> rows;
        rows.reserve(node.children.size());
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(rows, evaluate(*child));
        }
        auto result = runtimeArrayOperationBuiltin("vertcat", rows);
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue evaluateCellRow(const HirNode& node) {
        std::vector<RuntimeValue> values;
        values.reserve(node.children.size());
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(values, evaluate(*child));
        }
        return cellValue(std::move(values));
    }

    RuntimeValue evaluateNameValueArgument(const HirNode& node) {
        if (node.children.size() != 1) {
            addDiagnostic(node, "name=value argument requires one value");
            return missingValue();
        }
        const auto value = runtimeRequireSingleValue(
            evaluate(*node.children.front()), "name=value argument");
        if (!value.succeeded) {
            addDiagnostic(node, value.error);
            return missingValue();
        }
        return makeRuntimeNameValueArgument(node.label, value.value);
    }

    RuntimeValue evaluateMemberAccess(const HirNode& node) {
        if (node.children.empty()) {
            addDiagnostic(node, "member access is missing a target");
            return missingValue();
        }
        const RuntimeValue target = evaluate(*node.children.front());
        if (!isStruct(target) && !isRuntimeException(target)) {
            addDiagnostic(node,
                          "member access requires a structure or MException "
                          "in the reference interpreter");
            return missingValue();
        }
        std::string fieldName = node.label;
        if (node.label == ".()") {
            if (node.children.size() != 2) {
                addDiagnostic(node,
                              "dynamic member access requires one field name "
                              "expression");
                return missingValue();
            }
            const auto dynamicName = runtimeStructFieldName(
                evaluate(*node.children[1]));
            if (!dynamicName.succeeded) {
                addDiagnostic(node, dynamicName.error);
                return missingValue();
            }
            fieldName = dynamicName.name;
        }
        if (isRuntimeException(target)) {
            const RuntimeValue* property =
                runtimeExceptionProperty(target, fieldName);
            if (!property) {
                addDiagnostic(node, "MException property is not available: " +
                                        fieldName);
                return missingValue();
            }
            return *property;
        }
        auto field = runtimeStructFieldValues(target, fieldName);
        if (!field.succeeded) {
            addDiagnostic(node, field.error);
            return missingValue();
        }
        return std::move(field.value);
    }

    RuntimeValue evaluatePostfix(const HirNode& node) {
        if (node.children.empty()) {
            return missingValue();
        }

        const RuntimeValue value = evaluate(*node.children.front());
        if (node.label != "'" && node.label != ".'") {
            addDiagnostic(node, "unsupported postfix operator: " + node.label);
            return missingValue();
        }

        if (isNumeric(value)) {
            auto result = runtimeTransposeNumeric(
                value, node.label == "'");
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (value.kind == RuntimeValueKind::MissingArray ||
            isRuntimeTextValue(value)) {
            if (runtimeDimensionCount(value) > 2) {
                addDiagnostic(node,
                              "transpose requires a two-dimensional array");
                return missingValue();
            }
            auto result = runtimeArrayOperationBuiltin(
                "permute", {value, vectorValue({2.0, 1.0})});
            if (!result.succeeded) {
                addDiagnostic(node, result.error);
                return missingValue();
            }
            return std::move(result.value);
        }

        addDiagnostic(
            node,
            "transpose requires missing, numeric, or text input");
        return missingValue();
    }

    RuntimeValue evaluateName(const HirNode& node) {
        if (node.binding.kind == BindingKind::GlobalVariable ||
            node.binding.kind == BindingKind::PersistentVariable) {
            const auto variable = loadStoredVariable(node);
            return variable ? *variable : missingValue();
        }
        const auto variable = currentFrame().find(node.label);
        if (variable != currentFrame().end()) {
            return variable->second;
        }

        if (node.binding.kind == BindingKind::Builtin) {
            const BuiltinDescriptor* descriptor =
                builtinRegistry().find(node.label);
            const std::string_view builtinName =
                descriptor ? std::string_view(descriptor->name)
                           : std::string_view(node.label);
            if (descriptor && descriptor->handler &&
                descriptor->inputs.accepts(0)) {
                auto result = callBuiltin(
                    node, node.label, {}, 1);
                return firstOutput(std::move(result));
            }
            if (builtinName == "clc" ||
                builtinName == "tic" || builtinName == "toc") {
                return firstOutput(callBuiltin(node, node.label, {}, 1));
            }
            if (builtinName == "pi") {
                return numberValue(3.14159265358979323846);
            }
            if (builtinName == "i" || builtinName == "j") {
                return *runtimeParseNumericLiteral("1i");
            }
            if (builtinName == "eps") {
                return numberValue(std::numeric_limits<double>::epsilon());
            }
            if (builtinName == "inf") {
                return numberValue(std::numeric_limits<double>::infinity());
            }
            if (builtinName == "nan") {
                return numberValue(std::numeric_limits<double>::quiet_NaN());
            }
            if (builtinName == "true") {
                return logicalValue(true);
            }
            if (builtinName == "false") {
                return logicalValue(false);
            }
        }

        addDiagnostic(node, "unknown runtime variable: " + node.label);
        return missingValue();
    }

    RuntimeValue evaluateLiteral(const HirNode& node) {
        if (node.label == "end" || node.label == ":") {
            if (!activeIndexContexts_.empty()) {
                const ActiveIndexContext& context =
                    activeIndexContexts_.back();
                if (context.target) {
                    const size_t end = static_cast<size_t>(
                        endValueForIndex(*context.target,
                                         context.position,
                                         context.total));
                    return node.label == "end"
                               ? numberValue(static_cast<double>(end))
                               : oneBasedIndexRange(end);
                }
            }
            addDiagnostic(node, "literal is not executable in this context: " +
                                    node.label);
            return missingValue();
        }

        if (node.raw.size() >= 2 &&
            (node.raw.front() == '\'' || node.raw.front() == '"')) {
            const std::string decoded = decodeStringLiteral(node.raw);
            return node.raw.front() == '\''
                       ? makeRuntimeCharacterVectorUtf8(decoded)
                       : makeRuntimeStringScalarUtf8(decoded);
        }

        if (auto number = runtimeParseNumericLiteral(node.label)) {
            return std::move(*number);
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

        auto result = runtimeApplyNumericUnary(node.label, value);
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue evaluateMatrix(const HirNode& node) {
        if (node.children.empty()) {
            return matrixValue(0, 0, {});
        }

        if (node.children.front()->kind != HirKind::MatrixRow) {
            std::vector<RuntimeValue> values;
            for (const auto& child : node.children) {
                appendRuntimeExpandedValues(values, evaluate(*child));
            }
            return concatenateMatrixLiteral(node, "horzcat", values);
        }

        std::vector<RuntimeValue> rows;
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(rows, evaluate(*child));
        }
        return concatenateMatrixLiteral(node, "vertcat", rows);
    }

    RuntimeValue evaluateMatrixRow(const HirNode& node) {
        std::vector<RuntimeValue> values;
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(values, evaluate(*child));
        }
        return concatenateMatrixLiteral(node, "horzcat", values);
    }

    RuntimeValue concatenateMatrixLiteral(
        const HirNode& node, std::string_view operation,
        const std::vector<RuntimeValue>& values) {
        auto result = runtimeArrayOperationBuiltin(operation, values);
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return missingValue();
        }
        return std::move(result.value);
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

            const auto leftValue = runtimeNumericTruthValue(left);
            if (!leftValue) {
                addDiagnostic(
                    node,
                    "short-circuit operands must be real numeric values without NaN");
                return missingValue();
            }
            if (node.label == "&&" && !*leftValue) {
                return logicalValue(false);
            }
            if (node.label == "||" && *leftValue) {
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
            const auto rightValue = runtimeNumericTruthValue(right);
            if (!rightValue) {
                addDiagnostic(
                    node,
                    "short-circuit operands must be real numeric values without NaN");
                return missingValue();
            }
            return logicalValue(*rightValue);
        }

        const RuntimeValue right = evaluate(*node.children[1]);
        if (isText(left) || isText(right)) {
            if (isText(left) && isText(right)) {
                RuntimeTextOperationResult result;
                if (node.label == "+" &&
                    (isRuntimeStringArray(left) ||
                     isRuntimeStringArray(right))) {
                    result = runtimeAppendText(left, right);
                } else {
                    result = runtimeCompareText(node.label, left, right);
                }
                if (result.succeeded) {
                    return std::move(result.value);
                }
                addDiagnostic(node, result.error);
                return missingValue();
            }

            addDiagnostic(node,
                          "text binary operators require compatible text values");
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
        auto result = runtimeApplyNumericBinary(node.label, left, right);
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue evaluateColon(const HirNode& node) {
        const size_t diagnosticBase = diagnostics_.size();
        std::vector<RuntimeValue> terms;
        collectColonTerms(node, terms);
        if (diagnostics_.size() != diagnosticBase) {
            return missingValue();
        }
        auto range = runtimeMaterializeColonValue(terms);
        if (!range.succeeded) {
            addDiagnostic(node, range.error);
            return missingValue();
        }
        return std::move(range.value);
    }

    void collectColonTerms(const HirNode& node,
                           std::vector<RuntimeValue>& terms) {
        if (node.kind == HirKind::Binary && node.label == ":") {
            for (const auto& child : node.children) {
                collectColonTerms(*child, terms);
            }
            return;
        }

        const RuntimeValue value = evaluate(node);
        terms.push_back(value);
    }

    std::vector<RuntimeValue> evaluateArguments(const HirNode& node) {
        std::vector<RuntimeValue> arguments;
        for (size_t index = 1; index < node.children.size(); ++index) {
            appendRuntimeExpandedValues(
                arguments, evaluate(*node.children[index]));
        }
        return arguments;
    }

    std::vector<RuntimeValue> evaluateIndexArguments(
        const HirNode& node, const RuntimeValue& target) {
        std::vector<RuntimeValue> arguments;
        const size_t total = node.children.size() - 1;
        for (size_t index = 1; index < node.children.size(); ++index) {
            IndexContextGuard context(
                activeIndexContexts_, target, index - 1, total);
            appendRuntimeExpandedValues(
                arguments,
                evaluateWithIndexContext(*node.children[index], target,
                                         index - 1, total));
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

        auto result = runtimeApplyNumericUnary(node.label, value);
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return missingValue();
        }
        return std::move(result.value);
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
        const size_t diagnosticBase = diagnostics_.size();
        std::vector<RuntimeValue> terms;
        collectColonTermsWithIndexContext(node, target, position, total, terms);
        if (diagnostics_.size() != diagnosticBase) {
            return missingValue();
        }
        auto range = runtimeMaterializeColonValue(terms);
        if (!range.succeeded) {
            addDiagnostic(node, range.error);
            return missingValue();
        }
        return std::move(range.value);
    }

    void collectColonTermsWithIndexContext(
        const HirNode& node, const RuntimeValue& target, size_t position,
        size_t total, std::vector<RuntimeValue>& terms) {
        if (node.kind == HirKind::Binary && node.label == ":") {
            for (const auto& child : node.children) {
                collectColonTermsWithIndexContext(*child, target, position,
                                                  total, terms);
            }
            return;
        }

        const RuntimeValue value =
            evaluateWithIndexContext(node, target, position, total);
        terms.push_back(value);
    }

    RuntimeValue evaluateMatrixWithIndexContext(const HirNode& node,
                                                const RuntimeValue& target,
                                                size_t position, size_t total) {
        if (node.children.empty()) {
            return matrixValue(0, 0, {});
        }

        if (node.children.front()->kind != HirKind::MatrixRow) {
            std::vector<RuntimeValue> values;
            for (const auto& child : node.children) {
                appendRuntimeExpandedValues(
                    values, evaluateWithIndexContext(
                                *child, target, position, total));
            }
            return concatenateMatrixLiteral(node, "horzcat", values);
        }

        std::vector<RuntimeValue> rows;
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(
                rows, evaluateWithIndexContext(
                          *child, target, position, total));
        }
        return concatenateMatrixLiteral(node, "vertcat", rows);
    }

    RuntimeValue evaluateMatrixRowWithIndexContext(
        const HirNode& node, const RuntimeValue& target, size_t position,
        size_t total) {
        std::vector<RuntimeValue> values;
        for (const auto& child : node.children) {
            appendRuntimeExpandedValues(
                values, evaluateWithIndexContext(
                            *child, target, position, total));
        }
        return concatenateMatrixLiteral(node, "horzcat", values);
    }

    RuntimeValue firstOutput(const FunctionCallResult& result) const {
        if (result.outputs.empty()) {
            return missingValue();
        }
        return result.outputs.front();
    }

    std::optional<RuntimeValue> functionHandleFromText(
        const HirNode& node, std::string_view target) {
        if (target.empty()) {
            addDiagnostic(node, "function name string cannot be empty");
            return std::nullopt;
        }
        if (target.front() == '@') {
            addDiagnostic(
                node,
                "str2func does not parse anonymous function text; create "
                "anonymous handles with @(...) syntax");
            return std::nullopt;
        }

        const HirNode* candidate = nullptr;
        for (const auto& [name, function] : functionsByName_) {
            const bool privateFunction = name.starts_with("$private");
            const bool localFunction =
                name.find('>') != std::string::npos &&
                !name.starts_with("$path");
            if (privateFunction || localFunction ||
                (name != target && publicFunctionName(name) != target)) {
                continue;
            }
            if (candidate && candidate != function) {
                addDiagnostic(node, "function name string is ambiguous: " +
                                        std::string(target));
                return std::nullopt;
            }
            candidate = function;
        }

        RuntimeFunctionHandle info;
        info.display = "@" + std::string(target);
        info.span = node.span;
        if (candidate) {
            info.kind = RuntimeFunctionHandleKind::Function;
            info.backend = RuntimeFunctionHandleBackend::Hir;
            info.context = callableContext_;
            info.targetName = candidate->label;
            info.span = candidate->span;
            info.sourceFile = sourceFileName(candidate->span);
            return makeRuntimeFunctionHandleValue(std::move(info));
        }
        if (builtinRegistry().contains(target)) {
            info.kind = RuntimeFunctionHandleKind::Builtin;
            info.backend = RuntimeFunctionHandleBackend::Independent;
            info.targetName = std::string(target);
            return makeRuntimeFunctionHandleValue(std::move(info));
        }

        addDiagnostic(node, "function name string is not available: " +
                                std::string(target));
        return std::nullopt;
    }

    RuntimeValue evaluateFunctionHandle(const HirNode& node) {
        RuntimeFunctionHandle info;
        info.display = node.raw.empty()
                           ? (node.label == "@()" ? std::string("@()")
                                                  : "@" + node.label)
                           : node.raw;
        info.span = node.span;
        info.lexicalClassName = node.lexicalClassName;
        info.sourceFile = sourceFileName(node.span);

        if (node.label == "@()") {
            if (node.children.size() < 2 ||
                node.children.front()->kind != HirKind::ParameterList) {
                addDiagnostic(node,
                              "anonymous function handle requires parameters "
                              "and a body");
                return missingValue();
            }
            info.kind = RuntimeFunctionHandleKind::Anonymous;
            info.backend = RuntimeFunctionHandleBackend::Hir;
            info.context = callableContext_;
            info.parameters =
                anonymousParameterNames(node.children.front()->raw);
            info.capturedVariables = captureRuntimeWorkspace(
                currentFrame(), anonymousFunctionCaptureNames(node));
            info.hirBody = node.children[1].get();
            return makeRuntimeFunctionHandleValue(std::move(info));
        }

        if (node.binding.kind == BindingKind::Method) {
            addDiagnostic(
                node,
                "HIR interpreter does not execute method function handles; "
                "use the bytecode runtime for: " +
                    info.display);
            return missingValue();
        }
        if (node.binding.kind == BindingKind::Function) {
            const auto function = functionsByName_.find(node.label);
            if (function == functionsByName_.end()) {
                addDiagnostic(node,
                              "function handle target is not available: " +
                                  info.display);
                return missingValue();
            }
            info.kind = RuntimeFunctionHandleKind::Function;
            info.backend = RuntimeFunctionHandleBackend::Hir;
            info.context = callableContext_;
            info.targetName = function->first;
            info.span = function->second->span;
            info.sourceFile = sourceFileName(function->second->span);
            return makeRuntimeFunctionHandleValue(std::move(info));
        }
        if (node.binding.kind == BindingKind::Builtin ||
            builtinRegistry().contains(node.label)) {
            info.kind = RuntimeFunctionHandleKind::Builtin;
            info.backend = RuntimeFunctionHandleBackend::Independent;
            info.targetName = node.label;
            return makeRuntimeFunctionHandleValue(std::move(info));
        }

        addDiagnostic(node, "function handle target is not available: " +
                                info.display);
        return missingValue();
    }

    FunctionCallResult callFunctionHandle(
        const HirNode& node, const RuntimeValue& handle,
        const std::vector<RuntimeValue>& arguments,
        size_t requestedOutputCount,
        std::optional<size_t> callerOutputCount = std::nullopt) {
        const auto missingOutputs = [&] {
            return FunctionCallResult{std::vector<RuntimeValue>(
                requestedOutputCount, missingValue())};
        };
        if (!isFunctionHandle(handle)) {
            addDiagnostic(node,
                          "function handle descriptor is unavailable");
            return missingOutputs();
        }

        const RuntimeFunctionHandle& info = *handle.functionHandle;
        if (info.kind != RuntimeFunctionHandleKind::Builtin &&
            (info.backend != RuntimeFunctionHandleBackend::Hir ||
             !info.context || info.context != callableContext_)) {
            addDiagnostic(node,
                          "function handle belongs to a different runtime "
                          "module");
            return missingOutputs();
        }
        if (info.kind == RuntimeFunctionHandleKind::Builtin) {
            return callBuiltin(node, info.targetName, arguments,
                               requestedOutputCount, callerOutputCount);
        }
        if (info.kind == RuntimeFunctionHandleKind::Method) {
            addDiagnostic(
                node,
                "HIR interpreter does not execute method function handles; "
                "use the bytecode runtime for: " +
                    info.display);
            return missingOutputs();
        }
        if (info.kind == RuntimeFunctionHandleKind::Function) {
            const auto function = functionsByName_.find(info.targetName);
            if (function == functionsByName_.end()) {
                addDiagnostic(node,
                              "function handle target is unavailable: " +
                                  info.display);
                return missingOutputs();
            }
            return callFunction(*function->second, arguments, false,
                                requestedOutputCount, node.span,
                                callerOutputCount);
        }

        if (arguments.size() != info.parameters.size()) {
            addDiagnostic(node,
                          "anonymous function argument count mismatch: " +
                              info.display);
            return missingOutputs();
        }
        if (requestedOutputCount > 1) {
            addDiagnostic(node,
                          "anonymous function supports at most one output: " +
                              info.display);
            return missingOutputs();
        }
        if (!info.hirBody) {
            addDiagnostic(node,
                          "anonymous function body is unavailable: " +
                              info.display);
            return missingOutputs();
        }

        frames_.push_back(makeRuntimeFunctionFrame(
            RuntimeCallFrameKind::AnonymousFunction,
            info.display.empty() ? std::string("<anonymous>")
                                 : info.display,
            info.span, arguments.size(),
            callerOutputCount.value_or(requestedOutputCount),
            info.capturedVariables));
        for (size_t index = 0; index < info.parameters.size(); ++index) {
            if (info.parameters[index] != "~") {
                currentFrame()[info.parameters[index]] = arguments[index];
            }
        }

        RuntimeValue output = missingValue();
        const size_t diagnosticCount = diagnostics_.size();
        {
            IndexContextSuspension indexContext(activeIndexContexts_);
            FunctionNameGuard functionName(
                activeFunctionNames_,
                info.display.empty() ? std::string("<anonymous>")
                                     : info.display);
            output = evaluate(*info.hirBody);
        }
        frames_.pop_back();
        if (diagnostics_.size() != diagnosticCount) {
            return missingOutputs();
        }
        return requestedOutputCount == 0
                   ? FunctionCallResult{}
                   : FunctionCallResult{{std::move(output)}};
    }

    std::vector<RuntimeValue> evaluateCallOrIndexValues(
        const HirNode& node, size_t requestedOutputCount,
        bool implicitExpressionOutput = false) {
        if (node.children.empty()) {
            return std::vector<RuntimeValue>(requestedOutputCount,
                                             missingValue());
        }
        const HirNode& callee = *node.children.front();
        const size_t methodOutputCount =
            implicitExpressionOutput &&
                    callee.kind == HirKind::MemberAccess
                ? preferredImplicitOutputCount(
                      builtinRegistry().find(callee.label))
                : requestedOutputCount;
        if (const auto method =
                callRuntimeExceptionMethod(
                    node, methodOutputCount,
                    implicitExpressionOutput
                        ? std::optional<size_t>{0}
                        : std::nullopt)) {
            return method->outputs;
        }

        if (node.label == "system-command") {
            return callBuiltin(node, "system", evaluateArguments(node), 0)
                .outputs;
        }

        const bool runtimeNameShadowsCallable =
            callee.kind == HirKind::NameRef &&
            currentFrame().find(callee.label) != currentFrame().end();

        if (!runtimeNameShadowsCallable &&
            node.binding.kind == BindingKind::Builtin) {
            std::vector<RuntimeValue> arguments = evaluateArguments(node);
            const size_t outputCount =
                implicitExpressionOutput
                    ? preferredImplicitOutputCount(
                          builtinRegistry().find(callee.label),
                          arguments.size())
                    : requestedOutputCount;
            return callBuiltin(
                       node, callee.label, arguments, outputCount,
                       implicitExpressionOutput
                           ? std::optional<size_t>{0}
                           : std::nullopt)
                .outputs;
        }
        if (!runtimeNameShadowsCallable &&
            node.binding.kind == BindingKind::Function) {
            size_t outputCount = requestedOutputCount;
            if (implicitExpressionOutput) {
                const auto function = functionsByName_.find(callee.label);
                if (function != functionsByName_.end()) {
                    outputCount = preferredImplicitOutputCount(
                        parseFunctionSignature(*function->second));
                }
            }
            return callLocalFunction(node, callee.label,
                                     evaluateArguments(node),
                                     outputCount,
                                     implicitExpressionOutput
                                         ? std::optional<size_t>{0}
                                         : std::nullopt)
                .outputs;
        }

        const RuntimeValue target = evaluate(callee);
        if (isFunctionHandle(target)) {
            std::vector<RuntimeValue> arguments = evaluateArguments(node);
            const size_t outputCount =
                implicitExpressionOutput
                    ? preferredImplicitHandleOutputCount(
                          target, arguments.size())
                    : requestedOutputCount;
            return callFunctionHandle(
                       node, target, arguments, outputCount,
                       implicitExpressionOutput
                           ? std::optional<size_t>{0}
                           : std::nullopt)
                .outputs;
        }

        RuntimeValue indexed =
            evaluateIndex(node, target,
                          evaluateIndexArguments(node, target));
        if (requestedOutputCount > 1) {
            addDiagnostic(node,
                          "parenthesis indexing supports at most one output");
            return std::vector<RuntimeValue>(requestedOutputCount,
                                             missingValue());
        }
        return requestedOutputCount == 0
                   ? std::vector<RuntimeValue>{}
                   : std::vector<RuntimeValue>{std::move(indexed)};
    }

    RuntimeValue evaluateCallOrIndex(const HirNode& node) {
        const auto outputs = evaluateCallOrIndexValues(node, 1);
        return outputs.empty() ? missingValue() : outputs.front();
    }

    FunctionCallResult
    callLocalFunction(const HirNode& node, const std::string& name,
                      const std::vector<RuntimeValue>& arguments,
                      size_t requestedOutputCount = 1,
                      std::optional<size_t> callerOutputCount =
                          std::nullopt) {
        const auto function = functionsByName_.find(name);
        if (function == functionsByName_.end()) {
            addDiagnostic(node, "local function is not available: " + name);
            return FunctionCallResult{{missingValue()}};
        }

        return callFunction(*function->second, arguments, false,
                            requestedOutputCount, node.span,
                            callerOutputCount);
    }

    RuntimeValue evaluateBraceIndex(const HirNode& node) {
        if (node.children.empty()) {
            addDiagnostic(node, "brace indexing is missing a target");
            return missingValue();
        }

        const RuntimeValue target = evaluate(*node.children.front());
        const std::vector<RuntimeValue> arguments =
            evaluateIndexArguments(node, target);
        if (isRuntimeStringArray(target)) {
            auto result = runtimeIndexStringContents(target, arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        auto result = runtimeIndexCellContents(target, arguments);
        if (!result.succeeded) {
            addDiagnostic(node, std::move(result.error));
            return missingValue();
        }
        return std::move(result.value);
    }

    RuntimeValue evaluateIndex(const HirNode& node, const RuntimeValue& target,
                               const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty()) {
            addDiagnostic(node, "indexing requires subscripts");
            return missingValue();
        }

        const bool linearColon =
            arguments.size() == 1 && node.children.size() == 2 &&
            node.children[1]->kind == HirKind::Literal &&
            node.children[1]->label == ":";

        if (isStruct(target)) {
            auto result = runtimeIndexStruct(target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(node, result.error);
                return missingValue();
            }
            return std::move(result.value);
        }
        if (isCell(target)) {
            auto result = runtimeIndexCell(target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (isRuntimeTextValue(target)) {
            auto result = runtimeIndexText(target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        if (target.kind == RuntimeValueKind::MissingArray) {
            auto result = runtimeIndexMissingArray(
                target, arguments, linearColon);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return missingValue();
            }
            return std::move(result.value);
        }
        auto result = runtimeIndexNumeric(target, arguments, linearColon);
        if (!result.succeeded) {
            addDiagnostic(node, std::move(result.error));
            return missingValue();
        }
        return std::move(result.value);
    }

    FunctionCallResult
    callBuiltin(const HirNode& node, const std::string& name,
                const std::vector<RuntimeValue>& arguments,
                size_t requestedOutputCount,
                std::optional<size_t> callerOutputCount = std::nullopt) {
        const auto missingOutputs = [&] {
            return FunctionCallResult{std::vector<RuntimeValue>(
                requestedOutputCount, missingValue())};
        };

        if (const BuiltinDescriptor* descriptor =
                builtinRegistry().find(name);
            descriptor &&
            descriptor->implementation !=
                BuiltinImplementationKind::Intrinsic) {
            BuiltinWorkspaceAccess workspace;
            workspace.variables = &currentFrame();
            workspace.resolveVariables = [this](BuiltinWorkspaceScope scope) {
                return workspaceFor(scope);
            };
            workspace.clearVariables = [this] {
                currentFrame().clear();
                if (frames_.size() == 1) {
                    baseGlobalNames_.clear();
                }
            };
            workspace.eraseVariable = [this](std::string_view variable) {
                if (frames_.size() == 1) {
                    baseGlobalNames_.erase(std::string(variable));
                }
                return currentFrame().erase(std::string(variable)) != 0;
            };
            workspace.functionExists = [this](std::string_view name) {
                return std::any_of(
                    functionsByName_.begin(), functionsByName_.end(),
                    [this, name](const auto& entry) {
                        return entry.first == name ||
                               publicFunctionName(entry.first) == name;
                    });
            };
            workspace.classExists = [this](std::string_view name) {
                return classNames_.contains(std::string(name));
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
            context.outputSink = &runtimeOutputSink_;
            context.systemContext =
                sessionState_->systemContext().get();
            context.displayFormat = &displayFormat;
            context.registry = &builtinRegistry();
            if (hasBuiltinContextPermission(
                    descriptor->contextPermissions,
                    BuiltinContextPermission::DynamicCall)) {
                context.dynamicInvoker =
                    [this, &node](
                    const RuntimeValue& callable,
                    const std::vector<RuntimeValue>& callbackArguments,
                    size_t callbackOutputCount, SourceSpan) {
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
                        handle = functionHandleFromText(node, *text);
                    } else {
                        addDiagnostic(
                            node,
                            "dynamic builtin callback expects a function "
                            "handle or function name string");
                    }
                    if (handle) {
                        outputs = callFunctionHandle(
                                      node, *handle, callbackArguments,
                                      callbackOutputCount)
                                      .outputs;
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
                    [this](const BuiltinSourceEvaluationRequest& request) {
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
                    options.outputSink = runtimeOutputSink_;
                    options.inheritedWorkspaceFrames =
                        workspaceAncestorsFor(request.workspace);
                    return evaluateRuntimeSource(request, *target, options);
                };
            }
            BuiltinResult result = builtinRegistry().invoke(
                name, BuiltinCall{arguments, requestedOutputCount,
                                  node.span, &context,
                                  callerOutputCount});
            appendBuiltinDiagnostics(
                node, std::move(result.diagnostics));
            if (!result.succeeded) {
                return missingOutputs();
            }
            return FunctionCallResult{std::move(result.outputs)};
        }

        if (name == "feval") {
            if (arguments.empty()) {
                addDiagnostic(
                    node,
                    "feval expects a function handle or function name string");
                return missingOutputs();
            }

            RuntimeValue handle;
            if (isFunctionHandle(arguments.front())) {
                handle = arguments.front();
            } else if (const auto text =
                           runtimeTextScalarUtf8(arguments.front())) {
                const auto resolved = functionHandleFromText(node, *text);
                if (!resolved) {
                    return missingOutputs();
                }
                handle = *resolved;
            } else {
                addDiagnostic(
                    node,
                    "feval expects a function handle or function name string");
                return missingOutputs();
            }

            return callFunctionHandle(
                node, handle,
                std::vector<RuntimeValue>(arguments.begin() + 1,
                                          arguments.end()),
                requestedOutputCount, callerOutputCount);
        }

        if (name == "str2func" || name == "func2str" ||
            name == "functions") {
            if (requestedOutputCount > 1) {
                addDiagnostic(node, name + " supports at most one output");
                return missingOutputs();
            }
            if (arguments.size() != 1) {
                addDiagnostic(node, name + " expects exactly one argument");
                return missingOutputs();
            }

            RuntimeValue result;
            if (name == "str2func") {
                const auto text = runtimeTextScalarUtf8(arguments.front());
                if (!text) {
                    addDiagnostic(node,
                                  "str2func expects a function name string");
                    return missingOutputs();
                }
                const auto resolved = functionHandleFromText(node, *text);
                if (!resolved) {
                    return missingOutputs();
                }
                result = *resolved;
            } else {
                if (!isFunctionHandle(arguments.front())) {
                    addDiagnostic(node,
                                  name + " expects a function handle");
                    return missingOutputs();
                }
                result = name == "func2str"
                             ? characterValue(runtimeFunctionHandleText(
                                   arguments.front()))
                             : runtimeFunctionHandleMetadata(
                                   arguments.front());
            }
            return requestedOutputCount == 0
                       ? FunctionCallResult{}
                       : FunctionCallResult{{std::move(result)}};
        }

        if (name == "MException") {
            if (requestedOutputCount > 1) {
                addDiagnostic(node,
                              "MException supports at most one output",
                              "MParser:InvalidException");
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            auto result = runtimeConstructMException(arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error),
                              "MParser:InvalidException");
                return FunctionCallResult{{missingValue()}};
            }
            return requestedOutputCount == 0
                       ? FunctionCallResult{}
                       : FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "addCause") {
            if (requestedOutputCount > 1) {
                addDiagnostic(node, "addCause supports at most one output",
                              "MParser:InvalidException");
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            auto result = runtimeAddExceptionCause(arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error),
                              "MParser:InvalidException");
                return FunctionCallResult{{missingValue()}};
            }
            return requestedOutputCount == 0
                       ? FunctionCallResult{}
                       : FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "getReport") {
            if (requestedOutputCount > 1) {
                addDiagnostic(node, "getReport supports at most one output",
                              "MParser:InvalidException");
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            auto result = runtimeGetExceptionReport(arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error),
                              "MParser:InvalidException");
                return FunctionCallResult{{missingValue()}};
            }
            return requestedOutputCount == 0
                       ? FunctionCallResult{}
                       : FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "addCorrection") {
            addDiagnostic(
                node,
                "MException correction objects are outside the supported "
                "exception subset",
                "MParser:UnsupportedExceptionCorrection");
            return FunctionCallResult{
                std::vector<RuntimeValue>(requestedOutputCount,
                                          missingValue())};
        }
        if (name == "error") {
            auto result = runtimeCreateErrorException(arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error),
                              "MParser:InvalidException");
                return FunctionCallResult{{missingValue()}};
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
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            raiseException(
                node, result.value,
                runtimeExceptionFrameCount(result.value) == 0
                    ? RuntimeExceptionStackPolicy::Replace
                    : RuntimeExceptionStackPolicy::Preserve);
            return FunctionCallResult{{missingValue()}};
        }
        if (name == "throw" || name == "rethrow" ||
            name == "throwAsCaller") {
            if (arguments.size() != 1) {
                addDiagnostic(node, name + " expects one MException object",
                              "MParser:InvalidException");
                return FunctionCallResult{{missingValue()}};
            }
            const auto policy =
                name == "rethrow"
                    ? RuntimeExceptionStackPolicy::Preserve
                    : name == "throwAsCaller"
                          ? RuntimeExceptionStackPolicy::AsCaller
                          : RuntimeExceptionStackPolicy::Replace;
            raiseException(node, arguments.front(), policy);
            return FunctionCallResult{{missingValue()}};
        }
        if (name == "assert") {
            if (requestedOutputCount != 0) {
                addDiagnostic(node, "assert does not produce outputs",
                              "MParser:InvalidAssertion");
                return FunctionCallResult{
                    std::vector<RuntimeValue>(requestedOutputCount,
                                              missingValue())};
            }
            if (arguments.empty() || !isNumeric(arguments.front())) {
                addDiagnostic(node,
                              "assert condition must be numeric or logical",
                              "MParser:InvalidAssertion");
                return FunctionCallResult{{missingValue()}};
            }
            const auto condition = runtimeNumericTruthValue(
                arguments.front());
            if (!condition) {
                addDiagnostic(
                    node,
                    "assert condition must be a real numeric value without NaN",
                    "MParser:InvalidAssertion");
                return FunctionCallResult{{missingValue()}};
            }
            if (*condition) {
                return {};
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
                addDiagnostic(node, std::move(result.error),
                              "MParser:InvalidAssertion");
                return FunctionCallResult{{missingValue()}};
            }
            raiseException(node, result.value,
                           RuntimeExceptionStackPolicy::Replace);
            return FunctionCallResult{{missingValue()}};
        }
        if (name == "clc" || name == "tic" ||
            name == "toc") {
            if (!arguments.empty()) {
                addDiagnostic(node, name + " currently expects no arguments");
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
        if (name == "strcmp" || name == "strcmpi") {
            return callStrcmpBuiltin(node, name, arguments);
        }
        if (name == "char" || name == "string" || name == "cellstr" ||
            name == "strlength" || name == "ismissing") {
            if (arguments.size() != 1) {
                addDiagnostic(node, name + " expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            RuntimeTextOperationResult result;
            if (name == "char") {
                result = runtimeConvertToCharacter(arguments.front());
            } else if (name == "string") {
                result = runtimeConvertToString(arguments.front());
            } else if (name == "cellstr") {
                result = runtimeCellstr(arguments.front());
            } else if (name == "strlength") {
                result = runtimeStringLengths(arguments.front());
            } else {
                result = runtimeTextMissingMask(arguments.front());
            }
            if (!result.succeeded) {
                addDiagnostic(node, result.error);
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "ischar" || name == "isstring" ||
            name == "isStringScalar") {
            if (arguments.size() != 1) {
                addDiagnostic(node, name + " expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            const bool result = name == "ischar"
                                    ? isRuntimeCharacterArray(arguments.front())
                                : name == "isstring"
                                    ? isRuntimeStringArray(arguments.front())
                                    : isRuntimeStringScalar(arguments.front());
            return FunctionCallResult{{logicalValue(result)}};
        }
        if (name == "struct") {
            auto result = runtimeConstructScalarStruct(arguments);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "isfield") {
            if (arguments.size() != 2) {
                addDiagnostic(node, "isfield expects two arguments");
                return FunctionCallResult{{missingValue()}};
            }
            auto result = runtimeStructIsField(arguments[0], arguments[1]);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "fieldnames") {
            if (arguments.size() != 1) {
                addDiagnostic(node, "fieldnames expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            auto result = runtimeStructFieldNames(arguments.front());
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "rmfield") {
            if (arguments.size() != 2) {
                addDiagnostic(node, "rmfield expects two arguments");
                return FunctionCallResult{{missingValue()}};
            }
            auto result = runtimeRemoveStructFields(arguments[0],
                                                    arguments[1]);
            if (!result.succeeded) {
                addDiagnostic(node, std::move(result.error));
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
        }
        if (name == "isstruct") {
            if (arguments.size() != 1) {
                addDiagnostic(node, "isstruct expects one argument");
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{logicalValue(
                isStruct(arguments.front()))}};
        }
        if (name == "double" && arguments.size() == 1 &&
            isRuntimeCharacterArray(arguments.front())) {
            auto result = runtimeCharacterCodes(arguments.front());
            if (!result.succeeded) {
                addDiagnostic(node, result.error);
                return FunctionCallResult{{missingValue()}};
            }
            return FunctionCallResult{{std::move(result.value)}};
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
            return FunctionCallResult{{characterValue(
                runtimeValueClassName(arguments.front()))}};
        }
        if (name == "isa") {
            const auto target = arguments.size() == 2
                                    ? runtimeTextScalarUtf8(arguments[1])
                                    : std::nullopt;
            if (!target) {
                addDiagnostic(node,
                              "isa expects a value and class-name string");
                return FunctionCallResult{{missingValue()}};
            }
            const RuntimeValue& value = arguments.front();
            bool matches = false;
            if (isNumeric(value)) {
                matches = *target ==
                              runtimeNumericClassName(value.numericClass) ||
                          (*target == "numeric" &&
                           value.numericClass !=
                               RuntimeNumericClass::Logical);
            } else if (isRuntimeCharacterArray(value)) {
                matches = *target == "char";
            } else if (isRuntimeStringArray(value)) {
                matches = *target == "string";
            } else if (isCell(value)) {
                matches = *target == "cell";
            } else if (isStruct(value)) {
                matches = *target == "struct";
            } else if (isRuntimeException(value)) {
                matches = *target == kRuntimeExceptionClassName;
            }
            return FunctionCallResult{{logicalValue(matches)}};
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

        addDiagnostic(node, "builtin is not executable yet: " + name);
        return FunctionCallResult{{missingValue()}};
    }

    FunctionCallResult
    callStrcmpBuiltin(const HirNode& node, std::string_view name,
                      const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 || !isText(arguments[0]) ||
            !isText(arguments[1])) {
            addDiagnostic(node, std::string(name) +
                                    " expects two text arguments");
            return FunctionCallResult{{missingValue()}};
        }
        auto result = runtimeCompareText(
            name, arguments[0], arguments[1], name == "strcmpi");
        if (!result.succeeded) {
            addDiagnostic(node, result.error);
            return FunctionCallResult{{missingValue()}};
        }
        return FunctionCallResult{{std::move(result.value)}};
    }

    const SemanticResult* semantic_ = nullptr;
    std::shared_ptr<RuntimeCallableContext> callableContext_;
    std::shared_ptr<RuntimeSessionState> sessionState_;
    std::vector<RuntimeCallFrame> frames_;
    std::vector<ActiveIndexContext> activeIndexContexts_;
    std::vector<std::string> activeFunctionNames_;
    std::vector<std::string> activePersistentFunctionKeys_;
    std::set<std::string> baseGlobalNames_;
    std::vector<RuntimeExceptionFrame> exceptionCallerFrames_;
    std::map<std::string, const HirNode*> functionsByName_;
    std::set<std::string> classNames_;
    ArgumentContractCatalog argumentCatalog_;
    RuntimeWorkspace resultFrame_;
    RuntimeOutputSink runtimeOutputSink_;
    std::vector<RuntimeOutputEvent> outputEvents_;
    std::vector<RuntimeExpressionResult> expressionResults_;
    std::uint64_t nextConsoleSequence_ = 0;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Diagnostic> warnings_;
    std::optional<RuntimeValue> pendingException_;
    std::optional<size_t> diagnosticTrapBase_;
    std::optional<std::chrono::steady_clock::time_point> ticStart_;
    size_t loopDepth_ = 0;
    ControlSignal controlSignal_ = ControlSignal::None;
    static constexpr size_t kMaxWhileIterations = 1'000'000;
};

} // namespace

InterpreterResult Interpreter::run(const SemanticResult& semantic) {
    return run(semantic, InterpreterOptions{});
}

InterpreterResult Interpreter::run(
    const SemanticResult& semantic,
    const InterpreterOptions& options) {
    InterpreterContext context;
    return context.run(semantic, options);
}

} // namespace mparser
