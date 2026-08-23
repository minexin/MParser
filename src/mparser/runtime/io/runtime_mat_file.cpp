#include "mparser/runtime/io/runtime_mat_file.h"

#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include "miniz.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mparser {
namespace {

constexpr size_t kMatHeaderBytes = 128;
constexpr std::uint32_t kMiInt8 = 1;
constexpr std::uint32_t kMiUInt8 = 2;
constexpr std::uint32_t kMiInt16 = 3;
constexpr std::uint32_t kMiUInt16 = 4;
constexpr std::uint32_t kMiInt32 = 5;
constexpr std::uint32_t kMiUInt32 = 6;
constexpr std::uint32_t kMiSingle = 7;
constexpr std::uint32_t kMiDouble = 9;
constexpr std::uint32_t kMiInt64 = 12;
constexpr std::uint32_t kMiUInt64 = 13;
constexpr std::uint32_t kMiMatrix = 14;
constexpr std::uint32_t kMiCompressed = 15;
constexpr std::uint32_t kMiUtf8 = 16;
constexpr std::uint32_t kMiUtf16 = 17;

constexpr std::uint32_t kMxCell = 1;
constexpr std::uint32_t kMxStruct = 2;
constexpr std::uint32_t kMxChar = 4;
constexpr std::uint32_t kMxDouble = 6;
constexpr std::uint32_t kMxSingle = 7;
constexpr std::uint32_t kMxInt8 = 8;
constexpr std::uint32_t kMxUInt8 = 9;
constexpr std::uint32_t kMxInt16 = 10;
constexpr std::uint32_t kMxUInt16 = 11;
constexpr std::uint32_t kMxInt32 = 12;
constexpr std::uint32_t kMxUInt32 = 13;
constexpr std::uint32_t kMxInt64 = 14;
constexpr std::uint32_t kMxUInt64 = 15;

constexpr std::uint32_t kArrayFlagLogical = 0x0200U;
constexpr std::uint32_t kArrayFlagComplex = 0x0800U;

enum class ByteOrder {
  Little,
  Big,
};

struct MatElement {
  std::uint32_t type = 0;
  std::string_view data;
};

struct DecodedMatrix {
  std::string name;
  RuntimeValue value;
};

bool checkedAdd(size_t left, size_t right, size_t &result) {
  if (left > std::numeric_limits<size_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

bool checkedMultiply(size_t left, size_t right, size_t &result) {
  if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

struct DecodeBudget {
  size_t remainingBytes = 0;

  bool consume(size_t count, size_t bytesPerItem, std::string_view resource,
               std::string &error) {
    size_t bytes = 0;
    if (!checkedMultiply(count, bytesPerItem, bytes) ||
        bytes > remainingBytes) {
      error = "MAT v5 decoded " + std::string(resource) +
              " exceeds the configured byte limit";
      return false;
    }
    remainingBytes -= bytes;
    return true;
  }
};

bool paddedByteCount(size_t size, size_t &result) {
  size_t rounded = 0;
  if (!checkedAdd(size, 7, rounded)) {
    return false;
  }
  result = rounded & ~size_t{7};
  return true;
}

template <typename Unsigned>
void appendLittleUnsigned(std::string &output, Unsigned value) {
  static_assert(std::is_unsigned_v<Unsigned>);
  for (size_t index = 0; index < sizeof(Unsigned); ++index) {
    output.push_back(static_cast<char>(value & Unsigned{0xff}));
    if constexpr (sizeof(Unsigned) > 1) {
      value >>= 8U;
    }
  }
}

void appendFloat(std::string &output, float value) {
  appendLittleUnsigned(output, std::bit_cast<std::uint32_t>(value));
}

void appendDouble(std::string &output, double value) {
  appendLittleUnsigned(output, std::bit_cast<std::uint64_t>(value));
}

bool appendBytes(std::string &output, std::string_view bytes,
                 size_t maximumBytes, std::string &error) {
  size_t total = 0;
  if (!checkedAdd(output.size(), bytes.size(), total) || total > maximumBytes) {
    error = "MAT v5 output exceeds the configured byte limit";
    return false;
  }
  output.append(bytes);
  return true;
}

bool appendDataElement(std::string &output, std::uint32_t type,
                       std::string_view payload, size_t maximumBytes,
                       std::string &error) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "MAT v5 data element exceeds the 32-bit format limit";
    return false;
  }
  size_t stored = payload.size();
  size_t total = 0;
  if ((type != kMiCompressed && !paddedByteCount(payload.size(), stored)) ||
      !checkedAdd(size_t{8}, stored, total) || output.size() > maximumBytes ||
      total > maximumBytes - output.size()) {
    error = "MAT v5 output exceeds the configured byte limit";
    return false;
  }
  appendLittleUnsigned(output, type);
  appendLittleUnsigned(output, static_cast<std::uint32_t>(payload.size()));
  output.append(payload);
  output.append(stored - payload.size(), '\0');
  return true;
}

std::optional<size_t> logicalStorageOffset(const RuntimeValue &value,
                                           size_t logicalIndex) {
  return runtimeColumnMajorLinearToStorageOffset(value, logicalIndex);
}

std::optional<size_t>
logicalStorageOffset(const std::vector<size_t> &dimensions,
                     size_t logicalIndex) {
  const auto coordinates =
      runtimeColumnMajorCoordinates(logicalIndex, dimensions);
  return coordinates ? runtimeRowMajorStorageOffset(*coordinates, dimensions)
                     : std::nullopt;
}

std::optional<std::uint32_t> matNumericType(RuntimeNumericClass numericClass) {
  switch (numericClass) {
  case RuntimeNumericClass::Double:
    return kMiDouble;
  case RuntimeNumericClass::Single:
    return kMiSingle;
  case RuntimeNumericClass::Logical:
    return kMiUInt8;
  case RuntimeNumericClass::Int8:
    return kMiInt8;
  case RuntimeNumericClass::UInt8:
    return kMiUInt8;
  case RuntimeNumericClass::Int16:
    return kMiInt16;
  case RuntimeNumericClass::UInt16:
    return kMiUInt16;
  case RuntimeNumericClass::Int32:
    return kMiInt32;
  case RuntimeNumericClass::UInt32:
    return kMiUInt32;
  case RuntimeNumericClass::Int64:
    return kMiInt64;
  case RuntimeNumericClass::UInt64:
    return kMiUInt64;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> matNumericClass(RuntimeNumericClass numericClass) {
  switch (numericClass) {
  case RuntimeNumericClass::Double:
    return kMxDouble;
  case RuntimeNumericClass::Single:
    return kMxSingle;
  case RuntimeNumericClass::Logical:
    return kMxUInt8;
  case RuntimeNumericClass::Int8:
    return kMxInt8;
  case RuntimeNumericClass::UInt8:
    return kMxUInt8;
  case RuntimeNumericClass::Int16:
    return kMxInt16;
  case RuntimeNumericClass::UInt16:
    return kMxUInt16;
  case RuntimeNumericClass::Int32:
    return kMxInt32;
  case RuntimeNumericClass::UInt32:
    return kMxUInt32;
  case RuntimeNumericClass::Int64:
    return kMxInt64;
  case RuntimeNumericClass::UInt64:
    return kMxUInt64;
  }
  return std::nullopt;
}

bool appendNumericComponent(std::string &output,
                            const RuntimeNumericElementValue &element,
                            RuntimeNumericClass numericClass, bool imaginary) {
  const double floating = imaginary ? element.imaginary : element.real;
  const std::uint64_t integer =
      imaginary ? element.integerImaginaryBits : element.integerRealBits;
  switch (numericClass) {
  case RuntimeNumericClass::Double:
    appendDouble(output, floating);
    return true;
  case RuntimeNumericClass::Single:
    appendFloat(output, static_cast<float>(floating));
    return true;
  case RuntimeNumericClass::Logical:
    output.push_back(floating == 0.0 ? '\0' : '\1');
    return true;
  case RuntimeNumericClass::Int8:
  case RuntimeNumericClass::UInt8:
    appendLittleUnsigned(output, static_cast<std::uint8_t>(integer));
    return true;
  case RuntimeNumericClass::Int16:
  case RuntimeNumericClass::UInt16:
    appendLittleUnsigned(output, static_cast<std::uint16_t>(integer));
    return true;
  case RuntimeNumericClass::Int32:
  case RuntimeNumericClass::UInt32:
    appendLittleUnsigned(output, static_cast<std::uint32_t>(integer));
    return true;
  case RuntimeNumericClass::Int64:
  case RuntimeNumericClass::UInt64:
    appendLittleUnsigned(output, integer);
    return true;
  }
  return false;
}

RuntimeValue emptyDoubleValue() {
  auto value = runtimeNumericValueFromLogicalOrder({0, 0}, {},
                                                   RuntimeNumericClass::Double);
  return value ? std::move(*value) : RuntimeValue{};
}

size_t primitiveBytes(std::uint32_t type);

bool encodeMatrix(const RuntimeValue &value, std::string_view name,
                  const RuntimeMatEncodeOptions &options, size_t depth,
                  std::string &output, std::string &error);

bool appendMatrixHeader(std::string &payload, std::uint32_t arrayClass,
                        bool logical, bool complex,
                        const std::vector<size_t> &dimensions,
                        std::string_view name,
                        const RuntimeMatEncodeOptions &options,
                        std::string &error) {
  std::string flags;
  std::uint32_t bits = arrayClass;
  if (logical) {
    bits |= kArrayFlagLogical;
  }
  if (complex) {
    bits |= kArrayFlagComplex;
  }
  appendLittleUnsigned(flags, bits);
  appendLittleUnsigned(flags, std::uint32_t{0});
  if (!appendDataElement(payload, kMiUInt32, flags, options.maximumBytes,
                         error)) {
    return false;
  }

  std::string shape;
  size_t shapeBytes = 0;
  if (!checkedMultiply(dimensions.size(), sizeof(std::uint32_t), shapeBytes) ||
      shapeBytes > options.maximumBytes) {
    error = "MAT v5 shape metadata exceeds the configured byte limit";
    return false;
  }
  shape.reserve(shapeBytes);
  for (const size_t dimension : dimensions) {
    if (dimension >
        static_cast<size_t>(std::numeric_limits<std::int32_t>::max())) {
      error = "MAT v5 dimension exceeds the signed 32-bit limit";
      return false;
    }
    appendLittleUnsigned(shape, static_cast<std::uint32_t>(dimension));
  }
  if (!appendDataElement(payload, kMiInt32, shape, options.maximumBytes,
                         error)) {
    return false;
  }
  return appendDataElement(payload, kMiInt8, name, options.maximumBytes, error);
}

bool encodeNumericMatrix(const RuntimeValue &value, std::string_view name,
                         const RuntimeMatEncodeOptions &options,
                         std::string &payload, std::string &error) {
  const auto dataType = matNumericType(value.numericClass);
  const auto arrayClass = matNumericClass(value.numericClass);
  if (!dataType || !arrayClass) {
    error = "MAT v5 numeric class is unsupported";
    return false;
  }
  const auto dimensions = runtimeDimensions(value);
  const size_t count = runtimeShapeElementCount(value);
  if (!appendMatrixHeader(payload, *arrayClass,
                          value.numericClass == RuntimeNumericClass::Logical,
                          value.numericComplex, dimensions, name, options,
                          error)) {
    return false;
  }

  std::string real;
  std::string imaginary;
  const size_t bytesPerElement = primitiveBytes(*dataType);
  size_t componentBytes = 0;
  size_t numericBytes = 0;
  if (bytesPerElement == 0 ||
      !checkedMultiply(count, bytesPerElement, componentBytes) ||
      !checkedMultiply(componentBytes,
                       value.numericComplex ? size_t{2} : size_t{1},
                       numericBytes) ||
      numericBytes > options.maximumBytes) {
    error = "MAT v5 numeric output exceeds the configured byte limit";
    return false;
  }
  real.reserve(componentBytes);
  if (value.numericComplex) {
    imaginary.reserve(componentBytes);
  }
  for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
    const auto element = runtimeNumericElementValue(value, logicalIndex);
    if (!element ||
        !appendNumericComponent(real, *element, value.numericClass, false) ||
        (value.numericComplex &&
         !appendNumericComponent(imaginary, *element, value.numericClass,
                                 true))) {
      error = "MAT v5 numeric payload is inconsistent with its shape";
      return false;
    }
  }
  if (!appendDataElement(payload, *dataType, real, options.maximumBytes,
                         error)) {
    return false;
  }
  return !value.numericComplex ||
         appendDataElement(payload, *dataType, imaginary, options.maximumBytes,
                           error);
}

bool encodeCharacterMatrix(const RuntimeValue &value, std::string_view name,
                           const RuntimeMatEncodeOptions &options,
                           std::string &payload, std::string &error) {
  const auto dimensions = runtimeDimensions(value);
  if (runtimeShapeElementCount(value) != value.characterElements.size()) {
    error = "MAT v5 character payload is inconsistent with its shape";
    return false;
  }
  if (!appendMatrixHeader(payload, kMxChar, false, false, dimensions, name,
                          options, error)) {
    return false;
  }
  std::string characters;
  size_t characterBytes = 0;
  if (!checkedMultiply(value.characterElements.size(), sizeof(char16_t),
                       characterBytes) ||
      characterBytes > options.maximumBytes) {
    error = "MAT v5 character output exceeds the configured byte limit";
    return false;
  }
  characters.reserve(characterBytes);
  for (size_t logicalIndex = 0; logicalIndex < value.characterElements.size();
       ++logicalIndex) {
    const auto storageOffset = logicalStorageOffset(value, logicalIndex);
    if (!storageOffset || *storageOffset >= value.characterElements.size()) {
      error = "MAT v5 character payload could not map logical order";
      return false;
    }
    appendLittleUnsigned(
        characters,
        static_cast<std::uint16_t>(value.characterElements[*storageOffset]));
  }
  return appendDataElement(payload, kMiUtf16, characters, options.maximumBytes,
                           error);
}

bool encodeCellMatrix(const RuntimeValue &value, std::string_view name,
                      const RuntimeMatEncodeOptions &options, size_t depth,
                      std::string &payload, std::string &error) {
  const auto dimensions = runtimeDimensions(value);
  const size_t count = runtimeShapeElementCount(value);
  if (count != value.cells.size()) {
    error = "MAT v5 Cell payload is inconsistent with its shape";
    return false;
  }
  if (!appendMatrixHeader(payload, kMxCell, false, false, dimensions, name,
                          options, error)) {
    return false;
  }
  for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
    const auto storageOffset = logicalStorageOffset(value, logicalIndex);
    if (!storageOffset || *storageOffset >= value.cells.size() ||
        !encodeMatrix(value.cells[*storageOffset], {}, options, depth + 1,
                      payload, error)) {
      if (error.empty()) {
        error = "MAT v5 Cell payload could not map logical order";
      }
      return false;
    }
  }
  return true;
}

bool encodeStructMatrix(const RuntimeValue &value, std::string_view name,
                        const RuntimeMatEncodeOptions &options, size_t depth,
                        std::string &payload, std::string &error) {
  const auto dimensions = runtimeDimensions(value);
  const size_t count = runtimeStructElementCount(value);
  if (count != runtimeShapeElementCount(value)) {
    error = "MAT v5 Struct payload is inconsistent with its shape";
    return false;
  }
  const auto fields = runtimeStructFieldOrder(value);
  if (!appendMatrixHeader(payload, kMxStruct, false, false, dimensions, name,
                          options, error)) {
    return false;
  }

  size_t fieldNameBytes = 1;
  for (const std::string &field : fields) {
    size_t terminatedBytes = 0;
    if (!checkedAdd(field.size(), 1, terminatedBytes)) {
      error = "MAT v5 Struct field name is too long";
      return false;
    }
    fieldNameBytes = std::max(fieldNameBytes, terminatedBytes);
  }
  if (fieldNameBytes >
      static_cast<size_t>(std::numeric_limits<std::int32_t>::max())) {
    error = "MAT v5 Struct field name is too long";
    return false;
  }
  std::string fieldLength;
  appendLittleUnsigned(fieldLength, static_cast<std::uint32_t>(fieldNameBytes));
  if (!appendDataElement(payload, kMiInt32, fieldLength, options.maximumBytes,
                         error)) {
    return false;
  }

  std::string fieldNames;
  size_t fieldTableBytes = 0;
  if (!checkedMultiply(fields.size(), fieldNameBytes, fieldTableBytes) ||
      fieldTableBytes > options.maximumBytes) {
    error = "MAT v5 Struct field table exceeds the configured byte limit";
    return false;
  }
  fieldNames.reserve(fieldTableBytes);
  for (const std::string &field : fields) {
    fieldNames.append(field);
    fieldNames.append(fieldNameBytes - field.size(), '\0');
  }
  if (!appendDataElement(payload, kMiInt8, fieldNames, options.maximumBytes,
                         error)) {
    return false;
  }

  for (size_t logicalIndex = 0; logicalIndex < count; ++logicalIndex) {
    const auto storageOffset = logicalStorageOffset(value, logicalIndex);
    const auto *element =
        storageOffset ? runtimeStructElement(value, *storageOffset) : nullptr;
    for (const std::string &field : fields) {
      const auto found = element ? element->find(field)
                                 : RuntimeStructElement::const_iterator{};
      const RuntimeValue empty = element && found != element->end()
                                     ? RuntimeValue{}
                                     : emptyDoubleValue();
      const RuntimeValue &fieldValue =
          element && found != element->end() ? found->second : empty;
      if (!encodeMatrix(fieldValue, {}, options, depth + 1, payload, error)) {
        return false;
      }
    }
  }
  return true;
}

bool encodeMatrix(const RuntimeValue &value, std::string_view name,
                  const RuntimeMatEncodeOptions &options, size_t depth,
                  std::string &output, std::string &error) {
  if (depth > options.maximumDepth) {
    error = "MAT v5 value nesting exceeds the configured depth limit";
    return false;
  }
  std::string payload;
  bool encoded = false;
  if (isRuntimeNumericValue(value)) {
    encoded = encodeNumericMatrix(value, name, options, payload, error);
  } else if (isRuntimeCharacterArray(value)) {
    encoded = encodeCharacterMatrix(value, name, options, payload, error);
  } else if (value.kind == RuntimeValueKind::Cell) {
    encoded = encodeCellMatrix(value, name, options, depth, payload, error);
  } else if (value.kind == RuntimeValueKind::Struct) {
    encoded = encodeStructMatrix(value, name, options, depth, payload, error);
  } else {
    error = "MAT v5 cannot encode runtime value kind " +
            std::string(runtimeValueKindName(value.kind));
    return false;
  }
  return encoded && appendDataElement(output, kMiMatrix, payload,
                                      options.maximumBytes, error);
}

bool compressBytes(std::string_view input, size_t maximumBytes,
                   std::string &output, std::string &error) {
  if (input.size() > std::numeric_limits<mz_ulong>::max()) {
    error = "MAT v5 compressed input exceeds miniz limits";
    return false;
  }
  const mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(input.size()));
  if (bound > maximumBytes) {
    error = "MAT v5 compressed output exceeds the configured byte limit";
    return false;
  }
  output.resize(static_cast<size_t>(bound));
  mz_ulong outputBytes = bound;
  const int status = mz_compress2(
      reinterpret_cast<unsigned char *>(output.data()), &outputBytes,
      reinterpret_cast<const unsigned char *>(input.data()),
      static_cast<mz_ulong>(input.size()), MZ_DEFAULT_COMPRESSION);
  if (status != MZ_OK) {
    error = "miniz could not compress a MAT v5 matrix";
    output.clear();
    return false;
  }
  output.resize(static_cast<size_t>(outputBytes));
  return true;
}

std::uint16_t readUInt16(std::string_view bytes, size_t offset,
                         ByteOrder order) {
  const auto first = static_cast<std::uint8_t>(bytes[offset]);
  const auto second = static_cast<std::uint8_t>(bytes[offset + 1]);
  return order == ByteOrder::Little
             ? static_cast<std::uint16_t>(first | (std::uint16_t{second} << 8U))
             : static_cast<std::uint16_t>(second |
                                          (std::uint16_t{first} << 8U));
}

std::uint32_t readUInt32(std::string_view bytes, size_t offset,
                         ByteOrder order) {
  std::uint32_t value = 0;
  if (order == ByteOrder::Little) {
    for (size_t index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(
                   static_cast<std::uint8_t>(bytes[offset + index]))
               << (index * 8U);
    }
  } else {
    for (size_t index = 0; index < 4; ++index) {
      value = (value << 8U) | static_cast<std::uint8_t>(bytes[offset + index]);
    }
  }
  return value;
}

std::uint64_t readUInt64(std::string_view bytes, size_t offset,
                         ByteOrder order) {
  std::uint64_t value = 0;
  if (order == ByteOrder::Little) {
    for (size_t index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(
                   static_cast<std::uint8_t>(bytes[offset + index]))
               << (index * 8U);
    }
  } else {
    for (size_t index = 0; index < 8; ++index) {
      value = (value << 8U) | static_cast<std::uint8_t>(bytes[offset + index]);
    }
  }
  return value;
}

class ElementReader {
public:
  ElementReader(std::string_view bytes, ByteOrder order)
      : bytes_(bytes), order_(order) {}

