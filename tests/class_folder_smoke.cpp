#include "mparser/compiled_module.h"
#include "mparser/source_loader.h"

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
                ("mparser_class_folder_" + std::to_string(nonce));
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

const mparser::SemanticSymbol* findSymbolInScope(
    const mparser::CompiledModule& module, std::string_view scopeName,
    mparser::SymbolKind kind, std::string_view symbolName) {
    int scopeId = -1;
    for (const auto& scope : module.semantic().scopes) {
        if (scope.label == scopeName) {
            scopeId = scope.id;
            break;
        }
    }
    for (const auto& symbol : module.semantic().symbols) {
        if (symbol.scopeId == scopeId && symbol.kind == kind &&
            symbol.name == symbolName) {
            return &symbol;
        }
    }
    return nullptr;
}

void writeCounterClass(const std::filesystem::path& root) {
    const auto folder = root / "+folderpkg" / "@Counter";
    writeFile(folder / "Counter.m", R"(classdef Counter
    properties (Access = private)
        Value = 0
    end
    methods
        function obj = Counter(value)
            obj.Value = value;
        end
        result = scale(obj, factor)
        result = reveal(obj)
    end
    methods (Static)
        result = twice(value)
    end
    methods (Access = private)
        result = secret(obj)
    end
end
)");
    writeFile(folder / "scale.m", R"(function result = scale(obj, factor)
    result = localMultiply(obj.Value, factor);
end
function result = localMultiply(left, right)
    result = left * right;
end
)");
    writeFile(folder / "reveal.m", R"(function result = reveal(obj)
    result = obj.secret();
end
)");
    writeFile(folder / "secret.m", R"(function result = secret(obj)
    result = obj.Value + 100;
end
)");
    writeFile(folder / "offset.m", R"(function result = offset(obj, amount)
    result = obj.Value + amount;
end
)");
    writeFile(folder / "twice.m", R"(function result = twice(value)
    result = value * 2;
end
)");
}

void runClassFolderExecutionSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(counter = folderpkg.Counter(7);
scaled = counter.scale(3);
revealed = counter.reveal();
offset_value = counter.offset(5);
static_value = folderpkg.Counter.twice(6);
)");
    writeCounterClass(library);

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 7);
    size_t methodSources = 0;
    for (const auto& source : loaded.sources) {
        if (!source.classMethodOwner.empty()) {
            ++methodSources;
            assert(source.classMethodOwner == "folderpkg.Counter");
            assert(source.namespaceName == "folderpkg");
        }
    }
    assert(methodSources == 5);

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(module.functions().empty());
    assert(findSymbolInScope(
               module, "folderpkg.Counter.scale>localMultiply",
               mparser::SymbolKind::FunctionParameter, "left") != nullptr);
    const auto* staticValue = findSymbolInScope(
        module, "twice", mparser::SymbolKind::FunctionParameter, "value");
    assert(staticValue != nullptr);
    assert(staticValue->typeName.empty());
    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "scaled", 21);
    assertNumber(runtime, "revealed", 107);
    assertNumber(runtime, "offset_value", 12);
    assertNumber(runtime, "static_value", 12);
}

void writeChoiceClass(const std::filesystem::path& root, int value) {
    const auto folder = root / "@Choice";
    writeFile(folder / "Choice.m",
              "classdef Choice\n"
              "    methods (Static)\n"
              "        value = code()\n"
              "    end\n"
              "end\n");
    writeFile(folder / "code.m",
              "function value = code()\n"
              "    value = " + std::to_string(value) + ";\n"
              "end\n");
}

void runClassFolderPathPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto first = temporary.path / "first";
    const auto second = temporary.path / "second";
    writeFile(entry, "selected = Choice.code();\n");
    writeChoiceClass(first, 1);
    writeChoiceClass(second, 2);

    mparser::SourceLoaderOptions options;
    options.searchPaths = {first, second};
    auto loaded = mparser::SourceLoader{}.load(entry, options);
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 1);

    options.searchPaths = {second, first};
    loaded = mparser::SourceLoader{}.load(entry, options);
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 2);
}

void runClassFolderInheritanceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(obj = FolderChild();
direct_value = obj.code();
dynamic_value = obj.dispatch();
)");

    const auto baseFolder = library / "@FolderBase";
    writeFile(baseFolder / "FolderBase.m", R"(classdef FolderBase
    methods
        function obj = FolderBase()
        end
        value = code(obj)
        function value = dispatch(obj)
            value = obj.code();
        end
    end
end
)");
    writeFile(baseFolder / "code.m", R"(function value = code(obj)
    value = 1;
end
)");

    const auto childFolder = library / "@FolderChild";
    writeFile(childFolder / "FolderChild.m", R"(classdef FolderChild < FolderBase
    methods
        function obj = FolderChild()
            obj = obj@FolderBase();
        end
        value = code(obj)
    end
end
)");
    writeFile(childFolder / "code.m", R"(function value = code(obj)
    value = 2;
end
)");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    assert(loaded.sources.size() == 5);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "direct_value", 2);
    assertNumber(runtime, "dynamic_value", 2);
}

void runClassFolderAccessSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, R"(counter = folderpkg.Counter(7);
forbidden = counter.secret();
)");
    writeCounterClass(library);

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    const auto loaded = mparser::SourceLoader{}.load(entry, options);
    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(hasDiagnostic(runtime.diagnostics,
                         "method access is denied"));
}

void runClassFolderDiagnosticSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "app" / "main.m";
    const auto library = temporary.path / "library";
    writeFile(entry, "obj = Broken();\n");
    const auto folder = library / "@Broken";
    writeFile(folder / "Broken.m", R"(classdef Broken
    methods
        function obj = Broken()
        end
        value = compute(obj, input)
    end
end
)");
    writeFile(folder / "compute.m", R"(function value = compute(obj)
    value = 1;
end
)");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(library);
    auto loaded = mparser::SourceLoader{}.load(entry, options);
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(!module.valid());
    assert(hasDiagnostic(
        module.diagnostics(),
        "separate method signature does not match its declaration"));

    writeFile(folder / "compute.m", R"(function value = compute(obj, input)
    value = input;
end
)");
    writeFile(folder / "extra.m", R"(function value = different(obj)
    value = 2;
end
)");
    loaded = mparser::SourceLoader{}.load(entry, options);
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(!module.valid());
    assert(hasDiagnostic(
        module.diagnostics(),
        "class method file name must match its primary function"));

    auto duplicateTemporary = makeTemporaryDirectory();
    const auto duplicateEntry =
        duplicateTemporary.path / "app" / "main.m";
    const auto duplicateLibrary =
        duplicateTemporary.path / "library";
    const auto duplicateFolder = duplicateLibrary / "@Duplicate";
    writeFile(duplicateEntry, "obj = Duplicate();\n");
    writeFile(duplicateFolder / "Duplicate.m", R"(classdef Duplicate
    methods
        function obj = Duplicate()
        end
        function value = code(obj)
            value = 1;
        end
    end
end
)");
    writeFile(duplicateFolder / "code.m", R"(function value = code(obj)
    value = 2;
end
)");
    options.searchPaths = {duplicateLibrary};
    loaded = mparser::SourceLoader{}.load(duplicateEntry, options);
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(!module.valid());
    assert(hasDiagnostic(module.diagnostics(),
                         "duplicate method implementation"));
}

} // namespace

int main() {
    runClassFolderExecutionSmoke();
    runClassFolderPathPrecedenceSmoke();
    runClassFolderInheritanceSmoke();
    runClassFolderAccessSmoke();
    runClassFolderDiagnosticSmoke();
    std::cout << "class folder smoke tests passed\n";
    return 0;
}
