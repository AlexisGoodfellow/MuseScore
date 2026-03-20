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
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# editude_qml — the QML module that exposes Editude collaboration types.
#
# All editude C++ types registered with QML_ELEMENT / QML_SINGLETON live here
# so they compile into this target rather than into the notationscene module.
# Consumer modules import via:  import Editude 1.0
#
# The parent editude target is NOT a QML module (muse_create_module uses
# qt_add_library, not qt_add_qml_module).  editude_qml is the QML-facing
# companion target: it compiles the model .cpp files and carries the
# qt_add_qml_module call that registers them, exactly mirroring the
# notationscene / notationscene_qml two-target pattern.

muse_create_qml_module(editude_qml FOR editude)

qt_add_qml_module(editude_qml
    URI Editude
    VERSION 1.0
    SOURCES
        editudepresencemodel.h
        editudepresencemodel.cpp
        editudeannotationmodel.h
        editudeannotationmodel.cpp
    QML_FILES
        EditudeNotationOverlay.qml
        AnnotationSidePanel.qml
    IMPORTS
        TARGET muse_ui_qml
        TARGET muse_uicomponents_qml
)

fixup_qml_module_dependencies(editude_qml)
