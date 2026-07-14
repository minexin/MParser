#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/source_loader.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
                ("mparser_namespace_function_" + std::to_string(nonce));
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

template <typename Result>
const mparser::RuntimeValue* findVariable(const Result& result,
                                          std::string_view name) {
    for (const auto& variable : result.variables) {
        if (variable.name == name) {
            return &variable.value;
        }
    }
    return nullptr;
}

template <typename Result>
void assertNumber(const Result& result, std::string_view name,
                  double expected) {
    const auto* value = findVariable(result, name);
    assert(value != nullptr);
    assert(value->kind == mparser::RuntimeValueKind::Number);
    assert(std::fabs(value->number - expected) < 1e-9);
}

bool hasDiagnostic(const mparser::CompiledModule& module,
                   std::string_view text) {
    for (const auto& diagnostic : module.diagnostics()) {
        if (diagnostic.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const mparser::HirNode* findHirNode(const mparser::HirNode* node,
                                   mparser::HirKind kind,
                                   std::string_view label,
                                   mparser::BindingKind binding) {
    if (!node) {
        return nullptr;
    }
    if (node->kind == kind && node->label == label &&
        node->binding.kind == binding) {
        return node;
    }
    for (const auto& child : node->children) {
        if (const auto* result =
                findHirNode(child.get(), kind, label, binding)) {
            return result;
        }
    }
    return nullptr;
}

bool hasFunctionSymbol(const mparser::CompiledModule& module,
                       std::string_view name) {
    for (const auto& symbol : module.semantic().symbols) {
        if (symbol.kind == mparser::SymbolKind::Function &&
            symbol.name == name) {
            return true;
        }
    }
    return false;
}

std::vector<mparser::SourceUnit> choiceSources(std::string entry) {
    return {
        {"main.m", std::move(entry)},
        {"left/+left/choose.m",
         "function value = choose(input)\n"
         "    value = input + 10;\n"
         "end\n",
         "left"},
        {"right/+right/choose.m",
         "function value = choose(input)\n"
         "    value = input + 20;\n"
         "end\n",
         "right"},
    };
}

void runImportParserSmoke() {
    mparser::Lexer lexer(
        "import alpha.beta alpha.gamma.*\n"
        "value = 1;\n");
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    assert(parsed.diagnostics.empty());
    assert(parsed.root != nullptr);
    assert(parsed.root->children.size() == 2);
    const auto& import = *parsed.root->children[0];
    assert(import.kind == mparser::SyntaxKind::ImportStatement);
    assert(import.children.size() == 2);
    assert(import.children[0]->kind == mparser::SyntaxKind::ImportItem);
    assert(import.children[0]->label == "alpha.beta");
    assert(import.children[1]->label == "alpha.gamma.*");
}

void runQualifiedFunctionAndLocalHelperSmoke() {
    auto module = mparser::CompiledModule::compile({
        {"main.m", "result = mathops.scale(4);\n"},
        {"+mathops/scale.m",
         "function value = scale(input)\n"
         "    value = localDouble(input) + 1;\n"
         "end\n"
         "function value = localDouble(input)\n"
         "    value = input * 2;\n"
         "end\n",
         "mathops"},
    });
    assert(module.valid());
    assert(module.findFunction("mathops.scale") != nullptr);
    assert(module.findFunction("mathops.scale>localDouble") == nullptr);
    assert(hasFunctionSymbol(module, "mathops.scale"));
    assert(hasFunctionSymbol(module, "mathops.scale>localDouble"));

    auto bytecode = module.invoke();
    assert(bytecode.diagnostics.empty());
    assertNumber(bytecode, "result", 9);

    mparser::Interpreter interpreter;
    auto interpreted = interpreter.run(module.semantic());
    assert(interpreted.diagnostics.empty());
    assertNumber(interpreted, "result", 9);
}

void runNonFunctionFileLocalIdentitySmoke() {
    auto classFile = mparser::CompiledModule::compile({
        {"+pkg/Thing.m",
         "classdef Thing\n"
         "end\n"
         "function value = helper()\n"
         "    value = 1;\n"
         "end\n",
         "pkg"},
    });
    assert(classFile.valid());
    assert(classFile.findFunction("pkg.helper") == nullptr);
    assert(hasFunctionSymbol(classFile, "pkg.Thing>helper"));

    auto scriptFile = mparser::CompiledModule::compile({
        {"+pkg/run_script.m",
         "value = helper();\n"
         "function output = helper()\n"
         "    output = 7;\n"
         "end\n",
         "pkg"},
    });
    assert(scriptFile.valid());
    assert(scriptFile.findFunction("pkg.helper") == nullptr);
    assert(hasFunctionSymbol(scriptFile, "pkg.run_script>helper"));
    const auto runtime = scriptFile.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "value", 7);
}

void runImportPrecedenceAndScopeSmoke() {
    auto module = mparser::CompiledModule::compile(choiceSources(
        "value = choose(1);\n"
        "import left.choose\n"
        "import right.*\n"
        "function output = unscoped()\n"
        "    output = choose(2);\n"
        "end\n"));
    assert(module.valid());
    assert(findHirNode(module.semantic().root.get(),
                       mparser::HirKind::NameRef, "left.choose",
                       mparser::BindingKind::Function) != nullptr);
    assert(findHirNode(module.semantic().root.get(),
                       mparser::HirKind::NameRef, "choose",
                       mparser::BindingKind::Unresolved) != nullptr);

    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "value", 11);

    auto variableWins = mparser::CompiledModule::compile(choiceSources(
        "choose = 7;\n"
        "import left.choose\n"
        "value = choose;\n"));
    assert(variableWins.valid());
    const auto variableRuntime = variableWins.invoke();
    assert(variableRuntime.diagnostics.empty());
    assertNumber(variableRuntime, "value", 7);

    auto localWins = mparser::CompiledModule::compile(choiceSources(
        "value = choose(1);\n"
        "import right.*\n"
        "function output = choose(input)\n"
        "    output = input + 30;\n"
        "end\n"));
    assert(localWins.valid());
    const auto localRuntime = localWins.invoke();
    assert(localRuntime.diagnostics.empty());
    assertNumber(localRuntime, "value", 31);
}

void runImportDiagnosticSmoke() {
    auto conflicting = mparser::CompiledModule::compile(choiceSources(
        "import left.choose right.choose\n"
        "value = choose(1);\n"));
    assert(!conflicting.valid());
    assert(hasDiagnostic(conflicting,
                         "conflicting explicit imports for: choose"));

    auto ambiguous = mparser::CompiledModule::compile(choiceSources(
        "import left.* right.*\n"
        "value = choose(1);\n"));
    assert(!ambiguous.valid());
    assert(hasDiagnostic(ambiguous,
                         "ambiguous wildcard import for: choose"));

    auto missing = mparser::CompiledModule::compile(
        "import missing.member\nvalue = member(1);\n");
    assert(!missing.valid());
    assert(hasDiagnostic(missing,
                         "import target is not available: missing.member"));

    auto instanceMethod = mparser::CompiledModule::compile({
        {"main.m",
         "import pkg.Tool.value\nresult = value();\n",
         ""},
        {"+pkg/Tool.m",
         "classdef Tool\n"
         "    methods\n"
         "        function output = value(obj)\n"
         "            output = 1;\n"
         "        end\n"
         "    end\n"
         "end\n",
         "pkg"},
    });
    assert(!instanceMethod.valid());
    assert(hasDiagnostic(
        instanceMethod,
        "imported class method is not static: pkg.Tool.value"));
}

void runFilesystemNamespaceFunctionSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "run_demo.m";
    writeFile(entry,
              "qualified_value = mathops.scale(4);\n"
              "explicit_value = offset(5);\n"
              "obj = Tool(7);\n"
              "class_value = obj.Value;\n"
              "static_value = tag();\n"
              "wildcard_static_value = badge();\n"
              "wildcard_value = triple(3);\n"
              "nested_value = bias(2);\n"
              "import mathops.offset mathops.Tool mathops.Tool.tag\n"
              "import mathops.*\n"
              "import mathops.Tool.*\n"
              "import mathops.inner.*\n");
    writeFile(temporary.path / "+mathops" / "scale.m",
              "function value = scale(input)\n"
              "    value = localDouble(input) + 1;\n"
              "end\n"
              "function value = localDouble(input)\n"
              "    value = input * 2;\n"
              "end\n");
    writeFile(temporary.path / "+mathops" / "offset.m",
              "function value = offset(input)\n"
              "    value = input + 10;\n"
              "end\n");
    writeFile(temporary.path / "+mathops" / "triple.m",
              "function value = triple(input)\n"
              "    value = localDouble(input) + input;\n"
              "end\n"
              "function value = localDouble(input)\n"
              "    value = input * 2;\n"
              "end\n");
    writeFile(temporary.path / "+mathops" / "+inner" / "bias.m",
              "function value = bias(input)\n"
              "    value = input + 100;\n"
              "end\n");
    writeFile(temporary.path / "+mathops" / "Tool.m",
              "classdef Tool\n"
              "    properties\n"
              "        Value = 0\n"
              "    end\n"
              "    methods\n"
              "        function obj = Tool(value)\n"
              "            obj.Value = value;\n"
              "        end\n"
              "    end\n"
              "    methods (Static)\n"
              "        function value = tag()\n"
              "            value = 42;\n"
              "        end\n"
              "        function value = badge()\n"
              "            value = 43;\n"
              "        end\n"
              "    end\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(temporary.path);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 6);
    std::set<std::string> namespaces;
    for (const auto& source : loaded.sources) {
        namespaces.insert(source.namespaceName);
    }
    assert(namespaces.contains(""));
    assert(namespaces.contains("mathops"));
    assert(namespaces.contains("mathops.inner"));

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    for (const auto& diagnostic : module.diagnostics()) {
        std::cerr << module.sourceName(diagnostic.span) << ":"
                  << diagnostic.span.begin.line << ":"
                  << diagnostic.span.begin.column << ": "
                  << diagnostic.message << "\n";
    }
    assert(module.valid());
    assert(module.functions().size() == 4);
    assert(module.findFunction("mathops.scale") != nullptr);
    assert(module.findFunction("mathops.offset") != nullptr);
    assert(module.findFunction("mathops.triple") != nullptr);
    assert(module.findFunction("mathops.inner.bias") != nullptr);
    assert(hasFunctionSymbol(module, "mathops.scale>localDouble"));
    assert(hasFunctionSymbol(module, "mathops.triple>localDouble"));

    const auto runtime = module.invoke();
    for (const auto& diagnostic : runtime.diagnostics) {
        std::cerr << module.sourceName(diagnostic.span) << ":"
                  << diagnostic.span.begin.line << ":"
                  << diagnostic.span.begin.column << ": "
                  << diagnostic.message << "\n";
    }
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "qualified_value", 9);
    assertNumber(runtime, "explicit_value", 15);
    assertNumber(runtime, "class_value", 7);
    assertNumber(runtime, "static_value", 42);
    assertNumber(runtime, "wildcard_static_value", 43);
    assertNumber(runtime, "wildcard_value", 9);
    assertNumber(runtime, "nested_value", 102);
}

void runNamespaceFunctionPathPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto firstRoot = temporary.path / "first";
    const auto secondRoot = temporary.path / "second";
    const auto relativeFunction =
        std::filesystem::path("+selection") / "value.m";
    writeFile(entry, "selected = selection.value();\n");
    writeFile(firstRoot / relativeFunction,
              "function output = value()\n"
              "    output = 1;\n"
              "end\n");
    writeFile(secondRoot / relativeFunction,
              "function output = value()\n"
              "    output = 2;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths = {firstRoot, secondRoot};
    auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 2);
    assert(std::filesystem::equivalent(
        loaded.sources[1].name, firstRoot / relativeFunction));
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 1);

    options.searchPaths = {secondRoot, firstRoot};
    loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 2);
    assert(std::filesystem::equivalent(
        loaded.sources[1].name, secondRoot / relativeFunction));
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 2);
}

} // namespace

int main() {
    runImportParserSmoke();
    runQualifiedFunctionAndLocalHelperSmoke();
    runNonFunctionFileLocalIdentitySmoke();
    runImportPrecedenceAndScopeSmoke();
    runImportDiagnosticSmoke();
    runFilesystemNamespaceFunctionSmoke();
    runNamespaceFunctionPathPrecedenceSmoke();
    std::cout << "namespace function smoke tests passed\n";
    return 0;
}
