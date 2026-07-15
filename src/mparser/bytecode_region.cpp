#include "mparser/bytecode_region.h"
#include "mparser/runtime_math.h"

#include <cerrno>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

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

void copySet(const std::set<std::string>& source,
             std::vector<std::string>& destination) {
    destination.assign(source.begin(), source.end());
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
    if (contract.hasMutation) {
        return "region contains indexed or member mutation";
    }
    if (contract.hasUnsupportedControlFlow) {
        return "region contains unsupported control flow";
    }
    if (contract.hasUnsupportedOperations) {
        return "region contains unsupported bytecode operations";
    }
    return "eligible closed scalar loop region";
}

void analyzeInstruction(const BytecodeInstruction& instruction, size_t pc,
                        size_t loopBeginPc, size_t loopLatchPc,
                        std::set<std::string>& definitions,
                        std::set<std::string>& reads,
                        std::set<std::string>& inputs,
                        std::set<std::string>& writes,
                        std::set<std::string>& outputs,
                        std::set<std::string>& calls,
                        BytecodeRegionContract& contract) {
    switch (instruction.op) {
    case BytecodeOp::ForBegin:
        if (pc != loopBeginPc) {
            contract.hasUnsupportedControlFlow = true;
        }
        if (!instruction.operand.empty()) {
            definitions.insert(instruction.operand);
            writes.insert(instruction.operand);
            outputs.insert(instruction.operand);
        }
        break;
    case BytecodeOp::ForNext:
        if (pc != loopLatchPc) {
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
        if (!definitions.contains(instruction.operand)) {
            inputs.insert(instruction.operand);
        }
        break;
    case BytecodeOp::StoreName:
        if (instruction.operand.empty()) {
            contract.hasUnsupportedOperations = true;
            break;
        }
        definitions.insert(instruction.operand);
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
        if (!isScalarBinary(instruction.operand)) {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::PostfixOp:
        if (instruction.operand != "'") {
            contract.hasUnsupportedOperations = true;
        }
        break;
    case BytecodeOp::CallOrIndex:
        if (instruction.binding.kind == BindingKind::Builtin &&
            instruction.operandCount == 1 &&
            instruction.resultCount == 1 &&
            isRuntimePureUnaryMathBuiltin(instruction.calleeName)) {
            calls.insert(instruction.calleeName);
            break;
        }
        contract.hasCalls = true;
        break;
    case BytecodeOp::CallSuperclass:
        contract.hasCalls = true;
        break;
    case BytecodeOp::StoreMember:
    case BytecodeOp::StoreIndex:
        contract.hasMutation = true;
        if (!instruction.operand.empty()) {
            writes.insert(instruction.operand);
            outputs.insert(instruction.operand);
        }
        break;
    case BytecodeOp::Jump:
    case BytecodeOp::JumpIfFalse:
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
    case BytecodeOp::BeginIndexContext:
    case BytecodeOp::BeginIndexArgument:
    case BytecodeOp::MemberAccess:
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
    case BytecodeOp::EnterFunction:
    case BytecodeOp::LeaveFunction:
    case BytecodeOp::Unknown:
        contract.hasUnsupportedOperations = true;
        break;
    }
}

BytecodeRegionContract analyzePointRegion(const BytecodeProgram& program,
                                          size_t sourcePc,
                                          std::string_view target) {
    BytecodeRegionContract contract;
    if (sourcePc >= program.instructions.size()) {
        contract.reason = rejectionReason(contract);
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
    }
    if (instruction.op == BytecodeOp::CallOrIndex ||
        instruction.op == BytecodeOp::CallSuperclass) {
        contract.hasCalls = true;
        contract.callTargets.push_back(std::string(target));
    }
    contract.reason = rejectionReason(contract);
    return contract;
}

BytecodeRegionContract analyzeLoopRegion(const BytecodeProgram& program,
                                         size_t sourcePc) {
    BytecodeRegionContract contract;
    if (sourcePc >= program.instructions.size()) {
        contract.reason = rejectionReason(contract);
        return contract;
    }

    const auto& header = program.instructions[sourcePc];
    if (header.op != BytecodeOp::ForBegin || header.target < 0) {
        contract.reason = rejectionReason(contract);
        return contract;
    }
    const size_t exitPc = static_cast<size_t>(header.target);
    if (exitPc <= sourcePc + 1 || exitPc > program.instructions.size()) {
        contract.reason = rejectionReason(contract);
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
        contract.reason = rejectionReason(contract);
        return contract;
    }

    contract.available = true;
    contract.closed = true;
    contract.beginPc = sourcePc;
    contract.endPc = exitPc;
    contract.bodyBeginPc = bodyBeginPc;
    contract.bodyEndPc = latchPc;
    contract.stackInputCount = 1;

    std::set<std::string> definitions;
    std::set<std::string> reads;
    std::set<std::string> inputs;
    std::set<std::string> writes;
    std::set<std::string> outputs;
    std::set<std::string> calls;
    for (size_t pc = sourcePc; pc < exitPc; ++pc) {
        analyzeInstruction(program.instructions[pc], pc, sourcePc, latchPc,
                           definitions, reads, inputs, writes, outputs, calls,
                           contract);
    }

    copySet(reads, contract.reads);
    copySet(inputs, contract.inputs);
    copySet(writes, contract.writes);
    copySet(outputs, contract.outputs);
    copySet(calls, contract.callTargets);
    contract.eligibleForTypedExecution =
        !contract.hasCalls && !contract.hasMutation &&
        !contract.hasUnsupportedControlFlow &&
        !contract.hasUnsupportedOperations;
    contract.reason = rejectionReason(contract);
    return contract;
}

} // namespace

BytecodeRegionContract BytecodeRegionAnalyzer::analyze(
    const BytecodeProgram& program, std::string_view candidateKind,
    size_t sourcePc, std::string_view target) const {
    if (candidateKind == "hot-loop") {
        return analyzeLoopRegion(program, sourcePc);
    }
    return analyzePointRegion(program, sourcePc, target);
}

} // namespace mparser
