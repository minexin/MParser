#include "mparser/runtime/core/value/runtime_datetime.h"

#include "mparser/runtime/core/object_model/runtime_object.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/runtime/core/value/runtime_shape.h"
#include "mparser/runtime/core/value/runtime_text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mparser {
namespace {

RuntimeTemporalOperationResult failure(std::string error) {
    return RuntimeTemporalOperationResult{false, {}, std::move(error)};
}

RuntimeTemporalOperationResult success(RuntimeValue value) {
    return RuntimeTemporalOperationResult{true, std::move(value), {}};
}

constexpr double kSecondsPerDay = 86400.0;
constexpr std::int64_t kMinimumSupportedYear = 1;
constexpr std::int64_t kMaximumSupportedYear = 9999;

struct CivilDate {
    std::int64_t year = 1970;
    unsigned month = 1;
    unsigned day = 1;
};

// Proleptic Gregorian conversion, kept local so datetime does not inherit
// platform-specific calendar or timezone behavior.
std::int64_t daysFromCivil(std::int64_t year, unsigned month,
                           unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era =
        (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const int shiftedMonth =
        static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned dayOfYear =
        (153 * shiftedMonth + 2) /
            5 +
        day - 1;
    const unsigned dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

CivilDate civilFromDays(std::int64_t serialDays) {
    serialDays += 719468;
    const std::int64_t era =
        (serialDays >= 0 ? serialDays : serialDays - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(
        serialDays - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 -
         dayOfEra / 146096) /
        365;
    const std::int64_t year =
        static_cast<std::int64_t>(yearOfEra) + era * 400;
    const unsigned dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned month =
        (5 * dayOfYear + 2) / 153;
    const unsigned day =
        dayOfYear - (153 * month + 2) / 5 + 1;
    const std::int64_t adjustedYear =
        year + (month < 10 ? 0 : 1);
    return CivilDate{adjustedYear, month + (month < 10 ? 3 : -9), day};
}

bool validDate(std::int64_t year, unsigned month, unsigned day) {
    if (year < kMinimumSupportedYear ||
        year > kMaximumSupportedYear || month < 1 || month > 12 ||
        day < 1) {
        return false;
    }
    const unsigned nextMonth = month == 12 ? 1 : month + 1;
    const std::int64_t nextYear = month == 12 ? year + 1 : year;
    return daysFromCivil(nextYear, nextMonth, 1) -
               daysFromCivil(year, month, 1) >=
           static_cast<std::int64_t>(day);
}

bool supportedDateTimeSerial(double serial) {
    if (std::isnan(serial)) {
        return true;
    }
    if (!std::isfinite(serial)) {
        return false;
    }
    const double integral = std::floor(serial);
    const auto firstDay = daysFromCivil(
        kMinimumSupportedYear, 1, 1);
    const auto lastDay = daysFromCivil(
        kMaximumSupportedYear, 12, 31);
    return integral >= static_cast<double>(firstDay) &&
           integral <= static_cast<double>(lastDay);
}

bool validClock(double hour, double minute, double second) {
    return std::isfinite(hour) && std::isfinite(minute) &&
           std::isfinite(second) && hour >= 0.0 && hour <= 23.0 &&
           minute >= 0.0 && minute <= 59.0 && second >= 0.0 &&
           second < 60.0 && std::floor(hour) == hour &&
           std::floor(minute) == minute;
}

std::optional<double> realNumericScalar(const RuntimeValue& value) {
    if (!isRuntimeNumericValue(value) ||
        runtimeShapeElementCount(value) != 1) {
        return std::nullopt;
    }
    const auto element = runtimeNumericElementValue(value, 0);
    if (!element || element->complex) {
        return std::nullopt;
    }
    return element->real;
}

std::optional<std::vector<size_t>> broadcastDimensions(
    const std::vector<RuntimeValue>& values) {
    std::vector<size_t> dimensions{1, 1};
    for (const auto& value : values) {
        if (!isRuntimeNumericValue(value)) {
            return std::nullopt;
        }
        const auto expanded = runtimeImplicitExpansionDimensions(
            dimensions, runtimeDimensions(value));
        if (!expanded) {
            return std::nullopt;
        }
        dimensions = *expanded;
    }
    return dimensions;
}

std::optional<double> numericAtCoordinates(
    const RuntimeValue& value, const std::vector<size_t>& coordinates) {
    if (!isRuntimeNumericValue(value)) {
        return std::nullopt;
    }
    const auto dimensions = runtimeDimensions(value);
    if (runtimeShapeElementCount(value) == 1) {
        const auto element = runtimeNumericElementValue(value, 0);
        return element && !element->complex
                   ? std::optional<double>(element->real)
                   : std::nullopt;
    }
    const auto offset = runtimeImplicitExpansionStorageOffset(
        coordinates, dimensions);
    if (!offset) {
        return std::nullopt;
    }
    const auto element = runtimeNumericStorageElementValue(value, *offset);
    return element && !element->complex
               ? std::optional<double>(element->real)
               : std::nullopt;
}

std::optional<double> temporalAtCoordinates(
    const RuntimeValue& value, const std::vector<size_t>& coordinates) {
    if (!isRuntimeTemporalValue(value)) {
        return std::nullopt;
    }
    if (runtimeShapeElementCount(value) == 1) {
        return runtimeTemporalPayload(value, 0);
    }
    const auto offset = runtimeImplicitExpansionStorageOffset(
        coordinates, runtimeDimensions(value));
    if (!offset) {
        return std::nullopt;
    }
    const auto* element = runtimeObjectElement(value, *offset);
    if (!element || !isRuntimeTemporalValue(*element) ||
        !isRuntimeScalarObject(*element) || element->elements.size() != 1) {
        return std::nullopt;
    }
    return element->elements.front();
}

RuntimeValue makeTemporalScalar(RuntimeTemporalKind kind, double payload) {
    RuntimeValue result = makeRuntimeObjectScalar(
        kind == RuntimeTemporalKind::DateTime
            ? std::string(kRuntimeDateTimeClassName)
            : std::string(kRuntimeDurationClassName));
    result.elements = {payload};
    return result;
}

RuntimeTemporalOperationResult makeTemporal(
    RuntimeTemporalKind kind, std::vector<size_t> dimensions,
    std::vector<double> payloads) {
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count || *count != payloads.size()) {
        return failure("temporal payload dimensions do not match elements");
    }
    if (kind == RuntimeTemporalKind::DateTime &&
        std::any_of(payloads.begin(), payloads.end(),
                    [](double payload) {
                        return !supportedDateTimeSerial(payload);
                    })) {
        return failure("datetime payload is outside the supported range");
    }
    const std::string className =
        kind == RuntimeTemporalKind::DateTime
            ? std::string(kRuntimeDateTimeClassName)
            : std::string(kRuntimeDurationClassName);
    if (payloads.size() == 1) {
        return success(makeTemporalScalar(kind, payloads.front()));
    }
    std::vector<RuntimeValue> elements;
    elements.reserve(payloads.size());
    for (const double payload : payloads) {
        elements.push_back(makeTemporalScalar(kind, payload));
    }
    auto result = runtimeMakeObjectArrayFromLogicalOrder(
        std::move(elements), std::move(dimensions), className, false,
        {}, className);
    return result.succeeded ? success(std::move(result.value))
                           : failure(std::move(result.error));
}

std::optional<std::tuple<std::int64_t, unsigned, unsigned, double>>
dateParts(double serial) {
    if (std::isnan(serial) || !supportedDateTimeSerial(serial)) {
        return std::nullopt;
    }
    const double integral = std::floor(serial);
    std::int64_t days = static_cast<std::int64_t>(integral);
    double fraction = serial - integral;
    if (fraction < 0.0) {
        fraction += 1.0;
        --days;
    }
    const CivilDate civil = civilFromDays(days);
    return std::make_tuple(civil.year, civil.month, civil.day,
                           fraction * kSecondsPerDay);
}

std::optional<double> dateSerial(std::int64_t year, unsigned month,
                                 unsigned day, double hour, double minute,
                                 double second) {
    if (!validDate(year, month, day) ||
        !validClock(hour, minute, second)) {
        return std::nullopt;
    }
    return static_cast<double>(daysFromCivil(year, month, day)) +
           (hour * 3600.0 + minute * 60.0 + second) /
               kSecondsPerDay;
}

bool parseFixedInteger(std::string_view text, size_t offset, size_t count,
                       std::int64_t& value) {
    if (offset + count > text.size() || count == 0) {
        return false;
    }
    std::int64_t parsed = 0;
    for (size_t index = 0; index < count; ++index) {
        const char character = text[offset + index];
        if (character < '0' || character > '9') {
            return false;
        }
        parsed = parsed * 10 + (character - '0');
    }
    value = parsed;
    return true;
}

std::optional<double> parseDateTimeText(std::string_view text) {
    if (text == "NaT" || text == "nat") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    std::int64_t year = 0;
    std::int64_t month = 0;
    std::int64_t day = 0;
    if (!parseFixedInteger(text, 0, 4, year) ||
        !parseFixedInteger(text, 5, 2, month) ||
        !parseFixedInteger(text, 8, 2, day)) {
        return std::nullopt;
    }
    double hour = 0.0;
    double minute = 0.0;
    double second = 0.0;
    if (text.size() == 10) {
        return dateSerial(year, static_cast<unsigned>(month),
                          static_cast<unsigned>(day), hour, minute, second);
    }
    if ((text[10] != 'T' && text[10] != ' ') || text.size() < 19 ||
        text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    std::int64_t parsedHour = 0;
    std::int64_t parsedMinute = 0;
    std::int64_t parsedSecond = 0;
    if (!parseFixedInteger(text, 11, 2, parsedHour) ||
        !parseFixedInteger(text, 14, 2, parsedMinute) ||
        !parseFixedInteger(text, 17, 2, parsedSecond)) {
        return std::nullopt;
    }
    hour = static_cast<double>(parsedHour);
    minute = static_cast<double>(parsedMinute);
    second = static_cast<double>(parsedSecond);
    size_t offset = 19;
    if (offset < text.size() && text[offset] == '.') {
        ++offset;
        if (offset == text.size()) {
            return std::nullopt;
        }
        double scale = 0.1;
        while (offset < text.size()) {
            const char character = text[offset++];
            if (character < '0' || character > '9') {
                return std::nullopt;
            }
            second += static_cast<double>(character - '0') * scale;
            scale *= 0.1;
        }
    }
    if (offset != text.size()) {
        return std::nullopt;
    }
    return dateSerial(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day), hour, minute, second);
}

std::string formatDateTime(double serial) {
    if (std::isnan(serial)) {
        return "NaT";
    }
    if (!supportedDateTimeSerial(serial)) {
        return "NaT";
    }
    const double integral = std::floor(serial);
    std::int64_t serialDays = static_cast<std::int64_t>(integral);
    constexpr std::int64_t microsPerSecond = 1000000;
    constexpr std::int64_t microsPerDay = 86400 * microsPerSecond;
    std::int64_t roundedMicros = static_cast<std::int64_t>(
        std::llround((serial - integral) *
                     static_cast<double>(microsPerDay)));
    if (roundedMicros >= microsPerDay) {
        roundedMicros -= microsPerDay;
        ++serialDays;
    }
    if (serialDays > daysFromCivil(
                         kMaximumSupportedYear, 12, 31)) {
        return "NaT";
    }
    const CivilDate civil = civilFromDays(serialDays);
    const auto roundedSeconds = roundedMicros / microsPerSecond;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << civil.year << '-'
           << std::setw(2) << civil.month << '-' << std::setw(2) << civil.day;
    if (roundedMicros != 0) {
        const auto hour = roundedSeconds / 3600;
        const auto minute = (roundedSeconds % 3600) / 60;
        const auto second = roundedSeconds % 60;
        output << ' ' << std::setw(2) << hour << ':' << std::setw(2)
               << minute << ':' << std::setw(2) << second;
        const auto fractional = roundedMicros % microsPerSecond;
        if (fractional != 0) {
            output << '.' << std::setw(6) << fractional;
        }
    }
    return output.str();
}

std::string formatDuration(double seconds) {
    if (std::isnan(seconds)) {
        return "NaN";
    }
    if (std::isinf(seconds)) {
        return seconds < 0.0 ? "-Inf" : "Inf";
    }
    if (!std::isfinite(seconds) ||
        std::fabs(seconds) >
            static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
                1000000.0) {
        return "duration-out-of-range";
    }
    const bool negative = seconds < 0.0;
    constexpr std::int64_t microsPerSecond = 1000000;
    const auto micros = static_cast<std::int64_t>(
        std::llround(std::fabs(seconds) * microsPerSecond));
    const auto whole = micros / microsPerSecond;
    const auto hour = whole / 3600;
    const auto minute = (whole % 3600) / 60;
    const auto second = whole % 60;
    std::ostringstream output;
    if (negative) {
        output << '-';
    }
    output << std::setfill('0') << std::setw(2) << hour << ':'
           << std::setw(2) << minute << ':' << std::setw(2) << second;
    const auto fractional = micros % microsPerSecond;
    if (fractional != 0) {
        output << '.' << std::setw(6) << fractional;
    }
    return output.str();
}

std::string formatTemporalValue(RuntimeTemporalKind kind, double payload) {
    return kind == RuntimeTemporalKind::DateTime
               ? formatDateTime(payload)
               : formatDuration(payload);
}

RuntimeTemporalOperationResult formatTemporal(const RuntimeValue& value,
                                              bool asString) {
    const auto kind = runtimeTemporalKind(value);
    if (!kind) {
        return failure("value is not a datetime or duration");
    }
    const size_t count = runtimeShapeElementCount(value);
    std::vector<RuntimeStringElement> elements;
    elements.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto payload = runtimeTemporalPayload(value, index);
        if (!payload) {
            return failure("temporal value has an invalid element payload");
        }
        elements.push_back(RuntimeStringElement{
            runtimeUtf8ToUtf16(formatTemporalValue(*kind, *payload)), false});
    }
    if (asString) {
        return success(makeRuntimeStringArray(runtimeDimensions(value),
                                              std::move(elements)));
    }
    if (count == 1) {
        return success(makeRuntimeCharacterVector(
            elements.front().value));
    }
    size_t width = 0;
    for (const auto& element : elements) {
        width = std::max(width, element.value.size());
    }
    const auto outputCount = checkedRuntimeDimensionProduct({count, width});
    if (!outputCount) {
        return failure("temporal character conversion dimensions overflow");
    }
    std::u16string rows(*outputCount, u' ');
    for (size_t index = 0; index < count; ++index) {
        std::copy(elements[index].value.begin(), elements[index].value.end(),
                  rows.begin() + static_cast<std::ptrdiff_t>(index * width));
    }
    return success(makeRuntimeCharacterArray({count, width},
                                             std::move(rows)));
}

std::optional<RuntimeTemporalKind> kindForClass(std::string_view name) {
    if (name == kRuntimeDateTimeClassName) {
        return RuntimeTemporalKind::DateTime;
    }
    if (name == kRuntimeDurationClassName) {
        return RuntimeTemporalKind::Duration;
    }
    return std::nullopt;
}

bool isRelational(std::string_view operation) {
    return operation == "==" || operation == "~=" || operation == "<" ||
           operation == "<=" || operation == ">" || operation == ">=";
}

bool isDateTimeClass(RuntimeTemporalKind kind) {
    return kind == RuntimeTemporalKind::DateTime;
}

} // namespace

bool isRuntimeDateTimeValue(const RuntimeValue& value) {
    return isRuntimeClassObject(value) &&
           value.className == kRuntimeDateTimeClassName;
}

bool isRuntimeDurationValue(const RuntimeValue& value) {
    return isRuntimeClassObject(value) &&
           value.className == kRuntimeDurationClassName;
}

bool isRuntimeTemporalValue(const RuntimeValue& value) {
    return isRuntimeDateTimeValue(value) || isRuntimeDurationValue(value);
}

std::optional<RuntimeTemporalKind>
runtimeTemporalKind(const RuntimeValue& value) {
    if (!isRuntimeTemporalValue(value)) {
        return std::nullopt;
    }
    return kindForClass(value.className);
}

std::optional<double> runtimeTemporalPayload(const RuntimeValue& value,
                                             size_t logicalIndex) {
    if (!isRuntimeTemporalValue(value)) {
        return std::nullopt;
    }
    const RuntimeValue* scalar = nullptr;
    if (runtimeShapeElementCount(value) == 1 &&
        value.objectElements.empty()) {
        scalar = &value;
    } else {
        scalar = runtimeObjectLogicalElement(value, logicalIndex);
    }
    if (!scalar || !isRuntimeTemporalValue(*scalar) ||
        !isRuntimeScalarObject(*scalar) || scalar->elements.size() != 1) {
        return std::nullopt;
    }
    return scalar->elements.front();
}

RuntimeTemporalOperationResult runtimeConstructDateTime(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() == 1 && isRuntimeTextValue(arguments.front())) {
        const auto text = runtimeTextScalarUtf8(arguments.front());
        if (!text) {
            return failure("datetime text input must be a scalar");
        }
        const auto parsed = parseDateTimeText(*text);
        return parsed ? success(makeTemporalScalar(
                              RuntimeTemporalKind::DateTime, *parsed))
                      : failure("datetime text must use YYYY-MM-DD or "
                                "YYYY-MM-DD HH:MM:SS format");
    }
    if (arguments.size() < 3 || arguments.size() > 6) {
        return failure("datetime expects text or 3 to 6 numeric inputs");
    }
    const auto dimensions = broadcastDimensions(arguments);
    if (!dimensions) {
        return failure("datetime numeric inputs have incompatible dimensions "
                       "or are not real numeric values");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure("datetime result dimensions overflow");
    }
    std::vector<double> values;
    values.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(index,
                                                                 *dimensions);
        if (!coordinates) {
            return failure("datetime result coordinates overflow");
        }
        std::vector<double> parts;
        parts.reserve(arguments.size());
        for (const auto& argument : arguments) {
            const auto value = numericAtCoordinates(argument, *coordinates);
            if (!value) {
                return failure("datetime inputs must be real numeric values");
            }
            parts.push_back(*value);
        }
        const auto integral = [](double value) {
            return std::isfinite(value) && std::floor(value) == value;
        };
        if (!integral(parts[0]) || !integral(parts[1]) ||
            !integral(parts[2]) || parts[0] < 1.0 ||
            parts[0] > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            parts[1] < 1.0 || parts[1] > 12.0 || parts[2] < 1.0 ||
            parts[2] > 31.0) {
            return failure("datetime year, month, and day are invalid");
        }
        const double hour = parts.size() > 3 ? parts[3] : 0.0;
        const double minute = parts.size() > 4 ? parts[4] : 0.0;
        const double second = parts.size() > 5 ? parts[5] : 0.0;
        const auto serial = dateSerial(
            static_cast<std::int64_t>(parts[0]),
            static_cast<unsigned>(parts[1]),
            static_cast<unsigned>(parts[2]), hour, minute, second);
        if (!serial) {
            return failure("datetime contains an invalid civil date or time");
        }
        values.push_back(*serial);
    }
    return makeTemporal(RuntimeTemporalKind::DateTime, *dimensions,
                        std::move(values));
}

