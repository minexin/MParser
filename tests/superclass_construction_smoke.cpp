#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct AnalyzedSource {
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

mparser::ParseResult parse(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    return parser.parse();
}

mparser::SemanticResult analyze(std::string_view source) {
    auto parsed = parse(source);
    assert(parsed.diagnostics.empty());
    mparser::SemanticAnalyzer analyzer;
    return analyzer.analyze(*parsed.root);
}

AnalyzedSource compile(std::string_view source) {
    auto semantic = analyze(source);
    assert(semantic.diagnostics.empty());
    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    return {std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(const AnalyzedSource& compiled) {
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
}

bool containsSyntaxKind(const mparser::SyntaxNode& node,
                        mparser::SyntaxKind kind) {
    if (node.kind == kind) {
        return true;
    }
    for (const auto& child : node.children) {
        if (containsSyntaxKind(*child, kind)) {
            return true;
        }
    }
    return false;
}

const mparser::HirNode* findHirNode(const mparser::HirNode& node,
                                    mparser::HirKind kind,
                                    std::string_view label,
                                    mparser::BindingKind binding) {
    if (node.kind == kind && node.label == label &&
        node.binding.kind == binding) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found =
                findHirNode(*child, kind, label, binding)) {
            return found;
        }
    }
    return nullptr;
}

const mparser::RuntimeValue* findVariable(
    const mparser::BytecodeVmResult& result, std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

void assertNumber(const mparser::BytecodeVmResult& result,
                  std::string_view name, double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

bool hasSemanticDiagnostic(const mparser::SemanticResult& result,
                           std::string_view fragment) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasRuntimeDiagnostic(const mparser::BytecodeVmResult& result,
                          std::string_view fragment) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void runFrontendContractSmoke() {
    constexpr std::string_view source = R"(classdef Child < Base
    methods
        function obj = Child(value)
            obj = obj@Base(value);
        end
        function value = score(obj)
            value = score@Base(obj) + 1;
        end
    end
end
classdef Base
    methods
        function obj = Base(value)
        end
        function value = score(obj)
            value = 2;
        end
    end
end
)";

    auto parsed = parse(source);
    assert(parsed.diagnostics.empty());
    assert(containsSyntaxKind(*parsed.root,
                              mparser::SyntaxKind::SuperclassCallExpr));

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());
    const auto* constructorCall = findHirNode(
        *semantic.root, mparser::HirKind::SuperclassCall, "Base",
        mparser::BindingKind::Class);
    const auto* methodCall = findHirNode(
        *semantic.root, mparser::HirKind::SuperclassCall, "Base",
        mparser::BindingKind::Method);
    assert(constructorCall != nullptr);
    assert(methodCall != nullptr);

    mparser::BytecodeLowerer lowerer;
    const auto bytecode = lowerer.lower(semantic);
    int callCount = 0;
    bool foundConstructor = false;
    bool foundMethod = false;
    for (const auto& instruction : bytecode.instructions) {
        if (instruction.op != mparser::BytecodeOp::CallSuperclass) {
            continue;
        }
        ++callCount;
        assert(instruction.operand == "Base");
        foundConstructor = foundConstructor ||
                           instruction.binding.kind ==
                               mparser::BindingKind::Class;
        foundMethod = foundMethod ||
                      instruction.binding.kind ==
                          mparser::BindingKind::Method;
    }
    assert(callCount == 2);
    assert(foundConstructor);
    assert(foundMethod);
}

void runExplicitAndQualifiedCallSmoke() {
    const auto compiled = compile(R"(classdef Report < RecordBase & RevisionBase
    properties
        Pages
    end
    methods
        function obj = Report(id, revision, pages)
            obj@RecordBase(id);
            obj = obj@RevisionBase(revision);
            obj.Pages = pages;
        end
        function value = score(obj)
            value = score@RecordBase(obj) + obj.Pages;
        end
        function value = adjustment(obj)
            value = obj.Pages;
        end
    end
end
classdef RecordBase
    properties
        Id
    end
    methods
        function obj = RecordBase(id)
            obj.Id = id;
        end
        function value = score(obj)
            value = obj.Id * 10 + obj.adjustment();
        end
        function value = adjustment(obj)
            value = 0;
        end
    end
end
classdef RevisionBase
    properties
        Revision
    end
    methods
        function obj = RevisionBase(revision)
            obj.Revision = revision;
        end
    end
end
report = Report(4, 2, 3);
score_value = report.score();
id_value = report.Id;
revision_value = report.Revision;
pages_value = report.Pages;
)");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    const auto* report = findVariable(result, "report");
    assert(report != nullptr);
    assert(report->kind == mparser::RuntimeValueKind::Object);
    assert(report->className == "Report");
    assertNumber(result, "score_value", 46);
    assertNumber(result, "id_value", 4);
    assertNumber(result, "revision_value", 2);
    assertNumber(result, "pages_value", 3);
}

void runImplicitAndDefaultConstructorSmoke() {
    const auto compiled = compile(R"(classdef Forwarded < RequiredBase
end
classdef RequiredBase
    properties
        Value
    end
    methods
        function obj = RequiredBase(value)
            obj.Value = value;
        end
    end
end
classdef Tagged < DefaultBase
    properties
        Payload
    end
    methods
        function obj = Tagged(payload)
            obj.Payload = payload;
        end
    end
end
classdef DefaultBase
    properties
        Tag
    end
    methods
        function obj = DefaultBase()
            obj.Tag = 5;
        end
    end
end
forwarded = Forwarded(7);
forwarded_value = forwarded.Value;
tagged = Tagged(9);
tag_value = tagged.Tag;
payload_value = tagged.Payload;
)");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    assertNumber(result, "forwarded_value", 7);
    assertNumber(result, "tag_value", 5);
    assertNumber(result, "payload_value", 9);
}

