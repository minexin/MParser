#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
                            const std::string& path) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        require(signedValue >= 0, path + " is negative");
        return static_cast<std::uint64_t>(signedValue);
    }
    throw std::runtime_error(path + " is not an unsigned integer");
}

std::uint64_t addChecked(std::uint64_t total,
                         std::uint64_t value,
                         const std::string& path) {
    require(value <= std::numeric_limits<std::uint64_t>::max() - total,
            path + " overflowed uint64");
    return total + value;
}

bool approximatelyEqual(double left, double right) {
    const double scale = std::max({1.0, std::abs(left),
                                   std::abs(right)});
    return std::abs(left - right) <= scale * 1e-12;
}

void validateTiming(const Json& timing, std::uint64_t expectedCount,
                    const std::string& path) {
    require(timing.at("unit") == "nanoseconds",
            path + "/unit is not nanoseconds");
    const auto& samplesJson = timing.at("samples_ns");
    require(samplesJson.size() == expectedCount,
            path + "/samples_ns count does not match iterations");

    std::vector<std::uint64_t> samples;
    samples.reserve(samplesJson.size());
    std::uint64_t total = 0;
    for (size_t index = 0; index < samplesJson.size(); ++index) {
        const auto sample = unsignedValue(
            samplesJson[index],
            path + "/samples_ns/" + std::to_string(index));
        samples.push_back(sample);
        total = addChecked(total, sample, path + "/total_ns");
    }
    require(!samples.empty(), path + " has no samples");

    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t count = sorted.size();
    const double mean =
        static_cast<double>(total) / static_cast<double>(count);
    const double median =
        count % 2 == 0
            ? (static_cast<double>(sorted[count / 2 - 1]) +
               static_cast<double>(sorted[count / 2])) /
                  2.0
            : static_cast<double>(sorted[count / 2]);
    const size_t p95Index =
        std::min(count - 1, (count * 95 + 99) / 100 - 1);

    require(unsignedValue(timing.at("total_ns"),
                          path + "/total_ns") == total,
            path + "/total_ns is not the sample sum");
    require(approximatelyEqual(timing.at("mean_ns").get<double>(),
                               mean),
            path + "/mean_ns is inconsistent");
    require(approximatelyEqual(timing.at("median_ns").get<double>(),
                               median),
            path + "/median_ns is inconsistent");
    require(unsignedValue(timing.at("minimum_ns"),
                          path + "/minimum_ns") == sorted.front(),
            path + "/minimum_ns is inconsistent");
    require(unsignedValue(timing.at("p95_ns"),
                          path + "/p95_ns") == sorted[p95Index],
            path + "/p95_ns is inconsistent");
    require(unsignedValue(timing.at("maximum_ns"),
                          path + "/maximum_ns") == sorted.back(),
            path + "/maximum_ns is inconsistent");
}

void validateAllocationActivity(
    const Json& allocation, std::uint64_t expectedCount,
    const std::string& path) {
    require(allocation.at("aggregation") ==
                "sum-across-measured-iterations",
            path + "/aggregation is incorrect");
    const auto& countSamples =
        allocation.at("successful_request_samples");
    const auto& byteSamples =
        allocation.at("requested_byte_samples");
    require(countSamples.size() == expectedCount,
            path + "/successful_request_samples count is inconsistent");
    require(byteSamples.size() == expectedCount,
            path + "/requested_byte_samples count is inconsistent");

    std::uint64_t countTotal = 0;
    std::uint64_t byteTotal = 0;
    for (size_t index = 0; index < countSamples.size(); ++index) {
        countTotal = addChecked(
            countTotal,
            unsignedValue(
                countSamples[index],
                path + "/successful_request_samples/" +
                    std::to_string(index)),
            path + "/successful_request_count");
        byteTotal = addChecked(
            byteTotal,
            unsignedValue(
                byteSamples[index],
                path + "/requested_byte_samples/" +
                    std::to_string(index)),
            path + "/requested_bytes");
    }
    require(unsignedValue(
                allocation.at("successful_request_count"),
                path + "/successful_request_count") == countTotal,
            path + "/successful_request_count is not the sample sum");
    require(unsignedValue(allocation.at("requested_bytes"),
                          path + "/requested_bytes") == byteTotal,
            path + "/requested_bytes is not the sample sum");
}

