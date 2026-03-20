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

set(MODULE_TEST editude_tests)

set(MODULE_TEST_SRC
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/scoreapplicator_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/operationtranslator_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/editudeservice_reconnect_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/mocks/engravingconfigurationmock.h
)

set(MODULE_TEST_LINK
    editude
    editude_qml
    engraving
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)
