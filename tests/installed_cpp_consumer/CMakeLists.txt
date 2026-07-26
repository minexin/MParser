cmake_minimum_required(VERSION 3.20)

project(MParserInstalledCppConsumer LANGUAGES CXX)

if(DEFINED MPARSER_REQUIRED_VERSION)
    find_package(MParser ${MPARSER_REQUIRED_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS CPP CLI)
else()
    find_package(MParser CONFIG REQUIRED COMPONENTS CPP CLI)
endif()

if(NOT TARGET MParser::cpp_api OR NOT TARGET MParser::cli)
    message(FATAL_ERROR
        "The installed package must export MParser::cpp_api and MParser::cli")
endif()
if(NOT MParser_C_ABI_VERSION EQUAL 1 OR
   MParser_C_ABI_REVISION LESS 1)
    message(FATAL_ERROR
        "Unexpected installed C ABI: ${MParser_C_ABI_VERSION}.${MParser_C_ABI_REVISION}")
endif()

add_executable(mparser_installed_cpp_consumer main.cpp)
target_link_libraries(mparser_installed_cpp_consumer PRIVATE MParser::cpp_api)
target_compile_features(mparser_installed_cpp_consumer PRIVATE cxx_std_20)
target_compile_definitions(mparser_installed_cpp_consumer PRIVATE
    MPARSER_EXPECTED_VERSION_MAJOR=${MParser_VERSION_MAJOR}
    MPARSER_EXPECTED_VERSION_MINOR=${MParser_VERSION_MINOR}
    MPARSER_EXPECTED_VERSION_PATCH=${MParser_VERSION_PATCH})

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
        "installed-cpp-consumer = ${MParser_VERSION},5050,5,abi-1.1")

add_test(NAME installed_cpp_cli_version
    COMMAND MParser::cli --version)
set_tests_properties(installed_cpp_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "MParser ${MParser_VERSION}")
