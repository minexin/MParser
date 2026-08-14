#include "mparser/adaptive_bytecode_vm.h"
#include "mparser/bytecode.h"
#include "mparser/bytecode_region.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_fallback.h"
#include "mparser/runtime_value.h"
#include "mparser/semantic.h"
#include "mparser/typed_ir.h"
#include "mparser/typed_region_executor.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Compilation {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

Compilation compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.root);
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    return Compilation{std::move(semantic), std::move(bytecode)};
}

bool hasInvalidBytecodeDiagnostic(
    const std::vector<mparser::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.identifier ==
            mparser::kInvalidBytecodeProgramIdentifier) {
            return true;
        }
    }
    return false;
}

bool hasDiagnosticMessage(
    const std::vector<mparser::Diagnostic>& diagnostics,
    std::string_view fragment) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result,
    std::string_view name) {
    const auto variable = std::find_if(
        result.variables.begin(), result.variables.end(),
        [&](const mparser::RuntimeVariable& candidate) {
            return candidate.name == name;
        });
    return variable == result.variables.end()
               ? nullptr
               : &variable->value;
}

void assertSamePosition(const mparser::SourcePosition& first,
                        const mparser::SourcePosition& second) {
    assert(first.offset == second.offset);
    assert(first.line == second.line);
    assert(first.column == second.column);
    assert(first.sourceId == second.sourceId);
}

void assertInvalid(const mparser::BytecodeProgram& program) {
    const auto first = mparser::validateBytecodeProgram(program);
    const auto second = mparser::validateBytecodeProgram(program);
    assert(!first.succeeded);
    assert(!second.succeeded);
    assert(!first.diagnostics.empty());
    assert(first.diagnostics.size() == second.diagnostics.size());
    assert(hasInvalidBytecodeDiagnostic(first.diagnostics));
    for (size_t index = 0; index < first.diagnostics.size(); ++index) {
        const auto& left = first.diagnostics[index];
        const auto& right = second.diagnostics[index];
        assert(left.identifier == right.identifier);
        assert(left.message == right.message);
        assert(left.severity == right.severity);
        assertSamePosition(left.span.begin, right.span.begin);
        assertSamePosition(left.span.end, right.span.end);
    }
}

void runValidLoweringSmoke() {
    auto compiled = compile(R"(function y = verifier_case(x)
A = [1 2 3];
A(2) = x;
s.outer.inner(2) = x;
total = 0;
for i = 1:3
    if i < 2
        total = total + A(i);
    else
        total = total + i;
    end
end
switch x
    case 1
        total = total + 10;
    otherwise
        total = total + 20;
end
try
    value = A(x);
catch err
    value = 0;
end
offset = 4;
f = @(z) z + offset;
y = f(value) + total;
end
)");

    const auto validation = mparser::validateBytecodeProgram(
        compiled.bytecode, &compiled.semantic);
    assert(validation.succeeded);
    assert(validation.diagnostics.empty());
}

void runMultilineDelimitedLoweringSmoke() {
    auto compiled = compile(R"(matrix_value = [1 2
                3 4];
reshaped = reshape(...
    matrix_value, ...
    1, ...
    4);
total = sum(...
    abs(...
        reshaped), ...
    'all');
indexed = matrix_value(...
    2, ...
    1);
summary = total + indexed;
)");

    const auto validation = mparser::validateBytecodeProgram(
        compiled.bytecode, &compiled.semantic);
    assert(validation.succeeded);
    assert(validation.diagnostics.empty());

    mparser::BytecodeVm vm;
    const auto result = vm.run(compiled.bytecode, compiled.semantic);
    assert(result.diagnostics.empty());
    const auto* summary = findVariable(result, "summary");
    assert(summary != nullptr);
    assert(summary->kind == mparser::RuntimeValueKind::Number);
    assert(summary->number == 13.0);
}

