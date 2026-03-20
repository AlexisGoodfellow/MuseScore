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
#include "editudepresencemodel.h"

using namespace mu::editude::internal;

EditudePresenceModel* EditudePresenceModel::s_instance = nullptr;

EditudePresenceModel::EditudePresenceModel(QObject* parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    }
}

EditudePresenceModel* EditudePresenceModel::instance()
{
    if (!s_instance) {
        s_instance = new EditudePresenceModel();
    }
    return s_instance;
}

void EditudePresenceModel::setCanvasData(
    const QVector<QPair<QColor, QVector<muse::RectF>>>& canvasData)
{
    m_canvasData = canvasData;
    rebuild();
}

void EditudePresenceModel::showToast(const QString& text)
{
    m_toastText = text;
    emit toastTextChanged();

    if (!m_toastTimer) {
        m_toastTimer = new QTimer(this);
        m_toastTimer->setSingleShot(true);
        connect(m_toastTimer, &QTimer::timeout, this, [this]() {
            m_toastText.clear();
            emit toastTextChanged();
        });
    }
    m_toastTimer->start(4000);
}

void EditudePresenceModel::notifyScoreReady()
{
    emit scoreReady();
}

void EditudePresenceModel::setNotationViewMatrix(const QVariant& matrix)
{
    m_matrix = matrix.value<QTransform>();
    rebuild();
}

void EditudePresenceModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    for (const auto& [color, rects] : m_canvasData) {
        for (const muse::RectF& r : rects) {
            QRectF qr(r.x(), r.y(), r.width(), r.height());
            Row row;
            row.screenRect = m_matrix.mapRect(qr);
            row.color      = color;
            m_rows.append(row);
        }
    }
    endResetModel();
}

int EditudePresenceModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant EditudePresenceModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }
    const Row& row = m_rows.at(index.row());
    switch (role) {
    case RectRole:  return row.screenRect;
    case ColorRole: return row.color;
    default:        return {};
    }
}

QHash<int, QByteArray> EditudePresenceModel::roleNames() const
{
    return {
        { RectRole,  "screenRect" },
        { ColorRole, "rectColor"  },
    };
}
