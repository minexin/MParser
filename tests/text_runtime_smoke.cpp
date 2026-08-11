#include "mparser/bytecode.h"
#include "mparser/bytecode_vm.h"
#include "mparser/interpreter.h"
#include "mparser/lexer.h"
#include "mparser/parser.h"
#include "mparser/runtime_assignment.h"
#include "mparser/runtime_index.h"
#include "mparser/runtime_numeric.h"
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
              std::fabs(summary.number - 47.0) < 1e-9,
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

  const auto &rawMissing = variable(result, "raw_missing");
  require(rawMissing.kind == mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(rawMissing) ==
                  std::vector<size_t>({1, 1}) &&
              mparser::runtimeValueIsStorable(rawMissing),
          "first-class missing scalar contract mismatch");
  const auto &directMissing = variable(result, "direct_missing");
  const auto *missingElement =
      mparser::runtimeStringElement(directMissing, 0);
  require(missingElement && missingElement->missing,
          "string(missing) did not preserve missing state");

  const auto &missingGrid = variable(result, "missing_grid");
  require(missingGrid.kind == mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(missingGrid) ==
                  std::vector<size_t>({2, 2}) &&
              mparser::runtimeValueToString(missingGrid) ==
                  "missing(2x2)",
          "missing concatenation shape mismatch");
  const auto &missingReplica = variable(result, "missing_replica");
  require(missingReplica.kind ==
                  mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(missingReplica) ==
                  std::vector<size_t>({2, 3}),
          "missing repmat shape mismatch");
  const auto &mixedNumeric = variable(result, "mixed_missing_numeric");
  require(mixedNumeric.kind == mparser::RuntimeValueKind::Vector &&
              std::isnan(mparser::runtimeNumericElementValue(
                             mixedNumeric, 1)->real),
          "numeric/missing concatenation coercion mismatch");
  const auto &mixedString = variable(result, "mixed_missing_string");
  const auto *mixedMissing = mparser::runtimeStringElement(mixedString, 1);
  require(mixedMissing && mixedMissing->missing,
          "string/missing concatenation coercion mismatch");
  const auto &missingGrown = variable(result, "missing_grown");
  const auto &missingDeleted = variable(result, "missing_deleted");
  require(missingGrown.kind == mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(missingGrown) ==
                  std::vector<size_t>({1, 3}) &&
              missingDeleted.kind ==
                  mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(missingDeleted) ==
                  std::vector<size_t>({1, 2}),
          "missing growth/deletion shape mismatch");
  const auto &numericAssigned =
      variable(result, "numeric_assigned_missing");
  require(std::isnan(mparser::runtimeNumericElementValue(
                         numericAssigned, 1)->real),
          "floating numeric missing assignment mismatch");
  const auto &stringAssigned =
      variable(result, "string_assigned_missing");
  const auto *assignedMissing =
      mparser::runtimeStringElement(stringAssigned, 1);
  require(assignedMissing && assignedMissing->missing,
          "string missing assignment mismatch");
  const auto &genericMissingMask =
      variable(result, "generic_missing_mask");
  require(genericMissingMask.kind ==
                  mparser::RuntimeValueKind::Number &&
              genericMissingMask.number == 1.0,
          "generic ismissing mask semantics mismatch");

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

    auto missingTarget =
        mparser::makeRuntimeMissingArrayValue({1, 2});
    const auto incompatibleMissing =
        mparser::runtimeAssignMissingIndexed(
            missingTarget, {mparser::makeRuntimeNumberValue(1.0)},
            mparser::makeRuntimeNumberValue(5.0));
    require(!incompatibleMissing.succeeded,
            "missing target accepted a numeric assignment");

    auto integerTarget = mparser::makeRuntimeVectorValue(
        {1.0, 2.0}, mparser::RuntimeNumericClass::Int8);
    const auto incompatibleInteger =
        mparser::runtimeAssignNumericIndexed(
            integerTarget, {mparser::makeRuntimeNumberValue(2.0)},
            mparser::makeRuntimeMissingArrayValue());
    require(!incompatibleInteger.succeeded,
            "integer target accepted a missing assignment");

    auto shapeOnlyMissing =
        mparser::makeRuntimeMissingArrayValue({1, 1000000000});
    const auto largeGrowth = mparser::runtimeAssignMissingIndexed(
        shapeOnlyMissing,
        {mparser::makeRuntimeNumberValue(1000000001.0)},
        mparser::makeRuntimeMissingArrayValue());
    require(largeGrowth.succeeded &&
                mparser::runtimeDimensions(shapeOnlyMissing) ==
                    std::vector<size_t>({1, 1000000001}),
            "large missing growth materialized or lost its shape");
    const auto largeDeletion = mparser::runtimeDeleteMissingIndexed(
        shapeOnlyMissing,
        {mparser::makeRuntimeNumberValue(1000000001.0)}, {false});
    require(largeDeletion.succeeded &&
                mparser::runtimeDimensions(shapeOnlyMissing) ==
                    std::vector<size_t>({1, 1000000000}),
            "large missing deletion materialized or lost its shape");

    auto missingCube =
        mparser::makeRuntimeMissingArrayValue({2, 3, 4});
    const auto cubeElement = mparser::runtimeIndexMissingArray(
        missingCube,
        {mparser::makeRuntimeNumberValue(2.0),
         mparser::makeRuntimeNumberValue(3.0),
         mparser::makeRuntimeNumberValue(4.0)});
    require(cubeElement.succeeded &&
                cubeElement.value.kind ==
                    mparser::RuntimeValueKind::MissingArray &&
                mparser::runtimeDimensions(cubeElement.value) ==
                    std::vector<size_t>({1, 1}),
            "N-dimensional missing indexing mismatch");
    const auto cubeGrowth = mparser::runtimeAssignMissingIndexed(
        missingCube,
        {mparser::makeRuntimeNumberValue(2.0),
         mparser::makeRuntimeNumberValue(3.0),
         mparser::makeRuntimeNumberValue(5.0)},
        mparser::makeRuntimeMissingArrayValue());
    require(cubeGrowth.succeeded &&
                mparser::runtimeDimensions(missingCube) ==
                    std::vector<size_t>({2, 3, 5}),
            "N-dimensional missing growth mismatch");
    const auto cubeDeletion = mparser::runtimeDeleteMissingIndexed(
        missingCube,
        {mparser::makeRuntimeVectorValue({1.0, 2.0}),
         mparser::makeRuntimeVectorValue({1.0, 2.0, 3.0}),
         mparser::makeRuntimeNumberValue(2.0)},
        {true, true, false});
    require(cubeDeletion.succeeded &&
                mparser::runtimeDimensions(missingCube) ==
                    std::vector<size_t>({2, 3, 4}),
            "N-dimensional missing slice deletion mismatch");

    auto shapedMissing =
        mparser::makeRuntimeMissingArrayValue({2, 2});
    const auto incompatibleShape =
        mparser::runtimeAssignMissingIndexed(
            shapedMissing,
            {mparser::makeRuntimeVectorValue({1.0, 2.0}),
             mparser::makeRuntimeVectorValue({1.0, 2.0})},
            mparser::makeRuntimeMissingArrayValue({1, 4}));
    require(!incompatibleShape.succeeded,
            "missing assignment accepted an incompatible RHS shape");

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
