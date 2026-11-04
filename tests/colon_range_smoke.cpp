#include "mparser/cpp_api.hpp"
#include "mparser/execution/interpreter.h"
#include "mparser/frontend/lexer.h"
#include "mparser/frontend/parser.h"
#include "mparser/runtime/core/value/runtime_numeric.h"
#include "mparser/semantic/semantic.h"
#include <nlohmann/json.hpp>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Json = nlohmann::json;
using namespace mparser::sdk;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

std::vector<std::string> hexValues(const Json& test) {
    if (test.at("hex").is_string()) {
        const auto text = test.at("hex").get<std::string>();
        return text.empty() ? std::vector<std::string>{} : std::vector<std::string>{text};
    }
    return test.at("hex").get<std::vector<std::string>>();
}

template<class Number>
void checkNumber(Number actual, const std::string& expected, const std::string& label) {
    if constexpr (sizeof(Number) == sizeof(float)) {
        const auto bits = static_cast<std::uint32_t>(std::stoul(expected, nullptr, 16));
        const auto reference = std::bit_cast<float>(bits);
        require(std::isnan(reference) ? std::isnan(actual) : std::bit_cast<std::uint32_t>(actual) == bits, label);
    } else {
        const auto bits = static_cast<std::uint64_t>(std::stoull(expected, nullptr, 16));
        const auto reference = std::bit_cast<double>(bits);
        require(std::isnan(reference) ? std::isnan(actual) : std::bit_cast<std::uint64_t>(actual) == bits, label);
    }
}

void checkValue(const Value& value, const Json& test, const std::string& label) {
    require(value.dimensions() == test.at("dimensions").get<std::vector<size_t>>(), label + " shape");
    const auto expected = hexValues(test);
    const bool single = test.at("class") == "single";
    require(value.numericClass() == (single ? NumericClass::Single : NumericClass::Double), label + " class");
    if (single) {
        const auto data = value.numericData<float>();
        require(data.size() == expected.size(), label + " count");
        for (size_t index = 0; index < data.size(); ++index) {
            checkNumber(data[index], expected[index], label + " element " + std::to_string(index));
        }
    } else {
        const auto data = value.numericData<double>();
        require(data.size() == expected.size(), label + " count");
        for (size_t index = 0; index < data.size(); ++index) {
            checkNumber(data[index], expected[index], label + " element " + std::to_string(index));
        }
    }
}

void checkHir(const Json& test) {
    const auto source = "value=" + test.at("expression").get<std::string>() + ";";
    mparser::Lexer lexer(source);
    mparser::Parser parser(lexer.lex());
    auto parsed = parser.parse();
    require(parsed.diagnostics.empty(), "HIR reference parsing");
    auto semantic = mparser::SemanticAnalyzer{}.analyze(*parsed.root);
    require(semantic.diagnostics.empty(), "HIR reference semantics");
    const auto result = mparser::Interpreter{}.run(semantic);
    const auto label = test.at("id").get<std::string>() + "/HIR";
    require(result.diagnostics.empty() == test.at("succeeded").get<bool>(), label + " success");
    if (!result.diagnostics.empty()) { return; }
    const auto& value = result.variables.front().value;
    const auto expected = hexValues(test);
    require(value.rows == 1 && value.columns == expected.size(), label + " shape " +
        std::to_string(value.rows) + "x" + std::to_string(value.columns));
    const bool single = test.at("class") == "single";
    require(value.numericClass == (single ? mparser::RuntimeNumericClass::Single : mparser::RuntimeNumericClass::Double), label + " class");
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto number = mparser::runtimeNumericElementValue(value, index);
        require(number.has_value(), label + " numeric element");
        if (single) { checkNumber(static_cast<float>(number->real), expected[index], label + " element " + std::to_string(index)); }
        else { checkNumber(number->real, expected[index], label + " element " + std::to_string(index)); }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected R2024b colon corpus path");
        std::ifstream input(argv[1]);
        const auto corpus = Json::parse(input);
        require(corpus.at("release") == "2024b" && corpus.at("cases").size() == 38, "fixed colon reference corpus");
        size_t checks = 0;
        for (const auto& test : corpus.at("cases")) {
            checkHir(test);
            ++checks;
            for (const auto backend : {Backend::Automatic, Backend::Bytecode, Backend::Portable, Backend::Native}) {
                Invocation request;
                request.backend = backend;
                const auto result = Module::compile("value=" + test.at("expression").get<std::string>() + ";", "colon_reference.m").execute(request);
                const auto label = test.at("id").get<std::string>() + "/SDK";
                require(result.succeeded() == test.at("succeeded").get<bool>(), label + " success");
                if (result.succeeded()) { checkValue(result.variables().front().value, test, label); }
                ++checks;
            }
        }
        for (const auto backend : {Backend::Automatic, Backend::Bytecode, Backend::Portable, Backend::Native}) {
            Invocation request;
            request.backend = backend;
            const auto result = Module::compile("x=0:0.05:10; h=plot(x,sin(x));", "colon_plot.m").execute(request);
            require(result.succeeded(), "colon graphics execution");
            const auto graph = Json::parse(result.graphicsJson());
            const auto& x = graph.at("objects").back().at("properties").at("XData");
            require(x.size() == 201 && x.back() == 10, "colon graphics missing endpoint");
        }
        std::cout << "PASS colon reference: " << checks << " checks and four graphics snapshots\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
