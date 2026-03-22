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
#include <QColor>
#include <QQmlEngine>
#include <QRectF>
#include <QTransform>
#include <QVariant>
#include <QVector>
#include <QPair>
#include <QtQml/qqml.h>

#include "draw/types/geometry.h"

namespace mu::editude::internal {

/**
 * QAbstractListModel that holds annotation highlight rectangles,
 * pre-transformed from canvas coordinates to screen coordinates using the
 * notation view's matrix.
 *
 * Each row represents one rectangle to paint.  An annotation spanning
 * multiple systems produces one row per system.
 *
 * Roles:
 *   RectRole  (Qt::UserRole+1) → QRectF  — screen-space rectangle
 *   ColorRole (Qt::UserRole+2) → QColor  — highlight colour (accent with alpha)
 */
class EditudeAnnotationOverlayModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        RectRole  = Qt::UserRole + 1,
        ColorRole = Qt::UserRole + 2,
    };

    explicit EditudeAnnotationOverlayModel(QObject* parent = nullptr);

    static EditudeAnnotationOverlayModel* instance();

    // Called by EditudeService when annotation overlay data changes.
    // canvasData: list of (colour, list-of-canvas-rects) per annotation.
    void setCanvasData(const QVector<QPair<QColor, QVector<muse::RectF>>>& canvasData);

    // Called by QML when notationView.matrix changes.
    Q_INVOKABLE void setNotationViewMatrix(const QVariant& matrix);

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    // Emitted when the view matrix changes so the service can recompute
    // canvas rects from the live score layout (measures may have reflowed).
    void viewMatrixChanged();

private:
    void remapRows();

    struct Row {
        QRectF screenRect;
        QColor color;
    };

    QVector<QPair<QColor, QVector<muse::RectF>>> m_canvasData;
    QTransform m_matrix;
    QVector<Row> m_rows;

    static EditudeAnnotationOverlayModel* s_instance;
};

} // namespace mu::editude::internal
