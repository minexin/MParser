cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_EVIDENCE_ROOT
        MPARSER_EXPECTED_VERSION
        MPARSER_EXPECTED_TAG
        MPARSER_EXPECTED_REVISION
        MPARSER_EXPECTED_RUN_ID)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "release authentication evidence requires "
            "${required_variable}")
    endif()
endforeach()

set(manifest_path "${MPARSER_EVIDENCE_ROOT}/manifest.json")
set(root_checksums_path "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS")
set(readme_path "${MPARSER_EVIDENCE_ROOT}/README.md")
foreach(required_file IN ITEMS
        "${manifest_path}"
        "${root_checksums_path}"
        "${readme_path}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "release authentication evidence is missing: "
            "${required_file}")
    endif()
endforeach()

set(expected_repository "https://github.com/minexin/MParser")
set(expected_workflow ".github/workflows/ci.yml")
set(expected_identity
    "https://github.com/minexin/MParser/.github/workflows/ci.yml@"
    "refs/tags/${MPARSER_EXPECTED_TAG}")
string(CONCAT expected_identity ${expected_identity})
set(expected_issuer "https://token.actions.githubusercontent.com")
set(expected_run_url
    "https://github.com/minexin/MParser/actions/runs/"
    "${MPARSER_EXPECTED_RUN_ID}")
string(CONCAT expected_run_url ${expected_run_url})

file(READ "${manifest_path}" manifest_json)
string(JSON format_name GET "${manifest_json}" format name)
string(JSON format_major GET "${manifest_json}" format major)
string(JSON format_minor GET "${manifest_json}" format minor)
string(JSON candidate_version GET
    "${manifest_json}" candidate version)
string(JSON candidate_tag GET "${manifest_json}" candidate tag)
string(JSON candidate_repository GET
    "${manifest_json}" candidate repository)
string(JSON candidate_revision GET
    "${manifest_json}" candidate revision)
if(NOT format_name STREQUAL
       "mparser.release-authentication-evidence" OR
   NOT format_major EQUAL 1 OR
   NOT format_minor EQUAL 0 OR
   NOT candidate_version STREQUAL MPARSER_EXPECTED_VERSION OR
   NOT candidate_tag STREQUAL MPARSER_EXPECTED_TAG OR
   NOT candidate_repository STREQUAL expected_repository OR
   NOT candidate_revision STREQUAL MPARSER_EXPECTED_REVISION)
    message(FATAL_ERROR
        "release authentication candidate identity drifted")
endif()

string(JSON workflow_path GET "${manifest_json}" workflow path)
string(JSON workflow_event GET "${manifest_json}" workflow event)
string(JSON workflow_authenticate GET
    "${manifest_json}" workflow authenticate_release)
string(JSON workflow_run_id GET "${manifest_json}" workflow run_id)
string(JSON workflow_run_attempt GET
    "${manifest_json}" workflow run_attempt)
string(JSON workflow_url GET "${manifest_json}" workflow url)
string(JSON workflow_conclusion GET
    "${manifest_json}" workflow conclusion)
string(JSON execution_job_count LENGTH
    "${manifest_json}" workflow execution_jobs)
string(JSON authentication_job_id GET
    "${manifest_json}" workflow authentication_job id)
string(JSON authentication_job_conclusion GET
    "${manifest_json}" workflow authentication_job conclusion)
if(NOT workflow_path STREQUAL expected_workflow OR
   NOT workflow_event STREQUAL "workflow_dispatch" OR
   NOT workflow_authenticate OR
   NOT workflow_run_id EQUAL MPARSER_EXPECTED_RUN_ID OR
   NOT workflow_run_attempt EQUAL 1 OR
   NOT workflow_url STREQUAL expected_run_url OR
   NOT workflow_conclusion STREQUAL "success" OR
   NOT execution_job_count EQUAL 7 OR
   NOT authentication_job_id EQUAL 91485433297 OR
   NOT authentication_job_conclusion STREQUAL "success")
    message(FATAL_ERROR
        "release authentication workflow identity drifted")
endif()

set(expected_execution_jobs
    91483748138
    91483748140
    91483748154
    91483748158
    91483748168
    91483748179
    91483748185)
set(actual_execution_jobs)
math(EXPR execution_job_last "${execution_job_count} - 1")
foreach(job_index RANGE 0 ${execution_job_last})
    string(JSON job_id GET
        "${manifest_json}" workflow execution_jobs ${job_index})
    list(APPEND actual_execution_jobs "${job_id}")
