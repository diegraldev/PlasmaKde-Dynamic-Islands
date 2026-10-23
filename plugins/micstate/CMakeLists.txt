cmake_minimum_required(VERSION 3.16)
project(micstate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

add_library(micstate SHARED
    micstate.cpp
    micstate_plugin.cpp
)

target_include_directories(micstate PRIVATE
    ${Qt6Core_INCLUDE_DIRS}
    PkgConfig::PULSE
)

target_link_libraries(micstate PRIVATE
    Qt6::Core
    Qt6::Qml
    PkgConfig::PULSE
)

set_target_properties(micstate PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
