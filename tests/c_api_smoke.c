#include "mparser/c_api.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "c_api_smoke:%d: %s\n", __LINE__, #condition); \
            return 0;                                                      \
        }                                                                  \
    } while (0)

static const char k_module_source[] =
    "function total = sumTo(limit)\n"
    "total = 0;\n"
    "for i = 1:limit\n"
    "    total = total + i;\n"
    "end\n"
    "end\n"
    "\n"
    "function out = identity(value)\n"
    "out = value;\n"
    "end\n"
    "\n"
    "function out = spin()\n"
    "out = 0;\n"
    "while 1\n"
    "    out = out + 1;\n"
    "end\n"
    "end\n"
    "\n"
    "function handle = makeClosure(factor)\n"
    "handle = @(value)value * factor;\n"
    "end\n"
    "\n"
    "function handle = makeBuiltin()\n"
    "handle = @sin;\n"
    "end\n"
    "\n"
    "function result = applyHandle(handle, value)\n"
    "result = handle(value);\n"
    "end\n"
    "\n"
    "function [sumValue, difference] = pair(left, right)\n"
    "sumValue = left + right;\n"
    "difference = left - right;\n"
    "end\n";

static const char k_consumer_source[] =
    "function result = applyExternal(handle, value)\n"
    "result = handle(value);\n"
    "end\n"
    "\n"
    "function out = identity(value)\n"
    "out = value;\n"
    "end\n";

static const char k_object_source[] =
    "classdef Meter\n"
    "    properties\n"
    "        Value\n"
    "    end\n"
    "    methods\n"
    "        function obj = Meter(value)\n"
    "            obj.Value = value;\n"
    "        end\n"
    "        function result = scale(obj, factor)\n"
    "            result = obj.Value * factor;\n"
    "        end\n"
    "    end\n"
    "end\n"
    "if use_input\n"
    "    scaled = input_meter.scale(2);\n"
    "else\n"
    "    input_meter = Meter(4);\n"
    "    scaled = 0;\n"
    "end\n";

static const char k_object_consumer_source[] =
    "scaled = input_meter.Value;\n";

static const char k_inline_entry_source[] =
    "counter = InlineCounter(5);\n"
    "inline_value = counter.add(3);\n";

static const char k_inline_class_source[] =
    "classdef InlineCounter\n"
    "    properties\n"
    "        Value\n"
    "    end\n"
    "    methods\n"
    "        function obj = InlineCounter(value)\n"
    "            obj.Value = value;\n"
    "        end\n"
    "        function result = add(obj, amount)\n"
    "            result = obj.Value + amount;\n"
    "        end\n"
    "    end\n"
    "end\n";

typedef struct extended_invocation_options {
    mparser_invocation_options value;
    unsigned char future_tail[31];
} extended_invocation_options;

typedef struct extended_execution_summary {
    mparser_execution_summary value;
    unsigned char future_tail[29];
} extended_execution_summary;

typedef struct extended_source_load_options {
    mparser_source_load_options value;
    unsigned char future_tail[23];
} extended_source_load_options;

typedef struct extended_source_unit {
    mparser_source_unit value;
    unsigned char forbidden_tail[17];
} extended_source_unit;

static int bytes_equal(const unsigned char* bytes,
                       size_t size,
                       unsigned char expected) {
    size_t index;
    for (index = 0; index < size; ++index) {
        if (bytes[index] != expected) {
            return 0;
        }
    }
    return 1;
}

static int view_equals(mparser_utf8_view view, const char* expected) {
    const size_t size = strlen(expected);
    return view.size == size &&
           (size == 0 || memcmp(view.data, expected, size) == 0);
}

static int view_ends_with(mparser_utf8_view view, const char* suffix) {
    const size_t size = strlen(suffix);
    return view.size >= size &&
           memcmp(view.data + view.size - size, suffix, size) == 0;
}

static mparser_invocation_options options_for(const char* entry) {
    mparser_invocation_options options;
    const mparser_api_status status =
        MPARSER_INVOCATION_OPTIONS_INIT(&options);
    if (status != MPARSER_API_STATUS_OK) {
        memset(&options, 0, sizeof(options));
        return options;
    }
    options.entry_name = entry;
    options.entry_name_size = strlen(entry);
    options.has_requested_output_count = 1;
    options.requested_output_count = 1;
    return options;
}

