#include "mparser/bytecode.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

constexpr size_t kMaxValidationDiagnostics = 64;
constexpr size_t kNoPc = std::numeric_limits<size_t>::max();

bool isValidOpcode(BytecodeOp op) {
    const int value = static_cast<int>(op);
    return value >= static_cast<int>(BytecodeOp::EnterModule) &&
           value < static_cast<int>(BytecodeOp::Unknown);
}

bool isValidBindingKind(BindingKind kind) {
    const int value = static_cast<int>(kind);
    return value >= static_cast<int>(BindingKind::Unresolved) &&
           value <= static_cast<int>(BindingKind::Builtin);
}

std::optional<BytecodeOp> matchingLeave(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::EnterModule:
        return BytecodeOp::LeaveModule;
    case BytecodeOp::EnterClass:
        return BytecodeOp::LeaveClass;
    case BytecodeOp::EnterPropertyInitializer:
        return BytecodeOp::LeavePropertyInitializer;
    case BytecodeOp::EnterEnumerationMemberInitializer:
        return BytecodeOp::LeaveEnumerationMemberInitializer;
    case BytecodeOp::EnterArgumentDefault:
        return BytecodeOp::LeaveArgumentDefault;
    case BytecodeOp::EnterFunction:
        return BytecodeOp::LeaveFunction;
    case BytecodeOp::EnterControl:
        return BytecodeOp::LeaveControl;
    default:
        return std::nullopt;
    }
}

std::optional<BytecodeOp> matchingEnter(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::LeaveModule:
        return BytecodeOp::EnterModule;
    case BytecodeOp::LeaveClass:
        return BytecodeOp::EnterClass;
    case BytecodeOp::LeavePropertyInitializer:
        return BytecodeOp::EnterPropertyInitializer;
    case BytecodeOp::LeaveEnumerationMemberInitializer:
        return BytecodeOp::EnterEnumerationMemberInitializer;
    case BytecodeOp::LeaveArgumentDefault:
        return BytecodeOp::EnterArgumentDefault;
    case BytecodeOp::LeaveFunction:
        return BytecodeOp::EnterFunction;
    case BytecodeOp::LeaveControl:
        return BytecodeOp::EnterControl;
    default:
        return std::nullopt;
    }
}

bool hasControlTarget(const BytecodeInstruction& instruction) {
    switch (instruction.op) {
    case BytecodeOp::EnterArgumentDefault:
    case BytecodeOp::SwitchCase:
    case BytecodeOp::SwitchOtherwise:
    case BytecodeOp::TryBegin:
    case BytecodeOp::TryEnd:
    case BytecodeOp::Jump:
    case BytecodeOp::JumpIfFalse:
    case BytecodeOp::Break:
    case BytecodeOp::Continue:
    case BytecodeOp::ForBegin:
    case BytecodeOp::ForNext:
        return true;
    case BytecodeOp::MakeFunctionHandle:
        return instruction.operand == "@()";
    default:
        return false;
    }
}

bool hasVariableResultCount(BytecodeOp op) {
    return op == BytecodeOp::LoadName ||
           op == BytecodeOp::MemberAccess ||
           op == BytecodeOp::CallOrIndex ||
           op == BytecodeOp::CallSuperclass ||
           op == BytecodeOp::BraceIndex;
}

bool requiresZeroOperandCount(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::EnterModule:
    case BytecodeOp::LeaveModule:
    case BytecodeOp::EnterClass:
    case BytecodeOp::LeaveClass:
    case BytecodeOp::EnterPropertyInitializer:
    case BytecodeOp::LeavePropertyInitializer:
    case BytecodeOp::LeaveEnumerationMemberInitializer:
    case BytecodeOp::EnterArgumentDefault:
    case BytecodeOp::LeaveArgumentDefault:
    case BytecodeOp::EnterFunction:
    case BytecodeOp::LeaveFunction:
    case BytecodeOp::EnterControl:
    case BytecodeOp::LeaveControl:
    case BytecodeOp::ControlHeader:
    case BytecodeOp::ControlArm:
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
    case BytecodeOp::LoadName:
    case BytecodeOp::LoadLiteral:
    case BytecodeOp::StoreName:
    case BytecodeOp::MakeFunctionHandle:
    case BytecodeOp::LoadMetaClass:
        return true;
    default:
        return false;
    }
}

struct StackEffect {
    int64_t requiredDepth = 0;
    int64_t delta = 0;
};

StackEffect stackEffect(const BytecodeInstruction& instruction) {
    const int64_t operands = instruction.operandCount;
    const int64_t results = instruction.resultCount;
    switch (instruction.op) {
    case BytecodeOp::LoadName:
        return {0, results};
    case BytecodeOp::LoadLiteral:
    case BytecodeOp::LoadMetaClass:
    case BytecodeOp::MakeFunctionHandle:
        return {0, 1};
    case BytecodeOp::StoreName:
    case BytecodeOp::CaptureExpression:
    case BytecodeOp::Pop:
    case BytecodeOp::JumpIfFalse:
    case BytecodeOp::ForBegin:
    case BytecodeOp::SwitchBegin:
    case BytecodeOp::SwitchCase:
        return {1, -1};
    case BytecodeOp::UnaryOp:
    case BytecodeOp::PostfixOp:
    case BytecodeOp::MakeNameValueArgument:
        return {1, 0};
    case BytecodeOp::BinaryOp:
    case BytecodeOp::MakeMatrix:
    case BytecodeOp::MakeMatrixRow:
    case BytecodeOp::MakeCell:
    case BytecodeOp::MakeCellRow:
        return {operands, 1 - operands};
    case BytecodeOp::MemberAccess:
        return {operands, results - operands};
    case BytecodeOp::CallOrIndex:
        return {operands + 1, results - operands - 1};
    case BytecodeOp::CallSuperclass:
        return {operands, results - operands};
    case BytecodeOp::BraceIndex:
        return {operands + 1, results - operands - 1};
    case BytecodeOp::StoreMember: {
        const int64_t consumed =
            operands + (instruction.receiverName.empty() ? 1 : 0);
        return {consumed, -consumed};
    }
    case BytecodeOp::StoreIndex:
    case BytecodeOp::StoreBraceIndex:
        return {operands + 2, -operands - 2};
    case BytecodeOp::LvalueDescendMember:
        return {operands, -operands};
    case BytecodeOp::LvalueDescendIndex:
    case BytecodeOp::LvalueDescendBrace:
        return {operands, -operands};
    case BytecodeOp::StorePathMember:
        return {operands + 1, -operands - 1};
    case BytecodeOp::StorePathIndex:
    case BytecodeOp::StorePathBrace:
        return {operands + 1, -operands - 1};
    case BytecodeOp::BeginIndexContext:
        return {1, 0};
    default:
        return {};
    }
}

