#include "mparser/runtime/core/runtime_argument_validation.h"
#include "mparser/runtime/core/runtime_numeric.h"
#include "mparser/runtime/core/runtime_shape.h"
#include "mparser/runtime/core/runtime_text.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

mparser::RuntimeValue number(double value) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Number;
    result.number = value;
    mparser::setRuntimeDimensions(result, {1, 1});
    return result;
}

mparser::RuntimeValue row(std::vector<double> values) {
    mparser::RuntimeValue result;
    result.kind = mparser::RuntimeValueKind::Vector;
    result.elements = std::move(values);
    mparser::setRuntimeDimensions(result, {1, result.elements.size()});
    return result;
}

mparser::RuntimeValue numericScalar(
    mparser::RuntimeNumericElementValue element) {
    auto value = mparser::runtimeNumericValueFromElements(
        {1, 1}, {element}, element.numericClass);
    assert(value.has_value());
    return std::move(*value);
}

mparser::RuntimeValue complexNumber(double real, double imaginary) {
    mparser::RuntimeNumericElementValue element;
    element.real = real;
    element.imaginary = imaginary;
    element.complex = true;
    return numericScalar(element);
}

mparser::RuntimeValue integerNumber(
    mparser::RuntimeNumericClass numericClass, std::uint64_t bits) {
    mparser::RuntimeNumericElementValue element;
    element.numericClass = numericClass;
    element.integerRealBits = bits;
    element.real = static_cast<double>(bits);
    return numericScalar(element);
}

mparser::PropertySpec spec(
    std::string className,
    std::vector<std::string> validators = {}) {
    mparser::PropertySpec result;
    result.className = std::move(className);
    for (auto& validator : validators) {
        result.validators.push_back(
            mparser::PropertyValidatorSpec{std::move(validator), {}, {}, {}});
    }
    return result;
}

void primitiveConversionAndShapeSmoke() {
    auto logical = mparser::validateRuntimeArgument(
        number(2), spec("logical"));
    assert(logical.succeeded);
    assert(logical.value.numericClass ==
           mparser::RuntimeNumericClass::Logical);
    assert(logical.value.number == 1.0);

    auto shapedSpec = spec("double");
    shapedSpec.dimensions = {
        mparser::PropertyDimensionSpec{"1", {}},
        mparser::PropertyDimensionSpec{":", {}}};
    auto shaped = mparser::validateRuntimeArgument(
        row({1, 2, 3}), shapedSpec);
    assert(shaped.succeeded);
    assert(mparser::runtimeDimensions(shaped.value) ==
           std::vector<size_t>({1, 3}));

    shapedSpec.dimensions[0].text = "2";
    const auto rejected = mparser::validateRuntimeArgument(
        row({1, 2, 3}), shapedSpec);
    assert(!rejected.succeeded);
}

void numericValidatorSmoke() {
    const std::vector<std::string> validators{
        "mustBePositive", "mustBeNonnegative", "mustBeFinite",
        "mustBeNonNan", "mustBeNonzero", "mustBeInteger",
        "mustBeReal", "mustBeFloat", "mustBeNumeric",
        "mustBeNumericOrLogical", "mustBeNonsparse",
        "mustBeNonempty", "mustBeScalarOrEmpty", "mustBeNonmissing"};
    const auto accepted = mparser::validateRuntimeArgument(
        number(2), spec("double", validators));
    assert(accepted.succeeded);

    assert(mparser::validateRuntimeArgument(
               number(0), spec("double", {"mustBeNonpositive"}))
               .succeeded);
    assert(mparser::validateRuntimeArgument(
               number(-1), spec("double", {"mustBeNegative"}))
               .succeeded);
    assert(!mparser::validateRuntimeArgument(
                number(0), spec("double", {"mustBePositive"}))
                .succeeded);
    assert(!mparser::validateRuntimeArgument(
                number(std::nan("")),
                spec("double", {"mustBeNonNan"}))
                .succeeded);
    assert(!mparser::validateRuntimeArgument(
                number(1), spec("double", {"mustBeSparse"}))
                .succeeded);
}