void runHandleConstructionSmoke() {
    const auto compiled = compile(R"(classdef HandleChild < HandleBase
    methods
        function obj = HandleChild(value)
            obj@HandleBase(value);
        end
    end
end
classdef HandleBase < handle
    properties
        Value
    end
    methods
        function obj = HandleBase(value)
            obj.Value = value;
        end
        function setValue(obj, value)
            obj.Value = value;
        end
    end
end
item = HandleChild(3);
alias = item;
alias.setValue(8);
observed = item.Value;
)");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    const auto* item = findVariable(result, "item");
    const auto* alias = findVariable(result, "alias");
    assert(item != nullptr && alias != nullptr);
    assert(item->handleObject);
    assert(item->sharedFields == alias->sharedFields);
    assertNumber(result, "observed", 8);
}

void runDiamondConstructionSmoke() {
    const auto compiled = compile(R"(classdef Diamond < LeftBranch & RightBranch
    methods
        function obj = Diamond()
            obj@LeftBranch();
            obj@RightBranch();
        end
    end
end
classdef LeftBranch < RootBase
    methods
        function obj = LeftBranch()
            obj.Trace = obj.Trace + 10;
        end
    end
end
classdef RightBranch < RootBase
    methods
        function obj = RightBranch()
            obj.Trace = obj.Trace + 100;
        end
    end
end
classdef RootBase
    properties
        Trace
    end
    methods
        function obj = RootBase()
            obj.Trace = 1;
        end
    end
end
item = Diamond();
trace_value = item.Trace;
)");

    const auto result = run(compiled);
    assert(result.diagnostics.empty());
    assertNumber(result, "trace_value", 111);
}

void runSemanticRuleSmoke() {
    const auto duplicate = analyze(R"(classdef Child < Base
    methods
        function obj = Child()
            obj@Base();
            obj@Base();
        end
    end
end
classdef Base
end
)");
    assert(hasSemanticDiagnostic(
        duplicate, "superclass constructor called more than once: Base"));
    mparser::BytecodeLowerer lowerer;
    const auto invalidBytecode = lowerer.lower(duplicate);
    assert(!invalidBytecode.diagnostics.empty());

    const auto indirect = analyze(R"(classdef Leaf < Middle
    methods
        function obj = Leaf()
            obj@Root();
        end
    end
end
classdef Middle < Root
end
classdef Root
end
)");
    assert(hasSemanticDiagnostic(
        indirect, "not a direct executable superclass: Root"));

    const auto conditional = analyze(R"(classdef Child < Base
    methods
        function obj = Child(flag)
            if flag
                obj@Base();
            end
        end
    end
end
classdef Base
end
)");
    assert(hasSemanticDiagnostic(
        conditional, "superclass constructor call cannot be conditional"));

    const auto late = analyze(R"(classdef Child < Base
    properties
        Value
    end
    methods
        function obj = Child()
            obj.Value = 1;
            obj@Base();
        end
    end
end
classdef Base
end
)");
    assert(hasSemanticDiagnostic(
        late, "must precede all other references"));

    const auto objectArgument = analyze(R"(classdef Child < Base
    properties
        Value
    end
    methods
        function obj = Child()
            obj@Base(obj.Value);
        end
    end
end
classdef Base
end
)");
    assert(hasSemanticDiagnostic(
        objectArgument, "arguments cannot reference the constructed object"));

    const auto wrongMethod = analyze(R"(classdef Child < Base
    methods
        function value = other(obj)
            value = score@Base(obj);
        end
    end
end
classdef Base
    methods
        function value = score(obj)
            value = 1;
        end
    end
end
)");
    assert(hasSemanticDiagnostic(
        wrongMethod, "must name the current method"));

    const auto missingObject = analyze(R"(classdef Child < Base
    methods
        function value = score(obj)
            value = score@Base();
        end
    end
end
classdef Base
    methods
        function value = score(obj)
            value = 1;
        end
    end
end
)");
    assert(hasSemanticDiagnostic(
        missingObject, "requires an object argument"));

    const auto noOutput = analyze(R"(classdef Broken
    methods
        function Broken()
        end
    end
end
)");
    assert(hasSemanticDiagnostic(
        noOutput, "class constructor must declare an object output"));
}

void runImplicitArgumentFailureSmoke() {
    const auto compiled = compile(R"(classdef Child < RequiredBase
    properties
        Value
    end
    methods
        function obj = Child()
            obj.Value = 1;
        end
    end
end
classdef RequiredBase
    methods
        function obj = RequiredBase(value)
        end
    end
end
item = Child();
)");

    const auto result = run(compiled);
    assert(hasRuntimeDiagnostic(
        result, "function invocation failed for RequiredBase: function "
                "argument count mismatch"));
}

} // namespace

int main() {
    runFrontendContractSmoke();
    runExplicitAndQualifiedCallSmoke();
    runImplicitAndDefaultConstructorSmoke();
    runHandleConstructionSmoke();
    runDiamondConstructionSmoke();
    runSemanticRuleSmoke();
    runImplicitArgumentFailureSmoke();
    std::cout << "superclass construction smoke tests passed\n";
    return 0;
}
