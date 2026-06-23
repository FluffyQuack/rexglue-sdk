# exit2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(exit2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Shared, game-agnostic glue (settings/crash/timing/screenshot), linked by both
# EXIT 1 and EXIT 2. Added after generated/rexglue.cmake so the rex::runtime
# target it depends on already exists. The binary dir is named so two sibling
# game builds adding the same sibling source dir don't collide.
add_subdirectory(../exit_common "${CMAKE_CURRENT_BINARY_DIR}/exit_common")

# Sources
set(EXIT2_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(exit2 WIN32 ${EXIT2_SOURCES})
else()
    add_executable(exit2 ${EXIT2_SOURCES})
endif()

rexglue_setup_target(exit2)

target_link_libraries(exit2 PRIVATE exit_common)
