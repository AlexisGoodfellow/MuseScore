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

#include <algorithm>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include "engraving/dom/accidental.h"
#include "engraving/dom/arpeggio.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/drumset.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/glissando.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/volta.h"
#include "engraving/dom/rest.h"
#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

#include "editudeutils.h"

// Forward declaration for file-scope helper used by translateAll.
static QString lyricSyllabicName(LyricsSyllabic s);

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
// resolvePartUuid — look up or lazily register a Part's editude UUID.
// ---------------------------------------------------------------------------
QString OperationTranslator::resolvePartUuid(Part* part, QVector<QJsonObject>& lazyAddPartOps)
{
    if (!part) {
        return {};
    }
    auto it = m_knownPartUuids.find(part);
    if (it != m_knownPartUuids.end()) {
        return it.value();
    }
    // Part not yet registered (loaded from MSCX before any OT session).
    // Assign a UUID, emit AddPart so the server learns about this part,
    // and record the mapping for all subsequent ops in this session.
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_knownPartUuids[part] = uuid;
    lazyAddPartOps.append(buildAddPart(part, uuid));
    return uuid;
}

// ---------------------------------------------------------------------------
// registerKnownPart — external registration (e.g. after sync adopt).
// ---------------------------------------------------------------------------
void OperationTranslator::registerKnownPart(Part* part, const QString& uuid)
{
    if (part && !uuid.isEmpty()) {
        m_knownPartUuids[part] = uuid;
    }
}

// ---------------------------------------------------------------------------
// reset — clear all internal state for a fresh session.
// ---------------------------------------------------------------------------
void OperationTranslator::reset()
{
    m_knownPartUuids.clear();
    m_localUuidToElement.clear();
    m_localElementToUuid.clear();
}

// ---------------------------------------------------------------------------
// bootstrapPartsFromScore — register pre-existing MSCX parts.
// ---------------------------------------------------------------------------
QVector<QJsonObject> OperationTranslator::bootstrapPartsFromScore(
    Score* score,
    std::function<void(Part*, const QString&)> registerCallback)
{
    if (!score) {
        return {};
    }
    QVector<QJsonObject> ops;
    for (Part* part : score->parts()) {
        if (!part || m_knownPartUuids.contains(part)) {
            continue;
        }
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_knownPartUuids[part] = uuid;
        ops.append(buildAddPart(part, uuid));
        if (registerCallback) {
            registerCallback(part, uuid);
        }
    }
    return ops;
}

