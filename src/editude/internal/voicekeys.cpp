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
#include "voicekeys.h"

namespace mu::editude::internal {

VoiceKeyDecision decideVoiceKey(int key,
                                Qt::KeyboardModifiers mods,
                                bool isPress,
                                bool isAutoRepeat,
                                bool voiceActive,
                                bool textEntryActive)
{
    VoiceKeyDecision decision;

    // Without a joined voice session these keys belong to MuseScore.
    if (!voiceActive) {
        return decision;
    }

    // Typing wins: a backtick belongs in the lyric, not in the mic.
    if (textEntryActive) {
        return decision;
    }

    const bool bareBacktick = key == Qt::Key_QuoteLeft
        && !(mods & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier));

    // Swallow auto-repeat on held backtick rather than restarting talkback on
    // every repeat tick.
    if (isAutoRepeat) {
        decision.consume = bareBacktick;
        return decision;
    }

    if (bareBacktick) {
        // Backtick owns the mic. The shell distinguishes a tap (toggle mute)
        // from a hold (momentary talkback), so it needs both edges.
        decision.consume = true;
        decision.action = QStringLiteral("backtick");
        decision.pressed = isPress;
        return decision;
    }

    const bool comboModifiers = mods.testFlag(Qt::ShiftModifier)
        && (mods.testFlag(Qt::ControlModifier) || mods.testFlag(Qt::MetaModifier));
    if (!comboModifiers) {
        return decision;
    }

    QString action;
    switch (key) {
    case Qt::Key_M: action = QStringLiteral("mute"); break;
    case Qt::Key_V: action = QStringLiteral("toggleJoin"); break;
    case Qt::Key_A: action = QStringLiteral("devices"); break;
    default: break;
    }
    if (action.isEmpty()) {
        return decision;
    }

    // The combos are edge-triggered: consume the release so MuseScore never
    // sees half a shortcut, but only act on the press.
    decision.consume = true;
    if (isPress) {
        decision.action = action;
        decision.pressed = true;
    }
    return decision;
}

} // namespace mu::editude::internal