RuntimeTemporalOperationResult runtimeConstructDuration(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.size() != 3) {
        return failure("duration currently expects hours, minutes, and seconds");
    }
    const auto dimensions = broadcastDimensions(arguments);
    if (!dimensions) {
        return failure("duration inputs have incompatible dimensions or are "
                       "not real numeric values");
    }
    const auto count = checkedRuntimeDimensionProduct(*dimensions);
    if (!count) {
        return failure("duration result dimensions overflow");
    }
    std::vector<double> values;
    values.reserve(*count);
    for (size_t index = 0; index < *count; ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(index,
                                                                 *dimensions);
        if (!coordinates) {
            return failure("duration result coordinates overflow");
        }
        const auto hour = numericAtCoordinates(arguments[0], *coordinates);
        const auto minute = numericAtCoordinates(arguments[1], *coordinates);
        const auto second = numericAtCoordinates(arguments[2], *coordinates);
        if (!hour || !minute || !second || !std::isfinite(*hour) ||
            !std::isfinite(*minute) || !std::isfinite(*second)) {
            return failure("duration inputs must be finite real values");
        }
        values.push_back(*hour * 3600.0 + *minute * 60.0 + *second);
    }
    return makeTemporal(RuntimeTemporalKind::Duration, *dimensions,
                        std::move(values));
}