class BytecodeValidator {
public:
    BytecodeValidator(const BytecodeProgram& program,
                      const SemanticResult* semantic)
        : program_(program), semantic_(semantic),
          matchingBoundary_(program.instructions.size(), kNoPc),
          matchingFor_(program.instructions.size(), kNoPc),
          matchingSwitch_(program.instructions.size(), kNoPc),
          matchingTry_(program.instructions.size(), kNoPc),
          switchOwner_(program.instructions.size(), kNoPc),
          containerAt_(program.instructions.size(), kNoPc),
          anonymousOwnerAt_(program.instructions.size(), kNoPc),
          anonymousEndByOwner_(program.instructions.size(), kNoPc),
          expectedForAt_(program.instructions.size()),
          expectedSwitchAt_(program.instructions.size()),
          expectedTryAt_(program.instructions.size()) {}

    BytecodeValidationResult validate() {
        if (program_.instructions.size() >
            static_cast<size_t>(std::numeric_limits<int>::max())) {
            addProgramDiagnostic(
                "instruction count exceeds the bytecode target range");
            return std::move(result_);
        }

        validateInstructionMetadata();
        validateStructuredRanges();
        collectAnonymousExecutionRanges();
        validateStructuredExecutionOwners();
        validateTargets();
        validateIndexAndLvalueContexts();
        if (result_.succeeded) {
            validateReachableStackDepths();
        }
        return std::move(result_);
    }

private:
    struct Boundary {
        BytecodeOp op = BytecodeOp::Unknown;
        size_t pc = 0;
    };

    struct SwitchState {
        size_t pc = 0;
        bool hasOtherwise = false;
    };

    struct IndexContext {
        bool lvalue = false;
        size_t pc = 0;
        size_t total = 0;
        size_t nextPosition = 0;

        bool operator==(const IndexContext&) const = default;
    };

    struct LvalueContext {
        size_t pc = 0;
        size_t segmentCount = 0;
        size_t completedSegments = 0;

        bool operator==(const LvalueContext&) const = default;
    };

    struct ControlFlowState {
        int64_t operandDepth = 0;
        std::vector<size_t> forLoops;
        std::vector<size_t> switches;
        std::vector<size_t> tries;
        std::vector<IndexContext> indexContexts;
        std::vector<LvalueContext> lvalues;

        bool operator==(const ControlFlowState&) const = default;
    };

    struct ExecutionRange {
        size_t begin = 0;
        size_t end = 0;
        std::string label;
        int64_t expectedExitDepth = 0;
        size_t ownerPc = kNoPc;
    };

    bool sameExecutableOwner(size_t left, size_t right) const {
        const size_t leftContainer =
            left == program_.instructions.size()
                ? kNoPc
                : containerAt_[left];
        const size_t rightContainer =
            right == program_.instructions.size()
                ? kNoPc
                : containerAt_[right];
        const size_t leftAnonymous =
            left == program_.instructions.size()
                ? kNoPc
                : anonymousOwnerAt_[left];
        const size_t rightAnonymous =
            right == program_.instructions.size()
                ? kNoPc
                : anonymousOwnerAt_[right];
        return leftContainer == rightContainer &&
               leftAnonymous == rightAnonymous;
    }

    void validateSameExecutableBoundary(
        size_t begin, size_t end, std::string_view label) {
        if (!sameExecutableOwner(begin, end)) {
            addDiagnostic(
                end, std::string(label) +
                         " crosses an executable boundary");
        }
    }

    void addProgramDiagnostic(std::string message) {
        addDiagnostic(kNoPc, SourceSpan{}, std::move(message));
    }

    void addDiagnostic(size_t pc, const SourceSpan& span,
                       std::string message) {
        result_.succeeded = false;
        if (result_.diagnostics.size() >= kMaxValidationDiagnostics) {
            return;
        }
        std::string prefix = "invalid bytecode";
        if (pc != kNoPc) {
            prefix += " at pc " + std::to_string(pc) + " (";
            prefix += isValidOpcode(program_.instructions[pc].op)
                          ? bytecodeOpName(program_.instructions[pc].op)
                          : "InvalidOpcode";
            prefix += ")";
        }
        result_.diagnostics.push_back(Diagnostic{
            span, prefix + ": " + std::move(message),
            std::string(kInvalidBytecodeProgramIdentifier),
            DiagnosticSeverity::Error});
    }

    void addDiagnostic(size_t pc, std::string message) {
        addDiagnostic(pc, program_.instructions[pc].span,
                      std::move(message));
    }

    void validateBinding(size_t pc, std::string_view role,
                         const BindingRef& binding) {
        if (!isValidBindingKind(binding.kind)) {
            addDiagnostic(pc, std::string(role) +
                                  " has an invalid binding kind");
            return;
        }
        if (binding.symbolId < -1) {
            addDiagnostic(pc, std::string(role) +
                                  " has a negative symbol id below -1");
            return;
        }
        if (semantic_ && binding.symbolId >= 0 &&
            static_cast<size_t>(binding.symbolId) >=
                semantic_->symbols.size()) {
            addDiagnostic(pc, std::string(role) +
                                  " references a symbol outside the "
                                  "semantic symbol table");
        }
    }

    void validateSpan(size_t pc, const SourceSpan& span) {
        if (span.begin.line < 1 || span.begin.column < 1 ||
            span.end.line < 1 || span.end.column < 1) {
            addDiagnostic(pc, "source span has a non-positive line or column");
        }
        if (span.begin.sourceId == span.end.sourceId &&
            span.begin.offset > span.end.offset) {
            addDiagnostic(pc, "source span offsets are reversed");
        }
        if (!semantic_) {
            return;
        }
        for (const auto sourceId :
             {span.begin.sourceId, span.end.sourceId}) {
            if (sourceId != kInvalidSourceId &&
                sourceId >= semantic_->sources.size()) {
                addDiagnostic(pc,
                              "source span references an unknown source");
                break;
            }
        }
    }

    void expectOperandCount(size_t pc, int minimum, int maximum,
                            std::string_view description) {
        const int value = program_.instructions[pc].operandCount;
        if (value < minimum || value > maximum) {
            addDiagnostic(
                pc, std::string(description) + " requires operandCount in [" +
                        std::to_string(minimum) + ", " +
                        std::to_string(maximum) + "]");
        }
    }

