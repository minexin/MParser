#include "mparser/embedding/compiled_module.h"
#include "mparser/frontend/source_loader.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

TemporaryDirectory makeTemporaryDirectory() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    auto path = std::filesystem::temp_directory_path() /
                ("mparser_class_private_" + std::to_string(nonce));
    std::filesystem::create_directories(path);
    return TemporaryDirectory{std::move(path)};
}

void writeFile(const std::filesystem::path& path,
               std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output << content;
    assert(output.good());
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

bool hasDiagnostic(const std::vector<mparser::Diagnostic>& diagnostics,
                   std::string_view text) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::HirNode* findFunction(const mparser::HirNode* node,
                                     std::string_view name) {
    if (!node) {
        return nullptr;
    }
    if (node->kind == mparser::HirKind::Function && node->label == name) {
        return node;
    }
    for (const auto& child : node->children) {
        if (const auto* found = findFunction(child.get(), name)) {
            return found;
        }
    }
    return nullptr;
}

const mparser::HirNode* findCall(const mparser::HirNode& node,
                                mparser::BindingKind binding,
                                std::string_view calleeSuffix) {
    if (node.kind == mparser::HirKind::CallOrIndex &&
        node.binding.kind == binding && !node.children.empty()) {
        const auto& callee = *node.children.front();
        if (callee.label == calleeSuffix ||
            (callee.label.size() > calleeSuffix.size() &&
             callee.label.ends_with(calleeSuffix))) {
            return &node;
        }
    }
    for (const auto& child : node.children) {
        if (const auto* found = findCall(*child, binding, calleeSuffix)) {
            return found;
        }
    }
    return nullptr;
}

void writeVaultClass(const std::filesystem::path& library) {
    const auto folder = library / "+securepkg" / "@Vault";
    writeFile(folder / "Vault.m", R"(classdef Vault
    properties (Access = private)
        Secret = 0
    end
    methods
        function obj = Vault(value)
            obj.Secret = value;
        end
        function value = inlineRead(obj)
            value = readSecret(obj);
        end
        value = separateRead(obj)
        function [obj, value] = bump(obj)
            [obj, value] = increment(obj);
        end
        function value = functionChoice(obj)
            value = choose(obj);
        end
        function value = dotChoice(obj)
            value = obj.choose();
        end
        function value = choose(obj)
            value = obj.Secret + 1000;
        end
        function value = importChoice(obj)
            import preferred.*
            value = choose(obj);
        end
    end
    methods (Static)
        function value = staticValue()
            value = privateConstant();
        end
    end
end
)");
    writeFile(folder / "separateRead.m", R"(function value = separateRead(obj)
    value = readSecret(obj);
end
)");
    writeFile(folder / "localChoice.m", R"(function value = localChoice(obj)
    value = choose(obj);
end
function value = choose(obj)
    value = 200;
end
)");
    writeFile(folder / "private" / "readSecret.m",
              R"(function value = readSecret(obj)
    value = normalizeSecret(obj.Secret);
end
)");
    writeFile(folder / "private" / "normalizeSecret.m",
              R"(function value = normalizeSecret(value)
    value = value + 1;
end
)");
    writeFile(folder / "private" / "increment.m",
              R"(function [obj, value] = increment(obj)
    obj.Secret = obj.Secret + 1;
    value = obj.Secret;
end
)");
    writeFile(folder / "private" / "choose.m",
              R"(function value = choose(obj)
    value = obj.Secret + 10;
end
)");
    writeFile(folder / "private" / "privateConstant.m",
              R"(function value = privateConstant()
    value = 7;
end
)");
    writeFile(library / "+preferred" / "choose.m",
              R"(function value = choose(obj)
    value = 300;
end
)");
}

void runClassPrivateExecutionSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(obj = securepkg.Vault(5);
inline_value = obj.inlineRead();
separate_value = obj.separateRead();
[obj, bumped_value] = obj.bump();
after_bump_value = obj.inlineRead();
function_choice = obj.functionChoice();
dot_choice = obj.dotChoice();
local_choice = obj.localChoice();
import_choice = obj.importChoice();
static_value = securepkg.Vault.staticValue();
)");
    writeVaultClass(library);

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 10);

    size_t methodSources = 0;
    size_t privateSources = 0;
    for (const auto& source : loaded.sources) {
        if (!source.classMethodOwner.empty()) {
            ++methodSources;
            assert(source.classMethodOwner == "securepkg.Vault");
        }
        if (!source.classPrivateFunctionOwner.empty()) {
            ++privateSources;
            assert(source.classPrivateFunctionOwner == "securepkg.Vault");
            assert(source.primaryFunctionIdentity.starts_with("$private"));
        }
    }
    assert(methodSources == 2);
    assert(privateSources == 5);

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(module.functions().size() == 1);
    assert(module.findFunction("preferred.choose") != nullptr);

    const auto* functionChoice =
        findFunction(module.semantic().root.get(), "functionChoice");
    const auto* dotChoice =
        findFunction(module.semantic().root.get(), "dotChoice");
    const auto* localChoice =
        findFunction(module.semantic().root.get(), "localChoice");
    const auto* importChoice =
        findFunction(module.semantic().root.get(), "importChoice");
    assert(functionChoice != nullptr && dotChoice != nullptr &&
           localChoice != nullptr && importChoice != nullptr);
    assert(findCall(*functionChoice, mparser::BindingKind::Function,
                    ">choose") != nullptr);
    assert(findCall(*dotChoice, mparser::BindingKind::Method,
                    "choose") != nullptr);
    assert(findCall(*localChoice, mparser::BindingKind::Function,
                    ".localChoice>choose") != nullptr);
    assert(findCall(*importChoice, mparser::BindingKind::Function,
                    "preferred.choose") != nullptr);

    for (const auto& source : loaded.sources) {
        if (source.classPrivateFunctionOwner.empty()) {
            continue;
        }
        const auto* function = findFunction(
            module.semantic().root.get(), source.primaryFunctionIdentity);
        assert(function != nullptr);
        assert(function->lexicalClassName == "securepkg.Vault");
    }

    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "inline_value", 6);
    assertNumber(runtime, "separate_value", 6);
    assertNumber(runtime, "bumped_value", 6);
    assertNumber(runtime, "after_bump_value", 7);
    assertNumber(runtime, "function_choice", 16);
    assertNumber(runtime, "dot_choice", 1006);
    assertNumber(runtime, "local_choice", 200);
    assertNumber(runtime, "import_choice", 300);
    assertNumber(runtime, "static_value", 7);
}

