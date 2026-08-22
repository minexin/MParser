#include "mparser/execution/bytecode/bytecode.h"

#include "mparser/runtime/builtins/builtin_registry.h"
#include "mparser/semantic/function_signature.h"

#include <algorithm>
#include <cstddef>
#include <cctype>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

std::string trimAscii(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> parameterNames(std::string_view text) {
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

class BytecodeLoweringContext {
public:
    BytecodeProgram lower(const SemanticResult& semantic) {
        program_.diagnostics = semantic.diagnostics;
        builtinRegistry_ = semantic.builtinRegistry
                               ? semantic.builtinRegistry.get()
                               : defaultBuiltinRegistry().get();
        if (semantic.root) {
            collectFunctionSignatures(*semantic.root);
            lowerNode(*semantic.root);
        }
        return std::move(program_);
    }

private:
    struct LoopPatch {
        bool isFor = false;
        size_t continueTarget = 0;
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
    };

    size_t emit(BytecodeOp op, const HirNode& node, int operandCount = 0,
                int target = -1, int resultCount = 1) {
        program_.instructions.push_back(BytecodeInstruction{
            op, node.label.empty() ? node.raw : node.label, {}, {}, {},
            node.binding, {}, node.span, operandCount, target,
            resultCount, {}, false, {}, false});
        return program_.instructions.size() - 1;
    }

    void patchTarget(size_t instruction, size_t target) {
        program_.instructions[instruction].target =
            static_cast<int>(target);
    }

    void lowerNode(const HirNode& node) {
        switch (node.kind) {
        case HirKind::Module:
            emitBlock(BytecodeOp::EnterModule, BytecodeOp::LeaveModule, node);
            break;
        case HirKind::Class:
            emitBlock(BytecodeOp::EnterClass, BytecodeOp::LeaveClass, node);
            break;
        case HirKind::Function:
            emit(BytecodeOp::EnterFunction, node);
            ++functionDepth_;
            lowerChildren(node);
            --functionDepth_;
            emit(BytecodeOp::LeaveFunction, node);
            break;
        case HirKind::Import:
            break;
        case HirKind::GlobalDeclaration:
            for (const auto& child : node.children) {
                emit(BytecodeOp::DeclareGlobal, *child);
            }
            break;
        case HirKind::PersistentDeclaration:
            for (const auto& child : node.children) {
                emit(BytecodeOp::DeclarePersistent, *child);
            }
            break;
        case HirKind::ArgumentBlock:
            lowerChildren(node);
            break;
        case HirKind::Argument:
            if (node.property.hasExplicitDefault && !node.children.empty()) {
                const size_t enter = emit(BytecodeOp::EnterArgumentDefault, node);
                lowerExpression(*node.children.front());
                emit(BytecodeOp::LeaveArgumentDefault, node);
                patchTarget(enter, program_.instructions.size());
            }
            break;
        case HirKind::Control:
            lowerControl(node);
            break;
        case HirKind::ControlHeader:
            emit(BytecodeOp::ControlHeader, node);
            lowerChildren(node);
            break;
        case HirKind::ControlArm:
            emit(BytecodeOp::ControlArm, node);
            lowerChildren(node);
            break;
        case HirKind::Assignment:
            lowerAssignment(node);
            break;
        case HirKind::NameRef:
            emit(BytecodeOp::LoadName, node);
            break;
        case HirKind::Literal:
            emit(BytecodeOp::LoadLiteral, node);
            break;
        case HirKind::Unary:
            lowerChildren(node);
            emit(BytecodeOp::UnaryOp, node, childCount(node));
            break;
        case HirKind::Binary:
            if (node.label == ":") {
                emitColon(node);
            } else {
                lowerChildren(node);
                emit(BytecodeOp::BinaryOp, node, childCount(node));
            }
            break;
        case HirKind::Postfix:
            lowerChildren(node);
            emit(BytecodeOp::PostfixOp, node, childCount(node));
            break;
        case HirKind::Matrix:
            lowerChildren(node);
            emit(BytecodeOp::MakeMatrix, node, childCount(node));
            break;
        case HirKind::MatrixRow:
            lowerChildren(node);
            emit(BytecodeOp::MakeMatrixRow, node, childCount(node));
            break;
        case HirKind::Cell:
            lowerChildren(node);
            emit(BytecodeOp::MakeCell, node, childCount(node));
            break;
        case HirKind::CellRow:
            lowerChildren(node);
            emit(BytecodeOp::MakeCellRow, node, childCount(node));
            break;
        case HirKind::MemberAccess:
            lowerMemberAccess(node, 1);
            break;
        case HirKind::NameValueArgument:
            lowerChildren(node);
            emit(BytecodeOp::MakeNameValueArgument, node, childCount(node));
            break;
        case HirKind::CallOrIndex:
            lowerCallOrIndex(node, 1);
            break;
        case HirKind::SuperclassCall:
            lowerSuperclassCall(node, 1);
            break;
        case HirKind::BraceIndex:
            if (!node.children.empty()) {
                lowerNode(*node.children.front());
                lowerIndexArguments(node);
            }
            emit(BytecodeOp::BraceIndex, node, argumentCount(node));
            break;
        case HirKind::FunctionHandle:
            lowerFunctionHandle(node);
            break;
        case HirKind::MetaClass:
            emit(BytecodeOp::LoadMetaClass, node);
            break;
        case HirKind::OutputList:
        case HirKind::ParameterList:
        case HirKind::MethodPrototype:
            lowerChildren(node);
            break;
        case HirKind::Statement:
            if (lowerControlStatement(node)) {
                break;
            }
            if (node.raw.empty()) {
                lowerChildren(node);
                break;
            }
            if (node.children.size() == 1) {
                const auto& expression = *node.children.front();
                if (node.capturesExpressionResult && functionDepth_ == 0) {
                    if (!expressionProducesResult(expression)) {
                        lowerExpression(expression, 0);
                        break;
                    }
                    lowerExpression(expression, 1, true);
                    const size_t capture = emit(
                        BytecodeOp::CaptureExpression, node);
                    program_.instructions[capture].outputSuppressed =
                        node.outputSuppressed;
                    break;
                }
                if (expression.kind == HirKind::CallOrIndex ||
                    expression.kind == HirKind::SuperclassCall ||
                    expression.kind == HirKind::MemberAccess) {
                    lowerExpression(expression, 0);
                } else if (expression.kind == HirKind::NameRef &&
                           expression.binding.kind ==
                               BindingKind::Builtin) {
                    if (expressionProducesResult(expression)) {
                        lowerExpression(expression, 1, true);
                        emit(BytecodeOp::Pop, node);
                    } else {
                        lowerExpression(expression, 0);
                    }
                } else if (
                    expression.kind == HirKind::NameRef ||
                    expression.kind == HirKind::Literal ||
                    expression.kind == HirKind::Unary ||
                    expression.kind == HirKind::Binary ||
                    expression.kind == HirKind::Postfix ||
                    expression.kind == HirKind::Matrix ||
                    expression.kind == HirKind::MatrixRow ||
                    expression.kind == HirKind::Cell ||
                    expression.kind == HirKind::CellRow ||
                    expression.kind == HirKind::NameValueArgument ||
                    expression.kind == HirKind::BraceIndex ||
                    expression.kind == HirKind::FunctionHandle ||
                    expression.kind == HirKind::MetaClass ||
                    expression.kind == HirKind::Statement) {
                    lowerExpression(expression);
                    emit(BytecodeOp::Pop, node);
                } else {
                    lowerNode(expression);
                }
                break;
            }
            lowerChildren(node);
            break;
        case HirKind::Property:
            if (node.property.hasExplicitDefault) {
                emitBlock(BytecodeOp::EnterPropertyInitializer,
                          BytecodeOp::LeavePropertyInitializer, node);
            }
            break;
        case HirKind::Event:
            break;
        case HirKind::EnumerationMember:
            emit(BytecodeOp::EnterEnumerationMemberInitializer, node,
                 childCount(node));
            lowerChildren(node);
            emit(BytecodeOp::LeaveEnumerationMemberInitializer, node);
            break;
        case HirKind::Unknown:
            lowerChildren(node);
            break;
        }
    }

    void emitBlock(BytecodeOp enter, BytecodeOp leave, const HirNode& node) {
        emit(enter, node);
        lowerChildren(node);
        emit(leave, node);
    }

    void lowerFunctionHandle(const HirNode& node) {
        const size_t make = emit(BytecodeOp::MakeFunctionHandle, node);
        auto& instruction = program_.instructions[make];
        instruction.receiverName = node.lexicalClassName;
        instruction.calleeName = node.raw;

        if (node.label != "@()") {
            return;
        }
        if (!node.children.empty() &&
            node.children.front()->kind == HirKind::ParameterList) {
            instruction.parameters =
                parameterNames(node.children.front()->raw);
        }
        instruction.captureNames = anonymousFunctionCaptureNames(node);
        if (node.children.size() >= 2) {
            const size_t bodyBegin = program_.instructions.size();
            lowerExpression(*node.children[1]);
            if (program_.instructions.size() > bodyBegin &&
                (node.children[1]->kind == HirKind::CallOrIndex ||
                 node.children[1]->kind == HirKind::NameRef)) {
                program_.instructions.back().anonymousBodyOutput = true;
            }
        }
        patchTarget(make, program_.instructions.size());
    }

    bool lowerControlStatement(const HirNode& node) {
        if (node.label == "break") {
            const size_t index =
                emit(loopStack_.empty() || !loopStack_.back().isFor
                         ? BytecodeOp::Jump
                         : BytecodeOp::Break,
                     node);
            if (!loopStack_.empty()) {
                loopStack_.back().breaks.push_back(index);
            }
            return true;
        }
        if (node.label == "continue") {
            const size_t index = emit(BytecodeOp::Continue, node);
            if (!loopStack_.empty()) {
                loopStack_.back().continues.push_back(index);
            }
            return true;
        }
        if (node.label == "return") {
            emit(BytecodeOp::Return, node);
            return true;
        }
        return false;
    }

    void lowerControl(const HirNode& node) {
        if (node.label == "if") {
            lowerIf(node);
            return;
        }
        if (node.label == "while") {
            lowerWhile(node);
            return;
        }
        if (node.label == "for" || node.label == "parfor") {
            lowerFor(node);
            return;
        }
        if (node.label == "switch") {
            lowerSwitch(node);
            return;
        }
        if (node.label == "try") {
            lowerTry(node);
            return;
        }

        emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
    }

    void lowerIf(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
            return;
        }

        std::vector<size_t> arms;
        for (size_t index = 1; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm) {
                arms.push_back(index);
            }
        }

        std::vector<size_t> exits;
        const size_t firstArm =
            arms.empty() ? node.children.size() : arms.front();
        lowerHeader(*node.children.front());
        const size_t falseJump = emit(BytecodeOp::JumpIfFalse, node);
        lowerRange(node, 1, firstArm);
        exits.push_back(emit(BytecodeOp::Jump, node));

        size_t nextConditionJump = falseJump;
        for (size_t armIndex = 0; armIndex < arms.size(); ++armIndex) {
            const size_t current = arms[armIndex];
            const size_t next = armIndex + 1 < arms.size()
                                    ? arms[armIndex + 1]
                                    : node.children.size();
            patchTarget(nextConditionJump, program_.instructions.size());

            const HirNode& arm = *node.children[current];
            if (arm.label == "elseif" && !arm.children.empty()) {
                lowerHeader(*arm.children.front());
                nextConditionJump = emit(BytecodeOp::JumpIfFalse, arm);
                lowerRange(node, current + 1, next);
                exits.push_back(emit(BytecodeOp::Jump, arm));
                continue;
            }

            if (arm.label == "else") {
                lowerRange(node, current + 1, next);
                exits.push_back(emit(BytecodeOp::Jump, arm));
                nextConditionJump = static_cast<size_t>(-1);
                break;
            }

            nextConditionJump = static_cast<size_t>(-1);
        }

        const size_t after = program_.instructions.size();
        if (nextConditionJump != static_cast<size_t>(-1)) {
            patchTarget(nextConditionJump, after);
        }
        for (size_t jump : exits) {
            patchTarget(jump, after);
        }
    }

    void lowerWhile(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
            return;
        }

        const size_t condition = program_.instructions.size();
        lowerHeader(*node.children.front());
        const size_t exitJump = emit(BytecodeOp::JumpIfFalse, node);

        loopStack_.push_back(LoopPatch{false, condition, {}, {}});
        lowerRange(node, 1, node.children.size());
        LoopPatch loop = std::move(loopStack_.back());
        loopStack_.pop_back();

        emit(BytecodeOp::Jump, node, 0, static_cast<int>(condition));
        const size_t after = program_.instructions.size();
        patchTarget(exitJump, after);
        for (size_t jump : loop.breaks) {
            patchTarget(jump, after);
        }
        for (size_t jump : loop.continues) {
            patchTarget(jump, loop.continueTarget);
        }
    }

    void lowerFor(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader ||
            node.children.front()->children.empty() ||
            node.children.front()->children.front()->kind !=
                HirKind::Assignment) {
            emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
            return;
        }

        const HirNode& assignment = *node.children.front()->children.front();
        if (assignment.children.size() < 2 ||
            assignment.children.front()->kind != HirKind::NameRef) {
            emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
            return;
        }

        lowerNode(*assignment.children[1]);
        const size_t begin = emit(BytecodeOp::ForBegin, *assignment.children[0]);
        const size_t body = program_.instructions.size();

        loopStack_.push_back(LoopPatch{true, 0, {}, {}});
        lowerRange(node, 1, node.children.size());
        LoopPatch loop = std::move(loopStack_.back());
        loopStack_.pop_back();

        const size_t next =
            emit(BytecodeOp::ForNext, *assignment.children[0], 0,
                 static_cast<int>(body));
        const size_t after = program_.instructions.size();
        patchTarget(begin, after);
        for (size_t jump : loop.breaks) {
            patchTarget(jump, after);
        }
        for (size_t jump : loop.continues) {
            patchTarget(jump, next);
        }
    }

    void lowerSwitch(const HirNode& node) {
        if (node.children.empty() ||
            node.children.front()->kind != HirKind::ControlHeader) {
            emitBlock(BytecodeOp::EnterControl, BytecodeOp::LeaveControl, node);
            return;
        }

        lowerHeader(*node.children.front());
        emit(BytecodeOp::SwitchBegin, node);

        std::vector<size_t> exits;
        std::optional<size_t> otherwiseSkip;
        for (size_t index = 1; index < node.children.size(); ++index) {
            if (node.children[index]->kind != HirKind::ControlArm) {
                continue;
            }

            const HirNode& arm = *node.children[index];
            const size_t next = nextControlArm(node, index + 1);
            if (arm.label == "case") {
                if (arm.children.empty() ||
                    arm.children.front()->kind != HirKind::ControlHeader) {
                    emitBlock(BytecodeOp::EnterControl,
                              BytecodeOp::LeaveControl, node);
                    return;
                }

                lowerHeader(*arm.children.front());
                const size_t skip = emit(BytecodeOp::SwitchCase, arm);
                lowerRange(node, index + 1, next);
                exits.push_back(emit(BytecodeOp::Jump, arm));
                patchTarget(skip, program_.instructions.size());
                continue;
            }

            if (arm.label == "otherwise") {
                otherwiseSkip = emit(BytecodeOp::SwitchOtherwise, arm);
                lowerRange(node, index + 1, next);
                exits.push_back(emit(BytecodeOp::Jump, arm));
                break;
            }
        }

        const size_t switchEnd = program_.instructions.size();
        emit(BytecodeOp::SwitchEnd, node);
        const size_t after = program_.instructions.size();
        for (size_t jump : exits) {
            patchTarget(jump, switchEnd);
        }
        if (otherwiseSkip) {
            patchTarget(*otherwiseSkip, switchEnd);
        }
        (void)after;
    }

    void lowerTry(const HirNode& node) {
        const auto catchArm = findControlArm(node, "catch", 0);
        if (!catchArm) {
            const size_t bodyBegin =
                (!node.children.empty() &&
                 node.children.front()->kind == HirKind::ControlHeader)
                    ? 1
                    : 0;
            lowerRange(node, bodyBegin, node.children.size());
            return;
        }

        const size_t bodyBegin =
            (!node.children.empty() &&
             node.children.front()->kind == HirKind::ControlHeader)
                ? 1
                : 0;
        const size_t bodyEnd = *catchArm;
        const HirNode& arm = *node.children[*catchArm];
        const size_t begin = emit(BytecodeOp::TryBegin, arm);
        if (const auto name = catchVariableName(arm)) {
            program_.instructions[begin].operand = *name;
        } else {
            program_.instructions[begin].operand.clear();
        }

        lowerRange(node, bodyBegin, bodyEnd);
        const size_t success = emit(BytecodeOp::TryEnd, node);

        const size_t catchStart = program_.instructions.size();
        patchTarget(begin, catchStart);
        lowerRange(node, *catchArm + 1, nextControlArm(node, *catchArm + 1));

        const size_t after = program_.instructions.size();
        patchTarget(success, after);
    }

    void lowerHeader(const HirNode& header) {
        if (header.children.empty()) {
            return;
        }
        const HirNode& condition = *header.children.front();
        if (condition.kind == HirKind::Statement &&
            condition.children.size() == 1) {
            lowerExpression(*condition.children.front());
            return;
        }
        lowerExpression(condition);
    }

    std::optional<size_t> findControlArm(
        const HirNode& node, std::string_view label, size_t begin) const {
        for (size_t index = begin; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm &&
                node.children[index]->label == label) {
                return index;
            }
        }
        return std::nullopt;
    }

    size_t nextControlArm(const HirNode& node, size_t begin) const {
        for (size_t index = begin; index < node.children.size(); ++index) {
            if (node.children[index]->kind == HirKind::ControlArm) {
                return index;
            }
        }
        return node.children.size();
    }

    std::optional<std::string> catchVariableName(const HirNode& arm) const {
        if (arm.children.empty() ||
            arm.children.front()->kind != HirKind::ControlHeader) {
            return std::nullopt;
        }
        const HirNode& header = *arm.children.front();
        if (header.children.empty()) {
            return std::nullopt;
        }
        const HirNode& statement = *header.children.front();
        if (statement.children.size() == 1 &&
            statement.children.front()->kind == HirKind::NameRef) {
            return statement.children.front()->label;
        }
        if (statement.kind == HirKind::NameRef) {
            return statement.label;
        }
        return std::nullopt;
    }

    void lowerRange(const HirNode& node, size_t begin, size_t end) {
        for (size_t index = begin; index < end && index < node.children.size();
             ++index) {
            lowerNode(*node.children[index]);
        }
    }

    void lowerChildren(const HirNode& node) {
        for (const auto& child : node.children) {
            lowerNode(*child);
        }
    }

    size_t lowerColonTerms(const HirNode& node) {
        if (node.kind == HirKind::Binary && node.label == ":") {
            size_t count = 0;
            for (const auto& child : node.children) {
                count += lowerColonTerms(*child);
            }
            return count;
        }
        lowerNode(node);
        return 1;
    }

    void emitColon(const HirNode& node) {
        const size_t count = lowerColonTerms(node);
        emit(BytecodeOp::BinaryOp, node, static_cast<int>(count));
    }

    void lowerExpression(const HirNode& node, int resultCount = 1,
                         bool implicitExpressionOutput = false) {
        if (node.kind == HirKind::NameRef) {
            const size_t load = emit(BytecodeOp::LoadName, node);
            program_.instructions[load].resultCount = resultCount;
            program_.instructions[load].implicitExpressionOutput =
                implicitExpressionOutput;
            return;
        }
        if (node.kind == HirKind::CallOrIndex) {
            lowerCallOrIndex(node, resultCount,
                             implicitExpressionOutput);
            return;
        }
        if (node.kind == HirKind::SuperclassCall) {
            lowerSuperclassCall(node, resultCount);
            return;
        }
        if (node.kind == HirKind::MemberAccess) {
            lowerMemberAccess(node, resultCount);
            return;
        }
        if (node.kind == HirKind::BraceIndex) {
            if (!node.children.empty()) {
                lowerNode(*node.children.front());
                lowerIndexArguments(node);
            }
            emit(BytecodeOp::BraceIndex, node, argumentCount(node), -1,
                 resultCount);
            return;
        }
        lowerNode(node);
    }

    void lowerMemberAccess(const HirNode& node, int resultCount) {
        lowerChildren(node);
        emit(BytecodeOp::MemberAccess, node, childCount(node), -1,
             resultCount);
    }

    void lowerCallOrIndex(const HirNode& node, int resultCount,
                          bool implicitExpressionOutput = false) {
        if (node.children.empty()) {
            const size_t call =
                emit(BytecodeOp::CallOrIndex, node,
                     argumentCount(node), -1, resultCount);
            program_.instructions[call].implicitExpressionOutput =
                implicitExpressionOutput;
            return;
        }

        if (node.children.front()->kind == HirKind::NameRef) {
            const size_t callee = emit(
                BytecodeOp::LoadName, *node.children.front());
            program_.instructions[callee].calleeReference = true;
        } else {
            lowerNode(*node.children.front());
        }
        const bool hasIndexContext =
            !((node.binding.kind == BindingKind::Builtin ||
               node.binding.kind == BindingKind::Function) &&
              !argumentsRequireIndexContext(node));
        if (!hasIndexContext) {
            for (size_t index = 1; index < node.children.size(); ++index) {
                lowerNode(*node.children[index]);
            }
        } else {
            lowerIndexArguments(node);
        }
        const size_t call =
            emit(BytecodeOp::CallOrIndex, node, argumentCount(node), -1,
                 resultCount);
        program_.instructions[call].implicitExpressionOutput =
            implicitExpressionOutput;
        program_.instructions[call].hasIndexContext = hasIndexContext;
        attachColonSubscripts(call, node);
        if ((node.binding.kind == BindingKind::Builtin ||
             node.binding.kind == BindingKind::Function) &&
            node.children.front()->kind == HirKind::NameRef) {
            program_.instructions[call].calleeName =
                node.children.front()->label;
        }
    }

    void lowerSuperclassCall(const HirNode& node, int resultCount) {
        for (size_t index = 1; index < node.children.size(); ++index) {
            lowerNode(*node.children[index]);
        }
        const size_t call = emit(BytecodeOp::CallSuperclass, node,
                                 argumentCount(node), -1, resultCount);
        if (!node.children.empty()) {
            program_.instructions[call].receiverName =
                node.children.front()->label;
        }
    }

    void lowerIndexArguments(const HirNode& node) {
        emit(BytecodeOp::BeginIndexContext, node, argumentCount(node));
        for (size_t index = 1; index < node.children.size(); ++index) {
            emit(BytecodeOp::BeginIndexArgument, *node.children[index],
                 static_cast<int>(index - 1));
            lowerNode(*node.children[index]);
        }
    }

    static bool requiresIndexContext(const HirNode& node) {
        if (node.kind == HirKind::Literal &&
            (node.label == "end" || node.label == ":")) {
            return true;
        }
        if (node.kind == HirKind::CallOrIndex ||
            node.kind == HirKind::BraceIndex) {
            return false;
        }
        for (const auto& child : node.children) {
            if (requiresIndexContext(*child)) {
                return true;
            }
        }
        return false;
    }

    static bool argumentsRequireIndexContext(const HirNode& node) {
        for (size_t index = 1; index < node.children.size(); ++index) {
            if (requiresIndexContext(*node.children[index])) {
                return true;
            }
        }
        return false;
    }

    void lowerLvalueIndexArguments(const HirNode& node) {
        emit(BytecodeOp::BeginLvalueIndexContext, node,
             argumentCount(node));
        for (size_t index = 1; index < node.children.size(); ++index) {
            emit(BytecodeOp::BeginIndexArgument, *node.children[index],
                 static_cast<int>(index - 1));
            lowerNode(*node.children[index]);
        }
    }

    bool collectNestedLvaluePath(
        const HirNode& node, const HirNode*& root,
        std::vector<const HirNode*>& segments) const {
        const HirNode* current = &node;
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

    void attachColonSubscripts(size_t instructionIndex,
                               const HirNode& node) {
        auto& instruction = program_.instructions[instructionIndex];
        instruction.colonSubscripts.reserve(
            static_cast<size_t>(argumentCount(node)));
        for (size_t index = 1; index < node.children.size(); ++index) {
            const HirNode& subscript = *node.children[index];
            instruction.colonSubscripts.push_back(
                subscript.kind == HirKind::Literal &&
                subscript.label == ":");
        }
    }

    static std::string lvalueSeedKind(
        const std::vector<const HirNode*>& segments,
        size_t nextIndex) {
        const HirNode& nextSegment = *segments[nextIndex];
        if (nextSegment.kind == HirKind::BraceIndex) {
            return "cell";
        }
        if (nextSegment.kind == HirKind::CallOrIndex) {
            if (nextIndex + 1 < segments.size()) {
                const HirNode& selected = *segments[nextIndex + 1];
                if (selected.kind == HirKind::MemberAccess) {
                    return "struct";
                }
                if (selected.kind == HirKind::BraceIndex) {
                    return "cell";
                }
            }
            return "numeric";
        }
        return "struct";
    }

    void lowerLvalueDescent(
        const std::vector<const HirNode*>& segments, size_t index) {
        const HirNode& segment = *segments[index];
        const std::string seedKind =
            lvalueSeedKind(segments, index + 1);
        switch (segment.kind) {
        case HirKind::MemberAccess:
            if (segment.label == ".()" && segment.children.size() == 2) {
                lowerNode(*segment.children[1]);
            }
            {
                const size_t instruction = emit(
                    BytecodeOp::LvalueDescendMember, segment,
                    segment.label == ".()" ? 1 : 0);
                program_.instructions[instruction].receiverName = seedKind;
            }
            break;
        case HirKind::CallOrIndex:
            lowerLvalueIndexArguments(segment);
            {
                const size_t instruction = emit(
                    BytecodeOp::LvalueDescendIndex, segment,
                    argumentCount(segment));
                attachColonSubscripts(instruction, segment);
                program_.instructions[instruction].receiverName = seedKind;
            }
            break;
        case HirKind::BraceIndex:
            lowerLvalueIndexArguments(segment);
            {
                const size_t instruction = emit(
                    BytecodeOp::LvalueDescendBrace, segment,
                    argumentCount(segment));
                program_.instructions[instruction].receiverName = seedKind;
            }
            break;
        default:
            break;
        }
    }

    void lowerLvalueStore(const HirNode& segment, bool nullAssignment,
                          bool nondeterministicAssignment) {
        size_t store = 0;
        switch (segment.kind) {
        case HirKind::MemberAccess:
            if (segment.label == ".()" && segment.children.size() == 2) {
                lowerNode(*segment.children[1]);
            }
            store = emit(BytecodeOp::StorePathMember, segment,
                         segment.label == ".()" ? 1 : 0);
            break;
        case HirKind::CallOrIndex:
            lowerLvalueIndexArguments(segment);
            store = emit(BytecodeOp::StorePathIndex, segment,
                         argumentCount(segment));
            attachColonSubscripts(store, segment);
            break;
        case HirKind::BraceIndex:
            lowerLvalueIndexArguments(segment);
            store = emit(BytecodeOp::StorePathBrace, segment,
                         argumentCount(segment));
            break;
        default:
            return;
        }
        auto& instruction = program_.instructions[store];
        instruction.nullAssignment = nullAssignment;
        instruction.nondeterministicAssignment =
            nondeterministicAssignment;
    }

    bool lowerNestedLvalueTarget(const HirNode& node,
                                 bool nullAssignment,
                                 bool nondeterministicAssignment) {
        const HirNode* root = nullptr;
        std::vector<const HirNode*> segments;
        if (!collectNestedLvaluePath(node, root, segments)) {
            return false;
        }

        emit(BytecodeOp::BeginLvalue, *root,
             static_cast<int>(segments.size()));
        for (size_t index = 0; index + 1 < segments.size(); ++index) {
            lowerLvalueDescent(segments, index);
        }
        lowerLvalueStore(*segments.back(), nullAssignment,
                         nondeterministicAssignment);
        return true;
    }

    void lowerAssignment(const HirNode& node) {
        if (node.children.size() < 2) {
            lowerChildren(node);
            return;
        }

        const HirNode& target = *node.children.front();
        const bool nullAssignment =
            node.children.size() == 2 &&
            node.children[1]->kind == HirKind::Matrix &&
            node.children[1]->children.empty();
        const bool nondeterministicAssignment =
            node.children.size() == 2 &&
            containsBuiltin(*node.children[1], "toc");
        const int resultCount = target.kind == HirKind::OutputList
                                    ? childCount(target)
                                    : 1;
        for (size_t i = 1; i < node.children.size(); ++i) {
            lowerExpression(*node.children[i], resultCount);
        }
        lowerAssignmentTarget(target, nullAssignment,
                              nondeterministicAssignment);
    }

    void lowerAssignmentTarget(const HirNode& node,
                               bool nullAssignment = false,
                               bool nondeterministicAssignment = false) {
        if (lowerNestedLvalueTarget(node, nullAssignment,
                                    nondeterministicAssignment)) {
            return;
        }
        switch (node.kind) {
        case HirKind::NameRef:
            {
                const size_t store = emit(BytecodeOp::StoreName, node);
                program_.instructions[store].nondeterministicAssignment =
                    nondeterministicAssignment;
            }
            break;
        case HirKind::OutputList:
            for (auto it = node.children.rbegin();
                 it != node.children.rend(); ++it) {
                lowerAssignmentTarget(**it, false,
                                      nondeterministicAssignment);
            }
            break;
        case HirKind::Literal:
            if (node.label == "~") {
                emit(BytecodeOp::Pop, node);
                break;
            }
            lowerNode(node);
            break;
        case HirKind::MemberAccess:
        {
            const bool directVariableReceiver =
                !node.children.empty() &&
                node.children.front()->kind == HirKind::NameRef &&
                (node.children.front()->binding.kind ==
                     BindingKind::LocalVariable ||
                 node.children.front()->binding.kind ==
                     BindingKind::FunctionParameter ||
                 node.children.front()->binding.kind ==
                     BindingKind::FunctionOutput ||
                 node.children.front()->binding.kind ==
                     BindingKind::GlobalVariable ||
                 node.children.front()->binding.kind ==
                     BindingKind::PersistentVariable);
            if (!node.children.empty() && !directVariableReceiver) {
                lowerNode(*node.children.front());
            }
            if (node.label == ".()" && node.children.size() == 2) {
                lowerNode(*node.children[1]);
            }
            const size_t store =
                emit(BytecodeOp::StoreMember, node, childCount(node));
            if (directVariableReceiver) {
                program_.instructions[store].receiverName =
                    node.children.front()->label;
                program_.instructions[store].receiverBinding =
                    node.children.front()->binding;
            }
            break;
        }
        case HirKind::CallOrIndex:
            if (node.children.empty()) {
                const size_t store =
                    emit(BytecodeOp::StoreIndex, node, argumentCount(node));
                program_.instructions[store].nullAssignment = nullAssignment;
                break;
            }
            lowerNode(*node.children.front());
            lowerIndexArguments(node);
            {
                const size_t store =
                    emit(BytecodeOp::StoreIndex, *node.children.front(),
                         argumentCount(node));
                auto& instruction = program_.instructions[store];
                instruction.nullAssignment = nullAssignment;
                instruction.nondeterministicAssignment =
                    nondeterministicAssignment;
                instruction.colonSubscripts.reserve(
                    static_cast<size_t>(argumentCount(node)));
                for (size_t index = 1; index < node.children.size(); ++index) {
                    const HirNode& subscript = *node.children[index];
                    instruction.colonSubscripts.push_back(
                        subscript.kind == HirKind::Literal &&
                        subscript.label == ":");
                }
            }
            break;
        case HirKind::BraceIndex:
            if (node.children.empty()) {
                emit(BytecodeOp::StoreBraceIndex, node, argumentCount(node));
                break;
            }
            lowerNode(*node.children.front());
            lowerIndexArguments(node);
            emit(BytecodeOp::StoreBraceIndex, *node.children.front(),
                 argumentCount(node));
            break;
        default:
            lowerNode(node);
            break;
        }
    }

    static int childCount(const HirNode& node) {
        return static_cast<int>(node.children.size());
    }

    static int argumentCount(const HirNode& node) {
        const int count = childCount(node);
        return count > 0 ? count - 1 : 0;
    }

    static bool containsBuiltin(const HirNode& node,
                                std::string_view name) {
        if (node.kind == HirKind::NameRef &&
            node.binding.kind == BindingKind::Builtin &&
            node.label == name) {
            return true;
        }
        for (const auto& child : node.children) {
            if (containsBuiltin(*child, name)) {
                return true;
            }
        }
        return false;
    }

    void collectFunctionSignatures(const HirNode& node) {
        if (node.kind == HirKind::Function) {
            functionSignatures_[node.label] =
                parseFunctionSignature(node);
        }
        for (const auto& child : node.children) {
            collectFunctionSignatures(*child);
        }
    }

    bool expressionProducesResult(const HirNode& expression) const {
        if (expression.kind == HirKind::NameRef &&
            expression.binding.kind == BindingKind::Builtin &&
            builtinRegistry_) {
            const auto* descriptor =
                builtinRegistry_->find(expression.label);
            return !descriptor ||
                   descriptor->implicitOutputCount(0) != 0;
        }
        if (expression.kind != HirKind::CallOrIndex ||
            expression.children.empty()) {
            return true;
        }
        const HirNode& callee = *expression.children.front();
        if (expression.binding.kind == BindingKind::Builtin &&
            builtinRegistry_) {
            const auto* descriptor = builtinRegistry_->find(callee.label);
            return !descriptor ||
                   descriptor->implicitOutputCount(
                       static_cast<size_t>(argumentCount(expression))) != 0;
        }
        if (expression.binding.kind == BindingKind::Function) {
            const auto function = functionSignatures_.find(callee.label);
            return function == functionSignatures_.end() ||
                   functionOutputCountIsValid(function->second, 1);
        }
        return true;
    }

    BytecodeProgram program_;
    std::vector<LoopPatch> loopStack_;
    size_t functionDepth_ = 0;
    const BuiltinRegistry* builtinRegistry_ = nullptr;
    std::map<std::string, FunctionSignature> functionSignatures_;
};

} // namespace

