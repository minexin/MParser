#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;
using Validator = nlohmann::json_schema::json_validator;

Json readJson(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            std::string("failed to open JSON file: ") + path);
    }
    Json value;
    input >> value;
    return value;
}

void require(bool condition, std::string message) {
    if (!condition) {
        throw std::runtime_error(std::move(message));
    }
}

std::uint64_t requireUint64(const Json& value,
                            const std::string& path) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer() &&
        value.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(
            value.get<std::int64_t>());
    }
    throw std::runtime_error(path +
                             " is not an unsigned 64-bit integer");
}

size_t numericElementCount(const Json& value,
                           const std::string& path) {
    size_t result = 1;
    size_t index = 0;
    for (const auto& dimension : value.at("dimensions")) {
        const std::uint64_t count = requireUint64(
            dimension, path + "/dimensions/" +
                           std::to_string(index++));
        require(count <= std::numeric_limits<size_t>::max(),
                path + " dimension exceeds host size_t");
        const size_t hostCount = static_cast<size_t>(count);
        require(hostCount == 0 ||
                    result <= std::numeric_limits<size_t>::max() /
                                  hostCount,
                path + " dimensions overflow host size_t");
        result *= hostCount;
    }
    return result;
}

void validateIntegerElement(const Json& value,
                            const std::string& className,
                            const std::string& path) {
    const bool signedClass = !className.empty() &&
                             className.front() == 'i';
    if (signedClass) {
        static constexpr std::array signedRanges{
            std::pair{"int8", std::pair<std::int64_t, std::int64_t>{
                                   -128, 127}},
            std::pair{"int16", std::pair<std::int64_t, std::int64_t>{
                                    -32768, 32767}},
            std::pair{"int32", std::pair<std::int64_t, std::int64_t>{
                                    std::numeric_limits<std::int32_t>::min(),
                                    std::numeric_limits<std::int32_t>::max()}},
            std::pair{"int64", std::pair<std::int64_t, std::int64_t>{
                                    std::numeric_limits<std::int64_t>::min(),
                                    std::numeric_limits<std::int64_t>::max()}},
        };
        for (const auto& [name, range] : signedRanges) {
            if (className != name) {
                continue;
            }
            if (value.is_number_unsigned()) {
                require(value.get<std::uint64_t>() <=
                            static_cast<std::uint64_t>(range.second),
                        path + " exceeds " + className);
                return;
            }
            require(value.is_number_integer(),
                    path + " is not an integer");
            const std::int64_t number = value.get<std::int64_t>();
            require(number >= range.first && number <= range.second,
                    path + " exceeds " + className);
            return;
        }
    } else {
        static constexpr std::array unsignedMaximums{
            std::pair{"uint8", std::uint64_t{255}},
            std::pair{"uint16", std::uint64_t{65535}},
            std::pair{"uint32", std::uint64_t{4294967295ULL}},
            std::pair{"uint64",
                      std::numeric_limits<std::uint64_t>::max()},
        };
        const std::uint64_t number = requireUint64(value, path);
        for (const auto& [name, maximum] : unsignedMaximums) {
            if (className == name) {
                require(number <= maximum,
                        path + " exceeds " + className);
                return;
            }
        }
    }
    throw std::runtime_error(path + " has an unknown integer class");
}

void validateNumericChannel(const Json& data,
                            const std::string& className,
                            const std::string& path) {
    if (className.find("int") == std::string::npos) {
        return;
    }
    size_t index = 0;
    for (const auto& element : data) {
        validateIntegerElement(
            element, className,
            path + "/" + std::to_string(index++));
    }
}

