#include "mparser/runtime/core/value/runtime_sparse.h"

#include "mparser/runtime/core/value/runtime_shape.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace mparser {
namespace {

RuntimeSparseOperationResult failure(std::string message) {
    return RuntimeSparseOperationResult{false, {}, std::move(message)};
}

RuntimeSparseOperationResult success(RuntimeValue value) {
    return RuntimeSparseOperationResult{true, std::move(value), {}};
}

bool isZero(const RuntimeNumericElementValue& value) {
    return value.real == 0.0 &&
           (!value.complex || value.imaginary == 0.0);
}

bool isSupportedSparseClass(RuntimeNumericClass numericClass) {
    return numericClass == RuntimeNumericClass::Double ||
           numericClass == RuntimeNumericClass::Logical;
}

std::optional<size_t> nonnegativeScalar(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    return element ? runtimeNumericElementAsNonnegativeSize(*element)
                   : std::nullopt;
}

std::optional<std::vector<RuntimeNumericElementValue>> numericElements(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }
    const size_t count = runtimeShapeElementCount(value);
    std::vector<RuntimeNumericElementValue> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element) {
            return std::nullopt;
        }
        result.push_back(*element);
    }
    return result;
}

using SparseEntries =
    std::map<std::pair<size_t, size_t>, RuntimeNumericElementValue>;

RuntimeSparseOperationResult makeSparseFromEntries(
    size_t rows, size_t columns, RuntimeNumericClass numericClass,
    bool complex, SparseEntries entries) {
    if (!isSupportedSparseClass(numericClass)) {
        return failure("sparse supports double and logical values");
    }
    if (rows == std::numeric_limits<size_t>::max() ||
        columns == std::numeric_limits<size_t>::max() ||
        !checkedRuntimeDimensionProduct({rows, columns})) {
        return failure("sparse dimensions are too large");
    }
    auto storage = std::make_shared<RuntimeSparseStorage>();
    storage->rows = rows;
    storage->columns = columns;
    storage->numericClass = numericClass;
    storage->complex = complex;
    storage->columnPointers.assign(columns + 1, 0);

    auto entry = entries.begin();
    for (size_t column = 0; column < columns; ++column) {
        storage->columnPointers[column] = storage->values.size();
        while (entry != entries.end()) {
            if (entry->first.first != column) {
                break;
            }
            const size_t row = entry->first.second;
            RuntimeNumericElementValue value = entry->second;
            value.numericClass = numericClass;
            const auto converted = runtimeConvertNumericElementValue(
                value, numericClass);
            if (!converted) {
                return failure("sparse entry is not representable in its class");
            }
            if (row >= rows || isZero(*converted)) {
                ++entry;
                continue;
            }
            storage->rowIndices.push_back(row);
            storage->values.push_back(converted->real);
            if (complex) {
                storage->imaginaryValues.push_back(
                    converted->complex ? converted->imaginary : 0.0);
            }
            ++entry;
        }
    }
    storage->columnPointers[columns] = storage->values.size();
    return success(makeRuntimeSparseValue(std::move(storage)));
}

RuntimeSparseOperationResult makeSparseFromLogicalElements(
    const std::vector<size_t>& dimensions,
    const std::vector<RuntimeNumericElementValue>& elements,
    RuntimeNumericClass numericClass) {
    if (dimensions.size() != 2) {
        return failure("sparse values must be two-dimensional");
    }
    const size_t rows = dimensions[0];
    const size_t columns = dimensions[1];
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != elements.size()) {
        return failure("sparse input shape does not match its elements");
    }

    if (!isSupportedSparseClass(numericClass)) {
        return failure("sparse input must be double or logical");
    }
    bool complex = false;
    SparseEntries entries;
    for (size_t logicalIndex = 0; logicalIndex < elements.size();
         ++logicalIndex) {
        const auto converted = runtimeConvertNumericElementValue(
            elements[logicalIndex], numericClass);
        if (!converted) {
            return failure("sparse input contains an unsupported numeric value");
        }
        complex = complex || converted->complex;
        if (rows == 0) {
            continue;
        }
        const size_t row = logicalIndex % rows;
        const size_t column = logicalIndex / rows;
        if (!isZero(*converted)) {
            entries[{column, row}] = *converted;
        }
    }
    return makeSparseFromEntries(rows, columns, numericClass, complex,
                                 std::move(entries));
}

