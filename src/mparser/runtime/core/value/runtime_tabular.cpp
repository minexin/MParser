#include "mparser/runtime/core/value/runtime_tabular.h"

#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_categorical.h"
#include "mparser/runtime/core/value/runtime_cell.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_struct.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <memory>

namespace mparser {

bool isRuntimeTabularValue(const RuntimeValue& value) {
    if (value.kind != RuntimeValueKind::Object ||
        !value.tabularStorage) {
        return false;
    }
    return (value.tabularStorage->kind == RuntimeTabularKind::Table &&
            value.className == kRuntimeTableClassName) ||
           (value.tabularStorage->kind == RuntimeTabularKind::Timetable &&
            value.className == kRuntimeTimetableClassName);
}

const RuntimeTabularStorage* runtimeTabularStorage(
    const RuntimeValue& value) {
    return isRuntimeTabularValue(value) ? value.tabularStorage.get()
                                        : nullptr;
}

RuntimeTabularStorage* runtimeMutableTabularStorage(RuntimeValue& value) {
    if (!isRuntimeTabularValue(value)) {
        return nullptr;
    }
    if (value.tabularStorage.use_count() != 1) {
        value.tabularStorage =
            std::make_shared<RuntimeTabularStorage>(*value.tabularStorage);
    }
    return value.tabularStorage.get();
}

bool runtimeTabularVariableSupportsRows(const RuntimeValue& value) {
    return isRuntimeTabularValue(value) ||
           isRuntimeCategoricalValue(value) ||
           value.kind == RuntimeValueKind::Struct ||
           value.kind == RuntimeValueKind::Cell ||
           isRuntimeTextValue(value) ||
           value.kind == RuntimeValueKind::MissingArray ||
           isRuntimeClassObject(value) ||
           isRuntimeNumericValue(value);
}

} // namespace mparser
