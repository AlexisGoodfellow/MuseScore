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
#include "engraving/dom/guitarbend.h"
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
#include "engraving/dom/tremolosinglechord.h"
#include "engraving/dom/tremolotwochord.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/systemtext.h"
#include "engraving/dom/volta.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/rest.h"
#include "engraving/editing/editproperty.h"
#include "engraving/types/bps.h"
#include "engraving/types/fraction.h"

#include "log.h"

using namespace mu::editude::internal;
using namespace mu::engraving;

#include "editudeutils.h"

// Forward declaration for file-scope helper used by translateAll.
static QString lyricSyllabicName(LyricsSyllabic s);

// ---------------------------------------------------------------------------
// File-scope helper: find old MIDI pitch for a Note from the undo stack.
//
// After a pitch change is committed, the Note object already holds the NEW
// pitch.  To build a coordinate-addressed SetPitch op we need the OLD pitch
// (which identifies the note before the change).  The undo stack's last
// macro contains ChangeProperty commands that store the old value after
// flip().  We walk those entries to find the old MIDI pitch for the given
// note.
//
// Returns -1 if the old pitch cannot be determined.
// ---------------------------------------------------------------------------
static int findOldMidiPitch(Note* note)
{
    if (!note || !note->score()) {
        return -1;
    }
    const UndoStack* undoStack = note->score()->undoStack();
    if (!undoStack) {
        return -1;
    }
    // Try last() first (forward path).  After UndoStack::undo() decrements
    // m_currentIndex, the undone macro sits at next() rather than last(),
    // so fall back to next() for the undo path.
    for (const UndoMacro* macro : { undoStack->last(), undoStack->next() }) {
        if (!macro) {
            continue;
        }
        for (size_t i = 0; i < macro->childCount(); ++i) {
            const UndoCommand* cmd = macro->commands()[i];
            if (cmd->type() != CommandType::ChangeProperty) {
                continue;
            }
            const auto* cp = static_cast<const ChangeProperty*>(cmd);
            if (cp->getElement() == note && cp->getId() == Pid::PITCH) {
                return cp->data().toInt();
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// File-scope helper: find old voice (as wire value 1-4) for an item from the
// undo stack.  Returns -1 if not found.
// ---------------------------------------------------------------------------
static int findOldVoice(EngravingItem* item, Part* /*part*/)
{
    if (!item || !item->score()) {
        return -1;
    }
    const UndoStack* undoStack = item->score()->undoStack();
    if (!undoStack) {
        return -1;
    }
    // Try last() first (forward path), then next() (undo path).
    for (const UndoMacro* macro : { undoStack->last(), undoStack->next() }) {
        if (!macro) {
            continue;
        }
        for (size_t i = 0; i < macro->childCount(); ++i) {
            const UndoCommand* cmd = macro->commands()[i];
            if (cmd->type() != CommandType::ChangeProperty) {
                continue;
            }
            const auto* cp = static_cast<const ChangeProperty*>(cmd);
            if (cp->getElement() != item) {
                continue;
            }
            if (cp->getId() == Pid::VOICE) {
                return cp->data().toInt() + 1; // convert to wire 1-4
            }
            if (cp->getId() == Pid::TRACK) {
                // Track = staffIdx * VOICES + voice.  Extract voice component.
                return static_cast<int>(cp->data().value<track_idx_t>() % VOICES) + 1;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// File-scope helper: find old DurationTypeWithDots for a ChordRest from the
// undo stack.
//
// After a duration change is committed, the ChordRest already holds the NEW
// duration.  The undo stack's last macro contains a ChangeProperty command
// for Pid::DURATION_TYPE_WITH_DOTS that stores the old value after flip().
//
// Returns DurationTypeWithDots with V_INVALID if not found.
// ---------------------------------------------------------------------------
static DurationTypeWithDots findOldDuration(ChordRest* cr)
{
    DurationTypeWithDots invalid;
    if (!cr || !cr->score()) {
        return invalid;
    }
    const UndoStack* undoStack = cr->score()->undoStack();
    if (!undoStack) {
        return invalid;
    }
    // Try last() first (forward path), then next() (undo path).
    for (const UndoMacro* macro : { undoStack->last(), undoStack->next() }) {
        if (!macro) {
            continue;
        }
        for (size_t i = 0; i < macro->childCount(); ++i) {
            const UndoCommand* cmd = macro->commands()[i];
            if (cmd->type() != CommandType::ChangeProperty) {
                continue;
            }
            const auto* cp = static_cast<const ChangeProperty*>(cmd);
            if (cp->getElement() == cr
                && cp->getId() == Pid::DURATION_TYPE_WITH_DOTS) {
                return cp->data().value<DurationTypeWithDots>();
            }
        }
    }
    return invalid;
}

// ---------------------------------------------------------------------------
// File-scope helper: build pitch JSON from a MIDI pitch value.
// Uses the note's TPC if we can reverse-engineer it; otherwise builds from
// the MIDI pitch assuming no accidental (natural spelling).
// ---------------------------------------------------------------------------
static QJsonObject pitchJsonFromMidi(int midi)
{
    // Compute natural spelling from MIDI pitch.
    // MIDI pitch → step/octave using standard chromatic mapping.
    static const char* const kSteps[]  = { "C", "C", "D", "D", "E", "F",
                                           "F", "G", "G", "A", "A", "B" };
    static const char* const kAccs[]   = { nullptr, "sharp", nullptr, "sharp",
                                           nullptr, nullptr, "sharp", nullptr,
                                           "sharp", nullptr, "sharp", nullptr };
    const int pc = midi % 12;
    const int octave = (midi / 12) - 1;

    QJsonObject pitch;
    pitch["step"]   = QString::fromLatin1(kSteps[pc]);
    pitch["octave"] = octave;
    if (kAccs[pc]) {
        pitch["accidental"] = QString::fromLatin1(kAccs[pc]);
    } else {
        pitch["accidental"] = QJsonValue(QJsonValue::Null);
    }
    return pitch;
}

// ---------------------------------------------------------------------------
// File-scope helper: resolve Part* from track when element->part() is null.
// After undo the parent chain may be broken; use track-based lookup through
// m_knownPartUuids whose Part* pointers remain live in the score.
// ---------------------------------------------------------------------------
static Part* resolvePartFromTrack(track_idx_t track,
                                  const QHash<Part*, QString>& knownParts)
{
    const staff_idx_t staffIdx = track / VOICES;
    for (auto it = knownParts.cbegin(); it != knownParts.cend(); ++it) {
        Part* p = it.key();
        const staff_idx_t first = p->startTrack() / VOICES;
        if (staffIdx >= first
            && staffIdx < first + static_cast<staff_idx_t>(p->nstaves())) {
            return p;
        }
    }
    return nullptr;
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
// translateAll — two-phase pipeline (tier-based architecture)
//
// Phase 1: Compound-op detection — identifies operations that produce side
//          effects and claims those objects so Phase 2 doesn't re-emit them.
// Phase 2: Per-tier emission — iterates unclaimed objects grouped by tier.
//
// See ADR 2026-04-05-translator-tier-architecture.md for rationale.
// ---------------------------------------------------------------------------
QVector<QJsonObject> OperationTranslator::translateAll(
    const std::map<EngravingObject*, std::unordered_set<CommandType>>& changedObjects,
    const PropertyIdSet& changedPropertyIdSet,
    const QMap<QString, QString>& changedMetaTags)
{
    QVector<QJsonObject> ops;
    // AddPart ops for MSCX-loaded parts discovered lazily during this batch.
    // These must be prepended to ops so the server knows about the parts before
    // the element ops that reference them.
    QVector<QJsonObject> lazyAddPartOps;

    // Unified set of objects claimed by compound-op detection (Phase 1).
    // Phase 2 skips any object in this set to avoid double-emission.
    QSet<EngravingObject*> claimed;

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 1: Compound-op detection and side-effect claiming
    //
    // Detector ordering is fixed and load-bearing:
    //   1a. Note grouping by chord (data structure for Stream tier)
    //   1b. InsertBeats/DeleteBeats — claims TimeSig replications
    //   1c. AddPart/RemovePart — must precede SetTimeSignature early return
    //   1d. SetTimeSignature — early return claims entire batch
    //   1e. SetTempo — independent, no side effects
    //   1f. Duration side effects — claims fill rests
    //   1g. Structural voice changes — claims remove+add pattern
    //   1h. Tuplet operations — fullyRemovedTuplets, newlyCreatedTuplets
    //   1i. Undo-of-InsertRest heuristic
    //   1j. hasMeasureLenChange flag
    // ═══════════════════════════════════════════════════════════════════

    // ── 1a. Group newly-added Notes by parent Chord ──────────────────
    //
    // A Note with AddElement whose parent Chord also has AddElement is part
    // of a new chord insertion (InsertNote).
    // A Note with AddElement whose parent Chord does NOT have AddElement is
    // a pitch added to an existing chord — also emitted as InsertNote.
    QHash<Chord*, QVector<Note*>> newChordNotes;
    QSet<EngravingObject*> notesAddedToExistingChords;
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
        // Skip grace chords — handled by AddGraceNote in Stream tier.
        if (chord->isGrace()) {
            continue;
        }
        auto chordIt = changedObjects.find(chord);
        if (chordIt != changedObjects.end()
            && chordIt->second.count(CommandType::AddElement)) {
            newChordNotes[chord].append(note);
        } else {
            notesAddedToExistingChords.insert(obj);
        }
    }

    // ── 1b. InsertBeats / DeleteBeats ────────────────────────────────
    // Detect InsertMeasures/RemoveMeasures and emit structural ops.
    // When measures are inserted, MuseScore replicates the time signature
    // to new measures via AddElement on TimeSig objects.  These are side
    // effects — claim them so SetTimeSignature (1d) doesn't fire.
    //
    // IMPORTANT: When a genuine time signature change triggers
    // rewriteMeasures, InsertMeasures/RemoveMeasures appear as side
    // effects.  These must NOT be emitted as InsertBeats/DeleteBeats —
    // the receiver will call its own rewriteMeasures when it processes
    // SetTimeSignature.  Detect this by checking for ChangeProperty on
    // any TimeSig object (genuine time sig changes modify the existing
    // TimeSig via ChangeProperty; replications from measure insertion
    // only use AddElement).
    bool batchHasTimeSigPropertyChange = false;
    for (const auto& [obj, cmds] : changedObjects) {
        if (obj && obj->type() == ElementType::TIMESIG
            && cmds.count(CommandType::ChangeProperty)) {
            batchHasTimeSigPropertyChange = true;
            break;
        }
    }

    bool batchHasMeasureInsert = false;
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

        // If a genuine time sig change is present, InsertMeasures/
        // RemoveMeasures are from rewriteMeasures — don't emit structural
        // ops and don't claim TimeSigs.  Phase 1d will handle everything.
        if (!batchHasTimeSigPropertyChange) {
            batchHasMeasureInsert = !insertedMeasures.isEmpty();

            // Claim all TimeSig objects in this batch — they are replications
            // from the measure insertion, not genuine time signature changes.
            if (batchHasMeasureInsert || !removedMeasures.isEmpty()) {
                for (const auto& [obj, cmds] : changedObjects) {
                    if (obj && obj->type() == ElementType::TIMESIG) {
                        claimed.insert(obj);
                    }
                }
            }

            auto sortByTick = [](Measure* a, Measure* b) {
                return a->tick() < b->tick();
            };

            if (!insertedMeasures.isEmpty()) {
                std::sort(insertedMeasures.begin(), insertedMeasures.end(),
                          sortByTick);
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
                std::sort(removedMeasures.begin(), removedMeasures.end(),
                          sortByTick);
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
    }

    // ── 1c. AddPart / RemovePart ─────────────────────────────────────
    // Must run BEFORE the SetTimeSignature early return (1d) because
    // adding a part creates staves whose TimeSig/KeySig/Clef elements
    // appear in the same batch.  Without this, the early return would
    // suppress the AddPart op entirely.
    //
    // Track newly-added Parts: Parts tier must skip SetStaffCount for
    // these because AddPart already carries the staff_count field.
    QSet<Part*> newlyAddedParts;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::PART) {
            continue;
        }
        Part* part = static_cast<Part*>(obj);
        if (cmds.count(CommandType::AddElement)
            || cmds.count(CommandType::InsertPart)) {
            const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_knownPartUuids[part] = uuid;
            ops.append(buildAddPart(part, uuid));
            newlyAddedParts.insert(part);
        } else if (cmds.count(CommandType::RemoveElement)
                   || cmds.count(CommandType::RemovePart)) {
            const QString uuid = m_knownPartUuids.value(part);
            if (!uuid.isEmpty()) {
                m_knownPartUuids.remove(part);
                ops.append(buildRemovePart(uuid));
            }
        }
    }

    // ── 1d. SetTimeSignature ─────────────────────────────────────────
    // A TimeSig may appear with AddElement (new time sig added) OR
    // ChangeProperty (existing time sig modified by cmdAddTimeSig).
    //
    // Important: when a staff is added (InsertStaff/InsertPart),
    // MuseScore's cmdAddStaves replicates the existing TimeSig to the
    // new staff via undoAddElement.  This is a REPLICATION, not a
    // genuine time signature change.  We must NOT emit SetTimeSignature
    // or trigger the compound-op early return for these replications.
    //
    // Objects already claimed by 1b (InsertBeats) are also skipped.
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
        // Skip objects already claimed by InsertBeats (1b).
        if (claimed.contains(obj)) {
            continue;
        }
        if (obj->type() == ElementType::TIMESIG) {
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
    }

    // SetTimeSignature is a compound op: cmdAddTimeSig on the receiver
    // calls rewriteMeasures which restructures measure boundaries,
    // creates/removes rests, etc.  All other changes in this batch are
    // side effects and must NOT be emitted.
    if (emittedTimeSig) {
        if (!lazyAddPartOps.isEmpty()) {
            QVector<QJsonObject> result = lazyAddPartOps;
            result.append(ops);
            return result;
        }
        return ops;
    }

    // ── 1e. SetTempo ─────────────────────────────────────────────────
    // SetTempo has no side effects — emit it independently.
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TEMPO_TEXT) {
            continue;
        }
        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildSetTempo(static_cast<TempoText*>(obj)));
        }
    }

    // ── 1f. Duration side effects ────────────────────────────────────
    // changeCRlen() modifies a ChordRest's duration (via ChangeProperty
    // on Pid::DURATION_TYPE_WITH_DOTS) and creates fill rests / removes
    // overwritten elements as side effects.  The C++ applicator handles
    // all these side effects atomically via setNoteRest(), so emitting
    // separate InsertRest/DeleteNote ops for them would cause double-
    // application.  Claim them so Phase 2 skips them.
    //
    // Skip when a ChangeMeasureLen is present: adjustToLen() changes
    // rest durations as a side effect of the measure resize, and the
    // Navigation tier emits the SetMeasureLen op.
    bool hasMeasureLenChange = false;
    for (const auto& [obj, cmds] : changedObjects) {
        if (obj && obj->type() == ElementType::MEASURE
            && cmds.count(CommandType::ChangeMeasureLen)) {
            hasMeasureLenChange = true;
            break;
        }
    }
    if (changedPropertyIdSet.count(Pid::DURATION_TYPE_WITH_DOTS)
        && !hasMeasureLenChange) {
        // Find the target ChordRest's track.
        track_idx_t durationChangeTrack = muse::nidx;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            if (obj->type() != ElementType::CHORD
                && obj->type() != ElementType::REST) {
                continue;
            }
            auto* cr = static_cast<ChordRest*>(obj);
            if (findOldDuration(cr).type != DurationType::V_INVALID) {
                durationChangeTrack = cr->track();
                break;
            }
        }
        // Claim all AddElement/RemoveElement at the same track — they
        // are guaranteed side effects of changeCRlen's atomic operation.
        if (durationChangeTrack != muse::nidx) {
            for (const auto& [obj, cmds] : changedObjects) {
                if (!obj) {
                    continue;
                }
                const bool isAdd = cmds.count(CommandType::AddElement);
                const bool isRem = cmds.count(CommandType::RemoveElement);
                if (!isAdd && !isRem) {
                    continue;
                }
                if (obj->type() == ElementType::NOTE
                    || obj->type() == ElementType::CHORD
                    || obj->type() == ElementType::REST) {
                    auto* item = static_cast<EngravingItem*>(obj);
                    if (item->track() == durationChangeTrack) {
                        claimed.insert(obj);
                    }
                }
            }
        }
    }

    // ── 1g. Structural voice changes ─────────────────────────────────
    // changeSelectedElementsVoice() removes a chord from one voice and
    // creates a copy of its note(s) at another voice.  The undo entries
    // are structural (AddElement/RemoveElement), not ChangeProperty, so
    // the property-based SetVoice handler won't fire.  Detect this
    // pattern to emit SetVoice instead of decomposed InsertNote +
    // DeleteNote — the decomposed ops don't replicate the fill rests
    // changeSelectedElementsVoice creates.
    struct StructuralVoiceChange {
        Note* newNote;
        Chord* oldChord;
        int oldVoice;
        QString partUuid;
    };
    QVector<StructuralVoiceChange> structuralVoiceChanges;
    {
        QVector<Chord*> removedChords;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::RemoveElement)) continue;
            if (obj->type() != ElementType::CHORD) continue;
            auto* ch = static_cast<Chord*>(obj);
            if (ch->isGrace()) continue;
            removedChords.append(ch);
        }
        QVector<Note*> addedNotes;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::AddElement)) continue;
            if (obj->type() != ElementType::NOTE) continue;
            addedNotes.append(static_cast<Note*>(obj));
        }
        for (Chord* oldChord : removedChords) {
            for (Note* newNote : addedNotes) {
                Chord* newChord = newNote->chord();
                if (!newChord) continue;
                if (newChord->tick() != oldChord->tick()) continue;
                if (newChord->staffIdx() != oldChord->staffIdx()) continue;
                if (newChord->voice() == oldChord->voice()) continue;

                bool pitchMatch = false;
                for (Note* oldNote : oldChord->notes()) {
                    if (newNote->pitch() == oldNote->pitch()) {
                        pitchMatch = true;
                        break;
                    }
                }
                if (!pitchMatch) continue;

                Part* notePart = newNote->staff()
                                 ? newNote->staff()->part() : nullptr;
                if (!notePart) continue;
                const QString partUuid =
                    resolvePartUuid(notePart, lazyAddPartOps);
                if (partUuid.isEmpty()) continue;

                int oldVoice = voiceFromTrack(notePart, oldChord->track());
                structuralVoiceChanges.append(
                    {newNote, oldChord, oldVoice, partUuid});

                // Claim all related elements from Phase 2.
                claimed.insert(newNote);
                claimed.insert(newChord);
                claimed.insert(oldChord);
                for (Note* n : oldChord->notes()) {
                    claimed.insert(n);
                }
                for (const auto& [robj, rcmds] : changedObjects) {
                    if (!robj || robj->type() != ElementType::REST) continue;
                    auto* rest = static_cast<Rest*>(robj);
                    if (rest->staffIdx() != oldChord->staffIdx()) continue;
                    claimed.insert(robj);
                }
                break;
            }
        }
    }

    // ── 1h. Tuplet operations ────────────────────────────────────────
    // Detect fully-removed and newly-created tuplets.  These feed the
    // Stream tier (RemoveTuplet, AddTuplet, InsertRest suppression,
    // DeleteNote suppression).

    // Fully removed tuplets: cmdDeleteTuplet removes all member
    // ChordRests, emptying the Tuplet's elements() list.  The Tuplet
    // object itself does NOT get a RemoveElement entry.
    QSet<Tuplet*> fullyRemovedTuplets;
    QHash<Tuplet*, QVector<EngravingObject*>> removedTupletMembers;
    {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::RemoveElement)) {
                continue;
            }
            if (obj->type() != ElementType::REST
                && obj->type() != ElementType::CHORD) {
                continue;
            }
            auto* cr = static_cast<ChordRest*>(obj);
            Tuplet* tup = cr->tuplet();
            if (!tup) {
                continue;
            }
            removedTupletMembers[tup].append(const_cast<EngravingObject*>(obj));
        }
        for (auto it = removedTupletMembers.cbegin();
             it != removedTupletMembers.cend(); ++it) {
            if (it.key()->elements().empty()) {
                fullyRemovedTuplets.insert(it.key());
            }
        }
    }

    // Newly created tuplets: cmdCreateTuplet adds the Tuplet object via
    // undoAddElement, producing an AddElement entry with TUPLET type.
    QSet<Tuplet*> newlyCreatedTuplets;
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || !cmds.count(CommandType::AddElement)) {
            continue;
        }
        if (obj->type() == ElementType::TUPLET) {
            newlyCreatedTuplets.insert(
                static_cast<Tuplet*>(const_cast<EngravingObject*>(obj)));
        }
    }

    // ── 1i. Undo-of-InsertRest heuristic ─────────────────────────────
    // When more rests are removed than added (with no note involvement),
    // the user is undoing an InsertRest.  setNoteRest() always creates
    // fewer elements than it replaces, so the undo reversal produces
    // more removals than additions.  Suppress InsertRest here; the
    // Stream tier will emit DeleteRest instead.
    bool isUndoOfInsertRest = false;
    {
        const bool hasNewChords = !newChordNotes.isEmpty();
        bool hasNoteRemovals = false;
        int removedRestCount = 0;
        int addedRestCount   = 0;
        if (!hasNewChords) {
            for (const auto& [obj, cmds] : changedObjects) {
                if (!obj) {
                    continue;
                }
                if (obj->type() == ElementType::NOTE
                    && cmds.count(CommandType::RemoveElement)) {
                    hasNoteRemovals = true;
                }
                if (obj->type() == ElementType::REST) {
                    if (cmds.count(CommandType::RemoveElement)) {
                        ++removedRestCount;
                    }
                    if (cmds.count(CommandType::AddElement)) {
                        ++addedRestCount;
                    }
                }
            }
        }
        // Don't treat tuplet removal as undo-of-InsertRest:
        // cmdDeleteTuplet removes N member rests and adds 1 replacement,
        // which matches the removedRestCount > addedRestCount pattern
        // but is not an undo.
        isUndoOfInsertRest = !hasNewChords && !hasNoteRemovals
            && removedRestCount > 0 && addedRestCount > 0
            && removedRestCount > addedRestCount
            && fullyRemovedTuplets.isEmpty();
    }

    // ═══════════════════════════════════════════════════════════════════
    // PHASE 2: Per-tier emission on unclaimed objects
    //
    // Each tier iterates changedObjects once, skipping objects in the
    // `claimed` set.  Tiers are processed in dependency order: Parts
    // first (so server knows about parts before element ops), then
    // Stream, Decoration, Spanner, Navigation.
    // ═══════════════════════════════════════════════════════════════════

    // ── Parts tier ───────────────────────────────────────────────────
    // Part management beyond AddPart/RemovePart (handled in Phase 1).

    // SetPartName / SetPartInstrument
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

    // SetStaffCount (InsertStaff / RemoveStaff)
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
                    ops.append(buildSetStaffCount(
                        uuid, static_cast<int>(part->nstaves())));
                }
            }
        }
    }

    // ── Stream tier ──────────────────────────────────────────────────
    // Note/rest content changes on the beat grid.

    // InsertNote
    QSet<EngravingObject*> handledNotes;
    for (const auto& [chord, notes] : newChordNotes.asKeyValueRange()) {
        if (claimed.contains(chord)) {
            continue;
        }
        if (durationTypeName(chord->durationType().type()) == QLatin1String("unknown")) {
            continue;
        }
        Part* chordPart = chord->staff() ? chord->staff()->part() : nullptr;
        const QString chordPartUuid = resolvePartUuid(chordPart, lazyAddPartOps);
        if (chordPartUuid.isEmpty()) {
            continue;
        }
        for (Note* n : notes) {
            ops.append(buildInsertNote(n, chordPartUuid));
            handledNotes.insert(n);
        }
    }

    // InsertNote for notes added to existing chords
    for (EngravingObject* obj : notesAddedToExistingChords) {
        if (handledNotes.contains(obj) || claimed.contains(obj)) {
            continue;
        }
        Note* note   = static_cast<Note*>(obj);
        Chord* chord = note->chord();
        if (!chord) {
            continue;
        }
        Part* notePart = chord->staff() ? chord->staff()->part() : nullptr;
        const QString notePartUuid = resolvePartUuid(notePart, lazyAddPartOps);
        if (notePartUuid.isEmpty()) {
            LOGD() << "[editude] translateAll: InsertNote (existing chord): "
                      "part UUID unknown, skipping";
            continue;
        }
        ops.append(buildInsertNote(note, notePartUuid));
        handledNotes.insert(obj);
    }

    // RemoveTuplet — MUST precede InsertRest so the peer applies them
    // in the right order: delete tuplet first, then overwrite the
    // resulting V_MEASURE rest with the correct replacement rest.
    for (Tuplet* tup : fullyRemovedTuplets) {
        Part* tupPart = static_cast<EngravingItem*>(tup)->part();
        if (!tupPart) {
            tupPart = resolvePartFromTrack(
                tup->track(), m_knownPartUuids);
        }
        if (!tupPart) continue;
        const QString tupPartUuid =
            resolvePartUuid(tupPart, lazyAddPartOps);
        if (tupPartUuid.isEmpty()) continue;

        const int voice = voiceFromTrack(tupPart, tup->track());
        const int staff = staffFromTrack(tupPart, tup->track());
        ops.append(buildRemoveTuplet(tupPartUuid, tup->tick(),
                                     voice, staff));
    }

    // InsertRest (with fill-rest suppression)
    //
    // setNoteRest() creates the target element AND "fill rests" to pad
    // the remaining measure duration.  On the peer, applying the primary
    // op (InsertNote/InsertRest) via setNoteRest produces its own fills,
    // so emitting InsertRest for fills would double-apply.
    //
    // Suppression rules:
    //   (a) Pass 1a grouped new notes → ALL rests are fills.
    //   (b) Batch removes Notes (chord deletion creates fill rests) →
    //       suppress all rests.
    //   (c) Pure InsertRest: emit only the single rest at the earliest
    //       tick; the remainder are fills from setNoteRest.
    {
        const bool hasNewChords = !newChordNotes.isEmpty();

        bool hasNoteRemovals = false;
        if (!hasNewChords) {
            for (const auto& [obj, cmds] : changedObjects) {
                if (!obj) continue;
                if (obj->type() == ElementType::NOTE
                    && cmds.count(CommandType::RemoveElement)) {
                    hasNoteRemovals = true;
                    break;
                }
            }
        }

        if (!hasNewChords && !hasNoteRemovals && !isUndoOfInsertRest) {
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
                if (claimed.contains(obj)) {
                    continue;
                }
                Rest* rest = static_cast<Rest*>(obj);
                if (durationTypeName(rest->durationType().type())
                    == QLatin1String("unknown")) {
                    continue;
                }
                // Skip rests inside tuplets — AddTuplet handles them.
                if (rest->tuplet()) {
                    continue;
                }
                // Skip fill rests that are side effects of tuplet creation.
                if (!newlyCreatedTuplets.isEmpty()) {
                    bool isTupletFillRest = false;
                    for (Tuplet* tup : newlyCreatedTuplets) {
                        if (rest->track() == tup->track()) {
                            isTupletFillRest = true;
                            break;
                        }
                    }
                    if (isTupletFillRest) {
                        continue;
                    }
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
                ops.append(buildInsertRest(c.rest, c.partUuid));
            }
        }
    }

    // DeleteNote / DeleteRest
    {
        QSet<EngravingObject*> removedChordsAndRests;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::RemoveElement)) {
                continue;
            }
            if (claimed.contains(obj)) {
                continue;
            }
            if (obj->type() == ElementType::CHORD) {
                if (static_cast<Chord*>(obj)->isGrace()) {
                    continue;
                }
                removedChordsAndRests.insert(obj);
            } else if (obj->type() == ElementType::REST) {
                removedChordsAndRests.insert(obj);
            }
        }

        QSet<EngravingObject*> emittedRemovals;
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::RemoveElement)) {
                continue;
            }

            if (obj->type() == ElementType::NOTE) {
                Note* note   = static_cast<Note*>(obj);
                Chord* chord = note->chord();

                if (claimed.contains(obj)
                    || (chord && claimed.contains(chord))) {
                    continue;
                }
                if (chord && chord->isGrace()) {
                    continue;
                }

                if (chord && removedChordsAndRests.contains(chord)) {
                    if (chord->tuplet()
                        && fullyRemovedTuplets.contains(chord->tuplet())) {
                        continue;
                    }
                    if (!emittedRemovals.contains(chord)) {
                        emittedRemovals.insert(chord);
                        Part* notePart = chord->staff()
                                         ? chord->staff()->part() : nullptr;
                        if (!notePart) {
                            notePart = resolvePartFromTrack(
                                chord->track(), m_knownPartUuids);
                        }
                        if (!notePart) {
                            continue;
                        }
                        const QString partUuid =
                            resolvePartUuid(notePart, lazyAddPartOps);
                        if (partUuid.isEmpty()) {
                            continue;
                        }
                        for (Note* n : chord->notes()) {
                            ops.append(buildDeleteNote(n, partUuid));
                        }
                    }
                } else if (chord) {
                    Part* notePart = chord->staff()
                                     ? chord->staff()->part() : nullptr;
                    if (!notePart) {
                        notePart = resolvePartFromTrack(
                            chord->track(), m_knownPartUuids);
                    }
                    if (!notePart) {
                        continue;
                    }
                    const QString partUuid =
                        resolvePartUuid(notePart, lazyAddPartOps);
                    if (!partUuid.isEmpty()) {
                        ops.append(buildDeleteNote(note, partUuid));
                    }
                }
            }

            // Handle chords that have RemoveElement but whose individual
            // notes do NOT have their own RemoveElement entries.
            if (obj->type() == ElementType::CHORD
                && removedChordsAndRests.contains(obj)
                && !emittedRemovals.contains(obj)
                && !claimed.contains(obj)) {
                Chord* chord = static_cast<Chord*>(obj);
                if (chord->isGrace()) {
                    continue;
                }
                if (chord->tuplet()
                    && fullyRemovedTuplets.contains(chord->tuplet())) {
                    continue;
                }
                emittedRemovals.insert(obj);
                Part* notePart = chord->staff()
                                 ? chord->staff()->part() : nullptr;
                if (!notePart) {
                    notePart = resolvePartFromTrack(
                        chord->track(), m_knownPartUuids);
                }
                if (!notePart) {
                    continue;
                }
                const QString partUuid =
                    resolvePartUuid(notePart, lazyAddPartOps);
                if (!partUuid.isEmpty()) {
                    for (Note* n : chord->notes()) {
                        ops.append(buildDeleteNote(n, partUuid));
                    }
                }
            }
        }

        // Undo-of-InsertRest: emit a single DeleteRest for the removed
        // rest at the earliest tick.
        if (isUndoOfInsertRest) {
            Rest* earliest = nullptr;
            Fraction earliestTick = Fraction(INT_MAX, 1);
            for (EngravingObject* removedObj : removedChordsAndRests) {
                if (removedObj->type() != ElementType::REST) {
                    continue;
                }
                Rest* r = static_cast<Rest*>(removedObj);
                if (r->tick() < earliestTick) {
                    earliestTick = r->tick();
                    earliest = r;
                }
            }
            if (earliest) {
                Part* restPart = earliest->staff()
                                 ? earliest->staff()->part() : nullptr;
                if (!restPart) {
                    restPart = resolvePartFromTrack(
                        earliest->track(), m_knownPartUuids);
                }
                const QString partUuid =
                    resolvePartUuid(restPart, lazyAddPartOps);
                if (!partUuid.isEmpty()) {
                    ops.append(buildDeleteRest(earliest, partUuid));
                }
            }
        }
    }

    // SetPitch
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
        Note* note = static_cast<Note*>(obj);
        Part* notePart = note->staff() ? note->staff()->part() : nullptr;
        if (!notePart) {
            notePart = resolvePartFromTrack(note->track(), m_knownPartUuids);
        }
        if (!notePart) {
            continue;
        }
        const QString partUuid = resolvePartUuid(notePart, lazyAddPartOps);
        if (partUuid.isEmpty()) {
            continue;
        }
        const int oldMidi = findOldMidiPitch(note);
        QJsonObject oldPitch;
        if (oldMidi >= 0) {
            oldPitch = pitchJsonFromMidi(oldMidi);
        } else {
            oldPitch = pitchJsonFromNote(note);
        }
        ops.append(buildSetPitch(note, partUuid, oldPitch));
    }

    // SetTabNote
    if (changedPropertyIdSet.count(Pid::FRET)
        || changedPropertyIdSet.count(Pid::STRING)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::NOTE) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            Note* note = static_cast<Note*>(obj);
            Part* notePart = note->staff() ? note->staff()->part() : nullptr;
            if (!notePart) {
                notePart = resolvePartFromTrack(
                    note->track(), m_knownPartUuids);
            }
            if (!notePart) {
                continue;
            }
            const QString partUuid =
                resolvePartUuid(notePart, lazyAddPartOps);
            if (partUuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetTabNote(note, partUuid));
        }
    }

    // SetNoteHead
    if (changedPropertyIdSet.count(Pid::HEAD_GROUP)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::NOTE) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            Note* note = static_cast<Note*>(obj);
            Part* notePart = note->staff() ? note->staff()->part() : nullptr;
            if (!notePart) {
                notePart = resolvePartFromTrack(
                    note->track(), m_knownPartUuids);
            }
            if (!notePart) {
                continue;
            }
            const QString partUuid =
                resolvePartUuid(notePart, lazyAddPartOps);
            if (partUuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetNoteHead(note, partUuid));
        }
    }

    // SetVoice (property-based: Pid::VOICE or Pid::TRACK)
    if (changedPropertyIdSet.count(Pid::VOICE)
        || changedPropertyIdSet.count(Pid::TRACK)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            if (obj->type() != ElementType::NOTE
                && obj->type() != ElementType::CHORD
                && obj->type() != ElementType::REST) {
                continue;
            }
            auto* item = static_cast<EngravingItem*>(obj);
            Part* itemPart = item->staff() ? item->staff()->part() : nullptr;
            if (!itemPart) {
                itemPart = resolvePartFromTrack(
                    item->track(), m_knownPartUuids);
            }
            if (!itemPart) {
                continue;
            }
            const QString partUuid =
                resolvePartUuid(itemPart, lazyAddPartOps);
            if (partUuid.isEmpty()) {
                continue;
            }
            const int oldVoice = findOldVoice(item, itemPart);
            if (oldVoice < 0) {
                continue;
            }
            ops.append(buildSetVoice(item, partUuid, oldVoice));
        }
    }

    // SetVoice (structural voice changes from Phase 1g)
    for (const auto& vc : structuralVoiceChanges) {
        ops.append(buildSetVoice(vc.newNote, vc.partUuid, vc.oldVoice));
    }

    // SetTie
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TIE) {
            continue;
        }
        auto* tie = static_cast<Tie*>(obj);
        Note* startNote = tie->startNote();
        if (!startNote) continue;

        Part* tiePart = startNote->staff()
                        ? startNote->staff()->part() : nullptr;
        if (!tiePart) {
            tiePart = resolvePartFromTrack(
                startNote->track(), m_knownPartUuids);
        }
        if (!tiePart) continue;
        const QString tiePartUuid = resolvePartUuid(tiePart, lazyAddPartOps);
        if (tiePartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildSetTie(startNote, tiePartUuid, /*tieStart=*/true));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildSetTie(startNote, tiePartUuid, /*tieStart=*/false));
        }
    }

    // AddTuplet (RemoveTuplet already emitted above)
    {
        QSet<Tuplet*> handledTuplets;

        // Path A: Detect new tuplets directly via AddElement on Tuplet.
        for (Tuplet* tup : newlyCreatedTuplets) {
            if (handledTuplets.contains(tup)) {
                continue;
            }
            handledTuplets.insert(tup);

            Part* tupPart = static_cast<EngravingItem*>(tup)->part();
            if (!tupPart) continue;
            const QString tupPartUuid =
                resolvePartUuid(tupPart, lazyAddPartOps);
            if (tupPartUuid.isEmpty()) continue;

            ops.append(buildAddTuplet(tup, tupPartUuid,
                                      tup->ratio().numerator(),
                                      tup->ratio().denominator()));
        }

        // Path B (fallback): Detect new tuplets via their members.
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::AddElement)) {
                continue;
            }
            if (obj->type() != ElementType::REST
                && obj->type() != ElementType::CHORD) {
                continue;
            }
            auto* cr = static_cast<ChordRest*>(obj);
            Tuplet* tup = cr->tuplet();
            if (!tup || handledTuplets.contains(tup)) {
                continue;
            }
            bool allMembersTracked = true;
            int membersWithAdd = 0;
            for (DurationElement* elem : tup->elements()) {
                auto memberIt = changedObjects.find(elem);
                if (memberIt == changedObjects.end()) {
                    allMembersTracked = false;
                    break;
                }
                if (memberIt->second.count(CommandType::AddElement)) {
                    membersWithAdd++;
                }
            }
            if (!allMembersTracked || membersWithAdd == 0) {
                continue;
            }
            handledTuplets.insert(tup);

            Part* tupPart = static_cast<EngravingItem*>(tup)->part();
            if (!tupPart) continue;
            const QString tupPartUuid =
                resolvePartUuid(tupPart, lazyAddPartOps);
            if (tupPartUuid.isEmpty()) continue;

            ops.append(buildAddTuplet(tup, tupPartUuid,
                                      tup->ratio().numerator(),
                                      tup->ratio().denominator()));
        }
    }

    // AddGraceNote / RemoveGraceNote
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
            Part* gnPart = chord->staff() ? chord->staff()->part() : nullptr;
            if (!gnPart) {
                gnPart = resolvePartFromTrack(
                    chord->track(), m_knownPartUuids);
            }
            if (!gnPart) continue;
            const QString gnPartUuid =
                resolvePartUuid(gnPart, lazyAddPartOps);
            if (gnPartUuid.isEmpty()) continue;
            ops.append(buildAddGraceNote(chord, gnPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* gnPart = chord->staff() ? chord->staff()->part() : nullptr;
            if (!gnPart) {
                gnPart = resolvePartFromTrack(
                    chord->track(), m_knownPartUuids);
            }
            if (!gnPart) continue;
            const QString gnPartUuid =
                resolvePartUuid(gnPart, lazyAddPartOps);
            if (gnPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(gnPart, chord->track());
            const int staff = staffFromTrack(gnPart, chord->track());
            ops.append(buildRemoveGraceNote(gnPartUuid, chord->tick(),
                                            voice, staff,
                                            static_cast<int>(chord->graceIndex())));
        }
    }

    // SetDuration
    if (changedPropertyIdSet.count(Pid::DURATION_TYPE_WITH_DOTS)
        && !hasMeasureLenChange) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || !cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            if (obj->type() != ElementType::CHORD
                && obj->type() != ElementType::REST) {
                continue;
            }
            ChordRest* cr = static_cast<ChordRest*>(obj);
            const DurationTypeWithDots oldDur = findOldDuration(cr);
            if (oldDur.type == DurationType::V_INVALID) {
                continue;
            }
            Part* crPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!crPart) {
                crPart = resolvePartFromTrack(cr->track(), m_knownPartUuids);
            }
            if (!crPart) {
                continue;
            }
            const QString partUuid =
                resolvePartUuid(crPart, lazyAddPartOps);
            if (partUuid.isEmpty()) {
                continue;
            }
            ops.append(buildSetDuration(cr, partUuid));
        }
    }

    // ── Decoration tier ──────────────────────────────────────────────
    // Annotations on existing events and score-level directives.

    // ChordSymbol
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::HARMONY) {
            continue;
        }
        auto* harmony = static_cast<Harmony*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            const Fraction tick = harmony->tick().reduced();
            ops.append(buildAddChordSymbol(tick.numerator(), tick.denominator(),
                                           harmony->harmonyName().toQString()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const Fraction tick = harmony->tick().reduced();
            ops.append(buildRemoveChordSymbol(tick.numerator(), tick.denominator(),
                                               harmony->harmonyName().toQString()));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            const Fraction tick = harmony->tick().reduced();
            ops.append(buildSetChordSymbol(tick.numerator(), tick.denominator(),
                                           harmony->harmonyName().toQString()));
        }
    }

    // SetKeySignature / SetClef
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
                part = resolvePartFromTrack(ks->track(), m_knownPartUuids);
            }
            if (!part) continue;
            const QString partUuid = resolvePartUuid(part, lazyAddPartOps);
            if (partUuid.isEmpty()) continue;
            if (isAdd) {
                ops.append(buildSetKeySignature(ks, partUuid));
            } else {
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
                part = resolvePartFromTrack(clef->track(), m_knownPartUuids);
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

    // Articulations
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::ARTICULATION) {
            continue;
        }
        auto* art = static_cast<Articulation*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ChordRest* cr = art->chordRest();
            if (!cr) continue;
            Part* artPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!artPart) {
                artPart = resolvePartFromTrack(
                    cr->track(), m_knownPartUuids);
            }
            if (!artPart) continue;
            const QString artPartUuid =
                resolvePartUuid(artPart, lazyAddPartOps);
            if (artPartUuid.isEmpty()) continue;
            Note* anchorNote = nullptr;
            if (cr->isChord()) {
                Chord* ch = toChord(static_cast<EngravingItem*>(cr));
                if (!ch->notes().empty()) {
                    anchorNote = ch->notes().front();
                }
            }
            ops.append(buildAddArticulation(art, artPartUuid, anchorNote));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ChordRest* cr = art->chordRest();
            if (!cr) continue;
            Part* artPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!artPart) {
                artPart = resolvePartFromTrack(
                    cr->track(), m_knownPartUuids);
            }
            if (!artPart) continue;
            const QString artPartUuid =
                resolvePartUuid(artPart, lazyAddPartOps);
            if (artPartUuid.isEmpty()) continue;
            Note* anchorNote = nullptr;
            if (cr->isChord()) {
                Chord* ch = toChord(static_cast<EngravingItem*>(cr));
                if (!ch->notes().empty()) {
                    anchorNote = ch->notes().front();
                }
            }
            ops.append(buildRemoveArticulation(art, artPartUuid, anchorNote));
        }
    }

    // Dynamics
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::DYNAMIC) {
            continue;
        }
        auto* dyn = static_cast<Dynamic*>(obj);
        Part* dynPart = static_cast<EngravingItem*>(dyn)->part();
        if (!dynPart) {
            dynPart = resolvePartFromTrack(dyn->track(), m_knownPartUuids);
        }
        if (!dynPart) continue;
        const QString dynPartUuid = resolvePartUuid(dynPart, lazyAddPartOps);
        if (dynPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddDynamic(dyn, dynPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveDynamic(dynPartUuid, dyn->tick(),
                                            dynamicKindName(dyn->dynamicType())));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            ops.append(buildSetDynamic(dynPartUuid, dyn->tick(),
                                       dynamicKindName(dyn->dynamicType())));
        }
    }

    // Lyrics
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::LYRICS) {
            continue;
        }
        auto* lyr = static_cast<Lyrics*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ChordRest* cr = lyr->chordRest();
            if (!cr) continue;
            Part* lyrPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!lyrPart) {
                lyrPart = resolvePartFromTrack(
                    cr->track(), m_knownPartUuids);
            }
            if (!lyrPart) continue;
            const QString lyrPartUuid =
                resolvePartUuid(lyrPart, lazyAddPartOps);
            if (lyrPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(lyrPart, cr->track());
            const int staff = staffFromTrack(lyrPart, cr->track());
            ops.append(buildAddLyric(lyr, lyrPartUuid, voice, staff,
                                     lyr->verse(),
                                     lyricSyllabicName(lyr->syllabic()),
                                     lyr->plainText().toQString()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ChordRest* cr = lyr->chordRest();
            if (!cr) continue;
            Part* lyrPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!lyrPart) {
                lyrPart = resolvePartFromTrack(
                    cr->track(), m_knownPartUuids);
            }
            if (!lyrPart) continue;
            const QString lyrPartUuid =
                resolvePartUuid(lyrPart, lazyAddPartOps);
            if (lyrPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(lyrPart, cr->track());
            const int staff = staffFromTrack(lyrPart, cr->track());
            ops.append(buildRemoveLyric(lyrPartUuid, lyr->tick(),
                                        voice, staff, lyr->verse()));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            ChordRest* cr = lyr->chordRest();
            if (!cr) continue;
            Part* lyrPart = cr->staff() ? cr->staff()->part() : nullptr;
            if (!lyrPart) {
                lyrPart = resolvePartFromTrack(
                    cr->track(), m_knownPartUuids);
            }
            if (!lyrPart) continue;
            const QString lyrPartUuid =
                resolvePartUuid(lyrPart, lazyAddPartOps);
            if (lyrPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(lyrPart, cr->track());
            const int staff = staffFromTrack(lyrPart, cr->track());
            ops.append(buildSetLyric(lyrPartUuid, lyr->tick(),
                                     voice, staff, lyr->verse(),
                                     lyr->plainText().toQString(),
                                     lyricSyllabicName(lyr->syllabic())));
        }
    }

    // Staff Text (part-scoped)
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::STAFF_TEXT) {
            continue;
        }
        auto* staffText = static_cast<StaffText*>(obj);
        Part* textPart = static_cast<EngravingItem*>(staffText)->part();
        if (!textPart) {
            textPart = resolvePartFromTrack(
                staffText->track(), m_knownPartUuids);
        }
        if (!textPart) continue;
        const QString textPartUuid =
            resolvePartUuid(textPart, lazyAddPartOps);
        if (textPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddStaffText(staffText, textPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveStaffText(textPartUuid,
                                            staffText->tick(),
                                            staffText->plainText().toQString()));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            ops.append(buildSetStaffText(textPartUuid, staffText->tick(),
                                         staffText->plainText().toQString()));
        }
    }

    // System Text (score-global)
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::SYSTEM_TEXT) {
            continue;
        }
        auto* sysText = static_cast<SystemText*>(obj);

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddSystemText(sysText));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveSystemText(sysText->tick(),
                                              sysText->plainText().toQString()));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            ops.append(buildSetSystemText(sysText->tick(),
                                          sysText->plainText().toQString()));
        }
    }

    // Rehearsal Mark (score-global)
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::REHEARSAL_MARK) {
            continue;
        }
        auto* mark = static_cast<RehearsalMark*>(obj);

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddRehearsalMark(mark));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveRehearsalMark(mark->tick(),
                                                mark->plainText().toQString()));
        } else if (cmds.count(CommandType::ChangeProperty)) {
            ops.append(buildSetRehearsalMark(mark->tick(),
                                             mark->plainText().toQString()));
        }
    }

    // Arpeggios
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::ARPEGGIO) {
            continue;
        }
        auto* arp = static_cast<Arpeggio*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Chord* ch = arp->chord();
            if (!ch) continue;
            Part* arpPart = ch->staff() ? ch->staff()->part() : nullptr;
            if (!arpPart) {
                arpPart = resolvePartFromTrack(
                    ch->track(), m_knownPartUuids);
            }
            if (!arpPart) continue;
            const QString arpPartUuid =
                resolvePartUuid(arpPart, lazyAddPartOps);
            if (arpPartUuid.isEmpty()) continue;
            ops.append(buildAddArpeggio(arp, arpPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Chord* ch = arp->chord();
            if (!ch) continue;
            Part* arpPart = ch->staff() ? ch->staff()->part() : nullptr;
            if (!arpPart) {
                arpPart = resolvePartFromTrack(
                    ch->track(), m_knownPartUuids);
            }
            if (!arpPart) continue;
            const QString arpPartUuid =
                resolvePartUuid(arpPart, lazyAddPartOps);
            if (arpPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(arpPart, ch->track());
            const int staff = staffFromTrack(arpPart, ch->track());
            ops.append(buildRemoveArpeggio(arpPartUuid, ch->tick(),
                                           voice, staff));
        }
    }

    // Single-note Tremolos
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TREMOLO_SINGLECHORD) {
            continue;
        }
        auto* trem = static_cast<TremoloSingleChord*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Chord* ch = trem->chord();
            if (!ch) continue;
            Part* tremPart = ch->staff() ? ch->staff()->part() : nullptr;
            if (!tremPart) {
                tremPart = resolvePartFromTrack(
                    ch->track(), m_knownPartUuids);
            }
            if (!tremPart) continue;
            const QString tremPartUuid =
                resolvePartUuid(tremPart, lazyAddPartOps);
            if (tremPartUuid.isEmpty()) continue;
            ops.append(buildAddTremolo(trem, tremPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Chord* ch = trem->chord();
            if (!ch) continue;
            Part* tremPart = ch->staff() ? ch->staff()->part() : nullptr;
            if (!tremPart) {
                tremPart = resolvePartFromTrack(
                    ch->track(), m_knownPartUuids);
            }
            if (!tremPart) continue;
            const QString tremPartUuid =
                resolvePartUuid(tremPart, lazyAddPartOps);
            if (tremPartUuid.isEmpty()) continue;
            const int voice = voiceFromTrack(tremPart, ch->track());
            const int staff = staffFromTrack(tremPart, ch->track());
            ops.append(buildRemoveTremolo(tremPartUuid, ch->tick(),
                                          voice, staff));
        }
    }

    // Breath Marks / Caesuras
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::BREATH) {
            continue;
        }
        auto* breath = static_cast<Breath*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Segment* seg = breath->segment();
            if (!seg) continue;
            Part* bPart = breath->part();
            if (!bPart) {
                bPart = resolvePartFromTrack(
                    breath->track(), m_knownPartUuids);
            }
            const QString bPartUuid =
                resolvePartUuid(bPart, lazyAddPartOps);
            if (bPartUuid.isEmpty()) continue;
            ops.append(buildAddBreathMark(breath, bPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* bPart = breath->part();
            if (!bPart) {
                bPart = resolvePartFromTrack(
                    breath->track(), m_knownPartUuids);
            }
            if (!bPart) continue;
            const QString bPartUuid =
                resolvePartUuid(bPart, lazyAddPartOps);
            if (bPartUuid.isEmpty()) continue;
            ops.append(buildRemoveBreathMark(bPartUuid,
                                             breath->segment()
                                             ? breath->segment()->tick()
                                             : breath->tick(),
                                             breathTypeToString(breath->symId())));
        }
    }

    // ── Spanner tier ─────────────────────────────────────────────────
    // Connections between two beat positions.

    // Slurs
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::SLUR) {
            continue;
        }
        auto* slur = static_cast<Slur*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Part* slurPart = static_cast<EngravingItem*>(slur)->part();
            if (!slurPart) {
                slurPart = resolvePartFromTrack(
                    slur->track(), m_knownPartUuids);
            }
            if (!slurPart) continue;
            const QString slurPartUuid =
                resolvePartUuid(slurPart, lazyAddPartOps);
            if (slurPartUuid.isEmpty()) continue;
            ops.append(buildAddSlur(slur, slurPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* slurPart = static_cast<EngravingItem*>(slur)->part();
            if (!slurPart) {
                slurPart = resolvePartFromTrack(
                    slur->track(), m_knownPartUuids);
            }
            if (!slurPart) continue;
            const QString slurPartUuid =
                resolvePartUuid(slurPart, lazyAddPartOps);
            if (slurPartUuid.isEmpty()) continue;
            ops.append(buildRemoveSlur(slur, slurPartUuid));
        }
    }

    // Hairpins
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::HAIRPIN) {
            continue;
        }
        auto* hp = static_cast<Hairpin*>(obj);
        Part* hpPart = static_cast<EngravingItem*>(hp)->part();
        if (!hpPart) {
            hpPart = resolvePartFromTrack(hp->track(), m_knownPartUuids);
        }
        if (!hpPart) continue;
        const QString hpPartUuid = resolvePartUuid(hpPart, lazyAddPartOps);
        if (hpPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddHairpin(hp, hpPartUuid,
                                       hp->tick(), hp->tick2(),
                                       hp->isCrescendo()));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveHairpin(hpPartUuid,
                                          hp->tick(), hp->tick2(),
                                          hp->isCrescendo()
                                              ? QStringLiteral("crescendo")
                                              : QStringLiteral("diminuendo")));
        }
    }

    // Octave Lines
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::OTTAVA) {
            continue;
        }
        auto* ottava = static_cast<Ottava*>(obj);
        Part* ottavaPart = static_cast<EngravingItem*>(ottava)->part();
        if (!ottavaPart) {
            ottavaPart = resolvePartFromTrack(
                ottava->track(), m_knownPartUuids);
        }
        if (!ottavaPart) continue;
        const QString ottavaPartUuid =
            resolvePartUuid(ottavaPart, lazyAddPartOps);
        if (ottavaPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddOctaveLine(ottava, ottavaPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            QString ottavaKind;
            switch (ottava->ottavaType()) {
            case OttavaType::OTTAVA_8VA:  ottavaKind = QStringLiteral("8va");  break;
            case OttavaType::OTTAVA_8VB:  ottavaKind = QStringLiteral("8vb");  break;
            case OttavaType::OTTAVA_15MA: ottavaKind = QStringLiteral("15ma"); break;
            case OttavaType::OTTAVA_15MB: ottavaKind = QStringLiteral("15mb"); break;
            case OttavaType::OTTAVA_22MA: ottavaKind = QStringLiteral("22ma"); break;
            case OttavaType::OTTAVA_22MB: ottavaKind = QStringLiteral("22mb"); break;
            default:                      ottavaKind = QStringLiteral("8va");  break;
            }
            ops.append(buildRemoveOctaveLine(ottavaPartUuid,
                                             ottava->tick(), ottava->tick2(),
                                             ottavaKind));
        }
    }

    // Glissandos
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::GLISSANDO) {
            continue;
        }
        auto* gliss = static_cast<Glissando*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Part* glissPart = static_cast<EngravingItem*>(gliss)->part();
            if (!glissPart) {
                glissPart = resolvePartFromTrack(
                    gliss->track(), m_knownPartUuids);
            }
            if (!glissPart) continue;
            const QString glissPartUuid =
                resolvePartUuid(glissPart, lazyAddPartOps);
            if (glissPartUuid.isEmpty()) continue;
            ops.append(buildAddGlissando(gliss, glissPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* glissPart = static_cast<EngravingItem*>(gliss)->part();
            if (!glissPart) {
                glissPart = resolvePartFromTrack(
                    gliss->track(), m_knownPartUuids);
            }
            if (!glissPart) continue;
            const QString glissPartUuid =
                resolvePartUuid(glissPart, lazyAddPartOps);
            if (glissPartUuid.isEmpty()) continue;
            ops.append(buildRemoveGlissando(gliss, glissPartUuid));
        }
    }

    // Guitar Bends
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::GUITAR_BEND) {
            continue;
        }
        auto* bend = static_cast<GuitarBend*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Part* bendPart = static_cast<EngravingItem*>(bend)->part();
            if (!bendPart) {
                bendPart = resolvePartFromTrack(
                    bend->track(), m_knownPartUuids);
            }
            if (!bendPart) continue;
            const QString bendPartUuid =
                resolvePartUuid(bendPart, lazyAddPartOps);
            if (bendPartUuid.isEmpty()) continue;
            ops.append(buildAddGuitarBend(bend, bendPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* bendPart = static_cast<EngravingItem*>(bend)->part();
            if (!bendPart) {
                bendPart = resolvePartFromTrack(
                    bend->track(), m_knownPartUuids);
            }
            if (!bendPart) continue;
            const QString bendPartUuid =
                resolvePartUuid(bendPart, lazyAddPartOps);
            if (bendPartUuid.isEmpty()) continue;
            ops.append(buildRemoveGuitarBend(bend, bendPartUuid));
        }
    }

    // Pedal Lines
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::PEDAL) {
            continue;
        }
        auto* pedal = static_cast<Pedal*>(obj);
        Part* pedalPart = static_cast<EngravingItem*>(pedal)->part();
        if (!pedalPart) {
            pedalPart = resolvePartFromTrack(
                pedal->track(), m_knownPartUuids);
        }
        if (!pedalPart) continue;
        const QString pedalPartUuid =
            resolvePartUuid(pedalPart, lazyAddPartOps);
        if (pedalPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddPedalLine(pedal, pedalPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemovePedalLine(pedalPartUuid,
                                            pedal->tick(), pedal->tick2()));
        }
    }

    // Trill Lines
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TRILL) {
            continue;
        }
        auto* trill = static_cast<Trill*>(obj);
        Part* trillPart = static_cast<EngravingItem*>(trill)->part();
        if (!trillPart) {
            trillPart = resolvePartFromTrack(
                trill->track(), m_knownPartUuids);
        }
        if (!trillPart) continue;
        const QString trillPartUuid =
            resolvePartUuid(trillPart, lazyAddPartOps);
        if (trillPartUuid.isEmpty()) continue;

        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildAddTrillLine(trill, trillPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveTrillLine(trillPartUuid,
                                            trill->tick(), trill->tick2()));
        }
    }

    // Two-Note Tremolos
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::TREMOLO_TWOCHORD) {
            continue;
        }
        auto* trem = static_cast<TremoloTwoChord*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            Chord* ch1 = trem->chord1();
            Chord* ch2 = trem->chord2();
            if (!ch1 || !ch2) continue;
            Part* tremPart = ch1->staff() ? ch1->staff()->part() : nullptr;
            if (!tremPart) {
                tremPart = resolvePartFromTrack(
                    ch1->track(), m_knownPartUuids);
            }
            if (!tremPart) continue;
            const QString tremPartUuid =
                resolvePartUuid(tremPart, lazyAddPartOps);
            if (tremPartUuid.isEmpty()) continue;
            ops.append(buildAddTwoNoteTremolo(trem, tremPartUuid));
        } else if (cmds.count(CommandType::RemoveElement)) {
            Part* tremPart = nullptr;
            Chord* ch1 = trem->chord1();
            if (ch1) {
                tremPart = ch1->staff() ? ch1->staff()->part() : nullptr;
                if (!tremPart) {
                    tremPart = resolvePartFromTrack(
                        ch1->track(), m_knownPartUuids);
                }
            }
            if (!tremPart) continue;
            const QString tremPartUuid =
                resolvePartUuid(tremPart, lazyAddPartOps);
            if (tremPartUuid.isEmpty()) continue;
            ops.append(buildRemoveTwoNoteTremolo(trem, tremPartUuid));
        }
    }

    // ── Navigation tier ──────────────────────────────────────────────
    // Playback structure and metadata.

    // Voltas
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::VOLTA) {
            continue;
        }
        auto* volta = static_cast<Volta*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildInsertVolta(volta));
        } else if (cmds.count(CommandType::RemoveElement)) {
            QJsonArray voltaNumbers;
            for (int n : volta->endings()) {
                voltaNumbers.append(n);
            }
            ops.append(buildRemoveVolta(volta->tick(), volta->tick2(),
                                        voltaNumbers));
        } else if (cmds.count(CommandType::ChangeProperty)
                   && changedPropertyIdSet.count(Pid::VOLTA_ENDING)) {
            QJsonArray oldNumbers;
            const UndoStack* us = volta->score()->undoStack();
            if (us) {
                for (const UndoMacro* macro : { us->last(), us->next() }) {
                    if (!macro) continue;
                    for (size_t i = 0; i < macro->childCount(); ++i) {
                        const UndoCommand* cmd = macro->commands()[i];
                        if (cmd->type() != CommandType::ChangeProperty) continue;
                        const auto* cp = static_cast<const ChangeProperty*>(cmd);
                        if (cp->getElement() == volta && cp->getId() == Pid::VOLTA_ENDING) {
                            const auto oldEndings = cp->data().value<std::vector<int>>();
                            for (int n : oldEndings) {
                                oldNumbers.append(n);
                            }
                            break;
                        }
                    }
                    if (!oldNumbers.isEmpty()) break;
                }
            }
            ops.append(buildSetVoltaNumbers(volta, oldNumbers));
        }
    }

    // Markers
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::MARKER) {
            continue;
        }
        auto* marker = static_cast<Marker*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildInsertMarker(marker));
        } else if (cmds.count(CommandType::RemoveElement)) {
            const Fraction tick = marker->measure()
                                  ? marker->measure()->tick()
                                  : marker->tick();
            ops.append(buildRemoveMarker(tick, markerKindName(marker->markerType())));
        }
    }

    // Jumps
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::JUMP) {
            continue;
        }
        auto* jump = static_cast<Jump*>(obj);
        if (cmds.count(CommandType::AddElement)) {
            ops.append(buildInsertJump(jump));
        } else if (cmds.count(CommandType::RemoveElement)) {
            ops.append(buildRemoveJump(jump->tick()));
        }
    }

    // Repeats (SetStartRepeat / SetEndRepeat)
    if (changedPropertyIdSet.count(Pid::REPEAT_START)
        || changedPropertyIdSet.count(Pid::REPEAT_END)
        || changedPropertyIdSet.count(Pid::REPEAT_COUNT)) {
        for (const auto& [obj, cmds] : changedObjects) {
            if (!obj || obj->type() != ElementType::MEASURE) {
                continue;
            }
            if (!cmds.count(CommandType::ChangeProperty)) {
                continue;
            }
            auto* m = static_cast<Measure*>(obj);
            if (changedPropertyIdSet.count(Pid::REPEAT_START)) {
                ops.append(buildSetStartRepeat(m->tick(), m->repeatStart()));
            }
            if (changedPropertyIdSet.count(Pid::REPEAT_END)
                || changedPropertyIdSet.count(Pid::REPEAT_COUNT)) {
                ops.append(buildSetEndRepeat(m->tick(), m->repeatEnd(),
                                             m->repeatCount()));
            }
        }
    }

    // SetScoreMetadata
    if (!changedMetaTags.isEmpty()) {
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
        for (auto it = changedMetaTags.begin();
             it != changedMetaTags.end(); ++it) {
            const QString field =
                s_reverseFieldMap.value(it.key(), it.key());
            QJsonObject op;
            op["type"]  = QStringLiteral("SetScoreMetadata");
            op["field"] = field;
            op["value"] = it.value();
            ops.append(op);
        }
    }

    // SetMeasureLen (pickup / anacrusis measures)
    for (const auto& [obj, cmds] : changedObjects) {
        if (!obj || obj->type() != ElementType::MEASURE) {
            continue;
        }
        if (!cmds.count(CommandType::ChangeMeasureLen)) {
            continue;
        }
        auto* m = static_cast<Measure*>(obj);
        QJsonObject op;
        op["type"] = QStringLiteral("SetMeasureLen");
        op["beat"] = beatJson(m->tick());
        if (m->ticks() != m->timesig()) {
            op["actual_len"] = beatJson(m->ticks());
        } else {
            op["actual_len"] = QJsonValue(QJsonValue::Null);
        }
        ops.append(op);
    }

    // ── Finalize ─────────────────────────────────────────────────────
    // Prepend any lazily-generated AddPart ops so the server registers
    // parts before processing the element ops that reference them.
    if (!lazyAddPartOps.isEmpty()) {
        QVector<QJsonObject> result = lazyAddPartOps;
        result.append(ops);
        return result;
    }
    return ops;
}