RuntimeSparseOperationResult makeSparseFromTriplets(
    const RuntimeValue& rowIndices, const RuntimeValue& columnIndices,
    const RuntimeValue& values, size_t rows, size_t columns) {
    const size_t rowCount = runtimeShapeElementCount(rowIndices);
    const size_t columnCount = runtimeShapeElementCount(columnIndices);
    const size_t valueCount = runtimeShapeElementCount(values);
    if (rowCount != columnCount ||
        (valueCount != 1 && valueCount != rowCount)) {
        return failure("sparse triplet inputs must have matching lengths");
    }

    const RuntimeNumericClass numericClass = values.numericClass;
    if (!isSupportedSparseClass(numericClass)) {
        return failure("sparse triplet values must be double or logical");
    }
    bool complex = values.numericComplex;
    SparseEntries entries;
    size_t maximumRow = 0;
    size_t maximumColumn = 0;
    for (size_t index = 0; index < rowCount; ++index) {
        const auto rowValue = runtimeNumericElementValue(rowIndices, index);
        const auto columnValue =
            runtimeNumericElementValue(columnIndices, index);
        const auto value = runtimeNumericElementValue(
            values, valueCount == 1 ? 0 : index);
        if (!rowValue || !columnValue || !value || rowValue->complex ||
            columnValue->complex) {
            return failure("sparse triplet indices must be real integers");
        }
        const auto row = runtimeNumericElementAsNonnegativeSize(*rowValue);
        const auto column =
            runtimeNumericElementAsNonnegativeSize(*columnValue);
        if (!row || !column || *row == 0 || *column == 0) {
            return failure("sparse triplet indices must be positive integers");
        }
        const auto converted = runtimeConvertNumericElementValue(
            *value, numericClass);
        if (!converted) {
            return failure("sparse triplet value has an unsupported class");
        }
        const size_t zeroRow = *row - 1;
        const size_t zeroColumn = *column - 1;
        maximumRow = std::max(maximumRow, *row);
        maximumColumn = std::max(maximumColumn, *column);
        complex = complex || converted->complex;

        auto& accumulated = entries[{zeroColumn, zeroRow}];
        if (accumulated.numericClass != numericClass &&
            accumulated.real == 0.0 && !accumulated.complex) {
            accumulated.numericClass = numericClass;
        }
        if (numericClass == RuntimeNumericClass::Logical) {
            accumulated.real =
                (accumulated.real != 0.0 || converted->real != 0.0)
                    ? 1.0
                    : 0.0;
            accumulated.imaginary = 0.0;
            accumulated.complex = false;
        } else {
            accumulated.real += converted->real;
            accumulated.imaginary +=
                converted->complex ? converted->imaginary : 0.0;
            accumulated.complex =
                accumulated.complex || converted->complex;
        }
        accumulated.numericClass = numericClass;
    }

    if (maximumRow > rows || maximumColumn > columns) {
        return failure("sparse triplet index exceeds the requested shape");
    }
    return makeSparseFromEntries(rows, columns, numericClass, complex,
                                 std::move(entries));
}

std::optional<size_t> sparseOffset(const RuntimeSparseStorage& storage,
                                    size_t row, size_t column) {
    if (column >= storage.columns || row >= storage.rows) {
        return std::nullopt;
    }
    const size_t begin = storage.columnPointers[column];
    const size_t end = storage.columnPointers[column + 1];
    const auto iterator = std::lower_bound(
        storage.rowIndices.begin() +
            static_cast<std::ptrdiff_t>(begin),
        storage.rowIndices.begin() + static_cast<std::ptrdiff_t>(end), row);
    if (iterator == storage.rowIndices.begin() +
                        static_cast<std::ptrdiff_t>(end) ||
        *iterator != row) {
        return std::nullopt;
    }
    return static_cast<size_t>(
        std::distance(storage.rowIndices.begin(), iterator));
}