void runStructuredTransferSmoke() {
    auto compiled = compile(R"(total = 0;
for i = 1:5
    try
        switch i
            case 2
                continue
            case 4
                break
            otherwise
                total = total + i;
        end
    catch err
        total = -100;
    end
end
after_for = total;

while_value = 0;
while true
    try
        switch while_value
            case 1
                break
            otherwise
                while_value = while_value + 1;
        end
    catch err
        while_value = -100;
    end
end
after_while = while_value;
)");

    const auto validation = mparser::validateBytecodeProgram(
        compiled.bytecode, &compiled.semantic);
    assert(validation.succeeded);
    assert(validation.diagnostics.empty());

    mparser::BytecodeVm vm;
    const auto result =
        vm.run(compiled.bytecode, compiled.semantic);
    assert(result.diagnostics.empty());
    const auto* afterFor = findVariable(result, "after_for");
    const auto* afterWhile =
        findVariable(result, "after_while");
    assert(afterFor != nullptr && afterFor->number == 4.0);
    assert(afterWhile != nullptr &&
           afterWhile->number == 1.0);
}

void runMetadataRejectionSmoke() {
    mparser::BytecodeProgram unknown;
    unknown.instructions.push_back(mparser::BytecodeInstruction{});
    assertInvalid(unknown);

    mparser::BytecodeProgram negativeResult;
    mparser::BytecodeInstruction call;
    call.op = mparser::BytecodeOp::CallOrIndex;
    call.resultCount = -1;
    negativeResult.instructions.push_back(std::move(call));
    assertInvalid(negativeResult);

    mparser::BytecodeProgram invalidImplicitOutput;
    mparser::BytecodeInstruction literal;
    literal.op = mparser::BytecodeOp::LoadLiteral;
    literal.operand = "1";
    literal.implicitExpressionOutput = true;
    invalidImplicitOutput.instructions.push_back(std::move(literal));
    assertInvalid(invalidImplicitOutput);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(invalidImplicitOutput).diagnostics,
        "implicit expression output requires a one-result CallOrIndex or "
        "LoadName"));

    mparser::BytecodeProgram duplicateCapture;
    mparser::BytecodeInstruction handle;
    handle.op = mparser::BytecodeOp::MakeFunctionHandle;
    handle.operand = "@()";
    handle.target = 2;
    handle.captureNames = {"value", "value"};
    duplicateCapture.instructions.push_back(std::move(handle));
    mparser::BytecodeInstruction body;
    body.op = mparser::BytecodeOp::LoadLiteral;
    body.operand = "1";
    duplicateCapture.instructions.push_back(std::move(body));
    assertInvalid(duplicateCapture);
}

