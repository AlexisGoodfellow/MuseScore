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

#include <QJsonArray>

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/noteval.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/timesig.h"
#include "engraving/types/bps.h"
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

bool ScoreApplicator::applyInsertRest(Score* score, const QJsonObject& op)
{
    const QJsonObject beat   = op["beat"].toObject();
    const QJsonObject durObj = op["duration"].toObject();

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const DurationType dt = parseDurationType(durObj["type"].toString());
    const int dots        = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID) {
        LOGW() << "[editude] applyInsertRest: invalid duration";
        return false;
    }

    const track_idx_t track = static_cast<track_idx_t>(op["track"].toInt(0));
    const QString restUuid  = op["id"].toString();

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applyInsertRest: no segment at tick" << tick.toString();
        return false;
    }

    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("undoableAction", "Insert rest"));
    score->setNoteRest(seg, track, NoteVal(), dur.ticks());
    score->endCmd();

    if (!restUuid.isEmpty()) {
        Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
        if (seg2) {
            EngravingItem* el = seg2->element(track);
            if (el && el->type() == ElementType::REST) {
                m_uuidToElement[restUuid] = el;
                m_elementToUuid[el]       = restUuid;
            }
        }
    }

    return true;
}

bool ScoreApplicator::applyInsertChord(Score* score, const QJsonObject& op)
{
    const QJsonObject beat    = op["beat"].toObject();
    const QJsonArray  pitches = op["pitches"].toArray();
    const QJsonObject durObj  = op["duration"].toObject();

    if (pitches.isEmpty()) {
        LOGW() << "[editude] applyInsertChord: pitches array is empty";
        return false;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const DurationType dt = parseDurationType(durObj["type"].toString());
    const int dots        = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID) {
        LOGW() << "[editude] applyInsertChord: invalid duration";
        return false;
    }

    const track_idx_t track     = static_cast<track_idx_t>(op["track"].toInt(0));
    const QString     chordUuid = op["id"].toString();

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applyInsertChord: no segment at tick" << tick.toString();
        return false;
    }

    // Parse all pitches upfront so we can validate before mutating the score.
    QVector<int> midiPitches;
    midiPitches.reserve(pitches.size());
    for (const QJsonValue& v : pitches) {
        const QJsonObject p = v.toObject();
        const int midi = pitchToMidi(p["step"].toString(), p["octave"].toInt(), p["accidental"].toString());
        if (midi < 0 || midi > 127) {
            LOGW() << "[editude] applyInsertChord: invalid pitch in pitches array";
            return false;
        }
        midiPitches.append(midi);
    }

    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("undoableAction", "Insert chord"));

    // First pitch: use setNoteRest to place the chord on the score.
    NoteVal nval0(midiPitches[0]);
    score->setNoteRest(seg, track, nval0, dur.ticks());

    // Additional pitches: find the created chord and call addNote for each.
    Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
    Chord* chord  = nullptr;
    if (seg2) {
        EngravingItem* el = seg2->element(track);
        if (el && el->type() == ElementType::CHORD) {
            chord = toChord(el);
        }
    }
    if (chord) {
        for (int i = 1; i < midiPitches.size(); ++i) {
            NoteVal nv(midiPitches[i]);
            score->addNote(chord, nv);
        }
    }

    score->endCmd();

    // Register the whole chord under chordUuid (not individual notes).
    if (!chordUuid.isEmpty() && chord) {
        m_uuidToElement[chordUuid] = chord;
        m_elementToUuid[chord]     = chordUuid;
    }

    return true;
}

