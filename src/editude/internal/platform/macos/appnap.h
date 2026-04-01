/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 Alexis Goodfellow
 *
 * CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
 * EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
 */
#pragma once

namespace mu { namespace editude { namespace internal {

/// Prevent macOS App Nap from throttling timers and event delivery.
/// Call beginActivity() when a real-time collaborative session starts
/// and endActivity() when it ends.  On non-macOS platforms these are
/// no-ops.
void beginRealtimeActivity();
void endRealtimeActivity();

}}} // namespace mu::editude::internal
