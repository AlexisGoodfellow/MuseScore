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

#include "ui/iuiactionsmodule.h"

namespace mu::editude::internal {

class EditudeAnnotationModel;

class EditudeUiActions : public muse::ui::IUiActionsModule
{
public:
    EditudeUiActions();
    void setAnnotationModel(EditudeAnnotationModel* model);

    const muse::ui::UiActionList& actionsList() const override;
    bool actionEnabled(const muse::ui::UiAction& act) const override;
    bool actionChecked(const muse::ui::UiAction& act) const override;
    muse::async::Channel<muse::actions::ActionCodeList> actionEnabledChanged() const override;
    muse::async::Channel<muse::actions::ActionCodeList> actionCheckedChanged() const override;

    // Emit checked-state change for the toggle-annotations action.
    void notifyAnnotationToggleChanged();

private:
    static const muse::ui::UiActionList m_actions;
    EditudeAnnotationModel* m_annotationModel = nullptr;
    muse::async::Channel<muse::actions::ActionCodeList> m_actionCheckedChanged;
};

} // namespace mu::editude::internal