void runControlStructureRejectionSmoke() {
    mparser::BytecodeProgram outOfBounds;
    mparser::BytecodeInstruction jump;
    jump.op = mparser::BytecodeOp::Jump;
    jump.target = 2;
    outOfBounds.instructions.push_back(std::move(jump));
    assertInvalid(outOfBounds);

    mparser::BytecodeProgram mismatchedBoundary;
    mparser::BytecodeInstruction enter;
    enter.op = mparser::BytecodeOp::EnterFunction;
    enter.operand = "f";
    mismatchedBoundary.instructions.push_back(std::move(enter));
    mparser::BytecodeInstruction leave;
    leave.op = mparser::BytecodeOp::LeaveClass;
    leave.operand = "f";
    mismatchedBoundary.instructions.push_back(std::move(leave));
    assertInvalid(mismatchedBoundary);

    mparser::BytecodeProgram crossBoundary;
    mparser::BytecodeInstruction functionEnter;
    functionEnter.op = mparser::BytecodeOp::EnterFunction;
    functionEnter.operand = "f";
    crossBoundary.instructions.push_back(std::move(functionEnter));
    mparser::BytecodeInstruction escapingJump;
    escapingJump.op = mparser::BytecodeOp::Jump;
    escapingJump.target = 3;
    crossBoundary.instructions.push_back(std::move(escapingJump));
    mparser::BytecodeInstruction functionLeave;
    functionLeave.op = mparser::BytecodeOp::LeaveFunction;
    functionLeave.operand = "f";
    crossBoundary.instructions.push_back(std::move(functionLeave));
    assertInvalid(crossBoundary);

    mparser::BytecodeProgram malformedFor;
    mparser::BytecodeInstruction range;
    range.op = mparser::BytecodeOp::LoadLiteral;
    range.operand = "1:2";
    malformedFor.instructions.push_back(std::move(range));
    mparser::BytecodeInstruction begin;
    begin.op = mparser::BytecodeOp::ForBegin;
    begin.operand = "i";
    begin.target = 3;
    malformedFor.instructions.push_back(std::move(begin));
    mparser::BytecodeInstruction next;
    next.op = mparser::BytecodeOp::ForNext;
    next.operand = "i";
    next.target = 0;
    malformedFor.instructions.push_back(std::move(next));
    assertInvalid(malformedFor);

    mparser::BytecodeProgram crossBoundarySwitch;
    mparser::BytecodeInstruction switchFunctionEnter;
    switchFunctionEnter.op = mparser::BytecodeOp::EnterFunction;
    switchFunctionEnter.operand = "f";
    crossBoundarySwitch.instructions.push_back(
        std::move(switchFunctionEnter));
    mparser::BytecodeInstruction switchValue;
    switchValue.op = mparser::BytecodeOp::LoadLiteral;
    switchValue.operand = "1";
    crossBoundarySwitch.instructions.push_back(std::move(switchValue));
    mparser::BytecodeInstruction switchBegin;
    switchBegin.op = mparser::BytecodeOp::SwitchBegin;
    crossBoundarySwitch.instructions.push_back(std::move(switchBegin));
    mparser::BytecodeInstruction switchFunctionLeave;
    switchFunctionLeave.op = mparser::BytecodeOp::LeaveFunction;
    switchFunctionLeave.operand = "f";
    crossBoundarySwitch.instructions.push_back(
        std::move(switchFunctionLeave));
    mparser::BytecodeInstruction switchEnd;
    switchEnd.op = mparser::BytecodeOp::SwitchEnd;
    crossBoundarySwitch.instructions.push_back(std::move(switchEnd));
    assertInvalid(crossBoundarySwitch);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(crossBoundarySwitch).diagnostics,
        "switch crosses an executable boundary"));

    mparser::BytecodeProgram enterAnonymousBody;
    mparser::BytecodeInstruction condition;
    condition.op = mparser::BytecodeOp::LoadLiteral;
    condition.operand = "true";
    enterAnonymousBody.instructions.push_back(std::move(condition));
    mparser::BytecodeInstruction branch;
    branch.op = mparser::BytecodeOp::JumpIfFalse;
    branch.target = 3;
    enterAnonymousBody.instructions.push_back(std::move(branch));
    mparser::BytecodeInstruction handle;
    handle.op = mparser::BytecodeOp::MakeFunctionHandle;
    handle.operand = "@()";
    handle.target = 4;
    enterAnonymousBody.instructions.push_back(std::move(handle));
    mparser::BytecodeInstruction body;
    body.op = mparser::BytecodeOp::LoadLiteral;
    body.operand = "1";
    enterAnonymousBody.instructions.push_back(std::move(body));
    mparser::BytecodeInstruction discard;
    discard.op = mparser::BytecodeOp::Pop;
    enterAnonymousBody.instructions.push_back(std::move(discard));
    assertInvalid(enterAnonymousBody);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(enterAnonymousBody).diagnostics,
        "crosses an executable boundary"));
}