endforeach()
list(SORT expected_execution_jobs)
list(SORT actual_execution_jobs)
if(NOT actual_execution_jobs STREQUAL expected_execution_jobs)
    message(FATAL_ERROR
        "release authentication execution-job set drifted")
endif()

string(JSON signing_identity GET
    "${manifest_json}" signing identity)
string(JSON signing_issuer GET
    "${manifest_json}" signing oidc_issuer)
string(JSON installer_commit GET
    "${manifest_json}" signing cosign_installer_commit)
string(JSON cosign_version GET
    "${manifest_json}" signing cosign_version)
string(JSON bundle_media_type GET
    "${manifest_json}" signing bundle_media_type)
string(JSON signed_subjects GET
    "${manifest_json}" signing signed_subjects)
string(JSON workflow_verified_subjects GET
    "${manifest_json}" signing workflow_verified_subjects)
if(NOT signing_identity STREQUAL expected_identity OR
   NOT signing_issuer STREQUAL expected_issuer OR
   NOT installer_commit STREQUAL
       "6f9f17788090df1f26f669e9d70d6ae9567deba6" OR
   NOT cosign_version STREQUAL "3.0.6" OR
   NOT bundle_media_type STREQUAL
       "application/vnd.dev.sigstore.bundle.v0.3+json" OR
   NOT signed_subjects EQUAL 10 OR
   NOT workflow_verified_subjects EQUAL 10)
    message(FATAL_ERROR
        "release authentication signing contract drifted")
endif()

string(JSON artifact_id GET
    "${manifest_json}" actions_artifact id)
string(JSON artifact_name GET
    "${manifest_json}" actions_artifact name)
string(JSON artifact_size GET
    "${manifest_json}" actions_artifact size_bytes)
string(JSON artifact_sha256 GET
    "${manifest_json}" actions_artifact download_sha256)
string(JSON artifact_platform_count GET
    "${manifest_json}" actions_artifact exact_platform_directories)
string(JSON artifact_file_count GET
    "${manifest_json}" actions_artifact exact_files)
if(NOT artifact_id EQUAL 8832142356 OR
   NOT artifact_name STREQUAL
       "mparser-0.90.1-authenticated-v0.90.1" OR
   NOT artifact_size EQUAL 14858931 OR
   NOT artifact_sha256 STREQUAL
       "7460347da20b7b0a4b4cf041c965485c09d7622c7e349c5459f480daed2460d0" OR
   NOT artifact_platform_count EQUAL 5 OR
   NOT artifact_file_count EQUAL 30)
    message(FATAL_ERROR
        "release authentication Actions artifact identity drifted")
endif()

string(JSON independent_inputs GET
    "${manifest_json}" independent_verification validated_package_inputs)
string(JSON independent_verifier GET
    "${manifest_json}" independent_verification verifier)
string(JSON independent_version GET
    "${manifest_json}" independent_verification verifier_version)
string(JSON independent_mode GET
    "${manifest_json}" independent_verification mode)
string(JSON independent_subjects GET
    "${manifest_json}" independent_verification verified_subjects)
if(NOT independent_inputs EQUAL 5 OR
   NOT independent_verifier STREQUAL "sigstore-python" OR
   NOT independent_version STREQUAL "4.5.0" OR
   NOT independent_mode STREQUAL "offline-bundle" OR
   NOT independent_subjects EQUAL 10)
    message(FATAL_ERROR
        "release authentication independent-verification record drifted")
endif()

function(validate_bundle
        bundle_path expected_bundle_sha256 expected_digest_base64)
    if(NOT EXISTS "${bundle_path}")
        message(FATAL_ERROR
            "release authentication bundle is missing: ${bundle_path}")
    endif()
    file(SHA256 "${bundle_path}" actual_bundle_sha256)
    if(NOT actual_bundle_sha256 STREQUAL expected_bundle_sha256)
        message(FATAL_ERROR
            "release authentication bundle digest mismatch: "
            "${bundle_path}")
    endif()

    file(READ "${bundle_path}" bundle_json)
    string(JSON media_type GET "${bundle_json}" mediaType)
    string(JSON digest_algorithm GET
        "${bundle_json}" messageSignature messageDigest algorithm)
    string(JSON digest_base64 GET
        "${bundle_json}" messageSignature messageDigest digest)
    string(JSON signature GET
        "${bundle_json}" messageSignature signature)
    string(JSON certificate GET
        "${bundle_json}" verificationMaterial certificate rawBytes)
    string(JSON tlog_count LENGTH
        "${bundle_json}" verificationMaterial tlogEntries)
    string(JSON timestamp_count LENGTH
        "${bundle_json}" verificationMaterial timestampVerificationData
        rfc3161Timestamps)
    if(NOT media_type STREQUAL
           "application/vnd.dev.sigstore.bundle.v0.3+json" OR
       NOT digest_algorithm STREQUAL "SHA2_256" OR
       NOT digest_base64 STREQUAL expected_digest_base64 OR
       signature STREQUAL "" OR
       certificate STREQUAL "" OR
       NOT tlog_count EQUAL 1 OR
       NOT timestamp_count EQUAL 1)
        message(FATAL_ERROR
            "release authentication bundle contract mismatch: "
            "${bundle_path}")
    endif()