    void validateFunctionHandleMetadata(size_t pc) {
        const auto& instruction = program_.instructions[pc];
        if (instruction.op != BytecodeOp::MakeFunctionHandle) {
            if (!instruction.parameters.empty() ||
                !instruction.captureNames.empty()) {
                addDiagnostic(
                    pc, "only MakeFunctionHandle may carry parameters or "
                        "capture names");
            }
            return;
        }
        if (instruction.operand != "@()" &&
            (!instruction.parameters.empty() ||
             !instruction.captureNames.empty())) {
            addDiagnostic(
                pc, "a named function handle cannot carry an anonymous "
                    "capture contract");
            return;
        }

        std::set<std::string> parameters;
        for (const auto& parameter : instruction.parameters) {
            if (parameter.empty()) {
                addDiagnostic(pc,
                              "anonymous function parameter is empty");
            } else if (parameter != "~" &&
                       !parameters.insert(parameter).second) {
                addDiagnostic(
                    pc, "anonymous function parameter is duplicated: " +
                            parameter);
            }
        }

        std::set<std::string> captures;
        for (const auto& capture : instruction.captureNames) {
            if (capture.empty()) {
                addDiagnostic(pc, "anonymous function capture is empty");
            } else if (!captures.insert(capture).second) {
                addDiagnostic(
                    pc, "anonymous function capture is duplicated: " +
                            capture);
            } else if (parameters.contains(capture)) {
                addDiagnostic(
                    pc, "anonymous function capture duplicates a parameter: " +
                            capture);
            }
        }
    }

    void validateInstructionMetadata() {
        const size_t instructionCount = program_.instructions.size();
        for (size_t pc = 0; pc < instructionCount; ++pc) {
            const auto& instruction = program_.instructions[pc];
            if (!isValidOpcode(instruction.op)) {
                addDiagnostic(pc, "opcode is unknown or outside the "
                                  "supported opcode range");
                continue;
            }
            validateBinding(pc, "binding", instruction.binding);
            validateBinding(pc, "receiver binding",
                            instruction.receiverBinding);
            validateSpan(pc, instruction.span);
            validateFunctionHandleMetadata(pc);

            if (instruction.operandCount < 0) {
                addDiagnostic(pc, "operandCount is negative");
            } else if (static_cast<size_t>(instruction.operandCount) >
                       instructionCount) {
                addDiagnostic(pc, "operandCount exceeds the instruction "
                                  "count");
            }
            if (instruction.resultCount < 0) {
                addDiagnostic(pc, "resultCount is negative");
            } else if (static_cast<size_t>(instruction.resultCount) >
                       instructionCount) {
                addDiagnostic(pc, "resultCount exceeds the instruction "
                                  "count");
            }
            if (instruction.implicitExpressionOutput &&
                ((instruction.op != BytecodeOp::CallOrIndex &&
                  instruction.op != BytecodeOp::LoadName) ||
                 instruction.resultCount != 1)) {
                addDiagnostic(
                    pc,
                    "implicit expression output requires a one-result "
                    "CallOrIndex or LoadName instruction");
            }
            if (instruction.anonymousBodyOutput &&
                ((instruction.op != BytecodeOp::CallOrIndex &&
                  instruction.op != BytecodeOp::LoadName) ||
                 instruction.resultCount != 1)) {
                addDiagnostic(
                    pc,
                    "anonymous body output requires a one-result "
                    "CallOrIndex or LoadName instruction");
            }
            if (instruction.hasIndexContext &&
                instruction.op != BytecodeOp::CallOrIndex) {
                addDiagnostic(
                    pc,
                    "only CallOrIndex can consume an index context");
            }

            if (!hasVariableResultCount(instruction.op) &&
                instruction.resultCount != 1) {
                addDiagnostic(
                    pc, "this opcode requires resultCount equal to 1");
            }
            if (requiresZeroOperandCount(instruction.op) &&
                instruction.operandCount != 0) {
                addDiagnostic(
                    pc, "this opcode requires operandCount equal to 0");
            }

            switch (instruction.op) {
            case BytecodeOp::EnterEnumerationMemberInitializer:
            case BytecodeOp::BeginIndexContext:
            case BytecodeOp::BeginIndexArgument:
            case BytecodeOp::BeginLvalueIndexContext:
            case BytecodeOp::CallOrIndex:
            case BytecodeOp::CallSuperclass:
            case BytecodeOp::BraceIndex:
            case BytecodeOp::MakeMatrix:
            case BytecodeOp::MakeMatrixRow:
            case BytecodeOp::MakeCell:
            case BytecodeOp::MakeCellRow:
            case BytecodeOp::StoreIndex:
            case BytecodeOp::StoreBraceIndex:
            case BytecodeOp::LvalueDescendIndex:
            case BytecodeOp::LvalueDescendBrace:
            case BytecodeOp::StorePathIndex:
            case BytecodeOp::StorePathBrace:
                break;
            case BytecodeOp::UnaryOp:
            case BytecodeOp::PostfixOp:
            case BytecodeOp::MakeNameValueArgument:
                expectOperandCount(pc, 1, 1, bytecodeOpName(instruction.op));
                break;
            case BytecodeOp::BinaryOp:
                if (instruction.operand == ":") {
                    expectOperandCount(pc, 2, 3, "colon BinaryOp");
                } else {
                    expectOperandCount(pc, 2, 2, "BinaryOp");
                }
                break;
            case BytecodeOp::MemberAccess:
            case BytecodeOp::StoreMember:
                expectOperandCount(pc, 1, 2,
                                   bytecodeOpName(instruction.op));
                break;
            case BytecodeOp::BeginLvalue:
                expectOperandCount(pc, 2,
                                   static_cast<int>(instructionCount),
                                   "BeginLvalue");
                break;
            case BytecodeOp::LvalueDescendMember:
            case BytecodeOp::StorePathMember:
                expectOperandCount(pc, 0, 1,
                                   bytecodeOpName(instruction.op));
                break;
            default:
                break;
            }

            const bool requiresColonContract =
                instruction.op == BytecodeOp::StoreIndex ||
                instruction.op == BytecodeOp::LvalueDescendIndex ||
                instruction.op == BytecodeOp::StorePathIndex;
            const bool acceptsColonContract =
                requiresColonContract ||
                instruction.op == BytecodeOp::CallOrIndex;
            if (requiresColonContract &&
                instruction.operandCount >= 0 &&
                instruction.colonSubscripts.size() !=
                    static_cast<size_t>(instruction.operandCount)) {
                addDiagnostic(
                    pc, "colon subscript metadata does not match "
                        "operandCount");
            } else if (instruction.op == BytecodeOp::CallOrIndex &&
                       !instruction.colonSubscripts.empty() &&
                       instruction.operandCount >= 0 &&
                       instruction.colonSubscripts.size() !=
                           static_cast<size_t>(instruction.operandCount)) {
                addDiagnostic(
                    pc, "colon subscript metadata does not match "
                        "operandCount");
            } else if (!acceptsColonContract &&
                       !instruction.colonSubscripts.empty()) {
                addDiagnostic(
                    pc, "this opcode cannot carry colon subscript metadata");
            }
        }
    }

    bool insideBoundary(const std::vector<Boundary>& stack,
                        BytecodeOp op) const {
        return std::any_of(
            stack.begin(), stack.end(),
            [op](const Boundary& boundary) {
                return boundary.op == op;
            });
    }

