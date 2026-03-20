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
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include "draw/types/geometry.h"
#include "engraving/dom/score.h"
#include "types/ret.h"

namespace mu::editude::internal {

struct RemoteCursor {
    QString contributorId;
    QString state;            // "single" | "range" | "none"
    QVector<QString> elementIds;
    int startStaff = 0;
    int endStaff   = 0;
    int startTick  = 0;
    int endTick    = 0;
    QColor color;
};

class PresenceOverlay
{
public:
    // Update or clear a contributor's cursor from an incoming presence JSON selection.
    // If state == "none", the entry is removed.
    void updateCursor(const QString& contributorId, const QJsonObject& selection);

    // Remove all cursors (e.g. on project close).
    void clear();

    const QHash<QString, RemoteCursor>& cursors() const { return m_cursors; }

    // Reproject a range cursor to canvas-coordinate rects using the live score.
    // Uses ScoreRangeUtilities::boundingArea() internally.
    // Returns an empty vector if the score is null or the tick range is invalid.
    static QVector<muse::RectF> reprojectRange(const mu::engraving::Score* score,
                                                const RemoteCursor& cursor);

private:
    // Assign a stable colour from the palette based on contributor ID hash.
    QColor colorFor(const QString& contributorId);

    QHash<QString, RemoteCursor> m_cursors;
    QHash<QString, QColor> m_colorMap;

    // Six accessible, saturated colours that contrast with MuseScore's blue selection.
    static const QVector<QColor> k_palette;
};

} // namespace mu::editude::internal
