foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_CTEST_COMMAND
        MPARSER_BUILD_DIR
        MPARSER_CONSUMER_SOURCE_DIR
        MPARSER_TEST_ROOT
        MPARSER_INSTALL_INCLUDEDIR
        MPARSER_INSTALL_CMAKEDIR
        MPARSER_GENERATOR
        MPARSER_PROJECT_VERSION)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing installed-consumer variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_TEST_CONFIG)
    set(MPARSER_TEST_CONFIG "")
endif()

get_filename_component(
    mparser_build_dir "${MPARSER_BUILD_DIR}" ABSOLUTE)
get_filename_component(
    mparser_test_root "${MPARSER_TEST_ROOT}" ABSOLUTE)
file(RELATIVE_PATH
    mparser_test_relative "${mparser_build_dir}" "${mparser_test_root}")
if(mparser_test_relative STREQUAL "" OR
   IS_ABSOLUTE "${mparser_test_relative}" OR
   mparser_test_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "Installed-consumer test root must stay inside the build tree: "
        "${mparser_test_root}")
endif()

file(REMOVE_RECURSE "${mparser_test_root}")
file(MAKE_DIRECTORY "${mparser_test_root}")
set(mparser_initial_prefix "${mparser_test_root}/initial-prefix")
set(mparser_relocated_prefix "${mparser_test_root}/relocated-prefix")
set(mparser_consumer_build "${mparser_test_root}/consumer-build")
set(mparser_package_dir
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_CMAKEDIR}")

function(mparser_run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE command_result
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error)
    if(NOT command_result EQUAL 0)
        message(FATAL_ERROR
            "${description} failed (${command_result})\n"
            "stdout:\n${command_output}\n"
            "stderr:\n${command_error}")
    endif()
    if(NOT command_output STREQUAL "")
        string(STRIP "${command_output}" command_output)
        message(STATUS "${description}:\n${command_output}")
    endif()
endfunction()

set(mparser_install_command
    "${MPARSER_CMAKE_COMMAND}" --install "${mparser_build_dir}"
    --prefix "${mparser_initial_prefix}")
if(NOT MPARSER_TEST_CONFIG STREQUAL "")
    list(APPEND mparser_install_command
        --config "${MPARSER_TEST_CONFIG}")
endif()
mparser_run_checked("MParser install" ${mparser_install_command})

foreach(required_path IN ITEMS
        "${mparser_initial_prefix}/${MPARSER_INSTALL_INCLUDEDIR}/mparser/c_api.h"
        "${mparser_initial_prefix}/${MPARSER_INSTALL_CMAKEDIR}/MParserConfig.cmake"
        "${mparser_initial_prefix}/${MPARSER_INSTALL_CMAKEDIR}/MParserTargets.cmake")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Installed SDK artifact is missing: ${required_path}")
    endif()
endforeach()

file(RENAME
    "${mparser_initial_prefix}" "${mparser_relocated_prefix}")

set(mparser_configure_command
    "${MPARSER_CMAKE_COMMAND}"
    -S "${MPARSER_CONSUMER_SOURCE_DIR}"
    -B "${mparser_consumer_build}"
    -G "${MPARSER_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${mparser_relocated_prefix}"
    "-DMParser_DIR=${mparser_package_dir}"
    "-DMPARSER_REQUIRED_VERSION=${MPARSER_PROJECT_VERSION}")
if(DEFINED MPARSER_GENERATOR_PLATFORM AND
   NOT MPARSER_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND mparser_configure_command
        -A "${MPARSER_GENERATOR_PLATFORM}")
endif()
if(DEFINED MPARSER_GENERATOR_TOOLSET AND
   NOT MPARSER_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND mparser_configure_command
        -T "${MPARSER_GENERATOR_TOOLSET}")
endif()
if(DEFINED MPARSER_BUILD_TYPE AND
   NOT MPARSER_BUILD_TYPE STREQUAL "")
    list(APPEND mparser_configure_command
        "-DCMAKE_BUILD_TYPE=${MPARSER_BUILD_TYPE}")
endif()
if(DEFINED MPARSER_MAKE_PROGRAM AND
   NOT MPARSER_MAKE_PROGRAM STREQUAL "")
    list(APPEND mparser_configure_command
        "-DCMAKE_MAKE_PROGRAM=${MPARSER_MAKE_PROGRAM}")
endif()
if(DEFINED MPARSER_C_COMPILER AND
   NOT MPARSER_C_COMPILER STREQUAL "")
    list(APPEND mparser_configure_command
        "-DCMAKE_C_COMPILER=${MPARSER_C_COMPILER}")
endif()
if(DEFINED MPARSER_RC_COMPILER AND
   NOT MPARSER_RC_COMPILER STREQUAL "")
    list(APPEND mparser_configure_command
        "-DCMAKE_RC_COMPILER=${MPARSER_RC_COMPILER}")
endif()
if(DEFINED MPARSER_MT AND NOT MPARSER_MT STREQUAL "")
    list(APPEND mparser_configure_command
        "-DCMAKE_MT=${MPARSER_MT}")
endif()

mparser_run_checked(
    "Installed consumer configure"
    ${mparser_configure_command})
set(mparser_build_command
    "${MPARSER_CMAKE_COMMAND}" --build "${mparser_consumer_build}"
    --parallel)
if(NOT MPARSER_TEST_CONFIG STREQUAL "")
    list(APPEND mparser_build_command
        --config "${MPARSER_TEST_CONFIG}")
endif()
mparser_run_checked(
    "Installed consumer build" ${mparser_build_command})

set(mparser_test_command
    "${MPARSER_CTEST_COMMAND}" --test-dir "${mparser_consumer_build}"
    --output-on-failure)
if(NOT MPARSER_TEST_CONFIG STREQUAL "")
    list(APPEND mparser_test_command
        -C "${MPARSER_TEST_CONFIG}")
endif()
mparser_run_checked(
    "Installed consumer tests" ${mparser_test_command})
