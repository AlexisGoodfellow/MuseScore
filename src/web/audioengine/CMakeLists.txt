# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2021-2025 MuseScore Limited
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.22)

project(MuseAudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MUSE_MODULE_AUDIO_WORKER "Enable audio worker" OFF)
option(MUSE_FIX_MUSEAUDIO_FILENAME "Fix MuseAudio.js file name" OFF)

# src/web/audioengine
set(MU_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(MUSE_FRAMEWORK_PATH ${MU_ROOT})
set(MUSE_FRAMEWORK_SRC_PATH ${MU_ROOT}/src/framework)

if (NOT EXISTS ${MU_ROOT}/buildscripts)
    message(FATAL_ERROR "not root: ${MU_ROOT}")
endif()

set(CMAKE_MODULE_PATH
    ${MU_ROOT}
    ${MU_ROOT}/buildscripts
    ${MU_ROOT}/buildscripts/cmake
    ${MU_ROOT}/src/framework/cmake
    ${CMAKE_MODULE_PATH}
    )


include(GetPlatformInfo)
include(MuseCreateModule)
include(SetupCompileWarnings)
include(GetBuildType)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${MU_ROOT}/build.artifacts)

set(EMCC_COMPILE_FLAGS "-s USE_ZLIB=1 -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EMCC_COMPILE_FLAGS}")

if (BUILD_IS_DEBUG)
    set(EMCC_LINKER_FLAGS -O0)
else()
    set(EMCC_LINKER_FLAGS -Os)
endif()

# [editude] Defines and Qt stubs must come BEFORE add_subdirectory so
# audio/common, audio/engine, and global compile without real Qt headers.
add_definitions(-DNO_QT_SUPPORT)
# Note: Q_OS_WASM intentionally NOT defined for the standalone MuseAudio build.
# The only Q_OS_WASM code in the engine subtree is network-based soundfont
# loading in soundfontrepository.cpp, which requires Asyncable inheritance
# that doesn't exist in the standalone build.  MuseAudio receives soundfonts
# via RPC from the main thread instead.
set(GLOBAL_NO_INTERNAL ON)

# Minimal Qt type stubs (QString, QSettings) — satisfies framework headers
# that reference Qt types outside NO_QT_SUPPORT guards. The audio engine
# never calls these types at runtime. Force-included because the framework
# files use these types without #include (they're transitively available
# from Qt in normal builds).
#
# Applied per-target (not add_compile_options) because FluidSynth C++ files
# must not see the QString stub — it includes muse::String which requires
# framework include paths that FluidSynth doesn't have.
set(_QT_STUBS
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/qt_stubs/QString"
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/qt_stubs/QSettings"
)

set(AUDIO_ROOT ${MU_ROOT}/src/framework/audio)
add_subdirectory(${AUDIO_ROOT}/common common)
target_compile_options(muse_audio_common PRIVATE ${_QT_STUBS})

set(WEBWORKER_FACADE_MODE OFF)
add_subdirectory(${AUDIO_ROOT}/engine engine)
target_compile_options(muse_audio_engine PRIVATE ${_QT_STUBS})

add_executable(MuseAudio
    ${CMAKE_CURRENT_LIST_DIR}/engine_export.cpp
    ${CMAKE_CURRENT_LIST_DIR}/webaudioengine.cpp
    ${CMAKE_CURRENT_LIST_DIR}/webaudioengine.h

    ${AUDIO_ROOT}/driver/platform/web/webaudiochannel.cpp
    ${AUDIO_ROOT}/driver/platform/web/webaudiochannel.h
)

target_include_directories(MuseAudio PRIVATE
    ${PROJECT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}

    ${PROJECT_SOURCE_DIR}/src

    ${MUSE_FRAMEWORK_PATH}
    ${MUSE_FRAMEWORK_PATH}/framework
    ${MUSE_FRAMEWORK_PATH}/framework/global
    ${MUSE_FRAMEWORK_PATH}/framework/audio

    # compat
    ${MUSE_FRAMEWORK_PATH}/src
    ${MUSE_FRAMEWORK_PATH}/src/framework
    ${MUSE_FRAMEWORK_PATH}/src/framework/global
    ${MUSE_FRAMEWORK_PATH}/src/framework/audio
    # end compat
)

# MUSE_MODULE_AUDIO_WORKMODE must be set before configure_file — the template
# substitutes it with #define (not #cmakedefine), so an empty value produces
# invalid C.  Mode 1 = MUSE_MODULE_AUDIO_WORKER_MODE (standalone worker).
set(MUSE_MODULE_AUDIO_WORKMODE 1)
configure_file(${MU_ROOT}/src/framework/cmake/muse_framework_config.h.in muse_framework_config.h )

add_subdirectory(${MU_ROOT}/src/framework/global global)
target_compile_options(muse_global PRIVATE ${_QT_STUBS})

target_compile_options(MuseAudio PRIVATE ${_QT_STUBS})

target_link_libraries(MuseAudio PRIVATE
    muse_audio_common
    muse_audio_engine
    embind
    muse::global
)

#set_target_properties(MuseAudio PROPERTIES OUTPUT_NAME "MuseAudio.wasm")
#target_link_options(MuseAudio PRIVATE "SHELL:-o MuseAudio.wasm")
target_link_options(MuseAudio PRIVATE ${EMCC_LINKER_FLAGS})
target_link_options(MuseAudio PRIVATE "SHELL:-s EXPORT_NAME=MuseAudio_entry")
target_link_options(MuseAudio PRIVATE "SHELL:-s MODULARIZE=1")
target_link_options(MuseAudio PRIVATE "SHELL:-s FETCH=1")
target_link_options(MuseAudio PRIVATE "SHELL:-s INITIAL_MEMORY=50MB")
target_link_options(MuseAudio PRIVATE "SHELL:-s ALLOW_MEMORY_GROWTH=1")
target_link_options(MuseAudio PRIVATE "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,cwrap")
target_link_options(MuseAudio PRIVATE "SHELL:-s EXPORTED_FUNCTIONS=[_main,_malloc,_free,_Init,_process]")

if (MUSE_MODULE_AUDIO_WORKER)
    target_link_options(MuseAudio PRIVATE "SHELL:-s ENVIRONMENT=worker")
else()
    target_link_options(MuseAudio PRIVATE "SHELL:-s ENVIRONMENT=shell")
    target_link_options(MuseAudio PRIVATE "SHELL:-s BINARYEN_ASYNC_COMPILATION=0")
    target_link_options(MuseAudio PRIVATE "SHELL:-s SINGLE_FILE=1")
   # target_link_options(MuseAudio PRIVATE "SHELL:-s EXPORT_ES6=1")
endif()


if (MUSE_FIX_MUSEAUDIO_FILENAME)
    add_custom_command(
        TARGET MuseAudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MuseAudio"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MuseAudio.js"
        COMMENT "Fix output js file name"
    )
endif()
