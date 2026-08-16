#include "mparser/lexer.h"
#include "mparser/parser.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

mparser::ParseResult parse(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    return parser.parse();
}

bool hasChild(const mparser::SyntaxNode& node, mparser::SyntaxKind kind) {
    for (const auto& child : node.children) {
        if (child->kind == kind) {
            return true;
        }
    }
    return false;
}

const mparser::SyntaxNode* firstChild(const mparser::SyntaxNode& node,
                                      mparser::SyntaxKind kind) {
    for (const auto& child : node.children) {
        if (child->kind == kind) {
            return child.get();
        }
    }
    return nullptr;
}

bool containsKind(const mparser::SyntaxNode& node, mparser::SyntaxKind kind) {
    if (node.kind == kind) {
        return true;
    }

    for (const auto& child : node.children) {
        if (containsKind(*child, kind)) {
            return true;
        }
    }

    return false;
}

bool containsStatementLabel(const mparser::SyntaxNode& node,
                            std::string_view label) {
    if (node.kind == mparser::SyntaxKind::Statement && node.label == label) {
        return true;
    }

    for (const auto& child : node.children) {
        if (containsStatementLabel(*child, label)) {
            return true;
        }
    }

    return false;
}

bool containsControlArmLabel(const mparser::SyntaxNode& node,
                             std::string_view label) {
    if (node.kind == mparser::SyntaxKind::ControlArm && node.label == label) {
        return true;
    }

    for (const auto& child : node.children) {
        if (containsControlArmLabel(*child, label)) {
            return true;
        }
    }

    return false;
}

void parseClassdefSmoke() {
    const std::string source = R"(classdef (Sealed) BasicClass < handle
    properties (SetAccess = private)
        Value double = 0
    end

    methods (Static)
        function obj = create(v)
            arguments
                v double = 0
            end
            obj = BasicClass(v);
        end
    end

    events
        Changed
    end

    enumeration
        Red
        Green
    end
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& klass = *result.root->children.front();
    assert(klass.kind == mparser::SyntaxKind::ClassDef);
    assert(klass.label == "BasicClass");
    assert(hasChild(klass, mparser::SyntaxKind::PropertiesBlock));
    assert(hasChild(klass, mparser::SyntaxKind::MethodsBlock));
    assert(hasChild(klass, mparser::SyntaxKind::EventsBlock));
    assert(hasChild(klass, mparser::SyntaxKind::EnumerationBlock));
}

void parseModernClassdefSmoke() {
    const std::string source = R"(classdef (Abstract, Sealed = false) Fancy < pkg.Base & matlab.mixin.SetGet
    properties (Access = private, Dependent)
        Name (1,1) string {mustBeTextScalar} = "demo"
    end

    methods (Abstract, Access = protected)
        y = compute(obj, x)
    end

    events (NotifyAccess = private)
        Updated
    end
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& klass = *result.root->children.front();
    assert(klass.kind == mparser::SyntaxKind::ClassDef);
    assert(klass.label == "Fancy");
    assert(klass.attributes.size() == 2);

    const auto* supers =
        firstChild(klass, mparser::SyntaxKind::SuperclassList);
    assert(supers != nullptr);
    assert(supers->children.size() == 2);
    assert(supers->children[0]->label == "pkg.Base");
    assert(supers->children[1]->label == "matlab.mixin.SetGet");

    const auto* methods = firstChild(klass, mparser::SyntaxKind::MethodsBlock);
    assert(methods != nullptr);
    assert(methods->children.size() == 1);
    assert(methods->children[0]->kind == mparser::SyntaxKind::MethodPrototype);
    assert(methods->children[0]->label == "compute");
}

