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

    // Read-only view of the element→uuid reverse map, used by OperationTranslator
    // to produce DeleteEvent / SetPitch ops from local MuseScore changes.
    const QHash<mu::engraving::EngravingObject*, QString>& elementToUuid() const
    {
        return m_elementToUuid;
    }

    // Read-only view of the Tier 3 element→uuid reverse map, used by
    // EditudeTestServer to resolve articulation/dynamic/slur/hairpin/lyric UUIDs.
    const QHash<mu::engraving::EngravingObject*, QString>& tier3ElementToUuid() const
    {
        return m_tier3ElementToUuid;
    }

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
    // Note: parts baked into an MSCZ snapshot do not carry editude UUIDs;
    // this map is populated incrementally via applyAddPart / applyRemovePart.
    void bootstrapPartMap(mu::engraving::Score* score);

    // Clear all internal UUID ↔ element maps.  Called when connecting to a
    // new project so stale mappings from the previous session are discarded.
    void reset();

    // Register an existing Part* with an editude UUID without creating a new part.
    // Used when the OperationTranslator lazily assigns UUIDs to parts that were
    // loaded from an MSCX file (rather than created via AddPart from the server).
    void registerPart(mu::engraving::Part* part, const QString& uuid);

    // Shared static helpers — also used by EditudeTestServer.
    static int pitchToMidi(const QString& step, int octave, const QString& accidental);
    static mu::engraving::DurationType parseDurationType(const QString& name);

private:
    // Tier 1 — stream event operations
    bool applyInsertNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertRest(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertChord(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyDeleteEvent(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetPitch(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyAddChordNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveChordNote(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetDuration(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetTie(mu::engraving::Score* score, const QJsonObject& payload);
    bool applySetTrack(mu::engraving::Score* score, const QJsonObject& payload);

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

    // Tier 4 — navigation marks
    bool applyInsertVolta(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveVolta(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertMarker(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveMarker(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertJump(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyRemoveJump(mu::engraving::Score* score, const QJsonObject& payload);

    // Structural ops
    bool applySetScoreMetadata(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyInsertBeats(mu::engraving::Score* score, const QJsonObject& payload);
    bool applyDeleteBeats(mu::engraving::Score* score, const QJsonObject& payload);

    // Tier 1+2 UUID ↔ element maps, maintained across all apply* calls.
    // Keyed by the "id" field present in Insert* ops (and echoed in op_ack.payload).
    QHash<QString, mu::engraving::EngravingObject*> m_uuidToElement;
    QHash<mu::engraving::EngravingObject*, QString> m_elementToUuid;

    // Part UUID map, populated by AddPart once that handler is implemented.
    // Reserved for SetKeySignature / SetClef / SetPartName / SetStaffCount / RemovePart.
    QHash<QString, mu::engraving::Part*> m_partUuidToPart;

    // Tier 3 UUID ↔ element maps.
    // Keyed by the "id" field in AddArticulation / AddDynamic / AddSlur / etc.
    QHash<QString, mu::engraving::EngravingObject*> m_tier3UuidToElement;
    QHash<mu::engraving::EngravingObject*, QString> m_tier3ElementToUuid;
};
} // namespace mu::editude::internal