void complexAndNumericClassValidatorSmoke() {
    assert(mparser::validateRuntimeArgument(
               complexNumber(1, 2),
               spec("", {"mustBeFinite", "mustBeNonNan",
                           "mustBeNonzero", "mustBeFloat",
                           "mustBeNumeric"}))
               .succeeded);
    assert(!mparser::validateRuntimeArgument(
                complexNumber(1, std::numeric_limits<double>::infinity()),
                spec("", {"mustBeFinite"}))
                .succeeded);
    assert(!mparser::validateRuntimeArgument(
                complexNumber(1, std::nan("")),
                spec("", {"mustBeNonNan"}))
                .succeeded);
    assert(mparser::validateRuntimeArgument(
               complexNumber(0, 1), spec("", {"mustBeNonzero"}))
               .succeeded);
    assert(!mparser::validateRuntimeArgument(
                complexNumber(0, 0), spec("", {"mustBeNonzero"}))
                .succeeded);
    for (const auto& validator : {
             "mustBeReal", "mustBePositive", "mustBeInteger",
             "mustBeGreaterThan"}) {
        auto complexSpec = spec("", {validator});
        if (std::string_view(validator) == "mustBeGreaterThan") {
            complexSpec.validators.front().arguments = {"0"};
        }
        assert(!mparser::validateRuntimeArgument(
                    complexNumber(1, 0), complexSpec)
                    .succeeded);
    }

    const auto int8Value = integerNumber(
        mparser::RuntimeNumericClass::Int8, 1);
    assert(!mparser::validateRuntimeArgument(
                int8Value, spec("", {"mustBeFloat"}))
                .succeeded);
    assert(mparser::validateRuntimeArgument(
               int8Value, spec("", {"mustBeInteger",
                                      "mustBeNumeric"}))
               .succeeded);

    auto logical = number(1);
    logical.numericClass = mparser::RuntimeNumericClass::Logical;
    assert(!mparser::validateRuntimeArgument(
                logical, spec("", {"mustBeNumeric"}))
                .succeeded);
    assert(mparser::validateRuntimeArgument(
               logical, spec("", {"mustBeNumericOrLogical"}))
               .succeeded);
}

void numericShapePreservationSmoke() {
    auto complexSpec = spec("single");
    complexSpec.dimensions = {
        mparser::PropertyDimensionSpec{"1", {}},
        mparser::PropertyDimensionSpec{"2", {}}};
    const auto complex = mparser::validateRuntimeArgument(
        complexNumber(3, 4), complexSpec);
    assert(complex.succeeded);
    assert(complex.value.numericClass ==
           mparser::RuntimeNumericClass::Single);
    assert(complex.value.numericComplex);
    assert(mparser::runtimeDimensions(complex.value) ==
           std::vector<size_t>({1, 2}));
    for (size_t index = 0; index < 2; ++index) {
        const auto element = mparser::runtimeNumericElementValue(
            complex.value, index);
        assert(element && element->real == 3.0 &&
               element->imaginary == 4.0);
    }

    constexpr std::uint64_t exact = 9007199254740993ULL;
    auto integerSpec = spec("uint64");
    integerSpec.dimensions = complexSpec.dimensions;
    const auto integer = mparser::validateRuntimeArgument(
        integerNumber(mparser::RuntimeNumericClass::UInt64, exact),
        integerSpec);
    assert(integer.succeeded);
    for (size_t index = 0; index < 2; ++index) {
        const auto element = mparser::runtimeNumericElementValue(
            integer.value, index);
        assert(element && element->integerRealBits == exact);
    }
}

