#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/source_loader.h"
#include "mparser/runtime/io/runtime_system.h"

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

std::shared_ptr<mparser::RuntimeSessionState> dynamicSession() {
    mparser::RuntimeSystemContextOptions options;
    options.capabilities =
        mparser::RuntimeSystemCapability::DynamicEvaluation;
    return std::make_shared<mparser::RuntimeSessionState>(
        std::make_shared<mparser::RuntimeSystemContext>(
            std::move(options)));
}

void assertBothDynamicRuntimes(
    const mparser::CompiledModule& module,
    const std::vector<std::pair<std::string_view, double>>& expected) {
    mparser::BytecodeVmOptions vmOptions;
    vmOptions.sessionState = dynamicSession();
    const auto bytecode = module.invoke(vmOptions);
    assert(bytecode.diagnostics.empty());
    for (const auto& [name, value] : expected) {
        assertNumber(bytecode, name, value);
    }

    mparser::InterpreterOptions interpreterOptions;
    interpreterOptions.sessionState = dynamicSession();
    mparser::Interpreter interpreter;
    const auto interpreted =
        interpreter.run(module.semantic(), interpreterOptions);
    assert(interpreted.diagnostics.empty());
    for (const auto& [name, value] : expected) {
        assertNumber(interpreted, name, value);
    }
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

void runLiteralDynamicDependencySmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry,
              "shadowed_builtin = feval('sin', 2);\n"
              "package_handle = str2func('tools.shift');\n"
              "package_value = package_handle(3);\n");
    writeFile(library / "sin.m",
              "function value = sin(input)\n"
              "    value = input + 100;\n"
              "end\n");
    writeFile(library / "+tools" / "shift.m",
              "function value = shift(input)\n"
              "    value = input + 20;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 3);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothRuntimes(module, "shadowed_builtin", 102);
    assertBothRuntimes(module, "package_value", 23);
}

void runPrivateTextBoundarySmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto library = temporary.path / "library";
    writeFile(library / "private" / "secret.m",
              "function value = secret(input)\n"
              "    value = input + 10;\n"
              "end\n");
    writeFile(library / "owner_authorized.m",
              "function value = owner_authorized(input)\n"
              "    handle = @secret;\n"
              "    value = handle(input);\n"
              "end\n");
    writeFile(library / "owner_denied.m",
              "function value = owner_denied(input)\n"
              "    handle = str2func('secret');\n"
              "    value = handle(input);\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);

    const auto allowedEntry = temporary.path / "allowed" / "main.m";
    writeFile(allowedEntry, "value = owner_authorized(1);\n");
    auto loaded = mparser::SourceLoader{}.load(allowedEntry, options);
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothRuntimes(module, "value", 11);

    const auto deniedEntry = temporary.path / "denied" / "main.m";
    writeFile(deniedEntry, "value = owner_denied(1);\n");
    loaded = mparser::SourceLoader{}.load(deniedEntry, options);
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());

    const auto bytecode = module.invoke();
    assert(hasDiagnostic(bytecode.diagnostics,
                         "cannot resolve a private or local function"));
    mparser::Interpreter interpreter;
    const auto interpreted = interpreter.run(module.semantic());
    assert(hasDiagnostic(interpreted.diagnostics,
                         "function name string is not available"));
}

void runDynamicEvaluationSourceGraphSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(
        entry,
        "path_anchor = @path_shift;\n"
        "package_anchor = @tools.shift;\n"
        "sin_anchor = @sin;\n"
        "path_direct = eval('path_shift(1)');\n"
        "package_direct = eval('tools.shift(2)');\n"
        "builtin_shadow = eval('sin(3)');\n"
        "path_created = eval('@path_shift');\n"
        "path_created_value = path_created(4);\n"
        "package_created = eval('@tools.shift');\n"
        "package_created_value = package_created(5);\n"
        "path_text = eval('str2func(''path_shift'')');\n"
        "path_text_value = path_text(6);\n"
        "package_text = eval('str2func(''tools.shift'')');\n"
        "package_text_value = package_text(7);\n"
        "private_direct = owner_dynamic(8);\n"
        "private_outside_caught = false;\n"
        "try\n"
        "    eval('secret(1)');\n"
        "catch\n"
        "    private_outside_caught = true;\n"
        "end\n"
        "private_handle_outside_caught = false;\n"
        "try\n"
        "    eval('@secret');\n"
        "catch\n"
        "    private_handle_outside_caught = true;\n"
        "end\n");
    writeFile(library / "path_shift.m",
              "function value = path_shift(input)\n"
              "    value = input + 10;\n"
              "end\n");
    writeFile(library / "+tools" / "shift.m",
              "function value = shift(input)\n"
              "    value = input + 20;\n"
              "end\n");
    writeFile(library / "sin.m",
              "function value = sin(input)\n"
              "    value = input + 100;\n"
              "end\n");
    writeFile(library / "owner_dynamic.m",
              "function value = owner_dynamic(input)\n"
              "    dependency = @secret;\n"
              "    value = eval('secret(input)');\n"
              "end\n");
    writeFile(library / "private" / "secret.m",
              "function value = secret(input)\n"
              "    value = input + 30;\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 6);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assertBothDynamicRuntimes(
        module,
        {{"path_direct", 11},
         {"package_direct", 22},
         {"builtin_shadow", 103},
         {"path_created_value", 14},
         {"package_created_value", 25},
         {"path_text_value", 16},
         {"package_text_value", 27},
         {"private_direct", 38},
         {"private_outside_caught", 1},
         {"private_handle_outside_caught", 1}});
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
    runLiteralDynamicDependencySmoke();
    runPrivateTextBoundarySmoke();
    runDynamicEvaluationSourceGraphSmoke();
    runEntryFunctionCatalogCompatibilitySmoke();
    std::cout << "function path smoke tests passed\n";
    return 0;
}
