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

    Q_PROPERTY(QString toastText READ toastText NOTIFY toastTextChanged)
    Q_PROPERTY(bool noteInputActive READ noteInputActive NOTIFY noteInputActiveChanged)

public:
    enum Roles {
        RectRole  = Qt::UserRole + 1,
        ColorRole = Qt::UserRole + 2,
    };

    explicit EditudePresenceModel(QObject* parent = nullptr);

    // Returns the application-scoped singleton.  Creates it on first call.
    static EditudePresenceModel* instance();

    // Called by EditudeService when presence data changes.
    // canvasData: list of (colour, list-of-canvas-rects) per contributor cursor.
    void setCanvasData(const QVector<QPair<QColor, QVector<muse::RectF>>>& canvasData);

    // Shows a transient toast notification; auto-clears after 4 s.
    void showToast(const QString& text);

    // Called by EditudeService when a notation has finished loading
    // (including any bootstrap ops).  QML uses this to request focus
    // on the notation paint view so that keyboard shortcuts work.
    void notifyScoreReady();

    // Called by EditudeService after applying a remote op.  Emits a signal
    // that the QML overlay handles by calling update() on the parent
    // NotationPaintView.  On WASM, QQuickPaintedItem::update() marks the
    // item dirty but may not schedule a new animation frame; routing through
    // QML's signal processing pipeline ensures the scene graph syncs.
    void kickSceneGraph();

    QString toastText() const { return m_toastText; }
    bool noteInputActive() const { return m_noteInputActive; }

    void setNoteInputActive(bool active);

    // Called by QML when notationView.matrix changes.
    Q_INVOKABLE void setNotationViewMatrix(const QVariant& matrix);

    // Called by QML touch toolbar to dispatch MuseScore actions.
    Q_INVOKABLE void dispatchAction(const QString& actionCode);

    // QAbstractListModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    void toastTextChanged();
    void noteInputActiveChanged();
    void scoreReady();
    void sceneGraphKickRequested();
    void actionDispatched(const QString& actionCode);

private:
    void remapRows();

    struct Row {
        QRectF screenRect;
        QColor color;
    };

    QVector<QPair<QColor, QVector<muse::RectF>>> m_canvasData;
    QTransform m_matrix;
    QVector<Row> m_rows;

    QString m_toastText;
    QTimer* m_toastTimer = nullptr;
    bool m_noteInputActive = false;

    static EditudePresenceModel* s_instance;
};

} // namespace mu::editude::internal
