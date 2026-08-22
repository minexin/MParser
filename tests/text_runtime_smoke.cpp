#include "mparser/execution/bytecode/bytecode.h"
#include "mparser/execution/bytecode/bytecode_vm.h"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/runtime_assignment.h"
#include "mparser/runtime/core/runtime_index.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"
#include "mparser/semantic/semantic.h"

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
              std::fabs(summary.number - 49.0) < 1e-9,
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

  const auto &parsedStrings = variable(result, "parsed_strings");
  require(parsedStrings.numericClass ==
                  mparser::RuntimeNumericClass::Double &&
              parsedStrings.numericComplex &&
              mparser::runtimeDimensions(parsedStrings) ==
                  std::vector<size_t>({2, 2}),
          "double(string array) shape or complexity mismatch");
  const auto parsedComplex =
      mparser::runtimeNumericElementValue(parsedStrings, 1);
  const auto parsedMissing =
      mparser::runtimeNumericElementValue(parsedStrings, 3);
  require(parsedComplex && parsedComplex->complex &&
              parsedComplex->real == 1.0 &&
              parsedComplex->imaginary == 2.0 && parsedMissing &&
              std::isnan(parsedMissing->real),
          "double(string array) payload mismatch");

  const auto &parsedCells = variable(result, "parsed_cells");
  require(mparser::runtimeDimensions(parsedCells) ==
                  std::vector<size_t>({2, 2}) &&
              mparser::runtimeNumericElementValue(parsedCells, 0)->real ==
                  31.0 &&
              std::isnan(mparser::runtimeNumericElementValue(
                             parsedCells, 1)->real) &&
              mparser::runtimeNumericElementValue(parsedCells, 2)->real ==
                  1234.5 &&
              mparser::runtimeNumericElementValue(parsedCells, 3)
                      ->imaginary == 3.0,
          "str2double cell-array conversion mismatch");

  const auto &numericTextEdges = variable(result, "numeric_text_edges");
  const auto imaginaryFirst =
      mparser::runtimeNumericElementValue(numericTextEdges, 0);
  const auto imaginaryProduct =
      mparser::runtimeNumericElementValue(numericTextEdges, 1);
  const auto joinedDigits =
      mparser::runtimeNumericElementValue(numericTextEdges, 2);
  const auto misplacedComma =
      mparser::runtimeNumericElementValue(numericTextEdges, 3);
  const auto realInfinity =
      mparser::runtimeNumericElementValue(numericTextEdges, 4);
  const auto imaginaryInfinity =
      mparser::runtimeNumericElementValue(numericTextEdges, 5);
  const auto signedHex =
      mparser::runtimeNumericElementValue(numericTextEdges, 6);
  const auto hexFloat =
      mparser::runtimeNumericElementValue(numericTextEdges, 7);
  const auto negativeHexExponent =
      mparser::runtimeNumericElementValue(numericTextEdges, 8);
  const auto suffixedHex =
      mparser::runtimeNumericElementValue(numericTextEdges, 9);
  const auto binaryText =
      mparser::runtimeNumericElementValue(numericTextEdges, 10);
  require(imaginaryFirst && imaginaryFirst->real == 1.0 &&
              imaginaryFirst->imaginary == 2.0 && imaginaryProduct &&
              imaginaryProduct->real == 0.0 &&
              imaginaryProduct->imaginary == 2.0 && joinedDigits &&
              std::isnan(joinedDigits->real) && misplacedComma &&
              std::isnan(misplacedComma->real) && realInfinity &&
              std::isinf(realInfinity->real) && imaginaryInfinity &&
              imaginaryInfinity->complex &&
              std::isinf(imaginaryInfinity->imaginary) && signedHex &&
              signedHex->real == -1.0 && hexFloat &&
              hexFloat->real == 3.0 && negativeHexExponent &&
              negativeHexExponent->real == 0.25 && suffixedHex &&
              std::isnan(suffixedHex->real) && binaryText &&
              std::isnan(binaryText->real),
          "numeric text grammar accepted or rejected the wrong forms");

  const auto &mixedCellText = variable(result, "mixed_cell_text");
  require(mparser::runtimeDimensions(mixedCellText) ==
                  std::vector<size_t>({2, 2}) &&
              std::isnan(mparser::runtimeNumericElementValue(
                             mixedCellText, 0)->real) &&
              mparser::runtimeNumericElementValue(mixedCellText, 2)->real ==
                  2.0 &&
              std::isnan(mparser::runtimeNumericElementValue(
                             mixedCellText, 1)->real) &&
              std::isnan(mparser::runtimeNumericElementValue(
                             mixedCellText, 3)->real),
          "str2double cell invalid-element behavior mismatch");
  const auto &nonTextNumber = variable(result, "non_text_number");
  require(mparser::runtimeShapeElementCount(nonTextNumber) == 1 &&
              std::isnan(mparser::runtimeNumericElementValue(
                             nonTextNumber, 0)->real),
          "str2double non-text input did not produce scalar NaN");
}

