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

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include "editude/internal/scoreapplicator.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/segment.h"
#include "engraving/types/fraction.h"
#include "engraving/tests/utils/scorerw.h"

using namespace mu::engraving;
using namespace mu::editude::internal;

static const String DATA_DIR("data/");

class Editude_ScoreApplicatorTests : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Helper: build a fully-populated InsertNote JSON payload.
// Duration is the ADR object format: {"type": "quarter", "dots": 0}.
// Callers override only the fields they care about.
// ---------------------------------------------------------------------------
// [editude] InsertNote and friends are part-scoped: the applicator resolves
// part_id through its UUID map, so a part must be registered first. These
// Group 2 tests predate that requirement.
static const QString kTestPartId = "test-part-uuid";

static QJsonObject makeBeatObj(int n, int d)
{
    QJsonObject b;
    b["numerator"]   = n;
    b["denominator"] = d;
    return b;
}


static QJsonObject makePayload(
    const QString& step = "C", int octave = 4,
    const QString& acc = "",
    const QString& durType = "quarter", int dots = 0,
    int beatN = 0, int beatD = 4, int track = 0,
    const QString& noteId = "")
{
    QJsonObject pitch;
    pitch["step"]   = step;
    pitch["octave"] = octave;
    if (!acc.isEmpty()) {
        pitch["accidental"] = acc;
    }

    QJsonObject beat;
    beat["numerator"]   = beatN;
    beat["denominator"] = beatD;

    QJsonObject duration;
    duration["type"] = durType;
    duration["dots"] = dots;

    QJsonObject op;
    op["type"]     = "InsertNote";
    op["part_id"]  = kTestPartId;  // [editude] ops are part-scoped
    op["pitch"]    = pitch;
    op["duration"] = duration;
    op["beat"]     = beat;
    op["track"]    = track;
    if (!noteId.isEmpty()) {
        op["id"] = noteId;
    }
    return op;
}

// ===========================================================================
// Group 1 — early-exit guards (no score object needed)
// ===========================================================================

// [editude] The applicator's element->UUID map was removed when addressing
// became coordinate-based (df1edaabf6). Assert the observable outcome — whether
// the score actually contains a tuplet — rather than internal bookkeeping.
static bool scoreHasTuplet(Score* score)
{
    for (Segment* seg = score->firstSegment(SegmentType::ChordRest); seg;
         seg = seg->next1(SegmentType::ChordRest)) {
        for (track_idx_t t = 0; t < score->ntracks(); ++t) {
            EngravingItem* e = seg->element(t);
            if (e && e->isChordRest() && toChordRest(e)->tuplet()) {
                return true;
            }
        }
    }
    return false;
}

TEST_F(Editude_ScoreApplicatorTests, unrecognizedType_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject payload = makePayload();
    payload["type"] = "UnknownOp";
    EXPECT_FALSE(applicator.apply(nullptr, payload));
}

TEST_F(Editude_ScoreApplicatorTests, invalidDuration_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject payload = makePayload("C", 4, "", "fortieth");
    EXPECT_FALSE(applicator.apply(nullptr, payload));
}

TEST_F(Editude_ScoreApplicatorTests, invalidStep_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject payload = makePayload("X", 4);
    EXPECT_FALSE(applicator.apply(nullptr, payload));
}

TEST_F(Editude_ScoreApplicatorTests, midiOutOfRange_returnsFalse)
{
    ScoreApplicator applicator;
    // G10: (10+1)*12 + 7 = 139 > 127
    QJsonObject payload = makePayload("G", 10);
    EXPECT_FALSE(applicator.apply(nullptr, payload));
}


static QJsonObject makeAddPartPayload(const QString& partId,
                                       const QString& name = "Violin",
                                       int staffCount = 1)
{
    QJsonObject instr;
    instr["musescore_id"] = "";
    instr["name"]         = name;
    instr["short_name"]   = name.left(3) + ".";

    QJsonObject op;
    op["type"]        = "AddPart";
    op["part_id"]     = partId;
    op["name"]        = name;
    op["staff_count"] = staffCount;
    op["instrument"]  = instr;
    return op;
}

static bool registerTestPart(ScoreApplicator& applicator, Score* score)
{
    return applicator.apply(score, makeAddPartPayload(kTestPartId));
}