// ---------------------------------------------------------------------------
// Insert builders — coordinate-addressed
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertNote(Note* note, const QString& partId)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    const Fraction tick = note->chord()->tick();
    const DurationType dt   = note->chord()->durationType().type();
    const int          dots = note->chord()->dots();

    LOGW() << "[editude] buildInsertNote: note=" << (void*)note
           << " pitch=" << note->pitch()
           << " tpc1=" << note->tpc1()
           << " tpc2=" << note->tpc2()
           << " tick=" << tick.toString()
           << " track=" << note->track();

    QJsonObject duration;
    duration["type"] = durationTypeName(dt);
    duration["dots"] = dots;

    QJsonObject payload;
    payload["type"]     = QStringLiteral("InsertNote");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(tick);
    payload["duration"] = duration;
    payload["pitch"]    = pitchJsonFromNote(note);
    payload["voice"]    = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]    = part ? staffFromTrack(part, note->track()) : 0;
    payload["tie"]      = QJsonValue::Null;

    // Tab fields: include fret/string if the note carries tab data.
    if (note->fret() >= 0 && note->string() >= 0) {
        payload["fret"]   = note->fret();
        payload["string"] = note->string();
    } else {
        payload["fret"]   = -1;
        payload["string"] = -1;
    }

    // Percussion: include notehead.
    payload["notehead"] = noteheadGroupToString(note->headGroup());

    return payload;
}

