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
#include "editudeuiactions.h"

#include "qml/Editude/editudeannotationmodel.h"

#include "context/uicontext.h"
#include "context/shortcutcontext.h"
#include "ui/view/iconcodes.h"

using namespace mu::editude::internal;
using namespace muse;
using namespace muse::ui;
using namespace muse::actions;

const UiActionList EditudeUiActions::m_actions = {
    UiAction("toggle-annotations",
             mu::context::UiCtxProjectOpened,
             mu::context::CTX_ANY,
             TranslatableString("action", "Annotations"),
             TranslatableString("action", "Show/hide annotations panel"),
             IconCode::Code::EDIT,
             Checkable::Yes),
};

EditudeUiActions::EditudeUiActions() = default;

void EditudeUiActions::setAnnotationModel(EditudeAnnotationModel* model)
{
    m_annotationModel = model;
}

const UiActionList& EditudeUiActions::actionsList() const
{
    return m_actions;
}

bool EditudeUiActions::actionEnabled(const UiAction&) const
{
    return true;
}

bool EditudeUiActions::actionChecked(const UiAction& act) const
{
    if (act.code == "toggle-annotations" && m_annotationModel) {
        return m_annotationModel->panelVisible();
    }
    return false;
}

async::Channel<ActionCodeList> EditudeUiActions::actionEnabledChanged() const
{
    static async::Channel<ActionCodeList> ch;
    return ch;
}

async::Channel<ActionCodeList> EditudeUiActions::actionCheckedChanged() const
{
    return m_actionCheckedChanged;
}

void EditudeUiActions::notifyAnnotationToggleChanged()
{
    m_actionCheckedChanged.send({ "toggle-annotations" });
}
