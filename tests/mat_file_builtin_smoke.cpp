#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/session/runtime_session_state.h"
#include "mparser/runtime/io/runtime_system.h"
#include "mparser/semantic/semantic.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

class TemporaryTree {
public:
  TemporaryTree() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("mparser-mat-builtin-" + std::to_string(stamp));
    interpreter = root / "interpreter";
    bytecode = root / "bytecode";
    std::filesystem::create_directories(interpreter);
    std::filesystem::create_directories(bytecode);
  }

  ~TemporaryTree() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  std::filesystem::path root;
  std::filesystem::path interpreter;
  std::filesystem::path bytecode;
};

struct RuntimePair {
  mparser::InterpreterResult interpreter;
  mparser::BytecodeVmResult bytecode;
};

std::shared_ptr<mparser::RuntimeSessionState>
session(const std::filesystem::path &directory,
        mparser::RuntimeSystemCapability capabilities) {
  mparser::RuntimeSystemContextOptions options;
  options.capabilities = capabilities;
  options.currentDirectory = directory;
  options.maximumFileReadBytes = 4U * 1024U * 1024U;
  return std::make_shared<mparser::RuntimeSessionState>(
      std::make_shared<mparser::RuntimeSystemContext>(std::move(options)));
}

RuntimePair runBoth(std::string_view source, const TemporaryTree &tree,
                    mparser::RuntimeSystemCapability capabilities =
                        mparser::RuntimeSystemCapability::CurrentDirectory |
                        mparser::RuntimeSystemCapability::SearchPaths |
                        mparser::RuntimeSystemCapability::FileSystemRead |
                        mparser::RuntimeSystemCapability::FileSystemWrite) {
  mparser::Lexer lexer(source);
  mparser::Parser parser(lexer.lex());
  auto parse = parser.parse();
  require(parse.diagnostics.empty(), "MAT builtin source did not parse");

  mparser::SemanticAnalyzer analyzer;
  auto semantic = analyzer.analyze(*parse.root);
  require(semantic.diagnostics.empty(),
          "MAT builtin source failed semantic analysis");

  mparser::BytecodeLowerer lowerer;
  auto lowered = lowerer.lower(semantic);
  require(lowered.diagnostics.empty(), "MAT builtin source did not lower");

  mparser::InterpreterOptions interpreterOptions;
  interpreterOptions.sessionState = session(tree.interpreter, capabilities);
  mparser::Interpreter interpreter;
  auto interpreted = interpreter.run(semantic, interpreterOptions);

  mparser::BytecodeVmOptions bytecodeOptions;
  bytecodeOptions.sessionState = session(tree.bytecode, capabilities);
  mparser::BytecodeVm vm;
  auto executed = vm.run(lowered, semantic, bytecodeOptions);
  return {std::move(interpreted), std::move(executed)};
}

template <typename Result>
const mparser::RuntimeValue &variable(const Result &result,
                                      std::string_view name) {
  for (const auto &candidate : result.variables) {
    if (candidate.name == name) {
      return candidate.value;
    }
  }
  throw std::runtime_error("missing MAT builtin variable: " +
                           std::string(name));
}

template <typename Result>
void requireNumber(const Result &result, std::string_view name,
                   double expected) {
  const auto value = mparser::runtimeNumericElement(variable(result, name), 0);
  require(value && *value == expected, "MAT builtin numeric result mismatch");
}

template <typename Result> void requireSuccess(const Result &result) {
  if (!result.diagnostics.empty()) {
    throw std::runtime_error(result.diagnostics.front().identifier + ": " +
                             result.diagnostics.front().message);
  }
  requireNumber(result, "summary", 143);
  requireNumber(result, "command_value", 73);
  requireNumber(result, "plain", 17);
}