void shapeTextAndComparisonSmoke() {
    assert(mparser::validateRuntimeArgument(
               row({1, 2}), spec("double", {"mustBeVector", "mustBeRow"}))
               .succeeded);

    auto column = row({1, 2});
    mparser::setRuntimeDimensions(column, {2, 1});
    assert(mparser::validateRuntimeArgument(
               column, spec("double", {"mustBeColumn", "mustBeMatrix"}))
               .succeeded);

    mparser::RuntimeValue text =
        mparser::makeRuntimeStringScalarUtf8("hello");
    assert(mparser::validateRuntimeArgument(
               text, spec("string", {"mustBeText", "mustBeTextScalar",
                                      "mustBeNonzeroLengthText"}))
               .succeeded);

    const std::vector<std::string> comparisons{
        "mustBeGreaterThan", "mustBeGreaterThanOrEqual"};
    for (const auto& name : comparisons) {
        auto comparison = spec("double");
        comparison.validators.push_back(
            mparser::PropertyValidatorSpec{name, {}, {}, {"1"}});
        assert(mparser::validateRuntimeArgument(number(2), comparison)
                   .succeeded);
    }
    for (const auto& name : {"mustBeLessThan", "mustBeLessThanOrEqual"}) {
        auto comparison = spec("double");
        comparison.validators.push_back(
            mparser::PropertyValidatorSpec{name, {}, {}, {"3"}});
        assert(mparser::validateRuntimeArgument(number(2), comparison)
                   .succeeded);
    }
    auto malformed = spec("double");
    malformed.validators.push_back(mparser::PropertyValidatorSpec{
        "mustBeGreaterThan", {}, {}, {"x", "1", "2"}});
    assert(!mparser::validateRuntimeArgument(number(3), malformed).succeeded);
}

void missingArgumentSmoke() {
    auto missingSpec = spec(
        "missing", {"mustBeMatrix", "mustBeNonempty"});
    missingSpec.dimensions = {
        mparser::PropertyDimensionSpec{"3", {}},
        mparser::PropertyDimensionSpec{"2", {}}};
    const auto reshaped = mparser::validateRuntimeArgument(
        mparser::makeRuntimeMissingArrayValue({2, 3}), missingSpec);
    assert(reshaped.succeeded);
    assert(reshaped.value.kind ==
           mparser::RuntimeValueKind::MissingArray);
    assert(mparser::runtimeDimensions(reshaped.value) ==
           std::vector<size_t>({3, 2}));

    assert(!mparser::validateRuntimeArgument(
                mparser::makeRuntimeMissingArrayValue({2, 3}),
                spec("missing", {"mustBeScalarOrEmpty"}))
                .succeeded);
    assert(!mparser::validateRuntimeArgument(
                mparser::makeRuntimeMissingArrayValue(),
                spec("missing", {"mustBeNonmissing"}))
                .succeeded);

    auto missingString = mparser::makeRuntimeStringArray(
        {1, 2}, {{u"value", false}, {u"", true}});
    assert(!mparser::validateRuntimeArgument(
                missingString,
                spec("string", {"mustBeNonmissing"}))
                .succeeded);
    assert(mparser::validateRuntimeArgument(
               mparser::makeRuntimeStringScalarUtf8("value"),
               spec("string", {"mustBeNonmissing"}))
               .succeeded);
}

void objectConstraintSmoke() {
    mparser::RuntimeValue object;
    object.kind = mparser::RuntimeValueKind::Object;
    object.className = "Derived";
    auto objectSpec = spec("Base");
    mparser::RuntimeArgumentValidationOptions options;
    options.classAvailable = [](const std::string& name) {
        return name == "Base" || name == "Derived";
    };
    options.objectIsA = [](const std::string& actual,
                           const std::string& expected) {
        return actual == expected ||
               (actual == "Derived" && expected == "Base");
    };
    assert(mparser::validateRuntimeArgument(object, objectSpec, options)
               .succeeded);
    assert(!mparser::validateRuntimeArgument(object, objectSpec).succeeded);
}

} // namespace

int main() {
    primitiveConversionAndShapeSmoke();
    numericValidatorSmoke();
    complexAndNumericClassValidatorSmoke();
    numericShapePreservationSmoke();
    shapeTextAndComparisonSmoke();
    missingArgumentSmoke();
    objectConstraintSmoke();
    std::cout << "runtime argument validation smoke tests passed\n";
    return 0;
}