void runContextAndStackRejectionSmoke() {
    mparser::BytecodeProgram missingIndexContext;
    mparser::BytecodeInstruction argument;
    argument.op = mparser::BytecodeOp::BeginIndexArgument;
    argument.operandCount = 0;
    missingIndexContext.instructions.push_back(std::move(argument));
    assertInvalid(missingIndexContext);

    mparser::BytecodeProgram incompleteLvalue;
    mparser::BytecodeInstruction lvalue;
    lvalue.op = mparser::BytecodeOp::BeginLvalue;
    lvalue.operand = "s";
    lvalue.operandCount = 2;
    incompleteLvalue.instructions.push_back(std::move(lvalue));
    mparser::BytecodeInstruction filler;
    filler.op = mparser::BytecodeOp::LoadLiteral;
    filler.operand = "1";
    incompleteLvalue.instructions.push_back(std::move(filler));
    assertInvalid(incompleteLvalue);

    mparser::BytecodeProgram underflow;
    mparser::BytecodeInstruction pop;
    pop.op = mparser::BytecodeOp::Pop;
    underflow.instructions.push_back(std::move(pop));
    assertInvalid(underflow);

    mparser::BytecodeProgram growingStackCycle;
    mparser::BytecodeInstruction cycleValue;
    cycleValue.op = mparser::BytecodeOp::LoadLiteral;
    cycleValue.operand = "1";
    growingStackCycle.instructions.push_back(std::move(cycleValue));
    mparser::BytecodeInstruction cycleJump;
    cycleJump.op = mparser::BytecodeOp::Jump;
    cycleJump.target = 0;
    growingStackCycle.instructions.push_back(std::move(cycleJump));
    assertInvalid(growingStackCycle);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(growingStackCycle).diagnostics,
        "inconsistent stack depths"));

    mparser::BytecodeProgram mismatchedMerge;
    mparser::BytecodeInstruction condition;
    condition.op = mparser::BytecodeOp::LoadLiteral;
    condition.operand = "true";
    mismatchedMerge.instructions.push_back(std::move(condition));
    mparser::BytecodeInstruction branch;
    branch.op = mparser::BytecodeOp::JumpIfFalse;
    branch.target = 3;
    mismatchedMerge.instructions.push_back(std::move(branch));
    mparser::BytecodeInstruction branchValue;
    branchValue.op = mparser::BytecodeOp::LoadLiteral;
    branchValue.operand = "1";
    mismatchedMerge.instructions.push_back(std::move(branchValue));
    mparser::BytecodeInstruction merge;
    merge.op = mparser::BytecodeOp::Return;
    mismatchedMerge.instructions.push_back(std::move(merge));
    assertInvalid(mismatchedMerge);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(mismatchedMerge).diagnostics,
        "inconsistent stack depths"));

    mparser::BytecodeProgram crossBoundaryIndex;
    mparser::BytecodeInstruction receiver;
    receiver.op = mparser::BytecodeOp::LoadName;
    receiver.operand = "x";
    crossBoundaryIndex.instructions.push_back(std::move(receiver));
    mparser::BytecodeInstruction beginIndex;
    beginIndex.op = mparser::BytecodeOp::BeginIndexContext;
    beginIndex.operandCount = 1;
    crossBoundaryIndex.instructions.push_back(std::move(beginIndex));
    mparser::BytecodeInstruction indexFunctionEnter;
    indexFunctionEnter.op = mparser::BytecodeOp::EnterFunction;
    indexFunctionEnter.operand = "f";
    crossBoundaryIndex.instructions.push_back(
        std::move(indexFunctionEnter));
    mparser::BytecodeInstruction indexArgument;
    indexArgument.op = mparser::BytecodeOp::BeginIndexArgument;
    indexArgument.operandCount = 0;
    crossBoundaryIndex.instructions.push_back(
        std::move(indexArgument));
    mparser::BytecodeInstruction indexValue;
    indexValue.op = mparser::BytecodeOp::LoadLiteral;
    indexValue.operand = "1";
    crossBoundaryIndex.instructions.push_back(std::move(indexValue));
    mparser::BytecodeInstruction discardIndexValue;
    discardIndexValue.op = mparser::BytecodeOp::Pop;
    crossBoundaryIndex.instructions.push_back(
        std::move(discardIndexValue));
    mparser::BytecodeInstruction indexFunctionLeave;
    indexFunctionLeave.op = mparser::BytecodeOp::LeaveFunction;
    indexFunctionLeave.operand = "f";
    crossBoundaryIndex.instructions.push_back(
        std::move(indexFunctionLeave));
    mparser::BytecodeInstruction outerIndexValue;
    outerIndexValue.op = mparser::BytecodeOp::LoadLiteral;
    outerIndexValue.operand = "1";
    crossBoundaryIndex.instructions.push_back(
        std::move(outerIndexValue));
    mparser::BytecodeInstruction indexCall;
    indexCall.op = mparser::BytecodeOp::CallOrIndex;
    indexCall.operandCount = 1;
    crossBoundaryIndex.instructions.push_back(std::move(indexCall));
    mparser::BytecodeInstruction discardIndexResult;
    discardIndexResult.op = mparser::BytecodeOp::Pop;
    crossBoundaryIndex.instructions.push_back(
        std::move(discardIndexResult));
    assertInvalid(crossBoundaryIndex);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(crossBoundaryIndex).diagnostics,
        "index argument crosses an executable boundary"));

    mparser::BytecodeProgram bypassIndexContext;
    mparser::BytecodeInstruction indexedValue;
    indexedValue.op = mparser::BytecodeOp::LoadName;
    indexedValue.operand = "x";
    bypassIndexContext.instructions.push_back(std::move(indexedValue));
    mparser::BytecodeInstruction indexCondition;
    indexCondition.op = mparser::BytecodeOp::LoadLiteral;
    indexCondition.operand = "true";
    bypassIndexContext.instructions.push_back(
        std::move(indexCondition));
    mparser::BytecodeInstruction indexBranch;
    indexBranch.op = mparser::BytecodeOp::JumpIfFalse;
    indexBranch.target = 4;
    bypassIndexContext.instructions.push_back(std::move(indexBranch));
    mparser::BytecodeInstruction context;
    context.op = mparser::BytecodeOp::BeginIndexContext;
    context.operandCount = 0;
    bypassIndexContext.instructions.push_back(std::move(context));
    mparser::BytecodeInstruction call;
    call.op = mparser::BytecodeOp::CallOrIndex;
    call.operandCount = 0;
    bypassIndexContext.instructions.push_back(std::move(call));
    mparser::BytecodeInstruction discardCall;
    discardCall.op = mparser::BytecodeOp::Pop;
    bypassIndexContext.instructions.push_back(std::move(discardCall));
    assertInvalid(bypassIndexContext);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(bypassIndexContext).diagnostics,
        "runtime context"));

    mparser::BytecodeProgram enterForBody;
    mparser::BytecodeInstruction forCondition;
    forCondition.op = mparser::BytecodeOp::LoadLiteral;
    forCondition.operand = "true";
    enterForBody.instructions.push_back(std::move(forCondition));
    mparser::BytecodeInstruction forBranch;
    forBranch.op = mparser::BytecodeOp::JumpIfFalse;
    forBranch.target = 4;
    enterForBody.instructions.push_back(std::move(forBranch));
    mparser::BytecodeInstruction range;
    range.op = mparser::BytecodeOp::LoadLiteral;
    range.operand = "1:2";
    enterForBody.instructions.push_back(std::move(range));
    mparser::BytecodeInstruction forBegin;
    forBegin.op = mparser::BytecodeOp::ForBegin;
    forBegin.operand = "i";
    forBegin.target = 7;
    enterForBody.instructions.push_back(std::move(forBegin));
    mparser::BytecodeInstruction loopValue;
    loopValue.op = mparser::BytecodeOp::LoadLiteral;
    loopValue.operand = "1";
    enterForBody.instructions.push_back(std::move(loopValue));
    mparser::BytecodeInstruction discardLoopValue;
    discardLoopValue.op = mparser::BytecodeOp::Pop;
    enterForBody.instructions.push_back(
        std::move(discardLoopValue));
    mparser::BytecodeInstruction forNext;
    forNext.op = mparser::BytecodeOp::ForNext;
    forNext.operand = "i";
    forNext.target = 4;
    enterForBody.instructions.push_back(std::move(forNext));
    assertInvalid(enterForBody);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(enterForBody).diagnostics,
        "structured runtime context"));

    mparser::BytecodeProgram enterSwitchArm;
    mparser::BytecodeInstruction switchCondition;
    switchCondition.op = mparser::BytecodeOp::LoadLiteral;
    switchCondition.operand = "true";
    enterSwitchArm.instructions.push_back(
        std::move(switchCondition));
    mparser::BytecodeInstruction switchBranch;
    switchBranch.op = mparser::BytecodeOp::JumpIfFalse;
    switchBranch.target = 4;
    enterSwitchArm.instructions.push_back(std::move(switchBranch));
    mparser::BytecodeInstruction selector;
    selector.op = mparser::BytecodeOp::LoadLiteral;
    selector.operand = "1";
    enterSwitchArm.instructions.push_back(std::move(selector));
    mparser::BytecodeInstruction switchBegin;
    switchBegin.op = mparser::BytecodeOp::SwitchBegin;
    enterSwitchArm.instructions.push_back(std::move(switchBegin));
    mparser::BytecodeInstruction otherwise;
    otherwise.op = mparser::BytecodeOp::SwitchOtherwise;
    otherwise.target = 6;
    enterSwitchArm.instructions.push_back(std::move(otherwise));
    mparser::BytecodeInstruction armExit;
    armExit.op = mparser::BytecodeOp::Jump;
    armExit.target = 6;
    enterSwitchArm.instructions.push_back(std::move(armExit));
    mparser::BytecodeInstruction switchEnd;
    switchEnd.op = mparser::BytecodeOp::SwitchEnd;
    enterSwitchArm.instructions.push_back(std::move(switchEnd));
    assertInvalid(enterSwitchArm);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(enterSwitchArm).diagnostics,
        "structured runtime context"));

    mparser::BytecodeProgram enterTryBody;
    mparser::BytecodeInstruction tryCondition;
    tryCondition.op = mparser::BytecodeOp::LoadLiteral;
    tryCondition.operand = "true";
    enterTryBody.instructions.push_back(std::move(tryCondition));
    mparser::BytecodeInstruction tryBranch;
    tryBranch.op = mparser::BytecodeOp::JumpIfFalse;
    tryBranch.target = 3;
    enterTryBody.instructions.push_back(std::move(tryBranch));
    mparser::BytecodeInstruction tryBegin;
    tryBegin.op = mparser::BytecodeOp::TryBegin;
    tryBegin.target = 6;
    enterTryBody.instructions.push_back(std::move(tryBegin));
    mparser::BytecodeInstruction tryValue;
    tryValue.op = mparser::BytecodeOp::LoadLiteral;
    tryValue.operand = "1";
    enterTryBody.instructions.push_back(std::move(tryValue));
    mparser::BytecodeInstruction discardTryValue;
    discardTryValue.op = mparser::BytecodeOp::Pop;
    enterTryBody.instructions.push_back(std::move(discardTryValue));
    mparser::BytecodeInstruction tryEnd;
    tryEnd.op = mparser::BytecodeOp::TryEnd;
    tryEnd.target = 8;
    enterTryBody.instructions.push_back(std::move(tryEnd));
    mparser::BytecodeInstruction catchValue;
    catchValue.op = mparser::BytecodeOp::LoadLiteral;
    catchValue.operand = "2";
    enterTryBody.instructions.push_back(std::move(catchValue));
    mparser::BytecodeInstruction discardCatchValue;
    discardCatchValue.op = mparser::BytecodeOp::Pop;
    enterTryBody.instructions.push_back(
        std::move(discardCatchValue));
    assertInvalid(enterTryBody);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(enterTryBody).diagnostics,
        "structured runtime context"));

    mparser::BytecodeProgram leakingTopLevelValue;
    mparser::BytecodeInstruction leakedValue;
    leakedValue.op = mparser::BytecodeOp::LoadLiteral;
    leakedValue.operand = "1";
    leakingTopLevelValue.instructions.push_back(
        std::move(leakedValue));
    assertInvalid(leakingTopLevelValue);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(
            leakingTopLevelValue).diagnostics,
        "exits with stack depth"));

    mparser::BytecodeProgram leakingFunctionValue;
    mparser::BytecodeInstruction enterFunction;
    enterFunction.op = mparser::BytecodeOp::EnterFunction;
    enterFunction.operand = "f";
    leakingFunctionValue.instructions.push_back(
        std::move(enterFunction));
    mparser::BytecodeInstruction functionValue;
    functionValue.op = mparser::BytecodeOp::LoadLiteral;
    functionValue.operand = "1";
    leakingFunctionValue.instructions.push_back(
        std::move(functionValue));
    mparser::BytecodeInstruction returnInstruction;
    returnInstruction.op = mparser::BytecodeOp::Return;
    leakingFunctionValue.instructions.push_back(
        std::move(returnInstruction));
    mparser::BytecodeInstruction leaveFunction;
    leaveFunction.op = mparser::BytecodeOp::LeaveFunction;
    leaveFunction.operand = "f";
    leakingFunctionValue.instructions.push_back(
        std::move(leaveFunction));
    assertInvalid(leakingFunctionValue);
    assert(hasDiagnosticMessage(
        mparser::validateBytecodeProgram(
            leakingFunctionValue).diagnostics,
        "Return leaves stack depth"));
}

