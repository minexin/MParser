cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS
        MPARSER_EVIDENCE_ROOT
        MPARSER_EXPECTED_VERSION
        MPARSER_EXPECTED_TAG
        MPARSER_EXPECTED_REVISION
        MPARSER_EXPECTED_RELEASE_ID
        MPARSER_EXPECTED_PUBLISHED_AT
        MPARSER_EXPECTED_RELEASE_CHECKSUM_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR
            "release publication evidence requires ${required}")
    endif()
endforeach()

set(manifest_path "${MPARSER_EVIDENCE_ROOT}/manifest.json")
set(release_checksums_name
    "mparser-${MPARSER_EXPECTED_VERSION}-release-assets.SHA256SUMS")
set(release_checksums_path
    "${MPARSER_EVIDENCE_ROOT}/${release_checksums_name}")
set(expected_entries
    README.md
    SHA256SUMS
    manifest.json
    "${release_checksums_name}")
file(GLOB actual_entries
    LIST_DIRECTORIES TRUE
    RELATIVE "${MPARSER_EVIDENCE_ROOT}"
    "${MPARSER_EVIDENCE_ROOT}/*")
list(SORT expected_entries)
list(SORT actual_entries)
if(NOT "${actual_entries}" STREQUAL "${expected_entries}")
    message(FATAL_ERROR
        "release publication evidence file set drifted\n"
        "actual: ${actual_entries}\nexpected: ${expected_entries}")
endif()

