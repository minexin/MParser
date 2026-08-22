#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/semantic/semantic.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::BytecodeProgram lower(std::string_view source,
                               mparser::SemanticResult& semantic) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parseResult = parser.parse();
    assert(parseResult.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    semantic = analyzer.analyze(*parseResult.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    return lowerer.lower(semantic);
}

bool containsOp(const mparser::BytecodeProgram& program,
                mparser::BytecodeOp op) {
    for (const auto& instruction : program.instructions) {
        if (instruction.op == op) {
            return true;
        }
    }
    return false;
}

const mparser::BytecodeInstruction*
findInstruction(const mparser::BytecodeProgram& program, mparser::BytecodeOp op,
                std::string_view operand) {
    for (const auto& instruction : program.instructions) {
        if (instruction.op == op && instruction.operand == operand) {
            return &instruction;
        }
    }
    return nullptr;
}

const mparser::BytecodeInstruction*
findBoundInstruction(const mparser::BytecodeProgram& program,
                     mparser::BytecodeOp op, mparser::BindingKind binding) {
    for (const auto& instruction : program.instructions) {
        if (instruction.op == op && instruction.binding.kind == binding) {
            return &instruction;
        }
    }
    return nullptr;
}

void lowerLoopAndBuiltinSmoke() {
    const std::string source = R"(function y = f(x)
for i = 1:3
    y = sin(x + i);
end
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    assert(!program.instructions.empty());
    assert(containsOp(program, mparser::BytecodeOp::EnterFunction));
    assert(containsOp(program, mparser::BytecodeOp::ForBegin));
    assert(containsOp(program, mparser::BytecodeOp::ForNext));

    const auto* storeI =
        findInstruction(program, mparser::BytecodeOp::ForBegin, "i");
    assert(storeI != nullptr);
    assert(storeI->binding.kind == mparser::BindingKind::LocalVariable);
    assert(storeI->target >= 0);

    const auto* nextI =
        findInstruction(program, mparser::BytecodeOp::ForNext, "i");
    assert(nextI != nullptr);
    assert(nextI->target >= 0);

    const auto* sinCall =
        findBoundInstruction(program, mparser::BytecodeOp::CallOrIndex,
                             mparser::BindingKind::Builtin);
    assert(sinCall != nullptr);
    assert(sinCall->operandCount == 1);
    assert(sinCall->calleeName == "sin");
}

void lowerClassMemberStoreSmoke() {
    const std::string source = R"(classdef Demo
    properties
        Value
    end

    methods
        function obj = Demo(v)
            obj.Value = v;
        end
    end
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    assert(containsOp(program, mparser::BytecodeOp::EnterClass));
    assert(containsOp(program, mparser::BytecodeOp::EnterFunction));

    const auto* storeValue =
        findInstruction(program, mparser::BytecodeOp::StoreMember, "Value");
    assert(storeValue != nullptr);
    assert(storeValue->binding.kind == mparser::BindingKind::Property);
}

void lowerMultiOutputCallSmoke() {
    const std::string source = R"(function y = f()
A = [1 2; 3 4];
[rows, cols] = size(A);
y = rows + cols;
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    const auto* sizeCall =
        findBoundInstruction(program, mparser::BytecodeOp::CallOrIndex,
                             mparser::BindingKind::Builtin);
    assert(sizeCall != nullptr);
    assert(sizeCall->operandCount == 1);
    assert(sizeCall->resultCount == 2);

    const auto* storeRows =
        findInstruction(program, mparser::BytecodeOp::StoreName, "rows");
    const auto* storeCols =
        findInstruction(program, mparser::BytecodeOp::StoreName, "cols");
    assert(storeRows != nullptr);
    assert(storeCols != nullptr);
}

void lowerIndexedAssignmentSmoke() {
    const std::string source = R"(function y = f()
A = [1 2 3; 4 5 6];
A(end, :) = 9;
y = sum(A(:));
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    assert(containsOp(program, mparser::BytecodeOp::BeginIndexContext));
    assert(containsOp(program, mparser::BytecodeOp::BeginIndexArgument));

    const auto* storeA =
        findInstruction(program, mparser::BytecodeOp::StoreIndex, "A");
    assert(storeA != nullptr);
    assert(storeA->operandCount == 2);
    assert(storeA->binding.kind == mparser::BindingKind::LocalVariable);
}

void lowerSwitchAndTrySmoke() {
    const std::string source = R"(function y = f(mode)
y = 0;
switch mode
    case "a"
        y = 1;
    otherwise
        y = 2;
end

try
    y = y + missingName;
catch err
    message = err;
    y = y + 10;
end
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    assert(containsOp(program, mparser::BytecodeOp::SwitchBegin));
    assert(containsOp(program, mparser::BytecodeOp::SwitchCase));
    assert(containsOp(program, mparser::BytecodeOp::SwitchOtherwise));
    assert(containsOp(program, mparser::BytecodeOp::SwitchEnd));
    assert(containsOp(program, mparser::BytecodeOp::TryBegin));
    assert(containsOp(program, mparser::BytecodeOp::TryEnd));
}

void lowerNondeterministicAssignmentSmoke() {
    mparser::SemanticResult semantic;
    const auto program = lower("tic; elapsed = toc;\n", semantic);
    const auto* store = findInstruction(
        program, mparser::BytecodeOp::StoreName, "elapsed");
    assert(store != nullptr);
    assert(store->nondeterministicAssignment);
}

void lowerDiscardedExpressionSmoke() {
    const std::string source = R"(function y = f(x)
try
    x;
catch err
end
y = 1;
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    assert(containsOp(program, mparser::BytecodeOp::Pop));
    const auto validation =
        mparser::validateBytecodeProgram(program, &semantic);
    assert(validation.succeeded);
    assert(validation.diagnostics.empty());
}

void lowerWorkspaceDeclarationSmoke() {
    const std::string source = R"(global shared
shared.Value = 1;
function y = f()
persistent cache
cache(1) = 2;
y = cache(1);
end
)";

    mparser::SemanticResult semantic;
    const auto program = lower(source, semantic);

    const auto* global =
        findInstruction(program, mparser::BytecodeOp::DeclareGlobal,
                        "shared");
    assert(global != nullptr);
    assert(global->binding.kind ==
           mparser::BindingKind::GlobalVariable);

    const auto* persistent =
        findInstruction(program, mparser::BytecodeOp::DeclarePersistent,
                        "cache");
    assert(persistent != nullptr);
    assert(persistent->binding.kind ==
           mparser::BindingKind::PersistentVariable);

    const auto* storeMember =
        findInstruction(program, mparser::BytecodeOp::StoreMember,
                        "Value");
    assert(storeMember != nullptr);
    assert(storeMember->receiverName == "shared");
    assert(storeMember->receiverBinding.kind ==
           mparser::BindingKind::GlobalVariable);

    const auto* storeIndex =
        findInstruction(program, mparser::BytecodeOp::StoreIndex,
                        "cache");
    assert(storeIndex != nullptr);
    assert(storeIndex->binding.kind ==
           mparser::BindingKind::PersistentVariable);
}

} // namespace

int main() {
    lowerLoopAndBuiltinSmoke();
    lowerClassMemberStoreSmoke();
    lowerMultiOutputCallSmoke();
    lowerIndexedAssignmentSmoke();
    lowerSwitchAndTrySmoke();
    lowerNondeterministicAssignmentSmoke();
    lowerDiscardedExpressionSmoke();
    lowerWorkspaceDeclarationSmoke();
    std::cout << "bytecode smoke tests passed\n";
    return 0;
}