endfunction()

set(platform_records
    "linux-aarch64|mparser-0.90.1-linux-aarch64|mparser-0.90.1-linux-aarch64.tar.gz|3095533|7a6919e2376197b760be279b07232c90bedffb2bb0a9819608905c9f4d12b79e|emkZ4jdhl7dgviebByMskL7f+yuwqYGWCJBcn00St54=|214b6ef9b70ca9f1ebdc6d7eb1980e9252e6b173c5bce64aa39a2032dede4514|8c9b428a13d9548167c4989798a1fea8a5fb541c1d61fb3be05c79ff1d43cf52|3969|27e63c6f9f9d2c8b56e2d713bd984e932175194b6efd8fd5cf084174668ab17c|J+Y8b5+dLItW4tcTvZhOkyF1GUtu/Y/VzwhBdGaKsXw=|6582fcb66ba16263ea1cf3a90345906dbacafc91576ba388b902a4a106b83f5b|92130d7d207669f872f23147bb069cbb1c04d3d86d0ff9ddbf271d7c259fcba0"
    "linux-x86_64|mparser-0.90.1-linux-x86_64|mparser-0.90.1-linux-x86_64.tar.gz|3525875|6acfabbb4bc83f86162e95260e0567b6843caeefd83d544629bfa937f7ab6572|as+ru0vIP4YWLpUmDgVntoQ8ru/YPVRGKb+pN/erZXI=|4261e5b5c76899cda65dd2bfee3aa333d8465c77212bc36fadb59c8403ba6ffa|30ff36f89700928367fe70786aaff51d72d18ee23bad0a7691ccb9b6c74ef8a2|3984|c3dfa7991d8ba5f8b141a4db165d40be0732e3ba09e538c60f769207f7a1d1c2|w9+nmR2LpfixQaTbFl1Avgcy47oJ5TjGD3aSB/eh0cI=|3c2f640a4c5e7f412b505b34c21c71add508810138085f545efc23f8eb81bd7e|0cc07971d37e84cc5ac04f05b7114cb26e12dc70f2193fc8f2d272b6bfd20794"
    "macos-arm64|mparser-0.90.1-macos-arm64|mparser-0.90.1-macos-arm64.tar.gz|2644223|e043a232f7240a595de96b728cb3a33b87c988e07527c67075748e165893fa3f|4EOiMvckClld6WtyjLOjO4fJiOB1J8ZwdXSOFliT+j8=|1bab5f62bb7ccf9340c0e5b3e08ad922f8072745ea6eadd1e79e8739317510b0|54ca51bc4ae24f74db730a230a65fb17452987f4322933b298d794aae8c16d4a|4012|48a762ef4eebfb5fe436ad03a3247b52a8674105a922af16abfecd95a9d27029|SKdi707r+1/kNq0DoyR7UqhnQQWpIq8Wq/7NlanScCk=|72572ec446b895666f2b763e58bcada79d17afad2af60293bb698d663d8c8923|0d9e72a2be017d5105b287274fa8c53b1bc26ed49117cc0e46f17ed3f941b282"
    "macos-x86_64|mparser-0.90.1-macos-x86_64|mparser-0.90.1-macos-x86_64.tar.gz|2862605|dee5abd30a59bc47ccc7cacfb862354d99a57e660670b36dd9d3e134f915eec6|3uWr0wpZvEfMx8rPuGI1TZmlfmYGcLNt2dPhNPkV7sY=|e45c64e256062ec674752ea71c39407b9497176c664d3a3cda8401af86af40f6|0426cda33f1a184da26bf8790ba53995bd5b46c6f038c0f16818fe2a377aecad|4014|b06df45d1772fd9dcd14ee4959fc1761fe57059c9f6379e11246696ea1758986|sG30XRdy/Z3NFO5JWfwXYf5XBZyfY3nhEkZpbqF1iYY=|1368ceeff8419661c6144104621072fb0c42fdce1cab1c38f658bbb7e64dfe6b|fc230928ac3f3a5f44b76cbae5abb434ee083fc2447e7a131acb241f21e60423"
    "windows-x86_64|mparser-0.90.1-windows-x86_64|mparser-0.90.1-windows-x86_64.zip|2671837|004a87b81be47ccd6e06ad9cc715cb7a0b67e1bf0b604a57e8ec3c7a7d16a617|AEqHuBvkfM1uBq2cxxXLegtn4b8LYEpX6Ow8en0Wphc=|114000976d8852bb02877414a50c9b22dd5e66f1fdb852a195e9381c1bb0ce31|613f6eae91fd512011b047a39b92b15478a31d9a6795ef43d37878e7e3cbc2d0|4147|e885a29b59dc9c7a70c7a23fd60cdc3c4c508ff9b488891ca99edd930efc9f44|6IWim1ncnHpwx6I/1gzcPExQj/m0iIkcqZ7dkw78n0Q=|76c7b3547b3576c4118bf907fccb5fbc865801409d8bc6d03bb79f566a754a3e|84d1d83e3fa09d4eaa381caf89853c4700b1ade1fd44bc76ed259d910c065540")

