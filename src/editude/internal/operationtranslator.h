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

#include <optional>
#include <unordered_set>

#include <QJsonObject>
#include <QString>

#include "engraving/editing/undo.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/note.h"
#include "engraving/types/types.h"

namespace mu::editude::internal {
class OperationTranslator
{
public:
    // Returns nullopt if this change type is not (yet) handled.
    std::optional<QJsonObject> translate(
        mu::engraving::EngravingObject* obj,
        const std::unordered_set<mu::engraving::CommandType>& cmds,
        const QString& partId);

private:
    QJsonObject buildInsertNote(mu::engraving::Note* note, const QString& partId);
    static QJsonObject pitchJson(int tpc, int octave);
    static QString durationTypeName(mu::engraving::DurationType dt);
};
}