bool ScoreApplicator::applyAddChordNote(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applyAddChordNote: unknown chord uuid" << uuid;
        return false;
    }

    Chord* chord = dynamic_cast<Chord*>(m_uuidToElement.value(uuid));
    if (!chord) {
        LOGW() << "[editude] applyAddChordNote: element is not a Chord";
        return false;
    }

    const QJsonObject pitchObj = op["pitch"].toObject();
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applyAddChordNote: invalid pitch";
        return false;
    }

    NoteVal nval(midi);
    score->startCmd(TranslatableString("undoableAction", "Add chord note"));
    score->addNote(chord, nval);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveChordNote(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applyRemoveChordNote: unknown chord uuid" << uuid;
        return false;
    }

    Chord* chord = dynamic_cast<Chord*>(m_uuidToElement.value(uuid));
    if (!chord) {
        LOGW() << "[editude] applyRemoveChordNote: element is not a Chord";
        return false;
    }

    const QJsonObject pitchObj = op["pitch"].toObject();
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applyRemoveChordNote: invalid pitch";
        return false;
    }

    Note* target = nullptr;
    for (Note* n : chord->notes()) {
        if (n->pitch() == midi) {
            target = n;
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveChordNote: pitch" << midi << "not found in chord";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove chord note"));
    score->deleteItem(target);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetDuration(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applySetDuration: unknown uuid" << uuid;
        return false;
    }

    EngravingObject* obj = m_uuidToElement.value(uuid);
    auto* cr = dynamic_cast<ChordRest*>(obj);
    if (!cr) {
        LOGW() << "[editude] applySetDuration: element is not a ChordRest";
        return false;
    }

    const QJsonObject durObj = op["duration"].toObject();
    const DurationType dt    = parseDurationType(durObj["type"].toString());
    const int dots           = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID) {
        LOGW() << "[editude] applySetDuration: invalid duration";
        return false;
    }

    TDuration dur(dt);
    dur.setDots(dots);

    const Fraction tick  = cr->tick();
    const track_idx_t track = cr->track();

    // Capture pitch if this is a chord (needed to re-find the element after replacement).
    int midiForRefind = -1;
    if (cr->type() == ElementType::CHORD) {
        Chord* chord = toChord(cr);
        if (!chord->notes().empty()) {
            midiForRefind = chord->notes().front()->pitch();
        }
    }

    // Remove old UUID entry before the element pointer is invalidated.
    m_uuidToElement.remove(uuid);
    m_elementToUuid.remove(obj);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applySetDuration: no segment at tick" << tick.toString();
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set duration"));
    score->setNoteRest(seg, track, midiForRefind >= 0 ? NoteVal(midiForRefind) : NoteVal(), dur.ticks());
    score->endCmd();

    // Re-register the new element under the same UUID.
    Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (seg2) {
        EngravingItem* el = seg2->element(track);
        if (el) {
            m_uuidToElement[uuid] = el;
            m_elementToUuid[el]   = uuid;
        }
    }

    return true;
}

bool ScoreApplicator::applySetTie(Score* /*score*/, const QJsonObject& op)
{
    // TODO: implement tie setting (requires next-note lookup).
    LOGD() << "[editude] applySetTie: event_id=" << op["event_id"].toString()
           << " (not yet implemented; ties are ignored)";
    return true;
}