void validateRuntimeValueSemantics(
    const Json& value, const std::string& path) {
    if (value.contains("dimensions")) {
        size_t index = 0;
        for (const auto& dimension : value.at("dimensions")) {
            requireUint64(
                dimension, path + "/dimensions/" +
                               std::to_string(index++));
        }
    }

    const std::string kind = value.value("kind", "");
    if (kind == "numeric") {
        const size_t count = numericElementCount(value, path);
        const std::string className = value.at("class");
        require(value.at("data").size() == count,
                path + "/data length does not match dimensions");
        validateNumericChannel(
            value.at("data"), className, path + "/data");
        if (value.contains("imaginary_data")) {
            require(value.value("complex", false),
                    path + " imaginary payload is not marked complex");
            require(value.at("imaginary_data").size() == count,
                    path + "/imaginary_data length does not match dimensions");
            validateNumericChannel(value.at("imaginary_data"),
                                   className,
                                   path + "/imaginary_data");
        }
    } else if (kind == "cell" ||
               kind == "comma-separated-list") {
        size_t index = 0;
        for (const auto& element : value.at("data")) {
            validateRuntimeValueSemantics(
                element, path + "/data/" +
                             std::to_string(index++));
        }
    } else if (kind == "struct") {
        size_t index = 0;
        for (const auto& element : value.at("data")) {
            for (const auto& [name, field] : element.items()) {
                validateRuntimeValueSemantics(
                    field, path + "/data/" +
                               std::to_string(index) + "/" + name);
            }
            ++index;
        }
    } else if (kind == "name-value-argument") {
        validateRuntimeValueSemantics(
            value.at("value"), path + "/value");
    }
}

void validateDiagnosticSemantics(
    const Json& diagnostic, const std::string& path) {
    if (diagnostic.contains("source") &&
        diagnostic.at("source").is_object()) {
        requireUint64(
            diagnostic.at("source").at("begin").at("offset"),
            path + "/source/begin/offset");
        requireUint64(
            diagnostic.at("source").at("end").at("offset"),
            path + "/source/end/offset");
    }
    size_t index = 0;
    for (const auto& cause : diagnostic.at("causes")) {
        validateDiagnosticSemantics(
            cause, path + "/causes/" + std::to_string(index++));
    }
}

void validateProtocolSemantics(const Json& document) {
    requireUint64(document.at("requested_output_count"),
                  "/requested_output_count");

    size_t index = 0;
    for (const auto& output : document.at("outputs")) {
        validateRuntimeValueSemantics(
            output.at("value"),
            "/outputs/" + std::to_string(index++) + "/value");
    }
    index = 0;
    for (const auto& variable : document.at("workspace")) {
        validateRuntimeValueSemantics(
            variable.at("value"),
            "/workspace/" + std::to_string(index++) + "/value");
    }
    index = 0;
    for (const auto& diagnostic : document.at("diagnostics")) {
        validateDiagnosticSemantics(
            diagnostic,
            "/diagnostics/" + std::to_string(index++));
    }

    static constexpr std::array executionCounters{
        "executed_instruction_count",
        "typed_region_count",
        "typed_region_attempt_count",
        "typed_region_execution_count",
        "typed_region_fallback_count",
        "native_compilation_count",
        "native_cache_hit_count",
        "maximum_call_depth",
        "maximum_array_bytes",
        "maximum_diagnostic_count",
        "elapsed_nanoseconds",
    };
    for (const char* name : executionCounters) {
        requireUint64(document.at("execution").at(name),
                      std::string("/execution/") + name);
    }
}

void validateDocument(const Validator& validator, const Json& value) {
    validator.validate(value);
    validateProtocolSemantics(value);
}