RuntimeTemporalOperationResult runtimeConstructNaT(
    const std::vector<RuntimeValue>& arguments) {
    if (arguments.empty()) {
        return success(makeTemporalScalar(
            RuntimeTemporalKind::DateTime,
            std::numeric_limits<double>::quiet_NaN()));
    }
    std::vector<size_t> dimensions;
    dimensions.reserve(arguments.size());
    for (const auto& argument : arguments) {
        const auto dimension = realNumericScalar(argument);
        if (!dimension || !std::isfinite(*dimension) ||
            *dimension < 0.0 || std::floor(*dimension) != *dimension) {
            return failure("NaT dimensions must be nonnegative integer scalars");
        }
        const auto parsed = checkedRuntimeNonnegativeInteger(*dimension);
        if (!parsed) {
            return failure("NaT dimensions exceed the runtime limit");
        }
        dimensions.push_back(*parsed);
    }
    dimensions = normalizeRuntimeDimensions(std::move(dimensions));
    const auto count = checkedRuntimeDimensionProduct(dimensions);
    if (!count) {
        return failure("NaT dimensions overflow");
    }
    return makeTemporal(
        RuntimeTemporalKind::DateTime, std::move(dimensions),
        std::vector<double>(*count,
                            std::numeric_limits<double>::quiet_NaN()));
}

