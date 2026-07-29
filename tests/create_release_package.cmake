cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_CPACK_COMMAND
        MPARSER_CPACK_CONFIG
        MPARSER_BUILD_DIR
        MPARSER_OUTPUT_DIR
        MPARSER_PACKAGE_BASENAME
        MPARSER_PACKAGE_EXTENSION
        MPARSER_SOURCE_DATE_EPOCH)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-package variable: ${required_variable}")
    endif()
endforeach()

get_filename_component(
    mparser_build_dir "${MPARSER_BUILD_DIR}" ABSOLUTE)
get_filename_component(
    mparser_output_dir "${MPARSER_OUTPUT_DIR}" ABSOLUTE)
file(RELATIVE_PATH
    mparser_output_relative
    "${mparser_build_dir}" "${mparser_output_dir}")
if(mparser_output_relative STREQUAL "" OR
   IS_ABSOLUTE "${mparser_output_relative}" OR
   mparser_output_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "Release-package output must stay inside the build tree: "
        "${mparser_output_dir}")
endif()

file(REMOVE_RECURSE "${mparser_output_dir}")
file(MAKE_DIRECTORY "${mparser_output_dir}")
set(package_command
    "${MPARSER_CMAKE_COMMAND}" -E env
    "SOURCE_DATE_EPOCH=${MPARSER_SOURCE_DATE_EPOCH}"
    "${MPARSER_CPACK_COMMAND}"
    --config "${MPARSER_CPACK_CONFIG}"
    -B "${mparser_output_dir}")
if(DEFINED MPARSER_CONFIG AND NOT MPARSER_CONFIG STREQUAL "")
    list(APPEND package_command -C "${MPARSER_CONFIG}")
endif()
execute_process(
    COMMAND ${package_command}
    RESULT_VARIABLE package_status
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
if(NOT package_status EQUAL 0)
    message(FATAL_ERROR
        "CPack failed (${package_status})\n"
        "stdout:\n${package_output}\n"
        "stderr:\n${package_error}")
endif()

set(archive
    "${mparser_output_dir}/${MPARSER_PACKAGE_BASENAME}${MPARSER_PACKAGE_EXTENSION}")
set(sidecar "${archive}.sha256")
if(NOT EXISTS "${archive}" OR NOT EXISTS "${sidecar}")
    message(FATAL_ERROR
        "Expected release archive or SHA-256 sidecar is missing: "
        "${archive}")
endif()
file(SHA256 "${archive}" archive_hash)
get_filename_component(archive_name "${archive}" NAME)
file(WRITE "${mparser_output_dir}/SHA256SUMS"
    "${archive_hash}  ${archive_name}\n")
message(STATUS
    "MParser release package: ${archive_name} ${archive_hash}")
