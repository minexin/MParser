#include "mparser/bytecode_vm.h"
#include "mparser/function_signature.h"
#include "mparser/typed_ir.h"
#include "mparser/typed_region_executor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mparser {
namespace {

constexpr size_t kHotLoopThreshold = 10;
constexpr std::string_view kScriptProfileName = "<script>";

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
    result.rows = rows;
    result.columns = columns;
    result.elements = std::move(values);
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

bool isArray(const RuntimeValue& value) {
    return isVector(value) || isMatrix(value);
}

bool isNumeric(const RuntimeValue& value) {
    return isNumber(value) || isArray(value);
}

size_t rowCount(const RuntimeValue& value) {
    return isNumber(value) || isString(value) ? 1 : value.rows;
}

size_t columnCount(const RuntimeValue& value) {
    if (isNumber(value)) {
        return 1;
    }
    if (isString(value)) {
        return value.text.size();
    }
    return value.columns;
}

std::string runtimeKindName(const RuntimeValue& value) {
    switch (value.kind) {
    case RuntimeValueKind::Missing:
        return "missing";
    case RuntimeValueKind::Number:
        return "number";
    case RuntimeValueKind::String:
        return "string";
    case RuntimeValueKind::Vector:
        return "vector";
    case RuntimeValueKind::Matrix:
        return "matrix";
    }
    return "unknown";
}

void observeValue(BytecodeValueObservation& observation,
                  const RuntimeValue& value) {
    const std::string kind = runtimeKindName(value);
    const size_t rows = rowCount(value);
    const size_t columns = columnCount(value);

    if (observation.observationCount == 0) {
        observation.kind = kind;
        observation.rows = rows;
        observation.columns = columns;
        observation.observationCount = 1;
        observation.stable = true;
        return;
    }

    ++observation.observationCount;
    if (!observation.stable) {
        return;
    }

    if (observation.kind != kind || observation.rows != rows ||
        observation.columns != columns) {
        observation.kind = "mixed";
        observation.rows = 0;
        observation.columns = 0;
        observation.stable = false;
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
        }
    }
}

size_t elementCount(const RuntimeValue& value) {
    if (isNumber(value) || isString(value)) {
        return 1;
    }
    return value.elements.size();
}

RuntimeValue arrayValueForShape(size_t rows, size_t columns,
                                std::vector<double> values) {
    if (rows == 1) {
        return vectorValue(std::move(values));
    }
    return matrixValue(rows, columns, std::move(values));
}

double elementAt(const RuntimeValue& value, size_t index) {
    return isNumber(value) ? value.number : value.elements[index];
}

RuntimeValue oneBasedIndexRange(size_t length) {
    std::vector<double> values;
    values.reserve(length);
    for (size_t index = 1; index <= length; ++index) {
        values.push_back(static_cast<double>(index));
    }
    return vectorValue(std::move(values));
}

double matrixElement(const RuntimeValue& value, size_t row, size_t column) {
    if (isNumber(value)) {
        return value.number;
    }
    return value.elements[row * columnCount(value) + column];
}

