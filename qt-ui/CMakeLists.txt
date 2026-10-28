cmake_minimum_required(VERSION 3.22)
get_filename_component(BAT_QT_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
find_program(BAT_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
execute_process(
    COMMAND "${BAT_NODE_EXECUTABLE}" "${BAT_QT_SOURCE_ROOT}/scripts/qt-version.mjs"
        prepare "${BAT_QT_SOURCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE BAT_VERSION_RESULT OUTPUT_VARIABLE BAT_VERSION_JSON
    ERROR_VARIABLE BAT_VERSION_ERROR OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT BAT_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "Qt version preparation failed: ${BAT_VERSION_ERROR}")
endif()
string(JSON BAT_QT_VERSION GET "${BAT_VERSION_JSON}" version)
project(BetterAgentTerminalQt VERSION "${BAT_QT_VERSION}" LANGUAGES CXX)

# Reconfigure on content changes and new/deleted runtime files, even when the
# caller builds an existing CMake directory directly instead of using pnpm.
file(GLOB_RECURSE BAT_VERSION_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*"
    "${BAT_QT_SOURCE_ROOT}/src-tauri/src/*"
    "${BAT_QT_SOURCE_ROOT}/node-sidecar/src/*"
    "${BAT_QT_SOURCE_ROOT}/shared/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${BAT_VERSION_INPUTS}
    "${CMAKE_CURRENT_SOURCE_DIR}/version.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/better-agent-terminal-qt.desktop"
    "${BAT_QT_SOURCE_ROOT}/scripts/qt-version.mjs"
    "${BAT_QT_SOURCE_ROOT}/scripts/build-qt.mjs"
    "${BAT_QT_SOURCE_ROOT}/scripts/package-qt-appimage.mjs"
    "${BAT_QT_SOURCE_ROOT}/src-tauri/Cargo.toml"
    "${BAT_QT_SOURCE_ROOT}/src-tauri/Cargo.lock"
    "${BAT_QT_SOURCE_ROOT}/src-tauri/build.rs"
    "${BAT_QT_SOURCE_ROOT}/node-sidecar/package.json"
    "${BAT_QT_SOURCE_ROOT}/node-sidecar/pnpm-lock.yaml"
    "${BAT_QT_SOURCE_ROOT}/package.json"
    "${BAT_QT_SOURCE_ROOT}/pnpm-lock.yaml"
    "${BAT_QT_SOURCE_ROOT}/runtime-catalog.json"
    "${BAT_QT_SOURCE_ROOT}/assets/icon.png")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network WebSockets Test)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Parts REQUIRED)

add_library(bat-qt-core STATIC
    src/hostclient.cpp src/hostclient.h
    src/transcript.cpp src/transcript.h
    src/terminalpanel.cpp src/terminalpanel.h
    src/settingsdialog.cpp src/settingsdialog.h
    src/gitpanel.cpp src/gitpanel.h
)
target_link_libraries(bat-qt-core PUBLIC Qt6::Widgets Qt6::Network Qt6::WebSockets
    KF6::CoreAddons KF6::Parts)
target_include_directories(bat-qt-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bat-qt-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(better-agent-terminal-qt
    src/main.cpp src/mainwindow.cpp src/mainwindow.h src/agentpanel.cpp src/agentpanel.h)
target_link_libraries(better-agent-terminal-qt PRIVATE bat-qt-core)
target_include_directories(better-agent-terminal-qt PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(better-agent-terminal-qt PRIVATE BAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
add_custom_command(TARGET better-agent-terminal-qt POST_BUILD
    COMMAND "${BAT_NODE_EXECUTABLE}" "${BAT_QT_SOURCE_ROOT}/scripts/qt-version.mjs"
        commit "${BAT_QT_SOURCE_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Record successful Qt build version ${BAT_QT_VERSION}" VERBATIM)
target_compile_options(better-agent-terminal-qt PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS better-agent-terminal-qt RUNTIME DESTINATION bin)
install(FILES better-agent-terminal-qt.desktop DESTINATION share/applications)

enable_testing()
add_executable(bat-qt-tests tests/core_test.cpp
    src/mainwindow.cpp src/mainwindow.h src/agentpanel.cpp src/agentpanel.h)
target_link_libraries(bat-qt-tests PRIVATE bat-qt-core Qt6::Test)
# Record the version only after both native targets have compiled successfully.
add_dependencies(better-agent-terminal-qt bat-qt-tests)
add_test(NAME qt-core COMMAND bat-qt-tests)
set_tests_properties(qt-core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
