cmake_minimum_required(VERSION 3.20)

set(MPARSER_PLATFORM_RECORDS
    "mparser-performance-0.90.0-linux-aarch64|Linux|aarch64|GNU")
set(MPARSER_EXPECTED_REPORT_COUNT 2)
set(MPARSER_EXPECTED_PLATFORM_COUNT 1)
set(MPARSER_EVIDENCE_LABEL
    "MParser Linux ARM64 performance evidence")

include("${CMAKE_CURRENT_LIST_DIR}/validate_cross_platform_performance_evidence.cmake")
