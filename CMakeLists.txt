cmake_minimum_required(VERSION 3.20)
project(R5 CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

# GLFW 3.4 builds both its X11 and Wayland backends on Linux by default and
# picks one at runtime, but hard-fails configure if either one's dev
# packages are missing. Enable each only when its packages are actually
# present (unless already set explicitly with -DGLFW_BUILD_X11/WAYLAND=...),
# so a box with just one of the two still builds.
if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    if (NOT DEFINED GLFW_BUILD_WAYLAND)
        pkg_check_modules(R5_WAYLAND QUIET wayland-client wayland-cursor wayland-egl xkbcommon wayland-protocols)
        find_program(R5_WAYLAND_SCANNER wayland-scanner)
        if (R5_WAYLAND_FOUND AND R5_WAYLAND_SCANNER)
            set(GLFW_BUILD_WAYLAND ON CACHE BOOL "")
        else()
            message(STATUS "R5: Wayland dev packages not found, building GLFW without Wayland")
            set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "")
        endif()
    endif()
    if (NOT DEFINED GLFW_BUILD_X11)
        find_package(X11 QUIET)
        if (X11_FOUND AND X11_Xrandr_INCLUDE_PATH AND X11_Xinerama_INCLUDE_PATH AND X11_Xkb_INCLUDE_PATH
                AND X11_Xcursor_INCLUDE_PATH AND X11_Xi_INCLUDE_PATH AND X11_Xshape_INCLUDE_PATH)
            set(GLFW_BUILD_X11 ON CACHE BOOL "")
        else()
            message(STATUS "R5: X11 dev packages not found, building GLFW without X11")
            set(GLFW_BUILD_X11 OFF CACHE BOOL "")
        endif()
    endif()
    if (NOT GLFW_BUILD_WAYLAND AND NOT GLFW_BUILD_X11)
        message(FATAL_ERROR "R5: neither X11 nor Wayland dev packages found. Debian/Ubuntu: "
            "libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev "
            "and/or libwayland-dev wayland-protocols libxkbcommon-dev. Fedora: "
            "libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel "
            "and/or wayland-devel wayland-protocols-devel libxkbcommon-devel.")
    endif()
endif()

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)
set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    kissfft
    GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(kissfft)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(glad STATIC vendor/glad/src/gl.c)
target_include_directories(glad PUBLIC vendor/glad/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw glad)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD2)

add_executable(R5
    src/main.cpp
    src/audio.cpp
)

target_link_libraries(R5 PRIVATE glfw glad kissfft imgui)

# audio.cpp (the FFT/band/smoothing pipeline) is fully shared and never
# changes per platform -- only the actual OS capture call, behind the
# AudioCapture interface (audio_capture.h), differs. Add the one matching
# implementation file and its OS libraries here; nothing else in this
# project should ever need a platform check.
if (WIN32)
    target_sources(R5 PRIVATE src/audio_capture_win.cpp)
    target_link_libraries(R5 PRIVATE opengl32 ole32)
elseif (APPLE)
    # TODO(macOS port): src/audio_capture_mac.mm implementing AudioCapture,
    # via either a virtual-device (BlackHole) + CoreAudio input capture, or
    # ScreenCaptureKit's audio tap (macOS 13+, no extra driver install but
    # needs Screen Recording permission). Not yet written.
    target_link_libraries(R5 PRIVATE
        "-framework Cocoa" "-framework OpenGL" "-framework IOKit" "-framework CoreVideo")
else()
    # PulseAudio client API -- also what PipeWire desktops expose via
    # pipewire-pulse, so this one backend covers both. Build dep:
    # libpulse-dev (Debian/Ubuntu) / pulseaudio-libs-devel (Fedora).
    pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)
    target_sources(R5 PRIVATE src/audio_capture_linux.cpp)
    target_link_libraries(R5 PRIVATE PkgConfig::PULSE)
endif()

target_compile_definitions(R5 PRIVATE
    SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
)

if (MSVC)
    target_compile_options(R5 PRIVATE /W4)
else()
    target_compile_options(R5 PRIVATE -Wall -Wextra)
endif()