RuntimeTemporalOperationResult runtimeTemporalComponent(
    std::string_view name, const RuntimeValue& value) {
    if (!isRuntimeDateTimeValue(value)) {
        return failure(std::string(name) + " expects a datetime value");
    }
    const auto dimensions = runtimeDimensions(value);
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto payload = runtimeTemporalPayload(value, index);
        if (!payload) {
            return failure("datetime contains an invalid payload");
        }
        if (std::isnan(*payload)) {
            result.push_back(std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        const auto parts = dateParts(*payload);
        if (!parts) {
            return failure("datetime payload is outside the supported range");
        }
        const auto seconds = std::get<3>(*parts);
        if (name == "year") {
            result.push_back(static_cast<double>(std::get<0>(*parts)));
        } else if (name == "month") {
            result.push_back(static_cast<double>(std::get<1>(*parts)));
        } else if (name == "day") {
            result.push_back(static_cast<double>(std::get<2>(*parts)));
        } else if (name == "hour") {
            result.push_back(std::floor(seconds / 3600.0));
        } else if (name == "minute") {
            result.push_back(std::floor(std::fmod(seconds, 3600.0) / 60.0));
        } else if (name == "second") {
            result.push_back(std::fmod(seconds, 60.0));
        } else {
            return failure("unsupported datetime component: " +
                           std::string(name));
        }
    }
    auto numeric = runtimeNumericValueFromLogicalOrder(
        dimensions, std::move(result), RuntimeNumericClass::Double);
    return numeric ? success(std::move(*numeric))
                   : failure("datetime component result has an invalid shape");
}

RuntimeTemporalOperationResult runtimeTemporalUnit(
    std::string_view name, const RuntimeValue& value) {
    const double multiplier = name == "days"   ? 86400.0
                              : name == "hours" ? 3600.0
                              : name == "minutes" ? 60.0
                              : name == "seconds" ? 1.0
                                                    : -1.0;
    if (multiplier < 0.0) {
        return failure("unsupported temporal unit: " + std::string(name));
    }
    if (isRuntimeDurationValue(value)) {
        const size_t count = runtimeShapeElementCount(value);
        std::vector<double> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto payload = runtimeTemporalPayload(value, index);
            if (!payload) {
                return failure("duration contains an invalid payload");
            }
            values.push_back(*payload / multiplier);
        }
        auto numeric = runtimeNumericValueFromLogicalOrder(
            runtimeDimensions(value), std::move(values),
            RuntimeNumericClass::Double);
        return numeric ? success(std::move(*numeric))
                       : failure("duration unit result has an invalid shape");
    }
    if (!isRuntimeNumericValue(value)) {
        return failure(std::string(name) +
                       " expects numeric values or a duration");
    }
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto element = runtimeNumericElementValue(value, index);
        if (!element || element->complex) {
            return failure(std::string(name) +
                           " requires real numeric values");
        }
        values.push_back(element->real * multiplier);
    }
    return makeTemporal(RuntimeTemporalKind::Duration,
                        runtimeDimensions(value), std::move(values));
}

