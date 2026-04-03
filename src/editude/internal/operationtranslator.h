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

#include <functional>
#include <map>
#include <unordered_set>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include "engraving/editing/undo.h"
#include "engraving/dom/engravingobject.h"
#include "engraving/dom/note.h"
#include "engraving/dom/property.h"
#include "engraving/dom/score.h"
#include "engraving/types/types.h"

namespace mu::engraving {
class Chord;
class ChordRest;
class Clef;
class KeySig;
class Part;
class Rest;
class TempoText;
class Tie;
class TimeSig;
}

namespace mu::editude::internal {
class OperationTranslator
{
public:
    // Translates all local MuseScore changes from one committed undo transaction
    // into a list of OT op payloads ready to be sent to the server.
    //
    // Coordinate-based: elements are addressed by (part_id, beat, pitch, voice,
    // staff) — no UUID maps needed. Part UUIDs are resolved from m_knownPartUuids.
    QVector<QJsonObject> translateAll(
        const std::map<mu::engraving::EngravingObject*,
                       std::unordered_set<mu::engraving::CommandType>>& changedObjects,
        const mu::engraving::PropertyIdSet& changedPropertyIdSet,
        const QMap<QString, QString>& changedMetaTags = {});

    // Bootstraps m_knownPartUuids for all parts in the score that do not yet
    // have an assigned editude UUID (e.g. parts loaded from an MSCX file).
    // Returns AddPart ops for each newly registered part; the caller should
    // send these to the server so it learns about the pre-existing parts.
    // Also calls registerCallback(part, uuid) for each new part so the caller
    // can register them in its ScoreApplicator.
    QVector<QJsonObject> bootstrapPartsFromScore(
        mu::engraving::Score* score,
        std::function<void(mu::engraving::Part*, const QString&)> registerCallback = {});

    // Register a known part in m_knownPartUuids without emitting an AddPart op.
    void registerKnownPart(mu::engraving::Part* part, const QString& uuid);

    // Clear all internal state.
    void reset();

    const QHash<mu::engraving::Part*, QString>& knownPartUuids() const
    {
        return m_knownPartUuids;
    }

private:
    // Resolve the editude UUID for a Part*. Lazily assigns if not yet known.
    QString resolvePartUuid(mu::engraving::Part* part,
                            QVector<QJsonObject>& lazyAddPartOps);

    // Insert builders — coordinate-addressed, no UUIDs.
    QJsonObject buildInsertNote(mu::engraving::Note* note, const QString& partId);
    QJsonObject buildInsertRest(mu::engraving::Rest* rest, const QString& partId);

    // Modification / deletion builders — coordinate-addressed.
    static QJsonObject buildDeleteNote(mu::engraving::Note* note, const QString& partId);
    static QJsonObject buildDeleteRest(mu::engraving::Rest* rest, const QString& partId);
    static QJsonObject buildSetPitch(mu::engraving::Note* note, const QString& partId,
                                     const QJsonObject& oldPitch);
    static QJsonObject buildSetTabNote(mu::engraving::Note* note, const QString& partId);
    static QJsonObject buildSetNoteHead(mu::engraving::Note* note, const QString& partId);
    static QJsonObject buildSetVoice(mu::engraving::EngravingItem* item,
                                     const QString& partId, int oldVoice);
    static QJsonObject buildSetDuration(mu::engraving::ChordRest* cr,
                                        const QString& partId);

    // Directive builders.
    static QJsonObject buildSetTimeSignature(mu::engraving::TimeSig* ts);
    static QJsonObject buildSetTempo(mu::engraving::TempoText* tt);
    static QJsonObject buildSetKeySignature(mu::engraving::KeySig* ks, const QString& partUuid);
    static QJsonObject buildSetClef(mu::engraving::Clef* clef, const QString& partUuid,
                                    int staffIdx);
    static QJsonObject buildSetTie(mu::engraving::Note* note, const QString& partId,
                                   bool tieStart);

    // Part/staff directive builders.
    static QJsonObject buildAddPart(mu::engraving::Part* part,
                                    const QString& uuid);
    static QJsonObject buildRemovePart(const QString& uuid);
    static QJsonObject buildSetPartName(const QString& uuid, const QString& name);
    static QJsonObject buildSetStaffCount(const QString& uuid, int count);
    static QJsonObject buildSetPartInstrument(const QString& uuid,
                                               mu::engraving::Part* part);

