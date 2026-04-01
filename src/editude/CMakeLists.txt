# SPDX-License-Identifier: GPL-3.0-only
#
# Copyright (C) 2026 Alexis Goodfellow
#
# CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
# EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
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
# along with this program. If not, see <https://www.gnu.org/licenses/>.

find_package(Qt6 REQUIRED COMPONENTS WebSockets)

muse_create_module(editude)

target_sources(editude PRIVATE
    editudemodule.h
    editudemodule.cpp
    internal/editudeservice.h
    internal/editudeservice.cpp
    internal/operationtranslator.h
    internal/operationtranslator.cpp
    internal/scoreapplicator.h
    internal/scoreapplicator.cpp
    internal/presenceoverlay.h
    internal/presenceoverlay.cpp
    internal/annotationoverlay.h
    internal/annotationoverlay.cpp
    internal/editudeuiactions.h
    internal/editudeuiactions.cpp
)

# Model headers live in the QML module directory; expose them to editude
# so editudemodule.h and editudeservice.cpp can include them by short name.
target_include_directories(editude PRIVATE qml/Editude)

if(APPLE)
    target_sources(editude PRIVATE
        internal/platform/macos/appnap.h
        internal/platform/macos/appnap.mm
    )
    # Compile the .mm file as its own Objective-C++ translation unit —
    # Foundation headers use @class syntax that breaks in Unity C++ builds.
    set_source_files_properties(internal/platform/macos/appnap.mm PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON
    )
else()
    target_sources(editude PRIVATE
        internal/platform/macos/appnap.h
        internal/platform/stub/appnap.cpp
    )
endif()

target_link_libraries(editude PRIVATE
    Qt::WebSockets
    Qt::Network
    context
    notation
    engraving
)

if(APPLE)
    target_link_libraries(editude PRIVATE "-framework Foundation")
endif()

# QML module: registers Editude types and QML files (import Editude 1.0).
add_subdirectory(qml/Editude)

if (MUE_BUILD_EDITUDE_TESTS)
    add_subdirectory(tests)
endif()

# Shared test actions — used by both the native HTTP test driver and the
# WASM Emscripten bridge.
if (MUE_BUILD_EDITUDE_TEST_DRIVER OR OS_IS_WASM)
    target_sources(editude PRIVATE
        testing/editudetestactions.h
        testing/editudetestactions.cpp
    )
endif()

if (MUE_BUILD_EDITUDE_TEST_DRIVER)
    target_sources(editude PRIVATE
        testing/editudetestdriver.h
        testing/editudetestdriver.cpp
    )
    target_compile_definitions(editude PRIVATE MUE_BUILD_EDITUDE_TEST_DRIVER)
endif()

if (OS_IS_WASM)
    target_sources(editude PRIVATE
        testing/editudetestbridge.h
        testing/editudetestbridge.cpp
    )
    # The bridge file uses emscripten_run_script() with a large raw string
    # literal that breaks Unity build concatenation.
    set_source_files_properties(testing/editudetestbridge.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
    )
endif()