RuntimeTemporalOperationResult runtimeTemporalPredicate(
    std::string_view name, const RuntimeValue& value) {
    if (name == "isdatetime") {
        return success(makeRuntimeLogicalValue(isRuntimeDateTimeValue(value)));
    }
    if (name == "isduration") {
        return success(makeRuntimeLogicalValue(isRuntimeDurationValue(value)));
    }
    if (name != "isnat") {
        return failure("unsupported temporal predicate: " + std::string(name));
    }
    if (!isRuntimeTemporalValue(value)) {
        return success(makeRuntimeLogicalValue(false));
    }
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto payload = runtimeTemporalPayload(value, index);
        if (!payload) {
            return failure("temporal predicate could not read payload");
        }
        values.push_back(std::isnan(*payload) ? 1.0 : 0.0);
    }
    auto result = runtimeNumericValueFromLogicalOrder(
        runtimeDimensions(value), std::move(values),
        RuntimeNumericClass::Logical);
    return result ? success(std::move(*result))
                  : failure("isnat result has an invalid shape");
}

RuntimeTemporalOperationResult runtimeTemporalFormat(
    const RuntimeValue& value, bool asString) {
    return formatTemporal(value, asString);
}

RuntimeTemporalOperationResult runtimeApplyTemporalUnary(
    std::string_view operation, const RuntimeValue& value) {
    if (!isRuntimeTemporalValue(value) ||
        (operation != "+" && operation != "-")) {
        return failure("unsupported unary operation for temporal value");
    }
    const auto kind = *runtimeTemporalKind(value);
    if (isDateTimeClass(kind) && operation == "-") {
        return failure("datetime does not support unary minus");
    }
    if (operation == "+") {
        return success(value);
    }
    const size_t count = runtimeShapeElementCount(value);
    std::vector<double> values;
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto payload = runtimeTemporalPayload(value, index);
        if (!payload) {
            return failure("temporal unary operation could not read payload");
        }
        values.push_back(-*payload);
    }
    return makeTemporal(kind, runtimeDimensions(value), std::move(values));
}

