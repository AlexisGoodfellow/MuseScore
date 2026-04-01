/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Copyright (C) 2026 Alexis Goodfellow
 *
 * CREATED EXCLUSIVELY FOR EDITUDE PURPOSES.
 * EDITUDE HAS NO BUSINESS AFFILIATION WITH MUSESCORE.
 */

#import <Foundation/Foundation.h>
#include "internal/platform/macos/appnap.h"

namespace mu { namespace editude { namespace internal {

static id<NSObject> s_activity = nil;

void beginRealtimeActivity()
{
    if (s_activity) {
        return;  // already active
    }
    // ARC retains the returned activity token automatically.
    s_activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated
                          reason:@"Real-time collaborative editing session"];
}

void endRealtimeActivity()
{
    if (!s_activity) {
        return;
    }
    [[NSProcessInfo processInfo] endActivity:s_activity];
    s_activity = nil;  // ARC releases
}

}}} // namespace mu::editude::internal