  bool exhausted() const noexcept { return offset_ == bytes_.size(); }
  size_t remaining() const noexcept { return bytes_.size() - offset_; }

  std::optional<MatElement> next(std::string &error) {
    if (remaining() < 8) {
      error = "MAT v5 data element has a truncated tag";
      return std::nullopt;
    }
    const std::uint32_t packed = readUInt32(bytes_, offset_, order_);
    const std::uint32_t packedHigh = packed >> 16U;
    if (packedHigh != 0) {
      const std::uint32_t smallType =
          order_ == ByteOrder::Little ? packed & 0xffffU : packedHigh;
      const std::uint32_t smallBytes =
          order_ == ByteOrder::Little ? packedHigh : packed & 0xffffU;
      if (smallBytes > 4) {
        error = "MAT v5 small data element exceeds four bytes";
        return std::nullopt;
      }
      MatElement element;
      element.type = smallType;
      element.data = bytes_.substr(offset_ + 4, smallBytes);
      offset_ += 8;
      return element;
    }

    const std::uint32_t size = readUInt32(bytes_, offset_ + 4, order_);
    size_t stored = size;
    size_t total = 0;
    if ((packed != kMiCompressed && !paddedByteCount(size, stored)) ||
        !checkedAdd(size_t{8}, stored, total) || total > remaining()) {
      error = "MAT v5 data element payload is truncated";
      return std::nullopt;
    }
    MatElement element;
    element.type = packed;
    element.data = bytes_.substr(offset_ + 8, size);
    offset_ += total;
    return element;
  }

private:
  std::string_view bytes_;
  ByteOrder order_;
  size_t offset_ = 0;
};

std::optional<RuntimeNumericClass> numericClassFromMat(std::uint32_t arrayClass,
                                                       bool logical) {
  if (logical) {
    return arrayClass == kMxUInt8 ? std::optional(RuntimeNumericClass::Logical)
                                  : std::nullopt;
  }
  switch (arrayClass) {
  case kMxDouble:
    return RuntimeNumericClass::Double;
  case kMxSingle:
    return RuntimeNumericClass::Single;
  case kMxInt8:
    return RuntimeNumericClass::Int8;
  case kMxUInt8:
    return RuntimeNumericClass::UInt8;
  case kMxInt16:
    return RuntimeNumericClass::Int16;
  case kMxUInt16:
    return RuntimeNumericClass::UInt16;
  case kMxInt32:
    return RuntimeNumericClass::Int32;
  case kMxUInt32:
    return RuntimeNumericClass::UInt32;
  case kMxInt64:
    return RuntimeNumericClass::Int64;
  case kMxUInt64:
    return RuntimeNumericClass::UInt64;
  default:
    return std::nullopt;
  }
}

size_t primitiveBytes(std::uint32_t type) {
  switch (type) {
  case kMiInt8:
  case kMiUInt8:
  case kMiUtf8:
    return 1;
  case kMiInt16:
  case kMiUInt16:
  case kMiUtf16:
    return 2;
  case kMiInt32:
  case kMiUInt32:
  case kMiSingle:
    return 4;
  case kMiInt64:
  case kMiUInt64:
  case kMiDouble:
    return 8;
  default:
    return 0;
  }
}

std::optional<RuntimeNumericClass>
numericClassFromPrimitive(std::uint32_t type) {
  switch (type) {
  case kMiInt8:
    return RuntimeNumericClass::Int8;
  case kMiUInt8:
    return RuntimeNumericClass::UInt8;
  case kMiInt16:
    return RuntimeNumericClass::Int16;
  case kMiUInt16:
    return RuntimeNumericClass::UInt16;
  case kMiInt32:
    return RuntimeNumericClass::Int32;
  case kMiUInt32:
    return RuntimeNumericClass::UInt32;
  case kMiSingle:
    return RuntimeNumericClass::Single;
  case kMiInt64:
    return RuntimeNumericClass::Int64;
  case kMiUInt64:
    return RuntimeNumericClass::UInt64;
  case kMiDouble:
    return RuntimeNumericClass::Double;
  default:
    return std::nullopt;
  }
}

std::uint64_t signExtendIntegerBits(std::uint64_t bits,
                                    RuntimeNumericClass numericClass) {
  unsigned width = 64;
  if (numericClass == RuntimeNumericClass::Int8) {
    width = 8;
  } else if (numericClass == RuntimeNumericClass::Int16) {
    width = 16;
  } else if (numericClass == RuntimeNumericClass::Int32) {
    width = 32;
  }
  if (width < 64 && (bits & (std::uint64_t{1} << (width - 1))) != 0) {
    bits |= ~((std::uint64_t{1} << width) - 1);
  }
  return bits;
}

bool decodeNumericComponent(const MatElement &element, ByteOrder order,
                            RuntimeNumericClass numericClass, size_t count,
                            bool imaginary,
                            std::vector<RuntimeNumericElementValue> &values,
                            std::string &error) {
  const auto expectedType = matNumericType(numericClass);
  const auto storageClass = numericClassFromPrimitive(element.type);
  const size_t bytesPerElement = primitiveBytes(element.type);
  const bool floatingTarget = runtimeNumericClassIsFloating(numericClass);
  if (!expectedType || !storageClass ||
      (!floatingTarget && element.type != *expectedType) ||
      bytesPerElement == 0 ||
      count > std::numeric_limits<size_t>::max() / bytesPerElement ||
      element.data.size() != count * bytesPerElement) {
    error = "MAT v5 numeric data type or size does not match array flags";
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    const size_t offset = index * bytesPerElement;
    RuntimeNumericElementValue &value = values[index];
    value.numericClass = numericClass;
    double floating = 0.0;
    std::uint64_t integer = 0;
    switch (element.type) {
    case kMiDouble:
      floating = std::bit_cast<double>(readUInt64(element.data, offset, order));
      break;
    case kMiSingle:
      floating = static_cast<double>(
          std::bit_cast<float>(readUInt32(element.data, offset, order)));
      break;
    case kMiInt8: {
      integer = static_cast<std::uint8_t>(element.data[offset]);
      const auto signedValue =
          std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(integer));
      integer = signExtendIntegerBits(integer, RuntimeNumericClass::Int8);
      floating = static_cast<double>(signedValue);
      break;
    }
    case kMiUInt8:
      integer = static_cast<std::uint8_t>(element.data[offset]);
      floating = static_cast<double>(integer);
      break;
    case kMiInt16: {
      integer = readUInt16(element.data, offset, order);
      const auto signedValue =
          std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(integer));
      integer = signExtendIntegerBits(integer, RuntimeNumericClass::Int16);
      floating = static_cast<double>(signedValue);
      break;
    }
    case kMiUInt16:
      integer = readUInt16(element.data, offset, order);
      floating = static_cast<double>(integer);
      break;
    case kMiInt32: {
      integer = readUInt32(element.data, offset, order);
      const auto signedValue =
          std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(integer));
      integer = signExtendIntegerBits(integer, RuntimeNumericClass::Int32);
      floating = static_cast<double>(signedValue);
      break;
    }
    case kMiUInt32:
      integer = readUInt32(element.data, offset, order);
      floating = static_cast<double>(integer);
      break;
    case kMiInt64: {
      integer = readUInt64(element.data, offset, order);
      floating = static_cast<double>(std::bit_cast<std::int64_t>(integer));
      break;
    }
    case kMiUInt64:
      integer = readUInt64(element.data, offset, order);
      floating = static_cast<double>(integer);
      break;
    default:
      error = "MAT v5 numeric primitive type is unsupported";
      return false;
    }
    if (runtimeNumericClassIsInteger(numericClass)) {
      if (imaginary) {
        value.integerImaginaryBits = integer;
      } else {
        value.integerRealBits = integer;
      }
    } else if (imaginary) {
      value.imaginary = floating;
    } else {
      value.real = floating;
    }
    value.complex = value.complex || imaginary;
  }
  return true;
}

