/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

import QtQuick
import Editude 1.0

/**
 * Presence cursor overlay for the notation view.
 *
 * Place this as a child of NotationPaintView in NotationView.qml:
 *
 *   NotationPaintView {
 *       id: notationView
 *       // ... existing content ...
 *       EditudeNotationOverlay { anchors.fill: parent; enabled: false }
 *   }
 *
 * The overlay watches its parent's matrixChanged signal to keep cursor
 * rectangles in sync with the canvas transform.  No Connections blocks
 * are needed in NotationView.qml — the integration surface in the upstream
 * MuseScore file is exactly one line.
 */
Item {
    id: root

    // Forward parent matrix changes to the C++ model so cursor rects
    // are recomputed in screen coordinates whenever the view scrolls or zooms.
    Component.onCompleted: {
        EditudePresenceModel.setNotationViewMatrix(root.parent.matrix)
    }

    Connections {
        target: root.parent
        function onMatrixChanged() {
            EditudePresenceModel.setNotationViewMatrix(root.parent.matrix)
        }
    }

    // Presence cursor rectangles — one rectangle per remote contributor selection.
    Repeater {
        model: EditudePresenceModel
        delegate: Rectangle {
            x: model.screenRect.x
            y: model.screenRect.y
            width: model.screenRect.width
            height: model.screenRect.height
            color: model.rectColor
            enabled: false
        }
    }

    // Transient toast notification (e.g. "Your edit conflicted with…").
    Rectangle {
        visible: EditudePresenceModel.toastText !== ""
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 16
        color: "#cc1a1a1a"
        radius: 6
        width: toastLabel.implicitWidth + 24
        height: toastLabel.implicitHeight + 12
        enabled: false

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: EditudePresenceModel.toastText
            color: "#ffffff"
            font.pixelSize: 13
        }
    }
}
