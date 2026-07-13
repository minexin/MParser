#pragma once

#include "mparser/diagnostic.h"
#include "mparser/semantic.h"
#include "mparser/source.h"

#include <string>
#include <vector>

namespace mparser {

enum class BytecodeOp {
    EnterModule,
    LeaveModule,
    EnterClass,
    LeaveClass,
    EnterPropertyInitializer,
    LeavePropertyInitializer,
    EnterFunction,
    LeaveFunction,
    EnterControl,
    LeaveControl,
    ControlHeader,
    ControlArm,
    SwitchBegin,
    SwitchCase,
    SwitchOtherwise,
    SwitchEnd,
    TryBegin,
    TryEnd,
    Jump,
    JumpIfFalse,
    Break,
    Continue,
    Return,
    ForBegin,
    ForNext,
    Pop,
    BeginIndexContext,
    BeginIndexArgument,
    LoadName,
    LoadLiteral,
    StoreName,
    StoreMember,
    StoreIndex,
    StoreBraceIndex,
    UnaryOp,
    BinaryOp,
    PostfixOp,
    MemberAccess,
    CallOrIndex,
    CallSuperclass,
    BraceIndex,
    MakeMatrix,
    MakeMatrixRow,
    MakeCell,
    MakeFunctionHandle,
    LoadMetaClass,
    Unknown,
};

struct BytecodeInstruction {
    BytecodeOp op = BytecodeOp::Unknown;
    std::string operand;
    std::string receiverName;
    BindingRef binding;
    SourceSpan span;
    int operandCount = 0;
    int target = -1;
    int resultCount = 1;
};

struct BytecodeProgram {
    std::vector<BytecodeInstruction> instructions;
    std::vector<Diagnostic> diagnostics;
};

class BytecodeLowerer {
public:
    BytecodeProgram lower(const SemanticResult& semantic);
};

const char* bytecodeOpName(BytecodeOp op);

} // namespace mparser
