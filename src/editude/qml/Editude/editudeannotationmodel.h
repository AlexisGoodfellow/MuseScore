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
#pragma once

#include <QAbstractListModel>
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
 * Supports accordion expand/collapse: exactly one annotation may be expanded
 * at a time (ExpandedRole). Expanding an annotation fetches its replies
 * (RepliesRole) and scrolls the score to its beat position.
 */
class EditudeAnnotationModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(bool panelVisible READ panelVisible WRITE setPanelVisible NOTIFY panelVisibleChanged)
    Q_PROPERTY(bool creationActive READ creationActive WRITE setCreationActive NOTIFY creationActiveChanged)
    Q_PROPERTY(QString expandedAnnotationId READ expandedAnnotationId NOTIFY expandedAnnotationIdChanged)

public:
    enum Roles {
        AnnotationIdRole = Qt::UserRole + 1,
        PartIdRole,
        PartIdsRole,
        BodyRole,
        ResolvedRole,
        OrphanedRole,
        AuthorIdRole,
        AuthorNameRole,
        StartBeatNumRole,
        StartBeatDenRole,
        EndBeatNumRole,
        EndBeatDenRole,
        ReplyCountRole,
        CreatedAtRole,
        ExpandedRole,
        RepliesRole,
    };

    explicit EditudeAnnotationModel(QObject* parent = nullptr);

    // Returns the application-scoped singleton.  Creates it on first call.
    static EditudeAnnotationModel* instance();

    // Replace all annotations (called after REST fetch on project join).
    void loadFromJson(const QJsonArray& annotations);

    // Add a single annotation from an annotation_created WS message.
    void addAnnotation(const QJsonObject& annotation);

    // Increment the reply count and append the reply JSON for the given annotation.
    void addReply(const QString& annotationId, const QJsonObject& reply);

    // Update resolved/body for an annotation (from PATCH response or WS).
    void updateAnnotation(const QString& annotationId, const QJsonObject& fields);

    // Remove an annotation from the model (called after successful server DELETE).
    void removeAnnotation(const QString& annotationId);

    // Accordion expand/collapse: set the currently expanded annotation.
    // Pass empty string to collapse all.
    Q_INVOKABLE void setExpanded(const QString& annotationId);

    // QML-invokable annotation actions.
    // These emit signals that EditudeService connects to.
    Q_INVOKABLE void requestCreation();
    Q_INVOKABLE void submitAnnotation(const QString& body);
    Q_INVOKABLE void cancelCreation();
    Q_INVOKABLE void submitReply(const QString& annotationId, const QString& body);
    Q_INVOKABLE void toggleResolve(const QString& annotationId, bool resolved);
    Q_INVOKABLE void deleteAnnotation(const QString& annotationId);

    // Called by the service to populate the creation anchor.
    void setCreationAnchor(const QJsonObject& anchor) { m_creationAnchor = anchor; }

    // Inline creation mode.
    bool creationActive() const { return m_creationActive; }
    void setCreationActive(bool active);

    // Currently expanded annotation ID (empty if none).
    QString expandedAnnotationId() const { return m_expandedId; }

    // Panel visibility toggle (driven by toolbar action).
    bool panelVisible() const { return m_panelVisible; }
    void setPanelVisible(bool visible);

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void panelVisibleChanged();
    void creationActiveChanged();
    void expandedAnnotationIdChanged();
    void annotationExpandedAt(qint64 startBeatNum, qint64 startBeatDen, const QString& partId);

    // Signals for EditudeService to connect to (avoid including editudeservice.h).
    void creationRequested();
    void annotationSubmitted(const QString& partId, const QJsonArray& partIds,
                             qint64 startNum, qint64 startDen,
                             qint64 endNum, qint64 endDen, const QString& body);
    void replySubmitted(const QString& annotationId, const QString& body);
    void resolveToggled(const QString& annotationId, bool resolved);
    void deletionRequested(const QString& annotationId);

private:
    struct Row {
        QString annotationId;
        QString partId;
        QJsonArray partIds;
        QString body;
        bool resolved = false;
        bool orphaned = false;
        QString authorId;
        QString authorName;
        qint64 startBeatNum = 0;
        qint64 startBeatDen = 1;
        qint64 endBeatNum = 0;
        qint64 endBeatDen = 1;
        int replyCount = 0;
        QString createdAt;
        QJsonArray replies;
    };

    static Row rowFromJson(const QJsonObject& obj);

    QVector<Row> m_rows;
    bool m_panelVisible = true;
    bool m_creationActive = false;
    QString m_expandedId;
    QJsonObject m_creationAnchor;  // cached anchor for the in-progress creation

    static EditudeAnnotationModel* s_instance;
};

} // namespace mu::editude::internal