void runDeterministicMutationCorpusSmoke() {
    auto compiled = compile(R"(function y = mutation_case(x)
A = [1 2 3];
for i = 1:3
    A(i) = A(i) + x;
end
f = @(value) value + A(1);
y = f(A(2));
end
)");
    assert(!compiled.bytecode.instructions.empty());

    for (size_t pc = 0;
         pc < compiled.bytecode.instructions.size(); ++pc) {
        auto invalidOpcode = compiled.bytecode;
        invalidOpcode.instructions[pc].op =
            mparser::BytecodeOp::Unknown;
        assertInvalid(invalidOpcode);

        auto invalidOperandCount = compiled.bytecode;
        invalidOperandCount.instructions[pc].operandCount = -1;
        assertInvalid(invalidOperandCount);

        auto invalidResultCount = compiled.bytecode;
        invalidResultCount.instructions[pc].resultCount = -1;
        assertInvalid(invalidResultCount);
    }

    mparser::BytecodeProgram cappedDiagnostics;
    cappedDiagnostics.instructions.resize(100);
    const auto validation =
        mparser::validateBytecodeProgram(cappedDiagnostics);
    assert(!validation.succeeded);
    assert(validation.diagnostics.size() == 64);
}

void runSemanticReferenceRejectionSmoke() {
    auto compiled = compile("x = 1;\n");
    assert(!compiled.bytecode.instructions.empty());
    auto& instruction = compiled.bytecode.instructions.front();
    instruction.binding.kind = mparser::BindingKind::LocalVariable;
    instruction.binding.symbolId =
        static_cast<int>(compiled.semantic.symbols.size() + 10);

    const auto validation = mparser::validateBytecodeProgram(
        compiled.bytecode, &compiled.semantic);
    assert(!validation.succeeded);
    assert(hasInvalidBytecodeDiagnostic(validation.diagnostics));
}