// ===========================================================================
// Group 2 — requires a real score loaded from the fixture
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertNote_C4_quarter_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject payload = makePayload("C", 4, "", "quarter", 0, 0, 4, 0);
    EXPECT_TRUE(applicator.apply(score, payload));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes()[0];
    ASSERT_TRUE(note);

    EXPECT_EQ(note->pitch(), 60);
    EXPECT_EQ(chord->durationType().type(), DurationType::V_QUARTER);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyInsertNote_BbFlat4_half_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    // pitchToMidi("B", 4, "flat") = (4+1)*12 + 11 - 1 = 70
    QJsonObject payload = makePayload("B", 4, "flat", "half", 0, 0, 4, 0);
    EXPECT_TRUE(applicator.apply(score, payload));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes()[0];
    ASSERT_TRUE(note);

    EXPECT_EQ(note->pitch(), 70);
    EXPECT_EQ(chord->durationType().type(), DurationType::V_HALF);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, noSegmentAtTick_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    // Tick 99/4 is well beyond the single 4/4 measure
    QJsonObject payload = makePayload("C", 4, "", "quarter", 0, 99, 4, 0);
    EXPECT_FALSE(applicator.apply(score, payload));

    delete score;
}

// ===========================================================================
// Group 3 — UUID tracking: DeleteEvent removes a previously inserted note
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyDeleteEvent_removesInsertedNote)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0)));

    QJsonObject pitch;
    pitch["step"]   = "C";
    pitch["octave"] = 4;

    QJsonObject deleteOp;
    deleteOp["type"]    = "DeleteNote";
    deleteOp["part_id"] = kTestPartId;
    deleteOp["beat"]    = makeBeatObj(0, 4);
    deleteOp["pitch"]   = pitch;
    EXPECT_TRUE(applicator.apply(score, deleteOp));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyDeleteEvent_unknownUuid_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject payload;
    payload["type"]     = "DeleteNote";
    payload["event_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    EXPECT_FALSE(applicator.apply(score, payload));

    delete score;
}

// ===========================================================================
// Group 4 — early-exit guards for new handlers (no score needed)
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, insertRest_invalidDuration_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject duration;
    duration["type"] = "fortieth";
    duration["dots"] = 0;
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 4;
    QJsonObject op;
    op["type"]     = "InsertRest";
    op["duration"] = duration;
    op["beat"]     = beat;
    op["track"]    = 0;
    EXPECT_FALSE(applicator.apply(nullptr, op));
}

