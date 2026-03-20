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

#include "editude/internal/operationtranslator.h"
#include "editude/internal/scoreapplicator.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/jump.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/editing/undo.h"
#include "engraving/types/fraction.h"
#include "engraving/tests/utils/scorerw.h"

using namespace mu::engraving;
using namespace mu::editude::internal;

static const String DATA_DIR("data/");
static const QString PART_ID("test-part-id");

class Editude_OperationTranslatorTests : public ::testing::Test {};

// Create a translator with the default part pre-registered.
static OperationTranslator makeTranslator(Score* score)
{
    OperationTranslator t;
    if (score && !score->parts().empty()) {
        t.registerKnownPart(score->parts().front(), PART_ID);
    }
    return t;
}

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
    const auto ops = translator.translateAll({}, {}, emptyMap);
    EXPECT_TRUE(ops.isEmpty());
}

TEST_F(Editude_OperationTranslatorTests, unknownCommandType_returnsEmpty)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;
    // Use a dummy non-null EngravingObject* that doesn't match any handled type.
    // Pass a null pointer — should be skipped gracefully.
    ChangedMap changed = { { nullptr, { CommandType::ChangeStyle } } };
    const auto ops = translator.translateAll(changed, {}, emptyMap);
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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

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
    auto translator = makeTranslator(score);
    auto insertOps = translator.translateAll(insertChange, {}, applicator.elementToUuid());
    ASSERT_EQ(insertOps.size(), 1);
    const QString assignedUuid = insertOps[0]["id"].toString();
    ASSERT_FALSE(assignedUuid.isEmpty());

    // Now simulate ChangePitch on the same note — translator should find the UUID.
    ChangedMap pitchChange = { { note, { CommandType::ChangePitch } } };
    const auto pitchOps = translator.translateAll(pitchChange, {}, applicator.elementToUuid());
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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

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
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, emptyMap);
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
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

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
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, emptyMap);
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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, propIds, applicator.elementToUuid());

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

    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, propIds, applicator.elementToUuid());
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}

// ===========================================================================
// Helpers for Groups 8–16
// ===========================================================================

// Apply a remote AddPart via the applicator with a fixed UUID.
// Returns the Part* that was added (the new last part in score->parts()).
static Part* applyRemoteAddPart(ScoreApplicator& applicator, Score* score,
                                const QString& partUuid = QStringLiteral("test-part"))
{
    QJsonObject instr;
    instr["musescore_id"] = "";
    instr["name"]         = "TestInstr";
    instr["short_name"]   = "T";

    QJsonObject op;
    op["type"]        = "AddPart";
    op["part_id"]     = partUuid;
    op["name"]        = "TestPart";
    op["staff_count"] = 1;
    op["instrument"]  = instr;

    if (!applicator.apply(score, op)) {
        return nullptr;
    }
    return score->parts().back();
}

// Register a Part* in the translator by simulating Pass 9 (AddElement on Part*).
// Returns the UUID the translator assigned to the part.
static QString registerPartInTranslator(OperationTranslator& translator, Part* part,
                                        const QHash<EngravingObject*, QString>& remoteMap)
{
    ChangedMap partChange = { { part, { CommandType::AddElement } } };
    const auto partOps = translator.translateAll(partChange, {}, remoteMap);
    if (partOps.size() != 1 || partOps[0]["type"].toString() != QLatin1String("AddPart")) {
        return {};
    }
    return partOps[0]["part_id"].toString();
}

// Find the first element of the given ElementType in the applicator's UUID map.
static EngravingObject* findElementByType(const QHash<EngravingObject*, QString>& map,
                                          ElementType type)
{
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key() && it.key()->type() == type) {
            return it.key();
        }
    }
    return nullptr;
}