void writeInheritanceClasses(const std::filesystem::path& library) {
    const auto base = library / "@BaseBox";
    writeFile(base / "BaseBox.m", R"(classdef BaseBox
    properties (Access = private)
        Value = 0
    end
    methods
        function obj = BaseBox(value)
            obj.Value = value;
        end
        function value = expose(obj)
            value = baseOnly(obj);
        end
    end
end
)");
    writeFile(base / "private" / "baseOnly.m",
              R"(function value = baseOnly(obj)
    value = obj.Value;
end
)");

    const auto child = library / "@ChildBox";
    writeFile(child / "ChildBox.m", R"(classdef ChildBox < BaseBox
    methods
        function obj = ChildBox(value)
            obj = obj@BaseBox(value);
        end
        function value = steal(obj)
            value = baseOnly(obj);
        end
    end
end
)");
}

void runClassPrivateIsolationSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(obj = ChildBox(9);
allowed_value = obj.expose();
denied_value = obj.steal();
)");
    writeInheritanceClasses(library);

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assertNumber(runtime, "allowed_value", 9);
    assert(hasDiagnostic(runtime.diagnostics,
                         "unknown bytecode runtime variable: baseOnly"));
}

void runPrivateSearchRootIsolationSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto classFolder = temporary.path / "library" / "@HiddenBox";
    writeFile(entry, "value = hiddenValue();\n");
    writeFile(classFolder / "HiddenBox.m", "classdef HiddenBox\nend\n");
    writeFile(classFolder / "private" / "hiddenValue.m",
              "function value = hiddenValue()\n"
              "    value = 99;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(classFolder / "private");
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 1);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(hasDiagnostic(runtime.diagnostics,
                         "unknown bytecode runtime variable: hiddenValue"));
}

void runLexicalPrivilegeIsolationSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(obj = LexicalBox(12);
value = obj.readThroughOrdinary();
)");
    writeFile(library / "LexicalBox.m", R"(classdef LexicalBox
    properties (Access = private)
        Value = 0
    end
    methods
        function obj = LexicalBox(value)
            obj.Value = value;
        end
        function value = readThroughOrdinary(obj)
            value = ordinaryRead(obj);
        end
    end
end
)");
    writeFile(library / "ordinaryRead.m", R"(function value = ordinaryRead(obj)
    value = obj.Value;
end
)");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(hasDiagnostic(runtime.diagnostics,
                         "property get access is denied: LexicalBox.Value"));
}

} // namespace

int main() {
    runClassPrivateExecutionSmoke();
    runClassPrivateIsolationSmoke();
    runPrivateSearchRootIsolationSmoke();
    runLexicalPrivilegeIsolationSmoke();
    std::cout << "class private function smoke tests passed\n";
    return 0;
}
