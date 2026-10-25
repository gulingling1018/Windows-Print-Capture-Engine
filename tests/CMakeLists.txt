# ── GoogleTest via FetchContent ──────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.16.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Contract tests ──────────────────────────────────────────────────────

add_executable(wpce_contract_tests contract_serialization_tests.cpp)
target_sources(wpce_contract_tests PRIVATE billing_storage_safety_tests.cpp)
target_sources(wpce_contract_tests PRIVATE capture_reliability_tests.cpp)
target_sources(wpce_contract_tests PRIVATE filter_registration_tests.cpp)

target_link_libraries(wpce_contract_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)
if(WIN32)
  target_link_libraries(wpce_contract_tests PRIVATE Psapi.lib)
endif()

target_include_directories(wpce_contract_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_contract_tests PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_contract_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_contract_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_contract_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Auto-discover tests from the GoogleTest executable.
include(GoogleTest)

if(WIN32)
  find_program(WPCE_POWERSHELL_EXECUTABLE NAMES powershell.exe REQUIRED)
  foreach(_case AtomicUnicode BackupFailure CopyFailure DryRun ExplicitRestart InvalidDll LegacySentinel
      MissingDll NoAdmin PartialFailure PendingRestart ProbeRevalidates RegisterFailure RollbackFailure
      StartFailure StartTimeoutFailure StopTimeoutFailure XmlCompatibility XmlIdempotency ZeroTargets
      MultipleTargetRollback RecordFailure SourceHashChange CliMissingDllDryRun BackupCorruption IdenticalDllDoesNotRewriteLoadedBinary StandardStreamInput)
    add_test(NAME InstallService.${_case} COMMAND "${WPCE_POWERSHELL_EXECUTABLE}"
      -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/install_service_tests.ps1" -Case "${_case}")
  endforeach()
  foreach(_case QtDebug QtRelease PresetMismatch DirectoryOverride ConfigureFailure BuildFailure TestFailure
      MissingCppcheck MissingClangTidy MissingDatabase WrongSource WrongConfiguration MissingTranslationUnit
      CppcheckFailure ClangTidyFailure StrictSuccess LocalDefect LocalClean MissingFileApi WrongQtDefinitions)
    add_test(NAME ScriptGate.${_case} COMMAND "${WPCE_POWERSHELL_EXECUTABLE}"
      -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/script_gate_tests.ps1" -Case "${_case}")
  endforeach()
endif()
gtest_discover_tests(wpce_contract_tests DISCOVERY_TIMEOUT 30)

# ── PrintTicket parser tests ────────────────────────────────────────────

add_executable(wpce_parser_tests print_ticket_parser_tests.cpp)

target_link_libraries(wpce_parser_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)

target_include_directories(wpce_parser_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_parser_tests PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_parser_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_parser_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(wpce_parser_tests DISCOVERY_TIMEOUT 30)

# ── Pricing engine tests ───────────────────────────────────────────────────

add_executable(wpce_pricing_engine_tests pricing_engine_tests.cpp)

target_link_libraries(wpce_pricing_engine_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)

target_include_directories(wpce_pricing_engine_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_pricing_engine_tests PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_pricing_engine_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_pricing_engine_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_pricing_engine_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(wpce_pricing_engine_tests DISCOVERY_TIMEOUT 30)

# ── Settlement bridge tests ────────────────────────────────────────────────

add_executable(wpce_settlement_tests settlement_bridge_tests.cpp)

target_link_libraries(wpce_settlement_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)

target_include_directories(wpce_settlement_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_settlement_tests PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_settlement_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_settlement_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_settlement_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(wpce_settlement_tests DISCOVERY_TIMEOUT 30)

# ── Counter store tests ─────────────────────────────────────────────────

add_executable(wpce_counter_store_tests counter_store_tests.cpp)

target_link_libraries(wpce_counter_store_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)

target_include_directories(wpce_counter_store_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_counter_store_tests PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_counter_store_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_counter_store_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_counter_store_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

gtest_discover_tests(wpce_counter_store_tests DISCOVERY_TIMEOUT 30)

# ── Billing service idempotency tests ──────────────────────────────────

if(TARGET Qt6::Core)
  add_executable(wpce_filter_install_service_tests filter_install_service_tests.cpp)
  target_link_libraries(wpce_filter_install_service_tests PRIVATE wpce_management GTest::gtest_main)
  if(MSVC)
    target_compile_options(wpce_filter_install_service_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  endif()
  add_custom_command(TARGET wpce_filter_install_service_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE_DIR:wpce_filter_install_service_tests>"
    VERBATIM)
  gtest_discover_tests(wpce_filter_install_service_tests DISCOVERY_TIMEOUT 30)

  add_executable(wpce_billing_service_tests
    billing_service_tests.cpp
    billing_service_safety_tests.cpp
    shutdown_bridge_tests.cpp
    print_ticket_delivery_tests.cpp
    ../src/ui/watcher_bridge.cpp
    ../src/ui/watcher_bridge_inbox.cpp
    ../src/ui/watcher_bridge.h
    ../src/billing/billing_service.cpp
    ../src/billing/billing_service_shutdown.cpp
    ../src/billing/billing_service.h
  )

  target_link_libraries(wpce_billing_service_tests PRIVATE
    wpce_core Qt6::Core GTest::gtest GTest::gtest_main)

  target_include_directories(wpce_billing_service_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/generated
  )

  target_compile_features(wpce_billing_service_tests PRIVATE cxx_std_20)

  if(WIN32)
    target_compile_definitions(wpce_billing_service_tests PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  endif()

  if(MSVC)
    target_compile_options(wpce_billing_service_tests PRIVATE
      /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  else()
    target_compile_options(wpce_billing_service_tests PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

  # GoogleTest discovers cases by executing the binary immediately after
  # linking. Place QtCore beside it first so Windows can load the test.
  add_custom_command(TARGET wpce_billing_service_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Core>"
            "$<TARGET_FILE_DIR:wpce_billing_service_tests>"
    VERBATIM)

  gtest_discover_tests(wpce_billing_service_tests DISCOVERY_TIMEOUT 30)
endif()

# ── Stress tests (non-GoogleTest) ────────────────────────────────────────

if(TARGET wpce_stress_test)
  add_test(NAME wpce_stress_1k COMMAND wpce_stress_test --job-count 1000)
  add_test(NAME wpce_stress_burst COMMAND wpce_stress_test --job-count 2000 --burst-size 500)
  # Note: concurrent multi-writer stress test available as manual test:
  #   stress_test.exe --job-count 2000 --writer-count 2
endif()

# ── Printer capability scanner (diagnostic tool) ────────────────────────

add_executable(wpce_printer_scanner printer_capability_scanner.cpp)

target_link_libraries(wpce_printer_scanner PRIVATE wpce_core)

target_include_directories(wpce_printer_scanner PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_printer_scanner PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_printer_scanner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_printer_scanner PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_printer_scanner PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Print job diagnostic tool ─────────────────────────────────────────

add_executable(wpce_job_diagnostic print_job_diagnostic.cpp)

target_link_libraries(wpce_job_diagnostic PRIVATE wpce_core)

target_include_directories(wpce_job_diagnostic PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_job_diagnostic PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_job_diagnostic PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_job_diagnostic PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_job_diagnostic PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Filter pipeline integration test (non-GoogleTest) ────────────────────

add_executable(wpce_filter_safety_tests
  filter_pipeline_safety_tests.cpp
  ../src/filter/filter_pipeline.cpp
  ../src/filter/print_ticket_extractor.cpp)
target_link_libraries(wpce_filter_safety_tests PRIVATE wpce_core GTest::gtest_main xpsprint.lib)
if(MSVC)
  target_compile_options(wpce_filter_safety_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
endif()
gtest_discover_tests(wpce_filter_safety_tests DISCOVERY_TIMEOUT 30)

if(TARGET filter_test_harness AND TARGET wpce_xps_filter)
  add_test(NAME wpce_filter_pipeline COMMAND filter_test_harness "$<TARGET_FILE:wpce_xps_filter>")
endif()