// ===========================================================================
// Group 8 — AddArticulation / RemoveArticulation (Pass 14)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, addArticulation_onSingleNoteEvent_emitsAddArticulation)
{
    // Verifies the uuidForChordRest fallback: the parent Chord* is not in the remote
    // map, but its single Note* is (InsertNote stores UUID on Note*).
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));
    ASSERT_TRUE(chord);

    // Apply staccato articulation on the note.
    QJsonObject artOp;
    artOp["type"]         = "AddArticulation";
    artOp["id"]           = QUuid::createUuid().toString(QUuid::WithoutBraces);
    artOp["event_id"]     = noteUuid;
    artOp["articulation"] = "staccato";
    artOp["part_id"]      = PART_ID;
    ASSERT_TRUE(applicator.apply(score, artOp));
    ASSERT_FALSE(chord->articulations().empty());

    auto* art = static_cast<Articulation*>(chord->articulations().front());

    // Simulate AddElement on the articulation locally (not in remote map).
    // The translator must find the parent UUID via the Note* fallback path.
    ChangedMap changed = { { art, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "AddArticulation");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_EQ(ops[0]["event_id"].toString(), noteUuid);
    EXPECT_EQ(ops[0]["articulation"].toString(), "staccato");

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, addArticulation_unknownParent_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));

    QJsonObject artOp;
    artOp["type"]         = "AddArticulation";
    artOp["id"]           = QUuid::createUuid().toString(QUuid::WithoutBraces);
    artOp["event_id"]     = noteUuid;
    artOp["articulation"] = "accent";
    artOp["part_id"]      = PART_ID;
    ASSERT_TRUE(applicator.apply(score, artOp));
    ASSERT_FALSE(chord->articulations().empty());

    auto* art = static_cast<Articulation*>(chord->articulations().front());

    // Pass empty remote map — parent UUID not findable, so the pass skips.
    QHash<EngravingObject*, QString> emptyMap;
    ChangedMap changed = { { art, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, emptyMap);
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, removeArticulation_knownElement_emitsRemoveArticulation)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    Segment* seg = score->tick2segment(Fraction(0, 4), false, SegmentType::ChordRest);
    ASSERT_TRUE(seg);
    Chord* chord = toChord(seg->cr(0));

    QJsonObject artOp;
    artOp["type"]         = "AddArticulation";
    artOp["id"]           = QUuid::createUuid().toString(QUuid::WithoutBraces);
    artOp["event_id"]     = noteUuid;
    artOp["articulation"] = "tenuto";
    artOp["part_id"]      = PART_ID;
    ASSERT_TRUE(applicator.apply(score, artOp));
    ASSERT_FALSE(chord->articulations().empty());

    auto* art = static_cast<Articulation*>(chord->articulations().front());

    // First, Add (registers art* in translator's local map).
    auto translator = makeTranslator(score);
    ChangedMap addChange = { { art, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString artUuid = addOps[0]["id"].toString();
    ASSERT_FALSE(artUuid.isEmpty());

    // Then Remove — translator finds UUID in its own local map.
    ChangedMap removeChange = { { art, { CommandType::RemoveElement } } };
    const auto removeOps = translator.translateAll(removeChange, {}, applicator.elementToUuid());
    ASSERT_EQ(removeOps.size(), 1);
    EXPECT_EQ(removeOps[0]["type"].toString(), "RemoveArticulation");
    EXPECT_EQ(removeOps[0]["id"].toString(), artUuid);

    delete score;
}

// ===========================================================================
// Group 9 — AddDynamic / SetDynamic / RemoveDynamic (Pass 15)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, addDynamic_unknownPart_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    Part* testPart = applyRemoteAddPart(applicator, score);
    ASSERT_TRUE(testPart);

    QJsonObject addDynOp;
    addDynOp["type"]    = "AddDynamic";
    addDynOp["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addDynOp["part_id"] = "test-part";
    addDynOp["kind"]    = "mf";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    addDynOp["beat"]    = beat;
    ASSERT_TRUE(applicator.apply(score, addDynOp));

    auto* dyn = static_cast<Dynamic*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::DYNAMIC));
    ASSERT_TRUE(dyn);

    // Translator lazily registers unknown part → emits AddPart + AddDynamic.
    OperationTranslator translator;
    ChangedMap changed = { { dyn, { CommandType::AddElement } } };
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());
    ASSERT_EQ(ops.size(), 2);
    EXPECT_EQ(ops[0]["type"].toString(), "AddPart");
    EXPECT_EQ(ops[1]["type"].toString(), "AddDynamic");

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, addDynamic_registeredPart_emitsAddDynamic)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    Part* testPart = applyRemoteAddPart(applicator, score);
    ASSERT_TRUE(testPart);

    QJsonObject addDynOp;
    addDynOp["type"]    = "AddDynamic";
    addDynOp["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addDynOp["part_id"] = "test-part";
    addDynOp["kind"]    = "ff";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    addDynOp["beat"]    = beat;
    ASSERT_TRUE(applicator.apply(score, addDynOp));

    auto* dyn = static_cast<Dynamic*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::DYNAMIC));
    ASSERT_TRUE(dyn);

    auto translator = makeTranslator(score);
    // Register the part in the translator before testing the dynamic.
    ASSERT_FALSE(registerPartInTranslator(translator, testPart, applicator.elementToUuid()).isEmpty());

    ChangedMap changed = { { dyn, { CommandType::AddElement } } };
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "AddDynamic");
    EXPECT_EQ(ops[0]["kind"].toString(), "ff");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_FALSE(ops[0]["part_id"].toString().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, setDynamic_changeProperty_emitsSetDynamic)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    Part* testPart = applyRemoteAddPart(applicator, score);
    ASSERT_TRUE(testPart);

    QJsonObject addDynOp;
    addDynOp["type"]    = "AddDynamic";
    addDynOp["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    addDynOp["part_id"] = "test-part";
    addDynOp["kind"]    = "p";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    addDynOp["beat"]    = beat;
    ASSERT_TRUE(applicator.apply(score, addDynOp));

    auto* dyn = static_cast<Dynamic*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::DYNAMIC));
    ASSERT_TRUE(dyn);

    auto translator = makeTranslator(score);
    ASSERT_FALSE(registerPartInTranslator(translator, testPart, applicator.elementToUuid()).isEmpty());

    // Register the dynamic locally via Add first.
    ChangedMap addChange = { { dyn, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString dynUuid = addOps[0]["id"].toString();

    // Simulate ChangeProperty (DynamicType changed) — translator emits SetDynamic.
    ChangedMap setPropChange = { { dyn, { CommandType::ChangeProperty } } };
    const auto setOps = translator.translateAll(setPropChange, {}, applicator.elementToUuid());
    ASSERT_EQ(setOps.size(), 1);
    EXPECT_EQ(setOps[0]["type"].toString(), "SetDynamic");
    EXPECT_EQ(setOps[0]["id"].toString(), dynUuid);

    delete score;
}

// ===========================================================================
// Group 10 — AddSlur / RemoveSlur (Pass 16)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, addSlur_singleNoteEndpoints_emitsAddSlur)
{
    // Verifies that slurs work on InsertNote events (Chord* → Note* UUID fallback).
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid1 = applyRemoteInsertNote(applicator, score, "C", 4, "quarter", 0, 4);
    const QString uuid2 = applyRemoteInsertNote(applicator, score, "E", 4, "quarter", 1, 4);
    ASSERT_FALSE(uuid1.isEmpty());
    ASSERT_FALSE(uuid2.isEmpty());

    QJsonObject slurOp;
    slurOp["type"]           = "AddSlur";
    slurOp["id"]             = QUuid::createUuid().toString(QUuid::WithoutBraces);
    slurOp["start_event_id"] = uuid1;
    slurOp["end_event_id"]   = uuid2;
    ASSERT_TRUE(applicator.apply(score, slurOp));

    auto* slur = static_cast<Slur*>(findElementByType(applicator.elementToUuid(),
                                                        ElementType::SLUR));
    ASSERT_TRUE(slur);

    ChangedMap changed = { { slur, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "AddSlur");
    EXPECT_EQ(ops[0]["start_event_id"].toString(), uuid1);
    EXPECT_EQ(ops[0]["end_event_id"].toString(), uuid2);
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, addSlur_unknownEndpoints_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString uuid1 = applyRemoteInsertNote(applicator, score, "C", 4, "quarter", 0, 4);
    const QString uuid2 = applyRemoteInsertNote(applicator, score, "E", 4, "quarter", 1, 4);
    ASSERT_FALSE(uuid1.isEmpty());
    ASSERT_FALSE(uuid2.isEmpty());

    QJsonObject slurOp;
    slurOp["type"]           = "AddSlur";
    slurOp["id"]             = QUuid::createUuid().toString(QUuid::WithoutBraces);
    slurOp["start_event_id"] = uuid1;
    slurOp["end_event_id"]   = uuid2;
    ASSERT_TRUE(applicator.apply(score, slurOp));

    auto* slur = static_cast<Slur*>(findElementByType(applicator.elementToUuid(),
                                                        ElementType::SLUR));
    ASSERT_TRUE(slur);

    // Empty remote map — translator cannot resolve endpoint UUIDs.
    QHash<EngravingObject*, QString> emptyMap;
    ChangedMap changed = { { slur, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, emptyMap);
    EXPECT_TRUE(ops.isEmpty());

    delete score;
}

// ===========================================================================
// Group 11 — AddHairpin / RemoveHairpin (Pass 17)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, addHairpin_unknownPart_emitsNothing)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    Part* testPart = applyRemoteAddPart(applicator, score);
    ASSERT_TRUE(testPart);

    QJsonObject hpOp;
    hpOp["type"]        = "AddHairpin";
    hpOp["id"]          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hpOp["part_id"]     = "test-part";
    hpOp["kind"]        = "crescendo";
    QJsonObject sb; sb["numerator"] = 0; sb["denominator"] = 4; hpOp["start_beat"] = sb;
    QJsonObject eb; eb["numerator"] = 2; eb["denominator"] = 4; hpOp["end_beat"]   = eb;
    ASSERT_TRUE(applicator.apply(score, hpOp));

    auto* hp = static_cast<Hairpin*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::HAIRPIN));
    ASSERT_TRUE(hp);

    // Translator lazily registers unknown part → emits AddPart + AddHairpin.
    OperationTranslator translator;
    ChangedMap changed = { { hp, { CommandType::AddElement } } };
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());
    ASSERT_EQ(ops.size(), 2);
    EXPECT_EQ(ops[0]["type"].toString(), "AddPart");
    EXPECT_EQ(ops[1]["type"].toString(), "AddHairpin");

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, addHairpin_crescendo_emitsAddHairpin)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    Part* testPart = applyRemoteAddPart(applicator, score);
    ASSERT_TRUE(testPart);

    QJsonObject hpOp;
    hpOp["type"]        = "AddHairpin";
    hpOp["id"]          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hpOp["part_id"]     = "test-part";
    hpOp["kind"]        = "crescendo";
    QJsonObject sb; sb["numerator"] = 0; sb["denominator"] = 4; hpOp["start_beat"] = sb;
    QJsonObject eb; eb["numerator"] = 2; eb["denominator"] = 4; hpOp["end_beat"]   = eb;
    ASSERT_TRUE(applicator.apply(score, hpOp));

    auto* hp = static_cast<Hairpin*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::HAIRPIN));
    ASSERT_TRUE(hp);

    auto translator = makeTranslator(score);
    ASSERT_FALSE(registerPartInTranslator(translator, testPart, applicator.elementToUuid()).isEmpty());

    ChangedMap changed = { { hp, { CommandType::AddElement } } };
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "AddHairpin");
    EXPECT_EQ(ops[0]["kind"].toString(), "crescendo");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());
    EXPECT_FALSE(ops[0]["part_id"].toString().isEmpty());

    delete score;
}

