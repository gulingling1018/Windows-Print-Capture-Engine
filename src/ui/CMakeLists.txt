set(UI_SOURCES
  main.cpp
  main_window.cpp
  job_table_model.cpp
  job_detail_panel.cpp
  lifecycle_timeline.cpp
  watcher_bridge.cpp
  statistics_panel.cpp
  user_task_model.cpp
  user_task_panel.cpp
)

add_executable(wpce_ui WIN32 ${UI_SOURCES})

target_include_directories(wpce_ui PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_BINARY_DIR}/generated
)

target_link_libraries(wpce_ui PRIVATE
  wpce_core
  Qt6::Core
  Qt6::Widgets
)

target_compile_features(wpce_ui PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_ui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

if(MSVC)
  target_compile_options(wpce_ui PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()
