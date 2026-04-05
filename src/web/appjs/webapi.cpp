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
#include "webapi.h"

#include "notation/notationtypes.h"

#ifdef Q_OS_WASM
#include <emscripten/bind.h>
#include <emscripten/val.h>
#endif

#include "global/io/file.h"

#include "log.h"

using namespace muse;
using namespace mu::appjs;

#ifdef Q_OS_WASM
static void callJsWithBytes(const char* fnname, const uint8_t* data, size_t size)
{
    emscripten::val jsArray = emscripten::val::global("Uint8Array").new_(
        emscripten::typed_memory_view(size, data)
        );

    emscripten::val::module_property(fnname)(jsArray);
}

#else

static void callJsWithBytes(const char*, const uint8_t*, size_t)
{
    NOT_SUPPORTED;
}

#endif

WebApi* WebApi::instance()
{
    static WebApi a;

    return &a;
}

void WebApi::init()
{
    // [editude] Resolve context-scoped globalContext at call time.
    auto gc = contextResolve<mu::context::IGlobalContext>();
    if (!gc) {
        LOGW() << "WebApi::init() — IGlobalContext not available yet, skipping save hook";
        return;
    }

    auto onProjectChanged = [this, gc]() {
        if (m_currentProject) {
            m_currentProject->saveComplited().disconnect(this);
        }

        m_currentProject = gc->currentProject();

        if (m_currentProject) {
            m_currentProject->saveComplited().onReceive(this, [this](const muse::io::path_t& path, project::SaveMode mode) {
                onProjectSaved(path, mode);
            });
        }
    };

    gc->currentProjectChanged().onNotify(this, onProjectChanged);

    onProjectChanged();
}

void WebApi::deinit()
{
    if (m_currentProject) {
        m_currentProject->saveComplited().disconnect(this);
    }
}

void WebApi::load(const void* source, unsigned int len)
{
    LOGI() << source << ", len: " << len;
    ByteArray data = ByteArray::fromRawData(reinterpret_cast<const char*>(source), len);
    io::path_t tempFilePath = "/mu/temp/current.mscz";

    //! NOTE Remove last previous
    io::File::remove(tempFilePath);

    //! NOTE Write new project
    io::File::writeFile(tempFilePath, data);

    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("file-open", actions::ActionData::make_arg1(QUrl::fromLocalFile(tempFilePath.toQString())));
    }
}

void WebApi::addSoundFont(const std::string& uri)
{
    auto sfc = contextResolve<muse::audio::ISoundFontController>();
    if (sfc) {
        sfc->addSoundFont(Uri(uri));
    }
}

void WebApi::startAudioProcessing()
{
    auto ctrl = contextResolve<muse::audio::IStartAudioController>();
    if (ctrl) {
        ctrl->startAudioProcessing(IApplication::RunMode::GuiApp);
    }
}

// [editude] Zoom controls — dispatch existing registered actions.
void WebApi::zoomIn()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("zoomin");
    }
}

void WebApi::zoomOut()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("zoomout");
    }
}

void WebApi::setZoom(int zoomPercent)
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("zoom-x-percent", muse::actions::ActionData::make_arg1<int>(zoomPercent));
    }
}

// [editude] Clipboard/delete/paste — dispatch existing registered actions.
void WebApi::clipboardCut()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("action://notation/cut");
    }
}

void WebApi::clipboardCopy()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("action://notation/copy");
    }
}

void WebApi::clipboardPaste()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("action://notation/paste");
    }
}

void WebApi::deleteSelection()
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch("action://notation/delete");
    }
}

// [editure] Selection state queries — let JS decide gesture routing and action bar state.
bool WebApi::isRangeSelected()
{
    auto gc = contextResolve<mu::context::IGlobalContext>();
    if (!gc) {
        return false;
    }
    auto notation = gc->currentNotation();
    if (!notation) {
        return false;
    }
    auto selection = notation->interaction()->selection();
    return selection && selection->isRange();
}

bool WebApi::isSingleSelected()
{
    auto gc = contextResolve<mu::context::IGlobalContext>();
    if (!gc) {
        return false;
    }
    auto notation = gc->currentNotation();
    if (!notation) {
        return false;
    }
    auto selection = notation->interaction()->selection();
    if (!selection) {
        return false;
    }
    // LIST state with exactly one element = single selection.
    return !selection->isNone() && !selection->isRange() && !selection->elements().empty();
}

// [editude] Two-tap range selection for touch devices.
// Step 1: save the currently selected element so the user's next tap can
//         define the other end of the range.
void WebApi::enterRangeSelectMode()
{
    m_rangeSelectPending = false;
    m_savedElement = nullptr;

    auto gc = contextResolve<mu::context::IGlobalContext>();
    if (!gc) {
        return;
    }
    auto notation = gc->currentNotation();
    if (!notation) {
        return;
    }
    auto selection = notation->interaction()->selection();
    if (!selection || selection->elements().empty()) {
        return;
    }

    m_savedElement = selection->elements().front();
    m_savedStaffIdx = m_savedElement->staffIdx();
    m_rangeSelectPending = true;
    LOGI() << "enterRangeSelectMode: saved element, staff=" << m_savedStaffIdx;
}

// Step 2: called after the user's second tap has been processed by Qt as a
//         normal single-select.  We restore the saved first element, then
//         extend to the newly-selected second element via SelectType::RANGE.
void WebApi::completeRangeSelect()
{
    if (!m_rangeSelectPending || !m_savedElement) {
        m_rangeSelectPending = false;
        return;
    }
    m_rangeSelectPending = false;

    auto gc = contextResolve<mu::context::IGlobalContext>();
    if (!gc) {
        return;
    }
    auto notation = gc->currentNotation();
    if (!notation) {
        return;
    }
    auto interaction = notation->interaction();
    auto selection = interaction->selection();
    if (!selection || selection->elements().empty()) {
        LOGW() << "completeRangeSelect: no second element selected";
        return;
    }

    // The second element — whatever Qt just single-selected from the user's tap.
    auto* secondElement = selection->elements().front();
    mu::engraving::staff_idx_t secondStaff = secondElement->staffIdx();

    // Restore the first element as a single selection, then extend to the second
    // with RANGE — this is exactly what Score::selectRange() expects.
    interaction->select({ m_savedElement }, mu::engraving::SelectType::SINGLE, m_savedStaffIdx);
    interaction->select({ secondElement }, mu::engraving::SelectType::RANGE, secondStaff);

    m_savedElement = nullptr;
    LOGI() << "completeRangeSelect: range created";
}

// [editude] Generic action dispatch — lets JS trigger any registered MuseScore
// action by name (e.g. "add-annotation", "toggle-annotations").
void WebApi::dispatchAction(const std::string& actionCode)
{
    auto disp = contextResolve<muse::actions::IActionsDispatcher>();
    if (disp) {
        disp->dispatch(actionCode);
    }
}

void WebApi::onProjectSaved(const muse::io::path_t& path, mu::project::SaveMode)
{
    IF_ASSERT_FAILED(io::File::exists(path)) {
        LOGE() << "file does not exist, path: " << path;
        return;
    }

    ByteArray data;
    Ret ret  = io::File::readFile(path, data);
    if (!ret) {
        LOGE() << "failed read file, path: " << path;
        return;
    }

    callJsWithBytes("onProjectSaved", data.constData(), data.size());
}
