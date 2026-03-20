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
#include "editudeannotationmodel.h"
#include "log.h"

using namespace mu::editude::internal;

EditudeAnnotationModel* EditudeAnnotationModel::s_instance = nullptr;

EditudeAnnotationModel::EditudeAnnotationModel(QObject* parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    }
}

void EditudeAnnotationModel::setPanelVisible(bool visible)
{
    if (m_panelVisible == visible) {
        return;
    }
    m_panelVisible = visible;
    emit panelVisibleChanged();
}

EditudeAnnotationModel* EditudeAnnotationModel::instance()
{
    if (!s_instance) {
        s_instance = new EditudeAnnotationModel();
    }
    return s_instance;
}

EditudeAnnotationModel::Row EditudeAnnotationModel::rowFromJson(const QJsonObject& obj)
{
    Row r;
    r.annotationId  = obj.value("id").toString();
    r.partId        = obj.value("part_id").toString();
    r.body          = obj.value("body").toString();
    r.resolved      = obj.value("resolved").toBool(false);
    r.orphaned      = obj.value("orphaned").toBool(false);
    r.authorId      = obj.value("author_id").toString();
    r.startBeatNum  = static_cast<qint64>(obj.value("start_beat_num").toDouble(0));
    r.startBeatDen  = static_cast<qint64>(obj.value("start_beat_den").toDouble(1));
    r.endBeatNum    = static_cast<qint64>(obj.value("end_beat_num").toDouble(0));
    r.endBeatDen    = static_cast<qint64>(obj.value("end_beat_den").toDouble(1));
    r.replyCount    = obj.value("replies").toArray().size();
    r.createdAt     = obj.value("created_at").toString();
    return r;
}

void EditudeAnnotationModel::loadFromJson(const QJsonArray& annotations)
{
    beginResetModel();
    m_rows.clear();
    for (const QJsonValue& v : annotations) {
        m_rows.append(rowFromJson(v.toObject()));
    }
    endResetModel();
}

void EditudeAnnotationModel::addAnnotation(const QJsonObject& annotation)
{
    // Insert sorted by (start_beat_num / start_beat_den) ascending.
    const double newPos = static_cast<double>(
        static_cast<qint64>(annotation.value("start_beat_num").toDouble(0)))
        / static_cast<double>(
            static_cast<qint64>(annotation.value("start_beat_den").toDouble(1)));

    int insertAt = m_rows.size();
    for (int i = 0; i < m_rows.size(); ++i) {
        const double pos = static_cast<double>(m_rows[i].startBeatNum)
                           / static_cast<double>(m_rows[i].startBeatDen);
        if (newPos < pos) {
            insertAt = i;
            break;
        }
    }

    beginInsertRows(QModelIndex(), insertAt, insertAt);
    m_rows.insert(insertAt, rowFromJson(annotation));
    endInsertRows();
}

void EditudeAnnotationModel::incrementReplyCount(const QString& annotationId)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].annotationId == annotationId) {
            m_rows[i].replyCount++;
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx, { ReplyCountRole });
            return;
        }
    }
}

int EditudeAnnotationModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant EditudeAnnotationModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }
    const Row& r = m_rows.at(index.row());
    switch (role) {
    case AnnotationIdRole: return r.annotationId;
    case PartIdRole:       return r.partId;
    case BodyRole:         return r.body;
    case ResolvedRole:     return r.resolved;
    case OrphanedRole:     return r.orphaned;
    case AuthorIdRole:     return r.authorId;
    case StartBeatNumRole: return r.startBeatNum;
    case StartBeatDenRole: return r.startBeatDen;
    case EndBeatNumRole:   return r.endBeatNum;
    case EndBeatDenRole:   return r.endBeatDen;
    case ReplyCountRole:   return r.replyCount;
    case CreatedAtRole:    return r.createdAt;
    default:               return {};
    }
}

QHash<int, QByteArray> EditudeAnnotationModel::roleNames() const
{
    return {
        { AnnotationIdRole, "annotationId" },
        { PartIdRole,       "partId"       },
        { BodyRole,         "body"         },
        { ResolvedRole,     "resolved"     },
        { OrphanedRole,     "orphaned"     },
        { AuthorIdRole,     "authorId"     },
        { StartBeatNumRole, "startBeatNum" },
        { StartBeatDenRole, "startBeatDen" },
        { EndBeatNumRole,   "endBeatNum"   },
        { EndBeatDenRole,   "endBeatDen"   },
        { ReplyCountRole,   "replyCount"   },
        { CreatedAtRole,    "createdAt"    },
    };
}
