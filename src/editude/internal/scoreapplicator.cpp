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
#include "scoreapplicator.h"
#include "editudeutils.h"

#include <QJsonArray>

#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/stringdata.h"
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
#include "engraving/editing/editscoreproperties.h"
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
#include "engraving/dom/tempotext.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/accidental.h"
#include "engraving/dom/arpeggio.h"
#include "engraving/dom/glissando.h"
#include "engraving/dom/ornament.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/tremolosinglechord.h"
#include "engraving/dom/tremolotwochord.h"
#include "engraving/dom/trill.h"
#include "engraving/editing/undo.h"
#include "engraving/dom/volta.h"
#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"
#include "engraving/types/symnames.h"
#include "engraving/types/typesconv.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

// ---------------------------------------------------------------------------
// Lifecycle helpers
// ---------------------------------------------------------------------------

void ScoreApplicator::reset()
{
    m_partUuidToPart.clear();
}

Part* ScoreApplicator::resolvePart(const QJsonObject& op) const
{
    const QString partId = op["part_id"].toString();
    if (partId.isEmpty() || !m_partUuidToPart.contains(partId)) {
        return nullptr;
    }
    return m_partUuidToPart.value(partId);
}

// ---------------------------------------------------------------------------
// Shared static helpers
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Tier 1 — coordinate-addressed note/rest operations
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyInsertNote(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyInsertNote: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitch    = op["pitch"].toObject();
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

    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const track_idx_t track = trackFromCoord(part, voice, stf);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applyInsertNote: no segment at tick" << tick.toString();
        return false;
    }

    NoteVal nval(midi); // TPC_INVALID — MuseScore infers spelling from key signature
    TDuration dur(dt);
    dur.setDots(dots);

    score->startCmd(TranslatableString("undoableAction", "Insert note"));
    // If a chord already exists at this segment/track, add the note to it
    // instead of replacing the entire ChordRest (which setNoteRest does).
    EngravingItem* existing = seg->element(track);
    if (existing && existing->isChord()) {
        score->addNote(toChord(existing), nval);
    } else {
        score->setNoteRest(seg, track, nval, dur.ticks());
    }
    score->endCmd();

    // Optional tab fields — set fret/string if provided by the op.
    const int tabFret   = op.value("fret").toInt(-1);
    const int tabString = op.value("string").toInt(-1);
    // Optional notehead — set head group if provided (percussion / manual override).
    const QString noteheadStr = op.value("notehead").toString();
    const bool hasNotehead = !noteheadStr.isEmpty();

    if (tabFret >= 0 || tabString >= 0 || hasNotehead) {
        Segment* seg2 = score->tick2segment(tick, false, SegmentType::ChordRest);
        if (seg2) {
            EngravingItem* el = seg2->element(track);
            if (el && el->type() == ElementType::CHORD) {
                Chord* chord = toChord(el);
                for (Note* n : chord->notes()) {
                    if (n->pitch() == midi) {
                        if (tabFret >= 0 && tabString >= 0) {
                            n->setFret(tabFret);
                            n->setString(tabString);
                        }
                        if (hasNotehead) {
                            n->setHeadGroup(noteheadGroupFromString(noteheadStr));
                        }
                        break;
                    }
                }
            }
        }
    }

    return true;
}

bool ScoreApplicator::applyInsertRest(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyInsertRest: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat   = op["beat"].toObject();
    const QJsonObject durObj = op["duration"].toObject();

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const DurationType dt = parseDurationType(durObj["type"].toString());
    const int dots        = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID) {
        LOGW() << "[editude] applyInsertRest: invalid duration";
        return false;
    }

    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const track_idx_t track = trackFromCoord(part, voice, stf);

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
    return true;
}

