cmake_minimum_required(VERSION 3.22)
project(BetterAgentTerminalQt VERSION 0.1.0 LANGUAGES CXX)

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
)
target_link_libraries(bat-qt-core PUBLIC Qt6::Widgets Qt6::Network Qt6::WebSockets
    KF6::CoreAddons KF6::Parts)
target_include_directories(bat-qt-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(bat-qt-core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(better-agent-terminal-qt
    src/main.cpp src/mainwindow.cpp src/mainwindow.h src/agentpanel.cpp src/agentpanel.h)
target_link_libraries(better-agent-terminal-qt PRIVATE bat-qt-core)
target_compile_definitions(better-agent-terminal-qt PRIVATE BAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(better-agent-terminal-qt PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS better-agent-terminal-qt RUNTIME DESTINATION bin)
install(FILES better-agent-terminal-qt.desktop DESTINATION share/applications)

enable_testing()
add_executable(bat-qt-tests tests/core_test.cpp
    src/mainwindow.cpp src/mainwindow.h src/agentpanel.cpp src/agentpanel.h)
target_link_libraries(bat-qt-tests PRIVATE bat-qt-core Qt6::Test)
add_test(NAME qt-core COMMAND bat-qt-tests)
set_tests_properties(qt-core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
