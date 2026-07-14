#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/semantic.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

struct CompiledSource {
    mparser::ParseResult parsed;
    mparser::SemanticResult semantic;
    mparser::BytecodeProgram bytecode;
};

CompiledSource compile(std::string_view source) {
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());

    mparser::SemanticAnalyzer analyzer;
    auto semantic = analyzer.analyze(*parsed.root);
    assert(semantic.diagnostics.empty());

    mparser::BytecodeLowerer lowerer;
    auto bytecode = lowerer.lower(semantic);
    assert(bytecode.diagnostics.empty());
    return {std::move(parsed), std::move(semantic), std::move(bytecode)};
}

mparser::BytecodeVmResult run(std::string_view source) {
    auto compiled = compile(source);
    mparser::BytecodeVm vm;
    return vm.run(compiled.bytecode, compiled.semantic);
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

bool hasDiagnostic(const mparser::BytecodeVmResult& result,
                   std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void executeSubclassPrivateIdentitySmoke() {
    const auto result = run(R"(classdef PrivateMethodBase
    methods
        function value = baseValue(obj)
            value = obj.compute();
        end
    end
    methods (Access = private)
        function value = compute(obj)
            value = 10;
        end
    end
end

classdef PrivateMethodChild < PrivateMethodBase
    methods
        function value = childValue(obj)
            value = obj.compute();
        end
    end
    methods (Access = private)
        function value = compute(obj)
            value = 20;
        end
    end
end

obj = PrivateMethodChild();
base_value = obj.baseValue();
child_value = obj.childValue();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_value", 10);
    assertNumber(result, "child_value", 20);
}

void executePrivateAndVirtualDispatchSmoke() {
    const auto result = run(R"(classdef DispatchBase
    methods
        function value = baseSecret(obj)
            value = obj.secret();
        end
        function value = dispatch(obj)
            value = obj.step();
        end
        function value = step(obj)
            value = 1;
        end
    end
    methods (Access = private)
        function value = secret(obj)
            value = 5;
        end
    end
end

classdef DispatchChild < DispatchBase
    methods
        function value = secret(obj)
            value = 9;
        end
        function value = step(obj)
            value = 2;
        end
    end
end

obj = DispatchChild();
base_secret = obj.baseSecret();
child_secret = obj.secret();
virtual_value = obj.dispatch();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_secret", 5);
    assertNumber(result, "child_secret", 9);
    assertNumber(result, "virtual_value", 2);
}

void executeMultipleInheritancePrivateIdentitySmoke() {
    const auto result = run(R"(classdef LeftPrivateMethod
    methods
        function value = leftValue(obj)
            value = obj.tag();
        end
    end
    methods (Access = private)
        function value = tag(obj)
            value = 3;
        end
    end
end

classdef RightPrivateMethod
    methods
        function value = rightValue(obj)
            value = obj.tag();
        end
    end
    methods (Access = private)
        function value = tag(obj)
            value = 4;
        end
    end
end

classdef CombinedPrivateMethod < LeftPrivateMethod & RightPrivateMethod
    methods
        function value = tag(obj)
            value = 30;
        end
    end
end

obj = CombinedPrivateMethod();
left_value = obj.leftValue();
right_value = obj.rightValue();
visible_value = obj.tag();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "left_value", 3);
    assertNumber(result, "right_value", 4);
    assertNumber(result, "visible_value", 30);
}

void executeStaticPrivateIdentitySmoke() {
    const auto result = run(R"(classdef StaticPrivateBase
    methods (Static)
        function value = baseCode()
            value = StaticPrivateBase.code();
        end
    end
    methods (Static, Access = private)
        function value = code()
            value = 11;
        end
    end
end

classdef StaticPrivateChild < StaticPrivateBase
    methods (Static)
        function value = childCode()
            value = StaticPrivateChild.code();
        end
    end
    methods (Static, Access = private)
        function value = code()
            value = 22;
        end
    end
end

base_code = StaticPrivateChild.baseCode();
child_code = StaticPrivateChild.childCode();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_code", 11);
    assertNumber(result, "child_code", 22);
}

void executeEmptyAccessListIdentitySmoke() {
    const auto result = run(R"(classdef EmptyMethodBase
    methods
        function value = baseValue(obj)
            value = obj.token();
        end
    end
    methods (Access = {})
        function value = token(obj)
            value = 13;
        end
    end
end

classdef EmptyMethodChild < EmptyMethodBase
    methods
        function value = childValue(obj)
            value = obj.token();
        end
    end
    methods (Access = {})
        function value = token(obj)
            value = 17;
        end
    end
end

obj = EmptyMethodChild();
base_value = obj.baseValue();
child_value = obj.childValue();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "base_value", 13);
    assertNumber(result, "child_value", 17);
}

void executeDiamondPrivateIdentitySmoke() {
    const auto result = run(R"(classdef DiamondPrivateRoot
    methods
        function value = rootValue(obj)
            value = obj.hiddenValue();
        end
    end
    methods (Access = private)
        function value = hiddenValue(obj)
            value = 41;
        end
    end
end

classdef DiamondPrivateLeft < DiamondPrivateRoot
end

classdef DiamondPrivateRight < DiamondPrivateRoot
end

classdef DiamondPrivateLeaf < DiamondPrivateLeft & DiamondPrivateRight
end

obj = DiamondPrivateLeaf();
diamond_value = obj.rootValue();
)");

    assert(result.diagnostics.empty());
    assertNumber(result, "diamond_value", 41);
}

void rejectExternalPrivateMethodAccessSmoke() {
    const auto result = run(R"(classdef HiddenMethod
    methods (Access = private)
        function value = secret(obj)
            value = 1;
        end
    end
end

obj = HiddenMethod();
value = obj.secret();
)");

    assert(hasDiagnostic(result, "method access is denied: HiddenMethod.secret"));
}

void rejectUnrelatedPublicMethodConflictSmoke() {
    const auto result = run(R"(classdef PublicMethodLeft
    methods
        function value = shared(obj)
            value = 1;
        end
    end
end

classdef PublicMethodRight
    methods
        function value = shared(obj)
            value = 2;
        end
    end
end

classdef PublicMethodConflict < PublicMethodLeft & PublicMethodRight
end

obj = PublicMethodConflict();
)");

    assert(hasDiagnostic(
        result,
        "ambiguous inherited method: PublicMethodConflict.shared"));
}

void rejectSealedPrivateRedefinitionSmoke() {
    const auto result = run(R"(classdef SealedPrivateBase
    methods (Access = private, Sealed)
        function value = token(obj)
            value = 1;
        end
    end
end

classdef SealedPrivateChild < SealedPrivateBase
    methods (Access = private)
        function value = token(obj)
            value = 2;
        end
    end
end

obj = SealedPrivateChild();
)");

    assert(hasDiagnostic(
        result,
        "sealed method cannot be redefined: SealedPrivateChild.token"));
}

} // namespace

int main() {
    executeSubclassPrivateIdentitySmoke();
    executePrivateAndVirtualDispatchSmoke();
    executeMultipleInheritancePrivateIdentitySmoke();
    executeStaticPrivateIdentitySmoke();
    executeEmptyAccessListIdentitySmoke();
    executeDiamondPrivateIdentitySmoke();
    rejectExternalPrivateMethodAccessSmoke();
    rejectUnrelatedPublicMethodConflictSmoke();
    rejectSealedPrivateRedefinitionSmoke();
    std::cout << "class method identity smoke tests passed\n";
    return 0;
}
