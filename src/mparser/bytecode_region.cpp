#include "mparser/bytecode_region.h"
#include "mparser/builtin_registry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

bool isVariableBinding(BindingKind kind) {
    return kind == BindingKind::LocalVariable ||
           kind == BindingKind::FunctionParameter ||
           kind == BindingKind::FunctionOutput ||
           kind == BindingKind::Property ||
           kind == BindingKind::Unresolved;
}

bool isCallableBinding(BindingKind kind) {
    return kind == BindingKind::Function || kind == BindingKind::Method ||
           kind == BindingKind::Builtin || kind == BindingKind::Class;
}

bool isNumericLiteral(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    const std::string value(text);
    char* end = nullptr;
    errno = 0;
    (void)std::strtod(value.c_str(), &end);
    return errno != ERANGE && end == value.c_str() + value.size();
}

bool isScalarUnary(std::string_view operation) {
    return operation == "+" || operation == "-" || operation == "~";
}

bool isScalarBinary(std::string_view operation) {
    static const std::set<std::string_view> supported{
        "+",  "-",  "*",  "/",  "^",  ".*", "./", ".^",
        "<",  "<=", ">",  ">=", "==", "~=",
        "&&", "||", "&",  "|"};
    return supported.contains(operation);
}

struct LoopBoundary {
    size_t beginPc = 0;
    size_t latchPc = 0;
    size_t exitPc = 0;
    std::string variable;
};

struct LoopStructure {
    bool valid = true;
    std::set<size_t> beginPcs;
    std::set<size_t> latchPcs;
    std::vector<size_t> depthAtPc;
    size_t maxDepth = 0;
};

LoopStructure collectLoopStructure(const BytecodeProgram& program,
                                   size_t sourcePc, size_t exitPc) {
    LoopStructure structure;
    structure.depthAtPc.resize(program.instructions.size() + 1, 0);
    std::vector<LoopBoundary> stack;
    for (size_t pc = sourcePc; pc < exitPc; ++pc) {
        structure.depthAtPc[pc] = stack.size();
        const auto& instruction = program.instructions[pc];
        if (instruction.op == BytecodeOp::ForBegin) {
            if (instruction.target < 0) {
                structure.valid = false;
                continue;
            }
            const size_t nestedExit =
                static_cast<size_t>(instruction.target);
            if (nestedExit <= pc + 1 || nestedExit > exitPc) {
                structure.valid = false;
                continue;
            }
            const size_t latchPc = nestedExit - 1;
            const auto& latch = program.instructions[latchPc];
            if (latch.op != BytecodeOp::ForNext ||
                latch.target != static_cast<int>(pc + 1) ||
                (!instruction.operand.empty() &&
                 latch.operand != instruction.operand) ||
                (!stack.empty() && nestedExit > stack.back().latchPc)) {
                structure.valid = false;
                continue;
            }
            structure.beginPcs.insert(pc);
            structure.latchPcs.insert(latchPc);
            stack.push_back(LoopBoundary{pc, latchPc, nestedExit,
                                         instruction.operand});
            structure.maxDepth =
                std::max(structure.maxDepth, stack.size());
            continue;
        }
        if (instruction.op != BytecodeOp::ForNext) {
            continue;
        }
        if (stack.empty() || stack.back().latchPc != pc ||
            instruction.target !=
                static_cast<int>(stack.back().beginPc + 1) ||
            (!stack.back().variable.empty() &&
             instruction.operand != stack.back().variable)) {
            structure.valid = false;
            continue;
        }
        stack.pop_back();
    }
    structure.depthAtPc[exitPc] = stack.size();
    if (!stack.empty() || !structure.beginPcs.contains(sourcePc) ||
        !structure.latchPcs.contains(exitPc - 1)) {
        structure.valid = false;
    }
    return structure;
}

