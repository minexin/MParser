#include "mparser/c_api.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(encoded.data()),
        encoded.size());
}

void writeFile(const std::filesystem::path& path,
               std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output);
    output << content;
    assert(output.good());
}

void printLoadFailure(mparser_api_status status,
                      const mparser_module* module) {
    const auto statusName = mparser_api_status_name(status);
    std::cerr << "load status: "
              << std::string_view(statusName.data, statusName.size)
              << '\n';
    for (size_t index = 0;
         index < mparser_module_diagnostic_count(module);
         ++index) {
        const auto* diagnostic =
            mparser_module_diagnostic(module, index);
        const auto identifier =
            mparser_diagnostic_identifier(diagnostic);
        const auto message =
            mparser_diagnostic_message(diagnostic);
        std::cerr
            << std::string_view(identifier.data, identifier.size)
            << ": "
            << std::string_view(message.data, message.size)
            << '\n';
    }
}

const mparser_value* findVariable(
    const mparser_result* result,
    std::string_view expectedName,
    mparser_value*& ownedValue) {
    ownedValue = nullptr;
    for (size_t index = 0;
         index < mparser_result_variable_count(result);
         ++index) {
        mparser_utf8_view name{};
        mparser_value* value = nullptr;
        assert(mparser_result_variable(
                   result, index, &name, &value) ==
               MPARSER_API_STATUS_OK);
        if (std::string_view(name.data, name.size) == expectedName) {
            ownedValue = value;
            return value;
        }
        mparser_value_release(value);
    }
    return nullptr;
}

} // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    TemporaryDirectory temporary{
        std::filesystem::temp_directory_path() /
        ("mparser_c_utf8_" + std::to_string(nonce))};
    const auto entryDirectory =
        temporary.path /
        std::filesystem::path(
            std::u8string(
                u8"entry_\u8def\u5f84_\U0001f9ea"));
    const auto libraryDirectory =
        temporary.path /
        std::filesystem::path(
            std::u8string(u8"library_\u6e90"));
    const auto entryPath = entryDirectory / "main.m";
    writeFile(entryPath,
              "object = utfpkg.Value(9);\n"
              "utf8_result = object.get();\n");
    writeFile(libraryDirectory / "+utfpkg" / "Value.m",
              "classdef Value\n"
              "    properties\n"
              "        Data\n"
              "    end\n"
              "    methods\n"
              "        function obj = Value(data)\n"
              "            obj.Data = data;\n"
              "        end\n"
              "        function result = get(obj)\n"
              "            result = obj.Data;\n"
              "        end\n"
              "    end\n"
              "end\n");

    const std::string entryUtf8 = pathToUtf8(entryPath);
    const std::string libraryUtf8 = pathToUtf8(libraryDirectory);
    mparser_utf8_view searchPath{
        libraryUtf8.data(), libraryUtf8.size()};
    mparser_source_load_options loadOptions{};
    assert(MPARSER_SOURCE_LOAD_OPTIONS_INIT(&loadOptions) ==
           MPARSER_API_STATUS_OK);
    loadOptions.search_paths = &searchPath;
    loadOptions.search_path_count = 1;

    mparser_module* module = nullptr;
    const auto loadStatus = mparser_module_load_file_utf8(
        entryUtf8.data(), entryUtf8.size(),
        &loadOptions, &module);
    if (loadStatus != MPARSER_API_STATUS_OK) {
        printLoadFailure(loadStatus, module);
    }
    assert(loadStatus == MPARSER_API_STATUS_OK);
    assert(module != nullptr);
    assert(mparser_module_is_valid(module) == 1);
    assert(mparser_module_source_count(module) == 2);
    const auto retainedEntry =
        mparser_module_source_name(module, 0);
    assert(std::string_view(
               retainedEntry.data, retainedEntry.size) ==
           pathToUtf8(std::filesystem::weakly_canonical(entryPath)));

    mparser_invocation_options invocation{};
    assert(MPARSER_INVOCATION_OPTIONS_INIT(&invocation) ==
           MPARSER_API_STATUS_OK);
    mparser_result* result = nullptr;
    assert(mparser_module_execute(
               module, &invocation, &result) ==
           MPARSER_API_STATUS_OK);
    assert(result != nullptr);
    assert(mparser_result_succeeded(result) == 1);

    mparser_value* value = nullptr;
    assert(findVariable(result, "utf8_result", value) != nullptr);
    mparser_numeric_buffer buffer{};
    assert(mparser_value_get_numeric_buffer(value, &buffer) ==
           MPARSER_API_STATUS_OK);
    assert(buffer.numeric_class == MPARSER_NUMERIC_DOUBLE);
    assert(buffer.is_complex == 0);
    assert(buffer.element_count == 1);
    assert(std::fabs(static_cast<const double*>(
                         buffer.real_data)[0] - 9.0) < 1e-9);

    mparser_value_release(value);
    mparser_result_release(result);
    mparser_module_release(module);
    return 0;
}