TEST_F(Editude_ScoreApplicatorTests, addChordNote_unknownUuid_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject pitch;
    pitch["step"]   = "G";
    pitch["octave"] = 4;
    QJsonObject op;
    op["type"]     = "AddChordNote";
    op["event_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["pitch"]    = pitch;
    EXPECT_FALSE(applicator.apply(nullptr, op));
}

TEST_F(Editude_ScoreApplicatorTests, removeChordNote_unknownUuid_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject pitch;
    pitch["step"]   = "E";
    pitch["octave"] = 4;
    QJsonObject op;
    op["type"]     = "RemoveChordNote";
    op["event_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["pitch"]    = pitch;
    EXPECT_FALSE(applicator.apply(nullptr, op));
}

TEST_F(Editude_ScoreApplicatorTests, setDuration_unknownUuid_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject duration;
    duration["type"] = "half";
    duration["dots"] = 0;
    QJsonObject op;
    op["type"]     = "SetDuration";
    op["event_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["duration"] = duration;
    EXPECT_FALSE(applicator.apply(nullptr, op));
}

TEST_F(Editude_ScoreApplicatorTests, setTrack_unknownUuid_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject op;
    op["type"]     = "SetVoice";
    op["event_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["track"]    = 1;
    EXPECT_FALSE(applicator.apply(nullptr, op));
}

// ===========================================================================
// Group 5 — integration tests for new handlers (require empty_measure.mscx)
// ===========================================================================

// Helper: build an InsertRest payload.
static QJsonObject makeRestPayload(
    const QString& durType = "quarter", int dots = 0,
    int beatN = 0, int beatD = 4, int track = 0,
    const QString& id = "")
{
    QJsonObject beat;
    beat["numerator"]   = beatN;
    beat["denominator"] = beatD;
    QJsonObject duration;
    duration["type"] = durType;
    duration["dots"] = dots;
    QJsonObject op;
    op["type"]     = "InsertRest";
    op["part_id"]  = kTestPartId;  // [editude] ops are part-scoped
    op["duration"] = duration;
    op["beat"]     = beat;
    op["track"]    = track;
    if (!id.isEmpty()) {
        op["id"] = id;
    }
    return op;
}

TEST_F(Editude_ScoreApplicatorTests, applyInsertRest_quarter_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    EXPECT_TRUE(applicator.apply(score, makeRestPayload()));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    EngravingItem* el = seg->element(0);
    ASSERT_TRUE(el);
    EXPECT_EQ(el->type(), ElementType::REST);

    delete score;
}

// [editude] InsertChord / AddChordNote / RemoveChordNote tests removed: none of
// these are editude ops.
// Chords are formed by inserting several notes at the same (beat, voice,
// staff) — see e2e/e2e/test_tier1_ops_extended.py::test_insert_chord_round_trip.

TEST_F(Editude_ScoreApplicatorTests, applySetDuration_quarterToHalf)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0)));

    QJsonObject duration;
    duration["type"] = "half";
    duration["dots"] = 0;

    QJsonObject setDurOp;
    setDurOp["type"]     = "SetDuration";
    setDurOp["part_id"]  = kTestPartId;
    setDurOp["beat"]     = makeBeatObj(0, 4);
    setDurOp["duration"] = duration;
    EXPECT_TRUE(applicator.apply(score, setDurOp));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = nullptr;
    for (track_idx_t t = 0; t < score->ntracks(); ++t) {
        if (seg->cr(t) && seg->cr(t)->isChord()) {
            chord = toChord(seg->cr(t));
            break;
        }
    }
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->durationType().type(), DurationType::V_HALF);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetTrack_movesToVoice1)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0)));

    QJsonObject setVoiceOp;
    setVoiceOp["type"]      = "SetVoice";
    setVoiceOp["part_id"]   = kTestPartId;
    setVoiceOp["beat"]      = makeBeatObj(0, 4);
    setVoiceOp["new_voice"] = 2;
    EXPECT_TRUE(applicator.apply(score, setVoiceOp));

    delete score;
}

// ---------------------------------------------------------------------------
// Group 6 — Part / Staff ops (Phase 1)
// ---------------------------------------------------------------------------


TEST_F(Editude_ScoreApplicatorTests, applyAddPart_appendsPart)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const int partsBefore = static_cast<int>(score->parts().size());
    ScoreApplicator applicator;

    // The first AddPart adopts the score's existing unassigned part rather than
    // appending a duplicate, so the part count is unchanged.
    EXPECT_TRUE(applicator.apply(score, makeAddPartPayload(kTestPartId)));
    EXPECT_EQ(static_cast<int>(score->parts().size()), partsBefore);

    // A second, distinct part_id has nothing left to adopt, so it appends.
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    EXPECT_TRUE(applicator.apply(score, makeAddPartPayload(uuid)));
    EXPECT_EQ(static_cast<int>(score->parts().size()), partsBefore + 1);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddPart_missingPartId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    QJsonObject op;
    op["type"]        = "AddPart";
    op["part_id"]     = "";
    op["name"]        = "Cello";
    op["staff_count"] = 1;
    ScoreApplicator applicator;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemovePart_decreasesPartCount)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(uuid)));
    const int partsAfterAdd = static_cast<int>(score->parts().size());

    QJsonObject removeOp;
    removeOp["type"]    = "RemovePart";
    removeOp["part_id"] = uuid;
    EXPECT_TRUE(applicator.apply(score, removeOp));
    EXPECT_EQ(static_cast<int>(score->parts().size()), partsAfterAdd - 1);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemovePart_unknownUuid_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    QJsonObject op;
    op["type"]    = "RemovePart";
    op["part_id"] = "no-such-part";
    ScoreApplicator applicator;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetPartName_renamesPart)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(uuid, "Violin")));

    QJsonObject renameOp;
    renameOp["type"]    = "SetPartName";
    renameOp["part_id"] = uuid;
    renameOp["name"]    = "Violin I";
    EXPECT_TRUE(applicator.apply(score, renameOp));

    // The last part added is the renamed one.
    Part* part = score->parts().back();
    EXPECT_EQ(part->partName(), u"Violin I");

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetPartName_unknownUuid_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    QJsonObject op;
    op["type"]    = "SetPartName";
    op["part_id"] = "ghost-part";
    op["name"]    = "Oboe";
    ScoreApplicator applicator;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