// ===========================================================================
// Group 12 — AddLyric / SetLyric / RemoveLyric (Pass 19)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, addLyric_singleNoteEvent_emitsAddLyric)
{
    // Also exercises uuidForChordRest fallback (same fix as articulations/slurs).
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    QJsonObject lyrOp;
    lyrOp["type"]       = "AddLyric";
    lyrOp["id"]         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    lyrOp["event_id"]   = noteUuid;
    lyrOp["verse"]      = 0;
    lyrOp["syllabic"]   = "single";
    lyrOp["text"]       = "la";
    lyrOp["part_id"]    = PART_ID;
    ASSERT_TRUE(applicator.apply(score, lyrOp));

    auto* lyr = static_cast<Lyrics*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::LYRICS));
    ASSERT_TRUE(lyr);

    ChangedMap changed = { { lyr, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "AddLyric");
    EXPECT_EQ(ops[0]["event_id"].toString(), noteUuid);
    EXPECT_EQ(ops[0]["verse"].toInt(), 0);
    EXPECT_EQ(ops[0]["syllabic"].toString(), "single");
    EXPECT_EQ(ops[0]["text"].toString(), "la");

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, setLyric_changeProperty_emitsSetLyric)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    QJsonObject lyrOp;
    lyrOp["type"]     = "AddLyric";
    lyrOp["id"]       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    lyrOp["event_id"] = noteUuid;
    lyrOp["verse"]    = 0;
    lyrOp["syllabic"] = "begin";
    lyrOp["text"]     = "Al-";
    lyrOp["part_id"]  = PART_ID;
    ASSERT_TRUE(applicator.apply(score, lyrOp));

    auto* lyr = static_cast<Lyrics*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::LYRICS));
    ASSERT_TRUE(lyr);

    auto translator = makeTranslator(score);
    // Register lyric locally.
    ChangedMap addChange = { { lyr, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString lyrUuid = addOps[0]["id"].toString();

    // Simulate ChangeProperty (text changed).
    ChangedMap setPropChange = { { lyr, { CommandType::ChangeProperty } } };
    const auto setOps = translator.translateAll(setPropChange, {}, applicator.elementToUuid());
    ASSERT_EQ(setOps.size(), 1);
    EXPECT_EQ(setOps[0]["type"].toString(), "SetLyric");
    EXPECT_EQ(setOps[0]["id"].toString(), lyrUuid);

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, removeLyric_knownElement_emitsRemoveLyric)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    const QString noteUuid = applyRemoteInsertNote(applicator, score);
    ASSERT_FALSE(noteUuid.isEmpty());

    QJsonObject lyrOp;
    lyrOp["type"]     = "AddLyric";
    lyrOp["id"]       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    lyrOp["event_id"] = noteUuid;
    lyrOp["verse"]    = 0;
    lyrOp["syllabic"] = "single";
    lyrOp["text"]     = "do";
    lyrOp["part_id"]  = PART_ID;
    ASSERT_TRUE(applicator.apply(score, lyrOp));

    auto* lyr = static_cast<Lyrics*>(findElementByType(applicator.elementToUuid(),
                                                         ElementType::LYRICS));
    ASSERT_TRUE(lyr);

    auto translator = makeTranslator(score);
    ChangedMap addChange = { { lyr, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString lyrUuid = addOps[0]["id"].toString();

    ChangedMap removeChange = { { lyr, { CommandType::RemoveElement } } };
    const auto removeOps = translator.translateAll(removeChange, {}, applicator.elementToUuid());
    ASSERT_EQ(removeOps.size(), 1);
    EXPECT_EQ(removeOps[0]["type"].toString(), "RemoveLyric");
    EXPECT_EQ(removeOps[0]["id"].toString(), lyrUuid);

    delete score;
}

// ===========================================================================
// Group 13 — InsertBeats / DeleteBeats (Pass 20)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertBeats_measureWithInsertMeasuresCmd_emitsInsertBeats)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    Measure* measure = score->firstMeasure();
    ASSERT_TRUE(measure);
    const Fraction expectedDuration = measure->ticks().reduced();

    ChangedMap changed = { { measure, { CommandType::InsertMeasures } } };
    auto translator = makeTranslator(score);
    QHash<EngravingObject*, QString> emptyMap;
    const auto ops = translator.translateAll(changed, {}, emptyMap);

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertBeats");
    EXPECT_EQ(ops[0]["at_beat"].toObject()["numerator"].toInt(),   0);
    EXPECT_EQ(ops[0]["duration"].toObject()["numerator"].toInt(),  expectedDuration.numerator());
    EXPECT_EQ(ops[0]["duration"].toObject()["denominator"].toInt(), expectedDuration.denominator());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, deleteBeats_measureWithRemoveMeasuresCmd_emitsDeleteBeats)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    Measure* measure = score->firstMeasure();
    ASSERT_TRUE(measure);

    ChangedMap changed = { { measure, { CommandType::RemoveMeasures } } };
    auto translator = makeTranslator(score);
    QHash<EngravingObject*, QString> emptyMap;
    const auto ops = translator.translateAll(changed, {}, emptyMap);

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "DeleteBeats");
    EXPECT_EQ(ops[0]["at_beat"].toObject()["numerator"].toInt(), 0);
    EXPECT_FALSE(ops[0]["duration"].toObject().isEmpty());

    delete score;
}

