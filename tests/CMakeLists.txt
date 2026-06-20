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

target_link_libraries(wpce_contract_tests PRIVATE wpce_core GTest::gtest GTest::gtest_main)

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
gtest_discover_tests(wpce_contract_tests DISCOVERY_TIMEOUT 30)

# ── Stress tests (non-GoogleTest) ────────────────────────────────────────

if(TARGET wpce_stress_test)
  add_test(NAME wpce_stress_1k COMMAND wpce_stress_test --job-count 1000)
  add_test(NAME wpce_stress_queue_full COMMAND wpce_stress_test --job-count 2000 --queue-capacity 16 --burst-size 500)
  # Note: concurrent multi-writer stress test available as manual test:
  #   stress_test.exe --job-count 2000 --writer-count 2
endif()

# ── Filter pipeline integration test (non-GoogleTest) ────────────────────

if(TARGET filter_test_harness AND TARGET wpce_xps_filter)
  add_test(NAME wpce_filter_pipeline COMMAND filter_test_harness "$<TARGET_FILE:wpce_xps_filter>")
endif()
