#include "mparser/embedding/compiled_module.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/source_loader.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

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
                ("mparser_exception_graph_" + std::to_string(nonce));
    std::filesystem::create_directories(path);
    return TemporaryDirectory{std::move(path)};
}

void writeFile(const std::filesystem::path& path,
               std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output),
            "could not create source-graph fixture");
    output << content;
    require(output.good(), "could not write source-graph fixture");
}

template <typename Result>
void verifyCrossFileDiagnostic(const Result& result) {
    require(result.diagnostics.size() == 1,
            "cross-file failure did not produce one diagnostic");
    const auto& diagnostic = result.diagnostics.front();
    require(diagnostic.severity == mparser::DiagnosticSeverity::Error &&
                diagnostic.identifier == "Graph:Failure" &&
                diagnostic.message == "cross-file failure",
            "cross-file diagnostic metadata mismatch");
    require(diagnostic.stack.size() == 3,
            "cross-file diagnostic stack depth mismatch");
    require(diagnostic.stack[0].name == "inner_failure" &&
                diagnostic.stack[0].line == 2 &&
                std::filesystem::path(diagnostic.stack[0].file)
                        .filename() == "inner_failure.m",
            "cross-file leaf frame mismatch");
    require(diagnostic.stack[1].name == "outer_failure" &&
                diagnostic.stack[1].line == 2 &&
                std::filesystem::path(diagnostic.stack[1].file)
                        .filename() == "outer_failure.m",
            "cross-file caller frame mismatch");
    require(diagnostic.stack[2].name == "<script>" &&
                diagnostic.stack[2].line == 1 &&
                std::filesystem::path(diagnostic.stack[2].file)
                        .filename() == "main.m",
            "cross-file entry-script frame mismatch");
}

void runSourceGraphSmoke() {
    auto temporary = makeTemporaryDirectory();
    const auto entry = temporary.path / "main.m";
    writeFile(entry, "result = outer_failure();\n");
    writeFile(temporary.path / "outer_failure.m",
              "function value = outer_failure()\n"
              "value = inner_failure();\n"
              "end\n");
    writeFile(temporary.path / "inner_failure.m",
              "function value = inner_failure()\n"
              "error(\"Graph:Failure\", \"cross-file failure\");\n"
              "value = 0;\n"
              "end\n");

    const auto loaded = mparser::SourceLoader{}.load(entry);
    require(loaded.sources.size() == 3,
            "source loader omitted a cross-file exception dependency");

    const auto module = mparser::CompiledModule::compile(loaded.sources);
    require(module.valid(),
            "cross-file exception module did not compile");
    const auto bytecode = module.invoke();
    verifyCrossFileDiagnostic(bytecode);
    require(std::filesystem::path(
                module.sourceName(bytecode.diagnostics.front().span))
                    .filename() == "inner_failure.m",
            "compiled-module diagnostic span did not identify leaf source");

    mparser::Interpreter interpreter;
    verifyCrossFileDiagnostic(interpreter.run(module.semantic()));
}

} // namespace

int main() {
    try {
        runSourceGraphSmoke();
        std::cout << "exception source-graph smoke tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Exception source-graph smoke failure: "
                  << error.what() << '\n';
        return 1;
    }
}
