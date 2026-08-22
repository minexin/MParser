#include "mparser/compiled_module.h"
#include "mparser/source_loader.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

const std::string kEntrySource = R"(obj = CrossFileChild(5, 7);
base_code = obj.baseCode();
child_code = obj.childCode();
dynamic_code = obj.dispatchCode();
)";

const std::string kChildSource = R"(classdef CrossFileChild < CrossFileBase
    properties (Access = private)
        Value = 2
    end
    methods
        function obj = CrossFileChild(baseValue, childValue)
            obj = obj@CrossFileBase(baseValue);
            obj.Value = childValue;
        end
        function value = childCode(obj)
            value = obj.code();
        end
        function value = step(obj)
            value = 2;
        end
    end
    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 200;
        end
    end
end
)";

const std::string kBaseSource = R"(classdef CrossFileBase
    properties (Access = private)
        Value = 1
    end
    methods
        function obj = CrossFileBase(value)
            obj.Value = value;
        end
        function value = baseCode(obj)
            value = obj.code();
        end
        function value = dispatchCode(obj)
            value = obj.step();
        end
        function value = step(obj)
            value = 1;
        end
    end
    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 100;
        end
    end
end
)";

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

const mparser::SemanticSymbol* findClassSymbol(
    const mparser::CompiledModule& module, std::string_view name) {
    for (const auto& symbol : module.semantic().symbols) {
        if (symbol.kind == mparser::SymbolKind::Class &&
            symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

const mparser::HirNode* findHirNode(const mparser::HirNode* node,
                                   mparser::HirKind kind,
                                   std::string_view label) {
    if (!node) {
        return nullptr;
    }
    if (node->kind == kind && node->label == label) {
        return node;
    }
    for (const auto& child : node->children) {
        if (const auto* match = findHirNode(child.get(), kind, label)) {
            return match;
        }
    }
    return nullptr;
}

std::vector<mparser::SourceUnit> crossFileSources() {
    return {
        {"main.m", kEntrySource},
        {"CrossFileChild.m", kChildSource},
        {"CrossFileBase.m", kBaseSource},
    };
}

void runMultiSourceCompilationSmoke() {
    const auto module =
        mparser::CompiledModule::compile(crossFileSources());
    assert(module.valid());
    assert(module.diagnostics().empty());
    assert(module.sources().size() == 3);
    assert(module.source() == kEntrySource);
    assert(module.sourceName(0) == "main.m");
    assert(module.sourceName(1) == "CrossFileChild.m");
    assert(module.sourceName(2) == "CrossFileBase.m");

    const auto* child = findClassSymbol(module, "CrossFileChild");
    const auto* base = findClassSymbol(module, "CrossFileBase");
    assert(child != nullptr && child->span.begin.sourceId == 1);
    assert(base != nullptr && base->span.begin.sourceId == 2);

    mparser::BytecodeVmOptions options;
    options.profiling = mparser::BytecodeVmProfilingMode::Disabled;
    const auto runtime = module.invoke(options);
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "base_code", 105);
    assertNumber(runtime, "child_code", 207);
    assertNumber(runtime, "dynamic_code", 2);
}

void runMultiSourceDiagnosticSmoke() {
    auto module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"main.m", "value = 1;\n"},
            {"Broken.m", "classdef Broken\nproperties\nValue(\nend\nend\n"},
        });
    assert(!module.valid());
    assert(!module.diagnostics().empty());
    assert(module.diagnostics().front().span.begin.sourceId == 1);
    assert(module.sourceName(module.diagnostics().front().span) ==
           "Broken.m");

    module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"First.m", "classdef DuplicateClass\nend\n"},
            {"Second.m", "classdef DuplicateClass\nend\n"},
        });
    assert(!module.valid());
    bool foundDuplicate = false;
    for (const auto& diagnostic : module.diagnostics()) {
        if (diagnostic.message ==
            "duplicate top-level class: DuplicateClass") {
            foundDuplicate = true;
            assert(diagnostic.span.begin.sourceId == 1);
            assert(module.sourceName(diagnostic.span) == "Second.m");
        }
    }
    assert(foundDuplicate);

    module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"Left.m", "classdef SharedName\nend\n", "leftpkg"},
            {"Right.m", "classdef SharedName\nend\n", "rightpkg"},
        });
    assert(module.valid());
    assert(findClassSymbol(module, "leftpkg.SharedName") != nullptr);
    assert(findClassSymbol(module, "rightpkg.SharedName") != nullptr);

    module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"First.m", "classdef SharedName\nend\n", "samepkg"},
            {"Second.m", "classdef SharedName\nend\n", "samepkg"},
        });
    assert(!module.valid());
    bool foundQualifiedDuplicate = false;
    for (const auto& diagnostic : module.diagnostics()) {
        if (diagnostic.message ==
            "duplicate top-level class: samepkg.SharedName") {
            foundQualifiedDuplicate = true;
            assert(module.sourceName(diagnostic.span) == "Second.m");
        }
    }
    assert(foundQualifiedDuplicate);

    module = mparser::CompiledModule::compile(
        std::vector<mparser::SourceUnit>{
            {"main.m",
             "shadowpkg = 1;\nvalue = shadowpkg.Shadowed;\n"},
            {"Shadowed.m", "classdef Shadowed\nend\n", "shadowpkg"},
        });
    assert(module.valid());
    const auto* shadowed = findHirNode(
        module.semantic().root.get(), mparser::HirKind::MemberAccess,
        "Shadowed");
    assert(shadowed != nullptr);
    assert(shadowed->binding.kind == mparser::BindingKind::Unresolved);
    assert(!shadowed->children.empty());
    assert(shadowed->children.front()->binding.kind ==
           mparser::BindingKind::LocalVariable);
}

