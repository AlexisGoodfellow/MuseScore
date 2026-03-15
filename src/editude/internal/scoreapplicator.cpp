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
#include "engraving/dom/hairpin.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/navigate.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/volta.h"
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

    // op_batch — apply all sub-ops as a single undo command so the user can
    // undo an entire paste/multi-op action in one step.
    if (type == QLatin1String("op_batch")) {
        const QJsonArray ops = payload["ops"].toArray();
        if (ops.isEmpty()) {
            return true;
        }
        score->startCmd(TranslatableString("undoableAction", "Remote batch edit"));
        bool ok = true;
        for (const QJsonValue& v : ops) {
            if (!apply(score, v.toObject())) {
                ok = false;
            }
        }
        score->endCmd();
        return ok;
    }

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

    // Tier 4 — navigation marks
    if (type == QLatin1String("InsertVolta"))  return applyInsertVolta(score, payload);
    if (type == QLatin1String("RemoveVolta"))  return applyRemoveVolta(score, payload);
    if (type == QLatin1String("InsertMarker")) return applyInsertMarker(score, payload);
    if (type == QLatin1String("RemoveMarker")) return applyRemoveMarker(score, payload);
    if (type == QLatin1String("InsertJump"))   return applyInsertJump(score, payload);
    if (type == QLatin1String("RemoveJump"))   return applyRemoveJump(score, payload);

    // Structural ops
    if (type == QLatin1String("SetScoreMetadata")) return applySetScoreMetadata(score, payload);
    if (type == QLatin1String("InsertBeats"))       return applyInsertBeats(score, payload);
    if (type == QLatin1String("DeleteBeats"))       return applyDeleteBeats(score, payload);

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

