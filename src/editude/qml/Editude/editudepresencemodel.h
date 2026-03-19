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
#include <QColor>
#include <QJSEngine>
#include <QQmlEngine>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QTransform>
#include <QVariant>
#include <QVector>
#include <QPair>
#include <QtQml/qqml.h>

#include "draw/types/geometry.h"

namespace mu::editude::internal {

/**
 * QAbstractListModel that holds the current remote presence cursor rects,
 * pre-transformed from canvas coordinates to screen coordinates using the
 * notation view's matrix.
 *
 * Each row represents one rectangle to paint (a single cursor may produce
 * multiple rows if its selection spans more than one system).
 *
 * Roles:
 *   RectRole  (Qt::UserRole+1) → QRectF  — screen-space rectangle
 *   ColorRole (Qt::UserRole+2) → QColor  — contributor's assigned colour
 */
class EditudePresenceModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString toastText READ toastText NOTIFY toastTextChanged)

public:
    enum Roles {
        RectRole  = Qt::UserRole + 1,
        ColorRole = Qt::UserRole + 2,
    };

    explicit EditudePresenceModel(QObject* parent = nullptr);

    // Qt 6 QML_SINGLETON factory — returns the C++-created instance so that
    // both EditudeService and QML operate on the same object.
    static EditudePresenceModel* create(QQmlEngine*, QJSEngine*);

    // Called by EditudeService when presence data changes.
    // canvasData: list of (colour, list-of-canvas-rects) per contributor cursor.
    void setCanvasData(const QVector<QPair<QColor, QVector<muse::RectF>>>& canvasData);

    // Shows a transient toast notification; auto-clears after 4 s.
    void showToast(const QString& text);

    // Called by EditudeService when a notation has finished loading
    // (including any bootstrap ops).  QML uses this to request focus
    // on the notation paint view so that keyboard shortcuts work.
    void notifyScoreReady();

    QString toastText() const { return m_toastText; }

    // Called by QML when notationView.matrix changes.
    Q_INVOKABLE void setNotationViewMatrix(const QVariant& matrix);

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void toastTextChanged();
    void scoreReady();

private:
    void rebuild();

    struct Row {
        QRectF screenRect;
        QColor color;
    };

    QVector<QPair<QColor, QVector<muse::RectF>>> m_canvasData;
    QTransform m_matrix;
    QVector<Row> m_rows;

    QString m_toastText;
    QTimer* m_toastTimer = nullptr;

    static EditudePresenceModel* s_instance;
};

} // namespace mu::editude::internal