    // Tier 3 builders — coordinate-addressed.
    static QJsonObject buildAddArticulation(mu::engraving::EngravingObject* art,
                                            const QString& partId,
                                            mu::engraving::Note* note);
    static QJsonObject buildRemoveArticulation(mu::engraving::EngravingObject* art,
                                               const QString& partId,
                                               mu::engraving::Note* note);

    // Tier 3 — dynamics (beat-addressed).
    static QJsonObject buildAddDynamic(mu::engraving::EngravingObject* dyn,
                                       const QString& partId);
    static QJsonObject buildSetDynamic(const QString& partId,
                                       const mu::engraving::Fraction& tick,
                                       const QString& kind);
    static QJsonObject buildRemoveDynamic(const QString& partId,
                                          const mu::engraving::Fraction& tick,
                                          const QString& kind);

    // Tier 3 — slurs (dual-coordinate).
    static QJsonObject buildAddSlur(mu::engraving::EngravingObject* slur,
                                    const QString& partId);
    static QJsonObject buildRemoveSlur(mu::engraving::EngravingObject* slur,
                                       const QString& partId);

    // Tier 3 — hairpins (beat-range).
    static QJsonObject buildAddHairpin(mu::engraving::EngravingObject* hp,
                                       const QString& partId,
                                       const mu::engraving::Fraction& startTick,
                                       const mu::engraving::Fraction& endTick,
                                       bool isCrescendo);
    static QJsonObject buildRemoveHairpin(const QString& partId,
                                          const mu::engraving::Fraction& startTick,
                                          const mu::engraving::Fraction& endTick,
                                          const QString& kind);

    // Tier 3 — tuplets.
    static QJsonObject buildAddTuplet(mu::engraving::EngravingObject* tup,
                                      const QString& partId,
                                      int actualNotes, int normalNotes);
    static QJsonObject buildRemoveTuplet(const QString& partId,
                                         const mu::engraving::Fraction& tick,
                                         int voice, int staff);

    // Tier 3 — chord symbols (score-global).
    static QJsonObject buildAddChordSymbol(int beatNum, int beatDen,
                                           const QString& name);
    static QJsonObject buildSetChordSymbol(int beatNum, int beatDen,
                                           const QString& name);
    static QJsonObject buildRemoveChordSymbol(int beatNum, int beatDen,
                                                const QString& name);

    // Tier 3 — lyrics.
    static QJsonObject buildAddLyric(mu::engraving::EngravingObject* lyr,
                                     const QString& partId,
                                     int voice, int staff,
                                     int verse,
                                     const QString& syllabic, const QString& text);
    static QJsonObject buildSetLyric(const QString& partId,
                                     const mu::engraving::Fraction& tick,
                                     int voice, int staff, int verse,
                                     const QString& text, const QString& syllabic);
    static QJsonObject buildRemoveLyric(const QString& partId,
                                        const mu::engraving::Fraction& tick,
                                        int voice, int staff, int verse);

    // Tier 3 — staff text (part-scoped).
    static QJsonObject buildAddStaffText(mu::engraving::EngravingObject* text,
                                         const QString& partId);
    static QJsonObject buildSetStaffText(const QString& partId,
                                         const mu::engraving::Fraction& tick,
                                         const QString& text);
    static QJsonObject buildRemoveStaffText(const QString& partId,
                                            const mu::engraving::Fraction& tick,
                                            const QString& text);

    // Tier 3 — system text (score-global).
    static QJsonObject buildAddSystemText(mu::engraving::EngravingObject* text);
    static QJsonObject buildSetSystemText(const mu::engraving::Fraction& tick,
                                          const QString& text);
    static QJsonObject buildRemoveSystemText(const mu::engraving::Fraction& tick,
                                             const QString& text);

    // Tier 3 — rehearsal marks (score-global).
    static QJsonObject buildAddRehearsalMark(mu::engraving::EngravingObject* mark);
    static QJsonObject buildSetRehearsalMark(const mu::engraving::Fraction& tick,
                                             const QString& text);
    static QJsonObject buildRemoveRehearsalMark(const mu::engraving::Fraction& tick,
                                                const QString& text);

    // Advanced spanners — octave lines.
    static QJsonObject buildAddOctaveLine(mu::engraving::EngravingObject* ottava,
                                           const QString& partId);
    static QJsonObject buildRemoveOctaveLine(const QString& partId,
                                             const mu::engraving::Fraction& startTick,
                                             const mu::engraving::Fraction& endTick,
                                             const QString& kind);

