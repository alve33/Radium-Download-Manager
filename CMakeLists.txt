# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager — Native C++20 / Win32 Build Orchestration
cmake_minimum_required(VERSION 3.24)
# Read authoritative VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RADIUM_AUTHORITATIVE_VERSION)
string(STRIP "${RADIUM_AUTHORITATIVE_VERSION}" RADIUM_AUTHORITATIVE_VERSION)
if(NOT RADIUM_AUTHORITATIVE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "Authoritative VERSION file is missing or invalid: '${RADIUM_AUTHORITATIVE_VERSION}'")
endif()

# Keep generated files synchronized with VERSION
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_config.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE GEN_RESULT
    )
    if(NOT GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to synchronize configuration from VERSION file!")
    endif()
endif()

project(Radium VERSION ${RADIUM_AUTHORITATIVE_VERSION} LANGUAGES C CXX)
add_compile_definitions(
    RADIUM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    RADIUM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    RADIUM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    RADIUM_VERSION_STRING="${PROJECT_VERSION}"
    FLUX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FLUX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FLUX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    FLUX_VERSION_STRING="${PROJECT_VERSION}"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(MSVC)
    enable_language(RC)
    add_compile_options(/W4 /WX /permissive- /utf-8 /EHsc /wd4996 /D_CRT_SECURE_NO_WARNINGS /MP /Gw /Gy)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    add_link_options("$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
else()
    add_compile_options(-Wall -Wextra -Werror -pthread)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# SQLite 3 Amalgamation
add_library(sqlite3 STATIC src/third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC src/third_party/sqlite3)
if(MSVC)
    target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_WAL=1 SQLITE_ENABLE_COLUMN_METADATA=1 _CRT_SECURE_NO_WARNINGS)
    target_compile_options(sqlite3 PRIVATE /W3 /wd4100 /wd4127 /wd4244 /wd4701 /wd4706)
else()
    target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_WAL=1 SQLITE_ENABLE_COLUMN_METADATA=1)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

set(RADIUM_CORE_SOURCES
    src/common/path_sanitizer.cpp
    src/common/logger.cpp
    src/common/crypto_utils.cpp
    src/storage/sqlite_store.cpp
    src/storage/import_export.cpp
    src/archive/zip_preview.cpp
    src/security/antivirus_scanner.cpp
    src/crawler/site_grabber.cpp
    src/engine_ftp/ftp_client.cpp
    src/engine_torrent/torrent_creator.cpp
    src/engine_torrent/torrent_parser.cpp
    src/engine_torrent/torrent_engine.cpp
    src/core/update_manager.cpp
    src/engine_stream/rtsp_mms_adapter.cpp
    src/brisk_engine/segment_scheduler.cpp
    src/brisk_engine/http_client.cpp
    src/brisk_engine/http_download_engine.cpp
    src/engine_stream/nm3u8dl_re_adapter.cpp
    src/engine_stream/video_extractor.cpp
    src/core/download_controller.cpp
    src/ipc/ipc_server.cpp
    src/ui/startup_log_dialog.cpp
    src/ui/localization.cpp
    src/ui/win32_main_window.cpp
)

set(RADIUM_RC_SOURCES "")
if(WIN32 AND MSVC)
    set(RADIUM_RC_SOURCES src/app/radium.rc)
endif()

# Core Object Library (compiled once, linked into all targets for fast reproducible builds)
add_library(radium_core OBJECT ${RADIUM_CORE_SOURCES})
target_include_directories(radium_core PUBLIC src third_party)
if(WIN32 AND MSVC)
    target_link_libraries(radium_core PUBLIC sqlite3 winhttp comctl32 crypt32 bcrypt shell32 ole32 user32 gdi32 dwmapi ws2_32 msimg32)
endif()

# Primary Radium Targets
add_executable(radium $<TARGET_OBJECTS:radium_core> src/app/main.cpp ${RADIUM_RC_SOURCES})
add_executable(radium-nmh src/nmh/main.cpp ${RADIUM_RC_SOURCES})
add_executable(radium_unit_tests $<TARGET_OBJECTS:radium_core> tests/cpp/unit_tests.cpp)
add_executable(radium_core_tests $<TARGET_OBJECTS:radium_core> tests/unit/core_unit_tests.cpp)
add_executable(N_m3u8DL-RE src/common/crypto_utils.cpp src/brisk_engine/http_client.cpp src/worker_stream/nm3u8dl_re_worker.cpp)

# Build prerequisite: unit tests expect worker binary to be available
add_dependencies(radium_unit_tests N_m3u8DL-RE)
add_dependencies(radium_core_tests N_m3u8DL-RE)

# Target Aliases for Developer and Legacy Build Script Compatibility (RDM-04)
add_library(flux_core ALIAS radium_core)
add_custom_target(flux DEPENDS radium)
add_custom_target(flux-nmh DEPENDS radium-nmh)
add_custom_target(flux_unit_tests DEPENDS radium_unit_tests)
add_custom_target(flux_core_tests DEPENDS radium_core_tests)
add_custom_target(openidm DEPENDS radium)
add_custom_target(openidm-nmh DEPENDS radium-nmh)
add_custom_target(openidm_unit_tests DEPENDS radium_unit_tests)
add_custom_target(openidm_core_tests DEPENDS radium_core_tests)


if(WIN32)
    set_target_properties(radium PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(radium PRIVATE sqlite3 winhttp comctl32 crypt32 bcrypt shell32 ole32 oleacc winmm user32 gdi32 dwmapi ws2_32 msimg32 dbghelp)
    target_link_libraries(radium-nmh PRIVATE advapi32)
    target_link_libraries(radium_unit_tests PRIVATE sqlite3 winhttp comctl32 crypt32 bcrypt shell32 ole32 oleacc winmm user32 gdi32 dwmapi ws2_32 msimg32 dbghelp)
    target_link_libraries(radium_core_tests PRIVATE sqlite3 winhttp comctl32 crypt32 bcrypt shell32 ole32 oleacc winmm user32 gdi32 dwmapi ws2_32 msimg32 dbghelp)
    target_link_libraries(N_m3u8DL-RE PRIVATE winhttp bcrypt crypt32)
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(radium PRIVATE sqlite3 OpenSSL::SSL OpenSSL::Crypto pthread)
    target_link_libraries(radium-nmh PRIVATE sqlite3 OpenSSL::SSL OpenSSL::Crypto pthread)
    target_link_libraries(radium_unit_tests PRIVATE sqlite3 OpenSSL::SSL OpenSSL::Crypto pthread)
    target_link_libraries(radium_core_tests PRIVATE sqlite3 OpenSSL::SSL OpenSSL::Crypto pthread)
    target_link_libraries(N_m3u8DL-RE PRIVATE OpenSSL::SSL OpenSSL::Crypto pthread)
endif()

foreach(target radium radium_unit_tests radium_core_tests)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/translations/desktop.json"
            "$<TARGET_FILE_DIR:${target}>/translations/desktop.json"
        VERBATIM)
endforeach()

enable_testing()
add_test(NAME RadiumCoreUnitTests COMMAND radium_core_tests)
add_test(NAME RadiumCppUnitTests COMMAND radium_unit_tests)
if(Python3_FOUND)
    add_test(NAME RadiumVersionAndConfigValidation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_config.py" --check)
endif()