bool parseDimensions(const MatElement &element, ByteOrder order,
                     DecodeBudget &budget, std::vector<size_t> &dimensions,
                     std::string &error) {
  if (element.type != kMiInt32 || element.data.size() < 8 ||
      element.data.size() % 4 != 0) {
    error = "MAT v5 dimensions element is invalid";
    return false;
  }
  const size_t dimensionCount = element.data.size() / 4;
  if (!budget.consume(dimensionCount, sizeof(size_t), "dimension metadata",
                      error)) {
    return false;
  }
  dimensions.clear();
  dimensions.reserve(dimensionCount);
  for (size_t offset = 0; offset < element.data.size(); offset += 4) {
    const std::uint32_t raw = readUInt32(element.data, offset, order);
    const std::int32_t signedDimension = std::bit_cast<std::int32_t>(raw);
    if (signedDimension < 0) {
      error = "MAT v5 dimension cannot be negative";
      return false;
    }
    dimensions.push_back(static_cast<size_t>(signedDimension));
  }
  dimensions = normalizeRuntimeDimensions(std::move(dimensions));
  if (!checkedRuntimeDimensionProduct(dimensions)) {
    error = "MAT v5 dimensions exceed runtime limits";
    return false;
  }
  return true;
}

std::optional<DecodedMatrix>
decodeMatrix(const MatElement &matrix, ByteOrder order,
             const RuntimeMatDecodeOptions &options, DecodeBudget &budget,
             size_t depth, std::string &error);

