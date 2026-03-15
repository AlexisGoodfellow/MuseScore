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

#include "engraving/dom/articulation.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/part.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/dom/rest.h"
#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

// Forward declarations for file-scope helpers used by translateAll.
static QString articulationNameFromSymId(SymId id);
static QString dynamicKindName(DynamicType dt);
static QString lyricSyllabicName(LyricsSyllabic s);
static QString markerKindName(MarkerType mt);

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

// UUID lookup for a ChordRest: checks the ChordRest pointer directly (works for
// InsertChord / InsertRest which store UUID on Chord* / Rest*), then falls back to
// checking the single Note* inside a single-note chord (InsertNote stores UUID on Note*).
// Without this fallback, articulation/slur/lyric ops on single-note events would be
// silently dropped because the Chord* is never in either UUID map.
QString OperationTranslator::uuidForChordRest(
    ChordRest* cr,
    const QHash<EngravingObject*, QString>& remoteElementToUuid) const
{
    const QString direct = uuidForElement(cr, remoteElementToUuid);
    if (!direct.isEmpty()) {
        return direct;
    }
    // Fall back: single-note Chord whose UUID was assigned via InsertNote.
    if (cr && cr->isChord()) {
        const std::vector<Note*>& notes = toChord(static_cast<EngravingItem*>(cr))->notes();
        if (notes.size() == 1) {
            return uuidForElement(notes.front(), remoteElementToUuid);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// translateAll — main dispatch
// ---------------------------------------------------------------------------
QVector<QJsonObject> OperationTranslator::translateAll(
    const std::map<EngravingObject*, std::unordered_set<CommandType>>& changedObjects,
    const PropertyIdSet& changedPropertyIdSet,
    const QString& partId,
    const QHash<EngravingObject*, QString>& remoteElementToUuid,
    const QMap<QString, QString>& changedMetaTags)
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
    if (changedPropertyIdSet.count(Pid::STAFF_LONG_NAME)) {
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

    // ── Pass 11: SetTie ───────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TIE) {
            continue;
        }
        auto* tie = static_cast<Tie*>(obj);
        Note* startNote = tie->startNote();
        if (!startNote) continue;

        const QString noteUuid = uuidForElement(startNote, remoteElementToUuid);
        if (noteUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildSetTie(noteUuid, /*tieStart=*/true));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildSetTie(noteUuid, /*tieStart=*/false));
        }
    }

    // ── Pass 12: ChordSymbol ──────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::HARMONY) {
            continue;
        }
        auto* harmony = static_cast<Harmony*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[harmony]  = uuid;
            m_localUuidToElement[uuid]     = harmony;
            const Fraction tick = harmony->tick();
            ops.append(buildAddChordSymbol(uuid, tick.numerator(), tick.denominator(),
                                           harmony->harmonyName().toQString()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(harmony, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveChordSymbol(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(harmony);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(harmony, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetChordSymbol(uuid, harmony->harmonyName().toQString()));
            }
        }
    }

    // ── Pass 13: SetKeySignature & SetClef ───────────────────────────────
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

    // ── Pass 14: Articulations ───────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::ARTICULATION) {
            continue;
        }
        auto* art = static_cast<Articulation*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ChordRest* cr = art->chordRest();
            if (!cr) continue;
            const QString eventUuid = uuidForChordRest(cr, remoteElementToUuid);
            if (eventUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddArticulation: parent UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[art]  = uuid;
            m_localUuidToElement[uuid] = art;
            ops.append(buildAddArticulation(art, uuid, partId, eventUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(art, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveArticulation(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(art);
            }
        }
    }

    // ── Pass 15: Dynamics ────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::DYNAMIC) {
            continue;
        }
        auto* dyn = static_cast<Dynamic*>(obj);
        Part* dynPart = static_cast<EngravingItem*>(dyn)->part();
        if (!dynPart) continue;
        const QString dynPartUuid = m_knownPartUuids.value(dynPart);
        if (dynPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[dyn]  = uuid;
            m_localUuidToElement[uuid] = dyn;
            ops.append(buildAddDynamic(dyn, uuid, dynPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(dyn, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveDynamic(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(dyn);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(dyn, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetDynamic(uuid, dynamicKindName(dyn->dynamicType())));
            }
        }
    }

    // ── Pass 16: Slurs ───────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::SLUR) {
            continue;
        }
        auto* slur = static_cast<Slur*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            EngravingItem* startEl = slur->startElement();
            EngravingItem* endEl   = slur->endElement();
            ChordRest* startCr = (startEl && startEl->isChordRest())
                                 ? toChordRest(startEl) : nullptr;
            ChordRest* endCr   = (endEl && endEl->isChordRest())
                                 ? toChordRest(endEl)   : nullptr;
            if (!startCr || !endCr) continue;
            const QString startUuid = uuidForChordRest(startCr, remoteElementToUuid);
            const QString endUuid   = uuidForChordRest(endCr,   remoteElementToUuid);
            if (startUuid.isEmpty() || endUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddSlur: start/end UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[slur]  = uuid;
            m_localUuidToElement[uuid]  = slur;
            ops.append(buildAddSlur(slur, uuid, partId, startUuid, endUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(slur, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveSlur(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(slur);
            }
        }
    }

    // ── Pass 17: Hairpins ────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::HAIRPIN) {
            continue;
        }
        auto* hp = static_cast<Hairpin*>(obj);
        Part* hpPart = static_cast<EngravingItem*>(hp)->part();
        if (!hpPart) continue;
        const QString hpPartUuid = m_knownPartUuids.value(hpPart);
        if (hpPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[hp]   = uuid;
            m_localUuidToElement[uuid] = hp;
            ops.append(buildAddHairpin(hp, uuid, hpPartUuid,
                                       hp->tick(), hp->tick2(),
                                       hp->isCrescendo()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(hp, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveHairpin(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(hp);
            }
        }
    }

    // ── Pass 18: Tuplets ─────────────────────────────────────────────────
    // Must run after Passes 2/3 so member UUIDs are already registered.
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TUPLET) {
            continue;
        }
        auto* tup = static_cast<Tuplet*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Part* tupPart = static_cast<EngravingItem*>(tup)->part();
            if (!tupPart) continue;
            const QString tupPartUuid = m_knownPartUuids.value(tupPart);
            if (tupPartUuid.isEmpty()) continue;

            // Collect member UUIDs (members must already be registered from Pass 2/3).
            QVector<QString> memberUuids;
            for (DurationElement* elem : tup->elements()) {
                const QString memberUuid = uuidForElement(elem, remoteElementToUuid);
                memberUuids.append(memberUuid); // may be empty if not tracked
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[tup]  = uuid;
            m_localUuidToElement[uuid] = tup;
            ops.append(buildAddTuplet(tup, uuid, tupPartUuid, memberUuids,
                                      tup->ratio().numerator(),
                                      tup->ratio().denominator()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(tup, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                // Unregister members before emitting RemoveTuplet.
                for (DurationElement* elem : tup->elements()) {
                    const QString memberUuid = uuidForElement(elem, remoteElementToUuid);
                    if (!memberUuid.isEmpty()) {
                        m_localUuidToElement.remove(memberUuid);
                        m_localElementToUuid.remove(elem);
                    }
                }
                ops.append(buildRemoveTuplet(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(tup);
            }
        }
    }

    // ── Pass 19: Lyrics ──────────────────────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::LYRICS) {
            continue;
        }
        auto* lyr = static_cast<Lyrics*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ChordRest* cr = lyr->chordRest();
            if (!cr) continue;
            const QString eventUuid = uuidForChordRest(cr, remoteElementToUuid);
            if (eventUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddLyric: parent UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[lyr]  = uuid;
            m_localUuidToElement[uuid] = lyr;
            ops.append(buildAddLyric(lyr, uuid, partId, eventUuid,
                                     lyr->verse(),
                                     lyricSyllabicName(lyr->syllabic()),
                                     lyr->plainText().toQString()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(lyr, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveLyric(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(lyr);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(lyr, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetLyric(uuid,
                                         lyr->plainText().toQString(),
                                         lyricSyllabicName(lyr->syllabic())));
            }
        }
    }

    // ── Pass 20: InsertBeats / DeleteBeats ───────────────────────────────
    // Collect MEASURE objects by their command type, sort by tick, then emit
    // a single InsertBeats or DeleteBeats covering the contiguous range.
    {
        QVector<Measure*> insertedMeasures;
        QVector<Measure*> removedMeasures;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::MEASURE) {
                continue;
            }
            auto* m = static_cast<Measure*>(obj);
            if (cmds.count(CommandType::InsertMeasures)) {
                insertedMeasures.append(m);
            } else if (cmds.count(CommandType::RemoveMeasures)) {
                removedMeasures.append(m);
            }
        }

        auto sortByTick = [](Measure* a, Measure* b) {
            return a->tick() < b->tick();
        };

        if (!insertedMeasures.isEmpty()) {
            std::sort(insertedMeasures.begin(), insertedMeasures.end(), sortByTick);
            const Fraction atBeat = insertedMeasures.first()->tick();
            Fraction totalDur;
            for (Measure* m : insertedMeasures) {
                totalDur += m->ticks();
            }
            QJsonObject op;
            op["type"]     = QStringLiteral("InsertBeats");
            op["at_beat"]  = beatJson(atBeat);
            op["duration"] = beatJson(totalDur);
            ops.append(op);
        }

        if (!removedMeasures.isEmpty()) {
            std::sort(removedMeasures.begin(), removedMeasures.end(), sortByTick);
            const Fraction atBeat = removedMeasures.first()->tick();
            Fraction totalDur;
            for (Measure* m : removedMeasures) {
                totalDur += m->ticks();
            }
            QJsonObject op;
            op["type"]     = QStringLiteral("DeleteBeats");
            op["at_beat"]  = beatJson(atBeat);
            op["duration"] = beatJson(totalDur);
            ops.append(op);
        }
    }

    // ── Pass 21: InsertVolta / RemoveVolta ───────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::VOLTA) {
            continue;
        }
        auto* volta = static_cast<Volta*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[volta]  = uuid;
            m_localUuidToElement[uuid]   = volta;
            ops.append(buildInsertVolta(volta, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(volta, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveVolta(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(volta);
            }
        }
    }

    // ── Pass 22: InsertMarker / RemoveMarker ─────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::MARKER) {
            continue;
        }
        auto* marker = static_cast<Marker*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[marker]  = uuid;
            m_localUuidToElement[uuid]    = marker;
            ops.append(buildInsertMarker(marker, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(marker, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveMarker(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(marker);
            }
        }
    }

    // ── Pass 23: InsertJump / RemoveJump ─────────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::JUMP) {
            continue;
        }
        auto* jump = static_cast<Jump*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[jump]  = uuid;
            m_localUuidToElement[uuid]  = jump;
            ops.append(buildInsertJump(jump, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(jump, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveJump(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(jump);
            }
        }
    }

    // ── Pass 24: SetScoreMetadata ─────────────────────────────────────────
    // changedMetaTags contains only the tags that changed in this transaction
    // (pre/post diff performed in EditudeService::onScoreChanges).
    if (!changedMetaTags.isEmpty()) {
        // Reverse map: MuseScore tag name → Python field name.
        static const QHash<QString, QString> s_reverseFieldMap = {
            { "workTitle",       "title"           },
            { "subtitle",        "subtitle"        },
            { "composer",        "composer"        },
            { "arranger",        "arranger"        },
            { "lyricist",        "lyricist"        },
            { "copyright",       "copyright"       },
            { "workNumber",      "work_number"     },
            { "movementNumber",  "movement_number" },
            { "movementTitle",   "movement_title"  },
        };
        for (auto it = changedMetaTags.begin(); it != changedMetaTags.end(); ++it) {
            const QString field = s_reverseFieldMap.value(it.key(), it.key());
            QJsonObject op;
            op["type"]  = QStringLiteral("SetScoreMetadata");
            op["field"] = field;
            op["value"] = it.value();
            ops.append(op);
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

QJsonObject OperationTranslator::buildSetTie(const QString& noteUuid, bool tieStart)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetTie");
    payload["event_id"] = noteUuid;
    if (tieStart) {
        payload["tie"] = QStringLiteral("start");
    } else {
        payload["tie"] = QJsonValue::Null;
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — articulations
// ---------------------------------------------------------------------------

// Reverse of the articulationSymId() map in scoreapplicator.cpp.
static QString articulationNameFromSymId(SymId id)
{
    static const QHash<SymId, QString> s_reverseMap = {
        { SymId::articStaccatoAbove,      QStringLiteral("staccato")      },
        { SymId::articAccentAbove,        QStringLiteral("accent")        },
        { SymId::articTenutoAbove,        QStringLiteral("tenuto")        },
        { SymId::articMarcatoAbove,       QStringLiteral("marcato")       },
        { SymId::articStaccatissimoAbove, QStringLiteral("staccatissimo") },
        { SymId::fermataAbove,            QStringLiteral("fermata")       },
        { SymId::ornamentTrill,           QStringLiteral("trill")         },
        { SymId::ornamentMordent,         QStringLiteral("mordent")       },
        { SymId::ornamentTurn,            QStringLiteral("turn")          },
    };
    return s_reverseMap.value(id, QStringLiteral("staccato"));
}

QJsonObject OperationTranslator::buildAddArticulation(EngravingObject* art,
                                                       const QString& uuid,
                                                       const QString& /*partId*/,
                                                       const QString& eventUuid)
{
    auto* a = static_cast<Articulation*>(art);
    QJsonObject payload;
    payload["type"]         = QStringLiteral("AddArticulation");
    payload["id"]           = uuid;
    payload["event_id"]     = eventUuid;
    payload["articulation"] = articulationNameFromSymId(a->symId());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveArticulation(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveArticulation");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — dynamics
// ---------------------------------------------------------------------------

// Reverse map: DynamicType → Python kind string.
static QString dynamicKindName(DynamicType dt)
{
    switch (dt) {
    case DynamicType::PPP: return QStringLiteral("ppp");
    case DynamicType::PP:  return QStringLiteral("pp");
    case DynamicType::P:   return QStringLiteral("p");
    case DynamicType::MP:  return QStringLiteral("mp");
    case DynamicType::MF:  return QStringLiteral("mf");
    case DynamicType::F:   return QStringLiteral("f");
    case DynamicType::FF:  return QStringLiteral("ff");
    case DynamicType::FFF: return QStringLiteral("fff");
    case DynamicType::SFZ: return QStringLiteral("sfz");
    case DynamicType::FP:  return QStringLiteral("fp");
    case DynamicType::RF:  return QStringLiteral("rf");
    default:               return QStringLiteral("mf");
    }
}

QJsonObject OperationTranslator::buildAddDynamic(EngravingObject* dyn,
                                                  const QString& uuid,
                                                  const QString& partId)
{
    auto* d = static_cast<Dynamic*>(dyn);
    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddDynamic");
    payload["id"]      = uuid;
    payload["part_id"] = partId;
    payload["kind"]    = dynamicKindName(d->dynamicType());
    payload["beat"]    = beatJson(d->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetDynamic(const QString& uuid, const QString& kind)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetDynamic");
    payload["id"]   = uuid;
    payload["kind"] = kind;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveDynamic(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveDynamic");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — slurs
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddSlur(EngravingObject* /*slur*/,
                                               const QString& uuid,
                                               const QString& /*partId*/,
                                               const QString& startEventUuid,
                                               const QString& endEventUuid)
{
    QJsonObject payload;
    payload["type"]           = QStringLiteral("AddSlur");
    payload["id"]             = uuid;
    payload["start_event_id"] = startEventUuid;
    payload["end_event_id"]   = endEventUuid;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveSlur(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveSlur");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — hairpins
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddHairpin(EngravingObject* /*hp*/,
                                                  const QString& uuid,
                                                  const QString& partId,
                                                  const Fraction& startTick,
                                                  const Fraction& endTick,
                                                  bool isCrescendo)
{
    QJsonObject payload;
    payload["type"]        = QStringLiteral("AddHairpin");
    payload["id"]          = uuid;
    payload["part_id"]     = partId;
    payload["kind"]        = isCrescendo ? QStringLiteral("crescendo")
                                         : QStringLiteral("diminuendo");
    payload["start_beat"]  = beatJson(startTick);
    payload["end_beat"]    = beatJson(endTick);
    return payload;
}

QJsonObject OperationTranslator::buildRemoveHairpin(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveHairpin");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — tuplets
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTuplet(EngravingObject* tup,
                                                 const QString& uuid,
                                                 const QString& partId,
                                                 const QVector<QString>& memberUuids,
                                                 int actualNotes,
                                                 int normalNotes)
{
    auto* t = static_cast<Tuplet*>(tup);

    QJsonArray members;
    for (const QString& mid : memberUuids) {
        QJsonObject m;
        m["id"] = mid;
        members.append(m);
    }

    QJsonObject baseDur;
    baseDur["type"] = durationTypeName(t->baseLen().type());
    baseDur["dots"] = t->baseLen().dots();

    QJsonObject payload;
    payload["type"]          = QStringLiteral("AddTuplet");
    payload["id"]            = uuid;
    payload["part_id"]       = partId;
    payload["actual_notes"]  = actualNotes;
    payload["normal_notes"]  = normalNotes;
    payload["base_duration"] = baseDur;
    payload["beat"]          = beatJson(t->tick());
    payload["members"]       = members;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveTuplet(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveTuplet");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — chord symbols
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddChordSymbol(const QString& uuid,
                                                      int beatNum, int beatDen,
                                                      const QString& name)
{
    QJsonObject beat;
    beat["numerator"]   = beatNum;
    beat["denominator"] = beatDen;

    QJsonObject payload;
    payload["type"] = QStringLiteral("AddChordSymbol");
    payload["id"]   = uuid;
    payload["name"] = name;
    payload["beat"] = beat;
    return payload;
}

QJsonObject OperationTranslator::buildSetChordSymbol(const QString& uuid, const QString& name)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetChordSymbol");
    payload["id"]   = uuid;
    payload["name"] = name;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveChordSymbol(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveChordSymbol");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — lyrics
// ---------------------------------------------------------------------------

// Maps LyricsSyllabic enum → Python syllabic name string.
static QString lyricSyllabicName(LyricsSyllabic s)
{
    switch (s) {
    case LyricsSyllabic::SINGLE: return QStringLiteral("single");
    case LyricsSyllabic::BEGIN:  return QStringLiteral("begin");
    case LyricsSyllabic::MIDDLE: return QStringLiteral("middle");
    case LyricsSyllabic::END:    return QStringLiteral("end");
    default:                     return QStringLiteral("single");
    }
}

QJsonObject OperationTranslator::buildAddLyric(EngravingObject* /*lyr*/,
                                                const QString& uuid,
                                                const QString& /*partId*/,
                                                const QString& eventUuid,
                                                int verse,
                                                const QString& syllabic,
                                                const QString& text)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("AddLyric");
    payload["id"]       = uuid;
    payload["event_id"] = eventUuid;
    payload["verse"]    = verse;
    payload["syllabic"] = syllabic;
    payload["text"]     = text;
    return payload;
}

QJsonObject OperationTranslator::buildSetLyric(const QString& uuid,
                                                const QString& text,
                                                const QString& syllabic)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetLyric");
    payload["id"]       = uuid;
    payload["text"]     = text;
    payload["syllabic"] = syllabic;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveLyric(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveLyric");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — volta
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertVolta(EngravingObject* volta, const QString& uuid)
{
    auto* v = static_cast<Volta*>(volta);
    QJsonArray numbers;
    for (int n : v->endings()) {
        numbers.append(n);
    }
    const bool openEnd = (v->voltaType() == Volta::Type::OPEN);

    QJsonObject payload;
    payload["type"]       = QStringLiteral("InsertVolta");
    payload["id"]         = uuid;
    payload["start_beat"] = beatJson(v->tick());
    payload["end_beat"]   = beatJson(v->tick2());
    payload["numbers"]    = numbers;
    payload["open_end"]   = openEnd;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveVolta(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveVolta");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — markers
// ---------------------------------------------------------------------------

// Reverse of s_markerKindMap in applyInsertMarker (scoreapplicator.cpp).
static QString markerKindName(MarkerType mt)
{
    switch (mt) {
    case MarkerType::SEGNO:    return QStringLiteral("segno");
    case MarkerType::CODA:     return QStringLiteral("coda");
    case MarkerType::FINE:     return QStringLiteral("fine");
    case MarkerType::TOCODA:   return QStringLiteral("to_coda");
    case MarkerType::VARSEGNO: return QStringLiteral("segno_var");
    default:                   return QStringLiteral("segno");
    }
}

QJsonObject OperationTranslator::buildInsertMarker(EngravingObject* marker, const QString& uuid)
{
    auto* m = static_cast<Marker*>(marker);
    QJsonObject payload;
    payload["type"]  = QStringLiteral("InsertMarker");
    payload["id"]    = uuid;
    payload["beat"]  = beatJson(m->measure() ? m->measure()->tick() : m->tick());
    payload["kind"]  = markerKindName(m->markerType());
    payload["label"] = m->label().toQString();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveMarker(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveMarker");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — jumps
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertJump(EngravingObject* jump, const QString& uuid)
{
    auto* j = static_cast<Jump*>(jump);
    QJsonObject payload;
    payload["type"]        = QStringLiteral("InsertJump");
    payload["id"]          = uuid;
    payload["beat"]        = beatJson(j->tick());
    payload["jump_to"]     = j->jumpTo().toQString();
    payload["play_until"]  = j->playUntil().toQString();
    payload["continue_at"] = j->continueAt().toQString();
    payload["text"]        = j->plainText().toQString();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveJump(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveJump");
    payload["id"]   = uuid;
    return payload;
}
