# exit_common - shared, game-agnostic ReXGlue glue
#
# Static library of the title-neutral project glue (settings/INI, symbolizing
# crash handler, timer-resolution fix, guest-output PNG capture). EXIT 1 and
# EXIT 2 both link this, so a fix here lands in both games. Anything that names
# a guest address stays in the per-game project (its *_hooks.cpp / *_app.h).
#
# This is added via add_subdirectory() from a game's CMakeLists *after* it has
# included generated/rexglue.cmake (which brings in the rex::runtime target), so
# the rex SDK headers are available transitively through rex::runtime.

cmake_minimum_required(VERSION 3.25)

set(EXIT_COMMON_SOURCES
    src/settings.cpp
    src/crash.cpp
    src/timing.cpp
    src/screenshot.cpp
    # Vendored lodepng (zlib license) - PNG encoder for the guest-output capture
    # (src/screenshot.cpp).
    thirdparty/lodepng/lodepng.cpp
)

add_library(exit_common STATIC ${EXIT_COMMON_SOURCES})

target_include_directories(exit_common
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lodepng"
)

# rex::runtime carries the SDK's public headers (rex/logging, rex/runtime,
# rex/graphics, rex/ui/presenter, rex/filesystem, ...). PUBLIC so a linking game
# target also sees them. dbghelp: SymFromAddr / StackWalk64 for the crash
# backtrace (src/crash.cpp). winmm: timeBeginPeriod for the timer fix
# (src/timing.cpp). #pragma comment(lib) covers the Win libs too, but be explicit.
target_link_libraries(exit_common PUBLIC rex::runtime)
if(WIN32)
    target_link_libraries(exit_common PUBLIC dbghelp winmm)
endif()

# src/screenshot.cpp pulls <rex/ui/presenter.h>, which transitively includes
# <imgui.h>. The SDK links imgui into rexruntime PRIVATE (src/kernel/CMakeLists),
# so its include dir is NOT propagated through rex::runtime to a separate lib.
# Borrow imgui's interface include dir directly (without linking the OBJECT lib,
# which would duplicate its objects into both exit_common and rexruntime). The
# imgui target exists because the SDK was add_subdirectory'd before us.
if(TARGET imgui)
    target_include_directories(exit_common PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