RuntimeNumericElementValue zeroElement(const RuntimeSparseStorage& storage) {
    RuntimeNumericElementValue result;
    result.numericClass = storage.numericClass;
    result.complex = storage.complex;
    return result;
}

} // namespace

bool isRuntimeSparseValue(const RuntimeValue& value) {
    return value.kind == RuntimeValueKind::Object &&
           value.className == kRuntimeSparseClassName &&
           value.sparseStorage != nullptr;
}

const RuntimeSparseStorage* runtimeSparseStorage(
    const RuntimeValue& value) {
    return isRuntimeSparseValue(value) ? value.sparseStorage.get() : nullptr;
}

RuntimeSparseStorage* runtimeMutableSparseStorage(RuntimeValue& value) {
    if (!isRuntimeSparseValue(value)) {
        return nullptr;
    }
    if (value.sparseStorage.use_count() != 1) {
        value.sparseStorage =
            std::make_shared<RuntimeSparseStorage>(*value.sparseStorage);
    }
    return value.sparseStorage.get();
}

RuntimeValue makeRuntimeSparseValue(
    std::shared_ptr<RuntimeSparseStorage> storage) {
    RuntimeValue result;
    result.kind = RuntimeValueKind::Object;
    result.className = std::string(kRuntimeSparseClassName);
    result.handleObject = false;
    result.sparseStorage = std::move(storage);
    if (result.sparseStorage) {
        result.numericClass = result.sparseStorage->numericClass;
        result.numericComplex = result.sparseStorage->complex;
        setRuntimeDimensions(result, {result.sparseStorage->rows,
                                      result.sparseStorage->columns});
    } else {
        setRuntimeDimensions(result, {0, 0});
    }
    return result;
}

bool validateRuntimeSparseStorage(const RuntimeValue& value,
                                  std::string& error) {
    const RuntimeSparseStorage* storage = runtimeSparseStorage(value);
    if (!storage) {
        error = "sparse value has no CSC storage";
        return false;
    }
    if (!isSupportedSparseClass(storage->numericClass)) {
        error = "sparse value has an unsupported numeric class";
        return false;
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions !=
        std::vector<size_t>{storage->rows, storage->columns}) {
        error = "sparse shape does not match CSC storage";
        return false;
    }
    if (storage->rows == std::numeric_limits<size_t>::max() ||
        storage->columns == std::numeric_limits<size_t>::max() ||
        storage->columnPointers.size() != storage->columns + 1 ||
        storage->columnPointers.empty() ||
        storage->columnPointers.front() != 0 ||
        storage->rowIndices.size() != storage->values.size() ||
        storage->columnPointers.back() != storage->values.size()) {
        error = "sparse CSC pointer and payload sizes are inconsistent";
        return false;
    }
    if (storage->complex &&
        storage->imaginaryValues.size() != storage->values.size()) {
        error = "complex sparse values need one imaginary entry per nonzero";
        return false;
    }
    if (!storage->complex && !storage->imaginaryValues.empty()) {
        error = "real sparse values cannot carry imaginary storage";
        return false;
    }
    for (size_t column = 0; column < storage->columns; ++column) {
        const size_t begin = storage->columnPointers[column];
        const size_t end = storage->columnPointers[column + 1];
        if (begin > end || end > storage->rowIndices.size()) {
            error = "sparse CSC column pointers are not monotonic";
            return false;
        }
        size_t previous = 0;
        bool first = true;
        for (size_t offset = begin; offset < end; ++offset) {
            const size_t row = storage->rowIndices[offset];
            if (row >= storage->rows || (!first && row <= previous) ||
                (storage->values[offset] == 0.0 &&
                 (!storage->complex ||
                  storage->imaginaryValues[offset] == 0.0))) {
                error = "sparse CSC rows are invalid or contain explicit zero";
                return false;
            }
            previous = row;
            first = false;
        }
    }
    return true;
}

