cmake_minimum_required(VERSION 3.20)

project(MParser VERSION 1.2.0 LANGUAGES C CXX)

set(MPARSER_V1_PERFORMANCE_EVIDENCE_VERSION "0.90.0")
set(MPARSER_V1_RELEASE_VERSION "1.0.0")
set(MPARSER_V1_PUBLIC_CONTRACT_ENGINE_VERSION "1.1.0")
set(MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION "1.0.0")
set(MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION
    "d8075575403bf30828a928a83bbbbfb9706ba902")
set(MPARSER_V1_AUTHENTICATION_EVIDENCE_RUN_ID "30780391460")
set(MPARSER_V1_AUTHENTICATION_EVIDENCE_DIRECTORY
    "v${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}-authentication")
set(MPARSER_V1_AUTHENTICATION_EVIDENCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/release-evidence/${MPARSER_V1_AUTHENTICATION_EVIDENCE_DIRECTORY}")
set(MPARSER_V1_PUBLICATION_EVIDENCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/release-evidence/v1.0.0-publication")
set(MPARSER_V1_RELEASE_ID "RE_kwDOTV8DlM4Vs2eC")
set(MPARSER_V1_RELEASE_PUBLISHED_AT "2026-08-03T08:59:01Z")
set(MPARSER_V1_RELEASE_CHECKSUM_SHA256
    "3e70530ef873050d7c8a798dcf133ecf0b6e99826a02f65598b530c61284ae71")

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
find_package(Threads REQUIRED)

set(MPARSER_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/MParser"
    CACHE STRING "Install directory for the MParser CMake package")
set(MPARSER_C_ABI_GENERATION 2)
set(MPARSER_C_ABI_REVISION 0)
set(MPARSER_C_LIBRARY_VERSION "${PROJECT_VERSION}")
set(MPARSER_C_ABI_SYMBOL_LIST
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api_generation2_symbols.txt")
set(MPARSER_PROJECT_LICENSE "Apache-2.0")
set(MPARSER_PROJECT_COPYRIGHT "Copyright 2026 Wang Xin")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MPARSER_ENABLE_NATIVE_JIT
    "Build the optional SLJIT native scalar-loop backend" ON)
option(MPARSER_ENABLE_RELEASE_PACKAGING
    "Enable reproducible-payload release archives and package validation" OFF)
option(MPARSER_ENABLE_MSVC_ASAN
    "Instrument a Windows MSVC no-JIT validation build with AddressSanitizer"
    OFF)
option(MPARSER_WARNINGS_AS_ERRORS
    "Treat first-party compiler warnings as build errors" OFF)
set(MPARSER_RELEASE_SOURCE_DATE_EPOCH "1767225600" CACHE STRING
    "UTC epoch used to normalize release archive metadata")
set(MPARSER_SLJIT_SOURCE_DIR "" CACHE PATH
    "SLJIT source override; empty uses third_party/sljit")
set(MPARSER_PERFORMANCE_EVIDENCE_REVISION "working-tree" CACHE STRING
    "Source revision recorded by the native performance evidence target")
set(MPARSER_PERFORMANCE_EVIDENCE_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/performance-evidence" CACHE PATH
    "Output directory for native performance evidence reports")

function(mparser_enable_project_warnings target)
    if(MSVC)
        set(warning_options
            /W4
            "$<$<COMPILE_LANGUAGE:CXX>:/permissive->")
        if(MPARSER_WARNINGS_AS_ERRORS)
            list(APPEND warning_options /WX)
        endif()
    else()
        set(warning_options -Wall -Wextra -Wpedantic)
        if(MPARSER_WARNINGS_AS_ERRORS)
            list(APPEND warning_options -Werror)
        endif()
    endif()
    target_compile_options(${target} PRIVATE ${warning_options})
endfunction()

if(MPARSER_ENABLE_MSVC_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR
            "MPARSER_ENABLE_MSVC_ASAN requires the MSVC toolchain")
    endif()
    if(MPARSER_ENABLE_NATIVE_JIT)
        message(FATAL_ERROR
            "MPARSER_ENABLE_MSVC_ASAN is a no-JIT validation mode; "
            "set MPARSER_ENABLE_NATIVE_JIT=OFF")
    endif()
    if(MPARSER_ENABLE_RELEASE_PACKAGING)
        message(FATAL_ERROR
            "AddressSanitizer validation builds are not release packages")
    endif()

    get_filename_component(
        _mparser_msvc_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _mparser_msvc_asan_runtime_candidates
        LIST_DIRECTORIES FALSE
        "${_mparser_msvc_compiler_dir}/clang_rt.asan_dynamic-*.dll")
    list(LENGTH _mparser_msvc_asan_runtime_candidates
        _mparser_msvc_asan_runtime_count)
    if(NOT _mparser_msvc_asan_runtime_count EQUAL 1)
        message(FATAL_ERROR
            "Expected exactly one MSVC AddressSanitizer runtime beside "
            "${CMAKE_CXX_COMPILER}; found "
            "${_mparser_msvc_asan_runtime_count}: "
            "${_mparser_msvc_asan_runtime_candidates}")
    endif()
    list(GET _mparser_msvc_asan_runtime_candidates 0
        MPARSER_MSVC_ASAN_RUNTIME_DLL)
    get_filename_component(
        MPARSER_MSVC_ASAN_RUNTIME_NAME
        "${MPARSER_MSVC_ASAN_RUNTIME_DLL}" NAME)

    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/fsanitize=address /INCREMENTAL:NO /DEBUG)
    message(STATUS
        "MParser MSVC AddressSanitizer runtime: "
        "${MPARSER_MSVC_ASAN_RUNTIME_DLL}")
endif()

if(MPARSER_ENABLE_RELEASE_PACKAGING)
    find_package(Git REQUIRED)
    set(MPARSER_RELEASE_SOURCE_REPOSITORY
        "https://github.com/minexin/MParser")
    set(MPARSER_RELEASE_BUILD_TYPE_URI
        "${MPARSER_RELEASE_SOURCE_REPOSITORY}/blob/main/docs/release-process.md#cmake-cpack-release-v1")
    set(MPARSER_RELEASE_BUILDER_ID
        "${MPARSER_RELEASE_SOURCE_REPOSITORY}/blob/main/docs/release-process.md#local-cmake-builder-v1")
    string(TOLOWER
        "${CMAKE_SYSTEM_PROCESSOR}" _mparser_release_processor)
    if(_mparser_release_processor MATCHES
       "^(amd64|x86_64|x64)$")
        set(MPARSER_RELEASE_ARCHITECTURE "x86_64")
    elseif(_mparser_release_processor STREQUAL "aarch64")
        set(MPARSER_RELEASE_ARCHITECTURE "aarch64")
    elseif(_mparser_release_processor STREQUAL "arm64")
        set(MPARSER_RELEASE_ARCHITECTURE "arm64")
    else()
        message(FATAL_ERROR
            "Release packaging does not recognize architecture: "
            "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(WIN32)
        set(MPARSER_RELEASE_PLATFORM "windows")
        set(MPARSER_RELEASE_PACKAGE_EXTENSION ".zip")
        set(MPARSER_RELEASE_ARCHIVE_MEDIA_TYPE "application/zip")
    elseif(APPLE)
        set(MPARSER_RELEASE_PLATFORM "macos")
        set(MPARSER_RELEASE_PACKAGE_EXTENSION ".tar.gz")
        set(MPARSER_RELEASE_ARCHIVE_MEDIA_TYPE "application/gzip")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(MPARSER_RELEASE_PLATFORM "linux")
        set(MPARSER_RELEASE_PACKAGE_EXTENSION ".tar.gz")
        set(MPARSER_RELEASE_ARCHIVE_MEDIA_TYPE "application/gzip")
    else()
        message(FATAL_ERROR
            "Release packaging does not recognize platform: "
            "${CMAKE_SYSTEM_NAME}")
    endif()
    set(MPARSER_RELEASE_PACKAGE_BASENAME
        "mparser-${PROJECT_VERSION}-${MPARSER_RELEASE_PLATFORM}-${MPARSER_RELEASE_ARCHITECTURE}")
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

if(MPARSER_ENABLE_NATIVE_JIT)
    if(MPARSER_SLJIT_SOURCE_DIR)
        set(_mparser_sljit_source_dir
            "${MPARSER_SLJIT_SOURCE_DIR}")
        message(STATUS
            "MParser native JIT: external SLJIT at "
            "${_mparser_sljit_source_dir}")
    else()
        set(_mparser_sljit_source_dir
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sljit")
        message(STATUS
            "MParser native JIT: bundled SLJIT revision "
            "3907e69005ba6e30b225000f24aaef3632f88347")
    endif()

    if(NOT EXISTS
       "${_mparser_sljit_source_dir}/sljit_src/sljitLir.c")
        message(FATAL_ERROR
            "SLJIT source tree is incomplete: "
            "${_mparser_sljit_source_dir}; expected sljit_src/sljitLir.c")
    endif()

    add_library(mparser_sljit STATIC
        "${_mparser_sljit_source_dir}/sljit_src/sljitLir.c")
    set_target_properties(mparser_sljit PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
    target_include_directories(mparser_sljit SYSTEM PUBLIC
        "${_mparser_sljit_source_dir}/sljit_src")
    if(MSVC)
        target_compile_options(mparser_sljit PRIVATE /W0)
    else()
        target_compile_options(mparser_sljit PRIVATE -w)
    endif()
endif()

add_library(mparser_core STATIC
    src/mparser/adaptive_module_runtime.cpp
    src/mparser/adaptive_bytecode_vm.cpp
    src/mparser/argument_contract.cpp
    src/mparser/builtin_registry.cpp
    src/mparser/bytecode.cpp
    src/mparser/bytecode_dump.cpp
    src/mparser/bytecode_region.cpp
    src/mparser/bytecode_verifier.cpp
    src/mparser/bytecode_vm.cpp
    src/mparser/compiled_module.cpp
    src/mparser/filesystem_utf8.cpp
    src/mparser/function_signature.cpp
    src/mparser/interpreter.cpp
    src/mparser/lexer.cpp
    src/mparser/machine_protocol.cpp
    src/mparser/module_execution.cpp
    src/mparser/native_scalar_jit.cpp
    src/mparser/optimization_plan.cpp
    src/mparser/parser.cpp
    src/mparser/runtime_array_ops.cpp
    src/mparser/runtime_argument_validation.cpp
    src/mparser/runtime_assignment.cpp
    src/mparser/runtime_benchmark.cpp
    src/mparser/runtime_call_frame.cpp
    src/mparser/runtime_cell.cpp
    src/mparser/runtime_exception.cpp
    src/mparser/runtime_execution_control.cpp
    src/mparser/runtime_fallback.cpp
    src/mparser/runtime_warning.cpp
    src/mparser/runtime_index.cpp
    src/mparser/runtime_lvalue.cpp
    src/mparser/runtime_math.cpp
    src/mparser/runtime_metadata.cpp
    src/mparser/runtime_numeric.cpp
    src/mparser/runtime_object.cpp
    src/mparser/runtime_output.cpp
    src/mparser/runtime_range.cpp
    src/mparser/runtime_reduction.cpp
    src/mparser/runtime_scan.cpp
    src/mparser/runtime_session_state.cpp
    src/mparser/runtime_shape.cpp
    src/mparser/runtime_struct.cpp
    src/mparser/runtime_text.cpp
    src/mparser/runtime_value.cpp
    src/mparser/runtime_value_ops.cpp
    src/mparser/semantic.cpp
    src/mparser/semantic_dump.cpp
    src/mparser/source.cpp
    src/mparser/source_loader.cpp
    src/mparser/syntax.cpp
    src/mparser/syntax_dump.cpp
    src/mparser/typed_ir.cpp
    src/mparser/typed_region_executor.cpp
    src/mparser/token.cpp
)

set_target_properties(mparser_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(mparser_core PUBLIC src)
target_link_libraries(mparser_core PRIVATE Threads::Threads)

if(MPARSER_ENABLE_NATIVE_JIT)
    target_link_libraries(mparser_core PRIVATE mparser_sljit)
    target_compile_definitions(mparser_core PRIVATE MPARSER_HAS_SLJIT=1)
endif()

add_library(mparser_c_api SHARED
    src/mparser/c_api.cpp)
target_include_directories(mparser_c_api
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mparser_c_api PRIVATE mparser_core)
target_compile_definitions(mparser_c_api
    PRIVATE
        MPARSER_C_API_BUILD=1
        MPARSER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        MPARSER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        MPARSER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        MPARSER_C_ABI_GENERATION_EXPECTED=${MPARSER_C_ABI_GENERATION}
        MPARSER_C_ABI_REVISION_EXPECTED=${MPARSER_C_ABI_REVISION}
    INTERFACE
        MPARSER_C_API_SHARED=1)
set_target_properties(mparser_c_api PROPERTIES
    EXPORT_NAME c_api
    OUTPUT_NAME mparser_c
    VERSION ${MPARSER_C_LIBRARY_VERSION}
    SOVERSION ${MPARSER_C_ABI_GENERATION}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
if(UNIX)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MPARSER_C_ABI_SYMBOL_LIST}")
    file(STRINGS "${MPARSER_C_ABI_SYMBOL_LIST}"
        _mparser_c_abi_symbols
        REGEX "^mparser_[A-Za-z0-9_]+$")
    list(REMOVE_DUPLICATES _mparser_c_abi_symbols)
    list(LENGTH _mparser_c_abi_symbols
        _mparser_c_abi_symbol_count)
    if(NOT _mparser_c_abi_symbol_count EQUAL 109)
        message(FATAL_ERROR
            "C ABI symbol manifest must contain exactly 109 symbols")
    endif()

    if(APPLE)
        set(_mparser_c_api_export_file
            "${CMAKE_CURRENT_BINARY_DIR}/mparser_c_api.exports")
        file(WRITE "${_mparser_c_api_export_file}" "")
        foreach(_mparser_c_abi_symbol IN LISTS
                _mparser_c_abi_symbols)
            file(APPEND "${_mparser_c_api_export_file}"
                "_${_mparser_c_abi_symbol}\n")
        endforeach()
        target_link_options(mparser_c_api PRIVATE
            "-Wl,-exported_symbols_list,${_mparser_c_api_export_file}")
    else()
        set(_mparser_c_api_export_file
            "${CMAKE_CURRENT_BINARY_DIR}/mparser_c_api.map")
        file(WRITE "${_mparser_c_api_export_file}"
            "{\n  global:\n")
        foreach(_mparser_c_abi_symbol IN LISTS
                _mparser_c_abi_symbols)
            file(APPEND "${_mparser_c_api_export_file}"
                "    ${_mparser_c_abi_symbol};\n")
        endforeach()
        file(APPEND "${_mparser_c_api_export_file}"
            "  local:\n    *;\n};\n")
        target_link_options(mparser_c_api PRIVATE
            "-Wl,--version-script=${_mparser_c_api_export_file}")
    endif()
    set_property(TARGET mparser_c_api APPEND PROPERTY LINK_DEPENDS
        "${_mparser_c_api_export_file}")
endif()

add_library(mparser_cpp_api INTERFACE)
target_include_directories(mparser_cpp_api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mparser_cpp_api INTERFACE mparser_c_api)
target_compile_features(mparser_cpp_api INTERFACE cxx_std_20)
set_target_properties(mparser_cpp_api PROPERTIES
    EXPORT_NAME cpp_api)

add_executable(mparser src/main.cpp)
target_link_libraries(mparser PRIVATE mparser_core)
target_compile_definitions(mparser PRIVATE MPARSER_VERSION="${PROJECT_VERSION}")
set_target_properties(mparser PROPERTIES EXPORT_NAME cli)
if(WIN32)
    target_link_libraries(mparser PRIVATE Wer)
endif()
if(MPARSER_ENABLE_MSVC_ASAN)
    add_custom_target(mparser_msvc_asan_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MPARSER_MSVC_ASAN_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:mparser>"
        DEPENDS mparser
        COMMENT
            "Staging ${MPARSER_MSVC_ASAN_RUNTIME_NAME} for local validation"
        VERBATIM)
endif()

add_library(MParser::c_api ALIAS mparser_c_api)
add_library(MParser::cpp_api ALIAS mparser_cpp_api)
add_executable(MParser::cli ALIAS mparser)

include(CTest)
if(BUILD_TESTING)

if(MPARSER_ENABLE_MSVC_ASAN)
    add_test(NAME msvc_asan_runtime_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMPARSER_ASAN_RUNTIME_SOURCE=${MPARSER_MSVC_ASAN_RUNTIME_DLL}"
            "-DMPARSER_RUNTIME_DIR=$<TARGET_FILE_DIR:mparser>"
            "-DMPARSER_EXECUTABLE=$<TARGET_FILE:mparser>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_msvc_asan_runtime.cmake")
    set_tests_properties(msvc_asan_runtime_smoke PROPERTIES
        LABELS "reliability;sanitizer")
endif()

add_subdirectory(third_party/json-schema-validator)

add_executable(mparser_performance_baseline
    tools/performance_baseline.cpp
    tools/performance_environment.cpp)
target_link_libraries(mparser_performance_baseline PRIVATE
    mparser_core)
target_include_directories(mparser_performance_baseline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include)
target_compile_definitions(mparser_performance_baseline PRIVATE
    MPARSER_BASELINE_PROJECT_VERSION="${PROJECT_VERSION}"
    MPARSER_BASELINE_BUILD_TYPE="$<CONFIG>"
    MPARSER_BASELINE_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    MPARSER_BASELINE_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    MPARSER_BASELINE_SYSTEM_NAME="${CMAKE_SYSTEM_NAME}"
    MPARSER_BASELINE_SYSTEM_VERSION="${CMAKE_SYSTEM_VERSION}")
if(WIN32)
    target_link_libraries(mparser_performance_baseline PRIVATE
        Psapi)
endif()

add_executable(mparser_smoke_tests tests/parser_smoke.cpp)
target_link_libraries(mparser_smoke_tests PRIVATE mparser_core)
add_test(NAME parser_smoke COMMAND mparser_smoke_tests)

add_executable(semantic_smoke_tests tests/semantic_smoke.cpp)
target_link_libraries(semantic_smoke_tests PRIVATE mparser_core)
add_test(NAME semantic_smoke COMMAND semantic_smoke_tests)

add_executable(parser_semantic_fuzz_tests
    tests/parser_semantic_fuzz.cpp)
target_link_libraries(parser_semantic_fuzz_tests PRIVATE mparser_core)
add_test(NAME parser_semantic_fuzz
    COMMAND parser_semantic_fuzz_tests)
set_tests_properties(parser_semantic_fuzz PROPERTIES
    LABELS reliability
    TIMEOUT 60)

add_executable(bytecode_smoke_tests tests/bytecode_smoke.cpp)
target_link_libraries(bytecode_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_smoke COMMAND bytecode_smoke_tests)

add_executable(bytecode_verifier_smoke_tests
    tests/bytecode_verifier_smoke.cpp)
target_link_libraries(bytecode_verifier_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_verifier_smoke
    COMMAND bytecode_verifier_smoke_tests)
set_tests_properties(bytecode_verifier_smoke PROPERTIES
    LABELS reliability
    TIMEOUT 60)

add_executable(bytecode_vm_smoke_tests tests/bytecode_vm_smoke.cpp)
target_link_libraries(bytecode_vm_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_vm_smoke COMMAND bytecode_vm_smoke_tests)

add_test(NAME v1_1_core_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/v1_1_core_compatibility_demo.m)
set_tests_properties(v1_1_core_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 188")

add_test(NAME v1_1_core_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/v1_1_core_compatibility_demo.m)
set_tests_properties(v1_1_core_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 188")

add_test(NAME v1_1_core_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/v1_1_core_compatibility_demo.m)
set_tests_properties(v1_1_core_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 188")

add_executable(struct_runtime_smoke_tests tests/struct_runtime_smoke.cpp)
target_link_libraries(struct_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME struct_runtime_smoke COMMAND struct_runtime_smoke_tests)

add_test(NAME struct_runtime_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_runtime_demo.m)
set_tests_properties(struct_runtime_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2193")

add_test(NAME struct_runtime_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_runtime_demo.m)
set_tests_properties(struct_runtime_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2193")

add_test(NAME struct_runtime_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_runtime_demo.m)
set_tests_properties(struct_runtime_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2193")

add_test(NAME struct_array_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_array_demo.m)
set_tests_properties(struct_array_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1760")

add_test(NAME struct_array_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_array_demo.m)
set_tests_properties(struct_array_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1760")

add_test(NAME struct_array_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/struct_array_demo.m)
set_tests_properties(struct_array_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1760")

add_executable(lvalue_runtime_smoke_tests
    tests/lvalue_runtime_smoke.cpp)
target_link_libraries(lvalue_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME lvalue_runtime_smoke
    COMMAND lvalue_runtime_smoke_tests)

add_executable(object_array_runtime_smoke_tests
    tests/object_array_runtime_smoke.cpp)
target_link_libraries(object_array_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME object_array_runtime_smoke
    COMMAND object_array_runtime_smoke_tests)

add_executable(object_array_vm_smoke_tests
    tests/object_array_vm_smoke.cpp)
target_link_libraries(object_array_vm_smoke_tests PRIVATE mparser_core)
add_test(NAME object_array_vm_smoke
    COMMAND object_array_vm_smoke_tests)

add_test(NAME object_array_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/object_array_demo.m)
set_tests_properties(object_array_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 14")

add_test(NAME object_array_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/object_array_demo.m)
set_tests_properties(object_array_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 14")

add_test(NAME lvalue_runtime_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/lvalue_copyback_demo.m)
set_tests_properties(lvalue_runtime_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 572")

add_test(NAME lvalue_runtime_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/lvalue_copyback_demo.m)
set_tests_properties(lvalue_runtime_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 572")

add_test(NAME lvalue_runtime_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/lvalue_copyback_demo.m)
set_tests_properties(lvalue_runtime_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 572")

add_test(NAME lvalue_object_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/lvalue_object_demo.m)
set_tests_properties(lvalue_object_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 839")

add_executable(exception_runtime_smoke_tests
    tests/exception_runtime_smoke.cpp)
target_link_libraries(exception_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME exception_runtime_smoke
    COMMAND exception_runtime_smoke_tests)

add_executable(exception_source_graph_smoke_tests
    tests/exception_source_graph_smoke.cpp)
target_link_libraries(exception_source_graph_smoke_tests PRIVATE mparser_core)
add_test(NAME exception_source_graph_smoke
    COMMAND exception_source_graph_smoke_tests)

add_test(NAME exception_runtime_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_runtime_demo.m)
set_tests_properties(exception_runtime_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111111")

add_test(NAME exception_runtime_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_runtime_demo.m)
set_tests_properties(exception_runtime_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111111")

add_test(NAME exception_runtime_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_runtime_demo.m)
set_tests_properties(exception_runtime_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111111")

add_test(NAME exception_diagnostics_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_diagnostics_demo.m)
set_tests_properties(exception_diagnostics_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111")

add_test(NAME exception_diagnostics_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_diagnostics_demo.m)
set_tests_properties(exception_diagnostics_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111")

add_test(NAME exception_diagnostics_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/exception_diagnostics_demo.m)
set_tests_properties(exception_diagnostics_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 111111")

add_test(NAME compatibility_matrix_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/docs/compatibility-matrix.json
        -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DTEST_REGISTRY=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_compatibility_matrix.cmake)

add_test(NAME project_license_smoke
    COMMAND ${CMAKE_COMMAND}
        -DLICENSE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        -DNOTICE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
        -DTHIRD_PARTY_NOTICES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_project_license.cmake)

# The v1 contract remains archived under docs/tests. The active API 1.2 / ABI
# generation 2 contract freezes only after the v1.2 surface is complete.
set_tests_properties(project_license_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "MParser project license validated: Apache-2.0, Copyright 2026 Wang Xin")

add_test(NAME release_documentation_smoke
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -DV1_RELEASE_VERSION=${MPARSER_V1_RELEASE_VERSION}
        -DV1_PUBLIC_CONTRACT_VERSION=${MPARSER_V1_PUBLIC_CONTRACT_ENGINE_VERSION}
        -DCLI_CONTRACT=${CMAKE_CURRENT_SOURCE_DIR}/docs/cli-contract-v1.json
        -DMPARSER_EXECUTABLE=$<TARGET_FILE:mparser>
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -DENTRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_documentation.cmake)
set_tests_properties(release_documentation_smoke PROPERTIES
    LABELS "documentation;release"
    PASS_REGULAR_EXPRESSION
        "MParser release documentation validated: CLI 1.0, manuals, production sample, and mparser.result 1.x")

add_test(NAME release_authentication_evidence_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_EVIDENCE_ROOT=${MPARSER_V1_AUTHENTICATION_EVIDENCE_ROOT}
        -DMPARSER_EXPECTED_VERSION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}
        -DMPARSER_EXPECTED_TAG=v${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}
        -DMPARSER_EXPECTED_REVISION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION}
        -DMPARSER_EXPECTED_RUN_ID=${MPARSER_V1_AUTHENTICATION_EVIDENCE_RUN_ID}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_authentication_evidence.cmake)
set_tests_properties(release_authentication_evidence_smoke PROPERTIES
    LABELS "documentation;release"
    PASS_REGULAR_EXPRESSION
        "MParser release authentication evidence validated: v1.0.0, run 30780391460, 5 platforms, 10 Sigstore subjects")

add_test(NAME release_publication_evidence_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_EVIDENCE_ROOT=${MPARSER_V1_PUBLICATION_EVIDENCE_ROOT}
        -DMPARSER_EXPECTED_VERSION=${MPARSER_V1_RELEASE_VERSION}
        -DMPARSER_EXPECTED_TAG=v${MPARSER_V1_RELEASE_VERSION}
        -DMPARSER_EXPECTED_REVISION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION}
        -DMPARSER_EXPECTED_RELEASE_ID=${MPARSER_V1_RELEASE_ID}
        -DMPARSER_EXPECTED_PUBLISHED_AT=${MPARSER_V1_RELEASE_PUBLISHED_AT}
        -DMPARSER_EXPECTED_RELEASE_CHECKSUM_SHA256=${MPARSER_V1_RELEASE_CHECKSUM_SHA256}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_publication_evidence.cmake)
set_tests_properties(release_publication_evidence_smoke PROPERTIES
    LABELS "documentation;release"
    PASS_REGULAR_EXPRESSION
        "MParser release publication evidence validated: v1.0.0, 32 assets, 15194301 bytes")

add_test(NAME release_candidate_readiness_smoke
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/docs/compatibility-matrix.json
        -DPUBLIC_CONTRACT=${CMAKE_CURRENT_SOURCE_DIR}/docs/public-contract-v1.json
        -DRELEASE_NOTES=${CMAKE_CURRENT_SOURCE_DIR}/docs/release-notes-v1.0.md
        -DROADMAP=${CMAKE_CURRENT_SOURCE_DIR}/docs/roadmap-v1.0.md
        -DJIT_SCOPE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_v1_jit_scope.cmake
        -DJIT_SCOPE_DECISION=${CMAKE_CURRENT_SOURCE_DIR}/docs/v1.0-jit-scope-decision.md
        -DNATIVE_SCALAR_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/native-scalar-loop-v1.json
        -DNATIVE_ARRAY_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/native-linear-array-v1.json
        -DNOJIT_ARRAY_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/nojit-linear-array-v1.json
        -DAUTHENTICATION_EVIDENCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_authentication_evidence.cmake
        -DAUTHENTICATION_EVIDENCE_ROOT=${MPARSER_V1_AUTHENTICATION_EVIDENCE_ROOT}
        -DPUBLICATION_EVIDENCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_publication_evidence.cmake
        -DPUBLICATION_EVIDENCE_ROOT=${MPARSER_V1_PUBLICATION_EVIDENCE_ROOT}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -DEXPECTED_RELEASE_VERSION=${MPARSER_V1_RELEASE_VERSION}
        -DEXPECTED_PUBLIC_CONTRACT_VERSION=${MPARSER_V1_PUBLIC_CONTRACT_ENGINE_VERSION}
        -DEXPECTED_JIT_EVIDENCE_VERSION=${MPARSER_V1_PERFORMANCE_EVIDENCE_VERSION}
        -DEXPECTED_AUTHENTICATION_VERSION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}
        -DEXPECTED_AUTHENTICATION_REVISION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION}
        -DEXPECTED_AUTHENTICATION_RUN_ID=${MPARSER_V1_AUTHENTICATION_EVIDENCE_RUN_ID}
        -DEXPECTED_RELEASE_ID=${MPARSER_V1_RELEASE_ID}
        -DEXPECTED_RELEASE_PUBLISHED_AT=${MPARSER_V1_RELEASE_PUBLISHED_AT}
        -DEXPECTED_RELEASE_CHECKSUM_SHA256=${MPARSER_V1_RELEASE_CHECKSUM_SHA256}
        -DEXPECTED_CONTRACT_STATE=frozen-v1
        -DEXPECTED_OPEN_MUST_HAVE=
        -DEXPECTED_OPEN_SHOULD_HAVE=G-SANITIZER-001:in-progress
        -DEXPECTED_DEFERRED_SHOULD_HAVE=G-JIT-001:deferred
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_candidate_readiness.cmake)
set_tests_properties(release_candidate_readiness_smoke PROPERTIES
    LABELS "documentation;release"
    PASS_REGULAR_EXPRESSION
        "MParser release readiness validated: ${PROJECT_VERSION}, 0 bounded Must-have blockers, 1 scoped Should-have items, 1 deferred Should-have items")

add_test(NAME release_candidate_readiness_guard_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_CMAKE_COMMAND=${CMAKE_COMMAND}
        -DMPARSER_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_candidate_readiness.cmake
        -DMPARSER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DMPARSER_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/release-candidate-readiness-guard
        -DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/docs/compatibility-matrix.json
        -DPUBLIC_CONTRACT=${CMAKE_CURRENT_SOURCE_DIR}/docs/public-contract-v1.json
        -DRELEASE_NOTES=${CMAKE_CURRENT_SOURCE_DIR}/docs/release-notes-v1.0.md
        -DROADMAP=${CMAKE_CURRENT_SOURCE_DIR}/docs/roadmap-v1.0.md
        -DJIT_SCOPE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_v1_jit_scope.cmake
        -DJIT_SCOPE_DECISION=${CMAKE_CURRENT_SOURCE_DIR}/docs/v1.0-jit-scope-decision.md
        -DNATIVE_SCALAR_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/native-scalar-loop-v1.json
        -DNATIVE_ARRAY_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/native-linear-array-v1.json
        -DNOJIT_ARRAY_REPORT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/windows-x86_64-msvc-release/nojit-linear-array-v1.json
        -DAUTHENTICATION_EVIDENCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_authentication_evidence.cmake
        -DAUTHENTICATION_EVIDENCE_ROOT=${MPARSER_V1_AUTHENTICATION_EVIDENCE_ROOT}
        -DPUBLICATION_EVIDENCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_publication_evidence.cmake
        -DPUBLICATION_EVIDENCE_ROOT=${MPARSER_V1_PUBLICATION_EVIDENCE_ROOT}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -DEXPECTED_RELEASE_VERSION=${MPARSER_V1_RELEASE_VERSION}
        -DEXPECTED_PUBLIC_CONTRACT_VERSION=${MPARSER_V1_PUBLIC_CONTRACT_ENGINE_VERSION}
        -DEXPECTED_JIT_EVIDENCE_VERSION=${MPARSER_V1_PERFORMANCE_EVIDENCE_VERSION}
        -DEXPECTED_AUTHENTICATION_VERSION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}
        -DEXPECTED_AUTHENTICATION_REVISION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION}
        -DEXPECTED_AUTHENTICATION_RUN_ID=${MPARSER_V1_AUTHENTICATION_EVIDENCE_RUN_ID}
        -DEXPECTED_RELEASE_ID=${MPARSER_V1_RELEASE_ID}
        -DEXPECTED_RELEASE_PUBLISHED_AT=${MPARSER_V1_RELEASE_PUBLISHED_AT}
        -DEXPECTED_RELEASE_CHECKSUM_SHA256=${MPARSER_V1_RELEASE_CHECKSUM_SHA256}
        -DEXPECTED_CONTRACT_STATE=frozen-v1
        -DEXPECTED_OPEN_MUST_HAVE=
        -DEXPECTED_OPEN_SHOULD_HAVE=G-SANITIZER-001:in-progress
        -DEXPECTED_DEFERRED_SHOULD_HAVE=G-JIT-001:deferred
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_candidate_readiness_guards.cmake)
set_tests_properties(release_candidate_readiness_guard_smoke PROPERTIES
    LABELS "documentation;release"
    PASS_REGULAR_EXPRESSION
        "MParser release readiness guards validated: Must-have/Should-have impact, deferred JIT reactivation, and blocker-set drift rejected")

add_executable(nd_array_smoke_tests tests/nd_array_smoke.cpp)
target_link_libraries(nd_array_smoke_tests PRIVATE mparser_core)
add_test(NAME nd_array_smoke COMMAND nd_array_smoke_tests)

add_executable(array_transform_smoke_tests
    tests/array_transform_smoke.cpp)
target_link_libraries(array_transform_smoke_tests PRIVATE mparser_core)
add_test(NAME array_transform_smoke COMMAND array_transform_smoke_tests)

add_executable(logical_index_smoke_tests
    tests/logical_index_smoke.cpp)
target_link_libraries(logical_index_smoke_tests PRIVATE mparser_core)
add_test(NAME logical_index_smoke COMMAND logical_index_smoke_tests)

add_executable(numeric_type_smoke_tests
    tests/numeric_type_smoke.cpp)
target_link_libraries(numeric_type_smoke_tests PRIVATE mparser_core)
add_test(NAME numeric_type_smoke COMMAND numeric_type_smoke_tests)

add_executable(complex_numeric_smoke_tests
    tests/complex_numeric_smoke.cpp)
target_link_libraries(complex_numeric_smoke_tests PRIVATE mparser_core)
add_test(NAME complex_numeric_smoke COMMAND complex_numeric_smoke_tests)

add_executable(core_numeric_builtin_smoke_tests
    tests/core_numeric_builtin_smoke.cpp)
target_link_libraries(core_numeric_builtin_smoke_tests PRIVATE mparser_core)
add_test(NAME core_numeric_builtin_smoke
    COMMAND core_numeric_builtin_smoke_tests)

add_test(NAME core_numeric_builtin_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/core_numeric_builtins_demo.m)
set_tests_properties(core_numeric_builtin_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 0")

add_test(NAME core_numeric_builtin_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/core_numeric_builtins_demo.m)
set_tests_properties(core_numeric_builtin_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 0")

add_test(NAME core_numeric_builtin_portable_jit_cli_smoke
    COMMAND mparser --run --jit=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/core_numeric_builtins_demo.m)
set_tests_properties(core_numeric_builtin_portable_jit_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 0")

if(MPARSER_ENABLE_NATIVE_JIT)
    add_test(NAME core_numeric_builtin_native_jit_cli_smoke
        COMMAND mparser --run --jit=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/core_numeric_builtins_demo.m)
    set_tests_properties(core_numeric_builtin_native_jit_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "summary = 0")
endif()

add_test(NAME complex_numeric_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/complex_numeric_demo.m)
set_tests_properties(complex_numeric_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1")

add_test(NAME complex_numeric_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/complex_numeric_demo.m)
set_tests_properties(complex_numeric_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1")

add_test(NAME complex_numeric_portable_jit_cli_smoke
    COMMAND mparser --run --jit=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/complex_numeric_demo.m)
set_tests_properties(complex_numeric_portable_jit_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1")

if(MPARSER_ENABLE_NATIVE_JIT)
    add_test(NAME complex_numeric_native_jit_cli_smoke
        COMMAND mparser --run --jit=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/complex_numeric_demo.m)
    set_tests_properties(complex_numeric_native_jit_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "summary = 1")
endif()

add_test(NAME numeric_type_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/numeric_types_demo.m)
set_tests_properties(numeric_type_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 5")

add_test(NAME numeric_type_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/numeric_types_demo.m)
set_tests_properties(numeric_type_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 5")

add_test(NAME numeric_type_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/numeric_types_demo.m)
set_tests_properties(numeric_type_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 5")

add_executable(array_deletion_smoke_tests
    tests/array_deletion_smoke.cpp)
target_link_libraries(array_deletion_smoke_tests PRIVATE mparser_core)
add_test(NAME array_deletion_smoke COMMAND array_deletion_smoke_tests)

add_executable(reduction_find_smoke_tests
    tests/reduction_find_smoke.cpp)
target_link_libraries(reduction_find_smoke_tests PRIVATE mparser_core)
add_test(NAME reduction_find_smoke COMMAND reduction_find_smoke_tests)

add_executable(scan_diff_smoke_tests tests/scan_diff_smoke.cpp)
target_link_libraries(scan_diff_smoke_tests PRIVATE mparser_core)
add_test(NAME scan_diff_smoke COMMAND scan_diff_smoke_tests)

add_executable(optimization_plan_smoke_tests tests/optimization_plan_smoke.cpp)
target_link_libraries(optimization_plan_smoke_tests PRIVATE mparser_core)
add_test(NAME optimization_plan_smoke COMMAND optimization_plan_smoke_tests)

add_executable(bytecode_region_smoke_tests tests/bytecode_region_smoke.cpp)
target_link_libraries(bytecode_region_smoke_tests PRIVATE mparser_core)
add_test(NAME bytecode_region_smoke COMMAND bytecode_region_smoke_tests)

add_executable(typed_ir_smoke_tests tests/typed_ir_smoke.cpp)
target_link_libraries(typed_ir_smoke_tests PRIVATE mparser_core)
add_test(NAME typed_ir_smoke COMMAND typed_ir_smoke_tests)

add_executable(typed_region_executor_smoke_tests
    tests/typed_region_executor_smoke.cpp)
target_link_libraries(typed_region_executor_smoke_tests PRIVATE mparser_core)
add_test(NAME typed_region_executor_smoke
    COMMAND typed_region_executor_smoke_tests)

add_executable(native_scalar_jit_cache_smoke_tests
    tests/native_scalar_jit_cache_smoke.cpp)
target_link_libraries(native_scalar_jit_cache_smoke_tests PRIVATE
    mparser_core Threads::Threads)
add_test(NAME native_scalar_jit_cache_smoke
    COMMAND native_scalar_jit_cache_smoke_tests)

add_test(NAME scalar_kernel_cli_smoke
    COMMAND mparser
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
set_tests_properties(scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "predecoded scalar kernel executed")

add_test(NAME guarded_jit_cli_smoke
    COMMAND mparser
        --run-jit
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(guarded_jit_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed.*baseline validation: skipped")

add_test(NAME cli_help_smoke
    COMMAND mparser --help)
set_tests_properties(cli_help_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "--run                  Run once with full bytecode semantics")

add_test(NAME production_run_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m)
set_tests_properties(production_run_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 705")

add_test(NAME reliability_boundary_cli_smoke
    COMMAND mparser --run --jit=off
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reliability_boundary_demo.m)
set_tests_properties(reliability_boundary_cli_smoke PROPERTIES
    LABELS reliability
    PASS_REGULAR_EXPRESSION "summary = 26")

add_test(NAME production_run_portable_jit_cli_smoke
    COMMAND mparser --run --jit=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m)
set_tests_properties(production_run_portable_jit_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 705")

add_test(NAME production_run_jit_off_cli_smoke
    COMMAND mparser --run --jit=off --native-cache-stats
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m)
set_tests_properties(production_run_jit_off_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 705.*compilations=0")

add_test(NAME production_run_cli_contract_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_EXECUTABLE=$<TARGET_FILE:mparser>
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -DENTRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_production_cli_contract.cmake)

add_test(NAME native_jit_benchmark_portable_cli_smoke
    COMMAND mparser --run --jit=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/native_jit_benchmark.m)
set_tests_properties(native_jit_benchmark_portable_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "finalValue = 834\\.32145639679")

add_test(NAME nested_scalar_kernel_cli_smoke
    COMMAND mparser
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(nested_scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed")

add_test(NAME nested_adaptive_kernel_cli_smoke
    COMMAND mparser
        --run-adaptive-bytecode
        --adaptive-runs=3
        --adaptive-hot-loop=10
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
set_tests_properties(nested_adaptive_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded nested scalar kernel executed")

add_test(NAME branched_scalar_kernel_cli_smoke
    COMMAND mparser
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/branched_typed_loop_demo.m)
set_tests_properties(branched_scalar_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 23520")

add_test(NAME linear_array_kernel_cli_smoke
    COMMAND mparser
        --run-typed-bytecode
        --typed-backend=portable
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_linear_array_demo.m)
set_tests_properties(linear_array_kernel_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "predecoded linear-array scalar kernel executed")

if(MPARSER_ENABLE_NATIVE_JIT)
    add_test(NAME production_run_native_jit_cli_smoke
        COMMAND mparser --run --jit=native --native-cache-stats
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/production_run_demo.m)
    set_tests_properties(production_run_native_jit_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "summary = 705.*compilations=[1-9]")

    add_test(NAME native_jit_benchmark_native_cli_smoke
        COMMAND mparser --run --jit=native --native-cache-stats
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/native_jit_benchmark.m)
    set_tests_properties(native_jit_benchmark_native_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "finalValue = 834\\.32145639679.*compilations=[1-9]")

    add_test(NAME native_scalar_kernel_cli_smoke
        COMMAND mparser
            --run-typed-bytecode
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
    set_tests_properties(native_scalar_kernel_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "backend=native")

    add_test(NAME native_kernel_cache_cli_smoke
        COMMAND mparser
            --run-adaptive-bytecode
            --adaptive-runs=3
            --adaptive-hot-loop=10
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/nested_typed_loop_demo.m)
    set_tests_properties(native_kernel_cache_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*nativeCacheHits=1")

    add_test(NAME native_branched_kernel_cache_cli_smoke
        COMMAND mparser
            --run-adaptive-bytecode
            --adaptive-runs=3
            --adaptive-hot-loop=10
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/branched_typed_loop_demo.m)
    set_tests_properties(native_branched_kernel_cache_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*nativeCacheHits=1")

    add_test(NAME native_linear_array_kernel_cli_smoke
        COMMAND mparser
            --run-typed-bytecode
            --typed-backend=native
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_linear_array_demo.m)
    set_tests_properties(native_linear_array_kernel_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "backend=native.*linear-array scalar kernel executed")

    add_test(NAME native_cache_lifecycle_cli_smoke
        COMMAND mparser
            --run-module-runtime
            --adaptive-hot-loop=5
            --typed-backend=native
            --native-cache-entries=2
            --native-cache-bytes=1048576
            --native-cache-stats
            --module-call=cache_add:2
            --module-call=cache_add:2
            --module-call=cache_add:2
            --module-call=cache_multiply:2
            --module-call=cache_multiply:2
            --module-call=cache_multiply:2
            --module-call=cache_absolute:2
            --module-call=cache_absolute:2
            --module-call=cache_absolute:2
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/native_cache_demo.m)
    set_tests_properties(native_cache_lifecycle_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "lifecycle: evictions=1")
else()
    add_test(NAME native_jit_disabled_cli_smoke
        COMMAND mparser
            --run-typed-bytecode
            --typed-backend=auto
            ${CMAKE_CURRENT_SOURCE_DIR}/samples/typed_region_demo.m)
    set_tests_properties(native_jit_disabled_cli_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION
            "native scalar JIT support was disabled at build time")
endif()

add_executable(runtime_benchmark_smoke_tests
    tests/runtime_benchmark_smoke.cpp)
target_link_libraries(runtime_benchmark_smoke_tests PRIVATE mparser_core)
add_test(NAME runtime_benchmark_smoke COMMAND runtime_benchmark_smoke_tests)

add_executable(adaptive_bytecode_vm_smoke_tests
    tests/adaptive_bytecode_vm_smoke.cpp)
target_link_libraries(adaptive_bytecode_vm_smoke_tests PRIVATE mparser_core)
add_test(NAME adaptive_bytecode_vm_smoke
    COMMAND adaptive_bytecode_vm_smoke_tests)

add_executable(adaptive_module_runtime_smoke_tests
    tests/adaptive_module_runtime_smoke.cpp)
target_link_libraries(adaptive_module_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME adaptive_module_runtime_smoke
    COMMAND adaptive_module_runtime_smoke_tests)

add_executable(compiled_module_smoke_tests
    tests/compiled_module_smoke.cpp)
target_link_libraries(compiled_module_smoke_tests PRIVATE mparser_core)
add_test(NAME compiled_module_smoke COMMAND compiled_module_smoke_tests)

add_executable(runtime_session_smoke_tests
    tests/runtime_session_smoke.cpp)
target_link_libraries(runtime_session_smoke_tests PRIVATE mparser_core)
add_test(NAME runtime_session_smoke COMMAND runtime_session_smoke_tests)

add_executable(runtime_soak_smoke_tests
    tests/runtime_soak_smoke.cpp)
target_link_libraries(runtime_soak_smoke_tests PRIVATE mparser_core)
add_test(NAME runtime_soak_smoke COMMAND runtime_soak_smoke_tests)
set_tests_properties(runtime_soak_smoke PROPERTIES
    LABELS reliability
    TIMEOUT 60)

add_executable(runtime_contract_smoke_tests
    tests/runtime_contract_smoke.cpp)
target_link_libraries(runtime_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME runtime_contract_smoke COMMAND runtime_contract_smoke_tests)

add_test(NAME runtime_contract_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/runtime_contract_demo.m)
set_tests_properties(runtime_contract_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 11023")

add_test(NAME runtime_contract_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/runtime_contract_demo.m)
set_tests_properties(runtime_contract_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 11023")

add_test(NAME runtime_contract_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/runtime_contract_demo.m)
set_tests_properties(runtime_contract_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 11023")

add_test(NAME workspace_session_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/workspace_session_demo.m)
set_tests_properties(workspace_session_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 14389")

add_test(NAME workspace_session_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/workspace_session_demo.m)
set_tests_properties(workspace_session_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 14389")

add_test(NAME workspace_session_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/workspace_session_demo.m)
set_tests_properties(workspace_session_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 14389")

add_executable(function_handle_runtime_smoke_tests
    tests/function_handle_runtime_smoke.cpp)
target_link_libraries(function_handle_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME function_handle_runtime_smoke
    COMMAND function_handle_runtime_smoke_tests)

add_test(NAME dynamic_call_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/dynamic_call_demo.m)
set_tests_properties(dynamic_call_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 106")

add_test(NAME dynamic_call_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/dynamic_call_demo.m)
set_tests_properties(dynamic_call_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 106")

add_test(NAME dynamic_call_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/dynamic_call_demo.m)
set_tests_properties(dynamic_call_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 106")

add_executable(source_loader_smoke_tests tests/source_loader_smoke.cpp)
target_link_libraries(source_loader_smoke_tests PRIVATE mparser_core)
add_test(NAME source_loader_smoke COMMAND source_loader_smoke_tests)

add_executable(namespace_function_smoke_tests
    tests/namespace_function_smoke.cpp)
target_link_libraries(namespace_function_smoke_tests PRIVATE mparser_core)
add_test(NAME namespace_function_smoke
    COMMAND namespace_function_smoke_tests)

add_executable(function_path_smoke_tests
    tests/function_path_smoke.cpp)
target_link_libraries(function_path_smoke_tests PRIVATE mparser_core)
add_test(NAME function_path_smoke
    COMMAND function_path_smoke_tests)

add_executable(class_folder_smoke_tests
    tests/class_folder_smoke.cpp)
target_link_libraries(class_folder_smoke_tests PRIVATE mparser_core)
add_test(NAME class_folder_smoke
    COMMAND class_folder_smoke_tests)

add_executable(class_private_function_smoke_tests
    tests/class_private_function_smoke.cpp)
target_link_libraries(class_private_function_smoke_tests PRIVATE mparser_core)
add_test(NAME class_private_function_smoke
    COMMAND class_private_function_smoke_tests)

add_test(NAME cross_file_class_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/cross_file_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/cross_file_classes/run_demo.m)
set_tests_properties(cross_file_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "dynamic_code = 2")

add_test(NAME package_class_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/package_classes
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/package_classes/app/run_demo.m)
set_tests_properties(package_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_code = 44")

add_test(NAME enumeration_class_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 56")

add_test(NAME enumeration_class_module_cli_smoke
    COMMAND mparser
        --module-info
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "namespace=palette")

add_test(NAME enumeration_class_hir_cli_smoke
    COMMAND mparser
        --hir
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/run_demo.m)
set_tests_properties(enumeration_class_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "EnumerationMember Ready type=palette.Status")

add_test(NAME enumeration_class_query_loader_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/enumeration_classes/app/query_only.m)
set_tests_properties(enumeration_class_query_loader_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "query_count = 2")

add_test(NAME event_listener_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 40")

add_test(NAME event_listener_module_cli_smoke
    COMMAND mparser
        --module-info
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "signals.onPulse")

add_test(NAME event_listener_hir_cli_smoke
    COMMAND mparser
        --hir
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/event_listeners/app/run_demo.m)
set_tests_properties(event_listener_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Event Tick type=signals.Pulse")

add_test(NAME nd_array_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nd_arrays_demo.m)
set_tests_properties(nd_array_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 171")

add_test(NAME nd_array_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/nd_arrays_demo.m)
set_tests_properties(nd_array_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 171")

add_test(NAME array_assignment_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_assignment_demo.m)
set_tests_properties(array_assignment_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 860")

add_test(NAME array_assignment_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_assignment_demo.m)
set_tests_properties(array_assignment_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 860")

add_test(NAME array_transform_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_transform_demo.m)
set_tests_properties(array_transform_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2126")

add_test(NAME array_transform_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_transform_demo.m)
set_tests_properties(array_transform_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2126")

add_test(NAME logical_index_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/logical_index_demo.m)
set_tests_properties(logical_index_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 996")

add_test(NAME logical_index_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/logical_index_demo.m)
set_tests_properties(logical_index_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 996")

add_test(NAME array_deletion_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_deletion_demo.m)
set_tests_properties(array_deletion_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 252")

add_test(NAME array_deletion_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/array_deletion_demo.m)
set_tests_properties(array_deletion_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 252")

add_test(NAME reduction_find_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reduction_find_demo.m)
set_tests_properties(reduction_find_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1450")

add_test(NAME reduction_find_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reduction_find_demo.m)
set_tests_properties(reduction_find_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 1450")

add_test(NAME scan_diff_bytecode_cli_smoke
    COMMAND mparser
        --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/scan_diff_demo.m)
set_tests_properties(scan_diff_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2359")

add_test(NAME scan_diff_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/scan_diff_demo.m)
set_tests_properties(scan_diff_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2359")

add_test(NAME namespace_function_cli_smoke
    COMMAND mparser
        --run-bytecode
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions/app/run_demo.m)
set_tests_properties(namespace_function_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_value = 102")

add_test(NAME namespace_function_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        --class-path=${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/namespace_functions/app/run_function_demo.m)
set_tests_properties(namespace_function_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "nested_value = 102")

add_test(NAME function_path_cli_smoke
    COMMAND mparser
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/first_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/second_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/global_library
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/app/run_demo.m)
set_tests_properties(function_path_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "outside_value = 101")

add_test(NAME function_path_interpreter_cli_smoke
    COMMAND mparser
        --run-hir
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/first_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/second_library
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/global_library
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/function_paths/app/run_demo.m)
set_tests_properties(function_path_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "outside_value = 101")

add_test(NAME class_folder_cli_smoke
    COMMAND mparser
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m)
set_tests_properties(class_folder_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "revealed = 107")

add_test(NAME class_folder_module_cli_smoke
    COMMAND mparser
        --module-info
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m)
set_tests_properties(class_folder_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "method-of=folderpkg.Counter")

add_test(NAME class_private_function_cli_smoke
    COMMAND mparser
        --run-bytecode
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "function_choice = 16")

add_test(NAME class_private_function_module_cli_smoke
    COMMAND mparser
        --module-info
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_module_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "private-of=securepkg.Vault")

add_test(NAME class_private_function_hir_cli_smoke
    COMMAND mparser
        --hir
        --path=${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_private_functions/app/run_demo.m)
set_tests_properties(class_private_function_hir_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "lexical-class=securepkg.Vault")

add_test(NAME source_loader_diagnostic_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_EXECUTABLE=$<TARGET_FILE:mparser>
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -DENTRY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_loader_diagnostic/main.m
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/assert_source_diagnostic.cmake)

add_executable(function_call_contract_smoke_tests
    tests/function_call_contract_smoke.cpp)
target_link_libraries(function_call_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME function_call_contract_smoke
    COMMAND function_call_contract_smoke_tests)

add_executable(cell_varargs_smoke_tests tests/cell_varargs_smoke.cpp)
target_link_libraries(cell_varargs_smoke_tests PRIVATE mparser_core)
add_test(NAME cell_varargs_smoke COMMAND cell_varargs_smoke_tests)

add_executable(class_runtime_smoke_tests tests/class_runtime_smoke.cpp)
target_link_libraries(class_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME class_runtime_smoke COMMAND class_runtime_smoke_tests)

add_executable(enumeration_class_smoke_tests
    tests/enumeration_class_smoke.cpp)
target_link_libraries(enumeration_class_smoke_tests PRIVATE mparser_core)
add_test(NAME enumeration_class_smoke
    COMMAND enumeration_class_smoke_tests)

add_executable(event_listener_smoke_tests
    tests/event_listener_smoke.cpp)
target_link_libraries(event_listener_smoke_tests PRIVATE mparser_core)
add_test(NAME event_listener_smoke
    COMMAND event_listener_smoke_tests)

add_executable(property_listener_smoke_tests
    tests/property_listener_smoke.cpp)
target_link_libraries(property_listener_smoke_tests PRIVATE mparser_core)
add_test(NAME property_listener_smoke
    COMMAND property_listener_smoke_tests)

add_test(NAME property_listener_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/property_events_demo.m)
set_tests_properties(property_listener_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 738")

add_executable(handle_lifecycle_smoke_tests
    tests/handle_lifecycle_smoke.cpp)
target_link_libraries(handle_lifecycle_smoke_tests PRIVATE mparser_core)
add_test(NAME handle_lifecycle_smoke
    COMMAND handle_lifecycle_smoke_tests)

add_test(NAME handle_lifecycle_bytecode_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/handle_lifecycle_demo.m)
set_tests_properties(handle_lifecycle_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 161")

add_executable(handle_semantics_smoke_tests tests/handle_semantics_smoke.cpp)
target_link_libraries(handle_semantics_smoke_tests PRIVATE mparser_core)
add_test(NAME handle_semantics_smoke COMMAND handle_semantics_smoke_tests)

add_executable(class_inheritance_smoke_tests
    tests/class_inheritance_smoke.cpp)
target_link_libraries(class_inheritance_smoke_tests PRIVATE mparser_core)
add_test(NAME class_inheritance_smoke COMMAND class_inheritance_smoke_tests)

add_executable(superclass_construction_smoke_tests
    tests/superclass_construction_smoke.cpp)
target_link_libraries(superclass_construction_smoke_tests PRIVATE mparser_core)
add_test(NAME superclass_construction_smoke
    COMMAND superclass_construction_smoke_tests)

add_executable(property_validation_smoke_tests
    tests/property_validation_smoke.cpp)
target_link_libraries(property_validation_smoke_tests PRIVATE mparser_core)
add_test(NAME property_validation_smoke
    COMMAND property_validation_smoke_tests)

add_executable(argument_validation_smoke_tests
    tests/argument_validation_smoke.cpp)
target_link_libraries(argument_validation_smoke_tests PRIVATE mparser_core)
add_test(NAME argument_validation_smoke
    COMMAND argument_validation_smoke_tests)

add_executable(repeating_argument_smoke_tests
    tests/repeating_argument_smoke.cpp)
target_link_libraries(repeating_argument_smoke_tests PRIVATE mparser_core)
add_test(NAME repeating_argument_smoke
    COMMAND repeating_argument_smoke_tests)

add_executable(name_value_argument_smoke_tests
    tests/name_value_argument_smoke.cpp)
target_link_libraries(name_value_argument_smoke_tests PRIVATE mparser_core)
add_test(NAME name_value_argument_smoke
    COMMAND name_value_argument_smoke_tests)

add_executable(class_property_argument_smoke_tests
    tests/class_property_argument_smoke.cpp)
target_link_libraries(class_property_argument_smoke_tests PRIVATE
    mparser_core)
add_test(NAME class_property_argument_smoke
    COMMAND class_property_argument_smoke_tests)

add_executable(output_argument_smoke_tests
    tests/output_argument_smoke.cpp)
target_link_libraries(output_argument_smoke_tests PRIVATE mparser_core)
add_test(NAME output_argument_smoke
    COMMAND output_argument_smoke_tests)

add_test(NAME output_arguments_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/output_arguments_demo.m)
set_tests_properties(output_arguments_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 34")

add_test(NAME output_arguments_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/output_arguments_demo.m)
set_tests_properties(output_arguments_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 34")

add_test(NAME output_arguments_entry_cli_smoke
    COMMAND mparser --run-bytecode --entry-function=repeatedOutputs
        --argument=4 --outputs=2
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/output_arguments_demo.m)
set_tests_properties(output_arguments_entry_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "values1 = 4")

add_test(NAME name_value_arguments_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/name_value_arguments_demo.m)
set_tests_properties(name_value_arguments_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 65")

add_test(NAME name_value_arguments_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/name_value_arguments_demo.m)
set_tests_properties(name_value_arguments_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 65")

add_test(NAME name_value_arguments_entry_cli_smoke
    COMMAND mparser --run-bytecode --entry-function=configure
        --argument=2 --argument=Scale=4 --outputs=2
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/name_value_arguments_demo.m)
set_tests_properties(name_value_arguments_entry_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "y = 9")

add_test(NAME class_property_arguments_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_property_arguments_demo.m)
set_tests_properties(class_property_arguments_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "answer = 34")

add_test(NAME class_property_arguments_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_property_arguments_demo.m)
set_tests_properties(class_property_arguments_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "answer = 34")

add_test(NAME repeating_arguments_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/repeating_arguments_demo.m)
set_tests_properties(repeating_arguments_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2145")

add_test(NAME repeating_arguments_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/repeating_arguments_demo.m)
set_tests_properties(repeating_arguments_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 2145")

add_executable(runtime_argument_validation_smoke_tests
    tests/runtime_argument_validation_smoke.cpp)
target_link_libraries(runtime_argument_validation_smoke_tests PRIVATE
    mparser_core)
add_test(NAME runtime_argument_validation_smoke
    COMMAND runtime_argument_validation_smoke_tests)

add_executable(class_member_access_smoke_tests
    tests/class_member_access_smoke.cpp)
target_link_libraries(class_member_access_smoke_tests PRIVATE mparser_core)
add_test(NAME class_member_access_smoke
    COMMAND class_member_access_smoke_tests)

add_executable(class_contract_smoke_tests tests/class_contract_smoke.cpp)
target_link_libraries(class_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME class_contract_smoke COMMAND class_contract_smoke_tests)

add_executable(class_access_policy_smoke_tests
    tests/class_access_policy_smoke.cpp)
target_link_libraries(class_access_policy_smoke_tests PRIVATE mparser_core)
add_test(NAME class_access_policy_smoke
    COMMAND class_access_policy_smoke_tests)

add_executable(class_property_identity_smoke_tests
    tests/class_property_identity_smoke.cpp)
target_link_libraries(class_property_identity_smoke_tests PRIVATE mparser_core)
add_test(NAME class_property_identity_smoke
    COMMAND class_property_identity_smoke_tests)

add_executable(class_method_identity_smoke_tests
    tests/class_method_identity_smoke.cpp)
target_link_libraries(class_method_identity_smoke_tests PRIVATE mparser_core)
add_test(NAME class_method_identity_smoke
    COMMAND class_method_identity_smoke_tests)

add_executable(class_metadata_smoke_tests
    tests/class_metadata_smoke.cpp)
target_link_libraries(class_metadata_smoke_tests PRIVATE mparser_core)
add_test(NAME class_metadata_smoke
    COMMAND class_metadata_smoke_tests)

add_test(NAME class_metadata_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_metadata_demo.m)
set_tests_properties(class_metadata_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 18")

add_executable(dynamic_property_smoke_tests
    tests/dynamic_property_smoke.cpp)
target_link_libraries(dynamic_property_smoke_tests PRIVATE mparser_core)
add_test(NAME dynamic_property_smoke
    COMMAND dynamic_property_smoke_tests)

add_test(NAME dynamic_property_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/dynamic_properties_demo.m)
set_tests_properties(dynamic_property_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 68")

add_executable(function_metadata_smoke_tests
    tests/function_metadata_smoke.cpp)
target_link_libraries(function_metadata_smoke_tests PRIVATE mparser_core)
add_test(NAME function_metadata_smoke
    COMMAND function_metadata_smoke_tests)

add_test(NAME function_metadata_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/function_metadata_demo.m)
set_tests_properties(function_metadata_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 17")

add_executable(reflection_contract_smoke_tests
    tests/reflection_contract_smoke.cpp)
target_link_libraries(reflection_contract_smoke_tests PRIVATE mparser_core)
add_test(NAME reflection_contract_smoke
    COMMAND reflection_contract_smoke_tests)

add_test(NAME reflection_contract_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reflection_contract_demo.m)
set_tests_properties(reflection_contract_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 22")

add_test(NAME reflection_contract_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/reflection_contract_demo.m)
set_tests_properties(reflection_contract_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 22")

add_executable(interpreter_smoke_tests tests/interpreter_smoke.cpp)
target_link_libraries(interpreter_smoke_tests PRIVATE mparser_core)
add_test(NAME interpreter_smoke COMMAND interpreter_smoke_tests)

add_executable(text_runtime_smoke_tests tests/text_runtime_smoke.cpp)
target_link_libraries(text_runtime_smoke_tests PRIVATE mparser_core)
add_test(NAME text_runtime_smoke
    COMMAND text_runtime_smoke_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/text_runtime_demo.m)

add_test(NAME text_runtime_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/text_runtime_demo.m)
set_tests_properties(text_runtime_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 49")

add_test(NAME text_runtime_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/text_runtime_demo.m)
set_tests_properties(text_runtime_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 49")

add_test(NAME text_runtime_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/text_runtime_demo.m)
set_tests_properties(text_runtime_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 49")

add_executable(builtin_registry_smoke_tests
    tests/builtin_registry_smoke.cpp)
target_link_libraries(builtin_registry_smoke_tests PRIVATE
    mparser_core)
add_test(NAME builtin_registry_smoke
    COMMAND builtin_registry_smoke_tests)

add_executable(runtime_output_smoke_tests
    tests/runtime_output_smoke.cpp)
target_link_libraries(runtime_output_smoke_tests PRIVATE
    mparser_core)
add_test(NAME runtime_output_smoke
    COMMAND runtime_output_smoke_tests)

add_executable(builtin_catalog_snapshot_tests
    tests/builtin_catalog_snapshot.cpp)
target_link_libraries(builtin_catalog_snapshot_tests PRIVATE
    mparser_core)
target_include_directories(builtin_catalog_snapshot_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include)
# This remains the snapshot generation/comparison tool. The active v1.2
# catalog is frozen after its numeric and complex builtin surface settles.

add_test(NAME builtin_registry_interpreter_cli_smoke
    COMMAND mparser --run-hir
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/builtin_registry_demo.m)
set_tests_properties(builtin_registry_interpreter_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 510446")

add_test(NAME builtin_registry_bytecode_cli_smoke
    COMMAND mparser --run-bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/builtin_registry_demo.m)
set_tests_properties(builtin_registry_bytecode_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 510446")

add_test(NAME builtin_registry_production_cli_smoke
    COMMAND mparser --run
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/builtin_registry_demo.m)
set_tests_properties(builtin_registry_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 510446")

add_executable(module_execution_smoke_tests
    tests/module_execution_smoke.cpp)
target_link_libraries(module_execution_smoke_tests PRIVATE
    mparser_core)
add_test(NAME module_execution_smoke
    COMMAND module_execution_smoke_tests)

add_executable(module_resource_control_smoke_tests
    tests/module_resource_control_smoke.cpp)
target_link_libraries(module_resource_control_smoke_tests PRIVATE
    mparser_core)
add_test(NAME module_resource_control_smoke
    COMMAND module_resource_control_smoke_tests)

add_test(NAME host_integration_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_CLI=$<TARGET_FILE:mparser>
        -DHOST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/host_integration_demo.m
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -DMPARSER_NATIVE_JIT=${MPARSER_ENABLE_NATIVE_JIT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_host_integration_cli.cmake)

add_executable(machine_protocol_smoke_tests
    tests/machine_protocol_smoke.cpp)
target_link_libraries(machine_protocol_smoke_tests PRIVATE
    mparser_core)
add_test(NAME machine_protocol_smoke
    COMMAND machine_protocol_smoke_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_v1.json
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_emergency_v1.json)

add_test(NAME machine_protocol_contract_smoke
    COMMAND ${CMAKE_COMMAND}
        -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/machine-result-v1.schema.json
        -DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_v1.json
        -DEMERGENCY=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_emergency_v1.json
        -DSNAPSHOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/public_contract/protocol/1.1/machine_result_1_1.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_machine_protocol_contract.cmake)
set_tests_properties(machine_protocol_contract_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "MParser machine protocol contract validated: schema 1, exact uint64, emergency, additive minor, five negative cases")

add_executable(machine_protocol_schema_smoke_tests
    tests/machine_protocol_schema_smoke.cpp)
target_link_libraries(machine_protocol_schema_smoke_tests PRIVATE
    mparser_json_schema_validator)
add_test(NAME machine_protocol_schema_smoke
    COMMAND machine_protocol_schema_smoke_tests
        --contract-cases
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/machine-result-v1.schema.json
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_v1.json
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/machine_result_emergency_v1.json
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/public_contract/protocol/1.1/machine_result_1_1.json)

add_test(NAME machine_protocol_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_CLI=$<TARGET_FILE:mparser>
        -DSCHEMA_VALIDATOR=$<TARGET_FILE:machine_protocol_schema_smoke_tests>
        -DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/machine-result-v1.schema.json
        -DSCHEMA_VALIDATION_DIR=${CMAKE_CURRENT_BINARY_DIR}/machine-protocol-schema
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -DSUCCESS_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/machine_protocol_demo.m
        -DHOST_OUTPUT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/host_integration_demo.m
        -DCOMPILE_FAILURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/machine_protocol_compile_failure.m
        -DRUNTIME_FAILURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/machine_protocol_runtime_failure.m
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_machine_protocol.cmake)

add_test(NAME machine_protocol_production_cli_smoke
    COMMAND mparser --run --result-format=json-v1
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/machine_protocol_demo.m)
set_tests_properties(machine_protocol_production_cli_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "\"status\":\"succeeded\".*\"execution\"")

add_executable(performance_baseline_schema_smoke_tests
    tests/performance_baseline_schema_smoke.cpp)
target_link_libraries(performance_baseline_schema_smoke_tests PRIVATE
    mparser_json_schema_validator)
add_executable(performance_environment_smoke_tests
    tests/performance_environment_smoke.cpp
    tools/performance_environment.cpp)
target_include_directories(performance_environment_smoke_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools)
add_test(NAME performance_environment_smoke
    COMMAND performance_environment_smoke_tests)
set_tests_properties(performance_environment_smoke PROPERTIES
    LABELS performance
    PASS_REGULAR_EXPRESSION
        "MParser performance environment validated")
add_test(NAME performance_baseline_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_BASELINE_TOOL=$<TARGET_FILE:mparser_performance_baseline>
        -DMPARSER_CLI=$<TARGET_FILE:mparser>
        -DMPARSER_C_API_LIBRARY=$<TARGET_FILE:mparser_c_api>
        -DMPARSER_SCHEMA_VALIDATOR=$<TARGET_FILE:performance_baseline_schema_smoke_tests>
        -DMPARSER_SCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/performance-baseline-v1.schema.json
        -DMPARSER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance_baseline_quick.m
        -DMPARSER_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/performance-baseline
        "-DMPARSER_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_performance_baseline.cmake)
set_tests_properties(performance_baseline_smoke PROPERTIES
    LABELS performance
    PASS_REGULAR_EXPRESSION
        "performance baseline protocol 1.0 validated"
    TIMEOUT 180)

add_test(NAME cross_platform_performance_evidence_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_SCHEMA_VALIDATOR=$<TARGET_FILE:performance_baseline_schema_smoke_tests>
        -DMPARSER_SCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/performance-baseline-v1.schema.json
        -DMPARSER_EVIDENCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/actions-30691616946
        -DMPARSER_SCALAR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_scalar_loop.m
        -DMPARSER_ARRAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_array_workload.m
        -DMPARSER_EXPECTED_REVISION=85685b88f8f8eb4e89b03abf53aa16dbbe60c68c
        -DMPARSER_EXPECTED_VERSION=${MPARSER_V1_PERFORMANCE_EVIDENCE_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cross_platform_performance_evidence.cmake)
set_tests_properties(cross_platform_performance_evidence_smoke PROPERTIES
    LABELS "documentation;performance;release"
    PASS_REGULAR_EXPRESSION
        "MParser cross-platform performance evidence validated: 8 reports"
    TIMEOUT 120)

add_test(NAME linux_arm64_performance_evidence_smoke
    COMMAND ${CMAKE_COMMAND}
        -DMPARSER_SCHEMA_VALIDATOR=$<TARGET_FILE:performance_baseline_schema_smoke_tests>
        -DMPARSER_SCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/performance-baseline-v1.schema.json
        -DMPARSER_EVIDENCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/docs/baselines/v1.0/actions-30732814590
        -DMPARSER_SCALAR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_scalar_loop.m
        -DMPARSER_ARRAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_array_workload.m
        -DMPARSER_EXPECTED_REVISION=3ec2bc074387283785068fe6ac98b4472a109a3a
        -DMPARSER_EXPECTED_VERSION=${MPARSER_V1_PERFORMANCE_EVIDENCE_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_linux_arm64_performance_evidence.cmake)
set_tests_properties(linux_arm64_performance_evidence_smoke PROPERTIES
    LABELS "documentation;performance;release"
    PASS_REGULAR_EXPRESSION
        "MParser Linux ARM64 performance evidence validated: 2 reports"
    TIMEOUT 120)

if(MPARSER_ENABLE_NATIVE_JIT AND NOT CMAKE_CROSSCOMPILING)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}"
        _mparser_performance_processor)
    if(_mparser_performance_processor MATCHES
       "^(amd64|x86_64|x64)$")
        set(_mparser_performance_architecture "x86_64")
    elseif(_mparser_performance_processor MATCHES
           "^(aarch64|arm64)$")
        set(_mparser_performance_architecture "aarch64")
    elseif(_mparser_performance_processor MATCHES "^(x86|i[3-6]86)$")
        set(_mparser_performance_architecture "x86")
    elseif(_mparser_performance_processor MATCHES "^arm")
        set(_mparser_performance_architecture "arm")
    else()
        set(_mparser_performance_architecture "unknown")
    endif()

    set(_mparser_performance_evidence_command
        "${CMAKE_COMMAND}"
        "-DMPARSER_BASELINE_TOOL=$<TARGET_FILE:mparser_performance_baseline>"
        "-DMPARSER_CLI=$<TARGET_FILE:mparser>"
        "-DMPARSER_C_API_LIBRARY=$<TARGET_FILE:mparser_c_api>"
        "-DMPARSER_SCHEMA_VALIDATOR=$<TARGET_FILE:performance_baseline_schema_smoke_tests>"
        "-DMPARSER_SCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/performance-baseline-v1.schema.json"
        "-DMPARSER_SCALAR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_scalar_loop.m"
        "-DMPARSER_ARRAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/samples/performance_array_workload.m"
        "-DMPARSER_OUTPUT_DIR=${MPARSER_PERFORMANCE_EVIDENCE_OUTPUT_DIR}"
        "-DMPARSER_REVISION=${MPARSER_PERFORMANCE_EVIDENCE_REVISION}"
        "-DMPARSER_EXPECTED_OS=${CMAKE_SYSTEM_NAME}"
        "-DMPARSER_EXPECTED_ARCHITECTURE=${_mparser_performance_architecture}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/collect_performance_evidence.cmake")

    add_custom_target(mparser_performance_evidence
        COMMAND ${_mparser_performance_evidence_command}
        DEPENDS
            mparser_performance_baseline
            performance_baseline_schema_smoke_tests
            mparser
            mparser_c_api
        COMMENT "Collecting validated native performance evidence"
        VERBATIM)

    add_test(NAME performance_evidence_contract_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMPARSER_BASELINE_TOOL=$<TARGET_FILE:mparser_performance_baseline>"
            "-DMPARSER_CLI=$<TARGET_FILE:mparser>"
            "-DMPARSER_C_API_LIBRARY=$<TARGET_FILE:mparser_c_api>"
            "-DMPARSER_SCHEMA_VALIDATOR=$<TARGET_FILE:performance_baseline_schema_smoke_tests>"
            "-DMPARSER_SCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/docs/performance-baseline-v1.schema.json"
            "-DMPARSER_SCALAR_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance_baseline_quick.m"
            "-DMPARSER_ARRAY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance_baseline_quick.m"
            "-DMPARSER_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/performance-evidence-smoke"
            -DMPARSER_REVISION=ctest-working-tree
            "-DMPARSER_EXPECTED_OS=${CMAKE_SYSTEM_NAME}"
            "-DMPARSER_EXPECTED_ARCHITECTURE=${_mparser_performance_architecture}"
            -DMPARSER_QUICK=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/collect_performance_evidence.cmake")
    set_tests_properties(performance_evidence_contract_smoke PROPERTIES
        LABELS performance
        PASS_REGULAR_EXPRESSION
            "performance evidence collected: 2 native reports"
        TIMEOUT 180)
endif()

add_executable(mparser_embedding_execution_demo
    samples/embedding_execution_demo.cpp)
target_link_libraries(mparser_embedding_execution_demo PRIVATE
    mparser_core)
add_test(NAME embedding_execution_demo_smoke
    COMMAND mparser_embedding_execution_demo)
set_tests_properties(embedding_execution_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "summary = 5075")

add_executable(mparser_embedding_resource_control_demo
    samples/embedding_resource_control_demo.cpp)
target_link_libraries(mparser_embedding_resource_control_demo PRIVATE
    mparser_core)
add_test(NAME embedding_resource_control_demo_smoke
    COMMAND mparser_embedding_resource_control_demo)
set_tests_properties(embedding_resource_control_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "summary = instruction-limit,cancelled,42")

add_executable(c_api_smoke_tests tests/c_api_smoke.c)
target_link_libraries(c_api_smoke_tests PRIVATE mparser_c_api)
target_compile_definitions(c_api_smoke_tests PRIVATE
    MPARSER_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MPARSER_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MPARSER_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH})
set_target_properties(c_api_smoke_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
add_test(NAME c_api_smoke
    COMMAND c_api_smoke_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib)

add_executable(c_api_allocation_failure_tests
    tests/c_api_allocation_failure.cpp
    src/mparser/c_api.cpp)
target_include_directories(c_api_allocation_failure_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(c_api_allocation_failure_tests PRIVATE
    mparser_core)
target_compile_definitions(c_api_allocation_failure_tests PRIVATE
    MPARSER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MPARSER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MPARSER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    MPARSER_C_ABI_GENERATION_EXPECTED=${MPARSER_C_ABI_GENERATION}
    MPARSER_C_ABI_REVISION_EXPECTED=${MPARSER_C_ABI_REVISION})
add_test(NAME c_api_allocation_failure
    COMMAND c_api_allocation_failure_tests)
set_tests_properties(c_api_allocation_failure PROPERTIES
    PASS_REGULAR_EXPRESSION
        "c api allocation failure = protocol,compile,value,token,session,execute,output")

add_executable(c_api_named_fault_smoke_tests
    tests/c_api_named_fault_smoke.cpp
    src/mparser/c_api.cpp)
target_include_directories(c_api_named_fault_smoke_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(c_api_named_fault_smoke_tests PRIVATE
    mparser_core Threads::Threads)
target_compile_definitions(c_api_named_fault_smoke_tests PRIVATE
    MPARSER_C_API_TEST_FAULTS=1
    MPARSER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MPARSER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MPARSER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    MPARSER_C_ABI_GENERATION_EXPECTED=${MPARSER_C_ABI_GENERATION}
    MPARSER_C_ABI_REVISION_EXPECTED=${MPARSER_C_ABI_REVISION})
add_test(NAME c_api_named_fault_smoke
    COMMAND c_api_named_fault_smoke_tests)
set_tests_properties(c_api_named_fault_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "c api named faults = allocation,internal,thread-local,nontransactional")

add_executable(c_api_lifecycle_stress_tests
    tests/c_api_lifecycle_stress.cpp)
target_link_libraries(c_api_lifecycle_stress_tests PRIVATE
    mparser_c_api Threads::Threads)
add_test(NAME c_api_lifecycle_stress
    COMMAND c_api_lifecycle_stress_tests)
set_tests_properties(c_api_lifecycle_stress PROPERTIES
    PASS_REGULAR_EXPRESSION
        "c api lifecycle stress = 160000,retained-session,retained-handle"
    TIMEOUT 120)

add_executable(embedding_unload_stress_tests
    tests/embedding_unload_stress.cpp)
add_dependencies(embedding_unload_stress_tests mparser_c_api)
target_link_libraries(embedding_unload_stress_tests PRIVATE
    ${CMAKE_DL_LIBS})
add_test(NAME embedding_unload_stress
    COMMAND embedding_unload_stress_tests
        $<TARGET_FILE:mparser_c_api>)
set_tests_properties(embedding_unload_stress PROPERTIES
    PASS_REGULAR_EXPRESSION
        "embedding unload stress = 256,abi-generation-2-revision-0"
    TIMEOUT 120)

add_test(NAME c_api_shared_library_abi
    COMMAND "${CMAKE_COMMAND}"
        "-DMPARSER_SHARED_LIBRARY=$<TARGET_FILE:mparser_c_api>"
        "-DMPARSER_SYMBOL_LIST=${MPARSER_C_ABI_SYMBOL_LIST}"
        "-DMPARSER_ABI_SOVERSION=${MPARSER_C_ABI_GENERATION}"
        "-DMPARSER_LINKER=${CMAKE_LINKER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_shared_library_abi.cmake")
set_tests_properties(c_api_shared_library_abi PROPERTIES
    PASS_REGULAR_EXPRESSION
        "MParser shared C ABI validated: 109 symbols, SOVERSION 2")

add_executable(cpp_api_smoke_tests
    tests/cpp_api_smoke.cpp)
target_link_libraries(cpp_api_smoke_tests PRIVATE
    mparser_cpp_api)
add_test(NAME cpp_api_smoke
    COMMAND cpp_api_smoke_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib)
set_tests_properties(cpp_api_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "cpp api smoke = 5050,42,21,abi-generation-2-revision-0,cpp-api-1.2")

add_executable(cpp_api_concurrency_stress_tests
    tests/cpp_api_concurrency_stress.cpp)
target_link_libraries(cpp_api_concurrency_stress_tests PRIVATE
    mparser_cpp_api Threads::Threads)
add_test(NAME cpp_api_concurrency_stress
    COMMAND cpp_api_concurrency_stress_tests)
set_tests_properties(cpp_api_concurrency_stress PROPERTIES
    PASS_REGULAR_EXPRESSION
        "cpp api concurrency stress = 800,800,200,50,mutated-400,cancelled-8,limited-8"
    TIMEOUT 120)

add_executable(mparser_cpp_embedding_demo
    samples/cpp_embedding_demo.cpp)
target_link_libraries(mparser_cpp_embedding_demo PRIVATE
    mparser_cpp_api)
add_test(NAME cpp_embedding_demo_smoke
    COMMAND mparser_cpp_embedding_demo)
set_tests_properties(cpp_embedding_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "cpp sdk = 5050,5,abi-generation-2-revision-0,cpp-api-1.2,host-output-2-2")

add_executable(c_api_layout_contract_tests
    tests/c_api_layout_contract.c)
target_include_directories(c_api_layout_contract_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(c_api_layout_contract_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
add_test(NAME c_api_layout_contract
    COMMAND c_api_layout_contract_tests)
set_tests_properties(c_api_layout_contract PROPERTIES
    PASS_REGULAR_EXPRESSION
        "c api layout contract = api-1.2,abi-generation-2,64-bit")

add_executable(c_api_utf8_source_graph_smoke_tests
    tests/c_api_utf8_source_graph_smoke.cpp)
target_link_libraries(c_api_utf8_source_graph_smoke_tests PRIVATE
    mparser_c_api)
add_test(NAME c_api_utf8_source_graph_smoke
    COMMAND c_api_utf8_source_graph_smoke_tests)

add_executable(mparser_c_embedding_demo
    samples/c_embedding_demo.c)
target_link_libraries(mparser_c_embedding_demo PRIVATE
    mparser_c_api)
set_target_properties(mparser_c_embedding_demo PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
add_test(NAME c_embedding_demo_smoke
    COMMAND mparser_c_embedding_demo)
set_tests_properties(c_embedding_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "summary = 5050,instruction-limit,42,host-output-2-2")

add_executable(mparser_c_abi_compat_demo
    samples/c_abi_compat_demo.c)
target_link_libraries(mparser_c_abi_compat_demo PRIVATE
    mparser_c_api)
set_target_properties(mparser_c_abi_compat_demo PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
add_test(NAME c_abi_compat_demo_smoke
    COMMAND mparser_c_abi_compat_demo)
set_tests_properties(c_abi_compat_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "api = 1.2, abi-generation = 2, revision = 0, result = 42, request-bytes = ")

add_executable(mparser_c_source_graph_demo
    samples/c_source_graph_demo.c)
target_link_libraries(mparser_c_source_graph_demo PRIVATE
    mparser_c_api)
set_target_properties(mparser_c_source_graph_demo PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
add_test(NAME c_source_graph_demo_smoke
    COMMAND mparser_c_source_graph_demo
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/app/run_demo.m
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/class_folders/lib)
set_tests_properties(c_source_graph_demo_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION
        "summary = 21,107,12,12")

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME installed_c_consumer_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMPARSER_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMPARSER_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DMPARSER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DMPARSER_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_c_consumer"
            "-DMPARSER_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installed-consumer-smoke/$<CONFIG>"
            "-DMPARSER_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
            "-DMPARSER_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DMPARSER_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
            "-DMPARSER_INSTALL_DOCDIR=${CMAKE_INSTALL_DOCDIR}"
            "-DMPARSER_INSTALL_CMAKEDIR=${MPARSER_INSTALL_CMAKEDIR}"
            "-DMPARSER_GENERATOR=${CMAKE_GENERATOR}"
            "-DMPARSER_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DMPARSER_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DMPARSER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DMPARSER_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DMPARSER_RC_COMPILER=${CMAKE_RC_COMPILER}"
            "-DMPARSER_MT=${CMAKE_MT}"
            "-DMPARSER_TEST_CONFIG=$<CONFIG>"
            "-DMPARSER_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DMPARSER_PROJECT_VERSION=${PROJECT_VERSION}"
            "-DMPARSER_AUTHENTICATION_EVIDENCE_DIRECTORY=${MPARSER_V1_AUTHENTICATION_EVIDENCE_DIRECTORY}"
            "-DMPARSER_TEST_RUNTIME_DLL=${MPARSER_MSVC_ASAN_RUNTIME_DLL}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_installed_c_consumer.cmake")
    set_tests_properties(installed_c_consumer_smoke PROPERTIES
        TIMEOUT 180)

    add_test(NAME installed_cpp_consumer_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DMPARSER_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMPARSER_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            "-DMPARSER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DMPARSER_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_cpp_consumer"
            "-DMPARSER_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/installed-cpp-consumer-smoke/$<CONFIG>"
            "-DMPARSER_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
            "-DMPARSER_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DMPARSER_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
            "-DMPARSER_INSTALL_DOCDIR=${CMAKE_INSTALL_DOCDIR}"
            "-DMPARSER_INSTALL_CMAKEDIR=${MPARSER_INSTALL_CMAKEDIR}"
            "-DMPARSER_GENERATOR=${CMAKE_GENERATOR}"
            "-DMPARSER_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DMPARSER_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DMPARSER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DMPARSER_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DMPARSER_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DMPARSER_RC_COMPILER=${CMAKE_RC_COMPILER}"
            "-DMPARSER_MT=${CMAKE_MT}"
            "-DMPARSER_TEST_CONFIG=$<CONFIG>"
            "-DMPARSER_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DMPARSER_PROJECT_VERSION=${PROJECT_VERSION}"
            "-DMPARSER_AUTHENTICATION_EVIDENCE_DIRECTORY=${MPARSER_V1_AUTHENTICATION_EVIDENCE_DIRECTORY}"
            "-DMPARSER_TEST_RUNTIME_DLL=${MPARSER_MSVC_ASAN_RUNTIME_DLL}"
            -DMPARSER_REQUIRE_CPP_SDK=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_installed_c_consumer.cmake")
    set_tests_properties(installed_cpp_consumer_smoke PROPERTIES
        TIMEOUT 180)

    if(MPARSER_ENABLE_RELEASE_PACKAGING)
        add_test(NAME release_archive_smoke
            COMMAND "${CMAKE_COMMAND}"
                "-DMPARSER_CMAKE_COMMAND=${CMAKE_COMMAND}"
                "-DMPARSER_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
                "-DMPARSER_CPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                "-DMPARSER_CPACK_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
                "-DMPARSER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DMPARSER_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DMPARSER_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/release-archive-smoke/$<CONFIG>"
                "-DMPARSER_RELEASE_PACKAGE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/create_release_package.cmake"
                "-DMPARSER_PROVENANCE_CREATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/create_release_provenance.cmake"
                "-DMPARSER_PROVENANCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_provenance.cmake"
                "-DMPARSER_GIT_COMMAND=${GIT_EXECUTABLE}"
                "-DMPARSER_PACKAGE_BASENAME=${MPARSER_RELEASE_PACKAGE_BASENAME}"
                "-DMPARSER_PACKAGE_EXTENSION=${MPARSER_RELEASE_PACKAGE_EXTENSION}"
                "-DMPARSER_ARCHIVE_MEDIA_TYPE=${MPARSER_RELEASE_ARCHIVE_MEDIA_TYPE}"
                "-DMPARSER_SOURCE_REPOSITORY=${MPARSER_RELEASE_SOURCE_REPOSITORY}"
                "-DMPARSER_BUILD_TYPE_URI=${MPARSER_RELEASE_BUILD_TYPE_URI}"
                "-DMPARSER_BUILDER_ID=${MPARSER_RELEASE_BUILDER_ID}"
                "-DMPARSER_NATIVE_JIT=${MPARSER_ENABLE_NATIVE_JIT}"
                "-DMPARSER_SOURCE_DATE_EPOCH=${MPARSER_RELEASE_SOURCE_DATE_EPOCH}"
                "-DMPARSER_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
                "-DMPARSER_INSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
                "-DMPARSER_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
                "-DMPARSER_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
                "-DMPARSER_INSTALL_DOCDIR=${CMAKE_INSTALL_DOCDIR}"
                "-DMPARSER_INSTALL_CMAKEDIR=${MPARSER_INSTALL_CMAKEDIR}"
                "-DMPARSER_GENERATOR=${CMAKE_GENERATOR}"
                "-DMPARSER_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DMPARSER_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DMPARSER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                "-DMPARSER_C_COMPILER=${CMAKE_C_COMPILER}"
                "-DMPARSER_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DMPARSER_RC_COMPILER=${CMAKE_RC_COMPILER}"
                "-DMPARSER_MT=${CMAKE_MT}"
                "-DMPARSER_TEST_CONFIG=$<CONFIG>"
                "-DMPARSER_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
                "-DMPARSER_PROJECT_VERSION=${PROJECT_VERSION}"
                "-DMPARSER_AUTHENTICATION_EVIDENCE_DIRECTORY=${MPARSER_V1_AUTHENTICATION_EVIDENCE_DIRECTORY}"
                "-DMPARSER_AUTHENTICATION_EVIDENCE_VERSION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_VERSION}"
                "-DMPARSER_AUTHENTICATION_EVIDENCE_REVISION=${MPARSER_V1_AUTHENTICATION_EVIDENCE_REVISION}"
                "-DMPARSER_AUTHENTICATION_EVIDENCE_RUN_ID=${MPARSER_V1_AUTHENTICATION_EVIDENCE_RUN_ID}"
                "-DMPARSER_CMAKE_VERSION=${CMAKE_VERSION}"
                "-DMPARSER_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
                "-DMPARSER_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
                "-DMPARSER_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
                "-DMPARSER_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}"
                "-DMPARSER_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
                "-DMPARSER_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                "-DMPARSER_C_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_c_consumer"
                "-DMPARSER_CPP_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/installed_cpp_consumer"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_archive.cmake")
        set_tests_properties(release_archive_smoke PROPERTIES
            LABELS release
            PASS_REGULAR_EXPRESSION
                "MParser release archive validated: ${MPARSER_RELEASE_PACKAGE_BASENAME}${MPARSER_RELEASE_PACKAGE_EXTENSION}"
            TIMEOUT 300)
    endif()
endif()

# The smoke executables use standard assert as a lightweight test primitive.
# Keep those contracts active in optimized CI and release-validation builds.
get_property(mparser_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(mparser_test_assertion_header
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_assertions_enabled.h")
foreach(mparser_target IN LISTS mparser_directory_targets)
    if(mparser_target MATCHES "_tests$")
        if(MSVC)
            target_compile_options(${mparser_target} PRIVATE
                "/FI${mparser_test_assertion_header}")
        else()
            target_compile_options(${mparser_target} PRIVATE
                -include "${mparser_test_assertion_header}")
        endif()
    endif()
endforeach()
endif()

# Apply one warning policy to every first-party compiled target. Bundled SLJIT
# keeps its upstream warning policy and third-party subdirectory targets are
# outside this directory target list.
get_property(mparser_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(mparser_target IN LISTS mparser_directory_targets)
    get_target_property(mparser_target_type ${mparser_target} TYPE)
    if(NOT mparser_target STREQUAL "mparser_sljit" AND
       mparser_target_type MATCHES
           "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
        mparser_enable_project_warnings(${mparser_target})
    endif()
endforeach()

configure_package_config_file(
    cmake/MParserConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MParserConfig.cmake"
    INSTALL_DESTINATION "${MPARSER_INSTALL_CMAKEDIR}"
    PATH_VARS
        CMAKE_INSTALL_INCLUDEDIR
        CMAKE_INSTALL_BINDIR
        CMAKE_INSTALL_DOCDIR)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MParserConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(TARGETS mparser_c_api mparser_cpp_api mparser
    EXPORT MParserTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES include/mparser/c_api.h include/mparser/cpp_api.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mparser")
install(EXPORT MParserTargets
    FILE MParserTargets.cmake
    NAMESPACE MParser::
    DESTINATION "${MPARSER_INSTALL_CMAKEDIR}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MParserConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MParserConfigVersion.cmake"
    DESTINATION "${MPARSER_INSTALL_CMAKEDIR}")
install(FILES
    LICENSE
    NOTICE
    README.md
    THIRD_PARTY_NOTICES.md
    docs/user-manual.md
    docs/build-and-install.md
    docs/support-matrix.md
    docs/cli-reference.md
    docs/jit-and-fallback.md
    docs/runtime-boundaries.md
    docs/migration-v1.0.md
    docs/release-process.md
    docs/release-authentication.md
    docs/release-notes-v1.0.md
    docs/roadmap-v1.x.md
    docs/public-contract-v1.json
    docs/cli-contract-v1.json
    docs/machine-result-v1.schema.json
    docs/performance-baseline-v1.schema.json
    docs/compatibility-matrix.json
    docs/roadmap-v1.0.md
    docs/v0.90.md
    docs/v0.90.1.md
    docs/v1.0.md
    docs/v1.1.md
    docs/v1.0-contract-freeze.md
    docs/v1.0-cross-platform-validation.md
    docs/v1.0-reliability.md
    docs/v1.0-performance-baseline.md
    docs/v1.0-jit-scope-decision.md
    docs/v1.0-documentation.md
    docs/architecture.md
    docs/c-abi-compatibility.md
    docs/embedding-c-api.md
    docs/embedding-cpp-api.md
    docs/extending-builtins.md
    docs/machine-result-protocol.md
    docs/versioning-and-deprecation.md
    docs/v1.x-external-gap-plan.md
    tests/public_contract/builtin/1.0/default_catalog.json
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES docs/README.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    RENAME documentation-index.md)
install(DIRECTORY docs/release-evidence/
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/release-evidence")
install(FILES
    samples/c_abi_compat_demo.c
    samples/c_embedding_demo.c
    samples/cpp_embedding_demo.cpp
    samples/c_source_graph_demo.c
    samples/machine_protocol_demo.m
    samples/host_integration_demo.m
    samples/numeric_types_demo.m
    samples/complex_numeric_demo.m
    samples/core_numeric_builtins_demo.m
    samples/performance_scalar_loop.m
    samples/performance_array_workload.m
    samples/reliability_boundary_demo.m
    samples/v1_1_core_compatibility_demo.m
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mparser/examples")
install(DIRECTORY samples/class_folders/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mparser/examples/class_folders"
    FILES_MATCHING PATTERN "*.m")

if(MPARSER_ENABLE_RELEASE_PACKAGING)
    set(CPACK_PACKAGE_NAME "mparser")
    set(CPACK_PACKAGE_VENDOR "Wang Xin")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Embeddable MATLAB-like subset runtime")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME
        "${MPARSER_RELEASE_PACKAGE_BASENAME}")
    set(CPACK_PACKAGE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/packages")
    set(CPACK_RESOURCE_FILE_LICENSE
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
    set(CPACK_MONOLITHIC_INSTALL ON)
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_THREADS 1)
    if(WIN32)
        set(CPACK_GENERATOR ZIP)
    else()
        set(CPACK_GENERATOR TGZ)
    endif()
    if(POLICY CMP0206)
        cmake_policy(SET CMP0206 NEW)
        set(CPACK_ARCHIVE_UID 0)
        set(CPACK_ARCHIVE_GID 0)
    endif()
    include(CPack)

    add_custom_target(mparser_release_package
        COMMAND "${CMAKE_COMMAND}"
            "-DMPARSER_CMAKE_COMMAND=${CMAKE_COMMAND}"
            "-DMPARSER_CPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
            "-DMPARSER_CPACK_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
            "-DMPARSER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DMPARSER_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/packages"
            "-DMPARSER_PROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DMPARSER_PROVENANCE_CREATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/create_release_provenance.cmake"
            "-DMPARSER_PROVENANCE_VALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_provenance.cmake"
            "-DMPARSER_GIT_COMMAND=${GIT_EXECUTABLE}"
            "-DMPARSER_PACKAGE_BASENAME=${MPARSER_RELEASE_PACKAGE_BASENAME}"
            "-DMPARSER_PACKAGE_EXTENSION=${MPARSER_RELEASE_PACKAGE_EXTENSION}"
            "-DMPARSER_ARCHIVE_MEDIA_TYPE=${MPARSER_RELEASE_ARCHIVE_MEDIA_TYPE}"
            "-DMPARSER_SOURCE_REPOSITORY=${MPARSER_RELEASE_SOURCE_REPOSITORY}"
            "-DMPARSER_BUILD_TYPE_URI=${MPARSER_RELEASE_BUILD_TYPE_URI}"
            "-DMPARSER_BUILDER_ID=${MPARSER_RELEASE_BUILDER_ID}"
            "-DMPARSER_PROJECT_VERSION=${PROJECT_VERSION}"
            "-DMPARSER_NATIVE_JIT=${MPARSER_ENABLE_NATIVE_JIT}"
            "-DMPARSER_SOURCE_DATE_EPOCH=${MPARSER_RELEASE_SOURCE_DATE_EPOCH}"
            "-DMPARSER_GENERATOR=${CMAKE_GENERATOR}"
            "-DMPARSER_CMAKE_VERSION=${CMAKE_VERSION}"
            "-DMPARSER_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DMPARSER_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
            "-DMPARSER_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
            "-DMPARSER_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}"
            "-DMPARSER_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
            -DMPARSER_REQUIRE_CLEAN_SOURCE=ON
            "-DMPARSER_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/create_release_package.cmake"
        DEPENDS mparser mparser_c_api
        USES_TERMINAL)
endif()