void parseControlFlowSmoke() {
    const std::string source = R"(function y = f(x)
for i = 1:10
    if x > i
        y = x + i;
        continue
    else
        y = i;
        break
    end
end
return
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);
    const auto& function = *result.root->children.front();
    assert(function.kind == mparser::SyntaxKind::FunctionDef);
    assert(function.label == "f");
    assert(hasChild(function, mparser::SyntaxKind::ForBlock));

    const auto* forBlock = firstChild(function, mparser::SyntaxKind::ForBlock);
    assert(forBlock != nullptr);
    const auto* forHeader = firstChild(*forBlock, mparser::SyntaxKind::ControlHeader);
    assert(forHeader != nullptr);
    assert(containsKind(*forHeader, mparser::SyntaxKind::AssignmentStatement));

    const auto* ifBlock = firstChild(*forBlock, mparser::SyntaxKind::IfBlock);
    assert(ifBlock != nullptr);
    const auto* ifHeader = firstChild(*ifBlock, mparser::SyntaxKind::ControlHeader);
    assert(ifHeader != nullptr);
    assert(containsKind(*ifHeader, mparser::SyntaxKind::ExpressionStatement));

    const auto* elseArm = firstChild(*ifBlock, mparser::SyntaxKind::ControlArm);
    assert(elseArm != nullptr);
    assert(elseArm->label == "else");
    assert(elseArm->children.empty());
    assert(containsStatementLabel(*ifBlock, "continue"));
    assert(containsStatementLabel(*ifBlock, "break"));
    assert(containsStatementLabel(function, "return"));
}

void parseExpressionSmoke() {
    const std::string source = R"(function y = g(A, obj)
[a, b] = obj.Value + A(1:10, end)' .* 2;
h = @(x) x.^2 + sin(x);
meta = ?pkg.MyClass;
y = h(a) + b;
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& function = *result.root->children.front();
    assert(function.kind == mparser::SyntaxKind::FunctionDef);
    assert(function.children.size() == 4);
    assert(function.children[0]->kind == mparser::SyntaxKind::AssignmentStatement);
    assert(function.children[1]->kind == mparser::SyntaxKind::AssignmentStatement);
    assert(function.children[2]->kind == mparser::SyntaxKind::AssignmentStatement);
    assert(function.children[3]->kind == mparser::SyntaxKind::AssignmentStatement);

    assert(containsKind(*function.children[0], mparser::SyntaxKind::OutputList));
    assert(containsKind(*function.children[0], mparser::SyntaxKind::MemberAccessExpr));
    assert(containsKind(*function.children[0], mparser::SyntaxKind::CallOrIndexExpr));
    assert(containsKind(*function.children[0], mparser::SyntaxKind::EndExpr));
    assert(containsKind(*function.children[0], mparser::SyntaxKind::PostfixExpr));
    assert(containsKind(*function.children[1], mparser::SyntaxKind::FunctionHandleExpr));
    assert(containsKind(*function.children[2], mparser::SyntaxKind::MetaClassExpr));
}

void parseKeywordMemberSmoke() {
    auto result = parse("value = details.function;\n");
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);
    const auto& assignment = *result.root->children.front();
    assert(assignment.kind == mparser::SyntaxKind::AssignmentStatement);
    assert(assignment.children.size() == 2);
    const auto& member = *assignment.children[1];
    assert(member.kind == mparser::SyntaxKind::MemberAccessExpr);
    assert(member.label == "function");
}

void parseSwitchSmoke() {
    const std::string source = R"(function y = f(mode)
switch mode
    case "fast"
        y = 1;
    case 2
        y = 2;
    otherwise
        y = 0;
end
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& function = *result.root->children.front();
    const auto* switchBlock =
        firstChild(function, mparser::SyntaxKind::SwitchBlock);
    assert(switchBlock != nullptr);
    assert(containsKind(*switchBlock, mparser::SyntaxKind::ControlHeader));
    assert(containsKind(*switchBlock, mparser::SyntaxKind::StringLiteralExpr));
    assert(containsControlArmLabel(*switchBlock, "case"));
    assert(containsControlArmLabel(*switchBlock, "otherwise"));
}

void parseTryCatchSmoke() {
    const std::string source = R"(function y = f()
try
    y = missingName;
catch err
    y = 1;
end
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& function = *result.root->children.front();
    const auto* tryBlock = firstChild(function, mparser::SyntaxKind::TryBlock);
    assert(tryBlock != nullptr);
    assert(containsControlArmLabel(*tryBlock, "catch"));
    assert(containsKind(*tryBlock, mparser::SyntaxKind::ControlHeader));
    assert(containsKind(*tryBlock, mparser::SyntaxKind::IdentifierExpr));
}