std::optional<RuntimeNumericElementValue> runtimeSparseElementValue(
    const RuntimeValue& value, size_t logicalIndex) {
    const RuntimeSparseStorage* storage = runtimeSparseStorage(value);
    const auto count = storage
                           ? checkedRuntimeDimensionProduct(
                                 {storage->rows, storage->columns})
                           : std::nullopt;
    if (!storage || !count || storage->rows == 0 ||
        logicalIndex >= *count) {
        return std::nullopt;
    }
    const size_t row = logicalIndex % storage->rows;
    const size_t column = logicalIndex / storage->rows;
    const auto offset = sparseOffset(*storage, row, column);
    if (!offset) {
        return zeroElement(*storage);
    }
    RuntimeNumericElementValue result;
    result.numericClass = storage->numericClass;
    result.real = storage->values[*offset];
    result.complex = storage->complex;
    if (storage->complex) {
        result.imaginary = storage->imaginaryValues[*offset];
    }
    return result;
}

std::optional<RuntimeNumericElementValue>
runtimeSparseStorageElementValue(const RuntimeValue& value,
                                 size_t storageOffset) {
    const RuntimeSparseStorage* storage = runtimeSparseStorage(value);
    const auto count = storage
                           ? checkedRuntimeDimensionProduct(
                                 {storage->rows, storage->columns})
                           : std::nullopt;
    if (!storage || !count || storage->columns == 0 ||
        storageOffset >= *count) {
        return std::nullopt;
    }
    const size_t row = storageOffset / storage->columns;
    const size_t column = storageOffset % storage->columns;
    return runtimeSparseElementValue(
        value, row + column * storage->rows);
}

bool runtimeStoreSparseElementValue(
    RuntimeValue& target, size_t logicalIndex,
    const RuntimeNumericElementValue& value) {
    RuntimeSparseStorage* storage = runtimeMutableSparseStorage(target);
    const auto count = storage
                           ? checkedRuntimeDimensionProduct(
                                 {storage->rows, storage->columns})
                           : std::nullopt;
    if (!storage || !count || storage->rows == 0 ||
        logicalIndex >= *count) {
        return false;
    }
    const auto converted = runtimeConvertNumericElementValue(
        value, storage->numericClass);
    if (!converted) {
        return false;
    }
    const size_t row = logicalIndex % storage->rows;
    const size_t column = logicalIndex / storage->rows;
    const size_t begin = storage->columnPointers[column];
    const size_t end = storage->columnPointers[column + 1];
    auto iterator = std::lower_bound(
        storage->rowIndices.begin() + static_cast<std::ptrdiff_t>(begin),
        storage->rowIndices.begin() + static_cast<std::ptrdiff_t>(end), row);
    const size_t offset = static_cast<size_t>(
        std::distance(storage->rowIndices.begin(), iterator));
    const bool present = iterator != storage->rowIndices.begin() +
                                      static_cast<std::ptrdiff_t>(end) &&
                         *iterator == row;

    if (converted->complex && !storage->complex) {
        storage->complex = true;
        storage->imaginaryValues.assign(storage->values.size(), 0.0);
        target.numericComplex = true;
    }
    if (isZero(*converted)) {
        if (!present) {
            return true;
        }
        storage->rowIndices.erase(storage->rowIndices.begin() +
                                  static_cast<std::ptrdiff_t>(offset));
        storage->values.erase(storage->values.begin() +
                              static_cast<std::ptrdiff_t>(offset));
        if (storage->complex) {
            storage->imaginaryValues.erase(
                storage->imaginaryValues.begin() +
                static_cast<std::ptrdiff_t>(offset));
        }
        for (size_t index = column + 1;
             index < storage->columnPointers.size(); ++index) {
            --storage->columnPointers[index];
        }
        return true;
    }

    if (present) {
        storage->values[offset] = converted->real;
        if (storage->complex) {
            storage->imaginaryValues[offset] =
                converted->complex ? converted->imaginary : 0.0;
        }
        return true;
    }

    storage->rowIndices.insert(
        storage->rowIndices.begin() + static_cast<std::ptrdiff_t>(offset),
        row);
    storage->values.insert(
        storage->values.begin() + static_cast<std::ptrdiff_t>(offset),
        converted->real);
    if (storage->complex) {
        storage->imaginaryValues.insert(
            storage->imaginaryValues.begin() +
                static_cast<std::ptrdiff_t>(offset),
            converted->complex ? converted->imaginary : 0.0);
    }
    for (size_t index = column + 1;
         index < storage->columnPointers.size(); ++index) {
        ++storage->columnPointers[index];
    }
    return true;
}

