cmake_minimum_required(VERSION 3.15)

# Several vendored dependencies (OpenCC, tweeny, ...) still declare
# cmake_minimum_required(VERSION 2.8) or 3.x < 3.5, which CMake 3.31+ rejects
# outright. Allow them to configure instead of patching every vendored file.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# -----------------------------------------------------------------------------
# Global build options
# -----------------------------------------------------------------------------
option(PLATFORM_DESKTOP "build for linux macos or windows" OFF)
option(PLATFORM_SWITCH "build for NintendoSwitch" OFF)
option(PLATFORM_PSV "build for PSVita" OFF)
option(USE_GLFW "using glfw for input and create window" ON)
option(USE_SDL2 "using sdl2 for input and create window" OFF)

# Which sample apps to build. Disable the ones you don't need.
option(NXKIT_APP_CLOUD "build the cloud drive app" ON)

if(USE_SDL2)
    message("SDL2")
    set(USE_GLFW OFF)
elseif(USE_GLFW)
    message("GLFW")
    set(USE_SDL2 OFF)
endif()

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
if(PLATFORM_DESKTOP)
    message("building for Desktop")
    set(TARGET_PLATFORM desktop CACHE STRING "")
    message("Build Type: ${CMAKE_BUILD_TYPE}")
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g2 -ggdb -Wall")
    set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
elseif(PLATFORM_SWITCH)
    message("building for SWITCH")
    message("Build Type: ${CMAKE_BUILD_TYPE}")
    set(CMAKE_USE_SYSTEM_ENVIRONMENT_PATH OFF)
    file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO_PATH)
    if(NOT IS_DIRECTORY ${DEVKITPRO_PATH})
        message(FATAL_ERROR "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
    endif()
    set(DEVKITPRO $ENV{DEVKITPRO} CACHE BOOL "DEVKITPRO")
    include(${DEVKITPRO}/cmake/Switch.cmake REQUIRED)
    set(__SWITCH__ ON)
    set(CMAKE_C_FLAGS "-I${DEVKITPRO}/libnx/include -I${DEVKITPRO}/portlibs/switch/include")
    set(CMAKE_CXX_FLAGS "-I${DEVKITPRO}/libnx/include -I${DEVKITPRO}/portlibs/switch/include")
    # Embed an NSP forwarder so the app can install a home-menu (desktop) icon.
    # Requires the nspmini library and hacbrewpack; see scripts/switch-forwarder.
    option(BUILTIN_NSP "Built in NSP forwarder (install home-menu icon)" OFF)
elseif(PLATFORM_PSV)
    message("building for PSVita")
    set(USE_GLFW ON)
    set(USE_SDL2 OFF)
    message("Force using GLFW")
    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        if(DEFINED ENV{VITASDK})
            set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
        else()
            message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
        endif()
    endif()
    set(VITASDK $ENV{VITASDK} CACHE BOOL "VITASDK")
    include("${VITASDK}/share/vita.cmake" REQUIRED)
else()
    message(FATAL_ERROR "Please set build target -DPLATFORM_DESKTOP=ON or -DPLATFORM_SWITCH=ON or -DPLATFORM_PSV=ON")
endif()

# -----------------------------------------------------------------------------
# Project info
# -----------------------------------------------------------------------------
project(AnyCloud C CXX)
set(VERSION_MAJOR "1")
set(VERSION_MINOR "5")
set(VERSION_REVISION "18")
set(NXKIT_RESOURCES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(NXKIT_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/core)

# Every app gets its own title id / author, see cmake/NXKitApp.cmake
set(NXKIT_DEFAULT_TITLEID "0x010FF000FFFF00FF")
set(NXKIT_DEFAULT_AUTHOR "nxkit")
set(NXKIT_DEFAULT_ICON ${NXKIT_RESOURCES_DIR}/img/icon.jpg)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NXKitApp.cmake)

# -----------------------------------------------------------------------------
# Platform libraries (shared by every app)
# -----------------------------------------------------------------------------
if(PLATFORM_SWITCH)
    set(NXKIT_PLATFORM_LIBS
            # needed by borealis
            glfw3 EGL glapi drm_nouveau
            # needed by cpr
            curl z
            # base lib
            nx m
            )
elseif(PLATFORM_PSV)
    set(NXKIT_PLATFORM_LIBS
            glfw3
            libgpu_es4_ext_stub_weak
            libGLESv2_stub_weak
            libIMGEGL_stub_weak
            SceCtrl_stub
            SceTouch_stub
            pthread
            )
    add_definitions(-D__psp2__ -D__PSV__)
endif()

# -----------------------------------------------------------------------------
# Framework + apps
# -----------------------------------------------------------------------------
add_subdirectory(library)
add_subdirectory(core)

if(NXKIT_APP_CLOUD)
    add_subdirectory(apps/cloud)
endif()