    void validateStructuredRanges() {
        std::vector<Boundary> boundaries;
        std::vector<size_t> forStack;
        std::vector<SwitchState> switchStack;
        std::vector<size_t> tryStack;

        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            const auto& instruction = program_.instructions[pc];
            containerAt_[pc] =
                boundaries.empty() ? kNoPc : boundaries.back().pc;
            expectedForAt_[pc] = forStack;
            expectedSwitchAt_[pc].reserve(switchStack.size());
            for (const auto& state : switchStack) {
                expectedSwitchAt_[pc].push_back(state.pc);
            }
            expectedTryAt_[pc] = tryStack;
            if (!isValidOpcode(instruction.op)) {
                continue;
            }

            if (const auto leave = matchingLeave(instruction.op)) {
                (void)leave;
                if (instruction.op == BytecodeOp::EnterModule &&
                    !boundaries.empty()) {
                    addDiagnostic(pc, "module boundary is nested");
                }
                if ((instruction.op ==
                         BytecodeOp::EnterPropertyInitializer ||
                     instruction.op ==
                         BytecodeOp::EnterEnumerationMemberInitializer) &&
                    !insideBoundary(boundaries,
                                    BytecodeOp::EnterClass)) {
                    addDiagnostic(pc,
                                  "class initializer is outside a class");
                }
                if (instruction.op ==
                        BytecodeOp::EnterArgumentDefault &&
                    !insideBoundary(boundaries,
                                    BytecodeOp::EnterFunction)) {
                    addDiagnostic(
                        pc, "argument default is outside a function");
                }
                boundaries.push_back(Boundary{instruction.op, pc});
            } else if (const auto enter =
                           matchingEnter(instruction.op)) {
                if (boundaries.empty() ||
                    boundaries.back().op != *enter) {
                    addDiagnostic(
                        pc, "boundary leave does not match the active "
                            "boundary");
                    boundaryPairsValid_ = false;
                } else {
                    const size_t begin = boundaries.back().pc;
                    boundaries.pop_back();
                    matchingBoundary_[begin] = pc;
                    matchingBoundary_[pc] = begin;
                }
            }

            switch (instruction.op) {
            case BytecodeOp::ForBegin:
                forStack.push_back(pc);
                break;
            case BytecodeOp::ForNext:
                if (forStack.empty()) {
                    addDiagnostic(pc,
                                  "ForNext has no active ForBegin");
                } else {
                    const size_t begin = forStack.back();
                    forStack.pop_back();
                    matchingFor_[begin] = pc;
                    matchingFor_[pc] = begin;
                    validateSameExecutableBoundary(
                        begin, pc, "for loop");
                }
                break;
            case BytecodeOp::Break:
                if (forStack.empty()) {
                    addDiagnostic(
                        pc, "Break has no active bytecode for loop");
                }
                break;
            case BytecodeOp::SwitchBegin:
                switchStack.push_back(SwitchState{pc, false});
                break;
            case BytecodeOp::SwitchCase:
                if (switchStack.empty()) {
                    addDiagnostic(pc,
                                  "SwitchCase has no active switch");
                } else if (switchStack.back().hasOtherwise) {
                    addDiagnostic(
                        pc, "SwitchCase appears after SwitchOtherwise");
                } else {
                    switchOwner_[pc] = switchStack.back().pc;
                }
                break;
            case BytecodeOp::SwitchOtherwise:
                if (switchStack.empty()) {
                    addDiagnostic(
                        pc, "SwitchOtherwise has no active switch");
                } else if (switchStack.back().hasOtherwise) {
                    addDiagnostic(
                        pc, "switch has more than one otherwise arm");
                } else {
                    switchStack.back().hasOtherwise = true;
                    switchOwner_[pc] = switchStack.back().pc;
                }
                break;
            case BytecodeOp::SwitchEnd:
                if (switchStack.empty()) {
                    addDiagnostic(pc,
                                  "SwitchEnd has no active SwitchBegin");
                } else {
                    const size_t begin = switchStack.back().pc;
                    switchStack.pop_back();
                    matchingSwitch_[begin] = pc;
                    matchingSwitch_[pc] = begin;
                    validateSameExecutableBoundary(
                        begin, pc, "switch");
                }
                break;
            case BytecodeOp::TryBegin:
                tryStack.push_back(pc);
                break;
            case BytecodeOp::TryEnd:
                if (tryStack.empty()) {
                    addDiagnostic(pc,
                                  "TryEnd has no active TryBegin");
                } else {
                    const size_t begin = tryStack.back();
                    tryStack.pop_back();
                    matchingTry_[begin] = pc;
                    matchingTry_[pc] = begin;
                    validateSameExecutableBoundary(
                        begin, pc, "try block");
                }
                break;
            default:
                break;
            }
        }

