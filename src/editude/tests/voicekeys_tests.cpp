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

#include "internal/voicekeys.h"

using namespace mu::editude::internal;

namespace {
constexpr bool kPress = true;
constexpr bool kRelease = false;
constexpr bool kNoRepeat = false;
constexpr bool kVoiceOn = true;
constexpr bool kVoiceOff = false;
constexpr bool kTyping = true;
constexpr bool kNotTyping = false;

VoiceKeyDecision backtick(bool isPress, bool voiceActive, bool textEntry,
                          bool autoRepeat = kNoRepeat,
                          Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    return decideVoiceKey(Qt::Key_QuoteLeft, mods, isPress, autoRepeat,
                          voiceActive, textEntry);
}
}

// ---------------------------------------------------------------------------
// Gating: outside a voice session these keys belong to MuseScore.
// ---------------------------------------------------------------------------

TEST(VoiceKeys, BacktickIgnoredWhenVoiceInactive)
{
    const auto d = backtick(kPress, kVoiceOff, kNotTyping);
    EXPECT_FALSE(d.consume);
    EXPECT_TRUE(d.action.isEmpty());
}

TEST(VoiceKeys, CombosIgnoredWhenVoiceInactive)
{
    const auto d = decideVoiceKey(Qt::Key_M,
                                  Qt::ControlModifier | Qt::ShiftModifier,
                                  kPress, kNoRepeat, kVoiceOff, kNotTyping);
    EXPECT_FALSE(d.consume);
    EXPECT_TRUE(d.action.isEmpty());
}

// ---------------------------------------------------------------------------
// Typing wins: a backtick belongs in the lyric, not in the mic.
// ---------------------------------------------------------------------------

TEST(VoiceKeys, BacktickNotCapturedWhileEnteringText)
{
    const auto d = backtick(kPress, kVoiceOn, kTyping);
    EXPECT_FALSE(d.consume);
    EXPECT_TRUE(d.action.isEmpty());
}

TEST(VoiceKeys, CombosNotCapturedWhileEnteringText)
{
    const auto d = decideVoiceKey(Qt::Key_M,
                                  Qt::ControlModifier | Qt::ShiftModifier,
                                  kPress, kNoRepeat, kVoiceOn, kTyping);
    EXPECT_FALSE(d.consume);
    EXPECT_TRUE(d.action.isEmpty());
}

// ---------------------------------------------------------------------------
// Backtick relays both edges so the shell can tell a tap from a hold.
// ---------------------------------------------------------------------------

TEST(VoiceKeys, BacktickPressRelayed)
{
    const auto d = backtick(kPress, kVoiceOn, kNotTyping);
    EXPECT_TRUE(d.consume);
    EXPECT_EQ(d.action, QStringLiteral("backtick"));
    EXPECT_TRUE(d.pressed);
}

TEST(VoiceKeys, BacktickReleaseRelayed)
{
    const auto d = backtick(kRelease, kVoiceOn, kNotTyping);
    EXPECT_TRUE(d.consume);
    EXPECT_EQ(d.action, QStringLiteral("backtick"));
    EXPECT_FALSE(d.pressed);
}

TEST(VoiceKeys, HeldBacktickAutoRepeatIsSwallowedNotRelayed)
{
    const auto d = backtick(kPress, kVoiceOn, kNotTyping, /*autoRepeat=*/true);
    EXPECT_TRUE(d.consume);
    EXPECT_TRUE(d.action.isEmpty())
        << "auto-repeat must not restart talkback on every tick";
}

TEST(VoiceKeys, ModifiedBacktickIsNotAVoiceShortcut)
{
    for (auto mod : { Qt::ControlModifier, Qt::MetaModifier, Qt::AltModifier }) {
        const auto d = backtick(kPress, kVoiceOn, kNotTyping, kNoRepeat, mod);
        EXPECT_FALSE(d.consume);
        EXPECT_TRUE(d.action.isEmpty());
    }
}

// ---------------------------------------------------------------------------
// Cmd/Ctrl+Shift combos are edge-triggered.
// ---------------------------------------------------------------------------

TEST(VoiceKeys, ComboActionsOnPress)
{
    struct Expectation { int key; const char* action; };
    const Expectation cases[] = {
        { Qt::Key_M, "mute" },
        { Qt::Key_V, "toggleJoin" },
        { Qt::Key_A, "devices" },
    };

    for (const auto& c : cases) {
        for (auto primary : { Qt::ControlModifier, Qt::MetaModifier }) {
            const auto d = decideVoiceKey(c.key, primary | Qt::ShiftModifier,
                                          kPress, kNoRepeat, kVoiceOn, kNotTyping);
            EXPECT_TRUE(d.consume) << c.action;
            EXPECT_EQ(d.action, QString::fromUtf8(c.action));
            EXPECT_TRUE(d.pressed);
        }
    }
}

TEST(VoiceKeys, ComboReleaseIsConsumedButNotRelayed)
{
    const auto d = decideVoiceKey(Qt::Key_M,
                                  Qt::ControlModifier | Qt::ShiftModifier,
                                  kRelease, kNoRepeat, kVoiceOn, kNotTyping);
    EXPECT_TRUE(d.consume) << "MuseScore must not see half a shortcut";
    EXPECT_TRUE(d.action.isEmpty()) << "combos act on press only";
}

TEST(VoiceKeys, ComboNeedsShiftAndAPrimaryModifier)
{
    // Shift alone, or the primary modifier alone, is not a voice shortcut.
    for (auto mods : { Qt::KeyboardModifiers(Qt::ShiftModifier),
                       Qt::KeyboardModifiers(Qt::ControlModifier),
                       Qt::KeyboardModifiers(Qt::NoModifier) }) {
        const auto d = decideVoiceKey(Qt::Key_M, mods, kPress, kNoRepeat,
                                      kVoiceOn, kNotTyping);
        EXPECT_FALSE(d.consume);
        EXPECT_TRUE(d.action.isEmpty());
    }
}

TEST(VoiceKeys, UnrelatedKeysArePassedThrough)
{
    // Space in particular: it is MuseScore's transport key and talkback
    // deliberately does not use it. See the amendment in
    // adr/2026-04-07-talkback-mic.md.
    for (int key : { Qt::Key_Space, Qt::Key_A, Qt::Key_Escape, Qt::Key_Return }) {
        const auto d = decideVoiceKey(key, Qt::NoModifier, kPress, kNoRepeat,
                                      kVoiceOn, kNotTyping);
        EXPECT_FALSE(d.consume) << "key " << key;
        EXPECT_TRUE(d.action.isEmpty()) << "key " << key;
    }
}

TEST(VoiceKeys, SpaceIsNeverCapturedEvenWithModifiers)
{
    const auto d = decideVoiceKey(Qt::Key_Space,
                                  Qt::ControlModifier | Qt::ShiftModifier,
                                  kPress, kNoRepeat, kVoiceOn, kNotTyping);
    EXPECT_FALSE(d.consume);
    EXPECT_TRUE(d.action.isEmpty());
}