foreach(required_file IN ITEMS
        "${manifest_path}"
        "${release_checksums_path}"
        "${MPARSER_EVIDENCE_ROOT}/README.md"
        "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "release publication evidence is missing: ${required_file}")
    endif()
endforeach()

file(READ "${manifest_path}" manifest)
string(JSON schema_name GET "${manifest}" schema name)
string(JSON schema_major GET "${manifest}" schema major)
string(JSON version GET "${manifest}" release version)
string(JSON tag GET "${manifest}" release tag)
string(JSON revision GET "${manifest}" release source_revision)
string(JSON release_id GET "${manifest}" release id)
string(JSON release_url GET "${manifest}" release url)
string(JSON published_at GET "${manifest}" release published_at)
string(JSON declared_asset_count GET "${manifest}" release asset_count)
string(JSON declared_total_bytes GET "${manifest}" release total_bytes)
string(JSON checksum_name GET
    "${manifest}" release release_checksum_asset name)
string(JSON checksum_size GET
    "${manifest}" release release_checksum_asset size)
string(JSON checksum_sha256 GET
    "${manifest}" release release_checksum_asset sha256)

set(expected_release_url
    "https://github.com/minexin/MParser/releases/tag/${MPARSER_EXPECTED_TAG}")
if(NOT schema_name STREQUAL "mparser.release-publication-evidence" OR
   NOT schema_major EQUAL 1 OR
   NOT version STREQUAL MPARSER_EXPECTED_VERSION OR
   NOT tag STREQUAL MPARSER_EXPECTED_TAG OR
   NOT revision STREQUAL MPARSER_EXPECTED_REVISION OR
   NOT release_id STREQUAL MPARSER_EXPECTED_RELEASE_ID OR
   NOT release_url STREQUAL expected_release_url OR
   NOT published_at STREQUAL MPARSER_EXPECTED_PUBLISHED_AT OR
   NOT declared_asset_count EQUAL 32 OR
   NOT declared_total_bytes EQUAL 15194301 OR
   NOT checksum_name STREQUAL release_checksums_name OR
   NOT checksum_size EQUAL 3492 OR
   NOT checksum_sha256 STREQUAL
       MPARSER_EXPECTED_RELEASE_CHECKSUM_SHA256)
    message(FATAL_ERROR
        "release publication identity or aggregate metadata drifted")
endif()

file(SHA256 "${release_checksums_path}" actual_checksum_sha256)
file(SIZE "${release_checksums_path}" actual_checksum_size)
if(NOT actual_checksum_sha256 STREQUAL
       MPARSER_EXPECTED_RELEASE_CHECKSUM_SHA256 OR
   NOT actual_checksum_size EQUAL checksum_size)
    message(FATAL_ERROR
        "retained release-wide checksum asset drifted")
endif()

file(READ "${release_checksums_path}" release_checksums)
string(REPLACE "\r\n" "\n" release_checksums "${release_checksums}")
string(REGEX MATCHALL "[^\n]+\n" checksum_lines "${release_checksums}")
list(LENGTH checksum_lines checksum_line_count)
if(NOT checksum_line_count EQUAL 31)
    message(FATAL_ERROR
        "release-wide checksum asset must contain exactly 31 entries")
endif()

string(JSON asset_count LENGTH "${manifest}" assets)
if(NOT asset_count EQUAL declared_asset_count)
    message(FATAL_ERROR
        "release publication asset count does not match the manifest")
endif()

set(asset_names)
set(total_bytes 0)
set(release_checksum_records 0)
math(EXPR asset_last "${asset_count} - 1")
foreach(index RANGE 0 ${asset_last})
    string(JSON asset_name GET "${manifest}" assets ${index} name)
    string(JSON asset_size GET "${manifest}" assets ${index} size)
    string(JSON asset_sha256 GET "${manifest}" assets ${index} sha256)
    list(FIND asset_names "${asset_name}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "release publication manifest repeats asset: ${asset_name}")
    endif()
    list(APPEND asset_names "${asset_name}")
    math(EXPR total_bytes "${total_bytes} + ${asset_size}")

    string(LENGTH "${asset_sha256}" digest_length)
    if(NOT digest_length EQUAL 64 OR
       NOT asset_sha256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "release publication asset has invalid SHA-256: ${asset_name}")
    endif()

    if(asset_name STREQUAL release_checksums_name)
        math(EXPR release_checksum_records
            "${release_checksum_records} + 1")
        if(NOT asset_size EQUAL checksum_size OR
           NOT asset_sha256 STREQUAL checksum_sha256)
            message(FATAL_ERROR
                "release-wide checksum asset record drifted")
        endif()
    else()
        set(expected_line "${asset_sha256}  ${asset_name}\n")
        string(FIND "${release_checksums}" "${expected_line}" line_at)
        if(line_at EQUAL -1)
            message(FATAL_ERROR
                "release-wide checksum is missing asset: ${asset_name}")
        endif()
    endif()
endforeach()

if(NOT total_bytes EQUAL declared_total_bytes OR
   NOT release_checksum_records EQUAL 1)
    message(FATAL_ERROR
        "release publication aggregate bytes or checksum record drifted")
endif()

foreach(required_asset IN ITEMS
        mparser-1.0.0-authentication-manifest.json
        mparser-1.0.0-linux-aarch64.tar.gz
        mparser-1.0.0-linux-x86_64.tar.gz
        mparser-1.0.0-macos-arm64.tar.gz
        mparser-1.0.0-macos-x86_64.tar.gz
        mparser-1.0.0-windows-x86_64.zip)
    list(FIND asset_names "${required_asset}" required_index)
    if(required_index EQUAL -1)
        message(FATAL_ERROR
            "release publication manifest is missing ${required_asset}")
    endif()
endforeach()

foreach(expectation IN ITEMS
        api_digest_mismatches=0
        downloaded_asset_count=32
        downloaded_bytes=15194301
        download_hash_mismatches=0
        checksum_entries=31
        checksum_errors=0
        package_inputs_verified=5
        sigstore_subjects_verified=10
        windows_c_consumer_tests=2
        windows_cpp_consumer_tests=2)
    string(REPLACE "=" ";" expectation_parts "${expectation}")
    list(GET expectation_parts 0 field)
    list(GET expectation_parts 1 expected)
    string(JSON actual GET "${manifest}" verification ${field})
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR
            "release publication verification field drifted: ${field}")
    endif()
endforeach()
string(JSON machine_protocol GET
    "${manifest}" verification machine_protocol)
string(JSON class_folder_sample GET
    "${manifest}" verification class_folder_sample)
if(NOT machine_protocol STREQUAL "mparser.result/1.0" OR
   NOT class_folder_sample STREQUAL "passed")
    message(FATAL_ERROR
        "release publication CLI verification record drifted")
endif()

file(READ "${MPARSER_EVIDENCE_ROOT}/README.md" readme)
string(FIND "${readme}" "${expected_release_url}" release_url_at)
string(FIND "${readme}" "32 assets" asset_count_at)
string(FIND "${readme}" "15,194,301 bytes" asset_bytes_at)
if(release_url_at EQUAL -1 OR
   asset_count_at EQUAL -1 OR
   asset_bytes_at EQUAL -1)
    message(FATAL_ERROR
        "release publication README is missing the frozen release summary")
endif()

file(READ "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS" evidence_checksums)
string(REPLACE "\r\n" "\n" evidence_checksums "${evidence_checksums}")
foreach(evidence_file IN ITEMS
        README.md
        manifest.json
        "${release_checksums_name}")
    file(SHA256
        "${MPARSER_EVIDENCE_ROOT}/${evidence_file}"
        evidence_sha256)
    set(expected_line "${evidence_sha256}  ${evidence_file}\n")
    string(FIND "${evidence_checksums}" "${expected_line}" line_at)
    if(line_at EQUAL -1)
        message(FATAL_ERROR
            "publication evidence SHA256SUMS is missing ${evidence_file}")
    endif()
endforeach()
string(REGEX MATCHALL "[^\n]+\n"
    evidence_checksum_lines "${evidence_checksums}")
list(LENGTH evidence_checksum_lines evidence_checksum_count)
if(NOT evidence_checksum_count EQUAL 3)
    message(FATAL_ERROR
        "publication evidence SHA256SUMS must contain exactly three entries")
endif()

message(STATUS
    "MParser release publication evidence validated: v1.0.0, "
    "32 assets, 15194301 bytes")
