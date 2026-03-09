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
