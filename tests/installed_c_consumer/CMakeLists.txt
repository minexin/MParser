cmake_minimum_required(VERSION 3.20)

project(MParserInstalledCConsumer LANGUAGES C)

if(DEFINED MPARSER_REQUIRED_VERSION)
    find_package(MParser ${MPARSER_REQUIRED_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS C CLI)
else()
    find_package(MParser CONFIG REQUIRED COMPONENTS C CLI)
endif()

if(NOT TARGET MParser::c_api OR NOT TARGET MParser::cli)
    message(FATAL_ERROR
        "The installed package must export MParser::c_api and MParser::cli")
endif()
if(NOT MParser_C_ABI_VERSION EQUAL 1)
    message(FATAL_ERROR
        "Unexpected installed C ABI version: ${MParser_C_ABI_VERSION}")
endif()
if(NOT MParser_C_ABI_REVISION EQUAL 1)
    message(FATAL_ERROR
        "Unexpected installed C ABI revision: ${MParser_C_ABI_REVISION}")
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
        "installed-consumer = ${MParser_VERSION},42,36,abi-1.1")

add_test(NAME installed_cli_version
    COMMAND MParser::cli --version)
set_tests_properties(installed_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "MParser ${MParser_VERSION}")