void parseMatrixRowsSmoke() {
    const std::string source = R"(function y = f()
y = [1 2; 3 4];
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 1);

    const auto& function = *result.root->children.front();
    assert(function.kind == mparser::SyntaxKind::FunctionDef);
    assert(function.children.size() == 1);
    assert(containsKind(*function.children.front(), mparser::SyntaxKind::MatrixExpr));

    const auto* assignment = function.children.front().get();
    const auto* matrix = firstChild(*assignment, mparser::SyntaxKind::MatrixExpr);
    assert(matrix != nullptr);
    assert(matrix->children.size() == 2);
    assert(matrix->children[0]->kind == mparser::SyntaxKind::MatrixRow);
    assert(matrix->children[1]->kind == mparser::SyntaxKind::MatrixRow);
    assert(matrix->children[0]->children.size() == 2);
    assert(matrix->children[1]->children.size() == 2);
}

void parseCellRowsSmoke() {
    const std::string source = R"(function y = f()
y = {1, [2 3]; "four", missing};
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    const auto& function = *result.root->children.front();
    const auto* assignment = function.children.front().get();
    const auto* cell =
        firstChild(*assignment, mparser::SyntaxKind::CellExpr);
    assert(cell != nullptr);
    assert(cell->children.size() == 2);
    assert(cell->children[0]->kind == mparser::SyntaxKind::CellRow);
    assert(cell->children[1]->kind == mparser::SyntaxKind::CellRow);
    assert(cell->children[0]->children.size() == 2);
    assert(cell->children[1]->children.size() == 2);
}

void parseConcatenationSignedElementSmoke() {
    auto signedElements = parse("value = [-1 2 -3];\n");
    assert(signedElements.diagnostics.empty());
    const auto* signedMatrix = firstChild(
        *signedElements.root->children.front(),
        mparser::SyntaxKind::MatrixExpr);
    assert(signedMatrix != nullptr);
    assert(signedMatrix->children.size() == 1);
    assert(signedMatrix->children.front()->children.size() == 3);

    auto binary = parse("value = [1 - 2];\n");
    assert(binary.diagnostics.empty());
    const auto* binaryMatrix = firstChild(
        *binary.root->children.front(), mparser::SyntaxKind::MatrixExpr);
    assert(binaryMatrix != nullptr);
    assert(binaryMatrix->children.front()->children.size() == 1);
    assert(binaryMatrix->children.front()->children.front()->kind ==
           mparser::SyntaxKind::BinaryExpr);

    auto unaryPlus = parse("value = [1 +2];\n");
    assert(unaryPlus.diagnostics.empty());
    const auto* plusMatrix = firstChild(
        *unaryPlus.root->children.front(), mparser::SyntaxKind::MatrixExpr);
    assert(plusMatrix != nullptr);
    assert(plusMatrix->children.front()->children.size() == 2);

    auto cell = parse("value = {1 -2};\n");
    assert(cell.diagnostics.empty());
    const auto* cellExpression = firstChild(
        *cell.root->children.front(), mparser::SyntaxKind::CellExpr);
    assert(cellExpression != nullptr);
    assert(cellExpression->children.size() == 1);
    assert(cellExpression->children.front()->kind ==
           mparser::SyntaxKind::CellRow);
    assert(cellExpression->children.front()->children.size() == 2);
}

void parseMultilineDelimitedSmoke() {
    const std::string source = R"([first, second] = outer(...
    inner(...
        missing, ...
        missing), ...
    'mode');
matrix_value = [missing missing
                missing missing];
cell_value = {missing missing
              missing missing};
indexed = matrix_value(...
    2, ...
    1);
