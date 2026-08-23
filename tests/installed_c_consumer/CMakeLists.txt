cmake_minimum_required(VERSION 3.20)

project(MParserInstalledCConsumer LANGUAGES C)

if(DEFINED MPARSER_MINIMUM_VERSION)
    find_package(MParser ${MPARSER_MINIMUM_VERSION} CONFIG REQUIRED
        COMPONENTS C CLI)
elseif(DEFINED MPARSER_REQUIRED_VERSION)
    find_package(MParser ${MPARSER_REQUIRED_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS C CLI)
else()
    find_package(MParser CONFIG REQUIRED COMPONENTS C CLI)
endif()

if(DEFINED MPARSER_EXPECTED_ENGINE_VERSION AND
   NOT MParser_VERSION VERSION_EQUAL MPARSER_EXPECTED_ENGINE_VERSION)
    message(FATAL_ERROR
        "Unexpected installed engine version: ${MParser_VERSION}")
endif()

if(NOT TARGET MParser::c_api OR NOT TARGET MParser::cli)
    message(FATAL_ERROR
        "The installed package must export MParser::c_api and MParser::cli")
endif()
if(NOT MParser_C_API_VERSION_MAJOR EQUAL 1 OR
   NOT MParser_C_API_VERSION_MINOR EQUAL 3)
    message(FATAL_ERROR
        "Unexpected installed C API: ${MParser_C_API_VERSION_MAJOR}.${MParser_C_API_VERSION_MINOR}")
endif()
if(NOT MParser_C_ABI_GENERATION EQUAL 2)
    message(FATAL_ERROR
        "Unexpected installed C ABI generation: ${MParser_C_ABI_GENERATION}")
endif()
if(NOT MParser_C_ABI_REVISION EQUAL 1)
    message(FATAL_ERROR
        "Unexpected installed C ABI revision: ${MParser_C_ABI_REVISION}")
endif()
if(NOT MParser_CLI_CONTRACT_MAJOR EQUAL 1 OR
   NOT MParser_CLI_CONTRACT_MINOR EQUAL 0)
    message(FATAL_ERROR
        "Unexpected CLI contract: ${MParser_CLI_CONTRACT_MAJOR}.${MParser_CLI_CONTRACT_MINOR}")
endif()
if(NOT MParser_BUILTIN_SOURCE_CONTRACT_MAJOR EQUAL 1 OR
   NOT MParser_BUILTIN_SOURCE_CONTRACT_MINOR EQUAL 10)
    message(FATAL_ERROR
        "Unexpected builtin source contract: ${MParser_BUILTIN_SOURCE_CONTRACT_MAJOR}.${MParser_BUILTIN_SOURCE_CONTRACT_MINOR}")
endif()
if(NOT MParser_LICENSE STREQUAL "Apache-2.0")
    message(FATAL_ERROR
        "Unexpected installed project license: ${MParser_LICENSE}")
endif()
if(NOT MParser_COPYRIGHT STREQUAL "Copyright 2026 Wang Xin")
    message(FATAL_ERROR
        "Unexpected installed project copyright: ${MParser_COPYRIGHT}")
endif()

add_executable(mparser_installed_c_consumer main.c)
target_link_libraries(mparser_installed_c_consumer PRIVATE MParser::c_api)
target_compile_definitions(mparser_installed_c_consumer PRIVATE
    MPARSER_EXPECTED_VERSION_MAJOR=${MParser_VERSION_MAJOR}
    MPARSER_EXPECTED_VERSION_MINOR=${MParser_VERSION_MINOR}
    MPARSER_EXPECTED_VERSION_PATCH=${MParser_VERSION_PATCH})
set_target_properties(mparser_installed_c_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

if(WIN32)
    add_custom_command(TARGET mparser_installed_c_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MParser::c_api>"
            "$<TARGET_FILE_DIR:mparser_installed_c_consumer>")
endif()

enable_testing()
add_test(NAME installed_c_api_runtime
    COMMAND mparser_installed_c_consumer)
set_tests_properties(installed_c_api_runtime PROPERTIES
    PASS_REGULAR_EXPRESSION
        "installed-consumer = ${MParser_VERSION},42,36,abi-generation-2-revision-1,host-output-2-2,system-context-rooted-retained")

add_test(NAME installed_cli_version
    COMMAND MParser::cli --version)
set_tests_properties(installed_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "MParser ${MParser_VERSION}")