BytecodeProgram BytecodeLowerer::lower(const SemanticResult& semantic) {
    BytecodeLoweringContext context;
    return context.lower(semantic);
}

const char* bytecodeOpName(BytecodeOp op) {
    switch (op) {
    case BytecodeOp::EnterModule:
        return "EnterModule";
    case BytecodeOp::LeaveModule:
        return "LeaveModule";
    case BytecodeOp::EnterClass:
        return "EnterClass";
    case BytecodeOp::LeaveClass:
        return "LeaveClass";
    case BytecodeOp::EnterPropertyInitializer:
        return "EnterPropertyInitializer";
    case BytecodeOp::LeavePropertyInitializer:
        return "LeavePropertyInitializer";
    case BytecodeOp::EnterEnumerationMemberInitializer:
        return "EnterEnumerationMemberInitializer";
    case BytecodeOp::LeaveEnumerationMemberInitializer:
        return "LeaveEnumerationMemberInitializer";
    case BytecodeOp::EnterArgumentDefault:
        return "EnterArgumentDefault";
    case BytecodeOp::LeaveArgumentDefault:
        return "LeaveArgumentDefault";
    case BytecodeOp::EnterFunction:
        return "EnterFunction";
    case BytecodeOp::LeaveFunction:
        return "LeaveFunction";
    case BytecodeOp::EnterControl:
        return "EnterControl";
    case BytecodeOp::LeaveControl:
        return "LeaveControl";
    case BytecodeOp::ControlHeader:
        return "ControlHeader";
    case BytecodeOp::ControlArm:
        return "ControlArm";
    case BytecodeOp::SwitchBegin:
        return "SwitchBegin";
    case BytecodeOp::SwitchCase:
        return "SwitchCase";
    case BytecodeOp::SwitchOtherwise:
        return "SwitchOtherwise";
    case BytecodeOp::SwitchEnd:
        return "SwitchEnd";
    case BytecodeOp::TryBegin:
        return "TryBegin";
    case BytecodeOp::TryEnd:
        return "TryEnd";
    case BytecodeOp::Jump:
        return "Jump";
    case BytecodeOp::JumpIfFalse:
        return "JumpIfFalse";
    case BytecodeOp::Break:
        return "Break";
    case BytecodeOp::Continue:
        return "Continue";
    case BytecodeOp::Return:
        return "Return";
    case BytecodeOp::ForBegin:
        return "ForBegin";
    case BytecodeOp::ForNext:
        return "ForNext";
    case BytecodeOp::CaptureExpression:
        return "CaptureExpression";
    case BytecodeOp::Pop:
        return "Pop";
    case BytecodeOp::DeclareGlobal:
        return "DeclareGlobal";
    case BytecodeOp::DeclarePersistent:
        return "DeclarePersistent";
    case BytecodeOp::BeginIndexContext:
        return "BeginIndexContext";
    case BytecodeOp::BeginIndexArgument:
        return "BeginIndexArgument";
    case BytecodeOp::BeginLvalue:
        return "BeginLvalue";
    case BytecodeOp::BeginLvalueIndexContext:
        return "BeginLvalueIndexContext";
    case BytecodeOp::LvalueDescendMember:
        return "LvalueDescendMember";
    case BytecodeOp::LvalueDescendIndex:
        return "LvalueDescendIndex";
    case BytecodeOp::LvalueDescendBrace:
        return "LvalueDescendBrace";
    case BytecodeOp::LoadName:
        return "LoadName";
    case BytecodeOp::LoadLiteral:
        return "LoadLiteral";
    case BytecodeOp::StoreName:
        return "StoreName";
    case BytecodeOp::StoreMember:
        return "StoreMember";
    case BytecodeOp::StoreIndex:
        return "StoreIndex";
    case BytecodeOp::StoreBraceIndex:
        return "StoreBraceIndex";
    case BytecodeOp::StorePathMember:
        return "StorePathMember";
    case BytecodeOp::StorePathIndex:
        return "StorePathIndex";
    case BytecodeOp::StorePathBrace:
        return "StorePathBrace";
    case BytecodeOp::UnaryOp:
        return "UnaryOp";
    case BytecodeOp::BinaryOp:
        return "BinaryOp";
    case BytecodeOp::PostfixOp:
        return "PostfixOp";
    case BytecodeOp::MemberAccess:
        return "MemberAccess";
    case BytecodeOp::MakeNameValueArgument:
        return "MakeNameValueArgument";
    case BytecodeOp::CallOrIndex:
        return "CallOrIndex";
    case BytecodeOp::CallSuperclass:
        return "CallSuperclass";
    case BytecodeOp::BraceIndex:
        return "BraceIndex";
    case BytecodeOp::MakeMatrix:
        return "MakeMatrix";
    case BytecodeOp::MakeMatrixRow:
        return "MakeMatrixRow";
    case BytecodeOp::MakeCell:
        return "MakeCell";
    case BytecodeOp::MakeCellRow:
        return "MakeCellRow";
    case BytecodeOp::MakeFunctionHandle:
        return "MakeFunctionHandle";
    case BytecodeOp::LoadMetaClass:
        return "LoadMetaClass";
    case BytecodeOp::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace mparser