continued = outer(...
    missing, ...
    missing);
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(!containsKind(*result.root, mparser::SyntaxKind::Error));
    assert(result.root->children.size() == 5);

    const auto& callAssignment = *result.root->children[0];
    assert(callAssignment.kind == mparser::SyntaxKind::AssignmentStatement);
    assert(callAssignment.children.size() == 2);
    const auto& outerCall = *callAssignment.children[1];
    assert(outerCall.kind == mparser::SyntaxKind::CallOrIndexExpr);
    assert(outerCall.children.size() == 3);
    assert(outerCall.children[1]->kind ==
           mparser::SyntaxKind::CallOrIndexExpr);
    assert(outerCall.children[1]->children.size() == 3);

    for (size_t assignmentIndex : {size_t{1}, size_t{2}}) {
        const auto& assignment = *result.root->children[assignmentIndex];
        const auto& literal = *assignment.children[1];
        assert(literal.children.size() == 2);
        assert(literal.children[0]->children.size() == 2);
        assert(literal.children[1]->children.size() == 2);
    }

    const auto& indexCall = *result.root->children[3]->children[1];
    assert(indexCall.kind == mparser::SyntaxKind::CallOrIndexExpr);
    assert(indexCall.children.size() == 3);

    const auto& continuedCall = *result.root->children[4]->children[1];
    assert(continuedCall.kind == mparser::SyntaxKind::CallOrIndexExpr);
    assert(continuedCall.children.size() == 3);

    auto bareCall = parse(R"(value = outer(
    missing, missing);
)");
    assert(!bareCall.diagnostics.empty());
    assert(containsKind(*bareCall.root, mparser::SyntaxKind::Error));

    auto brokenMatrix = parse(R"(value = [1 +
    2];
)");
    assert(!brokenMatrix.diagnostics.empty());
    assert(containsKind(*brokenMatrix.root, mparser::SyntaxKind::Error));
}

void parseV11CoreCompatibilitySmoke() {
    const std::string source = R"(power = 2^3^2;
dotPower = 2.^3.^2;
if power == 64, branch = 10; else, branch = -1; end
switch branch, case 10, switched = 20; otherwise, switched = -1; end
for value = [1 2 3], total = value; end
while branch < 12, branch = branch + 1; end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 6);

    for (size_t index = 0; index < 2; ++index) {
        const auto& assignment = *result.root->children[index];
        assert(assignment.kind == mparser::SyntaxKind::AssignmentStatement);
        assert(assignment.children.size() == 2);
        const auto& power = *assignment.children[1];
        assert(power.kind == mparser::SyntaxKind::BinaryExpr);
        assert(power.children.size() == 2);
        assert(power.children[0]->kind == mparser::SyntaxKind::BinaryExpr);
    }

    assert(result.root->children[2]->kind == mparser::SyntaxKind::IfBlock);
    assert(result.root->children[3]->kind == mparser::SyntaxKind::SwitchBlock);
    assert(result.root->children[4]->kind == mparser::SyntaxKind::ForBlock);
    assert(result.root->children[5]->kind == mparser::SyntaxKind::WhileBlock);
    assert(containsControlArmLabel(*result.root->children[2], "else"));
    assert(containsControlArmLabel(*result.root->children[3], "case"));
    assert(containsControlArmLabel(*result.root->children[3], "otherwise"));
}

void parseWorkspaceDeclarationSmoke() {
    const std::string source = R"(global shared, cache
function y = f(x)
persistent count values
global shared
y = x;
end
)";

    auto result = parse(source);
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 2);

    const auto& global = *result.root->children.front();
    assert(global.kind == mparser::SyntaxKind::GlobalStatement);
    assert(global.children.size() == 2);
    assert(global.children[0]->label == "shared");
    assert(global.children[1]->label == "cache");

    const auto& function = *result.root->children[1];
    assert(function.kind == mparser::SyntaxKind::FunctionDef);
    const auto* persistent =
        firstChild(function, mparser::SyntaxKind::PersistentStatement);
    assert(persistent != nullptr);
    assert(persistent->children.size() == 2);
    assert(persistent->children[0]->label == "count");
    assert(persistent->children[1]->label == "values");

    const auto* functionGlobal =
        firstChild(function, mparser::SyntaxKind::GlobalStatement);
    assert(functionGlobal != nullptr);
    assert(functionGlobal->children.size() == 1);
    assert(functionGlobal->children[0]->label == "shared");

    assert(!parse("global\n").diagnostics.empty());
    assert(!parse("global first,\n").diagnostics.empty());
    assert(!parse("persistent first,,second\n").diagnostics.empty());
}

