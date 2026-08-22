#include "mparser/runtime_mat_file.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_struct.h"
#include "mparser/runtime_text.h"
#include "mparser/runtime_value_ops.h"

#include "miniz.h"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Unsigned>
void appendLittleUnsigned(std::string &output, Unsigned value) {
  for (size_t index = 0; index < sizeof(Unsigned); ++index) {
    output.push_back(static_cast<char>(value & Unsigned{0xff}));
    value >>= 8U;
  }
}

void writeLittleUInt32(std::string &bytes, size_t offset, std::uint32_t value) {
  require(offset + sizeof(value) <= bytes.size(),
          "MAT fixture mutation exceeds its storage");
  for (size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<char>(value & 0xffU);
    value >>= 8U;
  }
}

std::string compressFixturePayload(std::string_view input) {
  const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(input.size()));
  std::string output(static_cast<size_t>(bound), '\0');
  mz_ulong outputBytes = bound;
  const int status = mz_compress2(
      reinterpret_cast<unsigned char *>(output.data()), &outputBytes,
      reinterpret_cast<const unsigned char *>(input.data()),
      static_cast<mz_ulong>(input.size()), MZ_DEFAULT_COMPRESSION);
  require(status == MZ_OK, "could not compress nested MAT fixture");
  output.resize(static_cast<size_t>(outputBytes));
  return output;
}

std::string wrapCompressedElements(std::string bytes, size_t layers) {
  constexpr size_t headerBytes = 128;
  constexpr std::uint32_t compressedType = 15;
  require(bytes.size() >= headerBytes, "nested MAT fixture omitted its header");
  const std::string header = bytes.substr(0, headerBytes);
  std::string body = bytes.substr(headerBytes);
  for (size_t layer = 0; layer < layers; ++layer) {
    const std::string compressed = compressFixturePayload(body);
    require(compressed.size() <= std::numeric_limits<std::uint32_t>::max(),
            "nested MAT fixture exceeds the format limit");
    std::string wrapped;
    appendLittleUnsigned(wrapped, compressedType);
    appendLittleUnsigned(wrapped,
                         static_cast<std::uint32_t>(compressed.size()));
    wrapped.append(compressed);
    body = std::move(wrapped);
  }
  return header + body;
}

mparser::RuntimeValue numeric(std::vector<size_t> dimensions,
                              std::vector<double> values,
                              mparser::RuntimeNumericClass numericClass) {
  auto result = mparser::runtimeNumericValueFromLogicalOrder(
      std::move(dimensions), std::move(values), numericClass);
  require(result.has_value(), "numeric test value construction failed");
  return std::move(*result);
}

mparser::RuntimeValue exactNumeric(std::vector<size_t> dimensions,
                                   std::vector<std::uint64_t> realBits,
                                   mparser::RuntimeNumericClass numericClass) {
  std::vector<mparser::RuntimeNumericElementValue> elements;
  elements.reserve(realBits.size());
  for (const std::uint64_t bits : realBits) {
    mparser::RuntimeNumericElementValue element;
    element.numericClass = numericClass;
    element.integerRealBits = bits;
    elements.push_back(element);
  }
  auto result = mparser::runtimeNumericValueFromElements(
      std::move(dimensions), std::move(elements), numericClass);
  require(result.has_value(), "exact numeric test value construction failed");
  return std::move(*result);
}

mparser::RuntimeValue complexSingle() {
  std::vector<mparser::RuntimeNumericElementValue> elements(4);
  for (size_t index = 0; index < elements.size(); ++index) {
    elements[index].numericClass = mparser::RuntimeNumericClass::Single;
    elements[index].real = static_cast<double>(index) + 0.5;
    elements[index].imaginary = -static_cast<double>(index) - 0.25;
    elements[index].complex = true;
  }
  auto result = mparser::runtimeNumericValueFromElements(
      {2, 2}, std::move(elements), mparser::RuntimeNumericClass::Single);
  require(result.has_value(), "complex test value construction failed");
  return std::move(*result);
}

