add_library(wpce_core
  collector/metadata_collector.cpp
  common/app_config.cpp
  common/business_clock.cpp
  common/build_info.cpp
  common/counter_store.cpp
  common/document_name.cpp
  common/file_type_detector.cpp
  common/logger.cpp
  common/paper_size_map.cpp
  common/pricing_engine.cpp
  common/printer_profile.cpp
  core/data_contracts.cpp
  core/perf_monitor.cpp
  domain/billing_batch.cpp
  domain/billing_batch_repository.cpp
  domain/billing_migration.cpp
  ipc/named_event.cpp
  storage/file_utils.cpp
  storage/reliable_file_writer.cpp
  storage/capture_retention.cpp
  storage/job_snapshot_writer.cpp
  storage/json_serializer.cpp
  storage/page_artifact_writer.cpp
  storage/xml_serializer.cpp
  watcher/print_watcher.cpp
  parser/print_ticket_parser.cpp
  parser/spool_file_parser.cpp
  bridge/metadata_merger.cpp
  bridge/print_ticket_inbox.cpp
  bridge/settlement_bridge.cpp
)

target_include_directories(wpce_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/generated
)

target_compile_features(wpce_core PUBLIC cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(wpce_core PRIVATE Winspool.lib bcrypt.lib ole32.lib)
endif()

if(MSVC)
  target_compile_options(wpce_core PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(wpce app/main.cpp)
if(TARGET Qt6::Core)
  add_library(wpce_management STATIC management/filter_install_service.cpp management/filter_install_service.h)
  target_include_directories(wpce_management PUBLIC ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(wpce_management PUBLIC Qt6::Core)
  target_compile_features(wpce_management PUBLIC cxx_std_20)
  if(WIN32)
    target_compile_definitions(wpce_management PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  endif()
  if(MSVC)
    target_compile_options(wpce_management PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  endif()
endif()
target_link_libraries(wpce PRIVATE wpce_core)

if(MSVC)
  target_compile_options(wpce PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── XPS Capture Filter DLL (Phase 3-4) ──────────────────────────────────

add_library(wpce_xps_filter SHARED
  filter/filter_entry.cpp
  filter/filter_guid.cpp
  filter/filter_pipeline.cpp
  filter/print_ticket_extractor.cpp
  filter/wpce_xps_filter.def
)

target_include_directories(wpce_xps_filter
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(wpce_xps_filter PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(wpce_xps_filter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(wpce_xps_filter PRIVATE wpce_core xpsprint.lib)
endif()

if(MSVC)
  target_compile_options(wpce_xps_filter PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(wpce_xps_filter PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Filter test harness ─────────────────────────────────────────────────

add_executable(filter_test_harness
  ${PROJECT_SOURCE_DIR}/tools/filter_test_harness.cpp
)

target_include_directories(filter_test_harness PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(filter_test_harness PRIVATE cxx_std_20)

if(WIN32)
  target_compile_definitions(filter_test_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(filter_test_harness PRIVATE xpsprint.lib)
endif()

if(MSVC)
  target_compile_options(filter_test_harness PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(filter_test_harness PRIVATE -Wall -Wextra -Wpedantic)
endif()