std::optional<DecodedMatrix>
decodeNumericMatrix(ElementReader &reader, std::string name,
                    std::uint32_t arrayClass, bool logical, bool complex,
                    std::vector<size_t> dimensions, ByteOrder order,
                    DecodeBudget &budget, std::string &error) {
  const auto numericClass = numericClassFromMat(arrayClass, logical);
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!numericClass || !count) {
    error = "MAT v5 numeric array class is unsupported";
    return std::nullopt;
  }
  const auto real = reader.next(error);
  if (!real) {
    return std::nullopt;
  }
  constexpr size_t persistentBytesPerElement = sizeof(double) * 3;
  if (!budget.consume(*count,
                      sizeof(RuntimeNumericElementValue) +
                          persistentBytesPerElement,
                      "numeric representation", error)) {
    return std::nullopt;
  }
  std::vector<RuntimeNumericElementValue> values(*count);
  if (!decodeNumericComponent(*real, order, *numericClass, *count, false,
                              values, error)) {
    return std::nullopt;
  }
  if (complex) {
    const auto imaginary = reader.next(error);
    if (!imaginary || !decodeNumericComponent(*imaginary, order, *numericClass,
                                              *count, true, values, error)) {
      return std::nullopt;
    }
  }
  auto value = runtimeNumericValueFromElements(dimensions, std::move(values),
                                               *numericClass);
  if (!value) {
    error = "MAT v5 numeric array could not be represented";
    return std::nullopt;
  }
  if (complex && *count == 0) {
    value->numericComplex = true;
  }
  return DecodedMatrix{std::move(name), std::move(*value)};
}