std::vector<mparser::RuntimeMatVariable> representativeVariables() {
  using NumericClass = mparser::RuntimeNumericClass;
  std::vector<mparser::RuntimeMatVariable> variables;
  variables.push_back({"double_nd", numeric({2, 2, 2}, {1, 2, 3, 4, 5, 6, 7, 8},
                                            NumericClass::Double)});
  variables.push_back({"logical_value", numeric({2, 3}, {0, 1, 1, 0, 1, 0},
                                                NumericClass::Logical)});
  variables.push_back({"single_complex", complexSingle()});
  variables.push_back(
      {"int8_value", numeric({1, 4}, {-128, -1, 0, 127}, NumericClass::Int8)});
  variables.push_back(
      {"uint8_value", numeric({1, 3}, {0, 127, 255}, NumericClass::UInt8)});
  variables.push_back({"int16_value", numeric({1, 3}, {-32768, 0, 32767},
                                              NumericClass::Int16)});
  variables.push_back({"uint16_value", numeric({1, 3}, {0, 32768, 65535},
                                               NumericClass::UInt16)});
  variables.push_back(
      {"int32_value",
       numeric({1, 3}, {-2147483648.0, 0, 2147483647.0}, NumericClass::Int32)});
  variables.push_back(
      {"uint32_value", exactNumeric({1, 3}, {0, 2147483648ULL, 4294967295ULL},
                                    NumericClass::UInt32)});
  variables.push_back(
      {"int64_value",
       exactNumeric({1, 3}, {0x8000000000000000ULL, 0, 0x7fffffffffffffffULL},
                    NumericClass::Int64)});
  variables.push_back(
      {"uint64_value",
       exactNumeric({1, 3}, {0, 9007199254740993ULL, 0xffffffffffffffffULL},
                    NumericClass::UInt64)});

  auto character =
      mparser::makeRuntimeCharacterArray({2, 3}, u"A\u4e2dB\U0001f642C");
  require(character.kind == mparser::RuntimeValueKind::CharacterArray,
          "character test value construction failed");
  variables.push_back({"characters", std::move(character)});

  auto cell = mparser::makeRuntimeCellValue(
      {2, 2}, {mparser::makeRuntimeNumberValue(1),
               mparser::makeRuntimeCharacterVectorUtf8("right"),
               mparser::makeRuntimeLogicalValue(true),
               numeric({0, 0}, {}, NumericClass::Double)});
  variables.push_back({"cell_value", std::move(cell)});

  std::vector<mparser::RuntimeStructElement> structureElements(4);
  for (size_t index = 0; index < structureElements.size(); ++index) {
    structureElements[index].emplace(
        "number",
        mparser::makeRuntimeNumberValue(static_cast<double>(index + 10)));
    structureElements[index].emplace("label",
                                     mparser::makeRuntimeCharacterVectorUtf8(
                                         "item-" + std::to_string(index)));
  }
  auto structure = mparser::makeRuntimeStructArrayValue(
      {"label", "number"}, std::move(structureElements), {2, 2});
  variables.push_back({"structure", std::move(structure)});
  return variables;
}

const mparser::RuntimeMatVariable &
findVariable(const std::vector<mparser::RuntimeMatVariable> &variables,
             std::string_view name) {
  for (const auto &variable : variables) {
    if (variable.name == name) {
      return variable;
    }
  }
  throw std::runtime_error("missing decoded MAT variable: " +
                           std::string(name));
}