// ---------------------------------------------------------------------------
// translateAll — main dispatch
// ---------------------------------------------------------------------------
QVector<QJsonObject> OperationTranslator::translateAll(
    const std::map<EngravingObject*, std::unordered_set<CommandType>>& changedObjects,
    const PropertyIdSet& changedPropertyIdSet,
    const QHash<EngravingObject*, QString>& remoteElementToUuid,
    const QMap<QString, QString>& changedMetaTags)
{
    QVector<QJsonObject> ops;
    // AddPart ops for MSCX-loaded parts discovered lazily during this batch.
    // These must be prepended to ops so the server knows about the parts before
    // the element ops that reference them.
    QVector<QJsonObject> lazyAddPartOps;

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
        // Skip grace chords — they are handled by Pass 26c (AddGraceNote).
        if (chord->isGrace()) {
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
        // Skip elements with unsupported duration types (e.g. V_MEASURE for
        // measure rests created during MuseScore's internal initialisation).
        if (durationTypeName(chord->durationType().type()) == QLatin1String("unknown")) {
            continue;
        }
        Part* chordPart = chord->staff() ? chord->staff()->part() : nullptr;
        const QString chordPartUuid = resolvePartUuid(chordPart, lazyAddPartOps);
        if (chordPartUuid.isEmpty()) {
            continue; // no part available — skip
        }
        const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (notes.size() == 1) {
            // Single-note chord: UUID lives on the Note* (mirrors applyInsertNote).
            Note* n = notes[0];
            m_localElementToUuid[n]    = uuid;
            m_localUuidToElement[uuid] = n;
            ops.append(buildInsertNote(n, uuid, chordPartUuid));
        } else {
            // Multi-note chord: UUID lives on the Chord* (mirrors applyInsertChord).
            m_localElementToUuid[chord]  = uuid;
            m_localUuidToElement[uuid]   = chord;
            ops.append(buildInsertChord(chord, notes, uuid, chordPartUuid));
        }
        for (Note* n : notes) {
            handledNotes.insert(n);
        }
    }

    // ── Pass 3: InsertRest (with fill-rest suppression) ──────────────────
    //
    // setNoteRest() creates the target element AND "fill rests" to pad the
    // remaining measure duration.  On the peer, applying the primary op
    // (InsertNote/InsertRest/InsertChord) via setNoteRest produces its own
    // fills, so emitting InsertRest for fills would double-apply and corrupt
    // the score.
    //
    // Suppression rules:
    //   (a) Pass 2 emitted InsertNote/InsertChord → ALL rests are fills.
    //   (b) Batch removes Notes (chord deletion creates fill rests) →
    //       suppress all rests.
    //   (c) Pure InsertRest: emit only the single rest at the earliest tick;
    //       the remainder are fills from setNoteRest.
    {
        const bool hasNewChords = !newChordNotes.isEmpty();

        // Pre-scan: does this batch remove any Notes?
        bool hasNoteRemovals = false;
        if (!hasNewChords) {
            for (const auto& [obj, cmds] : changedObjects) {
                if (!obj || !cmds.count(CommandType::RemoveElement)) {
                    continue;
                }
                if (obj->type() == ElementType::NOTE) {
                    hasNoteRemovals = true;
                    break;
                }
            }
        }

        if (!hasNewChords && !hasNoteRemovals) {
            // Collect valid InsertRest candidates.
            struct RestCandidate {
                Rest* rest;
                Fraction tick;
                QString partUuid;
            };
            QVector<RestCandidate> candidates;

            for (const auto& [obj, cmds] : changedObjects) {
                if (!obj || !cmds.count(CommandType::AddElement)) {
                    continue;
                }
                if (obj->type() != ElementType::REST) {
                    continue;
                }
                Rest* rest = static_cast<Rest*>(obj);
                if (durationTypeName(rest->durationType().type())
                    == QLatin1String("unknown")) {
                    continue;
                }
                Part* restPart = rest->staff() ? rest->staff()->part()
                                               : nullptr;
                const QString restPartUuid =
                    resolvePartUuid(restPart, lazyAddPartOps);
                if (restPartUuid.isEmpty()) {
                    continue;
                }
                candidates.append({rest, rest->tick(), restPartUuid});
            }

            if (!candidates.isEmpty()) {
                // Sort by tick — emit only the earliest (the primary rest).
                std::sort(candidates.begin(), candidates.end(),
                          [](const RestCandidate& a, const RestCandidate& b) {
                              return a.tick < b.tick;
                          });
                const auto& c = candidates.first();
                const QString uuid =
                    QUuid::createUuid().toString(QUuid::WithoutBraces);
                m_localElementToUuid[c.rest] = uuid;
                m_localUuidToElement[uuid]   = c.rest;
                ops.append(buildInsertRest(c.rest, uuid, c.partUuid));
            }
        }
        // else: hasNewChords || hasNoteRemovals — all rests are fill rests
        // created as side effects; skip the entire pass.
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

    // ── Pass 5a: AddPart / RemovePart ───────────────────────────────────
    // Must run BEFORE the emittedTimeSig early return (Pass 5b) because
    // adding a part creates staves whose TimeSig/KeySig/Clef elements
    // appear in the same batch.  Without this, the TimeSig early return
    // would suppress the AddPart op entirely.
    //
    // Track newly-added Parts: Pass 10b must skip SetStaffCount for these
    // because AddPart already carries the staff_count field.
    QSet<Part*> newlyAddedParts;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::PART) {
            continue;
        }
        Part* part = static_cast<Part*>(obj);
        if (cmds.count(CommandType::AddElement) || cmds.count(CommandType::InsertPart)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_knownPartUuids[part] = uuid;
            ops.append(buildAddPart(part, uuid));
            newlyAddedParts.insert(part);
        } else if (cmds.count(CommandType::RemoveElement) || cmds.count(CommandType::RemovePart)) {
            const QString uuid = m_knownPartUuids.value(part);
            if (!uuid.isEmpty()) {
                m_knownPartUuids.remove(part);
                ops.append(buildRemovePart(uuid));
            }
        }
    }

    // ── Pass 5b: SetTimeSignature & SetTempo ─────────────────────────────
    // A TimeSig may appear with AddElement (new time sig added) OR
    // ChangeProperty (existing time sig modified by cmdAddTimeSig).
    //
    // Important: when a staff is added (InsertStaff/InsertPart), MuseScore's
    // cmdAddStaves replicates the existing TimeSig to the new staff via
    // undoAddElement(timesig).  This is a REPLICATION, not a genuine time
    // signature change.  We must NOT emit SetTimeSignature or trigger the
    // compound-op early return for these replications — doing so suppresses
    // the real ops in the batch (SetStaffCount, SetKeySignature, etc.) and
    // also inflates the revision count, breaking subsequent wait_revision
    // expectations.
    bool batchHasStaffInsert = false;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj) {
            continue;
        }
        if (obj->type() == ElementType::STAFF
            && (cmds.count(CommandType::InsertStaff)
                || cmds.count(CommandType::RemoveStaff))) {
            batchHasStaffInsert = true;
            break;
        }
        if (obj->type() == ElementType::PART
            && (cmds.count(CommandType::InsertPart)
                || cmds.count(CommandType::AddElement))) {
            batchHasStaffInsert = true;
            break;
        }
    }

    bool emittedTimeSig = false;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj) {
            continue;
        }
        if (obj->type() == ElementType::TIMESIG) {
            // ChangeProperty always means a genuine time sig change
            // (cmdAddTimeSig modifies an existing TimeSig element).
            // AddElement without a staff insert is also genuine (new
            // time sig at a previously empty position).
            // AddElement WITH a staff insert is a replication — skip it.
            const bool isGenuine =
                cmds.count(CommandType::ChangeProperty)
                || (cmds.count(CommandType::AddElement) && !batchHasStaffInsert);
            if (isGenuine) {
                auto* ts = toTimeSig(static_cast<EngravingItem*>(obj));
                if (ts) {
                    ops.append(buildSetTimeSignature(ts));
                    emittedTimeSig = true;
                }
            }
        }
        if (obj->type() == ElementType::TEMPO_TEXT
            && cmds.count(CommandType::AddElement)) {
            ops.append(buildSetTempo(static_cast<TempoText*>(obj)));
        }
    }

    // SetTimeSignature is a compound op: cmdAddTimeSig on the receiver
    // calls rewriteMeasures which restructures measure boundaries, creates/
    // removes rests, etc.  All other changes in this batch (InsertRest,
    // DeleteEvent, InsertBeats, …) are side effects of the same rewrite and
    // must NOT be emitted — applying them on the peer would double-apply and
    // corrupt the score or crash during layout.
    if (emittedTimeSig) {
        if (!lazyAddPartOps.isEmpty()) {
            QVector<QJsonObject> result = lazyAddPartOps;
            result.append(ops);
            return result;
        }
        return ops;
    }

    // ── Pass 6: RemoveElement ─────────────────────────────────────────────
    // Identify which chords/rests are being fully removed in this transaction.
    QSet<EngravingObject*> removedChordsAndRests;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::RemoveElement)) {
            continue;
        }
        if (obj->type() == ElementType::CHORD) {
            // Skip grace chords — they are handled by Pass 26c (RemoveGraceNote).
            if (static_cast<Chord*>(obj)->isGrace()) {
                continue;
            }
            removedChordsAndRests.insert(obj);
        } else if (obj->type() == ElementType::REST) {
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

            // Skip notes belonging to grace chords — handled by Pass 26c.
            if (chord && chord->isGrace()) {
                continue;
            }

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
    // Accept both CommandType::ChangePitch (interactive pitch change via
    // Score::changePitch) and CommandType::ChangeProperty with Pid::PITCH
    // (programmatic pitch change via undoChangeProperty).
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::NOTE) {
            continue;
        }
        const bool hasPitchChange = cmds.count(CommandType::ChangePitch)
            || (cmds.count(CommandType::ChangeProperty)
                && changedPropertyIdSet.count(Pid::PITCH));
        if (!hasPitchChange) {
            continue;
        }
        const QString uuid = uuidForElement(obj, remoteElementToUuid);
        if (uuid.isEmpty()) {
            continue;
        }
        ops.append(buildSetPitch(uuid, static_cast<Note*>(obj)));
    }

    // ── Pass 7b: ChangeProperty + Pid::FRET/STRING → SetTabNote ──────────
    // A fret/string change at constant pitch is a distinct user intent from a
    // pitch change — it represents changing the voicing on a fretted instrument.
    if (changedPropertyIdSet.count(Pid::FRET) || changedPropertyIdSet.count(Pid::STRING)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::NOTE) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            const QString uuid = uuidForElement(obj, remoteElementToUuid);
            if (uuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetTabNote(uuid, static_cast<Note*>(obj)));
        }
    }

    // ── Pass 7c: ChangeProperty + Pid::HEAD_GROUP → SetNoteHead ─────────
    if (changedPropertyIdSet.count(Pid::HEAD_GROUP)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::NOTE) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            const QString uuid = uuidForElement(obj, remoteElementToUuid);
            if (uuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetNoteHead(uuid, static_cast<Note*>(obj)));
        }
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

    // ── Pass 10: SetPartName / SetPartInstrument ────────────────────────
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
            ops.append(buildSetPartInstrument(uuid, part));
        }
    }

    // ── Pass 10b: SetStaffCount (InsertStaff / RemoveStaff) ──────────────
    // Skip parts that were just added in Pass 5a — AddPart already carries
    // the staff_count, so a redundant SetStaffCount would only inflate the
    // server revision count.
    {
        QSet<Part*> staffChangedParts;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::STAFF) {
                continue;
            }
            if (!cmds.count(CommandType::InsertStaff)
                && !cmds.count(CommandType::RemoveStaff)) {
                continue;
            }
            auto* staff = static_cast<Staff*>(obj);
            Part* part = staff->part();
            if (part && !staffChangedParts.contains(part)
                && !newlyAddedParts.contains(part)) {
                staffChangedParts.insert(part);
                const QString uuid = m_knownPartUuids.value(part);
                if (!uuid.isEmpty()) {
                    ops.append(buildSetStaffCount(uuid, static_cast<int>(part->nstaves())));
                }
            }
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
            const Fraction tick = harmony->tick().reduced();
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
    // AddElement → emit set op with the new value.
    // RemoveElement → emit set op with null value (undo / removal).
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj) {
            continue;
        }
        const bool isAdd    = cmds.count(CommandType::AddElement) > 0;
        const bool isRemove = cmds.count(CommandType::RemoveElement) > 0;
        if (!isAdd && !isRemove) {
            continue;
        }
        if (obj->type() == ElementType::KEYSIG) {
            auto* ks = static_cast<KeySig*>(obj);
            Part* part = static_cast<EngravingItem*>(obj)->part();
            if (!part) {
                // After undo, the element's KeySig segment may have been
                // removed from the Measure (undoGetSegment reversal), breaking
                // the parent chain so element->part() returns nullptr.
                // Fall back to a track-based lookup through m_knownPartUuids
                // whose Part* pointers remain live in the score.
                const staff_idx_t staffIdx = ks->track() / VOICES;
                for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                    Part* p = it.key();
                    const staff_idx_t first = p->startTrack() / VOICES;
                    if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                        part = p;
                        break;
                    }
                }
            }
            if (!part) continue;
            const QString partUuid = resolvePartUuid(part, lazyAddPartOps);
            if (partUuid.isEmpty()) continue;
            if (isAdd) {
                ops.append(buildSetKeySignature(ks, partUuid));
            } else {
                // Removal: emit SetKeySignature with null key_signature.
                QJsonObject payload;
                payload["type"]          = QStringLiteral("SetKeySignature");
                payload["part_id"]       = partUuid;
                payload["beat"]          = beatJson(ks->tick());
                payload["key_signature"] = QJsonValue::Null;
                ops.append(payload);
            }
        } else if (obj->type() == ElementType::CLEF) {
            auto* clef = static_cast<Clef*>(obj);
            Part* part = static_cast<EngravingItem*>(obj)->part();
            if (!part) {
                // Same detached-element fallback as KEYSIG above.
                const staff_idx_t staffIdx = clef->track() / VOICES;
                for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                    Part* p = it.key();
                    const staff_idx_t first = p->startTrack() / VOICES;
                    if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                        part = p;
                        break;
                    }
                }
            }
            if (!part) continue;
            const QString partUuid = resolvePartUuid(part, lazyAddPartOps);
            if (partUuid.isEmpty()) continue;
            const staff_idx_t globalStaff = clef->track() / VOICES;
            const staff_idx_t firstStaff  = part->startTrack() / VOICES;
            const int staffIdx = static_cast<int>(globalStaff - firstStaff);
            if (isAdd) {
                ops.append(buildSetClef(clef, partUuid, staffIdx));
            } else {
                // Removal: emit SetClef with null clef.
                QJsonObject payload;
                payload["type"]    = QStringLiteral("SetClef");
                payload["part_id"] = partUuid;
                payload["beat"]    = beatJson(clef->tick());
                payload["staff"]   = staffIdx;
                payload["clef"]    = QJsonValue::Null;
                ops.append(payload);
            }
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
            Part* artPart = cr->staff() ? cr->staff()->part() : nullptr;
            const QString artPartUuid = resolvePartUuid(artPart, lazyAddPartOps);
            if (artPartUuid.isEmpty()) continue;
            ops.append(buildAddArticulation(art, uuid, artPartUuid, eventUuid));
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
        const QString dynPartUuid = resolvePartUuid(dynPart, lazyAddPartOps);
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
            Part* slurPart = startCr->staff() ? startCr->staff()->part() : nullptr;
            const QString slurPartUuid = resolvePartUuid(slurPart, lazyAddPartOps);
            if (slurPartUuid.isEmpty()) continue;
            ops.append(buildAddSlur(slur, uuid, slurPartUuid, startUuid, endUuid));
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
        const QString hpPartUuid = resolvePartUuid(hpPart, lazyAddPartOps);
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
            const QString tupPartUuid = resolvePartUuid(tupPart, lazyAddPartOps);
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
            Part* lyrPart = cr->staff() ? cr->staff()->part() : nullptr;
            const QString lyrPartUuid = resolvePartUuid(lyrPart, lazyAddPartOps);
            if (lyrPartUuid.isEmpty()) continue;
            ops.append(buildAddLyric(lyr, uuid, lyrPartUuid, eventUuid,
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

    // ── Pass 20: Staff Text (part-scoped) ──────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::STAFF_TEXT) {
            continue;
        }
        auto* staffText = static_cast<StaffText*>(obj);
        Part* textPart = static_cast<EngravingItem*>(staffText)->part();
        if (!textPart) {
            // After undo the parent chain may be broken; use track-based fallback.
            const staff_idx_t staffIdx = staffText->track() / VOICES;
            for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                Part* p = it.key();
                const staff_idx_t first = p->startTrack() / VOICES;
                if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                    textPart = p;
                    break;
                }
            }
        }
        if (!textPart) continue;
        const QString textPartUuid = resolvePartUuid(textPart, lazyAddPartOps);
        if (textPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[staffText]  = uuid;
            m_localUuidToElement[uuid] = staffText;
            ops.append(buildAddStaffText(staffText, uuid, textPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(staffText, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveStaffText(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(staffText);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(staffText, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetStaffText(uuid, staffText->plainText().toQString()));
            }
        }
    }

    // ── Pass 21: System Text (score-global) ─────────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::SYSTEM_TEXT) {
            continue;
        }
        auto* sysText = static_cast<SystemText*>(obj);

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[sysText]  = uuid;
            m_localUuidToElement[uuid] = sysText;
            ops.append(buildAddSystemText(sysText, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(sysText, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveSystemText(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(sysText);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(sysText, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetSystemText(uuid, sysText->plainText().toQString()));
            }
        }
    }

    // ── Pass 22: Rehearsal Mark (score-global) ──────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::REHEARSAL_MARK) {
            continue;
        }
        auto* mark = static_cast<RehearsalMark*>(obj);

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[mark]  = uuid;
            m_localUuidToElement[uuid] = mark;
            ops.append(buildAddRehearsalMark(mark, uuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(mark, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveRehearsalMark(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(mark);
            }
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const QString uuid = uuidForElement(mark, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildSetRehearsalMark(uuid, mark->plainText().toQString()));
            }
        }
    }

    // ── Pass 23: Octave Lines (part-scoped, beat-range) ────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::OTTAVA) {
            continue;
        }
        auto* ottava = static_cast<Ottava*>(obj);
        Part* ottavaPart = static_cast<EngravingItem*>(ottava)->part();
        if (!ottavaPart) {
            // After undo the parent chain may be broken; use track-based fallback.
            const staff_idx_t staffIdx = ottava->track() / VOICES;
            for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                Part* p = it.key();
                const staff_idx_t first = p->startTrack() / VOICES;
                if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                    ottavaPart = p;
                    break;
                }
            }
        }
        if (!ottavaPart) continue;
        const QString ottavaPartUuid = resolvePartUuid(ottavaPart, lazyAddPartOps);
        if (ottavaPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[ottava]  = uuid;
            m_localUuidToElement[uuid]    = ottava;
            ops.append(buildAddOctaveLine(ottava, uuid, ottavaPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(ottava, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveOctaveLine(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(ottava);
            }
        }
    }

    // ── Pass 24: Glissandos (event-UUID anchored) ───────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::GLISSANDO) {
            continue;
        }
        auto* gliss = static_cast<Glissando*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            EngravingItem* startEl = gliss->startElement();
            EngravingItem* endEl   = gliss->endElement();
            // Glissandos anchor to Notes, not ChordRests.
            Note* startNote = (startEl && startEl->isNote()) ? toNote(startEl) : nullptr;
            Note* endNote   = (endEl && endEl->isNote())     ? toNote(endEl)   : nullptr;
            if (!startNote || !endNote) continue;
            // Look up the parent chord UUID for each note (same as slur pass).
            ChordRest* startCr = startNote->chord();
            ChordRest* endCr   = endNote->chord();
            if (!startCr || !endCr) continue;
            const QString startUuid = uuidForChordRest(startCr, remoteElementToUuid);
            const QString endUuid   = uuidForChordRest(endCr,   remoteElementToUuid);
            if (startUuid.isEmpty() || endUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddGlissando: start/end UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[gliss]  = uuid;
            m_localUuidToElement[uuid]   = gliss;
            Part* glissPart = startCr->staff() ? startCr->staff()->part() : nullptr;
            const QString glissPartUuid = resolvePartUuid(glissPart, lazyAddPartOps);
            if (glissPartUuid.isEmpty()) continue;
            ops.append(buildAddGlissando(gliss, uuid, glissPartUuid, startUuid, endUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(gliss, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveGlissando(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(gliss);
            }
        }
    }

    // ── Pass 25: Pedal Lines (part-scoped, beat-range) ──────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::PEDAL) {
            continue;
        }
        auto* pedal = static_cast<Pedal*>(obj);
        Part* pedalPart = static_cast<EngravingItem*>(pedal)->part();
        if (!pedalPart) {
            // After undo the parent chain may be broken; use track-based fallback.
            const staff_idx_t staffIdx = pedal->track() / VOICES;
            for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                Part* p = it.key();
                const staff_idx_t first = p->startTrack() / VOICES;
                if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                    pedalPart = p;
                    break;
                }
            }
        }
        if (!pedalPart) continue;
        const QString pedalPartUuid = resolvePartUuid(pedalPart, lazyAddPartOps);
        if (pedalPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[pedal]  = uuid;
            m_localUuidToElement[uuid]   = pedal;
            ops.append(buildAddPedalLine(pedal, uuid, pedalPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(pedal, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemovePedalLine(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(pedal);
            }
        }
    }

    // ── Pass 26: Trill Lines (part-scoped, beat-range) ──────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TRILL) {
            continue;
        }
        auto* trill = static_cast<Trill*>(obj);
        Part* trillPart = static_cast<EngravingItem*>(trill)->part();
        if (!trillPart) {
            // After undo the parent chain may be broken; use track-based fallback.
            const staff_idx_t staffIdx = trill->track() / VOICES;
            for (auto it = m_knownPartUuids.cbegin(); it != m_knownPartUuids.cend(); ++it) {
                Part* p = it.key();
                const staff_idx_t first = p->startTrack() / VOICES;
                if (staffIdx >= first && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
                    trillPart = p;
                    break;
                }
            }
        }
        if (!trillPart) continue;
        const QString trillPartUuid = resolvePartUuid(trillPart, lazyAddPartOps);
        if (trillPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[trill]  = uuid;
            m_localUuidToElement[uuid]   = trill;
            ops.append(buildAddTrillLine(trill, uuid, trillPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(trill, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveTrillLine(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(trill);
            }
        }
    }

    // ── Pass 26b: Arpeggios (event-UUID anchored) ─────────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::ARPEGGIO) {
            continue;
        }
        auto* arp = static_cast<Arpeggio*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Chord* ch = arp->chord();
            if (!ch) continue;
            const QString eventUuid = uuidForChordRest(ch, remoteElementToUuid);
            if (eventUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddArpeggio: parent chord UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[arp]  = uuid;
            m_localUuidToElement[uuid] = arp;
            Part* arpPart = ch->staff() ? ch->staff()->part() : nullptr;
            const QString arpPartUuid = resolvePartUuid(arpPart, lazyAddPartOps);
            if (arpPartUuid.isEmpty()) continue;
            ops.append(buildAddArpeggio(arp, uuid, arpPartUuid, eventUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(arp, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveArpeggio(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(arp);
            }
        }
    }

    // ── Pass 26c: Grace notes (event-UUID anchored) ───────────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::CHORD) {
            continue;
        }
        auto* chord = static_cast<Chord*>(obj);
        if (!chord->isGrace()) {
            continue;
        }
        if (cmds.count(CommandType::AddElement)) {
            Chord* parentChord = toChord(chord->explicitParent());
            if (!parentChord) continue;
            const QString eventUuid = uuidForChordRest(parentChord, remoteElementToUuid);
            if (eventUuid.isEmpty()) {
                LOGD() << "[editude] translateAll: AddGraceNote: parent chord UUID unknown, skipping";
                continue;
            }
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[chord]  = uuid;
            m_localUuidToElement[uuid]   = chord;
            Part* gnPart = chord->staff() ? chord->staff()->part() : nullptr;
            const QString gnPartUuid = resolvePartUuid(gnPart, lazyAddPartOps);
            if (gnPartUuid.isEmpty()) continue;
            ops.append(buildAddGraceNote(chord, uuid, gnPartUuid, eventUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(chord, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveGraceNote(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(chord);
            }
        }
    }

    // ── Pass 26d: Breath marks / caesuras (beat-anchored) ────────────────
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::BREATH) {
            continue;
        }
        auto* breath = static_cast<Breath*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Segment* seg = breath->segment();
            if (!seg) continue;
            Part* bPart = breath->part();
            const QString bPartUuid = resolvePartUuid(bPart, lazyAddPartOps);
            if (bPartUuid.isEmpty()) continue;
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_localElementToUuid[breath] = uuid;
            m_localUuidToElement[uuid]   = breath;
            ops.append(buildAddBreathMark(breath, uuid, bPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const QString uuid = uuidForElement(breath, remoteElementToUuid);
            if (!uuid.isEmpty()) {
                ops.append(buildRemoveBreathMark(uuid));
                m_localUuidToElement.remove(uuid);
                m_localElementToUuid.remove(breath);
            }
        }
    }

    // ── Pass 27: InsertBeats / DeleteBeats ───────────────────────────────
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

    // ── Pass 28: InsertVolta / RemoveVolta ───────────────────────────────
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

    // ── Pass 29: InsertMarker / RemoveMarker ─────────────────────────────
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

    // ── Pass 30: InsertJump / RemoveJump ─────────────────────────────────
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

    // ── Pass 31: SetScoreMetadata ─────────────────────────────────────────
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

    // Prepend any lazily-generated AddPart ops so the server registers parts
    // before processing the element ops that reference them.
    if (!lazyAddPartOps.isEmpty()) {
        QVector<QJsonObject> result = lazyAddPartOps;
        result.append(ops);
        return result;
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

    // Tab fields: include fret/string if the note carries tab data.
    if (note->fret() >= 0 && note->string() >= 0) {
        payload["fret"]   = note->fret();
        payload["string"] = note->string();
    }

    // Percussion: include notehead if non-default.
    if (note->headGroup() != NoteHeadGroup::HEAD_NORMAL) {
        payload["notehead"] = noteheadGroupToString(note->headGroup());
    }
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

QJsonObject OperationTranslator::buildSetTabNote(const QString& uuid, Note* note)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetTabNote");
    payload["event_id"] = uuid;
    payload["fret"]     = note->fret();
    payload["string"]   = note->string();
    return payload;
}

QJsonObject OperationTranslator::buildSetNoteHead(const QString& uuid, Note* note)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetNoteHead");
    payload["event_id"] = uuid;
    payload["notehead"] = noteheadGroupToString(note->headGroup());
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
    const Fraction r = tick.reduced();
    QJsonObject beat;
    beat["numerator"]   = r.numerator();
    beat["denominator"] = r.denominator();
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
    instr["musescore_id"] = part->instrumentId().toQString();
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
    instr["musescore_id"] = part->instrumentId().toQString();
    instr["name"]         = part->longName().toQString();
    instr["short_name"]   = part->shortName().toQString();

    // Serialise StringData if the instrument has fretted-instrument string data.
    const Instrument* inst = part->instrument();
    if (inst) {
        const StringData* sd = inst->stringData();
        if (sd && !sd->stringList().empty()) {
            QJsonArray strings;
            for (const instrString& s : sd->stringList()) {
                QJsonObject sObj;
                sObj["pitch"]      = s.pitch;
                sObj["open"]       = s.open;
                sObj["start_fret"] = s.startFret;
                strings.append(sObj);
            }
            QJsonObject sdObj;
            sdObj["frets"]   = sd->frets();
            sdObj["strings"] = strings;
            instr["string_data"] = sdObj;
        }

        // Percussion: serialize use_drumset and drumset_overrides.
        instr["use_drumset"] = inst->useDrumset();
        if (inst->useDrumset() && inst->drumset()) {
            const Drumset* ds = inst->drumset();
            QJsonObject instruments;
            for (int pitch = 0; pitch < 128; ++pitch) {
                if (!ds->isValid(pitch)) {
                    continue;
                }
                QJsonObject entry;
                entry["name"]           = ds->name(pitch).toQString();
                entry["notehead"]       = noteheadGroupToString(ds->noteHead(pitch));
                entry["line"]           = ds->line(pitch);
                entry["stem_direction"] = stemDirectionToString(ds->stemDirection(pitch));
                entry["voice"]          = ds->voice(pitch);
                entry["shortcut"]       = ds->shortcut(pitch).toQString();

                // Serialize variants for this pitch.
                const auto& variants = ds->variants(pitch);
                if (!variants.empty()) {
                    QJsonArray varArr;
                    for (const DrumInstrumentVariant& v : variants) {
                        QJsonObject vObj;
                        vObj["pitch"] = v.pitch;
                        if (v.tremolo != TremoloType::INVALID_TREMOLO) {
                            vObj["tremolo_type"] = tremoloTypeToString(v.tremolo);
                        }
                        if (!v.articulationName.isEmpty()) {
                            vObj["articulation_name"] = v.articulationName.toQString();
                        }
                        varArr.append(vObj);
                    }
                    entry["variants"] = varArr;
                }

                instruments[QString::number(pitch)] = entry;
            }
            QJsonObject dsObj;
            dsObj["instruments"] = instruments;
            instr["drumset_overrides"] = dsObj;
        }
    }

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
        { ClefType::G,       QStringLiteral("treble")       },
        { ClefType::G8_VB,   QStringLiteral("treble_8vb")   },
        { ClefType::G8_VA,   QStringLiteral("treble_8va")   },
        { ClefType::G15_MB,  QStringLiteral("treble_15mb")  },
        { ClefType::G15_MA,  QStringLiteral("treble_15ma")  },
        { ClefType::F,       QStringLiteral("bass")         },
        { ClefType::F8_VB,   QStringLiteral("bass_8vb")     },
        { ClefType::F_8VA,   QStringLiteral("bass_8va")     },
        { ClefType::F15_MB,  QStringLiteral("bass_15mb")    },
        { ClefType::F_15MA,  QStringLiteral("bass_15ma")    },
        { ClefType::C3,      QStringLiteral("alto")         },
        { ClefType::C4,      QStringLiteral("tenor")        },
        { ClefType::C2,      QStringLiteral("mezzo_soprano") },
        { ClefType::C1,      QStringLiteral("soprano")      },
        { ClefType::C5,      QStringLiteral("baritone")     },
        { ClefType::PERC,    QStringLiteral("percussion")   },
        { ClefType::TAB,     QStringLiteral("tab")          },
        { ClefType::TAB4,    QStringLiteral("tab4")         },
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

QJsonObject OperationTranslator::buildAddArticulation(EngravingObject* art,
                                                       const QString& uuid,
                                                       const QString& partId,
                                                       const QString& eventUuid)
{
    auto* a = static_cast<Articulation*>(art);
    QJsonObject payload;
    payload["type"]         = QStringLiteral("AddArticulation");
    payload["id"]           = uuid;
    payload["part_id"]      = partId;
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
                                               const QString& partId,
                                               const QString& startEventUuid,
                                               const QString& endEventUuid)
{
    QJsonObject payload;
    payload["type"]           = QStringLiteral("AddSlur");
    payload["id"]             = uuid;
    payload["part_id"]        = partId;
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
                                                const QString& partId,
                                                const QString& eventUuid,
                                                int verse,
                                                const QString& syllabic,
                                                const QString& text)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("AddLyric");
    payload["id"]       = uuid;
    payload["part_id"]  = partId;
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
// Tier 3 builders — staff text
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddStaffText(EngravingObject* text,
                                                    const QString& uuid,
                                                    const QString& partId)
{
    auto* t = static_cast<StaffText*>(text);
    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddStaffText");
    payload["id"]      = uuid;
    payload["part_id"] = partId;
    payload["text"]    = t->plainText().toQString();
    payload["beat"]    = beatJson(t->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetStaffText(const QString& uuid, const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetStaffText");
    payload["id"]   = uuid;
    payload["text"] = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveStaffText(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveStaffText");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — system text
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddSystemText(EngravingObject* text,
                                                     const QString& uuid)
{
    auto* t = static_cast<SystemText*>(text);
    QJsonObject payload;
    payload["type"] = QStringLiteral("AddSystemText");
    payload["id"]   = uuid;
    payload["text"] = t->plainText().toQString();
    payload["beat"] = beatJson(t->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetSystemText(const QString& uuid, const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetSystemText");
    payload["id"]   = uuid;
    payload["text"] = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveSystemText(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveSystemText");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — rehearsal mark
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddRehearsalMark(EngravingObject* mark,
                                                        const QString& uuid)
{
    auto* m = static_cast<RehearsalMark*>(mark);
    QJsonObject payload;
    payload["type"] = QStringLiteral("AddRehearsalMark");
    payload["id"]   = uuid;
    payload["text"] = m->plainText().toQString();
    payload["beat"] = beatJson(m->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetRehearsalMark(const QString& uuid, const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetRehearsalMark");
    payload["id"]   = uuid;
    payload["text"] = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveRehearsalMark(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveRehearsalMark");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — octave lines
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddOctaveLine(EngravingObject* ottava,
                                                      const QString& uuid,
                                                      const QString& partId)
{
    auto* o = static_cast<Ottava*>(ottava);

    QString kind;
    switch (o->ottavaType()) {
    case OttavaType::OTTAVA_8VA:  kind = QStringLiteral("8va");  break;
    case OttavaType::OTTAVA_8VB:  kind = QStringLiteral("8vb");  break;
    case OttavaType::OTTAVA_15MA: kind = QStringLiteral("15ma"); break;
    case OttavaType::OTTAVA_15MB: kind = QStringLiteral("15mb"); break;
    case OttavaType::OTTAVA_22MA: kind = QStringLiteral("22ma"); break;
    case OttavaType::OTTAVA_22MB: kind = QStringLiteral("22mb"); break;
    default:                      kind = QStringLiteral("8va");  break;
    }

    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddOctaveLine");
    payload["id"]         = uuid;
    payload["part_id"]    = partId;
    payload["kind"]       = kind;
    payload["start_beat"] = beatJson(o->tick());
    payload["end_beat"]   = beatJson(o->tick2());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveOctaveLine(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveOctaveLine");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — glissandos
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddGlissando(EngravingObject* gliss,
                                                     const QString& uuid,
                                                     const QString& partId,
                                                     const QString& startEventUuid,
                                                     const QString& endEventUuid)
{
    auto* g = static_cast<Glissando*>(gliss);

    const QString style = (g->glissandoType() == GlissandoType::WAVY)
                          ? QStringLiteral("wavy")
                          : QStringLiteral("straight");

    QJsonObject payload;
    payload["type"]           = QStringLiteral("AddGlissando");
    payload["id"]             = uuid;
    payload["part_id"]        = partId;
    payload["start_event_id"] = startEventUuid;
    payload["end_event_id"]   = endEventUuid;
    payload["style"]          = style;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveGlissando(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveGlissando");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — pedal lines
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddPedalLine(EngravingObject* pedal,
                                                     const QString& uuid,
                                                     const QString& partId)
{
    auto* p = static_cast<Pedal*>(pedal);
    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddPedalLine");
    payload["id"]         = uuid;
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(p->tick());
    payload["end_beat"]   = beatJson(p->tick2());
    return payload;
}

QJsonObject OperationTranslator::buildRemovePedalLine(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemovePedalLine");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — trill lines
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTrillLine(EngravingObject* trill,
                                                     const QString& uuid,
                                                     const QString& partId)
{
    auto* t = static_cast<Trill*>(trill);

    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddTrillLine");
    payload["id"]         = uuid;
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(t->tick());
    payload["end_beat"]   = beatJson(t->tick2());

    // Include accidental if the trill has one.
    if (t->accidental()) {
        AccidentalType at = t->accidental()->accidentalType();
        QString accName;
        switch (at) {
        case AccidentalType::FLAT:         accName = QStringLiteral("flat");         break;
        case AccidentalType::SHARP:        accName = QStringLiteral("sharp");        break;
        case AccidentalType::NATURAL:      accName = QStringLiteral("natural");      break;
        case AccidentalType::FLAT2:        accName = QStringLiteral("double-flat");  break;
        case AccidentalType::SHARP2:       accName = QStringLiteral("double-sharp"); break;
        default:                           accName = QString();                      break;
        }
        if (!accName.isEmpty()) {
            payload["accidental"] = accName;
        } else {
            payload["accidental"] = QJsonValue::Null;
        }
    } else {
        payload["accidental"] = QJsonValue::Null;
    }

    return payload;
}

QJsonObject OperationTranslator::buildRemoveTrillLine(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveTrillLine");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — arpeggios
// ---------------------------------------------------------------------------

static QString arpeggioTypeName(ArpeggioType t)
{
    switch (t) {
    case ArpeggioType::NORMAL:        return QStringLiteral("normal");
    case ArpeggioType::UP:            return QStringLiteral("up");
    case ArpeggioType::DOWN:          return QStringLiteral("down");
    case ArpeggioType::BRACKET:       return QStringLiteral("bracket");
    case ArpeggioType::UP_STRAIGHT:   return QStringLiteral("up_straight");
    case ArpeggioType::DOWN_STRAIGHT: return QStringLiteral("down_straight");
    }
    return QStringLiteral("normal");
}

QJsonObject OperationTranslator::buildAddArpeggio(EngravingObject* arp,
                                                    const QString& uuid,
                                                    const QString& partId,
                                                    const QString& eventUuid)
{
    auto* a = static_cast<Arpeggio*>(arp);
    QJsonObject payload;
    payload["type"]      = QStringLiteral("AddArpeggio");
    payload["id"]        = uuid;
    payload["part_id"]   = partId;
    payload["event_id"]  = eventUuid;
    payload["direction"] = arpeggioTypeName(a->arpeggioType());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveArpeggio(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveArpeggio");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Grace note builders
// ---------------------------------------------------------------------------

static QString graceNoteTypeName(NoteType nt)
{
    static const QHash<NoteType, QString> s_map = {
        { NoteType::ACCIACCATURA,  QStringLiteral("acciaccatura") },
        { NoteType::APPOGGIATURA,  QStringLiteral("appoggiatura") },
        { NoteType::GRACE4,        QStringLiteral("grace4") },
        { NoteType::GRACE16,       QStringLiteral("grace16") },
        { NoteType::GRACE32,       QStringLiteral("grace32") },
        { NoteType::GRACE8_AFTER,  QStringLiteral("grace8_after") },
        { NoteType::GRACE16_AFTER, QStringLiteral("grace16_after") },
        { NoteType::GRACE32_AFTER, QStringLiteral("grace32_after") },
    };
    return s_map.value(nt, QStringLiteral("acciaccatura"));
}

QJsonObject OperationTranslator::buildAddGraceNote(EngravingObject* graceObj,
                                                    const QString& uuid,
                                                    const QString& partId,
                                                    const QString& eventUuid)
{
    auto* gc = static_cast<Chord*>(graceObj);
    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddGraceNote");
    payload["id"]         = uuid;
    payload["part_id"]    = partId;
    payload["event_id"]   = eventUuid;
    payload["order"]      = static_cast<int>(gc->graceIndex());
    payload["grace_type"] = graceNoteTypeName(gc->noteType());

    // Pitch from first note in the grace chord.
    if (!gc->notes().empty()) {
        payload["pitch"] = pitchJson(gc->notes().front()->tpc(),
                                     gc->notes().front()->octave());
    }

    return payload;
}

QJsonObject OperationTranslator::buildRemoveGraceNote(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveGraceNote");
    payload["id"]   = uuid;
    return payload;
}

// ---------------------------------------------------------------------------
// Breath mark / caesura builders
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddBreathMark(EngravingObject* breathObj,
                                                     const QString& uuid,
                                                     const QString& partId)
{
    auto* b = static_cast<Breath*>(breathObj);
    QJsonObject payload;
    payload["type"]        = QStringLiteral("AddBreathMark");
    payload["id"]          = uuid;
    payload["part_id"]     = partId;
    payload["beat"]        = beatJson(b->segment()->tick());
    payload["breath_type"] = breathTypeToString(b->symId());
    payload["pause"]       = b->pause();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveBreathMark(const QString& uuid)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveBreathMark");
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