std::optional<DecodedMatrix>
decodeCharacterMatrix(ElementReader &reader, std::string name,
                      std::vector<size_t> dimensions, ByteOrder order,
                      DecodeBudget &budget, std::string &error) {
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  const auto characters = reader.next(error);
  if (!count || !characters) {
    return std::nullopt;
  }
  if (!budget.consume(*count, sizeof(char16_t) * 2, "character representation",
                      error)) {
    return std::nullopt;
  }
  std::u16string logicalCharacters;
  if (characters->type == kMiUInt16 || characters->type == kMiUtf16) {
    if (characters->data.size() != *count * 2) {
      error = "MAT v5 character payload size does not match shape";
      return std::nullopt;
    }
    logicalCharacters.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
      logicalCharacters.push_back(static_cast<char16_t>(
          readUInt16(characters->data, index * 2, order)));
    }
  } else if (characters->type == kMiUInt8 || characters->type == kMiUtf8) {
    logicalCharacters = runtimeUtf8ToUtf16(characters->data);
    if (logicalCharacters.size() != *count) {
      error = "MAT v5 UTF-8 character count does not match shape";
      return std::nullopt;
    }
  } else {
    error = "MAT v5 character primitive type is unsupported";
    return std::nullopt;
  }

  std::u16string storage(*count, u'\0');
  for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
    const auto storageOffset = logicalStorageOffset(dimensions, logicalIndex);
    if (!storageOffset || *storageOffset >= storage.size()) {
      error = "MAT v5 character array could not map logical order";
      return std::nullopt;
    }
    storage[*storageOffset] = logicalCharacters[logicalIndex];
  }
  return DecodedMatrix{
      std::move(name),
      makeRuntimeCharacterArray(std::move(dimensions), std::move(storage))};
}