void verifyRoundTrip(bool compressed) {
  const auto expected = representativeVariables();
  mparser::RuntimeMatEncodeOptions options;
  options.compress = compressed;
  const auto encoded = mparser::runtimeEncodeMatV5(expected, options);
  require(encoded.succeeded, encoded.error);
  require(encoded.bytes.size() >= 128, "MAT output omitted its fixed header");
  require(encoded.bytes.substr(126, 2) == "IM",
          "MAT output has the wrong endian marker");

  const auto decoded = mparser::runtimeDecodeMatV5(encoded.bytes);
  require(decoded.succeeded, decoded.error);
  require(decoded.variables.size() == expected.size(),
          "MAT round trip changed the variable count");
  for (const auto &variable : expected) {
    const auto &actual = findVariable(decoded.variables, variable.name);
    require(mparser::runtimeValuesEqual(actual.value, variable.value,
                                        mparser::RuntimeNaNEquality::Equal),
            "MAT round trip changed variable " + variable.name);
  }
  const auto &structure = findVariable(decoded.variables, "structure");
  require(mparser::runtimeStructFieldOrder(structure.value) ==
              std::vector<std::string>{"label", "number"},
          "MAT round trip changed Struct field order");
}

std::string oversizedShapeFixture(const mparser::RuntimeValue &value) {
  mparser::RuntimeMatEncodeOptions options;
  options.compress = false;
  const auto encoded =
      mparser::runtimeEncodeMatV5({{"oversized", value}}, options);
  require(encoded.succeeded, encoded.error);

  std::string bytes = encoded.bytes;
  constexpr size_t firstDimensionOffset = 160;
  writeLittleUInt32(bytes, firstDimensionOffset, 0x7fffffffU);
  writeLittleUInt32(bytes, firstDimensionOffset + 4, 0x7fffffffU);
  return bytes;
}

void requireOversizedShapeRejected(const mparser::RuntimeValue &value,
                                   std::string_view label) {
  mparser::RuntimeMatDecodeOptions options;
  options.maximumBytes = 1024U * 1024U;
  const auto decoded =
      mparser::runtimeDecodeMatV5(oversizedShapeFixture(value), options);
  require(!decoded.succeeded &&
              decoded.error.find("decoded") != std::string::npos,
          std::string("MAT decoder allocated an oversized ") +
              std::string(label) + " representation");
}

void verifyDecodeResourceBoundaries() {
  requireOversizedShapeRejected(mparser::makeRuntimeNumberValue(1), "numeric");
  requireOversizedShapeRejected(mparser::makeRuntimeCharacterVectorUtf8("x"),
                                "character");
  requireOversizedShapeRejected(
      mparser::makeRuntimeCellValue({mparser::makeRuntimeNumberValue(1)}),
      "Cell");

  mparser::RuntimeStructElement element;
  element.emplace("field", mparser::makeRuntimeNumberValue(1));
  requireOversizedShapeRejected(mparser::makeRuntimeStructArrayValue(
                                    {"field"}, {std::move(element)}, {1, 1}),
                                "Struct");

  mparser::RuntimeMatEncodeOptions uncompressed;
  uncompressed.compress = false;
  const auto encoded = mparser::runtimeEncodeMatV5(
      {{"value", mparser::makeRuntimeNumberValue(42)}}, uncompressed);
  require(encoded.succeeded, encoded.error);
  const std::string nested = wrapCompressedElements(encoded.bytes, 3);
  mparser::RuntimeMatDecodeOptions shallow;
  shallow.maximumDepth = 1;
  const auto depthLimited = mparser::runtimeDecodeMatV5(nested, shallow);
  require(!depthLimited.succeeded &&
              depthLimited.error.find("compressed nesting") !=
                  std::string::npos,
          "MAT decoder ignored compressed nesting depth");
}

