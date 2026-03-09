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
#include "operationtranslator.h"

#include <QJsonObject>
#include <QUuid>

#include "engraving/dom/chord.h"
#include "engraving/types/fraction.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

std::optional<QJsonObject> OperationTranslator::translate(
    EngravingObject* obj,
    const std::unordered_set<CommandType>& cmds,
    const QString& partId)
{
    if (cmds.find(CommandType::AddElement) == cmds.end()) {
        return std::nullopt;
    }
    if (!obj || obj->type() != ElementType::NOTE) {
        return std::nullopt;
    }
    return buildInsertNote(static_cast<Note*>(obj), partId);
}

QJsonObject OperationTranslator::buildInsertNote(Note* note, const QString& partId)
{
    const Fraction tick = note->chord()->tick();
    QJsonObject beat;
    beat["numerator"] = tick.numerator();
    beat["denominator"] = tick.denominator();

    const DurationType dt = note->chord()->durationType().type();
    const int dots = note->chord()->dots();

    QJsonObject payload;
    payload["type"] = "InsertNote";
    payload["part_id"] = partId;
    payload["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    payload["beat"] = beat;
    payload["duration"] = durationTypeName(dt);
    if (dots > 0) {
        payload["dots"] = dots;
    }
    payload["track"] = static_cast<int>(note->track());
    payload["pitch"] = pitchJson(note->tpc1(), note->octave());

    return payload;
}

// Converts a MuseScore TPC (tonal pitch class, circle-of-fifths integer) and
// octave into a JSON pitch object with "step", "octave", and optional "accidental".
//
// TPC circle-of-fifths layout: F=13, C=14, G=15, D=16, A=17, E=18, B=19 (natural),
// each flat subtracts 7, each sharp adds 7.
QJsonObject OperationTranslator::pitchJson(int tpc, int octave)
{
    static const char* const kSteps[] = { "F", "C", "G", "D", "A", "E", "B" };
    static const int kNaturalTpc[] = { 13, 14, 15, 16, 17, 18, 19 };
    static const char* const kAccidentals[] = {
        "double-flat", "flat", nullptr, "sharp", "double-sharp"
    };

    const int stepIndex = (tpc + 1) % 7;
    const int accOffset = (tpc - kNaturalTpc[stepIndex]) / 7; // range: -2..+2

    QJsonObject pitch;
    pitch["step"] = kSteps[stepIndex];
    pitch["octave"] = octave;

    const int accIdx = accOffset + 2; // map -2..+2 → 0..4
    if (accIdx >= 0 && accIdx <= 4 && kAccidentals[accIdx]) {
        pitch["accidental"] = kAccidentals[accIdx];
    }

    return pitch;
}

QString OperationTranslator::durationTypeName(DurationType dt)
{
    switch (dt) {
    case DurationType::V_WHOLE:   return QStringLiteral("whole");
    case DurationType::V_HALF:    return QStringLiteral("half");
    case DurationType::V_QUARTER: return QStringLiteral("quarter");
    case DurationType::V_EIGHTH:  return QStringLiteral("eighth");
    case DurationType::V_16TH:    return QStringLiteral("16th");
    case DurationType::V_32ND:    return QStringLiteral("32nd");
    case DurationType::V_64TH:    return QStringLiteral("64th");
    default:                      return QStringLiteral("unknown");
    }
}
