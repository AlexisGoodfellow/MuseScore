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

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/timesig.h"
#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

// ---------------------------------------------------------------------------
// Private helper: UUID lookup (local map first, then remote map).
// ---------------------------------------------------------------------------
QString OperationTranslator::uuidForElement(
    EngravingObject* obj,
    const QHash<EngravingObject*, QString>& remoteElementToUuid) const
{
    const QString local = m_localElementToUuid.value(obj);
    if (!local.isEmpty()) {
        return local;
    }
    return remoteElementToUuid.value(obj);
}

// ---------------------------------------------------------------------------
// translateAll — main dispatch
// ---------------------------------------------------------------------------
QVector<QJsonObject> OperationTranslator::translateAll(
    const std::map<EngravingObject*, std::unordered_set<CommandType>>& changedObjects,
    const PropertyIdSet& changedPropertyIdSet,
    const QString& partId,
    const QHash<EngravingObject*, QString>& remoteElementToUuid)
{
    QVector<QJsonObject> ops;

    // ── Pass 1: Group newly-added Notes by parent Chord ───────────────────
    //
    // A Note with AddElement whose parent Chord also has AddElement is part of
    // a new chord insertion (InsertNote / InsertChord).
    // A Note with AddElement whose parent Chord does NOT have AddElement is a
    // pitch added to an existing chord (AddChordNote).
    QHash<Chord*, QVector<Note*>> newChordNotes;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() != ElementType::NOTE) {
            continue;
        }
        Note* note   = static_cast<Note*>(obj);
        Chord* chord = note->chord();
        if (!chord) {
            continue;
        }
        auto chordIt = changedObjects.find(chord);
        if (chordIt != changedObjects.end() && chordIt->second.count(CommandType::AddElement)) {
            newChordNotes[chord].append(note);
        }
    }

    // ── Pass 2: InsertNote / InsertChord ──────────────────────────────────
    QSet<EngravingObject*> handledNotes;
    for (const auto& [chord, notes] : newChordNotes.asKeyValueRange()) {
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (notes.size() == 1) {
            // Single-note chord: UUID lives on the Note* (mirrors applyInsertNote).
            Note* n = notes[0];
            m_localElementToUuid[n]    = uuid;
            m_localUuidToElement[uuid] = n;
            ops.append(buildInsertNote(n, uuid, partId));
        } else {
            // Multi-note chord: UUID lives on the Chord* (mirrors applyInsertChord).
            m_localElementToUuid[chord]  = uuid;
            m_localUuidToElement[uuid]   = chord;
            ops.append(buildInsertChord(chord, notes, uuid, partId));
        }
        for (Note* n : notes) {
            handledNotes.insert(n);
        }
    }

    // ── Pass 3: InsertRest ────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() != ElementType::REST) {
            continue;
        }
        Rest* rest             = static_cast<Rest*>(obj);
        const QString uuid     = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_localElementToUuid[rest]   = uuid;
        m_localUuidToElement[uuid]   = rest;
        ops.append(buildInsertRest(rest, uuid, partId));
    }

    // ── Pass 4: AddChordNote (notes added to existing chords) ────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() != ElementType::NOTE) {
            continue;
        }
        if (handledNotes.contains(const_cast<EngravingObject*>(obj))) {
            continue; // already emitted as part of InsertNote/InsertChord
        }
        Note* note   = static_cast<Note*>(obj);
        Chord* chord = note->chord();
        if (!chord) {
            continue;
        }
        const QString chordUuid = uuidForElement(chord, remoteElementToUuid);
        if (chordUuid.isEmpty()) {
            LOGD() << "[editude] translateAll: AddChordNote: parent chord UUID unknown, skipping";
            continue;
        }
        ops.append(buildAddChordNote(chordUuid, note));
    }

    // ── Pass 5: SetTimeSignature & SetTempo ───────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() == ElementType::TIMESIG) {
            auto* ts = toTimeSig(static_cast<EngravingItem*>(obj));
            if (ts) {
                ops.append(buildSetTimeSignature(ts));
            }
        }
        if (obj->type() == ElementType::TEMPO_TEXT) {
            ops.append(buildSetTempo(static_cast<TempoText*>(obj)));
        }
    }

    // ── Pass 6: RemoveElement ─────────────────────────────────────────────
    // Identify which chords/rests are being fully removed in this transaction.
    QSet<EngravingObject*> removedChordsAndRests;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::RemoveElement)) {
            continue;
        }
        if (obj->type() == ElementType::CHORD || obj->type() == ElementType::REST) {
            removedChordsAndRests.insert(obj);
        }
    }

    QSet<EngravingObject*> emittedRemovals; // guard against double-emitting DeleteEvent
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::RemoveElement)) {
            continue;
        }

        if (obj->type() == ElementType::NOTE) {
            Note* note   = static_cast<Note*>(obj);
            Chord* chord = note->chord();

            if (chord && removedChordsAndRests.contains(chord)) {
                // The whole chord is being removed. Emit one DeleteEvent per chord.
                if (!emittedRemovals.contains(chord)) {
                    emittedRemovals.insert(chord);
                    // Try chord UUID first (InsertChord case: UUID on Chord*).
                    const QString chordUuid = uuidForElement(chord, remoteElementToUuid);
                    if (!chordUuid.isEmpty()) {
                        ops.append(buildDeleteEvent(chordUuid));
                        m_localUuidToElement.remove(chordUuid);
                        m_localElementToUuid.remove(chord);
                    } else {
                        // InsertNote case: UUID on Note*.
                        const QString noteUuid = uuidForElement(note, remoteElementToUuid);
                        if (!noteUuid.isEmpty()) {
                            ops.append(buildDeleteEvent(noteUuid));
                            m_localUuidToElement.remove(noteUuid);
                            m_localElementToUuid.remove(note);
                        }
                    }
                }
            } else if (chord) {
                // Note removed from a still-alive chord → RemoveChordNote.
                const QString chordUuid = uuidForElement(chord, remoteElementToUuid);
                if (!chordUuid.isEmpty()) {
                    ops.append(buildRemoveChordNote(chordUuid, note));
                }
            }
        }

        if (obj->type() == ElementType::REST) {
            const QString uuid = uuidForElement(obj, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildDeleteEvent(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(obj);
            }
        }
    }

    // ── Pass 7: ChangePitch → SetPitch ────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::ChangePitch)) {
            continue;
        }
        if (obj->type() != ElementType::NOTE) {
            continue;
        }
        const QString uuid = uuidForElement(obj, remoteElementToUuid);
        if (uuid.isEmpty()) {
            continue;
        }
        ops.append(buildSetPitch(uuid, static_cast<Note*>(obj)));
    }

    // ── Pass 8: ChangeProperty + Pid::TRACK → SetTrack ───────────────────
    if (changedPropertyIdSet.count(Pid::TRACK)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            if (obj->type() != ElementType::NOTE && obj->type() != ElementType::CHORD) {
                continue;
            }
            const QString uuid = uuidForElement(obj, remoteElementToUuid);
            if (uuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetTrack(uuid, static_cast<EngravingItem*>(obj)));
        }
    }

    // ── Pass 9: AddPart / RemovePart ─────────────────────────────────────
    // MuseScore may emit Part objects through changesChannel when a part is
    // added or removed. Guard with ElementType::PART; if parts don't appear
    // in changedObjects (score-level change not propagated), this pass is a
    // no-op and the user's local part edits will not be transmitted.
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::PART) {
            continue;
        }
        Part* part = static_cast<Part*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_knownPartUuids[part] = uuid;
            ops.append(buildAddPart(part, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = m_knownPartUuids.value(part);
            if (!uuid.isEmpty()) {
                m_knownPartUuids.remove(part);
                ops.append(buildRemovePart(uuid));
            }
        }
    }

    // ── Pass 10: SetPartName / SetStaffCount / SetPartInstrument ─────────
    if (changedPropertyIdSet.count(Pid::PARTNAME)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::PART) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            Part* part = static_cast<Part*>(obj);
            const QString uuid = m_knownPartUuids.value(part);
            if (uuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetPartName(uuid, part->partName().toQString()));
        }
    }

    // ── Pass 11: SetKeySignature & SetClef ───────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() == ElementType::KEYSIG) {
            auto* ks = static_cast<KeySig*>(obj);
            Part* part = static_cast<EngravingItem*>(obj)->part();
            if (!part) continue;
            const QString partUuid = m_knownPartUuids.value(part);
            if (partUuid.isEmpty()) continue;
            ops.append(buildSetKeySignature(ks, partUuid));
        } else if (obj->type() == ElementType::CLEF) {
            auto* clef = static_cast<Clef*>(obj);
            Part* part = static_cast<EngravingItem*>(obj)->part();
            if (!part) continue;
            const QString partUuid = m_knownPartUuids.value(part);
            if (partUuid.isEmpty()) continue;
            // Local staff index within the part.
            const staff_idx_t globalStaff = clef->track() / VOICES;
            const staff_idx_t firstStaff  = part->startTrack() / VOICES;
            const int staffIdx = static_cast<int>(globalStaff - firstStaff);
            ops.append(buildSetClef(clef, partUuid, staffIdx));
        }
    }

    return ops;
}

