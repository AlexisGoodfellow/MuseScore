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
#pragma once

#include <QAbstractListModel>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlEngine>
#include <QString>
#include <QVector>
#include <QtQml/qqml.h>

namespace mu::editude::internal {

/**
 * QAbstractListModel that holds the current project's annotations.
 *
 * Populated from REST GET /projects/{pid}/annotations on project join,
 * then updated incrementally from annotation_created and
 * annotation_reply_created WebSocket broadcast messages.
 *
 * Roles:
 *   AnnotationIdRole  — QString  — annotation UUID
 *   PartIdRole        — QString  — part UUID (elementToUuid key)
 *   BodyRole          — QString  — annotation body text
 *   ResolvedRole      — bool
 *   OrphanedRole      — bool
 *   AuthorIdRole      — QString  — contributor UUID (may be empty)
 *   StartBeatNumRole  — qint64   — Fraction numerator, whole-note units
 *   StartBeatDenRole  — qint64   — Fraction denominator
 *   EndBeatNumRole    — qint64
 *   EndBeatDenRole    — qint64
 *   ReplyCountRole    — int
 *   CreatedAtRole     — QString  — ISO 8601 timestamp
 */
class EditudeAnnotationModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Roles {
        AnnotationIdRole = Qt::UserRole + 1,
        PartIdRole,
        BodyRole,
        ResolvedRole,
        OrphanedRole,
        AuthorIdRole,
        StartBeatNumRole,
        StartBeatDenRole,
        EndBeatNumRole,
        EndBeatDenRole,
        ReplyCountRole,
        CreatedAtRole,
    };

    explicit EditudeAnnotationModel(QObject* parent = nullptr);

    // Qt 6 QML_SINGLETON factory — returns the C++-created instance so that
    // both EditudeService and QML operate on the same object.
    static EditudeAnnotationModel* create(QQmlEngine*, QJSEngine*);

    // Replace all annotations (called after REST fetch on project join).
    void loadFromJson(const QJsonArray& annotations);

    // Add a single annotation from an annotation_created WS message.
    void addAnnotation(const QJsonObject& annotation);

    // Increment the reply count for the given annotation UUID.
    void incrementReplyCount(const QString& annotationId);

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    struct Row {
        QString annotationId;
        QString partId;
        QString body;
        bool resolved = false;
        bool orphaned = false;
        QString authorId;
        qint64 startBeatNum = 0;
        qint64 startBeatDen = 1;
        qint64 endBeatNum = 0;
        qint64 endBeatDen = 1;
        int replyCount = 0;
        QString createdAt;
    };

    static Row rowFromJson(const QJsonObject& obj);

    QVector<Row> m_rows;

    static EditudeAnnotationModel* s_instance;
};

} // namespace mu::editude::internal
