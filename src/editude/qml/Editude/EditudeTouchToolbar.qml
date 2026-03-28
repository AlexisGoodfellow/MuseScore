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
import QtQuick.Layouts
import Editude 1.0

/**
 * Touch-optimized note entry toolbar for WASM/iPad.
 *
 * Provides duration selection, accidentals, and tool mode switching
 * for touch input when no physical keyboard is available. Dispatches
 * the same MuseScore actions as keyboard shortcuts (e.g., "note-input",
 * "pad-note-4" for quarter note, "sharp" for accidental).
 *
 * Place as a sibling of EditudeNotationOverlay in NotationView.qml:
 *
 *   EditudeTouchToolbar { anchors.bottom: parent.bottom }
 *
 * Only visible when EditudePresenceModel.touchToolbarVisible is true
 * (set by the C++ module when OS_IS_WASM is detected at runtime, or
 * when an external flag is set).
 */
Item {
    id: toolbar
    anchors.left: parent.left
    anchors.right: parent.right
    height: visible ? toolbarContent.implicitHeight + 16 : 0
    visible: EditudePresenceModel.touchToolbarVisible

    // Semi-transparent backdrop.
    Rectangle {
        anchors.fill: parent
        color: "#e6222222"
    }

    ColumnLayout {
        id: toolbarContent
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // Row 1: Tool mode selector.
        RowLayout {
            spacing: 6
            Layout.alignment: Qt.AlignHCenter

            ToolbarButton {
                text: "Select"
                icon: "\u2190"  // ←
                highlighted: !EditudePresenceModel.noteInputActive
                onClicked: EditudePresenceModel.dispatchAction("escape")
            }
            ToolbarButton {
                text: "Note"
                icon: "\u266A"  // ♪
                highlighted: EditudePresenceModel.noteInputActive
                onClicked: EditudePresenceModel.dispatchAction("note-input")
            }
            ToolbarButton {
                text: "Rest"
                icon: "\uD834\uDD3E"
                onClicked: EditudePresenceModel.dispatchAction("rest")
            }

            // Separator.
            Rectangle { width: 1; height: 32; color: "#555" }

            ToolbarButton {
                text: "\u266F"  // ♯
                onClicked: EditudePresenceModel.dispatchAction("sharp2")
            }
            ToolbarButton {
                text: "\u266E"  // ♮
                onClicked: EditudePresenceModel.dispatchAction("nat")
            }
            ToolbarButton {
                text: "\u266D"  // ♭
                onClicked: EditudePresenceModel.dispatchAction("flat2")
            }

            Rectangle { width: 1; height: 32; color: "#555" }

            ToolbarButton {
                text: "Tie"
                onClicked: EditudePresenceModel.dispatchAction("tie")
            }
            ToolbarButton {
                text: "Dot"
                onClicked: EditudePresenceModel.dispatchAction("pad-dot")
            }
        }

        // Row 2: Duration buttons.
        RowLayout {
            spacing: 4
            Layout.alignment: Qt.AlignHCenter

            Repeater {
                // Durations: whole, half, quarter, 8th, 16th, 32nd, 64th.
                // MuseScore actions: pad-note-1 through pad-note-64.
                model: [
                    { label: "1", action: "pad-note-1" },
                    { label: "½", action: "pad-note-2" },
                    { label: "¼", action: "pad-note-4" },
                    { label: "⅛", action: "pad-note-8" },
                    { label: "1⁄16", action: "pad-note-16" },
                    { label: "1⁄32", action: "pad-note-32" },
                    { label: "1⁄64", action: "pad-note-64" }
                ]

                delegate: ToolbarButton {
                    text: modelData.label
                    onClicked: EditudePresenceModel.dispatchAction(modelData.action)
                }
            }
        }

        // Row 3: Pitch buttons (C through B) — visible only in note input mode.
        RowLayout {
            spacing: 4
            Layout.alignment: Qt.AlignHCenter
            visible: EditudePresenceModel.noteInputActive

            Repeater {
                // MuseScore note input actions: note-c through note-b.
                model: [
                    { label: "C", action: "note-c" },
                    { label: "D", action: "note-d" },
                    { label: "E", action: "note-e" },
                    { label: "F", action: "note-f" },
                    { label: "G", action: "note-g" },
                    { label: "A", action: "note-a" },
                    { label: "B", action: "note-b" }
                ]

                delegate: ToolbarButton {
                    text: modelData.label
                    onClicked: EditudePresenceModel.dispatchAction(modelData.action)
                    font.bold: true
                }
            }

            Rectangle { width: 1; height: 32; color: "#555" }

            ToolbarButton {
                text: "\u2191"  // ↑ octave up
                onClicked: EditudePresenceModel.dispatchAction("pitch-up-octave")
            }
            ToolbarButton {
                text: "\u2193"  // ↓ octave down
                onClicked: EditudePresenceModel.dispatchAction("pitch-down-octave")
            }
        }
    }

    // Individual toolbar button.
    component ToolbarButton: Rectangle {
        property alias text: label.text
        property alias icon: label.text
        property alias font: label.font
        property bool highlighted: false
        signal clicked()

        implicitWidth: Math.max(44, label.implicitWidth + 16)
        implicitHeight: 40
        radius: 6
        color: highlighted ? "#3388ff" : mouseArea.pressed ? "#555" : "#333"

        Text {
            id: label
            anchors.centerIn: parent
            color: "#ffffff"
            font.pixelSize: 15
        }

        MouseArea {
            id: mouseArea
            anchors.fill: parent
            onClicked: parent.clicked()
        }
    }
}
