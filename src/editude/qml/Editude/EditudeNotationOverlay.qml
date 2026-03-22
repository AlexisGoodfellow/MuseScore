/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 Alexis Goodfellow
 *
 * CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
 * EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
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
        EditudeAnnotationOverlayModel.setNotationViewMatrix(root.parent.matrix)
        // Start focus retries immediately.  On the snapshot path, scoreReady
        // was emitted before this overlay existed (the page hadn't been
        // created yet), so the onScoreReady connection below never fired.
        // Starting here ensures keyboard focus is established once the
        // NotationPaintView can accept it.
        focusRetry.attempts = 0
        focusRetry.start()
    }

    Connections {
        target: root.parent
        function onMatrixChanged() {
            EditudePresenceModel.setNotationViewMatrix(root.parent.matrix)
            EditudeAnnotationOverlayModel.setNotationViewMatrix(root.parent.matrix)
        }
    }

    // When the C++ EditudeService finishes bootstrap (ops applied, WS open),
    // it emits scoreReady.  At that point the page transition may still be
    // in progress, so forceFocusIn() won't succeed immediately.  Retry every
    // 50 ms until the NotationPaintView actually has active focus (keyboard
    // shortcuts work) or 2 s elapse.  This replaces both the old fixed-200 ms
    // QML timer and the C++ requestActivateByName retry, which returned true
    // (controls found) even when the controls were still disabled during the
    // page transition.
    Connections {
        target: EditudePresenceModel
        function onScoreReady() {
            focusRetry.attempts = 0
            focusRetry.start()
        }
    }

    Timer {
        id: focusRetry
        interval: 50
        repeat: true
        property int attempts: 0
        onTriggered: {
            root.parent.forceFocusIn()
            if (root.parent.activeFocus || ++attempts >= 40) {
                stop()
            }
        }
    }

    // Annotation highlight rectangles — faint/active beat-range highlights.
    // Rendered below presence cursors (lower z-order in QML document order).
    Repeater {
        model: EditudeAnnotationOverlayModel
        delegate: Rectangle {
            x: model.screenRect.x
            y: model.screenRect.y
            width: model.screenRect.width
            height: model.screenRect.height
            color: model.rectColor
            enabled: false
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