// ===========================================================================
// Group 14 — InsertVolta / RemoveVolta (Pass 21)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertVolta_fromLocalEdit_emitsInsertVolta)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject voltaOp;
    voltaOp["type"]    = "InsertVolta";
    voltaOp["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject sb; sb["numerator"] = 0; sb["denominator"] = 4; voltaOp["start_beat"] = sb;
    QJsonObject eb; eb["numerator"] = 4; eb["denominator"] = 4; voltaOp["end_beat"]   = eb;
    QJsonArray numbers; numbers.append(1);
    voltaOp["numbers"]  = numbers;
    voltaOp["open_end"] = false;
    ASSERT_TRUE(applicator.apply(score, voltaOp));

    auto* volta = static_cast<Volta*>(findElementByType(applicator.elementToUuid(),
                                                          ElementType::VOLTA));
    ASSERT_TRUE(volta);

    ChangedMap changed = { { volta, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertVolta");
    EXPECT_EQ(ops[0]["numbers"].toArray().size(), 1);
    EXPECT_EQ(ops[0]["numbers"].toArray()[0].toInt(), 1);
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, removeVolta_knownElement_emitsRemoveVolta)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject voltaOp;
    voltaOp["type"]    = "InsertVolta";
    voltaOp["id"]      = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject sb; sb["numerator"] = 0; sb["denominator"] = 4; voltaOp["start_beat"] = sb;
    QJsonObject eb; eb["numerator"] = 4; eb["denominator"] = 4; voltaOp["end_beat"]   = eb;
    QJsonArray numbers; numbers.append(1);
    voltaOp["numbers"]  = numbers;
    voltaOp["open_end"] = false;
    ASSERT_TRUE(applicator.apply(score, voltaOp));

    auto* volta = static_cast<Volta*>(findElementByType(applicator.elementToUuid(),
                                                          ElementType::VOLTA));
    ASSERT_TRUE(volta);

    // Add locally first.
    auto translator = makeTranslator(score);
    ChangedMap addChange = { { volta, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString voltaUuid = addOps[0]["id"].toString();

    // Now remove.
    ChangedMap removeChange = { { volta, { CommandType::RemoveElement } } };
    const auto removeOps = translator.translateAll(removeChange, {}, applicator.elementToUuid());
    ASSERT_EQ(removeOps.size(), 1);
    EXPECT_EQ(removeOps[0]["type"].toString(), "RemoveVolta");
    EXPECT_EQ(removeOps[0]["id"].toString(), voltaUuid);

    delete score;
}

// ===========================================================================
// Group 15 — InsertMarker / RemoveMarker (Pass 22)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertMarker_segno_emitsInsertMarker)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject markerOp;
    markerOp["type"]  = "InsertMarker";
    markerOp["id"]    = QUuid::createUuid().toString(QUuid::WithoutBraces);
    markerOp["kind"]  = "segno";
    markerOp["label"] = "S";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    markerOp["beat"]  = beat;
    ASSERT_TRUE(applicator.apply(score, markerOp));

    auto* marker = static_cast<Marker*>(findElementByType(applicator.elementToUuid(),
                                                            ElementType::MARKER));
    ASSERT_TRUE(marker);

    ChangedMap changed = { { marker, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertMarker");
    EXPECT_EQ(ops[0]["kind"].toString(), "segno");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, removeMarker_knownElement_emitsRemoveMarker)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject markerOp;
    markerOp["type"]  = "InsertMarker";
    markerOp["id"]    = QUuid::createUuid().toString(QUuid::WithoutBraces);
    markerOp["kind"]  = "coda";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    markerOp["beat"]  = beat;
    ASSERT_TRUE(applicator.apply(score, markerOp));

    auto* marker = static_cast<Marker*>(findElementByType(applicator.elementToUuid(),
                                                            ElementType::MARKER));
    ASSERT_TRUE(marker);

    auto translator = makeTranslator(score);
    ChangedMap addChange = { { marker, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString markerUuid = addOps[0]["id"].toString();

    ChangedMap removeChange = { { marker, { CommandType::RemoveElement } } };
    const auto removeOps = translator.translateAll(removeChange, {}, applicator.elementToUuid());
    ASSERT_EQ(removeOps.size(), 1);
    EXPECT_EQ(removeOps[0]["type"].toString(), "RemoveMarker");
    EXPECT_EQ(removeOps[0]["id"].toString(), markerUuid);

    delete score;
}

// ===========================================================================
// Group 16 — InsertJump / RemoveJump (Pass 23)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, insertJump_fromLocalEdit_emitsInsertJump)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject jumpOp;
    jumpOp["type"]        = "InsertJump";
    jumpOp["id"]          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    jumpOp["jump_to"]     = "start";
    jumpOp["play_until"]  = "end";
    jumpOp["continue_at"] = "";
    jumpOp["text"]        = "D.C.";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    jumpOp["beat"]        = beat;
    ASSERT_TRUE(applicator.apply(score, jumpOp));

    auto* jump = static_cast<Jump*>(findElementByType(applicator.elementToUuid(),
                                                        ElementType::JUMP));
    ASSERT_TRUE(jump);

    ChangedMap changed = { { jump, { CommandType::AddElement } } };
    auto translator = makeTranslator(score);
    const auto ops = translator.translateAll(changed, {}, applicator.elementToUuid());

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "InsertJump");
    EXPECT_EQ(ops[0]["jump_to"].toString(), "start");
    EXPECT_FALSE(ops[0]["id"].toString().isEmpty());

    delete score;
}

TEST_F(Editude_OperationTranslatorTests, removeJump_knownElement_emitsRemoveJump)
{
    MasterScore* score = ScoreRW::readScore(DATA_DIR + u"empty_measure.mscx");
    ASSERT_TRUE(score);

    ScoreApplicator applicator;
    QJsonObject jumpOp;
    jumpOp["type"]        = "InsertJump";
    jumpOp["id"]          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    jumpOp["jump_to"]     = "segno";
    jumpOp["play_until"]  = "";
    jumpOp["continue_at"] = "";
    QJsonObject beat; beat["numerator"] = 0; beat["denominator"] = 4;
    jumpOp["beat"]        = beat;
    ASSERT_TRUE(applicator.apply(score, jumpOp));

    auto* jump = static_cast<Jump*>(findElementByType(applicator.elementToUuid(),
                                                        ElementType::JUMP));
    ASSERT_TRUE(jump);

    auto translator = makeTranslator(score);
    ChangedMap addChange = { { jump, { CommandType::AddElement } } };
    auto addOps = translator.translateAll(addChange, {}, applicator.elementToUuid());
    ASSERT_EQ(addOps.size(), 1);
    const QString jumpUuid = addOps[0]["id"].toString();

    ChangedMap removeChange = { { jump, { CommandType::RemoveElement } } };
    const auto removeOps = translator.translateAll(removeChange, {}, applicator.elementToUuid());
    ASSERT_EQ(removeOps.size(), 1);
    EXPECT_EQ(removeOps[0]["type"].toString(), "RemoveJump");
    EXPECT_EQ(removeOps[0]["id"].toString(), jumpUuid);

    delete score;
}

// ===========================================================================
// Group 17 — SetScoreMetadata (Pass 24)
// ===========================================================================

TEST_F(Editude_OperationTranslatorTests, setScoreMetadata_titleChange_emitsOpWithPythonFieldName)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;

    QMap<QString, QString> metaChanges;
    metaChanges["workTitle"] = "Symphony No. 1";

    const auto ops = translator.translateAll({}, {}, emptyMap, metaChanges);

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["type"].toString(), "SetScoreMetadata");
    EXPECT_EQ(ops[0]["field"].toString(), "title");
    EXPECT_EQ(ops[0]["value"].toString(), "Symphony No. 1");
}

TEST_F(Editude_OperationTranslatorTests, setScoreMetadata_multipleFields_emitsOneOpEach)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;

    QMap<QString, QString> metaChanges;
    metaChanges["composer"] = "Beethoven";
    metaChanges["subtitle"] = "Op. 125";

    const auto ops = translator.translateAll({}, {}, emptyMap, metaChanges);

    ASSERT_EQ(ops.size(), 2);
    // Collect emitted fields (order not guaranteed).
    QStringList fields;
    for (const auto& op : ops) {
        EXPECT_EQ(op["type"].toString(), "SetScoreMetadata");
        fields.append(op["field"].toString());
    }
    fields.sort();
    ASSERT_EQ(fields.size(), 2);
    EXPECT_EQ(fields[0], "composer");
    EXPECT_EQ(fields[1], "subtitle");
}

TEST_F(Editude_OperationTranslatorTests, setScoreMetadata_emptyDelta_emitsNothing)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;

    const auto ops = translator.translateAll({}, {}, emptyMap, {});
    EXPECT_TRUE(ops.isEmpty());
}

TEST_F(Editude_OperationTranslatorTests, setScoreMetadata_unmappedTag_usesTagNameAsField)
{
    OperationTranslator translator;
    QHash<EngravingObject*, QString> emptyMap;

    QMap<QString, QString> metaChanges;
    metaChanges["customProjectCode"] = "PROJ-42";

    const auto ops = translator.translateAll({}, {}, emptyMap, metaChanges);

    ASSERT_EQ(ops.size(), 1);
    EXPECT_EQ(ops[0]["field"].toString(), "customProjectCode");
    EXPECT_EQ(ops[0]["value"].toString(), "PROJ-42");
}