std::optional<DecodedMatrix>
decodeCellMatrix(ElementReader &reader, std::string name,
                 std::vector<size_t> dimensions, ByteOrder order,
                 const RuntimeMatDecodeOptions &options, DecodeBudget &budget,
                 size_t depth, std::string &error) {
  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count) {
    error = "MAT v5 Cell dimensions exceed runtime limits";
    return std::nullopt;
  }
  if (!budget.consume(*count, sizeof(RuntimeValue), "Cell representation",
                      error)) {
    return std::nullopt;
  }
  std::vector<RuntimeValue> storage(*count);
  for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
    const auto element = reader.next(error);
    auto decoded = element ? decodeMatrix(*element, order, options, budget,
                                          depth + 1, error)
                           : std::nullopt;
    const auto storageOffset = logicalStorageOffset(dimensions, logicalIndex);
    if (!decoded || !storageOffset || *storageOffset >= storage.size()) {
      if (error.empty()) {
        error = "MAT v5 Cell array could not map logical order";
      }
      return std::nullopt;
    }
    storage[*storageOffset] = std::move(decoded->value);
  }
  return DecodedMatrix{
      std::move(name),
      makeRuntimeCellValue(std::move(dimensions), std::move(storage))};
}

std::optional<DecodedMatrix>
decodeStructMatrix(ElementReader &reader, std::string name,
                   std::vector<size_t> dimensions, ByteOrder order,
                   const RuntimeMatDecodeOptions &options, DecodeBudget &budget,
                   size_t depth, std::string &error) {
  const auto fieldLengthElement = reader.next(error);
  const auto fieldNamesElement =
      fieldLengthElement ? reader.next(error) : std::nullopt;
  if (!fieldLengthElement || !fieldNamesElement ||
      fieldLengthElement->type != kMiInt32 ||
      fieldLengthElement->data.size() != 4 ||
      (fieldNamesElement->type != kMiInt8 &&
       fieldNamesElement->type != kMiUInt8)) {
    error = "MAT v5 Struct field metadata is invalid";
    return std::nullopt;
  }
  const size_t fieldNameBytes = readUInt32(fieldLengthElement->data, 0, order);
  if (fieldNameBytes == 0 ||
      fieldNamesElement->data.size() % fieldNameBytes != 0) {
    error = "MAT v5 Struct field name table is invalid";
    return std::nullopt;
  }
  const size_t fieldCount = fieldNamesElement->data.size() / fieldNameBytes;
  if (!budget.consume(fieldCount, sizeof(std::string), "Struct field metadata",
                      error) ||
      !budget.consume(fieldNamesElement->data.size(), sizeof(char),
                      "Struct field names", error)) {
    return std::nullopt;
  }
  std::vector<std::string> fields;
  fields.reserve(fieldCount);
  for (size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
    std::string_view bytes = fieldNamesElement->data.substr(
        fieldIndex * fieldNameBytes, fieldNameBytes);
    const size_t end = bytes.find('\0');
    std::string field(bytes.substr(0, end));
    if (!isRuntimeStructFieldName(field)) {
      error = "MAT v5 Struct contains an invalid field name";
      return std::nullopt;
    }
    fields.push_back(std::move(field));
  }

  const auto count = checkedRuntimeDimensionProduct(dimensions);
  if (!count) {
    error = "MAT v5 Struct dimensions exceed runtime limits";
    return std::nullopt;
  }
  constexpr size_t mapNodeOverhead = sizeof(void *) * 4;
  constexpr size_t fieldValueBytes =
      sizeof(std::pair<const std::string, RuntimeValue>) + mapNodeOverhead;
  size_t entryCount = 0;
  if (!checkedMultiply(*count, fieldCount, entryCount) ||
      !budget.consume(*count, sizeof(RuntimeStructElement),
                      "Struct container representation", error) ||
      !budget.consume(entryCount, fieldValueBytes,
                      "Struct field representation", error)) {
    if (error.empty()) {
      error = "MAT v5 decoded Struct field representation exceeds "
              "the configured byte limit";
    }
    return std::nullopt;
  }
  std::vector<RuntimeStructElement> elements(*count);
  for (size_t logicalIndex = 0; logicalIndex < *count; ++logicalIndex) {
    const auto storageOffset = logicalStorageOffset(dimensions, logicalIndex);
    for (const std::string &field : fields) {
      const auto element = reader.next(error);
      auto decoded = element ? decodeMatrix(*element, order, options, budget,
                                            depth + 1, error)
                             : std::nullopt;
      if (!decoded || !storageOffset || *storageOffset >= elements.size()) {
        if (error.empty()) {
          error = "MAT v5 Struct could not map logical order";
        }
        return std::nullopt;
      }
      elements[*storageOffset].emplace(field, std::move(decoded->value));
    }
  }
  return DecodedMatrix{std::move(name),
                       makeRuntimeStructArrayValue(std::move(fields),
                                                   std::move(elements),
                                                   std::move(dimensions))};
}

