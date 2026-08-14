#include "test_assertions_enabled.h"

#include "mparser/runtime_numeric.h"
#include "mparser/runtime_output.h"
#include "mparser/runtime_text.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

mparser::RuntimeValue format(std::string text) {
    return mparser::makeRuntimeCharacterVectorUtf8(text);
}

mparser::RuntimeValue number(double value) {
    return mparser::makeRuntimeNumberValue(value);
}

mparser::RuntimeValue requireNumeric(
    std::optional<mparser::RuntimeValue> value, const char* context) {
    if (!value) {
        throw std::runtime_error(
            std::string("numeric fixture rejected: ") + context);
    }
    return std::move(*value);
}

mparser::RuntimeValue integer(mparser::RuntimeNumericClass numericClass,
                              std::uint64_t bits) {
    mparser::RuntimeNumericElementValue element;
    element.numericClass = numericClass;
    element.integerRealBits = bits;
    return requireNumeric(mparser::runtimeNumericValueFromElements(
        {1, 1}, {element}, numericClass), "integer");
}

void requireSuccess(const mparser::RuntimeFormatResult& result,
                    const std::string& expected) {
    assert(result.succeeded);
    assert(result.error.empty());
    assert(result.text == expected);
}

void requireFailure(const mparser::RuntimeFormatResult& result) {
    assert(!result.succeeded);
    assert(result.text.empty());
    assert(!result.error.empty());
}

void runFormattingSmoke() {
    const std::string threeCodePoints =
        "\xe4\xb8\xad\xe5\x9b\xbd\xe4\xba\xba";
    const std::string twoCodePoints =
        "\xe4\xb8\xad\xe6\x96\x87";
    const std::string firstCodePoint = "\xe4\xb8\xad";
    requireSuccess(
        mparser::runtimeFormatDisplay(
            mparser::makeRuntimeStringScalarUtf8("hello")),
        "hello\n\n");
    mparser::RuntimeDisplayFormat compact;
    compact.spacing = mparser::RuntimeLineSpacing::Compact;
    requireSuccess(
        mparser::runtimeFormatDisplay(
            mparser::makeRuntimeStringScalarUtf8("hello"), compact),
        "hello\n");

    const auto console = [](const mparser::RuntimeValue& value,
                            mparser::RuntimeNumericDisplayFormat numeric) {
        mparser::RuntimeDisplayFormat display;
        display.numeric = numeric;
        display.spacing = mparser::RuntimeLineSpacing::Compact;
        return mparser::runtimeFormatConsoleValue(value, display);
    };
    const auto pi = number(3.14159265358979323846);
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Short) ==
                "3.1416",
            "short numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Long) ==
                "3.141592653589793",
            "long numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::ShortE) ==
                "3.1416e+00",
            "short-E numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::LongG) ==
                "3.14159265358979",
            "long-G numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::ShortEng) ==
                "3.1416e+000",
            "short engineering display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::LongEng) ==
                "3.14159265358979e+000",
            "long engineering display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Bank) ==
                "3.14",
            "bank numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Hex) ==
                "400921fb54442d18",
            "hex numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Rational) ==
                "355/113",
            "rational numeric display mismatch");
    require(console(pi, mparser::RuntimeNumericDisplayFormat::Plus) == "+",
            "plus numeric display mismatch");

    mparser::RuntimeNumericElementValue complexElement;
    complexElement.real = 1.0;
    complexElement.imaginary = -2.0;
    complexElement.complex = true;
    const auto complexValue = requireNumeric(
        mparser::runtimeNumericValueFromElements(
            {1, 1}, {complexElement},
            mparser::RuntimeNumericClass::Double),
        "complex display");
    require(console(complexValue,
                    mparser::RuntimeNumericDisplayFormat::Short) ==
                "1.0000 - 2.0000i",
            "short complex display mismatch");
    require(console(complexValue,
                    mparser::RuntimeNumericDisplayFormat::Bank) == "1.00",
            "bank complex display mismatch");
    require(console(complexValue,
                    mparser::RuntimeNumericDisplayFormat::Hex) ==
                "3ff0000000000000   c000000000000000i",
            "hex complex display mismatch");

    requireSuccess(
        mparser::runtimeFormatPrintf({
            format("%+08d|% 6.2f|%-5s|%.2s|%c|%c|%%\\n"),
            number(42), number(3.5),
            mparser::makeRuntimeStringScalarUtf8("x"),
            mparser::makeRuntimeStringScalarUtf8(threeCodePoints),
            number(0x4e2d),
            mparser::makeRuntimeStringScalarUtf8(twoCodePoints)}),
        "+0000042|  3.50|x    |" +
            threeCodePoints.substr(0, 6) + "|" + firstCodePoint +
            "|" + firstCodePoint + "|%\n");

    requireSuccess(
        mparser::runtimeFormatPrintf({
            format("%08d"), number(-42)}),
        "-0000042");

    const auto uint64Maximum = integer(
        mparser::RuntimeNumericClass::UInt64,
        std::numeric_limits<std::uint64_t>::max());
    const auto int64Minimum = integer(
        mparser::RuntimeNumericClass::Int64,
        std::bit_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::min()));
    require(console(uint64Maximum,
                    mparser::RuntimeNumericDisplayFormat::Long) ==
                "18446744073709551615",
            "integer display lost exactness");
    requireSuccess(
        mparser::runtimeFormatPrintf({
            format("%u|%d"), uint64Maximum, int64Minimum}),
        "18446744073709551615|-9223372036854775808");

    std::vector<mparser::RuntimeNumericElementValue> elements;
    for (double value : {1.0, 2.0, 3.0}) {
        mparser::RuntimeNumericElementValue element;
        element.real = value;
        elements.push_back(element);
    }
    const auto array = requireNumeric(mparser::runtimeNumericValueFromElements(
        {1, 3}, std::move(elements),
        mparser::RuntimeNumericClass::Double), "array");
    requireSuccess(
        mparser::runtimeFormatPrintf({format("%d,"), array}),
        "1,2,3,");

    std::vector<mparser::RuntimeNumericElementValue> matrixElements;
    for (double value : {1.0, 3.0, 2.0, 4.0}) {
        mparser::RuntimeNumericElementValue element;
        element.real = value;
        matrixElements.push_back(element);
    }
    const auto matrix = requireNumeric(
        mparser::runtimeNumericValueFromElements(
            {2, 2}, std::move(matrixElements),
            mparser::RuntimeNumericClass::Double),
        "matrix display");
    require(console(matrix, mparser::RuntimeNumericDisplayFormat::Short) ==
                "[1 2; 3 4]",
            "matrix display lost column-major order");
    requireSuccess(
        mparser::runtimeFormatPrintf({format("line\\n%%")}),
        "line\n%");
}