void verifyFailureBoundaries() {
  mparser::RuntimeMatEncodeOptions tinyEncode;
  tinyEncode.maximumBytes = 127;
  const auto tooSmall = mparser::runtimeEncodeMatV5({}, tinyEncode);
  require(!tooSmall.succeeded && !tooSmall.error.empty(),
          "MAT encoder ignored a header byte limit");

  mparser::RuntimeMatEncodeOptions boundedEncode;
  boundedEncode.maximumBytes = 256;
  const auto oversizedNumeric = mparser::runtimeEncodeMatV5(
      {{"numeric", numeric({1, 100}, std::vector<double>(100, 1.0),
                           mparser::RuntimeNumericClass::Double)}},
      boundedEncode);
  require(!oversizedNumeric.succeeded && !oversizedNumeric.error.empty(),
          "MAT encoder ignored a numeric output limit");
  const auto oversizedCharacter = mparser::runtimeEncodeMatV5(
      {{"characters", mparser::makeRuntimeCharacterArray(
                          {1, 200}, std::u16string(200, u'x'))}},
      boundedEncode);
  require(!oversizedCharacter.succeeded && !oversizedCharacter.error.empty(),
          "MAT encoder ignored a character output limit");

  std::vector<std::string> fieldOrder;
  mparser::RuntimeStructElement manyFields;
  for (size_t index = 0; index < 32; ++index) {
    const std::string field = "field_name_" + std::to_string(index);
    fieldOrder.push_back(field);
    manyFields.emplace(field, mparser::makeRuntimeNumberValue(1));
  }
  const auto oversizedStruct = mparser::runtimeEncodeMatV5(
      {{"structure",
        mparser::makeRuntimeStructArrayValue(std::move(fieldOrder),
                                             {std::move(manyFields)}, {1, 1})}},
      boundedEncode);
  require(!oversizedStruct.succeeded && !oversizedStruct.error.empty(),
          "MAT encoder ignored a Struct field-table limit");

  const auto unsupported = mparser::runtimeEncodeMatV5(
      {{"text", mparser::makeRuntimeStringScalarUtf8("unsupported")}});
  require(!unsupported.succeeded &&
              unsupported.error.find("variable text") != std::string::npos,
          "MAT encoder accepted an unsupported String value");

  const auto encoded = mparser::runtimeEncodeMatV5(
      {{"value", mparser::makeRuntimeNumberValue(42)}});
  require(encoded.succeeded, encoded.error);

  mparser::RuntimeMatDecodeOptions tinyDecode;
  tinyDecode.maximumBytes = encoded.bytes.size() - 1;
  const auto limited = mparser::runtimeDecodeMatV5(encoded.bytes, tinyDecode);
  require(!limited.succeeded && !limited.error.empty(),
          "MAT decoder ignored its byte limit");

  std::string truncated = encoded.bytes;
  truncated.pop_back();
  const auto truncatedResult = mparser::runtimeDecodeMatV5(truncated);
  require(!truncatedResult.succeeded && !truncatedResult.error.empty(),
          "MAT decoder accepted a truncated file");

  std::string badEndian = encoded.bytes;
  badEndian.replace(126, 2, "XX");
  const auto endianResult = mparser::runtimeDecodeMatV5(badEndian);
  require(!endianResult.succeeded && !endianResult.error.empty(),
          "MAT decoder accepted an invalid endian marker");

  auto nested = mparser::makeRuntimeCellValue({mparser::makeRuntimeCellValue(
      {mparser::makeRuntimeCellValue({mparser::makeRuntimeNumberValue(1)})})});
  mparser::RuntimeMatEncodeOptions shallow;
  shallow.maximumDepth = 1;
  const auto depthLimited =
      mparser::runtimeEncodeMatV5({{"nested", std::move(nested)}}, shallow);
  require(!depthLimited.succeeded && !depthLimited.error.empty(),
          "MAT encoder ignored its nesting depth limit");

  const auto repeated = numeric({1, 10000}, std::vector<double>(10000, 7.0),
                                mparser::RuntimeNumericClass::Double);
  const auto compressed = mparser::runtimeEncodeMatV5({{"repeated", repeated}});
  require(compressed.succeeded, compressed.error);
  mparser::RuntimeMatDecodeOptions expansionLimited;
  expansionLimited.maximumBytes = compressed.bytes.size() + 64;
  const auto expansion =
      mparser::runtimeDecodeMatV5(compressed.bytes, expansionLimited);
  require(!expansion.succeeded && !expansion.error.empty(),
          "MAT decoder ignored its decompression expansion limit");
}

template <typename Unsigned>
void appendBigUnsigned(std::string &output, Unsigned value) {
  for (size_t index = 0; index < sizeof(Unsigned); ++index) {
    const unsigned shift =
        static_cast<unsigned>((sizeof(Unsigned) - index - 1) * 8);
    output.push_back(static_cast<char>(value >> shift));
  }
}

