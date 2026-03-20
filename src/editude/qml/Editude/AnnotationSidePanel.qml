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
import QtQuick.Controls

import Muse.Ui
import Muse.UiComponents
import Editude 1.0

/**
 * DAW-style annotation side panel.
 *
 * Lists all annotations for the current project sorted by beat position.
 * Clicking an annotation emits annotationSelected(annotationId) so the
 * caller can scroll the notation view to the relevant beat range.
 *
 * Annotation markers in the score itself are non-interactive decorators
 * rendered separately; this panel is the sole interaction surface.
 */
Rectangle {
    id: root

    color: ui.theme.backgroundPrimaryColor

    signal annotationSelected(string annotationId)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            height: 36
            color: ui.theme.backgroundSecondaryColor

            Text {
                anchors.centerIn: parent
                text: qsTr("Comments")
                color: ui.theme.fontPrimaryColor
                font.pixelSize: 13
                font.bold: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: ui.theme.strokeColor
        }

        ListView {
            id: listView

            Layout.fillWidth: true
            Layout.fillHeight: true

            model: EditudeAnnotationModel
            clip: true

            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: delegate

                required property string annotationId
                required property string body
                required property bool resolved
                required property bool orphaned
                required property int replyCount
                required property string createdAt

                width: listView.width
                height: contentCol.implicitHeight + 16
                color: delegate.resolved
                    ? Qt.alpha(ui.theme.backgroundSecondaryColor, 0.5)
                    : ui.theme.backgroundPrimaryColor

                Rectangle {
                    width: 3
                    height: parent.height
                    color: delegate.orphaned ? ui.theme.errorColor
                         : delegate.resolved  ? ui.theme.fontSecondaryColor
                         :                     ui.theme.accentColor
                }

                ColumnLayout {
                    id: contentCol

                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        leftMargin: 12
                        rightMargin: 8
                        topMargin: 8
                    }
                    spacing: 4

                    Text {
                        Layout.fillWidth: true
                        text: delegate.body
                        color: delegate.resolved
                            ? ui.theme.fontSecondaryColor
                            : ui.theme.fontPrimaryColor
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        spacing: 8

                        Text {
                            text: delegate.orphaned ? qsTr("⚠ passage deleted")
                                : delegate.resolved  ? qsTr("Resolved")
                                :                     ""
                            color: delegate.orphaned ? ui.theme.errorColor
                                 : ui.theme.fontSecondaryColor
                            font.pixelSize: 10
                            visible: delegate.orphaned || delegate.resolved
                        }

                        Text {
                            text: delegate.replyCount > 0
                                ? qsTr("%1 repl%2").arg(delegate.replyCount)
                                      .arg(delegate.replyCount === 1 ? "y" : "ies")
                                : ""
                            color: ui.theme.fontSecondaryColor
                            font.pixelSize: 10
                            visible: delegate.replyCount > 0
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: ui.theme.strokeColor
                    opacity: 0.5
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.annotationSelected(delegate.annotationId)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: listView.count === 0
                text: qsTr("No comments yet")
                color: ui.theme.fontSecondaryColor
                font.pixelSize: 12
            }
        }
    }
}