void validateMeasuredPhase(
    const Json& measurement, std::uint64_t expectedWarmup,
    std::uint64_t expectedIterations, bool expectEngineTiming,
    bool expectAllocationActivity, bool expectExecutionSummary,
    const std::string& path) {
    require(measurement.at("status") == "measured",
            path + " is not measured");
    require(measurement.at("reason").get<std::string>().empty(),
            path + "/reason is not empty");
    require(!measurement.at("boundary").get<std::string>().empty(),
            path + "/boundary is empty");
    require(unsignedValue(measurement.at("warmup_iterations"),
                          path + "/warmup_iterations") ==
                expectedWarmup,
            path + "/warmup_iterations is inconsistent");
    const auto measuredIterations =
        unsignedValue(measurement.at("measured_iterations"),
                      path + "/measured_iterations");
    require(measuredIterations == expectedIterations,
            path + "/measured_iterations is inconsistent");
    validateTiming(measurement.at("host_wall"), measuredIterations,
                   path + "/host_wall");

    if (expectEngineTiming) {
        require(measurement.at("engine_elapsed").is_object(),
                path + "/engine_elapsed is missing");
        validateTiming(measurement.at("engine_elapsed"),
                       measuredIterations,
                       path + "/engine_elapsed");
    } else {
        require(measurement.at("engine_elapsed").is_null(),
                path + "/engine_elapsed must be null");
    }

    require(measurement.at("allocation_activity").is_object() ==
                expectAllocationActivity,
            path + "/allocation_activity state is inconsistent");
    if (expectAllocationActivity) {
        validateAllocationActivity(
            measurement.at("allocation_activity"),
            measuredIterations, path + "/allocation_activity");
    }
    require(measurement.at("execution_summary").is_object() ==
                expectExecutionSummary,
            path + "/execution_summary state is inconsistent");
    if (expectExecutionSummary) {
        const auto& summary = measurement.at("execution_summary");
        require(!summary.at("effective_tiers").empty(),
                path + "/execution_summary/effective_tiers is empty");
        const auto fallback = unsignedValue(
            summary.at("fallback_iterations"),
            path + "/execution_summary/fallback_iterations");
        require(fallback <= measuredIterations,
                path + "/execution_summary/fallback_iterations "
                       "exceeds measured iterations");
    }
}

void validateUnavailablePhase(const Json& measurement,
                              const std::string& path) {
    require(measurement.at("status") == "unavailable",
            path + " is not unavailable");
    require(!measurement.at("reason").get<std::string>().empty(),
            path + "/reason is empty");
    require(unsignedValue(measurement.at("warmup_iterations"),
                          path + "/warmup_iterations") == 0,
            path + "/warmup_iterations must be zero");
    require(unsignedValue(measurement.at("measured_iterations"),
                          path + "/measured_iterations") == 0,
            path + "/measured_iterations must be zero");
    for (const char* name : {"host_wall", "engine_elapsed",
                             "allocation_activity",
                             "execution_summary"}) {
        require(measurement.at(name).is_null(),
                path + "/" + name + " must be null");
    }
}

void validateCacheMonotonic(const Json& before, const Json& after,
                            const std::string& path) {
    require(before.at("limits") == after.at("limits"),
            path + "/limits changed during the baseline");
    static constexpr std::string_view counters[]{
        "lookup_count",
        "hit_count",
        "miss_count",
        "compilation_count",
        "compilation_failure_count",
        "insertion_count",
        "duplicate_compilation_count",
        "bypass_count",
        "eviction_count",
        "evicted_code_bytes",
        "clear_count",
        "cleared_entry_count",
        "cleared_code_bytes",
    };
    for (const auto counter : counters) {
        const std::string name(counter);
        require(unsignedValue(after.at(name), path + "/" + name) >=
                    unsignedValue(before.at(name), path + "/" + name),
                path + "/" + name + " decreased");
    }
}