// ── Group 7: SetKeySignature & SetClef ──────────────────────────────────────

TEST_F(Editude_ScoreApplicatorTests, applySetKeySignature_addsKeySig)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(uuid, "Piano")));

    QJsonObject op;
    op["type"]    = "SetKeySignature";
    op["part_id"] = uuid;
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    op["beat"] = beat;
    QJsonObject keySig;
    keySig["sharps"] = 3;
    op["key_signature"] = keySig;

    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetKeySignature_unknownPart_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    QJsonObject op;
    op["type"]    = "SetKeySignature";
    op["part_id"] = "ghost-part";
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    op["beat"] = beat;
    QJsonObject keySig;
    keySig["sharps"] = 0;
    op["key_signature"] = keySig;

    ScoreApplicator applicator;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetClef_setsClefOnPart)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(uuid, "Cello")));

    QJsonObject op;
    op["type"]    = "SetClef";
    op["part_id"] = uuid;
    op["staff"]   = 0;
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    op["beat"] = beat;
    QJsonObject clef;
    clef["name"] = "bass";
    op["clef"] = clef;

    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetClef_unknownClefName_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(uuid, "Piano")));

    QJsonObject op;
    op["type"]    = "SetClef";
    op["part_id"] = uuid;
    op["staff"]   = 0;
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    op["beat"] = beat;
    QJsonObject clef;
    clef["name"] = "not-a-clef";
    op["clef"] = clef;

    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

