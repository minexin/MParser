#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"
#include "mparser/semantic.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct RuntimePair {
  mparser::InterpreterResult interpreter;
  mparser::BytecodeVmResult vm;
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::string readSource(const char *path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "text runtime sample is unavailable");
  std::ostringstream source;
  source << input.rdbuf();
  return source.str();
}

RuntimePair runBoth(std::string_view source) {
  mparser::Lexer lexer(source);
  mparser::Parser parser(lexer.lex());
  auto parse = parser.parse();
  require(parse.diagnostics.empty(), "text runtime sample did not parse");

  mparser::SemanticAnalyzer analyzer;
  auto semantic = analyzer.analyze(*parse.root);
  require(semantic.diagnostics.empty(),
          "text runtime sample failed semantic analysis");

  mparser::BytecodeLowerer lowerer;
  auto bytecode = lowerer.lower(semantic);
  require(bytecode.diagnostics.empty(), "text runtime sample did not lower");

  mparser::Interpreter interpreter;
  auto interpreterResult = interpreter.run(semantic);
  mparser::BytecodeVm vm;
  auto vmResult = vm.run(bytecode, semantic);
  return RuntimePair{std::move(interpreterResult), std::move(vmResult)};
}

template <typename Result>
const mparser::RuntimeValue &variable(const Result &result,
                                      std::string_view name) {
  for (const auto &candidate : result.variables) {
    if (candidate.name == name) {
      return candidate.value;
    }
  }
  throw std::runtime_error("missing text runtime variable: " +
                           std::string(name));
}

template <typename Result> void verify(const Result &result) {
  if (!result.diagnostics.empty()) {
    throw std::runtime_error(result.diagnostics.front().message);
  }
  const auto &summary = variable(result, "summary");
  require(summary.kind == mparser::RuntimeValueKind::Number &&
              std::fabs(summary.number - 36.0) < 1e-9,
          "text runtime summary mismatch");

  const auto &characters = variable(result, "char_value");
  require(characters.kind == mparser::RuntimeValueKind::CharacterArray,
          "single-quoted literal is not a character array");
  require(mparser::runtimeDimensions(characters) == std::vector<size_t>({1, 5}),
          "character vector shape mismatch");

  const auto &string = variable(result, "string_value");
  require(string.kind == mparser::RuntimeValueKind::StringArray,
          "double-quoted literal is not a string array");
  require(mparser::runtimeDimensions(string) == std::vector<size_t>({1, 1}),
          "string scalar shape mismatch");

  const auto &grid = variable(result, "string_grid");
  require(grid.kind == mparser::RuntimeValueKind::StringArray &&
              mparser::runtimeDimensions(grid) == std::vector<size_t>({2, 2}),
          "string matrix shape mismatch");

  const auto &unicode = variable(result, "unicode_value");
  const auto unicodeUnits = mparser::runtimeTextScalarCodeUnits(unicode);
  require(unicodeUnits && unicodeUnits->size() == 3,
          "Unicode text is not stored as UTF-16 code units");

  const auto &grown = variable(result, "grown");
  require(mparser::runtimeDimensions(grown) == std::vector<size_t>({1, 2}),
          "string deletion did not preserve row orientation");
  const auto *tail = mparser::runtimeStringElement(grown, 1);
  require(tail && !tail->missing &&
              mparser::runtimeUtf16ToUtf8(tail->value) == "tail",
          "string growth/deletion payload mismatch");

  const auto &cells = variable(result, "string_cells");
  require(cells.kind == mparser::RuntimeValueKind::Cell &&
              mparser::runtimeDimensions(cells) == std::vector<size_t>({2, 2}),
          "cellstr string-array shape mismatch");
  require(cells.cells.size() == 4 &&
              mparser::runtimeTextScalarUtf8(cells.cells[1]) == "b" &&
              mparser::runtimeTextScalarUtf8(cells.cells[2]) == "c",
          "cellstr string-array storage layout mismatch");
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "text runtime smoke expects the sample path");
    const std::string source = readSource(argv[1]);
    const RuntimePair result = runBoth(source);
    verify(result.interpreter);
    verify(result.vm);

    const std::string emoji = "\xF0\x9F\x98\x80";
    const auto units = mparser::runtimeUtf8ToUtf16(emoji);
    require(units.size() == 2 && mparser::runtimeUtf16ToUtf8(units) == emoji,
            "UTF-8/UTF-16 supplementary-plane round trip failed");
    std::cout << "text runtime smoke tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Text runtime smoke failure: " << error.what() << '\n';
    return 1;
  }
}