template <typename Result> void verifyCellLiteralRows(const Result &result) {
  require(result.diagnostics.empty(), "cell literal row execution failed");
  const auto &mixed = variable(result, "mixed");
  require(mixed.kind == mparser::RuntimeValueKind::Cell &&
              mparser::runtimeDimensions(mixed) ==
                  std::vector<size_t>({2, 2}) &&
              mixed.cells.size() == 4,
          "two-dimensional cell literal shape mismatch");
  require(mparser::runtimeDimensions(mixed.cells[0]) ==
                  std::vector<size_t>({1, 2}) &&
              mixed.cells[1].kind ==
                  mparser::RuntimeValueKind::StringArray &&
              mixed.cells[2].kind ==
                  mparser::RuntimeValueKind::MissingArray &&
              mixed.cells[3].kind == mparser::RuntimeValueKind::Number &&
              mixed.cells[3].number == 4.0,
          "cell literal elements were flattened or reordered");

  const auto &empty = variable(result, "empty_cell");
  require(empty.kind == mparser::RuntimeValueKind::Cell &&
              mparser::runtimeDimensions(empty) ==
                  std::vector<size_t>({0, 0}) &&
              empty.cells.empty(),
          "empty cell literal is not 0-by-0");
}

template <typename Result>
void verifyRaggedCellLiteralRejected(const Result &result) {
  require(!result.diagnostics.empty(),
          "ragged cell literal unexpectedly succeeded");
  require(result.diagnostics.front().message.find(
              "dimensions must agree") != std::string::npos,
          "ragged cell literal reported the wrong diagnostic");
}

template <typename Result> void verifyMissingRow(const Result &result) {
  require(result.diagnostics.empty(),
          "missing row construction produced a runtime diagnostic");
  const auto &value = variable(result, "a");
  require(value.kind == mparser::RuntimeValueKind::MissingArray &&
              mparser::runtimeDimensions(value) ==
                  std::vector<size_t>({1, 2}),
          "[missing missing] did not produce a 1-by-2 missing array");
}

template <typename Result>
void verifyCellTextComparison(const Result &result) {
  require(result.diagnostics.empty(),
          "Cell text comparison produced a runtime diagnostic");
  const auto &matches = variable(result, "matches");
  const auto &folded = variable(result, "folded");
  require(mparser::runtimeDimensions(matches) ==
                  std::vector<size_t>({2, 2}) &&
              matches.elements == std::vector<double>({1, 0, 0, 0}),
          "strcmp did not preserve Cell shape or scalar-expand text");
  require(mparser::runtimeDimensions(folded) ==
                  std::vector<size_t>({2, 2}) &&
              folded.elements == std::vector<double>({1, 0, 0, 1}),
          "strcmpi did not compare Cell text case-insensitively");
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "text runtime smoke expects the sample path");
    const std::string source = readSource(argv[1]);
    const RuntimePair result = runBoth(source);
    verify(result.interpreter);
    verify(result.vm);

    const RuntimePair cellRows = runBoth(R"(
mixed = {[1 2], "text"; missing, 4};
empty_cell = {};
)");
    verifyCellLiteralRows(cellRows.interpreter);
    verifyCellLiteralRows(cellRows.vm);

    const RuntimePair ragged = runBoth("bad = {1, 2; 3};\n");
    verifyRaggedCellLiteralRejected(ragged.interpreter);
    verifyRaggedCellLiteralRejected(ragged.vm);

    const RuntimePair missingRow = runBoth("a = [missing missing];\n");
    verifyMissingRow(missingRow.interpreter);
    verifyMissingRow(missingRow.vm);

    const RuntimePair cellTextComparison = runBoth(R"(
names = {'Alpha', 'other'; 'third', 'Target'};
matches = strcmp(names, 'Alpha');
folded = strcmpi(names, {'alpha', 'none'; 'none', 'target'});
)");
    verifyCellTextComparison(cellTextComparison.interpreter);
    verifyCellTextComparison(cellTextComparison.vm);

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
