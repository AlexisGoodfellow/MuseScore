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
#include "engraving/dom/staff.h"
#include "engraving/dom/articulation.h"
#include "engraving/types/propertyvalue.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/navigate.h"
#include "engraving/dom/tie.h"
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

bool ScoreApplicator::applySetTie(Score* score, const QJsonObject& op)
{
    const QString uuid = op["event_id"].toString();
    if (uuid.isEmpty() || !m_uuidToElement.contains(uuid)) {
        LOGW() << "[editude] applySetTie: unknown event_id" << uuid;
        return false;
    }

    Note* note = dynamic_cast<Note*>(m_uuidToElement.value(uuid));
    if (!note) {
        LOGW() << "[editude] applySetTie: element is not a note" << uuid;
        return false;
    }

    const QJsonValue tieVal = op["tie"];
    const bool wantTie = !tieVal.isNull() && !tieVal.isUndefined()
                         && tieVal.toString() != QStringLiteral("stop");

    score->startCmd(TranslatableString("undoableAction", "Set tie"));

    if (wantTie) {
        // "start" or "continue": ensure a forward tie exists.
        if (!note->tieFor()) {
            // Find the next chord/rest in the same track.
            ChordRest* nextCR = nextChordRest(note->chord());
            Note* endNote = nullptr;
            if (nextCR && nextCR->isChord()) {
                Chord* nextChord = toChord(nextCR);
                for (Note* n : nextChord->notes()) {
                    if (n->pitch() == note->pitch()) {
                        endNote = n;
                        break;
                    }
                }
            }
            Tie* tie = Factory::createTie(note);
            tie->setStartNote(note);
            tie->setTrack(note->track());
            tie->setTick(note->chord()->segment()->tick());
            if (endNote) {
                if (endNote->tieBack()) {
                    score->undoRemoveElement(endNote->tieBack());
                }
                tie->setEndNote(endNote);
                tie->setTicks(endNote->chord()->segment()->tick()
                              - note->chord()->segment()->tick());
            }
            score->undoAddElement(tie);
        }
    } else {
        // null or "stop": remove the forward tie from this note.
        if (note->tieFor()) {
            score->undoRemoveElement(note->tieFor());
        }
    }

    score->endCmd();
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

void ScoreApplicator::bootstrapPartMap(Score* score)
{
    // Called after the initial score is loaded (snapshot or empty).
    // Parts baked into an MSCZ snapshot do not carry editude UUIDs, so this
    // function cannot populate m_partUuidToPart from the score alone.
    // The map is populated incrementally: applyAddPart registers each new
    // part, and applyRemovePart removes it.
    // This function exists as the designated call-site hook; once a protocol
    // extension supplies per-part editude UUIDs in the bootstrap payload,
    // the mapping can be built here.
    Q_UNUSED(score);
    LOGD() << "[editude] bootstrapPartMap: "
           << m_partUuidToPart.size() << " parts already registered";
}

bool ScoreApplicator::applySetKeySignature(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applySetKeySignature: unknown part_id" << uuid;
        return false;
    }

    const QJsonValue ksSigVal = op["key_signature"];
    if (ksSigVal.isNull() || ksSigVal.isUndefined()) {
        // Remove not yet implemented — silently accept.
        LOGD() << "[editude] applySetKeySignature: null (remove) not yet implemented";
        return true;
    }

    const QJsonObject beat   = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int sharps         = ksSigVal.toObject()["sharps"].toInt(0);

    if (sharps < -7 || sharps > 7) {
        LOGW() << "[editude] applySetKeySignature: sharps out of range" << sharps;
        return false;
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetKeySignature: no measure at tick" << tick.toString();
        return false;
    }

    Part* part = m_partUuidToPart.value(uuid);
    const staff_idx_t firstStaff = part->startTrack() / VOICES;
    const staff_idx_t nStaves    = static_cast<staff_idx_t>(part->nstaves());
    const Key key = static_cast<Key>(sharps);

    score->startCmd(TranslatableString("undoableAction", "Set key signature"));
    Segment* seg = measure->undoGetSegment(SegmentType::KeySig, tick);
    for (staff_idx_t i = 0; i < nStaves; ++i) {
        const track_idx_t track = (firstStaff + i) * VOICES;
        KeySig* ks = Factory::createKeySig(seg);
        ks->setTrack(track);
        ks->setKey(key);
        seg->add(ks);
        score->undoAddElement(ks);
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetClef(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applySetClef: unknown part_id" << uuid;
        return false;
    }

    const QJsonValue clefVal = op["clef"];
    if (clefVal.isNull() || clefVal.isUndefined()) {
        // Remove not yet implemented — silently accept.
        LOGD() << "[editude] applySetClef: null (remove) not yet implemented";
        return true;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int staffIdx     = op["staff"].toInt(0);
    const QString clefName = clefVal.toObject()["name"].toString();

    static const QHash<QString, ClefType> s_clefMap = {
        { "treble",     ClefType::G    },
        { "bass",       ClefType::F    },
        { "alto",       ClefType::C3   },
        { "tenor",      ClefType::C4   },
        { "percussion", ClefType::PERC },
    };

    const ClefType ct = s_clefMap.value(clefName, ClefType::INVALID);
    if (ct == ClefType::INVALID) {
        LOGW() << "[editude] applySetClef: unknown clef name" << clefName;
        return false;
    }

    Part* part = m_partUuidToPart.value(uuid);
    if (staffIdx < 0 || staffIdx >= static_cast<int>(part->nstaves())) {
        LOGW() << "[editude] applySetClef: staff index out of range" << staffIdx;
        return false;
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetClef: no measure at tick" << tick.toString();
        return false;
    }

    const track_idx_t track = (part->startTrack() / VOICES + staffIdx) * VOICES;
    Segment* seg = measure->undoGetSegment(SegmentType::Clef, tick);

    score->startCmd(TranslatableString("undoableAction", "Set clef"));
    Clef* clef = Factory::createClef(score->dummy()->segment());
    clef->setClefType(ct);
    clef->setTrack(track);
    clef->setParent(seg);
    score->doUndoAddElement(clef);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetPartName(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applySetPartName: unknown part_id" << uuid;
        return false;
    }
    Part* part = m_partUuidToPart.value(uuid);
    const QString name = op["name"].toString();
    score->startCmd(TranslatableString("undoableAction", "Set part name"));
    part->setPartName(String(name));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetStaffCount(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applySetStaffCount: unknown part_id" << uuid;
        return false;
    }
    Part* part = m_partUuidToPart.value(uuid);
    const int target = op["staff_count"].toInt(0);
    if (target <= 0) {
        LOGW() << "[editude] applySetStaffCount: invalid staff_count" << target;
        return false;
    }
    const int current = static_cast<int>(part->nstaves());
    if (target == current) {
        return true;
    }
    score->startCmd(TranslatableString("undoableAction", "Set staff count"));
    if (target > current) {
        for (int i = current; i < target; ++i) {
            Staff* staff = Factory::createStaff(part);
            score->appendStaff(staff);
        }
    } else {
        // Remove from the tail of this part's staves.
        const staff_idx_t partStart = part->startTrack() / VOICES;
        for (int i = current; i > target; --i) {
            score->cmdRemoveStaff(static_cast<staff_idx_t>(partStart + i - 1));
        }
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddPart(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty()) {
        LOGW() << "[editude] applyAddPart: missing part_id";
        return false;
    }

    const QString name = op["name"].toString();
    const int staffCount = op["staff_count"].toInt(1);

    Part* part = new Part(score);
    part->setPartName(String(name));

    // Short name from instrument if provided.
    const QJsonObject instr = op["instrument"].toObject();
    if (!instr.isEmpty()) {
        const QString longName  = instr["name"].toString(name);
        const QString shortName = instr["short_name"].toString();
        part->setLongNameAll(String(longName));
        part->setShortNameAll(String(shortName));
    }

    score->startCmd(TranslatableString("undoableAction", "Add part"));
    score->appendPart(part);
    for (int i = 0; i < staffCount; ++i) {
        Staff* staff = Factory::createStaff(part);
        score->appendStaff(staff);
    }
    score->endCmd();

    m_partUuidToPart[uuid] = part;
    return true;
}

bool ScoreApplicator::applyRemovePart(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applyRemovePart: unknown part_id" << uuid;
        return false;
    }
    Part* part = m_partUuidToPart.value(uuid);
    m_partUuidToPart.remove(uuid);

    score->startCmd(TranslatableString("undoableAction", "Remove part"));
    score->removePart(part);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetPartInstrument(Score* score, const QJsonObject& op)
{
    const QString uuid = op["part_id"].toString();
    if (uuid.isEmpty() || !m_partUuidToPart.contains(uuid)) {
        LOGW() << "[editude] applySetPartInstrument: unknown part_id" << uuid;
        return false;
    }
    const QJsonObject instr = op["instrument"].toObject();
    if (instr.isEmpty()) {
        LOGW() << "[editude] applySetPartInstrument: null instrument — ignored";
        return false;
    }
    Part* part = m_partUuidToPart.value(uuid);
    const QString longName  = instr["name"].toString();
    const QString shortName = instr["short_name"].toString();
    // Update display names only; full playback-instrument change (transposition,
    // MIDI bank, etc.) requires the InstrumentChange API and is deferred.
    score->startCmd(TranslatableString("undoableAction", "Set part instrument"));
    part->setPartName(String(longName));
    part->setLongNameAll(String(longName));
    part->setShortNameAll(String(shortName));
    score->endCmd();
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
    if (type == QLatin1String("AddPart"))            return applyAddPart(score, payload);
    if (type == QLatin1String("RemovePart"))         return applyRemovePart(score, payload);
    if (type == QLatin1String("SetPartInstrument")) return applySetPartInstrument(score, payload);

    // Tier 3 — articulations
    if (type == QLatin1String("AddArticulation"))    return applyAddArticulation(score, payload);
    if (type == QLatin1String("RemoveArticulation")) return applyRemoveArticulation(score, payload);

    // Tier 3 — dynamics
    if (type == QLatin1String("AddDynamic"))    return applyAddDynamic(score, payload);
    if (type == QLatin1String("SetDynamic"))    return applySetDynamic(score, payload);
    if (type == QLatin1String("RemoveDynamic")) return applyRemoveDynamic(score, payload);

    // Tier 3 — slurs
    if (type == QLatin1String("AddSlur"))    return applyAddSlur(score, payload);
    if (type == QLatin1String("RemoveSlur")) return applyRemoveSlur(score, payload);

    // Tier 3 — hairpins
    if (type == QLatin1String("AddHairpin"))    return applyAddHairpin(score, payload);
    if (type == QLatin1String("RemoveHairpin")) return applyRemoveHairpin(score, payload);

    // Tier 3 — tuplets
    if (type == QLatin1String("AddTuplet"))    return applyAddTuplet(score, payload);
    if (type == QLatin1String("RemoveTuplet")) return applyRemoveTuplet(score, payload);

    // Tier 3 — lyrics
    if (type == QLatin1String("AddLyric"))    return applyAddLyric(score, payload);
    if (type == QLatin1String("SetLyric"))    return applySetLyric(score, payload);
    if (type == QLatin1String("RemoveLyric")) return applyRemoveLyric(score, payload);

    // Tier 3 — chord symbols (score-global)
    if (type == QLatin1String("AddChordSymbol"))    return applyAddChordSymbol(score, payload);
    if (type == QLatin1String("SetChordSymbol"))    return applySetChordSymbol(score, payload);
    if (type == QLatin1String("RemoveChordSymbol")) return applyRemoveChordSymbol(score, payload);

    LOGD() << "[editude] ScoreApplicator: unhandled op type" << type;
    return false;
}

// ---------------------------------------------------------------------------
// Tier 3 — implementation stubs
// ---------------------------------------------------------------------------
//
// Each stub logs the op and returns true (non-fatal) so the OT engine continues
// normally.  Full INotationInteraction calls will be wired in a follow-up once
// the MuseScore API surface for each element type is confirmed.

// ---------------------------------------------------------------------------
// Tier 3 articulation helpers
// ---------------------------------------------------------------------------

static SymId articulationSymId(const QString& name)
{
    static const QHash<QString, SymId> s_map = {
        { "staccato",      SymId::articStaccatoAbove      },
        { "accent",        SymId::articAccentAbove         },
        { "tenuto",        SymId::articTenutoAbove         },
        { "marcato",       SymId::articMarcatoAbove        },
        { "staccatissimo", SymId::articStaccatissimoAbove  },
        { "fermata",       SymId::fermataAbove             },
        { "trill",         SymId::ornamentTrill            },
        { "mordent",       SymId::ornamentMordent          },
        { "turn",          SymId::ornamentTurn             },
    };
    return s_map.value(name, SymId::noSym);
}

bool ScoreApplicator::applyAddArticulation(Score* score, const QJsonObject& op)
{
    const QString id       = op["id"].toString();
    const QString eventId  = op["event_id"].toString();
    const QString artName  = op["articulation"].toString();

    if (id.isEmpty() || eventId.isEmpty()) {
        LOGW() << "[editude] applyAddArticulation: missing id or event_id";
        return false;
    }

    EngravingObject* evObj = m_uuidToElement.value(eventId);
    if (!evObj) {
        LOGW() << "[editude] applyAddArticulation: unknown event_id" << eventId;
        return false;
    }

    ChordRest* cr = nullptr;
    if (evObj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(evObj))->chord();
    } else if (evObj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(evObj));
    }
    if (!cr) {
        LOGW() << "[editude] applyAddArticulation: event is not a note/chordrest" << eventId;
        return false;
    }

    const SymId symId = articulationSymId(artName);
    if (symId == SymId::noSym) {
        LOGW() << "[editude] applyAddArticulation: unknown articulation name" << artName;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Add articulation"));
    Articulation* art = Factory::createArticulation(score->dummy()->chord());
    art->setSymId(symId);
    art->setParent(cr);
    art->setTrack(cr->track());
    score->undoAddElement(art);
    score->endCmd();

    m_uuidToElement[id] = art;
    m_elementToUuid[art] = id;
    return true;
}

bool ScoreApplicator::applyRemoveArticulation(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveArticulation: unknown id" << id;
        return false;
    }

    Articulation* art = dynamic_cast<Articulation*>(m_uuidToElement.value(id));
    if (!art) {
        LOGW() << "[editude] applyRemoveArticulation: element is not Articulation" << id;
        return false;
    }

    m_elementToUuid.remove(art);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove articulation"));
    score->undoRemoveElement(art);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 dynamic helpers
// ---------------------------------------------------------------------------

static DynamicType dynamicTypeFromName(const QString& name)
{
    static const QHash<QString, DynamicType> s_map = {
        { "ppp", DynamicType::PPP },
        { "pp",  DynamicType::PP  },
        { "p",   DynamicType::P   },
        { "mp",  DynamicType::MP  },
        { "mf",  DynamicType::MF  },
        { "f",   DynamicType::F   },
        { "ff",  DynamicType::FF  },
        { "fff", DynamicType::FFF },
        { "sfz", DynamicType::SFZ },
        { "fp",  DynamicType::FP  },
        { "rf",  DynamicType::RF  },
    };
    return s_map.value(name, DynamicType::OTHER);
}

bool ScoreApplicator::applyAddDynamic(Score* score, const QJsonObject& op)
{
    const QString id      = op["id"].toString();
    const QString partId  = op["part_id"].toString();
    const QString kind    = op["kind"].toString();
    const QJsonObject beat = op["beat"].toObject();

    if (id.isEmpty() || partId.isEmpty()) {
        LOGW() << "[editude] applyAddDynamic: missing id or part_id";
        return false;
    }

    if (!m_partUuidToPart.contains(partId)) {
        LOGW() << "[editude] applyAddDynamic: unknown part_id" << partId;
        return false;
    }

    const DynamicType dt = dynamicTypeFromName(kind);
    if (dt == DynamicType::OTHER) {
        LOGW() << "[editude] applyAddDynamic: unknown dynamic kind" << kind;
        return false;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddDynamic: no measure at tick" << tick.toString();
        return false;
    }

    Part* part = m_partUuidToPart.value(partId);
    const track_idx_t track = part->startTrack();
    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add dynamic"));
    Dynamic* dyn = Factory::createDynamic(seg);
    dyn->setParent(seg);
    dyn->setTrack(track);
    dyn->setDynamicType(dt);
    score->undoAddElement(dyn);
    score->endCmd();

    m_uuidToElement[id] = dyn;
    m_elementToUuid[dyn] = id;
    return true;
}

bool ScoreApplicator::applySetDynamic(Score* score, const QJsonObject& op)
{
    const QString id   = op["id"].toString();
    const QString kind = op["kind"].toString();

    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applySetDynamic: unknown id" << id;
        return false;
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(m_uuidToElement.value(id));
    if (!dyn) {
        LOGW() << "[editude] applySetDynamic: element is not Dynamic" << id;
        return false;
    }

    const DynamicType dt = dynamicTypeFromName(kind);
    if (dt == DynamicType::OTHER) {
        LOGW() << "[editude] applySetDynamic: unknown dynamic kind" << kind;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set dynamic"));
    dyn->undoChangeProperty(Pid::DYNAMIC_TYPE, PropertyValue(dt));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveDynamic(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveDynamic: unknown id" << id;
        return false;
    }

    Dynamic* dyn = dynamic_cast<Dynamic*>(m_uuidToElement.value(id));
    if (!dyn) {
        LOGW() << "[editude] applyRemoveDynamic: element is not Dynamic" << id;
        return false;
    }

    m_elementToUuid.remove(dyn);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove dynamic"));
    score->undoRemoveElement(dyn);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddSlur(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyAddSlur: id=" << op["id"].toString()
           << " start=" << op["start_event_id"].toString()
           << " end=" << op["end_event_id"].toString();
    return true;
}

bool ScoreApplicator::applyRemoveSlur(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyRemoveSlur: id=" << op["id"].toString();
    return true;
}

bool ScoreApplicator::applyAddHairpin(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyAddHairpin: id=" << op["id"].toString()
           << " kind=" << op["kind"].toString();
    return true;
}

bool ScoreApplicator::applyRemoveHairpin(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyRemoveHairpin: id=" << op["id"].toString();
    return true;
}

bool ScoreApplicator::applyAddTuplet(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyAddTuplet: id=" << op["id"].toString()
           << " actual=" << op["actual_notes"].toInt()
           << " normal=" << op["normal_notes"].toInt();
    return true;
}

bool ScoreApplicator::applyRemoveTuplet(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyRemoveTuplet: id=" << op["id"].toString();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 lyric helpers
// ---------------------------------------------------------------------------

static LyricsSyllabic lyricSyllabicFromName(const QString& name)
{
    if (name == QStringLiteral("single"))  return LyricsSyllabic::SINGLE;
    if (name == QStringLiteral("begin"))   return LyricsSyllabic::BEGIN;
    if (name == QStringLiteral("middle"))  return LyricsSyllabic::MIDDLE;
    if (name == QStringLiteral("end"))     return LyricsSyllabic::END;
    return LyricsSyllabic::SINGLE;
}

bool ScoreApplicator::applyAddLyric(Score* score, const QJsonObject& op)
{
    const QString id      = op["id"].toString();
    const QString eventId = op["event_id"].toString();
    const int verse       = op["verse"].toInt(0);
    const QString syllabic = op["syllabic"].toString(QStringLiteral("single"));
    const QString text    = op["text"].toString();

    if (id.isEmpty() || eventId.isEmpty()) {
        LOGW() << "[editude] applyAddLyric: missing id or event_id";
        return false;
    }

    EngravingObject* evObj = m_uuidToElement.value(eventId);
    if (!evObj) {
        LOGW() << "[editude] applyAddLyric: unknown event_id" << eventId;
        return false;
    }

    ChordRest* cr = nullptr;
    if (evObj->isNote()) {
        cr = toNote(static_cast<EngravingItem*>(evObj))->chord();
    } else if (evObj->isChordRest()) {
        cr = toChordRest(static_cast<EngravingItem*>(evObj));
    }
    if (!cr) {
        LOGW() << "[editude] applyAddLyric: event is not a note/chordrest" << eventId;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Add lyric"));
    Lyrics* lyric = Factory::createLyrics(cr);
    lyric->setTrack(cr->track());
    lyric->setParent(cr);
    lyric->setVerse(verse);
    lyric->setSyllabic(lyricSyllabicFromName(syllabic));
    lyric->setPlainText(String(text));
    score->undoAddElement(lyric);
    score->endCmd();

    m_uuidToElement[id] = lyric;
    m_elementToUuid[lyric] = id;
    return true;
}

bool ScoreApplicator::applySetLyric(Score* score, const QJsonObject& op)
{
    const QString id       = op["id"].toString();
    const QString text     = op["text"].toString();
    const QString syllabic = op["syllabic"].toString();

    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applySetLyric: unknown id" << id;
        return false;
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(m_uuidToElement.value(id));
    if (!lyric) {
        LOGW() << "[editude] applySetLyric: element is not Lyrics" << id;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set lyric"));
    lyric->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    if (!syllabic.isEmpty()) {
        lyric->undoChangeProperty(Pid::SYLLABIC,
                                   PropertyValue(static_cast<int>(lyricSyllabicFromName(syllabic))));
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveLyric(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveLyric: unknown id" << id;
        return false;
    }

    Lyrics* lyric = dynamic_cast<Lyrics*>(m_uuidToElement.value(id));
    if (!lyric) {
        LOGW() << "[editude] applyRemoveLyric: element is not Lyrics" << id;
        return false;
    }

    m_elementToUuid.remove(lyric);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove lyric"));
    score->undoRemoveElement(lyric);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddChordSymbol(Score* /*score*/, const QJsonObject& op)
{
    // Chord symbols are score-global (no part_id). Full rendering via MuseScore
    // Harmony API is deferred; log and continue.
    LOGD() << "[editude] applyAddChordSymbol: id=" << op["id"].toString()
           << " name=" << op["name"].toString();
    return true;
}

bool ScoreApplicator::applySetChordSymbol(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applySetChordSymbol: id=" << op["id"].toString()
           << " name=" << op["name"].toString();
    return true;
}

bool ScoreApplicator::applyRemoveChordSymbol(Score* /*score*/, const QJsonObject& op)
{
    LOGD() << "[editude] applyRemoveChordSymbol: id=" << op["id"].toString();
    return true;
}