// ===========================================================================
// Group 8 — Tier 3: Articulations
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddArticulation_staccato_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString artUuid  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    QJsonObject op;
    op["type"]         = "AddArticulation";
    op["part_id"] = kTestPartId;
    op["beat"] = makeBeatObj(0, 4);
    op["articulation"] = "staccato";
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddArticulation_unknownEventId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"]         = "AddArticulation";
    op["id"]           = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["event_id"]     = "no-such-event";
    op["articulation"] = "staccato";
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveArticulation_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0)));

    QJsonObject addOp;
    addOp["type"]         = "AddArticulation";
    addOp["part_id"]      = kTestPartId;
    addOp["beat"]         = makeBeatObj(0, 4);
    addOp["articulation"] = "accent";
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]         = "RemoveArticulation";
    removeOp["part_id"]      = kTestPartId;
    removeOp["beat"]         = makeBeatObj(0, 4);
    removeOp["articulation"] = "accent";
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 9 — Tier 3: Dynamics
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddDynamic_mf_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString partUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString dynUuid  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(partUuid)));

    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    QJsonObject op;
    op["type"]    = "AddDynamic";
    op["id"]      = dynUuid;
    op["part_id"] = partUuid;
    op["kind"]    = "mf";
    op["beat"]    = beat;
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddDynamic_unknownPartId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 1;
    QJsonObject op;
    op["type"]    = "AddDynamic";
    op["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["part_id"] = "no-such-part";
    op["kind"]    = "f";
    op["beat"]    = beat;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetDynamic_changesKind)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject addOp;
    addOp["type"]    = "AddDynamic";
    addOp["part_id"] = kTestPartId;
    addOp["kind"]    = "p";
    addOp["beat"]    = makeBeatObj(0, 1);
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject setOp;
    setOp["type"]    = "SetDynamic";
    setOp["part_id"] = kTestPartId;
    setOp["beat"]    = makeBeatObj(0, 1);
    setOp["kind"]    = "ff";
    EXPECT_TRUE(applicator.apply(score, setOp));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveDynamic_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject addOp;
    addOp["type"]    = "AddDynamic";
    addOp["part_id"] = kTestPartId;
    addOp["kind"]    = "pp";
    addOp["beat"]    = makeBeatObj(0, 1);
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]    = "RemoveDynamic";
    removeOp["part_id"] = kTestPartId;
    removeOp["beat"]    = makeBeatObj(0, 1);
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 10 — Tier 3: Slurs
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddSlur_twoNotes_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString uuid1    = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString uuid2    = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString slurUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    // Two quarter notes at beats 0/4 and 1/4.
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, uuid1)));
    ASSERT_TRUE(applicator.apply(score, makePayload("G", 4, "", "quarter", 0, 1, 4, 0, uuid2)));

    QJsonObject op;
    op["type"]           = "AddSlur";
    op["part_id"] = kTestPartId;
    op["start_beat"]  = makeBeatObj(0, 4);
    op["end_beat"]    = makeBeatObj(1, 4);
    op["start_voice"] = 1;
    op["end_voice"]   = 1;
    op["start_staff"] = 0;
    op["end_staff"]   = 0;
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddSlur_unknownEventId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"]           = "AddSlur";
    op["id"]             = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["start_event_id"] = "no-such-start";
    op["end_event_id"]   = "no-such-end";
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveSlur_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0)));
    ASSERT_TRUE(applicator.apply(score, makePayload("G", 4, "", "quarter", 0, 1, 4, 0)));

    QJsonObject addOp;
    addOp["type"]        = "AddSlur";
    addOp["part_id"]     = kTestPartId;
    addOp["start_beat"]  = makeBeatObj(0, 4);
    addOp["end_beat"]    = makeBeatObj(1, 4);
    addOp["start_voice"] = 1;
    addOp["end_voice"]   = 1;
    addOp["start_staff"] = 0;
    addOp["end_staff"]   = 0;
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]        = "RemoveSlur";
    removeOp["part_id"]     = kTestPartId;
    removeOp["start_beat"]  = makeBeatObj(0, 4);
    removeOp["end_beat"]    = makeBeatObj(1, 4);
    removeOp["start_voice"] = 1;
    removeOp["start_staff"] = 0;
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 11 — Tier 3: Hairpins
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddHairpin_crescendo_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString partUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString hpUuid   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(partUuid)));

    QJsonObject op;
    op["type"]       = "AddHairpin";
    op["id"]         = hpUuid;
    op["part_id"]    = partUuid;
    op["kind"]       = "crescendo";
    op["start_beat"] = makeBeatObj(0, 4);
    op["end_beat"]   = makeBeatObj(2, 4);
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddHairpin_unknownPartId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"]       = "AddHairpin";
    op["id"]         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    op["part_id"]    = "no-such-part";
    op["kind"]       = "diminuendo";
    op["start_beat"] = makeBeatObj(0, 4);
    op["end_beat"]   = makeBeatObj(2, 4);
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveHairpin_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject addOp;
    addOp["type"]       = "AddHairpin";
    addOp["part_id"]    = kTestPartId;
    addOp["kind"]       = "crescendo";
    addOp["start_beat"] = makeBeatObj(0, 4);
    addOp["end_beat"]   = makeBeatObj(2, 4);
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]       = "RemoveHairpin";
    removeOp["part_id"]    = kTestPartId;
    removeOp["start_beat"] = makeBeatObj(0, 4);
    removeOp["end_beat"]   = makeBeatObj(2, 4);
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 12 — Tier 3: Chord symbols
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddChordSymbol_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString csUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject op;
    op["type"] = "AddChordSymbol";
    op["id"]   = csUuid;
    op["name"] = "Cmaj7";
    op["beat"] = makeBeatObj(0, 1);
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetChordSymbol_changesName)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString csUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject addOp;
    addOp["type"] = "AddChordSymbol";
    addOp["part_id"] = kTestPartId;
    addOp["name"] = "Am";
    addOp["beat"] = makeBeatObj(0, 1);
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject setOp;
    setOp["type"] = "SetChordSymbol";
    setOp["part_id"] = kTestPartId;
    setOp["beat"] = makeBeatObj(0, 1);
    setOp["name"] = "Am7";
    EXPECT_TRUE(applicator.apply(score, setOp));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveChordSymbol_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString csUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject addOp;
    addOp["type"] = "AddChordSymbol";
    addOp["part_id"] = kTestPartId;
    addOp["name"] = "G7";
    addOp["beat"] = makeBeatObj(0, 1);
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"] = "RemoveChordSymbol";
    removeOp["part_id"] = kTestPartId;
    removeOp["beat"] = makeBeatObj(0, 1);
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 13 — Tier 3: Lyrics
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyAddLyric_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString lyrUuid  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    QJsonObject op;
    op["type"]     = "AddLyric";
    op["part_id"] = kTestPartId;
    op["beat"] = makeBeatObj(0, 4);
    op["verse"]    = 0;
    op["syllabic"] = "single";
    op["text"]     = "la";
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetLyric_changesText)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString lyrUuid  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    QJsonObject addOp;
    addOp["type"]     = "AddLyric";
    addOp["part_id"] = kTestPartId;
    addOp["beat"] = makeBeatObj(0, 4);
    addOp["verse"]    = 0;
    addOp["syllabic"] = "begin";
    addOp["text"]     = "hel-";
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject setOp;
    setOp["type"]     = "SetLyric";
    setOp["beat"] = makeBeatObj(0, 1);
    setOp["part_id"] = kTestPartId;
    setOp["text"]     = "lo-";
    setOp["syllabic"] = "begin";
    EXPECT_TRUE(applicator.apply(score, setOp));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveLyric_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString lyrUuid  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    QJsonObject addOp;
    addOp["type"]     = "AddLyric";
    addOp["part_id"] = kTestPartId;
    addOp["beat"] = makeBeatObj(0, 4);
    addOp["verse"]    = 0;
    addOp["syllabic"] = "single";
    addOp["text"]     = "la";
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"] = "RemoveLyric";
    removeOp["beat"] = makeBeatObj(0, 1);
    removeOp["part_id"] = kTestPartId;
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 14 — Tier 4: Voltas
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertVolta_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString voltaUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonArray numbers;
    numbers.append(1);

    QJsonObject op;
    op["type"]       = "InsertVolta";
    op["id"]         = voltaUuid;
    op["start_beat"] = makeBeatObj(0, 1);
    op["end_beat"]   = makeBeatObj(0, 1);
    op["numbers"]    = numbers;
    op["open_end"]   = false;
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveVolta_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString voltaUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonArray numbers;
    numbers.append(1);

    QJsonObject addOp;
    addOp["type"]       = "InsertVolta";
    addOp["id"]         = voltaUuid;
    addOp["start_beat"] = makeBeatObj(0, 1);
    addOp["end_beat"]   = makeBeatObj(0, 1);
    addOp["numbers"]    = numbers;
    addOp["open_end"]   = false;
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]       = "RemoveVolta";
    removeOp["start_beat"] = makeBeatObj(0, 1);
    removeOp["end_beat"]   = makeBeatObj(0, 1);
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 15 — Tier 4: Markers
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertMarker_segno_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString markerUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject op;
    op["type"] = "InsertMarker";
    op["id"]   = markerUuid;
    op["beat"] = makeBeatObj(0, 1);
    op["kind"] = "segno";
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveMarker_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject addOp;
    addOp["type"]    = "InsertMarker";
    addOp["part_id"] = kTestPartId;
    addOp["beat"]    = makeBeatObj(0, 1);
    addOp["kind"]    = "coda";
    addOp["label"]   = "Coda";
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"]    = "RemoveMarker";
    removeOp["part_id"] = kTestPartId;
    removeOp["beat"]    = makeBeatObj(0, 1);
    removeOp["kind"]    = "coda";
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 16 — Tier 4: Jumps
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertJump_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString jumpUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject op;
    op["type"]    = "InsertJump";
    op["id"]      = jumpUuid;
    op["beat"]    = makeBeatObj(0, 1);
    op["jump_to"] = "start";
    op["text"]    = "D.C.";
    EXPECT_TRUE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveJump_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString jumpUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    QJsonObject addOp;
    addOp["type"]    = "InsertJump";
    addOp["id"]      = jumpUuid;
    addOp["beat"]    = makeBeatObj(0, 1);
    addOp["jump_to"] = "start";
    ASSERT_TRUE(applicator.apply(score, addOp));

    QJsonObject removeOp;
    removeOp["type"] = "RemoveJump";
    removeOp["id"]   = jumpUuid;
    EXPECT_TRUE(applicator.apply(score, removeOp));

    delete score;
}

