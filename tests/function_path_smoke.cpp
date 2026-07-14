#include "mparser/compiled_module.h"
#include "mparser/interpreter.h"
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
                ("mparser_function_path_" + std::to_string(nonce));
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

void assertBothRuntimes(const mparser::CompiledModule& module,
                        std::string_view name, double expected) {
    const auto bytecode = module.invoke();
    assert(bytecode.diagnostics.empty());
    assertNumber(bytecode, name, expected);

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(module.semantic());
    assert(interpreted.diagnostics.empty());
    assertNumber(interpreted, name, expected);
}

void runCurrentFolderPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto pathRoot = temporary.path / "path";
    writeFile(entry, "selected = choose();\n");
    writeFile(entry.parent_path() / "choose.m",
              "function value = choose()\n"
              "    value = 1;\n"
              "end\n");
    writeFile(pathRoot / "choose.m",
              "function value = choose()\n"
              "    value = 2;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(pathRoot);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 2);
    assert(std::filesystem::equivalent(
        loaded.sources[1].name, entry.parent_path() / "choose.m"));
    assert(loaded.sources[0].functionBindings.size() == 1);

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(module.findFunction("choose") == nullptr);
    assertBothRuntimes(module, "selected", 1);
}

void runSearchPathPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto firstRoot = temporary.path / "first";
    const auto secondRoot = temporary.path / "second";
    writeFile(entry, "selected = choose();\n");
    writeFile(firstRoot / "choose.m",
              "function value = choose()\n"
              "    value = 10;\n"
              "end\n");
    writeFile(secondRoot / "choose.m",
              "function value = choose()\n"
              "    value = 20;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths = {firstRoot, secondRoot};
    auto loaded = mparser::SourceLoader{}.load(entry, options);
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothRuntimes(module, "selected", 10);

    options.searchPaths = {secondRoot, firstRoot};
    loaded = mparser::SourceLoader{}.load(entry, options);
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothRuntimes(module, "selected", 20);
}

void runPrivateIsolationSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto firstLibrary = temporary.path / "first_library";
    const auto secondLibrary = temporary.path / "second_library";
    const auto globalLibrary = temporary.path / "global_library";
    writeFile(entry,
              "first_value = run_first(1);\n"
              "second_value = run_second(1);\n"
              "local_value = run_local(1);\n"
              "outside_value = adjust(1);\n");
    writeFile(firstLibrary / "run_first.m",
              "function value = run_first(input)\n"
              "    value = adjust(input);\n"
              "end\n");
    writeFile(secondLibrary / "run_second.m",
              "function value = run_second(input)\n"
              "    value = adjust(input);\n"
              "end\n");
    writeFile(firstLibrary / "run_local.m",
              "function value = run_local(input)\n"
              "    value = adjust(input);\n"
              "end\n"
              "function value = adjust(input)\n"
              "    value = input + 30;\n"
              "end\n");
    writeFile(firstLibrary / "private" / "adjust.m",
              "function value = adjust(input)\n"
              "    value = input + 10;\n"
              "end\n");
    writeFile(secondLibrary / "private" / "adjust.m",
              "function value = adjust(input)\n"
              "    value = input + 20;\n"
              "end\n");
    writeFile(globalLibrary / "adjust.m",
              "function value = adjust(input)\n"
              "    value = input + 100;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths = {firstLibrary, secondLibrary, globalLibrary};
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 7);

    std::set<std::string> privateIdentities;
    for (const auto& source : loaded.sources) {
        if (source.primaryFunctionIdentity.starts_with("$private")) {
            privateIdentities.insert(source.primaryFunctionIdentity);
        }
    }
    assert(privateIdentities.size() == 2);

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(module.functions().empty());
    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "first_value", 11);
    assertNumber(runtime, "second_value", 21);
    assertNumber(runtime, "local_value", 31);
    assertNumber(runtime, "outside_value", 101);

    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(module.semantic());
    assert(interpreted.diagnostics.empty());
    assertNumber(interpreted, "first_value", 11);
    assertNumber(interpreted, "second_value", 21);
    assertNumber(interpreted, "local_value", 31);
    assertNumber(interpreted, "outside_value", 101);
}

void runWildcardImportPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry,
              "selected = choose();\n"
              "import preferred.*\n");
    writeFile(library / "choose.m",
              "function value = choose()\n"
              "    value = 5;\n"
              "end\n");
    writeFile(library / "+preferred" / "choose.m",
              "function value = choose()\n"
              "    value = 9;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 3);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothRuntimes(module, "selected", 9);
}

void runEntryFunctionCatalogCompatibilitySmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "entry_functions.m";
    writeFile(entry,
              "function value = entry_functions()\n"
              "    value = -1;\n"
              "end\n"
              "function value = selected(input)\n"
              "    value = input + 5;\n"
              "end\n");

    const auto loaded = mparser::SourceLoader{}.load(entry);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(module.findFunction("entry_functions") != nullptr);
    assert(module.findFunction("selected") != nullptr);
}

} // namespace

int main() {
    runCurrentFolderPrecedenceSmoke();
    runSearchPathPrecedenceSmoke();
    runPrivateIsolationSmoke();
    runWildcardImportPrecedenceSmoke();
    runEntryFunctionCatalogCompatibilitySmoke();
    std::cout << "function path smoke tests passed\n";
    return 0;
}