QJsonObject OperationTranslator::buildInsertRest(Rest* rest, const QString& partId)
{
    Part* part = rest->staff() ? rest->staff()->part() : nullptr;
    const Fraction tick = rest->tick();
    const DurationType dt   = rest->durationType().type();
    const int          dots = rest->dots();

    QJsonObject duration;
    duration["type"] = durationTypeName(dt);
    duration["dots"] = dots;

    QJsonObject payload;
    payload["type"]     = QStringLiteral("InsertRest");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(tick);
    payload["duration"] = duration;
    payload["voice"]    = part ? voiceFromTrack(part, rest->track()) : 1;
    payload["staff"]    = part ? staffFromTrack(part, rest->track()) : 0;
    return payload;
}

// ---------------------------------------------------------------------------
// Deletion builders — coordinate-addressed
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildDeleteNote(Note* note, const QString& partId)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    Chord* chord = note->chord();
    LOGW() << "[editude] buildDeleteNote: note=" << (void*)note
           << " chord=" << (void*)chord
           << " segment=" << (void*)chord->segment()
           << " tick=" << chord->tick().toString()
           << " pitch=" << note->pitch()
           << " tpc1=" << note->tpc1()
           << " tpc2=" << note->tpc2()
           << " track=" << note->track()
           << " partId=" << partId;
    QJsonObject payload;
    payload["type"]    = QStringLiteral("DeleteNote");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(note->chord()->tick());
    payload["pitch"]   = pitchJsonFromNote(note);
    payload["voice"]   = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]   = part ? staffFromTrack(part, note->track()) : 0;
    LOGW() << "[editude] buildDeleteNote result:"
           << " beat=" << QJsonDocument(payload["beat"].toObject()).toJson(QJsonDocument::Compact)
           << " pitch=" << QJsonDocument(payload["pitch"].toObject()).toJson(QJsonDocument::Compact)
           << " voice=" << payload["voice"].toInt()
           << " staff=" << payload["staff"].toInt();
    return payload;
}