RuntimeSparseOperationResult runtimeSparseFromNumeric(
    const RuntimeValue& value) {
    if (isRuntimeSparseValue(value)) {
        return success(value);
    }
    if (!isRuntimeNumericValue(value)) {
        return failure("sparse input must be numeric");
    }
    const auto dimensions = runtimeDimensions(value);
    const auto elements = numericElements(value);
    if (!elements) {
        return failure("sparse input contains an unreadable numeric element");
    }
    return makeSparseFromLogicalElements(
        dimensions, *elements, value.numericClass);
}

RuntimeSparseOperationResult runtimeSparseToFull(
    const RuntimeValue& value) {
    const RuntimeSparseStorage* storage = runtimeSparseStorage(value);
    if (!storage) {
        return failure("full requires a sparse value");
    }
    const auto count = checkedRuntimeDimensionProduct(
        {storage->rows, storage->columns});
    if (!count) {
        return failure("sparse dimensions are too large");
    }
    std::vector<RuntimeNumericElementValue> elements(*count,
                                                      zeroElement(*storage));
    for (size_t column = 0; column < storage->columns; ++column) {
        for (size_t offset = storage->columnPointers[column];
             offset < storage->columnPointers[column + 1]; ++offset) {
            const size_t logicalIndex =
                storage->rowIndices[offset] + column * storage->rows;
            elements[logicalIndex].real = storage->values[offset];
            elements[logicalIndex].complex = storage->complex;
            if (storage->complex) {
                elements[logicalIndex].imaginary =
                    storage->imaginaryValues[offset];
            }
        }
    }
    auto result = runtimeNumericValueFromElements(
        {storage->rows, storage->columns}, std::move(elements),
        storage->numericClass);
    return result ? success(std::move(*result))
                  : failure("full could not construct a dense result");
}

RuntimeSparseOperationResult runtimeSparseNonzeros(
    const RuntimeValue& value) {
    if (isRuntimeSparseValue(value)) {
        const RuntimeSparseStorage& storage = *value.sparseStorage;
        std::vector<RuntimeNumericElementValue> elements;
        elements.reserve(storage.values.size());
        for (size_t offset = 0; offset < storage.values.size(); ++offset) {
            RuntimeNumericElementValue element;
            element.numericClass = storage.numericClass;
            element.real = storage.values[offset];
            element.complex = storage.complex;
            if (storage.complex) {
                element.imaginary = storage.imaginaryValues[offset];
            }
            elements.push_back(element);
        }
        const size_t count = elements.size();
        auto result = runtimeNumericValueFromElements(
            {count, 1}, std::move(elements),
            storage.numericClass);
        return result ? success(std::move(*result))
                      : failure("nonzeros could not construct its result");
    }
    if (!isRuntimeNumericValue(value)) {
        return failure("nonzeros requires a numeric value");
    }
    const auto elements = numericElements(value);
    if (!elements) {
        return failure("nonzeros could not read its input");
    }
    std::vector<RuntimeNumericElementValue> nonzero;
    for (const auto& element : *elements) {
        if (!isZero(element)) {
            nonzero.push_back(element);
        }
    }
    const size_t count = nonzero.size();
    auto result = runtimeNumericValueFromElements(
        {count, 1}, std::move(nonzero), value.numericClass);
    return result ? success(std::move(*result))
                  : failure("nonzeros could not construct its result");
}