// ===========================================================================
// Group 17 — Structural: SetScoreMetadata
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applySetScoreMetadata_title_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"]  = "SetScoreMetadata";
    op["field"] = "title";
    op["value"] = "My Symphony";
    EXPECT_TRUE(applicator.apply(score, op));
    EXPECT_EQ(score->metaTag(u"workTitle"), u"My Symphony");

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetScoreMetadata_composer_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"]  = "SetScoreMetadata";
    op["field"] = "composer";
    op["value"] = "J.S. Bach";
    EXPECT_TRUE(applicator.apply(score, op));
    EXPECT_EQ(score->metaTag(u"composer"), u"J.S. Bach");

    delete score;
}

// ===========================================================================
// Group 18 — Structural: InsertBeats / DeleteBeats
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertBeats_addsMeasures)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const size_t measBefore = score->nmeasures();
    ScoreApplicator applicator;

    QJsonObject op;
    op["type"]     = "InsertBeats";
    op["at_beat"]  = makeBeatObj(0, 1);
    op["duration"] = makeBeatObj(1, 1);
    EXPECT_TRUE(applicator.apply(score, op));
    EXPECT_GT(score->nmeasures(), measBefore);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyDeleteBeats_removesMeasure)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    // Use InsertBeats to add a second measure, giving us two to work with.
    QJsonObject insertOp;
    insertOp["type"]     = "InsertBeats";
    insertOp["at_beat"]  = makeBeatObj(0, 1);
    insertOp["duration"] = makeBeatObj(1, 1);
    ASSERT_TRUE(applicator.apply(score, insertOp));
    ASSERT_EQ(score->nmeasures(), static_cast<size_t>(2));

    // Delete the first measure.
    QJsonObject op;
    op["type"]     = "DeleteBeats";
    op["at_beat"]  = makeBeatObj(0, 1);
    op["duration"] = makeBeatObj(1, 1);
    EXPECT_TRUE(applicator.apply(score, op));
    EXPECT_EQ(score->nmeasures(), static_cast<size_t>(1));

    delete score;
}