std::optional<DecodedMatrix>
decodeMatrix(const MatElement &matrix, ByteOrder order,
             const RuntimeMatDecodeOptions &options, DecodeBudget &budget,
             size_t depth, std::string &error) {
  if (matrix.type != kMiMatrix) {
    error = "MAT v5 nested value is not an miMATRIX element";
    return std::nullopt;
  }
  if (depth > options.maximumDepth) {
    error = "MAT v5 value nesting exceeds the configured depth limit";
    return std::nullopt;
  }
  ElementReader reader(matrix.data, order);
  const auto flags = reader.next(error);
  const auto shape = flags ? reader.next(error) : std::nullopt;
  const auto nameElement = shape ? reader.next(error) : std::nullopt;
  if (!flags || !shape || !nameElement || flags->type != kMiUInt32 ||
      flags->data.size() < 8 ||
      (nameElement->type != kMiInt8 && nameElement->type != kMiUInt8 &&
       nameElement->type != kMiUtf8)) {
    error = "MAT v5 matrix header is invalid";
    return std::nullopt;
  }
  const std::uint32_t bits = readUInt32(flags->data, 0, order);
  const std::uint32_t arrayClass = bits & 0xffU;
  const bool logical = (bits & kArrayFlagLogical) != 0;
  const bool complex = (bits & kArrayFlagComplex) != 0;
  std::vector<size_t> dimensions;
  if (!parseDimensions(*shape, order, budget, dimensions, error)) {
    return std::nullopt;
  }
  if (!budget.consume(nameElement->data.size(), sizeof(char), "variable names",
                      error)) {
    return std::nullopt;
  }
  std::string name(nameElement->data);

  std::optional<DecodedMatrix> decoded;
  if (numericClassFromMat(arrayClass, logical)) {
    decoded = decodeNumericMatrix(reader, std::move(name), arrayClass, logical,
                                  complex, std::move(dimensions), order, budget,
                                  error);
  } else if (arrayClass == kMxChar && !logical && !complex) {
    decoded = decodeCharacterMatrix(
        reader, std::move(name), std::move(dimensions), order, budget, error);
  } else if (arrayClass == kMxCell && !logical && !complex) {
    decoded = decodeCellMatrix(reader, std::move(name), std::move(dimensions),
                               order, options, budget, depth, error);
  } else if (arrayClass == kMxStruct && !logical && !complex) {
    decoded = decodeStructMatrix(reader, std::move(name), std::move(dimensions),
                                 order, options, budget, depth, error);
  } else {
    error =
        "MAT v5 array class " + std::to_string(arrayClass) + " is unsupported";
    return std::nullopt;
  }
  if (!decoded) {
    return std::nullopt;
  }
  if (!reader.exhausted()) {
    error = "MAT v5 matrix contains unexpected trailing data elements";
    return std::nullopt;
  }
  return decoded;
}