        for (const auto& boundary : boundaries) {
            addDiagnostic(boundary.pc,
                          "boundary enter has no matching leave");
            boundaryPairsValid_ = false;
        }
        for (const size_t pc : forStack) {
            addDiagnostic(pc, "ForBegin has no matching ForNext");
        }
        for (const auto& state : switchStack) {
            addDiagnostic(state.pc,
                          "SwitchBegin has no matching SwitchEnd");
        }
        for (const size_t pc : tryStack) {
            addDiagnostic(pc, "TryBegin has no matching TryEnd");
        }
    }

    void collectAnonymousExecutionRanges() {
        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            const auto& instruction = program_.instructions[pc];
            if (instruction.op != BytecodeOp::MakeFunctionHandle ||
                instruction.operand != "@()" ||
                instruction.target < 0) {
                continue;
            }
            const size_t target =
                static_cast<size_t>(instruction.target);
            if (target > program_.instructions.size() ||
                target <= pc + 1) {
                continue;
            }
            anonymousRanges_.push_back(ExecutionRange{
                pc + 1, target, "anonymous function", 1, pc});
            anonymousEndByOwner_[pc] = target;
        }

        std::vector<size_t> order(anonymousRanges_.size());
        for (size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::sort(
            order.begin(), order.end(),
            [&](size_t left, size_t right) {
                const auto& lhs = anonymousRanges_[left];
                const auto& rhs = anonymousRanges_[right];
                if (lhs.begin != rhs.begin) {
                    return lhs.begin < rhs.begin;
                }
                return lhs.end > rhs.end;
            });

        std::vector<size_t> active;
        for (const size_t index : order) {
            const auto& range = anonymousRanges_[index];
            while (!active.empty() &&
                   anonymousRanges_[active.back()].end <=
                       range.begin) {
                active.pop_back();
            }
            if (!active.empty() &&
                range.end > anonymousRanges_[active.back()].end) {
                addDiagnostic(
                    range.ownerPc,
                    "anonymous function bodies overlap without nesting");
                continue;
            }
            active.push_back(index);
            for (size_t pc = range.begin; pc < range.end; ++pc) {
                anonymousOwnerAt_[pc] = range.ownerPc;
            }
        }
    }

    void validateStructuredExecutionOwners() {
        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            if (matchingFor_[pc] != kNoPc &&
                matchingFor_[pc] > pc) {
                validateSameExecutableBoundary(
                    pc, matchingFor_[pc], "for loop");
            }
            if (matchingSwitch_[pc] != kNoPc &&
                matchingSwitch_[pc] > pc) {
                validateSameExecutableBoundary(
                    pc, matchingSwitch_[pc], "switch");
            }
            if (matchingTry_[pc] != kNoPc &&
                matchingTry_[pc] > pc) {
                validateSameExecutableBoundary(
                    pc, matchingTry_[pc], "try block");
            }
        }
    }

    std::optional<size_t> checkedTarget(size_t pc) {
        const int raw = program_.instructions[pc].target;
        if (raw < 0) {
            addDiagnostic(pc, "control instruction has no target");
            return std::nullopt;
        }
        const size_t target = static_cast<size_t>(raw);
        if (target > program_.instructions.size()) {
            addDiagnostic(pc, "control target is out of bounds");
            return std::nullopt;
        }
        if (boundaryPairsValid_) {
            const size_t sourceAnonymous =
                anonymousOwnerAt_[pc];
            const size_t sourceContainer = containerAt_[pc];
            const size_t targetContainer =
                target == program_.instructions.size()
                    ? kNoPc
                    : containerAt_[target];
            const bool nestedHandleReturnsFromOwner =
                program_.instructions[pc].op ==
                    BytecodeOp::MakeFunctionHandle &&
                sourceAnonymous != kNoPc &&
                anonymousEndByOwner_[sourceAnonymous] == target &&
                sourceContainer == targetContainer;
            if (!sameExecutableOwner(pc, target) &&
                !nestedHandleReturnsFromOwner) {
                addDiagnostic(
                    pc, "control target crosses an executable boundary");
            }
        }
        return target;
    }

    void validateTargets() {
        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            const auto& instruction = program_.instructions[pc];
            if (!isValidOpcode(instruction.op)) {
                continue;
            }

            if (!hasControlTarget(instruction)) {
                if (instruction.target != -1) {
                    addDiagnostic(
                        pc, "opcode carries an unexpected control target");
                }
                continue;
            }

            const auto target = checkedTarget(pc);
            if (!target) {
                continue;
            }
            switch (instruction.op) {
            case BytecodeOp::EnterArgumentDefault: {
                const size_t leave = matchingBoundary_[pc];
                if (leave == kNoPc || *target != leave + 1) {
                    addDiagnostic(
                        pc, "argument default target does not follow its "
                            "matching leave");
                }
                break;
            }
            case BytecodeOp::MakeFunctionHandle:
                if (*target <= pc + 1) {
                    addDiagnostic(
                        pc, "anonymous function body is empty or reversed");
                }
                break;
            case BytecodeOp::JumpIfFalse:
            case BytecodeOp::Break:
                if (*target <= pc) {
                    addDiagnostic(
                        pc, "forward control target does not advance");
                }
                break;
            case BytecodeOp::ForBegin: {
                const size_t next = matchingFor_[pc];
                if (next == kNoPc || *target != next + 1) {
                    addDiagnostic(
                        pc, "ForBegin exit does not follow its ForNext");
                }
                break;
            }
            case BytecodeOp::ForNext: {
                const size_t begin = matchingFor_[pc];
                if (begin == kNoPc || *target != begin + 1 ||
                    *target > pc) {
                    addDiagnostic(
                        pc, "ForNext target is not the matching loop body");
                } else if (program_.instructions[begin].operand !=
                           instruction.operand) {
                    addDiagnostic(
                        pc, "ForBegin and ForNext use different variables");
                }
                break;
            }
            case BytecodeOp::SwitchCase:
            case BytecodeOp::SwitchOtherwise: {
                const size_t owner = switchOwner_[pc];
                const size_t end =
                    owner == kNoPc ? kNoPc : matchingSwitch_[owner];
                if (end == kNoPc || *target <= pc || *target > end) {
                    addDiagnostic(
                        pc, "switch arm target is outside its switch");
                }
                break;
            }
            case BytecodeOp::TryBegin: {
                const size_t end = matchingTry_[pc];
                if (end == kNoPc || *target != end + 1) {
                    addDiagnostic(
                        pc, "TryBegin catch target does not follow TryEnd");
                }
                break;
            }
            case BytecodeOp::TryEnd:
                if (*target <= pc) {
                    addDiagnostic(
                        pc, "TryEnd continuation does not advance");
                }
                break;
            default:
                break;
            }
        }
    }

    bool closesNormalIndexContext(
        const BytecodeInstruction& instruction) const {
        if (instruction.op == BytecodeOp::BraceIndex ||
            instruction.op == BytecodeOp::StoreIndex ||
            instruction.op == BytecodeOp::StoreBraceIndex) {
            return true;
        }
        return instruction.op == BytecodeOp::CallOrIndex &&
               instruction.hasIndexContext;
    }

    bool closesLvalueIndexContext(BytecodeOp op) const {
        return op == BytecodeOp::LvalueDescendIndex ||
               op == BytecodeOp::LvalueDescendBrace ||
               op == BytecodeOp::StorePathIndex ||
               op == BytecodeOp::StorePathBrace;
    }

    void closeIndexContext(size_t pc,
                           std::vector<IndexContext>& contexts,
                           bool lvalue) {
        if (contexts.empty() || contexts.back().lvalue != lvalue) {
            addDiagnostic(
                pc, lvalue
                        ? "lvalue index operation has no active context"
                        : "index operation has no active context");
            return;
        }
        const auto context = contexts.back();
        contexts.pop_back();
        const auto& instruction = program_.instructions[pc];
        if (!sameExecutableOwner(context.pc, pc)) {
            addDiagnostic(
                pc, "index context crosses an executable boundary");
        }
        if (context.nextPosition != context.total) {
            addDiagnostic(
                pc, "index context did not declare every argument");
        }
        if (instruction.operandCount < 0 ||
            static_cast<size_t>(instruction.operandCount) !=
                context.total) {
            addDiagnostic(
                pc, "index operation arity does not match its context");
        }
    }

    void validateIndexAndLvalueContexts() {
        std::vector<IndexContext> indexContexts;
        std::vector<LvalueContext> lvalues;
        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            const auto& instruction = program_.instructions[pc];
            if (!isValidOpcode(instruction.op) ||
                instruction.operandCount < 0) {
                continue;
            }

            if (instruction.op == BytecodeOp::BeginIndexContext ||
                instruction.op ==
                    BytecodeOp::BeginLvalueIndexContext) {
                indexContexts.push_back(IndexContext{
                    instruction.op ==
                        BytecodeOp::BeginLvalueIndexContext,
                    pc,
                    static_cast<size_t>(instruction.operandCount),
                    0});
            } else if (instruction.op ==
                       BytecodeOp::BeginIndexArgument) {
                if (indexContexts.empty()) {
                    addDiagnostic(
                        pc, "index argument has no active index context");
                } else {
                    auto& context = indexContexts.back();
                    if (!sameExecutableOwner(context.pc, pc)) {
                        addDiagnostic(
                            pc, "index argument crosses an executable "
                                "boundary");
                    }
                    const size_t position =
                        static_cast<size_t>(instruction.operandCount);
                    if (position != context.nextPosition ||
                        position >= context.total) {
                        addDiagnostic(
                            pc, "index argument position is out of order");
                    } else {
                        ++context.nextPosition;
                    }
                }
            }

            if (closesNormalIndexContext(instruction)) {
                closeIndexContext(pc, indexContexts, false);
            } else if (closesLvalueIndexContext(instruction.op)) {
                closeIndexContext(pc, indexContexts, true);
            }

            if (instruction.op == BytecodeOp::BeginLvalue) {
                if (!lvalues.empty()) {
                    addDiagnostic(pc,
                                  "nested lvalue paths are not valid");
                }
                lvalues.push_back(LvalueContext{
                    pc,
                    static_cast<size_t>(instruction.operandCount),
                    0});
            } else if (
                instruction.op == BytecodeOp::LvalueDescendMember ||
                instruction.op == BytecodeOp::LvalueDescendIndex ||
                instruction.op == BytecodeOp::LvalueDescendBrace) {
                if (lvalues.empty()) {
                    addDiagnostic(
                        pc, "lvalue descent has no active path");
                } else {
                    if (!sameExecutableOwner(
                            lvalues.back().pc, pc)) {
                        addDiagnostic(
                            pc, "lvalue path crosses an executable "
                                "boundary");
                    }
                    ++lvalues.back().completedSegments;
                    if (lvalues.back().completedSegments >=
                        lvalues.back().segmentCount) {
                        addDiagnostic(
                            pc, "lvalue descent consumes the final segment");
                    }
                }
            } else if (
                instruction.op == BytecodeOp::StorePathMember ||
                instruction.op == BytecodeOp::StorePathIndex ||
                instruction.op == BytecodeOp::StorePathBrace) {
                if (lvalues.empty()) {
                    addDiagnostic(
                        pc, "path store has no active lvalue path");
                } else {
                    auto context = lvalues.back();
                    lvalues.pop_back();
                    if (!sameExecutableOwner(context.pc, pc)) {
                        addDiagnostic(
                            pc, "lvalue path crosses an executable "
                                "boundary");
                    }
                    ++context.completedSegments;
                    if (context.completedSegments !=
                        context.segmentCount) {
                        addDiagnostic(
                            pc, "path store does not complete its declared "
                                "lvalue segments");
                    }
                }
            }
        }

        for (const auto& context : indexContexts) {
            addDiagnostic(context.pc,
                          "index context has no consuming operation");
        }
        for (const auto& context : lvalues) {
            addDiagnostic(context.pc,
                          "lvalue path has no final store");
        }
    }

    bool skipsBoundary(BytecodeOp op) const {
        return op == BytecodeOp::EnterClass ||
               op == BytecodeOp::EnterPropertyInitializer ||
               op ==
                   BytecodeOp::EnterEnumerationMemberInitializer ||
               op == BytecodeOp::EnterFunction;
    }

    std::vector<size_t> expectedContextsWithinRange(
        const std::vector<std::vector<size_t>>& expectedAt,
        size_t target, const ExecutionRange& range) const {
        std::vector<size_t> result;
        if (target >= range.end ||
            target >= expectedAt.size()) {
            return result;
        }
        for (const size_t owner : expectedAt[target]) {
            if (owner >= range.begin && owner < range.end) {
                result.push_back(owner);
            }
        }
        return result;
    }

    bool matchesStructuredContexts(
        const ControlFlowState& state, size_t target,
        const ExecutionRange& range) const {
        return state.forLoops ==
                   expectedContextsWithinRange(
                       expectedForAt_, target, range) &&
               state.switches ==
                   expectedContextsWithinRange(
                       expectedSwitchAt_, target, range) &&
               state.tries ==
                   expectedContextsWithinRange(
                       expectedTryAt_, target, range);
    }

    void validateStackRange(const ExecutionRange& range) {
        if (range.begin >= range.end) {
            return;
        }
        const size_t length = range.end - range.begin;
        std::vector<std::optional<ControlFlowState>> states(length + 1);
        std::vector<bool> stateMismatchReported(length + 1, false);
        std::vector<bool> lexicalMismatchReported(length + 1, false);
        std::vector<bool> underflowReported(length, false);
        std::deque<size_t> queue;
        states[0] = ControlFlowState{};
        queue.push_back(range.begin);

        auto relax = [&](size_t sourcePc, size_t target,
                         const ControlFlowState& state) {
            if (target < range.begin || target > range.end) {
                addDiagnostic(
                    sourcePc, "control target escapes the " +
                                  range.label + " execution range");
                return;
            }
            const size_t index = target - range.begin;
            if (!matchesStructuredContexts(state, target, range)) {
                if (!lexicalMismatchReported[index]) {
                    addDiagnostic(
                        sourcePc,
                        "control flow reaches pc " +
                            std::to_string(target) +
                            " with an invalid structured runtime context");
                    lexicalMismatchReported[index] = true;
                }
                return;
            }
            if (!states[index]) {
                states[index] = state;
                if (target < range.end) {
                    queue.push_back(target);
                }
            } else if (*states[index] != state &&
                       !stateMismatchReported[index]) {
                if (states[index]->operandDepth !=
                    state.operandDepth) {
                    addDiagnostic(
                        sourcePc,
                        "control flow reaches pc " +
                            std::to_string(target) +
                            " with inconsistent stack depths " +
                            std::to_string(
                                states[index]->operandDepth) +
                            " and " +
                            std::to_string(state.operandDepth));
                } else {
                    addDiagnostic(
                        sourcePc,
                        "control flow reaches pc " +
                            std::to_string(target) +
                            " with inconsistent runtime context state");
                }
                stateMismatchReported[index] = true;
            }
        };

        while (!queue.empty()) {
            const size_t pc = queue.front();
            queue.pop_front();
            const size_t local = pc - range.begin;
            if (!states[local]) {
                continue;
            }
            const ControlFlowState input = *states[local];
            const int64_t inputDepth = input.operandDepth;
            const auto& instruction = program_.instructions[pc];

            if (skipsBoundary(instruction.op)) {
                const size_t leave = matchingBoundary_[pc];
                if (leave == kNoPc) {
                    continue;
                }
                relax(pc, leave + 1, input);
                continue;
            }
            if (instruction.op ==
                BytecodeOp::EnterArgumentDefault) {
                if (instruction.target >= 0) {
                    relax(pc, static_cast<size_t>(instruction.target),
                          input);
                }
                continue;
            }
            if (instruction.op == BytecodeOp::EnterControl) {
                continue;
            }

            const StackEffect effect = stackEffect(instruction);
            if (inputDepth < effect.requiredDepth) {
                if (!underflowReported[local]) {
                    addDiagnostic(
                        pc, "reachable path enters with stack depth " +
                                std::to_string(inputDepth) +
                                " but requires " +
                                std::to_string(effect.requiredDepth));
                    underflowReported[local] = true;
                }
                continue;
            }
            if (effect.delta > 0 &&
                inputDepth >
                    std::numeric_limits<int64_t>::max() -
                        effect.delta) {
                addDiagnostic(pc,
                              "reachable stack depth overflows");
                continue;
            }
            const int64_t outputDepth =
                inputDepth + effect.delta;
            ControlFlowState output = input;
            output.operandDepth = outputDepth;

            auto rejectContext = [&](std::string message) {
                addDiagnostic(pc, std::move(message));
                return false;
            };

            bool contextValid = true;
            if (instruction.op == BytecodeOp::BeginIndexContext ||
                instruction.op ==
                    BytecodeOp::BeginLvalueIndexContext) {
                const bool lvalue =
                    instruction.op ==
                    BytecodeOp::BeginLvalueIndexContext;
                if (lvalue && output.lvalues.empty()) {
                    contextValid = rejectContext(
                        "lvalue index context has no active lvalue path");
                } else {
                    output.indexContexts.push_back(IndexContext{
                        lvalue, pc,
                        static_cast<size_t>(
                            instruction.operandCount),
                        0});
                }
            } else if (instruction.op ==
                       BytecodeOp::BeginIndexArgument) {
                if (output.indexContexts.empty()) {
                    contextValid = rejectContext(
                        "reachable index argument has no active context");
                } else {
                    auto& context = output.indexContexts.back();
                    const size_t position =
                        static_cast<size_t>(
                            instruction.operandCount);
                    if (position != context.nextPosition ||
                        position >= context.total) {
                        contextValid = rejectContext(
                            "reachable index argument position is out of "
                            "order");
                    } else {
                        ++context.nextPosition;
                    }
                }
            }

            const bool closeNormal =
                closesNormalIndexContext(instruction);
            const bool closeLvalue =
                closesLvalueIndexContext(instruction.op);
            if (contextValid && (closeNormal || closeLvalue)) {
                const bool expectedLvalue = closeLvalue;
                if (output.indexContexts.empty() ||
                    output.indexContexts.back().lvalue !=
                        expectedLvalue) {
                    contextValid = rejectContext(
                        expectedLvalue
                            ? "reachable lvalue index operation has no "
                              "active context"
                            : "reachable index operation has no active "
                              "context");
                } else {
                    const auto context =
                        output.indexContexts.back();
                    output.indexContexts.pop_back();
                    if (context.nextPosition != context.total ||
                        instruction.operandCount < 0 ||
                        static_cast<size_t>(
                            instruction.operandCount) !=
                            context.total) {
                        contextValid = rejectContext(
                            "reachable index operation does not match its "
                            "context");
                    }
                }
            }

            if (contextValid &&
                instruction.op == BytecodeOp::BeginLvalue) {
                if (!output.lvalues.empty()) {
                    contextValid = rejectContext(
                        "reachable nested lvalue path is not valid");
                } else {
                    output.lvalues.push_back(LvalueContext{
                        pc,
                        static_cast<size_t>(
                            instruction.operandCount),
                        0});
                }
            } else if (
                contextValid &&
                (instruction.op ==
                     BytecodeOp::LvalueDescendMember ||
                 instruction.op ==
                     BytecodeOp::LvalueDescendIndex ||
                 instruction.op ==
                     BytecodeOp::LvalueDescendBrace)) {
                if (output.lvalues.empty()) {
                    contextValid = rejectContext(
                        "reachable lvalue descent has no active path");
                } else {
                    auto& context = output.lvalues.back();
                    ++context.completedSegments;
                    if (context.completedSegments >=
                        context.segmentCount) {
                        contextValid = rejectContext(
                            "reachable lvalue descent consumes the final "
                            "segment");
                    }
                }
            } else if (
                contextValid &&
                (instruction.op == BytecodeOp::StorePathMember ||
                 instruction.op == BytecodeOp::StorePathIndex ||
                 instruction.op == BytecodeOp::StorePathBrace)) {
                if (output.lvalues.empty()) {
                    contextValid = rejectContext(
                        "reachable path store has no active lvalue path");
                } else {
                    auto context = output.lvalues.back();
                    output.lvalues.pop_back();
                    ++context.completedSegments;
                    if (context.completedSegments !=
                        context.segmentCount) {
                        contextValid = rejectContext(
                            "reachable path store does not complete its "
                            "lvalue path");
                    }
                }
            }
            if (!contextValid) {
                continue;
            }

            auto next = [&]() {
                relax(pc, pc + 1, output);
            };
            auto target = [&](const ControlFlowState& state) {
                if (instruction.target >= 0) {
                    relax(pc,
                          static_cast<size_t>(instruction.target),
                          state);
                }
            };
            auto unwindStructuredForTarget =
                [&](ControlFlowState& state, size_t targetPc) {
                    const auto expectedSwitches =
                        expectedContextsWithinRange(
                            expectedSwitchAt_, targetPc, range);
                    while (state.switches.size() >
                           expectedSwitches.size()) {
                        state.switches.pop_back();
                    }
                    const auto expectedTries =
                        expectedContextsWithinRange(
                            expectedTryAt_, targetPc, range);
                    while (state.tries.size() >
                           expectedTries.size()) {
                        state.tries.pop_back();
                    }
                };

            switch (instruction.op) {
            case BytecodeOp::Jump: {
                if (instruction.target >= 0) {
                    unwindStructuredForTarget(
                        output,
                        static_cast<size_t>(instruction.target));
                }
                target(output);
                break;
            }
            case BytecodeOp::JumpIfFalse: {
                next();
                ControlFlowState branch = output;
                if (instruction.target >= 0) {
                    unwindStructuredForTarget(
                        branch,
                        static_cast<size_t>(instruction.target));
                }
                target(branch);
                break;
            }
            case BytecodeOp::SwitchCase:
            case BytecodeOp::SwitchOtherwise:
                if (instruction.op == BytecodeOp::SwitchCase ||
                    instruction.op ==
                        BytecodeOp::SwitchOtherwise) {
                    const size_t owner = switchOwner_[pc];
                    if (owner == kNoPc ||
                        output.switches.empty() ||
                        output.switches.back() != owner) {
                        rejectContext(
                            "switch arm has no matching active switch");
                        break;
                    }
                }
                next();
                target(output);
                break;
            case BytecodeOp::ForBegin: {
                ControlFlowState entered = output;
                entered.forLoops.push_back(pc);
                relax(pc, pc + 1, entered);
                target(output);
                break;
            }
            case BytecodeOp::ForNext: {
                const size_t owner = matchingFor_[pc];
                if (owner == kNoPc ||
                    output.forLoops.empty() ||
                    output.forLoops.back() != owner) {
                    rejectContext(
                        "ForNext has no matching active for loop");
                    break;
                }
                target(output);
                output.forLoops.pop_back();
                next();
                break;
            }
            case BytecodeOp::Break:
                if (output.forLoops.empty()) {
                    rejectContext(
                        "Break has no matching active for loop");
                    break;
                }
                output.forLoops.pop_back();
                if (instruction.target >= 0) {
                    unwindStructuredForTarget(
                        output,
                        static_cast<size_t>(instruction.target));
                }
                target(output);
                break;
            case BytecodeOp::Continue:
                if (output.forLoops.empty() ||
                    matchingFor_[output.forLoops.back()] == kNoPc ||
                    instruction.target != static_cast<int>(
                        matchingFor_[output.forLoops.back()])) {
                    rejectContext(
                        "Continue does not target its active ForNext");
                    break;
                }
                if (instruction.target >= 0) {
                    unwindStructuredForTarget(
                        output,
                        static_cast<size_t>(instruction.target));
                }
                target(output);
                break;
            case BytecodeOp::SwitchBegin:
                output.switches.push_back(pc);
                next();
                break;
            case BytecodeOp::SwitchEnd: {
                const size_t owner = matchingSwitch_[pc];
                if (owner == kNoPc ||
                    output.switches.empty() ||
                    output.switches.back() != owner) {
                    rejectContext(
                        "SwitchEnd has no matching active switch");
                    break;
                }
                output.switches.pop_back();
                next();
                break;
            }
            case BytecodeOp::TryBegin:
                output.tries.push_back(pc);
                next();
                if (instruction.target >= 0) {
                    relax(pc,
                          static_cast<size_t>(instruction.target),
                          input);
                }
                break;
            case BytecodeOp::TryEnd: {
                const size_t owner = matchingTry_[pc];
                if (owner == kNoPc || output.tries.empty() ||
                    output.tries.back() != owner) {
                    rejectContext(
                        "TryEnd has no matching active try block");
                    break;
                }
                output.tries.pop_back();
                target(output);
                break;
            }
            case BytecodeOp::Return:
                if (output.operandDepth !=
                    range.expectedExitDepth) {
                    addDiagnostic(
                        pc, "Return leaves stack depth " +
                                std::to_string(
                                    output.operandDepth) +
                                " but the " + range.label +
                                " requires " +
                                std::to_string(
                                    range.expectedExitDepth));
                }
                if (!output.indexContexts.empty() ||
                    !output.lvalues.empty()) {
                    addDiagnostic(
                        pc, "Return abandons an active index or lvalue "
                            "context");
                }
                break;
            case BytecodeOp::MakeFunctionHandle:
                if (instruction.operand == "@()") {
                    target(output);
                } else {
                    next();
                }
                break;
            default:
                next();
                break;
            }
        }

        if (states[length]) {
            const auto& exit = *states[length];
            const size_t diagnosticPc = range.end - 1;
            if (exit.operandDepth != range.expectedExitDepth) {
                addDiagnostic(
                    diagnosticPc,
                    range.label + " exits with stack depth " +
                        std::to_string(exit.operandDepth) +
                        " but requires " +
                        std::to_string(range.expectedExitDepth));
            }
            if (!exit.forLoops.empty() ||
                !exit.switches.empty() ||
                !exit.tries.empty() ||
                !exit.indexContexts.empty() ||
                !exit.lvalues.empty()) {
                addDiagnostic(
                    diagnosticPc,
                    range.label +
                        " exits with an active runtime context");
            }
        }
    }

    void addBoundaryExecutionRanges(
        std::vector<ExecutionRange>& ranges) const {
        for (size_t pc = 0; pc < program_.instructions.size(); ++pc) {
            const size_t leave = matchingBoundary_[pc];
            if (leave == kNoPc || leave <= pc) {
                continue;
            }
            const auto op = program_.instructions[pc].op;
            std::string label;
            int64_t expectedExitDepth = 0;
            switch (op) {
            case BytecodeOp::EnterFunction:
                label = "function";
                break;
            case BytecodeOp::EnterPropertyInitializer:
                label = "property initializer";
                expectedExitDepth = 1;
                break;
            case BytecodeOp::EnterEnumerationMemberInitializer:
                label = "enumeration initializer";
                expectedExitDepth =
                    program_.instructions[pc].operandCount;
                break;
            case BytecodeOp::EnterArgumentDefault:
                label = "argument default";
                expectedExitDepth = 1;
                break;
            default:
                continue;
            }
            ranges.push_back(
                ExecutionRange{pc + 1, leave, std::move(label),
                               expectedExitDepth, pc});
        }
    }

    void validateReachableStackDepths() {
        std::vector<ExecutionRange> ranges;
        if (!program_.instructions.empty()) {
            ranges.push_back(ExecutionRange{
                0, program_.instructions.size(), "top-level", 0,
                kNoPc});
        }
        addBoundaryExecutionRanges(ranges);
        ranges.insert(ranges.end(), anonymousRanges_.begin(),
                      anonymousRanges_.end());
        for (const auto& range : ranges) {
            validateStackRange(range);
        }
    }

    const BytecodeProgram& program_;
    const SemanticResult* semantic_ = nullptr;
    BytecodeValidationResult result_;
    std::vector<size_t> matchingBoundary_;
    std::vector<size_t> matchingFor_;
    std::vector<size_t> matchingSwitch_;
    std::vector<size_t> matchingTry_;
    std::vector<size_t> switchOwner_;
    std::vector<size_t> containerAt_;
    std::vector<size_t> anonymousOwnerAt_;
    std::vector<size_t> anonymousEndByOwner_;
    std::vector<std::vector<size_t>> expectedForAt_;
    std::vector<std::vector<size_t>> expectedSwitchAt_;
    std::vector<std::vector<size_t>> expectedTryAt_;
    std::vector<ExecutionRange> anonymousRanges_;
    bool boundaryPairsValid_ = true;
};

} // namespace

BytecodeValidationResult validateBytecodeProgram(
    const BytecodeProgram& program, const SemanticResult* semantic) {
    BytecodeValidator validator(program, semantic);
    return validator.validate();
}

} // namespace mparser