    // Advanced spanners — glissandos (dual-coordinate).
    static QJsonObject buildAddGlissando(mu::engraving::EngravingObject* gliss,
                                          const QString& partId);
    static QJsonObject buildRemoveGlissando(mu::engraving::EngravingObject* gliss,
                                            const QString& partId);

    // Guitar bends (dual-coordinate, note-anchored).
    static QJsonObject buildAddGuitarBend(mu::engraving::EngravingObject* bend,
                                           const QString& partId);
    static QJsonObject buildRemoveGuitarBend(mu::engraving::EngravingObject* bend,
                                              const QString& partId);

    // Advanced spanners — pedal lines.
    static QJsonObject buildAddPedalLine(mu::engraving::EngravingObject* pedal,
                                          const QString& partId);
    static QJsonObject buildRemovePedalLine(const QString& partId,
                                            const mu::engraving::Fraction& startTick,
                                            const mu::engraving::Fraction& endTick);

    // Advanced spanners — trill lines.
    static QJsonObject buildAddTrillLine(mu::engraving::EngravingObject* trill,
                                          const QString& partId);
    static QJsonObject buildRemoveTrillLine(const QString& partId,
                                            const mu::engraving::Fraction& startTick,
                                            const mu::engraving::Fraction& endTick);

    // Tier 3 — arpeggios.
    static QJsonObject buildAddArpeggio(mu::engraving::EngravingObject* arp,
                                         const QString& partId);
    static QJsonObject buildRemoveArpeggio(const QString& partId,
                                           const mu::engraving::Fraction& tick,
                                           int voice, int staff);

    // Grace notes.
    static QJsonObject buildAddGraceNote(mu::engraving::EngravingObject* graceObj,
                                          const QString& partId);
    static QJsonObject buildRemoveGraceNote(const QString& partId,
                                            const mu::engraving::Fraction& tick,
                                            int voice, int staff, int index);

    // Breath marks / caesuras.
    static QJsonObject buildAddBreathMark(mu::engraving::EngravingObject* breath,
                                           const QString& partId);
    static QJsonObject buildRemoveBreathMark(const QString& partId,
                                             const mu::engraving::Fraction& tick,
                                             const QString& breathType);

    // Tremolos (single-note).
    static QJsonObject buildAddTremolo(mu::engraving::EngravingObject* trem,
                                        const QString& partId);
    static QJsonObject buildRemoveTremolo(const QString& partId,
                                          const mu::engraving::Fraction& tick,
                                          int voice, int staff);

    // Two-note tremolos.
    static QJsonObject buildAddTwoNoteTremolo(mu::engraving::EngravingObject* trem,
                                               const QString& partId);
    static QJsonObject buildRemoveTwoNoteTremolo(mu::engraving::EngravingObject* trem,
                                                  const QString& partId);

    // Tier 4 — repeat barlines.
    static QJsonObject buildSetStartRepeat(const mu::engraving::Fraction& tick, bool enabled);
    static QJsonObject buildSetEndRepeat(const mu::engraving::Fraction& tick, bool enabled, int count);

    // Tier 4 — volta.
    static QJsonObject buildInsertVolta(mu::engraving::EngravingObject* volta);
    static QJsonObject buildRemoveVolta(const mu::engraving::Fraction& startTick,
                                        const mu::engraving::Fraction& endTick,
                                        const QJsonArray& numbers);
    static QJsonObject buildSetVoltaNumbers(mu::engraving::EngravingObject* volta,
                                            const QJsonArray& oldNumbers);

    // Tier 4 — markers.
    static QJsonObject buildInsertMarker(mu::engraving::EngravingObject* marker);
    static QJsonObject buildRemoveMarker(const mu::engraving::Fraction& tick,
                                         const QString& kind);

    // Tier 4 — jumps.
    static QJsonObject buildInsertJump(mu::engraving::EngravingObject* jump);
    static QJsonObject buildRemoveJump(const mu::engraving::Fraction& tick);

    // Shared helpers.
    static QJsonObject pitchJson(int tpc, int octave);
    static QJsonObject beatJson(const mu::engraving::Fraction& tick);
    static QString durationTypeName(mu::engraving::DurationType dt);

    // Known parts map — used to detect Add/Remove by diffing against score->parts().
    QHash<mu::engraving::Part*, QString> m_knownPartUuids;
};
} // namespace mu::editude::internal