RuntimeSparseOperationResult runtimeSparseSpones(
    const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value)) {
        return failure("spones requires a numeric value");
    }
    const auto dimensions = runtimeDimensions(value);
    if (dimensions.size() != 2) {
        return failure("spones requires a two-dimensional value");
    }
    if (isRuntimeSparseValue(value)) {
        auto storage = std::make_shared<RuntimeSparseStorage>(
            *value.sparseStorage);
        storage->numericClass = RuntimeNumericClass::Double;
        storage->complex = false;
        storage->imaginaryValues.clear();
        std::fill(storage->values.begin(), storage->values.end(), 1.0);
        return success(makeRuntimeSparseValue(std::move(storage)));
    }

    const auto elements = numericElements(value);
    if (!elements) {
        return failure("spones could not read its input");
    }
    SparseEntries entries;
    const size_t rows = dimensions[0];
    for (size_t logicalIndex = 0; logicalIndex < elements->size();
         ++logicalIndex) {
        if (!isZero((*elements)[logicalIndex]) && rows != 0) {
            RuntimeNumericElementValue one;
            one.real = 1.0;
            entries[{logicalIndex / rows, logicalIndex % rows}] = one;
        }
    }
    return makeSparseFromEntries(
        dimensions[0], dimensions[1], RuntimeNumericClass::Double,
        false, std::move(entries));
}

RuntimeSparseOperationResult runtimeSparseConvertClass(
    const RuntimeValue& value, RuntimeNumericClass numericClass) {
    if (!isRuntimeSparseValue(value) || !isSupportedSparseClass(numericClass)) {
        return failure("sparse class conversion requires double or logical");
    }
    if (value.numericClass == numericClass) {
        return success(value);
    }
    auto dense = runtimeSparseToFull(value);
    if (!dense.succeeded) {
        return dense;
    }
    auto converted = runtimeConvertNumericClass(
        std::move(dense.value), numericClass);
    if (!converted) {
        return failure("sparse class conversion failed");
    }
    return runtimeSparseFromNumeric(*converted);
}

RuntimeSparseOperationResult runtimeConstructSparse(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() == 1) {
        return runtimeSparseFromNumeric(arguments.front());
    }
    if (arguments.size() == 2) {
        const auto rows = nonnegativeScalar(arguments[0]);
        const auto columns = nonnegativeScalar(arguments[1]);
        if (!rows || !columns) {
            return failure("sparse(m,n) requires nonnegative scalar dimensions");
        }
        return makeSparseFromEntries(*rows, *columns,
                                     RuntimeNumericClass::Double, false, {});
    }
    if (arguments.size() != 3 && arguments.size() != 5 &&
        arguments.size() != 6) {
        return failure("sparse expects one, two, three, five, or six inputs");
    }
    if (!isRuntimeNumericValue(arguments[0]) ||
        !isRuntimeNumericValue(arguments[1]) ||
        !isRuntimeNumericValue(arguments[2])) {
        return failure("sparse triplet inputs must be numeric");
    }

    size_t rows = 0;
    size_t columns = 0;
    if (arguments.size() >= 5) {
        const auto requestedRows = nonnegativeScalar(arguments[3]);
        const auto requestedColumns = nonnegativeScalar(arguments[4]);
        if (!requestedRows || !requestedColumns) {
            return failure("sparse dimensions must be nonnegative integers");
        }
        rows = *requestedRows;
        columns = *requestedColumns;
    } else {
        const auto rowCount = runtimeShapeElementCount(arguments[0]);
        const auto columnCount = runtimeShapeElementCount(arguments[1]);
        if (rowCount != columnCount) {
            return failure("sparse triplet inputs must have matching lengths");
        }
        for (size_t index = 0; index < rowCount; ++index) {
            const auto row = runtimeNumericElementValue(arguments[0], index);
            const auto column = runtimeNumericElementValue(arguments[1], index);
            if (!row || !column || row->complex || column->complex) {
                return failure("sparse triplet indices must be real integers");
            }
            const auto rowSize = runtimeNumericElementAsNonnegativeSize(*row);
            const auto columnSize =
                runtimeNumericElementAsNonnegativeSize(*column);
            if (!rowSize || !columnSize) {
                return failure("sparse triplet indices must be integers");
            }
            rows = std::max(rows, *rowSize);
            columns = std::max(columns, *columnSize);
        }
    }
    if (arguments.size() == 6 && !nonnegativeScalar(arguments[5])) {
        return failure("sparse nzmax must be a nonnegative integer");
    }
    return makeSparseFromTriplets(arguments[0], arguments[1], arguments[2],
                                  rows, columns);
}

