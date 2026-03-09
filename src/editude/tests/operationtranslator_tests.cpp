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

#include "editude/internal/operationtranslator.h"
#include "editude/internal/scoreapplicator.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/timesig.h"
#include "engraving/editing/undo.h"
#include "engraving/types/fraction.h"
#include "engraving/tests/utils/scorerw.h"

using namespace mu::engraving;
using namespace mu::editude::internal;

static const String DATA_DIR("data/");
static const QString PART_ID("test-part-id");

class Editude_OperationTranslatorTests : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Helper: apply a remote InsertNote via ScoreApplicator and return the UUID.
// ---------------------------------------------------------------------------
static QString applyRemoteInsertNote(ScoreApplicator& applicator, Score* score,
                                     const QString& step = "C", int octave = 4,
                                     const QString& durType = "quarter",
                                     int beatN = 0, int beatD = 4, int track = 0)
{
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QJsonObject pitch;
    pitch["step"]   = step;
    pitch["octave"] = octave;

    QJsonObject beat;
    beat["numerator"]   = beatN;
    beat["denominator"] = beatD;

    QJsonObject duration;
    duration["type"] = durType;
    duration["dots"] = 0;

    QJsonObject op;
    op["type"]     = "InsertNote";
    op["pitch"]    = pitch;
    op["beat"]     = beat;
    op["duration"] = duration;
    op["track"]    = track;
    op["id"]       = uuid;

    const bool ok = applicator.apply(score, op);
    return ok ? uuid : QString();
}

// ---------------------------------------------------------------------------
// Helper: build a minimal changedObjects map for a single element.
// ---------------------------------------------------------------------------
using ChangedMap = std::map<EngravingObject*, std::unordered_set<CommandType>>;

static ChangedMap singleChange(EngravingObject* obj, CommandType cmd)
{
    return { { obj, { cmd } } };
}

// ===========================================================================
// Group 1 — empty / no-op cases
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, emptyChanges_returnsEmpty)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;
    const auto ops = translator.translateAll({}, {}, PART_ID, emptyMap);
    EXPECT_TRUE(ops.isEmpty());
}

TEST_F(Editude_OperationTranslatorTests, unknownCommandType_returnsEmpty)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;
    // Use a dummy non-null EngravingObject* that doesn't match any handled type.
    // Pass a null pointer — should be skipped gracefully.
    ChangedMap changed = { { nullptr, { CommandType::ChangeStyle } } };
    const auto ops = translator.translateAll(changed, {}, PART_ID, emptyMap);
    EXPECT_TRUE(ops.isEmpty());
}

// ===========================================================================
// Group 2 — InsertNote: locally-inserted single-note chord
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertNote_fromLocalEdit_emitsInsertNote)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    // Apply a remote InsertNote so the segment is populated.
    // Then simulate a second (local) chord by directly reading what's in the score.
    // For this test, we build the changedObjects map manually from score elements.

    // Apply via applicator to get a note pointer.
    ScoreApplicator applicator;
    const QString remoteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(remoteUuid.isEmpty());

    // Get the inserted note.
    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes().front();
    ASSERT_TRUE(note);

    // Simulate translateAll seeing AddElement on the Note AND its parent Chord.
    ChangedMap changed = {
        { chord, { CommandType::AddElement } },
        { note,  { CommandType::AddElement } },
    };

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertNote");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_EQ(ops[0]["part_id"].toString(), PART_ID);

    const QJsonObject pitchObj = ops[0]["pitch"].toObject();
    EXPECT_EQ(pitchObj["step"].toString(), "C");
    EXPECT_EQ(pitchObj["octave"].toInt(), 4);

    EXPECT_EQ(ops[0]["duration"].toObject()["type"].toString(), "quarter");

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, insertNote_localUuidTracked_forSubsequentOps)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    applyRemoteInsertNote(applicator, score);

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes().front();

    // Translate the insert — this should register a local UUID for the note.
    ChangedMap insertChange = {
        { chord, { CommandType::AddElement } },
        { note,  { CommandType::AddElement } },
    };
    OperationTranslator translator;
    auto insertOps = translator.translateAll(insertChange, {}, PART_ID, applicator.elementToUuid());
    ASSERT_EQ(insertOps.size(), 1);
    const QString assignedUuid = insertOps[0]["id"].toString();
    ASSERT_FALSE(assignedUuid.isEmpty());

    // Now simulate ChangePitch on the same note — translator should find the UUID.
    ChangedMap pitchChange = { { note, { CommandType::ChangePitch } } };
    const auto pitchOps = translator.translateAll(pitchChange, {}, PART_ID, applicator.elementToUuid());
    ASSERT_EQ(pitchOps.size(), 1);
    EXPECT_EQ(pitchOps[0]["type"].toString(), "SetPitch");
    EXPECT_EQ(pitchOps[0]["event_id"].toString(), assignedUuid);

    delete score;
}