void runExecutionBoundarySmoke() {
    auto compiled = compile("x = 1;\ny = x + 2;\n");
    assert(compiled.bytecode.instructions.size() >= 2);
    compiled.bytecode.instructions[1].op =
        static_cast<mparser::BytecodeOp>(9999);

    mparser::BytecodeVm vm;
    const auto vmResult =
        vm.run(compiled.bytecode, compiled.semantic);
    assert(hasInvalidBytecodeDiagnostic(vmResult.diagnostics));
    assert(vmResult.executedInstructionCount == 0);
    assert(vmResult.variables.empty());

    mparser::AdaptiveBytecodeVmSession adaptive(
        compiled.bytecode, compiled.semantic);
    const auto adaptiveResult = adaptive.run();
    assert(hasInvalidBytecodeDiagnostic(
        adaptiveResult.runtime.diagnostics));
    assert(adaptiveResult.runtime.executedInstructionCount == 0);
    assert(!adaptiveResult.promotionOccurred);
    assert(!adaptive.hasTypedModule());

    mparser::BytecodeRegionAnalyzer analyzer;
    const auto region = analyzer.analyze(
        compiled.bytecode, "hot-loop", 0, "i");
    assert(!region.available);
    assert(!region.eligibleForTypedExecution);
    assert(region.fallbackKind ==
           mparser::RuntimeFallbackKind::InvalidContract);

    mparser::ScalarTypedRegionExecutor executor;
    const auto typed = executor.execute(
        compiled.bytecode, region,
        mparser::makeRuntimeNumberValue(1.0), {});
    assert(typed.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(typed.fallbackKind ==
           mparser::RuntimeFallbackKind::InvalidContract);
}

void runImmutableAdaptiveSnapshotSmoke() {
    auto compiled = compile("x = 1;\n");
    mparser::AdaptiveBytecodeVmSession session(
        compiled.bytecode, compiled.semantic);

    compiled.bytecode.instructions.front().op =
        static_cast<mparser::BytecodeOp>(9999);
    compiled.semantic.root.reset();
    compiled.semantic.symbols.clear();

    const auto result = session.run();
    assert(!hasInvalidBytecodeDiagnostic(
        result.runtime.diagnostics));
    const auto* value = findVariable(result.runtime, "x");
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(value->number == 1.0);
}

void runTypedContractBindingSmoke() {
    auto compiled = compile(R"(total = 0;
for i = 1:3
    total = total + i;
end
)");
    mparser::BytecodeOptimizationPlanner planner;
    mparser::BytecodeTypedIrBuilder builder;
    auto typed = builder.build(
        planner.planStaticLoops(compiled.bytecode));
    assert(typed.regions.size() == 1);
    typed.regions.front().region.endPc =
        typed.regions.front().region.beginPc + 1;

    mparser::BytecodeVmOptions options;
    options.profiling =
        mparser::BytecodeVmProfilingMode::Disabled;
    options.typedRegionBackend =
        mparser::TypedRegionBackend::Portable;
    mparser::BytecodeVm vm;
    const auto result = vm.run(
        compiled.bytecode, compiled.semantic, typed, options);
    assert(result.diagnostics.empty());
    const auto* total = findVariable(result, "total");
    assert(total != nullptr);
    assert(total->kind == mparser::RuntimeValueKind::Number);
    assert(total->number == 6.0);
    assert(result.typedRegionExecutions.size() == 1);
    assert(!result.typedRegionExecutions.front().eligible);
    assert(result.typedRegionExecutions.front().executionCount == 0);
    assert(result.typedRegionExecutions.front().lastFallbackKind ==
           mparser::RuntimeFallbackKind::InvalidContract);

    mparser::ScalarTypedRegionExecutor executor;
    const auto direct = executor.execute(
        compiled.bytecode, typed.regions.front().region,
        mparser::makeRuntimeNumberValue(1.0), {});
    assert(direct.status ==
           mparser::TypedRegionExecutionStatus::Fallback);
    assert(direct.fallbackKind ==
           mparser::RuntimeFallbackKind::InvalidContract);
}

} // namespace

int main() {
    runValidLoweringSmoke();
    runMultilineDelimitedLoweringSmoke();
    runStructuredTransferSmoke();
    runMetadataRejectionSmoke();
    runControlStructureRejectionSmoke();
    runContextAndStackRejectionSmoke();
    runDeterministicMutationCorpusSmoke();
    runSemanticReferenceRejectionSmoke();
    runExecutionBoundarySmoke();
    runImmutableAdaptiveSnapshotSmoke();
    runTypedContractBindingSmoke();
    std::cout << "bytecode verifier smoke tests passed\n";
    return 0;
}