struct TemporaryDirectory {
    std::filesystem::path path;

    explicit TemporaryDirectory(std::filesystem::path value)
        : path(std::move(value)) {}

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    TemporaryDirectory(TemporaryDirectory&& other) noexcept
        : path(std::move(other.path)) {
        other.path.clear();
    }

    ~TemporaryDirectory() {
        if (path.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeFile(const std::filesystem::path& path,
               std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("failed to create test source: " +
                                 path.string());
    }
    output << content;
}

TemporaryDirectory makeTemporaryDirectory() {
    const auto suffix = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    TemporaryDirectory temporary(
        std::filesystem::temp_directory_path() /
        ("mparser_source_loader_" + std::to_string(suffix)));
    std::filesystem::create_directories(temporary.path);
    return temporary;
}

std::string renameClass(std::string source, std::string_view from,
                        std::string_view to) {
    size_t position = 0;
    while ((position = source.find(from, position)) != std::string::npos) {
        source.replace(position, from.size(), to);
        position += to.size();
    }
    return source;
}

void runFileDependencyLoadingSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entryDirectory = temporary.path / "entry";
    const auto classDirectory = temporary.path / "classes";
    const auto entryPath = entryDirectory / "main.m";

    const std::string entrySource = renameClass(
        kEntrySource, "CrossFileChild", "LoadedChild");
    std::string childSource = renameClass(
        kChildSource, "CrossFileChild", "LoadedChild");
    childSource = renameClass(
        std::move(childSource), "CrossFileBase", "LoadedBase");
    const std::string baseSource = renameClass(
        kBaseSource, "CrossFileBase", "LoadedBase");

    writeFile(entryPath, entrySource);
    writeFile(entryDirectory / "LoadedChild.m", childSource);
    writeFile(classDirectory / "LoadedBase.m", baseSource);
    writeFile(entryDirectory / "Unused.m",
              "classdef Unused\nproperties\nBroken(\nend\nend\n");

    mparser::SourceLoader loader;
    mparser::SourceLoaderOptions loaderOptions;
    loaderOptions.searchPaths.push_back(classDirectory);
    const auto loaded = loader.load(entryPath, loaderOptions);
    assert(loaded.sources.size() == 3);
    assert(std::filesystem::path(loaded.sources[0].name).filename() ==
           "main.m");
    assert(std::filesystem::path(loaded.sources[1].name).filename() ==
           "LoadedChild.m");
    assert(std::filesystem::path(loaded.sources[2].name).filename() ==
           "LoadedBase.m");

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "base_code", 105);
    assertNumber(runtime, "child_code", 207);
    assertNumber(runtime, "dynamic_code", 2);
}

