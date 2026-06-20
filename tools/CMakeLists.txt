add_executable(wpce_stress_test stress_test.cpp)

target_link_libraries(wpce_stress_test PRIVATE wpce_core)

target_include_directories(wpce_stress_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_stress_test PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_stress_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_stress_test PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_stress_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
