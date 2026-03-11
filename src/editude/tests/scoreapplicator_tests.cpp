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

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

#include "editude/internal/scoreapplicator.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
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

// ===========================================================================
// Group 2 — requires a real score loaded from the fixture
// ===========================================================================

TEST_F(Editude_ScoreApplicatorTests, applyInsertNote_C4_quarter_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
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

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    ScoreApplicator applicator;

    // Insert with explicit UUID so we can reference it in DeleteEvent.
    QJsonObject insertPayload = makePayload("C", 4, "", "quarter", 0, 0, 4, 0, uuid);
    ASSERT_TRUE(applicator.apply(score, insertPayload));

    // The UUID map should now contain the note.
    EXPECT_FALSE(applicator.elementToUuid().isEmpty());

    // Delete the note via its UUID.
    QJsonObject deletePayload;
    deletePayload["type"]     = "DeleteEvent";
    deletePayload["event_id"] = uuid;
    EXPECT_TRUE(applicator.apply(score, deletePayload));

    // After deletion the UUID map entry must be removed.
    EXPECT_TRUE(applicator.elementToUuid().isEmpty());

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyDeleteEvent_unknownUuid_returnsFalse)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject payload;
    payload["type"]     = "DeleteEvent";
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

TEST_F(Editude_ScoreApplicatorTests, insertChord_emptyPitches_returnsFalse)
{
    ScoreApplicator applicator;
    QJsonObject duration;
    duration["type"] = "quarter";
    duration["dots"] = 0;
    QJsonObject beat;
    beat["numerator"]   = 0;
    beat["denominator"] = 4;
    QJsonObject op;
    op["type"]     = "InsertChord";
    op["pitches"]  = QJsonArray(); // empty
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
    op["type"]     = "SetTrack";
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
    op["duration"] = duration;
    op["beat"]     = beat;
    op["track"]    = track;
    if (!id.isEmpty()) {
        op["id"] = id;
    }
    return op;
}

// Helper: build an InsertChord payload with two pitches (C4, E4).
static QJsonObject makeChordPayload(
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

    QJsonObject p1;
    p1["step"] = "C"; p1["octave"] = 4;
    QJsonObject p2;
    p2["step"] = "E"; p2["octave"] = 4;
    QJsonArray pitches;
    pitches.append(p1);
    pitches.append(p2);

    QJsonObject op;
    op["type"]     = "InsertChord";
    op["pitches"]  = pitches;
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
    EXPECT_TRUE(applicator.apply(score, makeRestPayload()));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    EngravingItem* el = seg->element(0);
    ASSERT_TRUE(el);
    EXPECT_EQ(el->type(), ElementType::REST);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyInsertChord_twoPitches_succeeds)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    EXPECT_TRUE(applicator.apply(score, makeChordPayload()));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->notes().size(), static_cast<size_t>(2));

    // Verify C4 (midi 60) and E4 (midi 64) are both present.
    bool hasC4 = false, hasE4 = false;
    for (Note* n : chord->notes()) {
        if (n->pitch() == 60) hasC4 = true;
        if (n->pitch() == 64) hasE4 = true;
    }
    EXPECT_TRUE(hasC4);
    EXPECT_TRUE(hasE4);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyAddChordNote_addsToExistingChord)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString chordUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    // Insert 2-pitch chord (C4, E4) with UUID.
    ASSERT_TRUE(applicator.apply(score, makeChordPayload("quarter", 0, 0, 4, 0, chordUuid)));

    // Add G4 (midi 67).
    QJsonObject pitch;
    pitch["step"]   = "G";
    pitch["octave"] = 4;
    QJsonObject addOp;
    addOp["type"]     = "AddChordNote";
    addOp["event_id"] = chordUuid;
    addOp["pitch"]    = pitch;
    EXPECT_TRUE(applicator.apply(score, addOp));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->notes().size(), static_cast<size_t>(3));

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applyRemoveChordNote_reducesChord)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString chordUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    // Insert 2-pitch chord (C4=60, E4=64).
    ASSERT_TRUE(applicator.apply(score, makeChordPayload("quarter", 0, 0, 4, 0, chordUuid)));

    // Remove E4.
    QJsonObject pitch;
    pitch["step"]   = "E";
    pitch["octave"] = 4;
    QJsonObject removeOp;
    removeOp["type"]     = "RemoveChordNote";
    removeOp["event_id"] = chordUuid;
    removeOp["pitch"]    = pitch;
    EXPECT_TRUE(applicator.apply(score, removeOp));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->notes().size(), static_cast<size_t>(1));
    EXPECT_EQ(chord->notes().front()->pitch(), 60);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetDuration_quarterToHalf)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    // Insert C4 quarter.
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    // Change duration to half.
    QJsonObject duration;
    duration["type"] = "half";
    duration["dots"] = 0;
    QJsonObject setDurOp;
    setDurOp["type"]     = "SetDuration";
    setDurOp["event_id"] = noteUuid;
    setDurOp["duration"] = duration;
    EXPECT_TRUE(applicator.apply(score, setDurOp));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    EXPECT_EQ(chord->durationType().type(), DurationType::V_HALF);

    delete score;
}

TEST_F(Editude_ScoreApplicatorTests, applySetTrack_movesToVoice1)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    const QString noteUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ScoreApplicator applicator;

    // Insert C4 on track 0.
    ASSERT_TRUE(applicator.apply(score, makePayload("C", 4, "", "quarter", 0, 0, 4, 0, noteUuid)));

    // Move to track 1 (voice 2 of staff 0).
    QJsonObject setTrackOp;
    setTrackOp["type"]     = "SetTrack";
    setTrackOp["event_id"] = noteUuid;
    setTrackOp["track"]    = 1;
    EXPECT_TRUE(applicator.apply(score, setTrackOp));

    // The UUID map should still contain an entry for the note.
    EXPECT_FALSE(applicator.elementToUuid().isEmpty());

    delete score;
}
