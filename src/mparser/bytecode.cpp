#include "mparser/bytecode.h"

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mparser {
namespace {

class BytecodeLoweringContext {
public:
    BytecodeProgram lower(const SemanticResult& semantic) {
        if (semantic.root) {
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
            op, node.label.empty() ? node.raw : node.label, {}, node.binding,
            node.span, operandCount, target, resultCount});
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
            emitBlock(BytecodeOp::EnterFunction, BytecodeOp::LeaveFunction,
                      node);
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
            lowerChildren(node);
            emit(BytecodeOp::BinaryOp, node, childCount(node));
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
        case HirKind::MemberAccess:
            lowerChildren(node);
            emit(BytecodeOp::MemberAccess, node, childCount(node));
            break;
        case HirKind::CallOrIndex:
            lowerCallOrIndex(node, 1);
            break;
        case HirKind::BraceIndex:
            if (!node.children.empty()) {
                lowerNode(*node.children.front());
                for (size_t index = 1; index < node.children.size(); ++index) {
                    lowerNode(*node.children[index]);
                }
            }
            emit(BytecodeOp::BraceIndex, node, argumentCount(node));
            break;
        case HirKind::FunctionHandle:
            lowerChildren(node);
            emit(BytecodeOp::MakeFunctionHandle, node, childCount(node));
            break;
        case HirKind::MetaClass:
            emit(BytecodeOp::LoadMetaClass, node);
            break;
        case HirKind::OutputList:
        case HirKind::ParameterList:
        case HirKind::MethodPrototype:
        case HirKind::Property:
        case HirKind::Statement:
            if (lowerControlStatement(node)) {
                break;
            }
            lowerChildren(node);
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
        lowerNode(*header.children.front());
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

    void lowerExpression(const HirNode& node, int resultCount = 1) {
        if (node.kind == HirKind::CallOrIndex) {
            lowerCallOrIndex(node, resultCount);
            return;
        }
        lowerNode(node);
    }

    void lowerCallOrIndex(const HirNode& node, int resultCount) {
        if (node.children.empty()) {
            emit(BytecodeOp::CallOrIndex, node, argumentCount(node), -1,
                 resultCount);
            return;
        }

        lowerNode(*node.children.front());
        if (node.binding.kind == BindingKind::Builtin ||
            node.binding.kind == BindingKind::Function) {
            for (size_t index = 1; index < node.children.size(); ++index) {
                lowerNode(*node.children[index]);
            }
        } else {
            lowerIndexArguments(node);
        }
        emit(BytecodeOp::CallOrIndex, node, argumentCount(node), -1,
             resultCount);
    }

    void lowerIndexArguments(const HirNode& node) {
        emit(BytecodeOp::BeginIndexContext, node, argumentCount(node));
        for (size_t index = 1; index < node.children.size(); ++index) {
            emit(BytecodeOp::BeginIndexArgument, *node.children[index],
                 static_cast<int>(index - 1));
            lowerNode(*node.children[index]);
        }
    }

    void lowerAssignment(const HirNode& node) {
        if (node.children.size() < 2) {
            lowerChildren(node);
            return;
        }

        const HirNode& target = *node.children.front();
        const int resultCount = target.kind == HirKind::OutputList
                                    ? childCount(target)
                                    : 1;
        for (size_t i = 1; i < node.children.size(); ++i) {
            lowerExpression(*node.children[i], resultCount);
        }
        lowerAssignmentTarget(target);
    }

    void lowerAssignmentTarget(const HirNode& node) {
        switch (node.kind) {
        case HirKind::NameRef:
            emit(BytecodeOp::StoreName, node);
            break;
        case HirKind::OutputList:
            for (auto it = node.children.rbegin();
                 it != node.children.rend(); ++it) {
                lowerAssignmentTarget(**it);
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
            if (!node.children.empty()) {
                lowerNode(*node.children.front());
            }
            {
                const size_t store =
                    emit(BytecodeOp::StoreMember, node, childCount(node));
                if (!node.children.empty() &&
                    node.children.front()->kind == HirKind::NameRef) {
                    program_.instructions[store].receiverName =
                        node.children.front()->label;
                }
            }
            break;
        case HirKind::CallOrIndex:
            if (node.children.empty()) {
                emit(BytecodeOp::StoreIndex, node, argumentCount(node));
                break;
            }
            lowerNode(*node.children.front());
            lowerIndexArguments(node);
            emit(BytecodeOp::StoreIndex, *node.children.front(),
                 argumentCount(node));
            break;
        case HirKind::BraceIndex:
            if (node.children.empty()) {
                emit(BytecodeOp::StoreBraceIndex, node, argumentCount(node));
                break;
            }
            lowerNode(*node.children.front());
            for (size_t index = 1; index < node.children.size(); ++index) {
                lowerNode(*node.children[index]);
            }
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

    BytecodeProgram program_;
    std::vector<LoopPatch> loopStack_;
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
    case BytecodeOp::Pop:
        return "Pop";
    case BytecodeOp::BeginIndexContext:
        return "BeginIndexContext";
    case BytecodeOp::BeginIndexArgument:
        return "BeginIndexArgument";
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
    case BytecodeOp::UnaryOp:
        return "UnaryOp";
    case BytecodeOp::BinaryOp:
        return "BinaryOp";
    case BytecodeOp::PostfixOp:
        return "PostfixOp";
    case BytecodeOp::MemberAccess:
        return "MemberAccess";
    case BytecodeOp::CallOrIndex:
        return "CallOrIndex";
    case BytecodeOp::BraceIndex:
        return "BraceIndex";
    case BytecodeOp::MakeMatrix:
        return "MakeMatrix";
    case BytecodeOp::MakeMatrixRow:
        return "MakeMatrixRow";
    case BytecodeOp::MakeCell:
        return "MakeCell";
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
