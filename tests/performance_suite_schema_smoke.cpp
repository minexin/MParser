#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
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

std::uint64_t unsignedValue(const Json& value,
                            std::string_view path) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        require(signedValue >= 0,
                std::string(path) + " is negative");
        return static_cast<std::uint64_t>(signedValue);
    }
    throw std::runtime_error(
        std::string(path) + " is not an unsigned integer");
}

struct CoverageCounts {
    std::uint64_t executed = 0;
    std::uint64_t guardedFallback = 0;
    std::uint64_t uncovered = 0;
    std::uint64_t unavailable = 0;
};

void addCoverage(CoverageCounts& counts, const std::string& coverage,
                 std::string_view path) {
    if (coverage == "executed") {
        ++counts.executed;
    } else if (coverage == "guarded-fallback") {
        ++counts.guardedFallback;
    } else if (coverage == "uncovered") {
        ++counts.uncovered;
    } else if (coverage == "unavailable") {
        ++counts.unavailable;
    } else {
        throw std::runtime_error(
            std::string(path) + " has unknown coverage: " + coverage);
    }
}

void validateCoverageCounts(const Json& reported,
                            const CoverageCounts& expected,
                            std::string_view path) {
    require(unsignedValue(reported.at("executed"), path) ==
                expected.executed,
            std::string(path) + "/executed is inconsistent");
    require(unsignedValue(reported.at("guarded_fallback"), path) ==
                expected.guardedFallback,
            std::string(path) +
                "/guarded_fallback is inconsistent");
    require(unsignedValue(reported.at("uncovered"), path) ==
                expected.uncovered,
            std::string(path) + "/uncovered is inconsistent");
    require(unsignedValue(reported.at("unavailable"), path) ==
                expected.unavailable,
            std::string(path) + "/unavailable is inconsistent");
}

void validateExecutionCoverage(const Json& execution,
                               const std::string& coverage,
                               std::string_view path) {
    if (coverage == "unavailable") {
        require(execution.is_null(),
                std::string(path) +
                    " must be null when the backend is unavailable");
        return;
    }
    require(execution.is_object(),
            std::string(path) + " must be an execution object");
    const auto attempts = unsignedValue(execution.at("attempts"), path);
    const auto executions =
        unsignedValue(execution.at("executions"), path);
    const auto fallbacks =
        unsignedValue(execution.at("fallbacks"), path);
    require(executions <= attempts,
            std::string(path) + " executions exceed attempts");
    require(fallbacks <= attempts,
            std::string(path) + " fallbacks exceed attempts");

    if (coverage == "executed") {
        require(executions > 0 && fallbacks == 0,
                std::string(path) +
                    " does not represent executed coverage");
    } else if (coverage == "guarded-fallback") {
        require(attempts > 0 && fallbacks > 0,
                std::string(path) +
                    " does not represent guarded fallback");
    } else if (coverage == "uncovered") {
        require(attempts == 0 && executions == 0 && fallbacks == 0,
                std::string(path) +
                    " does not represent uncovered execution");
    }
}