bool ScoreApplicator::applyAddSlur(Score* score, const QJsonObject& op)
{
    const QString id           = op["id"].toString();
    const QString startEventId = op["start_event_id"].toString();
    const QString endEventId   = op["end_event_id"].toString();

    if (id.isEmpty() || startEventId.isEmpty() || endEventId.isEmpty()) {
        LOGW() << "[editude] applyAddSlur: missing id, start_event_id, or end_event_id";
        return false;
    }

    EngravingObject* startObj = m_uuidToElement.value(startEventId);
    EngravingObject* endObj   = m_uuidToElement.value(endEventId);
    if (!startObj || !endObj) {
        LOGW() << "[editude] applyAddSlur: unknown start or end event_id";
        return false;
    }

    auto toCR = [](EngravingObject* obj) -> ChordRest* {
        if (!obj) return nullptr;
        if (obj->isNote()) return toNote(static_cast<EngravingItem*>(obj))->chord();
        if (obj->isChordRest()) return toChordRest(static_cast<EngravingItem*>(obj));
        return nullptr;
    };

    ChordRest* startCR = toCR(startObj);
    ChordRest* endCR   = toCR(endObj);
    if (!startCR || !endCR) {
        LOGW() << "[editude] applyAddSlur: start or end is not a note/chordrest";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Add slur"));
    Slur* slur = Factory::createSlur(score->dummy());
    slur->setScore(score);
    slur->setTick(startCR->tick());
    slur->setTick2(endCR->tick());
    slur->setTrack(startCR->track());
    slur->setTrack2(endCR->track());
    slur->setStartElement(startCR);
    slur->setEndElement(endCR);
    score->undoAddElement(slur);
    score->endCmd();

    m_uuidToElement[id] = slur;
    m_elementToUuid[slur] = id;
    return true;
}

bool ScoreApplicator::applyRemoveSlur(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveSlur: unknown id" << id;
        return false;
    }

    Slur* slur = dynamic_cast<Slur*>(m_uuidToElement.value(id));
    if (!slur) {
        LOGW() << "[editude] applyRemoveSlur: element is not a Slur" << id;
        return false;
    }

    m_elementToUuid.remove(slur);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove slur"));
    score->undoRemoveElement(slur);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddHairpin(Score* score, const QJsonObject& op)
{
    const QString id       = op["id"].toString();
    const QString partId   = op["part_id"].toString();
    const QString kind     = op["kind"].toString();
    const QJsonObject sb   = op["start_beat"].toObject();
    const QJsonObject eb   = op["end_beat"].toObject();

    if (id.isEmpty() || partId.isEmpty()) {
        LOGW() << "[editude] applyAddHairpin: missing id or part_id";
        return false;
    }

    if (!m_partUuidToPart.contains(partId)) {
        LOGW() << "[editude] applyAddHairpin: unknown part_id" << partId;
        return false;
    }

    const HairpinType hpType = (kind == QStringLiteral("crescendo"))
                               ? HairpinType::CRESC_HAIRPIN
                               : HairpinType::DIM_HAIRPIN;

    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = m_partUuidToPart.value(partId)->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add hairpin"));
    Hairpin* hp = score->addHairpin(hpType, startTick, endTick, track);
    score->endCmd();

    if (hp) {
        m_uuidToElement[id] = hp;
        m_elementToUuid[hp] = id;
    }
    return true;
}

bool ScoreApplicator::applyRemoveHairpin(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveHairpin: unknown id" << id;
        return false;
    }

    Hairpin* hp = dynamic_cast<Hairpin*>(m_uuidToElement.value(id));
    if (!hp) {
        LOGW() << "[editude] applyRemoveHairpin: element is not a Hairpin" << id;
        return false;
    }

    m_elementToUuid.remove(hp);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove hairpin"));
    score->undoRemoveElement(hp);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyAddTuplet(Score* score, const QJsonObject& op)
{
    const QString id          = op["id"].toString();
    const QString partId      = op["part_id"].toString();
    const int actualNotes     = op["actual_notes"].toInt(0);
    const int normalNotes     = op["normal_notes"].toInt(0);
    const QJsonObject beat    = op["beat"].toObject();
    const QJsonObject baseDur = op["base_duration"].toObject();
    const QJsonArray members  = op["members"].toArray();

    if (id.isEmpty() || actualNotes <= 0 || normalNotes <= 0) {
        LOGW() << "[editude] applyAddTuplet: invalid payload";
        return false;
    }

    if (!m_partUuidToPart.contains(partId)) {
        LOGW() << "[editude] applyAddTuplet: unknown part_id" << partId;
        return false;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const QByteArray baseTypeUtf8 = baseDur["type"].toString().toUtf8();
    const DurationType baseType = TConv::fromXml(
        muse::AsciiStringView(baseTypeUtf8.constData()), DurationType::V_INVALID);
    if (baseType == DurationType::V_INVALID) {
        LOGW() << "[editude] applyAddTuplet: unknown base duration" << baseDur["type"].toString();
        return false;
    }

    // Find ChordRest at tick on the part's first track.
    const track_idx_t track = m_partUuidToPart.value(partId)->startTrack();
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddTuplet: no measure at tick" << tick.toString();
        return false;
    }

    ChordRest* ocr = nullptr;
    for (Segment* seg = measure->first(SegmentType::ChordRest); seg;
         seg = seg->next(SegmentType::ChordRest)) {
        if (seg->tick() == tick) {
            ocr = toChordRest(seg->element(track));
            break;
        }
    }
    if (!ocr) {
        LOGW() << "[editude] applyAddTuplet: no chordrest at tick" << tick.toString();
        return false;
    }

    const TDuration baseLen(baseType);
    // total ticks = normal_notes * base_duration
    const Fraction totalTicks = baseLen.fraction() * normalNotes;

    score->startCmd(TranslatableString("undoableAction", "Add tuplet"));
    Tuplet* tuplet = Factory::createTuplet(measure);
    tuplet->setRatio(Fraction(actualNotes, normalNotes));
    tuplet->setTicks(totalTicks);
    tuplet->setBaseLen(baseLen);
    tuplet->setTrack(track);
    tuplet->setTick(tick);
    tuplet->setParent(measure);
    score->cmdCreateTuplet(ocr, tuplet);
    score->endCmd();

    m_uuidToElement[id] = tuplet;
    m_elementToUuid[tuplet] = id;

    // Map member UUIDs to the created elements in order.
    const auto& elems = tuplet->elements();
    for (int i = 0; i < static_cast<int>(elems.size()) && i < members.size(); ++i) {
        const QString memberId = members[i].toObject()["id"].toString();
        if (!memberId.isEmpty() && elems[i]) {
            m_uuidToElement[memberId]  = elems[i];
            m_elementToUuid[elems[i]] = memberId;
        }
    }

    return true;
}

bool ScoreApplicator::applyRemoveTuplet(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveTuplet: unknown id" << id;
        return false;
    }

    Tuplet* tuplet = dynamic_cast<Tuplet*>(m_uuidToElement.value(id));
    if (!tuplet) {
        LOGW() << "[editude] applyRemoveTuplet: element is not Tuplet" << id;
        return false;
    }

    // Unregister members before removal.
    for (DurationElement* elem : tuplet->elements()) {
        const QString memberUuid = m_elementToUuid.value(elem);
        if (!memberUuid.isEmpty()) {
            m_uuidToElement.remove(memberUuid);
            m_elementToUuid.remove(elem);
        }
    }
    m_elementToUuid.remove(tuplet);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove tuplet"));
    score->cmdDeleteTuplet(tuplet, /*replaceWithRest=*/true);
    score->endCmd();
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

bool ScoreApplicator::applyAddChordSymbol(Score* score, const QJsonObject& op)
{
    const QString id   = op["id"].toString();
    const QString name = op["name"].toString();
    const QJsonObject beat = op["beat"].toObject();

    if (id.isEmpty() || name.isEmpty()) {
        LOGW() << "[editude] applyAddChordSymbol: missing id or name";
        return false;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddChordSymbol: no measure at tick" << tick.toString();
        return false;
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add chord symbol"));
    Harmony* harmony = Factory::createHarmony(seg);
    harmony->setTrack(0);
    harmony->setParent(seg);
    harmony->setHarmonyType(HarmonyType::STANDARD);
    harmony->setHarmony(String(name));
    score->undoAddElement(harmony);
    score->endCmd();

    m_uuidToElement[id] = harmony;
    m_elementToUuid[harmony] = id;
    return true;
}

bool ScoreApplicator::applySetChordSymbol(Score* score, const QJsonObject& op)
{
    const QString id   = op["id"].toString();
    const QString name = op["name"].toString();

    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applySetChordSymbol: unknown id" << id;
        return false;
    }

    Harmony* harmony = dynamic_cast<Harmony*>(m_uuidToElement.value(id));
    if (!harmony) {
        LOGW() << "[editude] applySetChordSymbol: element is not Harmony" << id;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set chord symbol"));
    harmony->setHarmony(String(name));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveChordSymbol(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveChordSymbol: unknown id" << id;
        return false;
    }

    Harmony* harmony = dynamic_cast<Harmony*>(m_uuidToElement.value(id));
    if (!harmony) {
        LOGW() << "[editude] applyRemoveChordSymbol: element is not Harmony" << id;
        return false;
    }

    m_elementToUuid.remove(harmony);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove chord symbol"));
    score->undoRemoveElement(harmony);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 4 — navigation marks
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyInsertVolta(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty()) {
        LOGW() << "[editude] applyInsertVolta: missing id";
        return false;
    }

    const QJsonObject startBeatObj = op["start_beat"].toObject();
    const QJsonObject endBeatObj   = op["end_beat"].toObject();
    const Fraction startTick(startBeatObj["numerator"].toInt(),
                             startBeatObj["denominator"].toInt());
    const Fraction endTick(endBeatObj["numerator"].toInt(),
                           endBeatObj["denominator"].toInt());

    Measure* startMeasure = score->tick2measure(startTick);
    Measure* endMeasure   = score->tick2measure(endTick);
    if (!startMeasure || !endMeasure) {
        LOGW() << "[editude] applyInsertVolta: measure not found for tick range";
        return false;
    }

    // Build endings vector from JSON array.
    std::vector<int> endings;
    const QJsonArray numbersArr = op["numbers"].toArray();
    for (const auto& v : numbersArr) {
        endings.push_back(v.toInt());
    }
    const bool openEnd = op["open_end"].toBool(false);

    score->startCmd(TranslatableString("undoableAction", "Insert volta"));
    Volta* volta = Factory::createVolta(score->dummy());
    volta->setTrack(0);
    volta->setTick(startMeasure->tick());
    volta->setTick2(endMeasure->endTick());
    volta->setEndings(endings);
    volta->setVoltaType(openEnd ? Volta::Type::OPEN : Volta::Type::CLOSED);
    score->undoAddElement(volta);
    score->endCmd();

    m_uuidToElement[id] = volta;
    m_elementToUuid[volta] = id;
    return true;
}

bool ScoreApplicator::applyRemoveVolta(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveVolta: unknown id" << id;
        return false;
    }

    Volta* volta = dynamic_cast<Volta*>(m_uuidToElement.value(id));
    if (!volta) {
        LOGW() << "[editude] applyRemoveVolta: element is not Volta" << id;
        return false;
    }

    m_elementToUuid.remove(volta);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove volta"));
    score->undoRemoveElement(volta);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertMarker(Score* score, const QJsonObject& op)
{
    const QString id   = op["id"].toString();
    const QString kind = op["kind"].toString();
    if (id.isEmpty() || kind.isEmpty()) {
        LOGW() << "[editude] applyInsertMarker: missing id or kind";
        return false;
    }

    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyInsertMarker: no measure at tick" << tick.toString();
        return false;
    }

    // Map Python MarkerKind → MuseScore MarkerType.
    static const QHash<QString, MarkerType> s_markerKindMap = {
        { "segno",     MarkerType::SEGNO   },
        { "coda",      MarkerType::CODA    },
        { "fine",      MarkerType::FINE    },
        { "to_coda",   MarkerType::TOCODA  },
        { "segno_var", MarkerType::VARSEGNO },
    };
    MarkerType markerType = s_markerKindMap.value(kind, MarkerType::SEGNO);

    const QString label = op["label"].toString();

    score->startCmd(TranslatableString("undoableAction", "Insert marker"));
    Marker* marker = Factory::createMarker(measure);
    marker->setParent(measure);
    marker->setTrack(0);
    marker->setMarkerType(markerType);
    if (!label.isEmpty()) {
        marker->setLabel(String(label));
    }
    score->undoAddElement(marker);
    score->endCmd();

    m_uuidToElement[id] = marker;
    m_elementToUuid[marker] = id;
    return true;
}

bool ScoreApplicator::applyRemoveMarker(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveMarker: unknown id" << id;
        return false;
    }

    Marker* marker = dynamic_cast<Marker*>(m_uuidToElement.value(id));
    if (!marker) {
        LOGW() << "[editude] applyRemoveMarker: element is not Marker" << id;
        return false;
    }

    m_elementToUuid.remove(marker);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove marker"));
    score->undoRemoveElement(marker);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertJump(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty()) {
        LOGW() << "[editude] applyInsertJump: missing id";
        return false;
    }

    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyInsertJump: no measure at tick" << tick.toString();
        return false;
    }

    const QString jumpTo     = op["jump_to"].toString();
    const QString playUntil  = op["play_until"].toString();
    const QString continueAt = op["continue_at"].toString();
    const QString text       = op["text"].toString();

    score->startCmd(TranslatableString("undoableAction", "Insert jump"));
    Jump* jump = Factory::createJump(measure);
    jump->setParent(measure);
    jump->setTrack(0);
    if (!jumpTo.isEmpty()) {
        jump->setJumpTo(String(jumpTo));
    }
    if (!playUntil.isEmpty()) {
        jump->setPlayUntil(String(playUntil));
    }
    if (!continueAt.isEmpty()) {
        jump->setContinueAt(String(continueAt));
    }
    if (!text.isEmpty()) {
        jump->setPlainText(String(text));
    }
    score->undoAddElement(jump);
    score->endCmd();

    m_uuidToElement[id] = jump;
    m_elementToUuid[jump] = id;
    return true;
}