string(JSON platform_count LENGTH "${manifest_json}" platforms)
list(LENGTH platform_records expected_platform_count)
if(NOT platform_count EQUAL expected_platform_count OR
   NOT platform_count EQUAL 5)
    message(FATAL_ERROR
        "release authentication platform set drifted")
endif()

set(expected_evidence_paths
    "README.md"
    "SHA256SUMS"
    "manifest.json")
set(platform_index 0)
foreach(platform_record IN LISTS platform_records)
    string(REPLACE "|" ";" fields "${platform_record}")
    list(GET fields 0 expected_platform)
    list(GET fields 1 expected_directory)
    list(GET fields 2 expected_archive)
    list(GET fields 3 expected_archive_size)
    list(GET fields 4 expected_archive_sha256)
    list(GET fields 5 expected_archive_base64)
    list(GET fields 6 expected_checksum_sha256)
    list(GET fields 7 expected_archive_bundle_sha256)
    list(GET fields 8 expected_provenance_size)
    list(GET fields 9 expected_provenance_sha256)
    list(GET fields 10 expected_provenance_base64)
    list(GET fields 11 expected_provenance_bundle_sha256)
    list(GET fields 12 expected_checksums_sha256)

    string(JSON platform GET
        "${manifest_json}" platforms ${platform_index} platform)
    string(JSON directory GET
        "${manifest_json}" platforms ${platform_index} directory)
    string(JSON archive_name GET
        "${manifest_json}" platforms ${platform_index} archive name)
    string(JSON archive_size GET
        "${manifest_json}" platforms ${platform_index} archive size_bytes)
    string(JSON archive_sha256 GET
        "${manifest_json}" platforms ${platform_index} archive sha256)
    string(JSON archive_base64 GET
        "${manifest_json}" platforms ${platform_index} archive digest_base64)
    string(JSON archive_checked_in GET
        "${manifest_json}" platforms ${platform_index} archive checked_in)
    string(JSON checksum_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        archive checksum_file_sha256)
    string(JSON archive_bundle_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        archive bundle_sha256)
    string(JSON provenance_name GET
        "${manifest_json}" platforms ${platform_index} provenance name)
    string(JSON provenance_size GET
        "${manifest_json}" platforms ${platform_index}
        provenance size_bytes)
    string(JSON provenance_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        provenance sha256)
    string(JSON provenance_base64 GET
        "${manifest_json}" platforms ${platform_index}
        provenance digest_base64)
    string(JSON provenance_bundle_sha256 GET
        "${manifest_json}" platforms ${platform_index}
        provenance bundle_sha256)
    string(JSON checksums_sha256 GET
        "${manifest_json}" platforms ${platform_index} checksums_sha256)

    if(NOT platform STREQUAL expected_platform OR
       NOT directory STREQUAL expected_directory OR
       NOT archive_name STREQUAL expected_archive OR
       NOT archive_size EQUAL expected_archive_size OR
       NOT archive_sha256 STREQUAL expected_archive_sha256 OR
       NOT archive_base64 STREQUAL expected_archive_base64 OR
       archive_checked_in OR
       NOT checksum_sha256 STREQUAL expected_checksum_sha256 OR
       NOT archive_bundle_sha256 STREQUAL
           expected_archive_bundle_sha256 OR
       NOT provenance_name STREQUAL
           "${expected_archive}.provenance.json" OR
       NOT provenance_size EQUAL expected_provenance_size OR
       NOT provenance_sha256 STREQUAL expected_provenance_sha256 OR
       NOT provenance_base64 STREQUAL expected_provenance_base64 OR
       NOT provenance_bundle_sha256 STREQUAL
           expected_provenance_bundle_sha256 OR
       NOT checksums_sha256 STREQUAL expected_checksums_sha256)
        message(FATAL_ERROR
            "release authentication platform manifest mismatch: "
            "${expected_platform}")
    endif()

    set(platform_root
        "${MPARSER_EVIDENCE_ROOT}/${expected_directory}")
    set(checksum_path
        "${platform_root}/${expected_archive}.sha256")
    set(provenance_path
        "${platform_root}/${expected_archive}.provenance.json")
    set(archive_bundle_path
        "${platform_root}/${expected_archive}.sigstore.json")
    set(provenance_bundle_path
        "${provenance_path}.sigstore.json")
    set(checksums_path "${platform_root}/SHA256SUMS")
    foreach(required_platform_file IN ITEMS
            "${checksum_path}"
            "${provenance_path}"
            "${archive_bundle_path}"
            "${provenance_bundle_path}"
            "${checksums_path}")
        if(NOT EXISTS "${required_platform_file}")
            message(FATAL_ERROR
                "release authentication retained file is missing: "
                "${required_platform_file}")
        endif()
    endforeach()

    file(SHA256 "${checksum_path}" actual_checksum_sha256)
    file(SHA256 "${provenance_path}" actual_provenance_sha256)
    file(SHA256 "${checksums_path}" actual_checksums_sha256)
    if(NOT actual_checksum_sha256 STREQUAL expected_checksum_sha256 OR
       NOT actual_provenance_sha256 STREQUAL
           expected_provenance_sha256 OR
       NOT actual_checksums_sha256 STREQUAL expected_checksums_sha256)
        message(FATAL_ERROR
            "release authentication retained digest mismatch: "
            "${expected_platform}")
    endif()

    file(STRINGS "${checksum_path}" checksum_lines)
    list(LENGTH checksum_lines checksum_line_count)
    if(NOT checksum_line_count EQUAL 1)
        message(FATAL_ERROR
            "release authentication archive sidecar drifted: "
            "${expected_platform}")
    endif()
    list(GET checksum_lines 0 checksum_line)
    if(NOT checksum_line STREQUAL
       "${expected_archive_sha256}  ${expected_archive}")
        message(FATAL_ERROR
            "release authentication archive checksum mismatch: "
            "${expected_platform}")
    endif()

    file(STRINGS "${checksums_path}" platform_checksum_lines)
    list(LENGTH platform_checksum_lines platform_checksum_count)
    list(FIND platform_checksum_lines
        "${expected_archive_sha256}  ${expected_archive}"
        archive_checksum_index)
    string(CONCAT expected_provenance_checksum_line
        "${expected_provenance_sha256}  "
        "${expected_archive}.provenance.json")
    list(FIND platform_checksum_lines
        "${expected_provenance_checksum_line}"
        provenance_checksum_index)
    if(NOT platform_checksum_count EQUAL 2 OR
       archive_checksum_index EQUAL -1 OR
       provenance_checksum_index EQUAL -1)
        message(FATAL_ERROR
            "release authentication platform SHA256SUMS drifted: "
            "${expected_platform}")
    endif()

    file(READ "${provenance_path}" provenance_json)
    string(JSON statement_type GET "${provenance_json}" _type)
    string(JSON predicate_type GET
        "${provenance_json}" predicateType)
    string(JSON subject_count LENGTH "${provenance_json}" subject)
    string(JSON subject_name GET
        "${provenance_json}" subject 0 name)
    string(JSON subject_sha256 GET
        "${provenance_json}" subject 0 digest sha256)
    string(JSON subject_size GET
        "${provenance_json}" subject 0 annotations mparser_sizeBytes)
    string(JSON provenance_version GET
        "${provenance_json}" predicate buildDefinition
        externalParameters projectVersion)
    string(JSON provenance_repository GET
        "${provenance_json}" predicate buildDefinition
        externalParameters repository)
    string(JSON provenance_revision GET
        "${provenance_json}" predicate buildDefinition
        externalParameters revision)
    string(JSON provenance_configuration GET
        "${provenance_json}" predicate buildDefinition
        externalParameters configuration)
    string(JSON provenance_native_jit GET
        "${provenance_json}" predicate buildDefinition
        externalParameters nativeJit)
    string(JSON provenance_packaging GET
        "${provenance_json}" predicate buildDefinition
        externalParameters releasePackaging)
    string(JSON provenance_authentication GET
        "${provenance_json}" predicate runDetails
        mparser_authentication)
    string(JSON provenance_clean GET
        "${provenance_json}" predicate runDetails
        mparser_sourceTreeClean)
    string(JSON source_revision GET
        "${provenance_json}" predicate buildDefinition
        resolvedDependencies 0 digest gitCommit)
    if(NOT statement_type STREQUAL
           "https://in-toto.io/Statement/v1" OR
       NOT predicate_type STREQUAL
           "https://slsa.dev/provenance/v1" OR
       NOT subject_count EQUAL 1 OR
       NOT subject_name STREQUAL expected_archive OR
       NOT subject_sha256 STREQUAL expected_archive_sha256 OR
       NOT subject_size EQUAL expected_archive_size OR
       NOT provenance_version STREQUAL MPARSER_EXPECTED_VERSION OR
       NOT provenance_repository STREQUAL expected_repository OR
       NOT provenance_revision STREQUAL MPARSER_EXPECTED_REVISION OR
       NOT provenance_configuration STREQUAL "Release" OR
       NOT provenance_native_jit OR
       NOT provenance_packaging OR
       NOT provenance_authentication STREQUAL "unsigned" OR
       NOT provenance_clean OR
       NOT source_revision STREQUAL MPARSER_EXPECTED_REVISION)
        message(FATAL_ERROR
            "release authentication provenance contract mismatch: "
            "${expected_platform}")
    endif()

    validate_bundle(
        "${archive_bundle_path}"
        "${expected_archive_bundle_sha256}"
        "${expected_archive_base64}")
    validate_bundle(
        "${provenance_bundle_path}"
        "${expected_provenance_bundle_sha256}"
        "${expected_provenance_base64}")

    list(APPEND expected_evidence_paths
        "${expected_directory}/SHA256SUMS"
        "${expected_directory}/${expected_archive}.provenance.json"
        "${expected_directory}/${expected_archive}.provenance.json.sigstore.json"
        "${expected_directory}/${expected_archive}.sha256"
        "${expected_directory}/${expected_archive}.sigstore.json")
    math(EXPR platform_index "${platform_index} + 1")