void validateDocument(Validator& validator, const Json& document) {
    validator.validate(document);
    require(document.at("protocol").at("name") ==
                "mparser.performance-suite" &&
                document.at("protocol").at("major") == 1,
            "performance suite protocol identity is invalid");

    const auto& reports = document.at("reports");
    const auto reportCount = unsignedValue(
        document.at("settings").at("report_count"),
        "/settings/report_count");
    require(reportCount == reports.size(),
            "/settings/report_count does not match reports");

    const bool nativeAvailable =
        document.at("build").at("native_jit_available").get<bool>();
    CoverageCounts portableCounts;
    CoverageCounts nativeCounts;
    std::set<std::string> workloadIds;
    std::set<std::string> reportFiles;
    std::set<std::string> sourcePaths;
    for (size_t index = 0; index < reports.size(); ++index) {
        const auto& report = reports[index];
        const std::string prefix =
            "/reports/" + std::to_string(index);
        require(report.at("all_runtime_results_match").get<bool>(),
                prefix + " records a correctness mismatch");
        require(std::isfinite(report.at("reference_value").get<double>()),
                prefix + "/reference_value is not finite");
        require(workloadIds.insert(
                    report.at("workload_id").get<std::string>()).second,
                prefix + " duplicates a workload id");
        require(reportFiles.insert(
                    report.at("report_file").get<std::string>()).second,
                prefix + " duplicates a report file");
        require(sourcePaths.insert(
                    report.at("source_path").get<std::string>()).second,
                prefix + " duplicates a source path");

        const auto portableCoverage =
            report.at("typed_coverage").at("portable")
                .get<std::string>();
        const auto nativeCoverage =
            report.at("typed_coverage").at("native")
                .get<std::string>();
        require(portableCoverage != "unavailable",
                prefix + " marks the portable backend unavailable");
        addCoverage(portableCounts, portableCoverage,
                    prefix + "/typed_coverage/portable");
        addCoverage(nativeCounts, nativeCoverage,
                    prefix + "/typed_coverage/native");
        validateExecutionCoverage(
            report.at("typed_execution").at("portable"),
            portableCoverage, prefix + "/typed_execution/portable");
        validateExecutionCoverage(
            report.at("typed_execution").at("native"),
            nativeCoverage, prefix + "/typed_execution/native");

        const auto& timing = report.at("timing_median_ns");
        const auto& allocations =
            report.at("allocation_requested_bytes");
        if (nativeAvailable) {
            require(nativeCoverage != "unavailable" &&
                        timing.at("native_cold").is_number() &&
                        timing.at("native_warm").is_number() &&
                        allocations.at("native_warm").is_number(),
                    prefix +
                        " is inconsistent with native JIT availability");
        } else {
            require(nativeCoverage == "unavailable" &&
                        timing.at("native_cold").is_null() &&
                        timing.at("native_warm").is_null() &&
                        allocations.at("native_warm").is_null(),
                    prefix +
                        " exposes native measurements in a no-JIT build");
        }
    }

    validateCoverageCounts(
        document.at("coverage_summary").at("portable"),
        portableCounts, "/coverage_summary/portable");
    validateCoverageCounts(
        document.at("coverage_summary").at("native"),
        nativeCounts, "/coverage_summary/native");
}

bool documentRejected(Validator& validator, const Json& document) {
    try {
        validateDocument(validator, document);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

void runNegativeCases(Validator& validator, const Json& document) {
    Json wrongCount = document;
    wrongCount["settings"]["report_count"] =
        wrongCount["settings"]["report_count"].get<std::uint64_t>() + 1;
    require(documentRejected(validator, wrongCount),
            "suite validator accepted a wrong report count");

    Json wrongCoverage = document;
    auto& executed =
        wrongCoverage["coverage_summary"]["portable"]["executed"];
    executed = unsignedValue(
                   executed, "/coverage_summary/portable/executed") +
               1;
    require(documentRejected(validator, wrongCoverage),
            "suite validator accepted inconsistent coverage counts");

    Json wrongCorrectness = document;
    wrongCorrectness["reports"][0]["all_runtime_results_match"] = false;
    require(documentRejected(validator, wrongCorrectness),
            "suite validator accepted a correctness mismatch");

    Json duplicateIdentity = document;
    if (duplicateIdentity["reports"].size() > 1) {
        duplicateIdentity["reports"][1]["workload_id"] =
            duplicateIdentity["reports"][0]["workload_id"];
        require(documentRejected(validator, duplicateIdentity),
                "suite validator accepted a duplicate workload id");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error(
                "usage: performance_suite_schema_smoke "
                "<schema.json> <suite-index.json>");
        }
        Validator validator;
        validator.set_root_schema(readJson(argv[1]));
        const Json document = readJson(argv[2]);
        validateDocument(validator, document);
        runNegativeCases(validator, document);
        std::cout
            << "performance suite protocol 1.0 validated: schema, "
               "identity, coverage, native/no-JIT state, correctness, "
               "and four negative cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Performance suite schema smoke failure: "
                  << error.what() << "\n";
        return 1;
    }
}
