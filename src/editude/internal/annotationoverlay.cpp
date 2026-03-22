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
#include "annotationoverlay.h"

#include "engraving/dom/part.h"
#include "engraving/dom/segment.h"
#include "engraving/types/fraction.h"
#include "notation/utilities/scorerangeutilities.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

QVector<muse::RectF> AnnotationOverlay::reprojectBeatRange(
    const Score* score,
    qint64 startBeatNum, qint64 startBeatDen,
    qint64 endBeatNum, qint64 endBeatDen,
    const QString& partId,
    const QHash<Part*, QString>& partUuids)
{
    if (!score || partId == "_score") {
        return {};
    }

    // Find the Part* for this partId and its staff range.
    Part* matchedPart = nullptr;
    for (auto it = partUuids.begin(); it != partUuids.end(); ++it) {
        if (it.value() == partId) {
            matchedPart = it.key();
            break;
        }
    }
    if (!matchedPart) {
        return {};
    }

    auto startStaff = score->staffIdx(matchedPart);
    auto endStaff   = startStaff + matchedPart->nstaves();  // exclusive upper bound

    // Convert beat fractions (whole-note units) to tick fractions.
    // Annotations store beats as whole-note fractions, MuseScore uses the same
    // Fraction type internally.
    const Fraction startFrac(static_cast<int>(startBeatNum), static_cast<int>(startBeatDen));
    const Fraction endFrac(static_cast<int>(endBeatNum), static_cast<int>(endBeatDen));

    // For single-element annotations (start == end), extend to the next segment
    // so there's a visible rect.
    Fraction effectiveEnd = endFrac;
    if (startFrac == endFrac) {
        // Find the segment at this tick and use the next one as the end.
        Segment* seg = score->tick2segment(startFrac, true, SegmentType::ChordRest);
        if (seg && seg->next1(SegmentType::ChordRest)) {
            effectiveEnd = seg->next1(SegmentType::ChordRest)->tick();
        } else {
            // Last segment — extend by a quarter note as fallback.
            effectiveEnd = startFrac + Fraction(1, 4);
        }
    }

    Segment* startSeg = score->tick2segment(startFrac, true, SegmentType::ChordRest);
    Segment* endSeg   = score->tick2segment(effectiveEnd, false, SegmentType::ChordRest);

    if (!startSeg || !endSeg) {
        return {};
    }

    const auto& rects = mu::notation::ScoreRangeUtilities::boundingArea(
        score, startSeg, endSeg,
        static_cast<staff_idx_t>(startStaff),
        static_cast<staff_idx_t>(endStaff));

    QVector<muse::RectF> result;
    result.reserve(static_cast<int>(rects.size()));
    for (const auto& r : rects) {
        result.append(r);
    }
    return result;
}