void runRoundTripSmoke(const TemporaryTree &tree) {
  constexpr std::string_view source = R"MATLAB(
matrix = reshape(1:8, [2 2 2]);
z = single([1 + 2i 3 - 4i]);
flags = logical([0 1; 1 0]);
characters = char([65 20013; 66 67]);
values = {1, 'two'; true, single(4)};
record = struct('label', 'answer', 'value', 42);
save('roundtrip', 'matrix', 'z', 'flags', 'characters', 'values', 'record');
clear matrix z flags characters values record;

snapshot = load('roundtrip');
assert(exist('matrix', 'var') == 0);
assert(snapshot.matrix(8) == 8);
assert(strcmp(snapshot.characters, char([65 20013; 66 67])));
assert(strcmp(snapshot.values{1, 2}, 'two'));
assert(snapshot.record.value == 42);

load('roundtrip', 'matrix', 'z', 'flags', 'characters', 'values', 'record');
assert(matrix(8) == 8 && real(z(1)) == 1 && imag(z(1)) == 2);

plain = 17;
save('plain', 'plain', '-nocompression');
clear plain;
load('plain');

command_value = 73;
save command_case command_value
clear command_value;
load command_case

summary = matrix(8) + real(z(1)) + plain + record.value + ...
    command_value + values{1, 1} + flags(2, 1);
)MATLAB";
  const auto pair = runBoth(source, tree);
  requireSuccess(pair.interpreter);
  requireSuccess(pair.bytecode);
  require(std::filesystem::exists(tree.interpreter / "roundtrip.mat") &&
              std::filesystem::exists(tree.bytecode / "roundtrip.mat") &&
              std::filesystem::exists(tree.interpreter / "plain.mat") &&
              std::filesystem::exists(tree.bytecode / "plain.mat") &&
              std::filesystem::exists(tree.interpreter / "command_case.mat") &&
              std::filesystem::exists(tree.bytecode / "command_case.mat"),
          "MAT builtin did not create expected files");
}

template <typename Result>
void requireDiagnostic(const Result &result, std::string_view identifier) {
  require(!result.diagnostics.empty(),
          "MAT builtin failure produced no diagnostic");
  require(result.diagnostics.front().identifier == identifier,
          "MAT builtin failure diagnostic identifier mismatch");
}

void runFailureSmoke(const TemporaryTree &tree) {
  const auto denied =
      runBoth("value = 1; save('denied', 'value');", tree,
              mparser::RuntimeSystemCapability::CurrentDirectory |
                  mparser::RuntimeSystemCapability::FileSystemRead);
  requireDiagnostic(denied.interpreter, "MParser:MatFileWriteFailed");
  requireDiagnostic(denied.bytecode, "MParser:MatFileWriteFailed");
  require(!std::filesystem::exists(tree.interpreter / "denied.mat") &&
              !std::filesystem::exists(tree.bytecode / "denied.mat"),
          "denied MAT save created a file");

  const auto unsupported =
      runBoth("value = 1; save('legacy', 'value', '-v6');", tree);
  requireDiagnostic(unsupported.interpreter,
                    "MParser:UnsupportedMatFileOption");
  requireDiagnostic(unsupported.bytecode, "MParser:UnsupportedMatFileOption");
  require(!std::filesystem::exists(tree.interpreter / "legacy.mat") &&
              !std::filesystem::exists(tree.bytecode / "legacy.mat"),
          "unsupported MAT save created a file");

  const auto unsupportedValue =
      runBoth("text = \"value\"; save('string_value', 'text');", tree);
  requireDiagnostic(unsupportedValue.interpreter,
                    "MParser:MatFileEncodeFailed");
  requireDiagnostic(unsupportedValue.bytecode, "MParser:MatFileEncodeFailed");
  require(!std::filesystem::exists(tree.interpreter / "string_value.mat") &&
              !std::filesystem::exists(tree.bytecode / "string_value.mat"),
          "failed MAT encoding created a file");

  for (const auto &directory : {tree.interpreter, tree.bytecode}) {
    std::ofstream stream(directory / "broken.mat",
                         std::ios::binary | std::ios::trunc);
    stream << "not a MAT file";
  }
  const auto corrupted = runBoth("marker = 5; load('broken');", tree);
  requireDiagnostic(corrupted.interpreter, "MParser:MatFileDecodeFailed");
  requireDiagnostic(corrupted.bytecode, "MParser:MatFileDecodeFailed");
  requireNumber(corrupted.interpreter, "marker", 5);
  requireNumber(corrupted.bytecode, "marker", 5);
}

} // namespace

int main() {
  try {
    TemporaryTree tree;
    runRoundTripSmoke(tree);
    runFailureSmoke(tree);
    std::cout << "MAT-file builtin smoke passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
