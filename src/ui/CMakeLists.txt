set(UI_HEADERS
  job_table_model.h
  billing_view_data.h
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
  filter_install_dialog.cpp
  filter_install_dialog.h
  completion_dialog.cpp
  job_table_model.cpp
  job_detail_panel.cpp
  watcher_bridge.cpp
  watcher_bridge_inbox.cpp
  receipt_preview_panel.cpp
  settings_dialog.cpp
  quick_settle_dialog.cpp
  billing_batch_model.cpp
  history_explorer_dialog.cpp
  ../billing/billing_service.cpp
  ../billing/billing_service_shutdown.cpp
  ${UI_HEADERS}
)

# ── Version propagation: inject CMake project version into the .rc resource ──
string(REPLACE "." "," RC_FILEVERSION_COMMA "${PROJECT_VERSION}")
set(RC_FILEVERSION_COMMA "${RC_FILEVERSION_COMMA},0")
set(WPCE_UI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/wpce_ui.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/wpce_ui.rc"
  @ONLY
)

set(UI_RESOURCES
  "${CMAKE_CURRENT_BINARY_DIR}/wpce_ui.rc"
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
  wpce_management
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
  # Run on incremental builds too: script-only fixes must reach the GUI tree.
  add_custom_target(wpce_install_assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wpce_xps_filter>" "$<TARGET_FILE_DIR:wpce_ui>/wpce_xps_filter.dll"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:wpce_ui>/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/scripts/install-xps-filter.ps1" "$<TARGET_FILE_DIR:wpce_ui>/scripts/install-xps-filter.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/scripts/lib" "$<TARGET_FILE_DIR:wpce_ui>/scripts/lib"
    VERBATIM)
  add_dependencies(wpce_install_assets wpce_xps_filter)
  add_dependencies(wpce_ui wpce_install_assets)
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