// ---------------------------------------------------------------------------
// Insert builders
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertNote(Note* note, const QString& uuid, const QString& partId)
{
    const Fraction tick = note->chord()->tick();
    const DurationType dt   = note->chord()->durationType().type();
    const int          dots = note->chord()->dots();

    QJsonObject duration;
    duration["type"] = durationTypeName(dt);
    duration["dots"] = dots;

    QJsonObject payload;
    payload["type"]     = "InsertNote";
    payload["part_id"]  = partId;
    payload["id"]       = uuid;
    payload["beat"]     = beatJson(tick);
    payload["duration"] = duration;
    payload["track"]    = static_cast<int>(note->track());
    payload["pitch"]    = pitchJson(note->tpc1(), note->octave());
    payload["tie"]      = QJsonValue::Null;
    return payload;
}

QJsonObject OperationTranslator::buildInsertRest(Rest* rest, const QString& uuid, const QString& partId)
{
    const Fraction tick = rest->tick();
    const DurationType dt   = rest->durationType().type();
    const int          dots = rest->dots();

    QJsonObject duration;
    duration["type"] = durationTypeName(dt);
    duration["dots"] = dots;

    QJsonObject payload;
    payload["type"]     = "InsertRest";
    payload["part_id"]  = partId;
    payload["id"]       = uuid;
    payload["beat"]     = beatJson(tick);
    payload["duration"] = duration;
    payload["track"]    = static_cast<int>(rest->track());
    return payload;
}

