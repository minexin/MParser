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
    std::cout << "parser smoke tests passed\n";
    return 0;
}