RuntimeTemporalOperationResult runtimeApplyTemporalBinary(
    std::string_view operation, const RuntimeValue& left,
    const RuntimeValue& right) {
    const bool leftTemporal = isRuntimeTemporalValue(left);
    const bool rightTemporal = isRuntimeTemporalValue(right);
    if (!leftTemporal && !rightTemporal) {
        return failure("temporal binary operation requires a temporal value");
    }
    const auto leftKind = leftTemporal ? runtimeTemporalKind(left)
                                      : std::nullopt;
    const auto rightKind = rightTemporal ? runtimeTemporalKind(right)
                                         : std::nullopt;
    if (leftKind && rightKind && *leftKind != *rightKind &&
        isRelational(operation)) {
        return failure("datetime and duration cannot be compared");
    }
    if (leftKind && rightKind && isRelational(operation)) {
        const auto dimensions = runtimeImplicitExpansionDimensions(
            runtimeDimensions(left), runtimeDimensions(right));
        if (!dimensions) {
            return failure("temporal operands have incompatible dimensions");
        }
        const size_t count = checkedRuntimeDimensionProduct(*dimensions).value_or(0);
        std::vector<double> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto coordinates = runtimeColumnMajorCoordinates(index,
                                                                     *dimensions);
            const auto lhs = coordinates ? temporalAtCoordinates(left, *coordinates)
                                         : std::nullopt;
            const auto rhs = coordinates ? temporalAtCoordinates(right, *coordinates)
                                         : std::nullopt;
            if (!lhs || !rhs) {
                return failure("temporal comparison could not map an element");
            }
            bool result = false;
            if (std::isnan(*lhs) || std::isnan(*rhs)) {
                result = operation == "~=";
            } else if (operation == "==") {
                result = *lhs == *rhs;
            } else if (operation == "~=") {
                result = *lhs != *rhs;
            } else if (operation == "<") {
                result = *lhs < *rhs;
            } else if (operation == "<=") {
                result = *lhs <= *rhs;
            } else if (operation == ">") {
                result = *lhs > *rhs;
            } else {
                result = *lhs >= *rhs;
            }
            values.push_back(result ? 1.0 : 0.0);
        }
        auto result = runtimeNumericValueFromLogicalOrder(
            *dimensions, std::move(values), RuntimeNumericClass::Logical);
        return result ? success(std::move(*result))
                      : failure("temporal comparison result has an invalid shape");
    }

    const bool numericLeft = isRuntimeNumericValue(left);
    const bool numericRight = isRuntimeNumericValue(right);
    if (leftTemporal && rightTemporal) {
        const bool leftDate = *leftKind == RuntimeTemporalKind::DateTime;
        const bool rightDate = *rightKind == RuntimeTemporalKind::DateTime;
        const bool supported =
            (leftDate && rightDate && operation == "-") ||
            (leftDate && !rightDate &&
             (operation == "+" || operation == "-")) ||
            (!leftDate && rightDate && operation == "+") ||
            (!leftDate && !rightDate &&
             (operation == "+" || operation == "-"));
        if (!supported) {
            return failure("unsupported datetime/duration arithmetic operation");
        }
    } else if ((leftTemporal && numericRight) ||
               (rightTemporal && numericLeft)) {
        const auto temporalKind = leftKind ? *leftKind : *rightKind;
        const bool multiply = operation == "*" || operation == ".*";
        const bool divideDuration =
            leftTemporal && (operation == "/" || operation == "./");
        if (temporalKind != RuntimeTemporalKind::Duration ||
            (!multiply && !divideDuration)) {
            return failure("only duration values support numeric scaling");
        }
    } else if (!leftTemporal || !rightTemporal) {
        return failure("temporal arithmetic requires compatible operands");
    }

    const auto dimensions = runtimeImplicitExpansionDimensions(
        runtimeDimensions(left), runtimeDimensions(right));
    if (!dimensions) {
        return failure("temporal operands have incompatible dimensions");
    }
    const size_t count = checkedRuntimeDimensionProduct(*dimensions).value_or(0);
    std::vector<double> values;
    values.reserve(count);
    const auto leftIsDate = leftKind == RuntimeTemporalKind::DateTime;
    const auto rightIsDate = rightKind == RuntimeTemporalKind::DateTime;
    RuntimeTemporalKind resultKind = RuntimeTemporalKind::Duration;
    if (leftIsDate || rightIsDate) {
        resultKind = RuntimeTemporalKind::DateTime;
        if (leftIsDate && rightIsDate && operation == "-") {
            resultKind = RuntimeTemporalKind::Duration;
        }
    }
    for (size_t index = 0; index < count; ++index) {
        const auto coordinates = runtimeColumnMajorCoordinates(index,
                                                                 *dimensions);
        if (!coordinates) {
            return failure("temporal arithmetic coordinates overflow");
        }
        const auto lhs = leftTemporal
                             ? temporalAtCoordinates(left, *coordinates)
                             : numericAtCoordinates(left, *coordinates);
        const auto rhs = rightTemporal
                             ? temporalAtCoordinates(right, *coordinates)
                             : numericAtCoordinates(right, *coordinates);
        if (!lhs || !rhs) {
            return failure("temporal arithmetic could not map an element");
        }
        double result = std::numeric_limits<double>::quiet_NaN();
        if (leftIsDate && rightIsDate) {
            result = (*lhs - *rhs) * kSecondsPerDay;
        } else if (leftIsDate && rightTemporal) {
            result = *lhs + (operation == "-" ? -*rhs : *rhs) /
                                      kSecondsPerDay;
        } else if (rightIsDate && leftTemporal) {
            result = *rhs + *lhs / kSecondsPerDay;
        } else if (leftTemporal && rightTemporal) {
            result = operation == "-" ? *lhs - *rhs : *lhs + *rhs;
        } else if (leftTemporal) {
            result = operation == "/" || operation == "./"
                         ? *lhs / *rhs
                         : *lhs * *rhs;
        } else {
            result = *lhs * *rhs;
        }
        values.push_back(result);
    }
    return makeTemporal(resultKind, *dimensions, std::move(values));
}