void validateBaselineSemantics(const Json& report,
                               bool requireNativeCache) {
    require(report.at("protocol").at("name") ==
                "mparser.performance-baseline",
            "/protocol/name is incorrect");
    require(report.at("protocol").at("major") == 1,
            "/protocol/major is incorrect");

    const auto& settings = report.at("settings");
    const auto parseWarmup = unsignedValue(
        settings.at("parse_warmup"), "/settings/parse_warmup");
    const auto parseIterations = unsignedValue(
        settings.at("parse_iterations"), "/settings/parse_iterations");
    const auto compileWarmup = unsignedValue(
        settings.at("compile_warmup"), "/settings/compile_warmup");
    const auto compileIterations = unsignedValue(
        settings.at("compile_iterations"),
        "/settings/compile_iterations");
    const auto runtimeWarmup = unsignedValue(
        settings.at("runtime_warmup"), "/settings/runtime_warmup");
    const auto runtimeIterations = unsignedValue(
        settings.at("runtime_iterations"),
        "/settings/runtime_iterations");
    const auto processIterations = unsignedValue(
        settings.at("process_iterations"),
        "/settings/process_iterations");

    const auto& measurements = report.at("measurements");
    validateMeasuredPhase(measurements.at("parse"), parseWarmup,
                          parseIterations, false, true, false,
                          "/measurements/parse");
    validateMeasuredPhase(measurements.at("compile"), compileWarmup,
                          compileIterations, false, true, false,
                          "/measurements/compile");
    validateMeasuredPhase(
        measurements.at("process_cold_start"), 0,
        processIterations, false, false, false,
        "/measurements/process_cold_start");
    validateMeasuredPhase(measurements.at("bytecode"),
                          runtimeWarmup, runtimeIterations,
                          true, true, true,
                          "/measurements/bytecode");
    validateMeasuredPhase(measurements.at("portable"),
                          runtimeWarmup, runtimeIterations,
                          true, true, true,
                          "/measurements/portable");

    const bool nativeAvailable =
        report.at("build").at("native_jit_available").get<bool>();
    if (nativeAvailable) {
        validateMeasuredPhase(measurements.at("native_cold"), 0, 1,
                              true, true, true,
                              "/measurements/native_cold");
        validateMeasuredPhase(
            measurements.at("native_warm"), runtimeWarmup,
            runtimeIterations, true, true, true,
            "/measurements/native_warm");
    } else {
        validateUnavailablePhase(measurements.at("native_cold"),
                                 "/measurements/native_cold");
        validateUnavailablePhase(measurements.at("native_warm"),
                                 "/measurements/native_warm");
    }

    const auto& resources = report.at("resources");
    require(unsignedValue(resources.at("peak_resident_bytes"),
                          "/resources/peak_resident_bytes") > 0,
            "/resources/peak_resident_bytes is zero");
    for (const char* name : {"baseline_tool", "mparser_cli",
                             "mparser_c_api"}) {
        const auto& artifact =
            resources.at("binary_artifacts").at(name);
        require(unsignedValue(
                    artifact.at("size_bytes"),
                    std::string("/resources/binary_artifacts/") +
                        name + "/size_bytes") > 0,
                std::string("/resources/binary_artifacts/") + name +
                    "/size_bytes is zero");
        require(!artifact.at("path").get<std::string>().empty(),
                std::string("/resources/binary_artifacts/") + name +
                    "/path is empty");
    }

    const auto& cache = resources.at("native_cache");
    const auto& before = cache.at("before");
    const auto& afterCold = cache.at("after_cold");
    const auto& afterWarm = cache.at("after_warm");
    require(unsignedValue(before.at("entry_count"),
                          "/resources/native_cache/before/entry_count") ==
                0,
            "native cache was not empty before the cold phase");
    require(unsignedValue(before.at("code_bytes"),
                          "/resources/native_cache/before/code_bytes") ==
                0,
            "native cache code bytes were not empty before cold phase");
    validateCacheMonotonic(
        before, afterCold, "/resources/native_cache/after_cold");
    validateCacheMonotonic(
        afterCold, afterWarm, "/resources/native_cache/after_warm");

    if (nativeAvailable && requireNativeCache) {
        require(unsignedValue(
                    afterCold.at("compilation_count"),
                    "/resources/native_cache/after_cold/"
                    "compilation_count") > 0,
                "eligible native cold phase did not compile a kernel");
        require(unsignedValue(
                    afterCold.at("insertion_count"),
                    "/resources/native_cache/after_cold/"
                    "insertion_count") > 0,
                "eligible native cold phase did not populate cache");
        require(unsignedValue(
                    afterWarm.at("hit_count"),
                    "/resources/native_cache/after_warm/hit_count") >
                    unsignedValue(
                        afterCold.at("hit_count"),
                        "/resources/native_cache/after_cold/hit_count"),
                "eligible native warm phase did not hit cache");
        require(unsignedValue(
                    measurements.at("native_cold")
                        .at("execution_summary")
                        .at("native_compilation_count"),
                    "/measurements/native_cold/execution_summary/"
                    "native_compilation_count") > 0,
                "native cold execution summary did not report compile");
        require(unsignedValue(
                    measurements.at("native_warm")
                        .at("execution_summary")
                        .at("native_cache_hit_count"),
                    "/measurements/native_warm/execution_summary/"
                    "native_cache_hit_count") > 0,
                "native warm execution summary did not report hit");
    }

    const auto& correctness = report.at("correctness");
    require(correctness.at("result_variable") ==
                report.at("workload").at("result_variable"),
            "/correctness/result_variable differs from workload");
    require(correctness.at("bytecode_matches").get<bool>(),
            "bytecode correctness flag is false");
    require(correctness.at("portable_matches").get<bool>(),
            "portable correctness flag is false");
    require(correctness.at("native_matches").get<bool>(),
            "native correctness flag is false");
    require(correctness.at("all_runtime_results_match").get<bool>(),
            "aggregate correctness flag is false");
    require(unsignedValue(
                correctness.at("process_exit_success_count"),
                "/correctness/process_exit_success_count") ==
                processIterations,
            "process success count differs from process iterations");
}

