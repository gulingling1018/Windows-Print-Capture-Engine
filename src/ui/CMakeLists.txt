set(UI_HEADERS
  job_table_model.h
  user_group_model.h
  user_group_delegate.h
  completion_dialog.h
  document_link_delegate.h
  grip_splitter.h
  settings_dialog.h
  receipt_preview_panel.h
  quick_settle_dialog.h
  billing_batch_model.h
  history_explorer_dialog.h
)

set(UI_SOURCES
  main.cpp
  main_window.cpp
  user_group_model.cpp
  completion_dialog.cpp
  job_table_model.cpp
  job_detail_panel.cpp
  watcher_bridge.cpp
  user_task_model.cpp
  receipt_preview_panel.cpp
  settings_dialog.cpp
  quick_settle_dialog.cpp
  billing_batch_model.cpp
  history_explorer_dialog.cpp
  ../billing/billing_service.cpp
  ${UI_HEADERS}
)

set(UI_RESOURCES
  wpce_ui.rc
)

set(UI_QRC
  wpce_theme.qrc
)

add_executable(wpce_ui WIN32 ${UI_SOURCES} ${UI_RESOURCES} ${UI_QRC})

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

if(WIN32)
  get_target_property(_wpce_qmake_executable Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_wpce_qt_bin_dir "${_wpce_qmake_executable}" DIRECTORY)

  find_program(WPCE_WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt
    HINTS "${_wpce_qt_bin_dir}"
    REQUIRED
  )

  add_custom_command(TARGET wpce_ui POST_BUILD
    COMMAND "${WPCE_WINDEPLOYQT_EXECUTABLE}"
            "$<$<CONFIG:Debug>:--debug>$<$<NOT:$<CONFIG:Debug>>:--release>"
            --no-translations
            --dir "$<TARGET_FILE_DIR:wpce_ui>"
            "$<TARGET_FILE:wpce_ui>"
    COMMENT "Deploying Qt runtime libraries for wpce_ui"
    VERBATIM
  )
endif()
