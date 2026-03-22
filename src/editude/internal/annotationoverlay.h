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

#include <QColor>
#include <QString>
#include <QVector>

#include "draw/types/geometry.h"
#include "engraving/dom/score.h"

namespace mu::editude::internal {

struct AnnotationRect {
    QString annotationId;
    QColor color;
    QVector<muse::RectF> canvasRects;
};

class AnnotationOverlay
{
public:
    // Reproject a beat range to canvas-coordinate rects, scoped to a single part.
    // Returns an empty vector if the score is null or the range is invalid.
    static QVector<muse::RectF> reprojectBeatRange(
        const mu::engraving::Score* score,
        qint64 startBeatNum, qint64 startBeatDen,
        qint64 endBeatNum, qint64 endBeatDen,
        const QString& partId,
        const QHash<mu::engraving::Part*, QString>& partUuids);
};

} // namespace mu::editude::internal