bool ScoreApplicator::applyRemoveJump(Score* score, const QJsonObject& op)
{
    const QString id = op["id"].toString();
    if (id.isEmpty() || !m_uuidToElement.contains(id)) {
        LOGW() << "[editude] applyRemoveJump: unknown id" << id;
        return false;
    }

    Jump* jump = dynamic_cast<Jump*>(m_uuidToElement.value(id));
    if (!jump) {
        LOGW() << "[editude] applyRemoveJump: element is not Jump" << id;
        return false;
    }

    m_elementToUuid.remove(jump);
    m_uuidToElement.remove(id);

    score->startCmd(TranslatableString("undoableAction", "Remove jump"));
    score->undoRemoveElement(jump);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Structural ops
// ---------------------------------------------------------------------------

bool ScoreApplicator::applySetScoreMetadata(Score* score, const QJsonObject& op)
{
    const QString field = op["field"].toString();
    const QString value = op["value"].toString();

    if (field.isEmpty()) {
        LOGW() << "[editude] applySetScoreMetadata: missing field";
        return false;
    }

    // Map Python field names to MuseScore meta tag names.
    static const QHash<QString, QString> s_fieldMap = {
        { "title",           "workTitle"       },
        { "subtitle",        "subtitle"        },
        { "composer",        "composer"        },
        { "arranger",        "arranger"        },
        { "lyricist",        "lyricist"        },
        { "copyright",       "copyright"       },
        { "work_number",     "workNumber"      },
        { "movement_number", "movementNumber"  },
        { "movement_title",  "movementTitle"   },
    };

    const QString tag = s_fieldMap.value(field, field);

    score->startCmd(TranslatableString("undoableAction", "Set score metadata"));
    score->setMetaTag(String(tag), String(value));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertBeats(Score* score, const QJsonObject& op)
{
    const QJsonObject atObj  = op["at_beat"].toObject();
    const QJsonObject durObj = op["duration"].toObject();

    const Fraction atTick(atObj["numerator"].toInt(), atObj["denominator"].toInt());
    const Fraction duration(durObj["numerator"].toInt(), durObj["denominator"].toInt());

    if (duration <= Fraction(0, 1)) {
        LOGW() << "[editude] applyInsertBeats: non-positive duration";
        return false;
    }

    Measure* targetMeasure = score->tick2measure(atTick);
    if (!targetMeasure) {
        LOGW() << "[editude] applyInsertBeats: no measure at tick" << atTick.toString();
        return false;
    }

    // Insert one measure at a time until we have filled `duration` of time.
    // Each new measure inherits the time signature of the target position.
    score->startCmd(TranslatableString("undoableAction", "Insert beats"));

    Fraction remaining = duration;
    while (remaining > Fraction(0, 1)) {
        // Re-resolve target each iteration because prior inserts shift ticks.
        Measure* m = score->tick2measure(atTick);
        if (!m) {
            break;
        }
        const Fraction measureLen = m->ticks();

        Score::InsertMeasureOptions opts;
        opts.createMeasureRests = true;
        opts.needDeselectAll = false;
        score->insertMeasure(ElementType::MEASURE, m, opts);
        remaining -= measureLen;
    }

    score->endCmd();
    return true;
}

bool ScoreApplicator::applyDeleteBeats(Score* score, const QJsonObject& op)
{
    const QJsonObject atObj  = op["at_beat"].toObject();
    const QJsonObject durObj = op["duration"].toObject();

    const Fraction atTick(atObj["numerator"].toInt(), atObj["denominator"].toInt());
    const Fraction duration(durObj["numerator"].toInt(), durObj["denominator"].toInt());

    if (duration <= Fraction(0, 1)) {
        LOGW() << "[editude] applyDeleteBeats: non-positive duration";
        return false;
    }

    const Fraction endTick = atTick + duration;

    Measure* firstMeasure = score->tick2measure(atTick);
    if (!firstMeasure) {
        LOGW() << "[editude] applyDeleteBeats: no measure at tick" << atTick.toString();
        return false;
    }

    // Collect all measures whose tick falls strictly before endTick.
    Measure* lastMeasure = nullptr;
    for (Measure* m = firstMeasure; m; m = m->nextMeasure()) {
        if (m->tick() >= endTick) {
            break;
        }
        lastMeasure = m;
    }
    if (!lastMeasure) {
        LOGW() << "[editude] applyDeleteBeats: no measures in range";
        return false;
    }

    // Purge UUID maps for any element inside the deleted range so stale
    // pointers do not outlive the undo stack.
    const Fraction actualEndTick = lastMeasure->tick() + lastMeasure->ticks();
    QList<QString> keysToRemove;
    for (auto it = m_uuidToElement.constBegin(); it != m_uuidToElement.constEnd(); ++it) {
        auto* item = dynamic_cast<EngravingItem*>(it.value());
        if (item && item->tick() >= atTick && item->tick() < actualEndTick) {
            m_elementToUuid.remove(it.value());
            keysToRemove.append(it.key());
        }
    }
    for (const QString& k : keysToRemove) {
        m_uuidToElement.remove(k);
    }

    score->startCmd(TranslatableString("undoableAction", "Delete beats"));
    score->deleteMeasures(firstMeasure, lastMeasure);
    score->endCmd();
    return true;
}