QJsonObject OperationTranslator::buildDeleteRest(Rest* rest, const QString& partId)
{
    Part* part = rest->staff() ? rest->staff()->part() : nullptr;
    QJsonObject payload;
    payload["type"]    = QStringLiteral("DeleteRest");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(rest->tick());
    payload["voice"]   = part ? voiceFromTrack(part, rest->track()) : 1;
    payload["staff"]   = part ? staffFromTrack(part, rest->track()) : 0;
    return payload;
}

// ---------------------------------------------------------------------------
// Modification builders — coordinate-addressed
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildSetPitch(Note* note, const QString& partId,
                                                const QJsonObject& oldPitch)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    QJsonObject payload;
    payload["type"]      = QStringLiteral("SetPitch");
    payload["part_id"]   = partId;
    payload["beat"]      = beatJson(note->chord()->tick());
    payload["pitch"]     = oldPitch;
    payload["voice"]     = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]     = part ? staffFromTrack(part, note->track()) : 0;
    payload["new_pitch"] = pitchJsonFromNote(note);
    return payload;
}

QJsonObject OperationTranslator::buildSetTabNote(Note* note, const QString& partId)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetTabNote");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(note->chord()->tick());
    payload["pitch"]   = pitchJsonFromNote(note);
    payload["voice"]   = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]   = part ? staffFromTrack(part, note->track()) : 0;
    payload["fret"]    = note->fret();
    payload["string"]  = note->string();
    return payload;
}

