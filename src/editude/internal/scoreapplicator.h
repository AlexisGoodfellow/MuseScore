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

#include <QJsonObject>

#include "engraving/dom/score.h"
#include "engraving/types/types.h"

namespace mu::editude::internal {
class ScoreApplicator {
public:
    // Dispatches on payload["type"]. Returns false for unrecognised types.
    bool apply(mu::engraving::Score* score, const QJsonObject& payload);

private:
    bool applyInsertNote(mu::engraving::Score* score, const QJsonObject& payload);
    static int pitchToMidi(const QString& step, int octave, const QString& accidental);
    static mu::engraving::DurationType parseDurationType(const QString& name);
};
} // namespace mu::editude::internal