bool isStructuredForwardJump(const BytecodeInstruction& instruction,
                             size_t pc, size_t exitPc,
                             const LoopStructure& loops) {
    if (instruction.target < 0) {
        return false;
    }
    const size_t target = static_cast<size_t>(instruction.target);
    return target > pc && target <= exitPc &&
           target < loops.depthAtPc.size() &&
           loops.depthAtPc[pc] == loops.depthAtPc[target];
}

void copySet(const std::set<std::string>& source,
             std::vector<std::string>& destination) {
    destination.assign(source.begin(), source.end());
}

RuntimeFallbackKind rejectionKind(
    const BytecodeRegionContract& contract) {
    if (!contract.available) {
        return RuntimeFallbackKind::RegionUnavailable;
    }
    if (!contract.closed) {
        return RuntimeFallbackKind::RegionNotClosed;
    }
    if (contract.hasCalls) {
        return RuntimeFallbackKind::ContainsCall;
    }
    if (contract.hasUnsupportedMutation) {
        return RuntimeFallbackKind::UnsupportedMutation;
    }
    if (contract.hasUnsupportedControlFlow) {
        return RuntimeFallbackKind::UnsupportedControlFlow;
    }
    if (contract.hasUnsupportedOperations) {
        return RuntimeFallbackKind::UnsupportedOperation;
    }
    return RuntimeFallbackKind::None;
}

std::string rejectionReason(const BytecodeRegionContract& contract) {
    if (!contract.available) {
        return "region boundary is unavailable";
    }
    if (!contract.closed) {
        return "candidate is not a closed stack region";
    }
    if (contract.hasCalls) {
        return "region contains a call or dynamic index operation";
    }
    if (contract.hasUnsupportedMutation) {
        return "region contains unsupported indexed or member mutation";
    }
    if (contract.hasUnsupportedControlFlow) {
        return "region contains unsupported control flow";
    }
    if (contract.hasUnsupportedOperations) {
        return "region contains unsupported bytecode operations";
    }
    std::string reason = contract.nestedLoopCount > 0
                             ? "eligible closed nested scalar loop region"
                             : "eligible closed scalar loop region";
    if (contract.conditionalBranchCount > 0) {
        reason += " with structured branches";
    }
    if (contract.linearIndexReadCount > 0 ||
        contract.linearIndexWriteCount > 0) {
        reason += " with linear numeric indexing";
    }
    return reason;
}

void finalizeContract(BytecodeRegionContract& contract) {
    contract.fallbackKind = rejectionKind(contract);
    contract.reason = rejectionReason(contract);
}