QJsonObject OperationTranslator::buildSetNoteHead(Note* note, const QString& partId)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetNoteHead");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(note->chord()->tick());
    payload["pitch"]    = pitchJsonFromNote(note);
    payload["voice"]    = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]    = part ? staffFromTrack(part, note->track()) : 0;
    payload["notehead"] = noteheadGroupToString(note->headGroup());
    return payload;
}

QJsonObject OperationTranslator::buildSetVoice(EngravingItem* item,
                                                const QString& partId,
                                                int oldVoice)
{
    Part* part = item->staff() ? item->staff()->part() : nullptr;
    const int newVoice = part ? voiceFromTrack(part, item->track()) : 1;
    const int staff    = part ? staffFromTrack(part, item->track()) : 0;

    // Determine beat from the item.
    Fraction tick;
    if (item->isChordRest()) {
        tick = toChordRest(item)->tick();
    } else if (item->isNote()) {
        tick = toNote(item)->chord()->tick();
    }

    QJsonObject payload;
    payload["type"]      = QStringLiteral("SetVoice");
    payload["part_id"]   = partId;
    payload["beat"]      = beatJson(tick);
    payload["voice"]     = oldVoice;
    payload["staff"]     = staff;
    payload["new_voice"] = newVoice;

    // Include pitch if it's a note.
    if (item->isNote()) {
        payload["pitch"] = pitchJsonFromNote(toNote(item));
    } else if (item->isChord()) {
        Chord* ch = toChord(item);
        if (!ch->notes().empty()) {
            payload["pitch"] = pitchJsonFromNote(ch->notes().front());
        }
    }

    return payload;
}