RuntimeSparseOperationResult runtimeSpalloc(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 3) {
        return failure("spalloc expects m, n, and nzmax");
    }
    const auto rows = nonnegativeScalar(arguments[0]);
    const auto columns = nonnegativeScalar(arguments[1]);
    const auto capacity = nonnegativeScalar(arguments[2]);
    if (!rows || !columns || !capacity) {
        return failure("spalloc dimensions and nzmax must be nonnegative integers");
    }
    (void)capacity;
    return makeSparseFromEntries(*rows, *columns,
                                 RuntimeNumericClass::Double, false, {});
}

RuntimeSparseOperationResult runtimeSpeye(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 1 && arguments.size() != 2) {
        return failure("speye expects n or m and n");
    }
    const auto rows = nonnegativeScalar(arguments.front());
    const auto columns = arguments.size() == 1
                             ? rows
                             : nonnegativeScalar(arguments[1]);
    if (!rows || !columns) {
        return failure("speye dimensions must be nonnegative integers");
    }
    SparseEntries entries;
    const size_t diagonal = std::min(*rows, *columns);
    for (size_t index = 0; index < diagonal; ++index) {
        RuntimeNumericElementValue one;
        one.real = 1.0;
        entries[{index, index}] = one;
    }
    return makeSparseFromEntries(*rows, *columns,
                                 RuntimeNumericClass::Double, false,
                                 std::move(entries));
}

RuntimeSparseOperationResult runtimeSparseUnary(
    std::string_view operation, const RuntimeValue& value) {
    if (!isRuntimeSparseValue(value)) {
        return failure("sparse unary operation requires a sparse value");
    }
    if (operation == "+") {
        return success(value);
    }
    if (operation != "-") {
        return failure("only unary plus and minus preserve sparse storage");
    }
    auto storage = std::make_shared<RuntimeSparseStorage>(
        *value.sparseStorage);
    for (double& element : storage->values) {
        element = -element;
    }
    for (double& element : storage->imaginaryValues) {
        element = -element;
    }
    return success(makeRuntimeSparseValue(std::move(storage)));
}

RuntimeSparseOperationResult runtimeSparseTranspose(
    const RuntimeValue& value, bool conjugate) {
    const RuntimeSparseStorage* source = runtimeSparseStorage(value);
    if (!source) {
        return failure("transpose requires a sparse value");
    }
    auto storage = std::make_shared<RuntimeSparseStorage>();
    storage->rows = source->columns;
    storage->columns = source->rows;
    storage->numericClass = source->numericClass;
    storage->complex = source->complex;
    storage->columnPointers.assign(storage->columns + 1, 0);
    for (const size_t row : source->rowIndices) {
        ++storage->columnPointers[row + 1];
    }
    for (size_t column = 0; column < storage->columns; ++column) {
        storage->columnPointers[column + 1] +=
            storage->columnPointers[column];
    }
    storage->rowIndices.resize(source->rowIndices.size());
    storage->values.resize(source->values.size());
    if (storage->complex) {
        storage->imaginaryValues.resize(source->values.size());
    }
    std::vector<size_t> next = storage->columnPointers;
    for (size_t sourceColumn = 0; sourceColumn < source->columns;
         ++sourceColumn) {
        for (size_t offset = source->columnPointers[sourceColumn];
             offset < source->columnPointers[sourceColumn + 1]; ++offset) {
            const size_t outputColumn = source->rowIndices[offset];
            const size_t outputOffset = next[outputColumn]++;
            storage->rowIndices[outputOffset] = sourceColumn;
            storage->values[outputOffset] = source->values[offset];
            if (storage->complex) {
                storage->imaginaryValues[outputOffset] =
                    conjugate ? -source->imaginaryValues[offset]
                              : source->imaginaryValues[offset];
            }
        }
    }
    return success(makeRuntimeSparseValue(std::move(storage)));
}

} // namespace mparser