void runFailureSmoke() {
    requireFailure(mparser::runtimeFormatPrintf({}));
    requireFailure(mparser::runtimeFormatPrintf({format("%d")}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("%d %d"), number(1)}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("literal"), number(1)}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("%s"), number(1)}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("%x"), number(1)}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("%1048577d"), number(1)}));
    requireFailure(mparser::runtimeFormatPrintf({
        format("%.4097f"), number(1)}));

    mparser::RuntimeNumericElementValue complex;
    complex.real = 1;
    complex.imaginary = 2;
    complex.complex = true;
    const auto complexValue = requireNumeric(
        mparser::runtimeNumericValueFromElements(
            {1, 1}, {complex}, mparser::RuntimeNumericClass::Double),
        "complex");
    requireFailure(mparser::runtimeFormatPrintf({
        format("%f"), complexValue}));

    std::vector<mparser::RuntimeNumericElementValue> many(17);
    for (auto& element : many) {
        element.real = 1;
    }
    const size_t manyCount = many.size();
    const auto oversized = requireNumeric(
        mparser::runtimeNumericValueFromElements(
        {1, manyCount}, std::move(many),
        mparser::RuntimeNumericClass::Double), "oversized");
    requireFailure(mparser::runtimeFormatPrintf({
        format("%1048576d"), oversized}));
}

} // namespace

int main() {
    try {
        runFormattingSmoke();
        runFailureSmoke();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime output smoke failure: "
                  << error.what() << "\n";
        return 1;
    }
}