bool documentRejected(const Validator& validator, const Json& value) {
    try {
        validateDocument(validator, value);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

Json* findNumericClass(Json& document, const std::string& className) {
    for (auto& variable : document.at("workspace")) {
        auto& value = variable.at("value");
        if (value.value("kind", "") == "numeric" &&
            value.value("class", "") == className) {
            return &value;
        }
    }
    return nullptr;
}

void runNegativeCases(const Validator& validator, const Json& golden) {
    Json invalidMajor = golden;
    invalidMajor["protocol"]["major"] = 2;
    require(documentRejected(validator, invalidMajor),
            "schema accepted protocol major 2");

    Json futureMinor = golden;
    futureMinor["protocol"]["minor"] = 7;
    futureMinor["future_root"] = Json::object();
    validateDocument(validator, futureMinor);

    Json invalidCause = golden;
    auto& causes = invalidCause["diagnostics"][0]["causes"];
    require(!causes.empty(), "golden fixture has no diagnostic cause");
    causes[0].erase("message");
    require(documentRejected(validator, invalidCause),
            "schema accepted a cause without a message");

    Json invalidNestedCause = golden;
    auto& nested =
        invalidNestedCause["diagnostics"][0]["causes"][0]["causes"];
    require(!nested.empty(), "golden fixture has no nested cause");
    nested[0].erase("identifier");
    require(documentRejected(validator, invalidNestedCause),
            "schema accepted a nested cause without an identifier");

    Json invalidLogical = golden;
    Json* logical = findNumericClass(invalidLogical, "logical");
    require(logical && !logical->at("data").empty(),
            "golden fixture has no logical numeric payload");
    (*logical)["data"][0] = 1;
    require(documentRejected(validator, invalidLogical),
            "schema accepted numeric data for a logical array");

    Json invalidDouble = golden;
    Json* numeric = findNumericClass(invalidDouble, "double");
    require(numeric && !numeric->at("data").empty(),
            "golden fixture has no double numeric payload");
    (*numeric)["data"][0] = true;
    require(documentRejected(validator, invalidDouble),
            "schema accepted boolean data for a double array");

    Json invalidToken = golden;
    numeric = findNumericClass(invalidToken, "double");
    require(numeric && !numeric->at("data").empty(),
            "golden fixture has no double payload for token check");
    (*numeric)["data"][0] = "not-a-number";
    require(documentRejected(validator, invalidToken),
            "schema accepted an unknown nonfinite token");

    Json invalidUnsigned = golden;
    invalidUnsigned["requested_output_count"] = -1;
    require(documentRejected(validator, invalidUnsigned),
            "protocol semantic validation accepted a negative uint64");

    Json missingImaginary = golden;
    Json* complex = findNumericClass(missingImaginary, "single");
    require(complex && complex->value("complex", false),
            "golden fixture has no complex numeric payload");
    complex->erase("imaginary_data");
    require(documentRejected(validator, missingImaginary),
            "schema accepted a complex value without imaginary_data");

    Json invalidImaginary = golden;
    complex = findNumericClass(invalidImaginary, "single");
    (*complex)["imaginary_data"][0] = true;
    require(documentRejected(validator, invalidImaginary),
            "schema accepted a boolean imaginary float component");

    Json truncatedComplex = golden;
    complex = findNumericClass(truncatedComplex, "single");
    complex->at("imaginary_data").erase(
        complex->at("imaginary_data").begin());
    require(documentRejected(validator, truncatedComplex),
            "semantic validation accepted a truncated imaginary channel");

    Json invalidUint64 = golden;
    Json* uint64Value = findNumericClass(invalidUint64, "uint64");
    require(uint64Value && !uint64Value->at("data").empty(),
            "golden fixture has no uint64 numeric payload");
    (*uint64Value)["data"][0] = -1;
    require(documentRejected(validator, invalidUint64),
            "semantic validation accepted a negative uint64 element");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            throw std::runtime_error(
                "usage: machine_protocol_schema_smoke "
                "[--contract-cases] <schema.json> <document.json>...");
        }

        const bool runContractCases =
            std::string_view(argv[1]) == "--contract-cases";
        const int schemaIndex = runContractCases ? 2 : 1;
        const int documentIndex = schemaIndex + 1;
        if (argc <= documentIndex) {
            throw std::runtime_error(
                "schema validation requires at least one document");
        }

        Validator validator;
        validator.set_root_schema(readJson(argv[schemaIndex]));
        for (int index = documentIndex; index < argc; ++index) {
            validateDocument(validator, readJson(argv[index]));
        }

        if (runContractCases) {
            runNegativeCases(
                validator, readJson(argv[documentIndex]));
        }
        std::cout << "machine protocol JSON Schema validation passed for "
                  << argc - documentIndex << " document(s)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Machine protocol schema smoke failure: "
                  << error.what() << "\n";
        return 1;
    }
}