void analyzeInstruction(const BytecodeInstruction& instruction, size_t pc,
                        size_t exitPc, const LoopStructure& loops,
                        const BuiltinRegistry& builtinRegistry,
                        std::set<std::string>& reads,
                        std::set<std::string>& writes,
                        std::set<std::string>& outputs,
                        std::set<std::string>& calls,
                        BytecodeRegionContract& contract) {
    switch (instruction.op) {
    case BytecodeOp::ForBegin:
        if (!loops.beginPcs.contains(pc)) {
            contract.hasUnsupportedControlFlow = true;
        }
        if (!isVariableBinding(instruction.binding.kind)) {
            contract.hasUnsupportedOperations = true;
        }
        if (!instruction.operand.empty()) {
            writes.insert(instruction.operand);
            outputs.insert(instruction.operand);
        }
        break;
    case BytecodeOp::ForNext:
        if (!loops.latchPcs.contains(pc)) {
            contract.hasUnsupportedControlFlow = true;
        }
        break;
    case BytecodeOp::LoadName:
        if (isCallableBinding(instruction.binding.kind)) {
            if (!instruction.operand.empty()) {
                calls.insert(instruction.operand);
            }
            break;
        }
        if (!isVariableBinding(instruction.binding.kind) ||
            instruction.operand.empty()) {
            contract.hasUnsupportedOperations = true;
            break;
        }
        reads.insert(instruction.operand);
        break;
    case BytecodeOp::StoreName:
        if (!isVariableBinding(instruction.binding.kind) ||
            instruction.operand.empty()) {
            contract.hasUnsupportedOperations = true;
            break;
        }
        writes.insert(instruction.operand);
        outputs.insert(instruction.operand);
        break;
    case BytecodeOp::LoadLiteral:
        if (!isNumericLiteral(instruction.operand)) {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::UnaryOp:
        if (!isScalarUnary(instruction.operand)) {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::BinaryOp:
        if (instruction.operand == ":") {
            if (instruction.operandCount != 2 &&
                instruction.operandCount != 3) {
                contract.hasUnsupportedOperations = true;
            }
        } else if (!isScalarBinary(instruction.operand)) {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::PostfixOp:
        if (instruction.operand != "'") {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::CallOrIndex:
        if (const BuiltinDescriptor* descriptor =
                builtinRegistry.find(instruction.calleeName);
            instruction.binding.kind == BindingKind::Builtin &&
            instruction.operandCount == 1 &&
            instruction.resultCount == 1 && descriptor &&
            descriptor->purity == BuiltinPurity::Pure &&
            descriptor->typedLowering != BuiltinTypedLowering::None) {
            calls.insert(instruction.calleeName);
            break;
        }
        if (!isCallableBinding(instruction.binding.kind) &&
            instruction.calleeName.empty() &&
            instruction.operandCount == 1 &&
            instruction.resultCount == 1 &&
            (instruction.colonSubscripts.empty() ||
             !instruction.colonSubscripts.front())) {
            ++contract.linearIndexReadCount;
            break;
        }
        contract.hasCalls = true;
        break;
    case BytecodeOp::CallSuperclass:
        contract.hasCalls = true;
        break;
    case BytecodeOp::StoreIndex:
        contract.hasMutation = true;
        if (!instruction.operand.empty()) {
            writes.insert(instruction.operand);
            outputs.insert(instruction.operand);
        }
        if (isVariableBinding(instruction.binding.kind) &&
            instruction.operandCount == 1 &&
            !instruction.nullAssignment &&
            !instruction.nondeterministicAssignment &&
            (instruction.colonSubscripts.empty() ||
             !instruction.colonSubscripts.front())) {
            ++contract.linearIndexWriteCount;
        } else {
            contract.hasUnsupportedMutation = true;
        }
        break;
    case BytecodeOp::StoreMember:
    case BytecodeOp::StoreBraceIndex:
    case BytecodeOp::StorePathMember:
    case BytecodeOp::StorePathIndex:
    case BytecodeOp::StorePathBrace:
        contract.hasMutation = true;
        contract.hasUnsupportedMutation = true;
        if (!instruction.operand.empty()) {
            writes.insert(instruction.operand);
            outputs.insert(instruction.operand);
        }
        break;
    case BytecodeOp::Jump:
        if (!isStructuredForwardJump(instruction, pc, exitPc, loops)) {
            contract.hasUnsupportedControlFlow = true;
        }
        break;
    case BytecodeOp::JumpIfFalse:
        ++contract.conditionalBranchCount;
        if (!isStructuredForwardJump(instruction, pc, exitPc, loops)) {
            contract.hasUnsupportedControlFlow = true;
        }
        break;
    case BytecodeOp::Break:
    case BytecodeOp::Continue:
    case BytecodeOp::Return:
    case BytecodeOp::SwitchBegin:
    case BytecodeOp::SwitchCase:
    case BytecodeOp::SwitchOtherwise:
    case BytecodeOp::SwitchEnd:
    case BytecodeOp::TryBegin:
    case BytecodeOp::TryEnd:
    case BytecodeOp::EnterControl:
    case BytecodeOp::LeaveControl:
    case BytecodeOp::ControlHeader:
    case BytecodeOp::ControlArm:
        contract.hasUnsupportedControlFlow = true;
        break;
    case BytecodeOp::Pop:
        break;
    case BytecodeOp::DeclareGlobal:
    case BytecodeOp::DeclarePersistent:
        contract.hasUnsupportedOperations = true;
        break;
    case BytecodeOp::BeginIndexContext:
        if (instruction.operandCount != 1) {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::BeginIndexArgument:
        break;
    case BytecodeOp::BeginLvalue:
    case BytecodeOp::BeginLvalueIndexContext:
    case BytecodeOp::LvalueDescendMember:
    case BytecodeOp::LvalueDescendIndex:
    case BytecodeOp::LvalueDescendBrace:
        contract.hasUnsupportedOperations = true;
        break;
    case BytecodeOp::MemberAccess:
    case BytecodeOp::MakeNameValueArgument:
    case BytecodeOp::BraceIndex:
    case BytecodeOp::MakeMatrix:
    case BytecodeOp::MakeMatrixRow:
    case BytecodeOp::MakeCell:
    case BytecodeOp::MakeFunctionHandle:
    case BytecodeOp::LoadMetaClass:
    case BytecodeOp::EnterModule:
    case BytecodeOp::LeaveModule:
    case BytecodeOp::EnterClass:
    case BytecodeOp::LeaveClass:
    case BytecodeOp::EnterPropertyInitializer:
    case BytecodeOp::LeavePropertyInitializer:
    case BytecodeOp::EnterEnumerationMemberInitializer:
    case BytecodeOp::LeaveEnumerationMemberInitializer:
    case BytecodeOp::EnterArgumentDefault:
    case BytecodeOp::LeaveArgumentDefault:
    case BytecodeOp::EnterFunction:
    case BytecodeOp::LeaveFunction:
    case BytecodeOp::Unknown:
        contract.hasUnsupportedOperations = true;
        break;
    }
}

using DefinitionSet = std::set<std::string>;

bool mergeDefinitions(std::optional<DefinitionSet>& destination,
                      const DefinitionSet& incoming) {
    if (!destination) {
        destination = incoming;
        return true;
    }

    bool changed = false;
    for (auto iterator = destination->begin();
         iterator != destination->end();) {
        if (incoming.contains(*iterator)) {
            ++iterator;
        } else {
            iterator = destination->erase(iterator);
            changed = true;
        }
    }
    return changed;
}

bool collectDefiniteInputs(const BytecodeProgram& program,
                           size_t sourcePc, size_t exitPc,
                           std::set<std::string>& inputs) {
    std::vector<std::optional<DefinitionSet>> states(
        exitPc - sourcePc + 1);
    std::deque<size_t> pending;
    states.front() = DefinitionSet{};
    pending.push_back(sourcePc);
    bool valid = true;

    const auto propagate = [&](size_t target,
                               const DefinitionSet& definitions) {
        if (target < sourcePc || target > exitPc) {
            valid = false;
            return;
        }
        auto& state = states[target - sourcePc];
        if (mergeDefinitions(state, definitions)) {
            pending.push_back(target);
        }
    };

    while (!pending.empty()) {
        const size_t pc = pending.front();
        pending.pop_front();
        if (pc == exitPc) {
            continue;
        }

        const auto& instruction = program.instructions[pc];
        DefinitionSet definitions = *states[pc - sourcePc];
        if (instruction.op == BytecodeOp::LoadName &&
            isVariableBinding(instruction.binding.kind) &&
            !instruction.operand.empty() &&
            !definitions.contains(instruction.operand)) {
            inputs.insert(instruction.operand);
        }
        if (instruction.op == BytecodeOp::StoreName &&
            !instruction.operand.empty()) {
            definitions.insert(instruction.operand);
        }

        switch (instruction.op) {
        case BytecodeOp::ForBegin: {
            if (instruction.target < 0) {
                valid = false;
                break;
            }
            propagate(static_cast<size_t>(instruction.target),
                      definitions);
            if (!instruction.operand.empty()) {
                definitions.insert(instruction.operand);
            }
            propagate(pc + 1, definitions);
            break;
        }
        case BytecodeOp::ForNext:
            if (instruction.target < 0) {
                valid = false;
                break;
            }
            propagate(static_cast<size_t>(instruction.target),
                      definitions);
            propagate(pc + 1, definitions);
            break;
        case BytecodeOp::Jump:
            if (instruction.target < 0) {
                valid = false;
            } else {
                propagate(static_cast<size_t>(instruction.target),
                          definitions);
            }
            break;
        case BytecodeOp::JumpIfFalse:
            if (instruction.target < 0) {
                valid = false;
            } else {
                propagate(static_cast<size_t>(instruction.target),
                          definitions);
                propagate(pc + 1, definitions);
            }
            break;
        case BytecodeOp::Return:
            break;
        default:
            propagate(pc + 1, definitions);
            break;
        }
    }
    return valid;
}

BytecodeRegionContract analyzePointRegion(const BytecodeProgram& program,
                                          size_t sourcePc,
                                          std::string_view target) {
    BytecodeRegionContract contract;
    if (sourcePc >= program.instructions.size()) {
        finalizeContract(contract);
        return contract;
    }

    contract.available = true;
    contract.beginPc = sourcePc;
    contract.endPc = sourcePc + 1;
    contract.bodyBeginPc = sourcePc;
    contract.bodyEndPc = sourcePc + 1;
    const auto& instruction = program.instructions[sourcePc];
    if (instruction.op == BytecodeOp::StoreName ||
        instruction.op == BytecodeOp::StoreIndex) {
        contract.writes.push_back(std::string(target));
        contract.outputs.push_back(std::string(target));
        contract.hasMutation = instruction.op == BytecodeOp::StoreIndex;
        contract.hasUnsupportedMutation =
            instruction.op == BytecodeOp::StoreIndex;
    }
    if (instruction.op == BytecodeOp::CallOrIndex ||
        instruction.op == BytecodeOp::CallSuperclass) {
        contract.hasCalls = true;
        contract.callTargets.push_back(std::string(target));
    }
    finalizeContract(contract);
    return contract;
}

BytecodeRegionContract analyzeLoopRegion(const BytecodeProgram& program,
                                         size_t sourcePc,
                                         const BuiltinRegistry&
                                             builtinRegistry) {
    BytecodeRegionContract contract;
    if (sourcePc >= program.instructions.size()) {
        finalizeContract(contract);
        return contract;
    }

    const auto& header = program.instructions[sourcePc];
    if (header.op != BytecodeOp::ForBegin || header.target < 0) {
        finalizeContract(contract);
        return contract;
    }
    const size_t exitPc = static_cast<size_t>(header.target);
    if (exitPc <= sourcePc + 1 || exitPc > program.instructions.size()) {
        finalizeContract(contract);
        return contract;
    }

    const size_t bodyBeginPc = sourcePc + 1;
    size_t latchPc = exitPc;
    for (size_t pc = exitPc; pc-- > bodyBeginPc;) {
        const auto& instruction = program.instructions[pc];
        if (instruction.op == BytecodeOp::ForNext &&
            instruction.target == static_cast<int>(bodyBeginPc) &&
            (header.operand.empty() ||
             instruction.operand == header.operand)) {
            latchPc = pc;
            break;
        }
    }
    if (latchPc == exitPc) {
        finalizeContract(contract);
        return contract;
    }

    contract.available = true;
    contract.closed = true;
    contract.beginPc = sourcePc;
    contract.endPc = exitPc;
    contract.bodyBeginPc = bodyBeginPc;
    contract.bodyEndPc = latchPc;
    contract.stackInputCount = 1;

    const auto loops = collectLoopStructure(program, sourcePc, exitPc);
    if (!loops.valid) {
        contract.hasUnsupportedControlFlow = true;
    }
    contract.nestedLoopCount =
        loops.beginPcs.empty() ? 0 : loops.beginPcs.size() - 1;
    contract.maxLoopDepth = loops.maxDepth;

    std::set<std::string> reads;
    std::set<std::string> inputs;
    std::set<std::string> writes;
    std::set<std::string> outputs;
    std::set<std::string> calls;
    for (size_t pc = sourcePc; pc < exitPc; ++pc) {
        analyzeInstruction(program.instructions[pc], pc, exitPc, loops,
                           builtinRegistry, reads, writes, outputs,
                           calls, contract);
    }
    if (!collectDefiniteInputs(program, sourcePc, exitPc, inputs)) {
        contract.hasUnsupportedControlFlow = true;
    }

    copySet(reads, contract.reads);
    copySet(inputs, contract.inputs);
    copySet(writes, contract.writes);
    copySet(outputs, contract.outputs);
    copySet(calls, contract.callTargets);
    contract.eligibleForTypedExecution =
        !contract.hasCalls && !contract.hasUnsupportedMutation &&
        !contract.hasUnsupportedControlFlow &&
        !contract.hasUnsupportedOperations;
    finalizeContract(contract);
    return contract;
}

} // namespace

bool bytecodeRegionContractsEquivalent(
    const BytecodeRegionContract& left,
    const BytecodeRegionContract& right) {
    return left.available == right.available &&
           left.closed == right.closed &&
           left.beginPc == right.beginPc &&
           left.endPc == right.endPc &&
           left.bodyBeginPc == right.bodyBeginPc &&
           left.bodyEndPc == right.bodyEndPc &&
           left.nestedLoopCount == right.nestedLoopCount &&
           left.maxLoopDepth == right.maxLoopDepth &&
           left.conditionalBranchCount ==
               right.conditionalBranchCount &&
           left.linearIndexReadCount == right.linearIndexReadCount &&
           left.linearIndexWriteCount == right.linearIndexWriteCount &&
           left.stackInputCount == right.stackInputCount &&
           left.stackOutputCount == right.stackOutputCount &&
           left.reads == right.reads &&
           left.inputs == right.inputs &&
           left.writes == right.writes &&
           left.outputs == right.outputs &&
           left.callTargets == right.callTargets &&
           left.hasCalls == right.hasCalls &&
           left.hasMutation == right.hasMutation &&
           left.hasUnsupportedMutation ==
               right.hasUnsupportedMutation &&
           left.hasUnsupportedControlFlow ==
               right.hasUnsupportedControlFlow &&
           left.hasUnsupportedOperations ==
               right.hasUnsupportedOperations &&
           left.eligibleForTypedExecution ==
               right.eligibleForTypedExecution &&
           left.fallbackKind == right.fallbackKind &&
           left.reason == right.reason;
}

BytecodeRegionAnalyzer::BytecodeRegionAnalyzer()
    : builtinRegistry_(defaultBuiltinRegistry()) {}

BytecodeRegionAnalyzer::BytecodeRegionAnalyzer(
    std::shared_ptr<const BuiltinRegistry> builtinRegistry)
    : builtinRegistry_(builtinRegistry
                           ? std::move(builtinRegistry)
                           : defaultBuiltinRegistry()) {}

BytecodeRegionContract BytecodeRegionAnalyzer::analyze(
    const BytecodeProgram& program, std::string_view candidateKind,
    size_t sourcePc, std::string_view target) const {
    const auto validation = validateBytecodeProgram(program);
    if (!validation.succeeded) {
        BytecodeRegionContract contract;
        contract.fallbackKind = RuntimeFallbackKind::InvalidContract;
        contract.reason = validation.diagnostics.empty()
                              ? "bytecode program validation failed"
                              : validation.diagnostics.front().message;
        return contract;
    }
    return analyzeValidated(program, candidateKind, sourcePc, target);
}

BytecodeRegionContract BytecodeRegionAnalyzer::analyzeValidated(
    const BytecodeProgram& program, std::string_view candidateKind,
    size_t sourcePc, std::string_view target) const {
    if (candidateKind == "hot-loop") {
        return analyzeLoopRegion(
            program, sourcePc, *builtinRegistry_);
    }
    return analyzePointRegion(program, sourcePc, target);
}

} // namespace mparser