bool ScoreApplicator::applySetTrack(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applySetTrack: unknown uuid" << uuid;
        return false;
    }

    auto* item = dynamic_cast<EngravingItem*>(m_uuidToElement.value(uuid));
    if (!item) {
        LOGW() << "[editude] applySetTrack: element is not an EngravingItem";
        return false;
    }

    const track_idx_t newTrack = static_cast<track_idx_t>(op["track"].toInt(0));
    score->startCmd(TranslatableString("undoableAction", "Set track"));
    item->undoChangeProperty(Pid::TRACK, newTrack);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetTimeSignature(Score* score, const QJsonObject& op)
{
    const QJsonObject beat  = op["beat"].toObject();
    const QJsonObject tsObj = op["time_signature"].toObject();

    if (tsObj.isEmpty()) {
        // null time_signature → remove: not yet implemented.
        LOGD() << "[editude] applySetTimeSignature: null value (remove) not yet implemented";
        return true;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int num   = tsObj["numerator"].toInt(0);
    const int denom = tsObj["denominator"].toInt(0);

    if (num <= 0 || denom <= 0) {
        LOGW() << "[editude] applySetTimeSignature: invalid time signature" << num << "/" << denom;
        return false;
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetTimeSignature: no measure at tick" << tick.toString();
        return false;
    }

    TimeSig* ts = Factory::createTimeSig(score->dummy()->segment());
    ts->setSig(Fraction(num, denom), TimeSigType::NORMAL);

    score->startCmd(TranslatableString("undoableAction", "Set time signature"));
    score->cmdAddTimeSig(measure, 0, ts, false);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetTempo(Score* score, const QJsonObject& op)
{
    const QJsonObject beat    = op["beat"].toObject();
    const QJsonObject tempoObj = op["tempo"].toObject();

    if (tempoObj.isEmpty()) {
        // null tempo → remove: not yet implemented.
        LOGD() << "[editude] applySetTempo: null value (remove) not yet implemented";
        return true;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const double bpm = tempoObj["bpm"].toDouble(0.0);

    if (bpm <= 0.0) {
        LOGW() << "[editude] applySetTempo: invalid bpm" << bpm;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set tempo"));
    score->setTempo(tick, BeatsPerSecond(bpm / 60.0));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetKeySignature(Score* /*score*/, const QJsonObject& op)
{
    // TODO: requires part→staff mapping via m_partUuidToPart.
    LOGD() << "[editude] applySetKeySignature: part_id=" << op["part_id"].toString()
           << " (not yet implemented; key signature changes are ignored)";
    return true;
}

bool ScoreApplicator::applySetClef(Score* /*score*/, const QJsonObject& op)
{
    // TODO: requires part→staff mapping via m_partUuidToPart.
    LOGD() << "[editude] applySetClef: part_id=" << op["part_id"].toString()
           << " (not yet implemented; clef changes are ignored)";
    return true;
}

bool ScoreApplicator::applySetPartName(Score* /*score*/, const QJsonObject& op)
{
    // TODO: requires part lookup via m_partUuidToPart.
    LOGD() << "[editude] applySetPartName: part_id=" << op["part_id"].toString()
           << " (not yet implemented; part renames are ignored)";
    return true;
}

bool ScoreApplicator::applySetStaffCount(Score* /*score*/, const QJsonObject& op)
{
    // TODO: requires part lookup via m_partUuidToPart.
    LOGD() << "[editude] applySetStaffCount: part_id=" << op["part_id"].toString()
           << " (not yet implemented; staff count changes are ignored)";
    return true;
}

bool ScoreApplicator::applyAddPart(Score* /*score*/, const QJsonObject& op)
{
    // TODO: implement using Score::appendPart / insertPart; populate m_partUuidToPart.
    LOGD() << "[editude] applyAddPart: part_id=" << op["part_id"].toString()
           << " (not yet implemented; remote part additions are ignored)";
    return true; // non-fatal — caller continues
}

bool ScoreApplicator::applyRemovePart(Score* /*score*/, const QJsonObject& op)
{
    // TODO: requires part lookup via m_partUuidToPart.
    LOGD() << "[editude] applyRemovePart: part_id=" << op["part_id"].toString()
           << " (not yet implemented; remote part removals are ignored)";
    return true;
}

bool ScoreApplicator::apply(Score* score, const QJsonObject& payload)
{
    const QString type = payload["type"].toString();

    // Tier 1 — stream event operations
    if (type == QLatin1String("InsertNote"))      return applyInsertNote(score, payload);
    if (type == QLatin1String("InsertRest"))      return applyInsertRest(score, payload);
    if (type == QLatin1String("InsertChord"))     return applyInsertChord(score, payload);
    if (type == QLatin1String("DeleteEvent"))     return applyDeleteEvent(score, payload);
    if (type == QLatin1String("SetPitch"))        return applySetPitch(score, payload);
    if (type == QLatin1String("AddChordNote"))    return applyAddChordNote(score, payload);
    if (type == QLatin1String("RemoveChordNote")) return applyRemoveChordNote(score, payload);
    if (type == QLatin1String("SetDuration"))     return applySetDuration(score, payload);
    if (type == QLatin1String("SetTie"))          return applySetTie(score, payload);
    if (type == QLatin1String("SetTrack"))        return applySetTrack(score, payload);

    // Tier 2 — score directive operations
    if (type == QLatin1String("SetTimeSignature")) return applySetTimeSignature(score, payload);
    if (type == QLatin1String("SetTempo"))         return applySetTempo(score, payload);
    if (type == QLatin1String("SetKeySignature"))  return applySetKeySignature(score, payload);
    if (type == QLatin1String("SetClef"))          return applySetClef(score, payload);
    if (type == QLatin1String("SetPartName"))      return applySetPartName(score, payload);
    if (type == QLatin1String("SetStaffCount"))    return applySetStaffCount(score, payload);
    if (type == QLatin1String("AddPart"))          return applyAddPart(score, payload);
    if (type == QLatin1String("RemovePart"))       return applyRemovePart(score, payload);

    LOGD() << "[editude] ScoreApplicator: unhandled op type" << type;
    return false;
}