QJsonObject OperationTranslator::buildInsertChord(Chord* chord, const QVector<Note*>& notes,
                                                   const QString& uuid, const QString& partId)
{
    const Fraction tick = chord->tick();
    const DurationType dt   = chord->durationType().type();
    const int          dots = chord->dots();

    QJsonObject duration;
    duration["type"] = durationTypeName(dt);
    duration["dots"] = dots;

    QJsonArray pitches;
    for (Note* n : notes) {
        pitches.append(pitchJson(n->tpc1(), n->octave()));
    }

    QJsonObject payload;
    payload["type"]     = "InsertChord";
    payload["part_id"]  = partId;
    payload["id"]       = uuid;
    payload["beat"]     = beatJson(tick);
    payload["pitches"]  = pitches;
    payload["duration"] = duration;
    payload["track"]    = static_cast<int>(chord->track());
    payload["tie"]      = QJsonValue::Null;
    return payload;
}

// ---------------------------------------------------------------------------
// Modification / deletion builders
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddChordNote(const QString& chordUuid, Note* note)
{
    QJsonObject payload;
    payload["type"]     = "AddChordNote";
    payload["event_id"] = chordUuid;
    payload["pitch"]    = pitchJson(note->tpc1(), note->octave());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveChordNote(const QString& chordUuid, Note* note)
{
    QJsonObject payload;
    payload["type"]     = "RemoveChordNote";
    payload["event_id"] = chordUuid;
    payload["pitch"]    = pitchJson(note->tpc1(), note->octave());
    return payload;
}

QJsonObject OperationTranslator::buildDeleteEvent(const QString& uuid)
{
    QJsonObject payload;
    payload["type"]     = "DeleteEvent";
    payload["event_id"] = uuid;
    return payload;
}

QJsonObject OperationTranslator::buildSetPitch(const QString& uuid, Note* note)
{
    QJsonObject payload;
    payload["type"]     = "SetPitch";
    payload["event_id"] = uuid;
    payload["pitch"]    = pitchJson(note->tpc1(), note->octave());
    return payload;
}

QJsonObject OperationTranslator::buildSetTrack(const QString& uuid, EngravingItem* item)
{
    QJsonObject payload;
    payload["type"]     = "SetTrack";
    payload["event_id"] = uuid;
    payload["track"]    = static_cast<int>(item->track());
    return payload;
}

// ---------------------------------------------------------------------------
// Directive builders
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildSetTimeSignature(TimeSig* ts)
{
    const Fraction sig  = ts->sig();

    QJsonObject timeSig;
    timeSig["numerator"]   = sig.numerator();
    timeSig["denominator"] = sig.denominator();

    QJsonObject payload;
    payload["type"]           = "SetTimeSignature";
    payload["beat"]           = beatJson(ts->tick());
    payload["time_signature"] = timeSig;
    return payload;
}

QJsonObject OperationTranslator::buildSetTempo(TempoText* tt)
{
    const double bpm = tt->tempo().toBPM().val;

    // Default referent: quarter note (most common for tempo markings).
    QJsonObject referent;
    referent["type"] = QStringLiteral("quarter");
    referent["dots"] = 0;

    QJsonObject tempo;
    tempo["bpm"]      = bpm;
    tempo["referent"] = referent;
    tempo["text"]     = QJsonValue::Null;

    QJsonObject payload;
    payload["type"]  = "SetTempo";
    payload["beat"]  = beatJson(tt->tick());
    payload["tempo"] = tempo;
    return payload;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Converts a MuseScore TPC (tonal pitch class, circle-of-fifths integer) and
// octave into a JSON pitch object with "step", "octave", and "accidental".
//
// TPC circle-of-fifths layout: F=13, C=14, G=15, D=16, A=17, E=18, B=19
// (natural); each flat subtracts 7, each sharp adds 7.
QJsonObject OperationTranslator::pitchJson(int tpc, int octave)
{
    static const char* const kSteps[] = { "F", "C", "G", "D", "A", "E", "B" };
    static const int kNaturalTpc[]    = { 13, 14, 15, 16, 17, 18, 19 };
    static const char* const kAccidentals[] = {
        "double-flat", "flat", nullptr, "sharp", "double-sharp"
    };

    const int stepIndex = (tpc + 1) % 7;
    const int accOffset = (tpc - kNaturalTpc[stepIndex]) / 7; // range: -2..+2

    QJsonObject pitch;
    pitch["step"]   = kSteps[stepIndex];
    pitch["octave"] = octave;

    const int accIdx = accOffset + 2; // map -2..+2 → 0..4
    if (accIdx >= 0 && accIdx <= 4 && kAccidentals[accIdx]) {
        pitch["accidental"] = kAccidentals[accIdx];
    } else {
        pitch["accidental"] = QJsonValue::Null;
    }

    return pitch;
}

QJsonObject OperationTranslator::beatJson(const Fraction& tick)
{
    QJsonObject beat;
    beat["numerator"]   = tick.numerator();
    beat["denominator"] = tick.denominator();
    return beat;
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

// ---------------------------------------------------------------------------
// Part/staff directive builders (Pass 9+10)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddPart(Part* part, const QString& uuid)
{
    QJsonObject instr;
    instr["musescore_id"] = QStringLiteral("");
    instr["name"]         = part->longName().toQString();
    instr["short_name"]   = part->shortName().toQString();

    QJsonObject payload;
    payload["type"]        = QStringLiteral("AddPart");
    payload["part_id"]     = uuid;
    payload["name"]        = part->partName().toQString();
    payload["staff_count"] = static_cast<int>(part->nstaves());
    payload["instrument"]  = instr;
    return payload;
}

QJsonObject OperationTranslator::buildRemovePart(const QString& uuid)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemovePart");
    payload["part_id"] = uuid;
    return payload;
}

QJsonObject OperationTranslator::buildSetPartName(const QString& uuid, const QString& name)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetPartName");
    payload["part_id"] = uuid;
    payload["name"]    = name;
    return payload;
}

QJsonObject OperationTranslator::buildSetStaffCount(const QString& uuid, int count)
{
    QJsonObject payload;
    payload["type"]        = QStringLiteral("SetStaffCount");
    payload["part_id"]     = uuid;
    payload["staff_count"] = count;
    return payload;
}

QJsonObject OperationTranslator::buildSetPartInstrument(const QString& uuid, Part* part)
{
    QJsonObject instr;
    instr["musescore_id"] = QStringLiteral("");
    instr["name"]         = part->longName().toQString();
    instr["short_name"]   = part->shortName().toQString();

    QJsonObject payload;
    payload["type"]       = QStringLiteral("SetPartInstrument");
    payload["part_id"]    = uuid;
    payload["instrument"] = instr;
    return payload;
}

QJsonObject OperationTranslator::buildSetKeySignature(KeySig* ks, const QString& partUuid)
{
    const int sharps = static_cast<int>(ks->key());

    QJsonObject keySig;
    keySig["sharps"] = sharps;

    QJsonObject payload;
    payload["type"]          = QStringLiteral("SetKeySignature");
    payload["part_id"]       = partUuid;
    payload["beat"]          = beatJson(ks->tick());
    payload["key_signature"] = keySig;
    return payload;
}

QJsonObject OperationTranslator::buildSetClef(Clef* clef, const QString& partUuid, int staffIdx)
{
    static const QHash<ClefType, QString> s_clefNames = {
        { ClefType::G,    QStringLiteral("treble")     },
        { ClefType::F,    QStringLiteral("bass")       },
        { ClefType::C3,   QStringLiteral("alto")       },
        { ClefType::C4,   QStringLiteral("tenor")      },
        { ClefType::PERC, QStringLiteral("percussion") },
    };

    const ClefType ct   = clef->clefType();
    const QString cname = s_clefNames.value(ct, QStringLiteral("treble"));

    QJsonObject clefObj;
    clefObj["name"] = cname;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetClef");
    payload["part_id"] = partUuid;
    payload["beat"]    = beatJson(clef->tick());
    payload["staff"]   = staffIdx;
    payload["clef"]    = clefObj;
    return payload;
}