// ===========================================================================
// Group 3 — InsertChord: multi-note chord
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertChord_twoPitches_emitsInsertChord)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    // Use ScoreApplicator to insert a 2-pitch chord so we have real pointers.
    ScoreApplicator applicator;

    QJsonObject beat;
    beat["numerator"] = 0; beat["denominator"] = 4;
    QJsonObject dur;
    dur["type"] = "quarter"; dur["dots"] = 0;
    QJsonObject p1; p1["step"] = "C"; p1["octave"] = 4;
    QJsonObject p2; p2["step"] = "E"; p2["octave"] = 4;
    QJsonArray pitches; pitches.append(p1); pitches.append(p2);

    QJsonObject op;
    op["type"] = "InsertChord"; op["beat"] = beat; op["duration"] = dur;
    op["pitches"] = pitches; op["track"] = 0;

    ASSERT_TRUE(applicator.apply(score, op));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    ASSERT_EQ(chord->notes().size(), static_cast<size_t>(2));

    // Simulate AddElement for chord + both notes.
    ChangedMap changed;
    changed[chord] = { CommandType::AddElement };
    for (Note* n : chord->notes()) {
        changed[n] = { CommandType::AddElement };
    }

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertChord");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_EQ(ops[0]["pitches"].toArray().size(), 2);

    delete score;
}

// ===========================================================================
// Group 4 — InsertRest
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertRest_emitsInsertRest)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;

    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    QJsonObject dur;  dur["type"] = "quarter"; dur["dots"] = 0;
    QJsonObject restOp;
    restOp["type"] = "InsertRest"; restOp["beat"] = beat;
    restOp["duration"] = dur; restOp["track"] = 0;
    ASSERT_TRUE(applicator.apply(score, restOp));

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    EngravingItem* el = seg->element(0);
    ASSERT_TRUE(el && el->type() == ElementType::REST);
    Rest* rest = toRest(el);

    ChangedMap changed = { { rest, { CommandType::AddElement } } };
    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertRest");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_EQ(ops[0]["duration"].toObject()["type"].toString(), "quarter");

    delete score;
}

// ===========================================================================
// Group 5 — DeleteEvent for remotely-inserted notes
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, deleteEvent_remoteNote_emitsDeleteEvent)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(uuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes().front();

    // Simulate RemoveElement on both chord and note (whole chord deleted).
    ChangedMap changed = {
        { chord, { CommandType::RemoveElement } },
        { note,  { CommandType::RemoveElement } },
    };

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "DeleteEvent");
    EXPECT_EQ(ops[0]["event_id"].toString(), uuid);

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, deleteEvent_unknownNote_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    applyRemoteInsertNote(applicator, score); // UUID in applicator, but don't give to translator

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);
    Note* note = chord->notes().front();

    // Remote map not passed in — simulates an element the translator doesn't know.
    QHash<EngravingObject*, QString> emptyMap;
    ChangedMap changed = {
        { chord, { CommandType::RemoveElement } },
        { note,  { CommandType::RemoveElement } },
    };

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, emptyMap);
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}

// ===========================================================================
// Group 6 — SetPitch for a remotely-inserted note
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, setPitch_remoteNote_emitsSetPitch)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(uuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Note* note = toChord(seg->cr(0))->notes().front();

    ChangedMap changed = { { note, { CommandType::ChangePitch } } };
    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "SetPitch");
    EXPECT_EQ(ops[0]["event_id"].toString(), uuid);
    EXPECT_FALSE(ops[0]["pitch"].toObject().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, setPitch_unknownNote_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    applyRemoteInsertNote(applicator, score);

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Note* note = toChord(seg->cr(0))->notes().front();

    QHash<EngravingObject*, QString> emptyMap;
    ChangedMap changed = { { note, { CommandType::ChangePitch } } };
    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, {}, PART_ID, emptyMap);
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}

// ===========================================================================
// Group 7 — SetTrack for a remotely-inserted note
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, setTrack_remoteNote_emitsSetTrack)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(uuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Note* note = toChord(seg->cr(0))->notes().front();

    ChangedMap changed = { { note, { CommandType::ChangeProperty } } };
    PropertyIdSet propIds = { Pid::TRACK };

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, propIds, PART_ID, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "SetTrack");
    EXPECT_EQ(ops[0]["event_id"].toString(), uuid);

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, setTrack_noTrackInPropertyIdSet_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(uuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Note* note = toChord(seg->cr(0))->notes().front();

    ChangedMap changed = { { note, { CommandType::ChangeProperty } } };
    PropertyIdSet propIds = { Pid::COLOR }; // not TRACK

    OperationTranslator translator;
    const auto ops = translator.translateAll(changed, propIds, PART_ID, applicator.elementToUuid());
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}