bool truthy(const RuntimeValue& value) {
    if (isNumber(value)) {
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

struct StackValue {
    RuntimeValue value;
    bool isBuiltinReference = false;
    std::string builtinName;
    bool isFunctionReference = false;
    std::string functionName;
};

struct ForLoopState {
    std::string variable;
    std::vector<double> values;
    size_t nextIndex = 0;
    size_t headerPc = 0;
};

struct IndexContext {
    RuntimeValue target;
    size_t total = 0;
    size_t position = 0;
};

struct SwitchContext {
    RuntimeValue selector;
    bool matched = false;
};

struct TryContext {
    size_t diagnosticBase = 0;
    size_t catchTarget = 0;
    std::string catchVariable;
    size_t stackDepth = 0;
    size_t forLoopDepth = 0;
    size_t indexContextDepth = 0;
    size_t switchContextDepth = 0;
};

struct FunctionInfo {
    std::string name;
    FunctionSignature signature;
    size_t entry = 0;
    size_t end = 0;
    SourceSpan span;
};

struct ActiveTypedLoopRegion {
    size_t regionId = 0;
    std::string kind;
    std::string target;
    BytecodeRegionContract contract;
};

StackValue runtimeStackValue(RuntimeValue value) {
    StackValue result;
    result.value = std::move(value);
    return result;
}

StackValue builtinStackValue(std::string name) {
    StackValue result;
    result.isBuiltinReference = true;
    result.builtinName = std::move(name);
    return result;
}

StackValue functionStackValue(std::string name) {
    StackValue result;
    result.isFunctionReference = true;
    result.functionName = std::move(name);
    return result;
}

bool isBoundaryEnter(BytecodeOp op) {
    return op == BytecodeOp::EnterClass || op == BytecodeOp::EnterFunction ||
           op == BytecodeOp::EnterControl;
}

bool isBoundaryLeave(BytecodeOp op) {
    return op == BytecodeOp::LeaveClass || op == BytecodeOp::LeaveFunction ||
           op == BytecodeOp::LeaveControl;
}

bool isTopLevelRuntimeOp(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::LoadName:
    case BytecodeOp::LoadLiteral:
    case BytecodeOp::StoreName:
    case BytecodeOp::StoreMember:
    case BytecodeOp::StoreIndex:
    case BytecodeOp::UnaryOp:
    case BytecodeOp::BinaryOp:
    case BytecodeOp::PostfixOp:
    case BytecodeOp::MemberAccess:
    case BytecodeOp::CallOrIndex:
    case BytecodeOp::BraceIndex:
    case BytecodeOp::MakeMatrix:
    case BytecodeOp::MakeMatrixRow:
    case BytecodeOp::MakeCell:
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
    case BytecodeOp::Pop:
    case BytecodeOp::BeginIndexContext:
    case BytecodeOp::BeginIndexArgument:
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
                         const BytecodeVmOptions& options) {
        program_ = &program;
        semantic_ = &semantic;
        profilingEnabled_ =
            options.profiling == BytecodeVmProfilingMode::Full;
        requestedEntryFunction_ = options.entryFunction;
        entryArguments_ = options.arguments;
        requestedEntryOutputCount_ = options.requestedOutputCount;
        executedEntryFunction_.clear();
        executedRequestedOutputCount_ = 0;
        entrySignature_.reset();
        entryOutputs_.clear();
        diagnostics_ = program.diagnostics;
        frames_.clear();
        frames_.push_back({});
        resetProfiling(program.instructions.size());
        initializeWorkspace(options.initialWorkspace);
        collectFunctionNodes(semantic.root.get(), false);
        collectFunctionRanges(program);
        collectTypedRegions(typedIr);

        const bool scriptMode = requestedEntryFunction_.empty() &&
                                hasTopLevelExecutable(program);
        if (scriptMode && !entryArguments_.empty()) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "script entry does not accept arguments"});
        } else if (scriptMode && requestedEntryOutputCount_.value_or(0) > 0) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "script entry does not declare outputs"});
        } else {
            execute(scriptMode);
        }
        finalizeEntryOutputs();

        BytecodeVmResult result;
        const auto& variables = frames_.front();
        for (const auto& [name, value] : variables) {
            result.variables.push_back(RuntimeVariable{name, value});
        }
        result.entryFunction = executedEntryFunction_;
        if (entrySignature_) {
            const size_t count = std::min(
                executedRequestedOutputCount_, entrySignature_->outputs.size());
            result.outputNames.assign(entrySignature_->outputs.begin(),
                                      entrySignature_->outputs.begin() + count);
        }
        result.outputs = entryOutputs_;
        result.requestedOutputCount = executedRequestedOutputCount_;
        result.diagnostics = std::move(diagnostics_);
        result.executedInstructionCount = executedInstructionCount_;
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
        workspaceInputProfiles_.clear();
        functionEntryProfiles_.clear();
        typedLoopRegions_.clear();
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

        for (const auto& [name, input] : workspaceInputProfiles_) {
            profile.workspaceInputs.push_back(input);
        }

        for (const auto& [name, entry] : functionEntryProfiles_) {
            profile.functionEntries.push_back(entry);
        }

        return profile;
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
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = functionProfiles_[name];
        if (profile.name.empty()) {
            profile.name = name;
            profile.span = span;
        }
        ++profile.callCount;
        functionProfileStack_.push_back(std::move(name));
    }

    void leaveFunctionProfile() {
        if (!profilingEnabled_) {
            return;
        }
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
        if (!profilingEnabled_) {
            return;
        }
        auto& profile = assignmentProfiles_[currentPc_];
        if (profile.kind.empty()) {
            profile.pc = currentPc_;
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

    void collectFunctionNodes(const HirNode* node, bool inClass) {
        if (!node) {
            return;
        }

        const bool childInClass = inClass || node->kind == HirKind::Class;
        if (!inClass && node->kind == HirKind::Function) {
            functionNodes_[node->label] = node;
        }

        for (const auto& child : node->children) {
            collectFunctionNodes(child.get(), childInClass);
        }
    }

    void collectFunctionRanges(const BytecodeProgram& program) {
        for (size_t pc = 0; pc < program.instructions.size(); ++pc) {
            const auto& instruction = program.instructions[pc];
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
            if (const auto hir = functionNodes_.find(info.name);
                hir != functionNodes_.end()) {
                info.signature = parseFunctionSignature(*hir->second);
            }
            functionsByName_[info.name] = std::move(info);
            pc = end;
        }
    }

    void collectTypedRegions(const BytecodeTypedIrModule* typedIr) {
        if (!typedIr) {
            return;
        }

        for (const auto& region : typedIr->regions) {
            BytecodeTypedRegionExecutionProfile execution;
            execution.regionId = region.id;
            execution.sourcePc = region.sourcePc;
            execution.kind = region.kind;
            execution.target = region.target;
            execution.eligible =
                region.region.eligibleForTypedExecution;
            execution.lastReason = region.region.reason;
            typedRegionExecutions_[region.id] = execution;

            if (region.kind != "scalar-loop" ||
                !region.region.eligibleForTypedExecution) {
                continue;
            }
            typedLoopRegions_[region.sourcePc] = ActiveTypedLoopRegion{
                region.id, region.kind, region.target, region.region};
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
        bool activeFunction = scriptMode;
        bool ranFirstFunction = false;
        bool enteredProfile = false;
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
                enteredProfile = true;
                activeFunction = true;
                ranFirstFunction = true;
                ++pc;
                continue;
            }

            if (instruction.op == BytecodeOp::LeaveFunction) {
                if (activeFunction && !scriptMode) {
                    leaveFunctionProfile();
                    enteredProfile = false;
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

            recordInstruction(pc, instruction);
            const auto nextPc = executeInstruction(instruction);
            ++executedInstructionCount_;
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
        if (!scriptMode && !requestedEntryFunction_.empty() &&
            !ranFirstFunction && diagnostics_.empty()) {
            diagnostics_.push_back(Diagnostic{
                SourceSpan{}, "entry function is not available: " +
                                  requestedEntryFunction_});
        }
    }

    bool prepareEntryFunction(const BytecodeInstruction& instruction) {
        const auto function = functionsByName_.find(instruction.operand);
        if (function == functionsByName_.end()) {
            return true;
        }

        const auto& signature = function->second.signature;
        if (entryArguments_.size() != signature.parameters.size()) {
            addDiagnostic(instruction,
                          "function argument count mismatch for: " +
                              instruction.operand);
            return false;
        }
        const size_t requestedOutputCount =
            requestedEntryOutputCount_.value_or(signature.outputs.size());
        if (requestedOutputCount > signature.outputs.size()) {
            addDiagnostic(instruction,
                          "function output count mismatch for: " +
                              instruction.operand);
            return false;
        }

        for (size_t index = 0; index < signature.parameters.size(); ++index) {
            currentFrame()[signature.parameters[index]] =
                entryArguments_[index];
        }
        for (const auto& output : signature.outputs) {
            currentFrame()[output] = missingValue();
        }
        currentFrame()["nargin"] =
            numberValue(static_cast<double>(entryArguments_.size()));
        currentFrame()["nargout"] =
            numberValue(static_cast<double>(requestedOutputCount));
        executedEntryFunction_ = instruction.operand;
        executedRequestedOutputCount_ = requestedOutputCount;
        entrySignature_ = signature;
        if (profilingEnabled_) {
            auto& profile = functionEntryProfiles_[instruction.operand];
            profile.name = instruction.operand;
            profile.parameters = signature.parameters;
            profile.outputs = signature.outputs;
            ++profile.invocationCount;
            observeValues(profile.argumentObservations, entryArguments_);
        }
        return true;
    }

    void finalizeEntryOutputs() {
        if (!entrySignature_) {
            return;
        }
        const auto& frame = frames_.front();
        for (size_t index = 0; index < executedRequestedOutputCount_; ++index) {
            const auto& name = entrySignature_->outputs[index];
            const auto value = frame.find(name);
            entryOutputs_.push_back(value == frame.end()
                                        ? missingValue()
                                        : value->second);
        }
        if (profilingEnabled_) {
            auto& profile = functionEntryProfiles_[executedEntryFunction_];
            observeValues(profile.resultObservations, entryOutputs_);
        }
    }

    void executeFunctionBody(size_t entry, size_t end) {
        size_t pc = entry;
        while (pc < end && pc < program_->instructions.size()) {
            const auto& instruction = program_->instructions[pc];
            if (instruction.op == BytecodeOp::EnterControl) {
                addDiagnostic(instruction,
                              "bytecode VM does not execute control blocks "
                              "yet");
                break;
            }

            recordInstruction(pc, instruction);
            const auto nextPc = executeInstruction(instruction);
            ++executedInstructionCount_;
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
        switch (instruction.op) {
        case BytecodeOp::LoadName:
            loadName(instruction);
            break;
        case BytecodeOp::LoadLiteral:
            loadLiteral(instruction);
            break;
        case BytecodeOp::StoreName:
            storeName(instruction);
            break;
        case BytecodeOp::StoreIndex:
            storeIndex(instruction);
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
        case BytecodeOp::CallOrIndex:
            callOrIndex(instruction);
            break;
        case BytecodeOp::Jump:
            return jump(instruction);
        case BytecodeOp::JumpIfFalse:
            return jumpIfFalse(instruction);
        case BytecodeOp::Break:
            return breakLoop(instruction);
        case BytecodeOp::Continue:
            recordContinue(instruction);
            return checkedTarget(instruction);
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
        case BytecodeOp::Pop:
            (void)popRuntime(instruction, "discard");
            break;
        case BytecodeOp::BeginIndexContext:
            beginIndexContext(instruction);
            break;
        case BytecodeOp::BeginIndexArgument:
            beginIndexArgument(instruction);
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
        case BytecodeOp::StoreMember:
        case BytecodeOp::MemberAccess:
        case BytecodeOp::BraceIndex:
        case BytecodeOp::MakeCell:
        case BytecodeOp::MakeFunctionHandle:
        case BytecodeOp::LoadMetaClass:
        case BytecodeOp::EnterModule:
        case BytecodeOp::LeaveModule:
        case BytecodeOp::EnterClass:
        case BytecodeOp::LeaveClass:
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
        }
        return target;
    }

    std::optional<size_t> jumpIfFalse(
        const BytecodeInstruction& instruction) {
        const auto condition = popRuntime(instruction, "conditional jump");
        if (!condition) {
            return std::nullopt;
        }
        if (!truthy(*condition)) {
            return checkedTarget(instruction);
        }
        return std::nullopt;
    }

    std::optional<size_t> beginFor(
        const BytecodeInstruction& instruction) {
        const auto range = popRuntime(instruction, "for loop range");
        if (!range) {
            return std::nullopt;
        }

        const auto values = valuesForLoopRange(instruction, *range);
        if (!values) {
            return std::nullopt;
        }
        RuntimeValue firstValue;
        const RuntimeValue* observedValue = nullptr;
        if (!values->empty()) {
            firstValue = numberValue(values->front());
            observedValue = &firstValue;
        }
        recordForEntry(instruction, values->size(), observedValue);
        if (values->empty()) {
            return checkedTarget(instruction);
        }

        currentFrame()[instruction.operand] = firstValue;
        forLoopStack_.push_back(
            ForLoopState{instruction.operand, *values, 1, currentPc_});
        return std::nullopt;
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
            execution.lastReason = "typed loop range stack value is missing";
            return std::nullopt;
        }
        const StackValue& rangeValue = stack_.back();
        if (rangeValue.isBuiltinReference ||
            rangeValue.isFunctionReference) {
            ++execution.fallbackCount;
            execution.lastReason =
                "typed loop range is not a runtime value";
            return std::nullopt;
        }

        ScalarTypedRegionExecutor executor;
        auto result = executor.execute(
            *program_, active->second.contract, rangeValue.value,
            currentFrame());
        if (result.status != TypedRegionExecutionStatus::Executed) {
            ++execution.fallbackCount;
            execution.lastReason = result.reason;
            return std::nullopt;
        }

        std::vector<double> loopValues;
        if (isNumber(rangeValue.value)) {
            loopValues.push_back(rangeValue.value.number);
        } else if (isArray(rangeValue.value)) {
            loopValues = rangeValue.value.elements;
        }

        stack_.pop_back();
        currentFrame() = std::move(result.variables);
        ++execution.executionCount;
        execution.iterationCount += result.iterationCount;
        execution.executedInstructionCount +=
            result.executedInstructionCount;
        execution.lastReason = result.reason;

        RuntimeValue firstValue;
        const RuntimeValue* observedValue = nullptr;
        if (!loopValues.empty()) {
            firstValue = numberValue(loopValues.front());
            observedValue = &firstValue;
        }
        recordForEntry(instruction, loopValues.size(), observedValue);
        if (!loopValues.empty()) {
            const auto& latch =
                program_->instructions[active->second.contract.bodyEndPc];
            ForLoopState state{instruction.operand, loopValues, 1,
                               currentPc_};
            for (size_t index = 1; index < loopValues.size(); ++index) {
                recordForBackedge(state, latch,
                                  numberValue(loopValues[index]));
            }
            recordForCompletion(state, latch);
        }
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

        RuntimeValue nextValue = numberValue(state.values[state.nextIndex]);
        currentFrame()[state.variable] = nextValue;
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
        recordForBreak(forLoopStack_.back(), instruction);
        forLoopStack_.pop_back();
        return checkedTarget(instruction);
    }

    void switchBegin(const BytecodeInstruction& instruction) {
        const auto selector = popRuntime(instruction, "switch selector");
        if (!selector) {
            return;
        }
        switchContextStack_.push_back(SwitchContext{*selector, false});
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
        if (context.matched ||
            !runtimeEqual(context.selector, *candidate)) {
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
            switchContextStack_.size()});
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

        const std::string message = diagnostics_.back().message;
        diagnostics_.resize(context.diagnosticBase);
        stack_.resize(context.stackDepth);
        forLoopStack_.resize(context.forLoopDepth);
        indexContextStack_.resize(context.indexContextDepth);
        switchContextStack_.resize(context.switchContextDepth);

        if (!context.catchVariable.empty()) {
            currentFrame()[context.catchVariable] = stringValue(message);
        }
        return context.catchTarget;
    }

    std::optional<std::vector<double>> valuesForLoopRange(
        const BytecodeInstruction& instruction, const RuntimeValue& value) {
        if (isNumber(value)) {
            return std::vector<double>{value.number};
        }
        if (isArray(value)) {
            return value.elements;
        }

        addDiagnostic(instruction,
                      "bytecode for loop range must be numeric");
        return std::nullopt;
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
        if (target.isBuiltinReference || target.isFunctionReference) {
            addDiagnostic(instruction,
                          "bytecode index context requires a runtime target");
            return;
        }
        if (!isNumeric(target.value)) {
            addDiagnostic(instruction,
                          "bytecode indexing requires a numeric target");
            return;
        }

        indexContextStack_.push_back(IndexContext{
            target.value, static_cast<size_t>(instruction.operandCount), 0});
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

        const IndexContext& context = indexContextStack_.back();
        if (instruction.operand == "end") {
            return numberValue(
                endValueForIndex(context.target, context.position,
                                 context.total));
        }
        if (instruction.operand == ":") {
            return oneBasedIndexRange(static_cast<size_t>(endValueForIndex(
                context.target, context.position, context.total)));
        }
        return std::nullopt;
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

    void finishIndexContext() {
        if (!indexContextStack_.empty()) {
            indexContextStack_.pop_back();
        }
    }

    void loadName(const BytecodeInstruction& instruction) {
        if (const auto variable = currentFrame().find(instruction.operand);
            variable != currentFrame().end()) {
            stack_.push_back(runtimeStackValue(variable->second));
            return;
        }

        if (instruction.binding.kind == BindingKind::Builtin) {
            if (instruction.operand == "pi") {
                stack_.push_back(
                    runtimeStackValue(numberValue(3.14159265358979323846)));
                return;
            }
            if (instruction.operand == "eps") {
                stack_.push_back(runtimeStackValue(
                    numberValue(std::numeric_limits<double>::epsilon())));
                return;
            }
            if (instruction.operand == "inf") {
                stack_.push_back(runtimeStackValue(numberValue(
                    std::numeric_limits<double>::infinity())));
                return;
            }
            if (instruction.operand == "nan") {
                stack_.push_back(runtimeStackValue(numberValue(
                    std::numeric_limits<double>::quiet_NaN())));
                return;
            }
            if (instruction.operand == "true") {
                stack_.push_back(runtimeStackValue(numberValue(1.0)));
                return;
            }
            if (instruction.operand == "false") {
                stack_.push_back(runtimeStackValue(numberValue(0.0)));
                return;
            }

            stack_.push_back(builtinStackValue(instruction.operand));
            return;
        }

        if (instruction.binding.kind == BindingKind::Function) {
            stack_.push_back(functionStackValue(instruction.operand));
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
            stack_.push_back(runtimeStackValue(
                stringValue(decodeStringLiteral(instruction.operand))));
            return;
        }

        if (const auto number = parseNumber(instruction.operand)) {
            stack_.push_back(runtimeStackValue(numberValue(*number)));
            return;
        }

        addDiagnostic(instruction,
                      "bytecode VM cannot load literal: " +
                          instruction.operand);
    }

    void storeName(const BytecodeInstruction& instruction) {
        const auto value = popRuntime(instruction, "store");
        if (!value) {
            return;
        }
        currentFrame()[instruction.operand] = *value;
        recordAssignment(instruction, "name", *value);
    }

    void storeIndex(const BytecodeInstruction& instruction) {
        const auto arguments = popRuntimeValues(instruction,
                                                instruction.operandCount,
                                                "indexed assignment "
                                                "arguments");
        const auto target = popRuntime(instruction,
                                       "indexed assignment target");
        const auto value = popRuntime(instruction,
                                      "indexed assignment value");
        finishIndexContext();
        if (!arguments || !target || !value) {
            return;
        }
        if (instruction.operand.empty()) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment requires a variable "
                          "target");
            return;
        }
        if (!isNumber(*value)) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment currently requires a "
                          "scalar numeric value");
            return;
        }
        if (!isNumeric(*target)) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment requires a numeric "
                          "target");
            return;
        }
        if (arguments->size() != 1 && arguments->size() != 2) {
            addDiagnostic(instruction,
                          "bytecode indexed assignment supports one or two "
                          "subscripts");
            return;
        }

        RuntimeValue updated = *target;
        const size_t diagnosticCount = diagnostics_.size();
        if (arguments->size() == 2) {
            assignTwoSubscriptTarget(instruction, updated, *arguments,
                                     value->number);
        } else {
            assignLinearTarget(instruction, updated, arguments->front(),
                               value->number);
        }
        if (diagnostics_.size() != diagnosticCount) {
            return;
        }
        recordAssignment(instruction, "index", updated);
        currentFrame()[instruction.operand] = std::move(updated);
    }

    void assignTwoSubscriptTarget(
        const BytecodeInstruction& instruction, RuntimeValue& target,
        const std::vector<RuntimeValue>& arguments, double value) {
        const auto rows =
            checkedIndices(instruction, arguments[0], rowCount(target));
        const auto columns =
            checkedIndices(instruction, arguments[1], columnCount(target));
        if (!rows || !columns) {
            return;
        }

        for (size_t row : *rows) {
            for (size_t column : *columns) {
                assignMatrixElement(target, row, column, value);
            }
        }
    }

    void assignLinearTarget(const BytecodeInstruction& instruction,
                            RuntimeValue& target,
                            const RuntimeValue& subscript, double value) {
        const auto indices =
            checkedIndices(instruction, subscript, elementCount(target));
        if (!indices) {
            return;
        }

        for (size_t index : *indices) {
            assignLinearElement(target, index, value);
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
        assignMatrixElement(target, row, column, value);
    }

    void assignMatrixElement(RuntimeValue& target, size_t row, size_t column,
                             double value) {
        if (isNumber(target)) {
            target.number = value;
            return;
        }
        target.elements[row * columnCount(target) + column] = value;
    }

    void applyUnary(const BytecodeInstruction& instruction) {
        const auto value = popRuntime(instruction, "unary operator");
        if (!value) {
            return;
        }
        if (!isNumeric(*value)) {
            addDiagnostic(instruction,
                          "bytecode unary operator requires numeric input");
            return;
        }
        if (instruction.operand == "+") {
            pushRuntime(*value);
            return;
        }
        if (instruction.operand == "-") {
            pushRuntime(mapUnary(*value, [](double element) {
                return -element;
            }));
            return;
        }
        if (instruction.operand == "~") {
            pushRuntime(mapUnary(*value, [](double element) {
                return (element != 0.0 && !std::isnan(element)) ? 0.0 : 1.0;
            }));
            return;
        }

        addDiagnostic(instruction,
                      "unsupported bytecode unary operator: " +
                          instruction.operand);
    }

    void applyBinary(const BytecodeInstruction& instruction) {
        const auto right = popRuntime(instruction, "binary operator");
        const auto left = popRuntime(instruction, "binary operator");
        if (!left || !right) {
            return;
        }

        if (instruction.operand == ":") {
            applyColon(instruction, *left, *right);
            return;
        }

        if (isString(*left) || isString(*right)) {
            if (isString(*left) && isString(*right) &&
                (instruction.operand == "==" || instruction.operand == "~=")) {
                const bool equal = runtimeEqual(*left, *right);
                pushRuntime(numberValue((instruction.operand == "==") == equal
                                            ? 1.0
                                            : 0.0));
                return;
            }
            addDiagnostic(instruction,
                          "bytecode string operators support only == and ~=");
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

        if (instruction.operand != "'") {
            addDiagnostic(instruction,
                          "unsupported bytecode postfix operator: " +
                              instruction.operand);
            return;
        }

        if (isNumber(*value)) {
            pushRuntime(*value);
            return;
        }
        if (isVector(*value)) {
            pushRuntime(matrixValue(value->elements.size(), 1,
                                    value->elements));
            return;
        }
        if (isMatrix(*value)) {
            std::vector<double> transposed;
            transposed.reserve(value->elements.size());
            for (size_t column = 0; column < value->columns; ++column) {
                for (size_t row = 0; row < value->rows; ++row) {
                    transposed.push_back(
                        value->elements[row * value->columns + column]);
                }
            }
            pushRuntime(matrixValue(value->columns, value->rows,
                                    std::move(transposed)));
            return;
        }

        addDiagnostic(instruction, "bytecode transpose requires numeric input");
    }

    void makeMatrixRow(const BytecodeInstruction& instruction) {
        const auto values = popRuntimeValues(instruction,
                                             instruction.operandCount,
                                             "matrix row");
        if (!values) {
            return;
        }

        std::vector<double> elements;
        for (const auto& value : *values) {
            if (isNumber(value)) {
                elements.push_back(value.number);
            } else if (isArray(value)) {
                elements.insert(elements.end(), value.elements.begin(),
                                value.elements.end());
            } else {
                addDiagnostic(instruction,
                              "bytecode matrix rows require numeric values");
                return;
            }
        }
        pushRuntime(vectorValue(std::move(elements)));
    }

    void makeMatrix(const BytecodeInstruction& instruction) {
        const auto rows = popRuntimeValues(instruction, instruction.operandCount,
                                           "matrix");
        if (!rows) {
            return;
        }
        if (rows->empty()) {
            pushRuntime(vectorValue({}));
            return;
        }

        if (rows->size() == 1) {
            if (isVector(rows->front())) {
                pushRuntime(rows->front());
                return;
            }
            if (isNumber(rows->front())) {
                pushRuntime(vectorValue({rows->front().number}));
                return;
            }
        }

        size_t columns = 0;
        std::vector<double> elements;
        for (const auto& row : *rows) {
            if (!isVector(row)) {
                addDiagnostic(instruction,
                              "bytecode matrix rows must be row vectors");
                return;
            }
            if (columns == 0) {
                columns = row.elements.size();
            } else if (columns != row.elements.size()) {
                addDiagnostic(instruction,
                              "bytecode matrix rows must have equal length");
                return;
            }
            elements.insert(elements.end(), row.elements.begin(),
                            row.elements.end());
        }

        pushRuntime(matrixValue(rows->size(), columns, std::move(elements)));
    }

    void callOrIndex(const BytecodeInstruction& instruction) {
        const auto arguments = popRuntimeValues(instruction,
                                                instruction.operandCount,
                                                "call/index arguments");
        const auto callee = popStackValue(instruction, "call/index callee");
        if (!arguments || !callee) {
            return;
        }

        if (instruction.binding.kind == BindingKind::Function ||
            callee->isFunctionReference) {
            const std::string name = symbolName(instruction.binding)
                                         .value_or(callee->functionName);
            BytecodeCallSiteProfile* profile = nullptr;
            if (profilingEnabled_) {
                profile = &recordCallSite(instruction, "function", name);
                observeValues(profile->argumentObservations, *arguments);
            }
            auto outputs = callLocalFunction(instruction, name, *arguments,
                                             instruction.resultCount);
            if (profile) {
                observeValues(profile->resultObservations, outputs);
            }
            pushOutputValues(instruction, outputs);
            return;
        }

        if (instruction.binding.kind == BindingKind::Builtin ||
            callee->isBuiltinReference) {
            const std::string name = symbolName(instruction.binding)
                                         .value_or(callee->builtinName);
            BytecodeCallSiteProfile* profile = nullptr;
            if (profilingEnabled_) {
                profile = &recordCallSite(instruction, "builtin", name);
                observeValues(profile->argumentObservations, *arguments);
            }
            auto outputs = callBuiltinOutputs(instruction, name, *arguments,
                                              instruction.resultCount);
            if (profile) {
                observeValues(profile->resultObservations, outputs);
            }
            pushOutputValues(instruction, outputs);
            return;
        }

        if (instruction.resultCount != 1) {
            finishIndexContext();
            addDiagnostic(instruction,
                          "bytecode indexing cannot produce multiple outputs");
            return;
        }

        if (!isNumeric(callee->value)) {
            finishIndexContext();
            addDiagnostic(instruction,
                          "bytecode indexing requires a numeric target");
            return;
        }
        BytecodeCallSiteProfile* profile = nullptr;
        if (profilingEnabled_) {
            profile = &recordCallSite(instruction, "index",
                                      instruction.operand);
            profile->hasReceiverObservation = true;
            observeValue(profile->receiverObservation, callee->value);
            observeValues(profile->argumentObservations, *arguments);
        }
        RuntimeValue result = indexValue(instruction, callee->value,
                                         *arguments);
        const std::vector<RuntimeValue> outputs{result};
        if (profile) {
            observeValues(profile->resultObservations, outputs);
        }
        finishIndexContext();
        pushRuntime(std::move(result));
    }

    std::vector<RuntimeValue> callBuiltinOutputs(
        const BytecodeInstruction& instruction, const std::string& name,
        const std::vector<RuntimeValue>& arguments, int requestedCount) {
        if (requestedCount < 1) {
            addDiagnostic(instruction,
                          "bytecode call result count must be positive");
            return {};
        }

        if (name == "size" && requestedCount > 1) {
            if (arguments.size() != 1 || !isNumeric(arguments.front())) {
                addDiagnostic(instruction,
                              "bytecode size expects one numeric argument");
                return missingOutputs(requestedCount);
            }

            std::vector<RuntimeValue> outputs;
            outputs.reserve(static_cast<size_t>(requestedCount));
            outputs.push_back(
                numberValue(static_cast<double>(rowCount(arguments.front()))));
            outputs.push_back(numberValue(
                static_cast<double>(columnCount(arguments.front()))));
            while (outputs.size() < static_cast<size_t>(requestedCount)) {
                outputs.push_back(numberValue(1.0));
            }
            return outputs;
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
        if (requestedCount < 1) {
            addDiagnostic(instruction,
                          "bytecode call result count must be positive");
            return {};
        }

        const auto function = functionsByName_.find(name);
        if (function == functionsByName_.end()) {
            addDiagnostic(instruction,
                          "local function is not available: " + name);
            return missingOutputs(requestedCount);
        }

        const FunctionInfo& info = function->second;
        if (arguments.size() != info.signature.parameters.size()) {
            addDiagnostic(instruction,
                          "function argument count mismatch for: " + name);
            return missingOutputs(requestedCount);
        }
        if (requestedCount >
            static_cast<int>(info.signature.outputs.size())) {
            addDiagnostic(instruction,
                          "function output count mismatch for: " + name);
            return missingOutputs(requestedCount);
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

        frames_.push_back({});
        for (size_t index = 0; index < info.signature.parameters.size();
             ++index) {
            currentFrame()[info.signature.parameters[index]] =
                arguments[index];
        }
        for (const auto& output : info.signature.outputs) {
            currentFrame()[output] = missingValue();
        }
        currentFrame()["nargin"] =
            numberValue(static_cast<double>(arguments.size()));
        currentFrame()["nargout"] =
            numberValue(static_cast<double>(requestedCount));

        enterFunctionProfile(name, info.span);
        executeFunctionBody(info.entry, info.end);
        leaveFunctionProfile();

        auto completedFrame = std::move(currentFrame());
        frames_.pop_back();
        returnRequested_ = savedReturnRequested;
        forLoopStack_ = std::move(savedForLoopStack);
        indexContextStack_ = std::move(savedIndexContextStack);
        switchContextStack_ = std::move(savedSwitchContextStack);
        tryContextStack_ = std::move(savedTryContextStack);

        std::vector<RuntimeValue> outputs;
        outputs.reserve(static_cast<size_t>(requestedCount));
        for (int index = 0; index < requestedCount; ++index) {
            if (index >= static_cast<int>(info.signature.outputs.size())) {
                outputs.push_back(missingValue());
                continue;
            }
            const auto output =
                completedFrame.find(info.signature.outputs[index]);
            outputs.push_back(output == completedFrame.end()
                                  ? missingValue()
                                  : output->second);
        }
        return outputs;
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
        if (name == "zeros" || name == "ones" || name == "eye") {
            return arrayConstructor(instruction, name, arguments);
        }
        if (name == "linspace") {
            return linspaceBuiltin(instruction, arguments);
        }
        if (name == "strcmp") {
            if (arguments.size() != 2 || !isString(arguments[0]) ||
                !isString(arguments[1])) {
                addDiagnostic(instruction,
                              "bytecode strcmp expects two string arguments");
                return missingValue();
            }
            return numberValue(runtimeEqual(arguments[0], arguments[1]) ? 1.0
                                                                         : 0.0);
        }

        if (arguments.size() != 1 || !isNumeric(arguments.front())) {
            addDiagnostic(instruction,
                          "bytecode builtin expects one numeric argument: " +
                              name);
            return missingValue();
        }

        if (name == "sin") {
            return mapUnary(arguments.front(), [](double value) {
                return std::sin(value);
            });
        }
        if (name == "cos") {
            return mapUnary(arguments.front(), [](double value) {
                return std::cos(value);
            });
        }
        if (name == "sqrt") {
            return mapUnary(arguments.front(), [](double value) {
                return std::sqrt(value);
            });
        }
        if (name == "abs") {
            return mapUnary(arguments.front(), [](double value) {
                return std::fabs(value);
            });
        }
        if (name == "length") {
            return numberValue(static_cast<double>(
                std::max(rowCount(arguments.front()),
                         columnCount(arguments.front()))));
        }
        if (name == "numel") {
            return numberValue(
                static_cast<double>(elementCount(arguments.front())));
        }
        if (name == "size") {
            return vectorValue({static_cast<double>(rowCount(arguments.front())),
                                static_cast<double>(
                                    columnCount(arguments.front()))});
        }
        if (name == "sum") {
            double total = 0.0;
            if (isNumber(arguments.front())) {
                total = arguments.front().number;
            } else {
                for (double element : arguments.front().elements) {
                    total += element;
                }
            }
            return numberValue(total);
        }
        if (name == "any") {
            return numberValue(truthyAny(arguments.front()) ? 1.0 : 0.0);
        }
        if (name == "all") {
            return numberValue(truthy(arguments.front()) ? 1.0 : 0.0);
        }

        addDiagnostic(instruction,
                      "bytecode builtin is not executable yet: " + name);
        return missingValue();
    }

    RuntimeValue arrayConstructor(const BytecodeInstruction& instruction,
                                  const std::string& name,
                                  const std::vector<RuntimeValue>& arguments) {
        const auto shape = constructorShape(instruction, arguments);
        if (!shape) {
            return missingValue();
        }

        const auto [rows, columns] = *shape;
        std::vector<double> elements(rows * columns,
                                     name == "ones" ? 1.0 : 0.0);
        if (name == "eye") {
            const size_t diagonal = std::min(rows, columns);
            for (size_t index = 0; index < diagonal; ++index) {
                elements[index * columns + index] = 1.0;
            }
        }
        return arrayValueForShape(rows, columns, std::move(elements));
    }

    std::optional<std::pair<size_t, size_t>>
    constructorShape(const BytecodeInstruction& instruction,
                     const std::vector<RuntimeValue>& arguments) {
        if (arguments.empty() || arguments.size() > 2) {
            addDiagnostic(instruction,
                          "bytecode array constructor expects one or two "
                          "dimensions");
            return std::nullopt;
        }

        if (arguments.size() == 2) {
            if (!isNumber(arguments[0]) || !isNumber(arguments[1])) {
                addDiagnostic(instruction,
                              "bytecode constructor dimensions must be scalar");
                return std::nullopt;
            }
            const auto rows = checkedDimension(instruction,
                                               arguments[0].number);
            const auto columns = checkedDimension(instruction,
                                                  arguments[1].number);
            if (!rows || !columns) {
                return std::nullopt;
            }
            return std::pair<size_t, size_t>{*rows, *columns};
        }

        if (isNumber(arguments.front())) {
            const auto dimension = checkedDimension(instruction,
                                                    arguments.front().number);
            if (!dimension) {
                return std::nullopt;
            }
            return std::pair<size_t, size_t>{*dimension, *dimension};
        }

        if (!isArray(arguments.front()) || arguments.front().elements.size() < 2) {
            addDiagnostic(instruction,
                          "bytecode constructor shape must contain two "
                          "dimensions");
            return std::nullopt;
        }
        const auto rows = checkedDimension(instruction,
                                           arguments.front().elements[0]);
        const auto columns = checkedDimension(instruction,
                                              arguments.front().elements[1]);
        if (!rows || !columns) {
            return std::nullopt;
        }
        return std::pair<size_t, size_t>{*rows, *columns};
    }

    std::optional<size_t> checkedDimension(
        const BytecodeInstruction& instruction, double value) {
        if (!isWholeNumber(value) || value < 0.0) {
            addDiagnostic(instruction,
                          "bytecode constructor dimension must be a "
                          "nonnegative integer");
            return std::nullopt;
        }
        return static_cast<size_t>(value);
    }

    RuntimeValue linspaceBuiltin(const BytecodeInstruction& instruction,
                                 const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 2 && arguments.size() != 3) {
            addDiagnostic(instruction,
                          "bytecode linspace expects two or three arguments");
            return missingValue();
        }
        if (!isNumber(arguments[0]) || !isNumber(arguments[1]) ||
            (arguments.size() == 3 && !isNumber(arguments[2]))) {
            addDiagnostic(instruction,
                          "bytecode linspace arguments must be scalar");
            return missingValue();
        }

        size_t count = 100;
        if (arguments.size() == 3) {
            if (!isWholeNumber(arguments[2].number) ||
                arguments[2].number < 0.0) {
                addDiagnostic(instruction,
                              "bytecode linspace count must be nonnegative");
                return missingValue();
            }
            count = static_cast<size_t>(arguments[2].number);
        }
        if (count == 0) {
            return vectorValue({});
        }
        if (count == 1) {
            return vectorValue({arguments[1].number});
        }

        std::vector<double> values;
        values.reserve(count);
        const double denominator = static_cast<double>(count - 1);
        for (size_t index = 0; index < count; ++index) {
            const double t = static_cast<double>(index) / denominator;
            values.push_back(arguments[0].number +
                             (arguments[1].number - arguments[0].number) * t);
        }
        return vectorValue(std::move(values));
    }

    RuntimeValue indexValue(const BytecodeInstruction& instruction,
                            const RuntimeValue& target,
                            const std::vector<RuntimeValue>& arguments) {
        if (arguments.size() != 1 && arguments.size() != 2) {
            addDiagnostic(instruction,
                          "bytecode indexing supports one or two subscripts");
            return missingValue();
        }

        if (arguments.size() == 2) {
            const auto row =
                checkedIndices(instruction, arguments[0], rowCount(target));
            const auto column =
                checkedIndices(instruction, arguments[1],
                               columnCount(target));
            if (!row || !column) {
                return missingValue();
            }

            std::vector<double> values;
            values.reserve(row->size() * column->size());
            for (size_t rowIndex : *row) {
                for (size_t columnIndex : *column) {
                    values.push_back(
                        matrixElement(target, rowIndex, columnIndex));
                }
            }
            if (values.size() == 1) {
                return numberValue(values.front());
            }
            return arrayValueForShape(row->size(), column->size(),
                                      std::move(values));
        }

        if (isNumber(arguments.front())) {
            const auto index =
                checkedIndex(instruction, arguments.front().number,
                             elementCount(target));
            if (!index) {
                return missingValue();
            }
            return numberValue(linearElement(target, *index));
        }

        if (!isArray(arguments.front())) {
            addDiagnostic(instruction,
                          "bytecode indexing requires numeric subscripts");
            return missingValue();
        }

        std::vector<double> values;
        for (double rawIndex : arguments.front().elements) {
            const auto index =
                checkedIndex(instruction, rawIndex, elementCount(target));
            if (!index) {
                return missingValue();
            }
            values.push_back(linearElement(target, *index));
        }
        return vectorValue(std::move(values));
    }

    std::optional<std::vector<size_t>>
    checkedIndices(const BytecodeInstruction& instruction,
                   const RuntimeValue& subscript, size_t length) {
        std::vector<size_t> indices;
        if (isNumber(subscript)) {
            const auto index = checkedIndex(instruction, subscript.number,
                                            length);
            if (!index) {
                return std::nullopt;
            }
            indices.push_back(*index);
            return indices;
        }

        if (!isArray(subscript)) {
            addDiagnostic(instruction,
                          "bytecode indexing requires numeric subscripts");
            return std::nullopt;
        }

        indices.reserve(subscript.elements.size());
        for (double rawIndex : subscript.elements) {
            const auto index = checkedIndex(instruction, rawIndex, length);
            if (!index) {
                return std::nullopt;
            }
            indices.push_back(*index);
        }
        return indices;
    }

    std::optional<size_t> checkedIndex(const BytecodeInstruction& instruction,
                                       double rawIndex, size_t length) {
        if (!isWholeNumber(rawIndex)) {
            addDiagnostic(instruction,
                          "bytecode index must be a positive integer");
            return std::nullopt;
        }
        if (rawIndex < 1.0 || rawIndex > static_cast<double>(length)) {
            addDiagnostic(instruction, "bytecode index is out of bounds");
            return std::nullopt;
        }
        return static_cast<size_t>(rawIndex) - 1;
    }

    double linearElement(const RuntimeValue& value,
                         size_t zeroBasedIndex) const {
        if (!isMatrix(value)) {
            return elementAt(value, zeroBasedIndex);
        }

        const size_t row = zeroBasedIndex % value.rows;
        const size_t column = zeroBasedIndex / value.rows;
        return matrixElement(value, row, column);
    }

    void applyColon(const BytecodeInstruction& instruction,
                    const RuntimeValue& left, const RuntimeValue& right) {
        if (!isNumber(left) || !isNumber(right)) {
            addDiagnostic(instruction,
                          "bytecode colon operands must be scalar numbers");
            return;
        }

        std::vector<double> values;
        if (left.number <= right.number) {
            for (double value = left.number; value <= right.number;
                 value += 1.0) {
                values.push_back(value);
            }
        } else {
            for (double value = left.number; value >= right.number;
                 value -= 1.0) {
                values.push_back(value);
            }
        }
        pushRuntime(vectorValue(std::move(values)));
    }

    RuntimeValue applyNumericBinary(const BytecodeInstruction& instruction,
                                    const RuntimeValue& left,
                                    const RuntimeValue& right) {
        if (isNumber(left) && isNumber(right)) {
            return applyScalarBinary(instruction, left.number, right.number);
        }

        if (instruction.operand == "*" && isArray(left) && isArray(right)) {
            return matrixMultiply(instruction, left, right);
        }

        if (isArray(left) && isArray(right) &&
            (rowCount(left) != rowCount(right) ||
             columnCount(left) != columnCount(right))) {
            addDiagnostic(instruction,
                          "bytecode elementwise operands must have the same "
                          "shape");
            return missingValue();
        }

        const size_t rows = isArray(left) ? rowCount(left) : rowCount(right);
        const size_t columns =
            isArray(left) ? columnCount(left) : columnCount(right);
        const size_t count = rows * columns;
        std::vector<double> elements;
        elements.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const double leftValue = isArray(left) ? left.elements[index]
                                                   : left.number;
            const double rightValue = isArray(right) ? right.elements[index]
                                                     : right.number;
            const RuntimeValue value =
                applyScalarBinary(instruction, leftValue, rightValue);
            if (!isNumber(value)) {
                return missingValue();
            }
            elements.push_back(value.number);
        }
        return arrayValueForShape(rows, columns, std::move(elements));
    }

    RuntimeValue applyScalarBinary(const BytecodeInstruction& instruction,
                                   double left, double right) {
        if (instruction.operand == "+") {
            return numberValue(left + right);
        }
        if (instruction.operand == "-") {
            return numberValue(left - right);
        }
        if (instruction.operand == "*" || instruction.operand == ".*") {
            return numberValue(left * right);
        }
        if (instruction.operand == "/" || instruction.operand == "./") {
            return numberValue(left / right);
        }
        if (instruction.operand == "^" || instruction.operand == ".^") {
            return numberValue(std::pow(left, right));
        }
        if (instruction.operand == ">") {
            return numberValue(left > right ? 1.0 : 0.0);
        }
        if (instruction.operand == "<") {
            return numberValue(left < right ? 1.0 : 0.0);
        }
        if (instruction.operand == ">=") {
            return numberValue(left >= right ? 1.0 : 0.0);
        }
        if (instruction.operand == "<=") {
            return numberValue(left <= right ? 1.0 : 0.0);
        }
        if (instruction.operand == "==") {
            return numberValue(left == right ? 1.0 : 0.0);
        }
        if (instruction.operand == "~=") {
            return numberValue(left != right ? 1.0 : 0.0);
        }
        if (instruction.operand == "&" || instruction.operand == "&&") {
            return numberValue((left != 0.0 && right != 0.0) ? 1.0 : 0.0);
        }
        if (instruction.operand == "|" || instruction.operand == "||") {
            return numberValue((left != 0.0 || right != 0.0) ? 1.0 : 0.0);
        }

        addDiagnostic(instruction,
                      "unsupported bytecode binary operator: " +
                          instruction.operand);
        return missingValue();
    }

    RuntimeValue matrixMultiply(const BytecodeInstruction& instruction,
                                const RuntimeValue& left,
                                const RuntimeValue& right) {
        const size_t leftRows = rowCount(left);
        const size_t leftColumns = columnCount(left);
        const size_t rightRows = rowCount(right);
        const size_t rightColumns = columnCount(right);

        if (leftColumns != rightRows) {
            addDiagnostic(instruction,
                          "bytecode matrix dimensions do not agree for *");
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

    RuntimeValue mapUnary(const RuntimeValue& value,
                          double (*operation)(double)) const {
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
                       std::string message) {
        diagnostics_.push_back(Diagnostic{instruction.span,
                                          std::move(message)});
    }

    std::map<std::string, RuntimeValue>& currentFrame() {
        return frames_.back();
    }

    const std::map<std::string, RuntimeValue>& currentFrame() const {
        return frames_.back();
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
    std::vector<StackValue> stack_;
    std::vector<ForLoopState> forLoopStack_;
    std::vector<IndexContext> indexContextStack_;
    std::vector<SwitchContext> switchContextStack_;
    std::vector<TryContext> tryContextStack_;
    std::vector<std::map<std::string, RuntimeValue>> frames_;
    std::map<std::string, const HirNode*> functionNodes_;
    std::map<std::string, FunctionInfo> functionsByName_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<size_t> instructionExecutionCounts_;
    std::map<std::string, BytecodeFunctionProfile> functionProfiles_;
    std::map<size_t, BytecodeLoopProfile> loopProfiles_;
    std::map<size_t, BytecodeCallSiteProfile> callSiteProfiles_;
    std::map<size_t, BytecodeAssignmentProfile> assignmentProfiles_;
    std::map<std::string, BytecodeWorkspaceInputProfile>
        workspaceInputProfiles_;
    std::map<std::string, BytecodeFunctionEntryProfile>
        functionEntryProfiles_;
    std::map<size_t, ActiveTypedLoopRegion> typedLoopRegions_;
    std::map<size_t, BytecodeTypedRegionExecutionProfile>
        typedRegionExecutions_;
    std::vector<std::string> functionProfileStack_;
    size_t executedInstructionCount_ = 0;
    size_t currentPc_ = 0;
    bool returnRequested_ = false;
    bool profilingEnabled_ = true;
    std::string requestedEntryFunction_;
    std::vector<RuntimeValue> entryArguments_;
    std::optional<size_t> requestedEntryOutputCount_;
    std::string executedEntryFunction_;
    size_t executedRequestedOutputCount_ = 0;
    std::optional<FunctionSignature> entrySignature_;
    std::vector<RuntimeValue> entryOutputs_;
};

} // namespace

BytecodeVmResult BytecodeVm::run(const BytecodeProgram& program,
                                 const SemanticResult& semantic) {
    BytecodeVmContext context;
    return context.run(program, semantic, nullptr, BytecodeVmOptions{});
}

BytecodeVmResult BytecodeVm::run(const BytecodeProgram& program,
                                 const SemanticResult& semantic,
                                 const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, nullptr, options);
}

BytecodeVmResult BytecodeVm::run(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeTypedIrModule& typedIr) {
    BytecodeVmContext context;
    return context.run(program, semantic, &typedIr, BytecodeVmOptions{});
}

BytecodeVmResult BytecodeVm::run(
    const BytecodeProgram& program, const SemanticResult& semantic,
    const BytecodeTypedIrModule& typedIr,
    const BytecodeVmOptions& options) {
    BytecodeVmContext context;
    return context.run(program, semantic, &typedIr, options);
}

} // namespace mparser