bool inflateBytes(std::string_view compressed, size_t maximumBytes,
                  std::string &output, std::string &error) {
  if (compressed.size() > std::numeric_limits<mz_ulong>::max()) {
    error = "MAT v5 compressed element exceeds miniz limits";
    return false;
  }
  const size_t doubled = compressed.size() > maximumBytes / 2
                             ? maximumBytes
                             : compressed.size() * 2;
  size_t capacity = std::min(maximumBytes, std::max<size_t>(1024, doubled));
  while (capacity != 0 && capacity <= maximumBytes) {
    output.resize(capacity);
    mz_ulong outputBytes = static_cast<mz_ulong>(capacity);
    const int status = mz_uncompress(
        reinterpret_cast<unsigned char *>(output.data()), &outputBytes,
        reinterpret_cast<const unsigned char *>(compressed.data()),
        static_cast<mz_ulong>(compressed.size()));
    if (status == MZ_OK) {
      output.resize(static_cast<size_t>(outputBytes));
      return true;
    }
    if (status != MZ_BUF_ERROR || capacity == maximumBytes) {
      error = status == MZ_BUF_ERROR
                  ? "MAT v5 decompressed data exceeds the configured "
                    "byte limit"
                  : "miniz could not decompress a MAT v5 element";
      output.clear();
      return false;
    }
    const size_t next =
        capacity > maximumBytes / 2 ? maximumBytes : capacity * 2;
    if (next == capacity) {
      error = "MAT v5 decompressed data exceeds the configured byte "
              "limit";
      output.clear();
      return false;
    }
    capacity = next;
  }
  error = "MAT v5 decompressed data exceeds the configured byte limit";
  output.clear();
  return false;
}

bool decodeTopLevelElements(std::string_view bytes, ByteOrder order,
                            const RuntimeMatDecodeOptions &options,
                            size_t &expandedBytes, DecodeBudget &budget,
                            size_t compressionDepth,
                            std::vector<RuntimeMatVariable> &variables,
                            std::string &error) {
  if (compressionDepth > options.maximumDepth) {
    error = "MAT v5 compressed nesting exceeds the configured depth "
            "limit";
    return false;
  }
  ElementReader reader(bytes, order);
  while (!reader.exhausted()) {
    if (reader.remaining() < 8) {
      if (std::all_of(bytes.end() -
                          static_cast<std::ptrdiff_t>(reader.remaining()),
                      bytes.end(), [](char value) { return value == '\0'; })) {
        return true;
      }
      error = "MAT v5 file has trailing non-padding bytes";
      return false;
    }
    const auto element = reader.next(error);
    if (!element) {
      return false;
    }
    if (element->type == kMiCompressed) {
      const size_t remainingBudget = expandedBytes <= options.maximumBytes
                                         ? options.maximumBytes - expandedBytes
                                         : 0;
      std::string inflated;
      if (!inflateBytes(element->data, remainingBudget, inflated, error)) {
        return false;
      }
      expandedBytes += inflated.size();
      if (!decodeTopLevelElements(inflated, order, options, expandedBytes,
                                  budget, compressionDepth + 1, variables,
                                  error)) {
        return false;
      }
      continue;
    }
    auto decoded = decodeMatrix(*element, order, options, budget, 0, error);
    if (!decoded) {
      return false;
    }
    if (decoded->name.empty() || !isRuntimeStructFieldName(decoded->name)) {
      error = "MAT v5 top-level variable name is invalid";
      return false;
    }
    if (!budget.consume(1, sizeof(RuntimeMatVariable), "variable table",
                        error)) {
      return false;
    }
    variables.push_back(RuntimeMatVariable{std::move(decoded->name),
                                           std::move(decoded->value)});
  }
  return true;
}

} // namespace

RuntimeMatEncodeResult
runtimeEncodeMatV5(const std::vector<RuntimeMatVariable> &variables,
                   const RuntimeMatEncodeOptions &options) {
  RuntimeMatEncodeResult result;
  if (options.maximumBytes < kMatHeaderBytes) {
    result.error = "MAT v5 byte limit is smaller than its header";
    return result;
  }

  std::string header(116, ' ');
  constexpr std::string_view description =
      "MATLAB 5.0 MAT-file, Platform: MParser, Created by MParser";
  std::copy(description.begin(), description.end(), header.begin());
  header.append(8, '\0');
  appendLittleUnsigned(header, std::uint16_t{0x0100});
  header.append("IM", 2);
  result.bytes = std::move(header);

  for (const RuntimeMatVariable &variable : variables) {
    if (!isRuntimeStructFieldName(variable.name)) {
      result.error = "MAT v5 variable name is invalid: " + variable.name;
      result.bytes.clear();
      return result;
    }
    std::string matrix;
    if (!encodeMatrix(variable.value, variable.name, options, 0, matrix,
                      result.error)) {
      result.error = "variable " + variable.name + ": " + result.error;
      result.bytes.clear();
      return result;
    }
    if (options.compress) {
      std::string compressed;
      if (!compressBytes(matrix, options.maximumBytes, compressed,
                         result.error) ||
          !appendDataElement(result.bytes, kMiCompressed, compressed,
                             options.maximumBytes, result.error)) {
        result.error = "variable " + variable.name + ": " + result.error;
        result.bytes.clear();
        return result;
      }
    } else if (!appendBytes(result.bytes, matrix, options.maximumBytes,
                            result.error)) {
      result.error = "variable " + variable.name + ": " + result.error;
      result.bytes.clear();
      return result;
    }
  }
  result.succeeded = true;
  return result;
}

RuntimeMatDecodeResult
runtimeDecodeMatV5(std::string_view bytes,
                   const RuntimeMatDecodeOptions &options) {
  RuntimeMatDecodeResult result;
  if (bytes.size() < kMatHeaderBytes) {
    result.error = "MAT v5 file is smaller than its 128-byte header";
    return result;
  }
  if (bytes.size() > options.maximumBytes) {
    result.error = "MAT v5 input exceeds the configured byte limit";
    return result;
  }
  ByteOrder order;
  if (bytes.substr(126, 2) == "IM") {
    order = ByteOrder::Little;
  } else if (bytes.substr(126, 2) == "MI") {
    order = ByteOrder::Big;
  } else {
    result.error = "MAT v5 endian indicator is invalid";
    return result;
  }
  if (readUInt16(bytes, 124, order) != 0x0100U) {
    result.error = "MAT v5 version field is unsupported";
    return result;
  }
  size_t expandedBytes = bytes.size();
  DecodeBudget budget{options.maximumBytes};
  if (!decodeTopLevelElements(bytes.substr(kMatHeaderBytes), order, options,
                              expandedBytes, budget, 0, result.variables,
                              result.error)) {
    result.variables.clear();
    return result;
  }
  result.succeeded = true;
  return result;
}

} // namespace mparser
