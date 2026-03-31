/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2025 MuseScore Limited
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

#include "web/appjs/webapi.h"

#include <emscripten.h>

using namespace mu::appjs;

extern "C" {
EMSCRIPTEN_KEEPALIVE
void load(const void* source, unsigned int len) { WebApi::instance()->load(source, len); }

EMSCRIPTEN_KEEPALIVE
void addSoundFont(const char* uri)
{
    WebApi::instance()->addSoundFont(std::string(uri));
}

EMSCRIPTEN_KEEPALIVE
void startAudioProcessing() { WebApi::instance()->startAudioProcessing(); }

// [editude] Zoom controls for JS/iOS pinch-to-zoom bridge.
EMSCRIPTEN_KEEPALIVE
void webZoomIn() { WebApi::instance()->zoomIn(); }

EMSCRIPTEN_KEEPALIVE
void webZoomOut() { WebApi::instance()->zoomOut(); }

EMSCRIPTEN_KEEPALIVE
void webSetZoom(int zoomPercent) { WebApi::instance()->setZoom(zoomPercent); }

// [editude] Clipboard/delete/paste for touch action bar.
EMSCRIPTEN_KEEPALIVE
void webCut() { WebApi::instance()->clipboardCut(); }

EMSCRIPTEN_KEEPALIVE
void webCopy() { WebApi::instance()->clipboardCopy(); }

EMSCRIPTEN_KEEPALIVE
void webPaste() { WebApi::instance()->clipboardPaste(); }

EMSCRIPTEN_KEEPALIVE
void webDelete() { WebApi::instance()->deleteSelection(); }

// [editude] Selection state queries for touch gesture routing.
EMSCRIPTEN_KEEPALIVE
int webIsRangeSelected() { return WebApi::instance()->isRangeSelected() ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE
int webIsSingleSelected() { return WebApi::instance()->isSingleSelected() ? 1 : 0; }

// [editude] Two-tap range selection for touch devices.
EMSCRIPTEN_KEEPALIVE
void webEnterRangeSelectMode() { WebApi::instance()->enterRangeSelectMode(); }

EMSCRIPTEN_KEEPALIVE
void webCompleteRangeSelect() { WebApi::instance()->completeRangeSelect(); }

}