static int output_scalar_at(const mparser_result* result,
                            size_t index,
                            double expected) {
    mparser_value* value = NULL;
    const double* data = NULL;
    size_t count = 0;
    CHECK(result != NULL);
    CHECK(mparser_result_succeeded(result) == 1);
    CHECK(index < mparser_result_output_count(result));
    CHECK(mparser_result_output(result, index, &value) ==
          MPARSER_API_STATUS_OK);
    CHECK(value != NULL);
    CHECK(mparser_value_get_kind(value) == MPARSER_VALUE_NUMERIC);
    CHECK(mparser_value_numeric_data(value, &data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 1);
    CHECK(fabs(data[0] - expected) < 1e-9);
    mparser_value_release(value);
    return 1;
}

static int output_scalar(const mparser_result* result, double expected) {
    CHECK(mparser_result_output_count(result) == 1);
    return output_scalar_at(result, 0, expected);
}

static mparser_api_status find_variable(const mparser_result* result,
                                        const char* expected_name,
                                        mparser_value** out_value) {
    size_t index;
    *out_value = NULL;
    for (index = 0;
         index < mparser_result_variable_count(result);
         ++index) {
        mparser_utf8_view name;
        mparser_value* value = NULL;
        const mparser_api_status status =
            mparser_result_variable(result, index, &name, &value);
        if (status != MPARSER_API_STATUS_OK) {
            return status;
        }
        if (view_equals(name, expected_name)) {
            *out_value = value;
            return MPARSER_API_STATUS_OK;
        }
        mparser_value_release(value);
    }
    return MPARSER_API_STATUS_OUT_OF_RANGE;
}

static int variable_is_scalar(const mparser_result* result,
                              const char* name,
                              double expected) {
    mparser_value* value = NULL;
    const double* data = NULL;
    size_t count = 0;
    CHECK(find_variable(result, name, &value) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_numeric_data(value, &data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 1);
    CHECK(fabs(data[0] - expected) < 1e-9);
    mparser_value_release(value);
    return 1;
}

static int compile_valid(const char* source,
                         const char* source_name,
                         mparser_module** out_module) {
    CHECK(mparser_module_compile_utf8(
              source, strlen(source),
              source_name, strlen(source_name),
              out_module) == MPARSER_API_STATUS_OK);
    CHECK(*out_module != NULL);
    CHECK(mparser_module_is_valid(*out_module) == 1);
    CHECK(mparser_module_diagnostic_count(*out_module) == 0);
    return 1;
}

static int run_header_and_diagnostic_smoke(void) {
    const char invalid_source[] = "function out = broken(\n";
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_module* invalid_module = NULL;
    mparser_value* missing = NULL;
    const mparser_diagnostic* diagnostic;
    mparser_source_position begin;
    mparser_api_status status;

    CHECK(mparser_c_abi_version() == MPARSER_C_ABI_VERSION);
    CHECK(mparser_c_abi_revision() == MPARSER_C_ABI_REVISION);
    CHECK(MPARSER_C_ABI_VERSION_MAJOR == 1u);
    CHECK(MPARSER_C_ABI_REVISION == 1u);
    CHECK(MPARSER_INVOCATION_OPTIONS_V1_SIZE == sizeof(options));
    CHECK(MPARSER_EXECUTION_SUMMARY_V1_SIZE == sizeof(summary));
    CHECK(MPARSER_SOURCE_UNIT_V1_SIZE == sizeof(mparser_source_unit));
    CHECK(MPARSER_SOURCE_LOAD_OPTIONS_V1_SIZE ==
          sizeof(mparser_source_load_options));
    CHECK(mparser_version_major() == 0);
    CHECK(mparser_version_minor() == 88);
    CHECK(mparser_version_patch() == 0);
    CHECK(view_equals(
        mparser_api_status_name(MPARSER_API_STATUS_OWNER_MISMATCH),
        "owner-mismatch"));
    CHECK(mparser_invocation_options_init(NULL) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_execution_summary_init(NULL) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_source_unit_init(NULL) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_source_load_options_init(NULL) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_invocation_options_init_sized(
              NULL, sizeof(options), MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_execution_summary_init_sized(
              NULL, sizeof(summary), MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_source_load_options_init_sized(
              NULL, sizeof(mparser_source_load_options),
              MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_invocation_options_init(&options) ==
          MPARSER_API_STATUS_OK);
    CHECK(options.struct_size == sizeof(options));
    CHECK(options.abi_version == MPARSER_C_ABI_VERSION);
    CHECK(options.backend == MPARSER_BACKEND_AUTOMATIC);
    CHECK(mparser_execution_summary_init(&summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.struct_size == sizeof(summary));
    CHECK(summary.abi_version == MPARSER_C_ABI_VERSION);
    CHECK(mparser_value_create_missing(&missing) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_get_kind(missing) == MPARSER_VALUE_MISSING);
    mparser_value_release(missing);

    status = mparser_module_compile_utf8(
        invalid_source, strlen(invalid_source),
        "invalid.m", strlen("invalid.m"), &invalid_module);
    CHECK(status == MPARSER_API_STATUS_COMPILATION_FAILED);
    CHECK(invalid_module != NULL);
    CHECK(mparser_module_is_valid(invalid_module) == 0);
    CHECK(mparser_module_diagnostic_count(invalid_module) > 0);
    diagnostic = mparser_module_diagnostic(invalid_module, 0);
    CHECK(diagnostic != NULL);
    CHECK(mparser_diagnostic_get_phase(diagnostic) ==
          MPARSER_DIAGNOSTIC_COMPILATION);
    CHECK(mparser_diagnostic_get_severity(diagnostic) ==
          MPARSER_DIAGNOSTIC_ERROR);
    CHECK(mparser_diagnostic_message(diagnostic).size > 0);
    CHECK(mparser_diagnostic_has_source(diagnostic) == 1);
    CHECK(view_equals(
        mparser_diagnostic_source_name(diagnostic), "invalid.m"));
    begin = mparser_diagnostic_source_begin(diagnostic);
    CHECK(begin.line >= 1);
    CHECK(mparser_module_diagnostic(invalid_module, 100) == NULL);
    mparser_module_release(invalid_module);
    return 1;
}

static int run_versioned_structure_smoke(
    mparser_module* module,
    const char* entry_path,
    const char* library_path) {
    static const char entry[] = "sumTo";
    static const char source[] = "value = 1;\n";
    static const char source_name[] = "extended_source.m";
    extended_invocation_options invocation;
    extended_execution_summary summary;
    extended_source_load_options load_options;
    extended_source_unit source_unit;
    mparser_utf8_view search_path;
    const mparser_value* arguments[1];
    mparser_value* argument = NULL;
    mparser_result* result = NULL;
    mparser_module* rejected_module = NULL;
    mparser_module* loaded_module = NULL;

    memset(&invocation, 0xa5, sizeof(invocation));
    CHECK(mparser_invocation_options_init_sized(
              &invocation,
              MPARSER_INVOCATION_OPTIONS_V1_SIZE - 1u,
              MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(bytes_equal(
        (const unsigned char*)&invocation,
        sizeof(invocation), 0xa5));
    CHECK(mparser_invocation_options_init_sized(
              &invocation, sizeof(invocation),
              MPARSER_C_ABI_VERSION + 1u) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(bytes_equal(
        (const unsigned char*)&invocation,
        sizeof(invocation), 0xa5));

    CHECK(mparser_invocation_options_init(&invocation.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(invocation.value.struct_size ==
          MPARSER_INVOCATION_OPTIONS_V1_SIZE);
    CHECK(bytes_equal(
        invocation.future_tail,
        sizeof(invocation.future_tail), 0xa5));

    memset(&invocation, 0xa5, sizeof(invocation));
    CHECK(mparser_invocation_options_init_sized(
              &invocation, sizeof(invocation),
              MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_OK);
    CHECK(invocation.value.struct_size == sizeof(invocation));
    CHECK(invocation.value.backend == MPARSER_BACKEND_AUTOMATIC);
    CHECK(bytes_equal(
        invocation.future_tail,
        sizeof(invocation.future_tail), 0));

    CHECK(mparser_value_create_scalar(
              3.0, MPARSER_NUMERIC_DOUBLE, &argument) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = argument;
    invocation.value.entry_name = entry;
    invocation.value.entry_name_size = sizeof(entry) - 1u;
    invocation.value.arguments = arguments;
    invocation.value.argument_count = 1;
    invocation.value.requested_output_count = 1;
    invocation.value.has_requested_output_count = 1;
    CHECK(mparser_module_execute(
              module, &invocation.value, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(output_scalar(result, 6.0));

    memset(&summary, 0xa5, sizeof(summary));
    CHECK(mparser_execution_summary_init_sized(
              &summary, sizeof(summary),
              MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.value.struct_size == sizeof(summary));
    CHECK(bytes_equal(
        summary.future_tail, sizeof(summary.future_tail), 0));
    CHECK(mparser_result_execution_summary(
              result, &summary.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.value.struct_size == sizeof(summary));
    CHECK(summary.value.executed_instruction_count > 0);
    CHECK(bytes_equal(
        summary.future_tail, sizeof(summary.future_tail), 0));

    memset(&summary, 0xa5, sizeof(summary));
    CHECK(mparser_execution_summary_init(&summary.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.value.struct_size ==
          MPARSER_EXECUTION_SUMMARY_V1_SIZE);
    CHECK(bytes_equal(
        summary.future_tail, sizeof(summary.future_tail), 0xa5));
    CHECK(mparser_result_execution_summary(
              result, &summary.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(bytes_equal(
        summary.future_tail, sizeof(summary.future_tail), 0xa5));

    memset(&load_options, 0xa5, sizeof(load_options));
    CHECK(mparser_source_load_options_init(&load_options.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(load_options.value.struct_size ==
          MPARSER_SOURCE_LOAD_OPTIONS_V1_SIZE);
    CHECK(bytes_equal(
        load_options.future_tail,
        sizeof(load_options.future_tail), 0xa5));
    memset(&load_options, 0xa5, sizeof(load_options));
    CHECK(mparser_source_load_options_init_sized(
              &load_options, sizeof(load_options),
              MPARSER_C_ABI_VERSION) ==
          MPARSER_API_STATUS_OK);
    CHECK(load_options.value.struct_size == sizeof(load_options));
    CHECK(bytes_equal(
        load_options.future_tail,
        sizeof(load_options.future_tail), 0));
    search_path.data = library_path;
    search_path.size = strlen(library_path);
    load_options.value.search_paths = &search_path;
    load_options.value.search_path_count = 1;
    CHECK(mparser_module_load_file_utf8(
              entry_path, strlen(entry_path),
              &load_options.value, &loaded_module) ==
          MPARSER_API_STATUS_OK);
    CHECK(loaded_module != NULL);
    CHECK(mparser_module_is_valid(loaded_module) == 1);
    mparser_module_release(loaded_module);

    memset(&source_unit, 0xa5, sizeof(source_unit));
    CHECK(mparser_source_unit_init(&source_unit.value) ==
          MPARSER_API_STATUS_OK);
    CHECK(source_unit.value.struct_size ==
          MPARSER_SOURCE_UNIT_V1_SIZE);
    CHECK(bytes_equal(
        source_unit.forbidden_tail,
        sizeof(source_unit.forbidden_tail), 0xa5));
    source_unit.value.struct_size =
        (uint32_t)sizeof(source_unit);
    source_unit.value.source_name = source_name;
    source_unit.value.source_name_size = sizeof(source_name) - 1u;
    source_unit.value.source = source;
    source_unit.value.source_size = sizeof(source) - 1u;
    CHECK(mparser_module_compile_sources(
              &source_unit.value, 1, &rejected_module) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(rejected_module == NULL);

    mparser_result_release(result);
    mparser_value_release(argument);
    return 1;
}

static int run_multi_source_compilation_smoke(void) {
    char entry_name[] = "inline_main.m";
    char class_name[] = "InlineCounter.m";
    char entry_source[sizeof(k_inline_entry_source)];
    char class_source[sizeof(k_inline_class_source)];
    mparser_source_unit sources[2];
    mparser_source_unit invalid_source;
    mparser_module* module = NULL;
    mparser_invocation_options options;
    mparser_result* result = NULL;
    const mparser_diagnostic* diagnostic;

    memcpy(entry_source, k_inline_entry_source, sizeof(entry_source));
    memcpy(class_source, k_inline_class_source, sizeof(class_source));
    CHECK(mparser_source_unit_init(&sources[0]) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_source_unit_init(&sources[1]) ==
          MPARSER_API_STATUS_OK);
    CHECK(sources[0].struct_size == sizeof(sources[0]));
    CHECK(sources[0].abi_version == MPARSER_C_ABI_VERSION);
    sources[0].source_name = entry_name;
    sources[0].source_name_size = strlen(entry_name);
    sources[0].source = entry_source;
    sources[0].source_size = strlen(entry_source);
    sources[1].source_name = class_name;
    sources[1].source_name_size = strlen(class_name);
    sources[1].source = class_source;
    sources[1].source_size = strlen(class_source);

    CHECK(mparser_module_compile_sources(
              sources, 2, &module) == MPARSER_API_STATUS_OK);
    CHECK(module != NULL);
    CHECK(mparser_module_is_valid(module) == 1);
    CHECK(mparser_module_source_count(module) == 2);
    CHECK(view_equals(
        mparser_module_source_name(module, 0), "inline_main.m"));
    CHECK(view_equals(
        mparser_module_source_name(module, 1), "InlineCounter.m"));
    CHECK(mparser_module_source_name(module, 2).size == 0);

    entry_name[0] = 'x';
    class_name[0] = 'x';
    entry_source[0] = '?';
    class_source[0] = '?';
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(result != NULL && mparser_result_succeeded(result) == 1);
    CHECK(variable_is_scalar(result, "inline_value", 8.0));
    mparser_result_release(result);
    mparser_module_release(module);
    result = NULL;
    module = NULL;

    CHECK(mparser_module_compile_sources(NULL, 0, &module) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(module == NULL);
    CHECK(mparser_source_unit_init(&invalid_source) ==
          MPARSER_API_STATUS_OK);
    invalid_source.abi_version += 1;
    CHECK(mparser_module_compile_sources(
              &invalid_source, 1, &module) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(module == NULL);

    CHECK(mparser_source_unit_init(&sources[0]) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_source_unit_init(&sources[1]) ==
          MPARSER_API_STATUS_OK);
    sources[0].source_name = "valid.m";
    sources[0].source_name_size = strlen("valid.m");
    sources[0].source = "value = 1;\n";
    sources[0].source_size = strlen("value = 1;\n");
    sources[1].source_name = "broken_inline.m";
    sources[1].source_name_size = strlen("broken_inline.m");
    sources[1].source = "classdef Broken\nproperties\nValue(\nend\nend\n";
    sources[1].source_size = strlen(sources[1].source);
    CHECK(mparser_module_compile_sources(
              sources, 2, &module) ==
          MPARSER_API_STATUS_COMPILATION_FAILED);
    CHECK(module != NULL);
    CHECK(mparser_module_is_valid(module) == 0);
    CHECK(mparser_module_source_count(module) == 2);
    CHECK(mparser_module_diagnostic_count(module) > 0);
    diagnostic = mparser_module_diagnostic(module, 0);
    CHECK(diagnostic != NULL);
    CHECK(view_equals(
        mparser_diagnostic_source_name(diagnostic),
        "broken_inline.m"));
    mparser_module_release(module);
    return 1;
}

static int run_file_source_graph_smoke(const char* entry_path,
                                       const char* library_path) {
    mparser_source_load_options load_options;
    mparser_utf8_view search_path;
    mparser_module* module = NULL;
    mparser_invocation_options options;
    mparser_result* result = NULL;
    const mparser_diagnostic* diagnostic;
    const char missing_path[] =
        "mparser_c_api_missing_entry_84.m";
    const char invalid_utf8_path[] = {
        'b', 'a', 'd', '_', (char)0xc0, (char)0xaf, '.', 'm'};

    CHECK(MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options) ==
          MPARSER_API_STATUS_OK);
    CHECK(load_options.struct_size == sizeof(load_options));
    CHECK(load_options.abi_version == MPARSER_C_ABI_VERSION);
    search_path.data = library_path;
    search_path.size = strlen(library_path);
    load_options.search_paths = &search_path;
    load_options.search_path_count = 1;
    CHECK(mparser_module_load_file_utf8(
              entry_path, strlen(entry_path),
              &load_options, &module) == MPARSER_API_STATUS_OK);
    CHECK(module != NULL);
    CHECK(mparser_module_is_valid(module) == 1);
    CHECK(mparser_module_source_count(module) == 7);
    CHECK(view_ends_with(
        mparser_module_source_name(module, 0), "run_demo.m"));
    CHECK(mparser_module_diagnostic_count(module) == 0);

    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(result != NULL && mparser_result_succeeded(result) == 1);
    CHECK(variable_is_scalar(result, "scaled", 21.0));
    CHECK(variable_is_scalar(result, "revealed", 107.0));
    CHECK(variable_is_scalar(result, "offset_value", 12.0));
    CHECK(variable_is_scalar(result, "static_value", 12.0));
    mparser_result_release(result);
    mparser_module_release(module);
    result = NULL;
    module = NULL;

    load_options.abi_version += 1;
    CHECK(mparser_module_load_file_utf8(
              entry_path, strlen(entry_path),
              &load_options, &module) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(module == NULL);
    CHECK(MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options) ==
          MPARSER_API_STATUS_OK);
    search_path.data = NULL;
    search_path.size = 1;
    load_options.search_paths = &search_path;
    load_options.search_path_count = 1;
    CHECK(mparser_module_load_file_utf8(
              entry_path, strlen(entry_path),
              &load_options, &module) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(module == NULL);
    CHECK(mparser_module_load_file_utf8(
              "", 0, NULL, &module) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(module == NULL);
    CHECK(mparser_module_load_file_utf8(
              invalid_utf8_path, sizeof(invalid_utf8_path),
              NULL, &module) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(module == NULL);
    CHECK(MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options) ==
          MPARSER_API_STATUS_OK);
    search_path.data = invalid_utf8_path;
    search_path.size = sizeof(invalid_utf8_path);
    load_options.search_paths = &search_path;
    load_options.search_path_count = 1;
    CHECK(mparser_module_load_file_utf8(
              entry_path, strlen(entry_path),
              &load_options, &module) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(module == NULL);

    CHECK(mparser_module_load_file_utf8(
              missing_path, strlen(missing_path),
              NULL, &module) ==
          MPARSER_API_STATUS_SOURCE_LOAD_FAILED);
    CHECK(view_equals(
        mparser_api_status_name(MPARSER_API_STATUS_SOURCE_LOAD_FAILED),
        "source-load-failed"));
    CHECK(module != NULL);
    CHECK(mparser_module_is_valid(module) == 0);
    CHECK(mparser_module_source_count(module) == 0);
    CHECK(mparser_module_diagnostic_count(module) == 1);
    diagnostic = mparser_module_diagnostic(module, 0);
    CHECK(diagnostic != NULL);
    CHECK(view_equals(
        mparser_diagnostic_identifier(diagnostic),
        "MParser:SourceLoadFailed"));
    CHECK(view_equals(
        mparser_diagnostic_source_name(diagnostic), missing_path));
    CHECK(mparser_diagnostic_source_begin(diagnostic).line == 1);
    mparser_module_release(module);
    return 1;
}

static int run_scalar_resource_and_session_smoke(
    mparser_module* module) {
    mparser_value* limit = NULL;
    const mparser_value* arguments[1];
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_cancel_token* cancellation = NULL;
    mparser_session* session = NULL;
    mparser_result* result = NULL;

    CHECK(mparser_value_create_scalar(
              100.0, MPARSER_NUMERIC_DOUBLE, &limit) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = limit;
    options = options_for("sumTo");
    options.arguments = arguments;
    options.argument_count = 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(output_scalar(result, 5050.0));
    memset(&summary, 0, sizeof(summary));
    CHECK(mparser_result_execution_summary(result, &summary) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(MPARSER_EXECUTION_SUMMARY_INIT(&summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_execution_summary(result, &summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.stop_reason == MPARSER_STOP_NONE);
    mparser_result_release(result);
    result = NULL;
    mparser_value_release(limit);

    {
        mparser_value* left = NULL;
        mparser_value* right = NULL;
        const mparser_value* pair_arguments[2];
        CHECK(mparser_value_create_scalar(
                  9.0, MPARSER_NUMERIC_DOUBLE, &left) ==
              MPARSER_API_STATUS_OK);
        CHECK(mparser_value_create_scalar(
                  4.0, MPARSER_NUMERIC_DOUBLE, &right) ==
              MPARSER_API_STATUS_OK);
        pair_arguments[0] = left;
        pair_arguments[1] = right;
        options = options_for("pair");
        options.arguments = pair_arguments;
        options.argument_count = 2;
        options.requested_output_count = 2;
        CHECK(mparser_module_execute(module, &options, &result) ==
              MPARSER_API_STATUS_OK);
        CHECK(mparser_result_requested_output_count(result) == 2);
        CHECK(mparser_result_output_count(result) == 2);
        CHECK(view_equals(
            mparser_result_output_name(result, 0), "sumValue"));
        CHECK(view_equals(
            mparser_result_output_name(result, 1), "difference"));
        CHECK(output_scalar_at(result, 0, 13.0));
        CHECK(output_scalar_at(result, 1, 5.0));
        mparser_result_release(result);
        result = NULL;
        mparser_value_release(left);
        mparser_value_release(right);
    }

    options = options_for("spin");
    options.max_instruction_count = 64;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) == 0);
    CHECK(MPARSER_EXECUTION_SUMMARY_INIT(&summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_execution_summary(result, &summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.stop_reason == MPARSER_STOP_INSTRUCTION_LIMIT);
    CHECK(summary.executed_instruction_count == 64);
    CHECK(summary.resource_controls_active == 1);
    CHECK(summary.optimized_execution_suppressed == 1);
    mparser_result_release(result);
    result = NULL;

    CHECK(mparser_cancel_token_create(&cancellation) ==
          MPARSER_API_STATUS_OK);
    mparser_cancel_token_retain(cancellation);
    mparser_cancel_token_release(cancellation);
    mparser_cancel_token_request(cancellation);
    CHECK(mparser_cancel_token_is_requested(cancellation) == 1);
    CHECK(mparser_value_create_scalar(
              7.0, MPARSER_NUMERIC_DOUBLE, &limit) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = limit;
    options = options_for("identity");
    options.arguments = arguments;
    options.argument_count = 1;
    options.cancellation_token = cancellation;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(MPARSER_EXECUTION_SUMMARY_INIT(&summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_execution_summary(result, &summary) ==
          MPARSER_API_STATUS_OK);
    CHECK(summary.stop_reason == MPARSER_STOP_CANCELLED);
    mparser_result_release(result);
    result = NULL;
    mparser_value_release(limit);
    mparser_cancel_token_release(cancellation);

    CHECK(mparser_module_create_session(module, &session) ==
          MPARSER_API_STATUS_OK);
    mparser_session_retain(session);
    mparser_session_release(session);
    options = options_for("spin");
    options.max_instruction_count = 64;
    CHECK(mparser_session_execute(session, &options, &result) ==
          MPARSER_API_STATUS_OK);
    mparser_result_release(result);
    result = NULL;
    CHECK(mparser_value_create_scalar(
              42.0, MPARSER_NUMERIC_DOUBLE, &limit) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = limit;
    options = options_for("identity");
    options.arguments = arguments;
    options.argument_count = 1;
    CHECK(mparser_session_execute(session, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(output_scalar(result, 42.0));
    mparser_result_release(result);
    result = NULL;
    CHECK(mparser_session_clear_global(
              session, "unknown", strlen("unknown")) ==
          MPARSER_API_STATUS_OUT_OF_RANGE);
    CHECK(mparser_session_clear_globals(session) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_session_reset(session) ==
          MPARSER_API_STATUS_OK);
    mparser_value_release(limit);
    mparser_session_release(session);
    return 1;
}

static int run_array_text_and_composite_smoke(
    mparser_module* module) {
    const size_t matrix_dimensions[2] = {2, 3};
    const double matrix_data[6] = {1, 2, 3, 4, 5, 6};
    const uint16_t character_data[6] = {
        'A', 'B', 'C', 'D', 'E', 'F'};
    const uint16_t first_string[2] = {'h', 'i'};
    const mparser_utf16_view strings[2] = {
        {first_string, 2, 0},
        {NULL, 0, 1}};
    const size_t string_dimensions[2] = {1, 2};
    const size_t cell_dimensions[2] = {2, 1};
    mparser_value* matrix = NULL;
    mparser_value* characters = NULL;
    mparser_value* string_array = NULL;
    mparser_value* scalar = NULL;
    mparser_value* cell = NULL;
    mparser_value* structure = NULL;
    mparser_value* output = NULL;
    mparser_value* child = NULL;
    const mparser_value* arguments[1];
    const mparser_value* cell_elements[2];
    mparser_named_value fields[2];
    mparser_invocation_options options;
    mparser_result* result = NULL;
    const double* numeric_data = NULL;
    const uint16_t* text_data = NULL;
    size_t count = 0;
    size_t dimension = 0;
    size_t index;
    size_t answer_field = (size_t)-1;
    mparser_utf16_view string_element;

    CHECK(mparser_value_create_numeric_array(
              MPARSER_NUMERIC_DOUBLE,
              matrix_dimensions, 2,
              matrix_data, 6, &matrix) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = matrix;
    options = options_for("identity");
    options.arguments = arguments;
    options.argument_count = 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &output) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_rank(output) == 2);
    CHECK(mparser_value_dimension(output, 0, &dimension) ==
          MPARSER_API_STATUS_OK);
    CHECK(dimension == 2);
    CHECK(mparser_value_dimension(output, 1, &dimension) ==
          MPARSER_API_STATUS_OK);
    CHECK(dimension == 3);
    CHECK(mparser_value_numeric_data(
              output, &numeric_data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 6);
    for (index = 0; index < count; ++index) {
        CHECK(numeric_data[index] == matrix_data[index]);
    }
    mparser_value_release(output);
    output = NULL;
    mparser_result_release(result);
    result = NULL;

    CHECK(mparser_value_create_character_array(
              matrix_dimensions, 2,
              character_data, 6, &characters) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = characters;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &output) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_character_data(
              output, &text_data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 6);
    for (index = 0; index < count; ++index) {
        CHECK(text_data[index] == character_data[index]);
    }
    mparser_value_release(output);
    output = NULL;
    mparser_result_release(result);
    result = NULL;

    CHECK(mparser_value_create_string_array(
              string_dimensions, 2, strings, 2,
              &string_array) == MPARSER_API_STATUS_OK);
    arguments[0] = string_array;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &output) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_string_element(
              output, 0, &string_element) ==
          MPARSER_API_STATUS_OK);
    CHECK(string_element.missing == 0);
    CHECK(string_element.size == 2);
    CHECK(string_element.data[0] == 'h');
    CHECK(string_element.data[1] == 'i');
    CHECK(mparser_value_string_element(
              output, 1, &string_element) ==
          MPARSER_API_STATUS_OK);
    CHECK(string_element.missing == 1);
    mparser_value_release(output);
    output = NULL;
    mparser_result_release(result);
    result = NULL;

    CHECK(mparser_value_create_scalar(
              9.0, MPARSER_NUMERIC_DOUBLE, &scalar) ==
          MPARSER_API_STATUS_OK);
    cell_elements[0] = scalar;
    cell_elements[1] = characters;
    CHECK(mparser_value_create_cell(
              cell_dimensions, 2, cell_elements, 2, &cell) ==
          MPARSER_API_STATUS_OK);
    mparser_value_release(characters);
    characters = NULL;
    arguments[0] = cell;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &output) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_get_kind(output) == MPARSER_VALUE_CELL);
    CHECK(mparser_value_cell_element(output, 0, &child) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_numeric_data(
              child, &numeric_data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 1 && numeric_data[0] == 9.0);
    mparser_value_release(child);
    child = NULL;
    mparser_value_release(output);
    output = NULL;
    mparser_result_release(result);
    result = NULL;

    fields[0].name = "payload";
    fields[0].name_size = strlen("payload");
    fields[0].value = cell;
    fields[1].name = "answer";
    fields[1].name_size = strlen("answer");
    fields[1].value = scalar;
    CHECK(mparser_value_create_struct(fields, 2, &structure) ==
          MPARSER_API_STATUS_OK);
    mparser_value_release(scalar);
    scalar = NULL;
    mparser_value_release(cell);
    cell = NULL;
    arguments[0] = structure;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &output) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_struct_field_count(output) == 2);
    CHECK(view_equals(
        mparser_value_struct_field_name(output, 0), "payload"));
    CHECK(view_equals(
        mparser_value_struct_field_name(output, 1), "answer"));
    for (index = 0;
         index < mparser_value_struct_field_count(output);
         ++index) {
        if (view_equals(
                mparser_value_struct_field_name(output, index),
                "answer")) {
            answer_field = index;
        }
    }
    CHECK(answer_field != (size_t)-1);
    CHECK(mparser_value_struct_field(
              output, 0, answer_field, &child) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_numeric_data(
              child, &numeric_data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 1 && numeric_data[0] == 9.0);
    mparser_value_release(child);
    mparser_value_release(output);
    mparser_result_release(result);

    mparser_value_release(matrix);
    mparser_value_release(string_array);
    mparser_value_release(structure);
    return 1;
}

static int run_function_handle_ownership_smoke(
    mparser_module* module) {
    mparser_module* consumer = NULL;
    mparser_value* factor = NULL;
    mparser_value* argument = NULL;
    mparser_value* closure = NULL;
    mparser_value* builtin = NULL;
    mparser_value* bound_cell = NULL;
    const mparser_value* arguments[2];
    const mparser_value* cell_elements[1];
    const size_t cell_dimensions[2] = {1, 1};
    mparser_invocation_options options;
    mparser_result* result = NULL;

    CHECK(compile_valid(
        k_consumer_source, "consumer.m", &consumer));
    CHECK(mparser_value_create_scalar(
              4.0, MPARSER_NUMERIC_DOUBLE, &factor) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = factor;
    options = options_for("makeClosure");
    options.arguments = arguments;
    options.argument_count = 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &closure) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_get_kind(closure) ==
          MPARSER_VALUE_FUNCTION_HANDLE);
    CHECK(mparser_value_is_module_bound(closure) == 1);
    CHECK(mparser_value_function_text(closure).size > 0);
    mparser_result_release(result);
    result = NULL;
    mparser_value_release(factor);

    CHECK(mparser_value_create_scalar(
              5.0, MPARSER_NUMERIC_DOUBLE, &argument) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = closure;
    arguments[1] = argument;
    options = options_for("applyHandle");
    options.arguments = arguments;
    options.argument_count = 2;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(output_scalar(result, 20.0));
    mparser_result_release(result);
    result = NULL;

    options = options_for("applyExternal");
    options.arguments = arguments;
    options.argument_count = 2;
    CHECK(mparser_module_execute(consumer, &options, &result) ==
          MPARSER_API_STATUS_OWNER_MISMATCH);
    CHECK(result == NULL);

    cell_elements[0] = closure;
    CHECK(mparser_value_create_cell(
              cell_dimensions, 2, cell_elements, 1,
              &bound_cell) == MPARSER_API_STATUS_OK);
    CHECK(mparser_value_is_module_bound(bound_cell) == 1);
    arguments[0] = bound_cell;
    options = options_for("identity");
    options.arguments = arguments;
    options.argument_count = 1;
    CHECK(mparser_module_execute(consumer, &options, &result) ==
          MPARSER_API_STATUS_OWNER_MISMATCH);
    CHECK(result == NULL);
    mparser_value_release(bound_cell);
    mparser_value_release(closure);

    options = options_for("makeBuiltin");
    options.arguments = NULL;
    options.argument_count = 0;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_output(result, 0, &builtin) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_get_kind(builtin) ==
          MPARSER_VALUE_FUNCTION_HANDLE);
    CHECK(mparser_value_is_module_bound(builtin) == 0);
    mparser_result_release(result);
    result = NULL;

    mparser_value_release(argument);
    CHECK(mparser_value_create_scalar(
              0.0, MPARSER_NUMERIC_DOUBLE, &argument) ==
          MPARSER_API_STATUS_OK);
    arguments[0] = builtin;
    arguments[1] = argument;
    options = options_for("applyExternal");
    options.arguments = arguments;
    options.argument_count = 2;
    CHECK(mparser_module_execute(consumer, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(output_scalar(result, 0.0));
    mparser_result_release(result);
    mparser_value_release(argument);
    mparser_value_release(builtin);
    mparser_module_release(consumer);
    return 1;
}

static int run_object_transport_smoke(void) {
    mparser_module* module = NULL;
    mparser_module* consumer = NULL;
    mparser_value* use_input = NULL;
    mparser_value* object = NULL;
    mparser_value* scaled = NULL;
    mparser_named_value workspace[2];
    mparser_invocation_options options;
    mparser_result* result = NULL;
    const double* numeric_data = NULL;
    size_t count = 0;

    CHECK(compile_valid(
        k_object_source, "Meter.m", &module));
    CHECK(compile_valid(
        k_object_consumer_source,
        "object_consumer.m", &consumer));

    CHECK(mparser_value_create_scalar(
              0.0, MPARSER_NUMERIC_LOGICAL, &use_input) ==
          MPARSER_API_STATUS_OK);
    workspace[0].name = "use_input";
    workspace[0].name_size = strlen("use_input");
    workspace[0].value = use_input;
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
          MPARSER_API_STATUS_OK);
    options.initial_workspace = workspace;
    options.initial_workspace_count = 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) == 1);
    CHECK(find_variable(result, "input_meter", &object) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_get_kind(object) == MPARSER_VALUE_OBJECT);
    CHECK(mparser_value_is_module_bound(object) == 1);
    CHECK(view_equals(mparser_value_class_name(object), "Meter"));
    mparser_result_release(result);
    result = NULL;
    mparser_value_release(use_input);

    CHECK(mparser_value_create_scalar(
              1.0, MPARSER_NUMERIC_LOGICAL, &use_input) ==
          MPARSER_API_STATUS_OK);
    workspace[0].value = use_input;
    workspace[1].name = "input_meter";
    workspace[1].name_size = strlen("input_meter");
    workspace[1].value = object;
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
          MPARSER_API_STATUS_OK);
    options.initial_workspace = workspace;
    options.initial_workspace_count = 2;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(find_variable(result, "scaled", &scaled) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_value_numeric_data(
              scaled, &numeric_data, &count) ==
          MPARSER_API_STATUS_OK);
    CHECK(count == 1 && numeric_data[0] == 8.0);
    mparser_value_release(scaled);
    scaled = NULL;
    mparser_result_release(result);
    result = NULL;

    CHECK(mparser_module_execute(consumer, &options, &result) ==
          MPARSER_API_STATUS_OWNER_MISMATCH);
    CHECK(result == NULL);

    mparser_value_release(use_input);
    mparser_value_release(object);
    mparser_module_release(consumer);
    mparser_module_release(module);
    return 1;
}

static int run_request_validation_smoke(mparser_module* module) {
    mparser_invocation_options options = options_for("spin");
    mparser_result* result = NULL;
    options.abi_version += 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(result == NULL);
    options = options_for("spin");
    options.struct_size = 1;
    CHECK(mparser_module_execute(module, &options, &result) ==
          MPARSER_API_STATUS_ABI_MISMATCH);
    CHECK(result == NULL);
    return 1;
}

static int run_module_metadata_smoke(mparser_module* module) {
    CHECK(mparser_module_function_count(module) == 7);
    CHECK(mparser_module_function_name(module, 0).size > 0);
    mparser_module_retain(module);
    mparser_module_release(module);
    return 1;
}

int main(int argc, char** argv) {
    mparser_module* module = NULL;

    if (argc != 3) {
        fprintf(stderr,
                "usage: c_api_smoke <entry.m> <search-path>\n");
        return 1;
    }
    if (!run_header_and_diagnostic_smoke()) {
        return 1;
    }
    if (!compile_valid(
            k_module_source, "c_api_smoke.m", &module)) {
        return 1;
    }

    if (!run_multi_source_compilation_smoke() ||
        !run_file_source_graph_smoke(argv[1], argv[2]) ||
        !run_module_metadata_smoke(module) ||
        !run_versioned_structure_smoke(module, argv[1], argv[2]) ||
        !run_scalar_resource_and_session_smoke(module) ||
        !run_array_text_and_composite_smoke(module) ||
        !run_function_handle_ownership_smoke(module) ||
        !run_object_transport_smoke() ||
        !run_request_validation_smoke(module)) {
        mparser_module_release(module);
        return 1;
    }

    mparser_module_release(module);
    puts("c api smoke tests passed");
    return 0;
}
