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
#pragma once

#include "global/async/asyncable.h"

#include "global/modularity/ioc.h"
#include "global/iapplication.h"
#include "interactive/iinteractive.h"
#include "actions/iactionsdispatcher.h"
#include "context/iglobalcontext.h"
#include "audio/main/istartaudiocontroller.h"
#include "audio/main/isoundfontcontroller.h"
#include "engraving/dom/engravingitem.h"

namespace mu::appjs {

// [editude] Upstream WebApi used GlobalInject for interfaces that are now
// context-scoped (IInteractive, IActionsDispatcher, IGlobalContext,
// IStartAudioController, ISoundFontController).  Refactored to resolve
// from the first active context at call time — correct for the single-window
// WASM environment.
class WebApi : public muse::async::Asyncable
{
    inline static muse::GlobalInject<muse::IApplication> s_application;

public:

    static WebApi* instance();

    void init();
    void deinit();

    void load(const void* source, unsigned int len);
    void addSoundFont(const std::string& uri);
    void startAudioProcessing();

    // [editude] Zoom controls for JS/iOS pinch-to-zoom bridge.
    void zoomIn();
    void zoomOut();
    void setZoom(int zoomPercent);

    // [editude] Clipboard/delete/paste for touch action bar.
    void clipboardCut();
    void clipboardCopy();
    void clipboardPaste();
    void deleteSelection();

    // [editude] Selection state query for touch gesture routing.
    bool isRangeSelected();
    bool isSingleSelected();

    // [editude] Two-tap range selection for touch devices.
    void enterRangeSelectMode();
    void completeRangeSelect();

    // [editude] Generic action dispatch for JS-triggered MuseScore actions.
    void dispatchAction(const std::string& actionCode);

private:

    WebApi() = default;

    // Resolve a context-scoped service from the first (only) active context.
    template<typename T>
    std::shared_ptr<T> contextResolve() const
    {
        auto app = s_application();
        if (!app) {
            return nullptr;
        }
        auto ctxs = app->contexts();
        if (ctxs.empty()) {
            return nullptr;
        }
        return muse::modularity::ioc(ctxs.front())->template resolve<T>("appjs");
    }

    void onProjectSaved(const muse::io::path_t& path, mu::project::SaveMode mode);

    project::INotationProjectPtr m_currentProject;

    // [editude] Two-tap range selection state.
    bool m_rangeSelectPending = false;
    mu::engraving::EngravingItem* m_savedElement = nullptr;
    mu::engraving::staff_idx_t m_savedStaffIdx = 0;
};
}