// ===========================================================================
// Group 19 — Tier 3: Tuplets
// ===========================================================================

// Helper: build an AddTuplet payload for a triplet (3:2 quarter notes).
static QJsonObject makeAddTupletPayload(const QString& partId,
                                         int beatN = 0, int beatD = 1)
{
    QJsonObject baseDur;
    baseDur["type"] = "quarter";
    baseDur["dots"] = 0;

    QJsonObject op;
    op["type"]          = "AddTuplet";
    op["part_id"]       = partId;
    op["actual_notes"]  = 3;
    op["normal_notes"]  = 2;
    op["base_duration"] = baseDur;
    op["beat"]          = makeBeatObj(beatN, beatD);
    return op;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddTuplet_triplet_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    EXPECT_TRUE(applicator.apply(score, makeAddTupletPayload(kTestPartId)));
    EXPECT_TRUE(scoreHasTuplet(score));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddTuplet_unknownPartId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject op = makeAddTupletPayload("no-such-part");
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveTuplet_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    ASSERT_TRUE(applicator.apply(score, makeAddTupletPayload(kTestPartId)));
    ASSERT_TRUE(scoreHasTuplet(score));

    QJsonObject removeOp;
    removeOp["type"]    = "RemoveTuplet";
    removeOp["part_id"] = kTestPartId;
    removeOp["beat"]    = makeBeatObj(0, 1);
    EXPECT_TRUE(applicator.apply(score, removeOp));

    EXPECT_FALSE(scoreHasTuplet(score));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveTuplet_unknownId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));
    QJsonObject op;
    op["type"] = "RemoveTuplet";
    op["id"]   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}

// ===========================================================================
// Group 20 — SetPartInstrument
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applySetPartInstrument_updatesNames)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString partUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;
    ASSERT_TRUE(applicator.apply(score, makeAddPartPayload(partUuid, "Violin")));

    QJsonObject instr;
    instr["musescore_id"] = "flute";
    instr["name"]         = "Flute";
    instr["short_name"]   = "Fl.";

    QJsonObject op;
    op["type"]       = "SetPartInstrument";
    op["part_id"]    = partUuid;
    op["instrument"] = instr;
    EXPECT_TRUE(applicator.apply(score, op));

    // The last part added was our test part — verify name updated.
    Part* part = score->parts().back();
    EXPECT_EQ(part->partName(), u"Flute");

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetPartInstrument_unknownPartId_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    ASSERT_TRUE(registerTestPart(applicator, score));

    QJsonObject instr;
    instr["musescore_id"] = "oboe";
    instr["name"]         = "Oboe";
    instr["short_name"]   = "Ob.";

    QJsonObject op;
    op["type"]       = "SetPartInstrument";
    op["part_id"]    = "no-such-part";
    op["instrument"] = instr;
    EXPECT_FALSE(applicator.apply(score, op));

    delete score;
}
