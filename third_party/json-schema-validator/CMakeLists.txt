add_library(mparser_json_schema_validator STATIC
    src/json-patch.cpp
    src/json-schema-draft7.json.cpp
    src/json-uri.cpp
    src/json-validator.cpp
    src/smtp-address-validator.cpp
    src/string-format-check.cpp)

target_include_directories(mparser_json_schema_validator
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../nlohmann_json/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(mparser_json_schema_validator PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(mparser_json_schema_validator PRIVATE /W3)
else()
    target_compile_options(mparser_json_schema_validator PRIVATE
        -Wall -Wextra)
endif()
