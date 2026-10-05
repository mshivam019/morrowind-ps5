# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(SDL2G19 C)
option(G19_HOST_TEST "Run real SDL2 with a host EGL contract double" OFF)
option(G42_INPUT_VALIDATION "Select the bounded physical input consumer" OFF)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ${G19_HOST_TEST} CACHE BOOL "" FORCE)
set(SDL_VIRTUAL_JOYSTICK ${G19_HOST_TEST} CACHE BOOL "" FORCE)
set(SDL_HIDAPI ${G19_HOST_TEST} CACHE BOOL "" FORCE)
set(SDL_HIDAPI_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_PTHREADS ON CACHE BOOL "" FORCE)
set(SDL_SOURCE_VERSION "2.30.12-g8c56053f13ca-g26")
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
foreach(feature RENDER HAPTIC SENSOR OPENGL OPENGLES VULKAN X11
                WAYLAND KMSDRM OFFSCREEN JACK PULSEAUDIO DBUS IBUS FCITX LIBUDEV)
    set(SDL_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
if(G19_HOST_TEST)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if(NOT G19_HOST_TEST)
    set(PS5 ON CACHE BOOL "" FORCE)
    # The native folder's libc does not export these header-declared symbols.
    # Use SDL's existing implementations; compile-only probes cannot check imports.
    foreach(symbol WCSLCPY WCSLCAT WCSCASECMP WCSNCASECMP)
        set(HAVE_${symbol} 0 CACHE INTERNAL "native libc contract" FORCE)
    endforeach()
endif()
add_subdirectory("${SDL_SOURCE}" sdl)

# Use SDL's video-device extension point for rendering. Retain the upstream
# PS5 pad backend and adapt its IME callbacks for native titles.
get_target_property(sources SDL2-static SOURCES)
list(FILTER sources EXCLUDE REGEX "/video/ps5/")
set_property(TARGET SDL2-static PROPERTY SOURCES "${sources}")
target_sources(SDL2-static PRIVATE SDL_ps5g19.c)
if(NOT G19_HOST_TEST)
    target_sources(SDL2-static PRIVATE SDL_ps5keyboard.c)
endif()
target_include_directories(SDL2-static PRIVATE "${SDL_SOURCE}/src" "${PS5_OPENGL_PREFIX}/include")
target_compile_definitions(SDL2-static PRIVATE SDL_VIDEO_DRIVER_PS5=1 SDL_VIDEO_OPENGL=1)
if(NOT G19_HOST_TEST)
    # Reuse upstream's install/export rules; only this adapter's dependencies differ.
    set_property(TARGET SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES
        "$<INSTALL_INTERFACE:PS5OpenGLCore33::OpenGL>;ScePad;SceUserService;SceSystemService;SceAudioOut;SceSysmodule;SceImeDialog")
    target_compile_definitions(SDL2-static INTERFACE SDL_MAIN_HANDLED=1)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/sdl/SDL2Config.cmake"
        "\ninclude(CMakeFindDependencyMacro)\nfind_dependency(PS5OpenGLCore33 CONFIG)\n")
    configure_file(sdl2.pc.in "${CMAKE_CURRENT_BINARY_DIR}/sdl/sdl2.pc" @ONLY)
endif()
