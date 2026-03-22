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
 * DAW-style annotation side panel with interactive comment creation,
 * expandable cards, inline replies, and resolve toggle.
 *
 * Clicking an annotation emits annotationSelected(annotationId) so the
 * caller can scroll the notation view to the relevant beat range.
 */
Rectangle {
    id: root

    color: ui.theme.backgroundPrimaryColor

    signal annotationSelected(string annotationId)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ──────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 36
            color: ui.theme.backgroundSecondaryColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8

                Text {
                    text: qsTr("Comments")
                    color: ui.theme.fontPrimaryColor
                    font.pixelSize: 13
                    font.bold: true
                    Layout.fillWidth: true
                }

                FlatButton {
                    icon: IconCode.PLUS
                    toolTipTitle: qsTr("Add comment")
                    width: 28
                    height: 28
                    onClicked: EditudeAnnotationModel.requestCreation()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: ui.theme.strokeColor
        }

        // ── Inline creation input ───────────────────────────────────────
        Rectangle {
            id: creationInput
            Layout.fillWidth: true
            visible: EditudeAnnotationModel.creationActive
            color: ui.theme.backgroundPrimaryColor
            implicitHeight: creationCol.implicitHeight + 16

            ColumnLayout {
                id: creationCol
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: 8
                }
                spacing: 6

                Text {
                    text: qsTr("New comment")
                    color: ui.theme.fontSecondaryColor
                    font.pixelSize: 11
                }

                TextArea {
                    id: creationTextArea
                    Layout.fillWidth: true
                    placeholderText: qsTr("Write a comment...")
                    wrapMode: TextEdit.Wrap
                    color: ui.theme.fontPrimaryColor
                    font.pixelSize: 12
                    background: Rectangle {
                        color: ui.theme.backgroundSecondaryColor
                        radius: 4
                        border.color: ui.theme.strokeColor
                        border.width: 1
                    }
                }

                RowLayout {
                    spacing: 6
                    Layout.alignment: Qt.AlignRight

                    FlatButton {
                        text: qsTr("Cancel")
                        onClicked: {
                            creationTextArea.text = ""
                            EditudeAnnotationModel.cancelCreation()
                        }
                    }

                    FlatButton {
                        text: qsTr("Submit")
                        accentButton: true
                        enabled: creationTextArea.text.trim().length > 0
                        onClicked: {
                            EditudeAnnotationModel.submitAnnotation(creationTextArea.text)
                            creationTextArea.text = ""
                        }
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: ui.theme.strokeColor
            }
        }

        // ── Annotation list ─────────────────────────────────────────────
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
                required property string authorName
                required property bool expanded
                required property var replies

                width: listView.width
                implicitHeight: contentCol.implicitHeight + 16
                color: delegate.resolved
                    ? Qt.alpha(ui.theme.backgroundSecondaryColor, 0.5)
                    : ui.theme.backgroundPrimaryColor

                // Left color bar
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

                    // Author + timestamp (click to collapse when expanded)
                    RowLayout {
                        spacing: 6
                        Text {
                            text: delegate.authorName || qsTr("Unknown")
                            color: ui.theme.fontPrimaryColor
                            font.pixelSize: 11
                            font.bold: true
                        }
                        Text {
                            text: delegate.createdAt
                            color: ui.theme.fontSecondaryColor
                            font.pixelSize: 10
                            Layout.fillWidth: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            visible: delegate.expanded
                            cursorShape: Qt.PointingHandCursor
                            onClicked: EditudeAnnotationModel.setExpanded("")
                        }
                    }

                    // Body text
                    Text {
                        Layout.fillWidth: true
                        text: delegate.body
                        color: delegate.resolved
                            ? ui.theme.fontSecondaryColor
                            : ui.theme.fontPrimaryColor
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        maximumLineCount: delegate.expanded ? 0 : 3
                        elide: delegate.expanded ? Text.ElideNone : Text.ElideRight
                    }

                    // Status badges
                    RowLayout {
                        spacing: 8
                        visible: !delegate.expanded

                        Text {
                            text: delegate.orphaned ? qsTr("passage deleted")
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

                    // ── Expanded section: replies + actions ─────────────
                    ColumnLayout {
                        visible: delegate.expanded
                        spacing: 4
                        Layout.fillWidth: true

                        // Separator before replies
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: ui.theme.strokeColor
                            opacity: 0.5
                            visible: delegate.replies.length > 0
                        }

                        // Reply list
                        Repeater {
                            model: delegate.replies

                            ColumnLayout {
                                spacing: 2
                                Layout.fillWidth: true
                                Layout.leftMargin: 8

                                RowLayout {
                                    spacing: 4
                                    Text {
                                        text: modelData.author_name || qsTr("Unknown")
                                        color: ui.theme.fontPrimaryColor
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                    Text {
                                        text: modelData.created_at || ""
                                        color: ui.theme.fontSecondaryColor
                                        font.pixelSize: 9
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.body || ""
                                    color: ui.theme.fontPrimaryColor
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        // Reply input
                        RowLayout {
                            spacing: 4
                            Layout.fillWidth: true
                            Layout.topMargin: 4

                            TextField {
                                id: replyField
                                Layout.fillWidth: true
                                placeholderText: qsTr("Reply...")
                                font.pixelSize: 11
                                color: ui.theme.fontPrimaryColor
                                background: Rectangle {
                                    color: ui.theme.backgroundSecondaryColor
                                    radius: 3
                                    border.color: ui.theme.strokeColor
                                    border.width: 1
                                }
                            }

                            FlatButton {
                                text: qsTr("Send")
                                enabled: replyField.text.trim().length > 0
                                onClicked: {
                                    EditudeAnnotationModel.submitReply(
                                        delegate.annotationId, replyField.text)
                                    replyField.text = ""
                                }
                            }
                        }

                        // Action row: resolve toggle + delete
                        RowLayout {
                            spacing: 8
                            Layout.topMargin: 4

                            FlatButton {
                                text: delegate.resolved
                                    ? qsTr("Unresolve")
                                    : qsTr("Resolve")
                                onClicked: {
                                    EditudeAnnotationModel.toggleResolve(
                                        delegate.annotationId, !delegate.resolved)
                                }
                            }

                            FlatButton {
                                visible: delegate.resolved
                                text: qsTr("Delete")
                                onClicked: {
                                    EditudeAnnotationModel.deleteAnnotation(
                                        delegate.annotationId)
                                }
                            }
                        }
                    }
                }

                // Bottom separator
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: ui.theme.strokeColor
                    opacity: 0.5
                }

                MouseArea {
                    anchors.fill: parent
                    // Let interactive children (buttons, text fields) receive
                    // clicks when the card is expanded.
                    visible: !delegate.expanded
                    onClicked: {
                        EditudeAnnotationModel.setExpanded(delegate.annotationId)
                        root.annotationSelected(delegate.annotationId)
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: listView.count === 0 && !EditudeAnnotationModel.creationActive
                text: qsTr("No comments yet")
                color: ui.theme.fontSecondaryColor
                font.pixelSize: 12
            }
        }
    }
}
