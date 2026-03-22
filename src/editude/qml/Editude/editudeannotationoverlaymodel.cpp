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
#include "editudeannotationoverlaymodel.h"

using namespace mu::editude::internal;

EditudeAnnotationOverlayModel* EditudeAnnotationOverlayModel::s_instance = nullptr;

EditudeAnnotationOverlayModel::EditudeAnnotationOverlayModel(QObject* parent)
    : QAbstractListModel(parent)
{
    if (!s_instance) {
        s_instance = this;
    }
}

EditudeAnnotationOverlayModel* EditudeAnnotationOverlayModel::instance()
{
    if (!s_instance) {
        s_instance = new EditudeAnnotationOverlayModel();
    }
    return s_instance;
}

void EditudeAnnotationOverlayModel::setCanvasData(
    const QVector<QPair<QColor, QVector<muse::RectF>>>& canvasData)
{
    // Count incoming rows before modifying anything.
    int newCount = 0;
    for (const auto& [color, rects] : canvasData) {
        newCount += rects.size();
    }

    if (newCount != m_rows.size()) {
        // Row count changed — full reset (properly brackets the modification).
        beginResetModel();
        m_canvasData = canvasData;
        remapRows();
        endResetModel();
    } else {
        // Same row count — update positions in place.
        m_canvasData = canvasData;
        remapRows();
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0), index(m_rows.size() - 1));
        }
    }
}

void EditudeAnnotationOverlayModel::setNotationViewMatrix(const QVariant& matrix)
{
    m_matrix = matrix.value<QTransform>();
    // Immediate remap with cached canvas rects for smooth scroll tracking.
    remapRows();
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0), index(m_rows.size() - 1), { RectRole });
    }
    // Signal the service to recompute canvas rects from the live score
    // layout — measures may have reflowed due to zoom or resize.
    emit viewMatrixChanged();
}

void EditudeAnnotationOverlayModel::remapRows()
{
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
}

int EditudeAnnotationOverlayModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant EditudeAnnotationOverlayModel::data(const QModelIndex& index, int role) const
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

QHash<int, QByteArray> EditudeAnnotationOverlayModel::roleNames() const
{
    return {
        { RectRole,  "screenRect" },
        { ColorRole, "rectColor"  },
    };
}
