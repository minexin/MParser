cmake_minimum_required(VERSION 3.20)

project(MParserInstalledCppConsumer LANGUAGES CXX)

if(DEFINED MPARSER_MINIMUM_VERSION)
    find_package(MParser ${MPARSER_MINIMUM_VERSION} CONFIG REQUIRED
        COMPONENTS CPP CLI)
elseif(DEFINED MPARSER_REQUIRED_VERSION)
    find_package(MParser ${MPARSER_REQUIRED_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS CPP CLI)
else()
    find_package(MParser CONFIG REQUIRED COMPONENTS CPP CLI)
endif()

if(DEFINED MPARSER_EXPECTED_ENGINE_VERSION AND
   NOT MParser_VERSION VERSION_EQUAL MPARSER_EXPECTED_ENGINE_VERSION)
    message(FATAL_ERROR
        "Unexpected installed engine version: ${MParser_VERSION}")
endif()

if(NOT TARGET MParser::cpp_api OR NOT TARGET MParser::cli)
    message(FATAL_ERROR
        "The installed package must export MParser::cpp_api and MParser::cli")
endif()
if(NOT MParser_C_ABI_GENERATION EQUAL 2 OR
   NOT MParser_C_ABI_REVISION EQUAL 0)
    message(FATAL_ERROR
        "Unexpected installed C ABI generation/revision: ${MParser_C_ABI_GENERATION}/${MParser_C_ABI_REVISION}")
endif()
if(NOT MParser_CPP_API_VERSION_MAJOR EQUAL 1 OR
   NOT MParser_CPP_API_VERSION_MINOR EQUAL 2)
    message(FATAL_ERROR
        "Unexpected C++ source API: ${MParser_CPP_API_VERSION_MAJOR}.${MParser_CPP_API_VERSION_MINOR}")
endif()
if(NOT MParser_MACHINE_PROTOCOL_NAME STREQUAL "mparser.result" OR
    NOT MParser_MACHINE_PROTOCOL_MAJOR EQUAL 1 OR
    NOT MParser_MACHINE_PROTOCOL_MINOR EQUAL 1)
    message(FATAL_ERROR
        "Unexpected machine protocol: ${MParser_MACHINE_PROTOCOL_NAME} ${MParser_MACHINE_PROTOCOL_MAJOR}.${MParser_MACHINE_PROTOCOL_MINOR}")
endif()
if(NOT MParser_CLI_CONTRACT_MAJOR EQUAL 1 OR
   NOT MParser_CLI_CONTRACT_MINOR EQUAL 0)
    message(FATAL_ERROR
        "Unexpected CLI contract: ${MParser_CLI_CONTRACT_MAJOR}.${MParser_CLI_CONTRACT_MINOR}")
endif()
if(NOT MParser_BUILTIN_SOURCE_CONTRACT_MAJOR EQUAL 1 OR
   NOT MParser_BUILTIN_SOURCE_CONTRACT_MINOR EQUAL 1)
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

add_executable(mparser_installed_cpp_consumer
    main.cpp
    consumer_module.cpp)
target_link_libraries(mparser_installed_cpp_consumer PRIVATE MParser::cpp_api)
target_compile_features(mparser_installed_cpp_consumer PRIVATE cxx_std_20)
target_compile_definitions(mparser_installed_cpp_consumer PRIVATE
    MPARSER_EXPECTED_VERSION_MAJOR=${MParser_VERSION_MAJOR}
    MPARSER_EXPECTED_VERSION_MINOR=${MParser_VERSION_MINOR}
    MPARSER_EXPECTED_VERSION_PATCH=${MParser_VERSION_PATCH}
    MPARSER_EXPECTED_CPP_API_VERSION_MAJOR=${MParser_CPP_API_VERSION_MAJOR}
    MPARSER_EXPECTED_CPP_API_VERSION_MINOR=${MParser_CPP_API_VERSION_MINOR})

if(WIN32)
    add_custom_command(TARGET mparser_installed_cpp_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:MParser::c_api>"
            "$<TARGET_FILE_DIR:mparser_installed_cpp_consumer>")
endif()

enable_testing()
add_test(NAME installed_cpp_api_runtime
    COMMAND mparser_installed_cpp_consumer)
set_tests_properties(installed_cpp_api_runtime PROPERTIES
    PASS_REGULAR_EXPRESSION
        "installed-cpp-consumer = ${MParser_VERSION},5050,5,abi-generation-2-revision-0,cpp-api-1.2,host-output-2-2")

add_test(NAME installed_cpp_cli_version
    COMMAND MParser::cli --version)
set_tests_properties(installed_cpp_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "MParser ${MParser_VERSION}")