void validateDocument(const Validator& validator, const Json& report,
                      bool requireNativeCache) {
    validator.validate(report);
    validateBaselineSemantics(report, requireNativeCache);
}

bool documentRejected(const Validator& validator, const Json& report,
                      bool requireNativeCache) {
    try {
        validateDocument(validator, report, requireNativeCache);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

void runNegativeCases(const Validator& validator, const Json& report,
                      bool requireNativeCache) {
    Json invalidMajor = report;
    invalidMajor["protocol"]["major"] = 2;
    require(documentRejected(validator, invalidMajor,
                             requireNativeCache),
            "schema accepted protocol major 2");

    Json invalidTotal = report;
    auto& total =
        invalidTotal["measurements"]["parse"]["host_wall"]["total_ns"];
    total = unsignedValue(total,
                          "/measurements/parse/host_wall/total_ns") +
            1;
    require(documentRejected(validator, invalidTotal,
                             requireNativeCache),
            "semantic validator accepted an inconsistent timing total");

    Json invalidCorrectness = report;
    invalidCorrectness["correctness"]["portable_matches"] = false;
    require(documentRejected(validator, invalidCorrectness,
                             requireNativeCache),
            "semantic validator accepted a failed portable result");

    Json invalidNativeState = report;
    if (report.at("build").at("native_jit_available").get<bool>()) {
        invalidNativeState["measurements"]["native_cold"]["status"] =
            "unavailable";
    } else {
        invalidNativeState["measurements"]["native_cold"]["status"] =
            "measured";
    }
    require(documentRejected(validator, invalidNativeState,
                             requireNativeCache),
            "semantic validator accepted an inconsistent native state");
}

} // namespace

int main(int argc, char** argv) {
    try {
        bool requireNativeCache = false;
        int argument = 1;
        if (argument < argc &&
            std::string_view(argv[argument]) ==
                "--require-native-cache") {
            requireNativeCache = true;
            ++argument;
        }
        if (argc - argument != 2) {
            throw std::runtime_error(
                "usage: performance_baseline_schema_smoke "
                "[--require-native-cache] <schema.json> <report.json>");
        }

        Validator validator;
        validator.set_root_schema(readJson(argv[argument]));
        const Json report = readJson(argv[argument + 1]);
        validateDocument(validator, report, requireNativeCache);
        runNegativeCases(validator, report, requireNativeCache);
        std::cout
            << "performance baseline protocol 1.0 validated: "
               "schema, timing, resources, cache, correctness, "
               "four negative cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Performance baseline schema smoke failure: "
                  << error.what() << "\n";
        return 1;
    }
}
