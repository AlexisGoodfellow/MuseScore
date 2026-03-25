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
#include "presenceoverlay.h"

#include <QJsonArray>

#include "engraving/dom/segment.h"
#include "engraving/types/fraction.h"
#include "notation/utilities/scorerangeutilities.h"
#include "draw/types/geometry.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

// Six accessible colours distinct from MuseScore's blue selection highlight.
const QVector<QColor> PresenceOverlay::k_palette = {
    QColor(0xFF, 0x7F, 0x00, 0x60),  // orange
    QColor(0x2C, 0xA0, 0x2C, 0x60),  // green
    QColor(0xD6, 0x27, 0x28, 0x60),  // red
    QColor(0x94, 0x67, 0xBD, 0x60),  // purple
    QColor(0x8C, 0x56, 0x4B, 0x60),  // brown
    QColor(0xE3, 0x77, 0xC2, 0x60),  // pink
};

void PresenceOverlay::updateCursor(const QString& contributorId, const QJsonObject& selection)
{
    const QString state = selection.value("state").toString();

    if (state == "none" || state.isEmpty()) {
        m_cursors.remove(contributorId);
        return;
    }

    RemoteCursor cursor;
    cursor.contributorId = contributorId;
    cursor.state         = state;
    cursor.startStaff    = selection.value("start_staff").toInt(0);
    cursor.endStaff      = selection.value("end_staff").toInt(0);
    cursor.startTick     = selection.value("start_tick").toInt(0);
    cursor.endTick       = selection.value("end_tick").toInt(0);
    cursor.color         = colorFor(contributorId);

    const QJsonArray ticks = selection.value("element_ticks").toArray();
    for (const auto& v : ticks) {
        cursor.elementTicks.append(v.toInt());
    }
    const QJsonArray staves = selection.value("element_staves").toArray();
    for (const auto& v : staves) {
        cursor.elementStaves.append(v.toInt());
    }

    m_cursors.insert(contributorId, cursor);
}

void PresenceOverlay::clear()
{
    m_cursors.clear();
}

QColor PresenceOverlay::colorFor(const QString& contributorId)
{
    auto it = m_colorMap.find(contributorId);
    if (it != m_colorMap.end()) {
        return it.value();
    }
    // Stable assignment: hash the ID string to a palette index.
    size_t h = qHash(contributorId);
    QColor c = k_palette[static_cast<int>(h % static_cast<size_t>(k_palette.size()))];
    m_colorMap.insert(contributorId, c);
    return c;
}

QVector<muse::RectF> PresenceOverlay::reprojectRange(const Score* score, const RemoteCursor& cursor)
{
    if (!score || cursor.startTick > cursor.endTick) {
        return {};
    }

    using namespace mu::engraving;

    const Fraction startFraction = Fraction::fromTicks(cursor.startTick);
    const Fraction endFraction   = Fraction::fromTicks(cursor.endTick);

    Segment* startSeg = score->tick2segment(startFraction, true, SegmentType::ChordRest);
    Segment* endSeg   = score->tick2segment(endFraction,   false, SegmentType::ChordRest);

    if (!startSeg || !endSeg) {
        return {};
    }

    const auto& rects = mu::notation::ScoreRangeUtilities::boundingArea(
        score, startSeg, endSeg,
        static_cast<staff_idx_t>(cursor.startStaff),
        static_cast<staff_idx_t>(cursor.endStaff));

    QVector<muse::RectF> result;
    result.reserve(static_cast<int>(rects.size()));
    for (const auto& r : rects) {
        result.append(r);
    }
    return result;
}
