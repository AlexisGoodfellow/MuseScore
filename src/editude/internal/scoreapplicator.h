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
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

#include "engraving/dom/engravingobject.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/types/types.h"

namespace mu::editude::internal {
class ScoreApplicator {
public:
    // Dispatches on payload["type"]. Returns false for unrecognised types.
    bool apply(mu::engraving::Score* score, const QJsonObject& payload);

    // Read-only view of the part UUID → Part* map, used by EditudeService
    // to sync part registrations from the applicator into the translator
    // after applying sync ops.
    const QHash<QString, mu::engraving::Part*>& partUuidToPart() const
    {
        return m_partUuidToPart;
    }

    // Bootstraps m_partUuidToPart from ApplyAddPart registrations.
    // Call after loading a snapshot so that part-keyed ops (SetPartName,
    // SetKeySignature, SetClef, RemovePart) can resolve Part* by UUID.
    void bootstrapPartMap(mu::engraving::Score* score);

    // Clear internal state. Called when connecting to a new project so
    // stale mappings from the previous session are discarded.
    void reset();

    // Register an existing Part* with an editude UUID without creating a new part.
    // Used when the OperationTranslator lazily assigns UUIDs to parts that were
    // loaded from an MSCX file (rather than created via AddPart from the server).
    void registerPart(mu::engraving::Part* part, const QString& uuid);

    // Shared static helpers — also used by EditudeTestDriver.
    static int pitchToMidi(const QString& step, int octave, const QString& accidental);
    static mu::engraving::DurationType parseDurationType(const QString& name);

private:
    // Resolve Part* from part_id in payload. Returns nullptr on failure.
    mu::engraving::Part* resolvePart(const QJsonObject& op) const;

    // Tier 1 — stream event operations (coordinate-addressed)
    bool applyInsertNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertRest(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyDeleteNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyDeleteRest(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetPitch(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetDuration(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetTie(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetVoice(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 2 — score directive operations
    bool applySetTimeSignature(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetTempo(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetKeySignature(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetClef(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetPartName(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetStaffCount(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyAddPart(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemovePart(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetPartInstrument(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetStringData(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetCapo(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetTabNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetDrumset(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetNoteHead(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — articulations
    bool applyAddArticulation(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveArticulation(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — dynamics
    bool applyAddDynamic(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetDynamic(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveDynamic(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — slurs
    bool applyAddSlur(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveSlur(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — hairpins
    bool applyAddHairpin(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveHairpin(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — tuplets
    bool applyAddTuplet(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveTuplet(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — lyrics
    bool applyAddLyric(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetLyric(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveLyric(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — chord symbols (score-global, no part_id)
    bool applyAddChordSymbol(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetChordSymbol(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveChordSymbol(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — staff text (part-scoped)
    bool applyAddStaffText(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetStaffText(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveStaffText(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — system text (score-global)
    bool applyAddSystemText(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetSystemText(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveSystemText(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 3 — rehearsal marks (score-global)
    bool applyAddRehearsalMark(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetRehearsalMark(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveRehearsalMark(mu::engraving::Score* score, const QJsonObject& payload);

    // Advanced spanners — octave lines
    bool applyAddOctaveLine(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveOctaveLine(mu::engraving::Score* score, const QJsonObject& payload);
    // Advanced spanners — glissandos
    bool applyAddGlissando(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveGlissando(mu::engraving::Score* score, const QJsonObject& payload);
    // Advanced spanners — pedal lines
    bool applyAddPedalLine(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemovePedalLine(mu::engraving::Score* score, const QJsonObject& payload);
    // Advanced spanners — trill lines
    bool applyAddTrillLine(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveTrillLine(mu::engraving::Score* score, const QJsonObject& payload);
    // Arpeggios
    bool applyAddArpeggio(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveArpeggio(mu::engraving::Score* score, const QJsonObject& payload);
    // Grace notes
    bool applyAddGraceNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveGraceNote(mu::engraving::Score* score, const QJsonObject& payload);
    // Breath marks / caesuras
    bool applyAddBreathMark(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveBreathMark(mu::engraving::Score* score, const QJsonObject& payload);
    // Tremolos (single-note)
    bool applyAddTremolo(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveTremolo(mu::engraving::Score* score, const QJsonObject& payload);
    // Two-note tremolos
    bool applyAddTwoNoteTremolo(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveTwoNoteTremolo(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 4 — navigation marks
    bool applySetStartRepeat(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetEndRepeat(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertVolta(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveVolta(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertMarker(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveMarker(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertJump(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveJump(mu::engraving::Score* score, const QJsonObject& payload);

    // Structural ops
    bool applySetScoreMetadata(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetMeasureLen(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertBeats(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyDeleteBeats(mu::engraving::Score* score, const QJsonObject& payload);

    // Part UUID map — populated by AddPart, retained (parts are axes, not points).
    QHash<QString, mu::engraving::Part*> m_partUuidToPart;
};
} // namespace mu::editude::internal