RuntimeTemporalOperationResult runtimeTemporalMemberValue(
    const RuntimeValue& value, std::string_view member) {
    if (!isRuntimeTemporalValue(value)) {
        return failure("member access requires a temporal value");
    }
    if (member == "Year" || member == "Month" || member == "Day" ||
        member == "Hour" || member == "Minute" || member == "Second") {
        std::string lower(member);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return runtimeTemporalComponent(lower, value);
    }
    if (member == "Value" && isRuntimeDurationValue(value)) {
        const size_t count = runtimeShapeElementCount(value);
        std::vector<double> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const auto payload = runtimeTemporalPayload(value, index);
            if (!payload) {
                return failure("duration member access could not read payload");
            }
            values.push_back(*payload);
        }
        auto result = runtimeNumericValueFromLogicalOrder(
            runtimeDimensions(value), std::move(values),
            RuntimeNumericClass::Double);
        return result ? success(std::move(*result))
                      : failure("duration Value has an invalid shape");
    }
    if (member == "Format") {
        return success(makeRuntimeStringScalarUtf8(
            isRuntimeDateTimeValue(value) ? "yyyy-MM-dd" : "hh:mm:ss"));
    }
    return failure("temporal member is not available: " + std::string(member));
}

bool runtimeTemporalValuesEqual(const RuntimeValue& left,
                                const RuntimeValue& right,
                                bool equalNaNs) {
    if (!isRuntimeTemporalValue(left) || !isRuntimeTemporalValue(right) ||
        left.className != right.className ||
        runtimeDimensions(left) != runtimeDimensions(right)) {
        return false;
    }
    const size_t count = runtimeShapeElementCount(left);
    for (size_t index = 0; index < count; ++index) {
        const auto lhs = runtimeTemporalPayload(left, index);
        const auto rhs = runtimeTemporalPayload(right, index);
        if (!lhs || !rhs) {
            return false;
        }
        if (std::isnan(*lhs) || std::isnan(*rhs)) {
            if (!equalNaNs || !std::isnan(*lhs) || !std::isnan(*rhs)) {
                return false;
            }
        } else if (*lhs != *rhs) {
            return false;
        }
    }
    return true;
}

} // namespace mparser
