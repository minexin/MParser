#include "mparser/compiled_module.h"
#include "mparser/source_loader.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    loaderOptions.classPaths.push_back(classDirectory);
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

} // namespace

int main() {
    runMultiSourceCompilationSmoke();
    runMultiSourceDiagnosticSmoke();
    runFileDependencyLoadingSmoke();
    std::cout << "source loader smoke tests passed\n";
    return 0;
}