endforeach()

file(GLOB_RECURSE actual_evidence_paths
    LIST_DIRECTORIES false
    RELATIVE "${MPARSER_EVIDENCE_ROOT}"
    "${MPARSER_EVIDENCE_ROOT}/*")
list(SORT actual_evidence_paths)
list(SORT expected_evidence_paths)
list(LENGTH actual_evidence_paths actual_evidence_count)
list(LENGTH expected_evidence_paths expected_evidence_count)
if(NOT actual_evidence_paths STREQUAL expected_evidence_paths OR
   NOT actual_evidence_count EQUAL 28 OR
   NOT expected_evidence_count EQUAL 28)
    message(FATAL_ERROR
        "release authentication checked-in evidence set drifted\n"
        "expected: ${expected_evidence_paths}\n"
        "actual: ${actual_evidence_paths}")
endif()

file(STRINGS "${root_checksums_path}" root_checksum_lines)
list(LENGTH root_checksum_lines root_checksum_count)
if(NOT root_checksum_count EQUAL 27)
    message(FATAL_ERROR
        "release authentication root SHA256SUMS count drifted: "
        "${root_checksum_count}")
endif()
foreach(relative_path IN LISTS expected_evidence_paths)
    if(relative_path STREQUAL "SHA256SUMS")
        continue()
    endif()
    file(SHA256
        "${MPARSER_EVIDENCE_ROOT}/${relative_path}"
        retained_sha256)
    list(FIND root_checksum_lines
        "${retained_sha256}  ${relative_path}"
        retained_index)
    if(retained_index EQUAL -1)
        message(FATAL_ERROR
            "release authentication root SHA256SUMS mismatch: "
            "${relative_path}")
    endif()
endforeach()

message(STATUS
    "MParser release authentication evidence validated: "
    "${MPARSER_EXPECTED_TAG}, run ${MPARSER_EXPECTED_RUN_ID}, "
    "5 platforms, 10 Sigstore subjects")
