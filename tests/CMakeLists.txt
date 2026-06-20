add_executable(wpce_contract_tests contract_serialization_tests.cpp)
target_link_libraries(wpce_contract_tests PRIVATE wpce_core)

if(MSVC)
  target_compile_options(wpce_contract_tests PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_contract_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME wpce_contract_tests COMMAND wpce_contract_tests)

# Filter pipeline integration test.
if(TARGET filter_test_harness AND TARGET wpce_xps_filter)
  add_test(NAME wpce_filter_pipeline COMMAND filter_test_harness "$<TARGET_FILE:wpce_xps_filter>")
endif()

