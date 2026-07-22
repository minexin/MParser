#include "mparser/runtime_argument_validation.h"
#include "mparser/runtime_shape.h"
#include "mparser/runtime_text.h"

#include <cassert>
#include <cmath>
#include <iostream>
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
    shapeTextAndComparisonSmoke();
    objectConstraintSmoke();
    std::cout << "runtime argument validation smoke tests passed\n";
    return 0;
}
