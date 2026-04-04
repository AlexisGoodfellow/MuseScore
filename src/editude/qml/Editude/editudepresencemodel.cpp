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
    const QVector<std::tuple<QColor, QString, QVector<muse::RectF>>>& canvasData)
{
    // Count incoming rows before modifying anything.
    int newCount = 0;
    for (const auto& [color, name, rects] : canvasData) {
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

void EditudePresenceModel::kickSceneGraph()
{
    emit sceneGraphKickRequested();
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

void EditudePresenceModel::setNoteInputActive(bool active)
{
    if (m_noteInputActive == active) {
        return;
    }
    m_noteInputActive = active;
    emit noteInputActiveChanged();
}

void EditudePresenceModel::dispatchAction(const QString& actionCode)
{
    // Forward the action to EditudeService via the singleton.
    // EditudeService subscribes to a custom signal, or we use the
    // MuseScore actions dispatcher directly through the singleton pattern.
    // For now, emit a signal that EditudeService connects to.
    Q_UNUSED(actionCode);
    // TODO: Wire to IActionsDispatcher.  This requires access to the IOC
    // context, which the model doesn't have.  Two options:
    // (a) EditudeService sets a std::function callback on the model.
    // (b) Use a signal that EditudeService connects to.
    // Using option (b):
    emit actionDispatched(actionCode);
}

void EditudePresenceModel::setNotationViewMatrix(const QVariant& matrix)
{
    m_matrix = matrix.value<QTransform>();
    // Immediate remap with cached canvas rects for smooth scroll tracking.
    remapRows();
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0), index(m_rows.size() - 1), { RectRole });
    }
}

void EditudePresenceModel::remapRows()
{
    m_rows.clear();
    for (const auto& [color, name, rects] : m_canvasData) {
        bool first = true;
        for (const muse::RectF& r : rects) {
            QRectF qr(r.x(), r.y(), r.width(), r.height());
            Row row;
            row.screenRect = m_matrix.mapRect(qr);
            row.color      = color;
            row.name       = first ? name : QString();
            first = false;
            m_rows.append(row);
        }
    }
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
    case NameRole:  return row.name;
    default:        return {};
    }
}

QHash<int, QByteArray> EditudePresenceModel::roleNames() const
{
    return {
        { RectRole,  "screenRect"  },
        { ColorRole, "rectColor"   },
        { NameRole,  "displayName" },
    };
}