QJsonObject OperationTranslator::buildSetDuration(ChordRest* cr,
                                                   const QString& partId)
{
    Part* part = cr->staff() ? cr->staff()->part() : nullptr;
    QJsonObject durObj;
    durObj["type"] = durationTypeName(cr->durationType().type());
    durObj["dots"] = cr->dots();

    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetDuration");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(cr->tick());
    payload["duration"] = durObj;
    payload["voice"]    = part ? voiceFromTrack(part, cr->track()) : 1;
    payload["staff"]    = part ? staffFromTrack(part, cr->track()) : 0;

    // Include pitch for Chords so the peer can address the correct element.
    if (cr->type() == ElementType::CHORD) {
        Chord* chord = toChord(cr);
        if (!chord->notes().empty()) {
            payload["pitch"] = pitchJsonFromNote(chord->notes().front());
        }
    }

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
    payload["type"]           = QStringLiteral("SetTimeSignature");
    payload["beat"]           = beatJson(ts->tick());
    payload["time_signature"] = timeSig;
    return payload;
}

QJsonObject OperationTranslator::buildSetTempo(TempoText* tt)
{
    const double bpm = tt->tempo().toBPM().val;

    QJsonObject referent;
    referent["type"] = QStringLiteral("quarter");
    referent["dots"] = 0;

    QJsonObject tempo;
    tempo["bpm"]      = bpm;
    tempo["referent"] = referent;
    tempo["text"]     = QJsonValue::Null;

    QJsonObject payload;
    payload["type"]  = QStringLiteral("SetTempo");
    payload["beat"]  = beatJson(tt->tick());
    payload["tempo"] = tempo;
    return payload;
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::pitchJson(int tpc, int octave)
{
    static const char* const kSteps[] = { "F", "C", "G", "D", "A", "E", "B" };
    static const int kNaturalTpc[]    = { 13, 14, 15, 16, 17, 18, 19 };
    static const char* const kAccidentals[] = {
        "double-flat", "flat", nullptr, "sharp", "double-sharp"
    };

    const int stepIndex = (tpc + 1) % 7;
    const int accOffset = (tpc - kNaturalTpc[stepIndex]) / 7;

    QJsonObject pitch;
    pitch["step"]   = kSteps[stepIndex];
    pitch["octave"] = octave;

    const int accIdx = accOffset + 2;
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
// Part/staff directive builders
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
                            vObj["articulation_name"] =
                                v.articulationName.toQString();
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

QJsonObject OperationTranslator::buildSetTie(Note* note, const QString& partId, bool tieStart)
{
    Part* part = note->staff() ? note->staff()->part() : nullptr;
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetTie");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(note->chord()->tick());
    payload["pitch"]   = pitchJsonFromNote(note);
    payload["voice"]   = part ? voiceFromTrack(part, note->track()) : 1;
    payload["staff"]   = part ? staffFromTrack(part, note->track()) : 0;
    if (tieStart) {
        payload["tie"] = QStringLiteral("start");
    } else {
        payload["tie"] = QJsonValue::Null;
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — articulations (coordinate-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddArticulation(EngravingObject* art,
                                                       const QString& partId,
                                                       Note* note)
{
    auto* a = static_cast<Articulation*>(art);
    ChordRest* cr = a->chordRest();
    Part* part = cr && cr->staff() ? cr->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]         = QStringLiteral("AddArticulation");
    payload["part_id"]      = partId;
    payload["beat"]         = beatJson(cr ? cr->tick() : Fraction());
    payload["voice"]        = part ? voiceFromTrack(part, cr->track()) : 1;
    payload["staff"]        = part ? staffFromTrack(part, cr->track()) : 0;
    payload["articulation"] = articulationNameFromSymId(a->symId());
    if (note) {
        payload["pitch"] = pitchJsonFromNote(note);
    }
    return payload;
}

QJsonObject OperationTranslator::buildRemoveArticulation(EngravingObject* art,
                                                          const QString& partId,
                                                          Note* note)
{
    auto* a = static_cast<Articulation*>(art);
    ChordRest* cr = a->chordRest();
    Part* part = cr && cr->staff() ? cr->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]         = QStringLiteral("RemoveArticulation");
    payload["part_id"]      = partId;
    payload["beat"]         = beatJson(cr ? cr->tick() : Fraction());
    payload["voice"]        = part ? voiceFromTrack(part, cr->track()) : 1;
    payload["staff"]        = part ? staffFromTrack(part, cr->track()) : 0;
    payload["articulation"] = articulationNameFromSymId(a->symId());
    if (note) {
        payload["pitch"] = pitchJsonFromNote(note);
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — dynamics (beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddDynamic(EngravingObject* dyn,
                                                  const QString& partId)
{
    auto* d = static_cast<Dynamic*>(dyn);
    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddDynamic");
    payload["part_id"] = partId;
    payload["kind"]    = dynamicKindName(d->dynamicType());
    payload["beat"]    = beatJson(d->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetDynamic(const QString& partId,
                                                  const Fraction& tick,
                                                  const QString& kind)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetDynamic");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["kind"]    = kind;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveDynamic(const QString& partId,
                                                     const Fraction& tick,
                                                     const QString& kind)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveDynamic");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["kind"]    = kind;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — slurs (dual-coordinate)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddSlur(EngravingObject* slurObj,
                                               const QString& partId)
{
    auto* slur = static_cast<Slur*>(slurObj);
    EngravingItem* startEl = slur->startElement();
    EngravingItem* endEl   = slur->endElement();
    ChordRest* startCr = (startEl && startEl->isChordRest())
                         ? toChordRest(startEl) : nullptr;
    ChordRest* endCr   = (endEl && endEl->isChordRest())
                         ? toChordRest(endEl)   : nullptr;

    Part* part = startCr && startCr->staff()
                 ? startCr->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddSlur");
    payload["part_id"] = partId;

    if (startCr && part) {
        payload["start_beat"]  = beatJson(startCr->tick());
        payload["start_voice"] = voiceFromTrack(part, startCr->track());
        payload["start_staff"] = staffFromTrack(part, startCr->track());
        if (startCr->isChord()) {
            Chord* ch = toChord(static_cast<EngravingItem*>(startCr));
            if (!ch->notes().empty()) {
                payload["start_pitch"] = pitchJsonFromNote(ch->notes().front());
            }
        }
    }
    if (endCr && part) {
        payload["end_beat"]  = beatJson(endCr->tick());
        payload["end_voice"] = voiceFromTrack(part, endCr->track());
        payload["end_staff"] = staffFromTrack(part, endCr->track());
        if (endCr->isChord()) {
            Chord* ch = toChord(static_cast<EngravingItem*>(endCr));
            if (!ch->notes().empty()) {
                payload["end_pitch"] = pitchJsonFromNote(ch->notes().front());
            }
        }
    }
    return payload;
}

QJsonObject OperationTranslator::buildRemoveSlur(EngravingObject* slurObj,
                                                  const QString& partId)
{
    auto* slur = static_cast<Slur*>(slurObj);
    EngravingItem* startEl = slur->startElement();
    EngravingItem* endEl   = slur->endElement();
    ChordRest* startCr = (startEl && startEl->isChordRest())
                         ? toChordRest(startEl) : nullptr;
    ChordRest* endCr   = (endEl && endEl->isChordRest())
                         ? toChordRest(endEl)   : nullptr;

    Part* part = startCr && startCr->staff()
                 ? startCr->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveSlur");
    payload["part_id"] = partId;

    if (startCr && part) {
        payload["start_beat"]  = beatJson(startCr->tick());
        payload["start_voice"] = voiceFromTrack(part, startCr->track());
        payload["start_staff"] = staffFromTrack(part, startCr->track());
        if (startCr->isChord()) {
            Chord* ch = toChord(static_cast<EngravingItem*>(startCr));
            if (!ch->notes().empty()) {
                payload["start_pitch"] = pitchJsonFromNote(ch->notes().front());
            }
        }
    }
    if (endCr && part) {
        payload["end_beat"]  = beatJson(endCr->tick());
        payload["end_voice"] = voiceFromTrack(part, endCr->track());
        payload["end_staff"] = staffFromTrack(part, endCr->track());
        if (endCr->isChord()) {
            Chord* ch = toChord(static_cast<EngravingItem*>(endCr));
            if (!ch->notes().empty()) {
                payload["end_pitch"] = pitchJsonFromNote(ch->notes().front());
            }
        }
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — hairpins (beat-range)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddHairpin(EngravingObject* /*hp*/,
                                                  const QString& partId,
                                                  const Fraction& startTick,
                                                  const Fraction& endTick,
                                                  bool isCrescendo)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddHairpin");
    payload["part_id"]    = partId;
    payload["kind"]       = isCrescendo ? QStringLiteral("crescendo")
                                        : QStringLiteral("diminuendo");
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    return payload;
}

QJsonObject OperationTranslator::buildRemoveHairpin(const QString& partId,
                                                     const Fraction& startTick,
                                                     const Fraction& endTick,
                                                     const QString& kind)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("RemoveHairpin");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    payload["kind"]       = kind;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — tuplets (coordinate-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTuplet(EngravingObject* tup,
                                                 const QString& partId,
                                                 int actualNotes,
                                                 int normalNotes)
{
    auto* t = static_cast<Tuplet*>(tup);
    Part* part = static_cast<EngravingItem*>(tup)->part();

    // Build member payloads with coordinate info.
    QJsonArray members;
    const auto& elems = t->elements();
    for (int i = 0; i < static_cast<int>(elems.size()); ++i) {
        DurationElement* elem = elems[i];
        if (!elem->isChordRest()) {
            continue;
        }
        auto* cr = static_cast<ChordRest*>(elem);
        QJsonObject m;
        m["beat"]  = beatJson(cr->tick());
        m["voice"] = part ? voiceFromTrack(part, cr->track()) : 1;
        m["staff"] = part ? staffFromTrack(part, cr->track()) : 0;

        QJsonObject dur;
        dur["type"] = durationTypeName(cr->durationType().type());
        dur["dots"] = cr->durationType().dots();
        m["duration"] = dur;

        if (cr->isChord()) {
            Chord* chord = toChord(static_cast<EngravingItem*>(cr));
            if (chord->notes().size() == 1) {
                Note* n = chord->notes().front();
                m["kind"]  = QStringLiteral("note");
                m["pitch"] = pitchJsonFromNote(n);
            } else {
                m["kind"] = QStringLiteral("chord");
                QJsonArray pitches;
                for (Note* n : chord->notes()) {
                    pitches.append(pitchJsonFromNote(n));
                }
                m["pitches"] = pitches;
            }
        } else {
            m["kind"] = QStringLiteral("rest");
        }
        members.append(m);
    }

    QJsonObject baseDur;
    baseDur["type"] = durationTypeName(t->baseLen().type());
    baseDur["dots"] = t->baseLen().dots();

    QJsonObject payload;
    payload["type"]          = QStringLiteral("AddTuplet");
    payload["part_id"]       = partId;
    payload["actual_notes"]  = actualNotes;
    payload["normal_notes"]  = normalNotes;
    payload["base_duration"] = baseDur;
    payload["beat"]          = beatJson(t->tick());
    payload["voice"]         = part ? voiceFromTrack(part, t->track()) : 1;
    payload["staff"]         = part ? staffFromTrack(part, t->track()) : 0;
    payload["members"]       = members;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveTuplet(const QString& partId,
                                                    const Fraction& tick,
                                                    int voice, int staff)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveTuplet");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["voice"]   = voice;
    payload["staff"]   = staff;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — chord symbols (score-global, beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddChordSymbol(int beatNum, int beatDen,
                                                      const QString& name)
{
    QJsonObject beat;
    beat["numerator"]   = beatNum;
    beat["denominator"] = beatDen;

    QJsonObject payload;
    payload["type"] = QStringLiteral("AddChordSymbol");
    payload["name"] = name;
    payload["beat"] = beat;
    return payload;
}

QJsonObject OperationTranslator::buildSetChordSymbol(int beatNum, int beatDen,
                                                      const QString& name)
{
    QJsonObject beat;
    beat["numerator"]   = beatNum;
    beat["denominator"] = beatDen;

    QJsonObject payload;
    payload["type"] = QStringLiteral("SetChordSymbol");
    payload["beat"] = beat;
    payload["name"] = name;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveChordSymbol(int beatNum, int beatDen,
                                                         const QString& name)
{
    QJsonObject beat;
    beat["numerator"]   = beatNum;
    beat["denominator"] = beatDen;

    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveChordSymbol");
    payload["beat"] = beat;
    payload["name"] = name;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — lyrics (coordinate-addressed)
// ---------------------------------------------------------------------------

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

QJsonObject OperationTranslator::buildAddLyric(EngravingObject* lyr,
                                                const QString& partId,
                                                int voice, int staff,
                                                int verse,
                                                const QString& syllabic,
                                                const QString& text)
{
    auto* l = static_cast<Lyrics*>(lyr);
    QJsonObject payload;
    payload["type"]     = QStringLiteral("AddLyric");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(l->tick());
    payload["voice"]    = voice;
    payload["staff"]    = staff;
    payload["verse"]    = verse;
    payload["syllabic"] = syllabic;
    payload["text"]     = text;
    return payload;
}

QJsonObject OperationTranslator::buildSetLyric(const QString& partId,
                                                const Fraction& tick,
                                                int voice, int staff, int verse,
                                                const QString& text,
                                                const QString& syllabic)
{
    QJsonObject payload;
    payload["type"]     = QStringLiteral("SetLyric");
    payload["part_id"]  = partId;
    payload["beat"]     = beatJson(tick);
    payload["voice"]    = voice;
    payload["staff"]    = staff;
    payload["verse"]    = verse;
    payload["text"]     = text;
    payload["syllabic"] = syllabic;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveLyric(const QString& partId,
                                                   const Fraction& tick,
                                                   int voice, int staff, int verse)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveLyric");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["voice"]   = voice;
    payload["staff"]   = staff;
    payload["verse"]   = verse;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — staff text (part-scoped, beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddStaffText(EngravingObject* text,
                                                    const QString& partId)
{
    auto* t = static_cast<StaffText*>(text);
    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddStaffText");
    payload["part_id"] = partId;
    payload["text"]    = t->plainText().toQString();
    payload["beat"]    = beatJson(t->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetStaffText(const QString& partId,
                                                    const Fraction& tick,
                                                    const QString& text)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetStaffText");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["text"]    = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveStaffText(const QString& partId,
                                                       const Fraction& tick,
                                                       const QString& text)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveStaffText");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["text"]    = text;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — system text (score-global, beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddSystemText(EngravingObject* text)
{
    auto* t = static_cast<SystemText*>(text);
    QJsonObject payload;
    payload["type"] = QStringLiteral("AddSystemText");
    payload["text"] = t->plainText().toQString();
    payload["beat"] = beatJson(t->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetSystemText(const Fraction& tick,
                                                     const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetSystemText");
    payload["beat"] = beatJson(tick);
    payload["text"] = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveSystemText(const Fraction& tick,
                                                        const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveSystemText");
    payload["beat"] = beatJson(tick);
    payload["text"] = text;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — rehearsal marks (score-global, beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddRehearsalMark(EngravingObject* mark)
{
    auto* m = static_cast<RehearsalMark*>(mark);
    QJsonObject payload;
    payload["type"] = QStringLiteral("AddRehearsalMark");
    payload["text"] = m->plainText().toQString();
    payload["beat"] = beatJson(m->tick());
    return payload;
}

QJsonObject OperationTranslator::buildSetRehearsalMark(const Fraction& tick,
                                                        const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("SetRehearsalMark");
    payload["beat"] = beatJson(tick);
    payload["text"] = text;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveRehearsalMark(const Fraction& tick,
                                                          const QString& text)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveRehearsalMark");
    payload["beat"] = beatJson(tick);
    payload["text"] = text;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — octave lines (part-scoped, beat-range)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddOctaveLine(EngravingObject* ottava,
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
    payload["part_id"]    = partId;
    payload["kind"]       = kind;
    payload["start_beat"] = beatJson(o->tick());
    payload["end_beat"]   = beatJson(o->tick2());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveOctaveLine(const QString& partId,
                                                         const Fraction& startTick,
                                                         const Fraction& endTick,
                                                         const QString& kind)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("RemoveOctaveLine");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    payload["kind"]       = kind;
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — glissandos (dual-coordinate)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddGlissando(EngravingObject* glissObj,
                                                     const QString& partId)
{
    auto* g = static_cast<Glissando*>(glissObj);

    const QString style = (g->glissandoType() == GlissandoType::WAVY)
                          ? QStringLiteral("wavy")
                          : QStringLiteral("straight");

    EngravingItem* startEl = g->startElement();
    EngravingItem* endEl   = g->endElement();
    Note* startNote = (startEl && startEl->isNote()) ? toNote(startEl) : nullptr;
    Note* endNote   = (endEl && endEl->isNote())     ? toNote(endEl)   : nullptr;

    Part* part = startNote && startNote->staff()
                 ? startNote->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("AddGlissando");
    payload["part_id"] = partId;
    payload["style"]   = style;

    if (startNote && part) {
        payload["start_beat"]  = beatJson(startNote->chord()->tick());
        payload["start_pitch"] = pitchJsonFromNote(startNote);
        payload["start_voice"] = voiceFromTrack(part, startNote->track());
        payload["start_staff"] = staffFromTrack(part, startNote->track());
    }
    if (endNote && part) {
        payload["end_beat"]  = beatJson(endNote->chord()->tick());
        payload["end_pitch"] = pitchJsonFromNote(endNote);
        payload["end_voice"] = voiceFromTrack(part, endNote->track());
        payload["end_staff"] = staffFromTrack(part, endNote->track());
    }
    return payload;
}

QJsonObject OperationTranslator::buildRemoveGlissando(EngravingObject* glissObj,
                                                        const QString& partId)
{
    auto* g = static_cast<Glissando*>(glissObj);

    EngravingItem* startEl = g->startElement();
    EngravingItem* endEl   = g->endElement();
    Note* startNote = (startEl && startEl->isNote()) ? toNote(startEl) : nullptr;
    Note* endNote   = (endEl && endEl->isNote())     ? toNote(endEl)   : nullptr;

    Part* part = startNote && startNote->staff()
                 ? startNote->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveGlissando");
    payload["part_id"] = partId;

    if (startNote && part) {
        payload["start_beat"]  = beatJson(startNote->chord()->tick());
        payload["start_pitch"] = pitchJsonFromNote(startNote);
        payload["start_voice"] = voiceFromTrack(part, startNote->track());
        payload["start_staff"] = staffFromTrack(part, startNote->track());
    }
    if (endNote && part) {
        payload["end_beat"]  = beatJson(endNote->chord()->tick());
        payload["end_pitch"] = pitchJsonFromNote(endNote);
        payload["end_voice"] = voiceFromTrack(part, endNote->track());
        payload["end_staff"] = staffFromTrack(part, endNote->track());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Guitar bends (dual-coordinate, note-anchored)
// ---------------------------------------------------------------------------

static QString guitarBendTypeName(GuitarBendType t)
{
    switch (t) {
    case GuitarBendType::BEND:            return QStringLiteral("BEND");
    case GuitarBendType::PRE_BEND:        return QStringLiteral("PRE_BEND");
    case GuitarBendType::GRACE_NOTE_BEND: return QStringLiteral("GRACE_NOTE_BEND");
    case GuitarBendType::SLIGHT_BEND:     return QStringLiteral("SLIGHT_BEND");
    case GuitarBendType::DIVE:            return QStringLiteral("DIVE");
    case GuitarBendType::PRE_DIVE:        return QStringLiteral("PRE_DIVE");
    case GuitarBendType::DIP:             return QStringLiteral("DIP");
    case GuitarBendType::SCOOP:           return QStringLiteral("SCOOP");
    }
    return QStringLiteral("BEND");
}

QJsonObject OperationTranslator::buildAddGuitarBend(EngravingObject* bendObj,
                                                      const QString& partId)
{
    auto* bend = static_cast<GuitarBend*>(bendObj);

    Note* startNote = bend->startNote();
    Note* endNote   = bend->endNote();

    Part* part = startNote && startNote->staff()
                 ? startNote->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]      = QStringLiteral("AddGuitarBend");
    payload["part_id"]   = partId;
    payload["bend_type"] = guitarBendTypeName(bend->bendType());

    if (startNote && part) {
        payload["start_beat"]  = beatJson(startNote->chord()->tick());
        payload["start_pitch"] = pitchJsonFromNote(startNote);
        payload["start_voice"] = voiceFromTrack(part, startNote->track());
        payload["start_staff"] = staffFromTrack(part, startNote->track());
    }
    if (endNote && part) {
        payload["end_beat"]  = beatJson(endNote->chord()->tick());
        payload["end_pitch"] = pitchJsonFromNote(endNote);
        payload["end_voice"] = voiceFromTrack(part, endNote->track());
        payload["end_staff"] = staffFromTrack(part, endNote->track());
    }
    return payload;
}

QJsonObject OperationTranslator::buildRemoveGuitarBend(EngravingObject* bendObj,
                                                         const QString& partId)
{
    auto* bend = static_cast<GuitarBend*>(bendObj);

    Note* startNote = bend->startNote();
    Note* endNote   = bend->endNote();

    Part* part = startNote && startNote->staff()
                 ? startNote->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveGuitarBend");
    payload["part_id"] = partId;

    if (startNote && part) {
        payload["start_beat"]  = beatJson(startNote->chord()->tick());
        payload["start_pitch"] = pitchJsonFromNote(startNote);
        payload["start_voice"] = voiceFromTrack(part, startNote->track());
        payload["start_staff"] = staffFromTrack(part, startNote->track());
    }
    if (endNote && part) {
        payload["end_beat"]  = beatJson(endNote->chord()->tick());
        payload["end_pitch"] = pitchJsonFromNote(endNote);
        payload["end_voice"] = voiceFromTrack(part, endNote->track());
        payload["end_staff"] = staffFromTrack(part, endNote->track());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — pedal lines (part-scoped, beat-range)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddPedalLine(EngravingObject* pedal,
                                                     const QString& partId)
{
    auto* p = static_cast<Pedal*>(pedal);
    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddPedalLine");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(p->tick());
    payload["end_beat"]   = beatJson(p->tick2());
    return payload;
}

QJsonObject OperationTranslator::buildRemovePedalLine(const QString& partId,
                                                       const Fraction& startTick,
                                                       const Fraction& endTick)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("RemovePedalLine");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    return payload;
}

// ---------------------------------------------------------------------------
// Advanced spanners — trill lines (part-scoped, beat-range)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTrillLine(EngravingObject* trill,
                                                     const QString& partId)
{
    auto* t = static_cast<Trill*>(trill);

    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddTrillLine");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(t->tick());
    payload["end_beat"]   = beatJson(t->tick2());

    if (t->accidental()) {
        AccidentalType at = t->accidental()->accidentalType();
        QString accName;
        switch (at) {
        case AccidentalType::FLAT:    accName = QStringLiteral("flat");         break;
        case AccidentalType::SHARP:   accName = QStringLiteral("sharp");        break;
        case AccidentalType::NATURAL: accName = QStringLiteral("natural");      break;
        case AccidentalType::FLAT2:   accName = QStringLiteral("double-flat");  break;
        case AccidentalType::SHARP2:  accName = QStringLiteral("double-sharp"); break;
        default:                      accName = QString();                      break;
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

QJsonObject OperationTranslator::buildRemoveTrillLine(const QString& partId,
                                                        const Fraction& startTick,
                                                        const Fraction& endTick)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("RemoveTrillLine");
    payload["part_id"]    = partId;
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 3 builders — arpeggios (coordinate-addressed)
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
                                                    const QString& partId)
{
    auto* a = static_cast<Arpeggio*>(arp);
    Chord* ch = a->chord();
    Part* part = ch && ch->staff() ? ch->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]      = QStringLiteral("AddArpeggio");
    payload["part_id"]   = partId;
    payload["beat"]      = beatJson(ch ? ch->tick() : Fraction());
    payload["voice"]     = part ? voiceFromTrack(part, ch->track()) : 1;
    payload["staff"]     = part ? staffFromTrack(part, ch->track()) : 0;
    payload["direction"] = arpeggioTypeName(a->arpeggioType());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveArpeggio(const QString& partId,
                                                       const Fraction& tick,
                                                       int voice, int staff)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveArpeggio");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["voice"]   = voice;
    payload["staff"]   = staff;
    return payload;
}

// ---------------------------------------------------------------------------
// Grace note builders (coordinate-addressed)
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
                                                    const QString& partId)
{
    auto* gc = static_cast<Chord*>(graceObj);
    Chord* parentChord = toChord(gc->explicitParent());
    Part* part = gc->staff() ? gc->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]       = QStringLiteral("AddGraceNote");
    payload["part_id"]    = partId;
    payload["beat"]       = beatJson(parentChord ? parentChord->tick() : gc->tick());
    payload["voice"]      = part ? voiceFromTrack(part, gc->track()) : 1;
    payload["staff"]      = part ? staffFromTrack(part, gc->track()) : 0;
    payload["order"]      = static_cast<int>(gc->graceIndex());
    payload["grace_type"] = graceNoteTypeName(gc->noteType());

    if (!gc->notes().empty()) {
        Note* n = gc->notes().front();
        payload["pitch"] = pitchJsonFromNote(n);
    }

    return payload;
}

QJsonObject OperationTranslator::buildRemoveGraceNote(const QString& partId,
                                                       const Fraction& tick,
                                                       int voice, int staff,
                                                       int index)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveGraceNote");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["voice"]   = voice;
    payload["staff"]   = staff;
    payload["order"]   = index;
    return payload;
}

// ---------------------------------------------------------------------------
// Breath mark / caesura builders (beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddBreathMark(EngravingObject* breathObj,
                                                     const QString& partId)
{
    auto* b = static_cast<Breath*>(breathObj);
    QJsonObject payload;
    payload["type"]        = QStringLiteral("AddBreathMark");
    payload["part_id"]     = partId;
    payload["beat"]        = beatJson(b->segment()->tick());
    payload["breath_type"] = breathTypeToString(b->symId());
    payload["pause"]       = b->pause();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveBreathMark(const QString& partId,
                                                        const Fraction& tick,
                                                        const QString& breathType)
{
    QJsonObject payload;
    payload["type"]        = QStringLiteral("RemoveBreathMark");
    payload["part_id"]     = partId;
    payload["beat"]        = beatJson(tick);
    payload["breath_type"] = breathType;
    return payload;
}

// ---------------------------------------------------------------------------
// Tremolo builders — single-note (coordinate-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTremolo(EngravingObject* trem,
                                                  const QString& partId)
{
    auto* t = static_cast<TremoloSingleChord*>(trem);
    Chord* ch = t->chord();
    Part* part = ch && ch->staff() ? ch->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]         = QStringLiteral("AddTremolo");
    payload["part_id"]      = partId;
    payload["beat"]         = beatJson(ch ? ch->tick() : Fraction());
    payload["voice"]        = part ? voiceFromTrack(part, ch->track()) : 1;
    payload["staff"]        = part ? staffFromTrack(part, ch->track()) : 0;
    payload["tremolo_type"] = tremoloTypeToString(t->tremoloType());
    return payload;
}

QJsonObject OperationTranslator::buildRemoveTremolo(const QString& partId,
                                                     const Fraction& tick,
                                                     int voice, int staff)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveTremolo");
    payload["part_id"] = partId;
    payload["beat"]    = beatJson(tick);
    payload["voice"]   = voice;
    payload["staff"]   = staff;
    return payload;
}

// ---------------------------------------------------------------------------
// Two-note tremolo builders (dual-anchor, coordinate-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildAddTwoNoteTremolo(EngravingObject* trem,
                                                         const QString& partId)
{
    auto* t = static_cast<TremoloTwoChord*>(trem);
    Chord* ch1 = t->chord1();
    Chord* ch2 = t->chord2();
    Part* part = ch1 && ch1->staff() ? ch1->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]         = QStringLiteral("AddTwoNoteTremolo");
    payload["part_id"]      = partId;
    payload["tremolo_type"] = tremoloTypeToString(t->tremoloType());

    if (ch1 && part) {
        payload["start_beat"]  = beatJson(ch1->tick());
        payload["start_voice"] = voiceFromTrack(part, ch1->track());
        payload["start_staff"] = staffFromTrack(part, ch1->track());
        if (!ch1->notes().empty()) {
            payload["start_pitch"] = pitchJsonFromNote(ch1->notes().front());
        }
    }
    if (ch2 && part) {
        payload["end_beat"]  = beatJson(ch2->tick());
        payload["end_voice"] = voiceFromTrack(part, ch2->track());
        payload["end_staff"] = staffFromTrack(part, ch2->track());
        if (!ch2->notes().empty()) {
            payload["end_pitch"] = pitchJsonFromNote(ch2->notes().front());
        }
    }
    return payload;
}

QJsonObject OperationTranslator::buildRemoveTwoNoteTremolo(EngravingObject* trem,
                                                            const QString& partId)
{
    auto* t = static_cast<TremoloTwoChord*>(trem);
    Chord* ch1 = t->chord1();
    Chord* ch2 = t->chord2();
    Part* part = ch1 && ch1->staff() ? ch1->staff()->part() : nullptr;

    QJsonObject payload;
    payload["type"]    = QStringLiteral("RemoveTwoNoteTremolo");
    payload["part_id"] = partId;

    if (ch1 && part) {
        payload["start_beat"]  = beatJson(ch1->tick());
        payload["start_voice"] = voiceFromTrack(part, ch1->track());
        payload["start_staff"] = staffFromTrack(part, ch1->track());
        if (!ch1->notes().empty()) {
            payload["start_pitch"] = pitchJsonFromNote(ch1->notes().front());
        }
    }
    if (ch2 && part) {
        payload["end_beat"]  = beatJson(ch2->tick());
        payload["end_voice"] = voiceFromTrack(part, ch2->track());
        payload["end_staff"] = staffFromTrack(part, ch2->track());
        if (!ch2->notes().empty()) {
            payload["end_pitch"] = pitchJsonFromNote(ch2->notes().front());
        }
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — repeat barlines
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildSetStartRepeat(const Fraction& tick, bool enabled)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetStartRepeat");
    payload["beat"]    = beatJson(tick);
    payload["enabled"] = enabled;
    return payload;
}

QJsonObject OperationTranslator::buildSetEndRepeat(const Fraction& tick,
                                                    bool enabled, int count)
{
    QJsonObject payload;
    payload["type"]    = QStringLiteral("SetEndRepeat");
    payload["beat"]    = beatJson(tick);
    payload["enabled"] = enabled;
    payload["count"]   = count;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — volta (beat-range)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertVolta(EngravingObject* volta)
{
    auto* v = static_cast<Volta*>(volta);
    QJsonArray numbers;
    for (int n : v->endings()) {
        numbers.append(n);
    }
    const bool openEnd = (v->voltaType() == Volta::Type::OPEN);

    QJsonObject payload;
    payload["type"]       = QStringLiteral("InsertVolta");
    payload["start_beat"] = beatJson(v->tick());
    payload["end_beat"]   = beatJson(v->tick2());
    payload["numbers"]    = numbers;
    payload["open_end"]   = openEnd;
    return payload;
}

QJsonObject OperationTranslator::buildRemoveVolta(const Fraction& startTick,
                                                   const Fraction& endTick,
                                                   const QJsonArray& numbers)
{
    QJsonObject payload;
    payload["type"]       = QStringLiteral("RemoveVolta");
    payload["start_beat"] = beatJson(startTick);
    payload["end_beat"]   = beatJson(endTick);
    payload["numbers"]    = numbers;
    return payload;
}

QJsonObject OperationTranslator::buildSetVoltaNumbers(EngravingObject* voltaObj,
                                                       const QJsonArray& oldNumbers)
{
    auto* v = static_cast<Volta*>(voltaObj);
    QJsonArray newNumbers;
    for (int n : v->endings()) {
        newNumbers.append(n);
    }
    QJsonObject payload;
    payload["type"]        = QStringLiteral("SetVoltaNumbers");
    payload["start_beat"]  = beatJson(v->tick());
    payload["old_numbers"] = oldNumbers;
    payload["numbers"]     = newNumbers;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — markers (beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertMarker(EngravingObject* marker)
{
    auto* m = static_cast<Marker*>(marker);
    QJsonObject payload;
    payload["type"]  = QStringLiteral("InsertMarker");
    payload["beat"]  = beatJson(m->measure() ? m->measure()->tick() : m->tick());
    payload["kind"]  = markerKindName(m->markerType());
    payload["label"] = m->label().toQString();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveMarker(const Fraction& tick,
                                                    const QString& kind)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveMarker");
    payload["beat"] = beatJson(tick);
    payload["kind"] = kind;
    return payload;
}

// ---------------------------------------------------------------------------
// Tier 4 builders — jumps (beat-addressed)
// ---------------------------------------------------------------------------

QJsonObject OperationTranslator::buildInsertJump(EngravingObject* jump)
{
    auto* j = static_cast<Jump*>(jump);
    QJsonObject payload;
    payload["type"]        = QStringLiteral("InsertJump");
    payload["beat"]        = beatJson(j->tick());
    payload["jump_to"]     = j->jumpTo().toQString();
    payload["play_until"]  = j->playUntil().toQString();
    payload["continue_at"] = j->continueAt().toQString();
    payload["text"]        = j->plainText().toQString();
    return payload;
}

QJsonObject OperationTranslator::buildRemoveJump(const Fraction& tick)
{
    QJsonObject payload;
    payload["type"] = QStringLiteral("RemoveJump");
    payload["beat"] = beatJson(tick);
    return payload;
}