bool ScoreApplicator::applyDeleteNote(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyDeleteNote: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat  = op["beat"].toObject();
    const QJsonObject pitch = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int midi = pitchToMidi(pitch["step"].toString(),
                                 pitch["octave"].toInt(),
                                 pitch["accidental"].toString());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applyDeleteNote: invalid pitch";
        return false;
    }

    Note* note = findNoteAtCoord(score, part, tick, midi, voice, stf);
    if (!note) {
        LOGW() << "[editude] applyDeleteNote: note not found at coordinate";
        return false;
    }

    Chord* chord = note->chord();
    const track_idx_t track = trackFromCoord(part, voice, stf);
    score->startCmd(TranslatableString("undoableAction", "Delete note"));
    if (chord->notes().size() == 1) {
        // Last note in chord — check whether this is the only non-rest in
        // the voice for this measure.  If so, replace the entire voice
        // content with a full-measure rest (matching the editor's undo
        // behaviour).  Otherwise just delete the chord.
        Measure* measure = chord->measure();
        bool onlyNonRest = true;
        for (Segment* s = measure->first(SegmentType::ChordRest); s;
             s = s->next(SegmentType::ChordRest)) {
            EngravingItem* e = s->element(track);
            if (e && !e->isRest() && e != chord) {
                onlyNonRest = false;
                break;
            }
        }

        if (onlyNonRest) {
            // Replace the whole voice with a full-measure rest.
            Segment* seg0 = measure->getSegmentR(SegmentType::ChordRest,
                                                  measure->tick());
            score->setNoteRest(seg0, track, NoteVal(), measure->ticks());
        } else {
            score->deleteItem(chord);
        }
    } else {
        // Multiple notes — just remove this one pitch.
        score->deleteItem(note);
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyDeleteRest(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyDeleteRest: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    Rest* rest = findRestAtCoord(score, part, tick, voice, stf);
    if (!rest) {
        LOGW() << "[editude] applyDeleteRest: rest not found at coordinate";
        return false;
    }

    // Restore the voice to its "empty" state by replacing the entire voice
    // content with a full-measure rest.  InsertRest creates a rest plus fill
    // rests (e.g. quarter@0 + quarter@1/4 + half@1/2 in 4/4), so merely
    // calling deleteItem() on one rest leaves the fills behind.  setNoteRest
    // with the full measure duration replaces everything in the voice,
    // matching the editor's undo behaviour and the Python model (where
    // removing a RestEvent implicitly restores the whole-measure fill).
    Measure* measure = rest->measure();
    const track_idx_t track = trackFromCoord(part, voice, stf);

    score->startCmd(TranslatableString("undoableAction", "Delete rest"));
    Segment* seg0 = measure->getSegmentR(SegmentType::ChordRest, measure->tick());
    score->setNoteRest(seg0, track, NoteVal(), measure->ticks());
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetPitch(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetPitch: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitchObj = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int oldMidi = pitchToMidi(pitchObj["step"].toString(),
                                    pitchObj["octave"].toInt(),
                                    pitchObj["accidental"].toString());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    if (oldMidi < 0 || oldMidi > 127) {
        LOGW() << "[editude] applySetPitch: invalid old pitch";
        return false;
    }

    Note* note = findNoteAtCoord(score, part, tick, oldMidi, voice, stf);
    if (!note) {
        LOGW() << "[editude] applySetPitch: note not found at coordinate";
        return false;
    }

    const QJsonObject newPitchObj = op["new_pitch"].toObject();
    const int newMidi = pitchToMidi(newPitchObj["step"].toString(),
                                    newPitchObj["octave"].toInt(),
                                    newPitchObj["accidental"].toString());
    if (newMidi < 0 || newMidi > 127) {
        LOGW() << "[editude] applySetPitch: invalid new pitch";
        return false;
    }

    // Set PITCH, TPC1, and TPC2 together — setPitch(int) does NOT update
    // TPC, so the translator's pitchJson(tpc1, octave) would read stale TPC.
    const int tpc1 = note->tpc1default(newMidi);
    const int tpc2 = note->tpc2default(newMidi);
    score->startCmd(TranslatableString("undoableAction", "Set pitch"));
    note->undoChangeProperty(Pid::PITCH, newMidi);
    note->undoChangeProperty(Pid::TPC1, tpc1);
    note->undoChangeProperty(Pid::TPC2, tpc2);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetDuration(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetDuration: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat   = op["beat"].toObject();
    const QJsonObject durObj = op["duration"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const track_idx_t track = trackFromCoord(part, voice, stf);

    const DurationType dt = parseDurationType(durObj["type"].toString());
    const int dots        = durObj["dots"].toInt(0);

    if (dt == DurationType::V_INVALID) {
        LOGW() << "[editude] applySetDuration: invalid duration";
        return false;
    }

    // Locate the ChordRest at the coordinate.
    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr) {
        LOGW() << "[editude] applySetDuration: no ChordRest at coordinate";
        return false;
    }

    // If the op specifies a pitch, find that specific note's chord. Otherwise
    // use the ChordRest directly (could be a rest or single-note chord).
    int midiForRefind = -1;
    if (op.contains("pitch") && !op["pitch"].isNull()) {
        const QJsonObject pitchObj = op["pitch"].toObject();
        midiForRefind = pitchToMidi(pitchObj["step"].toString(),
                                    pitchObj["octave"].toInt(),
                                    pitchObj["accidental"].toString());
    } else if (cr->type() == ElementType::CHORD) {
        Chord* chord = toChord(cr);
        if (!chord->notes().empty()) {
            midiForRefind = chord->notes().front()->pitch();
        }
    }

    TDuration dur(dt);
    dur.setDots(dots);

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        LOGW() << "[editude] applySetDuration: no segment at tick" << tick.toString();
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set duration"));
    score->setNoteRest(seg, track,
                       midiForRefind >= 0 ? NoteVal(midiForRefind) : NoteVal(),
                       dur.ticks());
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetTie(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetTie: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitchObj = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applySetTie: invalid pitch";
        return false;
    }

    Note* note = findNoteAtCoord(score, part, tick, midi, voice, stf);
    if (!note) {
        LOGW() << "[editude] applySetTie: note not found at coordinate";
        return false;
    }

    const QJsonValue tieVal = op["tie"];
    const bool wantTie = !tieVal.isNull() && !tieVal.isUndefined()
                         && tieVal.toString() != QStringLiteral("stop");

    score->startCmd(TranslatableString("undoableAction", "Set tie"));

    if (wantTie) {
        // "start" or "continue": ensure a forward tie exists.
        if (!note->tieFor()) {
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

bool ScoreApplicator::applySetVoice(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetVoice: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int oldVoice = opVoice(op);
    const int stf      = opStaff(op);
    const int newVoice = op["new_voice"].toInt(1);

    // Find the ChordRest at the old voice coordinate.
    ChordRest* cr = findChordRestAtCoord(score, part, tick, oldVoice, stf);
    if (!cr) {
        LOGW() << "[editude] applySetVoice: no ChordRest at coordinate";
        return false;
    }

    const track_idx_t newTrack = trackFromCoord(part, newVoice, stf);
    score->startCmd(TranslatableString("undoableAction", "Set voice"));
    cr->undoChangeProperty(Pid::TRACK, static_cast<int>(newTrack));
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 2 — score directive operations (already position-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applySetTimeSignature(Score* score, const QJsonObject& op)
{
    const QJsonObject beat  = op["beat"].toObject();
    const QJsonObject tsObj = op["time_signature"].toObject();

    if (tsObj.isEmpty()) {
        LOGD() << "[editude] applySetTimeSignature: null value (remove) not yet implemented";
        return true;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int num   = tsObj["numerator"].toInt(0);
    const int denom = tsObj["denominator"].toInt(0);

    if (num <= 0 || denom <= 0) {
        LOGW() << "[editude] applySetTimeSignature: invalid time signature"
               << num << "/" << denom;
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
    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject tempoObj = op["tempo"].toObject();

    if (tempoObj.isEmpty()) {
        LOGD() << "[editude] applySetTempo: null value (remove) not yet implemented";
        return true;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const double bpm = tempoObj["bpm"].toDouble(0.0);

    if (bpm <= 0.0) {
        LOGW() << "[editude] applySetTempo: invalid bpm" << bpm;
        return false;
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetTempo: no measure at tick" << tick.toString();
        return false;
    }
    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Set tempo"));
    TempoText* tt = Factory::createTempoText(seg);
    tt->setTempo(BeatsPerSecond(bpm / 60.0));
    tt->setParent(seg);
    tt->setTrack(0);
    score->undoAddElement(tt);
    score->endCmd();
    return true;
}

void ScoreApplicator::bootstrapPartMap(Score* score)
{
    Q_UNUSED(score);
    LOGD() << "[editude] bootstrapPartMap: "
           << m_partUuidToPart.size() << " parts already registered";
}

void ScoreApplicator::registerPart(Part* part, const QString& uuid)
{
    m_partUuidToPart[uuid] = part;
}

bool ScoreApplicator::applySetKeySignature(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetKeySignature: unknown part_id";
        return false;
    }

    const QJsonValue ksSigVal = op["key_signature"];
    if (ksSigVal.isNull() || ksSigVal.isUndefined()) {
        const QJsonObject beat = op["beat"].toObject();
        const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
        Measure* measure = score->tick2measure(tick);
        if (!measure) {
            return true;
        }
        Segment* seg = measure->findSegment(SegmentType::KeySig, tick);
        if (!seg) {
            return true;
        }
        const staff_idx_t firstStaff = part->startTrack() / VOICES;
        const staff_idx_t nStaves    = static_cast<staff_idx_t>(part->nstaves());
        score->startCmd(TranslatableString("undoableAction", "Remove key signature"));
        for (staff_idx_t i = 0; i < nStaves; ++i) {
            const track_idx_t track = (firstStaff + i) * VOICES;
            EngravingItem* el = seg->element(track);
            if (el && el->isKeySig()) {
                score->undoRemoveElement(el);
            }
        }
        score->endCmd();
        return true;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int sharps = ksSigVal.toObject()["sharps"].toInt(0);

    if (sharps < -7 || sharps > 7) {
        LOGW() << "[editude] applySetKeySignature: sharps out of range" << sharps;
        return false;
    }

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetKeySignature: no measure at tick" << tick.toString();
        return false;
    }

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
        ks->setParent(seg);
        score->undoAddElement(ks);
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetClef(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetClef: unknown part_id";
        return false;
    }

    const QJsonValue clefVal = op["clef"];
    if (clefVal.isNull() || clefVal.isUndefined()) {
        const QJsonObject beat = op["beat"].toObject();
        const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
        const int staffIdx = op["staff"].toInt(0);
        Measure* measure = score->tick2measure(tick);
        if (!measure) {
            return true;
        }
        Segment* seg = measure->findSegment(SegmentType::Clef, tick);
        if (!seg) {
            return true;
        }
        const track_idx_t track = (part->startTrack() / VOICES + staffIdx) * VOICES;
        EngravingItem* el = seg->element(track);
        if (el && el->isClef()) {
            score->startCmd(TranslatableString("undoableAction", "Remove clef"));
            score->undoRemoveElement(el);
            score->endCmd();
        }
        return true;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int staffIdx     = op["staff"].toInt(0);
    const QString clefName = clefVal.toObject()["name"].toString();

    static const QHash<QString, ClefType> s_clefMap = {
        { "treble",         ClefType::G      },
        { "treble_8vb",     ClefType::G8_VB  },
        { "treble_8va",     ClefType::G8_VA  },
        { "treble_15mb",    ClefType::G15_MB },
        { "treble_15ma",    ClefType::G15_MA },
        { "bass",           ClefType::F      },
        { "bass_8vb",       ClefType::F8_VB  },
        { "bass_8va",       ClefType::F_8VA  },
        { "bass_15mb",      ClefType::F15_MB },
        { "bass_15ma",      ClefType::F_15MA },
        { "alto",           ClefType::C3     },
        { "tenor",          ClefType::C4     },
        { "mezzo_soprano",  ClefType::C2     },
        { "soprano",        ClefType::C1     },
        { "baritone",       ClefType::C5     },
        { "percussion",     ClefType::PERC   },
        { "tab",            ClefType::TAB    },
        { "tab4",           ClefType::TAB4   },
    };

    const ClefType ct = s_clefMap.value(clefName, ClefType::INVALID);
    if (ct == ClefType::INVALID) {
        LOGW() << "[editude] applySetClef: unknown clef name" << clefName;
        return false;
    }

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
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetPartName: unknown part_id";
        return false;
    }
    const QString name = op["name"].toString();
    score->startCmd(TranslatableString("undoableAction", "Set part name"));
    part->setPartName(String(name));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetStaffCount(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetStaffCount: unknown part_id";
        return false;
    }
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
            score->undoInsertStaff(staff, static_cast<staff_idx_t>(i), false);
        }
    } else {
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

    // Already registered under this UUID (idempotent).
    if (m_partUuidToPart.contains(uuid)) {
        return true;
    }

    // Bootstrap de-duplication: when two clients connect simultaneously to a
    // fresh project, both bootstrap their pre-existing MSCX part (with
    // different UUIDs). Each then receives the other's AddPart as a remote
    // op. Instead of creating a duplicate, register the existing unregistered
    // part under the remote UUID.
    const QSet<Part*> registered(m_partUuidToPart.cbegin(), m_partUuidToPart.cend());
    for (Part* existing : score->parts()) {
        if (registered.contains(existing)) {
            continue;
        }
        m_partUuidToPart[uuid] = existing;
        LOGD() << "[editude] applyAddPart: adopted existing part for uuid" << uuid;
        return true;
    }

    const QString name = op["name"].toString();
    const int staffCount = op["staff_count"].toInt(1);

    Part* part = new Part(score);
    part->setPartName(String(name));

    const QJsonObject instr = op["instrument"].toObject();
    if (!instr.isEmpty()) {
        const QString msId      = instr["musescore_id"].toString();
        const QString longName  = instr["name"].toString(name);
        const QString shortName = instr["short_name"].toString();
        Instrument* existing = part->instrument();
        if (existing && !msId.isEmpty()) {
            existing->setId(String(msId));
            existing->setLongName(String(longName));
            existing->setShortName(String(shortName));
        }
        part->setLongNameAll(String(longName));
        part->setShortNameAll(String(shortName));
    }

    score->startCmd(TranslatableString("undoableAction", "Add part"));
    score->undoInsertPart(part, score->parts().size());
    for (int i = 0; i < staffCount; ++i) {
        Staff* staff = Factory::createStaff(part);
        score->undoInsertStaff(staff, static_cast<staff_idx_t>(i), false);
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
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetPartInstrument: unknown part_id";
        return false;
    }
    const QJsonObject instr = op["instrument"].toObject();
    if (instr.isEmpty()) {
        LOGW() << "[editude] applySetPartInstrument: null instrument — ignored";
        return false;
    }
    const QString msId      = instr["musescore_id"].toString();
    const QString longName  = instr["name"].toString();
    const QString shortName = instr["short_name"].toString();
    score->startCmd(TranslatableString("undoableAction", "Set part instrument"));
    Instrument* existing = part->instrument();
    if (existing && !msId.isEmpty()) {
        existing->setId(String(msId));
        existing->setLongName(String(longName));
        existing->setShortName(String(shortName));

        const QJsonObject sdObj = instr["string_data"].toObject();
        if (!sdObj.isEmpty()) {
            const QJsonArray strings = sdObj["strings"].toArray();
            std::vector<instrString> table;
            for (const QJsonValue& v : strings) {
                const QJsonObject s = v.toObject();
                table.emplace_back(s["pitch"].toInt(), s["open"].toBool(false),
                                   s["start_fret"].toInt(0));
            }
            StringData sd(sdObj["frets"].toInt(), table);
            existing->setStringData(sd);
        }

        const bool useDrumset = instr["use_drumset"].toBool(false);
        if (useDrumset) {
            existing->setUseDrumset(true);
            const QJsonObject dsOverrides = instr["drumset_overrides"].toObject();
            if (!dsOverrides.isEmpty()) {
                const QJsonObject dsInstruments = dsOverrides["instruments"].toObject();
                Drumset drumset;
                for (auto it = dsInstruments.begin(); it != dsInstruments.end(); ++it) {
                    const int pitch = it.key().toInt();
                    const QJsonObject entry = it.value().toObject();
                    drumset.drum(pitch).name = String(entry["name"].toString());
                    drumset.drum(pitch).notehead = noteheadGroupFromString(
                        entry["notehead"].toString());
                    drumset.drum(pitch).line = entry["line"].toInt();
                    drumset.drum(pitch).stemDirection = stemDirectionFromString(
                        entry["stem_direction"].toString());
                    drumset.drum(pitch).voice = entry["voice"].toInt();
                    drumset.drum(pitch).shortcut = String(entry["shortcut"].toString());

                    const QJsonArray varArr = entry["variants"].toArray();
                    std::vector<DrumInstrumentVariant> variants;
                    for (const QJsonValue& vv : varArr) {
                        const QJsonObject vObj = vv.toObject();
                        DrumInstrumentVariant var;
                        var.pitch = vObj["pitch"].toInt();
                        const QString tt = vObj["tremolo_type"].toString();
                        if (!tt.isEmpty()) {
                            var.tremolo = tremoloTypeFromString(tt);
                        }
                        var.articulationName = String(
                            vObj["articulation_name"].toString());
                        variants.push_back(var);
                    }
                    drumset.drum(pitch).variants = std::move(variants);
                }
                existing->setDrumset(&drumset);
            }
        }
    }
    part->setPartName(String(longName));
    part->setLongNameAll(String(longName));
    part->setShortNameAll(String(shortName));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetStringData(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetStringData: unknown part_id";
        return false;
    }
    Instrument* inst = part->instrument();
    if (!inst) {
        LOGW() << "[editude] applySetStringData: part has no instrument";
        return false;
    }

    const QJsonObject sdObj = op["string_data"].toObject();
    score->startCmd(TranslatableString("undoableAction", "Set string data"));
    if (sdObj.isEmpty()) {
        inst->setStringData(StringData());
    } else {
        const QJsonArray strings = sdObj["strings"].toArray();
        std::vector<instrString> table;
        for (const QJsonValue& v : strings) {
            const QJsonObject s = v.toObject();
            table.emplace_back(s["pitch"].toInt(), s["open"].toBool(false),
                               s["start_fret"].toInt(0));
        }
        StringData sd(sdObj["frets"].toInt(), table);
        inst->setStringData(sd);
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetCapo(Score* score, const QJsonObject& op)
{
    Q_UNUSED(score);
    Q_UNUSED(op);
    return true;
}

bool ScoreApplicator::applySetTabNote(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetTabNote: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitchObj = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applySetTabNote: invalid pitch";
        return false;
    }

    Note* note = findNoteAtCoord(score, part, tick, midi, voice, stf);
    if (!note) {
        LOGW() << "[editude] applySetTabNote: note not found at coordinate";
        return false;
    }

    const int fret   = op["fret"].toInt(-1);
    const int string = op["string"].toInt(-1);
    if (fret < 0 || string < 0) {
        LOGW() << "[editude] applySetTabNote: invalid fret/string";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set tab note"));
    note->undoChangeProperty(Pid::FRET, fret);
    note->undoChangeProperty(Pid::STRING, string);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetDrumset(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetDrumset: unknown part_id";
        return false;
    }

    Instrument* inst = part->instrument();
    if (!inst) {
        LOGW() << "[editude] applySetDrumset: part has no instrument";
        return false;
    }

    const QJsonObject dsObj = op["drumset"].toObject();
    if (dsObj.isEmpty()) {
        return true;
    }

    const QJsonObject instruments = dsObj["instruments"].toObject();
    Drumset drumset;
    for (auto it = instruments.begin(); it != instruments.end(); ++it) {
        const int pitch = it.key().toInt();
        const QJsonObject entry = it.value().toObject();
        drumset.drum(pitch).name      = String(entry["name"].toString());
        drumset.drum(pitch).notehead  = noteheadGroupFromString(entry["notehead"].toString());
        drumset.drum(pitch).line      = entry["line"].toInt();
        drumset.drum(pitch).stemDirection = stemDirectionFromString(
            entry["stem_direction"].toString());
        drumset.drum(pitch).voice     = entry["voice"].toInt();
        drumset.drum(pitch).shortcut  = String(entry["shortcut"].toString());

        const QJsonArray varArr = entry["variants"].toArray();
        std::vector<DrumInstrumentVariant> variants;
        for (const QJsonValue& vv : varArr) {
            const QJsonObject vObj = vv.toObject();
            DrumInstrumentVariant var;
            var.pitch = vObj["pitch"].toInt();
            const QString tt = vObj["tremolo_type"].toString();
            if (!tt.isEmpty()) {
                var.tremolo = tremoloTypeFromString(tt);
            }
            var.articulationName = String(vObj["articulation_name"].toString());
            variants.push_back(var);
        }
        drumset.drum(pitch).variants = std::move(variants);
    }

    score->startCmd(TranslatableString("undoableAction", "Set drumset"));
    inst->setDrumset(&drumset);
    inst->setUseDrumset(true);
    part->undoChangeProperty(Pid::STAFF_LONG_NAME, PropertyValue(String(part->longName())));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetNoteHead(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetNoteHead: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat     = op["beat"].toObject();
    const QJsonObject pitchObj = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int midi = pitchToMidi(pitchObj["step"].toString(),
                                 pitchObj["octave"].toInt(),
                                 pitchObj["accidental"].toString());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applySetNoteHead: invalid pitch";
        return false;
    }

    Note* note = findNoteAtCoord(score, part, tick, midi, voice, stf);
    if (!note) {
        LOGW() << "[editude] applySetNoteHead: note not found at coordinate";
        return false;
    }

    const NoteHeadGroup headGroup = noteheadGroupFromString(op["notehead"].toString());

    score->startCmd(TranslatableString("undoableAction", "Set notehead"));
    note->undoChangeProperty(Pid::HEAD_GROUP, int(headGroup));
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// apply() — dispatch table
// ---------------------------------------------------------------------------

bool ScoreApplicator::apply(Score* score, const QJsonObject& payload)
{
    const QString type = payload["type"].toString();

    // op_batch — apply each sub-op in sequence, clearing undo entries after
    // each one.  Multi-step operations (e.g. RemoveTuplet) create
    // RemoveElement entries that reference shared elements like Tuplets.
    // If two sub-ops within the same batch both produce RemoveElement entries
    // for the same element, deferring clearAll() to the end of the batch
    // causes remove(0) to cleanup(true) the same element twice (double-free).
    // Clearing after each sub-op prevents cross-macro element conflicts.
    if (type == QLatin1String("op_batch")) {
        const QJsonArray ops = payload["ops"].toArray();
        if (ops.isEmpty()) {
            return true;
        }
        bool ok = true;
        for (const QJsonValue& v : ops) {
            if (!apply(score, v.toObject())) {
                ok = false;
            }
            score->undoStack()->clearAll();
        }
        return ok;
    }

    // Tier 1 — coordinate-addressed note/rest operations
    if (type == QLatin1String("InsertNote"))   return applyInsertNote(score, payload);
    if (type == QLatin1String("InsertRest"))   return applyInsertRest(score, payload);
    if (type == QLatin1String("DeleteNote"))   return applyDeleteNote(score, payload);
    if (type == QLatin1String("DeleteRest"))   return applyDeleteRest(score, payload);
    if (type == QLatin1String("SetPitch"))     return applySetPitch(score, payload);
    if (type == QLatin1String("SetDuration"))  return applySetDuration(score, payload);
    if (type == QLatin1String("SetTie"))       return applySetTie(score, payload);
    if (type == QLatin1String("SetVoice"))     return applySetVoice(score, payload);

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
    if (type == QLatin1String("SetStringData"))     return applySetStringData(score, payload);
    if (type == QLatin1String("SetCapo"))            return applySetCapo(score, payload);
    if (type == QLatin1String("SetTabNote"))         return applySetTabNote(score, payload);
    if (type == QLatin1String("SetDrumset"))         return applySetDrumset(score, payload);
    if (type == QLatin1String("SetNoteHead"))        return applySetNoteHead(score, payload);

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

    // Tier 3 — staff text (part-scoped)
    if (type == QLatin1String("AddStaffText"))    return applyAddStaffText(score, payload);
    if (type == QLatin1String("SetStaffText"))    return applySetStaffText(score, payload);
    if (type == QLatin1String("RemoveStaffText")) return applyRemoveStaffText(score, payload);

    // Tier 3 — system text (score-global)
    if (type == QLatin1String("AddSystemText"))    return applyAddSystemText(score, payload);
    if (type == QLatin1String("SetSystemText"))    return applySetSystemText(score, payload);
    if (type == QLatin1String("RemoveSystemText")) return applyRemoveSystemText(score, payload);

    // Tier 3 — rehearsal marks (score-global)
    if (type == QLatin1String("AddRehearsalMark"))    return applyAddRehearsalMark(score, payload);
    if (type == QLatin1String("SetRehearsalMark"))    return applySetRehearsalMark(score, payload);
    if (type == QLatin1String("RemoveRehearsalMark")) return applyRemoveRehearsalMark(score, payload);

    // Advanced spanners — octave lines
    if (type == QLatin1String("AddOctaveLine"))    return applyAddOctaveLine(score, payload);
    if (type == QLatin1String("RemoveOctaveLine")) return applyRemoveOctaveLine(score, payload);
    // Advanced spanners — glissandos
    if (type == QLatin1String("AddGlissando"))    return applyAddGlissando(score, payload);
    if (type == QLatin1String("RemoveGlissando")) return applyRemoveGlissando(score, payload);
    // Advanced spanners — pedal lines
    if (type == QLatin1String("AddPedalLine"))    return applyAddPedalLine(score, payload);
    if (type == QLatin1String("RemovePedalLine")) return applyRemovePedalLine(score, payload);
    // Advanced spanners — trill lines
    if (type == QLatin1String("AddTrillLine"))    return applyAddTrillLine(score, payload);
    if (type == QLatin1String("RemoveTrillLine")) return applyRemoveTrillLine(score, payload);
    // Arpeggios
    if (type == QLatin1String("AddArpeggio"))    return applyAddArpeggio(score, payload);
    if (type == QLatin1String("RemoveArpeggio")) return applyRemoveArpeggio(score, payload);
    if (type == QLatin1String("AddGraceNote"))   return applyAddGraceNote(score, payload);
    if (type == QLatin1String("RemoveGraceNote")) return applyRemoveGraceNote(score, payload);
    // Breath marks / caesuras
    if (type == QLatin1String("AddBreathMark"))    return applyAddBreathMark(score, payload);
    if (type == QLatin1String("RemoveBreathMark")) return applyRemoveBreathMark(score, payload);
    // Tremolos (single-note)
    if (type == QLatin1String("AddTremolo"))    return applyAddTremolo(score, payload);
    if (type == QLatin1String("RemoveTremolo")) return applyRemoveTremolo(score, payload);
    // Two-note tremolos
    if (type == QLatin1String("AddTwoNoteTremolo"))    return applyAddTwoNoteTremolo(score, payload);
    if (type == QLatin1String("RemoveTwoNoteTremolo")) return applyRemoveTwoNoteTremolo(score, payload);

    // Tier 4 — navigation marks
    if (type == QLatin1String("SetStartRepeat")) return applySetStartRepeat(score, payload);
    if (type == QLatin1String("SetEndRepeat"))   return applySetEndRepeat(score, payload);
    if (type == QLatin1String("InsertVolta"))  return applyInsertVolta(score, payload);
    if (type == QLatin1String("RemoveVolta"))  return applyRemoveVolta(score, payload);
    if (type == QLatin1String("InsertMarker")) return applyInsertMarker(score, payload);
    if (type == QLatin1String("RemoveMarker")) return applyRemoveMarker(score, payload);
    if (type == QLatin1String("InsertJump"))   return applyInsertJump(score, payload);
    if (type == QLatin1String("RemoveJump"))   return applyRemoveJump(score, payload);

    // Structural ops
    if (type == QLatin1String("SetScoreMetadata")) return applySetScoreMetadata(score, payload);
    if (type == QLatin1String("SetMeasureLen"))     return applySetMeasureLen(score, payload);
    if (type == QLatin1String("InsertBeats"))       return applyInsertBeats(score, payload);
    if (type == QLatin1String("DeleteBeats"))       return applyDeleteBeats(score, payload);

    LOGD() << "[editude] ScoreApplicator: unhandled op type" << type;
    return false;
}

// ---------------------------------------------------------------------------
// Tier 3 — articulations (coordinate-addressed)
// ---------------------------------------------------------------------------

static SymId articulationSymId(const QString& name)
{
    static const QHash<QString, SymId> s_map = {
        // --- Standard articulations (always use Above variant) ---
        { "staccato",                    SymId::articStaccatoAbove                 },
        { "accent",                      SymId::articAccentAbove                   },
        { "tenuto",                      SymId::articTenutoAbove                   },
        { "marcato",                     SymId::articMarcatoAbove                  },
        { "staccatissimo",               SymId::articStaccatissimoAbove            },
        { "staccatissimo_stroke",        SymId::articStaccatissimoStrokeAbove      },
        { "staccatissimo_wedge",         SymId::articStaccatissimoWedgeAbove       },
        { "tenuto_staccato",             SymId::articTenutoStaccatoAbove           },
        { "accent_staccato",             SymId::articAccentStaccatoAbove           },
        { "marcato_staccato",            SymId::articMarcatoStaccatoAbove          },
        { "marcato_tenuto",              SymId::articMarcatoTenutoAbove            },
        { "tenuto_accent",               SymId::articTenutoAccentAbove             },
        { "stress",                      SymId::articStressAbove                   },
        { "unstress",                    SymId::articUnstressAbove                 },
        { "soft_accent",                 SymId::articSoftAccentAbove               },
        { "soft_accent_staccato",        SymId::articSoftAccentStaccatoAbove       },
        { "soft_accent_tenuto",          SymId::articSoftAccentTenutoAbove         },
        { "soft_accent_tenuto_staccato", SymId::articSoftAccentTenutoStaccatoAbove },

        // --- Fermatas ---
        { "fermata",            SymId::fermataAbove          },
        { "fermata_short",      SymId::fermataShortAbove     },
        { "fermata_long",       SymId::fermataLongAbove      },
        { "fermata_very_short", SymId::fermataVeryShortAbove },
        { "fermata_very_long",  SymId::fermataVeryLongAbove  },
        { "fermata_long_henze", SymId::fermataLongHenzeAbove },
        { "fermata_short_henze", SymId::fermataShortHenzeAbove },

        // --- Ornaments ---
        { "trill",                 SymId::ornamentTrill                     },
        { "mordent",               SymId::ornamentMordent                   },
        { "turn",                  SymId::ornamentTurn                      },
        { "turn_inverted",         SymId::ornamentTurnInverted              },
        { "turn_slash",            SymId::ornamentTurnSlash                 },
        { "turn_up",               SymId::ornamentTurnUp                    },
        { "short_trill",           SymId::ornamentShortTrill                },
        { "tremblement",           SymId::ornamentTremblement               },
        { "prall_mordent",         SymId::ornamentPrallMordent              },
        { "up_prall",              SymId::ornamentUpPrall                   },
        { "mordent_upper_prefix",  SymId::ornamentPrecompMordentUpperPrefix },
        { "up_mordent",            SymId::ornamentUpMordent                 },
        { "down_mordent",          SymId::ornamentDownMordent               },
        { "prall_down",            SymId::ornamentPrallDown                 },
        { "prall_up",              SymId::ornamentPrallUp                   },
        { "line_prall",            SymId::ornamentLinePrall                 },
        { "precomp_slide",         SymId::ornamentPrecompSlide              },
        { "shake",                 SymId::ornamentShake3                    },
        { "shake_muffat",          SymId::ornamentShakeMuffat1              },
        { "tremblement_couperin",  SymId::ornamentTremblementCouperin       },
        { "pince_couperin",        SymId::ornamentPinceCouperin             },
        { "haydn",                 SymId::ornamentHaydn                     },

        // --- Bowing / string techniques ---
        { "up_bow",               SymId::stringsUpBow              },
        { "down_bow",             SymId::stringsDownBow            },
        { "harmonic",             SymId::stringsHarmonic            },
        { "snap_pizzicato",       SymId::pluckedSnapPizzicatoAbove },
        { "left_hand_pizzicato",  SymId::pluckedLeftHandPizzicato  },

        // --- Brass ---
        { "brass_mute_open",   SymId::brassMuteOpen   },
        { "brass_mute_closed", SymId::brassMuteClosed },

        // --- Guitar ---
        { "guitar_fade_in",      SymId::guitarFadeIn      },
        { "guitar_fade_out",     SymId::guitarFadeOut     },
        { "guitar_volume_swell", SymId::guitarVolumeSwell },
    };

    const SymId mapped = s_map.value(name, SymId::noSym);
    if (mapped != SymId::noSym) {
        return mapped;
    }
    return SymNames::symIdByName(name.toUtf8().constData());
}

bool ScoreApplicator::applyAddArticulation(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddArticulation: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const QString artName = op["articulation"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr) {
        LOGW() << "[editude] applyAddArticulation: no ChordRest at coordinate";
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
    return true;
}

bool ScoreApplicator::applyRemoveArticulation(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveArticulation: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const QString artName = op["articulation"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyRemoveArticulation: no Chord at coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    const SymId targetSym = articulationSymId(artName);

    // Scan the chord's articulations for one matching the target name/symbol.
    Articulation* target = nullptr;
    for (Articulation* a : chord->articulations()) {
        if (a->symId() == targetSym) {
            target = a;
            break;
        }
        // Also match by canonical name for fallback cases.
        if (articulationNameFromSymId(a->symId()) == artName) {
            target = a;
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveArticulation: articulation not found" << artName;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove articulation"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — dynamics (coordinate-addressed)
// ---------------------------------------------------------------------------

static DynamicType dynamicTypeFromName(const QString& name)
{
    static const QHash<QString, DynamicType> s_map = {
        { "pppppp", DynamicType::PPPPPP },
        { "ppppp",  DynamicType::PPPPP  },
        { "pppp",   DynamicType::PPPP   },
        { "ppp",    DynamicType::PPP    },
        { "pp",     DynamicType::PP     },
        { "p",      DynamicType::P      },
        { "mp",     DynamicType::MP     },
        { "mf",     DynamicType::MF     },
        { "f",      DynamicType::F      },
        { "ff",     DynamicType::FF     },
        { "fff",    DynamicType::FFF    },
        { "ffff",   DynamicType::FFFF   },
        { "fffff",  DynamicType::FFFFF  },
        { "ffffff", DynamicType::FFFFFF },
        { "fp",     DynamicType::FP     },
        { "pf",     DynamicType::PF     },
        { "sf",     DynamicType::SF     },
        { "sfz",    DynamicType::SFZ    },
        { "sff",    DynamicType::SFF    },
        { "sffz",   DynamicType::SFFZ   },
        { "sfff",   DynamicType::SFFF   },
        { "sfffz",  DynamicType::SFFFZ  },
        { "sfp",    DynamicType::SFP    },
        { "sfpp",   DynamicType::SFPP   },
        { "rfz",    DynamicType::RFZ    },
        { "rf",     DynamicType::RF     },
        { "fz",     DynamicType::FZ     },
    };
    return s_map.value(name, DynamicType::OTHER);
}

bool ScoreApplicator::applyAddDynamic(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddDynamic: unknown or missing part_id";
        return false;
    }

    const QString kind     = op["kind"].toString();
    const QJsonObject beat = op["beat"].toObject();

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

    const track_idx_t track = part->startTrack();
    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add dynamic"));
    Dynamic* dyn = Factory::createDynamic(seg);
    dyn->setParent(seg);
    dyn->setTrack(track);
    dyn->setDynamicType(dt);
    score->undoAddElement(dyn);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetDynamic(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetDynamic: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const QString kind     = op["kind"].toString();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    const DynamicType dt = dynamicTypeFromName(kind);
    if (dt == DynamicType::OTHER) {
        LOGW() << "[editude] applySetDynamic: unknown dynamic kind" << kind;
        return false;
    }

    // Find the Dynamic at this tick/track.
    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        // Try TimeTick segment.
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applySetDynamic: no segment at tick" << tick.toString();
        return false;
    }

    Dynamic* dyn = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isDynamic() && el->track() == track) {
            dyn = toDynamic(el);
            break;
        }
    }
    if (!dyn) {
        LOGW() << "[editude] applySetDynamic: no dynamic found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set dynamic"));
    dyn->undoChangeProperty(Pid::DYNAMIC_TYPE, PropertyValue(dt));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveDynamic(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveDynamic: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    // Find the Dynamic at this tick/track.
    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applyRemoveDynamic: no segment at tick" << tick.toString();
        return false;
    }

    Dynamic* dyn = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isDynamic() && el->track() == track) {
            dyn = toDynamic(el);
            break;
        }
    }
    if (!dyn) {
        LOGW() << "[editude] applyRemoveDynamic: no dynamic found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove dynamic"));
    score->undoRemoveElement(dyn);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — slurs (coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddSlur(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddSlur: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);
    const int endVoice   = op["end_voice"].toInt(1);
    const int endStaff   = op["end_staff"].toInt(0);

    ChordRest* startCR = findChordRestAtCoord(score, part, startTick, startVoice, startStaff);
    ChordRest* endCR   = findChordRestAtCoord(score, part, endTick, endVoice, endStaff);
    if (!startCR || !endCR) {
        LOGW() << "[editude] applyAddSlur: start or end ChordRest not found";
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
    return true;
}

bool ScoreApplicator::applyRemoveSlur(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveSlur: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);
    const track_idx_t startTrack = trackFromCoord(part, startVoice, startStaff);

    // Scan spanner map for a Slur matching the start/end ticks and track.
    Slur* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isSlur() && sp->track() == startTrack
            && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toSlur(sp);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveSlur: slur not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove slur"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — hairpins (coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddHairpin(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddHairpin: unknown or missing part_id";
        return false;
    }

    const QString kind   = op["kind"].toString();
    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();

    const HairpinType hpType = (kind == QStringLiteral("crescendo"))
                               ? HairpinType::CRESC_HAIRPIN
                               : HairpinType::DIM_HAIRPIN;

    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add hairpin"));
    score->addHairpin(hpType, startTick, endTick, track);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveHairpin(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveHairpin: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    // Scan spanner map for a Hairpin matching the range and track.
    Hairpin* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isHairpin() && sp->track() == track
            && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toHairpin(sp);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveHairpin: hairpin not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove hairpin"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — tuplets (coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddTuplet(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddTuplet: unknown or missing part_id";
        return false;
    }

    const int actualNotes     = op["actual_notes"].toInt(0);
    const int normalNotes     = op["normal_notes"].toInt(0);
    const QJsonObject beat    = op["beat"].toObject();
    const QJsonObject baseDur = op["base_duration"].toObject();

    if (actualNotes <= 0 || normalNotes <= 0) {
        LOGW() << "[editude] applyAddTuplet: invalid payload";
        return false;
    }

    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const track_idx_t track = trackFromCoord(part, voice, stf);

    const QByteArray baseTypeUtf8 = baseDur["type"].toString().toUtf8();
    const DurationType baseType = TConv::fromXml(
        muse::AsciiStringView(baseTypeUtf8.constData()), DurationType::V_INVALID);
    if (baseType == DurationType::V_INVALID) {
        LOGW() << "[editude] applyAddTuplet: unknown base duration"
               << baseDur["type"].toString();
        return false;
    }

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
    return true;
}

bool ScoreApplicator::applyRemoveTuplet(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveTuplet: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const track_idx_t track = trackFromCoord(part, voice, stf);

    // Find a tuplet at this tick on the given track.
    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyRemoveTuplet: no measure at tick" << tick.toString();
        return false;
    }

    Tuplet* tuplet = nullptr;
    for (Segment* seg = measure->first(SegmentType::ChordRest); seg;
         seg = seg->next(SegmentType::ChordRest)) {
        if (seg->tick() == tick) {
            EngravingItem* el = seg->element(track);
            if (el && el->isChordRest()) {
                ChordRest* cr = toChordRest(el);
                if (cr->tuplet()) {
                    tuplet = cr->tuplet();
                    break;
                }
            }
        }
    }
    if (!tuplet) {
        LOGW() << "[editude] applyRemoveTuplet: no tuplet at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove tuplet"));
    score->cmdDeleteTuplet(tuplet, /*replaceWithRest=*/true);

    // Replace the rests left by cmdDeleteTuplet with a single V_MEASURE
    // rest.  This ensures the peer shows events=[] (the score reader skips
    // V_MEASURE rests), matching the editor's state after an undo.
    // When the translator also emits an explicit InsertRest op (the
    // remove_tuplet path), that op overwrites the V_MEASURE rest with the
    // correct specific-duration rest.  Without this cleanup the peer would
    // retain cmdDeleteTuplet's divergent rest pattern.
    {
        bool allRests = true;
        QVector<EngravingItem*> toRemove;
        for (Segment* s = measure->first(SegmentType::ChordRest); s;
             s = s->next(SegmentType::ChordRest)) {
            EngravingItem* e = s->element(track);
            if (e && !e->isRest()) {
                allRests = false;
                break;
            }
            if (e) {
                toRemove.append(e);
            }
        }
        if (allRests && !toRemove.isEmpty()) {
            for (EngravingItem* e : toRemove) {
                score->undoRemoveElement(e);
            }
            Segment* seg0 = measure->getSegmentR(
                SegmentType::ChordRest, Fraction(0, 1));
            Rest* fmr = Factory::createRest(
                seg0, TDuration(DurationType::V_MEASURE));
            fmr->setTrack(track);
            fmr->setTicks(measure->ticks());
            score->undoAddElement(fmr);
        }
    }

    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — lyrics (coordinate-addressed)
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
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddLyric: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice    = opVoice(op);
    const int stf      = opStaff(op);
    const int verse    = op["verse"].toInt(0);
    const QString syllabic = op["syllabic"].toString(QStringLiteral("single"));
    const QString text = op["text"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr) {
        LOGW() << "[editude] applyAddLyric: no ChordRest at coordinate";
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
    return true;
}

bool ScoreApplicator::applySetLyric(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetLyric: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice    = opVoice(op);
    const int stf      = opStaff(op);
    const int verse    = op["verse"].toInt(0);
    const QString text     = op["text"].toString();
    const QString syllabic = op["syllabic"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr) {
        LOGW() << "[editude] applySetLyric: no ChordRest at coordinate";
        return false;
    }

    // Find the Lyrics element matching the verse number.
    Lyrics* target = nullptr;
    for (Lyrics* l : cr->lyrics()) {
        if (l->verse() == verse) {
            target = l;
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applySetLyric: lyric not found at coordinate verse" << verse;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set lyric"));
    target->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    if (!syllabic.isEmpty()) {
        target->undoChangeProperty(
            Pid::SYLLABIC,
            PropertyValue(static_cast<int>(lyricSyllabicFromName(syllabic))));
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveLyric(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveLyric: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const int verse = op["verse"].toInt(0);

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr) {
        LOGW() << "[editude] applyRemoveLyric: no ChordRest at coordinate";
        return false;
    }

    Lyrics* target = nullptr;
    for (Lyrics* l : cr->lyrics()) {
        if (l->verse() == verse) {
            target = l;
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveLyric: lyric not found at coordinate verse" << verse;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove lyric"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — chord symbols (score-global, beat-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddChordSymbol(Score* score, const QJsonObject& op)
{
    const QString name     = op["name"].toString();
    const QJsonObject beat = op["beat"].toObject();

    if (name.isEmpty()) {
        LOGW() << "[editude] applyAddChordSymbol: missing name";
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
    return true;
}

bool ScoreApplicator::applySetChordSymbol(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const QString name     = op["name"].toString();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    // Find the Harmony at this tick on track 0.
    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applySetChordSymbol: no segment at tick" << tick.toString();
        return false;
    }

    Harmony* harmony = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isHarmony() && el->track() == 0) {
            harmony = toHarmony(el);
            break;
        }
    }
    if (!harmony) {
        LOGW() << "[editude] applySetChordSymbol: harmony not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set chord symbol"));
    harmony->setHarmony(String(name));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveChordSymbol(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applyRemoveChordSymbol: no segment at tick" << tick.toString();
        return false;
    }

    Harmony* harmony = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isHarmony() && el->track() == 0) {
            harmony = toHarmony(el);
            break;
        }
    }
    if (!harmony) {
        LOGW() << "[editude] applyRemoveChordSymbol: harmony not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove chord symbol"));
    score->undoRemoveElement(harmony);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — staff text (part-scoped, beat-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddStaffText(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddStaffText: unknown or missing part_id";
        return false;
    }

    const QString text     = op["text"].toString();
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddStaffText: no measure at tick" << tick.toString();
        return false;
    }

    const track_idx_t track = part->startTrack();
    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add staff text"));
    StaffText* st = Factory::createStaffText(seg, TextStyleType::STAFF);
    st->setParent(seg);
    st->setTrack(track);
    st->setPlainText(String(text));
    score->undoAddElement(st);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetStaffText(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applySetStaffText: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const QString text     = op["text"].toString();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applySetStaffText: no segment at tick" << tick.toString();
        return false;
    }

    StaffText* st = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isStaffText() && el->track() == track) {
            st = toStaffText(el);
            break;
        }
    }
    if (!st) {
        LOGW() << "[editude] applySetStaffText: staff text not found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set staff text"));
    st->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveStaffText(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveStaffText: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applyRemoveStaffText: no segment at tick" << tick.toString();
        return false;
    }

    StaffText* st = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isStaffText() && el->track() == track) {
            st = toStaffText(el);
            break;
        }
    }
    if (!st) {
        LOGW() << "[editude] applyRemoveStaffText: staff text not found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove staff text"));
    score->undoRemoveElement(st);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — system text (score-global, beat-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddSystemText(Score* score, const QJsonObject& op)
{
    const QString text     = op["text"].toString();
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddSystemText: no measure at tick" << tick.toString();
        return false;
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add system text"));
    SystemText* st = Factory::createSystemText(seg, TextStyleType::SYSTEM);
    st->setParent(seg);
    st->setTrack(0);
    st->setPlainText(String(text));
    score->undoAddElement(st);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetSystemText(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const QString text     = op["text"].toString();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applySetSystemText: no segment at tick" << tick.toString();
        return false;
    }

    SystemText* st = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isSystemText() && el->track() == 0) {
            st = toSystemText(el);
            break;
        }
    }
    if (!st) {
        LOGW() << "[editude] applySetSystemText: system text not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set system text"));
    st->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveSystemText(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applyRemoveSystemText: no segment at tick" << tick.toString();
        return false;
    }

    SystemText* st = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isSystemText() && el->track() == 0) {
            st = toSystemText(el);
            break;
        }
    }
    if (!st) {
        LOGW() << "[editude] applyRemoveSystemText: system text not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove system text"));
    score->undoRemoveElement(st);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 3 — rehearsal marks (score-global, beat-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddRehearsalMark(Score* score, const QJsonObject& op)
{
    const QString text     = op["text"].toString();
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddRehearsalMark: no measure at tick" << tick.toString();
        return false;
    }

    Segment* seg = measure->undoGetChordRestOrTimeTickSegment(tick);

    score->startCmd(TranslatableString("undoableAction", "Add rehearsal mark"));
    RehearsalMark* rm = Factory::createRehearsalMark(seg);
    rm->setParent(seg);
    rm->setTrack(0);
    rm->setPlainText(String(text));
    score->undoAddElement(rm);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetRehearsalMark(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const QString text     = op["text"].toString();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applySetRehearsalMark: no segment at tick" << tick.toString();
        return false;
    }

    RehearsalMark* rm = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isRehearsalMark() && el->track() == 0) {
            rm = toRehearsalMark(el);
            break;
        }
    }
    if (!rm) {
        LOGW() << "[editude] applySetRehearsalMark: rehearsal mark not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Set rehearsal mark"));
    rm->undoChangeProperty(Pid::TEXT, PropertyValue(String(text)));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveRehearsalMark(Score* score, const QJsonObject& op)
{
    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Segment* seg = score->tick2segment(tick, false, SegmentType::ChordRest);
    if (!seg) {
        Measure* m = score->tick2measure(tick);
        if (m) {
            seg = m->undoGetChordRestOrTimeTickSegment(tick);
        }
    }
    if (!seg) {
        LOGW() << "[editude] applyRemoveRehearsalMark: no segment at tick" << tick.toString();
        return false;
    }

    RehearsalMark* rm = nullptr;
    for (EngravingItem* el : seg->annotations()) {
        if (el->isRehearsalMark() && el->track() == 0) {
            rm = toRehearsalMark(el);
            break;
        }
    }
    if (!rm) {
        LOGW() << "[editude] applyRemoveRehearsalMark: rehearsal mark not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove rehearsal mark"));
    score->undoRemoveElement(rm);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Advanced spanners — octave lines (beat-range + part-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddOctaveLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddOctaveLine: unknown or missing part_id";
        return false;
    }

    const QString kind   = op["kind"].toString();
    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();

    OttavaType ottType = OttavaType::OTTAVA_8VA;
    if (kind == QStringLiteral("8vb"))        ottType = OttavaType::OTTAVA_8VB;
    else if (kind == QStringLiteral("15ma"))  ottType = OttavaType::OTTAVA_15MA;
    else if (kind == QStringLiteral("15mb"))  ottType = OttavaType::OTTAVA_15MB;

    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add octave line"));
    Ottava* ottava = Factory::createOttava(score->dummy());
    ottava->setOttavaType(ottType);
    ottava->setTrack(track);
    ottava->setTick(startTick);
    ottava->setTick2(endTick);
    score->undoAddElement(ottava);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveOctaveLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveOctaveLine: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Ottava* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isOttava() && sp->track() == track
            && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toOttava(sp);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveOctaveLine: ottava not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove octave line"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Advanced spanners — glissandos (dual-anchor, coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddGlissando(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddGlissando: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const QJsonObject sp = op["start_pitch"].toObject();
    const QJsonObject ep = op["end_pitch"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const int startMidi = pitchToMidi(sp["step"].toString(), sp["octave"].toInt(),
                                      sp["accidental"].toString());
    const int endMidi   = pitchToMidi(ep["step"].toString(), ep["octave"].toInt(),
                                      ep["accidental"].toString());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);
    const int endVoice   = op["end_voice"].toInt(1);
    const int endStaff   = op["end_staff"].toInt(0);
    const QString style  = op["style"].toString();

    Note* startNote = findNoteAtCoord(score, part, startTick, startMidi, startVoice, startStaff);
    Note* endNote   = findNoteAtCoord(score, part, endTick, endMidi, endVoice, endStaff);
    if (!startNote || !endNote) {
        LOGW() << "[editude] applyAddGlissando: start or end note not found";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Add glissando"));
    Glissando* gliss = Factory::createGlissando(score->dummy());
    gliss->setGlissandoType(style == QStringLiteral("wavy")
                            ? GlissandoType::WAVY
                            : GlissandoType::STRAIGHT);
    gliss->setAnchor(Spanner::Anchor::NOTE);
    gliss->setTrack(startNote->track());
    gliss->setTrack2(endNote->track());
    gliss->setTick(startNote->tick());
    gliss->setTick2(endNote->tick());
    gliss->setStartElement(startNote);
    gliss->setEndElement(endNote);
    gliss->setParent(startNote);
    score->undoAddElement(gliss);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveGlissando(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveGlissando: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject sp = op["start_pitch"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const int startMidi = pitchToMidi(sp["step"].toString(), sp["octave"].toInt(),
                                      sp["accidental"].toString());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);

    Note* startNote = findNoteAtCoord(score, part, startTick, startMidi, startVoice, startStaff);
    if (!startNote) {
        LOGW() << "[editude] applyRemoveGlissando: start note not found";
        return false;
    }

    // Find glissando starting from this note.
    Glissando* target = nullptr;
    for (Spanner* sp2 : startNote->spannerFor()) {
        if (sp2->isGlissando()) {
            target = toGlissando(sp2);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveGlissando: glissando not found on start note";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove glissando"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Advanced spanners — pedal lines (beat-range + part-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddPedalLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddPedalLine: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add pedal line"));
    Pedal* pedal = Factory::createPedal(score->dummy());
    pedal->setTrack(track);
    pedal->setTick(startTick);
    pedal->setTick2(endTick);
    score->undoAddElement(pedal);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemovePedalLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemovePedalLine: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Pedal* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isPedal() && sp->track() == track
            && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toPedal(sp);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemovePedalLine: pedal not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove pedal line"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Advanced spanners — trill lines (beat-range + part-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddTrillLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddTrillLine: unknown or missing part_id";
        return false;
    }

    const QString kind   = op["kind"].toString();
    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();

    TrillType trType = TrillType::TRILL_LINE;
    if (kind == QStringLiteral("upprall"))         trType = TrillType::UPPRALL_LINE;
    else if (kind == QStringLiteral("downprall"))  trType = TrillType::DOWNPRALL_LINE;
    else if (kind == QStringLiteral("prallprall")) trType = TrillType::PRALLPRALL_LINE;

    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add trill line"));
    Trill* trill = Factory::createTrill(score->dummy());
    trill->setTrillType(trType);
    trill->setTrack(track);
    trill->setTick(startTick);
    trill->setTick2(endTick);

    const QJsonValue accVal = op.value("accidental");
    if (accVal.isString()) {
        const QString accStr = accVal.toString();
        AccidentalType accType = AccidentalType::NONE;
        if (accStr == QStringLiteral("flat"))              accType = AccidentalType::FLAT;
        else if (accStr == QStringLiteral("sharp"))        accType = AccidentalType::SHARP;
        else if (accStr == QStringLiteral("natural"))      accType = AccidentalType::NATURAL;
        else if (accStr == QStringLiteral("double-flat"))  accType = AccidentalType::FLAT2;
        else if (accStr == QStringLiteral("double-sharp")) accType = AccidentalType::SHARP2;
        if (accType != AccidentalType::NONE && trill->ornament()) {
            Accidental* acc = Factory::createAccidental(trill->ornament());
            acc->setAccidentalType(accType);
            trill->ornament()->setAccidentalAbove(acc);
        }
    }

    score->undoAddElement(trill);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveTrillLine(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveTrillLine: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Trill* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isTrill() && sp->track() == track
            && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toTrill(sp);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveTrillLine: trill not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove trill line"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Arpeggios (beat/voice/staff coordinate-addressed)
// ---------------------------------------------------------------------------

static ArpeggioType arpeggioTypeFromName(const QString& name)
{
    static const QHash<QString, ArpeggioType> s_map = {
        { QStringLiteral("normal"),        ArpeggioType::NORMAL },
        { QStringLiteral("up"),            ArpeggioType::UP },
        { QStringLiteral("down"),          ArpeggioType::DOWN },
        { QStringLiteral("bracket"),       ArpeggioType::BRACKET },
        { QStringLiteral("up_straight"),   ArpeggioType::UP_STRAIGHT },
        { QStringLiteral("down_straight"), ArpeggioType::DOWN_STRAIGHT },
    };
    return s_map.value(name, ArpeggioType::NORMAL);
}

bool ScoreApplicator::applyAddArpeggio(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddArpeggio: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice    = opVoice(op);
    const int stf      = opStaff(op);
    const QString dirName = op["direction"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyAddArpeggio: no chord at coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    score->startCmd(TranslatableString("undoableAction", "Add arpeggio"));
    Arpeggio* arp = Factory::createArpeggio(chord);
    arp->setArpeggioType(arpeggioTypeFromName(dirName));
    arp->setParent(chord);
    arp->setTrack(chord->track());
    score->undoAddElement(arp);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveArpeggio(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveArpeggio: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyRemoveArpeggio: no chord at coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    Arpeggio* arp = chord->arpeggio();
    if (!arp) {
        LOGW() << "[editude] applyRemoveArpeggio: chord has no arpeggio";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove arpeggio"));
    score->undoRemoveElement(arp);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Grace notes (coordinate-addressed)
// ---------------------------------------------------------------------------

static NoteType graceNoteTypeFromName(const QString& name)
{
    static const QHash<QString, NoteType> s_map = {
        { QStringLiteral("acciaccatura"),  NoteType::ACCIACCATURA },
        { QStringLiteral("appoggiatura"),  NoteType::APPOGGIATURA },
        { QStringLiteral("grace4"),        NoteType::GRACE4 },
        { QStringLiteral("grace16"),       NoteType::GRACE16 },
        { QStringLiteral("grace32"),       NoteType::GRACE32 },
        { QStringLiteral("grace8_after"),  NoteType::GRACE8_AFTER },
        { QStringLiteral("grace16_after"), NoteType::GRACE16_AFTER },
        { QStringLiteral("grace32_after"), NoteType::GRACE32_AFTER },
    };
    return s_map.value(name, NoteType::ACCIACCATURA);
}

static DurationType graceNoteDurationType(NoteType nt)
{
    switch (nt) {
    case NoteType::GRACE4:        return DurationType::V_QUARTER;
    case NoteType::GRACE16:
    case NoteType::GRACE16_AFTER: return DurationType::V_16TH;
    case NoteType::GRACE32:
    case NoteType::GRACE32_AFTER: return DurationType::V_32ND;
    default:                      return DurationType::V_EIGHTH;
    }
}

bool ScoreApplicator::applyAddGraceNote(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddGraceNote: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat  = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice         = opVoice(op);
    const int stf           = opStaff(op);
    const int order         = op["order"].toInt(0);
    const QString typeName  = op["grace_type"].toString();
    const QJsonObject pitch = op["pitch"].toObject();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyAddGraceNote: no chord at coordinate";
        return false;
    }
    Chord* parentChord = toChord(cr);

    const NoteType nt = graceNoteTypeFromName(typeName);
    const int midi = pitchToMidi(pitch["step"].toString(),
                                 pitch["octave"].toInt(),
                                 pitch["accidental"].toString());
    if (midi < 0 || midi > 127) {
        LOGW() << "[editude] applyAddGraceNote: invalid pitch";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Add grace note"));

    Chord* graceChord = Factory::createChord(parentChord->segment());
    graceChord->setNoteType(nt);
    graceChord->setGraceIndex(static_cast<size_t>(order));
    graceChord->setTrack(parentChord->track());
    graceChord->setParent(parentChord);

    TDuration dur(graceNoteDurationType(nt));
    graceChord->setDurationType(dur);
    graceChord->setTicks(dur.ticks());

    Note* note = Factory::createNote(graceChord);
    note->setPitch(midi);
    note->setTpc1(note->tpc1default(midi));
    note->setTpc2(note->tpc2default(midi));
    note->setTrack(parentChord->track());
    graceChord->add(note);

    score->undoAddElement(graceChord);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveGraceNote(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveGraceNote: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat  = op["beat"].toObject();
    const QJsonObject pitch = op["pitch"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);
    const int order = op["order"].toInt(0);
    const int midi  = pitchToMidi(pitch["step"].toString(),
                                  pitch["octave"].toInt(),
                                  pitch["accidental"].toString());

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyRemoveGraceNote: no chord at coordinate";
        return false;
    }
    Chord* parentChord = toChord(cr);

    // Find the grace chord by order and pitch.
    Chord* target = nullptr;
    int idx = 0;
    for (Chord* gc : parentChord->graceNotes()) {
        if (idx == order) {
            // Verify pitch if provided.
            if (midi >= 0 && midi <= 127) {
                bool pitchMatch = false;
                for (Note* n : gc->notes()) {
                    if (n->pitch() == midi) {
                        pitchMatch = true;
                        break;
                    }
                }
                if (!pitchMatch) {
                    ++idx;
                    continue;
                }
            }
            target = gc;
            break;
        }
        ++idx;
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveGraceNote: grace note not found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove grace note"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Breath marks / caesuras (beat + part-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddBreathMark(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddBreathMark: unknown or missing part_id";
        return false;
    }

    const QString typeName   = op["breath_type"].toString();
    const QJsonObject beat   = op["beat"].toObject();
    const double pause       = op["pause"].toDouble(0.0);
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyAddBreathMark: no measure at tick" << tick.toString();
        return false;
    }

    const track_idx_t track = part->startTrack();

    score->startCmd(TranslatableString("undoableAction", "Add breath mark"));
    Segment* seg = measure->undoGetSegment(SegmentType::Breath, tick);
    Breath* breath = Factory::createBreath(seg);
    breath->setParent(seg);
    breath->setTrack(track);
    breath->setSymId(breathTypeFromString(typeName));
    breath->setPause(pause);
    score->undoAddElement(breath);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveBreathMark(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveBreathMark: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const track_idx_t track = part->startTrack();

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyRemoveBreathMark: no measure at tick" << tick.toString();
        return false;
    }

    Segment* seg = measure->findSegment(SegmentType::Breath, tick);
    if (!seg) {
        LOGW() << "[editude] applyRemoveBreathMark: no breath segment at tick";
        return false;
    }

    Breath* breath = nullptr;
    EngravingItem* el = seg->element(track);
    if (el && el->isBreath()) {
        breath = toBreath(el);
    }
    if (!breath) {
        LOGW() << "[editude] applyRemoveBreathMark: breath not found at coordinate";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove breath mark"));
    score->undoRemoveElement(breath);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Single-note tremolos (beat/voice/staff coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddTremolo(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddTremolo: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice    = opVoice(op);
    const int stf      = opStaff(op);
    const QString typeName = op["tremolo_type"].toString();

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyAddTremolo: no chord at coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    score->startCmd(TranslatableString("undoableAction", "Add tremolo"));
    TremoloSingleChord* trem = Factory::createTremoloSingleChord(chord);
    trem->setTremoloType(tremoloTypeFromString(typeName));
    trem->setParent(chord);
    trem->setTrack(chord->track());
    score->undoAddElement(trem);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveTremolo(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveTremolo: unknown or missing part_id";
        return false;
    }

    const QJsonObject beat = op["beat"].toObject();
    const Fraction tick(beat["numerator"].toInt(), beat["denominator"].toInt());
    const int voice = opVoice(op);
    const int stf   = opStaff(op);

    ChordRest* cr = findChordRestAtCoord(score, part, tick, voice, stf);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyRemoveTremolo: no chord at coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    TremoloSingleChord* trem = chord->tremoloSingleChord();
    if (!trem) {
        LOGW() << "[editude] applyRemoveTremolo: chord has no single tremolo";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove tremolo"));
    score->undoRemoveElement(trem);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Two-note tremolos (dual-anchor, coordinate-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applyAddTwoNoteTremolo(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyAddTwoNoteTremolo: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const QJsonObject eb = op["end_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const Fraction endTick(eb["numerator"].toInt(), eb["denominator"].toInt());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);
    const int endVoice   = op["end_voice"].toInt(1);
    const int endStaff   = op["end_staff"].toInt(0);
    const QString typeName = op["tremolo_type"].toString();

    ChordRest* cr1 = findChordRestAtCoord(score, part, startTick, startVoice, startStaff);
    ChordRest* cr2 = findChordRestAtCoord(score, part, endTick, endVoice, endStaff);
    if (!cr1 || !cr1->isChord() || !cr2 || !cr2->isChord()) {
        LOGW() << "[editude] applyAddTwoNoteTremolo: start or end chord not found";
        return false;
    }
    Chord* chord1 = toChord(cr1);
    Chord* chord2 = toChord(cr2);

    score->startCmd(TranslatableString("undoableAction", "Add two-note tremolo"));
    TremoloTwoChord* trem = Factory::createTremoloTwoChord(chord1);
    trem->setTremoloType(tremoloTypeFromString(typeName));
    trem->setChords(chord1, chord2);
    trem->setParent(chord1);
    trem->setTrack(chord1->track());
    score->undoAddElement(trem);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyRemoveTwoNoteTremolo(Score* score, const QJsonObject& op)
{
    Part* part = resolvePart(op);
    if (!part) {
        LOGW() << "[editude] applyRemoveTwoNoteTremolo: unknown or missing part_id";
        return false;
    }

    const QJsonObject sb = op["start_beat"].toObject();
    const Fraction startTick(sb["numerator"].toInt(), sb["denominator"].toInt());
    const int startVoice = op["start_voice"].toInt(1);
    const int startStaff = op["start_staff"].toInt(0);

    ChordRest* cr = findChordRestAtCoord(score, part, startTick, startVoice, startStaff);
    if (!cr || !cr->isChord()) {
        LOGW() << "[editude] applyRemoveTwoNoteTremolo: no chord at start coordinate";
        return false;
    }
    Chord* chord = toChord(cr);

    TremoloTwoChord* trem = chord->tremoloTwoChord();
    if (!trem) {
        LOGW() << "[editude] applyRemoveTwoNoteTremolo: chord has no two-note tremolo";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove two-note tremolo"));
    score->undoRemoveElement(trem);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Tier 4 — navigation marks (already position-addressed)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applySetStartRepeat(Score* score, const QJsonObject& op)
{
    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetStartRepeat: no measure at tick" << tick.toString();
        return false;
    }

    const bool enabled = op["enabled"].toBool(true);
    score->startCmd(TranslatableString("undoableAction", "Set start repeat"));
    measure->undoChangeProperty(Pid::REPEAT_START, enabled);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetEndRepeat(Score* score, const QJsonObject& op)
{
    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetEndRepeat: no measure at tick" << tick.toString();
        return false;
    }

    const bool enabled = op["enabled"].toBool(true);
    const int count = op["count"].toInt(2);

    score->startCmd(TranslatableString("undoableAction", "Set end repeat"));
    measure->undoChangeProperty(Pid::REPEAT_END, enabled);
    if (enabled) {
        measure->undoChangeProperty(Pid::REPEAT_COUNT, count);
    }
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertVolta(Score* score, const QJsonObject& op)
{
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
    return true;
}

bool ScoreApplicator::applyRemoveVolta(Score* score, const QJsonObject& op)
{
    const QJsonObject startBeatObj = op["start_beat"].toObject();
    const QJsonObject endBeatObj   = op["end_beat"].toObject();
    const Fraction startTick(startBeatObj["numerator"].toInt(),
                             startBeatObj["denominator"].toInt());
    const Fraction endTick(endBeatObj["numerator"].toInt(),
                           endBeatObj["denominator"].toInt());

    Volta* target = nullptr;
    for (auto it = score->spanner().lower_bound(startTick.ticks());
         it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
        Spanner* sp = it->second;
        if (sp->isVolta() && sp->tick() == startTick && sp->tick2() == endTick) {
            target = toVolta(sp);
            break;
        }
    }
    // Also try matching by measure boundaries (volta tick2 = endMeasure.endTick).
    if (!target) {
        Measure* endMeasure = score->tick2measure(endTick);
        if (endMeasure) {
            const Fraction endMeasureTick2 = endMeasure->endTick();
            for (auto it = score->spanner().lower_bound(startTick.ticks());
                 it != score->spanner().end() && it->first == startTick.ticks(); ++it) {
                Spanner* sp = it->second;
                if (sp->isVolta() && sp->tick() == startTick
                    && sp->tick2() == endMeasureTick2) {
                    target = toVolta(sp);
                    break;
                }
            }
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveVolta: volta not found at coordinates";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove volta"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertMarker(Score* score, const QJsonObject& op)
{
    const QString kind = op["kind"].toString();
    if (kind.isEmpty()) {
        LOGW() << "[editude] applyInsertMarker: missing kind";
        return false;
    }

    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyInsertMarker: no measure at tick" << tick.toString();
        return false;
    }

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
    return true;
}

bool ScoreApplicator::applyRemoveMarker(Score* score, const QJsonObject& op)
{
    const QString kind = op["kind"].toString();
    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyRemoveMarker: no measure at tick" << tick.toString();
        return false;
    }

    // Find the marker by kind on this measure.
    static const QHash<QString, MarkerType> s_markerKindMap = {
        { "segno",     MarkerType::SEGNO   },
        { "coda",      MarkerType::CODA    },
        { "fine",      MarkerType::FINE    },
        { "to_coda",   MarkerType::TOCODA  },
        { "segno_var", MarkerType::VARSEGNO },
    };
    MarkerType mt = s_markerKindMap.value(kind, MarkerType::SEGNO);

    Marker* target = nullptr;
    for (EngravingItem* el : measure->el()) {
        if (el->isMarker()) {
            Marker* m = toMarker(el);
            if (m->markerType() == mt) {
                target = m;
                break;
            }
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveMarker: marker not found" << kind;
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove marker"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

bool ScoreApplicator::applyInsertJump(Score* score, const QJsonObject& op)
{
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
    return true;
}

bool ScoreApplicator::applyRemoveJump(Score* score, const QJsonObject& op)
{
    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applyRemoveJump: no measure at tick" << tick.toString();
        return false;
    }

    // Find the jump on this measure.
    Jump* target = nullptr;
    for (EngravingItem* el : measure->el()) {
        if (el->isJump()) {
            target = toJump(el);
            break;
        }
    }
    if (!target) {
        LOGW() << "[editude] applyRemoveJump: jump not found at beat";
        return false;
    }

    score->startCmd(TranslatableString("undoableAction", "Remove jump"));
    score->undoRemoveElement(target);
    score->endCmd();
    return true;
}

// ---------------------------------------------------------------------------
// Structural ops (already position-based — no UUID involvement)
// ---------------------------------------------------------------------------

bool ScoreApplicator::applySetScoreMetadata(Score* score, const QJsonObject& op)
{
    const QString field = op["field"].toString();
    const QString value = op["value"].toString();

    if (field.isEmpty()) {
        LOGW() << "[editude] applySetScoreMetadata: missing field";
        return false;
    }

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
    score->undo(new ChangeMetaText(score, String(tag), String(value)));
    score->endCmd();
    return true;
}

bool ScoreApplicator::applySetMeasureLen(Score* score, const QJsonObject& op)
{
    const QJsonObject beatObj = op["beat"].toObject();
    const Fraction tick(beatObj["numerator"].toInt(), beatObj["denominator"].toInt());

    Measure* measure = score->tick2measure(tick);
    if (!measure) {
        LOGW() << "[editude] applySetMeasureLen: no measure at tick" << tick.toString();
        return false;
    }

    if (op["actual_len"].isNull() || op["actual_len"].isUndefined()) {
        const Fraction fullLen = measure->timesig();
        if (measure->ticks() == fullLen) {
            LOGD() << "[editude] applySetMeasureLen: measure already at full length";
            return true;
        }
        score->startCmd(TranslatableString("undoableAction", "Restore measure length"));
        measure->adjustToLen(fullLen, /*appendRestsIfNecessary=*/true);
        score->endCmd();
    } else {
        const QJsonObject lenObj = op["actual_len"].toObject();
        const Fraction newLen(lenObj["numerator"].toInt(), lenObj["denominator"].toInt());
        if (newLen <= Fraction(0, 1)) {
            LOGW() << "[editude] applySetMeasureLen: non-positive actual_len";
            return false;
        }
        if (measure->ticks() == newLen) {
            LOGD() << "[editude] applySetMeasureLen: measure already at requested length";
            return true;
        }
        score->startCmd(TranslatableString("undoableAction", "Set measure length"));
        measure->adjustToLen(newLen, /*appendRestsIfNecessary=*/true);
        score->endCmd();
    }
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

    score->startCmd(TranslatableString("undoableAction", "Insert beats"));

    Fraction remaining = duration;
    while (remaining > Fraction(0, 1)) {
        Measure* m = score->tick2measure(atTick);
        if (!m) {
            break;
        }
        const Fraction measureLen = m->ticks();

        Score::InsertMeasureOptions opts;
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

    score->startCmd(TranslatableString("undoableAction", "Delete beats"));
    score->deleteMeasures(firstMeasure, lastMeasure);
    score->endCmd();
    return true;
}