std::string bigEndianScalarFixture() {
  std::string bytes(116, ' ');
  bytes.append(8, '\0');
  appendBigUnsigned(bytes, std::uint16_t{0x0100});
  bytes.append("MI", 2);

  appendBigUnsigned(bytes, std::uint32_t{14});
  appendBigUnsigned(bytes, std::uint32_t{56});

  appendBigUnsigned(bytes, std::uint32_t{6});
  appendBigUnsigned(bytes, std::uint32_t{8});
  appendBigUnsigned(bytes, std::uint32_t{6});
  appendBigUnsigned(bytes, std::uint32_t{0});

  appendBigUnsigned(bytes, std::uint32_t{5});
  appendBigUnsigned(bytes, std::uint32_t{8});
  appendBigUnsigned(bytes, std::uint32_t{1});
  appendBigUnsigned(bytes, std::uint32_t{1});

  appendBigUnsigned(bytes, std::uint16_t{1});
  appendBigUnsigned(bytes, std::uint16_t{1});
  bytes.append("x\0\0\0", 4);

  appendBigUnsigned(bytes, std::uint32_t{9});
  appendBigUnsigned(bytes, std::uint32_t{8});
  appendBigUnsigned(bytes, std::bit_cast<std::uint64_t>(42.0));
  return bytes;
}

void verifyBigEndianFixture() {
  const auto decoded = mparser::runtimeDecodeMatV5(bigEndianScalarFixture());
  require(decoded.succeeded, decoded.error);
  require(decoded.variables.size() == 1 &&
              decoded.variables.front().name == "x",
          "big-endian MAT fixture variable metadata mismatch");
  const auto value =
      mparser::runtimeNumericElement(decoded.variables.front().value, 0);
  require(value && *value == 42.0, "big-endian MAT fixture scalar mismatch");
}

std::string readBinaryFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  require(stream.good(), "could not open MAT fixture for reading");
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void writeRepresentativeFile(const std::filesystem::path &path,
                             bool compressed) {
  mparser::RuntimeMatEncodeOptions options;
  options.compress = compressed;
  const auto encoded =
      mparser::runtimeEncodeMatV5(representativeVariables(), options);
  require(encoded.succeeded, encoded.error);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  require(stream.good(), "could not open MAT fixture for writing");
  stream.write(encoded.bytes.data(),
               static_cast<std::streamsize>(encoded.bytes.size()));
  require(stream.good(), "could not write MAT fixture");
}

void verifyRepresentativeFile(const std::filesystem::path &path) {
  const auto expected = representativeVariables();
  const auto bytes = readBinaryFile(path);
  const auto decoded = mparser::runtimeDecodeMatV5(bytes);
  require(decoded.succeeded, decoded.error);
  require(decoded.variables.size() == expected.size(),
          "MAT fixture has an unexpected variable count");
  for (const auto &variable : expected) {
    const auto &actual = findVariable(decoded.variables, variable.name);
    require(mparser::runtimeValuesEqual(actual.value, variable.value,
                                        mparser::RuntimeNaNEquality::Equal),
            "MAT fixture changed variable " + variable.name);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--verify") {
      verifyRepresentativeFile(argv[2]);
      std::cout << "runtime MAT-file fixture verified\n";
      return 0;
    }
    if (argc == 3 && (std::string_view(argv[1]) == "--write-compressed" ||
                      std::string_view(argv[1]) == "--write-uncompressed")) {
      writeRepresentativeFile(argv[2], std::string_view(argv[1]) ==
                                           "--write-compressed");
      std::cout << "runtime MAT-file fixture written\n";
      return 0;
    }
    require(argc == 1, "unsupported runtime MAT-file smoke arguments");
    verifyRoundTrip(false);
    verifyRoundTrip(true);
    verifyFailureBoundaries();
    verifyDecodeResourceBoundaries();
    verifyBigEndianFixture();
    std::cout << "runtime MAT-file smoke passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
