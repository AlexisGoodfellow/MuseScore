# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2026 MuseScore Limited
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
)

target_link_libraries(editude PRIVATE
    Qt::WebSockets
    Qt::Network
    context
    notation
    engraving
)

if (MUE_BUILD_EDITUDE_TESTS)
    add_subdirectory(tests)
endif()
