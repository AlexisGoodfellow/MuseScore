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
#include "scoreapplicator.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/segment.h"
#include "engraving/types/fraction.h"
#include "engraving/types/typesconv.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

int ScoreApplicator::pitchToMidi(const QString& step, int octave, const QString& accidental)
{
    // Semitone offsets for C D E F G A B
    static const struct { const char* name; int offset; } kSteps[] = {
        { "C", 0 }, { "D", 2 }, { "E", 4 }, { "F", 5 },
        { "G", 7 }, { "A", 9 }, { "B", 11 }
    };

    int stepOffset = -1;
    for (const auto& s : kSteps) {
        if (step == QLatin1String(s.name)) {
            stepOffset = s.offset;
            break;
        }
    }
    if (stepOffset < 0) {
        return -1;
    }

    int accOffset = 0;
    if (accidental == QLatin1String("double-flat")) {
        accOffset = -2;
    } else if (accidental == QLatin1String("flat")) {
        accOffset = -1;
    } else if (accidental == QLatin1String("sharp")) {
        accOffset = 1;
    } else if (accidental == QLatin1String("double-sharp")) {
        accOffset = 2;
    }

    return (octave + 1) * 12 + stepOffset + accOffset;
}

DurationType ScoreApplicator::parseDurationType(const QString& name)
{
    if (name == QLatin1String("whole"))   return DurationType::V_WHOLE;
    if (name == QLatin1String("half"))    return DurationType::V_HALF;
    if (name == QLatin1String("quarter")) return DurationType::V_QUARTER;
    if (name == QLatin1String("eighth"))  return DurationType::V_EIGHTH;
    if (name == QLatin1String("16th"))    return DurationType::V_16TH;
    if (name == QLatin1String("32nd"))    return DurationType::V_32ND;
    if (name == QLatin1String("64th"))    return DurationType::V_64TH;
    return DurationType::V_INVALID;
}

bool ScoreApplicator::applyInsertNote(Score* score, const QJsonObject& op)
{
    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitch    = op["pitch"].toObject();
    // Duration is {"type": "quarter", "dots": 0} — an object, not a bare string.
    const QJsonObject durObj   = op["duration"].toObject();

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int midi = pitchToMidi(pitch["step"].toString(),
                                 pitch["octave"].toInt(),
                                 pitch["accidental"].toString());
    const DurationType dt = parseDurationType(durObj["type"].toString());
    const int dots        = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID || midi < 0 || midi > 127) {
        LOGW() << "[editude] applyInsertNote: invalid fields";
        return false;
    }

    const track_idx_t track = static_cast<track_idx_t>(op["track"].toInt(0));
    const QString noteUuid  = op["id"].toString();

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applyInsertNote: no segment at tick" << tick.toString();
        return false;
    }

    NoteVal nval(midi); // TPC_INVALID — MuseScore infers spelling from key signature
    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("undoableAction", "Insert note"));
    score->setNoteRest(seg, track, nval, dur.ticks());
    score->endCmd();

    // Register the UUID ↔ element mapping so that subsequent DeleteEvent /
    // SetPitch ops targeting this note can look it up by UUID.
    if (!noteUuid.isEmpty()) {
        Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
        if (seg2) {
            EngravingItem* el = seg2->element(track);
            if (el && el->type() == ElementType::CHORD) {
                Chord* chord = toChord(el);
                // Find the note we just inserted by MIDI pitch.
                for (Note* n : chord->notes()) {
                    if (n->pitch() == midi) {
                        m_uuidToElement[noteUuid] = n;
                        m_elementToUuid[n]        = noteUuid;
                        break;
                    }
                }
            }
        }
    }

    return true;
}

bool ScoreApplicator::applyDeleteEvent(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applyDeleteEvent: unknown uuid" << uuid;
        return false;
    }

    EngravingObject* obj = m_uuidToElement.value(uuid);
    m_uuidToElement.remove(uuid);
    m_elementToUuid.remove(obj);

    auto* item = dynamic_cast<EngravingItem*>(obj);
    if (!item) {
        LOGW() << "[editude] applyDeleteEvent: element is not an EngravingItem";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Delete element"));
    score->deleteItem(item);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetPitch(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applySetPitch: unknown uuid" << uuid;
        return false;
    }

    Note* note = dynamic_cast<Note*>(m_uuidToElement.value(uuid));
    if (!note) {
        LOGW() << "[editude] applySetPitch: element is not a Note";
        return false;
    }

    const QJsonObject pitchObj = op["pitch"].toObject();
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applySetPitch: invalid pitch";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set pitch"));
    note->undoChangeProperty(Pid::PITCH, midi);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddPart(Score* /*score*/, const QJsonObject& op)
{
    // TODO: implement AddPart using Score::appendPart / insertPart.
    LOGD() << "[editude] applyAddPart: part_id=" << op["part_id"].toString()
           << " (not yet implemented; remote part additions are ignored)";
    return true; // non-fatal — caller continues
}

bool ScoreApplicator::apply(Score* score, const QJsonObject& payload)
{
    const QString type = payload["type"].toString();
    if (type == QLatin1String("InsertNote")) {
        return applyInsertNote(score, payload);
    }
    if (type == QLatin1String("DeleteEvent")) {
        return applyDeleteEvent(score, payload);
    }
    if (type == QLatin1String("SetPitch")) {
        return applySetPitch(score, payload);
    }
    if (type == QLatin1String("AddPart")) {
        return applyAddPart(score, payload);
    }
    LOGD() << "[editude] ScoreApplicator: unhandled op type" << type;
    return false;
}
