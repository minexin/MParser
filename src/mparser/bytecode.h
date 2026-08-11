#pragma once

#include "mparser/diagnostic.h"
#include "mparser/semantic.h"
#include "mparser/source.h"

#include <string>
#include <string_view>
#include <vector>

namespace mparser {

enum class BytecodeOp {
    EnterModule,
    LeaveModule,
    EnterClass,
    LeaveClass,
    EnterPropertyInitializer,
    LeavePropertyInitializer,
    EnterEnumerationMemberInitializer,
    LeaveEnumerationMemberInitializer,
    EnterArgumentDefault,
    LeaveArgumentDefault,
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
    CaptureExpression,
    Pop,
    DeclareGlobal,
    DeclarePersistent,
    BeginIndexContext,
    BeginIndexArgument,
    BeginLvalue,
    BeginLvalueIndexContext,
    LvalueDescendMember,
    LvalueDescendIndex,
    LvalueDescendBrace,
    LoadName,
    LoadLiteral,
    StoreName,
    StoreMember,
    StoreIndex,
    StoreBraceIndex,
    StorePathMember,
    StorePathIndex,
    StorePathBrace,
    UnaryOp,
    BinaryOp,
    PostfixOp,
    MemberAccess,
    MakeNameValueArgument,
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
    std::vector<std::string> parameters;
    std::vector<std::string> captureNames;
    BindingRef binding;
    BindingRef receiverBinding;
    SourceSpan span;
    int operandCount = 0;
    int target = -1;
    int resultCount = 1;
    std::string calleeName;
    bool nullAssignment = false;
    std::vector<bool> colonSubscripts;
    bool nondeterministicAssignment = false;
    bool outputSuppressed = false;
    bool implicitExpressionOutput = false;
};

struct BytecodeProgram {
    std::vector<BytecodeInstruction> instructions;
    std::vector<Diagnostic> diagnostics;
};

struct BytecodeValidationResult {
    bool succeeded = true;
    std::vector<Diagnostic> diagnostics;
};

inline constexpr std::string_view
    kInvalidBytecodeProgramIdentifier =
        "MParser:Bytecode:InvalidProgram";

class BytecodeLowerer {
public:
    BytecodeProgram lower(const SemanticResult& semantic);
};

BytecodeValidationResult validateBytecodeProgram(
    const BytecodeProgram& program,
    const SemanticResult* semantic = nullptr);

const char* bytecodeOpName(BytecodeOp op);

} // namespace mparser