void runStaticReflectionDependencySmoke() {
    // Generalizes cap_195, cap_198b, and cap_199 across all static class-name
    // reflection entry points rather than relying on constructor references.
    auto temporary = makeTemporaryDirectory();
    const auto entryDirectory = temporary.path / "entry";
    const auto classDirectory = temporary.path / "classes";
    const auto entryPath = entryDirectory / "main.m";

    writeFile(entryPath, R"(colors = enumeration('LoaderColor');
class_info = meta.class.fromName('LoaderPoint');
legacy_class_info = matlab.metadata.Class.fromName('LoaderPoint');
method_names = methods('LoaderPoint');
property_names = properties("LoaderPoint");
event_names = events('LoaderPulse');
if false
    unused_full_methods = methods('LoaderOptional', '-full');
end
summary = numel(colors) + strcmp(class_info.Name, 'LoaderPoint') + ...
    strcmp(legacy_class_info.Name, 'LoaderPoint') + ...
    any(strcmp(method_names, 'norm2')) + ...
    any(strcmp(property_names, 'x')) + ...
    any(strcmp(event_names, 'Ticked'));
)");
    writeFile(classDirectory / "LoaderColor.m", R"(classdef LoaderColor
    enumeration
        Red
        Green
        Blue
    end
end
)");
    writeFile(classDirectory / "LoaderPoint.m", R"(classdef LoaderPoint
    properties
        x = 0
        y = 0
    end
    methods
        function value = norm2(obj)
            value = sqrt(obj.x^2 + obj.y^2);
        end
    end
end
)");
    writeFile(classDirectory / "LoaderPulse.m", R"(classdef LoaderPulse < handle
    events
        Ticked
    end
end
)");
    writeFile(classDirectory / "LoaderOptional.m", R"(classdef LoaderOptional
    methods
        function value = marker(~)
            value = 1;
        end
    end
end
)");

    mparser::SourceLoaderOptions options;
    options.searchPaths.push_back(classDirectory);
    const auto loaded = mparser::SourceLoader{}.load(entryPath, options);
    assert(loaded.sources.size() == 5);

    std::set<std::string> loadedFiles;
    for (const auto& source : loaded.sources) {
        loadedFiles.insert(
            std::filesystem::path(source.name).filename().string());
    }
    assert(loadedFiles.contains("LoaderColor.m"));
    assert(loadedFiles.contains("LoaderPoint.m"));
    assert(loadedFiles.contains("LoaderPulse.m"));
    assert(loadedFiles.contains("LoaderOptional.m"));

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    const auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "summary", 8);
}

void runNamespaceClassLoadingSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entryPath = temporary.path / "entry" / "main.m";

    writeFile(entryPath, R"(obj = pkgchild.Counter(5, 7);
base_code = obj.baseCode();
child_code = obj.childCode();
dynamic_code = obj.dispatchCode();
static_code = pkgchild.Counter.tag();
nested_code = pkgchild.inner.Marker.code();
other = pkgsibling.Counter(9);
other_code = other.code();
)");
    writeFile(temporary.path / "+pkgbase" / "CounterBase.m",
              R"(classdef (AllowedSubclasses = ?pkgchild.Counter) CounterBase
    properties (Access = private)
        Value = 1
    end
    methods
        function obj = CounterBase(value)
            obj.Value = value;
        end
        function value = baseCode(obj)
            value = obj.code();
        end
        function value = dispatchCode(obj)
            value = obj.step();
        end
        function value = step(obj)
            value = 1;
        end
    end
    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 100;
        end
    end
end
)");
    writeFile(temporary.path / "+pkgchild" / "Counter.m",
              R"(classdef Counter < pkgbase.CounterBase
    properties (Access = private)
        Value = 2
    end
    methods
        function obj = Counter(baseValue, childValue)
            obj = obj@pkgbase.CounterBase(baseValue);
            obj.Value = childValue;
        end
        function value = childCode(obj)
            value = obj.code();
        end
        function value = step(obj)
            value = 2;
        end
    end
    methods (Access = private)
        function value = code(obj)
            value = obj.Value + 200;
        end
    end
    methods (Static)
        function value = tag()
            value = 35;
        end
    end
end
)");
    writeFile(temporary.path / "+pkgsibling" / "Counter.m",
              R"(classdef Counter
    properties
        Value = 0
    end
    methods
        function obj = Counter(value)
            obj.Value = value;
        end
        function value = code(obj)
            value = obj.Value + 300;
        end
    end
end
)");
    writeFile(temporary.path / "+pkgchild" / "+inner" / "Marker.m",
              R"(classdef Marker
    methods (Static)
        function value = code()
            value = 44;
        end
    end
end
)");

    mparser::SourceLoaderOptions loaderOptions;
    loaderOptions.searchPaths.push_back(temporary.path);
    const auto loaded =
        mparser::SourceLoader{}.load(entryPath, loaderOptions);
    assert(loaded.sources.size() == 5);

    std::set<std::string> namespaces;
    for (const auto& source : loaded.sources) {
        namespaces.insert(source.namespaceName);
    }
    assert(namespaces.contains(""));
    assert(namespaces.contains("pkgbase"));
    assert(namespaces.contains("pkgchild"));
    assert(namespaces.contains("pkgchild.inner"));
    assert(namespaces.contains("pkgsibling"));

    const auto module =
        mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    assert(findClassSymbol(module, "pkgbase.CounterBase") != nullptr);
    assert(findClassSymbol(module, "pkgchild.Counter") != nullptr);
    assert(findClassSymbol(module, "pkgchild.inner.Marker") != nullptr);
    assert(findClassSymbol(module, "pkgsibling.Counter") != nullptr);

    const auto runtime = module.invoke();
    for (const auto& diagnostic : runtime.diagnostics) {
        std::cerr << module.sourceName(diagnostic.span) << ":"
                  << diagnostic.span.begin.line << ":"
                  << diagnostic.span.begin.column << ": "
                  << diagnostic.message << "\n";
    }
    if (!runtime.diagnostics.empty()) {
        throw std::runtime_error("namespace class runtime failed");
    }
    assertNumber(runtime, "base_code", 105);
    assertNumber(runtime, "child_code", 207);
    assertNumber(runtime, "dynamic_code", 2);
    assertNumber(runtime, "static_code", 35);
    assertNumber(runtime, "nested_code", 44);
    assertNumber(runtime, "other_code", 309);
}

void runNamespacePathPrecedenceSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entryPath = temporary.path / "app" / "main.m";
    const auto firstRoot = temporary.path / "first";
    const auto secondRoot = temporary.path / "second";
    const auto relativeClass =
        std::filesystem::path("+precedence") / "Choice.m";

    writeFile(entryPath,
              "selected = precedence.Choice.code();\n");
    writeFile(firstRoot / relativeClass,
              "classdef Choice\n"
              "    methods (Static)\n"
              "        function value = code()\n"
              "            value = 1;\n"
              "        end\n"
              "    end\n"
              "end\n");
    writeFile(secondRoot / relativeClass,
              "classdef Choice\n"
              "    methods (Static)\n"
              "        function value = code()\n"
              "            value = 2;\n"
              "        end\n"
              "    end\n"
              "end\n");

    mparser::SourceLoaderOptions options;
    options.searchPaths = {firstRoot, secondRoot};
    auto loaded = mparser::SourceLoader{}.load(entryPath, options);
    assert(loaded.sources.size() == 2);
    assert(std::filesystem::equivalent(
        loaded.sources[1].name, firstRoot / relativeClass));
    auto module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    auto runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 1);

    options.searchPaths = {secondRoot, firstRoot};
    loaded = mparser::SourceLoader{}.load(entryPath, options);
    assert(loaded.sources.size() == 2);
    assert(std::filesystem::equivalent(
        loaded.sources[1].name, secondRoot / relativeClass));
    module = mparser::CompiledModule::compile(loaded.sources);
    assert(module.valid());
    runtime = module.invoke();
    assert(runtime.diagnostics.empty());
    assertNumber(runtime, "selected", 2);
}

} // namespace

int main() {
    runMultiSourceCompilationSmoke();
    runMultiSourceDiagnosticSmoke();
    runFileDependencyLoadingSmoke();
    runStaticReflectionDependencySmoke();
    runNamespaceClassLoadingSmoke();
    runNamespacePathPrecedenceSmoke();
    std::cout << "source loader smoke tests passed\n";
    return 0;
}
