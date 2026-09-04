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

#include <QString>
#include <Qt>

namespace mu::editude::internal {

/**
 * Whether a key event is a talkback voice shortcut, and what to do with it.
 *
 * Deliberately separated from the transport (which is WASM-only) so the
 * decision compiles on every platform and can be unit-tested without a browser
 * or a running voice session.  See adr/2026-04-07-talkback-mic.md.
 */
struct VoiceKeyDecision {
    /// The editor should swallow this key rather than let MuseScore act on it.
    bool consume = false;
    /// Action to relay to the app shell; empty when nothing should be sent.
    /// One of "backtick", "mute", "toggleJoin", "devices".
    QString action;
    /// Which edge this was. Only meaningful when `action` is non-empty.
    bool pressed = false;
};

/**
 * Classify a key event.
 *
 * @param key             Qt key code.
 * @param mods            Active modifiers.
 * @param isPress         True for key-down, false for key-up.
 * @param isAutoRepeat    True when the platform generated this as a repeat.
 * @param voiceActive     Whether a voice session is joined. When false the
 *                        editor never captures these keys, so they behave
 *                        normally in MuseScore.
 * @param textEntryActive Whether a score text field is accepting input
 *                        (lyrics, rehearsal marks, tempo text). Typing always
 *                        wins: a backtick belongs in the lyric, not the mic.
 */
VoiceKeyDecision decideVoiceKey(int key,
                                Qt::KeyboardModifiers mods,
                                bool isPress,
                                bool isAutoRepeat,
                                bool voiceActive,
                                bool textEntryActive);

} // namespace mu::editude::internal