void parseCommandFormSmoke() {
    auto result = parse(R"(echo_fn 5
disp 'hello world'
ls ./d
disp "hello world"
a ./ d
a./d
format +
format short E
format
)");
    assert(result.diagnostics.empty());
    assert(result.root->children.size() == 9);

    const auto command = [&result](size_t index) {
        const auto& statement = *result.root->children[index];
        assert(statement.kind == mparser::SyntaxKind::ExpressionStatement);
        assert(statement.children.size() == 1);
        const auto* call = statement.children.front().get();
        assert(call->kind == mparser::SyntaxKind::CallOrIndexExpr);
        assert(call->label == "command");
        return call;
    };

    const auto* echo = command(0);
    assert(echo->children.size() == 2);
    assert(echo->children[0]->label == "echo_fn");
    assert(echo->children[1]->raw == "'5'");

    const auto* quoted = command(1);
    assert(quoted->children.size() == 2);
    assert(quoted->children[1]->raw == "'hello world'");

    const auto* path = command(2);
    assert(path->children.size() == 2);
    assert(path->children[1]->raw == "'./d'");

    const auto* doubleQuoted = command(3);
    assert(doubleQuoted->children.size() == 3);
    assert(doubleQuoted->children[1]->raw == "'\"hello'");
    assert(doubleQuoted->children[2]->raw == "'world\"'");

    for (size_t index = 4; index < 6; ++index) {
        const auto& statement = *result.root->children[index];
        assert(statement.kind == mparser::SyntaxKind::ExpressionStatement);
        assert(statement.children.size() == 1);
        assert(statement.children.front()->kind ==
               mparser::SyntaxKind::BinaryExpr);
    }

    const auto* plusFormat = command(6);
    assert(plusFormat->children.size() == 2);
    assert(plusFormat->children[1]->raw == "'+'");
    const auto* splitFormat = command(7);
    assert(splitFormat->children.size() == 3);
    assert(splitFormat->children[1]->raw == "'short'");
    assert(splitFormat->children[2]->raw == "'E'");

    const auto& bareFormat = *result.root->children[8];
    assert(bareFormat.kind == mparser::SyntaxKind::ExpressionStatement);
    assert(bareFormat.children.size() == 1);
    assert(bareFormat.children.front()->kind ==
           mparser::SyntaxKind::IdentifierExpr);
}

void parseSpaceSeparatedCharacterLiteralSmoke() {
    auto tokens = mparser::Lexer("a = ['ab' 'cd'];\n").lex();
    size_t strings = 0;
    size_t transposes = 0;
    for (const auto& token : tokens) {
        strings += token.kind == mparser::TokenKind::String ? 1 : 0;
        transposes += token.kind == mparser::TokenKind::Apostrophe ? 1 : 0;
    }
    assert(strings == 2);
    assert(transposes == 0);

    auto concatenation = parse("a = ['ab' 'cd'];\n");
    assert(concatenation.diagnostics.empty());

    auto cellTokens = mparser::Lexer("a = {'ab' 'cd'};\n").lex();
    strings = 0;
    transposes = 0;
    for (const auto& token : cellTokens) {
        strings += token.kind == mparser::TokenKind::String ? 1 : 0;
        transposes += token.kind == mparser::TokenKind::Apostrophe ? 1 : 0;
    }
    assert(strings == 2);
    assert(transposes == 0);
    assert(parse("a = {'ab' 'cd'};\n").diagnostics.empty());

    auto adjacentTranspose = parse("a = [x' y];\n");
    assert(adjacentTranspose.diagnostics.empty());
    assert(containsKind(*adjacentTranspose.root,
                        mparser::SyntaxKind::PostfixExpr));

    auto outsideArrayTranspose = parse("a = x ';\n");
    assert(outsideArrayTranspose.diagnostics.empty());
    assert(containsKind(*outsideArrayTranspose.root,
                        mparser::SyntaxKind::PostfixExpr));

    auto repeatedTranspose = parse("a = x'';\n");
    assert(repeatedTranspose.diagnostics.empty());
    auto repeatedNonconjugate = parse("a = x.'';\n");
    assert(repeatedNonconjugate.diagnostics.empty());
}

} // namespace

int main() {
    parseClassdefSmoke();
    parseModernClassdefSmoke();
    parseControlFlowSmoke();
    parseExpressionSmoke();
    parseKeywordMemberSmoke();
    parseSwitchSmoke();
    parseTryCatchSmoke();
    parseMatrixRowsSmoke();
    parseCellRowsSmoke();
    parseConcatenationSignedElementSmoke();
    parseMultilineDelimitedSmoke();
    parseV11CoreCompatibilitySmoke();
    parseWorkspaceDeclarationSmoke();
    parseCommandFormSmoke();
    parseSpaceSeparatedCharacterLiteralSmoke();
    std::cout << "parser smoke tests passed\n";
    return 0;
}
